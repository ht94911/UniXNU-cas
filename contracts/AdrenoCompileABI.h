/* SPDX-License-Identifier: MIT */
/* Frozen Contract: AdrenoCompileABI.h (v1.0)
 * Owner: Lead Architect (Coordinator). Workers MUST NOT edit this file directly.
 *
 * Pure C ABI between Adreno840Metal / offline pack builder and the decoupled
 * `adreno_compile` static library (Mesa NIR -> ir3 -> A840 ISA + fdl layout)
 * with ZERO VkDevice / Vulkan header dependency in the runtime binary.
 */
#ifndef ADRENO_COMPILE_ABI_H
#define ADRENO_COMPILE_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ADRENO_COMPILE_ABI_VERSION = 1u,
    ADRENO_COMPILE_MAX_VARYINGS = 32u,
    ADRENO_COMPILE_MAX_UBO_PUSH = 16u,
    ADRENO_COMPILE_MAX_CONST_PUSH_DWORDS = 64u,
    ADRENO_COMPILE_MAX_BIND_SETS = 8u
};

typedef enum {
    A840_STAGE_VERTEX = 0,
    A840_STAGE_FRAGMENT = 1,
    A840_STAGE_COMPUTE = 2
} A840ShaderStage;

typedef enum {
    A840_COMPILE_OK = 0,
    A840_COMPILE_INVALID_ARG = 1,
    A840_COMPILE_ABI_MISMATCH = 2,
    A840_COMPILE_UNSUPPORTED_FEATURE = 3,
    A840_COMPILE_SPIRV_ERROR = 4,
    A840_COMPILE_IR3_ERROR = 5,
    A840_COMPILE_OOM = 6
} A840CompileStatus;

typedef enum {
    A840_COMPILE_FLAG_ROBUST_OFF = 1u << 0,       /* Phase 5: zero-page backed, no bounds checks */
    A840_COMPILE_FLAG_EARLY_PREAMBLE = 1u << 1,   /* A840 has_early_preamble */
    A840_COMPILE_FLAG_FBFETCH_GMEM = 1u << 2,     /* Native GMEM programmable blending */
    A840_COMPILE_FLAG_HALF_ROUND_RTZ = 1u << 3,   /* Apple Metal f32->f16 RTZ semantics */
    A840_COMPILE_FLAG_DISABLE_MERGED_REGS = 1u << 4
} A840CompileFlags;

typedef struct A840CompileTarget {
    uint32_t abi_version;        /* Must equal ADRENO_COMPILE_ABI_VERSION */
    uint32_t reserved0;
    uint64_t chip_id;            /* Default 0x44050a31 (Adreno 840 v2) */
    uint32_t num_slices;         /* 3 */
    uint32_t num_ccu;            /* 6 */
    uint32_t highest_bank_bit;   /* 16 on Kaanapali hardware */
    uint32_t ubwc_swizzle;       /* 6 */
    uint32_t macrotile_mode;     /* 1 */
    uint32_t flags;              /* Bitmask of A840CompileFlags */
} A840CompileTarget;

typedef struct A840UboPushRange {
    uint32_t set_index;
    uint32_t binding_index;
    uint32_t src_offset_vec4;
    uint32_t dst_const_vec4;
    uint32_t size_vec4;
    uint32_t reserved;
} A840UboPushRange;

typedef struct A840StageInput {
    uint32_t abi_version;
    uint32_t stage;              /* A840ShaderStage */
    const uint32_t *spirv_words;
    size_t spirv_bytes;
    const char *entry_point;     /* NUL-terminated UTF-8 */
    uint32_t color_rt_mask;      /* Bound color RT bitmask (FS) */
    uint32_t half_rt_mask;       /* 16-bit float RT bitmask (FS) */
    uint32_t sample_count;       /* 1, 2, 4 */
    uint32_t local_size[3];      /* Compute specialization or 0 */
    uint32_t const_push_mask;    /* Known constant push words */
    uint32_t const_push_dwords[ADRENO_COMPILE_MAX_CONST_PUSH_DWORDS];
    uint32_t flags;              /* Per-stage override of A840CompileFlags */
    uint32_t reserved;
} A840StageInput;

typedef struct A840CompiledStage {
    uint32_t abi_version;
    uint32_t stage;              /* A840ShaderStage */
    const uint32_t *isa_words;   /* Immutable until a840_compile_stage_free() */
    uint32_t isa_dwords;
    uint32_t instruction_groups; /* 16-byte / instruction group count */
    int32_t max_full_reg;        /* -1 if none */
    int32_t max_half_reg;        /* -1 if none */
    uint32_t branchstack;
    uint32_t constlen_vec4;
    uint32_t const_data_dwords;
    const uint32_t *const_data_words;
    uint32_t private_mem_bytes;
    uint32_t shared_mem_bytes;   /* Compute SLM */
    uint32_t local_size[3];
    uint32_t flags_out;          /* bit0:early_z bit1:writes_depth bit2:uses_fbfetch bit3:merged_regs bit4:early_preamble */
    uint32_t ubo_push_count;
    A840UboPushRange ubo_push[ADRENO_COMPILE_MAX_UBO_PUSH];
    uint32_t vpc_varying_count;
    uint32_t vpc_varyings[ADRENO_COMPILE_MAX_VARYINGS];
    const uint32_t *sds_state_words; /* Pre-encoded static register payload (optional in Phase 1) */
    uint32_t sds_state_dwords;
    uint32_t reg_spills;
    uint32_t reg_fills;
    uint32_t max_waves;
    void *owner;                 /* Opaque backing allocation freed by a840_compile_stage_free() */
} A840CompiledStage;

typedef struct A840TextureLayoutQuery {
    uint32_t abi_version;
    uint32_t mtl_pixel_format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_layers;
    uint32_t mip_levels;
    uint32_t sample_count;
    uint32_t tile_mode;          /* 0=linear, 1=tiled, 2=ubwc */
    uint32_t flags;              /* bit0:is_3d bit1:is_cube bit2:for_scanout */
} A840TextureLayoutQuery;

typedef struct A840TextureLayoutResult {
    uint64_t total_bytes;
    uint32_t base_align;
    uint32_t ubwc_enabled;
    uint32_t pitch0_bytes;
    uint32_t slice_stride_bytes;
    uint64_t ubwc_meta_offset;
    uint32_t ubwc_meta_pitch;
    uint32_t tex_const_template[16]; /* A840 64-byte texture descriptor template */
} A840TextureLayoutResult;

/* Entry points exported by `libadreno_compile.a` (the ONLY global symbols). */
A840CompileStatus a840_compile_stage(
    const A840CompileTarget *target,
    const A840StageInput *input,
    A840CompiledStage *out_stage,
    char *err_buf,
    size_t err_buf_bytes);

void a840_compile_stage_free(A840CompiledStage *stage);

A840CompileStatus a840_compute_texture_layout(
    const A840CompileTarget *target,
    const A840TextureLayoutQuery *query,
    A840TextureLayoutResult *out_layout);

uint64_t a840_compile_abi_fingerprint(void);

#if defined(__cplusplus)
static_assert(sizeof(A840CompileTarget) == 40, "A840CompileTarget layout locked");
static_assert(sizeof(A840UboPushRange) == 24, "A840UboPushRange layout locked");
static_assert(sizeof(A840TextureLayoutQuery) == 40, "A840TextureLayoutQuery layout locked");
static_assert(sizeof(A840TextureLayoutResult) == 104, "A840TextureLayoutResult layout locked");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(A840CompileTarget) == 40, "A840CompileTarget layout locked");
_Static_assert(sizeof(A840UboPushRange) == 24, "A840UboPushRange layout locked");
_Static_assert(sizeof(A840TextureLayoutQuery) == 40, "A840TextureLayoutQuery layout locked");
_Static_assert(sizeof(A840TextureLayoutResult) == 104, "A840TextureLayoutResult layout locked");
#endif

#ifdef __cplusplus
}
#endif

#endif /* ADRENO_COMPILE_ABI_H */
