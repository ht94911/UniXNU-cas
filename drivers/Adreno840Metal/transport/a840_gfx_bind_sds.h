/* SPDX-License-Identifier: MIT */
#ifndef A840_GFX_BIND_SDS_H
#define A840_GFX_BIND_SDS_H

#include <stddef.h>
#include <stdint.h>

#include "Adreno840EmitABI.h"
#include "AdrenoCompileABI.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct A840GfxConstUpload {
    uint32_t stage;         /* A840_STAGE_VERTEX or A840_STAGE_FRAGMENT */
    uint32_t dst_vec4;
    uint32_t count_vec4;
    uint32_t constlen_vec4;
    const uint32_t *words;  /* count_vec4 * 4 dwords */
} A840GfxConstUpload;

/* Encode A8XX SP_GFX_BINDLESS_BASE_0..N (64B descriptor mode) into cs. */
A840EmitStatus a840_gfx_sds_emit_bindless(A840CmdStream *cs,
                                          uint32_t set_count,
                                          const uint64_t *set_vas);

/* Encode A8XX CP_LOAD_STATE6_GEOM (VS) or CP_LOAD_STATE6_FRAG (FS) direct
 * constant upload into cs (for embedding inside an SDS draw state buffer). */
A840EmitStatus a840_gfx_sds_emit_consts(A840CmdStream *cs,
                                        const A840GfxConstUpload *upload);

typedef struct A840GfxVfdBuffer {
    uint64_t gpu_va;
    uint32_t size_bytes;
    uint32_t stride_bytes;
} A840GfxVfdBuffer;

typedef struct A840GfxVfdAttr {
    uint32_t vb_slot;       /* 0..31 index into buffers[] */
    uint32_t offset_bytes;  /* 0..4095 byte offset into vertex buffer */
    uint32_t format_id;     /* A840_FMT6_* */
    uint32_t swap;          /* A840_WZYX / A840_WXYZ */
    uint32_t step_rate;     /* instance divisor (0 for per-vertex / constant) */
    uint8_t instanced;      /* 1 for per-instance, 0 for per-vertex */
    uint8_t is_float;       /* 1 for float/unorm/snorm, 0 for pure uint/sint */
} A840GfxVfdAttr;

/* Map Apple MTLVertexFormat enum value to A8XX VFD format/swap/float flags. */
int a840_vfd_map_mtl_vertex_format(uint32_t mtl_vertex_format,
                                   uint32_t *out_format_id,
                                   uint32_t *out_swap,
                                   uint8_t *out_is_float);

/* Emit VFD_VERTEX_BUFFER_0..N and VFD_FETCH_INSTR_0..M into SDS command stream. */
A840EmitStatus a840_gfx_sds_emit_vfd(A840CmdStream *cs,
                                     uint32_t buffer_count,
                                     const A840GfxVfdBuffer *buffers,
                                     uint32_t attr_count,
                                     const A840GfxVfdAttr *attrs);

#ifdef __cplusplus
}
#endif

#endif /* A840_GFX_BIND_SDS_H */
