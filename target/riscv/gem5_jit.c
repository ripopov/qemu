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
 * the run. Callbacks into gem5 therefore execute on the vCPU thread, either
 * with the BQL held (device accesses) or without it (LR/SC helpers and the
 * store guard); all of them are serialised with respect to the gem5 thread.
 */

#include "qemu/osdep.h"

#include <signal.h>

#include "gem5/qemu-jit.h"
#include "gem5_jit.h"

#include "accel/tcg/probe.h"
#include "accel/tcg/tcg-accel-ops.h"
#include "accel/tcg/tcg-accel-ops-icount.h"
#include "cpu.h"
#include "cpu-qom.h"
#include "exec/cputlb.h"
#include "exec/icount.h"
#include "exec/ramlist.h"
#include "exec/target_page.h"
#include "exec/tlb-flags.h"
#include "exec/translation-block.h"
#include "hw/core/cpu.h"
#include "hw/qdev-core.h"
#include "qapi/error.h"
#include "qemu-main.h"
#include "qemu/bitmap.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "system/gem5-jit.h"
#include "system/memory.h"
#include "system/physmem.h"
#include "system/replay.h"
#include "system/system.h"

/* system/main.c owns this in an emulator; UI backends still reference it. */
int (*qemu_main)(void);

typedef struct JitRam {
    Gem5QemuJitRam cfg;
    MemoryRegion mr;
    ram_addr_t ram_addr;
} JitRam;

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
    JitRam *ram;
    size_t ram_count;
    MemoryRegion io;
    char *isa_string;
    /* The batch being executed on the vCPU thread, if any. */
    JitHart *batch_hart;
    int64_t batch_start;
    /* An LR/SC translation probe is running: do not report it as a store. */
    bool probing;
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

static JitRam *jit_ram_from_paddr(uint64_t paddr)
{
    for (size_t i = 0; i < jit.ram_count; i++) {
        JitRam *ram = &jit.ram[i];
        if (paddr - ram->cfg.base < ram->cfg.size) {
            return ram;
        }
    }
    return NULL;
}

static JitRam *jit_ram_from_ram_addr(ram_addr_t ram_addr)
{
    for (size_t i = 0; i < jit.ram_count; i++) {
        JitRam *ram = &jit.ram[i];
        if (ram_addr - ram->ram_addr < ram->cfg.size) {
            return ram;
        }
    }
    return NULL;
}

/*
 * gem5 wrote size bytes at paddr behind QEMU's back (a store-conditional).
 * Translated code that covered them is stale. The running translation
 * block is deliberately not interrupted: it must complete the SC.
 */
static void jit_invalidate_code(uint64_t paddr, unsigned size)
{
    JitRam *ram = jit_ram_from_paddr(paddr);

    if (ram) {
        ram_addr_t start = ram->ram_addr + (paddr - ram->cfg.base);
        tb_invalidate_phys_range(NULL, start, start + size - 1);
    }
}

/* Physical accesses QEMU cannot serve from mapped RAM go to gem5. */
static MemTxResult jit_io_read(void *opaque, hwaddr addr, uint64_t *value,
                               unsigned size, MemTxAttrs attrs)
{
    JitRam *ram = opaque;
    uint64_t paddr = ram ? ram->cfg.base + addr : addr;
    uint8_t data[8] = { 0 };
    uint64_t result = 0;

    if (size > sizeof(data) ||
        jit.cb.mmio_read(jit.cb.opaque, jit_current_hart(), jit_executed(),
                         paddr, data, size) != 0) {
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
    JitRam *ram = opaque;
    uint64_t paddr = ram ? ram->cfg.base + addr : addr;
    uint8_t data[8];

    if (size > sizeof(data)) {
        return MEMTX_ERROR;
    }
    for (unsigned i = 0; i < size; i++) {
        data[i] = value >> (8 * i);
    }
    if (jit.cb.mmio_write(jit.cb.opaque, jit_current_hart(), jit_executed(),
                          paddr, data, size) != 0) {
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

/*
 * Store guard. A guarded page is clean for the VGA dirty-memory client,
 * which nothing else uses here: TLB entries filled for it carry
 * TLB_NOTDIRTY, so every store takes the not-dirty slow path and reaches
 * jit_store_hook() before the bytes are written. The hook releases the
 * guard when gem5 no longer needs it by letting notdirty_write() mark the
 * page dirty again.
 */
static void jit_page_set_clean(JitRam *ram, uint64_t paddr)
{
    ram_addr_t page = (ram->ram_addr + (paddr - ram->cfg.base))
                      >> TARGET_PAGE_BITS;
    DirtyMemoryBlocks *blocks;

    WITH_RCU_READ_LOCK_GUARD() {
        blocks = qatomic_rcu_read(&ram_list.dirty_memory[DIRTY_MEMORY_VGA]);
        bitmap_test_and_clear_atomic(
            blocks->blocks[page / DIRTY_MEMORY_BLOCK_SIZE],
            page % DIRTY_MEMORY_BLOCK_SIZE, 1);
    }
}

void gem5_qemu_jit_guard_page(uint32_t hart_index, uint64_t paddr)
{
    JitHart *hart = jit_hart(hart_index);
    JitRam *ram = jit_ram_from_paddr(paddr);
    uintptr_t host;

    if (!hart || !ram || !ram->cfg.writable) {
        return;
    }
    jit_page_set_clean(ram, paddr);
    /* Existing translations of the page in this hart's TLB lose their fast
     * store path; any virtual alias is covered. */
    host = (uintptr_t)ram->cfg.host + (paddr - ram->cfg.base);
    tlb_reset_dirty(hart->cs, host & ~(uintptr_t)(TARGET_PAGE_SIZE - 1),
                    TARGET_PAGE_SIZE);
}

/*
 * LR and SC (target/riscv/gem5_helper.c forwards the helpers here).
 *
 * Translate the address the way the translated load or store would:
 * misalignment, page and PMP faults are raised with the same cause and
 * tval. The access itself is performed by gem5, so the translation probe
 * must not act as a store: every probe variant runs the not-dirty slow path
 * for a guarded page, which would report the SC as a plain store and
 * invalidate the very reservation it is about to consume. The report is
 * suppressed around the non-faulting probe; only when that cannot translate
 * is the faulting probe run, which raises the trap before any side effect.
 */
static hwaddr jit_translate(CPURISCVState *env, vaddr addr, unsigned size,
                            MMUAccessType type, uintptr_t ra)
{
    int mmu_idx = riscv_env_mmu_index(env, false);
    CPUTLBEntryFull *full;
    void *host;
    int flags;

    if (addr & (size - 1)) {
        riscv_cpu_do_unaligned_access(env_cpu(env), addr, type, mmu_idx, ra);
    }
    jit.probing = true;
    flags = probe_access_full_mmu(env, addr, size, type, mmu_idx, &host,
                                  &full);
    jit.probing = false;
    if (unlikely(flags & TLB_INVALID_MASK)) {
        probe_access_full(env, addr, size, type, mmu_idx, false, &host,
                          &full, ra);
        g_assert_not_reached();
    }
    return full->phys_addr | (addr & ~TARGET_PAGE_MASK);
}

static G_NORETURN void jit_access_fault(CPURISCVState *env, vaddr addr,
                                        MMUAccessType type, uintptr_t ra)
{
    env->badaddr = addr;
    env->two_stage_lookup = false;
    env->two_stage_indirect_lookup = false;
    riscv_raise_exception(env, type == MMU_DATA_STORE ?
                          RISCV_EXCP_STORE_AMO_ACCESS_FAULT :
                          RISCV_EXCP_LOAD_ACCESS_FAULT, ra);
}

static target_ulong jit_load_reserved(CPURISCVState *env, vaddr addr,
                                      MemOp mop, uintptr_t ra)
{
    const unsigned size = memop_size(mop);
    hwaddr paddr = jit_translate(env, addr, size, MMU_DATA_LOAD, ra);
    uint64_t value = 0;
    JitRam *ram;

    if (jit.cb.load_reserved(jit.cb.opaque, jit_current_hart(),
                             jit_executed(), paddr, size, &value) != 0) {
        jit_access_fault(env, addr, MMU_DATA_LOAD, ra);
    }
    ram = jit_ram_from_paddr(paddr);
    if (ram && ram->cfg.writable) {
        /*
         * Guard the reserved page for this hart. Only its translation of the
         * LR's own virtual page is flushed (a TLB scan per LR would cost
         * more than the LR/SC pair itself); other harts and other aliases
         * are guarded before the next batch by gem5_qemu_jit_guard_page().
         */
        jit_page_set_clean(ram, paddr);
        tlb_flush_page(env_cpu(env), addr);
    }
    if (mop & MO_SIGN) {
        value = sextract64(value, 0, size * 8);
    }
    return value;
}

static target_ulong jit_store_conditional(CPURISCVState *env, vaddr addr,
                                          target_ulong value, MemOp mop,
                                          uintptr_t ra)
{
    const unsigned size = memop_size(mop);
    hwaddr paddr = jit_translate(env, addr, size, MMU_DATA_STORE, ra);
    int result = jit.cb.store_conditional(jit.cb.opaque, jit_current_hart(),
                                          jit_executed(), paddr, size, value);

    if (result < 0) {
        jit_access_fault(env, addr, MMU_DATA_STORE, ra);
    }
    if (result == 0) {
        jit_invalidate_code(paddr, size);
    }
    return result ? 1 : 0;
}

static const RISCVGem5Ops jit_riscv_ops = {
    .load_reserved = jit_load_reserved,
    .store_conditional = jit_store_conditional,
};

/* accel/tcg/cputlb.c not-dirty slow path, before the store is performed */
static bool jit_store_hook(ram_addr_t ram_addr, unsigned size)
{
    JitRam *ram;

    /* Pages that are clean only for the code client are not guarded. */
    if (physical_memory_get_dirty_flag(ram_addr, DIRTY_MEMORY_VGA)) {
        return false;
    }
    /* An LR/SC translation probe: keep the guard, report nothing. */
    if (jit.probing) {
        return true;
    }
    ram = jit_ram_from_ram_addr(ram_addr);
    if (!ram) {
        return false;
    }
    return jit.cb.store_notify(jit.cb.opaque, jit_current_hart(),
                               jit_executed(),
                               ram->cfg.base + (ram_addr - ram->ram_addr),
                               size) != 0;
}

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
            res->m5op = env->gem5_m5op;
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
 * Split an ISA string into lower-case extension names: single letters after
 * the base and underscore-separated multi-letter extensions. "g" expands
 * to its members. The base ("rv64i" or "rv64e") itself is not returned.
 */
static GPtrArray *jit_isa_tokens(const char *isa, char *error,
                                 size_t error_size)
{
    GPtrArray *tokens = g_ptr_array_new_with_free_func(g_free);
    const char *p = isa;

    if (g_ascii_strncasecmp(p, "rv64", 4) != 0) {
        jit_error(error, error_size, "ISA string '%s' is not rv64", isa);
        g_ptr_array_free(tokens, true);
        return NULL;
    }
    p += 4;
    if (*p != 'i' && *p != 'e' && *p != 'g' && *p != 'I' && *p != 'E' &&
        *p != 'G') {
        jit_error(error, error_size, "ISA string '%s' has no base", isa);
        g_ptr_array_free(tokens, true);
        return NULL;
    }
    for (; *p && *p != '_'; p++) {
        char c = g_ascii_tolower(*p);
        if (c == 'i' || c == 'e') {
            continue;
        }
        if (c == 'g') {
            const char *members[] = { "m", "a", "f", "d", "zicsr",
                                      "zifencei" };
            for (size_t i = 0; i < ARRAY_SIZE(members); i++) {
                g_ptr_array_add(tokens, g_strdup(members[i]));
            }
            continue;
        }
        g_ptr_array_add(tokens, g_strdup_printf("%c", c));
    }
    while (*p == '_') {
        const char *end = strchr(++p, '_');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (!len) {
            jit_error(error, error_size, "ISA string '%s' has an empty "
                      "extension", isa);
            g_ptr_array_free(tokens, true);
            return NULL;
        }
        g_ptr_array_add(tokens, g_ascii_strdown(p, len));
        p += len;
    }
    return tokens;
}

static bool jit_tokens_contain(GPtrArray *tokens, const char *name)
{
    for (guint i = 0; i < tokens->len; i++) {
        if (!strcmp(g_ptr_array_index(tokens, i), name)) {
            return true;
        }
    }
    return false;
}

static const RISCVIsaExtData *jit_isa_edata(const char *name)
{
    for (const RISCVIsaExtData *edata = isa_edata_arr; edata->name;
         edata++) {
        if (!strcmp(edata->name, name)) {
            return edata;
        }
    }
    return NULL;
}

/*
 * Extensions QEMU reports that a contract may leave out because they are
 * implied by members it does contain (Zca/Zcd by C+D, Zaamo/Zalrsc by A,
 * Zmmul by M, Zve* by V, Zksed/Zksh by Zks) or because QEMU derives them
 * from the privileged specification version rather than from a property.
 * xgem5 is this adapter's own hook, not guest ISA. Anything else QEMU
 * enables beyond the contract is an error.
 */
static const char *const jit_implied_extensions[] = {
    "xgem5",
    "zca", "zcd", "zcf", "zaamo", "zalrsc", "zmmul",
    "zve32f", "zve32x", "zve64d", "zve64f", "zve64x",
    "zksed", "zksh",
    "ziccamoa", "ziccif", "zicclsm", "ziccrse", "za64rs",
    "shcounterenw", "shgatpa", "shtvala", "shvsatpa", "shvstvala",
    "shvstvecd", "ssccptr", "sscounterenw", "ssstrict", "sstvala",
    "sstvecd", "ssu64xl",
};

/*
 * Contract members QEMU implements unconditionally and never names in its
 * ISA string: Svbare (satp mode Bare) and the paging modes, which QEMU
 * configures through the sv39/sv48/sv57 properties and a device tree
 * advertises as mmu-type.
 */
static bool jit_is_inherent(const char *name)
{
    return !strcmp(name, "svbare") || !strcmp(name, "sv39") ||
           !strcmp(name, "sv48") || !strcmp(name, "sv57");
}

static bool jit_is_implied(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(jit_implied_extensions); i++) {
        if (!strcmp(jit_implied_extensions[i], name)) {
            return true;
        }
    }
    return false;
}

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
                              GPtrArray *tokens, char *error,
                              size_t error_size)
{
    /*
     * Machine, supervisor and user modes; the contract has no H. No debug
     * triggers (Sdtrig) either: gem5 has none, and the contract says so.
     */
    if (!jit_set_bool(obj, "s", true, error, error_size) ||
        !jit_set_bool(obj, "u", true, error, error_size) ||
        !jit_set_bool(obj, "mmu", true, error, error_size) ||
        !jit_set_bool(obj, "pmp", true, error, error_size) ||
        !jit_set_bool(obj, "debug", false, error, error_size) ||
        !jit_set_bool(obj, "xgem5", true, error, error_size) ||
        !jit_set_uint(obj, "num-pmp-regions", config->pmp_regions, error,
                      error_size) ||
        !jit_set_uint(obj, "resetvec", config->reset_pc, error,
                      error_size) ||
        !jit_set_uint(obj, "mvendorid", config->mvendorid, error,
                      error_size) ||
        !jit_set_uint(obj, "marchid", config->marchid, error, error_size) ||
        !jit_set_uint(obj, "mimpid", config->mimpid, error, error_size)) {
        return false;
    }
    /* Paging modes gem5's walker implements: Sv39, Sv48 and Sv57. */
    if (!jit_set_bool(obj, "sv57", true, error, error_size)) {
        return false;
    }
    for (guint i = 0; i < tokens->len; i++) {
        const char *name = g_ptr_array_index(tokens, i);

        if (object_property_find(obj, name)) {
            if (!jit_set_bool(obj, name, true, error, error_size)) {
                return false;
            }
        } else if (!jit_isa_edata(name) && !jit_is_inherent(name)) {
            jit_error(error, error_size, "unknown RISC-V extension '%s'",
                      name);
            return false;
        }
        /* Otherwise QEMU derives it from the privileged specification. */
    }
    if (jit_tokens_contain(tokens, "v")) {
        if (!jit_set_uint(obj, "vlen", config->vlen, error, error_size) ||
            !jit_set_uint(obj, "elen", config->elen, error, error_size)) {
            return false;
        }
    }
    if (jit_tokens_contain(tokens, "zicbom") &&
        !jit_set_uint(obj, "cbom_blocksize", config->cache_line_size, error,
                      error_size)) {
        return false;
    }
    if (jit_tokens_contain(tokens, "zicbop") &&
        !jit_set_uint(obj, "cbop_blocksize", config->cache_line_size, error,
                      error_size)) {
        return false;
    }
    if (jit_tokens_contain(tokens, "zicboz") &&
        !jit_set_uint(obj, "cboz_blocksize", config->cache_line_size, error,
                      error_size)) {
        return false;
    }
    if (jit_tokens_contain(tokens, "sscofpmf") &&
        !jit_set_uint(obj, "pmu-mask", MAKE_64BIT_MASK(3, 29), error,
                      error_size)) {
        return false;
    }
    return true;
}

static bool jit_verify_isa(RISCVCPU *cpu, GPtrArray *tokens,
                           const char *contract, char *error,
                           size_t error_size)
{
    GPtrArray *enabled;

    jit.isa_string = riscv_isa_string(cpu);
    enabled = jit_isa_tokens(jit.isa_string, error, error_size);
    if (!enabled) {
        return false;
    }
    for (guint i = 0; i < tokens->len; i++) {
        const char *name = g_ptr_array_index(tokens, i);

        if (!jit_tokens_contain(enabled, name) && !jit_is_inherent(name)) {
            jit_error(error, error_size, "QEMU did not enable '%s' from "
                      "'%s'; it reports '%s'", name, contract,
                      jit.isa_string);
            g_ptr_array_free(enabled, true);
            return false;
        }
    }
    for (guint i = 0; i < enabled->len; i++) {
        const char *name = g_ptr_array_index(enabled, i);

        if (!jit_tokens_contain(tokens, name) && !jit_is_implied(name)) {
            jit_error(error, error_size, "QEMU enables '%s', which '%s' "
                      "does not advertise; it reports '%s'", name, contract,
                      jit.isa_string);
            g_ptr_array_free(enabled, true);
            return false;
        }
    }
    g_ptr_array_free(enabled, true);
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

    jit.ram_count = config->ram_count;
    jit.ram = g_new0(JitRam, jit.ram_count);
    for (size_t i = 0; i < jit.ram_count; i++) {
        JitRam *ram = &jit.ram[i];
        g_autofree char *name = g_strdup_printf("gem5-ram-%zu", i);

        ram->cfg = config->ram[i];
        if (!ram->cfg.size || (ram->cfg.base | ram->cfg.size) % page ||
            (uintptr_t)ram->cfg.host % page) {
            jit_error(error, error_size, "RAM [0x%" PRIx64 ", 0x%" PRIx64
                      ") at host %p is not page aligned", ram->cfg.base,
                      ram->cfg.base + ram->cfg.size, ram->cfg.host);
            return false;
        }
        if (ram->cfg.writable) {
            memory_region_init_ram_ptr(&ram->mr, NULL, name, ram->cfg.size,
                                       ram->cfg.host);
        } else {
            memory_region_init_rom_device_ptr(&ram->mr, NULL, &jit_io_ops,
                                              ram, name, ram->cfg.size,
                                              ram->cfg.host);
        }
        memory_region_add_subregion(sysmem, ram->cfg.base, &ram->mr);
        ram->ram_addr = memory_region_get_ram_addr(&ram->mr);
    }
    return true;
}

static bool jit_callbacks_complete(const Gem5QemuJitCallbacks *cb)
{
    return cb->mmio_read && cb->mmio_write && cb->load_reserved &&
           cb->store_conditional && cb->store_notify && cb->read_time &&
           cb->batch_begin;
}

int gem5_qemu_jit_init(const Gem5QemuJitConfig *config, char *error,
                       size_t error_size)
{
    static const int signals[] = { SIGINT, SIGHUP, SIGTERM, SIGUSR1,
                                   SIGUSR2 };
    struct sigaction actions[ARRAY_SIZE(signals)];
    sigset_t mask;
    GPtrArray *tokens;
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
    tokens = jit_isa_tokens(config->isa, error, error_size);
    if (!tokens) {
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

    jit.cb = config->callbacks;
    jit.hart_count = config->hart_count;
    jit.harts = g_new0(JitHart, jit.hart_count);
    for (uint32_t i = 0; i < jit.hart_count; i++) {
        JitHart *hart = &jit.harts[i];
        Object *obj = object_new(TYPE_RISCV_CPU_RV64I);
        Error *err = NULL;

        if (!jit_configure_cpu(obj, config, tokens, error, error_size)) {
            g_ptr_array_free(tokens, true);
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
            g_ptr_array_free(tokens, true);
            return -1;
        }
        hart->index = i;
        hart->cs = CPU(obj);
        hart->cpu = RISCV_CPU(obj);
        if (hart->cs->cpu_index != (int)i) {
            jit_error(error, error_size, "hart %u got cpu_index %d", i,
                      hart->cs->cpu_index);
            g_ptr_array_free(tokens, true);
            return -1;
        }
        riscv_cpu_set_rdtime_fn(&hart->cpu->env, jit_rdtime, hart);
    }
    if (!jit_verify_isa(jit.harts[0].cpu, tokens, config->isa, error,
                        error_size)) {
        g_ptr_array_free(tokens, true);
        return -1;
    }
    g_ptr_array_free(tokens, true);

    if (!jit_map_memory(config, error, error_size)) {
        return -1;
    }
    riscv_gem5_ops = &jit_riscv_ops;
    gem5_jit_store_hook = jit_store_hook;
    jit.initialized = true;
    return 0;
}
