/* SPDX-License-Identifier: MIT */
/* Wave 9 / Lane 2: Adreno 840 Robustness Safety Boundary Policy.
 *
 * Defines the explicit security/semantic contract between Apple Metal and
 * Vulkan/SPIR-V buffer robustness on Adreno 840 (Kaanapali):
 *   - Buffer (UBO/SSBO) out-of-bounds checks may only be elided when
 *     A840_COMPILE_FLAG_ROBUST_OFF is requested AND the KMD 64 KiB shared
 *     read-only zero-page guard is active AND the shader is not marked
 *     untrusted / debug-bounds-forced.
 *   - Apple XR10 wide-color (BGRA10_XR / BGR10_XR, [-0.752941, 1.25098])
 *     and FBFetch / image out-of-bounds semantics are ALWAYS preserved and
 *     never relaxed by buffer robustness elision.
 *   - When A840_COMPILE_FLAG_ROBUST_OFF is NOT set, behavior is 100%
 *     equivalent to Phase 1-4 defaults (robust UBO | SSBO modes enabled).
 */
#ifndef A840_ROBUSTNESS_POLICY_H
#define A840_ROBUSTNESS_POLICY_H

#include "AdrenoCompileABI.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bitmask matching Mesa nir_variable_mode for UBO and SSBO without pulling
 * Mesa NIR headers into pure-C callers/tests.
 */
enum {
    A840_NIR_ROBUST_MODE_NONE = 0u,
    A840_NIR_ROBUST_MODE_UBO  = 1u << 1,  /* matches nir_var_mem_ubo  (1 << 1) */
    A840_NIR_ROBUST_MODE_SSBO = 1u << 2   /* matches nir_var_mem_ssbo (1 << 2) */
};

/* Explicit Apple XR10 / wide-color clamp & image OOB preservation flag
 * accepted by the policy engine (also inferred from stage metadata).
 */
#ifndef A840_COMPILE_FLAG_XR10_CLAMP
#define A840_COMPILE_FLAG_XR10_CLAMP (1u << 5)
#endif

/* Extended policy hints passed by callers/harness to evaluate safety gates. */
enum {
    A840_ROBUST_HINT_ZERO_PAGE_READY     = 1u << 0, /* Phase 2 64 KiB RO zero page active */
    A840_ROBUST_HINT_XR10_PATH           = 1u << 1, /* XR10 wide-color sampling/output path */
    A840_ROBUST_HINT_IMAGE_OOB_SENSITIVE = 1u << 2, /* Explicit image OOB/border sensitivity */
    A840_ROBUST_HINT_UNTRUSTED_SHADER    = 1u << 3, /* Debug/untrusted shader: force bounds */
    A840_ROBUST_HINT_FORCE_FULL_BOUNDS   = 1u << 4, /* Explicit full bounds check requested */
    A840_ROBUST_HINT_FBFETCH_ACTIVE      = 1u << 5  /* FBFetch / GMEM input attachment */
};

/* Diagnostic reason bitmask produced by a840_robustness_decide(). */
enum {
    A840_ROBUST_REASON_DEFAULT_ROBUST_ON      = 1u << 0,
    A840_ROBUST_REASON_ROBUST_OFF_REQUESTED   = 1u << 1,
    A840_ROBUST_REASON_ZERO_PAGE_GUARD_OK     = 1u << 2,
    A840_ROBUST_REASON_REJECTED_NO_ZERO_PAGE  = 1u << 3,
    A840_ROBUST_REASON_REJECTED_UNTRUSTED     = 1u << 4,
    A840_ROBUST_REASON_REJECTED_FORCE_BOUNDS  = 1u << 5,
    A840_ROBUST_REASON_XR10_SEMANTICS_KEPT    = 1u << 6,
    A840_ROBUST_REASON_IMAGE_OOB_KEPT         = 1u << 7,
    A840_ROBUST_REASON_FBFETCH_SEMANTICS_KEPT = 1u << 8,
    A840_ROBUST_REASON_BUFFER_BOUNDS_ELIDED   = 1u << 9
};

/* Compact flags persisted into A840Ir3ShaderStats.robust_policy_flags. */
enum {
    A840_ROBUST_POLICY_FLAG_ROBUST_OFF       = 1u << 0,
    A840_ROBUST_POLICY_FLAG_ELIDE_BUF_BOUNDS = 1u << 1,
    A840_ROBUST_POLICY_FLAG_PRESERVE_XR_OOB  = 1u << 2,
    A840_ROBUST_POLICY_FLAG_ZERO_PAGE_GUARD  = 1u << 3,
    A840_ROBUST_POLICY_FLAG_PRESERVE_IMG_OOB = 1u << 4
};

/* Apple XR10 extended-range [BGRA10_XR / BGR10_XR] semantic bounds. */
#define A840_XR10_MIN_VALUE (-0.752941f)
#define A840_XR10_MAX_VALUE (1.25098f)
#define A840_ZERO_PAGE_GUARD_BYTES (64u * 1024u)

typedef struct A840RobustnessInput {
    uint32_t stage;              /* A840ShaderStage */
    uint32_t effective_flags;    /* target->flags | input->flags */
    uint32_t policy_hints;       /* Bitmask of A840_ROBUST_HINT_* */
    uint32_t color_rt_mask;      /* Bound color RT mask (FS) */
    uint32_t half_rt_mask;       /* 16-bit / extended RT mask (FS) */
    uint32_t zero_page_bytes;    /* Must be >= 65536 when ZERO_PAGE_READY is set */
    bool     has_storage_images; /* Shader touches storage/sampled images */
    bool     has_fbfetch;        /* Shader reads input attachment / GMEM */
} A840RobustnessInput;

typedef struct A840RobustnessDecision {
    uint32_t nir_robust_modes;         /* A840_NIR_ROBUST_MODE_* bitmask */
    bool     elide_buffer_bounds;      /* True: safe to elide UBO/SSBO sw bounds checks */
    bool     preserve_xr_oob;          /* True: Apple XR10 [-0.752941, 1.25098] kept */
    bool     preserve_image_oob;       /* True: image/texture OOB checks kept */
    bool     requires_zero_page_guard; /* True: runtime relies on 64 KiB RO zero page */
    uint32_t reason_mask;              /* Bitmask of A840_ROBUST_REASON_* */
    uint32_t policy_flags;             /* Compact A840_ROBUST_POLICY_FLAG_* summary */
} A840RobustnessDecision;

/* Pure deterministic policy evaluator.
 * Returns A840_COMPILE_OK on valid inputs, or A840_COMPILE_INVALID_ARG on
 * NULL pointers, invalid stage, or malformed zero-page precondition (e.g.,
 * ZERO_PAGE_READY claimed with zero_page_bytes < 64 KiB).
 * On error, out_decision is not modified (if non-NULL).
 */
A840CompileStatus a840_robustness_decide(
    const A840RobustnessInput *in_req,
    A840RobustnessDecision *out_decision);

/* Convenience helper matching a840_spirv_lower.c call site: derives policy
 * input from A840StageInput + effective_flags + NIR-detected image/fbfetch
 * usage and evaluates a840_robustness_decide().
 */
A840CompileStatus a840_robustness_decide_for_stage(
    const A840StageInput *stage_input,
    uint32_t effective_flags,
    bool zero_page_ready,
    bool has_storage_images,
    bool has_fbfetch,
    A840RobustnessDecision *out_decision);

#ifdef __cplusplus
}
#endif

#endif /* A840_ROBUSTNESS_POLICY_H */
