/* SPDX-License-Identifier: MIT */
#include "a840_ir3_stats.h"

#include <string.h>

enum {
    A840_STAGE_OUT_FLAG_USES_FBFETCH   = 1u << 2,
    A840_STAGE_OUT_FLAG_MERGED_REGS    = 1u << 3,
    A840_STAGE_OUT_FLAG_EARLY_PREAMBLE = 1u << 4,

    /* ir3 cat0 instruction bitfields (src/freedreno/isa/ir3-cat0.xml) */
    A840_IR3_CAT0_OPC_NOP  = 0u, /* 0000b with opc_hi bit0 = 0 */
    A840_IR3_CAT0_OPC_KILL = 5u, /* 0101b with opc_hi bit0 = 0 */
    A840_IR3_CAT0_OPC_END  = 6u  /* 0110b with opc_hi bit0 = 0 */
};

static bool
a840_ir3_is_cat0_nop(uint64_t word)
{
    const uint32_t cat = (uint32_t)(word >> 61);
    if (cat != 0u)
        return false;
    const uint32_t opc = (uint32_t)((word >> 55) & 0xfu);
    const uint32_t opc_hi_lsb = (uint32_t)((word >> 49) & 0x1u);
    return opc == A840_IR3_CAT0_OPC_NOP && opc_hi_lsb == 0u;
}

static bool
a840_ir3_is_cat0_kill(uint64_t word)
{
    const uint32_t cat = (uint32_t)(word >> 61);
    if (cat != 0u)
        return false;
    const uint32_t opc = (uint32_t)((word >> 55) & 0xfu);
    const uint32_t opc_hi_lsb = (uint32_t)((word >> 49) & 0x1u);
    return opc == A840_IR3_CAT0_OPC_KILL && opc_hi_lsb == 0u;
}

static bool
a840_ir3_is_cat0_end(uint64_t word)
{
    const uint32_t cat = (uint32_t)(word >> 61);
    if (cat != 0u)
        return false;
    const uint32_t opc = (uint32_t)((word >> 55) & 0xfu);
    const uint32_t opc_hi_lsb = (uint32_t)((word >> 49) & 0x1u);
    return opc == A840_IR3_CAT0_OPC_END && opc_hi_lsb == 0u;
}

uint32_t
a840_ir3_estimate_waves_per_sp(int32_t max_full_reg,
                               int32_t max_half_reg,
                               bool merged_regs,
                               uint32_t branchstack,
                               uint32_t shared_mem_bytes,
                               uint32_t stage)
{
    uint32_t full_vec4 = (max_full_reg >= 0) ? (uint32_t)(max_full_reg + 1) : 0u;
    uint32_t half_vec4 = (max_half_reg >= 0) ? (uint32_t)(max_half_reg + 1) : 0u;

    /* Gen8 mergedregs packs half registers into the shared register file
     * (2 half vec4 per full vec4 slot); non-merged adds half/2 ceiling.
     */
    uint32_t effective_vec4 = merged_regs
        ? (full_vec4 > ((half_vec4 + 1u) / 2u) ? full_vec4 : ((half_vec4 + 1u) / 2u))
        : (full_vec4 + ((half_vec4 + 1u) / 2u));

    uint32_t waves = A840_SP_MAX_WAVES;
    if (effective_vec4 > 0u) {
        uint32_t reg_waves =
            (A840_SP_REG_SIZE_VEC4 / effective_vec4) * A840_SP_WAVE_GRANULARITY;
        if (reg_waves < waves)
            waves = reg_waves;
    }

    if (branchstack > 0u) {
        uint32_t bs_waves = A840_SP_BRANCHSTACK_SIZE / branchstack;
        if (bs_waves < waves)
            waves = bs_waves;
    }

    /* Compute SLM pressure: Kaanapali SP has 32 KiB SLM. */
    if (stage == A840_STAGE_COMPUTE && shared_mem_bytes > 0u) {
        uint32_t slm_groups = 32768u / shared_mem_bytes;
        if (slm_groups == 0u)
            slm_groups = 1u;
        uint32_t slm_waves = slm_groups * A840_SP_WAVE_GRANULARITY;
        if (slm_waves < waves)
            waves = slm_waves;
    }

    if (waves == 0u)
        waves = 1u;
    if (waves > A840_SP_MAX_WAVES)
        waves = A840_SP_MAX_WAVES;
    return waves;
}

A840CompileStatus
a840_ir3_stats_analyze(const A840CompiledStage *compiled,
                       uint32_t robust_policy_flags,
                       A840Ir3ShaderStats *out_stats)
{
    if (compiled == NULL || out_stats == NULL)
        return A840_COMPILE_INVALID_ARG;
    if (compiled->isa_words == NULL || compiled->isa_dwords == 0u ||
        (compiled->isa_dwords % 2u) != 0u)
        return A840_COMPILE_INVALID_ARG;

    A840Ir3ShaderStats st;
    memset(&st, 0, sizeof(st));

    const uint32_t total_slots = compiled->isa_dwords / 2u;
    st.isa_dwords = compiled->isa_dwords;
    st.isa_bytes = compiled->isa_dwords * 4u;

    /* Walk 64-bit instructions until trailing post-end padding NOPs (>3 consecutive
     * NOPs after an `end` instruction, matching Mesa disasm-a3xx.c stop rule).
     */
    bool seen_end = false;
    uint32_t trailing_nops = 0u;
    uint32_t counted = 0u;

    for (uint32_t i = 0u; i < total_slots; ++i) {
        uint64_t lo = (uint64_t)compiled->isa_words[i * 2u];
        uint64_t hi = (uint64_t)compiled->isa_words[i * 2u + 1u];
        uint64_t word = (hi << 32) | lo;

        const bool is_nop = a840_ir3_is_cat0_nop(word);
        if (seen_end && is_nop) {
            trailing_nops++;
            if (trailing_nops > 3u)
                break;
        } else {
            trailing_nops = 0u;
        }

        const uint32_t cat = (uint32_t)(word >> 61);
        counted++;

        switch (cat) {
        case 0u:
            st.cat0_ctrl++;
            if (is_nop) {
                st.nop_count++;
            } else if (a840_ir3_is_cat0_kill(word)) {
                st.has_kill = true;
            } else if (a840_ir3_is_cat0_end(word)) {
                seen_end = true;
            }
            break;
        case 1u:
            st.cat1_mov++;
            break;
        case 2u:
            st.cat2_alu++;
            st.cat234_alu++;
            break;
        case 3u:
            st.cat3_alu++;
            st.cat234_alu++;
            break;
        case 4u:
            st.cat4_sfu++;
            st.cat234_alu++;
            break;
        case 5u:
            st.cat5_tex++;
            break;
        case 6u:
            st.cat6_mem++;
            break;
        case 7u:
            st.cat7_sync++;
            break;
        default:
            break;
        }
    }

    st.total_instrs = counted;
    st.active_instrs = (counted >= st.nop_count) ? (counted - st.nop_count) : 0u;

    st.max_full_reg = compiled->max_full_reg;
    st.max_half_reg = compiled->max_half_reg;
    st.full_regs = (compiled->max_full_reg >= 0) ? (uint32_t)(compiled->max_full_reg + 1) : 0u;
    st.half_regs = (compiled->max_half_reg >= 0) ? (uint32_t)(compiled->max_half_reg + 1) : 0u;
    st.const_dwords = compiled->constlen_vec4 * 4u;
    st.branchstack = compiled->branchstack;
    st.reg_spills = compiled->reg_spills;
    st.reg_fills = compiled->reg_fills;
    st.compiler_max_waves = compiled->max_waves;

    st.uses_fbfetch = (compiled->flags_out & A840_STAGE_OUT_FLAG_USES_FBFETCH) != 0u;
    st.merged_regs = (compiled->flags_out & A840_STAGE_OUT_FLAG_MERGED_REGS) != 0u;
    st.early_preamble = (compiled->flags_out & A840_STAGE_OUT_FLAG_EARLY_PREAMBLE) != 0u;

    st.est_waves_per_sp = a840_ir3_estimate_waves_per_sp(
        compiled->max_full_reg,
        compiled->max_half_reg,
        st.merged_regs,
        compiled->branchstack,
        compiled->shared_mem_bytes,
        compiled->stage);

    st.robust_policy_flags = robust_policy_flags;

    *out_stats = st;
    return A840_COMPILE_OK;
}

A840CompileStatus
a840_ir3_stats_diff(const A840Ir3ShaderStats *before_robust_on,
                    const A840Ir3ShaderStats *after_robust_off,
                    A840Ir3StatsDiff *out_diff)
{
    if (before_robust_on == NULL || after_robust_off == NULL || out_diff == NULL)
        return A840_COMPILE_INVALID_ARG;

    A840Ir3StatsDiff d;
    memset(&d, 0, sizeof(d));

    d.delta_total_instrs = (int32_t)after_robust_off->total_instrs -
                           (int32_t)before_robust_on->total_instrs;
    d.saved_total_instrs = (int32_t)before_robust_on->total_instrs -
                           (int32_t)after_robust_off->total_instrs;
    if (before_robust_on->total_instrs > 0u) {
        d.reduction_pct = (100.0 * (double)d.saved_total_instrs) /
                          (double)before_robust_on->total_instrs;
    } else {
        d.reduction_pct = 0.0;
    }

    d.delta_active_instrs = (int32_t)after_robust_off->active_instrs -
                            (int32_t)before_robust_on->active_instrs;
    d.delta_cat0_ctrl = (int32_t)after_robust_off->cat0_ctrl -
                        (int32_t)before_robust_on->cat0_ctrl;
    d.delta_cat1_mov = (int32_t)after_robust_off->cat1_mov -
                       (int32_t)before_robust_on->cat1_mov;
    d.delta_cat234_alu = (int32_t)after_robust_off->cat234_alu -
                         (int32_t)before_robust_on->cat234_alu;
    d.delta_cat5_tex = (int32_t)after_robust_off->cat5_tex -
                       (int32_t)before_robust_on->cat5_tex;
    d.delta_cat6_mem = (int32_t)after_robust_off->cat6_mem -
                       (int32_t)before_robust_on->cat6_mem;
    d.delta_cat7_sync = (int32_t)after_robust_off->cat7_sync -
                        (int32_t)before_robust_on->cat7_sync;
    d.delta_nop_count = (int32_t)after_robust_off->nop_count -
                        (int32_t)before_robust_on->nop_count;

    d.delta_full_regs = (int32_t)after_robust_off->full_regs -
                        (int32_t)before_robust_on->full_regs;
    d.delta_half_regs = (int32_t)after_robust_off->half_regs -
                        (int32_t)before_robust_on->half_regs;
    d.delta_const_dwords = (int32_t)after_robust_off->const_dwords -
                           (int32_t)before_robust_on->const_dwords;
    d.delta_est_waves_per_sp = (int32_t)after_robust_off->est_waves_per_sp -
                               (int32_t)before_robust_on->est_waves_per_sp;
    d.delta_compiler_max_waves = (int32_t)after_robust_off->compiler_max_waves -
                                 (int32_t)before_robust_on->compiler_max_waves;

    d.reg_regressed = (d.delta_full_regs > 0) || (d.delta_half_regs > 0);
    d.instr_regressed = (d.delta_total_instrs > 0);
    d.wave_regressed = (d.delta_est_waves_per_sp < 0);

    const bool before_xr =
        (before_robust_on->robust_policy_flags & A840_ROBUST_POLICY_FLAG_PRESERVE_XR_OOB) != 0u;
    const bool after_xr =
        (after_robust_off->robust_policy_flags & A840_ROBUST_POLICY_FLAG_PRESERVE_XR_OOB) != 0u;
    d.xr_preserved = (!before_xr) || after_xr;

    *out_diff = d;
    return A840_COMPILE_OK;
}
