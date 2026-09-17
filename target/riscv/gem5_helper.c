/*
 * RISC-V helpers for gem5 RiscvJitCPU co-simulation (the xgem5 extension)
 *
 * Copyright (c) 2026 Roman Popov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "gem5_jit.h"
#include "exec/helper-proto.h"

const RISCVGem5Ops *riscv_gem5_ops;

target_ulong helper_gem5_lr(CPURISCVState *env, target_ulong addr,
                            uint32_t memop)
{
    /* Only the gem5 adapter enables xgem5, and it installs the ops first. */
    g_assert(riscv_gem5_ops);
    return riscv_gem5_ops->load_reserved(env, addr, memop, GETPC());
}

target_ulong helper_gem5_sc(CPURISCVState *env, target_ulong addr,
                            target_ulong value, uint32_t memop)
{
    g_assert(riscv_gem5_ops);
    return riscv_gem5_ops->store_conditional(env, addr, value, memop,
                                             GETPC());
}

void helper_gem5_m5op(CPURISCVState *env, uint32_t opcode)
{
    CPUState *cs = env_cpu(env);

    env->gem5_m5op = opcode;
    cs->exception_index = EXCP_GEM5_M5OP;
    cpu_loop_exit(cs);
}
