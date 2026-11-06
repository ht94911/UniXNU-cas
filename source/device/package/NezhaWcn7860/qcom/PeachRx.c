/* SPDX-License-Identifier: ISC */
/*
 * Derived from hal_be_reo.c / hal_be_generic_api.c and Peach v2 fw-api:
 * Copyright (c) 2016-2021 The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "PeachRx.h"
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

#define RTRY(r, expr) do { int err_ = (expr); if (err_) { (r)->error = err_; return err_; } } while (0)
#define REO_BASE 0x00a38000u
#define WBM_BASE 0x00a34000u
#define RX_RBM 8u /* Peach single-PDEV RX release -> WBM2SW3. */
static uint32_t u32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static PeachSrngOps *ops(PeachRx *r) { return &r->rings[PeachSrngReoRx].ops; }
static void barrier(PeachRx *r) { ops(r)->barrier(ops(r)->context); }
static int health(PeachRx *r) { return ops(r)->health(ops(r)->context); }
static int invalid(PeachRx *r) { return r->error = PeachCeProtocol; }
static int rd(PeachRx *r, uint32_t reg, uint32_t *value)
{
    r->last_register = reg;
    int err = ops(r)->read32(ops(r)->context, reg, value);
    r->last_value = err ? 0xffffffffu : *value;
    if (!err && *value == 0xffffffffu) return PeachCeProtocol;
    return err;
}
static int wr(PeachRx *r, uint32_t reg, uint32_t value)
{
    r->last_register = reg; r->last_value = value;
    return ops(r)->write32(ops(r)->context, reg, value);
}
static int update(PeachRx *r, uint32_t reg, uint32_t mask, uint32_t value)
{
    uint32_t old, check;
    RTRY(r, rd(r, reg, &old));
    RTRY(r, wr(r, reg, (old & ~mask) | value));
    RTRY(r, rd(r, reg, &check));
    return (check & mask) == value ? 0 : invalid(r);
}
PeachMhiDma peach_rx_queue(const PeachRx *r, unsigned tid)
{
    if (!r || !r->prepared || tid > 16) return (PeachMhiDma){0};
    return (PeachMhiDma){(unsigned char *)r->queues.bytes + tid * 512u,
        r->queues.iova + tid * 512u, tid == 16 ? 128 : 512};
}
static void prepare_queues(PeachRx *r)
{
    /* rx_reo_queue/ext are 128B each, aligned to 128B. The QoS queues
     * retain all three extensions, matching hal_get_reo_qdesc_size_be.
     * Non-QoS TID 16 uses only the base descriptor. */
    for (unsigned tid = 0; tid <= 16; tid++) {
        uint32_t *p = peach_rx_queue(r, tid).bytes;
        unsigned ac = (tid == 0 || tid == 3) ? 0 :
                      (tid == 1 || tid == 2) ? 1 : (tid == 4 || tid == 5) ? 2 : 3;
        p[0] = 4u | 8u << 4 | 0xddbeefu << 8; /* REO owner / queue */
        p[1] = tid;
        p[2] = 1u | 1u << 1 | ac << 5 | 1u << 27; /* valid/counter1/ignore AMPDU */
        /* hal_update_non_ba_win_size: QoS window 2, non-QoS window 1.
         * SSN/SVLD remain zero, PN checking is not enabled yet. */
        p[2] |= tid == 16 ? 1u << 8 : 1u << 11;
        if (tid != 16) {
            static const uint32_t pattern[] = {0xadbeef, 0xbdbeef, 0xcdbeef};
            for (unsigned i = 0; i < 3; i++)
                p[(i + 1) * 32] = 4u | 10u << 4 | pattern[i] << 8;
        }
    }
}
int peach_rx_prepare(PeachRx *r, PeachSrng rings[PeachSrngKinds], PeachMhiDma memory)
{
    if (!r || !rings || !memory.bytes || !memory.iova ||
        ((uintptr_t)memory.bytes & 127) || (memory.iova & 127) ||
        memory.capacity < PEACH_RX_DMA_BYTES || memory.iova >= 0x100000000ull ||
        PEACH_RX_DMA_BYTES > 0x100000000ull - memory.iova) return PeachMhiInvalid;
    for (unsigned i = 0; i < PeachSrngKinds; i++) {
        if (!rings[i].prepared || rings[i].started || rings[i].error)
            return PeachMhiInvalid;
        PeachMhiDma d = rings[i].memory;
        if (d.iova < memory.iova + PEACH_RX_DMA_BYTES &&
            memory.iova < d.iova + d.capacity) return PeachMhiInvalid;
    }
    memset(r, 0, sizeof(*r)); r->rings = rings; r->memory = memory;
    RTRY(r, health(r));
    memset(memory.bytes, 0, PEACH_RX_DMA_BYTES);
    size_t off = PEACH_RX_LINKS * 128u;
    r->links = (PeachMhiDma){memory.bytes, memory.iova, off};
    r->queues = (PeachMhiDma){(unsigned char *)memory.bytes + off,
        memory.iova + off, PEACH_RX_QUEUE_BYTES};
    off += PEACH_RX_QUEUE_BYTES;
    r->buffers = (PeachMhiDma){(unsigned char *)memory.bytes + off,
        memory.iova + off, PEACH_RX_BUFFERS * PEACH_RX_BUFFER_BYTES};
    r->prepared = 1; prepare_queues(r); barrier(r);
    /* No kernel virtual-address cookie conversion. Preserve unrelated
     * register fields; this endpoint is solely owned by this driver. */
    RTRY(r, update(r, REO_BASE + 0x70, 0x180000, 0));
    RTRY(r, update(r, WBM_BASE + 0x94, 0x1ff, 0));
    /* Enable aging, preserving firmware-owned REO/ring enable bits. */
    RTRY(r, update(r, REO_BASE, 0xc, 0xc));
    for (unsigned i = 0; i < 4; i++)
        RTRY(r, wr(r, REO_BASE + 0xb2c + i * 4, i == 3 ? 40000 : 100000));
    /* Route hash destinations 16..31 to the sole REO2SW1 ring. Drop/reclaim
     * fragmented traffic via WBM (destination 5); no software defragmenter. */
    RTRY(r, wr(r, REO_BASE + 0xc, 0x11111111));
    RTRY(r, wr(r, REO_BASE + 0x10, 0x11111111));
    RTRY(r, update(r, REO_BASE + 0xba0, 0x1e0000 | 0x1e00000, 5u << 17));
    return 0;
}
static int replenish(PeachRx *r)
{
    if (!r->repost) return 0;
    for (unsigned i = 0; i < PEACH_RX_BUFFERS; i++) {
        if (r->owned[i]) continue;
        RTRY(r, health(r));
        unsigned char *p = (unsigned char *)r->buffers.bytes + i * PEACH_RX_BUFFER_BYTES;
        /* Clear old tags/MSDU_DONE before firmware can take this buffer. */
        memset(p, 0, PEACH_HTT_RX_PACKET_OFFSET);
        unsigned previous = r->cookies[i], generation = (previous >> 8) + 1;
        if (generation > 4095) generation = 1;
        r->cookies[i] = generation << 8 | i;
        uint32_t entry[2] = {(uint32_t)(r->buffers.iova + i * PEACH_RX_BUFFER_BYTES),
            RX_RBM << 8 | r->cookies[i] << 12};
        /* Mark ownership before publishing, including ambiguous failure. */
        r->owned[i] = 1; r->in_flight++;
        int err = peach_srng_push(&r->rings[PeachSrngRxBuffer], entry, sizeof(entry));
        if (err == PeachCeFull) {
            r->owned[i] = 0; r->in_flight--; r->cookies[i] = previous; return 0;
        }
        if (err) return r->error = err;
        r->repost--;
    }
    return 0;
}
int peach_rx_new_queues(PeachRx *r, PeachMhiDma memory)
{
    if (!r || !r->prepared || !memory.bytes || !memory.iova ||
        ((uintptr_t)memory.bytes & 127) || (memory.iova & 127) ||
        memory.capacity < PEACH_RX_QUEUE_BYTES || memory.iova >= 0x100000000ull ||
        PEACH_RX_QUEUE_BYTES > 0x100000000ull - memory.iova ||
        (memory.iova < r->memory.iova + r->memory.capacity &&
         r->memory.iova < memory.iova + PEACH_RX_QUEUE_BYTES) ||
        (memory.iova < r->queues.iova + r->queues.capacity &&
         r->queues.iova < memory.iova + PEACH_RX_QUEUE_BYTES)) return PeachMhiInvalid;
    if (r->error) return r->error;
    RTRY(r, health(r));
    r->queues = memory;
    memset(memory.bytes, 0, PEACH_RX_QUEUE_BYTES);
    prepare_queues(r); barrier(r);
    return 0;
}
int peach_rx_start(PeachRx *r, PeachHtt *h, unsigned pdev, unsigned peer_metadata_version)
{
    if (!r || !r->prepared || r->started || !h || !peer_metadata_version ||
        peer_metadata_version > 2) return PeachMhiInvalid;
    if (r->error) return r->error;
    for (unsigned i = 0; i < PeachSrngKinds; i++)
        if (!r->rings[i].started || r->rings[i].error) return PeachMhiInvalid;
    /* The idle link pool is published before WMI INIT, alongside host
     * TCL/WBM rings; firmware initialization may already reference it. */
    if (r->rings[PeachSrngIdleLink].wp != PEACH_RX_LINKS) return PeachMhiInvalid;
    RTRY(r, peach_htt_config_rx(h, pdev, PEACH_RX_BUFFER_BYTES));
    r->repost = PEACH_RX_BUFFERS;
    RTRY(r, replenish(r));
    r->peer_metadata_version = peer_metadata_version; r->started = 1;
    return 0;
}
static int buffer_index(PeachRx *r, uint32_t low, unsigned high, unsigned cookie, unsigned *index)
{
    unsigned i = cookie & 255;
    uint64_t address = low | (uint64_t)high << 32;
    if (!cookie || cookie != r->cookies[i] || !r->owned[i] ||
        address != r->buffers.iova + i * PEACH_RX_BUFFER_BYTES)
        return invalid(r);
    *index = i;
    return 0;
}
static void release(PeachRx *r, unsigned i)
{ r->owned[i] = 0; r->in_flight--; r->repost++; }
static int tag_valid(uint32_t word, unsigned tag, unsigned bytes)
{ return ((word >> 1) & 511) == tag && ((word >> 10) & 4095) == bytes; }
static int reap_link(PeachRx *r, uint32_t low, uint32_t info)
{
    unsigned char visited[(PEACH_RX_LINKS + 7) / 8] = {0};
    unsigned total = 0;
    for (unsigned limit = 0; limit < PEACH_RX_LINKS; limit++) {
        unsigned cookie = info >> 12;
        uint64_t address = low | (uint64_t)(info & 255) << 32;
        if (cookie >= PEACH_RX_LINKS || ((info >> 8) & 15) != 1 ||
            address != r->links.iova + cookie * 128u ||
            (visited[cookie / 8] & (1u << (cookie & 7)))) return invalid(r);
        visited[cookie / 8] |= 1u << (cookie & 7);
        RTRY(r, health(r)); barrier(r);
        uint32_t *p = (uint32_t *)((unsigned char *)r->links.bytes + cookie * 128u);
        uint32_t link[32]; memcpy(link, p, sizeof(link)); barrier(r);
        unsigned indices[6], count = 0;
        /* Peach rx_msdu_link: next buffer_addr_info at DWORD1, six
         * 16-byte rx_msdu_details starting at DWORD8. Like the vendor HAL,
         * a zero packet address terminates the list; MPDU count is advisory. */
        for (unsigned j = 0; j < 6; j++) {
            unsigned word = 8 + j * 4;
            if (!link[word] && !(link[word + 1] & 255)) break;
            if (((link[word + 1] >> 8) & 15) != RX_RBM) return invalid(r);
            RTRY(r, buffer_index(r, link[word], link[word + 1] & 255,
                link[word + 1] >> 12, &indices[j]));
            for (unsigned k = 0; k < j; k++)
                if (indices[k] == indices[j]) return invalid(r);
            count++;
        }
        if (!count || total + count > PEACH_RX_BUFFERS) return invalid(r);
        uint32_t entry[8] = {low, info, 4u | 1u << 6}; /* SW/MSDU link, idle-list action 0 */
        /* Clear stale next/packet pointers while SW owns the descriptor.
         * WBM only returns this link to its idle list; it does not release
         * packet children again. We retain their memory and repost later. */
        memset(p, 0, sizeof(link)); barrier(r);
        RTRY(r, peach_srng_push(&r->rings[PeachSrngWbmRelease], entry, sizeof(entry)));
        for (unsigned j = 0; j < count; j++) release(r, indices[j]);
        r->discarded += count; total += count;
        low = link[1]; info = link[2];
        if (!low && !(info & 255)) return 0;
    }
    return invalid(r);
}
static int reap_reo(PeachRx *r, const uint32_t d[8], PeachReceive receive, void *ctx)
{
    if (d[7] & 1) return reap_link(r, d[0], d[1]);
    unsigned i;
    if (((d[1] >> 8) & 15) != RX_RBM) return invalid(r);
    RTRY(r, buffer_index(r, d[0], d[1] & 255, d[1] >> 12, &i));
    RTRY(r, health(r)); barrier(r);
    unsigned char *p = (unsigned char *)r->buffers.bytes + i * PEACH_RX_BUFFER_BYTES;
    /* Normal REO2SW is MSDU memory with full noncompacted TLVs. A format
     * mismatch is a firmware/host ABI failure, not an empty receive queue. */
    if (!(u32(p + 4 + 0x7c) & 0x80000000u) ||
        !tag_valid(u32(p), 197, 128) ||
        !tag_valid(u32(p + PEACH_HTT_RX_MPDU_OFFSET), 194, 120)) return invalid(r);
    barrier(r);
    unsigned length = (d[4] >> 3) & 0x3fff;
    unsigned padding = (u32(p + 4 + 0x28) >> 26) & 3;
    unsigned offset = PEACH_HTT_RX_PACKET_OFFSET + padding;
    /* Bad over-air packets are dropped. This minimal path accepts complete
     * Ethernet-II MSDUs; no cross-buffer continuation or raw 802.11 input. */
    unsigned drop = ((d[7] >> 1) & 3) != 1 || (d[2] & 0x100) ||
        (d[4] & (4 | 0x20000)) || ((d[4] >> 29) & 3) != 2 ||
        ((u32(p + 4 + 0x4c) >> 8) & 3) != 2 ||
        (u32(p + 4 + 0x78) & 0xf8039000u) ||
        length < 14 || length > PEACH_RX_BUFFER_BYTES - offset ||
        length != (u32(p + 4 + 0x48) & 0x3fff);
    const unsigned char *m = p + PEACH_HTT_RX_MPDU_OFFSET + 4;
    uint32_t flags = u32(m + 0x20);
    /* Only complete, nonaggregated infrastructure data. Preserve hardware
     * encryption metadata so the connection owner can enforce its key/PN
     * policy; Ethernet decapsulation alone does not authenticate a packet. */
    drop |= (flags & 0x30009) != 0x10009 || (flags & 0x7c00) ||
        (u32(m + 0x30) & 0x10000000u) || (u32(m + 0x34) & 0x40000000u);
    PeachRxInfo info = {0};
    info.peer = u32(m + 0x28) >> 16;
    uint32_t peer_meta = u32(m + 0x24);
    unsigned vdev_shift = r->peer_metadata_version == 2 ? 14 : 16;
    /* RX_MPDU_START includes both the hardware SW peer and the negotiated
     * host metadata. Only the local vdev and an ordinary link peer belong
     * to this STA; ML peers and accelerator/passthrough frames are rejected. */
    drop |= (peer_meta & 8191) != info.peer || (peer_meta & (1u << 13)) ||
        ((peer_meta >> vdev_shift) & 255) ||
        (r->peer_metadata_version == 2 && (peer_meta & 0xe0000000u));
    info.tid = flags & 128 ? (u32(m + 4) >> 15) & 15 : 16;
    info.encrypted = !!(flags & 0x40000);
    info.cipher = (u32(m + 4) >> 2) & 15;
    info.key_index = (u32(m + 0x30) >> 6) & 3;
    info.multicast = !!(u32(p + 4 + 0x78) & 4);
    info.decrypted = (flags & 0x200) && !((u32(p + 4 + 0x7c) >> 10) & 7);
    info.pn = u32(m + 0x10) | (uint64_t)u32(m + 0x14) << 32;
    memcpy(info.transmitter, m + 0x42, 6);
    if (info.encrypted && (info.cipher != 6 || !info.decrypted ||
        info.pn >= 0x1000000000000ull || u32(m + 0x18) || u32(m + 0x1c))) drop = 1;
    int err = 0;
    if (drop || !receive) r->discarded++;
    else { r->received++; err = receive(ctx, p + offset, length, &info); }
    release(r, i);
    return err;
}
static int reap_wbm(PeachRx *r, const uint32_t d[8])
{
    /* RXDMA/REO error buffers need no completed MSDU_END tag. Like TX,
     * WBM retains buffer_addr_info in DWORDs0/1 when CC is disabled;
     * converted RX completions retain physical address/cookie at 6/7. */
    unsigned source = d[2] & 7, converted = !!(d[2] & (1u << 16));
    unsigned rbm = converted ? (d[2] >> 9) & 15 : (d[1] >> 8) & 15;
    if ((source != 1 && source != 2) || (d[2] & 0x80000000u) ||
        ((d[2] >> 6) & 7) != 0 || rbm != RX_RBM) return invalid(r);
    unsigned i;
    RTRY(r, buffer_index(r, converted ? d[6] : d[0],
        (converted ? d[7] : d[1]) & 255,
        converted ? (d[7] >> 8) & 0xfffff : d[1] >> 12, &i));
    r->released++; release(r, i);
    return 0;
}
int peach_rx_poll(PeachRx *r, PeachReceive receive, void *ctx)
{
    if (!r || !r->started) return 0;
    if (r->error) return r->error;
    static const unsigned kinds[] = {PeachSrngReoRx, PeachSrngReoException,
                                     PeachSrngRxException, PeachSrngWbmCompletion};
    for (unsigned ring = 0; ring < sizeof(kinds) / sizeof(kinds[0]); ring++) {
        for (unsigned budget = 0; budget < 64; budget++) {
            uint32_t d[8];
            int err = peach_srng_pop(&r->rings[kinds[ring]], d, sizeof(d));
            if (err == PeachSrngEmpty) break;
            if (err) return r->error = err;
            memcpy(r->last_descriptor, d, sizeof(d));
            if (kinds[ring] == PeachSrngRxException) {
                /* REO entrance format, always a link. A frameless BAR has
                 * no packet/link address to return. */
                if (!(d[0] | (d[1] & 255)) && (d[5] & (1u << 27))) continue;
                RTRY(r, reap_link(r, d[0], d[1]));
            } else RTRY(r, kinds[ring] == PeachSrngWbmCompletion ? reap_wbm(r, d) :
                            reap_reo(r, d, receive, ctx));
        }
    }
    return replenish(r);
}
