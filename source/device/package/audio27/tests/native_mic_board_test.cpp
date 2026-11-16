// SPDX-License-Identifier: BSD-2-Clause-Patent
// Include unchanged production adapter; only trace/MMIO/time providers are fake.
#define NEZHA_AUDIO_XPU_TRACE_H
#include "../os/NezhaAudioMicBoard.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
static bool locked;
static uint64_t now,trace_delay;
static bool trace_enabled=true;
static bool na_xpu_trace_enabled(void) {return trace_enabled;}
static int na_xpu_trace_before(const char *,uint64_t,uint64_t,uint32_t=0,uint32_t=0) {assert(!locked);if (trace_enabled) now+=trace_delay;return 1;}
static void na_xpu_trace_after(const char *,uint64_t,int,uint32_t=0,uint64_t=0) {assert(!locked);}
#include "../os/NezhaAudioMicBoard.cpp"
static uint32_t rsc[0x10000/4];
static unsigned map_calls,map_failure,write_calls,write_failure;
static std::vector<void *> allocations;
static void put16(uint8_t *p,uint16_t n) {p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void put32(uint8_t *p,uint32_t n) {put16(p,(uint16_t)n);put16(p+2,(uint16_t)(n>>16));}
static void fill_database(uint8_t *p) {
    put32(p+4,QCOM_CMD_DB_MAGIC);put16(p+8,4);put16(p+12,72);put16(p+14,3);put16(p+16,0x100);
    const char *names[]={"L15B_E0","B1B_E0","L1G_E0"};const uint32_t a[]={0x40b00,0x40000,0x42300};
    for (unsigned i=0;i<3;++i) {auto *e=p+144+24*i;memcpy(e,names[i],strlen(names[i]));put32(e+16,a[i]);put16(e+20,4);put16(e+22,(uint16_t)(i*4));}
}
extern "C" int na_native_map(NaNativeWindow *w,NaRange r,uint64_t lease) {
    assert(!locked && lease);if (++map_calls==map_failure) return NA_IO;
    void *m=calloc(1,(size_t)r.bytes);assert(m);allocations.push_back(m);
    w->mapping=m;w->descriptor=m;w->va=static_cast<uint8_t *>(m);w->lease=lease;w->physical=r;
    if (r.base==0x81c60000) fill_database(static_cast<uint8_t *>(m));
    return NA_OK;
}
extern "C" uint64_t na_native_now_us(void *) {return now;}
extern "C" void na_native_pause_us(void *,uint32_t n) {assert(!locked);now+=n;}
static int config_lock(void *) {assert(!locked);locked=true;return 0;}
static void config_unlock(void *) {assert(locked);locked=false;}
static int rd32(void *,unsigned d,uint32_t o,uint32_t *v) {assert(locked && d==Sm8850Rsc && o<sizeof rsc);*v=rsc[o/4];return 0;}
static int wr32(void *,unsigned d,uint32_t o,uint32_t v) {
    assert(locked && d==Sm8850Rsc && o<sizeof rsc);++write_calls;
    if (o==0xd08) {assert(v==4);rsc[0xd04/4]&=~v;}else rsc[o/4]=v;
    return write_calls==write_failure ? -1:0;
}
static int rd64(void *,unsigned,uint32_t,uint64_t *) {abort();}
static int barrier(void *) {assert(locked);return 0;}
static void cleanup(void) {for (void *p:allocations) free(p);allocations.clear();}
static void init(Sm8850Resources *c,NaNativeMicBoard *b,NaBoardIo *io) {
    cleanup();memset(rsc,0,sizeof rsc);map_calls=map_failure=write_calls=write_failure=0;now=trace_delay=0;trace_enabled=true;locked=false;
    rsc[0]=0x40300;rsc[3]=0x800c8104;rsc[(0xd00+2*0x2a0+0x28)/4]=1;
    rsc[0xd00/4]=0xa1;rsc[0xd04/4]=8; /* other TCS bits survive every update */
    *c={};*b={};Sm8850ResourceIo hardware={nullptr,config_lock,config_unlock,rd32,wr32,rd64,barrier};
    assert(!sm8850_resources_init(c,&hardware));assert(!na_native_mic_board(b,c,io));
}
static void complete_tcs(void) {
    rsc[0xd04/4]|=4;
    for (unsigned i=0;i<3;++i) rsc[(0xd00+2*0x2a0+0x40+24*i)/4]=0x10100;
}
int main(void) {
    Sm8850Resources c{};NaNativeMicBoard b{};NaBoardIo io{};init(&c,&b,&io);uint64_t lease=0;
    assert(io.claim(io.context,NaBoardTas,&lease)==NA_OWNERSHIP && !map_calls);
    assert(!io.claim(io.context,NaBoardCapture,&lease) && lease && map_calls==6);
    assert(io.trace_wait_us==&b.trace_wait_us);
    uint32_t v=0;
    trace_delay=15000;
    assert(!io.read(io.context,lease,NA_TLMM_GPIO_BASE+161*4096,&v));
    assert(b.trace_wait_us==15000 && now==15000);
    trace_enabled=false;
    assert(!io.read(io.context,lease,NA_TLMM_GPIO_BASE+161*4096,&v));
    assert(b.trace_wait_us==15000 && now==15000);
    trace_enabled=true;trace_delay=0;
    const uint64_t denied[]={NA_TLMM_GPIO_BASE+54*4096,NA_TLMM_GPIO_BASE+162*4096,0xf0a1000,0x776d000,0x81c60000,0x152018};
    for (uint64_t pa:denied) {assert(io.read(io.context,lease,pa,&v)==NA_OWNERSHIP);assert(io.update(io.context,lease,pa,1,1)==NA_OWNERSHIP);}
    assert(!io.update(io.context,lease,NA_TLMM_GPIO_BASE+161*4096,0x3ff,0x3c0));
    assert(!io.read(io.context,lease,NA_TLMM_GPIO_BASE+161*4096,&v) && v==0x3c0);
    assert(io.update(io.context,lease,NA_TLMM_GPIO_BASE+161*4096,0x400,0)==NA_OWNERSHIP);
    assert(io.update(io.context,lease+1,NA_TLMM_GPIO_BASE+161*4096,2,0)==NA_OWNERSHIP);
    assert(!io.update(io.context,lease,0x776e000,0x1bff,0x842));
    assert(io.release(io.context,NaBoardCapture,lease)==0);uint64_t old=lease;
    assert(!io.claim(io.context,NaBoardCapture,&lease) && lease!=old && map_calls==6);
    assert(io.read(io.context,old,0x7760000,&v)==NA_OWNERSHIP);
    assert(io.mic_rails(io.context,lease,1)==NA_PENDING && b.tcs_lease && b.irq_saved);
    assert(rsc[0xd00/4]==0xa5 && rsc[0xd04/4]==8);
    assert(io.release(io.context,NaBoardCapture,lease)==NA_OWNERSHIP);
    for (unsigned i=0;i<2;++i) {complete_tcs();now+=1000;assert(io.mic_rails(io.context,lease,1)==NA_PENDING);}
    complete_tcs();now+=1000;assert(!io.mic_rails(io.context,lease,1));
    assert(!b.tcs_lease && !b.irq_saved && b.power.held==7 && rsc[0xd00/4]==0xa1 && rsc[0xd04/4]==8);
    unsigned before=write_calls;
    assert(!io.mic_rails(io.context,lease,0) && write_calls==before && b.power.held==7);
    assert(!io.release(io.context,NaBoardCapture,lease) && !c.owner[10] && !c.owner[7]);
    for (unsigned fail=1;fail<=6;++fail) {
        init(&c,&b,&io);map_failure=fail;
        assert(io.claim(io.context,NaBoardCapture,&lease)==NA_IO && b.lease && c.owner[10]);
        assert(io.claim(io.context,NaBoardCapture,&lease)==NA_OWNERSHIP && !write_calls);
    }
    init(&c,&b,&io);assert(!io.claim(io.context,NaBoardCapture,&lease));
    uint64_t foreign=0;assert(!sm8850_resource_acquire(&c,7,0x1234,&foreign));
    assert(io.mic_rails(io.context,lease,1)==NA_PENDING && !b.tcs_lease && !write_calls);
    assert(!sm8850_resource_release(&c,7,0x1234,foreign,1));
    write_failure=2;
    assert(io.mic_rails(io.context,lease,1)==NA_IO && b.error && b.tcs_lease && c.quarantined[7]);
    before=write_calls;assert(io.mic_rails(io.context,lease,1)==NA_OWNERSHIP && write_calls==before);
    assert(io.release(io.context,NaBoardCapture,lease)==NA_OWNERSHIP);
    /* Dedicated reset still works after an uncertain RSC write. */
    assert(!io.update(io.context,lease,NA_TLMM_GPIO_BASE+161*4096+4,2,0));
    cleanup();puts("PASS: production mic dispatch, current/stale leases, GPIO/USBSS/TAS/CMDDB boundary, six mapping failures, shared TCS2 IRQ preservation, real-completion gate and failure quarantine; MMIO/trace/time are synthetic");
}
