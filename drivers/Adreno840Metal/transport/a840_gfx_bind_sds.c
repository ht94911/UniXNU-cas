/* SPDX-License-Identifier: MIT */
#include "a840_gfx_bind_sds.h"

#include <string.h>

#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

/* Mesa adreno_pm4.xml:810 (SB6_VS_SHADER=8) & :814 (SB6_FS_SHADER=12). */
enum {
    A840_GFX_LOAD_VS_SHADER = 8u,
    A840_GFX_LOAD_FS_SHADER = 12u,
    A840_GFX_MAX_DIRECT_VEC4 = 64u,
    A840_GFX_MAX_CONST_VEC4 = 512u
};

A840EmitStatus a840_gfx_sds_emit_bindless(A840CmdStream *cs,
                                          uint32_t set_count,
                                          const uint64_t *set_vas)
{
    uint32_t words[2u * ADRENO_COMPILE_MAX_BIND_SETS];
    uint32_t i;

    if (!cs || set_count > ADRENO_COMPILE_MAX_BIND_SETS)
        return A840_EMIT_INVALID_ARG;
    if (set_count == 0u)
        return A840_EMIT_OK;
    if (!set_vas)
        return A840_EMIT_INVALID_ARG;

    for (i = 0u; i < set_count; ++i) {
        const uint64_t va = set_vas[i];
        if (va != 0u) {
            if ((va % 64u) != 0u || va >= (UINT64_C(1) << 48))
                return A840_EMIT_ALIGNMENT;
            words[2u * i] = (uint32_t)va | A840_MODE_BINDLESS_64B;
            words[2u * i + 1u] = (uint32_t)(va >> 32);
        } else {
            words[2u * i] = 0u;
            words[2u * i + 1u] = 0u;
        }
    }
    return a840_cs_emit_pkt4(cs, A840_REG_SP_GFX_BINDLESS_BASE_0_DESCRIPTOR,
                             words, 2u * set_count);
}

A840EmitStatus a840_gfx_sds_emit_consts(A840CmdStream *cs,
                                        const A840GfxConstUpload *upload)
{
    uint32_t payload[3u + 4u * A840_GFX_MAX_DIRECT_VEC4];
    uint32_t block;
    uint32_t opcode;
    uint32_t control;

    if (!cs || !upload)
        return A840_EMIT_INVALID_ARG;
    if (upload->count_vec4 == 0u)
        return A840_EMIT_OK;
    if (!upload->words || upload->count_vec4 > A840_GFX_MAX_DIRECT_VEC4 ||
        upload->constlen_vec4 > A840_GFX_MAX_CONST_VEC4 ||
        upload->dst_vec4 > upload->constlen_vec4 ||
        upload->count_vec4 > upload->constlen_vec4 - upload->dst_vec4) {
        return A840_EMIT_RANGE_ERROR;
    }

    if (upload->stage == A840_STAGE_VERTEX) {
        block = A840_GFX_LOAD_VS_SHADER;
        opcode = A840_CP_LOAD_STATE6_GEOM;
    } else if (upload->stage == A840_STAGE_FRAGMENT) {
        block = A840_GFX_LOAD_FS_SHADER;
        opcode = A840_CP_LOAD_STATE6_FRAG;
    } else {
        return A840_EMIT_INVALID_ARG;
    }

    control = (upload->dst_vec4 << A840_LOAD_DST_SHIFT) |
              (A840_LOAD_CONSTANTS << A840_LOAD_TYPE_SHIFT) |
              (A840_LOAD_DIRECT << A840_LOAD_SOURCE_SHIFT) |
              (block << A840_LOAD_BLOCK_SHIFT) |
              (upload->count_vec4 << A840_LOAD_UNITS_SHIFT);
    payload[0] = control;
    payload[1] = 0u;
    payload[2] = 0u;
    memcpy(payload + 3u, upload->words, (size_t)upload->count_vec4 * 16u);
    return a840_cs_emit_pkt7(cs, opcode, payload, 3u + 4u * upload->count_vec4);
}
