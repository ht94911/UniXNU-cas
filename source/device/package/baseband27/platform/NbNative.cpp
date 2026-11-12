// SPDX-License-Identifier: BSD-2-Clause-Patent
// OS allocation and the existing ArmSmmu4K/common-owner binding, following
// the qualified audio native adapter's public IOKit API usage.
#ifdef NB_HOST_TEST
#include "NbNativeTestShim.h"
#else
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <arm/caches_internal.h>
#include <pexpert/device_tree.h>
#endif
#include "NbNative.h"
#include "NbLowMemory.h"
#ifdef IOKIT_ENABLE_SHARED_PTR
#error Requalify the native pointer ABI before changing the selected compile flags.
#endif
extern "C" IOReturn armcpu_spi_attach(uint32_t,bool,void (*)(void *),void *,uint32_t *,uint64_t *);
namespace {
NbLowMemory low_pool;
IODeviceMemory *low_owner;
IOMemoryMap *low_mapping;
uint8_t *low_cpu;
void barrier(){
#ifndef NB_HOST_TEST
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
int prepare_low_memory(){
    if(low_cpu)return NB_OK;
    DTEntry chosen=nullptr;const void *raw=nullptr;unsigned bytes=0;uint64_t range[2];
    if(SecureDTLookupEntry(nullptr,"/chosen",&chosen)!=kSuccess ||
       SecureDTGetProperty(chosen,"nezha-baseband-low-memory",&raw,&bytes)!=kSuccess ||
       !raw||bytes!=sizeof range)return NB_MISSING;
    memcpy(range,raw,sizeof range);
    if(nb_low_memory_init(&low_pool,range[0],range[1]))return NB_OWNERSHIP;
    low_owner=IODeviceMemory::withRange(range[0],range[1]);
    if(!low_owner)return NB_IO;
    low_mapping=low_owner->map(kIOMapAnywhere|kIOMapWriteCombineCache);
    if(!low_mapping)return NB_IO;
    low_cpu=reinterpret_cast<uint8_t *>(low_mapping->getVirtualAddress());
    if(!low_cpu)return NB_IO;
    printf("NEZHA27 BB stage=low-memory result=0 pa=%#llx bytes=%u\n",
           (unsigned long long)low_pool.physical,low_pool.bytes);
    return NB_OK;
}
int fence(void *){barrier();return 0;}
uint32_t raw(NbNativeSmmu *s,size_t at){barrier();uint32_t v=*reinterpret_cast<volatile uint32_t *>(s->registers+at);barrier();return v;}
bool fits(NbNativeSmmu *s,size_t at,unsigned n){return s->registers && !(at&(n-1)) && at<=0x100000-n;}
int read32(void *v,size_t at,uint32_t *value){auto *s=static_cast<NbNativeSmmu *>(v);if(!value||!fits(s,at,4)||__atomic_load_n(&s->fault,__ATOMIC_ACQUIRE))return -1;*value=raw(s,at);return 0;}
int read64(void *v,size_t at,uint64_t *value){auto *s=static_cast<NbNativeSmmu *>(v);if(!value||!fits(s,at,8)||__atomic_load_n(&s->fault,__ATOMIC_ACQUIRE))return -1;barrier();*value=*reinterpret_cast<volatile uint64_t *>(s->registers+at);barrier();return 0;}
int write32(void *v,size_t at,uint32_t value){
    auto *s=static_cast<NbNativeSmmu *>(v);if(!fits(s,at,4))return -1;
    uint32_t cb=s->context_offset,bank=s->assignment.bank;bool route=false,smr=false;
    for(unsigned i=0;i<2;i++){route|=at==0xc00+4*s->assignment.slot[i];smr|=at==0x800+4*s->assignment.slot[i];}
    bool local=at==cb||at==cb+0x10||at==cb+0x30||at==cb+0x38||at==cb+0x3c||at==cb+0x58||at==cb+0x610||at==cb+0x7f0;
    if(!local&&!route&&!smr&&at!=0x1000+4*bank&&at!=0x1800+4*bank)return -1;
    if(__atomic_load_n(&s->fault,__ATOMIC_ACQUIRE)&&!((route&&value==0x20000)||(at==cb&&value==0)||at==cb+0x610||at==cb+0x7f0))return -1;
#ifdef NB_HOST_TEST
    mock_mmio_write32(s->registers,at,value);
#else
    *reinterpret_cast<volatile uint32_t *>(s->registers+at)=value;
#endif
    barrier();return 0;
}
int write64(void *v,size_t at,uint64_t value){
    auto *s=static_cast<NbNativeSmmu *>(v);
    if(!fits(s,at,8)||s->fault||(at!=s->context_offset+0x20&&at!=s->context_offset+0x28))return -1;
    *reinterpret_cast<volatile uint64_t *>(s->registers+at)=value;barrier();return 0;
}
void fault_irq(void *v){
    auto *s=static_cast<NbNativeSmmu *>(v);uint32_t fsr=raw(s,s->context_offset+0x58);
    if(!(fsr&0xc00001feu))return;
    __atomic_store_n(&s->fault,fsr,__ATOMIC_RELEASE);
    // Retain translation and streams; mask only the owned bank's fault IRQ.
    *reinterpret_cast<volatile uint32_t *>(s->registers+s->context_offset)=raw(s,s->context_offset)&~0x40u;barrier();
}
}
extern "C" void *nb_native_map(uint64_t pa,uint32_t bytes,int memory){
    if(!pa||!bytes||((pa|bytes)&4095))return nullptr;
    auto *d=IODeviceMemory::withRange(pa,bytes);if(!d)return nullptr;
    auto *map=d->map(kIOMapAnywhere|(memory?kIOMapWriteCombineCache:kIOMapInhibitCache));d->release();
    return map?reinterpret_cast<void *>(map->getVirtualAddress()):nullptr; // retained for this boot
}
extern "C" int nb_native_allocate(NbNativeMemory *m,uint32_t bytes,uint32_t align,int low,int uncached){
    if(!m||m->descriptor||!bytes||bytes>0x2000000||(bytes&4095)||align<4096||(align&(align-1)))return NB_ARGUMENT;
    if(low){
        int r=prepare_low_memory();if(r)return r;
        uint64_t pa=0;r=nb_low_memory_take(&low_pool,bytes,align,&pa);if(r)return r;
        uint8_t *cpu=low_cpu+(pa-low_pool.physical);bzero(cpu,bytes);barrier();
        // The loader's reserved pool and WC alias stay retained through reset.
        // It is never returned to XNU's high-memory allocator.
        *m={low_owner,nullptr,cpu,pa,bytes};return NB_OK;
    }
    IOOptionBits opts=IOOptionBits(kIOMemoryPhysicallyContiguous)|IOOptionBits(kIODirectionInOut)|IOOptionBits(kIOMemoryMapperNone);
    IOBufferMemoryDescriptor *d=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,opts,bytes,align);
    if(!d)return NB_IO;
    IOByteCount length=0;uint64_t pa=d->getPhysicalSegment(0,&length,kIOMemoryMapperNone);auto *cpu=static_cast<uint8_t *>(d->getBytesNoCopy());
    if(!cpu||!pa||(pa&(align-1))||length<bytes||pa>UINT64_MAX-bytes) {d->release();return NB_OWNERSHIP;}
    bzero(cpu,bytes);flush_dcache64(pa,bytes,TRUE);barrier();
    IOMemoryMap *mapping=nullptr;
    if(uncached){mapping=d->map(kIOMapAnywhere|kIOMapWriteCombineCache);if(!mapping){d->release();return NB_IO;}cpu=reinterpret_cast<uint8_t *>(mapping->getVirtualAddress());}
    *m={d,mapping,cpu,pa,bytes};return NB_OK;
}
extern "C" void nb_native_release(NbNativeMemory *m){
    if(m->descriptor==low_owner&&low_owner){*m={};return;}
    if(m->mapping)static_cast<IOMemoryMap *>(m->mapping)->release();
    if(m->descriptor)static_cast<IOBufferMemoryDescriptor *>(m->descriptor)->release();
    *m={};
}
extern "C" void nb_native_publish_writes(){barrier();}
extern "C" int nb_native_low_sync(void *cpu,uint64_t pa,uint64_t bytes){
    if(!cpu||!bytes||!low_cpu||pa<low_pool.physical||
       pa-low_pool.physical>=low_pool.used||bytes>low_pool.used-(pa-low_pool.physical))
        return NB_OWNERSHIP;
    if(cpu!=low_cpu+(pa-low_pool.physical))return NB_OWNERSHIP;
    barrier();return NB_OK;
}
extern "C" int nb_native_sync(void *,void *cpu,uint64_t pa,uint64_t bytes){
    if(!cpu||!pa||!bytes||bytes>0x2000000||pa>UINT64_MAX-bytes)return NB_ARGUMENT;
    if(low_cpu&&pa>=low_pool.physical&&pa-low_pool.physical<low_pool.bytes){
        return nb_native_low_sync(cpu,pa,bytes);
    }
    flush_dcache64(pa,(unsigned)bytes,TRUE);barrier();return NB_OK;
}
extern "C" uint64_t nb_native_now_ms(){uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000000;}
extern "C" int nb_native_smmu_prepare(NbNativeSmmu *s,Sm8850Resources *common,const NbNativeMemory *memory,const NbIpaMemory *ipa){
    if(!s||!common||!memory||!ipa||s->common)return NB_ARGUMENT;
    s->common=common;s->assignment={8,0xfd00,{13,14,0,0},{0x4a0,0x4a2,0,0},2};
    int r=sm8850_resource_acquire(common,NB_RES_SMMU,NB_OWNER,&s->lease);if(r)return NB_OWNERSHIP;
    s->registers=static_cast<volatile uint8_t *>(nb_native_map(0x15000000,0x100000,0));if(!s->registers)return NB_IO;
    if(nb_native_allocate(&s->tables,0x10000,4096,0,0))return NB_IO;
    s->config={32,36,s->tables.physical,2048};
    const NbIpaDma *dma[]={&ipa->zero,&ipa->ring,&ipa->lan_rx,&ipa->modem_rx,&ipa->modem_tx};
    ArmSmmu4KRange ranges[6]={{0x14683000,0x14683000,0x2000,ArmSmmu4KRead|ArmSmmu4KWrite,0,2}};
    for(unsigned i=0;i<5;i++){
        if(!memory[i].descriptor||memory[i].cpu!=dma[i]->virt||memory[i].bytes!=dma[i]->bytes||dma[i]->iova<0x20000000||
           dma[i]->iova>=0x40000000||dma[i]->bytes>0x40000000-dma[i]->iova)return NB_OWNERSHIP;
        ranges[i+1]={dma[i]->iova,memory[i].physical,memory[i].bytes,ArmSmmu4KRead|ArmSmmu4KWrite,0,2};
    }
    if(ArmSmmu4KBuild(&s->config,ranges,6,reinterpret_cast<uint64_t *>(s->tables.cpu),16,&s->result))return NB_OWNERSHIP;
    if(nb_native_sync(nullptr,s->tables.cpu,s->tables.physical,s->tables.bytes))return NB_IO;
    s->context={&s->config,nullptr,&s->result,nullptr,0,0,s->assignment.asid,1,0,0,0};
    s->io={s,0x100000,read32,read64,write32,write64,fence};return NB_OK;
}
extern "C" int nb_native_smmu_enable(NbNativeSmmu *s){
    if(!s||!s->common)return NB_ARGUMENT;
    if(s->ready)return nb_native_smmu_status(s);
    if(s->attempted)return NB_OWNERSHIP;
    int r=sm8850_resource_enter(s->common,NB_RES_SMMU,NB_OWNER,s->lease);if(r)return r==Sm8850Busy?NB_BUSY:NB_OWNERSHIP;
    s->attempted=1;r=sm8850_smmu_check_assignment(s->common,&s->assignment);
    uint32_t id1=raw(s,0x24);s->context_offset=(2u<<((id1>>28)&7))*4096+s->assignment.bank*4096;
    if(!r&&(raw(s,s->context_offset+4)!=3||(raw(s,0x28)&15)!=1))r=NB_OWNERSHIP;
    if(!r&&armcpu_spi_attach(137,false,fault_irq,s,&s->irq_readback,&s->irq_route)!=kIOReturnSuccess)r=NB_IO;
    if(!r&&(s->irq_readback&7)!=3)r=NB_IO;
    const ArmSmmu4KStream streams[]={{13,0x4a0},{14,0x4a2}};
    if(!r)r=ArmSmmu4KCheckFaultedStreams(&s->io,streams,2,8,&s->stream_result);
    if(!r)r=ArmSmmu4KProgramDisabledContext(&s->io,8,&s->context,1000,&s->program);
    if(!r)r=ArmSmmu4KAttachStreams(&s->io,&s->streams,streams,2,8,&s->context,&s->stream_result);
    sm8850_resource_leave(s->common);
    printf("NEZHA27 BB stage=smmu-install result=%d\n",r);
    if(r||s->fault)return NB_OWNERSHIP;
    s->ready=1;return NB_OK;
}
extern "C" int nb_native_smmu_status(NbNativeSmmu *s){
    if(!s||!s->ready||__atomic_load_n(&s->fault,__ATOMIC_ACQUIRE))return NB_OWNERSHIP;
    // The same owner reads faults; no global clear/reset or recovery action.
    return raw(s,0x48)||(raw(s,s->context_offset+0x58)&0xc00001feu)?NB_IO:NB_OK;
}
