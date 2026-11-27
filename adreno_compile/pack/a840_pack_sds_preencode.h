/* SPDX-License-Identifier: MIT */
#ifndef A840_PACK_SDS_PREENCODE_H
#define A840_PACK_SDS_PREENCODE_H

#include <stddef.h>
#include <stdint.h>

#include "AdrenoCompileABI.h"
#include "Adreno840EmitABI.h"
#include "a840_pack_v1.h"

#if defined(__has_include)
#if __has_include("a840_emit_3d.h")
#include "a840_emit_3d.h"
#include "a840_emit_compute.h"
#else
#include "../../kits/Adreno840Emit/a840_emit_3d.h"
#include "../../kits/Adreno840Emit/a840_emit_compute.h"
#endif
#else
#include "../../kits/Adreno840Emit/a840_emit_3d.h"
#include "../../kits/Adreno840Emit/a840_emit_compute.h"
#endif

#define A840_PACK_SDS_MAX_DWORDS A840_EMIT_3D_PROGRAM_MAX_DWORDS
#define A840_PACK_SDS_MAX_RELOCS 8u

typedef enum A840SdsPreencodeStatus {
    A840_SDS_OK = 0,
    A840_SDS_INVALID_ARG = 1,
    A840_SDS_CAPACITY = 2,
    A840_SDS_RANGE_ERROR = 3,
    A840_SDS_EMIT_ERROR = 4,
    A840_SDS_VERIFY_ERROR = 5,
    A840_SDS_IO_ERROR = 6
} A840SdsPreencodeStatus;

typedef enum A840SdsEncodeMode {
    /* Pure static register state with ZERO runtime BO GPU VA registers.
     * Safe for read-only zero-copy pack.bin -> CP_SET_DRAW_STATE binding. */
    A840_SDS_MODE_STATIC_ONLY = 0,
    /* Full shader program PM4 template with deterministic relocation list
     * for patching SP_*_BASE and SP_*_PVT_MEM_BASE at load/bind time. */
    A840_SDS_MODE_RELOCATABLE_TEMPLATE = 1
} A840SdsEncodeMode;

typedef enum A840SdsPipelineKind {
    A840_SDS_PIPELINE_COMPUTE = 1,
    A840_SDS_PIPELINE_GRAPHICS = 2
} A840SdsPipelineKind;

typedef enum A840SdsRelocKind {
    A840_SDS_RELOC_NONE = 0,
    A840_SDS_RELOC_VS_CODE = 1,
    A840_SDS_RELOC_FS_CODE = 2,
    A840_SDS_RELOC_CS_CODE = 3,
    A840_SDS_RELOC_VS_PVT_MEM = 4,
    A840_SDS_RELOC_FS_PVT_MEM = 5,
    A840_SDS_RELOC_CS_PVT_MEM = 6
} A840SdsRelocKind;

typedef struct A840SdsRelocEntry {
    uint32_t dword_offset; /* Low 32-bit word offset inside A840SdsBlock.words */
    uint32_t reg_id;       /* Named A840_REG_* register identifier */
    uint32_t kind;         /* A840SdsRelocKind */
    uint32_t flags;        /* A840RelocFlags */
} A840SdsRelocEntry;

typedef struct A840SdsBlock {
    uint32_t words[A840_PACK_SDS_MAX_DWORDS];
    uint32_t word_count;
    uint32_t mode;          /* A840SdsEncodeMode */
    uint32_t pipeline_kind; /* A840SdsPipelineKind */
    uint32_t enable_mask;   /* A840_SDS_{BINNING,GMEM,SYSMEM} bitmask */
    uint32_t packet_count;
    uint32_t reloc_count;
    A840SdsRelocEntry relocs[A840_PACK_SDS_MAX_RELOCS];
    uint8_t sha256[32];
} A840SdsBlock;

typedef struct A840SdsPatchAddrs {
    uint64_t vs_code_va;
    uint64_t fs_code_va;
    uint64_t cs_code_va;
    uint64_t vs_pvt_va;
    uint64_t fs_pvt_va;
    uint64_t cs_pvt_va;
} A840SdsPatchAddrs;

#ifdef __cplusplus
extern "C" {
#endif

const char *a840_pack_sds_status_string(A840SdsPreencodeStatus status);

/* Pre-encode a compute pipeline's static PM4 state block using kits/Adreno840Emit.
 * Transactional: any error leaves *out_block untouched. */
A840SdsPreencodeStatus a840_pack_sds_preencode_compute(
    const A840ComputePipelineDesc *desc,
    A840SdsEncodeMode mode,
    A840SdsBlock *out_block);

/* Pre-encode a VS+FS graphics pipeline's static PM4 state block using kits/Adreno840Emit.
 * enable_mask must be a non-zero subset of A840_SDS_BINNING|GMEM|SYSMEM.
 * Transactional: any error leaves *out_block untouched. */
A840SdsPreencodeStatus a840_pack_sds_preencode_graphics(
    const A840GraphicsProgramDesc *desc,
    A840SdsEncodeMode mode,
    uint32_t enable_mask,
    A840SdsBlock *out_block);

/* Attach a verified SDS block to A840CompiledStage.sds_state_words/dwords
 * for pack.bin v1 serialization (caller keeps block alive until build). */
A840SdsPreencodeStatus a840_pack_sds_attach_stage(
    const A840SdsBlock *block,
    A840CompiledStage *stage);

/* Deterministically validate a pre-encoded SDS PM4 dword block.
 * Verifies every packet header/parity/bounds via a840_pm4_decode, checks
 * Type4-only state discipline, and in STATIC_ONLY rejects any BO VA register. */
A840SdsPreencodeStatus a840_pack_sds_verify(
    const uint32_t *words,
    uint32_t word_count,
    A840SdsEncodeMode mode,
    uint32_t *out_packet_count);

/* Emit a CP_SET_DRAW_STATE binding packet referencing a pre-encoded SDS block
 * (e.g., zero-copy GPU VA inside a read-only mapped pack.bin v1). */
A840SdsPreencodeStatus a840_pack_sds_emit_bind(
    A840CmdStream *cs,
    uint32_t group_id,
    uint32_t enable_mask,
    A840GpuAddress sds_gpu_addr,
    const uint32_t *sds_words,
    uint32_t sds_dwords);

/* Patch 48-bit GPU VAs into a copy of a RELOCATABLE_TEMPLATE SDS block.
 * Transactional: validates all addresses and offsets before mutating words. */
A840SdsPreencodeStatus a840_pack_sds_patch_relocs(
    uint32_t *words,
    uint32_t word_count,
    const A840SdsBlock *tmpl,
    const A840SdsPatchAddrs *addrs);

/* Export a verified SDS or CS PM4 dword stream to a little-endian .pm4 binary
 * file for offline pm4_check.py --chip A8XX verification. Rejects C:/mnt/c paths. */
A840SdsPreencodeStatus a840_pack_sds_export_pm4_file(
    const char *path,
    const uint32_t *words,
    uint32_t word_count);

#ifdef __cplusplus
}
#endif

#endif /* A840_PACK_SDS_PREENCODE_H */
