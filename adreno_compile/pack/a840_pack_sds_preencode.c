/* SPDX-License-Identifier: MIT */
#if defined(A840_BUILD_SDS_PREENCODE)
#define A840_SDS_PREENCODE_ENABLED 1
#elif defined(__has_include)
#if __has_include("a840_emit_3d.h")
#define A840_SDS_PREENCODE_ENABLED 1
#else
#define A840_SDS_PREENCODE_ENABLED 0
#endif
#else
#define A840_SDS_PREENCODE_ENABLED 0
#endif

#if A840_SDS_PREENCODE_ENABLED

#include "a840_pack_sds_preencode.h"
#include "Adreno840Regs.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    A840_SDS_VALID_ENABLE_MASK = A840_SDS_BINNING | A840_SDS_GMEM | A840_SDS_SYSMEM,
    A840_SDS_MAX_PACKETS = A840_PACK_SDS_MAX_DWORDS,
    A840_SDS_CODE_ALIGN = 128u,
    A840_SDS_PVT_ALIGN = 32u
};

/* Canonical 64 KiB-aligned synthetic template VAs used only while invoking
 * kits/Adreno840Emit to validate bounds and generate static register images.
 * They are stripped in STATIC_ONLY mode or zeroed and recorded in relocs in
 * RELOCATABLE_TEMPLATE mode, never leaked into persisted pack.bin payloads. */
static const uint64_t A840_SDS_TMPL_VS_CODE_VA = UINT64_C(65536) * UINT64_C(1);
static const uint64_t A840_SDS_TMPL_FS_CODE_VA = UINT64_C(65536) * UINT64_C(2);
static const uint64_t A840_SDS_TMPL_CS_CODE_VA = UINT64_C(65536) * UINT64_C(3);
static const uint64_t A840_SDS_TMPL_VS_PVT_VA  = UINT64_C(65536) * UINT64_C(4);
static const uint64_t A840_SDS_TMPL_FS_PVT_VA  = UINT64_C(65536) * UINT64_C(8);
static const uint64_t A840_SDS_TMPL_CS_PVT_VA  = UINT64_C(65536) * UINT64_C(12);
static const uint64_t A840_SDS_GPU_VA_LIMIT    = UINT64_C(1) << 48;

static bool
is_bo_va_register(uint32_t reg_id)
{
    return reg_id == A840_REG_SP_VS_BASE ||
           reg_id == A840_REG_SP_PS_BASE ||
           reg_id == A840_REG_SP_CS_BASE ||
           reg_id == A840_REG_SP_VS_PVT_MEM_BASE ||
           reg_id == A840_REG_SP_PS_PVT_MEM_BASE ||
           reg_id == A840_REG_SP_CS_PVT_MEM_BASE;
}

static bool
packet_touches_bo_va_register(const A840PM4Packet *pkt)
{
    if (pkt->type != 4u)
        return false;
    for (uint32_t i = 0u; i < pkt->payload_words; ++i) {
        if (is_bo_va_register(pkt->identifier + i))
            return true;
    }
    return false;
}

static A840SdsRelocKind
reg_to_reloc_kind(uint32_t reg_id)
{
    if (reg_id == A840_REG_SP_VS_BASE)
        return A840_SDS_RELOC_VS_CODE;
    if (reg_id == A840_REG_SP_PS_BASE)
        return A840_SDS_RELOC_FS_CODE;
    if (reg_id == A840_REG_SP_CS_BASE)
        return A840_SDS_RELOC_CS_CODE;
    if (reg_id == A840_REG_SP_VS_PVT_MEM_BASE)
        return A840_SDS_RELOC_VS_PVT_MEM;
    if (reg_id == A840_REG_SP_PS_PVT_MEM_BASE)
        return A840_SDS_RELOC_FS_PVT_MEM;
    if (reg_id == A840_REG_SP_CS_PVT_MEM_BASE)
        return A840_SDS_RELOC_CS_PVT_MEM;
    return A840_SDS_RELOC_NONE;
}

static bool
valid_mode(A840SdsEncodeMode mode)
{
    return mode == A840_SDS_MODE_STATIC_ONLY ||
           mode == A840_SDS_MODE_RELOCATABLE_TEMPLATE;
}

static bool
valid_enable_mask(uint32_t enable_mask)
{
    return enable_mask != 0u && (enable_mask & ~A840_SDS_VALID_ENABLE_MASK) == 0u;
}

static bool
path_forbidden(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return true;
    if (tolower((unsigned char)path[0]) == 'c' && path[1] == ':')
        return true;
    if (strncmp(path, "/mnt/c", 6u) == 0 &&
        (path[6] == '\0' || path[6] == '/'))
        return true;
    if (strncmp(path, "/mnt/C", 6u) == 0 &&
        (path[6] == '\0' || path[6] == '/'))
        return true;
    return false;
}

static A840SdsPreencodeStatus
finalize_from_stream(const A840CmdStream *emitted,
                     A840SdsEncodeMode mode,
                     A840SdsPipelineKind kind,
                     uint32_t enable_mask,
                     A840SdsBlock *out_block)
{
    A840PM4Packet packets[A840_SDS_MAX_PACKETS];
    size_t decoded_count = 0u;
    if (emitted->used_dwords == 0u || emitted->used_dwords > A840_PACK_SDS_MAX_DWORDS)
        return A840_SDS_RANGE_ERROR;
    if (a840_pm4_decode(emitted->words, emitted->used_dwords,
                        packets, A840_SDS_MAX_PACKETS, &decoded_count) != A840_EMIT_OK) {
        return A840_SDS_VERIFY_ERROR;
    }

    A840SdsBlock block;
    memset(&block, 0, sizeof(block));
    block.mode = (uint32_t)mode;
    block.pipeline_kind = (uint32_t)kind;
    block.enable_mask = enable_mask;

    if (mode == A840_SDS_MODE_STATIC_ONLY) {
        A840CmdStream dst;
        if (a840_cs_init(&dst, block.words, A840_PACK_SDS_MAX_DWORDS, NULL, 0u) != A840_EMIT_OK)
            return A840_SDS_EMIT_ERROR;
        for (size_t i = 0u; i < decoded_count; ++i) {
            const A840PM4Packet *pkt = &packets[i];
            if (pkt->type != 4u)
                return A840_SDS_VERIFY_ERROR;
            if (packet_touches_bo_va_register(pkt))
                continue;
            const uint32_t *payload = &emitted->words[pkt->header_word + 1u];
            if (a840_cs_emit_pkt4(&dst, pkt->identifier, payload, pkt->payload_words) != A840_EMIT_OK)
                return A840_SDS_EMIT_ERROR;
        }
        if (dst.used_dwords == 0u || dst.used_dwords > A840_PACK_SDS_MAX_DWORDS)
            return A840_SDS_VERIFY_ERROR;
        block.word_count = (uint32_t)dst.used_dwords;
        block.reloc_count = 0u;
    } else {
        memcpy(block.words, emitted->words, emitted->used_dwords * sizeof(uint32_t));
        block.word_count = (uint32_t)emitted->used_dwords;
        if (emitted->reloc_count > A840_PACK_SDS_MAX_RELOCS)
            return A840_SDS_CAPACITY;
        for (size_t r = 0u; r < emitted->reloc_count; ++r) {
            const A840Reloc *rel = &emitted->relocs[r];
            uint32_t found_reg = UINT32_MAX;
            for (size_t i = 0u; i < decoded_count; ++i) {
                const A840PM4Packet *pkt = &packets[i];
                size_t start = pkt->header_word + 1u;
                size_t end = start + pkt->payload_words;
                if (pkt->type == 4u &&
                    rel->dword_offset >= start &&
                    (size_t)rel->dword_offset + 2u <= end) {
                    found_reg = pkt->identifier + (uint32_t)(rel->dword_offset - start);
                    break;
                }
            }
            A840SdsRelocKind rkind = reg_to_reloc_kind(found_reg);
            if (rkind == A840_SDS_RELOC_NONE)
                return A840_SDS_VERIFY_ERROR;
            block.relocs[block.reloc_count].dword_offset = rel->dword_offset;
            block.relocs[block.reloc_count].reg_id = found_reg;
            block.relocs[block.reloc_count].kind = (uint32_t)rkind;
            block.relocs[block.reloc_count].flags = rel->flags;
            block.reloc_count++;
            /* Clear synthetic template VA words so templates carry zero fake VAs. */
            block.words[rel->dword_offset] = 0u;
            block.words[rel->dword_offset + 1u] = 0u;
        }
    }

    uint32_t verified_packets = 0u;
    A840SdsPreencodeStatus vst = a840_pack_sds_verify(
        block.words, block.word_count, mode, &verified_packets);
    if (vst != A840_SDS_OK)
        return vst;
    block.packet_count = verified_packets;

    if (a840_pack_v1_sha256(block.words, (size_t)block.word_count * sizeof(uint32_t),
                            block.sha256) != A840_PACK_OK) {
        return A840_SDS_VERIFY_ERROR;
    }

    *out_block = block;
    return A840_SDS_OK;
}

const char *
a840_pack_sds_status_string(A840SdsPreencodeStatus status)
{
    switch (status) {
    case A840_SDS_OK:           return "OK";
    case A840_SDS_INVALID_ARG:  return "INVALID_ARG";
    case A840_SDS_CAPACITY:     return "CAPACITY";
    case A840_SDS_RANGE_ERROR:  return "RANGE_ERROR";
    case A840_SDS_EMIT_ERROR:   return "EMIT_ERROR";
    case A840_SDS_VERIFY_ERROR: return "VERIFY_ERROR";
    case A840_SDS_IO_ERROR:     return "IO_ERROR";
    default:                    return "UNKNOWN";
    }
}

A840SdsPreencodeStatus
a840_pack_sds_preencode_compute(const A840ComputePipelineDesc *desc,
                                A840SdsEncodeMode mode,
                                A840SdsBlock *out_block)
{
    if (desc == NULL || out_block == NULL || desc->shader.stage == NULL || !valid_mode(mode))
        return A840_SDS_INVALID_ARG;
    if (desc->shader.stage->abi_version != ADRENO_COMPILE_ABI_VERSION ||
        desc->shader.stage->stage != A840_STAGE_COMPUTE) {
        return A840_SDS_INVALID_ARG;
    }

    A840ComputePipelineDesc patched = *desc;
    if (patched.shader.code.gpu_va == 0u)
        patched.shader.code.gpu_va = A840_SDS_TMPL_CS_CODE_VA;
    if (patched.shader.stage->private_mem_bytes > 0u &&
        patched.shader.private_memory.gpu_va == 0u) {
        patched.shader.private_memory.gpu_va = A840_SDS_TMPL_CS_PVT_VA;
    }

    uint32_t scratch_words[A840_EMIT_COMPUTE_PIPELINE_MAX_DWORDS];
    A840Reloc scratch_relocs[A840_PACK_SDS_MAX_RELOCS];
    A840CmdStream cs;
    if (a840_cs_init(&cs, scratch_words, A840_EMIT_COMPUTE_PIPELINE_MAX_DWORDS,
                     scratch_relocs, A840_PACK_SDS_MAX_RELOCS) != A840_EMIT_OK) {
        return A840_SDS_EMIT_ERROR;
    }
    if (a840_emit_compute_pipeline(&cs, &patched) != A840_EMIT_OK)
        return A840_SDS_EMIT_ERROR;

    return finalize_from_stream(&cs, mode, A840_SDS_PIPELINE_COMPUTE,
                                A840_SDS_SYSMEM, out_block);
}

A840SdsPreencodeStatus
a840_pack_sds_preencode_graphics(const A840GraphicsProgramDesc *desc,
                                 A840SdsEncodeMode mode,
                                 uint32_t enable_mask,
                                 A840SdsBlock *out_block)
{
    if (desc == NULL || out_block == NULL ||
        desc->vs.stage == NULL || desc->fs.stage == NULL ||
        !valid_mode(mode) || !valid_enable_mask(enable_mask)) {
        return A840_SDS_INVALID_ARG;
    }
    if (desc->vs.stage->abi_version != ADRENO_COMPILE_ABI_VERSION ||
        desc->fs.stage->abi_version != ADRENO_COMPILE_ABI_VERSION ||
        desc->vs.stage->stage != A840_STAGE_VERTEX ||
        desc->fs.stage->stage != A840_STAGE_FRAGMENT) {
        return A840_SDS_INVALID_ARG;
    }

    A840GraphicsProgramDesc patched = *desc;
    if (patched.vs.code.gpu_va == 0u)
        patched.vs.code.gpu_va = A840_SDS_TMPL_VS_CODE_VA;
    if (patched.fs.code.gpu_va == 0u)
        patched.fs.code.gpu_va = A840_SDS_TMPL_FS_CODE_VA;
    if (patched.vs.stage->private_mem_bytes > 0u &&
        patched.vs.private_memory.gpu_va == 0u) {
        patched.vs.private_memory.gpu_va = A840_SDS_TMPL_VS_PVT_VA;
    }
    if (patched.fs.stage->private_mem_bytes > 0u &&
        patched.fs.private_memory.gpu_va == 0u) {
        patched.fs.private_memory.gpu_va = A840_SDS_TMPL_FS_PVT_VA;
    }

    uint32_t scratch_words[A840_EMIT_3D_PROGRAM_MAX_DWORDS];
    A840Reloc scratch_relocs[A840_PACK_SDS_MAX_RELOCS];
    A840CmdStream cs;
    if (a840_cs_init(&cs, scratch_words, A840_EMIT_3D_PROGRAM_MAX_DWORDS,
                     scratch_relocs, A840_PACK_SDS_MAX_RELOCS) != A840_EMIT_OK) {
        return A840_SDS_EMIT_ERROR;
    }
    if (a840_emit_3d_program(&cs, &patched) != A840_EMIT_OK)
        return A840_SDS_EMIT_ERROR;

    return finalize_from_stream(&cs, mode, A840_SDS_PIPELINE_GRAPHICS,
                                enable_mask, out_block);
}

A840SdsPreencodeStatus
a840_pack_sds_verify(const uint32_t *words,
                     uint32_t word_count,
                     A840SdsEncodeMode mode,
                     uint32_t *out_packet_count)
{
    if (words == NULL || word_count == 0u || !valid_mode(mode))
        return A840_SDS_INVALID_ARG;
    if (word_count > A840_RECORD_MAX_SDS_DWORDS || word_count > A840_PACK_SDS_MAX_DWORDS)
        return A840_SDS_RANGE_ERROR;

    A840PM4Packet packets[A840_SDS_MAX_PACKETS];
    size_t count = 0u;
    if (a840_pm4_decode(words, word_count, packets, A840_SDS_MAX_PACKETS, &count) != A840_EMIT_OK)
        return A840_SDS_VERIFY_ERROR;
    if (count == 0u)
        return A840_SDS_VERIFY_ERROR;

    for (size_t i = 0u; i < count; ++i) {
        if (packets[i].type != 4u || packets[i].payload_words == 0u)
            return A840_SDS_VERIFY_ERROR;
        if (mode == A840_SDS_MODE_STATIC_ONLY && packet_touches_bo_va_register(&packets[i]))
            return A840_SDS_VERIFY_ERROR;
    }

    if (out_packet_count != NULL)
        *out_packet_count = (uint32_t)count;
    return A840_SDS_OK;
}

A840SdsPreencodeStatus
a840_pack_sds_attach_stage(const A840SdsBlock *block, A840CompiledStage *stage)
{
    if (block == NULL || stage == NULL)
        return A840_SDS_INVALID_ARG;
    if (stage->abi_version != ADRENO_COMPILE_ABI_VERSION || stage->stage > A840_STAGE_COMPUTE)
        return A840_SDS_INVALID_ARG;

    A840SdsEncodeMode mode = (A840SdsEncodeMode)block->mode;
    uint32_t pkt_count = 0u;
    A840SdsPreencodeStatus st = a840_pack_sds_verify(
        block->words, block->word_count, mode, &pkt_count);
    if (st != A840_SDS_OK)
        return st;
    if (pkt_count != block->packet_count)
        return A840_SDS_VERIFY_ERROR;

    uint8_t digest[32];
    if (a840_pack_v1_sha256(block->words, (size_t)block->word_count * sizeof(uint32_t),
                            digest) != A840_PACK_OK) {
        return A840_SDS_VERIFY_ERROR;
    }
    if (memcmp(digest, block->sha256, sizeof(digest)) != 0)
        return A840_SDS_VERIFY_ERROR;

    stage->sds_state_words = block->words;
    stage->sds_state_dwords = block->word_count;
    return A840_SDS_OK;
}

A840SdsPreencodeStatus
a840_pack_sds_emit_bind(A840CmdStream *cs,
                        uint32_t group_id,
                        uint32_t enable_mask,
                        A840GpuAddress sds_gpu_addr,
                        const uint32_t *sds_words,
                        uint32_t sds_dwords)
{
    if (cs == NULL || sds_words == NULL || sds_dwords == 0u ||
        group_id >= ADRENO840_MAX_SDS_SLOTS || !valid_enable_mask(enable_mask)) {
        return A840_SDS_INVALID_ARG;
    }
    A840SdsPreencodeStatus vst = a840_pack_sds_verify(
        sds_words, sds_dwords, A840_SDS_MODE_RELOCATABLE_TEMPLATE, NULL);
    if (vst != A840_SDS_OK)
        return vst;

    A840SdsGroupDesc grp;
    grp.group_id = group_id;
    grp.enable_mask = enable_mask;
    grp.state_dwords = sds_dwords;
    grp.state = sds_gpu_addr;

    A840EmitStatus est = a840_emit_sds_group(cs, &grp, 1u);
    if (est == A840_EMIT_CAPACITY)
        return A840_SDS_CAPACITY;
    if (est == A840_EMIT_RANGE_ERROR)
        return A840_SDS_RANGE_ERROR;
    if (est != A840_EMIT_OK)
        return A840_SDS_EMIT_ERROR;
    return A840_SDS_OK;
}

static bool
resolve_patch_va(A840SdsRelocKind kind, const A840SdsPatchAddrs *addrs,
                 uint64_t *out_va, uint32_t *out_align)
{
    switch (kind) {
    case A840_SDS_RELOC_VS_CODE:
        *out_va = addrs->vs_code_va;
        *out_align = A840_SDS_CODE_ALIGN;
        return true;
    case A840_SDS_RELOC_FS_CODE:
        *out_va = addrs->fs_code_va;
        *out_align = A840_SDS_CODE_ALIGN;
        return true;
    case A840_SDS_RELOC_CS_CODE:
        *out_va = addrs->cs_code_va;
        *out_align = A840_SDS_CODE_ALIGN;
        return true;
    case A840_SDS_RELOC_VS_PVT_MEM:
        *out_va = addrs->vs_pvt_va;
        *out_align = A840_SDS_PVT_ALIGN;
        return true;
    case A840_SDS_RELOC_FS_PVT_MEM:
        *out_va = addrs->fs_pvt_va;
        *out_align = A840_SDS_PVT_ALIGN;
        return true;
    case A840_SDS_RELOC_CS_PVT_MEM:
        *out_va = addrs->cs_pvt_va;
        *out_align = A840_SDS_PVT_ALIGN;
        return true;
    default:
        return false;
    }
}

A840SdsPreencodeStatus
a840_pack_sds_patch_relocs(uint32_t *words,
                           uint32_t word_count,
                           const A840SdsBlock *tmpl,
                           const A840SdsPatchAddrs *addrs)
{
    if (words == NULL || tmpl == NULL || addrs == NULL)
        return A840_SDS_INVALID_ARG;
    if (tmpl->mode != (uint32_t)A840_SDS_MODE_RELOCATABLE_TEMPLATE ||
        tmpl->word_count == 0u || word_count < tmpl->word_count ||
        tmpl->reloc_count > A840_PACK_SDS_MAX_RELOCS) {
        return A840_SDS_INVALID_ARG;
    }

    /* Validate all relocations before mutating caller buffer. */
    uint64_t resolved[A840_PACK_SDS_MAX_RELOCS];
    for (uint32_t i = 0u; i < tmpl->reloc_count; ++i) {
        const A840SdsRelocEntry *rel = &tmpl->relocs[i];
        if (rel->dword_offset + 2u > tmpl->word_count)
            return A840_SDS_RANGE_ERROR;
        uint64_t va = 0u;
        uint32_t align = 0u;
        if (!resolve_patch_va((A840SdsRelocKind)rel->kind, addrs, &va, &align))
            return A840_SDS_INVALID_ARG;
        if (va == 0u || va >= A840_SDS_GPU_VA_LIMIT || (va % align) != 0u)
            return A840_SDS_RANGE_ERROR;
        resolved[i] = va;
    }

    uint32_t staging[A840_PACK_SDS_MAX_DWORDS];
    memcpy(staging, tmpl->words, (size_t)tmpl->word_count * sizeof(uint32_t));
    for (uint32_t i = 0u; i < tmpl->reloc_count; ++i) {
        uint32_t off = tmpl->relocs[i].dword_offset;
        staging[off] = (uint32_t)resolved[i];
        staging[off + 1u] = (uint32_t)(resolved[i] >> 32);
    }

    A840SdsPreencodeStatus vst = a840_pack_sds_verify(
        staging, tmpl->word_count, A840_SDS_MODE_RELOCATABLE_TEMPLATE, NULL);
    if (vst != A840_SDS_OK)
        return vst;

    memcpy(words, staging, (size_t)tmpl->word_count * sizeof(uint32_t));
    return A840_SDS_OK;
}

A840SdsPreencodeStatus
a840_pack_sds_export_pm4_file(const char *path,
                              const uint32_t *words,
                              uint32_t word_count)
{
    if (path_forbidden(path) || words == NULL || word_count == 0u ||
        word_count > A840_RECORD_MAX_SDS_DWORDS) {
        return A840_SDS_INVALID_ARG;
    }

    /* Validate full PM4 stream structure (supports both Type4 SDS blocks and
     * Type7 CP_SET_DRAW_STATE binding streams). */
    A840PM4Packet packets[A840_RECORD_MAX_SDS_DWORDS];
    size_t count = 0u;
    if (a840_pm4_decode(words, word_count, packets, A840_RECORD_MAX_SDS_DWORDS, &count) != A840_EMIT_OK ||
        count == 0u) {
        return A840_SDS_VERIFY_ERROR;
    }

    FILE *fp = fopen(path, "wb");
    if (fp == NULL)
        return A840_SDS_IO_ERROR;

    for (uint32_t i = 0u; i < word_count; ++i) {
        uint8_t le[4];
        le[0] = (uint8_t)(words[i] & 0xffu);
        le[1] = (uint8_t)((words[i] >> 8) & 0xffu);
        le[2] = (uint8_t)((words[i] >> 16) & 0xffu);
        le[3] = (uint8_t)((words[i] >> 24) & 0xffu);
        if (fwrite(le, 1u, 4u, fp) != 4u) {
            (void)fclose(fp);
            return A840_SDS_IO_ERROR;
        }
    }

    if (fflush(fp) != 0 || fclose(fp) != 0)
        return A840_SDS_IO_ERROR;
    return A840_SDS_OK;
}

#else
typedef int a840_pack_sds_preencode_disabled_t;
#endif /* A840_SDS_PREENCODE_ENABLED */
