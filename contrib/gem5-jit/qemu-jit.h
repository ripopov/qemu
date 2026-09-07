/*
 * QEMU RISC-V TCG adapter for gem5 JitCPU
 *
 * SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0-or-later
 */

#ifndef GEM5_QEMU_JIT_H
#define GEM5_QEMU_JIT_H

#include <stddef.h>
#include <stdint.h>

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
typedef void (*Gem5QemuJitRunBoundary)(void *opaque);
typedef uint64_t (*Gem5QemuJitReadTime)(void *opaque);
typedef int (*Gem5QemuJitShouldStop)(void *opaque);

typedef struct Gem5QemuJitCallbacks {
    uint32_t instance_id;
    uint32_t instance_count;
    uint64_t hart_id;
    Gem5QemuJitMemoryRead memory_read;
    Gem5QemuJitMemoryWrite memory_write;
    Gem5QemuJitMemoryMap memory_map;
    Gem5QemuJitRunBoundary run_begin;
    Gem5QemuJitRunBoundary run_end;
    Gem5QemuJitReadTime read_time;
    Gem5QemuJitShouldStop should_stop;
    void *opaque;
} Gem5QemuJitCallbacks;

#define GEM5_QEMU_JIT_CONFIG_VERSION 1
enum Gem5QemuJitProfile {
    GEM5_QEMU_JIT_PROFILE_LEGACY = 0,
    GEM5_QEMU_JIT_PROFILE_RVA23S64 = 1,
};
typedef struct Gem5QemuJitConfig {
    uint32_t version;
    uint32_t size;
    uint32_t profile;
    uint32_t vlenb;
} Gem5QemuJitConfig;

#define GEM5_QEMU_JIT_TIMER_STATE_VERSION 1
#define GEM5_QEMU_JIT_STIMER_ENABLED 1u
#define GEM5_QEMU_JIT_VSTIMER_ENABLED 2u
#define GEM5_QEMU_JIT_STIMER_PENDING 4u
#define GEM5_QEMU_JIT_VSTIMER_PENDING 8u
typedef struct Gem5QemuJitTimerState {
    uint32_t version;
    uint32_t size;
    uint32_t flags;
    uint32_t reserved;
    uint64_t time;
    uint64_t stimecmp;
    uint64_t vstimecmp;
    uint64_t htimedelta;
} Gem5QemuJitTimerState;
/* Between runs, update timer levels from read_time and export deadlines.
 * The host must schedule future refreshes and bound execution by deadlines.
 * Timer-control writes during execution request an early batch boundary. */
GEM5_QEMU_JIT_API int gem5_qemu_jit_refresh_timers(
    uint32_t instance_id, Gem5QemuJitTimerState *state, size_t size);

enum Gem5QemuJitExitReason {
    GEM5_QEMU_JIT_EXIT_BUDGET = 0,
    GEM5_QEMU_JIT_EXIT_HALTED = 1,
    GEM5_QEMU_JIT_EXIT_INTERRUPT = 2,
    GEM5_QEMU_JIT_EXIT_EXCEPTION = 3,
    GEM5_QEMU_JIT_EXIT_ERROR = 4,
    GEM5_QEMU_JIT_EXIT_M5OP = 5,
};

typedef struct Gem5QemuJitRunResult {
    uint64_t instructions;
    int qemu_exception;
    enum Gem5QemuJitExitReason reason;
    uint32_t m5_function;
} Gem5QemuJitRunResult;

#define GEM5_QEMU_JIT_VECTOR_STATE_VERSION 1
#define GEM5_QEMU_JIT_MAX_VLENB 128
/* Migration state, not guest CSR accesses. Register bytes are little-endian;
 * bytes beyond vlenb in every register must be zero. VILL is separate from
 * QEMU's VTYPE payload. Call only between runs, and invalidate translations
 * after importing architectural state before resuming execution. This API
 * also preserves dormant state when V or mstatus.VS is disabled. */
typedef struct Gem5QemuJitVectorState {
    uint32_t version;
    uint32_t size;
    uint32_t vlenb;
    uint32_t vl;
    uint32_t vstart;
    uint32_t vxrm;
    uint32_t vxsat;
    uint32_t vill;
    uint64_t vtype;
    uint8_t registers[32][GEM5_QEMU_JIT_MAX_VLENB];
} Gem5QemuJitVectorState;

GEM5_QEMU_JIT_API int gem5_qemu_jit_get_vector_state(
    uint32_t instance_id, Gem5QemuJitVectorState *state, size_t size);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_vector_state(
    uint32_t instance_id, const Gem5QemuJitVectorState *state, size_t size);

/*
 * One backend image owns all embedded QEMU vCPUs. Each instance ID selects
 * independent architectural, software-TLB, and transient state for one
 * JitCPU; the QEMU runtime, physical address space, and TCG engine are shared.
 */
GEM5_QEMU_JIT_API int gem5_qemu_jit_init(
    const Gem5QemuJitCallbacks *callbacks);
/* First initialization fixes the process-wide profile/VLEN. Later harts
 * must request identical configuration. RVA23 timers still need host event
 * integration before use in gem5; profile selection alone is not handoff. */
GEM5_QEMU_JIT_API int gem5_qemu_jit_init_config(
    const Gem5QemuJitCallbacks *callbacks, const Gem5QemuJitConfig *config,
    size_t size);
GEM5_QEMU_JIT_API int gem5_qemu_jit_run(
    uint32_t instance_id, uint64_t max_instructions,
    Gem5QemuJitRunResult *result);

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
/* Migration mode changes bank HS/VS state when V changes. Not an xRET:
 * no trap-stack status updates. Legacy set_priv selects V=0. */
GEM5_QEMU_JIT_API int gem5_qemu_jit_get_mode(
    uint32_t instance_id, unsigned *privilege, unsigned *virtualization);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_mode(
    uint32_t instance_id, unsigned privilege, unsigned virtualization);
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
