/* SPDX-License-Identifier: MIT */
/* Frozen Contract: Adreno840EmitABI.h (v1.0)
 * Owner: Lead Architect (Coordinator). Workers MUST NOT edit this file directly.
 *
 * Pure C, allocation-free PM4 command-stream builder and A840 packet emitter
 * contract. Safe for both host unit tests (ASan/UBSan) and iOS 27 arm64e runtime.
 */
#ifndef ADRENO840_EMIT_ABI_H
#define ADRENO840_EMIT_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ADRENO840_EMIT_ABI_VERSION = 1u,
    ADRENO840_PM4_MAX_REG_BITS = 18u,
    ADRENO840_PM4_MAX_TYPE4_COUNT = 127u,
    ADRENO840_PM4_MAX_OPCODE_BITS = 7u,
    ADRENO840_PM4_MAX_TYPE7_COUNT = 16383u,
    ADRENO840_MAX_SDS_SLOTS = 64u /* Verified from Mesa __info29 max_draw_states=64 */
};

typedef enum {
    A840_EMIT_OK = 0,
    A840_EMIT_INVALID_ARG = 1,
    A840_EMIT_CAPACITY = 2,
    A840_EMIT_RANGE_ERROR = 3,
    A840_EMIT_ALIGNMENT = 4,
    A840_EMIT_ALIAS = 5,
    A840_EMIT_UNSUPPORTED = 6
} A840EmitStatus;

typedef enum {
    A840_RELOC_READ = 1u << 0,
    A840_RELOC_WRITE = 1u << 1,
    A840_RELOC_SHADER = 1u << 2,
    A840_RELOC_SDS = 1u << 3
} A840RelocFlags;

typedef struct A840Reloc {
    uint32_t dword_offset;   /* Offset of low 32-bit address word inside CS */
    uint32_t flags;          /* Bitmask of A840RelocFlags */
    uint64_t bo_handle;      /* Kernel BO handle (0 for host synthetic tests) */
    uint64_t gpu_va;         /* Resolved 48-bit GPU VA */
} A840Reloc;

/* Caller-owned bounded command stream buffer. Zero heap allocation inside emitters.
 * On any error status, `used_dwords` and `reloc_count` MUST remain unchanged
 * (transactional per-call rollback guarantee). */
typedef struct A840CmdStream {
    uint32_t *words;
    size_t capacity_dwords;
    size_t used_dwords;
    A840Reloc *relocs;       /* Optional (may be NULL when reloc_capacity == 0) */
    size_t reloc_capacity;
    size_t reloc_count;
} A840CmdStream;

/* Structural PM4 packet descriptor (mirroring UniXNU kits/AdrenoPM4 discipline). */
typedef struct A840PM4Packet {
    size_t header_word;
    uint32_t type;           /* 4 (register sequence) or 7 (opcode) */
    uint32_t identifier;     /* 18-bit reg index or 7-bit opcode */
    uint32_t payload_words;
} A840PM4Packet;

/* Core CS & PM4 primitives (implemented in Wave 0 Lane 3). */
A840EmitStatus a840_cs_init(
    A840CmdStream *cs,
    uint32_t *words,
    size_t capacity_dwords,
    A840Reloc *relocs,
    size_t reloc_capacity);

A840EmitStatus a840_pm4_type4_header(uint32_t reg, uint32_t count, uint32_t *out_header);
A840EmitStatus a840_pm4_type7_header(uint32_t opcode, uint32_t count, uint32_t *out_header);

A840EmitStatus a840_cs_emit_pkt4(A840CmdStream *cs, uint32_t reg, const uint32_t *values, uint32_t count);
A840EmitStatus a840_cs_emit_pkt7(A840CmdStream *cs, uint32_t opcode, const uint32_t *payload, uint32_t count);
A840EmitStatus a840_cs_emit_reg64_va(
    A840CmdStream *cs,
    uint32_t reg_lo,
    uint64_t bo_handle,
    uint64_t gpu_va,
    uint32_t reloc_flags);

A840EmitStatus a840_cs_emit_sds_group(
    A840CmdStream *cs,
    uint32_t group_id,       /* 0..ADRENO840_MAX_SDS_SLOTS-1 */
    uint32_t enable_mask,    /* BR/BV/GMEM/SYSMEM enable bits */
    uint64_t state_bo_handle,
    uint64_t state_gpu_va,
    uint32_t state_dwords);

/* Structural decoder (no allocation, validates parity/header/bounds). */
A840EmitStatus a840_pm4_decode(
    const uint32_t *words,
    size_t word_count,
    A840PM4Packet *out_packets,
    size_t packet_capacity,
    size_t *out_count);

#if defined(__cplusplus)
static_assert(sizeof(A840Reloc) == 24, "A840Reloc layout locked");
static_assert(sizeof(A840PM4Packet) == 24, "A840PM4Packet layout locked");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(A840Reloc) == 24, "A840Reloc layout locked");
_Static_assert(sizeof(A840PM4Packet) == 24, "A840PM4Packet layout locked");
#endif

#ifdef __cplusplus
}
#endif

#endif /* ADRENO840_EMIT_ABI_H */
