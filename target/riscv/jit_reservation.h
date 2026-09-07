/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RISCV_JIT_RESERVATION_H
#define RISCV_JIT_RESERVATION_H

#include "cpu.h"

/* Shared policy for guest and host physical writes. The inclusive range
 * must already have been validated not to wrap. One aligned 64-byte block
 * is monitored; the original LR width/address are retained separately. */
static inline void riscv_jit_reservation_write(CPURISCVState *env,
                                               uint64_t first, uint64_t last)
{
    uint64_t block = env->jit_load_paddr & ~UINT64_C(63);
    if (env->load_res != UINT64_MAX && env->jit_load_size &&
        first <= block + 63 && last >= block) {
        env->load_res = UINT64_MAX;
        env->jit_load_size = 0;
    }
}

#endif
