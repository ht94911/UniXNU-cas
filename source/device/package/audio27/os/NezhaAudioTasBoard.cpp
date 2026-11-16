// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NezhaAudioTasBoard.h"
#include "NezhaAudioXpuTrace.h"
namespace {
int trace_before(NaNativeTasBoard *b,const char *op,uint64_t pa,uint64_t span,
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
constexpr unsigned resource=9,owner=0x4e41;
constexpr uint64_t gcc=0x100000,tlmm=NA_TLMM_GPIO_BASE,geni=0x1980000;
constexpr uint32_t vote_mask=(1u<<12)|(1u<<8)|(1u<<9);
int error(int r) {return r==Sm8850Okay ? NA_OK:r==Sm8850Busy ? NA_BUSY:r==Sm8850Io ? NA_IO:NA_OWNERSHIP;}
bool pin(unsigned p) {const unsigned pins[]={54,56,64,65,174,122,123,124,125};for (unsigned v:pins) if (v==p) return true;return false;}
bool gcc_read(uint64_t pa) {
    return pa==gcc+0x52018 || pa==gcc+0x233f0 || pa==gcc+0x233f4 ||
        pa==gcc+0xa8004 || pa==gcc+0xa8018 || pa==gcc+0xa8028 ||
        (pa>=gcc+0xa8030 && pa<=gcc+0xa806c && !(pa&3));
}
volatile uint32_t *reg(NaNativeTasBoard *b,uint64_t pa,uint32_t *allowed) {
    if (pa&3) return nullptr;
    if (pa>=geni && pa<geni+0x4000 && b->geni.va) {*allowed=UINT32_MAX;return reinterpret_cast<volatile uint32_t *>(b->geni.va+pa-geni);}
    if (pa>=tlmm && pa<tlmm+NA_TLMM_GPIO_BYTES && b->tlmm.va) {
        unsigned p=(unsigned)((pa-tlmm)/4096),at=(unsigned)((pa-tlmm)&4095);
        if (pin(p) && (at==0 || (at==4 && (p==54 || p==56 || p==174)))) {
            *allowed=at ? 2:0x3ff;return reinterpret_cast<volatile uint32_t *>(b->tlmm.va+pa-tlmm);
        }
    }
    return nullptr;
}
int enter(NaNativeTasBoard *b,uint64_t lease) {
    if (!lease || lease!=b->lease) return NA_OWNERSHIP;
    return error(sm8850_resource_enter(b->common,resource,owner,lease));
}
int claim(void *v,uint32_t kind,uint64_t *lease) {
    auto *b=static_cast<NaNativeTasBoard *>(v);
    if (kind!=NaBoardTas || !lease || b->lease || b->error) return NA_OWNERSHIP;
    if (!trace_before(b,"tas-acquire",0,0)) return NA_IO;
    int r=sm8850_resource_acquire(b->common,resource,owner,&b->lease);if (r) return error(r);
    na_xpu_trace_after("tas-acquire",0,0,0,b->lease);
    *lease=b->lease;
    if (!b->geni.mapping) r=na_native_map(&b->geni,{geni,0x4000},b->lease);
    if (!r && !b->tlmm.mapping) r=na_native_map(&b->tlmm,{tlmm,NA_TLMM_GPIO_BYTES},b->lease);
    if (r) {b->error=NA_IO;return b->error;}
    b->geni.lease=b->tlmm.lease=b->lease;return NA_OK;
}
int release(void *v,uint32_t kind,uint64_t lease) {
    auto *b=static_cast<NaNativeTasBoard *>(v);
    if (kind!=NaBoardTas || lease!=b->lease || b->error) return NA_OWNERSHIP;
    int r=sm8850_resource_release(b->common,resource,owner,lease,1);
    if (!r) b->lease=0;return error(r);
}
int read(void *v,uint64_t lease,uint64_t pa,uint32_t *value) {
    auto *b=static_cast<NaNativeTasBoard *>(v);if (!value) return NA_ARGUMENT;
    if (!trace_before(b,"tas-read",pa,4)) {b->error=NA_IO;return NA_IO;}
    int r=enter(b,lease);if (r) return r;
    if (gcc_read(pa)) r=b->common->io.read32(b->common->io.context,Sm8850Gcc,(uint32_t)(pa-gcc),value) ? NA_IO:NA_OK;
    else {
        uint32_t allowed;auto *p=reg(b,pa,&allowed);
        if (!p) r=NA_OWNERSHIP;
        else {__asm__ volatile("dsb sy" ::: "memory");*value=*p;__asm__ volatile("dsb sy" ::: "memory");}
    }
    sm8850_resource_leave(b->common);
    na_xpu_trace_after("tas-read",pa,r,r ? 0:*value);return r;
}
int update(void *v,uint64_t lease,uint64_t pa,uint32_t mask,uint32_t value) {
    auto *b=static_cast<NaNativeTasBoard *>(v);
    if (!mask || (value&~mask)) return NA_ARGUMENT;
    if (!trace_before(b,"tas-update",pa,4,mask,value)) {b->error=NA_IO;return NA_IO;}
    int r=enter(b,lease);if (r) return r;
    if (pa==gcc+0x52018) r=error(sm8850_gcc_update(b->common,0x52018,vote_mask,mask,value));
    else {
        uint32_t allowed=0;auto *p=reg(b,pa,&allowed);
        if (!p || (mask&~allowed)) r=NA_OWNERSHIP;
        else {
            // GENI FIFO/IRQ-clear writes must not read a write-only or W1C
            // register first. The existing bus engine verifies completion.
            uint32_t old=mask==UINT32_MAX ? 0:*p;
            *p=(old&~mask)|value;__asm__ volatile("dsb sy" ::: "memory");
        }
    }
    sm8850_resource_leave(b->common);
    na_xpu_trace_after("tas-update",pa,r,value);return r;
}
}
extern "C" int na_native_tas_board(NaNativeTasBoard *b,Sm8850Resources *common,NaBoardIo *out) {
    if (!b || !common || !common->initialized || !out || b->common) return NA_ARGUMENT;
    b->common=common;
    *out={b,claim,release,read,update,nullptr,na_native_now_us,na_native_pause_us,&b->trace_wait_us};
    return NA_OK;
}
