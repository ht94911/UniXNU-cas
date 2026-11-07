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
extern "C" {
#include "NezhaPowerBoard.h"
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
    unsigned references, slot, bank, cb, sourceSlot;
    uint32_t baselineBits, route, sourceSmr, idr2;
    uint64_t dmaPa;
    uint32_t dmaBytes;
    bool ready, routeCreated;
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
unsigned addressBits(unsigned v) {
    static const unsigned bits[] = {32,36,40,42,44,48}; return v < 6 ? bits[v] : 0;
}
int dma(void *, uint64_t id, uint32_t sid, uint64_t pa, uint32_t bytes) {
    if (!valid(nullptr,id) || sid!=0xb6 || !bytes || !pa || (pa&0xfff)) return EACCES;
    IOLockLock(binding.lock);
    int error=0;
    for (unsigned shift=0;shift<=8;shift+=4) {
        unsigned bits=addressBits((binding.idr2>>shift)&15);
        if (!bits || pa >= (1ULL<<bits) || bytes > (1ULL<<bits)-pa) error=EOVERFLOW;
    }
    if (rd(binding.smmu,0x800+4*binding.sourceSlot)!=binding.sourceSmr ||
        rd(binding.smmu,0xc00+4*binding.sourceSlot)!=binding.route || rd(binding.smmu,binding.cb)&1 ||
        rd(binding.smmu,0x1800+4*binding.bank)!=1 || rd(binding.smmu,0x1000+4*binding.bank)!=0x10000) error=EACCES;
    if (!error && binding.dmaPa && (binding.dmaPa!=pa || binding.dmaBytes!=bytes)) error=EBUSY;
    if (!error && !binding.routeCreated) {
        uint32_t old=rd(binding.smmu,0x800+4*binding.slot);
        if (old && old!=(0x80000000u|sid)) error=EBUSY;
        else if (!(old&0x80000000u)) {
            wr(binding.smmu,0xc00+4*binding.slot,binding.route);
            wr(binding.smmu,0x800+4*binding.slot,0x80000000u|sid);
        }
        if (!error) binding.routeCreated=true;
    }
    if (!error && (rd(binding.smmu,0x800+4*binding.slot)!=(0x80000000u|sid) ||
                   rd(binding.smmu,0xc00+4*binding.slot)!=binding.route)) error=EIO;
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
int routePlan() {
    uint32_t id1=rd(binding.smmu,0x24), count=rd(binding.smmu,0x20)&255;
    unsigned banks=id1&255, shift=(id1>>31)?16:12, pages=2u<<((id1>>28)&7);
    if(!count || count>128 || !banks || banks>128)return ENODEV;
    binding.sourceSlot=UINT32_MAX;binding.slot=UINT32_MAX;
    for(unsigned i=0;i<count;++i) {
        uint32_t smr=rd(binding.smmu,0x800+4*i);
        if(!(smr&0x80000000u)) { if(binding.slot==UINT32_MAX)binding.slot=i;continue; }
        if(((0x60u^smr)&~(smr>>16)&0x7fff)==0) {
            if(binding.sourceSlot!=UINT32_MAX)return EBUSY;
            binding.sourceSlot=i;binding.sourceSmr=smr;binding.route=rd(binding.smmu,0xc00+4*i);
        }
    }
    binding.bank=binding.route&255;
    uint64_t cb=((uint64_t)pages+binding.bank)<<shift;
    if(binding.sourceSlot==UINT32_MAX || ((binding.route>>16)&3)!=0 || binding.bank>=banks || cb+4>0x100000)return ENODEV;
    binding.cb=(uint32_t)cb;binding.idr2=rd(binding.smmu,0x28);
    if(rd(binding.smmu,binding.cb)&1 || rd(binding.smmu,0x1800+4*binding.bank)!=1 ||
       rd(binding.smmu,0x1000+4*binding.bank)!=0x10000)return EACCES;
    unsigned existing=UINT32_MAX;
    for(unsigned i=0;i<count;++i) {
        uint32_t smr=rd(binding.smmu,0x800+4*i);
        if((smr&0x80000000u)&&(((0xb6u^smr)&~(smr>>16)&0x7fff)==0)) {
            if(existing!=UINT32_MAX || smr!=(0x80000000u|0xb6u) || rd(binding.smmu,0xc00+4*i)!=binding.route)return EBUSY;
            existing=i;
        }
    }
    if(existing!=UINT32_MAX) {binding.slot=existing;binding.routeCreated=true;}
    return binding.slot==UINT32_MAX?ENOSPC:0;
}
void bootstrap(void *,wait_result_t) {
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
        resources.lease_valid=valid;resources.read32=read32;resources.gauge_vote=vote;resources.dma_identity=dma;
        resources.smem_lock=smemLock;resources.smem_unlock=smemUnlock;resources.doorbell=ring;resources.now_ms=now;resources.pause_us=pause;
        NpPowerOps ops={};
        // Read schema qualified against the actual OS4.0.0.10 vendor modules
        // collected in the power004 window (see OS4-READ-SCHEMA.md). This does
        // not qualify charger writes, wireless, or any high-power policy.
        ops.schema_verified=1;ops.mca_reply_timeout_ms=1000;
        ops.property_online=0x20001;ops.property_status=0x2001;
        ops.property_input_uv=0x20002;ops.property_input_ua=0x20004;
        // Actual monitor .rodata+0x260 maps raw0..8 to
        // {charging,charging,charging,charging,charging,done,off,off,charging}.
        // These are PMIC state semantics, separate from net battery current.
        ops.status_valid_mask=0x1ff;ops.status_charging_mask=0x11f;ops.status_done=5;
        ops.notification_mask=0; // no unqualified notification IDs; poll reads
        error=np_board_prepare(&board,&resources,&ops);
        if(!error)error=np_power27_bind(&ops);
        IOLog("NEZHA POWER: bind=%d; gauge and OS4 wired MCA reads; charger writes disabled\n",error);
        if(!error)nezha_power27_start();
    }
done:
    thread_terminate(current_thread());
}
}
extern "C" void nezha_power27_probe(void) {
    int mode=0;PE_parse_boot_argn("nezha-power",&mode,sizeof(mode));
    if(mode!=1||__atomic_exchange_n(&started,1u,__ATOMIC_ACQ_REL))return;
    thread_t t;
    if(kernel_thread_start(bootstrap,nullptr,&t)==KERN_SUCCESS)thread_deallocate(t);
}
