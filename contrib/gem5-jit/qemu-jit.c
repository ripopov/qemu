/*
 * QEMU RISC-V TCG adapter for gem5 JitCPU
 *
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"

#include "qemu-jit.h"

#include "accel/accel-ops.h"
#include "accel/accel-cpu-ops.h"
#include "accel/tcg/tcg-accel-ops.h"
#include "accel/tcg/tcg-accel-ops-icount.h"
#include "exec/icount.h"
#include "exec/cputlb.h"
#include "exec/tb-flush.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "hw/core/cpu.h"
#include "hw/boards.h"
#include "qemu-main.h"
#include "qemu/main-loop.h"
#include "qemu/guest-random.h"
#include "system/address-spaces.h"
#include "system/cpus.h"
#include "system/memory.h"
#include "system/replay.h"
#include "system/system.h"
#include "target/riscv/cpu.h"
#include "target/riscv/pmu.h"
#include "tcg/startup.h"

#include <limits.h>

typedef struct Gem5QemuJitHart {
    Gem5QemuJitCallbacks callbacks;
    CPUState *cpu;
    RISCVCPU *riscv_cpu;
    char *isa;
    bool initialized;
    bool running;
    int64_t run_start;
    uint64_t instructions;
    uint64_t run_faults;
    uint64_t run_refunded_faults;
    bool pc_event_stopped;
    uint64_t pc_event_address;
} Gem5QemuJitHart;

typedef struct Gem5QemuJitState {
    Gem5QemuJitHart *harts;
    uint32_t hart_count;
    MemoryRegion memory;
    MemoryRegion ram[32];
    uint64_t ram_base[32];
    size_t ram_count;
    bool initialized;
} Gem5QemuJitState;

static Gem5QemuJitState jit;

static uint64_t
jit_cycles(CPUState *cpu)
{
    Gem5QemuJitHart *hart = &jit.harts[cpu->cpu_index];

    return hart->initialized ?
        hart->callbacks.read_cycles(hart->callbacks.opaque) : 0;
}

static uint64_t
jit_instret(CPUState *cpu, bool before_instruction)
{
    Gem5QemuJitHart *hart = &jit.harts[cpu->cpu_index];

    uint64_t pending = hart->running ?
        icount_get_raw() - hart->run_start - hart->run_faults : 0;

    return hart->instructions + pending -
        (before_instruction && hart->running && pending != 0);
}

static void
jit_fault(CPUState *cpu, bool charged)
{
    Gem5QemuJitHart *hart = &jit.harts[cpu->cpu_index];

    assert(hart->running);
    if (charged) {
        ++hart->run_faults;
    } else {
        /*
         * TCG restored icount to before this instruction. Charge its time,
         * but not retirement, and return before consuming another budget.
         */
        ++hart->run_refunded_faults;
        cpu_exit(cpu);
    }
}

/*
 * gem5 serializes all harts on the thread that initializes this adapter.
 * Keep QEMU's per-CPU setup, but do not create an independent scheduler.
 */
static void
jit_create_vcpu(CPUState *cpu)
{
    tcg_cpu_init_cflags(cpu, false);
    qemu_thread_get_self(cpu->thread);
    cpu->thread_id = qemu_get_thread_id();
    cpu->neg.can_do_io = true;
    cpu_thread_signal_created(cpu);
    if (cpu == first_cpu) {
        qemu_guest_random_seed_thread_part2(cpu->random_seed);
    }
}

/*
 * system/main.c owns this symbol in a normal QEMU executable. The embedded
 * library has no QEMU main function, but display backends still reference it.
 */
int (*qemu_main)(void);

static Gem5QemuJitHart *
jit_hart(uint32_t instance_id)
{
    if (!jit.initialized || instance_id >= jit.hart_count ||
        !jit.harts[instance_id].initialized) {
        return NULL;
    }
    return &jit.harts[instance_id];
}

static Gem5QemuJitHart *
jit_current_hart(void)
{
    uint32_t i;

    for (i = 0; i < jit.hart_count; ++i) {
        if (jit.harts[i].cpu == current_cpu) {
            return &jit.harts[i];
        }
    }
    return NULL;
}

static void
jit_maybe_stop(Gem5QemuJitHart *state)
{
    if (state->callbacks.should_stop &&
        state->callbacks.should_stop(state->callbacks.opaque)) {
        cpu_exit(state->cpu);
    }
}

static uint64_t
jit_read_time(void *opaque)
{
    Gem5QemuJitHart *state = opaque;

    if (!state->callbacks.read_time) {
        return 0;
    }
    return state->callbacks.read_time(state->callbacks.opaque);
}

static int jit_reservation(CPUState *cpu, uint64_t address, uint64_t value,
                           unsigned size, int store, uint64_t *result)
{
    Gem5QemuJitHart *hart = jit_current_hart();
    size_t i;

    if (!hart || hart->cpu != cpu ||
        hart->callbacks.reservation(hart->callbacks.opaque, address, value,
                                    size, store, result)) {
        return -1;
    }
    if (store && *result == 0) {
        for (i = 0; i < jit.ram_count; ++i) {
            uint64_t offset = address - jit.ram_base[i];
            uint64_t length = memory_region_size(&jit.ram[i]);

            if (offset < length && size <= length - offset) {
                ram_addr_t start = memory_region_get_ram_addr(&jit.ram[i]) +
                                   offset;
                memory_region_set_dirty(&jit.ram[i], offset, size);
                tb_invalidate_phys_range(cpu, start, start + size - 1);
                break;
            }
        }
    }
    jit_maybe_stop(hart);
    return 0;
}

static MemTxResult
jit_memory_read(void *opaque, hwaddr address, uint64_t *value,
                unsigned size, MemTxAttrs attrs)
{
    Gem5QemuJitHart *state = jit_current_hart();
    uint8_t data[8] = { 0 };
    uint64_t result = 0;
    unsigned i;

    if (!state || !state->initialized || size > sizeof(data) ||
        state->callbacks.memory_read(state->callbacks.opaque, address,
                                     data, size) != 0) {
        return MEMTX_ERROR;
    }
    jit_maybe_stop(state);

    for (i = 0; i < size; ++i) {
        result |= (uint64_t)data[i] << (i * 8);
    }
    *value = result;
    return MEMTX_OK;
}

static MemTxResult
jit_memory_write(void *opaque, hwaddr address, uint64_t value,
                 unsigned size, MemTxAttrs attrs)
{
    Gem5QemuJitHart *state = jit_current_hart();
    MemoryRegion *region = opaque;
    uint8_t data[8];
    unsigned i;

    if (!state || !state->initialized || size > sizeof(data)) {
        return MEMTX_ERROR;
    }
    for (i = 0; i < size; ++i) {
        data[i] = value >> (i * 8);
    }

    if (region != &jit.memory) {
        address += jit.ram_base[region - jit.ram];
    }
    if (state->callbacks.memory_write(state->callbacks.opaque, address,
                                      data, size) != 0) {
        return MEMTX_ERROR;
    }
    if (region != &jit.memory) {
        ram_addr_t offset = address - jit.ram_base[region - jit.ram];
        ram_addr_t start = memory_region_get_ram_addr(region) + offset;

        memory_region_set_dirty(region, offset, size);
        tb_invalidate_phys_range(state->cpu, start, start + size - 1);
    }
    jit_maybe_stop(state);
    return MEMTX_OK;
}

static const MemoryRegionOps jit_memory_ops = {
    .read_with_attrs = jit_memory_read,
    .write_with_attrs = jit_memory_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};

typedef struct Gem5QemuJitRunRequest {
    Gem5QemuJitHart *hart;
    uint64_t max_instructions;
    Gem5QemuJitRunResult result;
} Gem5QemuJitRunRequest;

static void
jit_run_on_vcpu(CPUState *cpu, run_on_cpu_data data)
{
    Gem5QemuJitRunRequest *request = data.host_ptr;
    Gem5QemuJitHart *hart = request->hart;
    int64_t before;
    int64_t after;
    int64_t budget;

    /*
     * env.bins carries a gem5 pseudo-instruction encoding only while the
     * translated code that stored it is the reason for this batch's EXCP_HLT.
     * QEMU never clears it, and WFI raises the same EXCP_HLT, so a stale value
     * would turn every later WFI into a spurious repeat of the last pseudo
     * instruction. Clear it so only this batch can set it.
     */
    hart->riscv_cpu->env.bins = 0;

    /*
     * cpu_exec() is normally entered by QEMU's round-robin TCG loop with the
     * BQL dropped. Keep exactly the same locking and icount protocol here.
     */
    qatomic_set(&cpu->exit_request, false);
    qatomic_set(&cpu->neg.icount_decr.u16.high, 0);
    bql_unlock();
    before = icount_get_raw();
    hart->run_start = before;
    hart->run_faults = 0;
    hart->run_refunded_faults = 0;
    hart->running = true;
    budget = MIN(request->max_instructions, (uint64_t)INT32_MAX);
    icount_prepare_for_run(cpu, budget);
    request->result.qemu_exception = tcg_cpu_exec(cpu);
    icount_process_data(cpu);
    after = icount_get_raw();
    assert(after - before >= hart->run_faults);
    request->result.retired = after - before - hart->run_faults;
    hart->instructions += request->result.retired;
    hart->running = false;
    bql_lock();

    request->result.instructions = after - before + hart->run_refunded_faults;
    if (request->result.qemu_exception == EXCP_HLT && !cpu->halted &&
        (hart->riscv_cpu->env.bins & 0x01ffffff) == 0x7b) {
        request->result.reason = GEM5_QEMU_JIT_EXIT_M5OP;
        request->result.m5_function = hart->riscv_cpu->env.bins >> 25;
    } else if (request->result.qemu_exception == EXCP_DEBUG &&
               cpu_breakpoint_test(cpu, hart->riscv_cpu->env.pc, BP_GDB)) {
        request->result.reason = GEM5_QEMU_JIT_EXIT_PC_EVENT;
        hart->pc_event_stopped = true;
        hart->pc_event_address = hart->riscv_cpu->env.pc;
    } else if (request->result.instructions == budget) {
        request->result.reason = GEM5_QEMU_JIT_EXIT_BUDGET;
    } else if (cpu->halted) {
        request->result.reason = cpu_has_work(cpu) ?
            GEM5_QEMU_JIT_EXIT_INTERRUPT : GEM5_QEMU_JIT_EXIT_HALTED;
    } else if (request->result.qemu_exception == EXCP_INTERRUPT) {
        request->result.reason = GEM5_QEMU_JIT_EXIT_INTERRUPT;
    } else {
        request->result.reason = GEM5_QEMU_JIT_EXIT_ERROR;
    }
}

static int
jit_global_init(const Gem5QemuJitCallbacks *callbacks)
{
    static const int signals[] = { SIGINT, SIGHUP, SIGTERM, SIGPIPE };
    struct sigaction saved_signals[G_N_ELEMENTS(signals)];
    sigset_t saved_mask;
    CPUState *cpu;
    const char *cpu_type;
    uint32_t cpu_count = 0;
    uint64_t guest_address;
    uint64_t size;
    uint8_t *host_address;
    int writable;
    size_t index;
    const char *cpu_model =
                "rv64,v=true,h=false,sstc=false,zawrs=false,"
                "vlen=256,elen=64,pmp=true,num-pmp-regions=16,"
                "zicbom=true,zicbop=true,zicboz=true,zicond=true,"
                "zimop=true,zcmop=true,zfa=true,zfh=true,zfhmin=true,"
                "zcb=true,zba=true,zbb=true,zbc=true,zbs=true,"
                "zbkb=true,zbkc=true,zbkx=true,zkne=true,zknd=true,"
                "zknh=true,zks=true,zvbc=true,sscofpmf=true,"
                "svade=true,svadu=false,svvptc=false,svnapot=false";
    char *argv[] = {
        (char *)"gem5-qemu-jit",
        (char *)"-machine", (char *)"none",
        (char *)"-accel", (char *)"tcg,thread=single",
        (char *)"-icount", (char *)"shift=0,sleep=off",
        (char *)"-S",
        (char *)"-nodefaults",
        (char *)"-no-user-config",
        (char *)"-display", (char *)"none",
        (char *)"-monitor", (char *)"none",
        (char *)"-serial", (char *)"none",
    };

    /*
     * The embedding process owns cancellation, statistics and host I/O
     * signals. No QEMU main loop or vCPU thread consumes its signalfd.
     */
    if (pthread_sigmask(SIG_SETMASK, NULL, &saved_mask)) {
        return -1;
    }
    for (index = 0; index < G_N_ELEMENTS(signals); ++index) {
        if (sigaction(signals[index], NULL, &saved_signals[index])) {
            return -1;
        }
    }
    qemu_init(G_N_ELEMENTS(argv), argv);
    for (index = 0; index < G_N_ELEMENTS(signals); ++index) {
        if (sigaction(signals[index], &saved_signals[index], NULL)) {
            return -1;
        }
    }
    if (pthread_sigmask(SIG_SETMASK, &saved_mask, NULL)) {
        return -1;
    }

    if (first_cpu) {
        return -1;
    }
    /*
     * Preserve TCG's reset, interrupt and icount operations. Only thread
     * creation/kicking differs: batches run synchronously on gem5's thread.
     * qemu_init registered this thread with RCU; register its TCG context.
     */
    ACCEL_GET_CLASS(current_accel())->ops->create_vcpu_thread = jit_create_vcpu;
    ACCEL_GET_CLASS(current_accel())->ops->kick_vcpu_thread =
        tcg_kick_vcpu_thread;
    tcg_register_thread();
    /*
     * The empty machine supplies no devices or memory. Set the hart count
     * before realization: QEMU sizes implied-extension bitmaps from it.
     */
    MACHINE(qdev_get_machine())->smp.cpus = callbacks->instance_count;
    cpu_type = parse_cpu_option(cpu_model);
    for (index = 0; index < callbacks->instance_count; ++index) {
        cpu = CPU(object_new(cpu_type));
        RISCV_CPU(cpu)->env.mhartid = index;
        qdev_realize(DEVICE(cpu), NULL, &error_fatal);
    }

    jit.hart_count = callbacks->instance_count;
    jit.harts = g_new0(Gem5QemuJitHart, jit.hart_count);
    riscv_gem5_jit_enabled = true;
    riscv_gem5_jit_reservation = jit_reservation;

    CPU_FOREACH(cpu) {
        Gem5QemuJitHart *hart;

        if (cpu->cpu_index >= jit.hart_count) {
            return -1;
        }
        hart = &jit.harts[cpu->cpu_index];
        hart->cpu = cpu;
        hart->riscv_cpu = RISCV_CPU(cpu);
        hart->isa = riscv_isa_string(hart->riscv_cpu);
        hart->riscv_cpu->env.mhartid = cpu->cpu_index;
        hart->riscv_cpu->env.rdtime_fn = jit_read_time;
        hart->riscv_cpu->env.rdtime_fn_arg = hart;
        ++cpu_count;
    }
    if (cpu_count != jit.hart_count) {
        return -1;
    }
    riscv_gem5_jit_instret = jit_instret;
    riscv_gem5_jit_cycles = jit_cycles;
    riscv_gem5_jit_fault = jit_fault;

    /*
     * qemu_init() returns with both locks held. The normal QEMU main() drops
     * them before entering its loop. This adapter retains the BQL so that
     * synchronous CPU operations can use it, but icount owns replay_lock
     * while executing a batch.
     */
    replay_mutex_unlock();

    memory_region_init_io(&jit.memory, NULL, &jit_memory_ops, &jit.memory,
                          "gem5-jit-physical-memory", UINT64_MAX);
    memory_region_add_subregion(get_system_memory(), 0, &jit.memory);

    if (callbacks->memory_map) {
        for (index = 0; ; ++index) {
            char *name;

            if (callbacks->memory_map(callbacks->opaque, index,
                                      &guest_address, &size,
                                      &host_address, &writable) != 0) {
                break;
            }
            if (!size || !host_address) {
                return -1;
            }

            if (jit.ram_count == G_N_ELEMENTS(jit.ram)) {
                return -1;
            }
            name = g_strdup_printf("gem5-jit-ram-%zu", jit.ram_count);
            memory_region_init_ram_ptr(&jit.ram[jit.ram_count], NULL, name,
                                       size, host_address);
            /*
             * A ROM device has direct reads/fetches and callback writes.
             * Reuse the external RAM allocator, then give it the same
             * region flags as memory_region_init_rom_device_nomigrate().
             * Unlike that initializer, this never allocates guest RAM.
             */
            jit.ram[jit.ram_count].ram = false;
            jit.ram[jit.ram_count].rom_device = true;
            jit.ram[jit.ram_count].ops = &jit_memory_ops;
            jit.ram[jit.ram_count].opaque = &jit.ram[jit.ram_count];
            jit.ram_base[jit.ram_count] = guest_address;
            memory_region_set_readonly(&jit.ram[jit.ram_count], !writable);
            memory_region_add_subregion_overlap(
                get_system_memory(), guest_address,
                &jit.ram[jit.ram_count], 1);
            ++jit.ram_count;
            g_free(name);
        }
    }

    jit.initialized = true;
    return 0;
}

int
gem5_qemu_jit_init(const Gem5QemuJitCallbacks *callbacks)
{
    Gem5QemuJitHart *hart;

    if (!callbacks || callbacks->abi_version != GEM5_QEMU_JIT_ABI_VERSION ||
        (callbacks->instance_count != 1 && callbacks->instance_count != 2 &&
         callbacks->instance_count != 4) ||
        callbacks->instance_id >= callbacks->instance_count ||
        callbacks->hart_id != callbacks->instance_id ||
        !callbacks->memory_read || !callbacks->memory_write ||
        !callbacks->read_cycles ||
        !callbacks->reservation) {
        return -1;
    }

    if (!jit.initialized && jit_global_init(callbacks) != 0) {
        return -1;
    }
    if (callbacks->instance_count != jit.hart_count) {
        return -1;
    }

    hart = &jit.harts[callbacks->instance_id];
    if (hart->initialized) {
        return -1;
    }
    hart->callbacks = *callbacks;
    hart->riscv_cpu->env.mhartid = callbacks->hart_id;
    hart->initialized = true;
    return 0;
}

const char *
gem5_qemu_jit_get_isa(uint32_t instance_id)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    return hart ? hart->isa : NULL;
}

int
gem5_qemu_jit_pmu_poll(uint32_t instance_id, uint64_t *cycles,
                       uint64_t *instructions)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    if (!hart || !cycles || !instructions || hart->running) {
        return -1;
    }
    riscv_pmu_gem5_poll(&hart->riscv_cpu->env, cycles, instructions);
    return 0;
}

int
gem5_qemu_jit_set_pc_events(uint32_t instance_id,
                            const uint64_t *addresses, size_t count)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    if (!hart || hart->running || (count && !addresses)) {
        return -1;
    }
    cpu_breakpoint_remove_all(hart->cpu, BP_GDB);
    for (size_t i = 0; i < count; ++i) {
        if (cpu_breakpoint_insert(hart->cpu, addresses[i], BP_GDB, NULL)) {
            return -1;
        }
    }
    return 0;
}

int
gem5_qemu_jit_pc_event_serviced(uint32_t instance_id)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    if (!hart || hart->running || !hart->pc_event_stopped) {
        return -1;
    }
    hart->pc_event_address = hart->riscv_cpu->env.pc;
    return 0;
}

int
gem5_qemu_jit_translate_debug(uint32_t instance_id, uint64_t address,
                              uint64_t *physical)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);
    hwaddr page;

    if (!hart || !physical || hart->running || current_cpu ||
        !qemu_cpu_is_self(hart->cpu)) {
        return -1;
    }
    /* Page-table reads can reach the host's physical-memory callback. */
    current_cpu = hart->cpu;
    page = cpu_get_phys_page_debug(hart->cpu, address);
    current_cpu = NULL;
    if (page == (hwaddr)-1) {
        return -1;
    }
    *physical = page | (address & ~TARGET_PAGE_MASK);
    return 0;
}

int
gem5_qemu_jit_run(uint32_t instance_id, uint64_t max_instructions,
                  Gem5QemuJitRunResult *result)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);
    Gem5QemuJitRunRequest request = {
        .hart = hart,
        .max_instructions = max_instructions,
    };
    bool bypass = false;
    uint64_t bypass_pc = 0;

    if (!hart || !result || max_instructions == 0 ||
        !qemu_cpu_is_self(hart->cpu) || current_cpu) {
        return -1;
    }

    if (hart->pc_event_stopped &&
        hart->riscv_cpu->env.pc == hart->pc_event_address) {
        bypass_pc = hart->pc_event_address;
        bypass = cpu_breakpoint_remove(hart->cpu, bypass_pc, BP_GDB) == 0;
        if (bypass) {
            request.max_instructions = 1;
        }
    }
    hart->pc_event_stopped = false;
    current_cpu = hart->cpu;
    qemu_process_cpu_events_common(hart->cpu);
    jit_run_on_vcpu(hart->cpu, RUN_ON_CPU_HOST_PTR(&request));
    current_cpu = NULL;
    if (bypass) {
        cpu_breakpoint_insert(hart->cpu, bypass_pc, BP_GDB, NULL);
        if (!request.result.instructions &&
            hart->riscv_cpu->env.pc == bypass_pc) {
            /* An asynchronous exit did not consume the serviced insn. */
            hart->pc_event_stopped = true;
            hart->pc_event_address = bypass_pc;
        }
    }
    memcpy(request.result.gprs, hart->riscv_cpu->env.gpr,
           sizeof(request.result.gprs));
    request.result.gprs[0] = 0;
    request.result.pc = hart->riscv_cpu->env.pc;
    request.result.privilege = hart->riscv_cpu->env.priv;
    *result = request.result;
    return 0;
}

uint64_t
gem5_qemu_jit_executed(uint32_t instance_id)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    if (!hart) {
        return UINT64_MAX;
    }
    if (!hart->running) {
        return 0;
    }
    if (current_cpu != hart->cpu || !hart->cpu->neg.can_do_io) {
        return UINT64_MAX;
    }
    return icount_get_raw() - hart->run_start + hart->run_refunded_faults;
}

uint64_t
gem5_qemu_jit_get_gpr(uint32_t instance_id, unsigned index)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    if (!hart || index >= 32) {
        return 0;
    }
    return index == 0 ? 0 : hart->riscv_cpu->env.gpr[index];
}

int
gem5_qemu_jit_set_gpr(uint32_t instance_id, unsigned index, uint64_t value)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    if (!hart || index >= 32) {
        return -1;
    }
    if (index != 0) {
        hart->riscv_cpu->env.gpr[index] = value;
    }
    return 0;
}

uint64_t
gem5_qemu_jit_get_fpr(uint32_t instance_id, unsigned index)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    if (!hart || index >= 32) {
        return 0;
    }
    return hart->riscv_cpu->env.fpr[index];
}

int
gem5_qemu_jit_set_fpr(uint32_t instance_id, unsigned index, uint64_t value)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    if (!hart || index >= 32) {
        return -1;
    }
    hart->riscv_cpu->env.fpr[index] = value;
    return 0;
}

uint64_t
gem5_qemu_jit_get_pc(uint32_t instance_id)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    return hart ? hart->riscv_cpu->env.pc : 0;
}

void
gem5_qemu_jit_set_pc(uint32_t instance_id, uint64_t value)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    if (hart) {
        hart->riscv_cpu->env.pc = value;
        hart->cpu->halted = 0;
    }
}

unsigned
gem5_qemu_jit_get_priv(uint32_t instance_id)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    return hart ? hart->riscv_cpu->env.priv : 0;
}

int
gem5_qemu_jit_set_priv(uint32_t instance_id, unsigned value)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    if (!hart || value > PRV_M) {
        return -1;
    }
    riscv_cpu_set_mode(&hart->riscv_cpu->env, value, false);
    return 0;
}

int
gem5_qemu_jit_get_csr(uint32_t instance_id, unsigned csr, uint64_t *value)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    if (!hart || !value || csr >= 0x1000) {
        return -1;
    }
    /*
     * fflags and frm remain architectural state while mstatus.FS is Off.
     * QEMU's CSR permission check intentionally rejects guest accesses in
     * that state, but CPU takeover must still be able to copy their values.
     */
    if (csr == CSR_FFLAGS) {
        *value = riscv_cpu_get_fflags(&hart->riscv_cpu->env);
        return 0;
    }
    if (csr == CSR_FRM) {
        *value = hart->riscv_cpu->env.frm;
        return 0;
    }
    return riscv_csrr(&hart->riscv_cpu->env, csr, value) ==
        RISCV_EXCP_NONE ? 0 : -1;
}

int
gem5_qemu_jit_set_csr(uint32_t instance_id, unsigned csr, uint64_t value)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    if (!hart || csr >= 0x1000) {
        return -1;
    }
    if (csr == CSR_FFLAGS) {
        riscv_cpu_set_fflags(&hart->riscv_cpu->env, value);
        return 0;
    }
    if (csr == CSR_FRM) {
        hart->riscv_cpu->env.frm = value & 0x7;
        return 0;
    }
    return riscv_csrrw(&hart->riscv_cpu->env, csr, NULL, value,
                      UINT64_MAX, 0) ==
        RISCV_EXCP_NONE ? 0 : -1;
}

uint64_t
gem5_qemu_jit_get_mip(uint32_t instance_id)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    return hart ? hart->riscv_cpu->env.mip : 0;
}

void
gem5_qemu_jit_set_mip(uint32_t instance_id, uint64_t value)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    if (hart) {
        /*
         * LCOFIP is generated and cleared by QEMU's architectural PMU, not
         * by gem5's platform interrupt controller.
         */
        riscv_cpu_update_mip(&hart->riscv_cpu->env, ~MIP_LCOFIP, value);
    }
}

static void
jit_invalidate_on_vcpu(CPUState *cpu, run_on_cpu_data data)
{
    Gem5QemuJitHart *hart = data.host_ptr;
    CPURISCVState *env = &hart->riscv_cpu->env;

    tlb_flush(cpu);
    cpu->halted = 0;
    cpu->exception_index = RISCV_EXCP_NONE;
    qatomic_set(&cpu->exit_request, false);
    env->load_res = -1;
    env->load_val = 0;
    env->badaddr = 0;
    env->guest_phys_fault_addr = 0;
    env->bins = 0;
    hart->pc_event_stopped = false;
}

void
gem5_qemu_jit_invalidate_translations(uint32_t instance_id)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    if (hart) {
        /* The queued TB flush executes before the synchronous callback. */
        queue_tb_flush(hart->cpu);
        run_on_cpu(hart->cpu, jit_invalidate_on_vcpu,
                   RUN_ON_CPU_HOST_PTR(hart));
    }
}
