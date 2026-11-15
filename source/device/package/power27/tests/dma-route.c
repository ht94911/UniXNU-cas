/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Current audio004 read-only census plus invalid/changed resource cases. */
#include "NezhaPowerDma.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t registers[0x100000/4], original[0x100000/4];
static uint32_t flipOffset, flipReads;
static uint32_t read32(void *ctx,uint32_t off) {
    (void)ctx; assert(!(off&3) && off<sizeof(registers));
    uint32_t value=registers[off/4];
    if(off==flipOffset && ++flipReads>1)value^=1;
    return value;
}
static void seed(void) {
    memset(registers,0,sizeof(registers));flipOffset=UINT32_MAX;flipReads=0;
    registers[0x20/4]=0x4c019274;registers[0x24/4]=0x60000051;registers[0x28/4]=0x5211;
    registers[0x800/4]=0x80000060;registers[0xc00/4]=0;
    registers[0x80000/4]=0x0a9f00e0;registers[0x1000/4]=0x1f000;registers[0x1800/4]=1;
    registers[(0x800+5*4)/4]=0x800000b6;registers[(0xc00+5*4)/4]=5;
    registers[0x85000/4]=0xe0;registers[(0x1000+5*4)/4]=0x1f000;registers[(0x1800+5*4)/4]=1;
    /* The companion QUP SID shares bank 5 and must not be altered. */
    registers[(0x800+6*4)/4]=0x800000a3;registers[(0xc00+6*4)/4]=5;
}
static NpDmaRoute expect(int result) {
    NpDmaRoute p;
    memcpy(original,registers,sizeof(registers));
    assert(np_dma_plan(read32,0,&p)==result);
    assert(!memcmp(original,registers,sizeof(registers)));
    return p;
}
int main(void) {
    seed();NpDmaRoute p=expect(NP_DMA_OK);
    assert(p.slot==5 && p.bank==5 && p.route==5 && p.cb==0x85000 && p.cbar==0x1f000);
    assert(np_dma_range(&p,0x8d04dc000ULL,0x10000)==NP_DMA_OK);
    assert(np_dma_range(&p,(1ULL<<36)-4096,4096)==NP_DMA_OK);
    assert(np_dma_range(&p,(1ULL<<36)-4096,4097)==NP_DMA_WIDTH);
    assert(np_dma_range(&p,1ULL<<36,4096)==NP_DMA_WIDTH);
    assert(np_dma_range(&p,0x8d04dc001ULL,4096)==NP_DMA_WIDTH);
    assert(np_dma_range(&p,0x8d04dc000ULL,0)==NP_DMA_WIDTH);
    p.id2=0x5261;assert(np_dma_range(&p,0x8d04dc000ULL,4096)==NP_DMA_WIDTH);
    seed();registers[0x85000/4]|=1;expect(NP_DMA_TRANSLATED);
    seed();registers[(0x1000+5*4)/4]=0xf000;expect(NP_DMA_TRANSLATED);
    seed();registers[(0x1800+5*4)/4]=0;expect(NP_DMA_TRANSLATED);
    seed();registers[(0xc00+5*4)/4]=0x10005;expect(NP_DMA_TRANSLATED);
    seed();registers[(0xc00+5*4)/4]=81;expect(NP_DMA_TRANSLATED);
    seed();registers[(0x800+5*4)/4]=0;expect(NP_DMA_MISSING);
    seed();registers[(0x800+5*4)/4]=0x800100b6;expect(NP_DMA_CONFLICT);
    seed();registers[(0x800+9*4)/4]=0x800000b6;expect(NP_DMA_CONFLICT);
    seed();registers[0x20/4]=0x4c019200;expect(NP_DMA_GEOMETRY);
    seed();registers[0x24/4]|=0x80000000;expect(NP_DMA_GEOMETRY);
    seed();registers[0x28/4]&=~0x1000u;expect(NP_DMA_GEOMETRY);
    seed();flipOffset=0x1000+5*4;expect(NP_DMA_CHANGED);
    seed();p=expect(NP_DMA_OK);registers[(0xc00+5*4)/4]=0;
    assert(!np_dma_unchanged(read32,0,&p));
    puts("PASS: actual firmware GPI bank retained; conflicts, translations, changes and DMA bounds rejected; no SMMU writes");
    return 0;
}
