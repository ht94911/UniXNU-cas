/* SPDX-License-Identifier: MIT */
#include "a840_kmd_ib_diag.h"
#include "Adreno840Regs.h"
#include "a840_context_init.h"

#include <string.h>

#define A840_IB_PKT_TYPE4 UINT32_C(4)
#define A840_IB_PKT_TYPE7 UINT32_C(7)
#define A840_IB_REG_LIMIT (UINT32_C(1) << ADRENO840_PM4_MAX_REG_BITS)
#define A840_IB_REG_MASK (A840_IB_REG_LIMIT - 1u)
#define A840_IB_OP_LIMIT (UINT32_C(1) << ADRENO840_PM4_MAX_OPCODE_BITS)
#define A840_IB_OP_MASK (A840_IB_OP_LIMIT - 1u)
#define A840_IB_TYPE7_RESERVED_HI_MASK UINT32_C(15)
#define A840_IB_HEX_RADIX 16u
#define A840_IB_DEC_RADIX 10u

static int ranges_overlap(
    const void *a_ptr, size_t a_bytes,
    const void *b_ptr, size_t b_bytes)
{
    uintptr_t a = (uintptr_t)a_ptr;
    uintptr_t b = (uintptr_t)b_ptr;
    if (a_bytes == 0u || b_bytes == 0u)
        return 0;
    if (a <= b)
        return (b - a) < a_bytes;
    return (a - b) < b_bytes;
}

static int span_valid(
    const void *ptr,
    size_t count,
    size_t elem_size,
    size_t alignment,
    size_t *out_bytes)
{
    uintptr_t addr = (uintptr_t)ptr;
    size_t total;
    if (out_bytes != NULL)
        *out_bytes = 0u;
    if (count == 0u)
        return 1;
    if (ptr == NULL)
        return 0;
    if (count > SIZE_MAX / elem_size)
        return 0;
    if ((addr % alignment) != 0u)
        return 0;
    total = count * elem_size;
    if (total > UINTPTR_MAX - addr)
        return 0;
    if (out_bytes != NULL)
        *out_bytes = total;
    return 1;
}

A840KmdIbDiagStatus a840_kmd_validate_ib_words(
    const uint32_t *words,
    size_t dword_count,
    A840KmdIbDiagResult *out)
{
    size_t words_bytes = 0u;
    size_t out_bytes = 0u;
    size_t at = 0u;
    size_t pkt_idx = 0u;

    if (out == NULL)
        return A840_KMD_IB_INVALID_ARG;
    if (!span_valid(out, 1u, sizeof(*out), _Alignof(A840KmdIbDiagResult), &out_bytes))
        return A840_KMD_IB_INVALID_ARG;
    if (words == NULL) {
        memset(out, 0, sizeof(*out));
        out->status = A840_KMD_IB_INVALID_ARG;
        return A840_KMD_IB_INVALID_ARG;
    }
    if (!span_valid(words, dword_count, sizeof(*words), _Alignof(uint32_t), &words_bytes)) {
        memset(out, 0, sizeof(*out));
        out->status = A840_KMD_IB_INVALID_ARG;
        return A840_KMD_IB_INVALID_ARG;
    }
    if (ranges_overlap(words, words_bytes, out, out_bytes))
        return A840_KMD_IB_INVALID_ARG;

    memset(out, 0, sizeof(*out));

    if (dword_count == 0u) {
        out->status = A840_KMD_IB_EMPTY;
        return A840_KMD_IB_EMPTY;
    }

    while (at < dword_count) {
        const uint32_t header = words[at];
        const uint32_t pkt_type = header >> 28u;

        if (pkt_type == A840_IB_PKT_TYPE4) {
            const uint32_t count = header & ADRENO840_PM4_MAX_TYPE4_COUNT;
            const uint32_t base_reg = (header >> 8u) & A840_IB_REG_MASK;
            const uint32_t reserved_bit26 = (header >> 26u) & 1u;
            uint32_t expected_header = 0u;
            A840EmitStatus hdr_st;
            uint32_t offset;

            if (count == 0u || reserved_bit26 != 0u ||
                count > A840_IB_REG_LIMIT - base_reg) {
                out->status = A840_KMD_IB_BAD_HEADER;
                out->offending_reg = base_reg;
                out->offending_word = header;
                out->offending_dword_index = at;
                out->offending_header_index = at;
                out->offending_packet_index = pkt_idx;
                out->validated_dwords = at;
                return A840_KMD_IB_BAD_HEADER;
            }

            hdr_st = a840_pm4_type4_header(base_reg, count, &expected_header);
            if (hdr_st != A840_EMIT_OK || expected_header != header) {
                out->status = A840_KMD_IB_BAD_PARITY;
                out->offending_reg = base_reg;
                out->offending_word = header;
                out->offending_dword_index = at;
                out->offending_header_index = at;
                out->offending_packet_index = pkt_idx;
                out->validated_dwords = at;
                return A840_KMD_IB_BAD_PARITY;
            }

            if ((size_t)count > dword_count - at - 1u) {
                out->status = A840_KMD_IB_TRUNCATED;
                out->offending_reg = base_reg;
                out->offending_word = header;
                out->offending_dword_index = at;
                out->offending_header_index = at;
                out->offending_packet_index = pkt_idx;
                out->validated_dwords = at;
                return A840_KMD_IB_TRUNCATED;
            }

            /* Hardware security guardrail: reject all 6 Gen8 UMD-blocked
             * KMD-only non-context registers before the IB can reach the CP.
             */
            for (offset = 0u; offset < count; ++offset) {
                const uint32_t target_reg = base_reg + offset;
                if (a840_reg_is_umd_blocked_on_a8xx(target_reg) != 0) {
                    const size_t value_index = at + 1u + (size_t)offset;
                    out->status = A840_KMD_IB_BLOCKED_REG;
                    out->offending_reg = target_reg;
                    out->offending_word = words[value_index];
                    out->offending_dword_index = value_index;
                    out->offending_header_index = at;
                    out->offending_packet_index = pkt_idx;
                    out->validated_dwords = value_index;
                    return A840_KMD_IB_BLOCKED_REG;
                }
            }

            out->type4_packets++;
            out->total_packets++;
            out->total_reg_writes += (size_t)count;
            at += (size_t)count + 1u;
            pkt_idx++;
        } else if (pkt_type == A840_IB_PKT_TYPE7) {
            const uint32_t count = header & ADRENO840_PM4_MAX_TYPE7_COUNT;
            const uint32_t reserved_bit14 = (header >> 14u) & 1u;
            const uint32_t op = (header >> 16u) & A840_IB_OP_MASK;
            const uint32_t reserved_hi = (header >> 24u) & A840_IB_TYPE7_RESERVED_HI_MASK;
            uint32_t expected_header = 0u;
            A840EmitStatus hdr_st;

            if (reserved_bit14 != 0u || reserved_hi != 0u) {
                out->status = A840_KMD_IB_BAD_HEADER;
                out->offending_opcode = op;
                out->offending_word = header;
                out->offending_dword_index = at;
                out->offending_header_index = at;
                out->offending_packet_index = pkt_idx;
                out->validated_dwords = at;
                return A840_KMD_IB_BAD_HEADER;
            }

            hdr_st = a840_pm4_type7_header(op, count, &expected_header);
            if (hdr_st != A840_EMIT_OK || expected_header != header) {
                out->status = A840_KMD_IB_BAD_PARITY;
                out->offending_opcode = op;
                out->offending_word = header;
                out->offending_dword_index = at;
                out->offending_header_index = at;
                out->offending_packet_index = pkt_idx;
                out->validated_dwords = at;
                return A840_KMD_IB_BAD_PARITY;
            }

            if ((size_t)count > dword_count - at - 1u) {
                out->status = A840_KMD_IB_TRUNCATED;
                out->offending_opcode = op;
                out->offending_word = header;
                out->offending_dword_index = at;
                out->offending_header_index = at;
                out->offending_packet_index = pkt_idx;
                out->validated_dwords = at;
                return A840_KMD_IB_TRUNCATED;
            }

            out->type7_packets++;
            out->total_packets++;
            at += (size_t)count + 1u;
            pkt_idx++;
        } else {
            out->status = A840_KMD_IB_UNKNOWN_PACKET;
            out->offending_word = header;
            out->offending_dword_index = at;
            out->offending_header_index = at;
            out->offending_packet_index = pkt_idx;
            out->validated_dwords = at;
            return A840_KMD_IB_UNKNOWN_PACKET;
        }
    }

    out->status = A840_KMD_IB_OK;
    out->validated_dwords = dword_count;
    return A840_KMD_IB_OK;
}

A840KmdIbDiagStatus a840_kmd_validate_ib_bytes(
    const void *bytes,
    size_t byte_count,
    A840KmdIbDiagResult *out)
{
    if (out == NULL)
        return A840_KMD_IB_INVALID_ARG;
    if (bytes == NULL) {
        memset(out, 0, sizeof(*out));
        out->status = A840_KMD_IB_INVALID_ARG;
        return A840_KMD_IB_INVALID_ARG;
    }
    if ((byte_count % sizeof(uint32_t)) != 0u ||
        (((uintptr_t)bytes) % _Alignof(uint32_t)) != 0u) {
        if (!ranges_overlap(bytes, byte_count, out, sizeof(*out))) {
            memset(out, 0, sizeof(*out));
            out->status = A840_KMD_IB_INVALID_ARG;
        }
        return A840_KMD_IB_INVALID_ARG;
    }
    return a840_kmd_validate_ib_words(
        (const uint32_t *)bytes,
        byte_count / sizeof(uint32_t),
        out);
}

/* Allocation-free bounded ASCII writer (kernel-safe, no stdio dependency). */
typedef struct A840AsciiWriter {
    char *buf;
    size_t cap;
    size_t pos;
} A840AsciiWriter;

static void ascii_init(A840AsciiWriter *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->pos = 0u;
    if (buf != NULL && cap > 0u)
        buf[0] = '\0';
}

static void ascii_putc(A840AsciiWriter *w, char ch)
{
    if (w->buf != NULL && w->cap > 0u && w->pos + 1u < w->cap) {
        w->buf[w->pos++] = ch;
        w->buf[w->pos] = '\0';
    }
}

static void ascii_puts(A840AsciiWriter *w, const char *s)
{
    if (s == NULL)
        return;
    while (*s != '\0') {
        ascii_putc(w, *s);
        s++;
    }
}

static void ascii_put_u64_dec(A840AsciiWriter *w, uint64_t val)
{
    char rev[24];
    size_t len = 0u;
    if (val == 0u) {
        ascii_putc(w, '0');
        return;
    }
    while (val > 0u && len < sizeof(rev)) {
        rev[len++] = (char)('0' + (char)(val % A840_IB_DEC_RADIX));
        val /= A840_IB_DEC_RADIX;
    }
    while (len > 0u) {
        len--;
        ascii_putc(w, rev[len]);
    }
}

static void ascii_put_hex_fixed(A840AsciiWriter *w, uint64_t val, unsigned width)
{
    static const char k_hex[16] = "0123456789abcdef";
    unsigned i;
    ascii_putc(w, '0');
    ascii_putc(w, 'x');
    for (i = 0u; i < width; ++i) {
        const unsigned shift = (width - 1u - i) * 4u;
        const unsigned nibble = (unsigned)((val >> shift) & (A840_IB_HEX_RADIX - 1u));
        ascii_putc(w, k_hex[nibble]);
    }
}

const char *a840_kmd_ib_diag_status_name(A840KmdIbDiagStatus status)
{
    switch (status) {
    case A840_KMD_IB_OK: return "ok";
    case A840_KMD_IB_INVALID_ARG: return "invalid_arg";
    case A840_KMD_IB_EMPTY: return "empty";
    case A840_KMD_IB_UNKNOWN_PACKET: return "unknown_packet";
    case A840_KMD_IB_BAD_PARITY: return "bad_parity";
    case A840_KMD_IB_BAD_HEADER: return "bad_header";
    case A840_KMD_IB_TRUNCATED: return "truncated";
    case A840_KMD_IB_BLOCKED_REG: return "blocked_reg";
    default: return "unknown";
    }
}

const char *a840_kmd_fault_kind_name(uint32_t fault_kind)
{
    switch (fault_kind) {
    case A840_KMD_FAULT_NONE: return "none";
    case A840_KMD_FAULT_GPU_HANG: return "gpu_hang";
    case A840_KMD_FAULT_SMMU_TRANSLATION: return "smmu_translation";
    case A840_KMD_FAULT_SMMU_PERMISSION: return "smmu_permission";
    case A840_KMD_FAULT_IB_ERROR: return "ib_error";
    default: return "unknown";
    }
}

size_t a840_kmd_format_fault_snapshot(
    const A840KmdFaultRecord *rec,
    char *buf,
    size_t cap)
{
    A840KmdFaultRecord snap;
    A840AsciiWriter w;

    if (buf == NULL || cap == 0u)
        return 0u;
    if (rec == NULL) {
        buf[0] = '\0';
        return 0u;
    }

    /* Copy to stack before touching buf so aliased buffers never corrupt input. */
    snap = *rec;
    ascii_init(&w, buf, cap);

    ascii_puts(&w, "a840_fault seq=");
    ascii_put_u64_dec(&w, snap.sequence);
    ascii_puts(&w, " kind=");
    ascii_puts(&w, a840_kmd_fault_kind_name(snap.fault_kind));
    ascii_puts(&w, " client=");
    ascii_put_u64_dec(&w, snap.client_id);
    ascii_puts(&w, " queue=");
    ascii_put_u64_dec(&w, snap.queue_id);
    ascii_puts(&w, " fence=");
    ascii_put_u64_dec(&w, snap.fence);
    ascii_puts(&w, " ib1_pc=");
    ascii_put_hex_fixed(&w, snap.ib1_pc, 16u);
    ascii_puts(&w, " ib2_pc=");
    ascii_put_hex_fixed(&w, snap.ib2_pc, 16u);
    ascii_puts(&w, " rbbm=");
    ascii_put_hex_fixed(&w, (uint64_t)snap.rbbm_status, 8u);
    ascii_puts(&w, " cp=");
    ascii_put_hex_fixed(&w, (uint64_t)snap.cp_status, 8u);
    ascii_puts(&w, " smmu_va=");
    ascii_put_hex_fixed(&w, snap.smmu_fault_va, 16u);
    ascii_puts(&w, " smmu_fsr=");
    ascii_put_hex_fixed(&w, (uint64_t)snap.smmu_fsr, 8u);
    ascii_puts(&w, " ticks=");
    ascii_put_u64_dec(&w, snap.timestamp_ticks);

    return w.pos;
}

size_t a840_kmd_format_ib_diag(
    const A840KmdIbDiagResult *diag,
    char *buf,
    size_t cap)
{
    A840KmdIbDiagResult copy;
    A840AsciiWriter w;

    if (buf == NULL || cap == 0u)
        return 0u;
    if (diag == NULL) {
        buf[0] = '\0';
        return 0u;
    }

    copy = *diag;
    ascii_init(&w, buf, cap);

    ascii_puts(&w, "a840_ib status=");
    ascii_puts(&w, a840_kmd_ib_diag_status_name(copy.status));
    ascii_puts(&w, " dword=");
    ascii_put_u64_dec(&w, (uint64_t)copy.offending_dword_index);
    ascii_puts(&w, " hdr=");
    ascii_put_u64_dec(&w, (uint64_t)copy.offending_header_index);
    ascii_puts(&w, " pkt=");
    ascii_put_u64_dec(&w, (uint64_t)copy.offending_packet_index);
    ascii_puts(&w, " reg=");
    ascii_put_hex_fixed(&w, (uint64_t)copy.offending_reg, 5u);
    ascii_puts(&w, " op=");
    ascii_put_hex_fixed(&w, (uint64_t)copy.offending_opcode, 2u);
    ascii_puts(&w, " word=");
    ascii_put_hex_fixed(&w, (uint64_t)copy.offending_word, 8u);
    ascii_puts(&w, " pkts=");
    ascii_put_u64_dec(&w, (uint64_t)copy.total_packets);
    ascii_puts(&w, " t4=");
    ascii_put_u64_dec(&w, (uint64_t)copy.type4_packets);
    ascii_puts(&w, " t7=");
    ascii_put_u64_dec(&w, (uint64_t)copy.type7_packets);
    ascii_puts(&w, " dwords=");
    ascii_put_u64_dec(&w, (uint64_t)copy.validated_dwords);

    return w.pos;
}
