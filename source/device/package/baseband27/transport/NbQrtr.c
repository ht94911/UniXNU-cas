/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Derived from sheng package/QcomQrtr.c (BSD-2-Clause-Patent). Changes: tables sized for the modem, both header
 * versions, packet size bounds per af_qrtr.c qrtr_endpoint_post, server removal reporting, DEL_CLIENT, an unrouted
 * port hook, and RESUME_TX as af_qrtr.c qrtr_send_resume_tx builds it (from the receiving port, carrying that port). */
#include "NbQrtr.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

#define CTRL_BYTES 20u  /* struct qrtr_ctrl_pkt: cmd + four words */

int nb_qrtr_init(NbQrtr *q, uint32_t local_node, NbQrtrSend send, void *send_context, uint8_t *tx, const NbQrtrEvents *events)
{
    if (!q || !send || !tx) return NbQrtrBad;
    *q = (NbQrtr){0};
    q->local_node = local_node;
    q->remote_node = NB_QRTR_NODE_BROADCAST;
    q->send = send;
    q->send_context = send_context;
    q->tx = tx;
    if (events) q->events = *events;
    return NbQrtrOk;
}

int nb_qrtr_parse_header(const uint8_t *packet, uint32_t length, NbQrtrHeader *h)
{
    if (!packet || !h || !length || (length & 3)) return NbQrtrBad;
    uint32_t size;
    if (packet[0] == 1) {
        if (length < NB_QRTR_V1_HEADER) return NbQrtrBad;
        h->version = 1; h->type = le32(packet + 4); h->src_node = le32(packet + 8); h->src_port = le32(packet + 12);
        h->confirm_rx = le32(packet + 16) != 0; size = le32(packet + 20); h->dst_node = le32(packet + 24);
        h->dst_port = le32(packet + 28); h->header = NB_QRTR_V1_HEADER;
    } else if (packet[0] == 3) {
        if (length < NB_QRTR_V2_HEADER) return NbQrtrBad;
        h->version = 2; h->type = packet[1]; h->confirm_rx = packet[2] & 1u; size = le32(packet + 4);
        h->src_node = le16(packet + 8); h->src_port = le16(packet + 10); h->dst_node = le16(packet + 12);
        h->dst_port = le16(packet + 14); h->header = NB_QRTR_V2_HEADER + packet[3];
        if (h->src_port == 0xffffu) h->src_port = NB_QRTR_PORT_CTRL;
        if (h->dst_port == 0xffffu) h->dst_port = NB_QRTR_PORT_CTRL;
    } else return NbQrtrBad;
    if (h->dst_port == NB_QRTR_PORT_CTRL_LEGACY) h->dst_port = NB_QRTR_PORT_CTRL;
    /* af_qrtr: !size || size > len || len != ALIGN(size, 4) + hdrlen */
    if (!size || size > length || h->header > length || length - h->header != ((size + 3u) & ~3u)) return NbQrtrBad;
    h->size = size;
    if ((h->type == NB_QRTR_NEW_SERVER || h->type == NB_QRTR_RESUME_TX) && size < CTRL_BYTES) return NbQrtrBad;
    if (h->dst_port != NB_QRTR_PORT_CTRL && h->type != NB_QRTR_DATA && h->type != NB_QRTR_RESUME_TX) return NbQrtrBad;
    return NbQrtrOk;
}

static int emit(NbQrtr *q, uint32_t type, uint32_t from_port, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length)
{
    const uint32_t padded = (length + 3u) & ~3u;
    if (padded < length || padded > NB_QRTR_MAX_PACKET - NB_QRTR_V1_HEADER) return NbQrtrTooBig;
    if (!q->up && type != NB_QRTR_HELLO) return NbQrtrDown;
    uint8_t *p = q->tx;
    put32(p, 1);
    put32(p + 4, type);
    put32(p + 8, q->local_node);
    put32(p + 12, from_port);
    put32(p + 16, 0);  /* confirm_rx: the AP sends few packets; the modem's flow control stays with the sender side */
    put32(p + 20, length);
    put32(p + 24, node);
    put32(p + 28, port);
    for (uint32_t i = 0; i < padded; ++i) p[NB_QRTR_V1_HEADER + i] = i < length ? data[i] : 0;
    if (q->send(q->send_context, p, NB_QRTR_V1_HEADER + padded)) { q->tx_refused++; return NbQrtrRefused; }
    q->tx_packets++;
    return NbQrtrOk;
}

static int control(NbQrtr *q, uint32_t command, uint32_t from_port, uint32_t node, uint32_t port, const uint32_t words[4])
{
    uint8_t pkt[CTRL_BYTES] = {0};
    put32(pkt, command);
    for (unsigned i = 0; words && i < 4; ++i) put32(pkt + 4 + 4 * i, words[i]);
    return emit(q, command, from_port, node, port, pkt, sizeof pkt);
}

static void announce(NbQrtr *q, unsigned i, uint32_t node)
{
    const uint32_t words[4] = { q->local[i].service, q->local[i].instance, q->local_node, q->local[i].port };
    control(q, NB_QRTR_NEW_SERVER, NB_QRTR_PORT_CTRL, node, NB_QRTR_PORT_CTRL, words);
}

int nb_qrtr_add_service(NbQrtr *q, uint32_t service, uint32_t instance, uint32_t port, NbQrtrReceive receive, void *context)
{
    if (!q || !receive || q->local_count == NB_QRTR_MAX_LOCAL || port == NB_QRTR_PORT_CTRL || !port) return NbQrtrFull;
    for (uint32_t i = 0; i < q->local_count; ++i) if (q->local[i].port == port) return NbQrtrBad;
    const unsigned i = q->local_count++;
    q->local[i].service = service; q->local[i].instance = instance; q->local[i].port = port;
    q->local[i].receive = receive; q->local[i].context = context;
    for (uint32_t n = 0; n < q->node_count; ++n) announce(q, i, q->nodes[n]);
    return NbQrtrOk;
}

static void forget_node(NbQrtr *q, uint32_t node)
{
    for (uint32_t i = q->remote_count; i > 0;) {
        --i;
        if (q->remote[i].node != node) continue;
        const uint32_t service = q->remote[i].service, instance = q->remote[i].instance, port = q->remote[i].port;
        q->remote[i] = q->remote[--q->remote_count];
        if (q->events.server_del) q->events.server_del(q->events.context, service, instance, node, port);
    }
    for (uint32_t i = 0; i < q->node_count; ++i) if (q->nodes[i] == node) {
        q->nodes[i] = q->nodes[--q->node_count];
        break;
    }
    if (q->events.client_gone) q->events.client_gone(q->events.context, node, NB_QRTR_NODE_BROADCAST);
    q->hello_received = q->node_count != 0;
    q->remote_node = q->node_count ? q->nodes[0] : NB_QRTR_NODE_BROADCAST;
}
static void forget_remote(NbQrtr *q)
{
    while (q->remote_count) forget_node(q, q->remote[q->remote_count-1].node);
    while (q->node_count) forget_node(q, q->nodes[q->node_count-1]);
}

void nb_qrtr_link(NbQrtr *q, int up)
{
    if (!q) return;
    if (!up) {
        if (q->up || q->hello_received) q->link_resets++;
        forget_remote(q);
        q->up = 0; q->hello_sent = 0; q->hello_received = 0;
        q->remote_node = NB_QRTR_NODE_BROADCAST;
        return;
    }
    if (q->up) return;
    q->up = 1;
    if (!control(q, NB_QRTR_HELLO, NB_QRTR_PORT_CTRL, NB_QRTR_NODE_BROADCAST, NB_QRTR_PORT_CTRL, 0)) q->hello_sent = 1;
}

int nb_qrtr_find_remote(const NbQrtr *q, uint32_t service, uint32_t instance)
{
    for (uint32_t i = 0; q && i < q->remote_count; ++i)
        if (q->remote[i].service == service && (instance == ~0u || q->remote[i].instance == instance)) return (int)i;
    return -1;
}

static void remote_server(NbQrtr *q, const uint8_t *w, int add)
{
    const uint32_t service = le32(w), instance = le32(w + 4), node = le32(w + 8), port = le32(w + 12);
    if (!service && !instance && !node && !port) return;  /* end of a listing */
    for (uint32_t i = 0; i < q->remote_count; ++i) {
        if (q->remote[i].node != node || q->remote[i].port != port) continue;
        if (add) {
            if (q->remote[i].service == service && q->remote[i].instance == instance) return;  /* repeated announce */
            if (q->events.server_del)
                q->events.server_del(q->events.context, q->remote[i].service, q->remote[i].instance, node, port);
            q->remote[i].service = service; q->remote[i].instance = instance;
            if (q->events.server_new) q->events.server_new(q->events.context, service, instance, node, port);
            return;
        }
        const uint32_t old_service = q->remote[i].service, old_instance = q->remote[i].instance;
        q->remote[i] = q->remote[--q->remote_count];
        if (q->events.server_del) q->events.server_del(q->events.context, old_service, old_instance, node, port);
        return;
    }
    if (!add) return;
    if (q->remote_count == NB_QRTR_MAX_REMOTE) { q->rx_dropped_remote++; return; }
    const uint32_t i = q->remote_count++;
    q->remote[i].service = service; q->remote[i].instance = instance; q->remote[i].node = node; q->remote[i].port = port;
    if (q->events.server_new) q->events.server_new(q->events.context, service, instance, node, port);
}

int nb_qrtr_receive(NbQrtr *q, const uint8_t *packet, uint32_t length)
{
    NbQrtrHeader h;
    if (!q || !q->up || length > NB_QRTR_MAX_PACKET || nb_qrtr_parse_header(packet, length, &h) ||
        (h.dst_node != q->local_node && h.dst_node != NB_QRTR_NODE_BROADCAST) || h.src_node == q->local_node ||
        h.src_node == NB_QRTR_NODE_BROADCAST) { if (q) q->rx_bad++; return NbQrtrBad; }
    const uint8_t *data = packet + h.header;
    q->rx_packets++;
    if (h.type == NB_QRTR_DATA) {
        q->rx_data++;
        unsigned i = 0;
        while (i < q->local_count && q->local[i].port != h.dst_port) ++i;
        if (i < q->local_count) q->local[i].receive(q->local[i].context, q, h.dst_port, h.src_node, h.src_port, data, h.size);
        else if (!q->events.unrouted || !q->events.unrouted(q->events.context, h.dst_port, h.src_node, h.src_port, data, h.size))
            q->rx_unrouted++;
        if (h.confirm_rx) {  /* the sender stops at its high watermark until this arrives */
            const uint32_t words[4] = { q->local_node, h.dst_port, 0, 0 };
            if (!control(q, NB_QRTR_RESUME_TX, h.dst_port, h.src_node, h.src_port, words)) q->resumes_sent++;
        }
        return NbQrtrOk;
    }
    if (h.type == NB_QRTR_RESUME_TX) return NbQrtrOk;  /* we never set confirm_rx */
    if (h.size < 4 || le32(data) != h.type) { q->rx_bad++; return NbQrtrBad; }
    switch (h.type) {
    case NB_QRTR_HELLO: {
        q->hellos++;
        uint32_t i = 0;
        while (i < q->node_count && q->nodes[i] != h.src_node) ++i;
        if (i == q->node_count) {
            if (i == NB_QRTR_MAX_NODES) { q->rx_dropped_remote++; return NbQrtrFull; }
            if (control(q, NB_QRTR_HELLO, NB_QRTR_PORT_CTRL, h.src_node, NB_QRTR_PORT_CTRL, 0)) return NbQrtrRefused;
            q->nodes[q->node_count++] = h.src_node;
        }
        q->remote_node = q->nodes[0];
        q->hello_received = 1;
        for (unsigned n = 0; n < q->local_count; ++n) announce(q, n, h.src_node);
        break;
    }
    case NB_QRTR_NEW_SERVER:
    case NB_QRTR_DEL_SERVER:
        if (h.size >= CTRL_BYTES) remote_server(q, data + 4, h.type == NB_QRTR_NEW_SERVER);
        break;
    case NB_QRTR_DEL_CLIENT:
        if (h.size >= 12 && q->events.client_gone) q->events.client_gone(q->events.context, le32(data + 4), le32(data + 8));
        break;
    case NB_QRTR_BYE:
        q->byes++;
        forget_node(q, h.src_node);
        break;
    default:  /* PING, EXIT, lookups: the modem's own name service covers its clients */
        break;
    }
    return NbQrtrOk;
}

int nb_qrtr_send_data(NbQrtr *q, uint32_t from_port, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length)
{
    if (!q || (!data && length) || !length) return NbQrtrBad;
    if (!q->hello_received) return NbQrtrDown;
    return emit(q, NB_QRTR_DATA, from_port, node, port, data, length);
}

int nb_qrtr_del_client(NbQrtr *q, uint32_t port)
{
    if (!q || !q->hello_received) return NbQrtrDown;
    const uint32_t words[4] = { q->local_node, port, 0, 0 };
    int result = NbQrtrOk;
    for (uint32_t i = 0; i < q->node_count; ++i) {
        int r = control(q, NB_QRTR_DEL_CLIENT, port, q->nodes[i], NB_QRTR_PORT_CTRL, words);
        if (r) result = r;
    }
    return result;
}
