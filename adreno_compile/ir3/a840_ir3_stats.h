/* SPDX-License-Identifier: MIT */
/* Wave 9 / Lane 2: Adreno 840 (Gen8) ir3 64-bit ISA static instruction &
 * register profiler and A/B diff engine.
 *
 * Parses real A840 64-bit ISA streams (8 bytes / 2 dwords per instruction)
 * directly from A840CompiledStage without linking any Vulkan/Turnip runtime:
 *   - Category bits [63:61] (opc_cat 0..7):
 *       cat0: branch / control / nop / end / kill
 *       cat1: mov / cov
 *       cat2/3/4: ALU / MAD / SFU
 *       cat5: texture / sampler / fbfetch
 *       cat6: ld / st / atomic / resinfo
 *       cat7: sync / barrier / cache
 *   - Deterministic A840 SP wave occupancy estimation matching Kaanapali
 *     hardware register-file geometry (reg_size_vec4=128, max_waves=16,
 *     wave_granularity=2).
 */
#ifndef A840_IR3_STATS_H
#define A840_IR3_STATS_H

#include "AdrenoCompileABI.h"
#include "a840_robustness_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Kaanapali Adreno 840 SP register-file & occupancy constants (chip_facts_a840.c). */
enum {
    A840_SP_REG_SIZE_VEC4     = 128u,
    A840_SP_MAX_WAVES         = 16u,
    A840_SP_WAVE_GRANULARITY  = 2u,
    A840_SP_THREADSIZE_BASE   = 64u,
    A840_SP_BRANCHSTACK_SIZE  = 64u
};

typedef struct A840Ir3ShaderStats {
    uint32_t isa_bytes;
    uint32_t isa_dwords;
    uint32_t total_instrs;
    uint32_t active_instrs;     /* total_instrs - nop_count (non-NOP instructions) */
    uint32_t cat0_ctrl;
    uint32_t cat1_mov;
    uint32_t cat234_alu;
    uint32_t cat2_alu;
    uint32_t cat3_alu;
    uint32_t cat4_sfu;
    uint32_t cat5_tex;
    uint32_t cat6_mem;
    uint32_t cat7_sync;
    uint32_t nop_count;
    uint32_t full_regs;         /* Allocated full vec4 register count (max_full_reg + 1) */
    uint32_t half_regs;         /* Allocated half vec4 register count (max_half_reg + 1) */
    int32_t  max_full_reg;      /* Raw A840CompiledStage.max_full_reg (-1 if none) */
    int32_t  max_half_reg;      /* Raw A840CompiledStage.max_half_reg (-1 if none) */
    uint32_t const_dwords;      /* Constant storage in dwords (constlen_vec4 * 4) */
    uint32_t branchstack;
    uint32_t reg_spills;
    uint32_t reg_fills;
    uint32_t compiler_max_waves;/* Raw A840CompiledStage.max_waves */
    uint32_t est_waves_per_sp;  /* Deterministic A840 SP wave occupancy estimate */
    bool     has_kill;          /* True if cat0 kill opcode present in ISA stream */
    bool     uses_fbfetch;      /* Mirrored from A840CompiledStage.flags_out bit2 */
    bool     merged_regs;       /* Mirrored from A840CompiledStage.flags_out bit3 */
    bool     early_preamble;    /* Mirrored from A840CompiledStage.flags_out bit4 */
    uint32_t robust_policy_flags; /* A840_ROBUST_POLICY_FLAG_* */
} A840Ir3ShaderStats;

typedef struct A840Ir3StatsDiff {
    int32_t  delta_total_instrs;    /* after - before (negative = reduction) */
    int32_t  saved_total_instrs;    /* before - after (positive = reduction) */
    double   reduction_pct;         /* 100.0 * (before - after) / before */
    int32_t  delta_active_instrs;
    int32_t  delta_cat0_ctrl;
    int32_t  delta_cat1_mov;
    int32_t  delta_cat234_alu;
    int32_t  delta_cat5_tex;
    int32_t  delta_cat6_mem;
    int32_t  delta_cat7_sync;
    int32_t  delta_nop_count;
    int32_t  delta_full_regs;
    int32_t  delta_half_regs;
    int32_t  delta_const_dwords;
    int32_t  delta_est_waves_per_sp;
    int32_t  delta_compiler_max_waves;
    bool     reg_regressed;         /* True if full_regs or half_regs increased */
    bool     instr_regressed;       /* True if total_instrs increased */
    bool     wave_regressed;        /* True if est_waves_per_sp decreased */
    bool     xr_preserved;          /* True if both sides kept XR OOB or XR not needed */
} A840Ir3StatsDiff;

/* Deterministic A840 SP wave occupancy estimator from register footprint and
 * branchstack depth (mirrors Mesa ir3_get_reg_dependent_max_waves for Gen8).
 */
uint32_t a840_ir3_estimate_waves_per_sp(
    int32_t max_full_reg,
    int32_t max_half_reg,
    bool merged_regs,
    uint32_t branchstack,
    uint32_t shared_mem_bytes,
    uint32_t stage);

/* Parse a compiled A840 stage's 64-bit ISA stream + metadata into out_stats.
 * `robust_policy_flags` records the A840_ROBUST_POLICY_FLAG_* summary used
 * to compile this stage.
 * Returns A840_COMPILE_OK or A840_COMPILE_INVALID_ARG (leaving out_stats untouched).
 */
A840CompileStatus a840_ir3_stats_analyze(
    const A840CompiledStage *compiled,
    uint32_t robust_policy_flags,
    A840Ir3ShaderStats *out_stats);

/* Compute A/B diff between before (ROBUST_ON) and after (ROBUST_OFF).
 * Returns A840_COMPILE_OK or A840_COMPILE_INVALID_ARG (leaving out_diff untouched).
 */
A840CompileStatus a840_ir3_stats_diff(
    const A840Ir3ShaderStats *before_robust_on,
    const A840Ir3ShaderStats *after_robust_off,
    A840Ir3StatsDiff *out_diff);

#ifdef __cplusplus
}
#endif

#endif /* A840_IR3_STATS_H */
