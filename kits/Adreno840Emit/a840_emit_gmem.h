/* SPDX-License-Identifier: MIT */
#ifndef A840_EMIT_GMEM_H
#define A840_EMIT_GMEM_H
#include "a840_gmem_planner.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct A840GmemRange { uint64_t bo, va, bytes; } A840GmemRange;
/* One pre-laid-out, padded FDL 2D surface. Ranges begin at this layer.
 * Nonmutable tiled views use tiled swap. MSAA STORE_RESOLVE uses the
 * separate single-sample resolve surface, allowing an independent LOAD. */
typedef struct A840GmemSurface {
    A840GmemRange data, flags;
    uint32_t pitch, flag_pitch, flag_array_pitch, swap;
    bool tiled;
} A840GmemSurface;
typedef struct A840GmemBinding {
    uint32_t format, clear[4]; /* HW color/blit format, packed clear value */
    bool sample_zero; /* request sample 0; integer and Z/S always select it */
    bool srgb;
    A840GmemSurface image, resolve;
} A840GmemBinding;
typedef struct A840GmemVsc {
    /* NULL VSC selects unconditional per-tile replay. Otherwise each
     * stream contains 32 strides with a 64-byte guard per pipe. */
    A840GmemRange primitive, draw, sizes;
    uint32_t primitive_stride, draw_stride;
} A840GmemVsc;

A840EmitStatus a840_emit_gmem_binning_begin(A840CmdStream *, const A840GmemPlan *, const A840GmemVsc *);
A840EmitStatus a840_emit_gmem_binning_end(A840CmdStream *, const A840GmemVsc *);
A840EmitStatus a840_emit_gmem_tile_select(A840CmdStream *, const A840GmemPlan *,
    uint32_t tx, uint32_t ty, const A840GmemVsc *);
A840EmitStatus a840_emit_gmem_tile_clear(A840CmdStream *, const A840GmemPlan *, const A840GmemBinding bindings[]);
A840EmitStatus a840_emit_gmem_tile_load(A840CmdStream *, const A840GmemPlan *, const A840GmemBinding bindings[]);
A840EmitStatus a840_emit_gmem_tile_store(A840CmdStream *, const A840GmemPlan *, const A840GmemBinding bindings[]);
/* Non-final tile only. pass_end closes the final tile and restores sysmem. */
A840EmitStatus a840_emit_gmem_tile_end(A840CmdStream *);
A840EmitStatus a840_emit_gmem_pass_end(A840CmdStream *);
#ifdef __cplusplus
}
#endif
#endif
