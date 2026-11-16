/* SPDX-License-Identifier: GPL-2.0-only */
/* Peach v2 tcl_data_cmd/wbm2sw_completion_ring_tx, hal_be_tx.h and
 * dp_be_tx.c. One nonfragmented Ethernet queue, no checksum/TSO offload. */
#include "PeachTx.h"
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <string.h>
#endif
#define TX_RBM 5u /* Peach TCL0 -> WBM2SW0; WBM2SW1 is firmware-owned. */
#define TTRY(t, expr) do { int err_ = (expr); if (err_) { (t)->error = err_; return err_; } } while (0)
static int invalid(PeachTx *t) { return t->error = PeachCeProtocol; }
static uint64_t now(PeachTx *t) { return t->htt->htc->ops.now_us(t->htt->htc->ops.context); }
int peach_tx_prepare(PeachTx *t, PeachSrng rings[PeachSrngKinds], PeachHtt *h,
                     PeachMhiDma memory, unsigned lmac, unsigned pdev)
{
    if (!t || !rings || !h || !h->htc || !h->htc->started || h->metadata_version != 1 ||
        lmac > 3 || pdev > 3 || !memory.bytes || !memory.iova ||
        ((uintptr_t)memory.bytes & 127) || (memory.iova & 127) ||
        memory.capacity < PEACH_TX_DMA_BYTES || memory.iova >= 0x100000000ull ||
        PEACH_TX_DMA_BYTES > 0x100000000ull - memory.iova) return PeachMhiInvalid;
    for (unsigned i = 0; i < PeachSrngKinds; i++) {
        /* RX rings may already be enabled before WMI INIT. */
        if (!rings[i].prepared || rings[i].error ||
            (rings[i].started && (i == PeachSrngTclTx || i == PeachSrngTxCompletion)))
            return PeachMhiInvalid;
        PeachMhiDma d = rings[i].memory;
        if (d.iova < memory.iova + PEACH_TX_DMA_BYTES && memory.iova < d.iova + d.capacity)
            return PeachMhiInvalid;
    }
    memset(t, 0, sizeof(*t));
    t->ring = &rings[PeachSrngTclTx]; t->completion = &rings[PeachSrngTxCompletion];
    t->htt = h; t->memory = memory; t->lmac = lmac; t->pdev = pdev;
    PeachSrngOps *ops = &t->ring->ops;
    TTRY(t, ops->health(ops->context));
    memset(memory.bytes, 0, PEACH_TX_DMA_BYTES); ops->barrier(ops->context);
    t->prepared = 1;
    return 0;
}
int peach_tx_start(PeachTx *t)
{
    if (!t || !t->prepared || t->started || !t->htt->confirmed ||
        t->htt->metadata_version != 1 || !t->ring->started || !t->completion->started)
        return PeachMhiInvalid;
    if (t->error) return t->error;
    PeachSrngOps *ops = &t->ring->ops;
    TTRY(t, ops->health(ops->context));
    /* Bank 0 is host-owned; Peach has eight banks, last reserved for FW.
     * hal_be_generic_api.h's Peach layout uses DSCP table bits at 17, not
     * newer MCAST_PACKET_CTRL bits. STA uses the real BSS AST index.
     * Non-raw encryption is taken from the peer, not this bank. */
    uint32_t config = 2u << 1 | 1u << 9 | t->lmac << 15, check;
    TTRY(t, ops->write32(ops->context, 0x00a4408c, config));
    TTRY(t, ops->read32(ops->context, 0x00a4408c, &check));
    if (check != config) return invalid(t);
    t->started = 1;
    return 0;
}
static int mapping_live(PeachTx *t)
{
    const PeachHttPeer *p = peach_htt_peer(t->htt, t->vdev, t->peer_mac);
    return !t->htt->error && p && p->generation == t->peer_generation &&
        p->peer == t->peer && p->ast_valid && p->hash_valid &&
        p->ast == t->ast && p->hash == t->hash;
}
int peach_tx_bind(PeachTx *t, unsigned vdev, const unsigned char mac[6])
{
    if (!t || !t->started || t->authorized || t->in_flight || vdev || !mac)
        return PeachMhiInvalid;
    if (t->error) return t->error;
    if (t->htt->error) return t->error = t->htt->error;
    const PeachHttPeer *p = peach_htt_peer(t->htt, vdev, mac);
    if (!p || !p->ast_valid || !p->hash_valid) return PeachMhiInvalid;
    t->vdev = vdev; t->peer = p->peer; t->ast = p->ast; t->hash = p->hash;
    t->peer_generation = p->generation; memcpy(t->peer_mac, mac, 6); t->bound = 1;
    return 0;
}
int peach_tx_authorize(PeachTx *t, unsigned enabled)
{
    if (!t || !t->prepared || enabled > 1 || (enabled && (!t->bound || !mapping_live(t))))
        return PeachMhiInvalid;
    if (t->error) return t->error;
    t->authorized = enabled;
    return 0;
}
int peach_tx_unbind(PeachTx *t)
{
    if (!t || !t->prepared || t->authorized || t->in_flight || t->control_pending)
        return PeachMhiInvalid;
    if (t->error) return t->error;
    t->bound = t->qos = t->control_done = t->control_acked = 0;
    t->peer_generation = 0; memset(t->peer_mac, 0, sizeof(t->peer_mac));
    return 0; /* retain cookie generations and all DMA storage */
}
static int send_packet(PeachTx *t, const void *packet, size_t bytes, unsigned tid, unsigned control)
{
    if (!t || !t->started || !t->bound || (!t->authorized && !control) || !packet ||
        bytes < 14 || bytes > 1518 || tid > 15 ||
        !t->ring->started || !t->completion->started) return PeachMhiInvalid;
    if (t->error) return t->error;
    if (!mapping_live(t)) { t->authorized = 0; return PeachMhiInvalid; }
    if (t->ring->error || t->completion->error)
        return t->error = t->ring->error ? t->ring->error : t->completion->error;
    unsigned i = t->next;
    for (unsigned n = 0; n < PEACH_TX_BUFFERS && t->owned[i]; n++)
        i = (i + 1) % PEACH_TX_BUFFERS;
    if (t->owned[i]) return PeachCeFull;
    PeachSrngOps *ops = &t->ring->ops;
    TTRY(t, ops->health(ops->context));
    /* Native skb data has NET_IP_ALIGN=2: after the 14-byte Ethernet
     * header the IP/EAPOL payload is word-aligned for target processing. */
    unsigned char *p = (unsigned char *)t->memory.bytes + i * PEACH_TX_BUFFER_BYTES + 2;
    memcpy(p, packet, bytes);
    size_t length = bytes < 60 ? 60 : bytes;
    if (length > bytes) memset(p + bytes, 0, length - bytes);
    unsigned previous = t->cookies[i];
    /* Native starts at slot ID zero. Add a generation only when a slot
     * is reused after its validated completion, not on its first send. */
    unsigned generation = t->submitted < PEACH_TX_BUFFERS ? 0 : (previous >> 6) + 1;
    if (generation > 16383) generation = 1;
    unsigned cookie = generation << 6 | i;
    t->cookies[i] = cookie;
    /* V1 metadata: vdev-based type 1, no optional HTT extension. */
    uint32_t metadata = 1u | t->vdev << 2 | t->pdev << 10;
    uint32_t entry[8] = {
        (uint32_t)(t->memory.iova + i * PEACH_TX_BUFFER_BYTES + 2), TX_RBM << 8 | cookie << 12,
        /* Captured native M2/M4: semi-hard notify, voice TID override and
         * critical-frame classifier; TO_FW is clear for these frames. */
        control ? 3u << 8 : 0, metadata << 16, (uint32_t)length,
        1u | (control && t->qos ? 6u : tid) << 4 | (control ? 6u : 0) |
            t->lmac << 9 | t->vdev << 24,
        t->ast | (t->hash & 15) << 20, 0};
    if (control && t->htt->htc->ops.log) {
        t->htt->htc->ops.log(t->htt->htc->ops.context, "TX EAPOL native notify/classifier", entry[2], entry[5]);
        t->htt->htc->ops.log(t->htt->htc->ops.context, "TX EAPOL DMA/cookie", entry[0], entry[1]);
        t->htt->htc->ops.log(t->htt->htc->ops.context, "TX EAPOL metadata/length", entry[3], entry[4]);
        t->htt->htc->ops.log(t->htt->htc->ops.context, "TX EAPOL search/hash", entry[6], 0);
    }
    t->owned[i] = 1; t->in_flight++; t->submitted_at[i] = now(t);
    int err = peach_srng_push(t->ring, entry, sizeof(entry));
    if (err == PeachCeFull) {
        t->owned[i] = 0; t->in_flight--; t->cookies[i] = previous; return err;
    }
    if (err) return t->error = err; /* ownership is ambiguous: retain buffer */
    t->next = (i + 1) % PEACH_TX_BUFFERS; t->submitted++;
    if (control) {
        t->control_cookie = cookie; t->control_pending = 1;
        t->control_done = t->control_acked = 0;
    }
    return 0;
}
int peach_tx_send(PeachTx *t, const void *packet, size_t bytes, unsigned tid)
{ return send_packet(t, packet, bytes, tid, 0); }
int peach_tx_eapol(PeachTx *t, const unsigned char local[6], const void *packet, size_t bytes)
{
    const unsigned char *p = packet;
    if (!t || !local || !p || bytes < 18 || bytes > 640 ||
        memcmp(p, t->peer_mac, 6) || memcmp(p + 6, local, 6) ||
        p[12] != 0x88 || p[13] != 0x8e || p[15] != 3 ||
        (size_t)(p[16] * 256u + p[17]) + 18 != bytes) return PeachMhiInvalid;
    if (t->control_pending) return PeachCeFull;
    return send_packet(t, packet, bytes, 0, 1);
}
int peach_tx_poll(PeachTx *t)
{
    if (!t || !t->started || !t->completion->started) return 0;
    if (t->error) return t->error;
    if (t->bound && !mapping_live(t)) t->authorized = 0;
    for (unsigned budget = 0; budget < 64; budget++) {
        uint32_t d[8];
        int err = peach_srng_pop(t->completion, d, sizeof(d));
        if (err == PeachSrngEmpty) break;
        if (err) return t->error = err;
        memcpy(t->last_descriptor, d, sizeof(d));
        t->last_status = d[2];
        /* FW overlays HTT v3 transmit status on DWORDs 2..6. WBM still
         * owns the cookie fields; the peer moves from DWORD7 to DWORD5. */
        unsigned source = d[2] & 7, status = (d[2] >> 13) & 15;
        /* MEC is a multicast learning notification, not ownership return
         * for this cookie. It may precede the actual TQM completion. */
        if (source == 3 && status == 5) {
            if (d[2] & 0x80000000u) return invalid(t);
            continue;
        }
        /* With CC disabled, native hal_tx_comp_get_desc_id reads the
         * original buffer_addr_info in DWORDs 0/1. Only converted records
         * move the cookie/RBM into the completion metadata. */
        unsigned converted = !!(d[2] & 0x40000000u);
        unsigned cookie = converted ?
            ((d[2] >> 18) & 4095) | ((d[7] >> 20) & 255) << 12 : d[1] >> 12;
        unsigned rbm = converted ? (d[2] >> 9) & 15 : (d[1] >> 8) & 15;
        unsigned peer = (source == 3 ? d[5] : d[7]) & 65535;
        if (t->acked + t->dropped < 3 && t->htt->htc->ops.log) {
            t->htt->htc->ops.log(t->htt->htc->ops.context, "TX completion source/status", source, status);
            t->htt->htc->ops.log(t->htt->htc->ops.context, "TX completion cookie/peer", cookie, peer);
        }
        /* Peach bit30 reports cookie conversion, not an internal error.
         * Keep using our cookie table; never dereference the returned VA. */
        if ((source != 0 && source != 3) || ((d[2] >> 6) & 7) != 0 ||
            rbm != TX_RBM || (d[2] & 0x80000000u)) return invalid(t);
        /* FW reinject/inspect return ownership too (dp_tx_*_handler),
         * but are dropped by this STA path and never counted as ACKs.
         * Their overlay is not the OK/DROP/TTL peer-status layout. */
        /* The FW status valid bit qualifies peer/TID metadata, not the
         * ownership return or OK status. The cookie identifies our packet
         * even when those optional fields are unavailable. */
        /* FW VDEVID_MISMATCH=6 also returns the packet. Native dp_be_tx.c
         * releases it as a drop; it is not a transport failure or an ACK.
         * MEC=5 was handled above and never transfers ownership. */
        if (source == 3 && status > 6) return invalid(t);
        unsigned i = cookie & (PEACH_TX_BUFFERS - 1);
        if (!t->bound || !t->owned[i] || cookie != t->cookies[i] ||
            ((source == 0 || (status < 3 && (d[5] & (1u << 21)))) && peer != t->peer)) return invalid(t);
        if (!converted && (d[0] | (uint64_t)(d[1] & 255) << 32) !=
            t->memory.iova + i * PEACH_TX_BUFFER_BYTES + 2) return invalid(t);
        /* A source TP advance did not release this buffer. Only this
         * validated completion permits reuse, including failed TX. */
        t->owned[i] = 0; t->in_flight--;
        unsigned acked = status == 0;
        if (t->control_pending && cookie == t->control_cookie) {
            t->control_pending = 0; t->control_done = 1; t->control_acked = acked;
        }
        if (acked) t->acked++;
        else t->dropped++;
    }
    if (t->in_flight) {
        uint64_t current = now(t);
        for (unsigned i = 0; i < PEACH_TX_BUFFERS; i++)
            if (t->owned[i] && current - t->submitted_at[i] > 10000000)
                return t->error = PeachMhiTimeout;
    }
    return 0;
}
