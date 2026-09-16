/*
 * QEMU RISC-V TCG adapter for gem5 JitCPU
 *
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0-or-later
 */

#ifndef GEM5_QEMU_JIT_H
#define GEM5_QEMU_JIT_H

#include <stddef.h>
#include <stdint.h>

#define GEM5_QEMU_JIT_ABI_VERSION 3

#if defined(__GNUC__)
#define GEM5_QEMU_JIT_API __attribute__((visibility("default")))
#else
#define GEM5_QEMU_JIT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*Gem5QemuJitMemoryRead)(void *opaque, uint64_t address,
                                     uint8_t *data, size_t size);
typedef int (*Gem5QemuJitMemoryWrite)(void *opaque, uint64_t address,
                                      const uint8_t *data, size_t size);
typedef int (*Gem5QemuJitMemoryMap)(void *opaque, size_t index,
                                    uint64_t *guest_address, uint64_t *size,
                                    uint8_t **host_address, int *writable);
typedef uint64_t (*Gem5QemuJitReadTime)(void *opaque);
typedef int (*Gem5QemuJitShouldStop)(void *opaque);
/*
 * Host-owned physical LR/SC monitor. SC returns 0 on success, 1 on failure.
 * Return a nonzero callback status only for a physical access fault.
 */
typedef int (*Gem5QemuJitReservation)(void *opaque, uint64_t address,
                                    uint64_t value, unsigned size, int store,
                                    uint64_t *result);

typedef struct Gem5QemuJitCallbacks {
    uint32_t abi_version;
    uint32_t instance_id;
    uint32_t instance_count;
    uint64_t hart_id;
    Gem5QemuJitMemoryRead memory_read;
    Gem5QemuJitMemoryWrite memory_write;
    Gem5QemuJitMemoryMap memory_map;
    Gem5QemuJitReadTime read_time;
    /* Host simulation cycles, including time while this hart sleeps. */
    Gem5QemuJitReadTime read_cycles;
    Gem5QemuJitShouldStop should_stop;
    Gem5QemuJitReservation reservation;
    void *opaque;
} Gem5QemuJitCallbacks;

enum Gem5QemuJitExitReason {
    GEM5_QEMU_JIT_EXIT_BUDGET = 0,
    GEM5_QEMU_JIT_EXIT_HALTED = 1,
    GEM5_QEMU_JIT_EXIT_INTERRUPT = 2,
    GEM5_QEMU_JIT_EXIT_EXCEPTION = 3,
    GEM5_QEMU_JIT_EXIT_ERROR = 4,
    GEM5_QEMU_JIT_EXIT_M5OP = 5,
    GEM5_QEMU_JIT_EXIT_PC_EVENT = 6,
};

typedef struct Gem5QemuJitRunResult {
    /* Attempted instructions consume the execution/time budget. */
    uint64_t instructions;
    uint64_t retired;
    int qemu_exception;
    enum Gem5QemuJitExitReason reason;
    uint32_t m5_function;
    /*
     * Architectural snapshot at the batch boundary, without per-register
     * crossings of the shared-library interface. x0 is always zero.
     */
    uint64_t gprs[32];
    uint64_t pc;
    unsigned privilege;
} Gem5QemuJitRunResult;

/*
 * One backend image owns all embedded QEMU vCPUs. Each instance ID selects
 * independent architectural, software-TLB, and transient state for one
 * JitCPU; the QEMU runtime, physical address space, and TCG engine are shared.
 * Initialization, execution and state access must use the same host thread.
 * Callbacks run synchronously on that thread; there is no vCPU worker.
 */
GEM5_QEMU_JIT_API int gem5_qemu_jit_init(
    const Gem5QemuJitCallbacks *callbacks);
/* Resolved QEMU ISA, including implied extensions; owned by the backend. */
GEM5_QEMU_JIT_API const char *gem5_qemu_jit_get_isa(uint32_t instance_id);
/*
 * Poll overflow flags and return remaining cycle/retirement deadlines.
 * UINT64_MAX means no deadline within the representable interval.
 */
GEM5_QEMU_JIT_API int gem5_qemu_jit_pmu_poll(uint32_t instance_id,
    uint64_t *cycles, uint64_t *instructions);
GEM5_QEMU_JIT_API int gem5_qemu_jit_run(
    uint32_t instance_id, uint64_t max_instructions,
    Gem5QemuJitRunResult *result);
/*
 * Replace host PC-event breakpoints between runs. A PC-event exit occurs
 * before executing/counting its instruction. The next run at that same PC
 * executes one instruction past the serviced event, then restores it.
 */
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_pc_events(
    uint32_t instance_id, const uint64_t *addresses, size_t count);
/*
 * Acknowledge the final PC after the host services an event chain and
 * imports its state changes. That destination has already been serviced.
 */
GEM5_QEMU_JIT_API int gem5_qemu_jit_pc_event_serviced(uint32_t instance_id);
/*
 * Functional/debug translation using QEMU's canonical privilege/SATP state.
 * No guest instruction executes, and no PTE A/D bits are modified.
 */
GEM5_QEMU_JIT_API int gem5_qemu_jit_translate_debug(
    uint32_t instance_id, uint64_t address, uint64_t *physical);

/*
 * Only from an execution callback: count through the current instruction.
 * Returns zero outside execution, UINT64_MAX for an invalid query.
 */
GEM5_QEMU_JIT_API uint64_t gem5_qemu_jit_executed(uint32_t instance_id);

GEM5_QEMU_JIT_API uint64_t gem5_qemu_jit_get_gpr(
    uint32_t instance_id, unsigned index);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_gpr(
    uint32_t instance_id, unsigned index, uint64_t value);
GEM5_QEMU_JIT_API uint64_t gem5_qemu_jit_get_fpr(
    uint32_t instance_id, unsigned index);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_fpr(
    uint32_t instance_id, unsigned index, uint64_t value);
GEM5_QEMU_JIT_API uint64_t gem5_qemu_jit_get_pc(uint32_t instance_id);
GEM5_QEMU_JIT_API void gem5_qemu_jit_set_pc(
    uint32_t instance_id, uint64_t value);
GEM5_QEMU_JIT_API unsigned gem5_qemu_jit_get_priv(uint32_t instance_id);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_priv(
    uint32_t instance_id, unsigned value);
GEM5_QEMU_JIT_API int gem5_qemu_jit_get_csr(
    uint32_t instance_id, unsigned csr, uint64_t *value);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_csr(
    uint32_t instance_id, unsigned csr, uint64_t value);
GEM5_QEMU_JIT_API uint64_t gem5_qemu_jit_get_mip(uint32_t instance_id);
GEM5_QEMU_JIT_API void gem5_qemu_jit_set_mip(
    uint32_t instance_id, uint64_t value);
/* Also clears software-TLB, halted, exception, and LR/SC transient state. */
GEM5_QEMU_JIT_API void gem5_qemu_jit_invalidate_translations(
    uint32_t instance_id);

#ifdef __cplusplus
}
#endif

#endif /* GEM5_QEMU_JIT_H */
