/* SPDX-License-Identifier: BSD-2-Clause */
/* NAS on QRTR identifies a client by its source port. The original iOS dual-SIM
 * bootstrap opens seventeen NAS clients; the stock service exhausts its client
 * state after ten. Keep one real NAS port per subscription and retain each
 * native client's transaction, binding and indication registration here.
 *
 * Sources: existing QcomBaseband framing, libqmi transaction ownership and
 * qmi-service-nas.json, and the exact stock NAS v1.421 IDL. WDS, VOICE, UIM and
 * their per-client sessions keep the original transport. No modem setting or
 * radio command is invented. All successful replies come from the real modem.
 * The caller holds the existing bridge lock and supplies allocation/emit hooks.
 */
#ifndef NB_NAS_MUX_H
#define NB_NAS_MUX_H
#include "coachz_bb_qmux.h"

#define NB_NAS_SERVICE 3u
#define NB_NAS_SUBSCRIPTIONS 2u
#define NB_NAS_PENDING CBQ_MAX_CLIENTS
#define NB_NAS_PORT_BASE (CBQ_PORT_BASE + CBQ_MAX_CLIENTS)
#define NB_NAS_BIND 0x45u
#define NB_NAS_REGISTER 3u
#define NB_NAS_NETWORK_TIME_CACHE 0xc0u
#define NB_NAS_REG_FIELDS 0x100u
#define NB_NAS_REG_MAX_BYTES 971u /* SDU +238 single-byte fields, two-byte0x21, four-byte0xc0 */
#define NB_NAS_WAIT_MS 300000u   /* libqmi proxy's existing maximum command wait */
#define NB_NAS_ERROR_INTERNAL 3u

struct nb_nas_registration {
    uint8_t present[NB_NAS_REG_FIELDS], enabled[NB_NAS_REG_FIELDS], suppress_reject;
    uint32_t network_time_cache_period;
};
struct nb_nas_client {
    uint8_t used, channel, cid, subscription;
    uint32_t generation, node, port;
    struct nb_nas_registration registration;
};
struct nb_nas_pending {
    uint8_t used, active, cancelled, is_register, is_bind;
    uint8_t subscription;
    uint16_t transaction, command;
    uint32_t client, generation, node, port, bytes, capacity;
    uint64_t order;
    uint8_t *data;
    uint8_t bind[11];
    struct nb_nas_registration registration;
};
struct nb_nas_master {
    uint8_t bound, hidden_bind, sending, reset_pending;
    uint16_t transaction, next_transaction;
    uint32_t pending, node, port, started;
    uint64_t order;
    struct nb_nas_registration registration;
};
struct nb_nas_packet {
    uint32_t master, local_port, node, port, bytes;
    uint16_t transaction;
    uint64_t order;
    const uint8_t *data;
};
typedef uint8_t *(*NbNasAllocate)(void *, uint32_t);
typedef void (*NbNasFree)(void *, uint8_t *, uint32_t);
typedef void (*NbNasEmit)(void *, uint32_t, uint32_t, uint32_t, const uint8_t *, uint32_t);
struct nb_nas_mux {
    struct nb_nas_client client[CBQ_MAX_CLIENTS];
    struct nb_nas_pending pending[NB_NAS_PENDING];
    struct nb_nas_master master[NB_NAS_SUBSCRIPTIONS];
    uint64_t next_order, forwarded, responses, indications, expired, refused;
    void *context;
    NbNasAllocate allocate;
    NbNasFree release;
    NbNasEmit emit;
};

static inline int nb_nas_sdu_ok(const uint8_t *s, uint32_t n)
{
    return s && n >= CBQ_SDU_HDR && n <= 65535u - CBQ_HDR
        && CBQ_SDU_HDR + cbq_rd16(s + 5) == n;
}
static inline uint32_t nb_nas_error(const uint8_t *s, uint32_t n)
{
    uint32_t size = 0;
    const uint8_t *p = cbq_tlv(s + 7, n - 7, 2, &size);
    return p && size == 4 ? (cbq_rd16(p) ? (cbq_rd16(p + 2) ? cbq_rd16(p + 2) : NB_NAS_ERROR_INTERNAL) : 0u)
                          : CBQ_ERR_MALFORMED_MESSAGE;
}
static inline void nb_nas_init(struct nb_nas_mux *s, void *context,
                              NbNasAllocate allocate, NbNasFree release, NbNasEmit emit)
{
    __builtin_memset(s, 0, sizeof *s);
    s->context = context; s->allocate = allocate; s->release = release; s->emit = emit;
    for (unsigned i = 0; i < NB_NAS_SUBSCRIPTIONS; ++i) s->master[i].pending = NB_NAS_PENDING;
}
static inline int nb_nas_owned(const struct nb_nas_mux *s, const struct nb_nas_pending *p)
{
    return p->client < CBQ_MAX_CLIENTS && s->client[p->client].used
        && s->client[p->client].generation == p->generation;
}
static inline void nb_nas_emit_error(struct nb_nas_mux *s, unsigned client,
                                    uint16_t transaction, uint16_t command, uint32_t error)
{
    if (client >= CBQ_MAX_CLIENTS || !s->client[client].used) return;
    uint8_t answer[14] = { CBQ_QMI_RESPONSE };
    cbq_wr16(answer + 1, transaction); cbq_wr16(answer + 3, command);
    cbq_wr16(answer + 5, 7); cbq_tlv_result(answer + 7, error);
    const struct nb_nas_client *c = &s->client[client];
    s->emit(s->context, client, c->channel, c->cid, answer, sizeof answer);
}
static inline void nb_nas_clear_pending(struct nb_nas_mux *s, unsigned index)
{
    struct nb_nas_pending *p = &s->pending[index];
    if (p->data) s->release(s->context, p->data, p->capacity);
    __builtin_memset(p, 0, sizeof *p);
}
static inline void nb_nas_retire(struct nb_nas_mux *s, unsigned client)
{
    if (client >= CBQ_MAX_CLIENTS) return;
    s->client[client].used = 0;
    for (unsigned i = 0; i < NB_NAS_PENDING; ++i) {
        struct nb_nas_pending *p = &s->pending[i];
        if (!p->used || p->client != client) continue;
        /* An active buffer remains owned until the worker has finished sending
         * and received/expired its response. It may be in use outside the lock. */
        if (p->active) p->cancelled = 1;
        else nb_nas_clear_pending(s, i);
    }
}
static inline void nb_nas_attach(struct nb_nas_mux *s, unsigned client,
                                uint32_t channel, uint32_t cid, uint32_t node, uint32_t port)
{
    struct nb_nas_client *c = &s->client[client];
    if (c->used && c->channel == channel && c->cid == cid && c->node == node && c->port == port) return;
    nb_nas_retire(s, client);
    uint32_t generation = c->generation + 1u;
    __builtin_memset(c, 0, sizeof *c);
    c->used = 1; c->channel = (uint8_t)channel; c->cid = (uint8_t)cid;
    c->generation = generation; c->node = node; c->port = port;
    /* NAS defaults to primary until BindSubscription actually succeeds. */
}
static inline int nb_nas_parse_registration(struct nb_nas_registration *r,
                                            const uint8_t *data, uint32_t bytes)
{
    uint8_t seen[NB_NAS_REG_FIELDS] = {0};
    for (uint32_t at = 0; at < bytes;) {
        if (bytes - at < 3) return 0;
        uint32_t id = data[at], n = cbq_rd16(data + at + 1);
        /* Native iOS also registers Apple fields. NetworkTimeCachePeriod0xc0
         * writes a little-endian uint32; frequency mode0xb0 writes one byte.
         * Preserve their values for the real modem to accept or reject. */
        const int stock_flag = id < 0x6bu && id != 0x11 && id != 0x16;
        const uint32_t expected = id == 0x21 ? 2u : id == NB_NAS_NETWORK_TIME_CACHE ? 4u : 1u;
        if (n > bytes - at - 3 || id < 0x10 || seen[id]
            || n != expected
            || (stock_flag && data[at + 3] > 1)
            || (id == 0x21 && data[at + 4] > 1)) return 0;
        seen[id] = r->present[id] = 1;
        if (id == NB_NAS_NETWORK_TIME_CACHE)
            r->network_time_cache_period = cbq_rd16(data + at + 3)
                | (uint32_t)cbq_rd16(data + at + 5) << 16;
        else r->enabled[id] = data[at + 3];
        if (id == 0x21) r->suppress_reject = data[at + 4];
        at += 3 + n;
    }
    return 1;
}
static inline uint32_t nb_nas_submit(struct nb_nas_mux *s, unsigned client,
                                    const uint8_t *data, uint32_t bytes)
{
    if (client >= CBQ_MAX_CLIENTS || !s->client[client].used
        || !nb_nas_sdu_ok(data, bytes) || data[0] != CBQ_QMI_REQUEST) return CBQ_ERR_MALFORMED_MESSAGE;
    unsigned free_slot = NB_NAS_PENDING;
    for (unsigned i = 0; i < NB_NAS_PENDING; ++i)
        if (!s->pending[i].used) { free_slot = i; break; }
    if (free_slot == NB_NAS_PENDING) return CBQ_ERR_CLIENT_IDS_EXHAUSTED;
    struct nb_nas_pending *p = &s->pending[free_slot];
    const struct nb_nas_client *c = &s->client[client];
    p->is_bind = cbq_rd16(data + 3) == NB_NAS_BIND;
    p->is_register = cbq_rd16(data + 3) == NB_NAS_REGISTER;
    if (p->is_bind) {
        uint32_t n = 0;
        const uint8_t *sub = cbq_tlv(data + 7, bytes - 7, 1, &n);
        if (!sub || n != 1 || bytes != 11) return CBQ_ERR_MALFORMED_MESSAGE;
        if (*sub >= NB_NAS_SUBSCRIPTIONS) return CBQ_ERR_NOT_SUPPORTED;
        p->subscription = *sub;
    }
    if (p->is_register) {
        /* Queue only this request's updates. Fold them into the client's last
         * successful registration when it reaches the wire, in request order. */
        __builtin_memset(&p->registration, 0, sizeof p->registration);
        if (!nb_nas_parse_registration(&p->registration, data + 7, bytes - 7))
            return CBQ_ERR_MALFORMED_MESSAGE;
    }
    p->capacity = p->is_register && bytes < NB_NAS_REG_MAX_BYTES ? NB_NAS_REG_MAX_BYTES : bytes;
    p->data = s->allocate(s->context, p->capacity);
    if (!p->data) { __builtin_memset(p, 0, sizeof *p); return NB_NAS_ERROR_INTERNAL; }
    __builtin_memcpy(p->data, data, bytes);
    p->used = 1; p->client = client; p->generation = c->generation;
    p->node = c->node; p->port = c->port; p->bytes = bytes;
    p->transaction = cbq_rd16(data + 1); p->command = cbq_rd16(data + 3);
    p->order = ++s->next_order;
    return 0;
}
static inline uint32_t nb_nas_registration_packet(struct nb_nas_mux *s, unsigned lane,
                                                  struct nb_nas_pending *p)
{
    struct nb_nas_registration update = p->registration;
    p->registration = s->client[p->client].registration;
    for (unsigned id = 0x10; id < NB_NAS_REG_FIELDS; ++id) {
        if (!update.present[id]) continue;
        p->registration.present[id] = 1;
        p->registration.enabled[id] = update.enabled[id];
        if (id == 0x21) p->registration.suppress_reject = update.suppress_reject;
        if (id == NB_NAS_NETWORK_TIME_CACHE)
            p->registration.network_time_cache_period = update.network_time_cache_period;
    }
    struct nb_nas_registration merged = s->master[lane].registration;
    __builtin_memset(merged.enabled, 0, sizeof merged.enabled);
    merged.suppress_reject = 1;
    int cache_period_seen = 0;
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i) {
        const struct nb_nas_client *c = &s->client[i];
        if (!c->used || c->subscription != lane || c->node != p->node || c->port != p->port) continue;
        const struct nb_nas_registration *r = i == p->client ? &p->registration : &c->registration;
        for (unsigned id = 0x10; id < NB_NAS_REG_FIELDS; ++id) {
            if (!r->present[id]) continue;
            merged.present[id] = 1;
            if (id == NB_NAS_NETWORK_TIME_CACHE) {
                /* This is a duration, including an explicit zero. Equal
                 * periods can share a subscription; differing periods have
                 * no established union. Keep the accepted modem state until
                 * a request with one unambiguous period actually succeeds. */
                if (cache_period_seen
                    && r->network_time_cache_period != merged.network_time_cache_period) return 0;
                merged.network_time_cache_period = r->network_time_cache_period;
                cache_period_seen = 1;
            } else if (id < 0x6bu && id != 0x11 && id != 0x16) {
                merged.enabled[id] |= r->enabled[id];
            } else {
                /* Opaque one-byte modes are not bit masks. Equal nonzero
                 * requests share the original value, and a client's zero
                 * does not withdraw another client's active report mode.
                 * Incompatible active modes cannot be merged without knowing
                 * their modem semantics; retain the last accepted state. */
                const uint8_t value = r->enabled[id];
                if (value && merged.enabled[id] && value != merged.enabled[id]) return 0;
                if (value) merged.enabled[id] = value;
            }
        }
        if (r->present[0x21] && r->enabled[0x21]) merged.suppress_reject &= r->suppress_reject;
    }
    if (!merged.enabled[0x21]) merged.suppress_reject = 0;
    uint32_t at = CBQ_SDU_HDR;
    for (unsigned id = 0x10; id < NB_NAS_REG_FIELDS; ++id) {
        if (!merged.present[id]) continue;
        const uint32_t size = id == 0x21 ? 2u : id == NB_NAS_NETWORK_TIME_CACHE ? 4u : 1u;
        p->data[at] = (uint8_t)id; cbq_wr16(p->data + at + 1, size);
        if (id == NB_NAS_NETWORK_TIME_CACHE) {
            cbq_wr16(p->data + at + 3, merged.network_time_cache_period);
            cbq_wr16(p->data + at + 5, merged.network_time_cache_period >> 16);
        } else {
            p->data[at + 3] = merged.enabled[id];
            if (id == 0x21) p->data[at + 4] = merged.suppress_reject;
        }
        at += 3 + size;
    }
    cbq_wr16(p->data + 5, at - CBQ_SDU_HDR);
    return at;
}
static inline int nb_nas_earlier_client(const struct nb_nas_mux *s, const struct nb_nas_pending *p)
{
    for (unsigned i = 0; i < NB_NAS_PENDING; ++i)
        if (s->pending[i].used && s->pending[i].client == p->client
            && s->pending[i].order < p->order) return 1;
    return 0;
}
static inline int nb_nas_has_registration(const struct nb_nas_registration *r)
{
    for (unsigned id = 0x10; id < NB_NAS_REG_FIELDS; ++id)
        if (r->present[id]) return 1;
    return 0;
}
static inline int nb_nas_next(struct nb_nas_mux *s, uint32_t now, struct nb_nas_packet *out)
{
    unsigned chosen = NB_NAS_PENDING, lane = 0;
    uint64_t order = ~(uint64_t)0;
    for (unsigned i = 0; i < NB_NAS_PENDING; ++i) {
        struct nb_nas_pending *p = &s->pending[i];
        if (!p->used || p->active) continue;
        if (p->cancelled || !nb_nas_owned(s, p)) { nb_nas_clear_pending(s, i); continue; }
        unsigned sub = p->is_bind ? p->subscription : s->client[p->client].subscription;
        if (s->master[sub].pending != NB_NAS_PENDING || nb_nas_earlier_client(s, p)) continue;
        if (p->order < order) { chosen = i; lane = sub; order = p->order; }
    }
    if (chosen == NB_NAS_PENDING) return 0;
    struct nb_nas_pending *p = &s->pending[chosen];
    if (p->is_bind && p->subscription != s->client[p->client].subscription
        && nb_nas_has_registration(&s->client[p->client].registration)) {
        /* Moving an already registered client also requires withdrawing its
         * previous subscription's merged flags. Do not leave stale cross-SIM
         * indications behind or report a bind that this scope cannot complete. */
        nb_nas_emit_error(s, p->client, p->transaction, p->command, CBQ_ERR_NOT_SUPPORTED);
        nb_nas_clear_pending(s, chosen);
        return 0;
    }
    struct nb_nas_master *m = &s->master[lane];
    if (m->port && (m->port != p->port || m->node != p->node)) {
        nb_nas_emit_error(s, p->client, p->transaction, p->command, CBQ_ERR_INVALID_CLIENT_ID);
        nb_nas_clear_pending(s, chosen);
        return 0;
    }
    m->node = p->node; m->port = p->port; m->pending = chosen;
    m->transaction = ++m->next_transaction;
    if (!m->transaction) m->transaction = ++m->next_transaction;
    m->hidden_bind = !m->bound && !p->is_bind; m->sending = 1;
    m->started = now; m->order = p->order; p->active = 1;
    const uint8_t *data;
    uint32_t bytes;
    if (m->hidden_bind) {
        __builtin_memset(p->bind, 0, sizeof p->bind);
        cbq_wr16(p->bind + 1, m->transaction); cbq_wr16(p->bind + 3, NB_NAS_BIND);
        cbq_wr16(p->bind + 5, 4); p->bind[7] = 1;
        cbq_wr16(p->bind + 8, 1); p->bind[10] = (uint8_t)lane;
        data = p->bind; bytes = sizeof p->bind;
    } else {
        if (p->is_register) {
            p->bytes = nb_nas_registration_packet(s, lane, p);
            if (!p->bytes) {
                nb_nas_emit_error(s, p->client, p->transaction, p->command, CBQ_ERR_NOT_SUPPORTED);
                nb_nas_clear_pending(s, chosen);
                m->pending = NB_NAS_PENDING; m->sending = 0;
                return 0;
            }
        }
        cbq_wr16(p->data + 1, m->transaction); data = p->data; bytes = p->bytes;
    }
    /* The pending request owns both buffers until sending completes, including
     * when its client/endpoint disappears while the worker is outside the lock. */
    *out = (struct nb_nas_packet){ lane, NB_NAS_PORT_BASE + lane, m->node, m->port,
                                  bytes, m->transaction, m->order, data };
    return 1;
}
static inline void nb_nas_failed(struct nb_nas_mux *s, unsigned lane, uint32_t error)
{
    struct nb_nas_master *m = &s->master[lane];
    struct nb_nas_pending *p = &s->pending[m->pending];
    if (!p->cancelled && nb_nas_owned(s, p))
        nb_nas_emit_error(s, p->client, p->transaction, p->command, error);
    nb_nas_clear_pending(s, m->pending);
    m->pending = NB_NAS_PENDING; m->sending = 0;
}
static inline void nb_nas_sent(struct nb_nas_mux *s, const struct nb_nas_packet *p, int result)
{
    struct nb_nas_master *m = &s->master[p->master];
    if (m->pending == NB_NAS_PENDING || m->order != p->order || m->transaction != p->transaction) return;
    m->sending = 0;
    if (m->reset_pending) {
        uint16_t next = m->next_transaction;
        nb_nas_clear_pending(s, m->pending);
        __builtin_memset(m, 0, sizeof *m);
        m->pending = NB_NAS_PENDING; m->next_transaction = next;
        return;
    }
    if (result) { s->refused++; nb_nas_failed(s, p->master, NB_NAS_ERROR_INTERNAL); }
    else s->forwarded++;
}
static inline int nb_nas_receive(struct nb_nas_mux *s, uint32_t local_port,
                                 uint32_t node, uint32_t port, const uint8_t *data, uint32_t bytes)
{
    if (local_port < NB_NAS_PORT_BASE || local_port >= NB_NAS_PORT_BASE + NB_NAS_SUBSCRIPTIONS) return 0;
    unsigned lane = local_port - NB_NAS_PORT_BASE;
    struct nb_nas_master *m = &s->master[lane];
    if (!m->port || node != m->node || port != m->port || m->reset_pending
        || !nb_nas_sdu_ok(data, bytes)) return 1;
    if (data[0] == CBQ_QMI_INDICATION) {
        if (!m->bound) return 1;
        for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i) {
            const struct nb_nas_client *c = &s->client[i];
            if (c->used && c->subscription == lane && c->node == node && c->port == port)
                s->emit(s->context, i, c->channel, c->cid, data, bytes);
        }
        s->indications++;
        return 1;
    }
    if (data[0] != CBQ_QMI_RESPONSE || m->pending == NB_NAS_PENDING
        || cbq_rd16(data + 1) != m->transaction || m->sending) return 1;
    struct nb_nas_pending *p = &s->pending[m->pending];
    if (cbq_rd16(data + 3) != (m->hidden_bind ? NB_NAS_BIND : p->command)) return 1;
    uint32_t error = nb_nas_error(data, bytes);
    s->responses++;
    if (m->hidden_bind) {
        if (error) nb_nas_failed(s, lane, error);
        else {
            m->bound = 1; p->active = 0; m->pending = NB_NAS_PENDING;
            if (p->cancelled || !nb_nas_owned(s, p)) nb_nas_clear_pending(s, (unsigned)(p - s->pending));
        }
        return 1;
    }
    if (!error && p->is_bind) {
        m->bound = 1;
        if (!p->cancelled && nb_nas_owned(s, p)) s->client[p->client].subscription = p->subscription;
    }
    if (!error && p->is_register) {
        /* The real modem just applied the merged packet. Retain that state only
         * after its successful response; a rejected request changes no cache. */
        __builtin_memset(&m->registration, 0, sizeof m->registration);
        nb_nas_parse_registration(&m->registration, p->data + 7, p->bytes - 7);
        if (!p->cancelled && nb_nas_owned(s, p)) s->client[p->client].registration = p->registration;
    }
    if (!error && p->command == 0) {
        /* NAS Reset clears this real client's report configuration. Keeping
         * the old union would silently suppress the next registration update. */
        __builtin_memset(&m->registration, 0, sizeof m->registration);
        for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i)
            if (s->client[i].used && s->client[i].subscription == lane)
                __builtin_memset(&s->client[i].registration, 0, sizeof s->client[i].registration);
        m->bound = 0;
    }
    if (!p->cancelled && nb_nas_owned(s, p)) {
        uint8_t *copy = s->allocate(s->context, bytes);
        if (copy) {
            __builtin_memcpy(copy, data, bytes); cbq_wr16(copy + 1, p->transaction);
            const struct nb_nas_client *c = &s->client[p->client];
            s->emit(s->context, p->client, c->channel, c->cid, copy, bytes);
            s->release(s->context, copy, bytes);
        } else nb_nas_emit_error(s, p->client, p->transaction, p->command, NB_NAS_ERROR_INTERNAL);
    }
    nb_nas_clear_pending(s, m->pending); m->pending = NB_NAS_PENDING;
    return 1;
}
static inline void nb_nas_poll_deadlines(struct nb_nas_mux *s, uint32_t now)
{
    for (unsigned i = 0; i < NB_NAS_SUBSCRIPTIONS; ++i) {
        struct nb_nas_master *m = &s->master[i];
        if (m->pending != NB_NAS_PENDING && !m->sending && now - m->started >= NB_NAS_WAIT_MS) {
            s->expired++; nb_nas_failed(s, i, NB_NAS_ERROR_INTERNAL);
        }
    }
}
static inline void nb_nas_endpoint_lost(struct nb_nas_mux *s, uint32_t node, uint32_t port)
{
    for (unsigned i = 0; i < CBQ_MAX_CLIENTS; ++i)
        if (s->client[i].used && s->client[i].node == node && s->client[i].port == port) nb_nas_retire(s, i);
    for (unsigned i = 0; i < NB_NAS_SUBSCRIPTIONS; ++i) {
        struct nb_nas_master *m = &s->master[i];
        if (m->node != node || m->port != port) continue;
        if (m->sending) { m->reset_pending = 1; m->bound = 0; continue; }
        if (m->pending != NB_NAS_PENDING) nb_nas_clear_pending(s, m->pending);
        uint16_t next = m->next_transaction;
        __builtin_memset(m, 0, sizeof *m);
        m->pending = NB_NAS_PENDING; m->next_transaction = next;
    }
}
#endif
