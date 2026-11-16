/* SPDX-License-Identifier: BSD-2-Clause */
/* Observe original DMS BindSubscription/GetDeviceSerialNumbers replies.
 * Only a successfully bound, endpoint-owned client supplies an activation
 * IMEI. No query, setting, NV write or radio command is generated here.
 * Sources: factory DMS v1.97 IDL, libqmi Get IDs, native24A446 activation
 * TLV12/55 and ActivationCommandDriver::parseImei. See SOURCE.md. */
#ifndef NB_DEVICE_IDENTITY_H
#define NB_DEVICE_IDENTITY_H
#include "coachz_bb_qmux.h"
#include "coachz_bb_data.h"

#define NB_DEVICE_DMS 2u
#define NB_DEVICE_BIND 0x54u
#define NB_DEVICE_GET_IDS 0x25u
#define NB_DEVICE_IMEI_BYTES 8u
#define NB_DEVICE_READS 16u
struct nb_device_client {
    uint8_t used, channel, cid, bound, subscription, binding, next_subscription;
    uint16_t bind_transaction;
    uint32_t node, port, generation;
};
struct nb_device_read {
    uint8_t used, subscription;
    uint16_t transaction;
    uint32_t client, generation, node, port;
};
struct nb_device_imei {
    uint8_t known, value[NB_DEVICE_IMEI_BYTES];
    uint32_t node, port;
};
struct nb_device_identity {
    struct nb_device_client client[CBQ_MAX_CLIENTS];
    struct nb_device_read read[NB_DEVICE_READS];
    struct nb_device_imei imei[2];
};
static inline void nb_device_reset(struct nb_device_identity *s)
{ __builtin_memset(s, 0, sizeof *s); }

static inline int nb_device_frame(const uint8_t *s, uint32_t n)
{
    if (!s || n < 7 || n != 7u + cbd_rd16(s + 5)) return 0;
    uint8_t seen[32] = {0};
    for (uint32_t at = 7; at < n;) {
        if (n - at < 3) return 0;
        unsigned tag = s[at], bytes = cbd_rd16(s + at + 1);
        if (bytes > n - at - 3 || (seen[tag >> 3] & (1u << (tag & 7)))) return 0;
        seen[tag >> 3] |= (uint8_t)(1u << (tag & 7));
        at += 3 + bytes;
    }
    return 1;
}
static inline int nb_device_success(const uint8_t *s, uint32_t n)
{
    uint32_t bytes;
    const uint8_t *v = cbd_find(s, n, 2, &bytes);
    return v && bytes == 4 && !cbd_rd16(v) && !cbd_rd16(v + 2);
}
static inline void nb_device_retire(struct nb_device_identity *s, unsigned slot)
{
    if (slot >= CBQ_MAX_CLIENTS) return;
    s->client[slot].used = 0;
    for (unsigned i = 0; i < NB_DEVICE_READS; ++i)
        if (s->read[i].used && s->read[i].client == slot) s->read[i].used = 0;
}
static inline void nb_device_sync(struct nb_device_identity *s, const struct cbq_bridge *router)
{
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i) {
        const struct nb_device_client *c = &s->client[i];
        const struct cbq_client *r = &router->client[i];
        if (c->used && (!r->used || r->service != NB_DEVICE_DMS
            || c->channel != r->channel || c->cid != r->cid
            || c->node != r->node || c->port != r->port)) nb_device_retire(s, i);
    }
}
static inline struct nb_device_client *nb_device_attach(struct nb_device_identity *s,
    unsigned slot, unsigned channel, unsigned cid, uint32_t node, uint32_t port)
{
    if (slot >= CBQ_MAX_CLIENTS || channel >= CBQ_CHANNELS || !cid) return 0;
    struct nb_device_client *c = &s->client[slot];
    if (!c->used || c->channel != channel || c->cid != cid
        || c->node != node || c->port != port) {
        uint32_t generation = c->generation + 1u;
        nb_device_retire(s, slot);
        __builtin_memset(c, 0, sizeof *c);
        c->used = 1; c->channel = (uint8_t)channel; c->cid = (uint8_t)cid;
        c->node = node; c->port = port; c->generation = generation;
    }
    return c;
}
static inline void nb_device_request(struct nb_device_identity *s, unsigned slot,
    unsigned channel, unsigned cid, uint32_t node, uint32_t port,
    const uint8_t *data, uint32_t bytes)
{
    if (!nb_device_frame(data, bytes) || data[0] != CBQ_QMI_REQUEST) return;
    unsigned message = cbd_rd16(data + 3);
    if (message != NB_DEVICE_BIND && message != NB_DEVICE_GET_IDS) return;
    struct nb_device_client *c = nb_device_attach(s, slot, channel, cid, node, port);
    if (!c) return;
    if (message == NB_DEVICE_BIND) {
        uint32_t n = 0;
        const uint8_t *v = cbd_find(data, bytes, 1, &n);
        if (!v || n != 4 || bytes != 14) return;
        unsigned subscription = cbd_rd32(v);
        c->binding = 1;
        c->next_subscription = subscription >= 1 && subscription <= 2
            ? (uint8_t)(subscription - 1) : 0xff;
        c->bind_transaction = (uint16_t)cbd_rd16(data + 1);
        /* Reads issued across a binding change cannot establish which
         * subscription the real modem used. Leave accepted identities intact. */
        for (unsigned i = 0; i < NB_DEVICE_READS; ++i)
            if (s->read[i].used && s->read[i].client == slot) s->read[i].used = 0;
        return;
    }
    if (bytes != 7 || !c->bound || c->binding) return;
    for (unsigned i = 0; i < NB_DEVICE_READS; ++i) {
        if (s->read[i].used) continue;
        s->read[i] = (struct nb_device_read){1, c->subscription,
            (uint16_t)cbd_rd16(data + 1), slot, c->generation, node, port};
        break;
    }
}
static inline int nb_device_pack_imei(const uint8_t *text, uint32_t bytes,
                                    uint8_t value[NB_DEVICE_IMEI_BYTES])
{
    if (!text || bytes != 15) return 0;
    unsigned nonzero = 0;
    for (unsigned i = 0; i < bytes; ++i) {
        if (text[i] < '0' || text[i] > '9') return 0;
        nonzero |= text[i] != '0';
    }
    if (!nonzero) return 0;
    /* Native parseImei shifts the low nibble of the following byte into
     * each digit pair. This is the original odd-length IMEI mobile identity,
     * not a little-endian integer or the ICCID's reading-order encoding. */
    value[0] = (uint8_t)((text[0] - '0') << 4 | 0x0a);
    for (unsigned i = 1; i < NB_DEVICE_IMEI_BYTES; ++i)
        value[i] = (uint8_t)((text[2u * i] - '0') << 4 | (text[2u * i - 1u] - '0'));
    return 1;
}
/* The caller has already accepted this frame through endpoint-bound QMUX. */
static inline int nb_device_answer(struct nb_device_identity *s, unsigned slot,
    unsigned channel, unsigned cid, uint32_t node, uint32_t port,
    const uint8_t *data, uint32_t bytes)
{
    if (slot >= CBQ_MAX_CLIENTS || !nb_device_frame(data, bytes)
        || data[0] != CBQ_QMI_RESPONSE) return 0;
    struct nb_device_client *c = &s->client[slot];
    if (!c->used || c->channel != channel || c->cid != cid
        || c->node != node || c->port != port) return 0;
    unsigned message = cbd_rd16(data + 3);
    if (message == NB_DEVICE_BIND) {
        if (!c->binding || c->bind_transaction != cbd_rd16(data + 1)) return 0;
        c->binding = 0;
        if (nb_device_success(data, bytes)) {
            c->bound = c->next_subscription < 2;
            c->subscription = c->next_subscription;
        }
        return 0;
    }
    if (message != NB_DEVICE_GET_IDS || !c->bound || c->binding) return 0;
    for (unsigned i = 0; i < NB_DEVICE_READS; ++i) {
        struct nb_device_read *r = &s->read[i];
        if (!r->used || r->client != slot || r->transaction != cbd_rd16(data + 1)) continue;
        r->used = 0;
        if (r->generation != c->generation || r->subscription != c->subscription
            || r->node != node || r->port != port || !nb_device_success(data, bytes)) return 0;
        uint32_t n = 0;
        const uint8_t *imei = cbd_find(data, bytes, 0x11, &n);
        uint8_t value[NB_DEVICE_IMEI_BYTES];
        if (!nb_device_pack_imei(imei, n, value)) return 0;
        struct nb_device_imei *out = &s->imei[r->subscription];
        int changed = !out->known || out->node != node || out->port != port
            || __builtin_memcmp(out->value, value, sizeof value);
        out->known = 1; out->node = node; out->port = port;
        __builtin_memcpy(out->value, value, sizeof value);
        return changed;
    }
    return 0;
}
static inline int nb_device_endpoint_lost(struct nb_device_identity *s, uint32_t node, uint32_t port)
{
    int changed = 0;
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i)
        if (s->client[i].used && s->client[i].node == node && s->client[i].port == port)
            nb_device_retire(s, i);
    for (unsigned i = 0; i < 2; ++i) {
        struct nb_device_imei *v = &s->imei[i];
        if (v->known && v->node == node && v->port == port) {
            __builtin_memset(v, 0, sizeof *v); changed = 1;
        }
    }
    return changed;
}
static inline uint32_t nb_device_activation(const struct nb_device_identity *s,
                                           uint8_t *tlvs, uint32_t room)
{
    uint32_t need = (s->imei[0].known + s->imei[1].known) * (3u + NB_DEVICE_IMEI_BYTES);
    if (room < need) return 0;
    uint32_t n = 0;
    for (unsigned i = 0; i < 2; ++i)
        if (s->imei[i].known)
            n += cbd_tlv(tlvs + n, room - n, i ? 0x55 : 0x12,
                         s->imei[i].value, NB_DEVICE_IMEI_BYTES);
    return n;
}
#endif
