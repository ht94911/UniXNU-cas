/* SPDX-License-Identifier: MIT */
#include "NezhaCameraNativeSmmu.h"
#include "Sm8850ResourcesNative.h"
#include "ArmSmmu4KMmio.h"
#include <cassert>
#include <cstdio>

static uint32_t registers[0x100000/4];
static Sm8850Resources common{};
static unsigned maps, attaches, locks;
static uint64_t sequence;
static void (*handler)(void *);
static void *irq_context;
extern "C" unsigned sm8850_usb_domain_ready(void) { return 1; }
extern "C" Sm8850Resources *sm8850_native_resources(void) { return &common; }
extern "C" int sm8850_resource_acquire(Sm8850Resources *,unsigned slot,uint32_t owner,uint64_t *lease)
{ assert(slot==11 && owner==0x4e43 && !common.lease[11]);*lease=++sequence;common.lease[11]=*lease;return 0; }
extern "C" int sm8850_resource_enter(Sm8850Resources *,unsigned slot,uint32_t owner,uint64_t lease)
{ assert(slot==11 && owner==0x4e43 && lease==common.lease[11] && !locks);++locks;return 0; }
extern "C" void sm8850_resource_leave(Sm8850Resources *) { assert(locks==1);--locks; }
extern "C" int sm8850_resource_release(Sm8850Resources *,unsigned slot,uint32_t owner,uint64_t lease,int quiet)
{ assert(slot==11 && owner==0x4e43 && lease==common.lease[11] && quiet && !locks);common.lease[11]=0;return 0; }
extern "C" int sm8850_resource_quarantine(Sm8850Resources *,unsigned,uint32_t,uint64_t)
{ assert(false);return -1; }
extern "C" int sm8850_smmu_check_assignment(Sm8850Resources *,const Sm8850SmmuAssignment *a)
{ assert(locks && a->count==1 && a->sid[0]==0x1c00 && a->bank==8 && a->slot[0]==80 && a->asid==0xfc00);return 0; }
extern "C" unsigned long ml_io_map(unsigned long physical,unsigned long bytes)
{ assert(physical==0x15000000 && bytes==sizeof(registers));++maps;return reinterpret_cast<unsigned long>(registers); }
extern "C" void IOSleep(unsigned ms) { assert(ms==1); }
static int read32(void *,size_t o,uint32_t *v) { assert(o<sizeof(registers));*v=registers[o/4];return 0; }
static int read64(void *,size_t o,uint64_t *v) { assert(o<sizeof(registers));*v=registers[o/4];return 0; }
static int write32(void *,size_t o,uint32_t v) {
    assert(o<sizeof(registers));
    if (o%4096==0x58) registers[o/4]&=~v;
    else registers[o/4]=v;
    return 0;
}
static int write64(void *,size_t o,uint64_t v) { assert(o<sizeof(registers));registers[o/4]=(uint32_t)v;return 0; }
static int barrier(void *) { return 0; }
extern "C" ArmSmmu4KStatus ArmSmmu4KBindMmio(ArmSmmu4KMmio *map,ArmSmmu4KIo *io)
{ assert(map->mapping==reinterpret_cast<volatile unsigned char *>(registers));*io={NULL,sizeof(registers),read32,read64,write32,write64,barrier};return ArmSmmu4KSuccess; }
extern "C" ArmSmmu4KStatus ArmSmmu4KSyncContextTlb(const ArmSmmu4KIo *,uint64_t offset,unsigned asid,unsigned all,unsigned budget,ArmSmmu4KProgramResult *)
{ assert(offset==136*4096 && asid==0xfc00 && all==1 && budget==1000 && locks);return ArmSmmu4KSuccess; }
extern "C" int armcpu_spi_attach(uint32_t id,bool edge,void (*fn)(void *),void *cookie,uint32_t *readback,uint64_t *route)
{ assert(id==137 && !edge && !handler);handler=fn;irq_context=cookie;*readback=3;*route=0;++attaches;return 0; }
static ArmSmmu4KConfig config{32,40,0x10000000,10};
static NezhaCameraDmaPlan plan{};
extern "C" const ArmSmmu4KConfig *nezha_camera_native_memory_config(const NezhaCameraNativeMemory *) { return &config; }
extern "C" const NezhaCameraDmaPlan *nezha_camera_native_memory_plan(const NezhaCameraNativeMemory *) { return &plan; }

int main()
{
    registers[0x24/4]=0x60000051;registers[0x28/4]=0x5221;
    constexpr size_t bank=136*4096;registers[(bank+4)/4]=0;
    NezhaCameraNativeSmmu *state=nullptr;
    for (unsigned turn=0;turn<2;++turn) {
        unsigned physical_bits=0;
        assert(nezha_camera_native_smmu_prepare(&state,80,8,0xfc00,&physical_bits)==ArmSmmu4KSuccess && physical_bits==40);
        assert(nezha_camera_native_smmu_bind(state,reinterpret_cast<const NezhaCameraNativeMemory *>(1))==ArmSmmu4KSuccess);
        assert(nezha_camera_native_smmu_capture_enter(state));
        assert(registers[(bank+4)/4]==1 && attaches==1 && maps==1);
        nezha_camera_native_smmu_capture_leave(state);
        assert(nezha_camera_native_smmu_prepare_detach(state) && locks==1);
        assert(nezha_camera_native_smmu_release(&state,true) && !state && !locks && !common.lease[11]);
        assert(registers[(bank+4)/4]==0);
        handler(irq_context); /* retired callback remains inert on permanent mapping */
    }
    unsigned physical_bits=0;
    assert(nezha_camera_native_smmu_prepare(&state,80,9,0xfc00,&physical_bits)==ArmSmmu4KBusy && !state);
    assert(nezha_camera_native_smmu_prepare(&state,80,8,0xfc00,&physical_bits)==ArmSmmu4KSuccess);
    assert(nezha_camera_native_smmu_bind(state,reinterpret_cast<const NezhaCameraNativeMemory *>(1))==ArmSmmu4KSuccess);
    assert(nezha_camera_native_smmu_capture_enter(state));nezha_camera_native_smmu_capture_leave(state);
    registers[(bank+0x58)/4]=2;registers[(bank+0x68)/4]=7;
    handler(irq_context);
    assert(nezha_camera_native_smmu_fault(state)==2);
    assert(nezha_camera_native_smmu_prepare_detach(state));
    assert(nezha_camera_native_smmu_release(&state,true) && !state);
    assert(nezha_camera_native_smmu_prepare(&state,80,8,0xfc00,&physical_bits)==ArmSmmu4KBusy && !state);
    puts("Native camera SMMU: two clean sessions reuse one map/IRQ, restore ACTLR and leases; changed routing or fault refuses restart");
}
