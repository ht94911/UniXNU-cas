/* SPDX-License-Identifier: GPL-2.0-only */
/* Register fields: fw-api/hw/peach/v2/soc_ce_seq_hwioreg.h and ce_*_desc.h.
 * Ownership/assignment: hif/src/ce/{ce_assignment.h,ce_main.c}; SRNG setup:
 * hal/wifi3.0/{hal_generic_api.h,hal_srng.c}. No Linux ABI is used. */
#include "PeachCe.h"
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

const PeachCeTarget peach_ce_target[9] = {
    {0, 2, 32, 2048, 0}, {1, 1, 32, 2048, 0},
    {2, 1, 32, 3520, 0}, {3, 2, 32, 3520, 0},
    {4, 2, 256, 256, 8}, {5, 1, 0, 2048, 0},
    {6, 3, 32, 16384, 0}, {7, 4, 0, 0, 8}, {8, 3, 32, 16384, 0},
};
const PeachCeService peach_ce_services[15] = {
    {0x101, 2, 3}, {0x101, 1, 2}, {0x102, 2, 3}, {0x102, 1, 2},
    {0x103, 2, 3}, {0x103, 1, 2}, {0x104, 2, 3}, {0x104, 1, 2},
    {0x100, 2, 3}, {0x100, 1, 2}, {1, 2, 0}, {1, 1, 2},
    {0x300, 2, 4}, {0x300, 1, 1}, {0, 0, 0},
};
/* SRNG pointers and lengths are in dwords, never byte addresses. */
enum { CE_BASE = 0x01b80000, STRIDE = 0x2000, ENABLE = 0x40 };
#define CTRY(c, expr) do { int err_ = (expr); if (err_) { (c)->error = err_; return err_; } } while (0)
static uint32_t get32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void put32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static void barrier(PeachCe *c) { c->ops.barrier(c->ops.context); }
static int read32(PeachCe *c, uint32_t r, uint32_t *v)
{
    c->last_register = r;
    int err = c->ops.read32(c->ops.context, r, v);
    c->last_value = err ? 0xffffffffu : *v;
    return err;
}
static int write32(PeachCe *c, uint32_t r, uint32_t v)
{
    c->last_register = r; c->last_value = v;
    return c->ops.write32(c->ops.context, r, v);
}
static int invalid(PeachCe *c) { return c->error = PeachCeProtocol; }

static int carve(PeachCe *c, size_t *used, size_t bytes, PeachMhiDma *out)
{
    size_t start = (*used + 63) & ~(size_t)63;
    if (start > c->memory.capacity || bytes > c->memory.capacity - start) return PeachMhiInvalid;
    *out = (PeachMhiDma){(unsigned char *)c->memory.bytes + start, c->memory.iova + start, bytes};
    *used = start + bytes;
    return 0;
}
static int prepare_ring(PeachCe *c, PeachCeRing *r, size_t *used,
                         uint32_t base, uint32_t doorbell, unsigned n, unsigned dw)
{
    r->base = base; r->doorbell = doorbell; r->entries = n; r->dwords = dw;
    CTRY(c, carve(c, used, n * dw * 4, &r->descriptors));
    return carve(c, used, 4, &r->pointer);
}
int peach_ce_prepare(PeachCe *c, PeachMhiDma memory, const PeachCeOps *ops)
{
    if (!c || !ops || !ops->read32 || !ops->write32 || !ops->barrier ||
        !ops->receive || !memory.bytes || ((uintptr_t)memory.bytes & 63) ||
        (memory.iova & 63) || memory.capacity < PEACH_CE_DMA_BYTES ||
        memory.iova >= 0x100000000ull || memory.capacity > 0x100000000ull - memory.iova)
        return PeachMhiInvalid;
    memset(c, 0, sizeof(*c)); c->ops = *ops; c->memory = memory;
    memset(memory.bytes, 0, PEACH_CE_DMA_BYTES);
    const unsigned entries[5] = {16, 512, 256, 32, 256};
    const unsigned limits[5] = {2048, 2048, 3520, 3520, 256};
    size_t used = 0;
    for (unsigned i = 0; i < PEACH_CE_CHANNELS; i++) {
        PeachCeChannel *ch = &c->channel[i];
        ch->receive = i == 1 || i == 2; ch->limit = limits[i];
        uint32_t base = CE_BASE + i * STRIDE + (ch->receive ? 0x1000 : 0);
        CTRY(c, prepare_ring(c, &ch->ring, &used, base, base + 0x400,
            entries[i], ch->receive ? 2 : 4));
        if (ch->receive)
            CTRY(c, prepare_ring(c, &ch->status, &used, base + 0x58,
                base + 0x408, entries[i], 4));
        CTRY(c, carve(c, &used, entries[i] * limits[i], &ch->buffers));
    }
    barrier(c); c->prepared = 1;
    return 0;
}

static int start_ring(PeachCe *c, PeachCeRing *r, unsigned status, unsigned ringid)
{
    uint32_t v;
    CTRY(c, read32(c, r->base + 0x10, &v));
    /* A freshly reset host-owned CE must be inactive. Never overwrite a
     * live ring, including if firmware ownership differs from this ABI. */
    if (v == 0xffffffffu || (v & ENABLE)) return invalid(c);
    CTRY(c, write32(c, r->base + 0x10, 0));
    CTRY(c, write32(c, r->base, (uint32_t)r->descriptors.iova));
    CTRY(c, write32(c, r->base + 4, (r->entries * r->dwords) << 8));
    CTRY(c, write32(c, r->base + 8, r->dwords | (status ? ringid << 8 : 0)));
    /* Explicitly turn off prior MSI state and timer/threshold interrupts. */
    CTRY(c, write32(c, r->base + 0x4c, 0));
    CTRY(c, write32(c, r->base + (status ? 0x24 : 0x30), 0));
    if (!status) CTRY(c, write32(c, r->base + 0x34, 0));
    CTRY(c, write32(c, r->base + (status ? 0x14 : 0x1c), (uint32_t)r->pointer.iova));
    CTRY(c, write32(c, r->base + (status ? 0x18 : 0x20), 0));
    CTRY(c, write32(c, r->doorbell, 0));
    CTRY(c, write32(c, r->doorbell + 4, 0));
    barrier(c);
    CTRY(c, write32(c, r->base + 0x10, ENABLE | (status ? 0 : 2)));
    CTRY(c, read32(c, r->base + 0x10, &v));
    if ((v & 0x7f) != (ENABLE | (status ? 0u : 2u))) return invalid(c);
    return 0;
}
static void post_rx(PeachCeChannel *ch, unsigned buffer)
{
    PeachCeRing *r = &ch->ring;
    unsigned char *d = (unsigned char *)r->descriptors.bytes + r->wp * 8;
    uint64_t addr = ch->buffers.iova + (uint64_t)buffer * ch->limit;
    put32(d, (uint32_t)addr); put32(d + 4, (uint32_t)(addr >> 32));
    r->wp = (r->wp + 1) & (r->entries - 1);
}
int peach_ce_start(PeachCe *c)
{
    if (!c || !c->prepared || c->started) return PeachMhiInvalid;
    if (c->error) return c->error;
    for (unsigned i = 0; i < PEACH_CE_CHANNELS; i++) {
        PeachCeChannel *ch = &c->channel[i];
        if (ch->receive) {
            uint32_t v;
            CTRY(c, read32(c, ch->ring.base + 0xb0, &v));
            CTRY(c, write32(c, ch->ring.base + 0xb0, (v & ~0xffffu) | ch->limit));
            CTRY(c, start_ring(c, &ch->status, 1, 100 + i));
        }
        CTRY(c, start_ring(c, &ch->ring, 0, 0));
        if (ch->receive) {
            /* Vendor CE workaround leaves two entries unposted. Buffers are
             * reclaimed in order; rp identifies the next completed buffer. */
            for (unsigned j = 0; j < ch->ring.entries - 2; j++) post_rx(ch, j);
            barrier(c);
            CTRY(c, write32(c, ch->ring.doorbell, ch->ring.wp * ch->ring.dwords));
        }
    }
    c->started = 1;
    return 0;
}

static int hardware_index(PeachCe *c, PeachCeRing *r, unsigned *index)
{
    barrier(c);
    uint32_t dw = *(volatile uint32_t *)r->pointer.bytes;
    if (dw >= r->entries * r->dwords || dw % r->dwords) return invalid(c);
    *index = dw / r->dwords;
    barrier(c);
    return 0;
}
static int reap_tx(PeachCe *c, PeachCeRing *r)
{
    unsigned tail;
    CTRY(c, hardware_index(c, r, &tail));
    unsigned complete = (tail - r->rp) & (r->entries - 1);
    unsigned posted = (r->wp - r->rp) & (r->entries - 1);
    if (complete > posted) return invalid(c);
    r->rp = tail;
    return 0;
}
int peach_ce_send(PeachCe *c, unsigned pipe, unsigned metadata, const void *p, size_t n)
{
    if (!c || !c->started || pipe >= PEACH_CE_CHANNELS || !p || !n || metadata > 0xffff)
        return PeachMhiInvalid;
    if (c->error) return c->error;
    PeachCeChannel *ch = &c->channel[pipe];
    PeachCeRing *r = &ch->ring;
    if (ch->receive || n > ch->limit) return PeachMhiInvalid;
    CTRY(c, reap_tx(c, r));
    unsigned next = (r->wp + 1) & (r->entries - 1);
    if (next == r->rp) return PeachCeFull;
    size_t off = r->wp * ch->limit;
    memcpy((unsigned char *)ch->buffers.bytes + off, p, n);
    unsigned char *d = (unsigned char *)r->descriptors.bytes + r->wp * 16;
    uint64_t addr = ch->buffers.iova + off;
    put32(d, (uint32_t)addr); put32(d + 4, (uint32_t)(addr >> 32) | (uint32_t)n << 16);
    put32(d + 8, metadata); put32(d + 12, 0);
    barrier(c); r->wp = next;
    CTRY(c, write32(c, r->doorbell, r->wp * r->dwords));
    return 0;
}

static int receive_one(PeachCe *c, unsigned pipe, unsigned *received)
{
    PeachCeChannel *ch = &c->channel[pipe];
    PeachCeRing *s = &ch->status, *r = &ch->ring;
    unsigned head;
    CTRY(c, hardware_index(c, s, &head));
    if (head == s->rp) return 0;
    unsigned char *d = (unsigned char *)s->descriptors.bytes + s->rp * 16;
    uint32_t info = get32(d), meta = get32(d + 12);
    unsigned n = info >> 16;
    /* Hardware can publish HP just before the status length becomes visible.
     * Defer rather than consume an incomplete descriptor (vendor CE race). */
    if (!n) return 0;
    if (n > ch->limit || (info & 0x1f00) || ((meta >> 20) & 0xff) != 100 + pipe)
        return invalid(c);
    unsigned buffer = r->rp;
    c->ops.receive(c->ops.receive_context, pipe,
        (unsigned char *)ch->buffers.bytes + buffer * ch->limit, n);
    put32(d, 0);
    s->rp = (s->rp + 1) & (s->entries - 1);
    /* Move this consumed buffer into the next free descriptor. The buffer
     * sequence remains in-order even though two ring entries stay empty. */
    /* Only entries-2 buffers circulate; r->rp is a buffer index on RX. */
    r->rp++;
    if (r->rp == r->entries - 2) r->rp = 0;
    post_rx(ch, buffer);
    barrier(c);
    CTRY(c, write32(c, s->doorbell + 4, s->rp * s->dwords));
    CTRY(c, write32(c, r->doorbell, r->wp * r->dwords));
    *received = 1;
    return 0;
}
int peach_ce_poll(PeachCe *c, unsigned budget)
{
    if (!c || !c->started || !budget || c->polling) return PeachMhiInvalid;
    if (c->error) return c->error;
    c->polling = 1;
    int err = 0;
    for (unsigned i = 0; i < PEACH_CE_CHANNELS && !err; i++)
        if (!c->channel[i].receive) err = reap_tx(c, &c->channel[i].ring);
    for (unsigned done = 0; done < budget && !err;) {
        unsigned received = 0;
        unsigned pipe = 1 + (c->next_channel++ & 1);
        err = receive_one(c, pipe, &received);
        if (err) break;
        if (!received) {
            pipe = 3 - pipe;
            err = receive_one(c, pipe, &received);
            if (!received) break;
        }
        done += received;
    }
    c->polling = 0;
    return err;
}
