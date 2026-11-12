// SPDX-License-Identifier: BSD-2-Clause
#ifdef NB_HOST_TEST
#include "NbNativeTestShim.h"
#else
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>
// The kernel's corecrypto headers carry no C linkage block of their own.
extern "C" {
#include <corecrypto/ccdigest.h>
#include <corecrypto/ccsha2.h>
}
#endif
#include <Sm8850ResourcesNative.h>
#include "NbNative.h"
#include "NbFirmware.h"
#include "NbScm.h"
extern "C" int native_ufs_bb_open(uint64_t *,uint8_t[32]);
extern "C" int native_ufs_bb_read(uint64_t,uint32_t,void *);
extern "C" IOReturn armcpu_spi_attach(uint32_t,bool,void (*)(void *),void *,uint32_t *,uint64_t *);
extern "C" void nezha_bb_class_init(void) __asm__("__GLOBAL__sub_I_coachz_baseband.cpp");
extern kmod_info_t g_kernel_kmod_info;
namespace {
const NbRange firmware_regions[]={{0x99590000,0xa000},{0x8aa00000,0xe100000},{0x99500000,0x80000}};
struct Board {
    Sm8850Resources *common;
    NbPower power;NbNativeSmmu smmu;
    NbArchive archive;NbFirmware firmware[3];NbPas pas[3];
    NbNativeMemory metadata[3],dma[5],shared,parameters,memshare[NB_MEMSHARE_CLIENTS];
    uint8_t *firmware_map[3];
    uint8_t mem_assigned[NB_MEMSHARE_CLIENTS];
    NbRmtfs rmtfs;uint8_t *read_scratch,*hash_scratch;
    uint64_t image_bytes,bridge_handle;int bridge_state;
    unsigned bind_attempted,bound,pas_touched,dsm_index,dsm_direction,dsm_mask,events;
    int error;
    void (*smp2p)(),(*watchdog)();
    uint32_t watchdog_readback;uint64_t watchdog_route;
};
Board board;
void log_stage(const char *name,int result){printf("NEZHA27 BB stage=%s result=%d\n",name,result);}
int fail(const char *name,int r){board.error=r<0?r:NB_IO;log_stage(name,board.error);return board.error;}
int image_read(void *v,uint64_t off,void *to,size_t length){
    auto *b=static_cast<Board *>(v);auto *dst=static_cast<uint8_t *>(to);
    if(!dst||off>b->image_bytes||length>b->image_bytes-off)return NB_ARGUMENT;
    while(length){
        unsigned skip=(unsigned)(off&4095);
        if(!skip&&length>=4096){unsigned blocks=(unsigned)(length/4096);if(blocks>16)blocks=16;
            if(native_ufs_bb_read(off/4096,blocks,dst))return NB_IO;
            size_t n=(size_t)blocks*4096;off+=n;dst+=n;length-=n;
        }else{
            if(native_ufs_bb_read(off/4096,1,b->read_scratch))return NB_IO;
            size_t n=4096-skip;if(n>length)n=length;memcpy(dst,b->read_scratch+skip,n);off+=n;dst+=n;length-=n;
        }
    }
    return NB_OK;
}
int hash_range(void *v,QcomMdtRead read,void *ctx,uint64_t off,uint64_t bytes,uint8_t digest[32]){
    auto *b=static_cast<Board *>(v);const struct ccdigest_info *di=ccsha256_di();
    ccdigest_di_decl(di,hash);ccdigest_init(di,hash);
    while(bytes){size_t n=bytes>65536?65536:(size_t)bytes;if(read(ctx,off,b->hash_scratch,n)){ccdigest_di_clear(di,hash);return NB_IO;}
        ccdigest_update(di,hash,n,b->hash_scratch);off+=n;bytes-=n;
    }
    ccdigest_final(di,hash,digest);ccdigest_di_clear(di,hash);return NB_OK;
}
int pas_call(void *,uint32_t command,uint32_t image,uint64_t a,uint64_t b){
    unsigned bit=image==4?1:image==0x26?2:4;
    if(command==1||command==5)board.pas_touched|=bit;
    log_stage(command==1?"pas-init":command==2?"pas-memory":command==5?"pas-auth":"pas-stop",NB_PENDING);
    int r=nezha_bb_scm_pas(command,image,a,b);
    if(!r&&command==6)board.pas_touched&=~bit;
    log_stage(command==1?"pas-init":command==2?"pas-memory":command==5?"pas-auth":"pas-stop",r);return r;
}
int pas_sync(void *v,void *cpu,uint64_t pa,uint64_t bytes){
    auto *b=static_cast<Board *>(v);
    if(b!=&board||!cpu||!bytes||bytes>0x10000||pa>UINT64_MAX-bytes)return NB_ARGUMENT;
    for(auto &m:b->metadata){
        if(!m.descriptor||pa<m.physical||pa-m.physical>=m.bytes||bytes>m.bytes-(pa-m.physical))continue;
        if(cpu!=m.cpu+(pa-m.physical))return NB_OWNERSHIP;
        return nb_native_low_sync(cpu,pa,bytes);
    }
    for(unsigned i=0;i<3;i++){
        const auto &r=firmware_regions[i];
        if(!b->firmware_map[i]||pa<r.base||pa-r.base>=r.bytes||bytes>r.bytes-(pa-r.base))continue;
        if(cpu!=b->firmware_map[i]+(pa-r.base))return NB_OWNERSHIP;
        // These three exact mappings use WriteCombineCache. The audio PAS
        // adapter orders the same stores with DSB instead of the RAM physmap.
        nb_native_publish_writes();return NB_OK;
    }
    return NB_OWNERSHIP;
}
void transport_trace(void *,const char *stage,int result){log_stage(stage,result);}
int smem_lock(void *){int r=sm8850_smem_trylock(board.common,NB_OWNER);return !r?NB_OK:r==Sm8850Busy?NB_BUSY:NB_IO;}
void smem_unlock(void *){if(sm8850_smem_unlock(board.common,NB_OWNER))board.error=NB_OWNERSHIP;}
void doorbell(void *,uint32_t signal){if((signal!=0&&signal!=2)||sm8850_ipcc_send(board.common,0x20000|signal))board.error=NB_IO;}
int proxy(void *,int on){return nb_power_set(&board.power,NB_POWER_MSS,on,nb_native_now_ms());}
int ipa_power(void *,int on){return nb_power_set(&board.power,NB_POWER_IPA,on,nb_native_now_ms());}
int ipa_smmu(void *,int on){if(!on)return NB_OWNERSHIP;int r=nb_native_smmu_enable(&board.smmu);return r==NB_BUSY?NB_PENDING:r;}
int qmp_send(const char *text){
    // Existing IPA configuration call is synchronous. Yield on the same worker
    // while its common mailbox lease waits for an acknowledgement.
    uint64_t deadline=nb_native_now_ms()+1000;
    for(;;){int r=nb_power_message(&board.power,text,nb_native_now_ms());if(r!=NB_BUSY&&r!=NB_PENDING)return r;
        if(nb_native_now_ms()>=deadline)return NB_TIMEOUT;
        IOSleep(1);}
}
int assign_range(NbRange region,int on,int shared){
    return nezha_bb_scm_assign(board.parameters.cpu,board.parameters.physical,&board.bridge_state,&board.bridge_handle,region,on,shared);
}
int dsm_assign(void *,int on){
    if((on!=0&&on!=1)||(!on&&(board.pas_touched&3)))return NB_OWNERSHIP;
    if(board.dsm_index==2){if(board.dsm_direction==(unsigned)on)return NB_OK;board.dsm_index=0;}
    if(board.dsm_index && board.dsm_direction!=(unsigned)on)return NB_OWNERSHIP;
    board.dsm_direction=(unsigned)on;
    const NbRange ranges[]={{0x85000000,0x4f00000},{0x89f00000,0x100000}};
    unsigned i=board.dsm_index;
    if(!on&&!(board.dsm_mask&(1u<<i)))return NB_OWNERSHIP;
    int r=assign_range(ranges[i],on,0);if(r)return r;
    if(on)board.dsm_mask|=1u<<i;else board.dsm_mask&=~(1u<<i);
    board.dsm_index++;return board.dsm_index==2?NB_OK:NB_PENDING;
}
int memory_alloc(void *,uint32_t index,uint32_t bytes,uint64_t *physical){
    if(index>=NB_MEMSHARE_CLIENTS||!physical||board.memshare[index].descriptor)return NB_ARGUMENT;
    int r=nb_native_allocate(&board.memshare[index],(bytes+4095)&~4095u,4096,0,1);
    if(!r)*physical=board.memshare[index].physical;
    return r;
}
int memory_assign(void *,uint64_t physical,uint32_t bytes,int on,int shared){
    for(unsigned i=0;i<NB_MEMSHARE_CLIENTS;i++)if(board.memshare[i].descriptor&&board.memshare[i].physical==physical&&bytes<=board.memshare[i].bytes){
        int r=assign_range({physical,board.memshare[i].bytes},on,shared);if(!r)board.mem_assigned[i]=(uint8_t)on;return r;
    }
    return NB_OWNERSHIP;
}
void memory_release(void *,uint32_t index,uint64_t physical,uint32_t bytes){
    if(index>=NB_MEMSHARE_CLIENTS||board.memshare[index].physical!=physical||bytes>board.memshare[index].bytes||board.mem_assigned[index]){board.error=NB_OWNERSHIP;return;}
    nb_native_release(&board.memshare[index]);
}
void remote_sync(void *,void *,uint64_t,int){
    // The shared window is normal non-cacheable on both sides; order accesses.
#ifndef NB_HOST_TEST
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
int lookup(void *,const char *path,uint64_t *bytes,uint32_t *handle){
    if(!path||!bytes||!handle||!strncmp(path,"ram/",4))return NB_ARGUMENT;
    for(uint32_t i=0;i<board.archive.count;i++)if(!strcmp(path,reinterpret_cast<char *>(board.archive.index)+i*NB_ARCHIVE_ENTRY)){
        NbArchiveFile f;int r=nb_archive_find(&board.archive,path,&f);if(r)return r;*bytes=f.bytes;*handle=i;return NB_OK;
    }
    return NB_MISSING;
}
int64_t read_file(void *,uint32_t handle,uint64_t offset,void *to,uint32_t bytes){
    if(handle>=board.archive.count)return NB_ARGUMENT;
    const char *name=reinterpret_cast<char *>(board.archive.index)+handle*NB_ARCHIVE_ENTRY;
    if(!strncmp(name,"ram/",4))return NB_OWNERSHIP;
    NbArchiveFile f;int r=nb_archive_find(&board.archive,name,&f);if(r)return r;
    if(offset>f.bytes)return NB_ARGUMENT;
    if(bytes>f.bytes-offset)bytes=(uint32_t)(f.bytes-offset);
    r=nb_archive_read(&f,offset,to,bytes);return r?(int64_t)r:(int64_t)bytes;
}
void ipcc_event(void *,uint32_t id){if(id==0x20002&&board.smp2p)board.smp2p();}
void watchdog_event(void *){if(board.watchdog)board.watchdog();}
}
extern "C" int nezha_bb_owners_ready(){
    if(!sm8850_usb_domain_ready())return NB_PENDING;
    IORegistryEntry *r=IORegistryEntry::fromPath("IOService:/IOResources");if(!r)return NB_PENDING;
    bool ready=r->getProperty("IOBSD")!=nullptr;r->release();return ready?NB_OK:NB_PENDING;
}
extern "C" int nezha_bb_bind(NbBasebandConfig *config){
    if(!config||board.bind_attempted)return NB_ARGUMENT;
    board.bind_attempted=1;*config={};
    if(nezha_bb_owners_ready()!=NB_OK)return fail("owners",NB_OWNERSHIP);
    board.common=sm8850_native_resources();if(!board.common)return fail("owners",NB_MISSING);
    log_stage("archive",NB_PENDING);
    uint64_t blocks=0;uint8_t digest[32];if(native_ufs_bb_open(&blocks,digest)||blocks>UINT64_MAX/4096)return fail("archive",NB_MISSING);
    board.image_bytes=blocks*4096;board.read_scratch=static_cast<uint8_t *>(IOMalloc(4096));board.hash_scratch=static_cast<uint8_t *>(IOMalloc(65536));
    void *index=IOMalloc(NB_ARCHIVE_FILES*NB_ARCHIVE_ENTRY);if(!index||!board.read_scratch||!board.hash_scratch)return fail("archive",NB_IO);
    int r=nb_archive_open(&board.archive,image_read,&board,board.image_bytes,digest,hash_range,&board,index,NB_ARCHIVE_FILES*NB_ARCHIVE_ENTRY);
    if(r)return fail("archive",r);
    log_stage("archive",NB_OK);
    if(nb_native_allocate(&board.parameters,4096,4096,1,0))return fail("parameters",NB_IO);
    config->smem=nb_native_map(0x81d00000,0x200000,0);config->smem_bytes=0x200000;config->smem_physical=0x81d00000;
    auto *qmp=static_cast<volatile uint32_t *>(nb_native_map(0x0c300000,4096,0));
    auto *db=static_cast<const uint8_t *>(nb_native_map(0x81c60000,0x20000,0));
    if(!config->smem||!qmp||!db)return fail("maps",NB_IO);
    r=nb_power_init(&board.power,board.common,qmp,db,0x20000);if(r)return fail("power-bind",r);
    NbIpaDma *dma[]={&config->ipa_memory.zero,&config->ipa_memory.ring,&config->ipa_memory.lan_rx,&config->ipa_memory.modem_rx,&config->ipa_memory.modem_tx};
    const uint32_t sizes[]={0x2000,0x10000,0x200000,0x200000,0x200000};uint64_t iova=0x20000000;
    for(unsigned i=0;i<5;i++){
        uint32_t align=i==1?65536:4096;iova=(iova+align-1)&~uint64_t(align-1);
        r=nb_native_allocate(&board.dma[i],sizes[i],align,0,1);if(r)return fail("dma",r);
        *dma[i]={board.dma[i].cpu,iova,sizes[i]};iova+=sizes[i];
    }
    r=nb_native_smmu_prepare(&board.smmu,board.common,board.dma,&config->ipa_memory);if(r)return fail("smmu-bind",r);
    const char *names[]={"ipa_fws","modem","modem_dtb"};
    const unsigned ids[]={15,4,0x26};
    const NbPasOps pas_ops={&board,pas_call,pas_sync};
    for(unsigned i=0;i<3;i++){
        r=nb_firmware_open(&board.firmware[i],&board.archive,names[i],board.hash_scratch,65536);if(r)return fail("firmware-index",r);
        r=nb_native_allocate(&board.metadata[i],65536,4096,1,0);if(r)return fail("metadata",r);
        board.firmware_map[i]=static_cast<uint8_t *>(nb_native_map(firmware_regions[i].base,(uint32_t)firmware_regions[i].bytes,1));
        if(!board.firmware_map[i])return fail("firmware-map",NB_IO);
        r=nb_pas_begin(&board.pas[i],ids[i],firmware_regions[i],board.firmware_map[i],board.firmware[i].bytes,nb_firmware_read,&board.firmware[i],
            board.metadata[i].cpu,board.metadata[i].physical,board.metadata[i].bytes,&pas_ops,nb_native_now_ms(),120000);
        if(r)return fail("firmware-parse",r);
    }
    log_stage("ram-store",NB_PENDING);
    const char *stored[]={"ram/modemst1","ram/modemst2","ram/fsg","ram/fsc"};
    const char *paths[]={"/boot/modem_fs1","/boot/modem_fs2","/boot/modem_fsg","/boot/modem_fsc"};
    const uint32_t expected[]={0x800000,0x800000,0x800000,0x20000};
    for(unsigned i=0;i<4;i++){
        NbArchiveFile f;if(nb_archive_find(&board.archive,stored[i],&f)||f.bytes!=expected[i])return fail("ram-store",NB_WIRE);
        auto *data=static_cast<uint8_t *>(IOMalloc(expected[i]));if(!data||nb_archive_read(&f,0,data,expected[i]))return fail("ram-store",NB_IO);
        board.rmtfs.image[i]={paths[i],data,expected[i]};
    }
    r=nb_native_allocate(&board.shared,0x400000,4096,1,1);if(r)return fail("ram-shared",r);
    r=assign_range({board.shared.physical,board.shared.bytes},1,1);if(r)return fail("ram-shared",r);
    board.rmtfs.count=4;board.rmtfs.shared=board.shared.cpu;board.rmtfs.shared_phys=board.shared.physical;board.rmtfs.shared_bytes=board.shared.bytes;
    board.rmtfs.sync=remote_sync;board.rmtfs.context=&board;if(nb_rmtfs_init(&board.rmtfs))return fail("ram-store",NB_WIRE);
    log_stage("ram-store",NB_OK);
    config->platform={&board,ipa_power,ipa_smmu,qmp_send};config->mss={&board,smem_lock,smem_unlock,doorbell,proxy,dsm_assign,transport_trace};
    config->memshare={&board,memory_alloc,memory_release,memory_assign};config->tftp={&board,lookup,read_file};
    config->ipa_fws=&board.pas[0];config->modem=&board.pas[1];config->modem_dtb=&board.pas[2];config->rmtfs=&board.rmtfs;
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);if(!handle)return fail("classes",NB_IO);
    nezha_bb_class_init();if(OSMetaClass::postModLoad(handle))return fail("classes",NB_IO);
    board.bound=1;log_stage("bind",NB_OK);return NB_OK;
}
extern "C" int nezha_bb_events(void (*smp2p)(),void (*watchdog)()){
    if(!board.bound||board.events||!smp2p||!watchdog)return NB_ARGUMENT;
    board.events=1;board.smp2p=smp2p;board.watchdog=watchdog;
    const uint32_t ids[]={0,0x20000,0x20002};
    for(unsigned i=0;i<3;i++){
        bool found=false;for(unsigned n=0;n<board.common->ipcc_count;n++)found|=board.common->ipcc[n].id==ids[i];
        if(i==0&&found)continue; // existing common AOP route remains its owner's
        if(found||sm8850_ipcc_register(board.common,ids[i],ipcc_event,&board))return fail("ipcc",NB_OWNERSHIP);
    }
    if(armcpu_spi_attach(264,true,watchdog_event,&board,&board.watchdog_readback,&board.watchdog_route)!=kIOReturnSuccess)return fail("events",NB_IO);
    log_stage("events",NB_OK);return NB_OK;
}
extern "C" int nezha_bb_dispatch(){
    if(board.error)return board.error;
    if(board.power.error)return board.power.error;
    if(board.smmu.ready){int r=nb_native_smmu_status(&board.smmu);if(r)return r;}
    int r=sm8850_ipcc_dispatch(board.common,32);return r==Sm8850Busy?NB_OK:r?NB_IO:NB_OK;
}
