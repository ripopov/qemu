/*
 * gem5 RiscvJitCPU hooks into the RISC-V target
 *
 * Copyright (c) 2026 Roman Popov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef RISCV_GEM5_JIT_H
#define RISCV_GEM5_JIT_H

#include "cpu.h"
#include "exec/memop.h"

/*
 * cpu_exec() return value for a gem5 pseudo instruction. Like EXCP_HLT
 * and EXCP_DEBUG it is above EXCP_INTERRUPT, so cpu_exec() hands it back to
 * its caller instead of taking a guest trap. The encoding is left in
 * env->gem5_m5op and env->pc points at the instruction.
 */
#define EXCP_GEM5_M5OP (EXCP_INTERRUPT + 0x100)

/*
 * LR and SC when the xgem5 extension is enabled: the adapter translates the
 * virtual address like the corresponding load or store would, raising the
 * same faults with return address ra, and performs the access in gem5's
 * reservation-tracking memory. store_conditional returns 0 when the store
 * was performed and 1 when the reservation was gone.
 */
typedef struct RISCVGem5Ops {
    target_ulong (*load_reserved)(CPURISCVState *env, vaddr addr, MemOp mop,
                                  uintptr_t ra);
    target_ulong (*store_conditional)(CPURISCVState *env, vaddr addr,
                                      target_ulong value, MemOp mop,
                                      uintptr_t ra);
} RISCVGem5Ops;

extern const RISCVGem5Ops *riscv_gem5_ops;

#endif
