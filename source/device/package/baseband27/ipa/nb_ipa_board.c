/* SPDX-License-Identifier: BSD-2-Clause */
/* See nb_ipa_board.h. One translation unit with the command, RX and modem files, as coachz_modem.c does. */
#include "nb_ipa55_cmd.c"
#include "nb_ipa55_rx.c"
#include "nb_ipa55_modem.c"
#include "nb_ipa_board.h"

#define NB_CMD_OFFSET 0xc000u
#define NB_CMD_BYTES 0x4000u
#define NB_SMP2P_RSP_COMPLETE (1u << 0)   /* IPA_SMP2P_OUT_CLK_RSP_CMPLT_IDX */
#define NB_SMP2P_CLOCK_VOTE (1u << 1)     /* IPA_SMP2P_OUT_CLK_VOTE_IDX */
#define NB_FIRST_MUX 1u
#define NB_LAST_MUX 4u

enum { NB_OFF, NB_CONFIGURED, NB_SET_UP };

static struct coachz_ipa nb_ipa;
static struct coachz_gsi nb_gsi;
static struct coachz_ipa_cmd nb_cmd;
static struct coachz_ipa_rx nb_lan, nb_modem_rx;
static struct coachz_ipa_modem nb_modem;
static struct ci_handshake nb_hs;
static NbIpaBoard nb_board;
static NbIpaMemory nb_memory;
static int nb_state, nb_modem_open, nb_clock_answered;
static uint32_t nb_modem_node, nb_modem_port, nb_peer_node, nb_peer_port;
static int nb_modem_known, nb_peer_known, nb_init_driver_sent;
static uint32_t nb_smem_size, nb_per_stats_addr;
static uint16_t nb_txn;
static void (*nb_data_rx)(uint32_t, const uint8_t *, uint32_t, uint32_t);

static void nb_data_deliver(void *context, uint32_t mux, const uint8_t *ip, uint32_t length, uint32_t ip_version)
{
    (void)context;
    if (nb_data_rx) nb_data_rx(mux, ip, length, ip_version);
}
void nb_ipa_data_receiver(void (*receiver)(uint32_t, const uint8_t *, uint32_t, uint32_t)) { nb_data_rx = receiver; }
int nb_ipa_data_ready(void) { return nb_modem_open; }
int nb_ipa_data_send(uint32_t mux, const uint8_t *ip, uint32_t length)
{
    if (!nb_modem_open) return ENETDOWN;
    const int result = coachz_ipa_modem_xmit(&nb_modem, mux, ip, length);
    return result < 0 ? -result : result;
}

int nb_ipa_configure(const NbIpaBoard *board, const NbIpaMemory *memory)
{
    const NbIpaDma *ring = &memory->ring;
    if (!board || !board->qrtr || !board->smp2p_set || ring->bytes < NB_CMD_OFFSET + NB_CMD_BYTES) return -EINVAL;
    nb_board = *board;
    nb_memory = *memory;
    int err = coachz_ipa_config(&nb_ipa, memory->zero.virt, memory->zero.iova, board->qmp_send);
    if (!err) err = coachz_gsi_init(&nb_gsi, nb_ipa.gsi, ring->virt, ring->iova, NB_CMD_OFFSET);
    if (!err) err = coachz_ipa_cmd_init(&nb_cmd, &nb_ipa, &nb_gsi, (uint8_t *)ring->virt + NB_CMD_OFFSET, ring->iova + NB_CMD_OFFSET, NB_CMD_BYTES);
    if (!err) err = coachz_ipa_rx_init(&nb_lan, &nb_ipa, &nb_gsi, CIPA_EP_AP_LAN_RX, memory->lan_rx.virt, memory->lan_rx.iova, memory->lan_rx.bytes);
    if (err) {
        printf("COACHZ ipa: configuration failed (%d), cellular data stays off\n", err);
        return err;
    }
    nb_state = NB_CONFIGURED;
    return 0;
}

void nb_ipa_modem_starting(void)
{
    if (nb_state != NB_OFF) coachz_ipa_modem_starting(&nb_ipa);
}

void nb_ipa_set_smem(uint32_t size, uint32_t per_stats_addr)
{
    nb_smem_size = size;
    nb_per_stats_addr = per_stats_addr;
}

static uint32_t nb_channel_state(uint32_t id)
{
    return cgs_state(gsi_rd(&nb_gsi, CGSI_CH_C_CNTXT_0 + id * CGSI_CH_C_CNTXT_0_STRIDE));
}

/* ipa_modem_start and opening rmnet_ipa0. */
static void nb_modem_start(void)
{
    const NbIpaDma *rx = &nb_memory.modem_rx, *tx = &nb_memory.modem_tx;
    int err = coachz_ipa_rx_init(&nb_modem_rx, &nb_ipa, &nb_gsi, CIPA_EP_AP_MODEM_RX, rx->virt, rx->iova, rx->bytes);
    if (!err) err = coachz_ipa_modem_init(&nb_modem, &nb_ipa, &nb_gsi, &nb_modem_rx, tx->virt, tx->iova, tx->bytes);
    if (!err) err = coachz_ipa_modem_start(&nb_modem);
    if (!err) err = coachz_ipa_modem_open(&nb_modem);
    if (err) {
        printf("COACHZ modem: data path failed (%d), the modem goes on without it\n", err);
        return;
    }
    for (uint32_t mux = NB_FIRST_MUX; mux <= NB_LAST_MUX; mux++) coachz_ipa_modem_mux_add(&nb_modem, mux);
    nb_modem.mux_deliver = nb_data_deliver;
    nb_modem_open = 1;
    printf("COACHZ modem: channel states TX %u:%u RX %u:%u\n", nb_modem.tx_channel, nb_channel_state(nb_modem.tx_channel),
           nb_modem_rx.channel, nb_channel_state(nb_modem_rx.channel));
}

static void nb_step(void)
{
    for (;;) {
        const struct ci_step step = ci_ready_step(&nb_hs);
        if (step.send_init_complete) {
            uint8_t message[32];
            const size_t length = ci_init_complete_ind(message, sizeof message, ++nb_txn);
            const int sent = nb_peer_known && length &&
                !nb_qrtr_send_data(nb_board.qrtr, nb_board.host_port, nb_peer_node, nb_peer_port, message, (uint32_t)length);
            printf("COACHZ ipa: INIT_COMPLETE %s\n", sent ? "sent" : "not sent");
            if (!sent) return;
            ci_indication_sent(&nb_hs);
            continue;
        }
        if (step.start_modem) {
            printf("COACHZ ipa: handshake complete, starting the modem endpoints\n");
            nb_modem_start();
        }
        return;
    }
}

static void nb_init_driver(void)
{
    uint8_t message[CI_INIT_DRIVER_REQ_MAX];
    if (nb_state != NB_SET_UP || !nb_modem_known || nb_init_driver_sent) return;
    const size_t length = ci_init_driver_req(message, sizeof message, ++nb_txn, nb_ipa.mem_offset, nb_ipa.uc_loaded, nb_smem_size, nb_per_stats_addr);
    const int result = length ? nb_qrtr_send_data(nb_board.qrtr, nb_board.client_port, nb_modem_node, nb_modem_port, message, (uint32_t)length) : -1;
    nb_init_driver_sent = !result;
    printf("COACHZ ipa: INIT_DRIVER %u bytes to %u:0x%x, mem_offset 0x%x, uc_loaded %d, send %d\n", (unsigned)length, nb_modem_node,
           nb_modem_port, nb_ipa.mem_offset, nb_ipa.uc_loaded, result);
}

/* A QMI message on one of the two ports: the client's (INIT_DRIVER's answer) or the server's (the modem's requests). */
static void nb_qmi(int client, uint32_t node, uint32_t port, const uint8_t *body, uint32_t length)
{
    struct ci_msg m;
    if (!ci_parse(body, length, &m)) {
        printf("COACHZ ipa: bad QMI message of %u bytes from %u:0x%x\n", length, node, port);
        return;
    }
    if (client) {
        struct ci_init_driver_rsp r;
        if (!ci_init_driver_rsp_parse(&m, &r)) {
            printf("COACHZ ipa: QMI type %u message 0x%x on the client port\n", m.type, m.id);
            return;
        }
        printf("COACHZ ipa: INIT_DRIVER answer: ok %d error %u, ctrl %d/%u default %d/%u pending %d/%u\n", r.ok, r.error, r.has_ctrl,
               r.ctrl_end_pt, r.has_default, r.default_end_pt, r.has_pending, r.pending);
        if (r.ok) ci_on_init_driver_success(&nb_hs);
    } else {
        uint8_t message[32];
        const int known = m.type == CI_QMI_REQUEST && (m.id == CI_INDICATION_REGISTER || m.id == CI_DRIVER_INIT_COMPLETE);
        printf("COACHZ ipa: QMI type %u message 0x%x from %u:0x%x%s\n", m.type, m.id, node, port, known ? "" : ", not handled");
        if (!known) return;
        nb_peer_node = node;
        nb_peer_port = port;
        nb_peer_known = 1;
        const size_t size = ci_success_rsp(message, sizeof message, m.txn, m.id);
        if (!size || nb_qrtr_send_data(nb_board.qrtr, nb_board.host_port, node, port, message, (uint32_t)size)) {
            printf("COACHZ ipa: response to 0x%x not sent\n", m.id);
            return;
        }
        if (m.id == CI_INDICATION_REGISTER) ci_on_indication_register_answered(&nb_hs);
        else ci_on_driver_init_complete_answered(&nb_hs);
    }
    nb_step();
}

static void nb_host_receive(void *context, NbQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    (void)context; (void)q; (void)port;
    nb_qmi(0, node, from, data, length);
}

int nb_ipa_client_packet(uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    if (port != nb_board.client_port) return 0;
    if (nb_state == NB_SET_UP) nb_qmi(1, node, from, data, length);
    return 1;
}

/* The rest of ipa_setup, ending where mainline calls ipa_qmi_setup. */
int nb_ipa_setup(void)
{
    if (nb_state != NB_CONFIGURED) {
        printf("COACHZ ipa: setup, but the IPA is not configured\n");
        return -EINVAL;
    }
    int err = coachz_gsi_setup(&nb_gsi);
    if (!err) err = coachz_ipa_setup_commands(&nb_cmd);
    if (!err) err = coachz_ipa_setup_lan_rx(&nb_lan);
    if (!err && nb_qrtr_add_service(nb_board.qrtr, CI_IPA_SERVICE, CI_QRTR_INSTANCE(CI_HOST_INSTANCE), nb_board.host_port, nb_host_receive, 0)) err = -ENOMEM;
    if (err) {
        printf("COACHZ ipa: setup failed (%d), cellular data stays off\n", err);
        nb_state = NB_OFF;
        return err;
    }
    nb_state = NB_SET_UP;
    ci_setup(&nb_hs);
    nb_init_driver();
    return 0;
}

void nb_ipa_server_new(uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
    if (!ci_is_modem_service(service, instance)) return;
    nb_modem_node = node;
    nb_modem_port = port;
    nb_modem_known = 1;
    nb_init_driver();
}

void nb_ipa_clock_query(void)
{
    if (nb_clock_answered || nb_state == NB_OFF) return;
    /* The clock stays on while the modem runs, so the answer is always "on". */
    if (!nb_board.smp2p_set(nb_board.context, NB_SMP2P_RSP_COMPLETE | NB_SMP2P_CLOCK_VOTE, NB_SMP2P_RSP_COMPLETE | NB_SMP2P_CLOCK_VOTE))
        nb_clock_answered = 1;
    printf("COACHZ ipa: modem clock query answered %d\n", nb_clock_answered);
}

/* gsi_channel_update checks EV_CH_E_CNTXT_4 for completed transactions; polled, so consult the ring when no IEOB was seen. */
static int nb_event_ready(uint32_t channel)
{
    const struct cgs_channel *c = &nb_gsi.t.ch[channel];
    const uint32_t ring = c->evt_ring;
    const uint32_t hw = *(volatile uint32_t *)(nb_gsi.virt + CGSI_EV_CH_E_CNTXT_4 + ring * CGSI_EV_CH_E_CNTXT_4_STRIDE);
    const uint32_t base = (uint32_t)(nb_gsi.ring_phys + nb_gsi.place.evt_offset[ring]);
    const uint32_t index = (hw - base) / CGS_ELEMENT;
    return (nb_gsi.ieob_pending & (1u << ring)) || index != nb_gsi.evt_index[ring] % c->event_count;
}

void nb_ipa_poll(void)
{
    if (nb_state == NB_OFF) return;
    coachz_ipa_interrupt(&nb_ipa);
    if (nb_state != NB_SET_UP) return;
    coachz_gsi_interrupt(&nb_gsi);
    if (nb_event_ready(nb_lan.channel)) {
        nb_gsi.ieob_pending &= ~(1u << nb_lan.evt_ring);
        coachz_ipa_rx_poll(&nb_lan);
    }
    if (!nb_modem_open) return;
    if (nb_event_ready(nb_modem_rx.channel)) {
        nb_gsi.ieob_pending &= ~(1u << nb_modem_rx.evt_ring);
        coachz_ipa_rx_poll(&nb_modem_rx);
    }
    if (nb_event_ready(nb_modem.tx_channel)) {
        nb_gsi.ieob_pending &= ~(1u << nb_modem.tx_evt_ring);
        coachz_ipa_modem_tx_poll(&nb_modem);
    }
}

void nb_ipa_modem_down(void)
{
    nb_modem_open = 0;
    nb_state = NB_OFF;
    printf("COACHZ ipa: modem down, cellular data off until the next boot\n");
}

void nb_ipa_report(unsigned second)
{
    if (nb_state != NB_SET_UP) return;
    printf("COACHZ ipa: %u s: uc_loaded %d modem_ready %d uc_ready %d indication %d/%d, LAN RX packets %llu delivered %llu dropped %llu\n",
           second, nb_ipa.uc_loaded, nb_hs.modem_ready, nb_hs.uc_ready, nb_hs.indication_requested, nb_hs.indication_sent,
           nb_lan.packets, nb_lan.delivered, nb_lan.dropped);
    if (nb_modem_open)
        printf("COACHZ modem: %u s: modem RX packets %llu delivered %llu dropped %llu, TX packets %llu\n", second, nb_modem_rx.packets,
               nb_modem_rx.delivered, nb_modem_rx.dropped, nb_modem.tx_packets);
}
