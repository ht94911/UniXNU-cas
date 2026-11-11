/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Lifecycle structure from audio27 platform/AdspLink.c (BSD-2-Clause-Patent); modem specifics from Linux qcom_q6v5_pas.c
 * (sm8750_mpss_resource, qcom_pas_start/stop, qcom_pas_decrypt_shutdown) and qcom_q6v5.c (ready/handover/fatal/wdog,
 * request_stop), with the nezha values listed in NbMss.h. */
#include "NbMss.h"

#define START_MS 30000u       /* assignment, setup and proxy votes */
#define READY_MS 5000u        /* qcom_q6v5_wait_for_start */
#define STOP_ACK_MS 5000u     /* qcom_q6v5_request_stop */
#define DECRYPT_TRIES 50u     /* qcom_pas_decrypt_shutdown */
#define DECRYPT_PAUSE_MS 100u

static volatile uint8_t *item(NbMss *m, const QcomSmemPartition *part, uint16_t id, uint32_t minimum, uint32_t *bytes)
{
    uint32_t at = 0, n = 0;
    if (qcom_smem_find(&m->smem, part, id, &at, &n) || n < minimum || at > m->smem.size || n > m->smem.size - at || (at & 3))
        return 0;
    if (bytes) *bytes = n;
    return m->smem.base + at;
}

static int alloc_item(NbMss *m, uint16_t id, uint32_t bytes)
{
    int r = m->ops.smem_lock(m->ops.context);
    if (r) return r == NB_BUSY ? NB_PENDING : r;
    r = qcom_smem_alloc(&m->smem, &m->partition, id, bytes);
    m->ops.smem_unlock(m->ops.context);
    return r == QcomSmemOk ? NB_OK : r == QcomSmemExists ? NB_OWNERSHIP : NB_IO;
}

static void glink_kick(void *v) { NbMss *m = v; m->ops.doorbell(m->ops.context, NB_MSS_IPCC_GLINK); }

int nb_mss_init(NbMss *m, volatile void *smem, uint32_t bytes, NbPas *main_image, NbPas *dtb_image, const NbMssOps *ops)
{
    if (!m || !smem || ((uintptr_t)smem & 3) || bytes != 0x200000 || !main_image || !dtb_image ||
        main_image->pas != NB_MSS_PAS || dtb_image->pas != NB_MSS_DTB_PAS || main_image->stage != NbPasInit ||
        dtb_image->stage != NbPasInit || !ops || !ops->smem_lock || !ops->smem_unlock || !ops->doorbell || !ops->proxy ||
        !ops->assign) return NB_ARGUMENT;
    *m = (NbMss){0};
    m->smem = (QcomSmem){(volatile uint8_t *)smem, bytes};
    m->main_image = main_image;
    m->dtb_image = dtb_image;
    m->ops = *ops;
    m->stop_entry = m->ipa_entry = m->mailbox_entry = -1;
    if (qcom_smem_partition(&m->smem, QCOM_SMEM_HOST_APPS, NB_MSS_SMEM_HOST, &m->partition)) return NB_MISSING;
    m->stage = NbMssCheck;
    return NB_OK;
}

void nb_mss_smp2p_event(NbMss *m) { if (m) __atomic_fetch_add(&m->doorbells, 1, __ATOMIC_RELEASE); }
void nb_mss_wdog_event(NbMss *m) { if (m) __atomic_fetch_add(&m->wdog_events, 1, __ATOMIC_RELEASE); }

static int failed(NbMss *m, int r)
{
    m->error = r < 0 ? r : NB_IO;
    m->stage = NbMssFailed;
    /* PAS, carveouts, assignment and votes stay owned: recovery needs a PAS shutdown and an IOMMU/assignment audit. */
    return m->error;
}

/* Bounded copy of the SMEM crash reason (a C string the modem writes before it stops). */
static void crash_reason(NbMss *m)
{
    QcomSmemPartition global;
    uint32_t n = 0;
    volatile uint8_t *p = 0;
    if (!qcom_smem_partition(&m->smem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &global))
        p = item(m, &global, NB_MSS_CRASH_REASON, 1, &n);
    if (!p) p = item(m, &m->partition, NB_MSS_CRASH_REASON, 1, &n);
    uint32_t i = 0;
    for (; p && i < n && i + 1 < sizeof m->crash_reason && p[i]; ++i)
        m->crash_reason[i] = p[i] >= 0x20 && p[i] < 0x7f ? (char)p[i] : '?';
    m->crash_reason[i] = 0;
}

static int crashed(NbMss *m)
{
    crash_reason(m);
    m->stage = NbMssCrashed;
    m->error = NB_REMOTE;
    m->glink.broken = 1;  /* no more traffic; the owner reports the link down to QRTR */
    return NB_REMOTE;
}

/* A modem started earlier in this boot (SMEM is rebuilt by the boot loader on every boot) must not be adopted: its items
 * exist already. Missing items are the cold proof. */
static int check_cold(NbMss *m)
{
    if (item(m, &m->partition, NB_MSS_SMP2P_OUT, 4, 0) || item(m, &m->partition, NB_MSS_SMP2P_IN, 4, 0) ||
        item(m, &m->partition, NB_MSS_GLINK_DESC, 4, 0)) return NB_OWNERSHIP;
    return NB_OK;
}

static int setup(NbMss *m)
{
    int r = alloc_item(m, NB_MSS_SMP2P_OUT, QCOM_SMP2P_ITEM_BYTES);
    if (!r) r = alloc_item(m, NB_MSS_GLINK_DESC, 32);
    if (!r) r = alloc_item(m, NB_MSS_GLINK_TX, NB_MSS_GLINK_TX_BYTES);
    if (r) return r;
    uint32_t tx_bytes = 0;
    volatile uint8_t *out = item(m, &m->partition, NB_MSS_SMP2P_OUT, QCOM_SMP2P_ITEM_BYTES, 0);
    volatile uint8_t *desc = item(m, &m->partition, NB_MSS_GLINK_DESC, 32, 0);
    volatile uint8_t *tx = item(m, &m->partition, NB_MSS_GLINK_TX, NB_MSS_GLINK_TX_BYTES, &tx_bytes);
    if (!out || !desc || !tx || tx_bytes != NB_MSS_GLINK_TX_BYTES) return NB_WIRE;
    /* The phone's smp2p.ko (qcom_smp2p_alloc_outbound_item) publishes version 1, or the modem's own version when its inbound
     * item already exists and says more; it then waits for equal versions without rewriting ours. V1 and v2 share the layout. */
    volatile uint32_t *in = (volatile uint32_t *)item(m, &m->partition, NB_MSS_SMP2P_IN, QCOM_SMP2P_ITEM_BYTES, 0);
    const uint8_t version = in && !qcom_smp2p_check_in(in, 0, NB_MSS_SMEM_HOST) && (in[1] & 0xff) > 1 ? (uint8_t)(in[1] & 0xff) : 1;
    if (qcom_smp2p_init_out_version(&m->p2p, (volatile uint32_t *)out, 0, NB_MSS_SMEM_HOST, version)) return NB_WIRE;
    m->stop_entry = qcom_smp2p_add_entry(&m->p2p, "master-kernel");
    m->ipa_entry = qcom_smp2p_add_entry(&m->p2p, "ipa");
    m->mailbox_entry = qcom_smp2p_add_entry(&m->p2p, "smem-mailbox");
    if (m->stop_entry < 0 || m->ipa_entry < 0 || m->mailbox_entry < 0) return NB_WIRE;
    m->ops.doorbell(m->ops.context, NB_MSS_IPCC_SMP2P);
    /* glink_smem descriptor: tx tail, tx head, rx tail, rx head. */
    volatile uint32_t *d = (volatile uint32_t *)desc;
    const QcomGlinkFifo tf = {&d[1], &d[0], tx, tx_bytes}, rf = {&d[3], &d[2], 0, 0};
    if(m->ops.trace)m->ops.trace(m->ops.context,"glink-init",NB_PENDING);
    qcom_glink_init(&m->glink, tf, rf, glink_kick, m);
    if(m->ops.trace)m->ops.trace(m->ops.context,"glink-init",NB_OK);
    m->glink_ready = 1;
    return NB_OK;
}

/* Inbound SMP2P: attach once the modem made its item, answer feature/restart handshakes, track slave-kernel and ipa. */
static int smp2p(NbMss *m)
{
    if (!m->p2p.out) return NB_OK;
    if (!m->p2p.in) m->p2p.in = (volatile uint32_t *)item(m, &m->partition, NB_MSS_SMP2P_IN, QCOM_SMP2P_ITEM_BYTES, 0);
    if (!m->p2p.in) return NB_PENDING;
    if (qcom_smp2p_check_in(m->p2p.in, 0, NB_MSS_SMEM_HOST)) return NB_PENDING;
    uint32_t changed = 0;
    const int r = qcom_smp2p_service(&m->p2p, "slave-kernel", &m->slave, &changed);
    if (r < 0) return NB_WIRE;
    if (r) m->ops.doorbell(m->ops.context, NB_MSS_IPCC_SMP2P);
    const int ipa = qcom_smp2p_find_in(&m->p2p, "ipa");
    if (ipa >= 0) {
        const uint32_t v = qcom_smp2p_in_value(&m->p2p, ipa);
        m->ipa_in_changed |= v ^ m->ipa_in;
        m->ipa_in = v;
    }
    return NB_OK;
}

static int stopping(uint32_t stage) { return stage >= NbMssStopRequest && stage <= NbMssUnvote; }

int nb_mss_poll(NbMss *m, uint64_t now)
{
    if (!m || m->stage == NbMssCold || now > UINT64_MAX - START_MS) return NB_ARGUMENT;
    if (m->stage == NbMssFailed) return m->error;
    if (m->stage == NbMssCrashed) return NB_REMOTE;
    if (m->stage == NbMssStopped) return NB_OK;
    if (!m->deadline) m->deadline = now + START_MS;
    if (m->stage != NbMssOnline && m->stage != NbMssDtb && m->stage != NbMssMain && now >= m->deadline) {
        if (m->stage == NbMssStopWait) {  /* Linux logs the missing stop-ack and shuts down anyway */
            m->cleanup_error = NB_TIMEOUT;
            m->stage = NbMssShutdownMain;
        } else return failed(m, NB_TIMEOUT);
    }
    int r = NB_OK;
    switch (m->stage) {
    case NbMssCheck:
        r = check_cold(m);
        if (!r) m->stage = NbMssAssign;
        break;
    case NbMssAssign:
        r = m->ops.assign(m->ops.context, 1);
        if (!r) { m->assigned = 1; m->stage = NbMssSetup; }
        break;
    case NbMssSetup:
        r = setup(m);
        if (!r) m->stage = NbMssPower;
        break;
    case NbMssPower:
        r = m->ops.proxy(m->ops.context, 1);
        m->voted = 1;  /* a partial vote is still ours to remove */
        if (!r) { m->stage = NbMssDtb; nb_pas_arm(m->dtb_image, now); }
        break;
    case NbMssDtb:
        r = nb_pas_poll(m->dtb_image, now);
        if (!r) { m->stage = NbMssMain; nb_pas_arm(m->main_image, now); }
        break;
    case NbMssMain:
        /* The event baseline is taken right before auth_and_reset, so a stale ready bit cannot accept the image. */
        if (m->main_image->stage == NbPasAuthenticate) m->auth_doorbells = __atomic_load_n(&m->doorbells, __ATOMIC_ACQUIRE);
        r = nb_pas_poll(m->main_image, now);
        if (!r) { m->stage = NbMssWaitReady; m->deadline = now + READY_MS; }
        break;
    case NbMssWaitReady:
    case NbMssOnline:
        if (__atomic_load_n(&m->wdog_events, __ATOMIC_ACQUIRE)) return crashed(m);
        r = smp2p(m);
        if (r < 0) break;
        r = NB_OK;
        if (__atomic_load_n(&m->doorbells, __ATOMIC_ACQUIRE) != m->auth_doorbells) {
            if (m->slave & NB_MSS_BIT_FATAL) return crashed(m);
            if (m->slave & NB_MSS_BIT_READY) m->stage = NbMssOnline;
        }
        if (m->stage != NbMssOnline) break;
        if (!m->glink_rx_attached) {
            uint32_t n = 0;
            volatile uint8_t *rx = item(m, &m->partition, NB_MSS_GLINK_RX, 64, &n);
            if (rx) {
                if (n > 0x100000 || (n & 7)) return failed(m, NB_WIRE);
                if(m->ops.trace)m->ops.trace(m->ops.context,"glink-rx",NB_PENDING);
                qcom_glink_set_rx(&m->glink, rx, n);
                if(m->ops.trace)m->ops.trace(m->ops.context,"glink-rx",NB_OK);
                m->glink_rx_attached = 1;
            }
        }
        if ((m->slave & NB_MSS_BIT_HANDOVER) && !m->handed) {
            r = m->ops.proxy(m->ops.context, 0);
            if (!r) { m->handed = 1; m->voted = 0; }
            else if (r < 0) { m->cleanup_error = r; return failed(m, r); }
            else r = NB_OK;
        }
        if(!m->glink_polled && m->ops.trace)m->ops.trace(m->ops.context,"glink-first-poll",NB_PENDING);
        {
            int result=qcom_glink_poll(&m->glink);
            if(!m->glink_polled && m->ops.trace)m->ops.trace(m->ops.context,"glink-first-poll",result);
            m->glink_polled=1;
            if(result==QcomGlinkCorrupt)return failed(m,NB_WIRE);
        }
        break;
    case NbMssStopRequest:
        if (qcom_smp2p_update(&m->p2p, m->stop_entry, 1u, 1u)) m->ops.doorbell(m->ops.context, NB_MSS_IPCC_SMP2P);
        m->deadline = now + STOP_ACK_MS;
        m->stage = NbMssStopWait;
        break;
    case NbMssStopWait:
        r = smp2p(m);
        if (r < 0) break;
        r = NB_OK;
        /* Sometimes the stop request trips the watchdog instead of the ack (qcom_q6v5 wdog handler). */
        if ((m->slave & NB_MSS_BIT_STOP_ACK) || __atomic_load_n(&m->wdog_events, __ATOMIC_ACQUIRE)) {
            if (qcom_smp2p_update(&m->p2p, m->stop_entry, 1u, 0u)) m->ops.doorbell(m->ops.context, NB_MSS_IPCC_SMP2P);
            m->stage = NbMssShutdownMain;
        }
        break;
    case NbMssShutdownMain:
        if (now < m->retry_at) return NB_PENDING;
        r = m->main_image->ops.pas(m->main_image->ops.context, NB_PAS_SHUTDOWN, NB_MSS_PAS, 0, 0);
        if (r == NB_BUSY) return NB_PENDING;
        if (r == NB_PAS_EINVAL && ++m->decrypt_retries < DECRYPT_TRIES) {
            m->retry_at = now + DECRYPT_PAUSE_MS;
            m->deadline = now + START_MS;
            return NB_PENDING;
        }
        if (!r) { m->stage = NbMssShutdownDtb; m->deadline = now + START_MS; }
        break;
    case NbMssShutdownDtb:
        r = m->dtb_image->ops.pas(m->dtb_image->ops.context, NB_PAS_SHUTDOWN, NB_MSS_DTB_PAS, 0, 0);
        if (r == NB_BUSY) return NB_PENDING;
        if (!r) m->stage = NbMssUnassign;
        break;
    case NbMssUnassign:
        /* Only after both PAS shutdowns succeeded: the modem can no longer reach DSM. */
        r = m->assigned ? m->ops.assign(m->ops.context, 0) : NB_OK;
        if (!r) { m->assigned = 0; m->stage = NbMssUnvote; }
        break;
    case NbMssUnvote:
        r = m->voted ? m->ops.proxy(m->ops.context, 0) : NB_OK;
        if (!r) { m->voted = 0; m->stage = NbMssStopped; }
        break;
    default:
        return failed(m, NB_OWNERSHIP);
    }
    if (r == NB_BUSY) r = NB_PENDING;
    if (r < 0) {
        if (stopping(m->stage)) m->cleanup_error = r;
        return failed(m, r);
    }
    if (m->stage == NbMssStopped) return NB_OK;
    return m->stage == NbMssOnline ? NB_OK : NB_PENDING;
}

int nb_mss_stop(NbMss *m, uint64_t now)
{
    if (!m || m->stage != NbMssOnline || now > UINT64_MAX - STOP_ACK_MS) return NB_ARGUMENT;
    m->glink.broken = 1;
    m->stage = NbMssStopRequest;
    m->deadline = now + STOP_ACK_MS;
    return NB_PENDING;
}

QcomGlink *nb_mss_glink(NbMss *m) { return m && m->glink_ready ? &m->glink : 0; }

int nb_mss_smp2p_set(NbMss *m, int entry, uint32_t mask, uint32_t value)
{
    if (!m || !m->p2p.out || entry < 0 || entry == m->stop_entry || m->stage == NbMssFailed) return NB_ARGUMENT;
    if (qcom_smp2p_update(&m->p2p, entry, mask, value)) m->ops.doorbell(m->ops.context, NB_MSS_IPCC_SMP2P);
    return NB_OK;
}
