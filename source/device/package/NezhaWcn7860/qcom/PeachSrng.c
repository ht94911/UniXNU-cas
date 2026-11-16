/* SPDX-License-Identifier: GPL-2.0-only */
/* Peach v2: hal_peach.c SRNG table, hal_generic_api.h initialization,
 * hal_srng.c pointer ownership, wcss_seq_hwioreg_umac.h register offsets.
 * This is the retained-ring primitive, not a complete packet datapath. */
#include "PeachSrng.h"
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

typedef struct { uint32_t base, doorbell; unsigned id, dwords, source, lmac; } RingConfig;
static const RingConfig config[PeachSrngKinds] = {
    {0x00a38500, 0x00a3b048, 1,   8, 0, 0}, /* REO2SW1 */
    {0x00a388c0, 0x00a3b088, 0,   8, 0, 0}, /* REO2SW0 exception */
    {0x00a44918, 0x00a46000, 24,  8, 1, 0}, /* SW2TCL1 */
    {0x00a34f70, 0x00a370e0, 131, 8, 0, 0}, /* WBM2SW3 RX release */
    {0, 0, 0, 2, 1, 1},                    /* FW-controlled RX refill */
    {0, 0, 0, 8, 0, 1},                    /* FW-controlled RXDMA2SW */
    {0x00a34d3c, 0x00a370b8, 120, 2, 1, 0}, /* WBM idle link */
    {0x00a3437c, 0x00a37010, 121, 8, 1, 0}, /* SW2WBM release */
    {0x00a34e08, 0x00a370c8, 128, 8, 0, 0}, /* WBM2SW0 TX; SW1 belongs to FW */
};
#define STRY(r, expr) do { int err_ = (expr); if (err_) { (r)->error = err_; return err_; } } while (0)
static uint32_t u32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void barrier(PeachSrng *r) { r->ops.barrier(r->ops.context); }
static int invalid(PeachSrng *r) { return r->error = PeachCeProtocol; }
static int rd(PeachSrng *r, uint32_t o, uint32_t *v)
{
    r->last_register = o;
    int err = r->ops.read32(r->ops.context, o, v);
    r->last_value = err ? 0xffffffffu : *v;
    return err;
}
static int wr(PeachSrng *r, uint32_t o, uint32_t v)
{
    r->last_register = o; r->last_value = v;
    return r->ops.write32(r->ops.context, o, v);
}
size_t peach_srng_bytes(unsigned kind, unsigned entries)
{
    if (kind >= PeachSrngKinds || entries < 4 || entries > 65535u / config[kind].dwords)
        return 0;
    size_t bytes = (size_t)entries * config[kind].dwords * 4;
    return ((bytes + 63) & ~(size_t)63) + 128;
}
int peach_srng_prepare(PeachSrng *r, unsigned kind, unsigned entries,
                       PeachMhiDma memory, const PeachSrngOps *ops)
{
    size_t bytes = peach_srng_bytes(kind, entries);
    if (!r || !ops || !ops->read32 || !ops->write32 || !ops->health || !ops->barrier ||
        !bytes || !memory.bytes || ((uintptr_t)memory.bytes & 63) || (memory.iova & 63) ||
        memory.capacity < bytes || !memory.iova || memory.iova >= 0x100000000ull ||
        bytes > 0x100000000ull - memory.iova) return PeachMhiInvalid;
    memset(r, 0, sizeof(*r)); r->ops = *ops; r->kind = kind; r->entries = entries;
    r->memory = memory; r->dwords = config[kind].dwords; r->source = config[kind].source;
    r->lmac = config[kind].lmac; r->ring_id = config[kind].id;
    r->base = config[kind].base; r->doorbell = config[kind].doorbell; r->loop = 1;
    size_t descriptors = (size_t)entries * r->dwords * 4;
    size_t pointer = (descriptors + 63) & ~(size_t)63;
    r->descriptors = (PeachMhiDma){memory.bytes, memory.iova, descriptors};
    r->head = (PeachMhiDma){(unsigned char *)memory.bytes + pointer, memory.iova + pointer, 4};
    r->tail = (PeachMhiDma){(unsigned char *)memory.bytes + pointer + 64, memory.iova + pointer + 64, 4};
    memset(memory.bytes, 0, bytes); barrier(r); r->prepared = 1;
    return 0;
}
int peach_srng_start_host(PeachSrng *r)
{
    if (!r || !r->prepared || r->started || r->lmac) return PeachMhiInvalid;
    if (r->error) return r->error;
    STRY(r, r->ops.health(r->ops.context));
    uint32_t v;
    STRY(r, rd(r, r->base + 0x10, &v));
    if (v == 0xffffffffu || (v & 0x40)) return invalid(r); /* never replace live DMA */
    STRY(r, wr(r, r->base + 0x10, 0));
    STRY(r, wr(r, r->base, (uint32_t)r->descriptors.iova));
    if (r->kind == PeachSrngIdleLink) {
        STRY(r, rd(r, r->base, &v));
        if (v != (uint32_t)r->descriptors.iova) return invalid(r);
    }
    STRY(r, wr(r, r->base + 4, r->entries * r->dwords << 8));
    STRY(r, wr(r, r->base + 8, r->dwords | (r->source ? 0 : r->ring_id << 8)));
    /* Direct target doorbells, with coherent remote hardware pointer.
     * No MSI, near-full interrupt, pointer coalescing or shadow acceleration. */
    STRY(r, wr(r, r->base + 0x4c, 0));
    if (!r->source) {
        STRY(r, wr(r, r->base + 0x5c, 0));
        STRY(r, wr(r, r->base + 0x54, 0));
        STRY(r, wr(r, r->base + 0x74, 0));
    }
    STRY(r, wr(r, r->base + (r->source ? 0x30 : 0x24), 0));
    if (r->source) STRY(r, wr(r, r->base + 0x34, 0));
    /* WBM owns its idle list producer and consumer. Vendor HAL explicitly
     * forbids a remote pointer here because it destabilizes WBM. */
    PeachMhiDma pointer = r->source ? r->tail : r->head;
    if (r->kind == PeachSrngIdleLink) pointer.iova = 0;
    STRY(r, wr(r, r->base + (r->source ? 0x1c : 0x14), (uint32_t)pointer.iova));
    STRY(r, wr(r, r->base + (r->source ? 0x20 : 0x18), 0));
    STRY(r, wr(r, r->doorbell, 0));
    STRY(r, wr(r, r->doorbell + 4, 0));
    barrier(r);
    uint32_t misc = 0x40 | (r->source ? 2 : 0);
    if (r->kind == PeachSrngIdleLink) misc |= 1; /* ring ID disabled */
    STRY(r, wr(r, r->base + 0x10, misc));
    STRY(r, rd(r, r->base + 0x10, &v));
    if ((v & 0x7f) != misc) return invalid(r);
    r->started = 1;
    return 0;
}
int peach_srng_seed_idle(PeachSrng *r, PeachMhiDma links, unsigned count)
{
    if (!r || !r->started || r->kind != PeachSrngIdleLink || r->wp ||
        !count || count >= r->entries || !links.bytes ||
        ((uintptr_t)links.bytes & 127) || !links.iova || (links.iova & 127) ||
        links.iova >= 0x100000000ull || links.capacity < (size_t)count * 128 ||
        (size_t)count * 128 > 0x100000000ull - links.iova)
        return PeachMhiInvalid;
    if (r->error) return r->error;
    if (links.iova < r->memory.iova + r->memory.capacity &&
        r->memory.iova < links.iova + (size_t)count * 128) return PeachMhiInvalid;
    STRY(r, r->ops.health(r->ops.context));
    /* hal_get_link_desc_size_peach: 32 DWORDs; dp_link_desc_ring_replenish:
     * clear pool, then post buffer_addr_info with WBM_IDLE_DESC_LIST=1.
     * Cookies are software indices only; never kernel virtual addresses. */
    memset(links.bytes, 0, (size_t)count * 128);
    uint32_t *p = r->descriptors.bytes;
    for (unsigned i = 0; i < count; i++) {
        p[i * 2] = (uint32_t)(links.iova + (size_t)i * 128);
        p[i * 2 + 1] = 1u << 8 | i << 12;
    }
    r->wp = count;
    barrier(r);
    STRY(r, wr(r, r->doorbell, count * r->dwords));
    return 0;
}
int peach_srng_start_lmac(PeachSrng *r, PeachHtt *h, unsigned pdev, unsigned id, unsigned type)
{
    if (!r || !r->prepared || r->started || !r->lmac || !h) return PeachMhiInvalid;
    if (r->error) return r->error;
    if (r->kind == PeachSrngRxBuffer) {
        if (!((id == 0 && type == PeachHttSwToHw) || (id == 5 && type == PeachHttSwToSw)))
            return PeachMhiInvalid;
    } else if (id != 7 || type != PeachHttHwToSw) return PeachMhiInvalid;
    STRY(r, r->ops.health(r->ops.context));
    PeachHttRing request = {r->descriptors, r->head, r->tail, pdev, id, type, r->entries, r->dwords};
    STRY(r, peach_htt_setup_ring(h, &request));
    r->started = 1;
    return 0;
}
static int hardware_pointer(PeachSrng *r, unsigned *index)
{
    STRY(r, r->ops.health(r->ops.context));
    barrier(r);
    uint32_t offset = *(volatile uint32_t *)(r->source ? r->tail.bytes : r->head.bytes);
    if (offset >= r->entries * r->dwords || offset % r->dwords) return invalid(r);
    *index = offset / r->dwords; barrier(r);
    return 0;
}
static unsigned distance(PeachSrng *r, unsigned a, unsigned b)
{ return a >= b ? a - b : r->entries - b + a; }
static int publish(PeachSrng *r)
{
    barrier(r);
    uint32_t offset = (r->source ? r->wp : r->rp) * r->dwords;
    if (r->lmac) {
        *(volatile uint32_t *)(r->source ? r->head.bytes : r->tail.bytes) = offset;
        barrier(r);
        return 0;
    }
    return wr(r, r->doorbell + (r->source ? 0 : 4), offset);
}
int peach_srng_push(PeachSrng *r, const void *descriptor, size_t n)
{
    if (!r || !r->started || !r->source || r->kind == PeachSrngIdleLink ||
        !descriptor || n != r->dwords * 4)
        return PeachMhiInvalid;
    if (r->error) return r->error;
    unsigned tail;
    STRY(r, hardware_pointer(r, &tail));
    if (distance(r, tail, r->rp) > distance(r, r->wp, r->rp)) return invalid(r);
    r->rp = tail;
    unsigned next = r->wp + 1;
    if (next == r->entries) next = 0;
    if (next == r->rp) return PeachCeFull;
    memcpy((unsigned char *)r->descriptors.bytes + r->wp * n, descriptor, n);
    r->wp = next;
    STRY(r, publish(r));
    return 0;
}
int peach_srng_pop(PeachSrng *r, void *descriptor, size_t n)
{
    if (!r || !r->started || r->source || !descriptor || n != r->dwords * 4)
        return PeachMhiInvalid;
    if (r->error) return r->error;
    unsigned head;
    STRY(r, hardware_pointer(r, &head));
    if (head == r->rp) return PeachSrngEmpty;
    unsigned char *p = (unsigned char *)r->descriptors.bytes + r->rp * n;
    /* A published pointer alone does not prove descriptor stores completed.
     * Peach destination formats put the expected loop in the final nibble. */
    if ((u32(p + n - 4) >> 28) != r->loop) return PeachSrngEmpty;
    barrier(r); memcpy(descriptor, p, n);
    r->rp++;
    if (r->rp == r->entries) { r->rp = 0; r->loop = (r->loop + 1) & 15; }
    STRY(r, publish(r));
    return 0;
}
