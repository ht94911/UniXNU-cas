/* SPDX-License-Identifier: MIT */
#ifndef A840_GMEM_PLANNER_H
#define A840_GMEM_PLANNER_H
#include <stdbool.h>
#include "Adreno840EmitABI.h"
#include "chip_facts_a840.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { A840_GMEM_COLORS = 8, A840_GMEM_DEPTH = 8, A840_GMEM_STENCIL = 9,
       A840_GMEM_PLANES = 10, A840_GMEM_PIPES = 32 };
typedef enum { A840_LOAD_DONT_CARE, A840_LOAD_CLEAR, A840_LOAD_LOAD } A840GmemLoad;
typedef enum { A840_STORE_DONT_CARE, A840_STORE_STORE, A840_STORE_RESOLVE } A840GmemStore;
typedef enum { A840_PATH_SYSMEM, A840_PATH_GMEM } A840GmemPath;

typedef struct A840GmemAttachment {
    uint32_t cpp, samples; /* cpp == 0 means absent; samples are per pixel */
    A840GmemLoad load;
    A840GmemStore store;
    bool memoryless, ubwc;
} A840GmemAttachment;

typedef struct A840GmemPass {
    uint32_t width, height, draw_count;
    uint64_t covered_pixels; /* zero = unknown, estimate only for path selection */
    bool uses_fbfetch, has_msaa_resolve;
    A840GmemAttachment attachments[A840_GMEM_PLANES];
} A840GmemPass;

typedef struct A840GmemSlice {
    uint32_t offset, pitch, bytes; /* GPU address = A840_GMEM_RANGE_MIN + offset */
} A840GmemSlice;

typedef struct A840GmemPlan {
    A840GmemPass pass;
    A840GmemPath path;
    uint32_t ccu_budget, attachment_budget, used_bytes;
    uint32_t tile_w, tile_h, tiles_x, tiles_y;
    uint32_t pipe_w, pipe_h, pipes_x, pipes_y;
    A840GmemSlice planes[A840_GMEM_PLANES];
} A840GmemPlan;

typedef struct A840GmemTile {
    uint32_t x, y, width, height, pipe, slot;
} A840GmemTile;

/* Output is assigned only on success. Impossible mandatory GMEM returns
 * UNSUPPORTED; a non-mandatory oversized pass can use sysmem. */
A840EmitStatus a840_gmem_plan(const A840GmemPass *pass, A840GmemPlan *out);
A840EmitStatus a840_gmem_validate(const A840GmemPlan *plan);
A840EmitStatus a840_gmem_tile(const A840GmemPlan *plan, uint32_t tx,
                              uint32_t ty, A840GmemTile *out);
#ifdef __cplusplus
}
#endif
#endif
