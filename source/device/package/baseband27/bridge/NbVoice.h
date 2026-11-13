/* SPDX-License-Identifier: BSD-2-Clause */
/* Passive VOICE state for Apple's AUDIO query. No call or modem request is
 * generated here. Formats come from the factory QMI IDL and native27 parsers. */
#ifndef NB_VOICE_H
#define NB_VOICE_H
#include "coachz_bb_qmux.h"
#include "coachz_bb_data.h"
#define NB_VOICE_SERVICE 9u
#define NB_AUDIO_SERVICE 0xe1u
#define NB_AUDIO_QUERY 0x2fu
#define NB_AUDIO_INFO 0x30u
#define NB_VOICE_CALLS 7u
struct nb_voice_call {
    uint8_t id, state, codec_known;
    uint32_t codec, encoder_hz;
};
struct nb_voice_client {
    uint8_t used, channel, cid, bound, subscription, binding, next_subscription;
    uint16_t bind_txn;
    uint32_t node, port;
    struct nb_voice_call call[NB_VOICE_CALLS];
};
struct nb_voice { struct nb_voice_client client[CBQ_MAX_CLIENTS]; };
static inline void nb_voice_reset(struct nb_voice *v) { __builtin_memset(v, 0, sizeof *v); }

/* Validate the entire TLV area; duplicate tags are not meaningful for these
 * QMI messages. Unknown optional tags remain available to the original caller. */
static inline int nb_voice_frame(const uint8_t *s, uint32_t n)
{
    if (!s || n < 7 || 7u + cbd_rd16(s + 5) != n) return 0;
    uint8_t tags[32] = {0};
    for (uint32_t at = 7; at < n;) {
        if (n - at < 3) return 0;
        unsigned tag = s[at], bytes = cbd_rd16(s + at + 1);
        if (bytes > n - at - 3 || (tags[tag >> 3] & (1u << (tag & 7)))) return 0;
        tags[tag >> 3] |= (uint8_t)(1u << (tag & 7));
        at += 3u + bytes;
    }
    return 1;
}
static inline int nb_voice_success(const uint8_t *s, uint32_t n)
{
    uint32_t bytes = 0;
    const uint8_t *result = cbd_find(s, n, 2, &bytes);
    return result && bytes == 4 && !cbd_rd16(result) && !cbd_rd16(result + 2);
}
static inline struct nb_voice_client *nb_voice_client(struct nb_voice *v, unsigned slot,
                                                      unsigned channel, unsigned cid, uint32_t node, uint32_t port)
{
    if (slot >= CBQ_MAX_CLIENTS || channel >= CBQ_CHANNELS) return 0;
    struct nb_voice_client *c = &v->client[slot];
    if (!c->used || c->cid != cid || c->channel != channel || c->node != node || c->port != port) {
        __builtin_memset(c, 0, sizeof *c);
        c->used = 1; c->cid = (uint8_t)cid; c->channel = (uint8_t)channel; c->node = node; c->port = port;
    }
    return c;
}
static inline void nb_voice_sync(struct nb_voice *v, const struct cbq_bridge *router)
{
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i) {
        const struct cbq_client *r = &router->client[i];
        struct nb_voice_client *c = &v->client[i];
        if (c->used && (!r->used || r->service != NB_VOICE_SERVICE || r->cid != c->cid ||
            r->channel != c->channel || r->node != c->node || r->port != c->port))
            __builtin_memset(c, 0, sizeof *c);
    }
}
/* BindSubscription is an explicit one-byte enum, confirmed only by its own
 * successful response. QMUX channel alone never decides which SIM is used. */
static inline void nb_voice_request(struct nb_voice *v, unsigned slot, unsigned channel, unsigned cid,
                                   uint32_t node, uint32_t port, const uint8_t *s, uint32_t n)
{
    if (!nb_voice_frame(s, n) || s[0] != 0 || cbd_rd16(s + 3) != 0x44) return;
    uint32_t bytes = 0;
    const uint8_t *sub = cbd_find(s, n, 1, &bytes);
    if (!sub || bytes != 1 || *sub > 1) return;
    struct nb_voice_client *c = nb_voice_client(v, slot, channel, cid, node, port);
    if (!c) return;
    c->binding = 1; c->next_subscription = *sub; c->bind_txn = (uint16_t)cbd_rd16(s + 1);
}
static inline int nb_voice_calls(struct nb_voice_client *c, const uint8_t *s, uint32_t n)
{
    uint32_t bytes = 0;
    const uint8_t *p = cbd_find(s, n, s[0] == 2 ? 0x10 : 1, &bytes);
    if (!p || !bytes || *p > NB_VOICE_CALLS || bytes != 1u + 7u * *p) return 0;
    struct nb_voice_call next[NB_VOICE_CALLS];
    __builtin_memset(next, 0, sizeof next);
    for (unsigned i = 0; i < *p; ++i) {
        const uint8_t *row = p + 1 + i * 7;
        if (!row[0] || row[1] < 1 || row[1] > 10) return 0;
        for (unsigned j = 0; j < i; ++j) if (next[j].id == row[0]) return 0;
        next[i].id = row[0]; next[i].state = row[1];
        if (row[1] == 8 || row[1] == 9) continue;
        for (unsigned j = 0; j < NB_VOICE_CALLS; ++j)
            if (c->call[j].id == row[0] && c->call[j].state != 8 && c->call[j].state != 9) {
                next[i].codec_known = c->call[j].codec_known;
                next[i].codec = c->call[j].codec; next[i].encoder_hz = c->call[j].encoder_hz;
            }
    }
    int changed = __builtin_memcmp(c->call, next, sizeof next) != 0;
    __builtin_memcpy(c->call, next, sizeof next);
    return changed;
}
static inline int nb_voice_codec(struct nb_voice_client *c, const uint8_t *s, uint32_t n)
{
    uint32_t bytes = 0;
    const uint8_t *id = cbd_find(s, n, 0x13, &bytes);
    if (!id || bytes != 1 || !*id) return 0;
    const uint8_t *codec = cbd_find(s, n, 0x11, &bytes);
    if (!codec || bytes != 4) return 0;
    uint32_t value = cbd_rd32(codec), hz = 0;
    const uint8_t *rate = cbd_find(s, n, 0x12, &bytes);
    if (rate) { if (bytes != 4) return 0; hz = cbd_rd32(rate); }
    for (unsigned i = 0; i < NB_VOICE_CALLS; ++i) {
        struct nb_voice_call *call = &c->call[i];
        if (call->id != *id || call->state == 8 || call->state == 9) continue;
        int changed = !call->codec_known || call->codec != value || call->encoder_hz != hz;
        call->codec_known = 1; call->codec = value; call->encoder_hz = hz;
        return changed;
    }
    return 0;
}
/* Called only after the endpoint-bound QMUX router accepted this original
 * response/indication. The original VOICE bytes are still forwarded intact. */
static inline int nb_voice_message(struct nb_voice *v, unsigned slot, unsigned channel, unsigned cid,
                                  uint32_t node, uint32_t port, const uint8_t *s, uint32_t n)
{
    if (!nb_voice_frame(s, n) || (s[0] != 2 && s[0] != 4)) return 0;
    struct nb_voice_client *c = nb_voice_client(v, slot, channel, cid, node, port);
    if (!c) return 0;
    unsigned msg = cbd_rd16(s + 3);
    if (s[0] == 2 && msg == 0x44) {
        if (!c->binding || c->bind_txn != cbd_rd16(s + 1)) return 0;
        c->binding = 0;
        if (!nb_voice_success(s, n)) return 0;
        int changed = !c->bound || c->subscription != c->next_subscription;
        if (changed) __builtin_memset(c->call, 0, sizeof c->call);
        c->bound = 1; c->subscription = c->next_subscription;
        return changed;
    }
    if (!c->bound || c->binding) return 0;
    if ((s[0] == 4 && msg == 0x2e) || (s[0] == 2 && msg == 0x2f && nb_voice_success(s, n)))
        return nb_voice_calls(c, s, n);
    if ((s[0] == 4 && msg == 0x53) || (s[0] == 2 && msg == 0x6e && nb_voice_success(s, n)))
        return nb_voice_codec(c, s, n);
    return 0;
}
static inline uint8_t nb_voice_rate(uint32_t hz)
{
    switch (hz) { case 8000: return 8; case 16000: return 16; case 24000: return 24;
        case 32000: return 32; case 48000: return 48; default: return 0; }
}
/* Factory codec enum -> Apple's actual codec enum. Unsupported QCELP/G.7xx
 * are not silently relabelled as EVRC or AMR. EVS includes its real encoder rate. */
static inline uint8_t nb_voice_apple_codec(uint32_t codec)
{
    switch (codec) {
    case 2: return 1; case 3: return 2; case 4: return 4; case 5: return 5;
    case 6: return 0x13; case 7: return 0x14; case 8: return 0x12;
    case 9: return 0x10; case 10: return 0x11;
    case 17: case 18: case 19: case 20: return 0x30;
    default: return 0xff;
    }
}
/* One foreground conversation per channel. Duplicate observers of the same
 * subscription/call are allowed; conflicting or simultaneous calls are unknown. */
static inline uint32_t nb_voice_vocoder(const struct nb_voice *v, unsigned channel, uint8_t *out, uint32_t room)
{
    if (channel >= CBQ_CHANNELS || room < 5) return 0;
    const struct nb_voice_call *found = 0; unsigned subscription = 0;
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i) {
        const struct nb_voice_client *c = &v->client[i];
        if (!c->used || !c->bound || c->binding || c->channel != channel) continue;
        for (unsigned j = 0; j < NB_VOICE_CALLS; ++j) {
            const struct nb_voice_call *call = &c->call[j];
            if (!call->id || call->state != 3) continue;
            if (found && (subscription != c->subscription || found->id != call->id ||
                          found->codec != call->codec || found->encoder_hz != call->encoder_hz ||
                          found->codec_known != call->codec_known)) return 0;
            found = call; subscription = c->subscription;
        }
    }
    if (!found || !found->codec_known || !nb_voice_rate(found->encoder_hz)) return 0;
    uint8_t codec = nb_voice_apple_codec(found->codec);
    if (codec == 0xff) return 0;
    out[0] = 1; cbd_wr16(out + 1, 2); out[3] = codec; out[4] = nb_voice_rate(found->encoder_hz);
    return 5;
}
#endif
