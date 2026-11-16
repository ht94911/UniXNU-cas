// SPDX-License-Identifier: BSD-2-Clause
#include "NbNativeTestShim.h"
#include "../../package/baseband27/platform/NbNative.h"
#include <map>
#include <vector>
#include <fstream>
static std::vector<void *> allocations;
static std::map<uint64_t,void *> maps;
static std::map<uint64_t,void *> buffers;
static std::vector<uint8_t> archive;
static uint8_t digest[32];static uint64_t low=0xb1000000,high=0x120000000,ticks;
static unsigned classes,assigned,pas_calls,locked,cache_cleans;
static bool missing_smem;
static Sm8850Resources common;
static void (*irq_callback[1024])(void *);static void *irq_context[1024];
void *kernel_task=nullptr;kmod_info_t g_kernel_kmod_info={"kernel"};
int SecureDTLookupEntry(void *,const char *name,DTEntry *entry){assert(!strcmp(name,"/chosen"));*entry=(void *)1;return 0;}
int SecureDTGetProperty(DTEntry,const char *name,const void **data,unsigned *bytes){
    static const uint64_t range[]={0xb1000000,5u*1024u*1024u};
    assert(!strcmp(name,"nezha-baseband-low-memory"));*data=range;*bytes=sizeof range;return 0;
}
void *mock_alloc(size_t n,size_t alignment){void *p=nullptr;assert(!posix_memalign(&p,alignment,n));allocations.push_back(p);return p;}
void mock_free(void *p){for(auto &a:allocations)if(a==p){free(p);a=nullptr;return;}assert(!p);}
void mock_mmio_write32(volatile uint8_t *base,size_t at,uint32_t value){
    auto *reg=reinterpret_cast<volatile uint32_t *>(base+at);
    if((at&4095)==0x58)*reg&=~value;else *reg=value;
}
void *IOMalloc(size_t n){return mock_alloc(n);}
void IOSleep(unsigned ms){ticks+=(uint64_t)ms*1000000;}
void flush_dcache64(uint64_t pa,unsigned,int){assert(pa>=0x100000000ULL);cache_cleans++;}
uint64_t mach_absolute_time(){return ticks;}
void absolutetime_to_nanoseconds(uint64_t t,uint64_t *n){*n=t;}
IOBufferMemoryDescriptor::IOBufferMemoryDescriptor(uint32_t n,uint32_t align,bool islow){
    bytes=n;p=static_cast<uint8_t *>(mock_alloc(n,align));uint64_t &next=islow?low:high;next=(next+align-1)&~uint64_t(align-1);pa=next;next+=n;
    buffers[pa]=p;
}
static void w16(uint8_t *p,uint32_t v){p[0]=v;p[1]=v>>8;}static void w32(uint8_t *p,uint32_t v){w16(p,v);w16(p+2,v>>16);}
void *mock_map(uint64_t pa,uint32_t bytes){
    auto it=maps.find(pa);if(it!=maps.end())return it->second;
    void *p=mock_alloc(bytes,4096);memset(p,0,bytes);maps[pa]=p;
    if(pa==0x15000000){auto *r=static_cast<uint32_t *>(p);r[0]=0x406;r[0x20/4]=1275166324;r[0x24/4]=1610612817;r[0x28/4]=21009;
        for(unsigned i=0;i<116;i++)r[(0xc00+i*4)/4]=0x20000;
        for(unsigned i=0;i<81;i++)r[((128+i)*4096+4)/4]=3;}
    if(pa==0x81d00000){
        auto *m=static_cast<uint8_t *>(p);unsigned toc=bytes-4096,part=0x10000,size=0x40000;
        w32(m+toc,0x434f5424);w32(m+toc+4,1);w32(m+toc+8,1);
        w32(m+toc+32,part);w32(m+toc+36,size);w32(m+toc+44,1u<<16);w32(m+toc+48,64);
        w32(m+part,0x54525024);w32(m+part+4,1u<<16);w32(m+part+8,size);
        w32(m+part+12,missing_smem?32:32+16+NB_IPA_SMEM_BYTES);w32(m+part+16,size);
        if(!missing_smem){w32(m+part+32,0xa5a5u|(NB_SMEM_IPA_FILTER_TABLE<<16));w32(m+part+36,NB_IPA_SMEM_BYTES);}
        if(!missing_smem)memset(m+part+48,0x6b,NB_IPA_SMEM_BYTES);
    }
    if(pa==0x81c60000){auto *db=static_cast<uint8_t *>(p);w32(db+4,QCOM_CMD_DB_MAGIC);w16(db+8,1);w16(db+12,1024);w16(db+14,NB_POWER_RESOURCES);
        const char *names[]={"xo.lvl","cx.lvl","mss.lvl","IP0","MC0","SH0","SH1","SN0","SN3","CE0","CN0"};
        for(unsigned i=0;i<NB_POWER_RESOURCES;i++){uint8_t *e=db+144+i*24,*aux=db+144+1024+i*32;memcpy(e,names[i],strlen(names[i]));w32(e+16,0x10000+i*4);w16(e+20,8);w16(e+22,i*32);
            if(i<3){w16(aux,0);w16(aux+2,320);w16(aux+4,384);w16(aux+6,512);}else{w32(aux,1000);w16(aux+4,16);aux[6]=1;}}}
    return p;
}
static int lock(void *){assert(!locked);locked=1;return 0;}static void unlock(void *){assert(locked);locked=0;}
static int read32(void *,unsigned d,uint32_t off,uint32_t *v){
    if(d==Sm8850Smmu)*v=static_cast<uint32_t *>(mock_map(0x15000000,0x100000))[off/4];
    else if(d==Sm8850Rsc)*v=off==0?0x40300:off==0xc?0x800c8104:0;
    else *v=d==Sm8850Mutex?1:0;
    return 0;
}
static int read64(void *,unsigned d,uint32_t off,uint64_t *v){uint32_t lo,hi;read32(nullptr,d,off,&lo);read32(nullptr,d,off+4,&hi);*v=lo|(uint64_t)hi<<32;return 0;}
static int write32(void *,unsigned d,uint32_t off,uint32_t v){
    assert(locked);assert((d==Sm8850Ipcc && off==0x14)||(d==Sm8850Mutex && off==0 && v<=1));return 0;
}
static int barrier(void *){return 0;}
extern "C" unsigned sm8850_usb_domain_ready(){return 1;}
extern "C" Sm8850Resources *sm8850_native_resources(){return &common;}
extern "C" IOReturn armcpu_spi_attach(uint32_t irq,bool,void (*cb)(void *),void *ctx,uint32_t *rd,uint64_t *route){assert(irq<1024&&!irq_callback[irq]);irq_callback[irq]=cb;irq_context[irq]=ctx;*rd=3;*route=0;return 0;}
extern "C" void class_init() __asm__("__GLOBAL__sub_I_coachz_baseband.cpp");extern "C" void class_init(){classes++;}
extern "C" int native_ufs_bb_open(uint64_t *blocks,uint8_t out[32]){*blocks=archive.size()/4096;memcpy(out,digest,32);return 0;}
extern "C" int native_ufs_bb_read(uint64_t rel,uint32_t blocks,void *out){assert(blocks&&blocks<=16&&rel+blocks<=archive.size()/4096);memcpy(out,archive.data()+rel*4096,blocks*4096);return 0;}
extern "C" int nezha_bb_scm_pas(unsigned,unsigned,uint64_t,uint64_t){pas_calls++;return 0;}
extern "C" int nezha_bb_scm_query(unsigned command,unsigned image,uint64_t result[4]){
    assert((command==7||command==9)&&(image==4||image==0x26));memset(result,0,32);return 0;
}
extern "C" int nezha_bb_scm_assign(void *,uint64_t pa,int *state,uint64_t *handle,NbRange r,int,int){assert(pa<0x100000000ULL&&r.bytes);*state=1;*handle=1;assigned++;return 0;}
static void event(){}
static int initial_proxy(void *,int on){assert(on);return NB_OK;}
static uint64_t translated(uint64_t root,uint64_t iova){
    const uint64_t mask=0x0000fffffffff000ULL;
    uint64_t pool=root&mask;auto it=buffers.find(pool);assert(it!=buffers.end());
    auto *table=static_cast<uint64_t *>(it->second);uint64_t entry=0;
    for(unsigned level=1;level<=3;level++){
        unsigned shift=39-9*level,index=(unsigned)((iova>>shift)&(level==1?3u:511u));
        entry=table[index];if((entry&3)!=3)return 0;
        if(level==3)return (entry&mask)+(iova&4095);
        assert((entry&mask)>=pool && (entry&mask)-pool<0x10000);
        table=reinterpret_cast<uint64_t *>(static_cast<uint8_t *>(it->second)+(entry&mask)-pool);
    }
    return 0;
}
int main(int argc,char **argv){
    setbuf(stdout,nullptr);assert(argc==2||argc==3);missing_smem=argc==3;
    std::ifstream in(argv[1],std::ios::binary);archive.assign(std::istreambuf_iterator<char>(in),{});assert(!archive.empty());
    unsigned n=32;EVP_Digest(archive.data(),archive.size(),digest,&n,EVP_sha256(),nullptr);
    Sm8850ResourceIo io={nullptr,lock,unlock,read32,write32,read64,barrier};assert(!sm8850_resources_init(&common,&io));
    NbBasebandConfig c;assert(!nezha_bb_bind(&c));assert(classes==1&&assigned==1&&pas_calls==0&&c.rmtfs->count==4);
    assert(!c.ipa_smem_bytes && !c.ipa_smem_address);
    assert(c.modem->stage==NbPasInit&&c.ipa_fws->image.elf64&&c.ipa_memory.ring.iova%65536==0);
    NbMss stopped;NbMssOps stop_ops=c.mss;stop_ops.proxy=initial_proxy;
    assert(!nb_mss_init(&stopped,c.smem,c.smem_bytes,c.modem,c.modem_dtb,&stop_ops));
    uint32_t address=0;
    assert(c.platform.ipa_smmu(c.platform.context,1,&stopped,&c.ipa_memory,&address)==NB_OWNERSHIP);
    assert(!address && !irq_callback[137]);
    assert(nb_mss_prepare_stop(&stopped,0)==NB_PENDING);
    assert(nb_mss_prepare_stop(&stopped,1)==NB_PENDING);
    assert(nb_mss_prepare_stop(&stopped,2)==NB_OK && stopped.masters_stopped && pas_calls==2);
    assert(!c.platform.ipa_smmu(c.platform.context,1,&stopped,&c.ipa_memory,&address));
    assert(address==0x81d10030 && irq_callback[137]);
    auto *stats=reinterpret_cast<volatile uint32_t *>(static_cast<volatile uint8_t *>(c.smem)+(address-0x81d00000));
    const unsigned ids[]={0,1,2,7,8,9,10,11,12,13,17,19};
    assert((stats[1]&0xffff)==12);
    for(unsigned i=0;i<12;i++){assert(stats[2+2*i]==((4u<<16)|ids[i]));assert(stats[3+2*i]==0);}
    assert(stats[0]==(missing_smem?0u:0x6b6b6b6bu));
    assert((stats[1]>>16)==(missing_smem?0u:0x6b6bu));
    assert(stats[26]==(missing_smem?0u:0x6b6b6b6bu));
    stats[3]=23; // An ordinary later counter must survive retrying SMMU enable.
    uint32_t repeated=0;
    assert(!c.platform.ipa_smmu(c.platform.context,1,&stopped,&c.ipa_memory,&repeated) && repeated==address);
    assert(stats[3]==23);
    unsigned cleaned=cache_cleans;
    // Exercise the real PAS state machine through each exact reserved WC alias.
    NbPas *loaders[]={c.ipa_fws,c.modem,c.modem_dtb};
    for(auto *l:loaders){
        int r=NB_PENDING;for(unsigned n=0;r==NB_PENDING&&n<16;n++)r=nb_pas_poll(l,n);
        assert(r==NB_OK&&l->stage==NbPasReady&&l->mapped[0]=='F'&&l->mapped[17]==0);
        assert(!l->ops.sync(l->ops.context,l->mapped+1,l->region.base+1,16));
        assert(!l->ops.sync(l->ops.context,l->metadata+1,l->metadata_pa+1,16));
        assert(l->ops.sync(l->ops.context,l->mapped,l->region.base+1,16)==NB_OWNERSHIP);
        assert(l->ops.sync(l->ops.context,l->metadata,l->metadata_pa+1,16)==NB_OWNERSHIP);
        assert(l->ops.sync(l->ops.context,l->mapped,l->region.base+l->region.bytes-1,2)==NB_OWNERSHIP);
        assert(l->ops.sync(l->ops.context,l->metadata,l->metadata_pa+l->metadata_capacity-1,2)==NB_OWNERSHIP);
        assert(l->ops.sync(l->ops.context,l->mapped,0x80000000,16)==NB_OWNERSHIP);
        assert(l->ops.sync(l->ops.context,l->mapped,UINT64_MAX-1,16)==NB_ARGUMENT);
    }
    assert(cache_cleans==cleaned&&pas_calls==11);
    for(auto *l:loaders)assert(!l->ops.pas(l->ops.context,NB_PAS_SHUTDOWN,l->pas,0,0));
    assert(!nb_native_low_sync(c.rmtfs->shared,c.rmtfs->shared_phys,c.rmtfs->shared_bytes));
    assert(nb_native_low_sync(c.rmtfs->shared,c.rmtfs->shared_phys+1,4096)==NB_OWNERSHIP);
    assert(nb_native_low_sync(c.rmtfs->shared,0xb1500000,4096)==NB_OWNERSHIP);
    assert(!nezha_bb_events(event,event)&&irq_callback[264]);
    auto *regs=static_cast<uint32_t *>(maps[0x15000000]);assert(regs[(0xc00+13*4)/4]==8&&regs[(0xc00+14*4)/4]==8&&regs[(0xc00+12*4)/4]==0x20000);
    uint64_t ttbr=0;memcpy(&ttbr,regs+((128+8)*4096+0x20)/4,8);
    assert(translated(ttbr,address)==address);
    assert(translated(ttbr,address+NB_IPA_SMEM_BYTES-1)==address+NB_IPA_SMEM_BYTES-1);
    assert(translated(ttbr,0x81d0f000)==0 && translated(ttbr,0x81d2a000)==0);
    assert(translated(ttbr,c.ipa_memory.ring.iova)!=0 && translated(ttbr,0x14683000)==0x14683000);
    uint64_t bytes=0;uint32_t handle=0;assert(c.tftp.lookup(c.tftp.context,"ram/modemst1",&bytes,&handle)<0);
    assert(!c.tftp.lookup(c.tftp.context,"modem.mdt",&bytes,&handle));uint8_t read[64];assert(c.tftp.read(c.tftp.context,handle,bytes+1,read,1)<0);
    assert(c.mss.assign(c.mss.context,1)==NB_PENDING);assert(!c.mss.assign(c.mss.context,1));
    assert(!c.modem->ops.pas(nullptr,1,4,0xb2000000,0));assert(c.mss.assign(c.mss.context,0)==NB_OWNERSHIP);
    assert(!c.modem->ops.pas(nullptr,6,4,0,0));assert(c.mss.assign(c.mss.context,0)==NB_PENDING);assert(!c.mss.assign(c.mss.context,0));
    uint64_t pa;assert(!c.memshare.alloc(nullptr,0,8192,&pa));assert(!c.memshare.assign(nullptr,pa,8192,1,1));assert(!c.memshare.assign(nullptr,pa,8192,0,1));c.memshare.release(nullptr,0,pa,8192);
    assert(nezha_bb_bind(&c)==NB_ARGUMENT&&classes==1);
    regs[((128+8)*4096+0x58)/4]=8;irq_callback[137](irq_context[137]);assert(nezha_bb_dispatch()<0);
    for(void *p:allocations)if(p)free(p);
    allocations.clear();maps.clear();buffers.clear();
    puts("PASS native binding: actual initial PAS stops precede SMEM497 and stock statistics once; retry preserves counters; first/last translations and original DMA/PAS/events");
}
