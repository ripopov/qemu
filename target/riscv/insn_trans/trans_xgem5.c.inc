/*
 * gem5 pseudo instructions (the xgem5 extension)
 *
 * Copyright (c) 2026 Roman Popov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * gem5 encodes its pseudo instructions (m5 exit, m5 workbegin, ...) in the
 * custom-3 major opcode with the function number in bits 31:25. A guest
 * running under gem5's RiscvJitCPU executes them in gem5: translation ends
 * the batch at the instruction and hands the encoding back through
 * EXCP_GEM5_M5OP. Standard QEMU keeps treating the opcode as illegal.
 */

static bool has_xgem5_p(const RISCVCPUConfig *cfg)
{
    return cfg->ext_xgem5;
}

static bool decode_xgem5(DisasContext *ctx, uint32_t opcode)
{
    if ((opcode & 0x7f) != 0x7b) {
        return false;
    }
    gen_update_pc(ctx, 0);
    gen_helper_gem5_m5op(tcg_env, tcg_constant_i32(opcode));
    ctx->base.is_jmp = DISAS_NORETURN;
    return true;
}
