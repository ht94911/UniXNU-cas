/* SPDX-License-Identifier: MIT */
#include "Adreno840EmitABI.h"
#include "Adreno840Regs.h"

#include <string.h>

#define A840_REG_LIMIT (UINT32_C(1) << ADRENO840_PM4_MAX_REG_BITS)
#define A840_OPCODE_LIMIT (UINT32_C(1) << ADRENO840_PM4_MAX_OPCODE_BITS)
#define A840_SDS_DISABLE (UINT32_C(1) << 17)
#define A840_SDS_ENABLE_MASK (UINT32_C(7) << 20)

typedef struct {
    uintptr_t address;
    size_t bytes;
} A840Span;

/* Compare integer addresses, never subtract or order unrelated C pointers. */
static int spans_overlap(A840Span a, A840Span b)
{
    if (!a.bytes || !b.bytes)
        return 0;
    if (a.address <= b.address)
        return b.address - a.address < a.bytes;
    return a.address - b.address < b.bytes;
}

static A840EmitStatus storage_span(const void *storage, size_t count,
    size_t element_size, size_t alignment, A840Span *span)
{
    span->address = (uintptr_t)storage;
    span->bytes = 0;
    if (!count)
        return A840_EMIT_OK;
    if (!storage)
        return A840_EMIT_INVALID_ARG;
    if (count > SIZE_MAX / element_size)
        return A840_EMIT_RANGE_ERROR;
    if (span->address % alignment)
        return A840_EMIT_ALIGNMENT;
    span->bytes = count * element_size;
    if (span->bytes > UINTPTR_MAX - span->address)
        return A840_EMIT_RANGE_ERROR;
    return A840_EMIT_OK;
}

/* Also works for an invalid, overflowing extent, without multiplying count.
 * The decoder must reject an aliased count before zeroing that count. */
static int storage_overlaps_span(const void *storage, size_t count,
    size_t element_size, A840Span span)
{
    uintptr_t address = (uintptr_t)storage;
    if (!storage || !count || !span.bytes)
        return 0;
    if (address <= span.address)
        return count > (span.address - address) / element_size;
    return address - span.address < span.bytes;
}

static A840EmitStatus cs_storage(A840CmdStream *cs, A840Span *words,
    A840Span *relocs, A840Span *metadata)
{
    A840EmitStatus status = storage_span(cs, 1, sizeof(*cs),
        _Alignof(A840CmdStream), metadata);
    if (status != A840_EMIT_OK)
        return status;
    if (cs->used_dwords > cs->capacity_dwords ||
        cs->reloc_count > cs->reloc_capacity)
        return A840_EMIT_INVALID_ARG;
    status = storage_span(cs->words, cs->capacity_dwords, sizeof(*cs->words),
        _Alignof(uint32_t), words);
    if (status != A840_EMIT_OK)
        return status;
    status = storage_span(cs->relocs, cs->reloc_capacity, sizeof(*cs->relocs),
        _Alignof(A840Reloc), relocs);
    if (status != A840_EMIT_OK)
        return status;
    if (spans_overlap(*words, *relocs) || spans_overlap(*metadata, *words) ||
        spans_overlap(*metadata, *relocs))
        return A840_EMIT_ALIAS;
    return A840_EMIT_OK;
}

static A840EmitStatus payload_storage(const uint32_t *payload, uint32_t count,
    A840Span words, A840Span relocs, A840Span metadata)
{
    A840Span source;
    A840EmitStatus status = storage_span(payload, count, sizeof(*payload),
        _Alignof(uint32_t), &source);
    if (status != A840_EMIT_OK)
        return status;
    if (spans_overlap(source, words) || spans_overlap(source, relocs) ||
        spans_overlap(source, metadata))
        return A840_EMIT_ALIAS;
    return A840_EMIT_OK;
}

/* Identical to UniXNU kits/AdrenoPM4/AdrenoPM4.c: total population is odd. */
static uint32_t odd_bit(uint32_t field)
{
    uint32_t parity = 1;
    while (field) {
        parity ^= 1;
        field &= field - 1;
    }
    return parity;
}

A840EmitStatus a840_cs_init(A840CmdStream *cs, uint32_t *words,
    size_t capacity_dwords, A840Reloc *relocs, size_t reloc_capacity)
{
    A840Span metadata, word_storage, reloc_storage;
    A840EmitStatus status = storage_span(cs, 1, sizeof(*cs),
        _Alignof(A840CmdStream), &metadata);
    if (status != A840_EMIT_OK)
        return status;
    status = storage_span(words, capacity_dwords, sizeof(*words),
        _Alignof(uint32_t), &word_storage);
    if (status != A840_EMIT_OK)
        return status;
    status = storage_span(relocs, reloc_capacity, sizeof(*relocs),
        _Alignof(A840Reloc), &reloc_storage);
    if (status != A840_EMIT_OK)
        return status;
    if (spans_overlap(word_storage, reloc_storage) ||
        spans_overlap(metadata, word_storage) ||
        spans_overlap(metadata, reloc_storage))
        return A840_EMIT_ALIAS;

    cs->words = words;
    cs->capacity_dwords = capacity_dwords;
    cs->used_dwords = 0;
    cs->relocs = relocs;
    cs->reloc_capacity = reloc_capacity;
    cs->reloc_count = 0;
    return A840_EMIT_OK;
}

A840EmitStatus a840_pm4_type4_header(uint32_t reg, uint32_t count,
    uint32_t *out_header)
{
    A840Span output;
    A840EmitStatus status = storage_span(out_header, 1, sizeof(*out_header),
        _Alignof(uint32_t), &output);
    if (status != A840_EMIT_OK)
        return status;
    if (reg >= A840_REG_LIMIT || !count ||
        count > ADRENO840_PM4_MAX_TYPE4_COUNT)
        return A840_EMIT_RANGE_ERROR;
    *out_header = UINT32_C(0x40000000) | count | (odd_bit(count) << 7) |
        (reg << 8) | (odd_bit(reg) << 27);
    return A840_EMIT_OK;
}

A840EmitStatus a840_pm4_type7_header(uint32_t opcode, uint32_t count,
    uint32_t *out_header)
{
    A840Span output;
    A840EmitStatus status = storage_span(out_header, 1, sizeof(*out_header),
        _Alignof(uint32_t), &output);
    if (status != A840_EMIT_OK)
        return status;
    if (opcode >= A840_OPCODE_LIMIT ||
        count > ADRENO840_PM4_MAX_TYPE7_COUNT)
        return A840_EMIT_RANGE_ERROR;
    *out_header = UINT32_C(0x70000000) | count | (odd_bit(count) << 15) |
        (opcode << 16) | (odd_bit(opcode) << 23);
    return A840_EMIT_OK;
}

static A840EmitStatus emit_packet(A840CmdStream *cs, uint32_t header,
    const uint32_t *payload, uint32_t count)
{
    A840Span words, relocs, metadata;
    A840EmitStatus status = cs_storage(cs, &words, &relocs, &metadata);
    size_t needed = (size_t)count + 1;
    size_t start;
    if (status != A840_EMIT_OK)
        return status;
    status = payload_storage(payload, count, words, relocs, metadata);
    if (status != A840_EMIT_OK)
        return status;
    if (needed > cs->capacity_dwords - cs->used_dwords)
        return A840_EMIT_CAPACITY;

    /* No fallible operations after this point: the whole call commits. */
    start = cs->used_dwords;
    cs->words[start] = header;
    if (count)
        memcpy(cs->words + start + 1, payload, (size_t)count * sizeof(*payload));
    cs->used_dwords = start + needed;
    return A840_EMIT_OK;
}

A840EmitStatus a840_cs_emit_pkt4(A840CmdStream *cs, uint32_t reg,
    const uint32_t *values, uint32_t count)
{
    uint32_t header;
    A840EmitStatus status = a840_pm4_type4_header(reg, count, &header);
    if (status != A840_EMIT_OK)
        return status;
    if (count > A840_REG_LIMIT - reg)
        return A840_EMIT_RANGE_ERROR;
    return emit_packet(cs, header, values, count);
}

A840EmitStatus a840_cs_emit_pkt7(A840CmdStream *cs, uint32_t opcode,
    const uint32_t *payload, uint32_t count)
{
    uint32_t header;
    A840EmitStatus status = a840_pm4_type7_header(opcode, count, &header);
    if (status != A840_EMIT_OK)
        return status;
    return emit_packet(cs, header, payload, count);
}

static A840EmitStatus relocation_capacity(A840CmdStream *cs,
    size_t address_word)
{
    if (cs->reloc_count >= cs->reloc_capacity)
        return A840_EMIT_CAPACITY;
    if (cs->used_dwords > (size_t)UINT32_MAX - address_word)
        return A840_EMIT_RANGE_ERROR;
    return A840_EMIT_OK;
}

static void record_relocation(A840CmdStream *cs, size_t address_word,
    uint64_t bo_handle, uint64_t gpu_va, uint32_t flags)
{
    A840Reloc *reloc = &cs->relocs[cs->reloc_count];
    reloc->dword_offset = (uint32_t)(cs->used_dwords + address_word);
    reloc->flags = flags;
    reloc->bo_handle = bo_handle;
    reloc->gpu_va = gpu_va;
    ++cs->reloc_count;
}

A840EmitStatus a840_cs_emit_reg64_va(A840CmdStream *cs, uint32_t reg_lo,
    uint64_t bo_handle, uint64_t gpu_va, uint32_t reloc_flags)
{
    A840Span words, relocs, metadata;
    uint32_t header;
    size_t start;
    A840EmitStatus status = cs_storage(cs, &words, &relocs, &metadata);
    if (status != A840_EMIT_OK)
        return status;
    if ((gpu_va >> 48) || reg_lo > A840_REG_LIMIT - 2)
        return A840_EMIT_RANGE_ERROR;
    if (cs->capacity_dwords - cs->used_dwords < 3)
        return A840_EMIT_CAPACITY;
    if (cs->relocs) {
        status = relocation_capacity(cs, 1);
        if (status != A840_EMIT_OK)
            return status;
    }
    status = a840_pm4_type4_header(reg_lo, 2, &header);
    if (status != A840_EMIT_OK)
        return status;

    start = cs->used_dwords;
    cs->words[start] = header;
    cs->words[start + 1] = (uint32_t)gpu_va;
    cs->words[start + 2] = (uint32_t)(gpu_va >> 32);
    if (cs->relocs)
        record_relocation(cs, 1, bo_handle, gpu_va, reloc_flags);
    cs->used_dwords = start + 3;
    return A840_EMIT_OK;
}

A840EmitStatus a840_cs_emit_sds_group(A840CmdStream *cs, uint32_t group_id,
    uint32_t enable_mask, uint64_t state_bo_handle, uint64_t state_gpu_va,
    uint32_t state_dwords)
{
    A840Span words, relocs, metadata;
    uint32_t header, word0;
    size_t start;
    A840EmitStatus status = cs_storage(cs, &words, &relocs, &metadata);
    if (status != A840_EMIT_OK)
        return status;
    /* Mesa adreno_pm4.xml: COUNT[15:0], DISABLE[17], render enables[22:20],
     * GROUP_ID[29:24]. enable_mask is already shifted, as in tu_cmd_buffer.cc.
     * Reject other bits rather than letting them overwrite count/group fields. */
    if (group_id >= ADRENO840_MAX_SDS_SLOTS || state_dwords > UINT32_C(0xffff) ||
        (state_gpu_va >> 48) || (enable_mask & ~A840_SDS_ENABLE_MASK))
        return A840_EMIT_RANGE_ERROR;
    if (state_gpu_va % 4)
        return A840_EMIT_ALIGNMENT;
    if (state_dwords && !state_gpu_va)
        return A840_EMIT_INVALID_ARG;
    if (cs->capacity_dwords - cs->used_dwords < 4)
        return A840_EMIT_CAPACITY;
    if (cs->relocs && state_dwords) {
        status = relocation_capacity(cs, 2);
        if (status != A840_EMIT_OK)
            return status;
    }
    status = a840_pm4_type7_header(A840_CP_SET_DRAW_STATE, 3, &header);
    if (status != A840_EMIT_OK)
        return status;
    word0 = state_dwords | enable_mask | (group_id << 24);
    if (!state_dwords)
        word0 |= A840_SDS_DISABLE;

    start = cs->used_dwords;
    cs->words[start] = header;
    cs->words[start + 1] = word0;
    cs->words[start + 2] = (uint32_t)state_gpu_va;
    cs->words[start + 3] = (uint32_t)(state_gpu_va >> 32);
    if (cs->relocs && state_dwords)
        record_relocation(cs, 2, state_bo_handle, state_gpu_va,
            A840_RELOC_SDS | A840_RELOC_READ);
    cs->used_dwords = start + 4;
    return A840_EMIT_OK;
}

static A840EmitStatus decode_header(uint32_t header, A840PM4Packet *packet)
{
    uint32_t rebuilt;
    A840EmitStatus status;
    packet->type = header >> 28;
    if (packet->type == 4) {
        packet->identifier = (header >> 8) & UINT32_C(0x3ffff);
        packet->payload_words = header & UINT32_C(0x7f);
        status = a840_pm4_type4_header(packet->identifier,
            packet->payload_words, &rebuilt);
        if (packet->payload_words > A840_REG_LIMIT - packet->identifier)
            return A840_EMIT_INVALID_ARG;
    } else if (packet->type == 7) {
        packet->identifier = (header >> 16) & UINT32_C(0x7f);
        packet->payload_words = header & UINT32_C(0x3fff);
        status = a840_pm4_type7_header(packet->identifier,
            packet->payload_words, &rebuilt);
    } else {
        return A840_EMIT_UNSUPPORTED;
    }
    /* Rebuilding checks both odd parity and every reserved header bit. */
    if (status != A840_EMIT_OK || rebuilt != header)
        return A840_EMIT_INVALID_ARG;
    return A840_EMIT_OK;
}

A840EmitStatus a840_pm4_decode(const uint32_t *words, size_t word_count,
    A840PM4Packet *out_packets, size_t packet_capacity, size_t *out_count)
{
    A840Span input, output, count_storage;
    A840PM4Packet packet;
    size_t at = 0, packets = 0;
    A840EmitStatus status = storage_span(out_count, 1, sizeof(*out_count),
        _Alignof(size_t), &count_storage);
    if (status != A840_EMIT_OK)
        return status;
    /* Input, output and count must be disjoint, following AdrenoPM4.h.
     * An invalid count alias is rejected without a write: zeroing it would
     * corrupt immutable input or violate the untouched-packet guarantee. */
    if (storage_overlaps_span(words, word_count, sizeof(*words), count_storage) ||
        storage_overlaps_span(out_packets, packet_capacity, sizeof(*out_packets),
            count_storage))
        return A840_EMIT_ALIAS;
    *out_count = 0;
    status = storage_span(words, word_count, sizeof(*words),
        _Alignof(uint32_t), &input);
    if (status != A840_EMIT_OK)
        return status;
    status = storage_span(out_packets, packet_capacity, sizeof(*out_packets),
        _Alignof(A840PM4Packet), &output);
    if (status != A840_EMIT_OK)
        return status;
    if (spans_overlap(input, output))
        return A840_EMIT_ALIAS;
    while (at < word_count) {
        status = decode_header(words[at], &packet);
        if (status != A840_EMIT_OK)
            return status;
        if (packet.payload_words > word_count - at - 1)
            return A840_EMIT_RANGE_ERROR;
        at += (size_t)packet.payload_words + 1;
        ++packets;
    }
    if (packets > packet_capacity)
        return A840_EMIT_CAPACITY;

    /* Second pass commits only after validating the complete immutable input. */
    at = packets = 0;
    while (at < word_count) {
        (void)decode_header(words[at], &packet);
        packet.header_word = at;
        out_packets[packets++] = packet;
        at += (size_t)packet.payload_words + 1;
    }
    *out_count = packets;
    return A840_EMIT_OK;
}
