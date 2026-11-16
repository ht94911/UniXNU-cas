/* SPDX-License-Identifier: GPL-2.0-only */
/* HTT v3 control ABI: fw-api/fw/htt.h, vendor dp/wifi3.0/dp_htt.c. */
#include "PeachHtt.h"
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <string.h>
#endif
static uint32_t u32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
const PeachHttPeer *peach_htt_peer(const PeachHtt *h, unsigned vdev, const unsigned char mac[6])
{
    if (!h || !mac) return NULL;
    for (unsigned i = 0; i < PEACH_HTT_PEERS; i++)
        if (h->peers[i].mapped && h->peers[i].vdev == vdev && !memcmp(h->peers[i].mac, mac, 6))
            return &h->peers[i];
    return NULL;
}
static int peer_message(PeachHtt *h, const unsigned char *p, size_t n)
{
    unsigned type = p[0], map = type == 3 || type == 0x1e, v2 = type >= 0x1e;
    if (!h->confirmed || n != (map ? (v2 ? 32u : 12u) : (v2 ? 28u : 4u)))
        return h->error = PeachCeProtocol;
    unsigned id = u32(p) >> 16, vdev = p[1];
    if (vdev || id == 65535) return h->error = PeachCeProtocol;
    PeachHttPeer *slot = NULL;
    for (unsigned i = 0; i < PEACH_HTT_PEERS; i++) {
        PeachHttPeer *s = &h->peers[i];
        if (s->mapped && s->peer == id) { slot = s; break; }
    }
    if (!map) {
        if (!slot || slot->vdev != vdev || (v2 &&
            (memcmp(slot->mac, p + 4, 6) || (u32(p + 8) & (1u << 16)) ||
             (u32(p + 16) & 65535)))) return h->error = PeachCeProtocol;
        slot->mapped = 0;
        return 0;
    }
    if (slot || (p[4] & 1) || !memcmp(p + 4, "\0\0\0\0\0\0", 6) ||
        peach_htt_peer(h, vdev, p + 4) || (v2 && (u32(p + 12) & (1u << 16))))
        return h->error = PeachCeProtocol; /* no WDS/duplicate peer ownership */
    for (unsigned i = 0; i < PEACH_HTT_PEERS; i++)
        if (!h->peers[i].mapped) { slot = &h->peers[i]; break; }
    if (!slot || h->peer_generation == 0xffffffffu) return h->error = PeachCeProtocol;
    memset(slot, 0, sizeof(*slot));
    slot->vdev = vdev; slot->peer = id; slot->ast = u32(p + 8) >> 16;
    slot->ast_valid = slot->ast != 65535;
    slot->hash_valid = v2; slot->hash = v2 ? u32(p + 12) & 65535 : 0;
    slot->generation = ++h->peer_generation;
    memcpy(slot->mac, p + 4, 6); slot->mapped = 1;
    return 0;
}
int peach_htt_init(PeachHtt *h, PeachHtc *htc)
{
    if (!h || !htc) return PeachMhiInvalid;
    memset(h, 0, sizeof(*h)); h->htc = htc;
    h->address_bits = 32; h->metadata_version = 1;
    return 0;
}
void peach_htt_receive(PeachHtt *h, const void *packet, size_t n)
{
    const unsigned char *p = packet;
    if (!h || h->error) return;
    if (!p || n < 4 || (n & 3)) { h->error = PeachCeProtocol; return; }
    uint32_t word = u32(p);
    h->last_message = word;
    if ((word & 255) == 0x30) { /* htt_t2h_ppdu_id_fmt_ind_t */
        if (!h->confirmed || n != 20 || word != 0x30) {
            h->error = PeachCeProtocol; return;
        }
        /* This describes PPDU statistics bit packing, with no DMA ownership
         * or ACK. Neither PPDU statistics nor monitor/MLO decoding is used. */
        if (h->htc->ops.log) h->htc->ops.log(h->htc->ops.context,
            "HTT PPDU format metadata", u32(p + 4), u32(p + 8));
        return;
    }
    if ((word & 255) == 0x24) {
        unsigned pdev = (word >> 8) & 255, kind = (word >> 16) & 255, ring = word >> 24;
        if (!h->confirmed || n != 12 || kind > 1 ||
            (kind ? !pdev || pdev > 3 || ring >= 13 : pdev || ring >= 18)) {
            h->error = PeachCeProtocol; return;
        }
        /* Vendor dp_htt_bkp_event_alert: a diagnostic after 100ms of ring
         * pressure, repeated while it persists. Keep servicing the real
         * rings; these reported indices never complete or release DMA. */
        if (h->backpressure_reports < 8) {
            h->backpressure_reports++;
            if (h->htc->ops.log) h->htc->ops.log(h->htc->ops.context,
                "HTT backpressure ring/time ms", word >> 8, u32(p + 8));
        }
        return;
    }
    if ((word & 255) == 0xb) { /* htt_sec_ind: 7 words, not a key ACK */
        unsigned type = (word >> 8) & 127, unicast = (word >> 15) & 1;
        if (!h->confirmed || n != 28 || (type && type != 6)) {
            h->error = PeachCeProtocol; return; /* none / AES-CCMP128 only */
        }
        for (unsigned i = 0; i < PEACH_HTT_PEERS; i++) {
            PeachHttPeer *s = &h->peers[i];
            if (!s->mapped || s->peer != (word >> 16)) continue;
            if (s->security_generation[unicast] == 0xffffffffu) break;
            s->security[unicast] = type; s->security_valid[unicast] = 1;
            s->security_generation[unicast]++;
            if (h->htc->ops.log) h->htc->ops.log(h->htc->ops.context,
                "HTT security indication/generation", word, s->security_generation[unicast]);
            return;
        }
        h->error = PeachCeProtocol; return;
    }
    if ((word & 255) == 3 || (word & 255) == 4 ||
        (word & 255) == 0x1e || (word & 255) == 0x1f) {
        (void)peer_message(h, p, n);
        return;
    }
    if ((word & 255) == 0x1a) {
        unsigned reply_id = h->setup_id, reply_pdev = h->setup_pdev;
        /* This phone's HTT 3.143 translates wire HOST1 ring 5 to WAL
         * ring 3, and RXDMA rings 0/7 to WAL ring 2, before building DONE.
         * DONE also carries the radio's zero-based pdev. Match these
         * target IDs while retaining the requested wire IDs. */
        if (h->minor == 143) {
            if (reply_pdev) reply_pdev--;
            if (reply_id == 5) reply_id = 3;
            else if (reply_id == 15) reply_id = 0; /* observed HOST4/MGMT completion */
            else if (reply_id == 0 || reply_id == 7) reply_id = 2;
        }
        if (n != 4 || !h->confirmed || !h->setup_pending || !h->setup_sent ||
            h->setup_done || ((word >> 8) & 255) != reply_pdev ||
            ((word >> 16) & 255) != reply_id) {
            if (h->htc->ops.log) h->htc->ops.log(h->htc->ops.context,
                "HTT SRING_SETUP mismatched request/reply", h->setup_id, word);
            h->error = PeachCeProtocol; return;
        }
        h->setup_status = word >> 24;
        if (h->setup_status) { h->error = PeachCeProtocol; return; }
        h->setup_done = 1;
        return;
    }
    /* Other notifications require an explicitly implemented DP handler. */
    if ((word & 255) || !h->requested || h->confirmed) { h->error = PeachCeProtocol; return; }
    h->major = (word >> 16) & 255; h->minor = (word >> 8) & 255;
    if (h->major != 3) { h->error = PeachCeProtocol; return; }
    unsigned seen = 0;
    for (size_t off = 4; off < n;) {
        word = u32(p + off);
        unsigned tag = word & 255, length = (word >> 8) & 255, value = word >> 16;
        /* Legacy options count the complete TLV in four-byte words (see
         * htt_h2t.c's MAX_TX_QUEUE_GROUPS). The current Peach dp_htt.c
         * instead emits TCL_METADATA_VER with byte length 4. Accept that
         * documented scalar's word/byte forms, without changing other tags. */
        unsigned bytes = tag == 5 && length == 4 ? 4 : length * 4;
        if (!bytes || bytes > n - off || (tag < 32 && (seen & (1u << tag)))) {
            h->error = PeachCeProtocol; return;
        }
        if (tag < 32) seen |= 1u << tag;
        if (tag == 1) {
            if (bytes != 4 || value > 1) { h->error = PeachCeProtocol; return; }
            h->address_bits = value ? 64 : 32;
        } else if (tag == 2 || tag == 3) {
            /* Preserve individual completions; no firmware TX queue groups.
             * Neither option may silently enable an unimplemented path. */
            if (bytes != 4 || value) { h->error = PeachCeProtocol; return; }
        } else if (tag == 4) {
            /* Target support for extended TX descriptors does not require
             * their use. The existing path leaves its extension bit clear. */
            if (bytes != 4 || value > 1) { h->error = PeachCeProtocol; return; }
        } else if (tag == 5) {
            if (bytes != 4 || value != 1) { h->error = PeachCeProtocol; return; }
            h->metadata_version = value;
        } else {
            /* No optional HTT feature was requested. Do not accept an
             * unsolicited format change and later misinterpret DMA. */
            h->error = PeachCeProtocol; return;
        }
        off += bytes;
    }
    h->confirmed = 1;
    if (h->htc->ops.log) h->htc->ops.log(h->htc->ops.context,
        "HTT VERSION_CONF major/minor", h->major, h->minor);
}
static int send_message(PeachHtt *h, const void *p, size_t n)
{
    if (h->error) return h->error;
    uint64_t end = h->htc->ops.now_us(h->htc->ops.context) + 10000000;
    int err;
    while ((err = peach_htc_send(h->htc, 0x300, p, n)) == PeachCeFull) {
        err = peach_htc_poll(h->htc);
        if (err || h->error) return h->error = err ? err : h->error;
        if (h->htc->ops.now_us(h->htc->ops.context) >= end) return h->error = PeachMhiTimeout;
        h->htc->ops.sleep_us(h->htc->ops.context, 1000);
    }
    if (err) return h->error = err;
    return 0;
}
static int wait_flag(PeachHtt *h, const unsigned *flag)
{
    uint64_t end = h->htc->ops.now_us(h->htc->ops.context) + 10000000;
    while (!*flag) {
        int err = peach_htc_poll(h->htc);
        if (err || h->error) return h->error = err ? err : h->error;
        if (h->htc->ops.now_us(h->htc->ops.context) >= end) return h->error = PeachMhiTimeout;
        if (!*flag) h->htc->ops.sleep_us(h->htc->ops.context, 1000);
    }
    return 0;
}
int peach_htt_negotiate(PeachHtt *h)
{
    if (!h || !h->htc || !h->htc->started || h->requested) return PeachMhiInvalid;
    /* The real phone's htt_soc_attach_target allocates and submits exactly
     * four payload bytes, all zero (disassembly +0x88..0xac). Its compiled
     * V1 builder sends no metadata TLV. Per htt.h, absence selects V1;
     * explicit version TLVs belong to the V2+ negotiation path. */
    const uint32_t request[] = {0};
    h->requested = 1;
    int err = send_message(h, request, sizeof(request));
    return err ? err : wait_flag(h, &h->confirmed);
}
static int dma_valid(const PeachHtt *h, PeachMhiDma d, size_t n, unsigned alignment)
{
    if (!d.bytes || !n || d.capacity < n || !d.iova ||
        (d.iova & (alignment - 1)) || ((uintptr_t)d.bytes & 3) ||
        d.iova > UINT64_MAX - (n - 1)) return 0;
    return h->address_bits == 64 || d.iova + n - 1 <= 0xffffffffull;
}
static int overlaps(PeachMhiDma a, size_t an, PeachMhiDma b, size_t bn)
{ return a.iova <= b.iova + bn - 1 && b.iova <= a.iova + an - 1; }
int peach_htt_setup_ring(PeachHtt *h, const PeachHttRing *r)
{
    if (!h || !h->confirmed || !r || h->setup_pending || r->pdev >= 4 || r->id >= 20 ||
        r->type > PeachHttSwToSw || r->entries < 4 || !r->entry_dwords ||
        r->entry_dwords > 255 || r->entries > 65535u / r->entry_dwords ||
        (h->configured[r->pdev] & (1u << r->id))) return PeachMhiInvalid;
    if (h->error) return h->error;
    size_t bytes = (size_t)r->entries * r->entry_dwords * 4;
    if (!dma_valid(h, r->descriptors, bytes, 64) || !dma_valid(h, r->head, 4, 4) ||
        !dma_valid(h, r->tail, 4, 4) || overlaps(r->descriptors, bytes, r->head, 4) ||
        overlaps(r->descriptors, bytes, r->tail, 4) || overlaps(r->head, 4, r->tail, 4) ||
        u32(r->head.bytes) || u32(r->tail.bytes)) return PeachMhiInvalid;
    /* Current vendor struct includes the final IPA thresholds word (56B).
     * Polling: no MSI, interrupt thresholds, swaps, IPA or direct refill. */
    uint32_t command[14] = {0};
    command[0] = 0xb | r->pdev << 8 | r->id << 16 | r->type << 24;
    command[1] = (uint32_t)r->descriptors.iova;
    command[2] = (uint32_t)(r->descriptors.iova >> 32);
    command[3] = r->entries * r->entry_dwords | r->entry_dwords << 16;
    if (r->type == PeachHttSwToHw) command[3] |= 1u << 25;
    command[4] = (uint32_t)r->head.iova; command[5] = (uint32_t)(r->head.iova >> 32);
    command[6] = (uint32_t)r->tail.iova; command[7] = (uint32_t)(r->tail.iova >> 32);
    /* This phone's complete native HTT setup stream leaves response_required
     * clear for every ring. Do not insert a completion wait into that sequence.
     * Frame reception is still required to prove the resulting RX path. */
    unsigned no_reply = h->minor == 143;
    command[12] = no_reply ? 0 : 1u << 19;
    if (no_reply && r->id == 0 && r->type == PeachHttSwToHw) {
        /* This ring is filled by firmware. Native RXMAC setup retains
         * its device low-watermark wakeup even though the host polls.
         * Captured 4096-entry ring: timer 0x40, low threshold 0x400. */
        command[11] = 0x40u << 16;
        command[12] = r->entries / 4;
    }
    h->setup_pending = 1; h->setup_sent = h->setup_done = 0;
    h->setup_pdev = r->pdev; h->setup_id = r->id; h->setup_status = 0xffffffffu;
    h->htc->ce->ops.barrier(h->htc->ce->ops.context);
    int err = send_message(h, command, sizeof(command));
    if (err) return err;
    h->setup_sent = 1;
    if (!no_reply) {
        err = wait_flag(h, &h->setup_done);
        if (err) return err; /* retain all DMA after submission, including timeout */
    }
    h->configured[r->pdev] |= 1u << r->id;
    h->setup_pending = 0;
    if (h->htc->ops.log) h->htc->ops.log(h->htc->ops.context,
        no_reply ? "HTT native SRING_SETUP submitted pdev/ring" :
        "HTT SRING_SETUP confirmed pdev/ring", r->pdev, r->id);
    return 0;
}
int peach_htt_config_rx(PeachHtt *h, unsigned pdev, unsigned buffer_bytes)
{
    if (!h || !h->confirmed || h->setup_pending || h->rx_config_sent || pdev >= 4 ||
        !(h->configured[0] & (1u << 5)) ||
        !(h->configured[pdev] & (1u << 7)) ||
        buffer_bytes < PEACH_HTT_RX_PACKET_OFFSET + 14 || buffer_bytes > 65535)
        return PeachMhiInvalid;
    /* htt_rx_ring_selection_cfg_t: all 33 words, including newer optional
     * masks. dp_rxdma_ring_sel_cfg_be selects RXDMA_HOST_BUF_RING=0 using
     * target pdev even when SRING_SETUP uses HOST1_TO_FW ring 5/host slot 0.
     * hal_be_rx_tlv.h noncompact/tagged/no-pkt-hdr layout: 132+8+124=264B. */
    uint32_t command[33] = {0};
    command[0] = 0xc | pdev << 8 | 1u << 26; /* ring 0; offsets valid */
    command[1] = buffer_bytes;
    command[4] = 1u << 24; /* FP CTRL BlockAckReq */
    command[5] = 1u << 18 | 1u << 21; /* FP multicast + unicast data */
    command[6] = 1u | 4u | 8u; /* MPDU_START, PACKET, MSDU_END */
    command[7] = PEACH_HTT_RX_PACKET_OFFSET;
    command[8] = PEACH_HTT_RX_MPDU_OFFSET << 16;
    command[9] = PEACH_HTT_RX_MSDU_OFFSET;
    /* word11 bit23 is deliberately zero: full, not compacted, TLVs. */
    int err = send_message(h, command, sizeof(command));
    if (err) return err;
    h->rx_config_sent = 1; h->rx_pdev = pdev;
    return 0;
}
