/*
 * gem5 RiscvJitCPU backend: drives the RISC-V TCG system emulator through
 * the C interface in include/gem5/qemu-jit.h.
 *
 * Copyright (c) 2026 Roman Popov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is compiled only into libgem5-qemu-jit (-Dgem5_jit=true). See
 * the header for the division of responsibilities between gem5 and QEMU.
 *
 * Threads and locks. gem5 calls every function here from one thread, which
 * qemu_init() turns into QEMU's main thread: it holds the Big QEMU Lock
 * between calls, exactly like the QEMU main loop does between its
 * iterations. Batches run on the single TCG vCPU thread through
 * run_on_cpu(), which releases the BQL while the gem5 thread waits, so the
 * two threads never run at the same time. The batch itself follows the
 * round-robin scheduler's protocol: expired QEMU timers are serviced, the
 * BQL is dropped while translated code runs and the icount budget brackets
 * the run. Callbacks into gem5 therefore execute on the vCPU thread,
 * with the BQL held for device accesses; all callbacks are serialised
 * with respect to the gem5 thread.
 */

#include "qemu/osdep.h"

#include <signal.h>

#include "gem5/qemu-jit.h"

#include "accel/tcg/tcg-accel-ops.h"
#include "accel/tcg/tcg-accel-ops-icount.h"
#include "cpu.h"
#include "cpu-qom.h"
#include "exec/icount.h"
#include "exec/target_page.h"
#include "exec/translation-block.h"
#include "hw/core/cpu.h"
#include "hw/qdev-core.h"
#include "qapi/error.h"
#include "qemu-main.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/replay.h"
#include "system/system.h"

/* system/main.c owns this in an emulator; UI backends still reference it. */
int (*qemu_main)(void);

typedef struct JitHart {
    CPUState *cs;
    RISCVCPU *cpu;
    uint32_t index;
    /* A breakpoint stopped the previous batch at this pc; step over it. */
    bool step_over;
    uint64_t step_over_pc;
} JitHart;

static struct {
    bool initialized;
    Gem5QemuJitCallbacks cb;
    JitHart *harts;
    uint32_t hart_count;
    MemoryRegion *ram;
    MemoryRegion io;
    char *isa_string;
    /* The batch being executed on the vCPU thread, if any. */
    JitHart *batch_hart;
    int64_t batch_start;
} jit;

static JitHart *jit_hart(uint32_t index)
{
    if (!jit.initialized || index >= jit.hart_count) {
        return NULL;
    }
    return &jit.harts[index];
}

static uint32_t jit_current_hart(void)
{
    return jit.batch_hart ? jit.batch_hart->index : 0;
}

/*
 * Instructions the running batch has completed so far: the budget minus
 * what icount still allows (cpu_get_icount_executed()). It is exact at the
 * end of a translation block, where device accesses happen thanks to the
 * icount I/O recompilation, and within one block otherwise.
 */
static uint64_t jit_executed(void)
{
    CPUState *cs;

    if (!jit.batch_hart) {
        return 0;
    }
    cs = jit.batch_hart->cs;
    return cs->icount_budget -
           (cs->neg.icount_decr.u16.low + cs->icount_extra);
}

/* Physical accesses QEMU cannot serve from mapped RAM go to gem5. */
static MemTxResult jit_io_read(void *opaque, hwaddr addr, uint64_t *value,
                               unsigned size, MemTxAttrs attrs)
{
    uint8_t data[8] = { 0 };
    uint64_t result = 0;

    if (size > sizeof(data) ||
        jit.cb.mmio_read(jit.cb.opaque, jit_current_hart(), jit_executed(),
                         addr, data, size) != 0) {
        return MEMTX_ERROR;
    }
    for (unsigned i = 0; i < size; i++) {
        result |= (uint64_t)data[i] << (8 * i);
    }
    *value = result;
    return MEMTX_OK;
}

static MemTxResult jit_io_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size, MemTxAttrs attrs)
{
    uint8_t data[8];

    if (size > sizeof(data)) {
        return MEMTX_ERROR;
    }
    for (unsigned i = 0; i < size; i++) {
        data[i] = value >> (8 * i);
    }
    if (jit.cb.mmio_write(jit.cb.opaque, jit_current_hart(), jit_executed(),
                          addr, data, size) != 0) {
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps jit_io_ops = {
    .read_with_attrs = jit_io_read,
    .write_with_attrs = jit_io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static uint64_t jit_rdtime(void *opaque)
{
    JitHart *hart = opaque;

    return jit.cb.read_time(jit.cb.opaque, hart->index, jit_executed());
}

/* Batch execution */
typedef struct JitRunRequest {
    JitHart *hart;
    uint64_t budget;
    Gem5QemuJitRunResult result;
} JitRunRequest;

/*
 * Run one cpu_exec() for at most budget instructions. Called on the vCPU
 * thread with the BQL held. Returns cpu_exec()'s result.
 */
static int jit_exec(JitHart *hart, int64_t budget, uint64_t *executed)
{
    CPUState *cs = hart->cs;
    int64_t before, after;
    int result;

    qatomic_set(&cs->exit_request, false);
    qatomic_set(&cs->neg.icount_decr.u16.high, 0);

    bql_unlock();
    replay_mutex_lock();
    bql_lock();
    icount_handle_deadline();
    replay_mutex_unlock();

    bql_unlock();
    jit.cb.batch_begin(jit.cb.opaque, hart->index);
    before = icount_get_raw();
    jit.batch_start = before;
    jit.batch_hart = hart;
    icount_prepare_for_run(cs, budget);
    result = tcg_cpu_exec(cs);
    icount_process_data(cs);
    after = icount_get_raw();
    jit.batch_hart = NULL;
    bql_lock();

    *executed = after - before;
    return result;
}

static void jit_run_on_vcpu(CPUState *cs, run_on_cpu_data data)
{
    JitRunRequest *req = data.host_ptr;
    JitHart *hart = req->hart;
    CPURISCVState *env = &hart->cpu->env;
    Gem5QemuJitRunResult *res = &req->result;
    uint64_t remaining = req->budget;
    bool step = hart->step_over && env->pc == hart->step_over_pc &&
                cpu_breakpoint_test(cs, env->pc, BP_GDB);
    int idle = 0;

    current_cpu = cs;
    hart->step_over = false;
    res->exit = GEM5_QEMU_JIT_EXIT_BUDGET;

    while (remaining) {
        uint64_t bp_pc = env->pc;
        int64_t budget = MIN(remaining, (uint64_t)INT32_MAX);
        uint64_t executed = 0;
        int result;

        if (step) {
            cpu_breakpoint_remove(cs, bp_pc, BP_GDB);
            budget = 1;
        }
        result = jit_exec(hart, budget, &executed);
        if (step) {
            cpu_breakpoint_insert(cs, bp_pc, BP_GDB, NULL);
        }
        res->instructions += executed;
        remaining -= MIN(executed, remaining);
        res->qemu_code = result;

        if (result == EXCP_GEM5_M5OP) {
            res->exit = GEM5_QEMU_JIT_EXIT_M5OP;
            res->m5op = env->bins;
            return;
        }
        if (result == EXCP_DEBUG) {
            res->exit = GEM5_QEMU_JIT_EXIT_BREAKPOINT;
            hart->step_over = true;
            hart->step_over_pc = env->pc;
            return;
        }
        if (cs->halted) {
            res->exit = GEM5_QEMU_JIT_EXIT_HALTED;
            return;
        }
        if (result != EXCP_INTERRUPT && result != EXCP_HLT) {
            res->exit = GEM5_QEMU_JIT_EXIT_ERROR;
            return;
        }
        if (step) {
            /* Continue the batch past the breakpoint. */
            step = false;
            continue;
        }
        if (executed == (uint64_t)budget) {
            return;
        }
        if (executed) {
            res->exit = GEM5_QEMU_JIT_EXIT_STOPPED;
            return;
        }
        /*
         * Nothing ran: a QEMU timer deadline truncated the budget to zero.
         * Service the timers and retry; give up rather than spin forever.
         */
        qemu_clock_run_all_timers();
        if (++idle > 8) {
            res->exit = GEM5_QEMU_JIT_EXIT_ERROR;
            return;
        }
    }
}

int gem5_qemu_jit_run(uint32_t hart_index, uint64_t budget,
                      Gem5QemuJitRunResult *result)
{
    JitHart *hart = jit_hart(hart_index);
    JitRunRequest req = { .hart = hart, .budget = budget };

    if (!hart || !result || !budget) {
        return -1;
    }
    run_on_cpu(hart->cs, jit_run_on_vcpu, RUN_ON_CPU_HOST_PTR(&req));
    *result = req.result;
    return 0;
}

void gem5_qemu_jit_request_exit(uint32_t hart_index)
{
    JitHart *hart = jit_hart(hart_index);

    if (hart) {
        cpu_exit(hart->cs);
    }
}

/* Register access */
uint64_t gem5_qemu_jit_get_gpr(uint32_t hart_index, unsigned index)
{
    JitHart *hart = jit_hart(hart_index);

    if (!hart || index == 0 || index >= 32) {
        return 0;
    }
    return hart->cpu->env.gpr[index];
}

void gem5_qemu_jit_set_gpr(uint32_t hart_index, unsigned index,
                           uint64_t value)
{
    JitHart *hart = jit_hart(hart_index);

    if (hart && index != 0 && index < 32) {
        hart->cpu->env.gpr[index] = value;
    }
}

uint64_t gem5_qemu_jit_get_reg(uint32_t hart_index, enum Gem5QemuJitReg reg)
{
    JitHart *hart = jit_hart(hart_index);
    CPURISCVState *env;

    if (!hart) {
        return 0;
    }
    env = &hart->cpu->env;
    switch (reg) {
    case GEM5_QEMU_JIT_REG_PC:
        return env->pc;
    case GEM5_QEMU_JIT_REG_PRIV:
        return env->priv;
    case GEM5_QEMU_JIT_REG_MSTATUS:
        return env->mstatus;
    case GEM5_QEMU_JIT_REG_SATP:
        return env->satp;
    }
    return 0;
}

void gem5_qemu_jit_set_pc(uint32_t hart_index, uint64_t pc)
{
    JitHart *hart = jit_hart(hart_index);

    if (hart) {
        hart->cpu->env.pc = pc;
    }
}

void gem5_qemu_jit_set_interrupts(uint32_t hart_index, uint64_t pending)
{
    JitHart *hart = jit_hart(hart_index);
    CPURISCVState *env;
    bool seip;

    if (!hart) {
        return;
    }
    env = &hart->cpu->env;
    riscv_cpu_update_mip(env, MIP_MEIP | MIP_MTIP | MIP_MSIP,
                         pending & (MIP_MEIP | MIP_MTIP | MIP_MSIP));
    /* Like riscv_cpu_set_irq(): SEIP is the external line or the software bit. */
    seip = (pending & MIP_SEIP) != 0;
    env->external_seip = seip;
    riscv_cpu_update_mip(env, MIP_SEIP,
                         BOOL_TO_MASK(seip || env->software_seip));
}

int gem5_qemu_jit_breakpoint(uint32_t hart_index, uint64_t pc, int insert)
{
    JitHart *hart = jit_hart(hart_index);

    if (!hart) {
        return -1;
    }
    if (insert) {
        return cpu_breakpoint_insert(hart->cs, pc, BP_GDB, NULL);
    }
    return cpu_breakpoint_remove(hart->cs, pc, BP_GDB);
}

const char *gem5_qemu_jit_isa_string(void)
{
    return jit.isa_string;
}

/* Initialisation */
static void jit_error(char *error, size_t error_size, const char *fmt, ...)
    G_GNUC_PRINTF(3, 4);

static void jit_error(char *error, size_t error_size, const char *fmt, ...)
{
    va_list ap;

    if (!error || !error_size) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(error, error_size, fmt, ap);
    va_end(ap);
}

/*
 * This POC implements one fixed ISA. Accept the contract's spelling and
 * gem5 RiscvISA's alphabetically ordered spelling (case-insensitively).
 * These are deliberately not a general ISA parser.
 */
static const char jit_contract_isa[] =
    "rv64imafdcv_zic64b_zicbom_zicbop_zicboz_ziccamoa_ziccif_zicclsm_ziccrse_"
    "zicntr_zicond_zicsr_zifencei_zihintntl_zihintpause_zihpm_zimop_za64rs_"
    "zfa_zfh_zfhmin_zcb_zcmop_zba_zbb_zbc_zbs_zbkb_zbkc_zbkx_zkne_zknd_zknh_"
    "zks_zvbc_ssccptr_sscofpmf_ssu64xl_svade_svbare";

static const char jit_gem5_isa[] =
    "rv64imafdcv_za64rs_zba_zbb_zbc_zbkb_zbkc_zbkx_zbs_zcb_zcmop_zfa_zfh_"
    "zfhmin_zic64b_zicbom_zicbop_zicboz_ziccamoa_ziccif_zicclsm_ziccrse_"
    "zicntr_zicond_zicsr_zifencei_zihintntl_zihintpause_zihpm_zimop_zknd_"
    "zkne_zknh_zks_zvbc_ssccptr_sscofpmf_ssu64xl_svade_svbare";

/*
 * QEMU v10.2.2's exact output, including implied and privileged-spec
 * extensions. Svbare is inherent, not printed.
 * Any QEMU upgrade that changes this output requires an explicit review.
 */
static const char jit_qemu_isa[] =
    "rv64imafdcv_zic64b_zicbom_zicbop_zicboz_ziccamoa_ziccif_zicclsm_ziccrse_"
    "zicond_zicntr_zicsr_zifencei_zihintntl_zihintpause_zihpm_zimop_zmmul_"
    "za64rs_zaamo_zalrsc_zfa_zfh_zfhmin_zca_zcb_zcd_zcmop_zba_zbb_zbc_zbkb_"
    "zbkc_zbkx_zbs_zknd_zkne_zknh_zks_zksed_zksh_zvbc_zve32f_zve32x_zve64f_"
    "zve64d_zve64x_shcounterenw_shgatpa_shtvala_shvsatpa_shvstvala_shvstvecd_"
    "ssccptr_sscofpmf_sscounterenw_ssstrict_sstvala_sstvecd_ssu64xl_svade";

static bool jit_set_bool(Object *obj, const char *name, bool value,
                         char *error, size_t error_size)
{
    Error *err = NULL;

    object_property_set_bool(obj, name, value, &err);
    if (err) {
        jit_error(error, error_size, "cannot set CPU property %s: %s", name,
                  error_get_pretty(err));
        error_free(err);
        return false;
    }
    return true;
}

static bool jit_set_uint(Object *obj, const char *name, uint64_t value,
                         char *error, size_t error_size)
{
    Error *err = NULL;

    object_property_set_uint(obj, name, value, &err);
    if (err) {
        jit_error(error, error_size, "cannot set CPU property %s: %s", name,
                  error_get_pretty(err));
        error_free(err);
        return false;
    }
    return true;
}

static bool jit_configure_cpu(Object *obj, const Gem5QemuJitConfig *config,
                              char *error, size_t error_size)
{
    /*
     * Start from bare rv64i; enable only the fixed demo's properties.
     * Zic64b and the memory/privilege attributes have no settable property;
     * QEMU supplies them and the exact ISA check below verifies them.
     */
    static const char *const enabled[] = {
        "m", "a", "f", "d", "c", "v", "s", "u", "mmu", "pmp",
        "sv39", "sv48", "sv57",
        "zicbom", "zicbop", "zicboz", "zicntr", "zicond",
        "zicsr", "zifencei", "zihintntl", "zihintpause", "zihpm", "zimop",
        "zfa", "zfh", "zfhmin", "zcb", "zcmop", "zba", "zbb", "zbc",
        "zbs", "zbkb", "zbkc", "zbkx", "zkne", "zknd", "zknh", "zks",
        "zvbc", "sscofpmf", "svade",
    };
    const struct {
        const char *name;
        uint64_t value;
    } values[] = {
        { "num-pmp-regions", config->pmp_regions },
        { "resetvec", config->reset_pc },
        { "mvendorid", config->mvendorid },
        { "marchid", config->marchid },
        { "mimpid", config->mimpid },
        { "vlen", config->vlen },
        { "elen", config->elen },
        { "cbom_blocksize", config->cache_line_size },
        { "cbop_blocksize", config->cache_line_size },
        { "cboz_blocksize", config->cache_line_size },
        { "pmu-mask", MAKE_64BIT_MASK(3, 29) },
    };

    if (!jit_set_bool(obj, "h", false, error, error_size) ||
        !jit_set_bool(obj, "debug", false, error, error_size)) {
        return false;
    }
    for (size_t i = 0; i < ARRAY_SIZE(enabled); i++) {
        if (!jit_set_bool(obj, enabled[i], true, error, error_size)) {
            return false;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
        if (!jit_set_uint(obj, values[i].name, values[i].value,
                          error, error_size)) {
            return false;
        }
    }
    return true;
}

static bool jit_verify_isa(RISCVCPU *cpu, char *error, size_t error_size)
{
    g_autofree char *isa = riscv_isa_string(cpu);

    /* A C D F I M S U V, with RV64 selected by the bare CPU type. */
    if (cpu->env.misa_ext != 0x34112d ||
        cpu->cfg.max_satp_mode != VM_1_10_SV57 ||
        strcmp(isa, jit_qemu_isa)) {
        jit_error(error, error_size,
                  "QEMU does not match the fixed demo ISA: "
                  "misa extensions 0x%x, max satp mode %u, ISA '%s'",
                  cpu->env.misa_ext, cpu->cfg.max_satp_mode, isa);
        return false;
    }
    if (!jit.isa_string) {
        jit.isa_string = g_steal_pointer(&isa);
    }
    return true;
}

static bool jit_map_memory(const Gem5QemuJitConfig *config, char *error,
                           size_t error_size)
{
    MemoryRegion *sysmem = get_system_memory();
    const uint64_t page = qemu_target_page_size();

    /* Everything not covered by RAM below is a gem5 device or hole. */
    memory_region_init_io(&jit.io, NULL, &jit_io_ops, NULL,
                          "gem5-physical", UINT64_MAX);
    memory_region_add_subregion_overlap(sysmem, 0, &jit.io, -1);

    jit.ram = g_new0(MemoryRegion, config->ram_count);
    for (size_t i = 0; i < config->ram_count; i++) {
        const Gem5QemuJitRam *ram = &config->ram[i];
        g_autofree char *name = g_strdup_printf("gem5-ram-%zu", i);

        /* Read-only ranges use the background gem5 callbacks for reads
         * and writes, leaving permission enforcement to their owner. */
        if (!ram->writable) {
            continue;
        }
        if (!ram->size || (ram->base | ram->size) % page ||
            (uintptr_t)ram->host % page) {
            jit_error(error, error_size, "RAM [0x%" PRIx64 ", 0x%" PRIx64
                      ") at host %p is not page aligned", ram->base,
                      ram->base + ram->size, ram->host);
            return false;
        }
        memory_region_init_ram_ptr(&jit.ram[i], NULL, name, ram->size,
                                   ram->host);
        memory_region_add_subregion(sysmem, ram->base, &jit.ram[i]);
    }
    return true;
}

static bool jit_callbacks_complete(const Gem5QemuJitCallbacks *cb)
{
    return cb->mmio_read && cb->mmio_write && cb->read_time &&
           cb->batch_begin;
}

int gem5_qemu_jit_init(const Gem5QemuJitConfig *config, char *error,
                       size_t error_size)
{
    static const int signals[] = { SIGINT, SIGHUP, SIGTERM, SIGUSR1,
                                   SIGUSR2 };
    struct sigaction actions[ARRAY_SIZE(signals)];
    sigset_t mask;
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

    if (jit.initialized) {
        jit_error(error, error_size, "the backend is already initialised");
        return -1;
    }
    if (!config || config->abi_version != GEM5_QEMU_JIT_ABI_VERSION) {
        jit_error(error, error_size, "backend ABI %u does not match the "
                  "caller's %u", GEM5_QEMU_JIT_ABI_VERSION,
                  config ? config->abi_version : 0);
        return -1;
    }
    if (!config->hart_count || !config->isa || !config->ram_count ||
        !config->ram || !jit_callbacks_complete(&config->callbacks)) {
        jit_error(error, error_size, "incomplete backend configuration");
        return -1;
    }
    if (g_ascii_strcasecmp(config->isa, jit_contract_isa) &&
        g_ascii_strcasecmp(config->isa, jit_gem5_isa)) {
        jit_error(error, error_size,
                  "unsupported ISA '%s'; this backend requires '%s'",
                  config->isa, jit_contract_isa);
        return -1;
    }

    /*
     * qemu_init() installs QEMU's terminal signal handlers and blocks the
     * signals its main loop consumes. gem5 keeps its own; restore them.
     */
    for (size_t i = 0; i < ARRAY_SIZE(signals); i++) {
        sigaction(signals[i], NULL, &actions[i]);
    }
    pthread_sigmask(SIG_SETMASK, NULL, &mask);
    qemu_init(ARRAY_SIZE(argv), argv);
    for (size_t i = 0; i < ARRAY_SIZE(signals); i++) {
        sigaction(signals[i], &actions[i], NULL);
    }
    pthread_sigmask(SIG_SETMASK, &mask, NULL);
    /*
     * qemu_init() returns with the BQL and the replay mutex held. The BQL
     * stays with this thread (run_on_cpu() needs it); icount takes the
     * replay mutex around each batch.
     */
    replay_mutex_unlock();

    riscv_gem5_jit_enabled = true;
    jit.cb = config->callbacks;
    jit.hart_count = config->hart_count;
    jit.harts = g_new0(JitHart, jit.hart_count);
    for (uint32_t i = 0; i < jit.hart_count; i++) {
        JitHart *hart = &jit.harts[i];
        Object *obj = object_new(TYPE_RISCV_CPU_RV64I);
        Error *err = NULL;

        if (!jit_configure_cpu(obj, config, error, error_size)) {
            return -1;
        }
        /*
         * The hart ID must be distinct before realize: QEMU records per
         * mhartid which implied-extension rules it has applied.
         */
        RISCV_CPU(obj)->env.mhartid = i;
        if (!qdev_realize(DEVICE(obj), NULL, &err)) {
            jit_error(error, error_size, "cannot realise CPU: %s",
                      error_get_pretty(err));
            error_free(err);
            return -1;
        }
        hart->index = i;
        hart->cs = CPU(obj);
        hart->cpu = RISCV_CPU(obj);
        if (hart->cs->cpu_index != (int)i) {
            jit_error(error, error_size, "hart %u got cpu_index %d", i,
                      hart->cs->cpu_index);
            return -1;
        }
        if (!jit_verify_isa(hart->cpu, error, error_size)) {
            return -1;
        }
        riscv_cpu_set_rdtime_fn(&hart->cpu->env, jit_rdtime, hart);
    }

    if (!jit_map_memory(config, error, error_size)) {
        return -1;
    }
    jit.initialized = true;
    return 0;
}
