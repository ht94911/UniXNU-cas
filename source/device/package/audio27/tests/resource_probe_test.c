/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../platform/AudioResourceProbe.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t mmio[0x100000/4],calls,fail_at,flip_at;
static NaResourceSnapshot snapshot;
static int read32(void *p,unsigned d,uint32_t o,uint32_t *v) {
    (void)p;++calls;if (calls==fail_at) return -1;
    assert(!(o&3));
    if (d==NaProbeSmmu) {assert(o<sizeof mmio);*v=mmio[o/4];}
    else if (d==NaProbeRsc) {assert(o<0x10000);*v=o==0 ? 0x40300:o==0xc ? 0x800c8104:0;}
    else {assert(d==NaProbeGcc && o==0x18);*v=0x07c00000;}
    if (calls==flip_at) *v^=1;
    return 0;
}
static int read64(void *p,unsigned d,uint32_t o,uint64_t *v) {
    (void)p;++calls;if (calls==fail_at) return -1;
    assert(d==NaProbeSmmu && !(o&7) && o<=sizeof mmio-8);
    *v=(uint64_t)mmio[o/4+1]<<32|mmio[o/4];return 0;
}
static void setup(unsigned banks) {
    memset(mmio,0,sizeof mmio);calls=fail_at=flip_at=0;
    mmio[0x20/4]=(1u<<30)|(1u<<27)|128;
    mmio[0x24/4]=(6u<<28)|(1u<<16)|banks; /* 128 pages, skip stage2 bank0 */
    mmio[0x28/4]=0x1322;
    for (unsigned i=0;i<128;++i) {mmio[(0x800+4*i)/4]=i==0 ? 0x80000060:i==7 ? 0x80000536:0;mmio[(0xc00+4*i)/4]=0;}
    for (unsigned i=1;i<banks;++i) {
        unsigned b=0x80000+4096*i;
        mmio[b/4]=i==1 ? 0xa9f00e0:0;mmio[(b+4)/4]=3;
        mmio[(b+0x24)/4]=(i<<16)|0x1234;mmio[(b+0x2c)/4]=(i<<16)|0x4321;
    }
}
int main(void) {
    NaResourceReader r={NULL,read32,read64};setup(128);
    assert(na_resource_capture(&snapshot,&r)==0);
    assert(snapshot.phase==6 && snapshot.stream_count==128 && snapshot.bank_count==128);
    assert(snapshot.stream[0].smr==0x80000060 && snapshot.stream[7].smr==0x80000536);
    assert(snapshot.bank[1].asid0==1 && snapshot.bank[127].asid1==127 && snapshot.bank[0].sctlr==0);
    assert(snapshot.gcc_vote==0x07c00000 && snapshot.rsc_id==0x40300);
    unsigned complete_reads=calls;assert(complete_reads<1400 && complete_reads==snapshot.reads);
    /* Every actual read can fail. Capture must stop at that precise operation,
     * preserve the error, and never publish a complete snapshot. */
    for (unsigned i=1;i<=complete_reads;++i) {
        calls=0;fail_at=i;assert(na_resource_capture(&snapshot,&r)==NA_IO);
        assert(calls==i && snapshot.reads==i && snapshot.phase!=6);
    }
    setup(4);assert(na_resource_capture(&snapshot,&r)==0);
    unsigned reread=calls-3-2*128+1;
    calls=0;flip_at=reread;assert(na_resource_capture(&snapshot,&r)==NA_BUSY);
    setup(4);mmio[0x20/4]|=255;assert(na_resource_capture(&snapshot,&r)==NA_MISSING && calls==3);
    setup(4);mmio[0x24/4]|=1u<<31;assert(na_resource_capture(&snapshot,&r)==NA_MISSING && calls==3);
    setup(4);mmio[0x24/4]=(6u<<28)|(5u<<16)|4;assert(na_resource_capture(&snapshot,&r)==NA_MISSING && calls==3);
    puts("resource probe: all reads fail closed, limits/ASIDs/changed routes checked; no write interface");
    return 0;
}
