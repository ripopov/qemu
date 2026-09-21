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
 * Ownership. gem5 owns simulated time, RAM allocation, devices and
 * interrupt controllers. QEMU owns LR/SC reservations, instruction
 * execution, the architectural register file and CSRs, virtual-to-physical
 * translation, PMP/PMA checks and architectural traps. RAM is shared by
 * host pointer for writable ranges: translated loads and stores access
 * gem5's backing store directly. Everything else (read-only memory,
 * device registers, address holes, memory excluded from direct access)
 * is forwarded to gem5 as a physical access. Every callback runs on
 * QEMU's vCPU thread while the
 * gem5 thread is blocked inside gem5_qemu_jit_run(); gem5 code may run there
 * but nothing runs concurrently.
 */

#ifndef GEM5_QEMU_JIT_H
#define GEM5_QEMU_JIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump when any struct or function in this header changes incompatibly. */
#define GEM5_QEMU_JIT_ABI_VERSION 2

/*
 * One host-backed guest RAM range. Writable ranges require page-aligned
 * base, size and host. Entries with writable == 0 are not mapped; reads,
 * instruction fetches and writes use the background mmio_read/mmio_write
 * callbacks so gem5's memory owner applies its write permission.
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
 * mmio_read/mmio_write: physical access outside mapped writable RAM,
 * including read-only memory. Return 0 on success; any other value becomes
 * a guest access fault.
 * read_time: the value of the platform mtime counter for rdtime/time.
 * batch_begin: the vCPU thread is about to execute a batch for hart.
 */
typedef struct Gem5QemuJitCallbacks {
    void *opaque;
    int (*mmio_read)(void *opaque, uint32_t hart, uint64_t executed,
                     uint64_t paddr, uint8_t *data, unsigned size);
    int (*mmio_write)(void *opaque, uint32_t hart, uint64_t executed,
                      uint64_t paddr, const uint8_t *data, unsigned size);
    uint64_t (*read_time)(void *opaque, uint32_t hart, uint64_t executed);
    void (*batch_begin)(void *opaque, uint32_t hart);
} Gem5QemuJitCallbacks;

typedef struct Gem5QemuJitConfig {
    uint32_t abi_version;
    uint32_t hart_count;
    /*
     * Fixed demo ISA (RV64IMAFDCV plus the extensions in the adapter's
     * jit_contract_isa). Accepts that spelling or gem5 RiscvISA's sorted
     * spelling, case-insensitively; other configurations are rejected.
     * Every realised hart is checked against QEMU's expected ISA output.
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
 * Insert or remove a breakpoint. A batch that reaches pc stops before
 * executing the instruction there with GEM5_QEMU_JIT_EXIT_BREAKPOINT; the
 * next batch steps over it.
 */
int gem5_qemu_jit_breakpoint(uint32_t hart, uint64_t pc, int insert);

#ifdef __cplusplus
}
#endif

#endif /* GEM5_QEMU_JIT_H */
