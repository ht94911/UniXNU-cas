/* SPDX-License-Identifier: MIT */
/* Wave 10 / Lane 2: Adreno 840 ir3 Metal Hotspot Specialization Passes.
 *
 * Implements three controllable NIR->ir3 optimization passes targeting real
 * Apple Metal / iPadOS 17 / iOS 27 workload bottlenecks on Adreno 840 (Gen8):
 *   1. Pass 1 (A840_METAL_OPT_FP16_ROUND_FOLD):
 *      Apple FP16 rounding-mode alignment (RTZ/RTNE) and redundant
 *      f2f16/f2f32 round-trip elimination + pure-half ALU narrowing, while
 *      strictly preserving exact/SignedZeroInfNanPreserve ops and Apple XR10
 *      ([-0.752941, 1.25098]) wide-color boundaries.
 *   2. Pass 2 (A840_METAL_OPT_UNIFORM_PUSH_FOLD):
 *      Uniform / Push-Constant offset folding, dominance-based duplicate &
 *      sub-vector read coalescing, and redundant scalar widen/DCE cleanup,
 *      preserving push_lo_bytes / push_dst_dwords layout invariants.
 *   3. Pass 3 (A840_METAL_OPT_REG_PRESSURE_SINK):
 *      Register-pressure reduction and anti-spill scheduling via cross-block
 *      nir_opt_sink and pressure-gated intra-block live-range sinking to
 *      keep/improve A840 SP wave occupancy (target up to 16 waves/SP).
 *
 * Pure C11 configuration, thread-local A/B toggle, and report APIs compile
 * cleanly both with and without A840_BUILD_MESA_IR3_BACKEND (zero Vulkan or
 * Turnip dependency).
 */
#ifndef A840_METAL_OPT_PASS_H
#define A840_METAL_OPT_PASS_H

#include "AdrenoCompileABI.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum A840MetalOptPassFlags {
    A840_METAL_OPT_NONE              = 0u,
    A840_METAL_OPT_FP16_ROUND_FOLD   = 1u << 0,
    A840_METAL_OPT_UNIFORM_PUSH_FOLD = 1u << 1,
    A840_METAL_OPT_REG_PRESSURE_SINK = 1u << 2,
    A840_METAL_OPT_ALL               = (1u << 0) | (1u << 1) | (1u << 2)
} A840MetalOptPassFlags;

enum {
    A840_METAL_OPT_DEFAULT_SINK_THRESH = 12u
};

typedef struct A840MetalOptConfig {
    uint32_t pass_mask;            /* Bitmask of A840MetalOptPassFlags */
    bool     preserve_xr10;        /* Strictly preserve XR10 wide-color clamps/precision */
    bool     respect_half_rtz;     /* Honor A840_COMPILE_FLAG_HALF_ROUND_RTZ / RTZ mode */
    bool     preserve_push_layout; /* Preserve push_lo_bytes / push_dst_dwords contract */
    uint32_t sink_pressure_thresh; /* Live vec4 threshold for intra-block sinking */
} A840MetalOptConfig;

typedef struct A840MetalOptReport {
    uint32_t active_pass_mask;
    uint32_t fp16_roundtrip_elided;
    uint32_t fp16_alu_narrowed;
    uint32_t fp16_rtz_aligned;
    uint32_t fp16_xr10_guarded;
    uint32_t push_const_loads_merged;
    uint32_t push_const_offsets_folded;
    uint32_t uniform_widens_elided;
    uint32_t sink_moves_applied;
    uint32_t peak_live_vec4_before;
    uint32_t peak_live_vec4_after;
    uint32_t pre_opt_alu_count;
    uint32_t post_opt_alu_count;
    bool     xr10_detected;
    bool     xr10_safe;
    bool     push_layout_preserved;
} A840MetalOptReport;

/* Build the default hotspot optimization config for a stage's effective flags,
 * respecting any thread-local A/B override set via a840_metal_opt_set_thread_mask().
 */
A840MetalOptConfig a840_metal_opt_default_config(uint32_t effective_flags);

/* Thread-local pass-mask control for deterministic same-process OPT_OFF vs
 * OPT_ON A/B verification without mutating frozen AdrenoCompileABI.h.
 */
void a840_metal_opt_set_thread_mask(uint32_t pass_mask);
uint32_t a840_metal_opt_get_thread_mask(void);
void a840_metal_opt_reset_thread_mask(void);

/* Thread-local diagnostic report recorded by the most recent compilation. */
bool a840_metal_opt_get_last_report(A840MetalOptReport *out_report);
void a840_metal_opt_record_last_report(const A840MetalOptReport *report);

#ifdef A840_BUILD_MESA_IR3_BACKEND
struct nir_shader;

/* Phase A (runs before push-constant range scan & descriptor lowering):
 * executes Pass 2 (Uniform / Push-Constant folding & coalescing) and
 * Pass 1 (Apple FP16 rounding alignment & redundant conversion/ALU narrowing).
 * Returns true if the NIR shader was modified.
 */
bool a840_metal_opt_run_nir_pre_push(struct nir_shader *nir,
                                     const A840MetalOptConfig *cfg,
                                     A840MetalOptReport *out_report);

/* Phase B (runs after ir3_finalize_nir):
 * executes Pass 3 (cross-block nir_opt_sink + pressure-gated intra-block
 * live-range sinking) and updates inout_report.
 * Returns true if the NIR shader was modified.
 */
bool a840_metal_opt_run_nir_post_finalize(struct nir_shader *nir,
                                          const A840MetalOptConfig *cfg,
                                          A840MetalOptReport *inout_report);

/* Convenience combined entry point executing both pre-push and post-finalize
 * passes on a lowered NIR shader.
 */
bool a840_metal_opt_run_nir(struct nir_shader *nir,
                            const A840MetalOptConfig *cfg,
                            A840MetalOptReport *out_report);
#endif

#ifdef __cplusplus
}
#endif

#endif /* A840_METAL_OPT_PASS_H */
