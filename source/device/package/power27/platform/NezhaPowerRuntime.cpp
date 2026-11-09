// SPDX-License-Identifier: BSD-2-Clause-Patent
// A single worker owns the request state and all hardware callbacks. Consumers
// copy one completed generation under a mutex; they never read half-written data.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <mach/thread_act.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include "NezhaPowerRuntime.h"
#include "NezhaPowerLog.h"
#include "QcomXnuUsbDebug.h"

extern "C" IOService *np_battery_service_create(void);
extern "C" uint32_t system_inshutdown;
namespace {
NpPowerOps io;
NpMca mca;
IOLock *snapshotLock;
NpGauge stagingGauge, gauge;
NpCharger stagingCharger, charger;
IOService *source;
uint32_t bound, started, stopRequested, workerExited;
uint32_t suspendRequested;
int lifecycle;
uint64_t generation, refreshAt, queuedAt, transportEpoch;
unsigned cursor;
bool cycle, outgoing, refreshRequested;
uint8_t request[NP_MCA_REQUEST];
uint32_t props[4];
IOLock *controlLock;
NpPowerControl control;
NpAuth auth;
NpAuthCommand authCommand;
NpAuxCommand auxiliaryCommand;
NpPump pumpSample;
uint64_t pumpAt,historyNext=1;
int pumpError=-1,persistentLog,manualDisable;
bool auxiliaryActive,draining;
bool authAuxiliary;
uint32_t loggedBatteryChecks;
bool pumpRegistersReported;
NpPowerHistoryRecord history[128];
qcom_usb_debug_status lastUsb;
bool usbReported;
struct AuxLog {
    uint64_t attachment;
    uint32_t size;
    int wire, decoded;
    uint8_t raw[NP_PD_BYTES];
    bool reported;
};
AuxLog lastAux[NP_META_COUNT];

void log_primary_auth(uint64_t now) {
    if(!persistentLog||!auth.enabled)return;
    IOLockLock(controlLock);
    unsigned checks=auth.battery_checks,verified=auth.battery_ok,error=auth.battery_error;
    IOLockUnlock(controlLock);
    if(checks==loggedBatteryChecks)return;
    char line[256];int size=snprintf(line,sizeof(line),
      "A2 ms=%llu stage=primary checks=%u verified=%u error=%u\n",
      (unsigned long long)now,checks,verified,error);
    if(size>0&&(size_t)size<sizeof(line)&&!np_power_log_push(line,(size_t)size))loggedBatteryChecks=checks;
    IOLog("NEZHA POWER PRIMARY AUTH: checks=%u verified=%u error=%u\n",checks,verified,error);
}

void log_usb(uint64_t now) {
    if(io.control_mode<2||!persistentLog)return;
    NpPowerLogStatus log={};if(!np_power_log_status(&log)||log.phase!=2)return;
    qcom_usb_debug_status status={};qcom_usb_debug_get_status(&status);
    if(status.error==QCOM_USB_DEBUG_STATUS_BUSY)return;
    /* CPU snapshot only. Poll counters do not cause one record per USB tick;
     * preserve connection/generation/failure changes, including failed USB. */
    if(usbReported&&status.phase==lastUsb.phase&&status.error==lastUsb.error&&
       status.core_result==lastUsb.core_result&&status.core_failed==lastUsb.core_failed&&
       status.rx_generation==lastUsb.rx_generation&&status.connected==lastUsb.connected&&
       status.configured==lastUsb.configured&&status.dtr==lastUsb.dtr&&status.io_failed==lastUsb.io_failed)return;
    char line[512];int size=snprintf(line,sizeof(line),
      "U1 ms=%llu phase=%u error=%u result=%d failed=%d io=%u gen=%u connected=%u configured=%u dtr=%u poll=%u events=%u commands=%u last=%x active=%x trace=%u trace_a=%x trace_b=%x buffered=%llu dropped=%llu\n",
      (unsigned long long)now,status.phase,status.error,status.core_result,status.core_failed,status.io_failed,
      status.rx_generation,status.connected,status.configured,status.dtr,status.poll_calls,status.core_events,
      status.core_commands,status.core_last_event,status.core_active_endpoints,status.last_trace,status.trace_a,status.trace_b,
      (unsigned long long)status.buffer_pending,(unsigned long long)status.dropped);
    if(size>0&&(size_t)size<sizeof(line)&&!np_power_log_push(line,(size_t)size)){lastUsb=status;usbReported=true;}
}
void log_aux(int error,int decoded,const uint8_t *bytes,size_t length,uint64_t now,unsigned state,unsigned blocked,unsigned backend,unsigned confirmation) {
    if(!persistentLog)return;
    const size_t rawSize=bytes&&length<=NP_PD_BYTES?length:0;
    AuxLog *previous=auxiliaryCommand.control?nullptr:&lastAux[auxiliaryCommand.index];
    if(previous&&previous->reported&&previous->attachment==auxiliaryCommand.attachment&&
       previous->wire==error&&previous->decoded==decoded&&previous->size==rawSize&&
       !memcmp(previous->raw,bytes?bytes:previous->raw,rawSize))return;
    char line[512];int at=snprintf(line,sizeof(line),
      "A1 ms=%llu attach=%llu control=%u write=%u id=%llu property=%x requested=%u wire=%d decoded=%d size=%zu raw=",
      (unsigned long long)now,(unsigned long long)auxiliaryCommand.attachment,(unsigned)auxiliaryCommand.control,
      (unsigned)auxiliaryCommand.write,(unsigned long long)auxiliaryCommand.id,auxiliaryCommand.property,
      auxiliaryCommand.value,error,decoded,length);
    if(!rawSize&&at>0&&(size_t)at+1<sizeof(line))line[at++]='-';
    for(size_t i=0;i<rawSize&&at>0&&(size_t)at+3<sizeof(line);++i)
        at+=snprintf(line+at,sizeof(line)-(size_t)at,"%02x",bytes[i]);
    if(at<=0||(size_t)at>=sizeof(line))return;
    at+=snprintf(line+at,sizeof(line)-(size_t)at," state=%u blocked=%u",state,blocked);
    if(auxiliaryCommand.control&&at>0&&(size_t)at<sizeof(line))
        at+=snprintf(line+at,sizeof(line)-(size_t)at," backend=%u confirmation=%u",backend,confirmation);
    if(at>0&&(size_t)at<sizeof(line))at+=snprintf(line+at,sizeof(line)-(size_t)at,"\n");
    if(at<=0||(size_t)at>=sizeof(line)||np_power_log_push(line,(size_t)at))return;
    if(previous){previous->reported=true;previous->attachment=auxiliaryCommand.attachment;
        previous->wire=error;previous->decoded=decoded;previous->size=(uint32_t)rawSize;
        if(rawSize)memcpy(previous->raw,bytes,rawSize);}
}

void control_publish(uint64_t now) {
    if(io.control_mode<2)return;
    NpSnapshot snapshot;np_snapshot(&snapshot,&gauge,&charger,now,gauge.generation);
    NpPowerHistoryRecord row={};
    IOLockLock(controlLock);
    np_control_sample(&control,&snapshot,&pumpSample,pumpError,pumpAt);
    row.id=historyNext;row.at_ms=now;row.generation=snapshot.generation;row.attachment=control.attachment;
    row.soc=gauge.soc;row.battery_mv=gauge.voltage_mv;row.battery_ma=gauge.current_ma;
    row.temperature=gauge.temperature_centi_c;row.remaining=gauge.remaining_mah;
    row.external=snapshot.external;row.charging=snapshot.charging;row.valid=snapshot.valid;
    row.vbus_uv=charger.usb_uv;row.buck_ua=charger.usb_ua;
    row.pump_valid=pumpSample.valid;row.pump_error=(uint32_t)pumpError;row.pump_mode=pumpSample.mode;
    row.pump_enable=pumpSample.enable;row.pump_ibus_ma=(uint32_t)pumpSample.bus_ma;
    row.real_type=control.meta[NP_META_TYPE].value;row.icl_ma=control.meta[NP_META_ICL].value;
    row.fcc_ma=control.meta[NP_META_FCC].value;row.state=control.buck.state;row.blocked=control.buck.blocked;row.owned=control.buck.owned;
    row.baseline_icl=control.buck.baseline_icl;row.baseline_fcc=control.buck.baseline_fcc;
    row.target_icl=control.buck.target_icl;row.target_fcc=control.buck.target_fcc;row.control_errors=control.buck.failures;
    row.backend=control.buck.backend;row.confirmation=control.buck.confirmation;
    row.feedback_baseline_ma=control.buck.feedback_baseline_ma;row.feedback_target_ma=control.buck.feedback_target_ma;
    row.confirmed_vbus_mv=control.buck.confirmed_vbus_mv;row.usb_ovp_baseline=control.buck.usb_ovp_baseline;
    row.usb_ovp_raw=pumpSample.control[2]&15;row.protection_restored=control.buck.protection_restored;
    for(unsigned i=0;i<NP_META_COUNT;++i)if(control.meta[i].valid)row.meta_valid|=1u<<i;
    if(historyNext!=UINT64_MAX){history[(historyNext-1)%128]=row;++historyNext;}
    IOLockUnlock(controlLock);
    if(persistentLog) {
        char line[1024];int bytes=snprintf(line,sizeof(line),
          "P1 id=%llu ms=%llu gen=%llu attach=%llu soc=%d mv=%d ma=%d temp=%d rm=%d ext=%d charging=%d valid=%x vbus_uv=%d buck_ua=%d cp_valid=%x cp_error=%u cp_mode=%u cp_on=%u cp_ibus=%u type=%u icl=%u fcc=%u meta=%x state=%u blocked=%u owned=%u base_icl=%u base_fcc=%u target_icl=%u target_fcc=%u ctl_errors=%u backend=%u confirmation=%u vbus_confirmed=%u ovp_base=%u ovp_now=%u ovp_restored=%u\n",
          (unsigned long long)row.id,(unsigned long long)row.at_ms,(unsigned long long)row.generation,(unsigned long long)row.attachment,
          row.soc,row.battery_mv,row.battery_ma,row.temperature,row.remaining,row.external,row.charging,row.valid,row.vbus_uv,row.buck_ua,
          row.pump_valid,row.pump_error,row.pump_mode,row.pump_enable,row.pump_ibus_ma,row.real_type,row.icl_ma,row.fcc_ma,row.meta_valid,row.state,row.blocked,row.owned,
          row.baseline_icl,row.baseline_fcc,row.target_icl,row.target_fcc,row.control_errors,
          row.backend,row.confirmation,row.confirmed_vbus_mv,row.usb_ovp_baseline,row.usb_ovp_raw,row.protection_restored);
        if(bytes>0&&(size_t)bytes<sizeof(line))(void)np_power_log_push(line,(size_t)bytes);
    }
}
void auxiliary_done(int error,const uint8_t *bytes,size_t length,uint64_t now) {
    if(authAuxiliary) {
        IOLockLock(controlLock);
        int decoded=np_auth_complete(&auth,&authCommand,error,bytes,length,now);
        if(auth.state==NP_AUTH_DONE) {
            // Authentication can change type/PDOs. Query them again without
            // inventing a new physical attachment.
            bzero(&control.meta[NP_META_PDOS],sizeof(control.meta[NP_META_PDOS]));
            bzero(&control.caps,sizeof(control.caps));
            control.meta[NP_META_TYPE].valid=control.meta[NP_META_VERIFY].valid=0;
        }
        unsigned state=auth.state,authError=auth.error,owned=auth.owned,verified=auth.verified;
        unsigned svid=auth.svid,adapter=auth.adapter_id,role=auth.data_role,roleRequests=auth.role_requests;
        IOLockUnlock(controlLock);
        // Authentication challenges/digests never use the auxiliary raw logger.
        char line[320];int size=snprintf(line,sizeof(line),
          "A2 ms=%llu id=%llu property=%x write=%u result=%d decoded=%d state=%u error=%u owned=%u verified=%u svid=%x adapter=%x role=%u role_requests=%u\n",
          (unsigned long long)now,(unsigned long long)authCommand.id,authCommand.property,
          (unsigned)authCommand.write,error,decoded,state,authError,owned,verified,svid,adapter,role,roleRequests);
        if(persistentLog&&size>0&&(size_t)size<sizeof(line))(void)np_power_log_push(line,(size_t)size);
        IOLog("NEZHA POWER AUTH: state=%u error=%u owned=%u verified=%u result=%d\n",state,authError,owned,verified,error);
        bzero(&authCommand,sizeof(authCommand));bzero(request,sizeof(request));bzero(mca.reply,sizeof(mca.reply));
        authAuxiliary=auxiliaryActive=outgoing=false;
        return;
    }
    if(!auxiliaryActive)return;
    IOLockLock(controlLock);
    int decoded=np_control_complete(&control,&auxiliaryCommand,error,bytes,length,now);
    unsigned state=control.buck.state,blocked=control.buck.blocked,backend=control.buck.backend,confirmation=control.buck.confirmation;
    IOLockUnlock(controlLock);
    uint32_t value=!error&&length?(length==1?bytes[0]:np_le32(bytes)):0;
    IOLog("NEZHA POWER AUX: control=%u write=%u id=%llu property=%x requested=%u result=%d observed=%u state=%u blocked=%u decoded=%d\n",
      (unsigned)auxiliaryCommand.control,(unsigned)auxiliaryCommand.write,(unsigned long long)auxiliaryCommand.id,
      auxiliaryCommand.property,auxiliaryCommand.value,error,value,state,blocked,decoded);
    log_aux(error,decoded,bytes,length,now,state,blocked,backend,confirmation);
    auxiliaryActive=false;outgoing=false;
}
void auxiliary_begin(uint64_t now) {
    if(io.control_mode<2||auxiliaryActive||cycle||mca.pending||!mca.up)return;
    NpPowerLogStatus log={};if(!np_power_log_status(&log))return;
    IOLockLock(controlLock);
    np_auth_source(&auth,control.epoch,control.attachment);
    if(draining||(auth.state!=NP_AUTH_IDLE&&auth.state!=NP_AUTH_DONE&&
       auth.state!=NP_AUTH_FAILED&&auth.state!=NP_AUTH_STOPPED&&(!persistentLog||log.phase!=2)))
        np_auth_stop(&auth);
    const NpSnapshot &sample=control.sample;
    uint32_t need=NP_BIT(NP_PRESENT)|NP_BIT(NP_EXTERNAL);
    uint64_t sampled=sample.gauge.end_ms<sample.charger.end_ms?sample.gauge.end_ms:sample.charger.end_ms;
    NpAuthFacts facts={control.epoch,control.attachment,now,(uint32_t)sample.external,
        control.meta[NP_META_TYPE].value,
        (uint32_t)((sample.valid&need)==need&&sample.gauge.present&&now>=sampled&&now-sampled<=3000&&
           control.meta[NP_META_TYPE].valid),0};
    const NpMeta &pdstate=control.meta[NP_META_PD_STATE],&pdos=control.meta[NP_META_PDOS];
    facts.pd_ready=control.caps.valid&&control.caps.epoch==control.epoch&&
        control.caps.entry[1].max_mv&&pdos.valid&&pdstate.valid&&pdstate.value==2&&
        now>=pdos.at_ms&&now-pdos.at_ms<=3000&&
        now>=pdstate.at_ms&&now-pdstate.at_ms<=3000;
    int authResult=0;
    if(np_auth_busy(&auth)||(persistentLog&&log.phase==2&&!draining))
        authResult=np_auth_step(&auth,&facts,&authCommand);
    now=io.now_ms(io.context); // Primary gauge authentication includes IOSleep(400).
    bool authBusy=np_auth_busy(&auth);
    if(authResult>0) {
        IOLockUnlock(controlLock);
        authAuxiliary=auxiliaryActive=true;
        int result=np_mca_begin(&mca,authCommand.property,authCommand.write?NP_MCA_WRITE:NP_MCA_READ,
            authCommand.write?authCommand.data:nullptr,authCommand.write_bytes,authCommand.reply_bytes,request);
        if(result!=NP_MCA_REQUEST){auxiliary_done(-EIO,nullptr,0,now);return;}
        queuedAt=now;outgoing=true;return;
    }
    // Keep a short VDM poll wait from starting an unrelated metadata request.
    if(authBusy){IOLockUnlock(controlLock);return;}
    control.allow_control=persistentLog&&log.phase==2&&!manualDisable&&!draining;
    // Finish the PD catalog/state preflight before the sole auth worker claims
    // verify_process. Metadata can still refresh while this gate is pending.
    if(auth.enabled&&auth.state==NP_AUTH_IDLE&&facts.online&&facts.real_type==10)
        control.allow_control=0;
    int result=np_control_next(&control,now,&auxiliaryCommand);
    IOLockUnlock(controlLock);
    if(result<=0)return;
    if(auxiliaryCommand.control==2) {
        auxiliaryActive=true;
        IOLockLock(controlLock);int allowed=np_control_register_allowed(&control,&auxiliaryCommand,now);IOLockUnlock(controlLock);
        uint8_t observed=255,attempted=0;
        result=allowed&&io.set_pump_usb_ovp?
          io.set_pump_usb_ovp(io.context,(uint8_t)(auxiliaryCommand.value>>8),
            (uint8_t)auxiliaryCommand.value,&observed,&attempted):EACCES;
        if(attempted){IOLockLock(controlLock);control.buck.wire_accepted=1;IOLockUnlock(controlLock);}
        if(!result)pumpSample.control[2]=(pumpSample.control[2]&0xf0)|(observed&15);
        auxiliary_done(result>0? -result:result,&observed,1,io.now_ms(io.context));return;
    }
    uint8_t value[4];np_put32(value,auxiliaryCommand.value);
    auxiliaryActive=true;
    result=np_mca_begin(&mca,auxiliaryCommand.property,auxiliaryCommand.write?NP_MCA_WRITE:NP_MCA_READ,
      auxiliaryCommand.write?value:nullptr,auxiliaryCommand.write?4:0,auxiliaryCommand.reply_bytes,request);
    if(result!=NP_MCA_REQUEST){auxiliary_done(-1,nullptr,0,now);return;}
    queuedAt=now;outgoing=true;
}

void commit(uint64_t now) {
    stagingCharger.end_ms = now;
    if (refreshRequested) { stagingCharger.valid = 0; stagingCharger.fault |= NP_FAULT_STALE; }
    IOLockLock(snapshotLock);
    gauge = stagingGauge; charger = stagingCharger;
    IOLockUnlock(snapshotLock);
    control_publish(now);
    IOLog("NEZHA POWER SAMPLE: gen=%llu gauge=%x soc=%d mv=%d ma=%d centiC=%d remaining=%d full=%d design=%d cycle=%d charger=%x faults=%x link=%u replies_timeout=%u ignored=%u\n",
          (unsigned long long)stagingGauge.generation,stagingGauge.valid,stagingGauge.soc,
          stagingGauge.voltage_mv,stagingGauge.current_ma,stagingGauge.temperature_centi_c,
          stagingGauge.remaining_mah,stagingGauge.full_mah,stagingGauge.design_mah,stagingGauge.cycles,
          stagingCharger.valid,stagingCharger.fault,(unsigned)mca.up,mca.timeouts,mca.ignored);
    cycle = outgoing = false; refreshAt = now + (refreshRequested ? 250 : 2000);
    refreshRequested = false;
}
void next(uint64_t now) {
    while (cursor < 4 && !props[cursor]) ++cursor; // unverified fields stay absent
    if (cursor == 4) { commit(now); return; }
    if (np_mca_begin(&mca, props[cursor], NP_MCA_READ, nullptr, 0, 4, request) != NP_MCA_REQUEST) {
        stagingCharger.fault |= NP_FAULT_TRANSPORT;
        commit(now); return;
    }
    queuedAt = now; outgoing = true;
}
void begin(uint64_t now) {
    if (generation == UINT64_MAX) { __atomic_store_n(&stopRequested, 1u, __ATOMIC_RELEASE); return; }
    const uint64_t gen = ++generation;
    uint8_t bytes[NP_GAUGE_WORDS][2] = {};
    uint32_t valid = 0;
    const int gaugeError = io.read_gauge(io.context, bytes, &valid);
    uint64_t end = io.now_ms(io.context);
    np_decode_gauge(&stagingGauge, bytes, valid, now, end, gen);
    if(io.control_mode>=2&&io.read_pump) {
        memset(&pumpSample,0,sizeof(pumpSample));pumpError=io.read_pump(io.context,&pumpSample);pumpAt=io.now_ms(io.context);
        IOLog("NEZHA POWER CP SAMPLE: error=%d chip=%x enabled=%u switching=%u/%u mode=%u valid=%x vbus=%d ibus=%d vbat=%d die=%d\n",
          pumpError,pumpSample.chip,pumpSample.enable,pumpSample.switching,pumpSample.switching_valid,pumpSample.mode,
          pumpSample.valid,pumpSample.bus_mv,pumpSample.bus_ma,pumpSample.battery_mv,pumpSample.die_centi_c);
        if(!pumpError&&pumpSample.controls_valid&&!pumpRegistersReported) {
            char line[256];int at=snprintf(line,sizeof(line),"CPREG initial_enable=%u sampled_before_update=",pumpSample.initial_enable);
            for(unsigned i=0;i<sizeof(pumpSample.control)&&at>0&&(size_t)at+3<sizeof(line);++i)
                at+=snprintf(line+at,sizeof(line)-(size_t)at,"%02x",pumpSample.control[i]);
            if(at>0&&(size_t)at+2<sizeof(line)) {line[at++]='\n';line[at]=0;IOLog("NEZHA POWER %s",line);
                if(!persistentLog||!np_power_log_push(line,(size_t)at))pumpRegistersReported=true;}
        }
    }
    // Retain the actual gauge words and successful-transfer mask alongside
    // decoded values, so a device run can verify units and invalid reads.
    const uint16_t *raw = stagingGauge.raw;
    IOLog("NEZHA POWER RAW: gen=%llu begin=%llu end=%llu mask=%x error=%d words=%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x,%04x\n",
          (unsigned long long)gen,(unsigned long long)now,(unsigned long long)end,valid,gaugeError,
          (unsigned)raw[0],(unsigned)raw[1],(unsigned)raw[2],(unsigned)raw[3],(unsigned)raw[4],
          (unsigned)raw[5],(unsigned)raw[6],(unsigned)raw[7],(unsigned)raw[8],(unsigned)raw[9]);
    bzero(&stagingCharger, sizeof(stagingCharger));
    stagingCharger.begin_ms = now; stagingCharger.epoch = mca.epoch;
    stagingCharger.generation = gen;
    if (gaugeError) stagingCharger.fault |= NP_FAULT_TRANSPORT;
    cycle = true; cursor = 0;
    if (!mca.up || !io.schema_verified) { commit(end); return; }
    next(end);
}
void step() {
    uint64_t now = io.now_ms(io.context);
    log_usb(now);
    // Time out before dispatching inbound messages; a delayed work-loop tick
    // cannot turn a response after its deadline into a successful command.
    if (np_mca_timeout(&mca, now)) {
        if(auxiliaryActive)auxiliary_done(-ETIMEDOUT,nullptr,0,now);
        else if(cycle){stagingCharger.fault |= NP_FAULT_TRANSPORT; ++cursor; next(now);}
    }
    if (io.poll_link(io.context)) np_power27_link(0, transportEpoch);
    now = io.now_ms(io.context); // callbacks may have queued the NEXT request
    if (outgoing) {
        if (now < queuedAt || now - queuedAt >= 250) {
            np_mca_cancel(&mca); outgoing = false;
            if(auxiliaryActive)auxiliary_done(-ETIMEDOUT,nullptr,0,now);
            else{stagingCharger.fault |= NP_FAULT_TRANSPORT; ++cursor; next(now);}
        } else {
            const int r = io.send_mca(io.context, request, sizeof(request));
            if (!r) {
                if(authAuxiliary) {
                    IOLockLock(controlLock);int accepted=np_auth_sent(&auth,io.now_ms(io.context));IOLockUnlock(controlLock);
                    if(accepted){np_mca_cancel(&mca);auxiliary_done(-EIO,nullptr,0,now);return;}
                } else if(auxiliaryActive&&auxiliaryCommand.control&&auxiliaryCommand.write) {
                    IOLockLock(controlLock);control.buck.wire_accepted=1;IOLockUnlock(controlLock);
                }
                outgoing = false;
                if (np_mca_sent(&mca, io.now_ms(io.context), io.mca_reply_timeout_ms)) {
                    if(auxiliaryActive)auxiliary_done(-EIO,nullptr,0,now);
                    else{stagingCharger.fault |= NP_FAULT_TRANSPORT; ++cursor; next(io.now_ms(io.context));}
                }
            }
            else if (r != -1) {
                np_mca_cancel(&mca); outgoing = false;
                if(auxiliaryActive)auxiliary_done(r,nullptr,0,now);
                else{stagingCharger.fault |= NP_FAULT_TRANSPORT; ++cursor; next(now);}
            }
        }
    }
    if (!cycle&&!auxiliaryActive&&!mca.pending) {
        if((!draining||(io.control_mode==4&&(control.buck.armed||control.buck.owned))||np_auth_busy(&auth))&&now>=refreshAt)begin(io.now_ms(io.context));
        else auxiliary_begin(io.now_ms(io.context));
    }
    log_primary_auth(io.now_ms(io.context));
}
int drain_control(void) {
    if(io.control_mode<2)return 0;
    draining=true;IOLockLock(controlLock);np_control_stop(&control);np_auth_stop(&auth);IOLockUnlock(controlLock);
    uint64_t until=io.now_ms(io.context)+(io.control_mode==4?8000:2500);
    while(io.now_ms(io.context)<until) {
        step();
        IOLockLock(controlLock);bool done=!auxiliaryActive&&!cycle&&
          !np_auth_busy(&auth)&&((!control.buck.armed&&!control.buck.owned)||control.buck.state==NP_BUCK_UNCERTAIN);IOLockUnlock(controlLock);
        if(done)break;
        IOSleep(20);
    }
    IOLockLock(controlLock);
    if(control.buck.pending||control.buck.unverified_effect||control.buck.owned) {
        control.buck.state=NP_BUCK_UNCERTAIN;control.buck.blocked=NP_BUCK_EFFECT_UNKNOWN;
    }
    // Every charger backend must finish its own effects before PM completion.
    // An FCC/ICL reduction with a missing completion is not a released owner.
    const bool incomplete=control.buck.pending||control.buck.unverified_effect||
      control.buck.owned||control.buck.state==NP_BUCK_UNCERTAIN||np_auth_busy(&auth);
    IOLockUnlock(controlLock);
    control_publish(io.now_ms(io.context));
    IOLog("NEZHA POWER CONTROL STOP: state=%u owned=%u pending_effect=%u; no replay\n",
      control.buck.state,control.buck.owned,control.buck.unverified_effect);
    if(persistentLog) {
        char line[320];int bytes=snprintf(line,sizeof(line),"S1 ms=%llu shutdown=%u state=%u owned=%u pending_effect=%u backend=%u confirmation=%u vbus_confirmed=%u ovp_restored=%u auth_state=%u auth_owned=%u auth_error=%u\n",
          (unsigned long long)io.now_ms(io.context),(unsigned)__atomic_load_n(&system_inshutdown,__ATOMIC_ACQUIRE),
          control.buck.state,control.buck.owned,control.buck.unverified_effect,control.buck.backend,control.buck.confirmation,
          control.buck.confirmed_vbus_mv,control.buck.protection_restored,auth.state,auth.owned,auth.error);
        if(bytes>0&&(size_t)bytes<sizeof(line))(void)np_power_log_push(line,(size_t)bytes);
    }
    return incomplete?EIO:0;
}
void invalidate(uint32_t fault) {
    np_mca_link(&mca, 0); cycle = outgoing = false;
    IOLockLock(snapshotLock);
    charger.valid = gauge.valid = 0; charger.fault = fault;
    IOLockUnlock(snapshotLock);
    // The CPU diagnostic copy must withdraw the same stale sample as the OS
    // power source while its worker is suspended or has stopped.
    IOLockLock(controlLock);
    control.sample.valid = control.sample.gauge.valid = control.sample.charger.valid = 0;
    control.sample.faults |= fault;
    pumpSample.valid = pumpSample.controls_valid = pumpSample.switching_valid = 0;
    control.pump.valid = control.pump.controls_valid = control.pump.switching_valid = 0;
    IOLockUnlock(controlLock);
}
void worker(void *, wait_result_t) {
    __atomic_store_n(&lifecycle, 1, __ATOMIC_RELEASE);
    source = np_battery_service_create();
    if(io.control_mode>=2) {
        (void)PE_parse_boot_argn("nezha-power-log",&persistentLog,sizeof(persistentLog));
        if(persistentLog&&np_power_log_start())persistentLog=0;
        np_power_diagnostic_start();
    }
    bool suspended = false;
    bool controlDrainAttempted=false;int drainError=0;
    int error = source ? 0 : -1;
    /* Normal Settings shutdown uses the same XNU flag as USB shutdown.
     * Finish controller/log work before the normal VFS unmount sequence. */
    while (!error && !__atomic_load_n(&stopRequested, __ATOMIC_ACQUIRE)&&
           !__atomic_load_n(&system_inshutdown,__ATOMIC_ACQUIRE)) {
        const bool requested = __atomic_load_n(&suspendRequested, __ATOMIC_ACQUIRE) != 0;
        if (requested && !suspended) {
            controlDrainAttempted=true;drainError=drain_control();error=drainError;
            invalidate(NP_FAULT_STALE);
            if(!error)error = io.quiesce(io.context);
            if (!error) { suspended = true; __atomic_store_n(&lifecycle, 3, __ATOMIC_RELEASE); }
        } else if (!requested && suspended) {
            error = io.resume(io.context);
            if (!error) {
                suspended = false; refreshAt = 0;
                controlDrainAttempted=false;drainError=0;
                draining=false;IOLockLock(controlLock);control.stopping=0;
                if(np_auth_resume(&auth))error=EIO;
                IOLockUnlock(controlLock);
                __atomic_store_n(&lifecycle, 1, __ATOMIC_RELEASE);
            }
        }
        if (!error && !suspended) step();
        IOSleep(50);
    }
    if(!suspended&&!controlDrainAttempted)drainError=drain_control();
    if(drainError)error=drainError;
    if(persistentLog) {
        np_power_log_stop();uint64_t until=io.now_ms(io.context)+2500;NpPowerLogStatus log={};
        do{(void)np_power_log_status(&log);if(log.phase==3||log.phase==4)break;IOSleep(20);}while(io.now_ms(io.context)<until);
        if(log.phase!=3&&log.phase!=4)error=ETIMEDOUT;
    }
    invalidate(NP_FAULT_TRANSPORT);
    // Do not repeat a failed quiesce or free quarantined buffers. The PM caller
    // must distinguish lifecycle -1 from a completed transition.
    if (!error && !suspended) error = io.quiesce(io.context);
    __atomic_store_n(&lifecycle, error ? -1 : 5, __ATOMIC_RELEASE);
    IOLog("NEZHA POWER: worker exit, error=%d; mappings retained\n", error);
    __atomic_store_n(&workerExited, 1u, __ATOMIC_RELEASE);
    thread_terminate(current_thread());
}
}
extern "C" int np_power27_bind(const NpPowerOps *ops) {
    if (!ops || !ops->now_ms || !ops->poll_link || !ops->send_mca || !ops->read_gauge || !ops->quiesce || !ops->resume ||
        __atomic_load_n(&started, __ATOMIC_ACQUIRE) || bound) return -1;
    if (ops->schema_verified && (ops->mca_reply_timeout_ms < 250 || ops->mca_reply_timeout_ms > 1000)) return -1;
    if(ops->control_mode==4&&!ops->set_pump_usb_ovp)return -1;
    if (ops->schema_verified && ops->property_status &&
        (!ops->status_valid_mask || (ops->status_charging_mask & ~ops->status_valid_mask))) return -1;
    snapshotLock = IOLockAlloc();
    controlLock=IOLockAlloc();if (!snapshotLock||!controlLock) return -1;
    io = *ops; props[0] = io.property_online; props[1] = io.property_status;
    props[2] = io.property_input_uv; props[3] = io.property_input_ua;
    np_mca_init(&mca);np_control_init(&control,io.control_mode);
    np_auth_init(&auth,io.auth_material,&io.auth,io.auth_enabled);bound = 1; return 0;
}
extern "C" void nezha_power27_start(void) {
    int mode = 0;
    (void)PE_parse_boot_argn("nezha-power", &mode, sizeof(mode));
    if (!mode) return;
    if ((mode != 1&&mode!=2&&mode!=3&&mode!=4) || !bound) { IOLog("NEZHA POWER: binding unavailable or unsupported mode\n"); return; }
    if (__atomic_exchange_n(&started, 1u, __ATOMIC_ACQ_REL)) return;
    thread_t t;
    if (kernel_thread_start(worker, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
    else {
        __atomic_store_n(&lifecycle, -1, __ATOMIC_RELEASE);
        __atomic_store_n(&workerExited, 1u, __ATOMIC_RELEASE);
    }
}
extern "C" void nezha_power27_stop(void) {
    __atomic_store_n(&stopRequested, 1u, __ATOMIC_RELEASE);
}
extern "C" void nezha_power27_suspend(void) {
    __atomic_store_n(&suspendRequested, 1u, __ATOMIC_RELEASE);
}
extern "C" void nezha_power27_resume(void) {
    __atomic_store_n(&suspendRequested, 0u, __ATOMIC_RELEASE);
}
extern "C" int nezha_power27_lifecycle(void) {
    const int s = __atomic_load_n(&lifecycle, __ATOMIC_ACQUIRE);
    if ((s == 1 || s == 3) && __atomic_load_n(&stopRequested, __ATOMIC_ACQUIRE)) return 4;
    if (s == 1 && __atomic_load_n(&suspendRequested, __ATOMIC_ACQUIRE)) return 2;
    if (s == 3 && !__atomic_load_n(&suspendRequested, __ATOMIC_ACQUIRE)) return 6;
    return s;
}
extern "C" int np_power_lifecycle_request(unsigned state) {
    if (state > 1 || !__atomic_load_n(&started, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&workerExited, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&stopRequested, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&system_inshutdown, __ATOMIC_ACQUIRE)) return -1;
    const int current = nezha_power27_lifecycle();
    if (current != 1 && current != 3)
        return current == 2 || current == 6 ? 0 : -1;
    // Reuse the same worker and ownership/drain checks as IOPMPowerSource.
    // No firmware reset, another device's PM state, or inline hardware I/O.
    if (state) nezha_power27_resume(); else nezha_power27_suspend();
    return 1;
}
extern "C" int np_snapshot_read(NpSnapshot *out) {
    if (!out || !bound || !snapshotLock) return 0;
    IOLockLock(snapshotLock);
    np_snapshot(out, &gauge, &charger, io.now_ms(io.context), gauge.generation);
    IOLockUnlock(snapshotLock); return 1;
}
extern "C" void np_power27_link(int up, uint64_t epoch) {
    if (!epoch || epoch < transportEpoch || (mca.up == !!up && epoch == transportEpoch)) return;
    transportEpoch = epoch;
    IOLog("NEZHA POWER SOCCP: up=%d epoch=%llu\n",up,(unsigned long long)epoch);
    if(auxiliaryActive)auxiliary_done(-ECONNRESET,nullptr,0,io.now_ms(io.context));
    IOLockLock(controlLock);np_control_link(&control,epoch,up);
    np_auth_source(&auth,control.epoch,control.attachment);IOLockUnlock(controlLock);
    np_mca_link(&mca, up); cycle = outgoing = false;
    IOLockLock(snapshotLock);
    charger.valid = 0; charger.fault = up ? NP_FAULT_STALE : NP_FAULT_REMOTE_RESET;
    IOLockUnlock(snapshotLock); refreshAt = 0;
}
extern "C" void np_power27_receive(uint64_t epoch, const uint8_t *p, uint32_t n) {
    if (!epoch || epoch != transportEpoch || __atomic_load_n(&workerExited,__ATOMIC_ACQUIRE)) return;
    uint32_t note = 0, value = 0;
    if (np_mca_timeout(&mca, io.now_ms(io.context))) {
        if(auxiliaryActive)auxiliary_done(-ETIMEDOUT,nullptr,0,io.now_ms(io.context));
        else if(cycle){stagingCharger.fault |= NP_FAULT_TRANSPORT; ++cursor; next(io.now_ms(io.context));}
    }
    const int r = np_mca_receive(&mca, mca.epoch, io.now_ms(io.context), p, n, &note, &value);
    if(r==NP_MCA_NOTE&&io.control_mode>=2&&note==0) {
        IOLockLock(controlLock);np_control_source_event(&control);
        np_auth_source(&auth,control.epoch,control.attachment);IOLockUnlock(controlLock);
        if(cycle)refreshRequested=true;else refreshAt=0;
    }
    if(auxiliaryActive&&(r==NP_MCA_REPLY||r==NP_MCA_ERROR)) {
        auxiliary_done(r==NP_MCA_REPLY?0:-EIO,mca.reply,mca.reply_bytes,io.now_ms(io.context));return;
    }
    if (r == NP_MCA_NOTE && io.schema_verified && note < 32 && (io.notification_mask & (1u << note))) {
        // Trigger a fresh cohort, without treating notification0's firmware-
        // dependent payload as a universally valid USB boolean.
        // Coalesce events into a new cohort. A notification during this
        // cohort invalidates its charger part; do not starve the request queue
        // by cancelling it repeatedly on a notification storm.
        if (cycle) refreshRequested = true;
        else refreshAt = 0;
        IOLockLock(snapshotLock); charger.valid = 0; IOLockUnlock(snapshotLock);
        return;
    }
    if (!cycle || (r != NP_MCA_REPLY && r != NP_MCA_ERROR)) return;
    if (r == NP_MCA_REPLY) {
        const uint32_t raw = np_le32(mca.reply);
        IOLog("NEZHA POWER MCA: epoch=%llu seq=%u property=%x raw=%x at=%llu\n",
              (unsigned long long)epoch,mca.sequence,mca.property,raw,
              (unsigned long long)io.now_ms(io.context));
        switch (cursor) {
        case 0:
            if (raw <= 1) { stagingCharger.external = (int32_t)raw; stagingCharger.valid |= NP_BIT(NP_EXTERNAL); }
            break;
        case 1:
            if (raw < 32 && (io.status_valid_mask & (1u << raw))) {
                stagingCharger.charging = !!(io.status_charging_mask & (1u << raw));
                stagingCharger.done = raw == io.status_done;
                stagingCharger.valid |= NP_BIT(NP_CHARGING) | NP_BIT(NP_CHARGE_DONE);
            }
            break;
        case 2:
            if (raw <= 25000000) { stagingCharger.usb_uv = (int32_t)raw; stagingCharger.valid |= NP_BIT(NP_USB_VOLTAGE); }
            break;
        case 3:
            if (raw <= 10000000) {
                stagingCharger.usb_ua = (int32_t)raw;
                stagingCharger.valid |= NP_BIT(NP_USB_CURRENT);
                /* The qualified OS4 property is a MAIN_BUCK_CHARGER getter,
                 * not aggregate adapter current across the parallel paths. */
                stagingCharger.input_current_scope = io.property_input_ua == 0x20004 ?
                    NP_INPUT_CURRENT_MAIN_BUCK : NP_INPUT_CURRENT_UNKNOWN;
            }
            break;
        }
    } else stagingCharger.fault |= NP_FAULT_CHARGER;
    ++cursor; next(io.now_ms(io.context));
}

extern "C" int np_power27_write_allowed(const uint8_t *p,uint32_t bytes) {
    if(authAuxiliary) {
        if(io.control_mode<2||!p||bytes!=NP_MCA_REQUEST||!auxiliaryActive||!outgoing||!mca.pending||
           !authCommand.write||np_le32(p)!=NP_MCA_OWNER||np_le32(p+4)!=1||
           np_le32(p+8)!=NP_MCA_WRITE||np_le32(p+12)!=authCommand.property||np_le32(p+16)!=mca.sequence)
            return 0;
        for(unsigned i=20+authCommand.write_bytes;i<bytes;++i)if(p[i])return 0;
        bool clear=authCommand.write_bytes==4&&(authCommand.property==0x21002||authCommand.property==0x2100b)&&
            np_le32(p+20)==0;
        if(!clear) {
            NpPowerLogStatus log={};
            if(!persistentLog)return 0;
            if(!np_power_log_status(&log))return -1; /* no wire submission while the logger is busy */
            if(log.phase!=2)return 0;
        }
        IOLockLock(controlLock);
        int allowed=control.epoch==authCommand.epoch&&control.attachment==authCommand.attachment&&
            np_auth_write_allowed(&auth,&authCommand,io.now_ms(io.context),p+20,authCommand.write_bytes);
        IOLockUnlock(controlLock);return allowed;
    }
    if(io.control_mode<2||!p||bytes!=NP_MCA_REQUEST||!auxiliaryActive||!outgoing||!mca.pending||
       !auxiliaryCommand.write||np_le32(p)!=NP_MCA_OWNER||np_le32(p+4)!=1||np_le32(p+8)!=NP_MCA_WRITE||
       np_le32(p+12)!=auxiliaryCommand.property||np_le32(p+16)!=mca.sequence||
       np_le32(p+20)!=auxiliaryCommand.value)return 0;
    for(unsigned i=24;i<bytes;++i)if(p[i])return 0;
    NpPowerLogStatus log={};
    if(control.buck.state==NP_BUCK_SET_ICL||control.buck.state==NP_BUCK_SET_FCC||
       control.buck.state==NP_BUCK_FCC_INITIALIZE||control.buck.state==NP_BUCK_PD_REQUEST_9V) {
        if(!np_power_log_status(&log))return -1; /* transient busy: no wire write or cancellation */
        if(log.phase!=2)return 0;
    }
    IOLockLock(controlLock);int allowed=np_control_write_allowed(&control,&auxiliaryCommand,io.now_ms(io.context));
    IOLockUnlock(controlLock);return allowed;
}
extern "C" int np_power_control_copy(NpPowerControl *out) {
    if(!out||!controlLock||!IOLockTryLock(controlLock))return 0;
    *out=control;IOLockUnlock(controlLock);return 1;
}
extern "C" int np_power_auth_copy(NpAuthStatus *out) {
    if(!out||!controlLock||!IOLockTryLock(controlLock))return 0;
    *out=NpAuthStatus{auth.epoch,auth.attachment,auth.verified_ms,
        auth.enabled,auth.state,auth.error,auth.owned,auth.attempts,auth.svid,auth.adapter_id,
        auth.battery_ok,auth.verified,auth.battery_checks,auth.battery_error,auth.discovery_until};
    IOLockUnlock(controlLock);return 1;
}
extern "C" int np_power_history_copy(uint64_t cursor,unsigned limit,NpPowerHistoryRecord *rows,unsigned *count,uint64_t *next,uint64_t *oldest) {
    if(!rows||!count||!next||!oldest||!limit||limit>8||!controlLock)return -1;
    if(!IOLockTryLock(controlLock))return 0;
    *count=0;*oldest=historyNext>128?historyNext-128:1;
    if(!cursor)cursor=*oldest;
    *next=cursor;
    if(cursor<*oldest||cursor>historyNext){IOLockUnlock(controlLock);return -1;}
    while(cursor<historyNext&&*count<limit) {
        const NpPowerHistoryRecord &row=history[(cursor-1)%128];
        if(row.id!=cursor){IOLockUnlock(controlLock);return -1;}
        rows[(*count)++]=row;++cursor;
    }
    *next=cursor;IOLockUnlock(controlLock);return 1;
}
extern "C" int np_power_control_request(unsigned mode) {
    if(io.control_mode<2||mode>1||!controlLock)return -1;
    NpPowerLogStatus log={};
    if(mode){if(!persistentLog)return -1;if(!np_power_log_status(&log))return 0;if(log.phase!=2)return -1;}
    if(!IOLockTryLock(controlLock))return 0;
    if(mode&&(control.buck.pending||control.buck.owned||control.buck.state==NP_BUCK_UNCERTAIN)){
        IOLockUnlock(controlLock);return 0;
    }
    manualDisable=mode?0:1;
    if(mode){control.last_armed_attachment=0;control.stopping=0;}
    else{control.allow_control=0;np_buck_stop(&control.buck);}
    IOLockUnlock(controlLock);return 1;
}
