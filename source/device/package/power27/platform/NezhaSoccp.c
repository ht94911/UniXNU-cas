/* SPDX-License-Identifier: APSL-2.0 */
/* Reuse of ShueiTsuki/data-a-live's SOCCP attachment, independently from ADSP
 * audio startup. Protocol cores retain UniXNU provenance with local hardening.
 * Poll SRAM only; never drain IPCC RECV_ID or acknowledge other clients. */
#include "NezhaSoccp.h"
#include <string.h>

#define REMOTE 19u
#define P2P_IN 617u
#define P2P_OUT 616u
#define DESC 478u
#define TX 479u
#define RX 480u
#define FIFO_BYTES 0x4000u

static volatile uint8_t *item(NpSoccp *s, uint16_t id, uint32_t *bytes)
{
    uint32_t offset = 0, n = 0;
    if (qcom_smem_find(&s->smem, &s->partition, id, &offset, &n) ||
        offset > s->smem.size || n > s->smem.size - offset) return 0;
    if (bytes) *bytes = n;
    return s->smem.base + offset;
}
static int alloc(NpSoccp *s, uint16_t id, uint32_t bytes)
{
    if (!s->io.smem_lock(s->io.context)) return -1;
    int r = qcom_smem_alloc(&s->smem, &s->partition, id, bytes);
    s->io.smem_unlock(s->io.context);
    return r == QcomSmemExists ? 0 : r;
}
static void kick(void *v)
{ NpSoccp *s = v; s->io.doorbell(s->io.context, 13, 0); }
static void state(void *v, QcomGlinkChannel *channel, int up)
{
    NpSoccp *s = v; (void)channel;
    if (s->up == !!up) return;
    s->up = !!up;
    if (++s->epoch == 0) { s->broken = 1; s->up = 0; }
    s->io.link(s->io.context, s->up, s->epoch);
}
static void receive(void *v, QcomGlinkChannel *channel, const uint8_t *p, uint32_t n)
{
    NpSoccp *s = v; (void)channel;
    if (s->active && s->up) s->io.receive(s->io.context, s->epoch, p, n);
}
static int start_link(NpSoccp *s)
{
    uint32_t ds = 0, ts = 0, rs = 0;
    if (alloc(s, DESC, 32) || alloc(s, TX, FIFO_BYTES)) return -1;
    volatile uint32_t *d = (volatile uint32_t *)item(s, DESC, &ds);
    volatile uint8_t *tx = item(s, TX, &ts), *rx = item(s, RX, &rs);
    if (!d || !tx || !rx || ds < 32 || ts != FIFO_BYTES || rs != FIFO_BYTES) return -1;
    /* Preserve all handoff indices. The resource owner must prove bootloader
     * and other AP clients relinquished this edge before attachment. */
    uint32_t tail = d[0], head = d[1], rtail = d[2], rhead = d[3];
    if (tail >= ts || head >= ts || rtail >= rs || rhead >= rs || ((tail | head | rtail | rhead) & 7)) return -1;
    QcomGlinkFifo tf = {&d[1], &d[0], tx, ts}, rf = {&d[3], &d[2], rx, rs};
    qcom_glink_attach(&s->glink, tf, rf, kick, s);
    const QcomGlinkClient client = {state, receive};
    s->channel = qcom_glink_channel(&s->glink, "PMIC_RTR_SOCCP_APPS", 1024, 8,
                                     s->intents, &client, s, 0);
    if (!s->channel) return -1;
    uint64_t now = s->io.now_ms(s->io.context);
    if (now > UINT64_MAX - 5000) return -1;
    s->open_deadline = now + 5000;
    return 0;
}
int np_soccp_attach(NpSoccp *s, volatile uint8_t *base, uint32_t bytes, const NpSoccpIo *io)
{
    if (!s || !base || bytes != 0x200000 || !io || !io->smem_lock || !io->smem_unlock ||
        !io->doorbell || !io->now_ms || !io->link || !io->receive || s->attached) return -1;
    memset(s, 0, sizeof(*s)); s->io = *io; s->smem.base = base; s->smem.size = bytes;
    if (qcom_smem_partition(&s->smem, 0, REMOTE, &s->partition)) return -1;
    uint32_t size = 0;
    s->smp2p.in = (volatile uint32_t *)item(s, P2P_IN, &size);
    if (!s->smp2p.in || size < QCOM_SMP2P_ITEM_BYTES || qcom_smp2p_check_in(s->smp2p.in, 0, REMOTE)) return -1;
    int in = qcom_smp2p_find_in(&s->smp2p, "slave-kernel");
    if (in < 0) return -1;
    s->slave = qcom_smp2p_in_value(&s->smp2p, in);
    if ((s->slave & 3) != 2) return -1; /* ready, no fatal; do not boot/reset it */
    s->restart_flag = s->smp2p.in[4] & QCOM_SMP2P_FLAG_RESTART_DONE;
    if (alloc(s, P2P_OUT, QCOM_SMP2P_ITEM_BYTES)) return -1;
    volatile uint32_t *out = (volatile uint32_t *)item(s, P2P_OUT, &size);
    if (!out || size < QCOM_SMP2P_ITEM_BYTES) return -1;
    volatile uint32_t *saved_in = s->smp2p.in;
    qcom_smp2p_init_out(&s->smp2p, out, 0, REMOTE); s->smp2p.in = saved_in;
    s->out_entry = qcom_smp2p_add_entry(&s->smp2p, "master-kernel");
    if (s->out_entry < 0) return -1;
    s->io.doorbell(s->io.context, 13, 2);
    if (start_link(s)) return -1;
    s->attached = s->active = 1;
    return 0;
}
static int refresh_partition(NpSoccp *s)
{
    if (qcom_smem_partition(&s->smem, 0, REMOTE, &s->partition)) return -1;
    uint32_t n = 0;
    volatile uint32_t *in = (volatile uint32_t *)item(s, P2P_IN, &n);
    if (!in || n < QCOM_SMP2P_ITEM_BYTES || qcom_smp2p_check_in(in, 0, REMOTE)) return -1;
    volatile uint32_t *out = (volatile uint32_t *)item(s, P2P_OUT, &n);
    if (!out || n < QCOM_SMP2P_ITEM_BYTES) return -1;
    if (in != s->smp2p.in || out != s->smp2p.out) {
        /* Remapping a live edge is not a restart certificate. Require the
         * already observed down/restart transition before writing new OUT. */
        if (!s->saw_remote_down && !s->restart_seen) return -1;
        qcom_smp2p_init_out(&s->smp2p, out, 0, REMOTE);
        s->out_entry = qcom_smp2p_add_entry(&s->smp2p, "master-kernel");
        if (s->out_entry < 0) return -1;
        s->io.doorbell(s->io.context, 13, 2);
    }
    s->smp2p.in = in;
    return 0;
}
int np_soccp_poll(NpSoccp *s)
{
    if (!s || !s->attached || !s->active) return -1;
    uint32_t value = 0, changed = 0;
    if (s->broken && refresh_partition(s)) return -1;
    if (qcom_smp2p_check_in(s->smp2p.in, 0, REMOTE)) {
        goto broken; /* a corrupt header alone is not a restart certificate */
    }
    uint32_t restart = s->smp2p.in[4] & QCOM_SMP2P_FLAG_RESTART_DONE;
    if (restart != s->restart_flag) {
        s->restart_seen = 1; s->broken = 1; s->restart_flag = restart;
        state(s, s->channel, 0);
    }
    if (qcom_smp2p_service(&s->smp2p, "slave-kernel", &value, &changed)) s->io.doorbell(s->io.context, 13, 2);
    s->slave = value;
    if ((value & 3) != 2) { s->saw_remote_down = 1; goto broken; }
    if (s->broken) {
        /* Rebuild only after an observed remote reset/down->ready transition.
         * A corrupt FIFO by itself is not permission to discard live traffic.
         * SOCCP is restarted by its own platform owner, never by this driver. */
        uint64_t now = s->io.now_ms(s->io.context);
        if ((!s->saw_remote_down && !s->restart_seen) || now < s->retry_at || s->retries >= 3) return -1;
        ++s->retries;
        if (now > UINT64_MAX - 1000) return -1;
        s->retry_at = now + 1000;
        if (refresh_partition(s) || start_link(s)) return -1;
        s->broken = s->saw_remote_down = s->restart_seen = 0;
        s->smp2p.negotiated = 0;
    }
    if (!s->up && s->glink.version_agreed && !s->channel->remote_open && s->open_deadline &&
        s->io.now_ms(s->io.context) >= s->open_deadline) {
        s->channel->initiator = 1; s->channel->open_sent = 0; s->open_deadline = 0;
    }
    if (qcom_glink_poll(&s->glink) < 0) goto broken;
    if (s->up) s->retries = 0;
    return 0;
broken:
    s->broken = 1; state(s, s->channel, 0); return -1;
}
int np_soccp_send(NpSoccp *s, const uint8_t *p, uint32_t n)
{
    if (!s || !s->active || !s->up || s->broken || !p || n != NP_MCA_REQUEST || np_le32(p) != NP_MCA_OWNER ||
        np_le32(p + 4) != 1 || np_le32(p + 8) != NP_MCA_READ)
        return QcomGlinkNotOpen;
    return qcom_glink_send(&s->glink, s->channel, p, n);
}
void np_soccp_quiesce(NpSoccp *s)
{
    if (s) { s->active = 0; state(s, s->channel, 0); }
    /* Intent storage stays alive with its owner. There is no upstream close
     * API; do not free remotely visible buffers or pretend it acknowledged. */
}
int np_soccp_resume(NpSoccp *s)
{
    if (!s || !s->attached) return -1;
    s->active = 1;
    if (np_soccp_poll(s)) return -1;
    if (s->channel->up) state(s, s->channel, 1);
    return 0;
}
