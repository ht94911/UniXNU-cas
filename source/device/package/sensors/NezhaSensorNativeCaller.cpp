// SPDX-License-Identifier: BSD-2-Clause-Patent
// Adapter install/poll hooks for the baseline's existing fresh ADSP worker.
#ifdef NEZHA_SENSORS_CALLER_HOST
#include "sensors-caller-host-shim.h"
#else
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>
#include "../als-owner/os/NezhaAudioNative.h"
#endif
#include "NezhaSensors.h"
#include "NezhaSensorNativeCaller.h"
#include "../als-owner/platform/AdspLink.h"
namespace {
struct Caller {
    NaAdsp *owner=nullptr;
    void *event=nullptr;
    IOLock *commands=nullptr;
    struct nezha_sensor_adapter adapter={};
    IOService *provider=nullptr;
    uint64_t epoch=0;
    uint64_t next_log=0;
    IONotifier *power=nullptr;
    uintptr_t sleep_ref=0;
    uint32_t lifecycle=0,stop_complete=0,power_request=0;
    int stop_result=0,error=0;
    bool attempted=false,installed=false,stop_started=false;
};
Caller caller;
void lock(void *v){IOLockLock(static_cast<Caller *>(v)->commands);}
void unlock(void *v){IOLockUnlock(static_cast<Caller *>(v)->commands);}
void wake(void *v){thread_wakeup((event_t)static_cast<Caller *>(v)->event);}
bool online(void *v){auto a=static_cast<Caller *>(v)->owner;
    return a&&a->stage==NaAdspOnline&&a->mapper.online&&!a->error&&!a->mapper.error&&!a->glink.broken;}
uint64_t now(void *){return na_native_now_us(nullptr);}
uint64_t darwinTicks(void *){return na_native_host_time(nullptr);}
uint64_t counter(void *){
    // Stock sensors_timeutil reads CNTVCT for SSC's platform QTimer domain.
    // Pairing removes the Darwin boot epoch; first hardware batches must
    // confirm that the retained firmware uses this same counter relation.
#ifdef NEZHA_SENSORS_CALLER_HOST
    return sensors_host_counter;
#else
    uint64_t value;__asm__ volatile("isb\n\tmrs %0, cntvct_el0":"=r"(value));return value;
#endif
}
int room(void *v,uint32_t bytes){auto c=static_cast<Caller *>(v);auto m=&c->owner->mapper;
    if(!online(v)||!bytes||bytes>NA_PD_PACKET_BYTES)return QsscRemote;
    return m->head-m->tail<NA_PD_TX_SLOTS?QsscOkay:QsscAgain;
}
IOReturn powerMessage(void *,void *,UInt32 message,IOService *,void *argument,vm_size_t){
    if(message==kIOMessageCanSystemSleep){
        acknowledgeSleepWakeNotification(argument);
    }else if(message==kIOMessageSystemWillSleep){
        __atomic_store_n(&caller.sleep_ref,(uintptr_t)argument,__ATOMIC_RELEASE);
        __atomic_store_n(&caller.power_request,1u,__ATOMIC_RELEASE);
        wake(&caller);
    }else if(message==kIOMessageSystemHasPoweredOn){
        __atomic_store_n(&caller.power_request,2u,__ATOMIC_RELEASE);
        wake(&caller);
    }
    return kIOReturnSuccess;
}
void sleepFinished(){
    uintptr_t ref=__atomic_exchange_n(&caller.sleep_ref,0,__ATOMIC_ACQ_REL);
    if(ref)acknowledgeSleepWakeNotification((void *)ref);
}
bool sleepDrained(){
    if(!caller.adapter.selected)return true;
    const auto &c=caller.adapter.client;
    if(c.awaiting||c.queued)return false;
    for(const auto &s:c.stream)if(s.enabled&&!s.wakeup)return false;
    return true;
}
int initializationFailed(int error){
    caller.error=error;
    // No SSC request has left this caller if the client epoch is still zero.
    caller.stop_result=caller.adapter.client.epoch?error:QsscOkay;
    __atomic_store_n(&caller.stop_complete,1u,__ATOMIC_RELEASE);
    return error;
}
}
extern "C" int nezha_sensors_native_install(NaAdsp *a,void *event,uint64_t epoch){
    unsigned selected=0;
    if(!PE_parse_boot_argn("ns",&selected,sizeof selected)||selected!=1||
       (__atomic_load_n(&caller.lifecycle,__ATOMIC_ACQUIRE)&2))return QsscOkay;
    if(!a||!event||!epoch||caller.owner||a->stage!=NaAdspPower||a->glink_initialized)return QsscArgument;
    caller.owner=a;caller.event=event;caller.epoch=epoch;
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&caller.lifecycle,&expected,1u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return QsscOkay;
    caller.installed=true;
    printf("NEZHA SENSORS: install ns=1 epoch=%llu same-ADSP-worker\n",(unsigned long long)epoch);
    return QsscOkay;
}
extern "C" int nezha_sensors_native_prepared(NaAdsp *a){
    if(__atomic_load_n(&caller.lifecycle,__ATOMIC_ACQUIRE)!=1)return QsscOkay;
    if(a!=caller.owner||!caller.installed||caller.attempted||a->stage!=NaAdspOnline||!a->glink_initialized||!a->dma_ready)return QsscArgument;
    caller.attempted=true;caller.provider=IOService::getPlatform();caller.commands=IOLockAlloc();
    if(!caller.provider||!caller.commands)return initializationFailed(QsscConsumer);caller.provider->retain();
#ifdef NEZHA_SENSORS_CALLER_HOST
    uint64_t frequency=sensors_host_frequency;
#else
    uint64_t frequency;__asm__ volatile("mrs %0, cntfrq_el0":"=r"(frequency));
#endif
    if(frequency!=19200000)return initializationFailed(QsscTime);
    struct nezha_sensor_owner_ops ops={&caller,lock,unlock,wake,online,now,counter,darwinTicks,room,19200000,19200000};
    caller.error=nezha_sensors_prepare(&caller.adapter,caller.provider,&a->mapper.qrtr,&ops,caller.epoch);
    if(!caller.error){
        caller.power=registerPrioritySleepWakeInterest(powerMessage,&caller);
        if(!caller.power){nezha_sensors_lost(&caller.adapter,QsscConsumer);caller.error=QsscConsumer;}
    }
    printf("NEZHA SENSORS: prepared rc=%d local=%u power-interest=%u\n",caller.error,caller.adapter.local_port,caller.power!=nullptr);
    return caller.error?initializationFailed(caller.error):QsscOkay;
}
extern "C" int nezha_sensors_native_after_receive(NaAdsp *a){
    if(!(__atomic_load_n(&caller.lifecycle,__ATOMIC_ACQUIRE)&1))return QsscOkay;
    if(caller.error){sleepFinished();return caller.error;}
    if(a!=caller.owner)return QsscArgument;
    if((__atomic_load_n(&caller.lifecycle,__ATOMIC_ACQUIRE)&2)&&!caller.stop_started){
        caller.stop_started=true;
        caller.stop_result=caller.adapter.prepared?nezha_sensors_stop(&caller.adapter):QsscOkay;
    }
    if(caller.adapter.prepared&&!caller.stop_started){
        unsigned power=__atomic_exchange_n(&caller.power_request,0,__ATOMIC_ACQ_REL);
        if(power&&!caller.adapter.error){
            int r=nezha_sensors_sleep(&caller.adapter,power==1);
            if(r)nezha_sensors_lost(&caller.adapter,r);
            printf("NEZHA SENSORS: power sleeping=%u rc=%d\n",power==1,r);
        }
    }
    // Called by the existing owner after GLINK receive and before mapper drain.
    // The callback path contains only software queues/HID work, no PAS/IRQ/FIFO.
    int r=caller.adapter.prepared?nezha_sensors_poll(&caller.adapter):caller.error;
    uint64_t time=na_native_now_us(nullptr);
    if(time>=caller.next_log&&caller.adapter.prepared){
        struct nezha_sensor_status s={};nezha_sensors_status(&caller.adapter,&s);
        printf("NEZHA SENSORS STATUS: selected=%u rc=%d ACK=%u report=%u sample=%u bad=%u stale=%u cmd=%u phase=%u,%u,%u,%u,%u,%u,%u\n",
            s.selected,s.error,s.responses,s.reports,s.delivered,s.invalid,s.stale,s.queued_commands,
            s.phase[0],s.phase[1],s.phase[2],s.phase[3],s.phase[4],s.phase[5],s.phase[6]);
        caller.next_log=time+2000000;
    }
    if(__atomic_load_n(&caller.sleep_ref,__ATOMIC_ACQUIRE)&&
       (caller.adapter.error||caller.stop_started||sleepDrained()))sleepFinished();
    if(caller.stop_started&&!__atomic_load_n(&caller.stop_complete,__ATOMIC_ACQUIRE)){
        if(caller.stop_result||r<0){caller.stop_result=caller.stop_result?caller.stop_result:r;
            __atomic_store_n(&caller.stop_complete,1u,__ATOMIC_RELEASE);}
        else if(!caller.adapter.prepared||nezha_sensors_native_drained(a)){
            printf("NEZHA SENSORS: stop disable-ACKs drained epoch=%llu prior-rc=%d\n",(unsigned long long)caller.epoch,caller.adapter.error);
            __atomic_store_n(&caller.stop_complete,1u,__ATOMIC_RELEASE);
        }
    }
    return r;
}
extern "C" void nezha_sensors_native_lost(NaAdsp *a,int error){
    if(a==caller.owner&&(__atomic_load_n(&caller.lifecycle,__ATOMIC_ACQUIRE)&1)){caller.error=error<0?error:QsscRemote;
        nezha_sensors_lost(&caller.adapter,caller.error);sleepFinished();
        caller.stop_result=!caller.adapter.client.epoch||a->masters_stopped?QsscOkay:caller.error;
        __atomic_store_n(&caller.stop_complete,1u,__ATOMIC_RELEASE);
    }
}
extern "C" int nezha_sensors_native_rebind(NaAdsp *a,uint64_t epoch){
    if(a!=caller.owner||a->stage!=NaAdspOnline||!a->mapper.online||epoch<=caller.epoch||
       (__atomic_load_n(&caller.lifecycle,__ATOMIC_ACQUIRE)&2))return QsscArgument;
    int r=nezha_sensors_rebind(&caller.adapter,&a->mapper.qrtr,epoch);
    if(!r){caller.epoch=epoch;caller.error=0;caller.stop_result=0;__atomic_store_n(&caller.stop_complete,0u,__ATOMIC_RELEASE);}return r;
}
extern "C" int nezha_sensors_native_stop(NaAdsp *a){return a==caller.owner?nezha_sensors_stop(&caller.adapter):QsscArgument;}
extern "C" bool nezha_sensors_native_drained(NaAdsp *a){
    return a==caller.owner&&caller.adapter.prepared&&caller.adapter.stopped&&
        (!caller.adapter.selected||qssc_client_drained(&caller.adapter.client));
}
extern "C" int nezha_sensors_stop_and_wait(uint32_t timeout){
    // Prevent a late prepare from creating subscriptions after shutdown starts.
    unsigned previous=__atomic_fetch_or(&caller.lifecycle,2u,__ATOMIC_ACQ_REL);
    if(!(previous&1))return QsscOkay;
    if(caller.event)wake(&caller);
    uint64_t deadline=na_native_now_us(nullptr)+(uint64_t)timeout*1000;
    while(!__atomic_load_n(&caller.stop_complete,__ATOMIC_ACQUIRE)){
        if(na_native_now_us(nullptr)>=deadline)return QsscTimeout;
        IOSleep(1);
    }
    return caller.stop_result;
}
