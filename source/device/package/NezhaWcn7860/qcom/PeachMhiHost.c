/* SPDX-License-Identifier: GPL-2.0-only */
/* Native Peach IPCR transport. MHI layouts and state transitions follow
 * Qualcomm's drivers/bus/mhi/{common.h,host/{init,main,pm}.c}. Polling replaces
 * IRQ scheduling only; firmware still produces real contexts/completions. */
#include "PeachMhiHost.h"
#include <string.h>

enum { CHCTX = 0, EVCTX = 0x800, CMDCTX = 0x900, CMDRING = 0x1000,
       CTRL_RING = 0x2000, DATA_RING = 0x3000, TX_RING = 0x4000,
       RX_RING = 0x5000, RX_DATA = 0x10000, TX_DATA = 0x90000, CTX_BYTES = 44 };
#define HOST_TRY(x) do { int e_ = (x); if (e_) { h->poisoned = 1; return e_; } } while (0)

/* MHI contexts have u64 fields aligned to 4, with 44-byte stride. Native
 * 32-bit accesses preserve that ABI without unaligned volatile u64 loads.
 * All ring/context IOVAs are deliberately constrained below 4 GiB. */
static uint32_t get32(PeachMhiHost *h, size_t off)
{ return *(volatile uint32_t *)((unsigned char *)h->memory.bytes + off); }
static void put32(PeachMhiHost *h, size_t off, uint32_t value)
{ *(volatile uint32_t *)((unsigned char *)h->memory.bytes + off) = value; }
static uint64_t get64(PeachMhiHost *h, size_t off)
{ uint32_t low = get32(h, off), high = get32(h, off + 4); return low | (uint64_t)high << 32; }
static void put64(PeachMhiHost *h, size_t off, uint64_t value)
{ put32(h, off + 4, (uint32_t)(value >> 32)); put32(h, off, (uint32_t)value); }
static uint64_t dma(PeachMhiHost *h, size_t off) { return h->memory.iova + off; }
static int rd(PeachMhiHost *h, unsigned off, uint32_t *v)
{ return h->io->read32(h->io->context, off, v) ? PeachMhiIoError : 0; }
static int wr(PeachMhiHost *h, unsigned off, uint32_t v)
{ return h->io->write32(h->io->context, off, v) ? PeachMhiIoError : 0; }
static int db(PeachMhiHost *h, unsigned off, uint64_t value)
{
    h->barrier(h->io->context);
    HOST_TRY(wr(h, off + 4, (uint32_t)(value >> 32)));
    return wr(h, off, (uint32_t)value);
}
static int poll_reg(PeachMhiHost *h, unsigned off, uint32_t mask, uint32_t want)
{
    uint64_t end = h->io->now_us(h->io->context) + 10000000;
    do {
        uint32_t value;
        HOST_TRY(rd(h, off, &value));
        if (value != 0xffffffffu && (value & mask) == want) return 0;
        h->io->sleep_us(h->io->context, 1000);
    } while (h->io->now_us(h->io->context) < end);
    return PeachMhiTimeout;
}
static int index_of(PeachMhiHost *h, uint64_t ptr, unsigned base, unsigned n, unsigned *index)
{
    uint64_t first = dma(h, base);
    if (ptr < first || ptr - first >= n * 16 || ((ptr - first) & 15)) return PeachMhiDeviceError;
    *index = (unsigned)((ptr - first) / 16);
    return 0;
}
static void context_ring(PeachMhiHost *h, unsigned ctx, unsigned base, unsigned n)
{
    put64(h, ctx + 12, dma(h, base));
    put64(h, ctx + 20, n * 16);
    put64(h, ctx + 28, dma(h, base));
    put64(h, ctx + 36, dma(h, base));
}
static void descriptor(PeachMhiHost *h, unsigned off, uint64_t address, unsigned len, unsigned flags)
{ put64(h, off, address); put32(h, off + 8, len); put32(h, off + 12, flags); }

int
peach_mhi_host_start(PeachMhiHost *h, const PeachMhiIo *io, PeachMhiDma memory,
                     void (*barrier)(void *))
{
    if (!h || !io || !io->read32 || !io->write32 || !io->sync ||
        !io->now_us || !io->sleep_us || !barrier || !memory.bytes ||
        ((uintptr_t)memory.bytes & 7) || (memory.iova & 0xfff) ||
        memory.capacity < PEACH_MHI_HOST_BYTES || memory.iova >= 0x100000000ull ||
        PEACH_MHI_HOST_BYTES > 0x100000000ull - memory.iova || io->bar_bytes < 0x1000 ||
        !io->dma_bytes || io->dma_base >= 0x100000000ull ||
        io->dma_bytes > 0x100000000ull - io->dma_base || memory.iova < io->dma_base ||
        memory.iova - io->dma_base >= io->dma_bytes ||
        PEACH_MHI_HOST_BYTES > io->dma_bytes - (memory.iova - io->dma_base))
        return PeachMhiInvalid;
    memset(h, 0, sizeof(*h));
    h->memory = memory; h->io = io; h->barrier = barrier;
    uint32_t bytes, bhi;
    HOST_TRY(rd(h, 0, &bytes));
    HOST_TRY(rd(h, 0x28, &bhi));
    HOST_TRY(rd(h, 0x18, &h->channel_db));
    HOST_TRY(rd(h, 0x20, &h->event_db));
    if (bytes < 0xa8 || bytes > io->bar_bytes || (h->channel_db & 7) ||
        (h->event_db & 7) || h->channel_db < 0xa8 || h->event_db < 0xa8 ||
        h->channel_db > io->bar_bytes - 128 * 8 || h->event_db > io->bar_bytes - 2 * 8 ||
        (bhi & 3) || bhi < 0xa8 || bhi > io->bar_bytes - 0x44) {
        h->poisoned = 1; return PeachMhiInvalid;
    }
    h->bhi = bhi;
    HOST_TRY(poll_reg(h, 0x38, 2, 0));
    HOST_TRY(poll_reg(h, 0x48, 1, 1));
    memset(memory.bytes, 0, PEACH_MHI_HOST_BYTES);
    for (unsigned i = 0; i < 32; i++) {
        put32(h, CHCTX + i * CTX_BYTES, 2u << 8); /* burst disabled */
        if (i == 20 || i == 21) {
            put32(h, CHCTX + i * CTX_BYTES + 4, i == 20 ? 1 : 2);
            put32(h, CHCTX + i * CTX_BYTES + 8, 1);
        }
    }
    context_ring(h, CMDCTX, CMDRING, 128);
    for (unsigned e = 0; e < 2; e++) {
        unsigned base = e ? DATA_RING : CTRL_RING, n = e ? 256 : 32;
        unsigned ctx = EVCTX + e * CTX_BYTES;
        put32(h, ctx + 4, 1); put32(h, ctx + 8, e + 1);
        context_ring(h, ctx, base, n);
        h->event_wp[e] = n - 1;
        put64(h, ctx + 36, dma(h, base + (n - 1) * 16));
    }
    io->sync(io->context, &h->memory, PEACH_MHI_HOST_BYTES);
    HOST_TRY(db(h, 0x58, dma(h, CHCTX)));
    HOST_TRY(db(h, 0x60, dma(h, EVCTX)));
    HOST_TRY(db(h, 0x68, dma(h, CMDCTX)));
    /* MHI's host aperture also covers retained BHIe images/pageable code,
     * crash reception and later WLFW allocations, as mhi_init_mmio does.
     * SMMU mappings, not this address envelope, grant actual DMA access. */
    HOST_TRY(db(h, 0x80, io->dma_base));
    HOST_TRY(db(h, 0x98, io->dma_base));
    HOST_TRY(db(h, 0x88, io->dma_base + io->dma_bytes - 1));
    HOST_TRY(db(h, 0xa0, io->dma_base + io->dma_bytes - 1));
    uint32_t cfg, control;
    HOST_TRY(rd(h, 0x10, &cfg));
    HOST_TRY(wr(h, 0x10, (cfg & 0x0000ffffu) | 2u << 16));
    HOST_TRY(db(h, h->event_db, dma(h, CTRL_RING + 31 * 16)));
    HOST_TRY(db(h, h->event_db + 8, dma(h, DATA_RING + 255 * 16)));
    HOST_TRY(rd(h, 0x38, &control));
    HOST_TRY(wr(h, 0x38, (control & ~0xff00u) | 2u << 8));
    HOST_TRY(poll_reg(h, 0x48, 0xff00, 2u << 8));
    HOST_TRY(db(h, h->channel_db + 127 * 8, 1)); /* hold DEVICE_WAKE */
    h->state = 2;
    return 0;
}

static int recycle_rx(PeachMhiHost *h)
{
    unsigned index = h->rx_wp;
    descriptor(h, RX_RING + index * 16, dma(h, RX_DATA + index * PEACH_MHI_IPCR_MTU),
        PEACH_MHI_IPCR_MTU, 2u << 16 | 1u << 9);
    h->rx_wp = (index + 1) & 63;
    put64(h, CHCTX + 21 * CTX_BYTES + 36, dma(h, RX_RING + h->rx_wp * 16));
    return db(h, h->channel_db + 21 * 8, dma(h, RX_RING + h->rx_wp * 16));
}

static int process_event(PeachMhiHost *h, unsigned off)
{
    uint64_t pointer = get64(h, off);
    uint32_t d0 = get32(h, off + 8), d1 = get32(h, off + 12);
    h->last_event_pointer = pointer;
    h->last_event_data[0] = d0; h->last_event_data[1] = d1;
    unsigned type = (d1 >> 16) & 255, ch = d1 >> 24, code = d0 >> 24;
    if (type == 0x20) {
        h->state = d0 >> 24;
        if (h->state == 0xff) return PeachMhiDeviceError;
        if (h->state == 3) { /* M1 while wake is held: request M0. */
            uint32_t ctrl;
            HOST_TRY(rd(h, 0x38, &ctrl));
            HOST_TRY(wr(h, 0x38, (ctrl & ~0xff00u) | 2u << 8));
        }
        return 0;
    }
    if (type == 0x40) {
        h->environment = d0 >> 24;
        if (h->environment == 3 || (h->ipcr_up && h->environment != 2))
            return PeachMhiWrongEnvironment; /* RDDM/restart requires containment. */
        return 0;
    }
    if (type == 0x21) {
        unsigned index;
        HOST_TRY(index_of(h, pointer, CMDRING, 128, &index));
        if (!h->pending_cmd || pointer != h->pending_cmd || code != 1 ||
            get32(h, CMDRING + index * 16 + 12) >> 24 != h->pending_channel)
            return PeachMhiDeviceError;
        h->command_done = 1;
        return 0;
    }
    if (type != 0x22 || (ch != 20 && ch != 21)) return PeachMhiDeviceError;
    if (code == 5 || code == 6) { /* OOB/doorbell mode request consumes no TRE. */
        unsigned base = ch == 20 ? TX_RING : RX_RING, wp = ch == 20 ? h->tx_wp : h->rx_wp;
        return db(h, h->channel_db + ch * 8, dma(h, base + wp * 16));
    }
    unsigned index, base = ch == 20 ? TX_RING : RX_RING;
    HOST_TRY(index_of(h, pointer, base, 64, &index));
    if (code != 2 || (d0 & 65535) > PEACH_MHI_IPCR_MTU) return PeachMhiDeviceError;
    if (ch == 20) {
        if (h->tx_rp == h->tx_wp || index != h->tx_rp) return PeachMhiDeviceError;
        h->tx_rp = (index + 1) & 63;
    } else {
        if (index != h->rx_rp) return PeachMhiDeviceError;
        if (h->receive) h->receive(h->receive_context,
            (unsigned char *)h->memory.bytes + RX_DATA + index * PEACH_MHI_IPCR_MTU, d0 & 65535);
        h->rx_rp = (index + 1) & 63;
        HOST_TRY(recycle_rx(h));
    }
    return 0;
}

int
peach_mhi_host_poll(PeachMhiHost *h, unsigned budget)
{
    if (!h || !h->io || h->poisoned || !budget || budget > 512) return PeachMhiInvalid;
    uint32_t status;
    HOST_TRY(rd(h, 0x48, &status));
    h->last_status = status;
    if (status == 0xffffffffu || (status & 4)) { h->poisoned = 1; return PeachMhiDeviceError; }
    for (unsigned e = 0; e < 2 && budget; e++) {
        unsigned base = e ? DATA_RING : CTRL_RING, n = e ? 256 : 32;
        unsigned ctx = EVCTX + e * CTX_BYTES, device_index, consumed = 0;
        h->barrier(h->io->context);
        h->last_ring_pointer = get64(h, ctx + 28);
        HOST_TRY(index_of(h, h->last_ring_pointer, base, n, &device_index));
        while (h->event_rp[e] != device_index && budget) {
            h->barrier(h->io->context);
            HOST_TRY(process_event(h, base + h->event_rp[e] * 16));
            h->event_rp[e] = (h->event_rp[e] + 1) % n;
            h->event_wp[e] = (h->event_wp[e] + 1) % n;
            consumed++; budget--;
        }
        if (consumed) {
            uint64_t wp = dma(h, base + h->event_wp[e] * 16);
            put64(h, ctx + 36, wp);
            HOST_TRY(db(h, h->event_db + e * 8, wp));
        }
    }
    return 0;
}

static int start_channel(PeachMhiHost *h, unsigned ch)
{
    unsigned ring = ch == 20 ? TX_RING : RX_RING, ctx = CHCTX + ch * CTX_BYTES;
    context_ring(h, ctx, ring, 64);
    put32(h, ctx, 2u << 8 | 1); /* enabled, burst disabled */
    unsigned off = CMDRING + h->cmd_wp * 16;
    descriptor(h, off, 0, 0, ch << 24 | 0x12u << 16);
    h->pending_cmd = dma(h, off); h->pending_channel = ch; h->command_done = 0;
    h->cmd_wp = (h->cmd_wp + 1) & 127;
    put64(h, CMDCTX + 36, dma(h, CMDRING + h->cmd_wp * 16));
    HOST_TRY(db(h, 0x70, dma(h, CMDRING + h->cmd_wp * 16)));
    uint64_t end = h->io->now_us(h->io->context) + 10000000;
    while (!h->command_done && h->io->now_us(h->io->context) < end) {
        HOST_TRY(peach_mhi_host_poll(h, 32));
        if (!h->command_done) h->io->sleep_us(h->io->context, 1000);
    }
    if (!h->command_done) { h->poisoned = 1; return PeachMhiTimeout; }
    h->pending_cmd = 0;
    return 0;
}

int
peach_mhi_host_open_ipcr(PeachMhiHost *h)
{
    if (!h || !h->io || h->poisoned || h->ipcr_up) return PeachMhiInvalid;
    uint32_t ee;
    HOST_TRY(rd(h, h->bhi + 0x28, &ee));
    if (ee != 2) return PeachMhiWrongEnvironment;
    /* BHI can advertise AMSS before its control-ring handoff completes.
     * As in mhi_pm_mission_mode_transition, consume that real EE event
     * and reach M0 before a client submits its first START command. */
    uint64_t end = h->io->now_us(h->io->context) + 10000000;
    do {
        HOST_TRY(peach_mhi_host_poll(h, 32));
        if (h->environment == 2 && h->state == 2) break;
        h->io->sleep_us(h->io->context, 1000);
    } while (h->io->now_us(h->io->context) < end);
    if (h->environment != 2 || h->state != 2) {
        h->poisoned = 1; return PeachMhiTimeout;
    }
    HOST_TRY(start_channel(h, 20));
    HOST_TRY(start_channel(h, 21));
    for (unsigned i = 0; i < 63; i++) HOST_TRY(recycle_rx(h));
    h->ipcr_up = 1;
    return 0;
}

int
peach_mhi_host_send(PeachMhiHost *h, const void *packet, size_t bytes)
{
    if (!h || !h->ipcr_up || h->poisoned || !packet || !bytes || bytes > PEACH_MHI_IPCR_MTU)
        return PeachMhiInvalid;
    unsigned next = (h->tx_wp + 1) & 63;
    if (next == h->tx_rp) return -6;
    unsigned offset = TX_DATA + h->tx_wp * PEACH_MHI_IPCR_MTU;
    memcpy((unsigned char *)h->memory.bytes + offset, packet, bytes);
    descriptor(h, TX_RING + h->tx_wp * 16, dma(h, offset), (unsigned)bytes, 2u << 16 | 1u << 9);
    h->tx_wp = next;
    put64(h, CHCTX + 20 * CTX_BYTES + 36, dma(h, TX_RING + next * 16));
    HOST_TRY(db(h, h->channel_db + 20 * 8, dma(h, TX_RING + next * 16)));
    return 0;
}
