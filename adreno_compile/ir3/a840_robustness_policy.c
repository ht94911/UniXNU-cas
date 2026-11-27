/* SPDX-License-Identifier: MIT */
#include "a840_robustness_policy.h"

#include <string.h>

static bool
a840_stage_is_valid(uint32_t stage)
{
    return stage == A840_STAGE_VERTEX ||
           stage == A840_STAGE_FRAGMENT ||
           stage == A840_STAGE_COMPUTE;
}

A840CompileStatus
a840_robustness_decide(const A840RobustnessInput *in_req,
                       A840RobustnessDecision *out_decision)
{
    if (in_req == NULL || out_decision == NULL)
        return A840_COMPILE_INVALID_ARG;
    if (!a840_stage_is_valid(in_req->stage))
        return A840_COMPILE_INVALID_ARG;

    const bool zero_page_hint =
        (in_req->policy_hints & A840_ROBUST_HINT_ZERO_PAGE_READY) != 0u;
    if (zero_page_hint && in_req->zero_page_bytes < A840_ZERO_PAGE_GUARD_BYTES)
        return A840_COMPILE_INVALID_ARG;

    A840RobustnessDecision dec;
    memset(&dec, 0, sizeof(dec));

    const bool robust_off_req =
        (in_req->effective_flags & A840_COMPILE_FLAG_ROBUST_OFF) != 0u;
    const bool untrusted =
        (in_req->policy_hints & A840_ROBUST_HINT_UNTRUSTED_SHADER) != 0u;
    const bool force_full_bounds =
        (in_req->policy_hints & A840_ROBUST_HINT_FORCE_FULL_BOUNDS) != 0u;
    const bool xr10_requested =
        ((in_req->effective_flags & A840_COMPILE_FLAG_XR10_CLAMP) != 0u) ||
        ((in_req->policy_hints & A840_ROBUST_HINT_XR10_PATH) != 0u);
    const bool fbfetch_active =
        in_req->has_fbfetch ||
        ((in_req->effective_flags & A840_COMPILE_FLAG_FBFETCH_GMEM) != 0u) ||
        ((in_req->policy_hints & A840_ROBUST_HINT_FBFETCH_ACTIVE) != 0u);
    const bool image_oob_sensitive =
        in_req->has_storage_images ||
        ((in_req->policy_hints & A840_ROBUST_HINT_IMAGE_OOB_SENSITIVE) != 0u);

    /* Apple XR10 wide-color ([-0.752941, 1.25098]) and image/FBFetch OOB
     * semantics are unconditionally preserved on fragment/XR/image paths,
     * regardless of buffer robustness elision.
     */
    dec.preserve_xr_oob =
        xr10_requested ||
        (in_req->stage == A840_STAGE_FRAGMENT &&
         (in_req->color_rt_mask != 0u || in_req->half_rt_mask != 0u || fbfetch_active));
    dec.preserve_image_oob = image_oob_sensitive || dec.preserve_xr_oob || fbfetch_active;

    if (dec.preserve_xr_oob)
        dec.reason_mask |= A840_ROBUST_REASON_XR10_SEMANTICS_KEPT;
    if (dec.preserve_image_oob)
        dec.reason_mask |= A840_ROBUST_REASON_IMAGE_OOB_KEPT;
    if (fbfetch_active)
        dec.reason_mask |= A840_ROBUST_REASON_FBFETCH_SEMANTICS_KEPT;

    if (!robust_off_req) {
        /* Phase 1-4 default compatibility path: keep full UBO|SSBO robustness. */
        dec.nir_robust_modes = A840_NIR_ROBUST_MODE_UBO | A840_NIR_ROBUST_MODE_SSBO;
        dec.elide_buffer_bounds = false;
        dec.requires_zero_page_guard = false;
        dec.reason_mask |= A840_ROBUST_REASON_DEFAULT_ROBUST_ON;
    } else {
        dec.reason_mask |= A840_ROBUST_REASON_ROBUST_OFF_REQUESTED;

        bool blocked = false;
        if (!zero_page_hint || in_req->zero_page_bytes < A840_ZERO_PAGE_GUARD_BYTES) {
            dec.reason_mask |= A840_ROBUST_REASON_REJECTED_NO_ZERO_PAGE;
            blocked = true;
        }
        if (untrusted) {
            dec.reason_mask |= A840_ROBUST_REASON_REJECTED_UNTRUSTED;
            blocked = true;
        }
        if (force_full_bounds) {
            dec.reason_mask |= A840_ROBUST_REASON_REJECTED_FORCE_BOUNDS;
            blocked = true;
        }

        if (blocked) {
            /* Fail closed: retain buffer robustness when preconditions fail. */
            dec.nir_robust_modes = A840_NIR_ROBUST_MODE_UBO | A840_NIR_ROBUST_MODE_SSBO;
            dec.elide_buffer_bounds = false;
            dec.requires_zero_page_guard = false;
        } else {
            /* Safe to elide UBO/SSBO buffer robustness checks: Phase 2 shared
             * 64 KiB RO zero page prevents SMMU page faults on OOB reads.
             * XR10 and image OOB semantics remain strictly protected.
             */
            dec.nir_robust_modes = A840_NIR_ROBUST_MODE_NONE;
            dec.elide_buffer_bounds = true;
            dec.requires_zero_page_guard = true;
            dec.reason_mask |= A840_ROBUST_REASON_ZERO_PAGE_GUARD_OK |
                               A840_ROBUST_REASON_BUFFER_BOUNDS_ELIDED;
        }
    }

    uint32_t pflags = 0u;
    if (robust_off_req)
        pflags |= A840_ROBUST_POLICY_FLAG_ROBUST_OFF;
    if (dec.elide_buffer_bounds)
        pflags |= A840_ROBUST_POLICY_FLAG_ELIDE_BUF_BOUNDS;
    if (dec.preserve_xr_oob)
        pflags |= A840_ROBUST_POLICY_FLAG_PRESERVE_XR_OOB;
    if (dec.requires_zero_page_guard)
        pflags |= A840_ROBUST_POLICY_FLAG_ZERO_PAGE_GUARD;
    if (dec.preserve_image_oob)
        pflags |= A840_ROBUST_POLICY_FLAG_PRESERVE_IMG_OOB;
    dec.policy_flags = pflags;

    *out_decision = dec;
    return A840_COMPILE_OK;
}

A840CompileStatus
a840_robustness_decide_for_stage(const A840StageInput *stage_input,
                                 uint32_t effective_flags,
                                 bool zero_page_ready,
                                 bool has_storage_images,
                                 bool has_fbfetch,
                                 A840RobustnessDecision *out_decision)
{
    if (stage_input == NULL || out_decision == NULL)
        return A840_COMPILE_INVALID_ARG;

    A840RobustnessInput req;
    memset(&req, 0, sizeof(req));
    req.stage = stage_input->stage;
    req.effective_flags = effective_flags;
    req.color_rt_mask = stage_input->color_rt_mask;
    req.half_rt_mask = stage_input->half_rt_mask;
    req.has_storage_images = has_storage_images;
    req.has_fbfetch = has_fbfetch;

    if (zero_page_ready) {
        req.policy_hints |= A840_ROBUST_HINT_ZERO_PAGE_READY;
        req.zero_page_bytes = A840_ZERO_PAGE_GUARD_BYTES;
    }
    if ((effective_flags & A840_COMPILE_FLAG_XR10_CLAMP) != 0u)
        req.policy_hints |= A840_ROBUST_HINT_XR10_PATH;
    if (has_storage_images)
        req.policy_hints |= A840_ROBUST_HINT_IMAGE_OOB_SENSITIVE;
    if (has_fbfetch || (effective_flags & A840_COMPILE_FLAG_FBFETCH_GMEM) != 0u)
        req.policy_hints |= A840_ROBUST_HINT_FBFETCH_ACTIVE;

    return a840_robustness_decide(&req, out_decision);
}
