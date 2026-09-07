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
#define GEM5_QEMU_JIT_RESERVATION_STATE_VERSION 1
/* Backend-local LR/SC migration token, NOT a physical coherence monitor.
 * virtual_address is QEMU's effective LR address; expected_value is its
 * comparison operand. No access width or physical reservation granule is
 * implied. Only stopped RV64 harts are supported. Import after translation
 * invalidation, which deliberately clears reservations. The host must ensure
 * memory and translation state belong to the same snapshot and account for
 * intervening writes; this API alone does not transfer an O3 reservation.
 * Invalid tokens have valid=0 and both payload fields zero. */
typedef struct Gem5QemuJitReservationState {
    uint32_t version;
    uint32_t size;
    uint32_t valid;
    uint32_t reserved;
    uint64_t virtual_address;
    uint64_t expected_value;
} Gem5QemuJitReservationState;
GEM5_QEMU_JIT_API int gem5_qemu_jit_get_reservation_state(
    uint32_t instance_id, Gem5QemuJitReservationState *state, size_t size);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_reservation_state(
    uint32_t instance_id, const Gem5QemuJitReservationState *state, size_t size);

#define GEM5_QEMU_JIT_PMP_STATE_VERSION 1
#define GEM5_QEMU_JIT_MAX_PMPS 64
/* Trusted migration state, not architectural CSR writes. Can replace locked
 * entries. Only between runs; mseccfg is separate from this PMP table. */
typedef struct Gem5QemuJitPmpState {
    uint32_t version;
    uint32_t size;
    uint32_t regions;
    uint32_t reserved;
    uint64_t address[GEM5_QEMU_JIT_MAX_PMPS];
    uint8_t config[GEM5_QEMU_JIT_MAX_PMPS];
} Gem5QemuJitPmpState;
GEM5_QEMU_JIT_API int gem5_qemu_jit_get_pmp_state(
    uint32_t instance_id, Gem5QemuJitPmpState *state, size_t size);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_pmp_state(
    uint32_t instance_id, const Gem5QemuJitPmpState *state, size_t size);

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
#define GEM5_QEMU_JIT_MODE_VERSION 1
/* Migration mode changes bank HS/VS state when V changes. Not an xRET:
 * no trap-stack status updates, and LR state is preserved for host-side
 * synchronization. Use explicit invalidation at takeover. Legacy set_priv
 * selects V=0. */
GEM5_QEMU_JIT_API int gem5_qemu_jit_get_mode(
    uint32_t instance_id, unsigned *privilege, unsigned *virtualization);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_mode(
    uint32_t instance_id, unsigned privilege, unsigned virtualization);
GEM5_QEMU_JIT_API int gem5_qemu_jit_get_csr(
    uint32_t instance_id, unsigned csr, uint64_t *value);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_csr(
    uint32_t instance_id, unsigned csr, uint64_t value);
#define GEM5_QEMU_JIT_MSTATUS_RESTORE_VERSION 1
#define GEM5_QEMU_JIT_STATEEN_VERSION 1
#define GEM5_QEMU_JIT_HPM_STATE_VERSION 1
#define GEM5_QEMU_JIT_FIXED_COUNTER_STATE_VERSION 1
typedef struct Gem5QemuJitFixedCounterState {
    uint32_t version;
    uint32_t size;
    uint32_t inhibited; /* MCOUNTINHIBIT.CY/IR only */
    uint32_t reserved;
    uint64_t cycle;
    uint64_t instret;
    uint64_t cyclecfg;
    uint64_t instretcfg;
} Gem5QemuJitFixedCounterState;
/* Samples and optional filter controls, never backend source offsets.
 * Restore requires a stopped RV64 M-mode hart and retires no instruction. */
GEM5_QEMU_JIT_API int gem5_qemu_jit_get_fixed_counters(
    uint32_t instance_id, Gem5QemuJitFixedCounterState *state, size_t size);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_fixed_counters(
    uint32_t instance_id, const Gem5QemuJitFixedCounterState *state, size_t size);
/* Programmable HPM bank only; fixed cycle/instret, their inhibition bits,
 * counter-access gates and interrupt pending state are owned separately.
 * Values are counter samples, not QEMU source offsets or timer objects.
 * Slots 0..2 and unimplemented slots must be zero. Restore requires a stopped
 * RV64 hart in M mode, validates the whole input before mutation, and does
 * not execute guest CSR writes or acknowledge/post an interrupt.
 * Multiple counters may select the same event, with independent values,
 * inhibition, privilege filters and overflow state.
 */
typedef struct Gem5QemuJitHpmState {
    uint32_t version;
    uint32_t size;
    uint32_t implemented;
    uint32_t inhibited;
    uint64_t counter[32];
    uint64_t event[32];
} Gem5QemuJitHpmState;
GEM5_QEMU_JIT_API int gem5_qemu_jit_get_hpm_state(
    uint32_t instance_id, Gem5QemuJitHpmState *state, size_t size);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_hpm_state(
    uint32_t instance_id, const Gem5QemuJitHpmState *state, size_t size);
typedef struct Gem5QemuJitStateenState {
    uint32_t version;
    uint32_t size;
    /* Rows M/H/S, columns 0..3. Raw storage, not ancestor-masked CSR reads.
     * Masks report implemented bits independently of current gate values. */
    uint64_t value[3][4];
    uint64_t mask[3][4];
} Gem5QemuJitStateenState;
GEM5_QEMU_JIT_API int gem5_qemu_jit_get_stateen(
    uint32_t instance_id, Gem5QemuJitStateenState *state, size_t size);
GEM5_QEMU_JIT_API int gem5_qemu_jit_set_stateen(
    uint32_t instance_id, const Gem5QemuJitStateenState *state, size_t size);
/* Trusted host restoration, only in M mode with V=0. Applies normal WARL
 * legalization plus exact restoration of MPV/GVA trap state. Does not change
 * the guest CSR-write policy. Rejects unsupported versions before mutation. */
GEM5_QEMU_JIT_API int gem5_qemu_jit_restore_mstatus(
    uint32_t instance_id, unsigned version, uint64_t value);
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
