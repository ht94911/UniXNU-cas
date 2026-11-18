// SPDX-License-Identifier: BSD-2-Clause-Patent
// Marble's char-device/worker boundary, adapted to the retained HAP530 core.
// One worker owns all bus operations. Queue decisions use a sleepable hapticGate,
// released during hardware work. cdev callbacks never access MMIO.
#include "NezhaHaptics.h"
#include <PaganiHaptics/HapticsRegs.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IONotifier.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/vnode.h>
#include <sys/namei.h>
#include <vm/memory_types.h>
extern "C" {
#include <QcomUsbDiagnostic.h>
}
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t,vm_size_t,uint32_t);
extern "C" void ml_io_unmap(vm_offset_t,vm_size_t);
extern "C" int nezha_spmi_bus_acquire(void);
extern "C" void nezha_spmi_bus_release(void);
namespace {
IOLock *hapticGate;
PghCore core;
NzHapticsBus bus;
PghFactoryState factory;
NzHapticsQueue queue;
int stopResult,terminalStopResult;
struct Map {uint64_t physical;vm_offset_t address;} maps[32];
unsigned mapped,mode;
uint32_t sessionCounter,logCount;
uint32_t pcmWrites,pcmFrames,pcmNonzero,pcmPeak,pcmLastError,pcmMaxQueue;
int64_t pcmLeadUs;
uint64_t cooldownUntil;
proc_t client;
bool enabled,exitRequested,exited,suspended,iosAssetsReady;
int major=-1;
void *node;
IONotifier *powerNotice;
volatile uint32_t stats[16];
NzHapticsStatus diagnosticStatus;
struct Electrical {uint32_t play,power,source,vmax,amplitude,voltage_raw,current_raw,voltage_mv,current_ma,stop_reads,recoveries,vsense_ctl,isense_ctl,visense_enable,frontend_check,valid,sequence;uint64_t sampled_us;} diagnosticElectrical;
bool diagnosticOwner;
int diagnosticSnapshot(void *,char *,size_t,size_t *,unsigned *);
int diagnosticSubmit(void *,const qud_diag_request *,char *,size_t,size_t *,unsigned *);
static const qud_diag_provider diagnosticProvider={12,1,(1u<<QUD_SNAPSHOT)|(1u<<QUD_CONFIG)|(1u<<QUD_CANCEL),4,500,
    "nezha-haptics",nullptr,diagnosticSnapshot,diagnosticSubmit};
static_assert(sizeof(NzHapticsPacket)==40,"haptics packet ABI");
uint64_t nowUs(void *) {uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000;}
void pauseUs(void *,unsigned us) {delay((int)us);}
int busAcquire(void *) {return nezha_spmi_bus_acquire();}
void busRelease(void *) {nezha_spmi_bus_release();}
bool physicalRead(uint64_t a) {
    return (a>=0x0c400000 && a<0x0c401000) || (a>=0x0c403000 && a<0x0c40b000) ||
        (a>=0x0c42a000 && a<0x0c432000) || (a>=0x0c43b000 && a<0x0c443000) ||
        (a>=0x0c4c0000 && a<0x0c8c0000) || (a>=0x0c900000 && a<0x0cd00000);
}
vm_offset_t address(uint64_t a) {
    if((a&3) || !physicalRead(a))return 0;
    const uint64_t page=a&~4095ull;
    for(unsigned i=0;i<mapped;++i)if(maps[i].physical==page)return maps[i].address+(a-page);
    if(mapped==sizeof maps/sizeof maps[0])return 0;
    vm_offset_t v=ml_io_map_unmappable(page,4096,VM_WIMG_IO);if(!v)return 0;
    maps[mapped++]={page,v};return v+(a-page);
}
int read32(void *,uint64_t a,uint32_t *v) {
    vm_offset_t p=address(a);if(!p || !v)return ENXIO;
    *v=*(volatile uint32_t *)p;__asm__ volatile("dmb oshld":::"memory");return 0;
}
int write32(void *,uint64_t a,uint32_t v) {
    if(!((a>=0x0c4c0000 && a<0x0c8c0000)||(a>=0x0c900000 && a<0x0cd00000)))return EACCES;
    vm_offset_t p=address(a);if(!p)return ENXIO;
    __asm__ volatile("dmb oshst":::"memory");*(volatile uint32_t *)p=v;
    __asm__ volatile("dsb osh":::"memory");return 0;
}
void trace(void *,const char *line) {if(__atomic_load_n(&logCount,__ATOMIC_RELAXED)<64 && __atomic_fetch_add(&logCount,1,__ATOMIC_RELAXED)<64)IOLog("NEZHA HAPTICS: %s\n",line);}
void note(const char *state,uint32_t seq,int error) {
    if(__atomic_load_n(&logCount,__ATOMIC_RELAXED)<64 && __atomic_fetch_add(&logCount,1,__ATOMIC_RELAXED)<64){IOLog("NEZHA HAPTICS: %s session=%u epoch=%u seq=%u error=%d play=%u stop=%u status=%x\n",
        state,queue.session,queue.epoch,seq,error,core.plays,core.stops,bus.bus.status);}
}
int darwinError(int e) {
    if(!e)return 0;
    switch(e) {case -16:return EBUSY;case -22:return EINVAL;case -1:case -13:return EACCES;
        case -19:return ENODEV;case -34:return ERANGE;case -110:return ETIMEDOUT;default:return EIO;}
}
void restPCM(uint64_t now) {
    cooldownUntil=now+500000;
    queue.rest_until_us=cooldownUntil;
    queue.head=queue.count=0;
    queue.active_sequence=0;queue.active_until_us=0;queue.on_begin_us=0;
    // The hardware time/duty cap is a rest interval, not a broken stream.
    // Silent/catch-up buffers retire normally until the next legal output.
}
int playbackResult(int result,uint64_t now) {
    // An original core budget/protection refusal is recoverable only after
    // its STOP succeeded and no hardware command remains outstanding.
    if(result==-EBUSY && core.state==PGH_ST_IDLE &&
       !(core.last_play_reg&PGH_PLAY_EN) && !bus.bus.pending) {
        restPCM(now);return 0;
    }
    return result;
}
void cacheElectrical(bool valid) {
    const uint32_t sequence=diagnosticElectrical.sequence+1;
    diagnosticElectrical={core.last_play_reg,core.last_hpwr,core.last_driver_source,core.last_vmax_reg,
        core.last_amplitude_reg,core.voltage_peak_raw,core.current_peak_raw,core.voltage_peak_mv,
        core.current_peak_ma,core.stop_readbacks,core.stop_recoveries,core.vsense_ctl,core.isense_ctl,
        core.visense_enable,core.frontend_check,valid?1u:0u,sequence,valid?nowUs(nullptr):0};
}
int stopped(const char *why,bool completion=false) {
    const bool hadPlay=queue.active_sequence || core.state==PGH_ST_DIRECT || core.state==PGH_ST_EFFECT;
    const PghCmd command={PGH_CMD_STOP,0,0,0};
    IOLockUnlock(hapticGate);
    int e=pgh_core_command(&core,&command);
    /* Cancel releases diagnostic ownership before this worker stops the
     * motor. Always sample the completed STOP before publishing its status. */
    if(!e)e=pgh_core_output(&core);
    IOLockLock(hapticGate);
    cacheElectrical(!e);
    if(hadPlay || e)note(why,queue.active_sequence,e);
    if(!e){queue.active_sequence=0;queue.active_until_us=0;queue.on_begin_us=0;if(hadPlay && completion)++queue.completed;}
    else {queue.error=(uint32_t)darwinError(e);queue.ready=0;nz_haptics_queue_close(&queue);}
    stopResult=darwinError(e);return e;
}
bool allowed(proc_t p) {
    const uid_t user=kauth_getuid();
    /* GPU051's actual signed launchd cache runs audiomxd as mobile(501). */
    if(!p || (user!=0 && user!=501))return false;
    char name[32]={0};proc_name(proc_pid(p),name,sizeof name);
    return !strcmp(name,"audiomxd") || !strcmp(name,"coreaudiod") ||
        !strcmp(name,"mediaserverd") || !strcmp(name,"NezhaHapticTest");
}
int openDevice(dev_t,int,int,struct proc *p) {
    if(!hapticGate || !allowed(p))return EACCES;
    IOLockLock(hapticGate);int e=0;
    if(!enabled || exitRequested || suspended)e=ENXIO;
    else if(client || queue.stopping || queue.active_sequence || nowUs(nullptr)<cooldownUntil)e=EBUSY;
    else if(sessionCounter==UINT32_MAX)e=EOVERFLOW;
    else {client=p;diagnosticOwner=false;nz_haptics_queue_open(&queue,++sessionCounter);}
    note("HAL open",0,e);
    IOLockUnlock(hapticGate);return e;
}
int closeDevice(dev_t,int,int,struct proc *) {
    if(!hapticGate)return 0;
    IOLockLock(hapticGate);
    /* An exclusive vnode can be closed by a passed fd's final owner. Closing
     * it always cancels the session; it never grants a new producer access. */
    if(client){client=nullptr;nz_haptics_queue_close(&queue);IOLockWakeup(hapticGate,&queue,false);}
    IOLockUnlock(hapticGate);return 0;
}
int ioctlDevice(dev_t,u_long command,caddr_t data,int,struct proc *p) {
    if(command!=NZ_HAPTICS_GET_STATUS || !data)return ENOTTY;
    IOLockLock(hapticGate);int e=client!=p?EACCES:0;
    if(!e)nz_haptics_status(&queue,(NzHapticsStatus *)data);
    note("HAL status",0,e);
    IOLockUnlock(hapticGate);return e;
}
uint64_t packetTime(uint64_t host,uint64_t now,int *error) {
    if(!host)return now;
    uint64_t at=mach_absolute_time(),limit=0,delta=host>at?host-at:at-host,ns=0;
    nanoseconds_to_absolutetime(250000000,&limit);
    if(delta>limit){*error=ERANGE;return 0;}
    absolutetime_to_nanoseconds(delta,&ns);uint64_t us=ns/1000;
    if(host>=at)return now+us;
    if(us>now){*error=ERANGE;return 0;}return now-us;
}
int request(const NzHapticsPacket *p,uint64_t now) {
    if(p->session!=queue.session || p->epoch!=queue.epoch || p->sequence!=queue.next_sequence ||
       !queue.running || !queue.ready)return ESTALE;
    if(queue.count || queue.active_sequence || queue.stopping || now<cooldownUntil)return EBUSY;
    if(!p->value || p->value>1000 || p->host_time)return EINVAL;
    if(p->operation==NzHapticsPulse && (!p->count || p->count>500))return EINVAL;
    if(p->operation==NzHapticsEffect && p->count>=NZ_HAPTICS_EFFECT_COUNT)return EINVAL;
    if(queue.next_sequence==UINT32_MAX)return EOVERFLOW;
    uint64_t duration=p->operation==NzHapticsPulse?(uint64_t)p->count*1000:100000;
    queue.segments[0]={now,now+duration,queue.epoch,p->sequence,p->value,p->operation,p->count};
    queue.head=0;queue.count=1;++queue.next_sequence;++queue.requests;return 0;
}
int diagnosticSnapshot(void *,char *out,size_t capacity,size_t *bytes,unsigned *truncated) {
    if(!out || !bytes || !truncated || !hapticGate)return QUD_INVALID;
    if(!IOLockTryLock(hapticGate))return QUD_BUSY;
    NzHapticsStatus s=diagnosticStatus;Electrical electrical=diagnosticElectrical;
    uint32_t phase=stats[0],plays=stats[4],stops=stats[5];
    uint32_t writes=pcmWrites,frames=pcmFrames,nonzero=pcmNonzero,peak=pcmPeak;
    uint32_t lastError=pcmLastError,maxQueue=pcmMaxQueue;
    int64_t lead=pcmLeadUs;
    IOLockUnlock(hapticGate);
    int n=snprintf(out,capacity,"{\"v\":1,\"cached\":true,\"ready\":%u,\"active\":%u,\"queued\":%u,\"error\":%u,\"requests\":%u,\"completed\":%u,\"cancelled\":%u,\"session\":%u,\"epoch\":%u,\"phase\":%u,\"plays\":%u,\"stops\":%u,\"service_ready\":%u,\"service_error\":%d,\"pcm\":{\"writes\":%u,\"frames\":%u,\"nonzero\":%u,\"peak\":%u,\"last_error\":%u,\"lead_us\":%lld,\"max_queue\":%u},\"electrical\":{\"play\":%u,\"power\":%u,\"source\":%u,\"vmax\":%u,\"amplitude\":%u,\"voltage_raw\":%u,\"current_raw\":%u,\"voltage_mv\":%u,\"current_ma\":%u,\"stop_reads\":%u,\"recoveries\":%u,\"vsense_ctl\":%u,\"isense_ctl\":%u,\"visense_enable\":%u,\"frontend_check\":%u,\"valid\":%u,\"sequence\":%u,\"sampled_us\":%llu},\"mechanical_verified\":false}",
        s.ready,s.active,s.queued,s.error,s.requests,s.completed,s.cancelled,s.session,s.epoch,phase,plays,stops,
        nezha_haptics_ready_state(),nezha_haptics_ready_error(),
        writes,frames,nonzero,peak,lastError,(long long)lead,maxQueue,
        electrical.play,electrical.power,electrical.source,electrical.vmax,electrical.amplitude,electrical.voltage_raw,
        electrical.current_raw,electrical.voltage_mv,electrical.current_ma,electrical.stop_reads,electrical.recoveries,
        electrical.vsense_ctl,electrical.isense_ctl,electrical.visense_enable,electrical.frontend_check,
        electrical.valid,electrical.sequence,(unsigned long long)electrical.sampled_us);
    *truncated=n<0 || (size_t)n>=capacity;*bytes=n<0?0:(size_t)n<capacity?(size_t)n:capacity;
    return QUD_OK;
}
int diagnosticSubmit(void *,const qud_diag_request *r,char *out,size_t capacity,size_t *bytes,unsigned *truncated) {
    if(!r || !out || !bytes || !truncated || !hapticGate || r->provider!=12)return QUD_INVALID;
    if(!IOLockTryLock(hapticGate))return QUD_BUSY;
    int e=0;
    if(!enabled || exitRequested || suspended)e=ENXIO;
    else if(client)e=EBUSY;
    else if(r->op==QUD_CANCEL) {
        if(!diagnosticOwner)e=EACCES;
        else {nz_haptics_queue_close(&queue);diagnosticOwner=false;IOLockWakeup(hapticGate,&queue,false);}
    } else if(r->op!=QUD_CONFIG || !r->arg0 || r->arg0>4)e=EINVAL;
    else if(queue.active_sequence || queue.count || queue.stopping)e=EBUSY;
    else if((r->arg0==1 && (!r->arg1 || r->arg1>100)) ||
            (r->arg0==2 && r->arg1>=NZ_HAPTICS_EFFECT_COUNT) ||
            (r->arg0>=3 && (!r->arg1 || r->arg1>500)))e=EINVAL;
    else if(sessionCounter==UINT32_MAX)e=EOVERFLOW;
    else {
        nz_haptics_queue_open(&queue,++sessionCounter);uint32_t epoch=0;
        e=-nz_haptics_queue_start(&queue,queue.session,&epoch);
        NzHapticsPacket p={NZ_HAPTICS_MAGIC,NZ_HAPTICS_ABI,r->arg0==2?NzHapticsEffect:NzHapticsPulse,
            r->arg1,queue.session,epoch,queue.next_sequence,r->arg0==4?1000u:r->arg0==3?500u:250u,0};
        if(!e)e=request(&p,nowUs(nullptr));
        if(!e){diagnosticOwner=true;IOLockWakeup(hapticGate,&queue,false);}
    }
    IOLockUnlock(hapticGate);
    int n=snprintf(out,capacity,"{\"queued\":%s,\"error\":%d}",e?"false":"true",e);
    *truncated=n<0 || (size_t)n>=capacity;*bytes=n<0?0:(size_t)n<capacity?(size_t)n:capacity;
    return !e?QUD_OK:e==EBUSY?QUD_BUSY:e==EINVAL?QUD_INVALID:QUD_FAILED;
}
int writeDevice(dev_t,struct uio *uio,int) {
    const auto n=uio_resid(uio);
    if(n<(int)sizeof(NzHapticsPacket) || n>(int)(sizeof(NzHapticsPacket)+NZ_HAPTICS_MAX_FRAMES*2))return EINVAL;
    uint8_t *bytes=(uint8_t *)IOMalloc((size_t)n);if(!bytes)return ENOMEM;
    int e=uiomove((char *)bytes,(int)n,uio);
    NzHapticsPacket packet={};if(!e)memcpy(&packet,bytes,sizeof packet);
    if(!e && (packet.magic!=NZ_HAPTICS_MAGIC || packet.version!=NZ_HAPTICS_ABI ||
       packet.operation<NzHapticsStart || packet.operation>NzHapticsEffect ||
       n!=(int)(sizeof packet+(packet.operation==NzHapticsPCM?(uint64_t)packet.count*2:0))))e=EINVAL;
    if(!e) {
        IOLockLock(hapticGate);
        if(client!=current_proc() || !enabled || exitRequested || suspended)e=ENXIO;
        else if(packet.operation==NzHapticsStart) {
            if(packet.count || packet.value || packet.host_time || packet.epoch!=queue.epoch || packet.sequence)e=EINVAL;
            else {uint32_t epoch=0;e=-nz_haptics_queue_start(&queue,packet.session,&epoch);}
            note("HAL start",packet.sequence,e);
        } else if(packet.operation==NzHapticsStop) {
            if(packet.count || packet.value || packet.host_time)e=EINVAL;
            else if(packet.sequence!=queue.next_sequence)e=ESTALE;
            else {
                e=-nz_haptics_queue_stop(&queue,packet.session,packet.epoch);
                if(!e) {
                    IOLockWakeup(hapticGate,&queue,false);
                    uint64_t deadline;clock_interval_to_deadline(100,NSEC_PER_MSEC,&deadline);
                    while(queue.stopping && !exited)if(IOLockSleepDeadline(hapticGate,&queue,deadline,THREAD_UNINT)==THREAD_TIMED_OUT)break;
                    e=queue.stopping?ETIMEDOUT:stopResult;
                    if(exited && !e)e=ENXIO;
                }
            }
        } else if(packet.operation==NzHapticsPCM) {
            uint64_t now=nowUs(nullptr),at=packetTime(packet.host_time,now,&e);
            ++pcmWrites;pcmFrames+=packet.count;
            pcmLeadUs=at>=now?(int64_t)(at-now):-(int64_t)(now-at);
            const int16_t *samples=(const int16_t *)(bytes+sizeof packet);
            for(unsigned i=0;i<packet.count;++i) {
                int32_t sample=samples[i];if(sample)++pcmNonzero;
                uint32_t peak=(uint32_t)(sample<0?-sample:sample);if(peak>pcmPeak)pcmPeak=peak;
            }
            queue.rest_until_us=cooldownUntil;
            if(!e)e=-nz_haptics_queue_pcm(&queue,&packet,(const int16_t *)(bytes+sizeof packet),now,at);
            pcmLastError=(uint32_t)e;if(queue.count>pcmMaxQueue)pcmMaxQueue=queue.count;
        } else e=request(&packet,nowUs(nullptr));
        if(!e){if(packet.operation!=NzHapticsPCM || queue.requests<=4)note("queued",packet.sequence,0);IOLockWakeup(hapticGate,&queue,false);}
        else if(packet.operation==NzHapticsPCM)note("PCM rejected",packet.sequence,e);
        IOLockUnlock(hapticGate);
    }
    IOFree(bytes,(size_t)n);return e;
}
struct cdevsw deviceSwitch={.d_open=openDevice,.d_close=closeDevice,.d_read=eno_rdwrt,.d_write=writeDevice,
    .d_ioctl=ioctlDevice,.d_stop=eno_stop,.d_reset=eno_reset,.d_ttys=nullptr,.d_select=eno_select,
    .d_mmap=eno_mmap,.d_strategy=eno_strat,.d_reserved_1=eno_getc,.d_reserved_2=eno_putc,.d_type=0};
IOReturn powerMessage(void *,void *,UInt32 type,IOService *,void *,vm_size_t) {
    if(type==kIOMessageSystemWillPowerOff || type==kIOMessageSystemWillRestart) {
        return nezha_haptics_quiesce()?kIOReturnNotReady:kIOReturnSuccess;
    }
    if(type==kIOMessageSystemWillSleep)nezha_haptics_suspended(1);
    if(type==kIOMessageSystemHasPoweredOn)nezha_haptics_suspended(0);
    return kIOReturnSuccess;
}
void worker(void *,wait_result_t) {
    Sm8850SpmiOps io={nullptr,read32,write32,pauseUs,busAcquire,busRelease};
    PghOps ops={nullptr,nowUs,pauseUs,trace,nullptr};PghConfig cfg={};
    int quiesceError=0;
    uint64_t electricalAt=0;
    int e=nz_haptics_bus_init(&bus,&io);
    nz_haptics_config(&cfg,nullptr,PGH_MODE_DRY);
    if(!e){e=pgh_core_init(&core,&cfg,&ops,&bus.io);if(e==1)e=0;}
    if(mode==NZ_HAPTICS_PROBE || e)goto done;
    /* Dry identity must be qualified before any status selector write. */
    if(!core.spmi.p[PGH_CFG].identified || core.spmi.p[PGH_CFG].rev[1]!=5 ||
       core.spmi.p[PGH_CFG].type!=0x15 || core.spmi.p[PGH_CFG].subtype!=0x0a ||
       !core.spmi.p[PGH_PTN].identified || core.spmi.p[PGH_PTN].rev[1]!=5 ||
       core.spmi.p[PGH_CFG].owner || core.spmi.p[PGH_PTN].owner){e=-ENODEV;goto done;}
    e=nz_haptics_factory(&bus,&factory);
    IOLog("NEZHA HAPTICS: factory result=%d period=%u rc-mode=%u rc-count=%u\n",e,factory.t_lra_us,factory.rc_mode,factory.rc_count);
    if(e)goto done;
    nz_haptics_config(&cfg,&factory,PGH_MODE_ON);e=pgh_core_init(&core,&cfg,&ops,&bus.io);
    if(e || mode==NZ_HAPTICS_PREPARE)goto done;
    major=cdevsw_add(-1,&deviceSwitch);
    if(major<0){e=-ENXIO;goto done;}
    node=devfs_make_node(makedev((uint32_t)major,0),DEVFS_CHAR,501,501,0600,"nezha-haptics27");
    if(!node){e=-ENXIO;goto done;}
    powerNotice=registerPrioritySleepWakeInterest(powerMessage,nullptr,nullptr);
    if(!powerNotice){e=-ENOMEM;goto done;}
    IOLockLock(hapticGate);enabled=true;queue.stopping=0;diagnosticStatus.ready=1;IOLockWakeup(hapticGate,&enabled,false);
    IOLockUnlock(hapticGate);qcom_usb_diag_register(&diagnosticProvider);IOLockLock(hapticGate);
    IOLog("NEZHA HAPTICS: Actuator endpoint ready, factory%uus cap2000mV envelope2ms\n",factory.t_lra_us);
    while(!exitRequested) {
        uint64_t now=nowUs(nullptr);
        // The motor worker serializes the service lifetime with hardware
        // readiness. Registry matching never runs while hapticGate is held.
        const bool serviceReady=iosAssetsReady && !suspended && !queue.error;
        IOLockUnlock(hapticGate);
        int serviceError=nezha_haptics_ready_update(serviceReady);
        IOLockLock(hapticGate);
        if(serviceError){e=-serviceError;queue.error=(uint32_t)serviceError;break;}
        if(exitRequested)break;
        if(queue.stopping || suspended) {
            e=stopped(queue.stopping?"cancel":"suspend");queue.stopping=0;IOLockWakeup(hapticGate,&queue,false);
            if(e)break;
        }
        unsigned oldState=(unsigned)core.state;
        IOLockUnlock(hapticGate);(void)pgh_core_poll(&core);IOLockLock(hapticGate);
        now=nowUs(nullptr);
        if(core.state==PGH_ST_FAULT){e=core.fault_rc;queue.error=(uint32_t)darwinError(e);break;}
        if(queue.stopping || exitRequested)continue;
        if(oldState==PGH_ST_DIRECT && core.state==PGH_ST_IDLE) {
            if(now>=core.deadline || core.budget_us<=0) {
                restPCM(now);note("duration rest",queue.active_sequence,0);
            } else {
                // The original core can stop for auto-resonance/HPWR
                // protection and restore its fallback before becoming idle.
                note("protected stop",queue.active_sequence,0);
            }
            queue.active_sequence=0;queue.on_begin_us=0;
            ++queue.completed;
        } else if(oldState==PGH_ST_EFFECT && core.state==PGH_ST_IDLE) {
            ++queue.completed;queue.active_sequence=0;
        }
        NzHapticsSegment segment={};
        if(!suspended && nz_haptics_queue_next(&queue,now,&segment)>0) {
            if(now<cooldownUntil) {++queue.underruns;continue;}
            const uint32_t session=queue.session;
            queue.active_sequence=segment.sequence;queue.active_operation=segment.operation;
            IOLockUnlock(hapticGate);
            if(segment.operation==NzHapticsEffect) {
                PghCmd command={PGH_CMD_EFFECT,segment.effect,segment.amplitude,0};e=pgh_core_command(&core,&command);
            } else if(!segment.amplitude) {
                const PghCmd stop={PGH_CMD_STOP,0,0,0};e=pgh_core_command(&core,&stop);
            }
            else if(core.state==PGH_ST_DIRECT)e=pgh_core_amplitude(&core,segment.amplitude);
            else {
                unsigned ms=segment.operation==NzHapticsPulse?(unsigned)((segment.end_us-now+999)/1000):500;
                PghCmd command={PGH_CMD_CONSTANT,segment.amplitude,ms,0};e=pgh_core_command(&core,&command);
            }
            IOLockLock(hapticGate);
            /* Cancellation can arrive while the bus operation runs. Retire
             * that operation, stop it, then acknowledge the cancelling epoch.
             * The request hapticGate never blocks PCM on a hardware transaction. */
            if(queue.stopping || session!=queue.session || segment.epoch!=queue.epoch || exitRequested) {
                int stop=stopped("cancel during hardware");queue.stopping=0;IOLockWakeup(hapticGate,&queue,false);
                if(stop){e=stop;break;}continue;
            }
            int result=playbackResult(e,nowUs(nullptr));
            if(e && !result)note("output rest",segment.sequence,e);
            e=result;
            if(e){note("rejected",segment.sequence,e);queue.error=(uint32_t)darwinError(e);queue.stopping=1;
                queue.head=queue.count=0;queue.running=0;}
            else if(segment.amplitude && core.state!=PGH_ST_IDLE){queue.active_sequence=segment.sequence;
                queue.active_until_us=segment.end_us+(segment.operation==NzHapticsPCM?8000:0);}
            else {queue.active_sequence=0;queue.active_until_us=0;}
        }
        now=nowUs(nullptr);
        /* Sample once at readiness, during either USB or the normal HAL
         * session, and after a hardware state transition. */
        if(!diagnosticElectrical.valid || oldState!=(unsigned)core.state ||
           ((diagnosticOwner || client || core.state==PGH_ST_DIRECT || core.state==PGH_ST_EFFECT) && now>=electricalAt)) {
            IOLockUnlock(hapticGate);e=pgh_core_output(&core);IOLockLock(hapticGate);
            electricalAt=now+10000;
            cacheElectrical(!e);
            if(e){queue.error=(uint32_t)darwinError(e);break;}
        }
        if(queue.active_sequence && now>=queue.active_until_us) {
            e=stopped("finished",true);if(e)break;
        }
        stats[0]=(uint32_t)core.state;stats[1]=queue.error;stats[2]=queue.requests;stats[3]=queue.completed;
        stats[4]=core.plays;stats[5]=core.stops;stats[6]=queue.underruns;
        nz_haptics_status(&queue,&diagnosticStatus);diagnosticStatus.ready=enabled && !queue.error;
        uint64_t deadline;clock_interval_to_deadline(1,NSEC_PER_MSEC,&deadline);
        (void)IOLockSleepDeadline(hapticGate,&queue,deadline,THREAD_UNINT);
    }
    enabled=false;IOLockUnlock(hapticGate);
done:
    // Withdraw the first-match provider before removing the endpoint. A
    // failed stop still retains the existing hardware mappings as before.
    (void)nezha_haptics_ready_update(0);
    if(core.state!=PGH_ST_DRY && core.state!=PGH_ST_OFF) {
        int stop=pgh_core_quiesce(&core);if(stop){e=stop;quiesceError=darwinError(stop);}
    }
    /* The notice and hapticGate have kernel lifetime. Removing a notifier from its
     * worker while the shutdown callback waits for exited would deadlock. */
    if(node){devfs_remove(node);node=nullptr;}
    if(major>=0){cdevsw_remove(major,&deviceSwitch);major=-1;}
    /* Failed quiescence/pending bus command retains its mappings. A timer or
     * a queued STOP is never used as permission to unmap live hardware. */
    if((!e || core.state==PGH_ST_DRY || core.state==PGH_ST_OFF) && !bus.bus.pending && !core.restore_pending)
        for(unsigned i=0;i<mapped;++i)ml_io_unmap(maps[i].address,4096);
    /* An identity/ownership refusal before touching the motor is a diagnostic
     * error, not a failed stop. Only unresolved hardware blocks shutdown. */
    IOLockLock(hapticGate);terminalStopResult=quiesceError?quiesceError:(bus.bus.pending || core.restore_pending?EIO:0);queue.error=(uint32_t)darwinError(e);enabled=false;exited=true;
    if(!terminalStopResult){queue.active_sequence=0;queue.active_until_us=0;queue.stopping=0;}
    nz_haptics_status(&queue,&diagnosticStatus);diagnosticStatus.ready=0;diagnosticStatus.error=queue.error;
    stats[0]=(uint32_t)core.state;stats[4]=core.plays;stats[5]=core.stops;
    /* OFF retains the terminal PLAY_EN/HPWR readback. Peak registers may
     * retain the last pulse, so the zero-output criterion uses enable/power. */
    cacheElectrical(diagnosticElectrical.valid && !terminalStopResult);
    IOLockWakeup(hapticGate,&exited,false);IOLockWakeup(hapticGate,&enabled,false);IOLockUnlock(hapticGate);
    IOLog("NEZHA HAPTICS: worker exit error=%d mapped=%u pending=%u\n",e,mapped,bus.bus.pending);
    thread_terminate(current_thread());
    thread_block(THREAD_CONTINUE_NULL);
}
}
extern "C" void nezha_haptics_start(void) {
    uint32_t selected=0;
    if(hapticGate || !PE_parse_boot_argn("nezha-haptics",&selected,sizeof selected) || !selected || selected>3)return;
    hapticGate=IOLockAlloc();if(!hapticGate)return;mode=selected;
    thread_t t;if(kernel_thread_start(worker,nullptr,&t)==KERN_SUCCESS)thread_deallocate(t);
    else {exited=true;queue.error=ENOMEM;}
}
extern "C" int nezha_haptics_quiesce(void) {
    if(!hapticGate)return 0;IOLockLock(hapticGate);exitRequested=true;queue.stopping=1;queue.running=0;
    IOLockWakeup(hapticGate,&queue,false);
    uint64_t deadline;clock_interval_to_deadline(1000,NSEC_PER_MSEC,&deadline);
    while(!exited)if(IOLockSleepDeadline(hapticGate,&exited,deadline,THREAD_UNINT)==THREAD_TIMED_OUT)break;
    int e=exited?terminalStopResult:ETIMEDOUT;IOLockUnlock(hapticGate);return e;
}
extern "C" int nezha_haptics_wait_ready(uint32_t timeout_ms) {
    if(!hapticGate || !timeout_ms || timeout_ms>2000)return EINVAL;
    IOLockLock(hapticGate);uint64_t deadline;clock_interval_to_deadline(timeout_ms,NSEC_PER_MSEC,&deadline);
    while(!enabled && !exited)
        if(IOLockSleepDeadline(hapticGate,&enabled,deadline,THREAD_UNINT)==THREAD_TIMED_OUT)break;
    int e=enabled?0:queue.error?(int)queue.error:ETIMEDOUT;IOLockUnlock(hapticGate);return e;
}
extern "C" int nezha_haptics_suspended(int suspend) {
    if(!hapticGate)return 0;IOLockLock(hapticGate);suspended=suspend!=0;
    if(suspended){queue.running=0;nz_haptics_queue_close(&queue);}IOLockWakeup(hapticGate,&queue,false);
    uint64_t deadline;clock_interval_to_deadline(100,NSEC_PER_MSEC,&deadline);
    while(suspended && queue.stopping && !exited)
        if(IOLockSleepDeadline(hapticGate,&queue,deadline,THREAD_UNINT)==THREAD_TIMED_OUT)break;
    int e=suspended && queue.stopping?ETIMEDOUT:stopResult;
    IOLockUnlock(hapticGate);return e;
}
extern "C" int nezha_haptics_runtime_ready(void);
extern "C" int nezha_haptics_runtime_ready(void) {
    if(!hapticGate)return 0;IOLockLock(hapticGate);int ready=enabled && !exitRequested && mode==NZ_HAPTICS_ON;
    IOLockUnlock(hapticGate);return ready;
}
extern "C" int nezha_haptics_ios_assets_ready(void) {
    if(!hapticGate)return ENXIO;
    IOLockLock(hapticGate);
    int error=enabled && !exitRequested && !exited && !suspended && !queue.error &&
        mode==NZ_HAPTICS_ON?0:ENXIO;
    if(!error){iosAssetsReady=true;IOLockWakeup(hapticGate,&queue,false);}
    IOLockUnlock(hapticGate);
    return error;
}
extern "C" boolean_t nezha_haptics_vnode(vfs_context_t ctx,vnode_t vp,struct componentname *name) {
    if(!ctx || !vp || !allowed(vfs_context_proc(ctx)))return FALSE;
    char filesystem[32]={0};vnode_vfsname(vp,filesystem);
    if(strcmp(filesystem,"devfs"))return FALSE;
    if(name)return vnode_vtype(vp)==VDIR && name->cn_namelen==sizeof("nezha-haptics27")-1 &&
        !memcmp(name->cn_nameptr,"nezha-haptics27",sizeof("nezha-haptics27")-1);
    if(vnode_vtype(vp)!=VCHR)return FALSE;
    const char *actual=vnode_getname(vp);bool matched=actual && !strcmp(actual,"nezha-haptics27");
    if(actual)vnode_putname(actual);return matched;
}
extern "C" boolean_t nezha_haptics_ioctl(vfs_context_t ctx,vnode_t vp,u_long command) {
    return command==NZ_HAPTICS_GET_STATUS && nezha_haptics_vnode(ctx,vp,nullptr);
}
