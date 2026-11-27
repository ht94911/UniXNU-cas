/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_METAL_TRANSLATE_H
#define NATIVE_METAL_TRANSLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NmbDevice NmbDevice;
typedef struct NmbLibrary NmbLibrary;
typedef struct NmbSpirvResult NmbSpirvResult;

enum {
    /* Retained for the Phase 5 backend. Wave 1 preserves translator robustness. */
    NMB_DEVICE_FLAG_ROBUST_OFF = 1u << 0,
    NMB_STAGE_VERTEX_BIT = 1u << 0,
    NMB_STAGE_FRAGMENT_BIT = 1u << 1,
    NMB_STAGE_COMPUTE_BIT = 1u << 2
};

/* Pipeline-wide descriptor reflection. A Metal slot index is UINT32_MAX
 * when the descriptor has no corresponding slot of that kind. */
typedef struct NmbSpirvBinding {
    uint32_t descriptor_set;
    uint32_t binding;
    uint32_t descriptor_type; /* VkDescriptorType numeric value. */
    uint32_t descriptor_count;
    uint32_t stage_mask;      /* NMB_STAGE_*_BIT. */
    uint32_t buffer_index;
    uint32_t texture_index;
    uint32_t sampler_index;
} NmbSpirvBinding;

uint32_t nmb_translate_abi_version(void);

/* Last error on this thread, empty before use and after a successful call.
 * Read or copy the borrowed string before the next call on this thread.
 * This query and the ABI version query preserve the current error. */
const char *nmb_last_error(void);

/* Locate AIR64 bytes in a big-endian universal container (up to 16 arches),
 * or return the whole input when it starts with MTLB. The caller supplies len
 * readable bytes and two distinct writable size_t slots. Failure leaves them
 * untouched. Use nmb_library_parse to open a native Mach-O shader archive. */
bool nmb_extract_air_slice(const uint8_t *bytes, size_t len,
                           size_t *out_offset, size_t *out_len);

/* Destroy/free accept NULL. A returned handle is owned by the caller and
 * kept alive for the duration of any call that borrows it. */
NmbDevice *nmb_device_create_a840(uint32_t flags);
void nmb_device_destroy(NmbDevice *dev);
uint32_t nmb_device_flags(const NmbDevice *dev);

/* Reads len bytes and retains its own storage. In a native Mach-O archive,
 * named AIR library members are exposed. Pre-specialized alias companions
 * are skipped because the ABI selects functions by name. */
NmbLibrary *nmb_library_parse(const NmbDevice *dev, const uint8_t *bytes, size_t len);
void nmb_library_destroy(NmbLibrary *lib);
size_t nmb_library_function_count(const NmbLibrary *lib);
/* Names are UTF-8. out_buf needs room for the entire name and its NUL.
 * Failure leaves it untouched. Archive member order is preserved. */
bool nmb_library_function_name(const NmbLibrary *lib, size_t index,
                               char *out_buf, size_t out_cap);

NmbSpirvResult *nmb_compile_compute_spirv(const NmbDevice *dev, const NmbLibrary *lib,
                                          const char *fn_name);
NmbSpirvResult *nmb_compile_render_spirv(const NmbDevice *dev, const NmbLibrary *lib,
                                         const char *vertex_fn, const char *fragment_fn,
                                         uint32_t color0_mtl_format);

/* Stage 0 is compute or vertex. Stage 1 is fragment. Words and entry point
 * remain valid until res is freed. Failure leaves out_bytes untouched. */
const uint32_t *nmb_spirv_words(const NmbSpirvResult *res, uint32_t stage_index,
                                size_t *out_bytes);
const char *nmb_spirv_entry_point(const NmbSpirvResult *res, uint32_t stage_index);
uint32_t nmb_spirv_stage_count(const NmbSpirvResult *res);
size_t nmb_spirv_binding_count(const NmbSpirvResult *res);
bool nmb_spirv_binding(const NmbSpirvResult *res, size_t index, NmbSpirvBinding *out);
void nmb_spirv_result_free(NmbSpirvResult *res);

/* Additive API. Existing entry points retain the v1 descriptor defaults and
 * SPIR-V binding numbers. Extended pipelines use A840 slot numbers in both
 * SPIR-V and NmbSpirvBinding. */
enum {
    NMB_MAX_COLORS = 8,
    NMB_MAX_VERTEX_ATTRIBUTES = 31,
    NMB_MAX_VERTEX_LAYOUTS = 31,
    NMB_FUNCTION_VERTEX = 0,
    NMB_FUNCTION_FRAGMENT = 1,
    NMB_FUNCTION_KERNEL = 2,
    NMB_FUNCTION_EXPORTED = 3,
    NMB_FUNCTION_VISIBLE = 4,
    NMB_FUNCTION_EXTERN = 5,
    NMB_FUNCTION_INTERSECTION = 6,
    NMB_FUNCTION_MESH = 7,
    NMB_FUNCTION_OBJECT = 8
};

typedef struct NmbVertexAttribute {
    uint32_t attribute_index, buffer_index, offset, mtl_vertex_format;
} NmbVertexAttribute;

typedef struct NmbVertexLayout {
    uint32_t buffer_index;
    uint32_t stride;        /* Bytes. UINT32_MAX requests a dynamic stride. */
    uint32_t step_function; /* MTLVertexStepFunction. */
    uint32_t step_rate;
} NmbVertexLayout;

typedef struct NmbRenderPipelineDesc {
    uint32_t color_attachment_count; /* Span 0..8; format 0 leaves a hole. */
    uint32_t color_mtl_formats[NMB_MAX_COLORS];
    uint32_t depth_mtl_format;       /* 0 leaves the aspect unbound. */
    uint32_t stencil_mtl_format;
    uint32_t sample_count;           /* 1, 2 or 4. */
    uint32_t vertex_attribute_count;
    NmbVertexAttribute vertex_attributes[NMB_MAX_VERTEX_ATTRIBUTES];
    uint32_t vertex_layout_count;
    NmbVertexLayout vertex_layouts[NMB_MAX_VERTEX_LAYOUTS];
} NmbRenderPipelineDesc;

typedef struct NmbComputePipelineDesc {
    uint32_t local_size[3]; /* All zero leaves the workgroup size specializable. */
    /* 1 enables explicit initialization of AIR-declared zeroed memory. */
    uint32_t zeroed_threadgroup_memory;
} NmbComputePipelineDesc;

typedef struct NmbSpirvPushConstantRange {
    uint32_t offset_bytes, size_bytes; /* Zero size means no reflected range. */
} NmbSpirvPushConstantRange;

/* Same index and ordering as nmb_library_function_name. Returns the raw
 * metallib TYPE value in out_kind, including types outside the AOT subset. */
bool nmb_library_function_stage_kind(const NmbLibrary *lib, size_t index,
                                     uint32_t *out_kind);

/* Offline defaults from vertex metadata and a compatible attachment probe.
 * Runtime callers supply the actual vertex layout and attachment formats. */
bool nmb_library_default_render_desc(const NmbDevice *dev, const NmbLibrary *lib,
                                     const char *vertex_fn, const char *fragment_fn,
                                     NmbRenderPipelineDesc *out);

NmbSpirvResult *nmb_compile_render_spirv_ex(const NmbDevice *dev, const NmbLibrary *lib,
                                           const char *vertex_fn, const char *fragment_fn,
                                           const NmbRenderPipelineDesc *desc);
NmbSpirvResult *nmb_compile_compute_spirv_ex(const NmbDevice *dev, const NmbLibrary *lib,
                                            const char *fn_name,
                                            const NmbComputePipelineDesc *desc);
bool nmb_spirv_push_constant_range(const NmbSpirvResult *res, uint32_t stage_index,
                                   NmbSpirvPushConstantRange *out);

/* Additive identity-domain push-constant placement reflection & handle-word helpers. */
enum {
    NMB_PUSH_NONE = 0,
    NMB_PUSH_BUFFER_ADDRESS = 1,
    NMB_PUSH_BUFFER_BOUNDS = 2,
    NMB_PUSH_ZERO_ADDRESS = 3,
    NMB_PUSH_TEXTURE_ID = 4,
    NMB_PUSH_SAMPLER_ID = 5,
    NMB_PUSH_DISPATCH_REGION = 6,
    NMB_PUSH_STAGE_IN_REGION = 7,
    NMB_PUSH_TEXTURE_LEVELS = 8,
    NMB_PUSH_TEXTURE_LEVEL_BASE = 9,
    NMB_PUSH_INTEGER_CHANNELS = 10,
    NMB_PUSH_ARGUMENT_TABLE_ADDRESS = 11,
    NMB_PUSH_RENDER_TARGET_ARRAY_LENGTH = 12,
    NMB_PUSH_VIEWPORT_COUNT = 13,
    NMB_PUSH_VERTICES_PER_PRIMITIVE = 14,
    NMB_PUSH_TAIL_WORD = 15,
    NMB_PUSH_ZERO_U32 = 16,
    NMB_PUSH_ZERO_U64 = 17
};

typedef struct NmbSpirvPushField {
    uint32_t kind;         /* NMB_PUSH_* */
    uint32_t in_arg_table; /* 0 = push window, 1 = argument table BO */
    uint32_t offset_bytes;
    uint32_t size_bytes;
    uint32_t stage_mask;   /* NMB_STAGE_*_BIT */
    uint32_t metal_slot;   /* Metal buffer/texture/sampler index, or UINT32_MAX */
    uint32_t extra;        /* Reserved / auxiliary metadata */
} NmbSpirvPushField;

size_t nmb_spirv_push_field_count(const NmbSpirvResult *res, uint32_t stage_index);
bool nmb_spirv_push_field(const NmbSpirvResult *res, uint32_t stage_index,
                          size_t index, NmbSpirvPushField *out);
uint32_t nmb_spirv_arg_table_bytes(const NmbSpirvResult *res, uint32_t stage_index);
uint32_t nmb_encode_sampler_word(bool normalized_coordinates,
                                 uint32_t min_filter, uint32_t mag_filter,
                                 uint32_t mip_filter,
                                 uint32_t s_address_mode, uint32_t t_address_mode,
                                 uint32_t r_address_mode,
                                 uint32_t border_color, uint32_t compare_function);
uint32_t nmb_encode_texture_word(uint32_t mtl_pixel_format);

/* Additive v2 pipeline API: function constant specialization + cross-library render stages. */
typedef struct NmbFunctionConstant {
    uint32_t mtl_data_type; /* MTLDataType raw value */
    uint32_t index;         /* Used when name == NULL */
    const char *name;       /* NULL for indexed constant; UTF-8 for named constant */
    const void *bytes;      /* Raw value bytes */
    size_t length;          /* Byte length of value */
} NmbFunctionConstant;

NmbSpirvResult *nmb_compile_compute_spirv_v2(const NmbDevice *dev,
                                             const NmbLibrary *lib,
                                             const char *fn_name,
                                             const NmbFunctionConstant *constants,
                                             size_t constant_count,
                                             const NmbComputePipelineDesc *desc);

NmbSpirvResult *nmb_compile_render_spirv_v2(const NmbDevice *dev,
                                            const NmbLibrary *vs_lib,
                                            const char *vertex_fn,
                                            const NmbFunctionConstant *vs_constants,
                                            size_t vs_constant_count,
                                            const NmbLibrary *fs_lib,
                                            const char *fragment_fn,
                                            const NmbFunctionConstant *fs_constants,
                                            size_t fs_constant_count,
                                            const NmbRenderPipelineDesc *desc);

#ifdef __cplusplus
}
#endif

#endif /* NATIVE_METAL_TRANSLATE_H */
