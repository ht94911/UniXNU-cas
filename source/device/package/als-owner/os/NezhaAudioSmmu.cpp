// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NezhaAudioSmmu.h"
#include <IOKit/IOLib.h>
#include <Sm8850ResourcesNative.h>
extern "C" IOReturn armcpu_spi_attach(uint32_t,bool,void (*)(void *),void *,uint32_t *,uint64_t *);
namespace {
int barrier(void *) {__asm__ volatile("dsb sy" ::: "memory");return 0;}
uint32_t read_raw(NaNativeSmmu *d,size_t at) {barrier(d);auto v=*reinterpret_cast<volatile uint32_t *>(d->window.va+at);barrier(d);return v;}
void write_raw(NaNativeSmmu *d,size_t at,uint32_t v) {*reinterpret_cast<volatile uint32_t *>(d->window.va+at)=v;barrier(d);}
bool fits(NaNativeSmmu *d,size_t at,unsigned n) {return d->window.va && !(at&(n-1)) && at<=0x100000-n;}
bool faulted(NaNativeSmmu *d) {return __atomic_load_n(&d->fault_pending,__ATOMIC_ACQUIRE)!=0;}
bool stream_offset(NaNativeSmmu *d,size_t at,uint32_t base) {
    for (unsigned i=0;i<4;++i) if (at==base+4*d->owner->assignment.slot[i]) return true;
    return false;
}
int read32(void *v,size_t at,uint32_t *value) {
    auto *d=static_cast<NaNativeSmmu *>(v);if (!value || !fits(d,at,4) || faulted(d)) return -1;
    *value=read_raw(d,at);return 0;
}
int read64(void *v,size_t at,uint64_t *value) {
    auto *d=static_cast<NaNativeSmmu *>(v);if (!value || !fits(d,at,8) || faulted(d)) return -1;
    barrier(d);*value=*reinterpret_cast<volatile uint64_t *>(d->window.va+at);barrier(d);return 0;
}
int write32(void *v,size_t at,uint32_t value) {
    auto *d=static_cast<NaNativeSmmu *>(v);if (!fits(d,at,4)) return -1;
    uint32_t cb=d->context_offset,bank=d->owner->assignment.bank;
    bool route=stream_offset(d,at,0xc00),smr=stream_offset(d,at,0x800);
    bool local=at==cb || at==cb+0x10 || at==cb+0x30 || at==cb+0x38 || at==cb+0x3c || at==cb+0x58 || at==cb+0x610 || at==cb+0x7f0;
    if (!local && !route && !smr && at!=0x1000+4*bank && at!=0x1800+4*bank) return -1;
    // After an IRQ fault only retain/revoke operations are permitted. Never
    // let a new stream become active or re-enable its interrupt while racing
    // an interrupt filter; ordinary recovery still requires PAS quiescence.
    if (faulted(d) && !((route && value==0x20000) || (at==cb && value==0) || at==cb+0x610 || at==cb+0x7f0)) return -1;
    write_raw(d,at,value);
    if (at==cb && (value&1) && faulted(d)) {write_raw(d,at,value&~0x40u);return -1;}
    return 0;
}
int write64(void *v,size_t at,uint64_t value) {
    auto *d=static_cast<NaNativeSmmu *>(v);
    if (!fits(d,at,8) || faulted(d) || (at!=d->context_offset+0x20 && at!=d->context_offset+0x28)) return -1;
    *reinterpret_cast<volatile uint64_t *>(d->window.va+at)=value;barrier(d);return 0;
}
void fault_irq(void *v) {
    auto *d=static_cast<NaNativeSmmu *>(v);uint32_t cb=d->context_offset;
    uint32_t fsr=read_raw(d,cb+0x58);
    if (!(fsr&0xc00001feu)) return;
    if (!faulted(d)) {
        d->fault_status=fsr;d->fault_syndrome=read_raw(d,cb+0x68);
        barrier(d);d->fault_address=*reinterpret_cast<volatile uint64_t *>(d->window.va+cb+0x60);barrier(d);
        __atomic_store_n(&d->fault_pending,1,__ATOMIC_RELEASE);
    }
    // Keep M and the routes intact: disabling translation while a DSP still
    // owns DMA could bypass the domain. Only mask this bank's fault IRQ;
    // the worker observes the saved fault and retains buffers/leases.
    write_raw(d,cb,read_raw(d,cb)&~0x40u);
}
int failed(NaNativeSmmu *d,int r) {d->error=r;return r;}
}
extern "C" int na_native_smmu_faulted(const NaNativeSmmu *d) {
    if (!d) return NA_ARGUMENT;
    return d->error ? d->error:__atomic_load_n(&d->fault_pending,__ATOMIC_ACQUIRE) ? NA_IO:NA_OK;
}
extern "C" int na_native_smmu_bind(NaNativeSmmu *d,NaResourceOwner *owner,NaSmmuOwner *config) {
    if (!d || !owner || !owner->common || !config || d->attempted) return NA_ARGUMENT;
    if (!sm8850_usb_domain_ready()) return NA_BUSY;
    d->attempted=1;d->owner=owner;
    int r=na_resource_owner_smmu(owner,config);if (r) return failed(d,r);
    r=na_native_map(&d->window,{0x15000000,0x100000},config->lease);if (r) return failed(d,r);
    r=sm8850_resource_enter(owner->common,NaResourceSmmu,0x4e41,config->lease);
    if (r) return failed(d,NA_OWNERSHIP);
    r=sm8850_smmu_check_assignment(owner->common,&owner->assignment);
    if (r || owner->assignment.bank!=7) {sm8850_resource_leave(owner->common);return failed(d,NA_OWNERSHIP);}
    uint32_t id1=read_raw(d,0x24),id2=read_raw(d,0x28);
    d->context_offset=(2u<<((id1>>28)&7))*4096+owner->assignment.bank*4096;
    // Native ArmCpu4bd3230 refuses an already registered INTID. No handler
    // replacement, global GIC reset or private vector-table patch is used.
    if (read_raw(d,d->context_offset+4)!=3 || (id2&15)!=1) {sm8850_resource_leave(owner->common);return failed(d,NA_OWNERSHIP);}
    IOReturn irq=armcpu_spi_attach(136,false,fault_irq,d,&d->irq_readback,&d->irq_router);
    sm8850_resource_leave(owner->common);
    if (irq!=kIOReturnSuccess || (d->irq_readback&7)!=3 || faulted(d)) return failed(d,NA_IO);
    d->irq_attached=1;
    config->io={d,0x100000,read32,read64,write32,write64,barrier};
    config->powered=1;config->fault_handler_installed=1;config->output_bits=36;
    config->sync_context=nullptr;config->clean=na_native_clean;
    return NA_OK;
}
