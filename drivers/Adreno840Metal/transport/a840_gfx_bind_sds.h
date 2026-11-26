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

#ifdef __cplusplus
}
#endif

#endif /* A840_GFX_BIND_SDS_H */
