/* SPDX-License-Identifier: MIT */
/* Wave 12 / Lane 2: Deterministic 6-Domain iOS 27 System pack.bin (A840PK01)
 * Builder & Zero-Copy mmap Verifier.
 *
 * Compiles >= 36 real system shaders across all 6 iOS 27 (24A446) domains:
 *   0. CA_LIQUID_GLASS  - CoreAnimation Liquid Glass (GMEM FBFetch, blur, XR10)
 *   1. SPRINGBOARD      - SpringBoard icons, folders, spring animation, dock
 *   2. UIKIT_SCROLL     - UIKit text/SDF, rounded corners, shadows, scroll tiles
 *   3. METAL_BUILTIN    - Metal blit copy/clear/scale, mipmap, MSAA resolve, ICB
 *   4. CAMERA155        - Camera 155 NV12/YUV->BGRA, XR10 preview, 90deg crop, RAW
 *   5. COREIMAGE_MPS    - CoreImage convolution/color matrix/unsharp, MPS GEMM/conv
 *
 * Uses real a840_compile_stage() (pinned Mesa 85fda3b36405 non-Vulkan ir3) and
 * a840_pack_v1, attaching deterministic pre-encoded static SDS PM4 blocks and
 * enforcing 100% XR10 wide-color safety, valid instrlen*8, and 0 unbound UBO
 * block1 references. Pure C11, Darwin-compatible, zero Turnip/Vulkan symbols.
 */
#ifndef A840_SYSTEM_PACK_BUILDER_H
#define A840_SYSTEM_PACK_BUILDER_H

#include "AdrenoCompileABI.h"
#include "a840_pack_v1.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_SYSDOMAIN_COUNT            = 6u,
    A840_SYSTEM_PACK_MIN_SHADERS    = 36u,
    A840_SYSTEM_PACK_MAX_CATALOG    = 64u,
    A840_SYSDOMAIN_ALL_MASK         = 0x3fu
};

typedef enum A840SystemPackDomain {
    A840_SYSDOMAIN_CA_LIQUID_GLASS = 0,
    A840_SYSDOMAIN_SPRINGBOARD     = 1,
    A840_SYSDOMAIN_UIKIT_SCROLL    = 2,
    A840_SYSDOMAIN_METAL_BUILTIN   = 3,
    A840_SYSDOMAIN_CAMERA155       = 4,
    A840_SYSDOMAIN_COREIMAGE_MPS   = 5
} A840SystemPackDomain;

typedef enum A840SystemPackStatus {
    A840_SYSPACK_OK = 0,
    A840_SYSPACK_INVALID_ARG = 1,
    A840_SYSPACK_COMPILE_ERROR = 2,
    A840_SYSPACK_XR10_VIOLATION = 3,
    A840_SYSPACK_INSTRLEN_ERROR = 4,
    A840_SYSPACK_UBO_BLOCK1_ERROR = 5,
    A840_SYSPACK_PACK_ERROR = 6,
    A840_SYSPACK_VERIFY_ERROR = 7,
    A840_SYSPACK_IO_ERROR = 8,
    A840_SYSPACK_OOM = 9,
    A840_SYSPACK_PATH_FORBIDDEN = 10
} A840SystemPackStatus;

typedef struct A840SystemShaderDesc {
    uint32_t index;
    A840SystemPackDomain domain;
    const char *name;
    bool is_render;           /* false = 1-stage compute, true = 2-stage VS+FS */
    bool uses_fbfetch;        /* true = GMEM input attachment FBFetch */
    bool requires_xr10;       /* true = Apple XR10 [-0.752941, 1.25098] safe */
    uint32_t compile_flags;
    uint32_t color_rt_mask;
    uint32_t half_rt_mask;
    uint32_t sample_count;
    uint32_t local_size[3];
    const char *entry_name;
    const char *fs_entry_name;
} A840SystemShaderDesc;

/* Optional stage compiler hook (NULL = invoke real a840_compile_stage / free). */
typedef A840CompileStatus (*A840SystemPackStageCompileFn)(
    void *user_ctx,
    const A840CompileTarget *target,
    const A840StageInput *input,
    A840CompiledStage *out_stage,
    char *err_buf,
    size_t err_buf_bytes);

typedef void (*A840SystemPackStageFreeFn)(
    void *user_ctx,
    A840CompiledStage *stage);

typedef struct A840SystemPackBuildConfig {
    uint64_t stack_key;
    uint64_t abi_fingerprint;        /* 0 -> a840_compile_abi_fingerprint() */
    uint32_t tier;                   /* 0 = Tier-0 static system pack */
    uint32_t domain_mask;            /* 0 -> A840_SYSDOMAIN_ALL_MASK */
    bool attach_sds_blocks;          /* Pre-encode deterministic static SDS PM4 */
    bool verify_after_build;         /* Zero-copy open + lookup benchmark */
    uint32_t bench_iters;            /* 0 -> default 200 lookups per entry */
    A840SystemPackStageCompileFn compile_stage_fn;
    A840SystemPackStageFreeFn free_stage_fn;
    void *compile_user_ctx;
} A840SystemPackBuildConfig;

typedef struct A840SystemPackBuildReport {
    uint32_t total_entries;
    uint32_t domain_counts[A840_SYSDOMAIN_COUNT];
    uint32_t total_stages;
    uint32_t fbfetch_stages;
    uint32_t xr10_checked_entries;
    uint32_t xr10_safe_entries;
    uint32_t valid_instrlen_stages;
    uint32_t unbound_ubo_block1_count;
    uint32_t sds_attached_stages;
    size_t pack_bytes;
    uint8_t pack_sha256[32];
    double lookup_mean_us;
    double lookup_p50_us;
    double lookup_p99_us;
    uint32_t entry_key_count;
    A840PipelineKey entry_keys[A840_SYSTEM_PACK_MAX_CATALOG];
} A840SystemPackBuildReport;

typedef struct A840SystemPackVerifyReport {
    uint32_t entry_count;
    uint32_t tier;
    size_t pack_bytes;
    uint8_t pack_sha256[32];
    bool all_isa_128b_aligned;
    bool all_sds_present;
    bool all_instrlen_valid;
    uint32_t unbound_ubo_block1_count;
    double lookup_mean_us;
    double lookup_p50_us;
    double lookup_p99_us;
} A840SystemPackVerifyReport;

const char *a840_system_pack_status_string(A840SystemPackStatus status);
const char *a840_system_pack_domain_name(A840SystemPackDomain domain);

size_t a840_system_pack_catalog_count(void);
const A840SystemShaderDesc *a840_system_pack_catalog_get(size_t index);

/* Synthesize canonical deterministic SPIR-V 1.3 words for catalog entry `index`
 * and stage `stage_idx` (0 = CS or VS, 1 = FS for render entries).
 * Caller owns *out_words and must free() it.
 */
A840SystemPackStatus a840_system_pack_entry_spirv(
    size_t index,
    uint32_t stage_idx,
    uint32_t **out_words,
    size_t *out_bytes);

/* Compile all selected domains and build a deterministic A840PK01 pack.bin
 * image in a 128-byte-aligned malloc-owned buffer (caller must free()).
 */
A840SystemPackStatus a840_system_pack_build_memory(
    const A840SystemPackBuildConfig *cfg,
    uint8_t **out_pack_buf,
    size_t *out_pack_bytes,
    A840SystemPackBuildReport *out_report);

/* Build and atomically persist pack.bin to output_path (rejecting C:/mnt/c,
 * /tmp, and /private/var/ngpu paths), then optionally mmap-verify.
 */
A840SystemPackStatus a840_system_pack_build_to_file(
    const A840SystemPackBuildConfig *cfg,
    const char *output_path,
    A840SystemPackBuildReport *out_report);

/* Zero-copy mmap verification + P50/P99 lookup latency benchmark on an
 * existing pack.bin file.
 */
A840SystemPackStatus a840_system_pack_verify_file_mmap(
    const char *pack_path,
    uint64_t stack_key,
    uint64_t abi_fingerprint,
    uint32_t bench_iters,
    A840SystemPackVerifyReport *out_report);

#ifdef __cplusplus
}
#endif

#endif /* A840_SYSTEM_PACK_BUILDER_H */
