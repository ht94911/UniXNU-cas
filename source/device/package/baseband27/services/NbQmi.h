/* SPDX-License-Identifier: BSD-2-Clause */
/* QMI message framing as carried in QRTR DATA packets: a 7-byte header (type, u16 transaction, u16 message id,
 * u16 TLV length) and TLVs of {u8 type, u16 length, value}. Parsing is bounded; writers never exceed capacity.
 * Header-only so the board services and host tests share one copy. Reference: sheng QcomQrtr.c qcom_qmi_parse. */
#ifndef NB_QMI_H
#define NB_QMI_H
#include <stdint.h>

#define NB_QMI_HEADER 7u
enum { NB_QMI_REQUEST = 0, NB_QMI_RESPONSE = 2, NB_QMI_INDICATION = 4 };

typedef struct { uint8_t type; uint16_t transaction, message; const uint8_t *tlvs; uint32_t length; } NbQmiMessage;

static inline uint16_t nb_le16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static inline uint32_t nb_le32(const uint8_t *p) { return (uint32_t)nb_le16(p) | (uint32_t)nb_le16(p + 2) << 16; }
static inline uint64_t nb_le64(const uint8_t *p) { return (uint64_t)nb_le32(p) | (uint64_t)nb_le32(p + 4) << 32; }
static inline void nb_put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void nb_put32(uint8_t *p, uint32_t v) { nb_put16(p, v); nb_put16(p + 2, v >> 16); }
static inline void nb_put64(uint8_t *p, uint64_t v) { nb_put32(p, (uint32_t)v); nb_put32(p + 4, (uint32_t)(v >> 32)); }

static inline int nb_qmi_parse(const uint8_t *data, uint32_t length, NbQmiMessage *m)
{
    if (!data || !m || length < NB_QMI_HEADER) return -1;
    m->type = data[0];
    m->transaction = nb_le16(data + 1);
    m->message = nb_le16(data + 3);
    m->length = nb_le16(data + 5);
    if (m->length > length - NB_QMI_HEADER) return -1;
    m->tlvs = data + NB_QMI_HEADER;
    return 0;
}

/* Value of TLV type (first occurrence) and its size, or null. A malformed TLV chain ends the search. */
static inline const uint8_t *nb_qmi_tlv(const NbQmiMessage *m, uint8_t type, uint32_t *size)
{
    for (uint32_t at = 0; at + 3u <= m->length;) {
        const uint32_t n = nb_le16(m->tlvs + at + 1);
        if (n > m->length - at - 3u) return 0;
        if (m->tlvs[at] == type) { if (size) *size = n; return m->tlvs + at + 3; }
        at += 3u + n;
    }
    return 0;
}

typedef struct { uint8_t *out; uint32_t capacity, at; int overflow; } NbQmiWriter;

static inline int nb_qmi_begin(NbQmiWriter *w, uint8_t *out, uint32_t capacity, uint8_t type, uint16_t txn, uint16_t message)
{
    if (!w || !out || capacity < NB_QMI_HEADER) return -1;
    w->out = out; w->capacity = capacity; w->at = NB_QMI_HEADER; w->overflow = 0;
    out[0] = type; nb_put16(out + 1, txn); nb_put16(out + 3, message); nb_put16(out + 5, 0);
    return 0;
}
static inline uint8_t *nb_qmi_put(NbQmiWriter *w, uint8_t type, const void *value, uint32_t length)
{
    if (w->overflow || length > 0xffffu || w->capacity - w->at < 3u + length) { w->overflow = 1; return 0; }
    uint8_t *p = w->out + w->at;
    p[0] = type; nb_put16(p + 1, length);
    const uint8_t *v = (const uint8_t *)value;
    for (uint32_t i = 0; i < length; ++i) p[3 + i] = v ? v[i] : 0;
    w->at += 3u + length;
    return p + 3;
}
static inline void nb_qmi_u8(NbQmiWriter *w, uint8_t type, uint8_t v) { nb_qmi_put(w, type, &v, 1); }
static inline void nb_qmi_u32(NbQmiWriter *w, uint8_t type, uint32_t v) { uint8_t b[4]; nb_put32(b, v); nb_qmi_put(w, type, b, 4); }
static inline void nb_qmi_u64(NbQmiWriter *w, uint8_t type, uint64_t v) { uint8_t b[8]; nb_put64(b, v); nb_qmi_put(w, type, b, 8); }
/* TLV 0x02: QMI result (0 success, 1 failure) and error code. */
static inline void nb_qmi_result(NbQmiWriter *w, uint16_t result, uint16_t error)
{
    uint8_t b[4]; nb_put16(b, result); nb_put16(b + 2, error); nb_qmi_put(w, 0x02, b, 4);
}
/* Finishes the header; 0 when anything overflowed. */
static inline uint32_t nb_qmi_end(NbQmiWriter *w)
{
    if (w->overflow || w->at - NB_QMI_HEADER > 0xffffu) return 0;
    nb_put16(w->out + 5, w->at - NB_QMI_HEADER);
    return w->at;
}

#define NB_QMI_ERR_NONE 0x00u
#define NB_QMI_ERR_MALFORMED_MSG 0x01u
#define NB_QMI_ERR_NO_MEMORY 0x02u
#define NB_QMI_ERR_INTERNAL 0x03u
#define NB_QMI_ERR_INVALID_ARG 0x30u
#define NB_QMI_ERR_NOT_SUPPORTED 0x5eu

#endif
