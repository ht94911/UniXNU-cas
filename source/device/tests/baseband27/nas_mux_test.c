/* SPDX-License-Identifier: BSD-2-Clause */
/* Synthetic QRTR replies exercise the real multiplexer. No phone or RF action. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "NbNasMux.h"

struct emission { unsigned client, channel, cid, bytes; uint8_t data[512]; };
static struct emission output[128];
static unsigned outputs, allocations, frees;
static uint8_t *allocate(void *context, uint32_t bytes)
{ (void)context; allocations++; return malloc(bytes); }
static void release(void *context, uint8_t *p, uint32_t bytes)
{ (void)context; (void)bytes; frees++; free(p); }
static void emit(void *context, uint32_t client, uint32_t channel, uint32_t cid,
                 const uint8_t *data, uint32_t bytes)
{
    (void)context;
    assert(outputs < 128 && bytes <= sizeof output[0].data);
    struct emission *e = &output[outputs++];
    e->client = client; e->channel = channel; e->cid = cid; e->bytes = bytes;
    memcpy(e->data, data, bytes);
}
static unsigned message(uint8_t *s, unsigned command, unsigned txn,
                         const uint8_t *body, unsigned bytes)
{
    memset(s, 0, 512);
    cbq_wr16(s + 1, txn); cbq_wr16(s + 3, command); cbq_wr16(s + 5, bytes);
    if (bytes) memcpy(s + 7, body, bytes);
    return 7 + bytes;
}
static void respond(struct nb_nas_mux *m, const struct nb_nas_packet *p, unsigned error)
{
    uint8_t reply[18] = { 2 };
    cbq_wr16(reply + 1, p->transaction); cbq_wr16(reply + 3, cbq_rd16(p->data + 3));
    cbq_wr16(reply + 5, 11); cbq_tlv_result(reply + 7, error);
    reply[14] = 0x10; reply[15] = 1; reply[17] = 0xa7;
    nb_nas_sent(m, p, 0);
    assert(nb_nas_receive(m, p->local_port, p->node, p->port, reply, sizeof reply) == 1);
}
static void stop(struct nb_nas_mux *m)
{
    nb_nas_endpoint_lost(m, 0, 0x123);
    assert(allocations == frees);
}
static void seventeen_clients(void)
{
    struct nb_nas_mux m;
    uint8_t data[512];
    nb_nas_init(&m, NULL, allocate, release, emit); outputs = 0;
    for (unsigned i = 0; i < 17; ++i) {
        nb_nas_attach(&m, i, i & 1, i + 1, 0, 0x123);
        const uint8_t bind[] = { 1, 1, 0, i & 1 };
        unsigned n = message(data, NB_NAS_BIND, 1, bind, sizeof bind);
        assert(!nb_nas_submit(&m, i, data, n));
    }
    struct nb_nas_packet primary, secondary;
    assert(nb_nas_next(&m, 100, &primary) && primary.master == 0);
    assert(nb_nas_next(&m, 100, &secondary) && secondary.master == 1);
    assert(primary.local_port != secondary.local_port);
    assert(primary.data[10] == 0 && secondary.data[10] == 1);
    /* Same transaction on two subscriptions; replies arrive in reverse order. */
    respond(&m, &secondary, 0); respond(&m, &primary, 0);
    assert(output[0].client == 1 && output[1].client == 0);
    unsigned count = 2;
    while (nb_nas_next(&m, 101, &primary)) { respond(&m, &primary, 0); count++; }
    assert(count == 17 && outputs == 17);
    for (unsigned i = 0; i < 17; ++i) {
        assert(m.client[i].subscription == (i & 1));
        unsigned n = message(data, 0x34, 0x1234, NULL, 0);
        assert(!nb_nas_submit(&m, i, data, n));
    }
    outputs = 0; count = 0;
    while (nb_nas_next(&m, 102, &primary)) { respond(&m, &primary, count == 5 ? 5 : 0); count++; }
    assert(count == 17 && outputs == 17);
    for (unsigned i = 0; i < 17; ++i) {
        assert(cbq_rd16(output[i].data + 1) == 0x1234 && output[i].data[17] == 0xa7);
        assert(nb_nas_error(output[i].data, output[i].bytes) == (i == 5 ? 5u : 0u));
    }
    /* Indications retain the full payload and go only to the bound subscription. */
    uint8_t indication[] = { 4, 0, 0, 0x34, 0, 4, 0, 0x10, 1, 0, 7 };
    outputs = 0;
    assert(nb_nas_receive(&m, NB_NAS_PORT_BASE + 1, 0, 0x123, indication, sizeof indication));
    assert(outputs == 8);
    for (unsigned i = 0; i < outputs; ++i)
        assert((output[i].client & 1) && !memcmp(output[i].data, indication, sizeof indication));
    stop(&m);
}
static void registration_union(void)
{
    struct nb_nas_mux m;
    uint8_t data[512];
    nb_nas_init(&m, NULL, allocate, release, emit); outputs = 0;
    nb_nas_attach(&m, 0, 0, 1, 0, 0x123); nb_nas_attach(&m, 1, 1, 1, 0, 0x123);
    uint8_t on[] = { 0x18, 1, 0, 1 }, off[] = { 0x18, 1, 0, 0 };
    unsigned n = message(data, 3, 9, on, sizeof on);
    assert(!nb_nas_submit(&m, 0, data, n));
    struct nb_nas_packet p;
    /* A query issued before a client's explicit bind performs one real bind. */
    assert(nb_nas_next(&m, 1, &p) && cbq_rd16(p.data + 3) == NB_NAS_BIND);
    respond(&m, &p, 0); assert(outputs == 0);
    assert(nb_nas_next(&m, 2, &p) && cbq_rd16(p.data + 3) == 3);
    respond(&m, &p, 0); assert(m.client[0].registration.enabled[0x18]);
    n = message(data, 3, 10, off, sizeof off);
    assert(!nb_nas_submit(&m, 1, data, n));
    assert(nb_nas_next(&m, 3, &p) && p.data[10] == 1);
    respond(&m, &p, 0); assert(!m.client[1].registration.enabled[0x18]);
    n = message(data, 3, 11, off, sizeof off);
    assert(!nb_nas_submit(&m, 0, data, n));
    assert(nb_nas_next(&m, 4, &p) && p.data[10] == 0);
    respond(&m, &p, 71);
    assert(m.client[0].registration.enabled[0x18] && m.master[0].registration.enabled[0x18]);
    assert(nb_nas_error(output[outputs - 1].data, output[outputs - 1].bytes) == 71);
    /* Invalid schema cannot update a subscriber or issue a real request. */
    on[3] = 2; n = message(data, 3, 12, on, sizeof on);
    assert(nb_nas_submit(&m, 0, data, n) == CBQ_ERR_MALFORMED_MESSAGE);
    assert(!nb_nas_next(&m, 5, &p));
    /* An already registered client cannot move subscriptions until the old
     * merged registration is withdrawn; report the actual unsupported scope. */
    uint8_t bind[] = { 1, 1, 0, 1 };
    n = message(data, NB_NAS_BIND, 13, bind, sizeof bind);
    assert(!nb_nas_submit(&m, 0, data, n));
    assert(!nb_nas_next(&m, 6, &p));
    assert(nb_nas_error(output[outputs - 1].data, output[outputs - 1].bytes) == CBQ_ERR_NOT_SUPPORTED);
    assert(m.client[0].subscription == 0 && m.master[0].registration.enabled[0x18]);
    n = message(data, 0, 14, NULL, 0);
    assert(!nb_nas_submit(&m, 0, data, n)); assert(nb_nas_next(&m, 7, &p));
    respond(&m, &p, 0);
    assert(!m.master[0].bound && !nb_nas_has_registration(&m.client[0].registration));
    stop(&m);
}
static void cancellation_and_endpoint(void)
{
    struct nb_nas_mux m;
    uint8_t data[512], bind[] = { 1, 1, 0, 0 };
    nb_nas_init(&m, NULL, allocate, release, emit); outputs = 0;
    nb_nas_attach(&m, 0, 0, 1, 0, 0x123);
    unsigned n = message(data, NB_NAS_BIND, 4, bind, sizeof bind);
    assert(!nb_nas_submit(&m, 0, data, n));
    struct nb_nas_packet p; assert(nb_nas_next(&m, 10, &p));
    nb_nas_retire(&m, 0);
    nb_nas_attach(&m, 0, 0, 2, 0, 0x123);
    respond(&m, &p, 0); assert(outputs == 0);
    n = message(data, 0x34, 5, NULL, 0);
    assert(!nb_nas_submit(&m, 0, data, n));
    assert(nb_nas_next(&m, 11, &p));
    uint8_t reply[14] = {2}; cbq_wr16(reply + 1, p.transaction);
    cbq_wr16(reply + 3, 0x34); cbq_wr16(reply + 5, 7); cbq_tlv_result(reply + 7, 0);
    nb_nas_sent(&m, &p, 0);
    nb_nas_receive(&m, p.local_port, 1, p.port, reply, sizeof reply); assert(outputs == 0);
    nb_nas_receive(&m, p.local_port, 0, p.port + 1, reply, sizeof reply); assert(outputs == 0);
    nb_nas_poll_deadlines(&m, 11 + NB_NAS_WAIT_MS);
    assert(outputs == 1 && nb_nas_error(output[0].data, output[0].bytes));
    assert(!nb_nas_submit(&m, 0, data, n)); assert(nb_nas_next(&m, 12, &p));
    unsigned old_frees = frees;
    nb_nas_endpoint_lost(&m, 0, 0x123);
    assert(m.master[p.master].reset_pending && frees == old_frees);
    nb_nas_sent(&m, &p, 0); assert(allocations == frees && !m.master[p.master].port);
    assert(nb_nas_receive(&m, p.local_port, p.node, p.port, reply, sizeof reply) && outputs == 1);
}
static void ordered_requests_and_failures(void)
{
    struct nb_nas_mux m;
    uint8_t data[512];
    nb_nas_init(&m, NULL, allocate, release, emit); outputs = 0;
    nb_nas_attach(&m, 0, 0, 1, 0, 0x123);
    uint8_t secondary[] = { 1, 1, 0, 1 };
    unsigned n = message(data, NB_NAS_BIND, 1, secondary, sizeof secondary);
    assert(!nb_nas_submit(&m, 0, data, n));
    n = message(data, 0x34, 2, NULL, 0);
    assert(!nb_nas_submit(&m, 0, data, n));
    struct nb_nas_packet p, other;
    assert(nb_nas_next(&m, 20, &p) && p.master == 1);
    assert(!nb_nas_next(&m, 20, &other));
    respond(&m, &p, 0);
    assert(nb_nas_next(&m, 21, &p) && p.master == 1 && cbq_rd16(p.data + 3) == 0x34);
    nb_nas_sent(&m, &p, -3);
    assert(outputs == 2 && nb_nas_error(output[1].data, output[1].bytes) == NB_NAS_ERROR_INTERNAL);
    /* A new client starts on primary; a failed implicit bind fails its original
     * query, without reporting success or changing the original message id. */
    nb_nas_attach(&m, 1, 0, 2, 0, 0x123);
    n = message(data, 0x4f, 0x5678, NULL, 0);
    assert(!nb_nas_submit(&m, 1, data, n)); assert(nb_nas_next(&m, 22, &p));
    assert(p.bytes == 11 && cbq_rd16(p.data + 3) == NB_NAS_BIND);
    respond(&m, &p, 5);
    assert(outputs == 3 && cbq_rd16(output[2].data + 1) == 0x5678
        && cbq_rd16(output[2].data + 3) == 0x4f
        && nb_nas_error(output[2].data, output[2].bytes) == 5);
    assert(!m.master[0].bound && !nb_nas_next(&m, 23, &p));
    /* Bounds and queued ownership do not evict previously accepted requests. */
    for (unsigned i = 0; i < NB_NAS_PENDING; ++i) {
        n = message(data, 0x34, i + 10, NULL, 0);
        assert(!nb_nas_submit(&m, 0, data, n));
    }
    assert(nb_nas_submit(&m, 0, data, n) == CBQ_ERR_CLIENT_IDS_EXHAUSTED);
    nb_nas_retire(&m, 0);
    assert(!nb_nas_next(&m, 24, &p));
    stop(&m);
}
static void original_client_exhaustion(void)
{
    /* The synthetic service keeps the observed stock ten-port capacity. The
     * original production QMUX path creates seventeen different QRTR ports.
     * The new production NAS path gives the same clients two real ports. */
    struct cbq_bridge b;
    cbq_init(&b); assert(cbq_server_new(&b, 3, 1, 0, 0x123));
    unsigned original_ports[17], errors = 0;
    for (unsigned i = 0; i < 17; ++i) {
        int slot = cbq_client_alloc(&b, 0, 3);
        assert(slot >= 0);
        uint8_t frame[17] = { 1, 16, 0, 0, 3, b.client[slot].cid, 0, 1, 0, 0x45, 0, 4, 0, 1, 1, 0, i & 1 };
        uint8_t out[32];
        struct cbq_result r = cbq_from_host(&b, 0, frame, sizeof frame, out, sizeof out);
        assert(r.kind == CBQ_TO_MODEM);
        original_ports[i] = r.local_port;
        for (unsigned j = 0; j < i; ++j) assert(original_ports[i] != original_ports[j]);
        uint8_t answer[14] = { 2, 1, 0, 0x45, 0, 7, 0 };
        cbq_tlv_result(answer + 7, i >= 10 ? 5 : 0);
        r = cbq_from_modem(&b, original_ports[i], 0, 0x123, answer, sizeof answer, out, sizeof out);
        assert(r.kind == CBQ_TO_HOST);
        errors += nb_nas_error(out + CBQ_HDR, r.len - CBQ_HDR) == 5;
    }
    assert(errors == 7);
    struct nb_nas_mux m;
    nb_nas_init(&m, NULL, allocate, release, emit); outputs = 0;
    unsigned used[2] = {0};
    for (unsigned i = 0; i < 17; ++i) {
        nb_nas_attach(&m, i, 0, b.client[i].cid, 0, 0x123);
        uint8_t data[512], bind[] = {1, 1, 0, i & 1};
        unsigned n = message(data, NB_NAS_BIND, 1, bind, sizeof bind);
        assert(!nb_nas_submit(&m, i, data, n));
    }
    struct nb_nas_packet p;
    while (nb_nas_next(&m, 40, &p)) {
        assert(p.local_port == NB_NAS_PORT_BASE + p.master);
        used[p.master]++; respond(&m, &p, 0);
    }
    assert(used[0] == 9 && used[1] == 8 && outputs == 17);
    for (unsigned i = 0; i < outputs; ++i) assert(!nb_nas_error(output[i].data, output[i].bytes));
    stop(&m);
}
static void native_extension_registration(void)
{
    struct nb_nas_mux m;
    uint8_t data[512];
    nb_nas_init(&m, NULL, allocate, release, emit); outputs = 0;
    nb_nas_attach(&m, 0, 0, 1, 0, 0x123);
    nb_nas_attach(&m, 1, 0, 2, 0, 0x123);
    /* Actual native Mav20QMIDesenseCommandDriver::registerFrequencyReporting
     * constructs TLV0xb0 and stores2, then its unchanged writer emits one byte.
     * Network registration also emits the three one-byte Apple flags below.
     * The018 parser rejected these structurally valid requests before QRTR. */
    const uint8_t native[] = {
        0x18, 1, 0, 1, 0x21, 2, 0, 1, 0,
        0xb0, 1, 0, 2, 0xd2, 1, 0, 1,
        0xe4, 1, 0, 1, 0xe7, 1, 0, 1,
    };
    unsigned n = message(data, NB_NAS_REGISTER, 0x34, native, sizeof native);
    assert(!nb_nas_submit(&m, 0, data, n));
    struct nb_nas_packet p;
    assert(nb_nas_next(&m, 1, &p) && cbq_rd16(p.data + 3) == NB_NAS_BIND);
    respond(&m, &p, 0);
    assert(nb_nas_next(&m, 2, &p) && cbq_rd16(p.data + 3) == NB_NAS_REGISTER);
    assert(p.bytes == n && !memcmp(p.data + 7, native, sizeof native));
    respond(&m, &p, 0);
    assert(m.client[0].registration.enabled[0xb0] == 2);
    const uint8_t withdraw[] = { 0xb0, 1, 0, 0, 0xe7, 1, 0, 0 };
    n = message(data, NB_NAS_REGISTER, 0x35, withdraw, sizeof withdraw);
    assert(!nb_nas_submit(&m, 1, data, n) && nb_nas_next(&m, 3, &p));
    uint32_t size = 0;
    const uint8_t *mode = cbq_tlv(p.data + 7, p.bytes - 7, 0xb0, &size);
    assert(mode && size == 1 && *mode == 2);
    respond(&m, &p, 0);
    assert(!m.client[1].registration.enabled[0xb0] && m.master[0].registration.enabled[0xb0] == 2);
    /* A real unsupported response must not change the extension cache. */
    n = message(data, NB_NAS_REGISTER, 0x36, withdraw, sizeof withdraw);
    assert(!nb_nas_submit(&m, 0, data, n) && nb_nas_next(&m, 4, &p));
    mode = cbq_tlv(p.data + 7, p.bytes - 7, 0xb0, &size);
    assert(mode && !*mode);
    respond(&m, &p, 71);
    assert(m.client[0].registration.enabled[0xb0] == 2);
    const uint8_t conflict[] = { 0xb0, 1, 0, 1 };
    n = message(data, NB_NAS_REGISTER, 0x37, conflict, sizeof conflict);
    assert(!nb_nas_submit(&m, 1, data, n));
    assert(!nb_nas_next(&m, 5, &p));
    assert(nb_nas_error(output[outputs - 1].data, output[outputs - 1].bytes) == CBQ_ERR_NOT_SUPPORTED);
    assert(m.master[0].registration.enabled[0xb0] == 2);
    /* Duplicate, truncated and malformed stock fields still fail on structure;
     * no modem reply is manufactured for any extension. */
    const uint8_t duplicate[] = { 0xb0, 1, 0, 2, 0xb0, 1, 0, 2 };
    n = message(data, NB_NAS_REGISTER, 0x38, duplicate, sizeof duplicate);
    assert(nb_nas_submit(&m, 1, data, n) == CBQ_ERR_MALFORMED_MESSAGE);
    n = message(data, NB_NAS_REGISTER, 0x39, native, sizeof native - 1);
    assert(nb_nas_submit(&m, 1, data, n) == CBQ_ERR_MALFORMED_MESSAGE);
    /* The original network-registration builder emits this optional field set
     * when its feature queries are true. Its TLVs are deliberately in native
     * insertion order, while the wire union is emitted in TLV-id order. */
    const uint8_t network[] = {
        0x10, 1, 0, 1, 0x23, 1, 0, 1, 0x13, 1, 0, 0, 0x18, 1, 0, 1,
        0x1e, 1, 0, 1, 0x17, 1, 0, 1, 0x25, 1, 0, 1, 0x29, 1, 0, 1,
        0x26, 1, 0, 1, 0x2b, 1, 0, 1, 0x21, 2, 0, 1, 0, 0x4b, 1, 0, 1,
        0x61, 1, 0, 1, 0xe4, 1, 0, 1, 0xd2, 1, 0, 1, 0x4a, 1, 0, 1,
        0xe7, 1, 0, 1,
    };
    n = message(data, NB_NAS_REGISTER, 0x3a, network, sizeof network);
    assert(!nb_nas_submit(&m, 1, data, n) && nb_nas_next(&m, 6, &p));
    for (unsigned at = 0; at < sizeof network;) {
        const uint8_t *field = cbq_tlv(p.data + 7, p.bytes - 7, network[at], &size);
        const unsigned bytes = cbq_rd16(network + at + 1);
        assert(field && size == bytes && !memcmp(field, network + at + 3, bytes));
        at += 3 + bytes;
    }
    respond(&m, &p, 0);
    assert(m.client[1].registration.enabled[0xe7] == 1);
    stop(&m);
}
int main(void)
{
    original_client_exhaustion(); seventeen_clients(); registration_union();
    native_extension_registration();
    cancellation_and_endpoint(); ordered_requests_and_failures();
    puts("PASS NAS mux:17 clients, two subscriptions, transaction restoration, real errors, registration union, cancellation and endpoint lifetime");
    return 0;
}
