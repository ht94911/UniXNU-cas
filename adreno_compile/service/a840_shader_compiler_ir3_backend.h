/* SPDX-License-Identifier: MIT */
/* Real NativeMetalTranslate + Mesa ir3 service backend bridge for
 * NezhaShaderCompiler (W08-L2).
 *
 * Pure C11, zero Vulkan headers/symbols, Darwin-compatible.
 * Defines the deterministic A840ScIr3CompilePayload wire layout (supporting
 * COMPUTE and RENDER_VS_FS with either metallib AIR or raw SPIR-V input),
 * a thread-safe compile context (A840ScIr3Backend), and the A840ScCompileFn
 * callback that translates and compiles payloads into validated A840PR01
 * pipeline records bound to stack_key and abi_fingerprint.
 */
#ifndef A840_SHADER_COMPILER_IR3_BACKEND_H
#define A840_SHADER_COMPILER_IR3_BACKEND_H

#include "a840_shader_compiler_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_SC_IR3_PAYLOAD_MAGIC = 0x31524938u, /* "8IR1" little-endian */
    A840_SC_IR3_PAYLOAD_VERSION = 1u,
    A840_SC_IR3_PAYLOAD_HEADER_BYTES = 256u,
    A840_SC_IR3_ENTRY_NAME_MAX = 64u,
    A840_SC_IR3_MAX_COLOR_RTS = 8u
};

typedef enum A840ScIr3PipelineKind {
    A840_SC_IR3_PIPELINE_COMPUTE = 1u,
    A840_SC_IR3_PIPELINE_RENDER_VS_FS = 2u
} A840ScIr3PipelineKind;

typedef enum A840ScIr3SourceKind {
    A840_SC_IR3_SRC_METALLIB = 1u, /* AIR64/MTLB/Mach-O metallib -> NativeMetalTranslate -> ir3 */
    A840_SC_IR3_SRC_SPIRV = 2u     /* Raw canonical little-endian SPIR-V -> ir3 */
} A840ScIr3SourceKind;

/* Canonical wire header layout (256 bytes, strictly little-endian, never memcpy'd):
 *   [  0..  4) magic                 (uint32_t, A840_SC_IR3_PAYLOAD_MAGIC)
 *   [  4..  8) version               (uint32_t, A840_SC_IR3_PAYLOAD_VERSION)
 *   [  8.. 12) pipeline_kind         (uint32_t, A840ScIr3PipelineKind)
 *   [ 12.. 16) source_kind           (uint32_t, A840ScIr3SourceKind)
 *   [ 16.. 20) compile_flags         (uint32_t, bitmask of A840CompileFlags)
 *   [ 20.. 24) color_rt_mask         (uint32_t, bitmask of bound color RTs 0..7)
 *   [ 24.. 28) half_rt_mask          (uint32_t, bitmask of f16 color RTs 0..7)
 *   [ 28.. 32) sample_count          (uint32_t, 1, 2, or 4)
 *   [ 32.. 44) local_size[3]         (3 x uint32_t, compute workgroup size)
 *   [ 44.. 76) color_mtl_formats[8]  (8 x uint32_t, MTLPixelFormat per RT)
 *   [ 76.. 80) depth_mtl_format      (uint32_t)
 *   [ 80.. 84) stencil_mtl_format    (uint32_t)
 *   [ 84.. 88) stage0_bytes          (uint32_t: metallib bytes, or CS/VS SPIR-V bytes)
 *   [ 88.. 92) stage1_bytes          (uint32_t: 0 for metallib/compute, or FS SPIR-V bytes)
 *   [ 92.. 96) reserved0             (uint32_t, MUST be 0)
 *   [ 96..104) reserved1             (uint64_t, MUST be 0)
 *   [104..128) reserved2[24]         (24 x uint8_t, MUST be 0)
 *   [128..192) entry_name[64]        (NUL-terminated UTF-8 compute/vertex entry name)
 *   [192..256) fs_entry_name[64]     (NUL-terminated UTF-8 fragment entry name; zeroed for compute)
 *
 * Followed immediately by:
 *   stage0_data[stage0_bytes]
 *   stage1_data[stage1_bytes]
 */
typedef struct A840ScIr3CompilePayload {
    uint32_t magic;
    uint32_t version;
    uint32_t pipeline_kind;
    uint32_t source_kind;
    uint32_t compile_flags;
    uint32_t color_rt_mask;
    uint32_t half_rt_mask;
    uint32_t sample_count;
    uint32_t local_size[3];
    uint32_t color_mtl_formats[A840_SC_IR3_MAX_COLOR_RTS];
    uint32_t depth_mtl_format;
    uint32_t stencil_mtl_format;
    uint32_t stage0_bytes;
    uint32_t stage1_bytes;
    uint32_t reserved0;
    uint64_t reserved1;
    char entry_name[A840_SC_IR3_ENTRY_NAME_MAX];
    char fs_entry_name[A840_SC_IR3_ENTRY_NAME_MAX];
} A840ScIr3CompilePayload;

typedef struct A840ScIr3Backend A840ScIr3Backend;

typedef struct A840ScIr3BackendConfig {
    uint64_t stack_key;
    uint64_t abi_fingerprint;       /* 0 -> use a840_compile_abi_fingerprint() */
    uint32_t nmb_device_flags;      /* NMB_DEVICE_FLAG_* */
    uint32_t default_compile_flags; /* Default A840CompileFlags ORed into compile_flags */
    bool enable_translate;          /* Enable NativeMetalTranslate for SRC_METALLIB */
} A840ScIr3BackendConfig;

typedef struct A840ScIr3BackendStats {
    uint64_t total_calls;
    uint64_t succeeded;
    uint64_t failed_payload;
    uint64_t failed_translate;
    uint64_t failed_ir3;
    uint64_t failed_record;
    uint64_t compute_compiled;
    uint64_t render_compiled;
} A840ScIr3BackendStats;

/* Encode a deterministic A840ScIr3CompilePayload header + body bytes into
 * caller-supplied buffer. Validates all fields, entry names, and sizes.
 * On failure, out_bytes is untouched.
 */
A840ScStatus a840_sc_ir3_payload_encode(const A840ScIr3CompilePayload *hdr,
    const void *stage0_data, const void *stage1_data,
    uint8_t *out_buf, size_t out_capacity, size_t *out_bytes);

/* Decode and strictly validate a wire payload buffer.
 * On success, *out_stage0 borrows into payload + 256 and *out_stage1 borrows
 * into payload + 256 + stage0_bytes (or NULL if stage1_bytes == 0).
 * On failure, output pointers are untouched.
 */
A840ScStatus a840_sc_ir3_payload_decode(const uint8_t *payload,
    size_t payload_bytes, A840ScIr3CompilePayload *out_hdr,
    const uint8_t **out_stage0, const uint8_t **out_stage1);

/* Compute a deterministic A840PipelineKey directly from a validated encoded
 * payload (suitable for clients submitting requests to NezhaShaderCompiler).
 */
A840ScStatus a840_sc_ir3_payload_key(const uint8_t *payload,
    size_t payload_bytes, A840PipelineKey *out_key);

/* Create/destroy the thread-safe real IR3 + NativeMetalTranslate backend. */
A840ScStatus a840_sc_ir3_backend_create(const A840ScIr3BackendConfig *cfg,
    A840ScIr3Backend **out_backend);
void a840_sc_ir3_backend_destroy(A840ScIr3Backend *backend);
void a840_sc_ir3_backend_get_stats(A840ScIr3Backend *backend,
    A840ScIr3BackendStats *out_stats);

/* A840ScCompileFn callback implementation for A840ScServiceConfig.
 * user_ctx MUST be an A840ScIr3Backend* created with a840_sc_ir3_backend_create.
 * Thread-safe for concurrent invocation from 1..8 service workers.
 */
A840ScStatus a840_sc_ir3_compile_callback(void *user_ctx, uint32_t worker_id,
    const A840ScFrameHeader *request, const uint8_t *payload,
    uint8_t **out_record_blob, size_t *out_record_bytes);

#ifdef __cplusplus
}
#endif
#endif /* A840_SHADER_COMPILER_IR3_BACKEND_H */
