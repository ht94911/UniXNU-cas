/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of the two transport rules the modem link takes from hardware evidence: GLINK remote intent ID 0 (accepted as
 * Linux does; the SM8850 SOCCP sends it, power27 e8d31f7) and SMP2P v1/v2 with the phone's smp2p.ko version choice
 * (qcom_smp2p_alloc_outbound_item: 1, or the remote's version when its item exists first). Not part of the driver. */
#define barrier glink_barrier   /* both sources define a static barrier() */
#include "../../package/baseband27/upstream/QcomGlink.c"
#undef barrier
#include "../../package/baseband27/upstream/QcomSmp2p.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static QcomGlink g;
static uint32_t th, tt, rh, rt;
static uint8_t txf[512], rxf[512], storage[64];
static QcomGlinkChannel *ch;
static void received(void *ctx, QcomGlinkChannel *c, const uint8_t *p, uint32_t n) { (void)ctx; (void)c; (void)p; (void)n; }
static void wr(uint8_t *p, uint32_t n) { p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 24); }
static void push(const uint8_t *m, uint32_t n) { for (uint32_t i = 0; i < n; ++i) { rxf[rh] = m[i]; rh = (rh + 1) % sizeof rxf; } }

static void glink_setup(void)
{
    th = tt = rh = rt = 0;
    memset(txf, 0, sizeof txf); memset(rxf, 0, sizeof rxf);
    QcomGlinkFifo a = {&th, &tt, txf, sizeof txf}, b = {&rh, &rt, rxf, sizeof rxf};
    qcom_glink_init(&g, a, b, 0, 0);
    QcomGlinkClient cb = {0, received};
    ch = qcom_glink_channel(&g, "IPCRTR", 64, 1, storage, &cb, 0, 0);
    assert(ch);
    g.version_sent = g.version_agreed = 1;
    ch->rcid = 7; ch->open_sent = ch->open_acked = ch->remote_open = ch->up = 1; ch->local[0].advertised = 1;
}

static void intent(uint32_t size, uint32_t id)
{
    uint8_t m[16] = {QCOM_GLINK_INTENT, 0, 7, 0};
    wr(m + 4, 1); wr(m + 8, size); wr(m + 12, id);
    push(m, sizeof m);
}

static void glink(void)
{
    glink_setup();
    intent(0x4000, 0);                                   /* ID 0 is a usable intent */
    assert(qcom_glink_poll(&g) >= 0);
    uint8_t body[100] = {1};
    const uint32_t at = th;
    assert(qcom_glink_send(&g, ch, body, sizeof body) == QcomGlinkOk);
    assert(txf[at] == QCOM_GLINK_TX_DATA && !txf[at + 4] && !txf[at + 5] && !txf[at + 6] && !txf[at + 7]);
    assert(qcom_glink_send(&g, ch, body, sizeof body) == QcomGlinkAgain);   /* busy until RX_DONE */
    uint8_t done[8] = {QCOM_GLINK_RX_DONE_W_REUSE, 0, 7, 0};
    wr(done + 4, 0);
    push(done, sizeof done);
    assert(qcom_glink_poll(&g) >= 0);
    tt = th;                                             /* the remote read our FIFO */
    assert(qcom_glink_send(&g, ch, body, sizeof body) == QcomGlinkOk);
    glink_setup();
    intent(0, 3);                                        /* a zero size is malformed */
    assert(qcom_glink_poll(&g) == QcomGlinkCorrupt);
    printf("PASS glink remote intent id 0\n");
}

static uint32_t out[QCOM_SMP2P_ITEM_BYTES / 4], in[QCOM_SMP2P_ITEM_BYTES / 4];
#define MODEM 1u
static void remote(uint32_t version, uint32_t flags)
{
    memset(in, 0, sizeof in);
    in[0] = QCOM_SMP2P_MAGIC;
    in[W_VERSION] = version | QCOM_SMP2P_FEATURE_SSR_ACK << 8;
    in[W_PIDS] = MODEM | 0u << 16;
    in[W_COUNTS] = QCOM_SMP2P_MAX_ENTRIES | 1u << 16;
    in[W_FLAGS] = flags;
    memcpy((void *)&in[W_ENTRY(0)], "slave-kernel", 13);
}
/* NbMss.c setup's choice */
static uint8_t choose(const uint32_t *item) { return item && !qcom_smp2p_check_in(item, 0, MODEM) && (item[1] & 0xff) > 1 ? (uint8_t)(item[1] & 0xff) : 1; }

static void smp2p(void)
{
    QcomSmp2p s;
    uint32_t v, changed;
    assert(choose(0) == 1);                              /* cold boot: the modem has no item yet */
    assert(!qcom_smp2p_init_out_version(&s, out, 0, MODEM, choose(0)) && (out[W_VERSION] & 0xff) == 1);
    remote(1, 0);
    s.in = in;
    in[W_ENTRY(0) + 4] = 1;
    assert(!qcom_smp2p_service(&s, "slave-kernel", &v, &changed) && s.negotiated && s.ssr_ack_enabled && v == 1 && changed == 1);

    remote(2, 0);                                        /* a v2 modem item made before ours: take its version */
    assert(choose(in) == 2);
    assert(!qcom_smp2p_init_out_version(&s, out, 0, MODEM, choose(in)) && (out[W_VERSION] & 0xff) == 2);
    s.in = in;
    in[W_ENTRY(0) + 4] = 1;
    assert(!qcom_smp2p_service(&s, "slave-kernel", &v, &changed) && s.negotiated && changed == 1);
    in[W_FLAGS] = QCOM_SMP2P_FLAG_RESTART_DONE;          /* v2 restart with the bit still set: reported again */
    assert(qcom_smp2p_service(&s, "slave-kernel", &v, &changed) == 1 && (out[W_FLAGS] & QCOM_SMP2P_FLAG_RESTART_ACK) && changed == 1);

    assert(qcom_smp2p_init_out_version(&s, out, 0, MODEM, 3) == -2);
    remote(3, 0);
    assert(qcom_smp2p_check_in(in, 0, MODEM) == -2 && choose(in) == 1);
    printf("PASS smp2p v1/v2 version choice\n");
}

int main(void)
{
    glink();
    smp2p();
    return 0;
}
