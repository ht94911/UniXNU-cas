/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Read-only qualification of the GPI stream already installed by firmware.
 * Current nezha: SID b6, slot 5 -> bank 5, SCTLR e0, CBAR 1f000, CBA2R 1.
 * CBAR.TYPE, not its memory-attribute bits, defines stage-2 bypass. No context
 * bank or stream entry is changed, including when qualification fails. */
#ifndef NEZHA_POWER_DMA_H
#define NEZHA_POWER_DMA_H
#include <stdint.h>

typedef uint32_t (*NpDmaRead32)(void *, uint32_t);
typedef struct {
    uint32_t id0, id1, id2, slot, smr, route, bank, cb;
    uint32_t sctlr, cbar, cba2r;
} NpDmaRoute;
enum {
    NP_DMA_OK, NP_DMA_GEOMETRY, NP_DMA_MISSING, NP_DMA_CONFLICT,
    NP_DMA_TRANSLATED, NP_DMA_CHANGED, NP_DMA_WIDTH
};

static inline const char *np_dma_reason(int error) {
    switch (error) {
    case NP_DMA_OK: return "firmware stream retained";
    case NP_DMA_GEOMETRY: return "unsupported SMMU geometry";
    case NP_DMA_MISSING: return "firmware GPI stream absent";
    case NP_DMA_CONFLICT: return "ambiguous or masked GPI stream";
    case NP_DMA_TRANSLATED: return "firmware GPI bank is not pass-through";
    case NP_DMA_CHANGED: return "firmware GPI route changed";
    case NP_DMA_WIDTH: return "DMA range exceeds declared address widths";
    default: return "invalid DMA qualification";
    }
}

static inline int np_dma_unchanged(NpDmaRead32 rd, void *ctx, const NpDmaRoute *p) {
    return rd(ctx,0x20)==p->id0 && rd(ctx,0x24)==p->id1 && rd(ctx,0x28)==p->id2 &&
        rd(ctx,0x800+4*p->slot)==p->smr && rd(ctx,0xc00+4*p->slot)==p->route &&
        rd(ctx,p->cb)==p->sctlr && rd(ctx,0x1000+4*p->bank)==p->cbar &&
        rd(ctx,0x1800+4*p->bank)==p->cba2r;
}

static inline int np_dma_plan(NpDmaRead32 rd, void *ctx, NpDmaRoute *p) {
    NpDmaRoute empty = {0,0,0,0,0,0,0,0,0,0,0};
    *p=empty; p->slot=UINT32_MAX;
    p->id0=rd(ctx,0x20);p->id1=rd(ctx,0x24);p->id2=rd(ctx,0x28);
    uint32_t count=p->id0&255, banks=p->id1&255, pages=2u<<((p->id1>>28)&7);
    if (!(p->id0&(1u<<30)) || !(p->id2&(1u<<12)) || (p->id1>>31) ||
        !count || count>128 || !banks || banks>128 || (pages+banks)*4096u>0x100000u)
        return NP_DMA_GEOMETRY;
    for (uint32_t i=0;i<count;++i) {
        uint32_t smr=rd(ctx,0x800+4*i);
        if (!(smr&0x80000000u) || ((0xb6u^smr)&~(smr>>16)&0x7fffu)) continue;
        if (p->slot!=UINT32_MAX || smr!=0x800000b6u) return NP_DMA_CONFLICT;
        p->slot=i;p->smr=smr;p->route=rd(ctx,0xc00+4*i);
    }
    if (p->slot==UINT32_MAX) return NP_DMA_MISSING;
    p->bank=p->route&255;
    if (((p->route>>16)&3) || p->bank>=banks) return NP_DMA_TRANSLATED;
    p->cb=(pages+p->bank)*4096u;
    p->sctlr=rd(ctx,p->cb);p->cbar=rd(ctx,0x1000+4*p->bank);p->cba2r=rd(ctx,0x1800+4*p->bank);
    if ((p->sctlr&1) || (p->cbar&0x30000u)!=0x10000u || p->cba2r!=1)
        return NP_DMA_TRANSLATED;
    return np_dma_unchanged(rd,ctx,p) ? NP_DMA_OK : NP_DMA_CHANGED;
}

static inline int np_dma_range(const NpDmaRoute *p, uint64_t pa, uint32_t bytes) {
    static const unsigned widths[]={32,36,40,42,44,48};
    if (!pa || !bytes || (pa&0xfff)) return NP_DMA_WIDTH;
    for (unsigned shift=0;shift<=8;shift+=4) {
        unsigned code=(p->id2>>shift)&15;
        if (code>=sizeof(widths)/sizeof(widths[0])) return NP_DMA_WIDTH;
        uint64_t limit=1ULL<<widths[code];
        if (pa>=limit || bytes>limit-pa) return NP_DMA_WIDTH;
    }
    return NP_DMA_OK;
}
#endif
