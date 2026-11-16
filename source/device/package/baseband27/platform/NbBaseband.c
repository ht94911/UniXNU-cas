/* SPDX-License-Identifier: BSD-2-Clause */
/* See NbBaseband.h. */
#include "NbBaseband.h"
#include "../services/NbServloc.h"

static int fail(NbBaseband *b, int error)
{
    b->stage = NbBasebandFailed;
    b->error = error;
    return error;
}

/* ---- QRTR over GLINK "IPCRTR": packets the channel cannot take yet wait here, in order ---- */

static int queue_packet(NbBaseband *b, const uint8_t *packet, uint32_t length)
{
    if (length > NB_TX_SLOT_BYTES || b->tx_count == NB_TX_QUEUE) { b->tx_dropped++; return NbQrtrFull; }
    const uint32_t at = (b->tx_head + b->tx_count) % NB_TX_QUEUE;
    uint8_t *slot = b->c.tx_queue + (uint64_t)at * NB_TX_SLOT_BYTES;
    for (uint32_t i = 0; i < length; ++i) slot[i] = packet[i];
    b->tx_length[at] = length;
    b->tx_count++;
    b->tx_queued++;
    return NbQrtrOk;
}

static void drain(NbBaseband *b)
{
    QcomGlink *g = nb_mss_glink(&b->mss);
    while (b->tx_count && g && b->ipcrtr) {
        const int r = qcom_glink_send(g, b->ipcrtr, b->c.tx_queue + (uint64_t)b->tx_head * NB_TX_SLOT_BYTES, b->tx_length[b->tx_head]);
        if (r == QcomGlinkAgain) return;
        if (r != QcomGlinkOk) b->tx_dropped++;
        b->tx_head = (b->tx_head + 1) % NB_TX_QUEUE;
        b->tx_count--;
    }
}

static int qrtr_send(void *context, const uint8_t *packet, uint32_t length)
{
    NbBaseband *b = context;
    QcomGlink *g = nb_mss_glink(&b->mss);
    if (!g || !b->ipcrtr || !b->link_up) return NbQrtrDown;
    if (!b->tx_count) {
        const int r = qcom_glink_send(g, b->ipcrtr, packet, length);
        if (r == QcomGlinkOk) return NbQrtrOk;
        if (r != QcomGlinkAgain) return r == QcomGlinkTooBig ? NbQrtrTooBig : NbQrtrDown;
    }
    return queue_packet(b, packet, length);
}

static void ipcrtr_state(void *context, QcomGlinkChannel *channel, int up)
{
    (void)channel;
    NbBaseband *b = context;
    b->link_up = (uint8_t)up;
    if (!up) { b->tx_head = 0; b->tx_count = 0; }
    nb_qrtr_link(&b->qrtr, up);
}

static void ipcrtr_receive(void *context, QcomGlinkChannel *channel, const uint8_t *data, uint32_t length)
{
    (void)channel;
    nb_qrtr_receive(&((NbBaseband *)context)->qrtr, data, length);
}

static void ds_state(void *context, QcomGlinkChannel *channel, int up) { (void)context; (void)channel; (void)up; }
static void ds_receive(void *context, QcomGlinkChannel *channel, const uint8_t *data, uint32_t length)
{
    (void)channel; (void)data; (void)length;
    ((NbBaseband *)context)->ds_packets++;
}

/* ---- QRTR events ---- */

static int unrouted(void *context, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    NbBaseband *b = context;
    if (nb_tftp_session_packet(&b->tftp, port, node, from, data, length)) return 1;
    if (nb_ipa_client_packet(port, node, from, data, length)) return 1;
    return b->c.bridge.unrouted ? b->c.bridge.unrouted(b->c.bridge.context, port, node, from, data, length) : 0;
}

static void server_new(void *context, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
    NbBaseband *b = context;
    nb_ipa_server_new(service, instance, node, port);
    if (b->c.bridge.server_new) b->c.bridge.server_new(b->c.bridge.context, service, instance, node, port);
}

static void server_del(void *context, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
    NbBaseband *b = context;
    if (b->c.bridge.server_del) b->c.bridge.server_del(b->c.bridge.context, service, instance, node, port);
}

static void client_gone(void *context, uint32_t node, uint32_t port) { nb_tftp_client_gone(&((NbBaseband *)context)->tftp, node, port); }

/* ---- rmtfs and the service locator answer in one packet ---- */

static void rmtfs_receive(void *context, NbQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    NbBaseband *b = context;
    const uint32_t n = nb_rmtfs_handle(b->c.rmtfs, data, length, b->reply, NB_REPLY_BYTES);
    if (n) nb_qrtr_send_data(q, port, node, from, b->reply, n);
}

static void servloc_receive(void *context, NbQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    NbBaseband *b = context;
    const uint32_t n = nb_servloc_answer(nb_nezha_domains, nb_nezha_domain_count, data, length, b->reply, NB_REPLY_BYTES);
    if (n) nb_qrtr_send_data(q, port, node, from, b->reply, n);
}

static int smp2p_ipa(void *context, uint32_t mask, uint32_t value)
{
    NbBaseband *b = context;
    return nb_mss_smp2p_set(&b->mss, b->mss.ipa_entry, mask, value);
}

int nb_baseband_init(NbBaseband *b, const NbBasebandConfig *c)
{
    if (!b || !c || !c->smem || !c->ipa_fws || !c->modem || !c->modem_dtb || !c->rmtfs || !c->platform.ipa_power || !c->platform.ipa_smmu ||
        !c->qrtr_tx || !c->tftp_packet || !c->tftp_files || !c->ipcrtr_intents || !c->ds_intents || !c->tx_queue) return NB_ARGUMENT;
    *b = (NbBaseband){0};
    b->c = *c;
    int r = nb_mss_init(&b->mss, c->smem, c->smem_bytes, c->modem, c->modem_dtb, &c->mss);
    if (r) return r;
    const NbQrtrEvents events = { b, unrouted, server_new, server_del, client_gone };
    if (nb_qrtr_init(&b->qrtr, 1, qrtr_send, b, c->qrtr_tx, &events)) return NB_ARGUMENT;
    if (nb_qrtr_add_service(&b->qrtr, NB_RMTFS_SERVICE, NB_RMTFS_INSTANCE, NB_PORT_RMTFS, rmtfs_receive, b) ||
        nb_qrtr_add_service(&b->qrtr, NB_SERVLOC_SERVICE, NB_SERVLOC_INSTANCE, NB_PORT_SERVLOC, servloc_receive, b)) return NB_ARGUMENT;
    if (nb_tftp_init(&b->tftp, &b->qrtr, NB_PORT_TFTP, NB_PORT_TFTP_SESSION, NB_TFTP_MAX_BLKSIZE, &c->tftp, c->tftp_packet, c->tftp_files)) return NB_ARGUMENT;
    if (nb_memshare_init(&b->memshare, &b->qrtr, NB_PORT_MEMSHARE, nb_nezha_memshare_clients, nb_nezha_memshare_client_count, &c->memshare)) return NB_ARGUMENT;
    b->stage = NbBasebandInitialStop;
    return NB_OK;
}

/* The channels must exist before the modem can open them: the SMEM items and the FIFOs are ready well before PAS authenticates. */
static void attach_glink(NbBaseband *b)
{
    QcomGlink *g = nb_mss_glink(&b->mss);
    if (!g || b->ipcrtr) return;
    const QcomGlinkClient ipcrtr = { ipcrtr_state, ipcrtr_receive }, ds = { ds_state, ds_receive };
    b->ipcrtr = qcom_glink_channel(g, "IPCRTR", NB_IPCRTR_INTENT_BYTES, NB_IPCRTR_INTENTS, b->c.ipcrtr_intents, &ipcrtr, b, 0);
    b->ds = qcom_glink_channel(g, "DS", NB_DS_INTENT_BYTES, NB_DS_INTENTS, b->c.ds_intents, &ds, b, 0);
}

static void service_online(NbBaseband *b, uint64_t now)
{
    if (b->mss.ipa_in_changed & b->mss.ipa_in & 1u) {
        b->mss.ipa_in_changed &= ~1u;
        nb_ipa_clock_query();
    }
    drain(b);
    nb_tftp_poll(&b->tftp, now);
    nb_memshare_poll(&b->memshare);
    nb_ipa_poll();
    drain(b);
}

int nb_baseband_poll(NbBaseband *b, uint64_t now)
{
    int r;
    if (!b) return NB_ARGUMENT;
    switch (b->stage) {
    case NbBasebandInitialStop:
        r = nb_mss_prepare_stop(&b->mss, now);
        if (r < 0) return fail(b, r);
        if (r == NB_OK) b->stage = NbBasebandPower;
        return NB_PENDING;
    case NbBasebandPower:
        r = b->c.platform.ipa_power(b->c.platform.context, 1);
        if (r < 0) return fail(b, r);
        if (r == NB_OK) b->stage = NbBasebandSmmu;
        return NB_PENDING;
    case NbBasebandSmmu:
        if (now >= b->mss.deadline) return fail(b, NB_TIMEOUT);
        r = b->c.platform.ipa_smmu(b->c.platform.context, 1, &b->mss,
                                  &b->c.ipa_memory, &b->c.ipa_smem_address);
        if (r < 0) return fail(b, r);
        if (r == NB_OK) {
            if (!b->c.ipa_smem_address) return fail(b, NB_OWNERSHIP);
            b->c.ipa_smem_bytes = NB_IPA_SMEM_BYTES;
            b->stage = NbBasebandConfigure;
        }
        return NB_PENDING;
    case NbBasebandConfigure: {
        const NbIpaBoard board = { &b->qrtr, NB_PORT_IPA_HOST, NB_PORT_IPA_CLIENT, b->c.platform.qmp_send, smp2p_ipa, b };
        nb_ipa_set_smem(b->c.ipa_smem_bytes, b->c.ipa_smem_address);
        r = nb_ipa_configure(&board, &b->c.ipa_memory);
        if (r) return fail(b, r);
        nb_pas_arm(b->c.ipa_fws, now);
        b->stage = NbBasebandFirmware;
        return NB_PENDING;
    }
    case NbBasebandFirmware:
        r = nb_pas_poll(b->c.ipa_fws, now);
        if (r < 0) return fail(b, r);
        if (r == NB_OK) b->stage = NbBasebandSetup;
        return NB_PENDING;
    case NbBasebandSetup:
        r = nb_ipa_setup();
        if (r) return fail(b, r);
        nb_ipa_modem_starting();
        b->stage = NbBasebandModem;
        return NB_PENDING;
    case NbBasebandModem:
        r = nb_mss_poll(&b->mss, now);
        attach_glink(b);
        if (r < 0) {
            if (b->reported_online && b->c.bridge.online) b->c.bridge.online(b->c.bridge.context, 0);
            nb_ipa_modem_down();
            b->stage = NbBasebandDown;
            b->error = r;
            return r;
        }
        service_online(b, now);
        if (r == NB_OK && !b->reported_online) {
            b->reported_online = 1;
            if (b->c.bridge.online) b->c.bridge.online(b->c.bridge.context, 1);
        }
        return r;
    case NbBasebandDown:
        return b->error;
    default:
        return b->error ? b->error : NB_IO;
    }
}

void nb_baseband_smp2p_event(NbBaseband *b) { if (b) nb_mss_smp2p_event(&b->mss); }
void nb_baseband_wdog_event(NbBaseband *b) { if (b) nb_mss_wdog_event(&b->mss); }
NbQrtr *nb_baseband_qrtr(NbBaseband *b) { return b ? &b->qrtr : 0; }
