/*
 * gem5 RiscvJitCPU backend interface
 *
 * Copyright (c) 2026 Roman Popov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This header is the whole contract between gem5's RiscvJitCPU and
 * libgem5-qemu-jit, the shared library built from this QEMU tree with
 * -Dgem5_jit=true. It deliberately uses only C types: gem5 loads the
 * library with dlopen(3) and never includes QEMU headers.
 *
 * Ownership. gem5 owns simulated time, RAM allocation, devices,
 * interrupt controllers and LR/SC reservations. QEMU owns instruction
 * execution, the architectural register file and CSRs, virtual-to-physical
 * translation, PMP/PMA checks and architectural traps. RAM is shared by
 * host pointer: translated loads and stores read and write gem5's backing
 * store directly. Everything that is not mapped RAM (device registers,
 * address holes, memory excluded from direct access) is forwarded to gem5
 * as a physical access. Every callback runs on QEMU's vCPU thread while the
 * gem5 thread is blocked inside gem5_qemu_jit_run(); gem5 code may run there
 * but nothing runs concurrently.
 *
 * Reservations. LR and SC never execute in translated code. They are
 * forwarded to gem5 (load_reserved and store_conditional) with the physical
 * address QEMU translated, so gem5's memory decides SC success exactly as it
 * does for its own CPU models, with the same reservation granule. A page
 * that holds a reservation is guarded: translated stores to it report their
 * physical address through store_notify before the bytes are written, which
 * lets gem5 invalidate overlapping reservations even when a store writes the
 * value already present. An LR guards its page for the reserving hart; gem5
 * guards every reserved page for a hart before running it
 * (gem5_qemu_jit_guard_page). A guard is released by the first reported
 * store gem5 answers with 0, so pages without reservations run at full
 * speed. Stores through a virtual alias the reserving hart already had a
 * translation for are not reported until its next batch.
 */

#ifndef GEM5_QEMU_JIT_H
#define GEM5_QEMU_JIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump when any struct or function in this header changes incompatibly. */
#define GEM5_QEMU_JIT_ABI_VERSION 1

/*
 * One host-backed guest RAM range. base, size and host must be page
 * aligned. A range with writable == 0 is mapped read-only: loads come from
 * host memory, stores are forwarded through mmio_write so gem5's memory
 * owner applies its write permission.
 */
typedef struct Gem5QemuJitRam {
    uint64_t base;
    uint64_t size;
    void *host;
    int writable;
} Gem5QemuJitRam;

/*
 * Callbacks into gem5. Every callback names the hart that caused it and
 * the number of instructions that hart had completed in the current batch
 * when the callback was made, so gem5 can advance simulated time to the
 * instruction that performs a device access.
 *
 * mmio_read/mmio_write: physical access outside mapped RAM (or a store to
 * read-only RAM). Return 0 on success; any other value becomes a guest
 * access fault.
 * load_reserved: LR of size bytes at paddr. Return 0 and the loaded value,
 * or nonzero for a guest access fault.
 * store_conditional: SC of size bytes at paddr. Return 0 when the store
 * was performed, 1 when the reservation was gone (nothing written), or a
 * negative value for a guest access fault.
 * store_notify: a translated store of size bytes at paddr is about to be
 * performed to a guarded page. Return nonzero to keep the page guarded,
 * 0 to release it.
 * read_time: the value of the platform mtime counter for rdtime/time.
 * batch_begin: the vCPU thread is about to execute a batch for hart.
 */
typedef struct Gem5QemuJitCallbacks {
    void *opaque;
    int (*mmio_read)(void *opaque, uint32_t hart, uint64_t executed,
                     uint64_t paddr, uint8_t *data, unsigned size);
    int (*mmio_write)(void *opaque, uint32_t hart, uint64_t executed,
                      uint64_t paddr, const uint8_t *data, unsigned size);
    int (*load_reserved)(void *opaque, uint32_t hart, uint64_t executed,
                         uint64_t paddr, unsigned size, uint64_t *value);
    int (*store_conditional)(void *opaque, uint32_t hart, uint64_t executed,
                             uint64_t paddr, unsigned size, uint64_t value);
    int (*store_notify)(void *opaque, uint32_t hart, uint64_t executed,
                        uint64_t paddr, unsigned size);
    uint64_t (*read_time)(void *opaque, uint32_t hart, uint64_t executed);
    void (*batch_begin)(void *opaque, uint32_t hart);
} Gem5QemuJitCallbacks;

typedef struct Gem5QemuJitConfig {
    uint32_t abi_version;
    uint32_t hart_count;
    /*
     * The ISA every hart implements, as a RISC-V ISA string
     * ("rv64imafdcv_zic64b_..."). Initialisation fails unless QEMU enables
     * exactly this set: see gem5_qemu_jit_init() for the extensions QEMU
     * always reports in addition.
     */
    const char *isa;
    uint32_t vlen;
    uint32_t elen;
    uint32_t cache_line_size; /* Zicbom/Zicbop/Zicboz block size */
    uint32_t pmp_regions;
    uint32_t mvendorid;
    uint64_t marchid;
    uint64_t mimpid;
    uint64_t reset_pc;
    const Gem5QemuJitRam *ram;
    size_t ram_count;
    Gem5QemuJitCallbacks callbacks;
} Gem5QemuJitConfig;

enum Gem5QemuJitExit {
    GEM5_QEMU_JIT_EXIT_BUDGET = 0,   /* the instruction budget was used */
    GEM5_QEMU_JIT_EXIT_STOPPED = 1,  /* stopped early, e.g. request_exit */
    GEM5_QEMU_JIT_EXIT_HALTED = 2,   /* WFI with no enabled pending interrupt */
    GEM5_QEMU_JIT_EXIT_M5OP = 3,     /* a gem5 pseudo instruction (m5op) */
    GEM5_QEMU_JIT_EXIT_BREAKPOINT = 4, /* reached a breakpoint pc */
    GEM5_QEMU_JIT_EXIT_ERROR = 5,
};

typedef struct Gem5QemuJitRunResult {
    uint64_t instructions;  /* instructions completed by this call */
    int exit;               /* enum Gem5QemuJitExit */
    int qemu_code;          /* QEMU's cpu_exec() return value, for ERROR */
    uint32_t m5op;          /* the full 32-bit encoding, for M5OP */
} Gem5QemuJitRunResult;

enum Gem5QemuJitReg {
    GEM5_QEMU_JIT_REG_PC,
    GEM5_QEMU_JIT_REG_PRIV,
    GEM5_QEMU_JIT_REG_MSTATUS,
    GEM5_QEMU_JIT_REG_SATP,
};

/* Interrupt lines the platform drives, as mip bits. */
#define GEM5_QEMU_JIT_IRQ_SSIP (1u << 1)
#define GEM5_QEMU_JIT_IRQ_MSIP (1u << 3)
#define GEM5_QEMU_JIT_IRQ_STIP (1u << 5)
#define GEM5_QEMU_JIT_IRQ_MTIP (1u << 7)
#define GEM5_QEMU_JIT_IRQ_SEIP (1u << 9)
#define GEM5_QEMU_JIT_IRQ_MEIP (1u << 11)

/*
 * Initialise QEMU once for the whole process and create hart_count vCPUs
 * in reset state at reset_pc, with the requested ISA, RAM map and
 * callbacks. Returns 0, or -1 with a message in error. The caller must
 * keep calling this library from the thread that called init.
 */
int gem5_qemu_jit_init(const Gem5QemuJitConfig *config, char *error,
                       size_t error_size);

/* QEMU's own ISA string for the created harts. Valid after init. */
const char *gem5_qemu_jit_isa_string(void);

/*
 * Execute up to budget instructions on hart. Instructions that trap
 * complete and the trap is taken inside the batch; only the exits above
 * return early. Returns 0, or -1 for a bad argument.
 */
int gem5_qemu_jit_run(uint32_t hart, uint64_t budget,
                      Gem5QemuJitRunResult *result);

/*
 * From inside a callback: end the running batch at the current
 * instruction boundary. The result reports GEM5_QEMU_JIT_EXIT_STOPPED.
 */
void gem5_qemu_jit_request_exit(uint32_t hart);

uint64_t gem5_qemu_jit_get_gpr(uint32_t hart, unsigned index);
void gem5_qemu_jit_set_gpr(uint32_t hart, unsigned index, uint64_t value);
uint64_t gem5_qemu_jit_get_reg(uint32_t hart, enum Gem5QemuJitReg reg);
void gem5_qemu_jit_set_pc(uint32_t hart, uint64_t pc);

/*
 * Set the level of the platform interrupt lines (MEIP, MTIP, MSIP and the
 * external SEIP) of hart from the GEM5_QEMU_JIT_IRQ_* bits in pending.
 * Other bits are ignored: the supervisor software-pending bits belong to
 * the guest running in QEMU. A halted hart wakes up if the new state has
 * an enabled pending interrupt.
 */
void gem5_qemu_jit_set_interrupts(uint32_t hart, uint64_t pending);

/*
 * Guard the RAM page containing paddr for hart: its translated stores to
 * the page are reported through store_notify until one is answered with 0.
 * Cheap for a page that is already guarded.
 */
void gem5_qemu_jit_guard_page(uint32_t hart, uint64_t paddr);

/*
 * Insert or remove a breakpoint. A batch that reaches pc stops before
 * executing the instruction there with GEM5_QEMU_JIT_EXIT_BREAKPOINT; the
 * next batch steps over it.
 */
int gem5_qemu_jit_breakpoint(uint32_t hart, uint64_t pc, int insert);

#ifdef __cplusplus
}
#endif

#endif /* GEM5_QEMU_JIT_H */
