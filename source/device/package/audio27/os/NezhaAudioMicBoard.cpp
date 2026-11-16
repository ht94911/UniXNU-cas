// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NezhaAudioMicBoard.h"
#include "NezhaAudioXpuTrace.h"
#include "../platform/AudioResourceOwner.h"
namespace {
int trace_before(NaNativeMicBoard *b,const char *op,uint64_t pa,uint64_t span,
                 uint32_t mask=0,uint32_t value=0) {
    const bool enabled=na_xpu_trace_enabled();
    const uint64_t start=enabled ? na_native_now_us(nullptr):0;
    const int confirmed=na_xpu_trace_before(op,pa,span,mask,value);
    if (enabled) {
        const uint64_t end=na_native_now_us(nullptr);
        if (end<start || end-start>UINT64_MAX-b->trace_wait_us) {
            b->error=NA_OVERFLOW;return 0;
        }
        b->trace_wait_us+=end-start;
    }
    return confirmed;
}
constexpr unsigned resource=10,owner=0x4e41,tcs=2;
constexpr NaRange ranges[6]={{NA_TLMM_GPIO_BASE+161*4096,4096},{0x7760000,0x17000},
    {0x6ae0000,4096},{0x7660000,4096},{0x7630000,0x10000},{0x81c60000,0x20000}};
int error(int r) {return r==Sm8850Okay ? NA_OK:r==Sm8850Busy ? NA_BUSY:r==Sm8850Io ? NA_IO:NA_OWNERSHIP;}
int enter(NaNativeMicBoard *b,uint64_t lease) {
    if (!lease || lease!=b->lease) return NA_OWNERSHIP;
    return error(sm8850_resource_enter(b->common,resource,owner,lease));
}
int tcs_acquire(void *v) {
    auto *b=static_cast<NaNativeMicBoard *>(v);
    if (!b->lease || b->tcs_lease || b->error) return NA_OWNERSHIP;
    int r=sm8850_resource_acquire(b->common,NaResourceMicTcs,owner,&b->tcs_lease);if (r) return error(r);
    r=sm8850_resource_enter(b->common,NaResourceMicTcs,owner,b->tcs_lease);
    if (!r) {
        r=sm8850_tcs_idle(b->common,tcs);
        if (!r) {
            r=sm8850_tcs_irq_update(b->common,tcs,1,&b->irq_previous);
            if (!r) b->irq_saved=1;
        }
        sm8850_resource_leave(b->common);
    }
    return error(r);
}
int tcs_release(void *v) {
    auto *b=static_cast<NaNativeMicBoard *>(v);
    if (!b->tcs_lease || !b->irq_saved) return NA_OWNERSHIP;
    int r=sm8850_resource_enter(b->common,NaResourceMicTcs,owner,b->tcs_lease);
    if (!r) {
        r=sm8850_tcs_idle(b->common,tcs);
        if (!r) {
            unsigned previous;r=sm8850_tcs_irq_update(b->common,tcs,b->irq_previous,&previous);
            if (!r) b->irq_saved=0;
        }
        sm8850_resource_leave(b->common);
    }
    if (!r) r=sm8850_resource_release(b->common,NaResourceMicTcs,owner,b->tcs_lease,1);
    if (!r) b->tcs_lease=0;
    return error(r);
}
int rsc_read(void *v,uint32_t offset,uint32_t *value) {
    auto *b=static_cast<NaNativeMicBoard *>(v);
    bool identity=offset==0 || offset==0xc;
    constexpr uint32_t base=0xd00+tcs*0x2a0;
    bool status=offset==0xd04 || offset==base+0x28 ||
        offset==base+0x40 || offset==base+0x58 || offset==base+0x70;
    if (!value || (!identity && !status)) return NA_OWNERSHIP;
    int r=identity ? enter(b,b->lease):error(sm8850_resource_enter(b->common,NaResourceMicTcs,owner,b->tcs_lease));
    if (r) return r;
    r=b->common->io.read32(b->common->io.context,Sm8850Rsc,offset,value) ? NA_IO:NA_OK;
    sm8850_resource_leave(b->common);return r;
}
int rsc_write(void *v,uint32_t offset,uint32_t value) {
    auto *b=static_cast<NaNativeMicBoard *>(v);
    int r=sm8850_resource_enter(b->common,NaResourceMicTcs,owner,b->tcs_lease);if (r) return error(r);
    r=sm8850_tcs_write(b->common,tcs,offset,value);
    sm8850_resource_leave(b->common);return error(r);
}
int claim(void *v,uint32_t kind,uint64_t *lease) {
    auto *b=static_cast<NaNativeMicBoard *>(v);
    if (kind!=NaBoardCapture || !lease || b->lease || b->error) return NA_OWNERSHIP;
    int r=sm8850_resource_acquire(b->common,resource,owner,&b->lease);if (r) return error(r);
    *lease=b->lease;
    for (unsigned i=0;i<6 && !r;++i) {
        if (!b->window[i].mapping) r=na_native_map(&b->window[i],ranges[i],b->lease);
        if (!r) b->window[i].lease=b->lease;
    }
    if (!r && !b->power.initialized) {
        NaMicPowerOps ops={b,tcs_acquire,tcs_release,rsc_read,rsc_write};
        r=na_mic_power_init(&b->power,const_cast<const uint8_t *>(b->window[5].va),0x20000,&ops);
    }
    if (r) b->error=r;return r;
}
int release(void *v,uint32_t kind,uint64_t lease) {
    auto *b=static_cast<NaNativeMicBoard *>(v);
    if (kind!=NaBoardCapture || lease!=b->lease || b->error || b->tcs_lease || b->power.busy || b->power.want) return NA_OWNERSHIP;
    int r=sm8850_resource_release(b->common,resource,owner,lease,1);
    if (!r) b->lease=0;return error(r);
}
volatile uint32_t *reg(NaNativeMicBoard *b,uint64_t pa,uint32_t *allowed) {
    if (pa&3) return nullptr;
    for (unsigned i=0;i<5;++i) if (pa>=ranges[i].base && pa-ranges[i].base<ranges[i].bytes && b->window[i].va) {
        uint64_t offset=pa-ranges[i].base;
        if (!i) {
            if (offset!=0 && offset!=4) return nullptr;
            *allowed=offset==4 ? 2:0x3ff;
        } else if (i==1) {
            unsigned pin=(unsigned)(offset/4096);
            if ((offset&4095) || (pin!=0 && pin!=1 && pin!=2 && pin!=14)) return nullptr;
            *allowed=0x1bff;
        } else *allowed=UINT32_MAX;
        return reinterpret_cast<volatile uint32_t *>(b->window[i].va+offset);
    }
    return nullptr; // The mapped command DB has no board write accessor.
}
int read(void *v,uint64_t lease,uint64_t pa,uint32_t *value) {
    auto *b=static_cast<NaNativeMicBoard *>(v);if (!value) return NA_ARGUMENT;
    if (!trace_before(b,"mic-read",pa,4)) {b->error=NA_IO;return NA_IO;}
    int r=enter(b,lease);if (r) return r;
    uint32_t allowed=0;auto *p=reg(b,pa,&allowed);
    if (!p) r=NA_OWNERSHIP;
    else if (b->common->io.barrier(b->common->io.context)) r=NA_IO;
    else {
        *value=*p;
        if (b->common->io.barrier(b->common->io.context)) r=NA_IO;
    }
    sm8850_resource_leave(b->common);
    na_xpu_trace_after("mic-read",pa,r,r ? 0:*value);return r;
}
int update(void *v,uint64_t lease,uint64_t pa,uint32_t mask,uint32_t value) {
    auto *b=static_cast<NaNativeMicBoard *>(v);
    if (!mask || (value&~mask)) return NA_ARGUMENT;
    if (!trace_before(b,"mic-update",pa,4,mask,value)) {b->error=NA_IO;return NA_IO;}
    int r=enter(b,lease);if (r) return r;
    uint32_t allowed=0;auto *p=reg(b,pa,&allowed);
    if (!p || (mask&~allowed)) r=NA_OWNERSHIP;
    else {
        uint32_t old=mask==UINT32_MAX ? 0:*p; // SoundWire FIFO/W1C never RMW.
        *p=(old&~mask)|value;
        if (b->common->io.barrier(b->common->io.context)) r=NA_IO;
    }
    sm8850_resource_leave(b->common);
    na_xpu_trace_after("mic-update",pa,r,value);return r;
}
int rails(void *v,uint64_t lease,int on) {
    auto *b=static_cast<NaNativeMicBoard *>(v);
    if (!lease || lease!=b->lease || b->error) return NA_OWNERSHIP;
    int r=na_mic_power_request(&b->power,on);
    if (!r) r=na_mic_power_poll(&b->power,na_native_now_us(nullptr)/1000);
    if (r<0) {
        b->error=r;
        if (b->tcs_lease && !b->quarantined) {
            if (!sm8850_resource_quarantine(b->common,NaResourceMicTcs,owner,b->tcs_lease)) b->quarantined=1;
        }
    }
    return r;
}
}
extern "C" int na_native_mic_board(NaNativeMicBoard *b,Sm8850Resources *common,NaBoardIo *out) {
    if (!b || !common || !common->initialized || !out || b->common) return NA_ARGUMENT;
    b->common=common;*out={b,claim,release,read,update,rails,na_native_now_us,na_native_pause_us,&b->trace_wait_us};return NA_OK;
}
