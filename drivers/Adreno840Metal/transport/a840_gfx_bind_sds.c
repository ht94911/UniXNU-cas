/* SPDX-License-Identifier: MIT */
#include "a840_gfx_bind_sds.h"

#include <string.h>

#include "Adreno840Regs.h"
#include "a840_emit_ops.h"
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

/* Exact Mesa a6xx.xml:3592-3602 VFD_FETCH_INSTR_INSTR & VFD_VERTEX_BUFFER_STRIDE. */
enum {
    A840_VFD_MAX_SLOTS = 32u,
    A840_VFD_OFFSET_SHIFT = 5u,
    A840_VFD_OFFSET_MAX = 4095u,
    A840_VFD_INSTANCED_BIT = UINT32_C(1) << 17,
    A840_VFD_FORMAT_SHIFT = 20u,
    A840_VFD_SWAP_SHIFT = 28u,
    A840_VFD_UNK30_BIT = UINT32_C(1) << 30,
    A840_VFD_FLOAT_BIT = UINT32_C(1) << 31,
    A840_VFD_STRIDE_MAX = 4095u
};

int a840_vfd_map_mtl_vertex_format(uint32_t mtl_vertex_format,
                                   uint32_t *out_format_id,
                                   uint32_t *out_swap,
                                   uint8_t *out_is_float)
{
    uint32_t fmt = 0u;
    uint32_t swap = A840_WZYX;
    uint8_t is_float = 1u;
    switch (mtl_vertex_format) {
    case 1u:  fmt = A840_FMT6_8_8_UINT; is_float = 0u; break;              /* UChar2 */
    case 2u:  fmt = A840_FMT6_8_8_8_UINT; is_float = 0u; break;            /* UChar3 */
    case 3u:  fmt = A840_FMT6_8_8_8_8_UINT; is_float = 0u; break;          /* UChar4 */
    case 4u:  fmt = A840_FMT6_8_8_SINT; is_float = 0u; break;              /* Char2 */
    case 5u:  fmt = A840_FMT6_8_8_8_SINT; is_float = 0u; break;            /* Char3 */
    case 6u:  fmt = A840_FMT6_8_8_8_8_SINT; is_float = 0u; break;          /* Char4 */
    case 7u:  fmt = A840_FMT6_8_8_UNORM; is_float = 1u; break;             /* UChar2Normalized */
    case 8u:  fmt = A840_FMT6_8_8_8_UNORM; is_float = 1u; break;           /* UChar3Normalized */
    case 9u:  fmt = A840_FMT6_8_8_8_8_UNORM; is_float = 1u; break;         /* UChar4Normalized */
    case 10u: fmt = A840_FMT6_8_8_SNORM; is_float = 1u; break;             /* Char2Normalized */
    case 11u: fmt = A840_FMT6_8_8_8_SNORM; is_float = 1u; break;           /* Char3Normalized */
    case 12u: fmt = A840_FMT6_8_8_8_8_SNORM; is_float = 1u; break;         /* Char4Normalized */
    case 13u: fmt = A840_FMT6_16_16_UINT; is_float = 0u; break;            /* UShort2 */
    case 14u: fmt = A840_FMT6_16_16_16_UINT; is_float = 0u; break;         /* UShort3 */
    case 15u: fmt = A840_FMT6_16_16_16_16_UINT; is_float = 0u; break;      /* UShort4 */
    case 16u: fmt = A840_FMT6_16_16_SINT; is_float = 0u; break;            /* Short2 */
    case 17u: fmt = A840_FMT6_16_16_16_SINT; is_float = 0u; break;         /* Short3 */
    case 18u: fmt = A840_FMT6_16_16_16_16_SINT; is_float = 0u; break;      /* Short4 */
    case 19u: fmt = A840_FMT6_16_16_UNORM; is_float = 1u; break;           /* UShort2Normalized */
    case 20u: fmt = A840_FMT6_16_16_16_UNORM; is_float = 1u; break;        /* UShort3Normalized */
    case 21u: fmt = A840_FMT6_16_16_16_16_UNORM; is_float = 1u; break;     /* UShort4Normalized */
    case 22u: fmt = A840_FMT6_16_16_SNORM; is_float = 1u; break;           /* Short2Normalized */
    case 23u: fmt = A840_FMT6_16_16_16_SNORM; is_float = 1u; break;        /* Short3Normalized */
    case 24u: fmt = A840_FMT6_16_16_16_16_SNORM; is_float = 1u; break;     /* Short4Normalized */
    case 25u: fmt = A840_FMT6_16_16_FLOAT; is_float = 1u; break;           /* Half2 */
    case 26u: fmt = A840_FMT6_16_16_16_FLOAT; is_float = 1u; break;        /* Half3 */
    case 27u: fmt = A840_FMT6_16_16_16_16_FLOAT; is_float = 1u; break;     /* Half4 */
    case 28u: fmt = A840_FMT6_32_FLOAT; is_float = 1u; break;              /* Float */
    case 29u: fmt = A840_FMT6_32_32_FLOAT; is_float = 1u; break;           /* Float2 */
    case 30u: fmt = A840_FMT6_32_32_32_FLOAT; is_float = 1u; break;        /* Float3 */
    case 31u: fmt = A840_FMT6_32_32_32_32_FLOAT; is_float = 1u; break;     /* Float4 */
    case 32u: fmt = A840_FMT6_32_SINT; is_float = 0u; break;               /* Int */
    case 33u: fmt = A840_FMT6_32_32_SINT; is_float = 0u; break;            /* Int2 */
    case 34u: fmt = A840_FMT6_32_32_32_SINT; is_float = 0u; break;         /* Int3 */
    case 35u: fmt = A840_FMT6_32_32_32_32_SINT; is_float = 0u; break;      /* Int4 */
    case 36u: fmt = A840_FMT6_32_UINT; is_float = 0u; break;               /* UInt */
    case 37u: fmt = A840_FMT6_32_32_UINT; is_float = 0u; break;            /* UInt2 */
    case 38u: fmt = A840_FMT6_32_32_32_UINT; is_float = 0u; break;         /* UInt3 */
    case 39u: fmt = A840_FMT6_32_32_32_32_UINT; is_float = 0u; break;      /* UInt4 */
    case 40u: fmt = A840_FMT6_10_10_10_2_SNORM; is_float = 1u; break;      /* Int1010102Normalized */
    case 41u: fmt = A840_FMT6_10_10_10_2_UNORM; is_float = 1u; break;      /* UInt1010102Normalized */
    case 42u: fmt = A840_FMT6_8_8_8_8_UNORM; swap = A840_WXYZ; is_float = 1u; break; /* UChar4Normalized_BGRA */
    case 45u: fmt = A840_FMT6_8_UINT; is_float = 0u; break;                /* UChar */
    case 46u: fmt = A840_FMT6_8_SINT; is_float = 0u; break;                /* Char */
    case 47u: fmt = A840_FMT6_8_UNORM; is_float = 1u; break;               /* UCharNormalized */
    case 48u: fmt = A840_FMT6_8_SNORM; is_float = 1u; break;               /* CharNormalized */
    case 49u: fmt = A840_FMT6_16_UINT; is_float = 0u; break;               /* UShort */
    case 50u: fmt = A840_FMT6_16_SINT; is_float = 0u; break;               /* Short */
    case 51u: fmt = A840_FMT6_16_UNORM; is_float = 1u; break;              /* UShortNormalized */
    case 52u: fmt = A840_FMT6_16_SNORM; is_float = 1u; break;              /* ShortNormalized */
    case 53u: fmt = A840_FMT6_16_FLOAT; is_float = 1u; break;              /* Half */
    case 54u: fmt = A840_FMT6_11_11_10_FLOAT; is_float = 1u; break;        /* FloatRG11B10 */
    case 55u: fmt = A840_FMT6_9_9_9_E5_FLOAT; is_float = 1u; break;        /* FloatRGB9E5 */
    default:
        return 0;
    }
    if (out_format_id) *out_format_id = fmt;
    if (out_swap) *out_swap = swap;
    if (out_is_float) *out_is_float = is_float;
    return 1;
}

int a840_vfd_map_vk_vertex_format(uint32_t vk_format,
                                  uint32_t *out_format_id,
                                  uint32_t *out_swap,
                                  uint8_t *out_is_float)
{
    uint32_t fmt = A840_FMT6_32_32_32_32_FLOAT;
    uint32_t swap = A840_WZYX;
    uint8_t is_float = 1u;
    /* Exact Mesa fd6_format_table.c + tu_pipeline.cc (._float = !is_pure_integer). */
    switch (vk_format) {
    case 9u:   fmt = A840_FMT6_8_UNORM; is_float = 1u; break;              /* R8_UNORM */
    case 10u:  fmt = A840_FMT6_8_SNORM; is_float = 1u; break;              /* R8_SNORM */
    case 11u:  fmt = A840_FMT6_8_UINT; is_float = 1u; break;               /* R8_USCALED */
    case 12u:  fmt = A840_FMT6_8_SINT; is_float = 1u; break;               /* R8_SSCALED */
    case 13u:  fmt = A840_FMT6_8_UINT; is_float = 0u; break;               /* R8_UINT */
    case 14u:  fmt = A840_FMT6_8_SINT; is_float = 0u; break;               /* R8_SINT */
    case 16u:  fmt = A840_FMT6_8_8_UNORM; is_float = 1u; break;            /* R8G8_UNORM */
    case 17u:  fmt = A840_FMT6_8_8_SNORM; is_float = 1u; break;            /* R8G8_SNORM */
    case 18u:  fmt = A840_FMT6_8_8_UINT; is_float = 1u; break;             /* R8G8_USCALED */
    case 19u:  fmt = A840_FMT6_8_8_SINT; is_float = 1u; break;             /* R8G8_SSCALED */
    case 20u:  fmt = A840_FMT6_8_8_UINT; is_float = 0u; break;             /* R8G8_UINT */
    case 21u:  fmt = A840_FMT6_8_8_SINT; is_float = 0u; break;             /* R8G8_SINT */
    case 23u:  fmt = A840_FMT6_8_8_8_UNORM; is_float = 1u; break;          /* R8G8B8_UNORM */
    case 24u:  fmt = A840_FMT6_8_8_8_SNORM; is_float = 1u; break;          /* R8G8B8_SNORM */
    case 25u:  fmt = A840_FMT6_8_8_8_UINT; is_float = 1u; break;           /* R8G8B8_USCALED */
    case 26u:  fmt = A840_FMT6_8_8_8_SINT; is_float = 1u; break;           /* R8G8B8_SSCALED */
    case 27u:  fmt = A840_FMT6_8_8_8_UINT; is_float = 0u; break;           /* R8G8B8_UINT */
    case 28u:  fmt = A840_FMT6_8_8_8_SINT; is_float = 0u; break;           /* R8G8B8_SINT */
    case 37u:  fmt = A840_FMT6_8_8_8_8_UNORM; is_float = 1u; break;        /* R8G8B8A8_UNORM */
    case 38u:  fmt = A840_FMT6_8_8_8_8_SNORM; is_float = 1u; break;        /* R8G8B8A8_SNORM */
    case 39u:  fmt = A840_FMT6_8_8_8_8_UINT; is_float = 1u; break;         /* R8G8B8A8_USCALED */
    case 40u:  fmt = A840_FMT6_8_8_8_8_SINT; is_float = 1u; break;         /* R8G8B8A8_SSCALED */
    case 41u:  fmt = A840_FMT6_8_8_8_8_UINT; is_float = 0u; break;         /* R8G8B8A8_UINT */
    case 42u:  fmt = A840_FMT6_8_8_8_8_SINT; is_float = 0u; break;         /* R8G8B8A8_SINT */
    case 44u:  fmt = A840_FMT6_8_8_8_8_UNORM; swap = A840_WXYZ; is_float = 1u; break; /* B8G8R8A8_UNORM */
    case 45u:  fmt = A840_FMT6_8_8_8_8_SNORM; swap = A840_WXYZ; is_float = 1u; break; /* B8G8R8A8_SNORM */
    case 46u:  fmt = A840_FMT6_8_8_8_8_UINT; swap = A840_WXYZ; is_float = 1u; break;  /* B8G8R8A8_USCALED */
    case 47u:  fmt = A840_FMT6_8_8_8_8_SINT; swap = A840_WXYZ; is_float = 1u; break;  /* B8G8R8A8_SSCALED */
    case 48u:  fmt = A840_FMT6_8_8_8_8_UINT; swap = A840_WXYZ; is_float = 0u; break;  /* B8G8R8A8_UINT */
    case 49u:  fmt = A840_FMT6_8_8_8_8_SINT; swap = A840_WXYZ; is_float = 0u; break;  /* B8G8R8A8_SINT */
    case 64u:  fmt = A840_FMT6_10_10_10_2_UNORM; is_float = 1u; break;     /* A2B10G10R10_UNORM_PACK32 */
    case 65u:  fmt = A840_FMT6_10_10_10_2_SNORM; is_float = 1u; break;     /* A2B10G10R10_SNORM_PACK32 */
    case 66u:  fmt = A840_FMT6_10_10_10_2_UINT; is_float = 1u; break;      /* A2B10G10R10_USCALED_PACK32 */
    case 67u:  fmt = A840_FMT6_10_10_10_2_SINT; is_float = 1u; break;      /* A2B10G10R10_SSCALED_PACK32 */
    case 68u:  fmt = A840_FMT6_10_10_10_2_UINT; is_float = 0u; break;      /* A2B10G10R10_UINT_PACK32 */
    case 69u:  fmt = A840_FMT6_10_10_10_2_SINT; is_float = 0u; break;      /* A2B10G10R10_SINT_PACK32 */
    case 70u:  fmt = A840_FMT6_16_UNORM; is_float = 1u; break;             /* R16_UNORM */
    case 71u:  fmt = A840_FMT6_16_SNORM; is_float = 1u; break;             /* R16_SNORM */
    case 72u:  fmt = A840_FMT6_16_UINT; is_float = 1u; break;              /* R16_USCALED */
    case 73u:  fmt = A840_FMT6_16_SINT; is_float = 1u; break;              /* R16_SSCALED */
    case 74u:  fmt = A840_FMT6_16_UINT; is_float = 0u; break;              /* R16_UINT */
    case 75u:  fmt = A840_FMT6_16_SINT; is_float = 0u; break;              /* R16_SINT */
    case 76u:  fmt = A840_FMT6_16_FLOAT; is_float = 1u; break;             /* R16_SFLOAT */
    case 77u:  fmt = A840_FMT6_16_16_UNORM; is_float = 1u; break;          /* R16G16_UNORM */
    case 78u:  fmt = A840_FMT6_16_16_SNORM; is_float = 1u; break;          /* R16G16_SNORM */
    case 79u:  fmt = A840_FMT6_16_16_UINT; is_float = 1u; break;           /* R16G16_USCALED */
    case 80u:  fmt = A840_FMT6_16_16_SINT; is_float = 1u; break;           /* R16G16_SSCALED */
    case 81u:  fmt = A840_FMT6_16_16_UINT; is_float = 0u; break;           /* R16G16_UINT */
    case 82u:  fmt = A840_FMT6_16_16_SINT; is_float = 0u; break;           /* R16G16_SINT */
    case 83u:  fmt = A840_FMT6_16_16_FLOAT; is_float = 1u; break;          /* R16G16_SFLOAT */
    case 84u:  fmt = A840_FMT6_16_16_16_UNORM; is_float = 1u; break;       /* R16G16B16_UNORM */
    case 85u:  fmt = A840_FMT6_16_16_16_SNORM; is_float = 1u; break;       /* R16G16B16_SNORM */
    case 86u:  fmt = A840_FMT6_16_16_16_UINT; is_float = 1u; break;        /* R16G16B16_USCALED */
    case 87u:  fmt = A840_FMT6_16_16_16_SINT; is_float = 1u; break;        /* R16G16B16_SSCALED */
    case 88u:  fmt = A840_FMT6_16_16_16_UINT; is_float = 0u; break;        /* R16G16B16_UINT */
    case 89u:  fmt = A840_FMT6_16_16_16_SINT; is_float = 0u; break;        /* R16G16B16_SINT */
    case 90u:  fmt = A840_FMT6_16_16_16_FLOAT; is_float = 1u; break;       /* R16G16B16_SFLOAT */
    case 91u:  fmt = A840_FMT6_16_16_16_16_UNORM; is_float = 1u; break;    /* R16G16B16A16_UNORM */
    case 92u:  fmt = A840_FMT6_16_16_16_16_SNORM; is_float = 1u; break;    /* R16G16B16A16_SNORM */
    case 93u:  fmt = A840_FMT6_16_16_16_16_UINT; is_float = 1u; break;     /* R16G16B16A16_USCALED */
    case 94u:  fmt = A840_FMT6_16_16_16_16_SINT; is_float = 1u; break;     /* R16G16B16A16_SSCALED */
    case 95u:  fmt = A840_FMT6_16_16_16_16_UINT; is_float = 0u; break;     /* R16G16B16A16_UINT */
    case 96u:  fmt = A840_FMT6_16_16_16_16_SINT; is_float = 0u; break;     /* R16G16B16A16_SINT */
    case 97u:  fmt = A840_FMT6_16_16_16_16_FLOAT; is_float = 1u; break;    /* R16G16B16A16_SFLOAT */
    case 98u:  fmt = A840_FMT6_32_UINT; is_float = 0u; break;              /* R32_UINT */
    case 99u:  fmt = A840_FMT6_32_SINT; is_float = 0u; break;              /* R32_SINT */
    case 100u: fmt = A840_FMT6_32_FLOAT; is_float = 1u; break;             /* R32_SFLOAT */
    case 101u: fmt = A840_FMT6_32_32_UINT; is_float = 0u; break;           /* R32G32_UINT */
    case 102u: fmt = A840_FMT6_32_32_SINT; is_float = 0u; break;           /* R32G32_SINT */
    case 103u: fmt = A840_FMT6_32_32_FLOAT; is_float = 1u; break;          /* R32G32_SFLOAT */
    case 104u: fmt = A840_FMT6_32_32_32_UINT; is_float = 0u; break;        /* R32G32B32_UINT */
    case 105u: fmt = A840_FMT6_32_32_32_SINT; is_float = 0u; break;        /* R32G32B32_SINT */
    case 106u: fmt = A840_FMT6_32_32_32_FLOAT; is_float = 1u; break;       /* R32G32B32_SFLOAT */
    case 107u: fmt = A840_FMT6_32_32_32_32_UINT; is_float = 0u; break;     /* R32G32B32A32_UINT */
    case 108u: fmt = A840_FMT6_32_32_32_32_SINT; is_float = 0u; break;     /* R32G32B32A32_SINT */
    case 109u: fmt = A840_FMT6_32_32_32_32_FLOAT; is_float = 1u; break;    /* R32G32B32A32_SFLOAT */
    case 122u: fmt = A840_FMT6_11_11_10_FLOAT; is_float = 1u; break;       /* B10G11R11_UFLOAT_PACK32 */
    case 123u: fmt = A840_FMT6_9_9_9_E5_FLOAT; is_float = 1u; break;       /* E5B9G9R9_UFLOAT_PACK32 */
    default:
        return 0;
    }
    if (out_format_id) *out_format_id = fmt;
    if (out_swap) *out_swap = swap;
    if (out_is_float) *out_is_float = is_float;
    return 1;
}

A840EmitStatus a840_gfx_sds_emit_vfd(A840CmdStream *cs,
                                     uint32_t buffer_count,
                                     const A840GfxVfdBuffer *buffers,
                                     uint32_t attr_count,
                                     const A840GfxVfdAttr *attrs)
{
    uint32_t vb_words[4u * A840_VFD_MAX_SLOTS];
    uint32_t fetch_words[2u * A840_VFD_MAX_SLOTS];
    uint32_t i;
    A840EmitStatus est;

    if (!cs || buffer_count > A840_VFD_MAX_SLOTS || attr_count > A840_VFD_MAX_SLOTS)
        return A840_EMIT_INVALID_ARG;
    if ((buffer_count > 0u && !buffers) || (attr_count > 0u && !attrs))
        return A840_EMIT_INVALID_ARG;

    if (buffer_count > 0u) {
        for (i = 0u; i < buffer_count; ++i) {
            const uint64_t va = buffers[i].gpu_va;
            if (va >= (UINT64_C(1) << 48) || buffers[i].stride_bytes > A840_VFD_STRIDE_MAX)
                return A840_EMIT_RANGE_ERROR;
            vb_words[4u * i + 0u] = (uint32_t)va;
            vb_words[4u * i + 1u] = (uint32_t)(va >> 32);
            vb_words[4u * i + 2u] = buffers[i].size_bytes;
            vb_words[4u * i + 3u] = buffers[i].stride_bytes;
        }
        est = a840_cs_emit_pkt4(cs, A840_REG_VFD_VERTEX_BUFFER_0_BASE, vb_words, 4u * buffer_count);
        if (est != A840_EMIT_OK)
            return est;
    }

    if (attr_count > 0u) {
        for (i = 0u; i < attr_count; ++i) {
            const A840GfxVfdAttr *a = &attrs[i];
            if (a->vb_slot >= A840_VFD_MAX_SLOTS || a->offset_bytes > A840_VFD_OFFSET_MAX ||
                a->format_id > 255u || a->swap > 3u)
                return A840_EMIT_RANGE_ERROR;
            fetch_words[2u * i + 0u] =
                a->vb_slot |
                (a->offset_bytes << A840_VFD_OFFSET_SHIFT) |
                (a->instanced ? A840_VFD_INSTANCED_BIT : 0u) |
                (a->format_id << A840_VFD_FORMAT_SHIFT) |
                (a->swap << A840_VFD_SWAP_SHIFT) |
                A840_VFD_UNK30_BIT |
                (a->is_float ? A840_VFD_FLOAT_BIT : 0u);
            fetch_words[2u * i + 1u] = a->step_rate;
        }
        est = a840_cs_emit_pkt4(cs, A840_REG_VFD_FETCH_INSTR_0_INSTR, fetch_words, 2u * attr_count);
        if (est != A840_EMIT_OK)
            return est;
    }
    return A840_EMIT_OK;
}
