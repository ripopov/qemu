/*
 * QEMU RISC-V TCG adapter for gem5 JitCPU
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qapi/error.h"

#include "qemu-jit.h"

#include "accel/tcg/tcg-accel-ops.h"
#include "accel/tcg/tcg-accel-ops-icount.h"
#include "exec/icount.h"
#include "exec/cputlb.h"
#include "exec/tb-flush.h"
#include "exec/translation-block.h"
#include "hw/core/cpu.h"
#include "qemu-main.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/replay.h"
#include "system/system.h"
#include "target/riscv/cpu.h"

#include <limits.h>

typedef struct Gem5QemuJitHart {
    Gem5QemuJitCallbacks callbacks;
    CPUState *cpu;
    RISCVCPU *riscv_cpu;
    bool initialized;
    bool running;
    bool stimer_active;
} Gem5QemuJitHart;

typedef struct Gem5QemuJitState {
    Gem5QemuJitHart *harts;
    uint32_t hart_count;
    MemoryRegion memory;
    MemoryRegion ram[32];
    size_t ram_count;
    bool initialized;
} Gem5QemuJitState;

static Gem5QemuJitState jit;
static Gem5QemuJitConfig jit_config;

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

static void
jit_refresh_timers(Gem5QemuJitHart *hart, Gem5QemuJitTimerState *state)
{
    CPURISCVState *env = &hart->riscv_cpu->env;
    uint64_t now = jit_read_time(hart);
    bool enabled = hart->riscv_cpu->cfg.ext_sstc &&
                   (env->menvcfg & MENVCFG_STCE);
    bool venabled = enabled && riscv_has_ext(env, RVH) &&
                    (env->henvcfg & HENVCFG_STCE);
    bool pending = enabled && now >= env->stimecmp;
    bool vpending = venabled && now + env->htimedelta >= env->vstimecmp;

    /* Never leave native QEMU deadlines armed in an embedded hart. */
    if (env->stimer) {
        timer_del(env->stimer);
    }
    if (env->vstimer) {
        timer_del(env->vstimer);
    }
    if (enabled || hart->stimer_active) {
        riscv_cpu_update_mip(env, MIP_STIP, pending ? MIP_STIP : 0);
    }
    hart->stimer_active = enabled;
    /* Hardware VS timer pending is separate from software-injected HVIP. */
    env->vstime_irq = vpending;
    riscv_cpu_update_mip(env, 0, 0);
    if (state) {
        *state = (Gem5QemuJitTimerState) {
            .version = GEM5_QEMU_JIT_TIMER_STATE_VERSION,
            .size = sizeof(*state),
            .flags = (enabled ? GEM5_QEMU_JIT_STIMER_ENABLED : 0) |
                     (venabled ? GEM5_QEMU_JIT_VSTIMER_ENABLED : 0) |
                     (pending ? GEM5_QEMU_JIT_STIMER_PENDING : 0) |
                     (vpending ? GEM5_QEMU_JIT_VSTIMER_PENDING : 0),
            .time = now,
            .stimecmp = env->stimecmp,
            .vstimecmp = env->vstimecmp,
            .htimedelta = env->htimedelta,
        };
    }
}

static void
jit_timer_changed(void *opaque)
{
    Gem5QemuJitHart *hart = opaque;
    jit_refresh_timers(hart, NULL);
    if (hart->running) {
        cpu_exit(hart->cpu);
    }
}

int
gem5_qemu_jit_refresh_timers(uint32_t instance_id,
                           Gem5QemuJitTimerState *state, size_t size)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);
    if (!hart || !state || size != sizeof(*state) || hart->running) {
        return -1;
    }
    jit_refresh_timers(hart, state);
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
    uint8_t data[8];
    unsigned i;

    if (!state || !state->initialized || size > sizeof(data)) {
        return MEMTX_ERROR;
    }
    for (i = 0; i < size; ++i) {
        data[i] = value >> (i * 8);
    }

    if (state->callbacks.memory_write(state->callbacks.opaque, address,
                                      data, size) != 0) {
        return MEMTX_ERROR;
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
    if (hart->callbacks.run_begin) {
        hart->callbacks.run_begin(hart->callbacks.opaque);
    }
    before = icount_get_raw();
    budget = MIN(request->max_instructions, (uint64_t)INT32_MAX);
    icount_prepare_for_run(cpu, budget);
    hart->running = true;
    request->result.qemu_exception = tcg_cpu_exec(cpu);
    hart->running = false;
    icount_process_data(cpu);
    after = icount_get_raw();
    if (hart->callbacks.run_end) {
        hart->callbacks.run_end(hart->callbacks.opaque);
    }
    bql_lock();

    request->result.instructions = after - before;
    if (request->result.qemu_exception == EXCP_HLT && !cpu->halted &&
        (hart->riscv_cpu->env.bins & 0x01ffffff) == 0x7b) {
        request->result.reason = GEM5_QEMU_JIT_EXIT_M5OP;
        request->result.m5_function = hart->riscv_cpu->env.bins >> 25;
    } else if (request->result.instructions == budget) {
        request->result.reason = GEM5_QEMU_JIT_EXIT_BUDGET;
    } else if (cpu->halted) {
        request->result.reason = GEM5_QEMU_JIT_EXIT_HALTED;
    } else if (request->result.qemu_exception == EXCP_INTERRUPT) {
        request->result.reason = GEM5_QEMU_JIT_EXIT_INTERRUPT;
    } else {
        request->result.reason = GEM5_QEMU_JIT_EXIT_ERROR;
    }
}

static int
jit_global_init(const Gem5QemuJitCallbacks *callbacks,
                const Gem5QemuJitConfig *config)
{
    CPUState *cpu;
    const char *cpu_type;
    uint32_t cpu_count = 0;
    uint64_t guest_address;
    uint64_t size;
    uint8_t *host_address;
    int writable;
    size_t index;
    char profile_cpu[64];
    char *argv[] = {
        (char *)"gem5-qemu-jit",
        (char *)"-machine", (char *)"none",
        (char *)"-cpu",
        (char *)"rv64,v=false,h=false,sstc=false,zicbom=false,"
                "zicboz=false,zicbop=false,zawrs=false,zfa=false,zbc=false,"
                "svadu=false,svvptc=false,svnapot=true",
        (char *)"-accel", (char *)"tcg,thread=single",
        (char *)"-icount", (char *)"shift=0,sleep=off",
        (char *)"-S",
        (char *)"-nodefaults",
        (char *)"-no-user-config",
        (char *)"-display", (char *)"none",
        (char *)"-monitor", (char *)"none",
        (char *)"-serial", (char *)"none",
    };

    if (config->profile == GEM5_QEMU_JIT_PROFILE_RVA23S64) {
        snprintf(profile_cpu, sizeof(profile_cpu), "rva23s64,vlen=%u",
                 config->vlenb * 8);
        argv[4] = profile_cpu;
    }
    qemu_init(G_N_ELEMENTS(argv), argv);

    if (!first_cpu || CPU_NEXT(first_cpu)) {
        return -1;
    }
    cpu_type = object_get_typename(OBJECT(first_cpu));
    for (index = 1; index < callbacks->instance_count; ++index) {
        CPUState *next = CPU(object_new(cpu_type));
        /* QEMU tracks implied-extension realization by hart ID. Assign a
         * unique internal ID before realizing, not only afterwards. */
        RISCV_CPU(next)->env.mhartid = index;
        object_property_set_int(OBJECT(next), "vlen", config->vlenb * 8,
                                &error_abort);
        if (!qdev_realize(DEVICE(next), NULL, &error_abort)) {
            return -1;
        }
    }

    jit.hart_count = callbacks->instance_count;
    jit.harts = g_new0(Gem5QemuJitHart, jit.hart_count);
    riscv_gem5_jit_enabled = true;

    CPU_FOREACH(cpu) {
        Gem5QemuJitHart *hart;

        if (cpu->cpu_index >= jit.hart_count) {
            return -1;
        }
        hart = &jit.harts[cpu->cpu_index];
        hart->cpu = cpu;
        hart->riscv_cpu = RISCV_CPU(cpu);
        if (hart->riscv_cpu->cfg.vlenb != config->vlenb) {
            return -1;
        }
        hart->riscv_cpu->env.mhartid = cpu->cpu_index;
        hart->riscv_cpu->env.rdtime_fn = jit_read_time;
        hart->riscv_cpu->env.rdtime_fn_arg = hart;
        hart->riscv_cpu->env.external_timer_update = jit_timer_changed;
        hart->riscv_cpu->env.external_timer_opaque = hart;
        ++cpu_count;
    }
    if (cpu_count != jit.hart_count) {
        return -1;
    }

    /*
     * qemu_init() returns with both locks held. The normal QEMU main() drops
     * them before entering its loop. This adapter retains the BQL so that
     * synchronous run_on_cpu() calls can use it, but icount owns replay_lock
     * while executing a batch.
     */
    replay_mutex_unlock();

    memory_region_init_io(&jit.memory, NULL, &jit_memory_ops, &jit,
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
            memory_region_set_readonly(&jit.ram[jit.ram_count], !writable);
            memory_region_add_subregion_overlap(
                get_system_memory(), guest_address,
                &jit.ram[jit.ram_count], 1);
            ++jit.ram_count;
        }
    }

    jit.initialized = true;
    jit_config = *config;
    return 0;
}

int
gem5_qemu_jit_init(const Gem5QemuJitCallbacks *callbacks)
{
    const Gem5QemuJitConfig config = {
        .version = GEM5_QEMU_JIT_CONFIG_VERSION,
        .size = sizeof(config),
        .profile = GEM5_QEMU_JIT_PROFILE_LEGACY,
        .vlenb = 16,
    };
    return gem5_qemu_jit_init_config(callbacks, &config, sizeof(config));
}

int
gem5_qemu_jit_init_config(const Gem5QemuJitCallbacks *callbacks,
                         const Gem5QemuJitConfig *config, size_t size)
{
    Gem5QemuJitHart *hart;

    if (!config || size != sizeof(*config) ||
        config->size != sizeof(*config) ||
        config->version != GEM5_QEMU_JIT_CONFIG_VERSION ||
        config->profile > GEM5_QEMU_JIT_PROFILE_RVA23S64 ||
        config->vlenb < 16 || config->vlenb > GEM5_QEMU_JIT_MAX_VLENB ||
        (config->vlenb & (config->vlenb - 1)) ||
        (config->profile == GEM5_QEMU_JIT_PROFILE_LEGACY &&
         config->vlenb != 16) ||
        (jit.initialized && memcmp(config, &jit_config, sizeof(*config))) ||
        !callbacks ||
        callbacks->instance_count == 0 ||
        callbacks->instance_id >= callbacks->instance_count ||
        !callbacks->memory_read || !callbacks->memory_write) {
        return -1;
    }

    if (!jit.initialized && jit_global_init(callbacks, config) != 0) {
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

int
gem5_qemu_jit_run(uint32_t instance_id, uint64_t max_instructions,
                  Gem5QemuJitRunResult *result)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);
    Gem5QemuJitRunRequest request = {
        .hart = hart,
        .max_instructions = max_instructions,
    };

    if (!hart || !result || max_instructions == 0) {
        return -1;
    }

    run_on_cpu(hart->cpu, jit_run_on_vcpu, RUN_ON_CPU_HOST_PTR(&request));
    *result = request.result;
    return 0;
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
    return gem5_qemu_jit_set_mode(instance_id, value, 0);
}

int
gem5_qemu_jit_get_mode(uint32_t instance_id, unsigned *privilege,
                      unsigned *virtualization)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);

    if (!hart || !privilege || !virtualization) {
        return -1;
    }
    *privilege = hart->riscv_cpu->env.priv;
    *virtualization = hart->riscv_cpu->env.virt_enabled;
    return 0;
}

int
gem5_qemu_jit_set_mode(uint32_t instance_id, unsigned privilege,
                      unsigned virtualization)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);
    CPURISCVState *env;

    if (!hart || privilege > PRV_M || privilege == PRV_RESERVED ||
        virtualization > 1 || (virtualization && privilege == PRV_M)) {
        return -1;
    }
    env = &hart->riscv_cpu->env;
    if (virtualization && !riscv_has_ext(env, RVH)) {
        return -1;
    }
    /* Like QEMU's debugger restore path, move banked registers before
     * changing V. set_mode alone changes execution flags, not the banks. */
    if (env->virt_enabled != virtualization) {
        riscv_cpu_swap_hypervisor_regs(env);
    }
    riscv_cpu_set_mode(env, privilege, virtualization);
    return 0;
}

int
gem5_qemu_jit_get_vector_state(uint32_t instance_id,
                              Gem5QemuJitVectorState *state, size_t size)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);
    CPURISCVState *env;
    unsigned reg, byte, vlenb;

    if (!hart || !state || size != sizeof(*state)) {
        return -1;
    }
    vlenb = hart->riscv_cpu->cfg.vlenb;
    if (!vlenb || vlenb > GEM5_QEMU_JIT_MAX_VLENB || vlenb % 8) {
        return -1;
    }
    env = &hart->riscv_cpu->env;
    memset(state, 0, sizeof(*state));
    state->version = GEM5_QEMU_JIT_VECTOR_STATE_VERSION;
    state->size = sizeof(*state);
    state->vlenb = vlenb;
    state->vl = env->vl;
    state->vstart = env->vstart;
    state->vxrm = env->vxrm;
    state->vxsat = env->vxsat;
    state->vill = env->vill;
    state->vtype = env->vtype;
    for (reg = 0; reg < 32; reg++) {
        for (byte = 0; byte < vlenb; byte += 8) {
            stq_le_p(&state->registers[reg][byte],
                     env->vreg[(reg * vlenb + byte) / 8]);
        }
    }
    return 0;
}

int
gem5_qemu_jit_set_vector_state(uint32_t instance_id,
                              const Gem5QemuJitVectorState *state, size_t size)
{
    Gem5QemuJitHart *hart = jit_hart(instance_id);
    CPURISCVState *env;
    unsigned reg, byte, vlenb;

    if (!hart || !state || size != sizeof(*state) ||
        state->version != GEM5_QEMU_JIT_VECTOR_STATE_VERSION ||
        state->size != sizeof(*state)) {
        return -1;
    }
    vlenb = hart->riscv_cpu->cfg.vlenb;
    if (!vlenb || vlenb > GEM5_QEMU_JIT_MAX_VLENB || vlenb % 8 ||
        state->vlenb != vlenb || state->vxrm > 3 ||
        state->vxsat > 1 || state->vill > 1) {
        return -1;
    }
    /* Validate the entire input before changing any architectural state. */
    for (reg = 0; reg < 32; reg++) {
        for (byte = vlenb; byte < GEM5_QEMU_JIT_MAX_VLENB; byte++) {
            if (state->registers[reg][byte]) {
                return -1;
            }
        }
    }
    env = &hart->riscv_cpu->env;
    for (reg = 0; reg < 32; reg++) {
        for (byte = 0; byte < vlenb; byte += 8) {
            env->vreg[(reg * vlenb + byte) / 8] =
                ldq_le_p(&state->registers[reg][byte]);
        }
    }
    env->vl = state->vl;
    env->vstart = state->vstart;
    env->vxrm = state->vxrm;
    env->vxsat = state->vxsat;
    env->vill = state->vill;
    env->vtype = state->vtype;
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
    return riscv_csr_read_i64(&hart->riscv_cpu->env, csr, value) ==
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
    return riscv_csr_write_i64(&hart->riscv_cpu->env, csr, value) ==
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
        riscv_cpu_update_mip(&hart->riscv_cpu->env, UINT64_MAX, value);
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
