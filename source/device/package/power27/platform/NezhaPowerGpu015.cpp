// SPDX-License-Identifier: BSD-2-Clause-Patent
// GPU015 binding. UFS and touch initialize first; this module never changes a
// context bank. QUP interconnect remains in the inherited firmware state, as
// in the same-device 17 reader. This is a telemetry qualification path, NOT an
// active ICC governor or a high-power charger controller.
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/thread_act.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include "NezhaTsensNative.h"
extern "C" {
#include "NezhaPowerBoard.h"
#include "NezhaPowerDma.h"
extern volatile int nezha_touch_stage;
// osfmk/arm/machine_routines.h; GPU015's input driver uses this same
// architecture entry point. The generic machine header does not declare it.
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
}
namespace {
constexpr uint64_t leaseId = 0x5032373031350001ULL;
constexpr uint32_t voteBits = (0xfu << 18) | (1u << 27);
struct Binding {
    uintptr_t gcc, smmu, gpi, se, smem, mutex, ipcc, tlmm;
    IOLock *lock, *smemLock;
    unsigned references;
    uint32_t baselineBits;
    NpDmaRoute dmaRoute;
    uint64_t dmaPa;
    uint32_t dmaBytes;
    bool ready;
    uint32_t pumpCfg,pumpOut,pumpOwnedCfg;
    bool pumpPinOwned;
    NpAuthMaterial authMaterial;
    NpAdcNativeBootProbe adcProbe;
} binding;
NpPowerBoard board;
uint32_t started;
uint32_t rd(uintptr_t base, uint32_t off) {
    uint32_t value = *reinterpret_cast<volatile uint32_t *>(base + off);
    __asm__ volatile("dmb oshld" ::: "memory"); return value;
}
void wr(uintptr_t base, uint32_t off, uint32_t value) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *reinterpret_cast<volatile uint32_t *>(base + off) = value;
    __asm__ volatile("dsb osh" ::: "memory");
}
uint64_t now(void *) {
    uint64_t ns = 0; absolutetime_to_nanoseconds(mach_continuous_time(), &ns); return ns / 1000000;
}
int valid(void *, uint64_t id) { return binding.ready && id == leaseId; }
int read32(void *, uint64_t id, uint64_t address, uint32_t *value) {
    if (!valid(nullptr,id) || !value || (address & 3)) return EACCES;
    if (address >= 0x100000 && address < 0x200000) *value = rd(binding.gcc, (uint32_t)(address - 0x100000));
    else if (address >= 0xa94000 && address < 0xa98000) *value = rd(binding.se, (uint32_t)(address - 0xa94000));
    else return EACCES;
    return 0;
}
int vote(void *, uint64_t id, int on) {
    if (!valid(nullptr,id)) return EACCES;
    IOLockLock(binding.lock);
    int error = 0;
    uint32_t bits = rd(binding.gcc,0x52008);
    if (on) {
        if (binding.references == UINT32_MAX) error = EOVERFLOW;
        else {
            if (!binding.references) {
                binding.baselineBits = bits & voteBits;
                wr(binding.gcc,0x52008,bits | voteBits);
                if ((rd(binding.gcc,0x52008)&voteBits)!=voteBits) error=EIO;
            }
            if (!error) ++binding.references;
        }
    } else if (!binding.references) error = EINVAL;
    else if (!--binding.references) {
        // Preserve unrelated live bits and the pre-existing firmware votes.
        wr(binding.gcc,0x52008,(bits & ~voteBits) | binding.baselineBits);
        if ((rd(binding.gcc,0x52008)&voteBits)!=binding.baselineBits) error=EIO;
    }
    IOLockUnlock(binding.lock); return error;
}
uint32_t dmaRead(void *,uint32_t off) { return rd(binding.smmu,off); }
int dma(void *, uint64_t id, uint32_t sid, uint64_t pa, uint32_t bytes) {
    if (!valid(nullptr,id) || sid!=0xb6 || !bytes || !pa || (pa&0xfff)) return EACCES;
    IOLockLock(binding.lock);
    int error=0;
    if(np_dma_range(&binding.dmaRoute,pa,bytes)!=NP_DMA_OK)error=EOVERFLOW;
    if(!np_dma_unchanged(dmaRead,nullptr,&binding.dmaRoute))error=EACCES;
    if (!error && binding.dmaPa && (binding.dmaPa!=pa || binding.dmaBytes!=bytes)) error=EBUSY;
    if (!error) { binding.dmaPa=pa;binding.dmaBytes=bytes; }
    IOLockUnlock(binding.lock); return error;
}
int smemLock(void *) {
    IOLockLock(binding.smemLock);
    for (unsigned n=0;n<1000;++n) {
        wr(binding.mutex,0,1); if(rd(binding.mutex,0)==1)return 1; delay(10);
    }
    IOLockUnlock(binding.smemLock); return 0;
}
void smemUnlock(void *) { wr(binding.mutex,0,0);IOLockUnlock(binding.smemLock); }
void ring(void *,uint32_t client,uint32_t signal) {
    if(client==13 && (signal==0 || signal==2))wr(binding.ipcc,0x0c,client<<16|signal);
}
void pause(void *,uint32_t us) {
    while(us) {
        const uint32_t step=us>1000?1000:us;
        delay(static_cast<int>(step));us-=step;
    }
}
void authWait(void *,uint32_t ms) { IOSleep(ms); }
int pumpWake(void *,uint64_t id,int on) {
    if(!valid(nullptr,id)||!binding.tlmm)return EACCES;
    if(on) {
        if(binding.pumpPinOwned)return 0;
        uint32_t cfg=rd(binding.tlmm,0),out=rd(binding.tlmm,4);
        if((cfg>>2)&15)return EBUSY; /* never take a different pin function */
        binding.pumpCfg=cfg;binding.pumpOut=out;
        uint32_t next=(cfg&~0x3ffu)|0x200u;
        if(next&(1u<<11))next|=1u<<12;
        binding.pumpOwnedCfg=next;binding.pumpPinOwned=true;
        wr(binding.tlmm,4,2);wr(binding.tlmm,0,next);
        IOSleep(400); /* exact stock sc8581_init_gpio NLPM startup wait */
        if(rd(binding.tlmm,0)!=next||(rd(binding.tlmm,4)&3)!=3)return EIO;
        IOLog("NEZHA POWER CP: own GPIO114 NLPM high cfg=%x; no other GPIO writes\n",next);
    } else if(binding.pumpPinOwned) {
        if(rd(binding.tlmm,0)!=binding.pumpOwnedCfg||(rd(binding.tlmm,4)&2)!=2)return EBUSY;
        wr(binding.tlmm,4,binding.pumpOut&2);wr(binding.tlmm,0,binding.pumpCfg);
        if(rd(binding.tlmm,0)!=binding.pumpCfg||(rd(binding.tlmm,4)&2)!=(binding.pumpOut&2))return EIO;
        binding.pumpPinOwned=false;
    }
    return 0;
}
int routePlan() {
    int result=np_dma_plan(dmaRead,nullptr,&binding.dmaRoute);
    const NpDmaRoute &p=binding.dmaRoute;
    IOLog("NEZHA POWER DMA: result=%d %s slot=%u smr=%x route=%x bank=%u sctlr=%x cbar=%x cba2r=%x; no SMMU writes\n",
        result,np_dma_reason(result),p.slot,p.smr,p.route,p.bank,p.sctlr,p.cbar,p.cba2r);
    return result==NP_DMA_OK ? 0 : EACCES;
}
int quiesceAdc(void *) { return np_adc_native_stop(); }
int beginConnector(void *,unsigned channel,NpAdcNativeReading *out) {return np_adc_native_begin(channel,out);}
int pollConnector(void *,NpAdcNativeReading *out) {return np_adc_native_poll(out);}
void bootstrap(void *,wait_result_t) {
    int mode=1;(void)PE_parse_boot_argn("nezha-power",&mode,sizeof(mode));
    int adcRequested=0;(void)PE_parse_boot_argn("nezha-power-adc",&adcRequested,sizeof(adcRequested));
    int tsensRequested=0;(void)PE_parse_boot_argn("nezha-power-tsens",&tsensRequested,sizeof(tsensRequested));
    int authRequested=0;
    int authOverride=PE_parse_boot_argn("nezha-power-auth",&authRequested,sizeof(authRequested));
    if((adcRequested==3||adcRequested==4)&&mode!=2&&mode!=5){IOLog("NEZHA POWER ADC: continuous connectors require mode2 or mode5\n");goto done;}
    // GPU015's only later apps-SMMU allocator is touch; no clock/GPI work is
    // started until its allocation has completed. Root UFS precedes IOBSD.
    // GPU015 deliberately delays touch startup by 25 s. The power caller
    // runs before that delay, so a 10 s wait exits before DMA can be ready.
    // Keep the readiness gate and allow the known delay plus initialization.
    unsigned n;
    IOLog("NEZHA POWER: waiting for touch DMA stage=%d, budget=60000ms\n",nezha_touch_stage);
    for(n=0;n<1200 && nezha_touch_stage<5 && nezha_touch_stage>=0;++n)IOSleep(50);
    if(nezha_touch_stage<5) {IOLog("NEZHA POWER: touch DMA unavailable stage=%d waited=%ums\n",nezha_touch_stage,n*50);goto done;}
    binding.lock=IOLockAlloc();binding.smemLock=IOLockAlloc();
    binding.gcc=ml_io_map(0x100000,0x100000);binding.smmu=ml_io_map(0x15000000,0x100000);
    binding.gpi=ml_io_map(0xa00000,0x60000);binding.se=ml_io_map(0xa94000,0x4000);
    binding.smem=ml_io_map(0x81d00000,0x200000);binding.mutex=ml_io_map(0x1f43000,0x1000);
    binding.ipcc=ml_io_map(0x1106000,0x1000);
    if(mode>=2)binding.tlmm=ml_io_map(0x0f100000+114*0x1000,0x1000);
    if(!binding.lock||!binding.smemLock||!binding.gcc||!binding.smmu||!binding.gpi||!binding.se||
       !binding.smem||!binding.mutex||!binding.ipcc)goto done;
    {
        int error=routePlan();if(error){IOLog("NEZHA POWER: DMA route qualification=%d\n",error);goto done;}
        binding.ready=true;
        if(vote(nullptr,leaseId,1))goto done;
        const uint32_t branches[]={0x2315c,0x23148,0x23140,0x23144,0x18508};
        bool clocks=true;
        for(uint32_t off:branches) {
            unsigned tries;for(tries=0;tries<100 && (rd(binding.gcc,off)&0x80000000u);++tries)delay(100);
            if(tries==100)clocks=false;
        }
        if(!clocks){(void)vote(nullptr,leaseId,0);IOLog("NEZHA POWER: QUP clock not running\n");goto done;}
        unsigned gpii=UINT32_MAX;
        // Preferred GPII1 matches the original firmware's I2C5 route. Never
        // stop/reset any pre-existing allocated AP GPII to make room.
        const unsigned order[]={1,0,2,3,4};
        for(unsigned index:order) {
            uint32_t cx=0x10000+0x4000*index;
            if(!(((rd(binding.gpi,cx)|rd(binding.gpi,cx+0x80)|rd(binding.gpi,cx+0x1000))>>20)&15)) {gpii=index;break;}
        }
        uint32_t firmware=rd(binding.se,0x68),fifo=rd(binding.se,0x64);
        uint32_t clk=rd(binding.se,0x48),scl=rd(binding.se,0x278),dfs=rd(binding.se,0x7c);
        IOLog("NEZHA POWER: GPU015 resources GPII=%u firmware=%x fifo=%x clock=%x scl=%x dfs=%x; ICC inherited, no active vote\n",gpii,firmware,fifo,clk,scl,dfs);
        int dropped=vote(nullptr,leaseId,0);
        if(dropped||gpii==UINT32_MAX||((firmware>>8)&255)!=3||!(fifo&1))goto done;
        // Validate actual firmware SCL timing. No guessing an RCG/DFS rate.
        PgpiI2cClock timing={(uint8_t)((clk>>4)&255),(uint8_t)((scl>>20)&255),(uint8_t)((scl>>10)&255),(uint8_t)(scl&255),(uint8_t)(dfs&15)};
        if(((clk>>4)&0xfff)>255 || ((scl>>20)&0x3ff)>255 || ((scl>>10)&0x3ff)>255 || (scl&0x3ff)>255 ||
           !timing.clk_div||!timing.t_cycle||dfs>15)goto done;
        NpBoardResources resources={};resources.context=&binding;resources.lease=leaseId;resources.gpii=gpii;
        resources.stock_clock=timing;resources.smem=(volatile uint8_t *)binding.smem;resources.enable_soccp=1;
        resources.pump_wake=pumpWake;
        if(mode>=2) {
            int pumpError=pumpWake(nullptr,leaseId,1);
            resources.enable_pump=!pumpError;
            if(pumpError&&binding.pumpPinOwned)(void)pumpWake(nullptr,leaseId,0);
            IOLog("NEZHA POWER CP: pin wake result=%d; registers owned=%u\n",pumpError,(unsigned)resources.enable_pump);
        }
        resources.lease_valid=valid;resources.read32=read32;resources.gauge_vote=vote;resources.dma_identity=dma;
        resources.smem_lock=smemLock;resources.smem_unlock=smemUnlock;resources.doorbell=ring;resources.now_ms=now;resources.pause_us=pause;
        NpPowerOps ops={};
        ops.control_mode=mode;
        // Current selected handset is the measured CN BM6N/F profile
        // (actual Android ro.boot.hwc=CN); this is not the user's UI locale.
        ops.country_cn=1;
        int materialError=ENOENT;
        if(mode>=2&&(!authOverride||authRequested==1)) {
            materialError=np_auth_native_material(&binding.authMaterial);
            if(!authOverride)authRequested=!materialError;
        }
        if(authRequested==1&&mode>=2) {
            resources.enable_auth=!materialError;resources.auth_wait_ms=authWait;
            ops.auth_enabled=1;
            ops.auth_material=materialError?nullptr:&binding.authMaterial;
            ops.auth.random_bytes=np_auth_native_random;ops.auth.hmac_sha256=np_auth_native_hmac;
            IOLog("NEZHA POWER AUTH: local parameter result=%d; primary gauge and adapter HMAC, pump remains off\n",materialError);
        }
        // Read schema qualified against the actual OS4.0.0.10 vendor modules
        // collected in the power004 window (see OS4-READ-SCHEMA.md). This does
        // not qualify charger writes, wireless, or any high-power policy.
        ops.schema_verified=1;ops.mca_reply_timeout_ms=1000;
        if(tsensRequested==1) {
            const int mapError=np_tsens_native_prepare();
            ops.read_tsens=np_tsens_native_sample;
            IOLog("NEZHA POWER TSENS: prepare=%d; diagnostic-only last readings\n",mapError);
        }
        ops.property_online=0x20001;ops.property_status=0x2001;
        ops.property_input_uv=0x20002;ops.property_input_ua=0x20004;
        // Actual monitor .rodata+0x260 maps raw0..8 to
        // {charging,charging,charging,charging,charging,done,off,off,charging}.
        // These are PMIC state semantics, separate from net battery current.
        ops.status_valid_mask=0x1ff;ops.status_charging_mask=0x11f;ops.status_done=5;
        ops.notification_mask=0; // no unqualified notification IDs; poll reads
        // No MCA/primary-auth request has started. The prepared single-shot
        // ADC conversion may wait500ms, so it runs only here, never in step().
        if(adcRequested==1||adcRequested==2) {
            int probeError=adcRequested==2?np_adc_native_boot_probe_qtm(&binding.adcProbe):np_adc_native_boot_probe(&binding.adcProbe);
            ops.adc_boot_probe=&binding.adcProbe;ops.quiesce_adc=quiesceAdc;
            IOLog("NEZHA POWER ADC BOOT: error=%d attempted=%u init=%d cleanup=%d; before MCA/auth, no thermal vote\n",
                probeError,binding.adcProbe.attempted,binding.adcProbe.init_error,binding.adcProbe.stop_error);
        }
        if(adcRequested==3||adcRequested==4) {
            ops.begin_connector=beginConnector;ops.poll_connector=pollConnector;
            ops.quiesce_adc=quiesceAdc;
            IOLog("NEZHA POWER ADC: continuous connectors on the existing worker; policy=%u\n",mode==5);
        }
        error=np_board_prepare(&board,&resources,&ops);
        if(!error&&adcRequested==4) {
            ops.read_original_temperature=np_board_read_original_temperature;
            IOLog("NEZHA POWER ADC: eight original board inputs; derived normal temperatures only, no board vote\n");
        }
        if(!error)error=np_power27_bind(&ops);
        if(mode==1)IOLog("NEZHA POWER: bind=%d; gauge and OS4 wired MCA reads; charger writes disabled\n",error);
        else if(mode==4)IOLog("NEZHA POWER: bind=%d; mode4 fixed5/9V, SC8585 USB-OVP readback and FCC<=1500mA; pump remains off\n",error);
        else if(mode==5)IOLog("NEZHA POWER: bind=%d; mode5 stock CN JEITA/full/recharge; normal5V FCC<=1500mA; pump remains off\n",error);
        else IOLog("NEZHA POWER: bind=%d; mode%d guarded ordinary-buck current control; pump remains off\n",error,mode);
        if(!error)nezha_power27_start();
    }
done:
    thread_terminate(current_thread());
}
}
extern "C" void nezha_power27_probe(void) {
    int mode=0;PE_parse_boot_argn("nezha-power",&mode,sizeof(mode));
    if((mode!=1&&mode!=2&&mode!=3&&mode!=4&&mode!=5)||__atomic_exchange_n(&started,1u,__ATOMIC_ACQ_REL))return;
    thread_t t;
    if(kernel_thread_start(bootstrap,nullptr,&t)==KERN_SUCCESS)thread_deallocate(t);
}
