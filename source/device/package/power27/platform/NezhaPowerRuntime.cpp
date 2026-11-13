// SPDX-License-Identifier: BSD-2-Clause-Patent
// A single worker owns the request state and all hardware callbacks. Consumers
// copy one completed generation under a mutex; they never read half-written data.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <mach/thread_act.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <limits.h>
#include "NezhaPowerRuntime.h"
#include "NezhaPowerLog.h"
#include "NezhaPowerThermal.h"
#include "QcomXnuUsbDebug.h"

extern "C" IOService *np_battery_service_create(void);
extern "C" uint32_t system_inshutdown;
namespace {
NpPowerOps io;
NpGaugeIdentity gaugeIdentity = {};
NpMca mca;
IOLock *snapshotLock;
NpGauge stagingGauge, gauge;
NpCharger stagingCharger, charger;
IOService *source;
uint32_t bound, started, stopRequested, workerExited;
constexpr unsigned WORKER_TICK_MS = 50;
constexpr unsigned LOG_STOP_BUDGET_MS = 2500;
unsigned control_drain_budget_ms() {
    return io.control_mode==5?12000:(io.control_mode==4||io.control_mode==6)?8000:2500;
}
uint32_t suspendRequested;
int lifecycle;
uint64_t generation, refreshAt, queuedAt, transportEpoch;
unsigned cursor;
bool cycle, outgoing, refreshRequested;
uint8_t request[NP_MCA_REQUEST];
uint32_t props[5];
bool packRequested;
NpPackThermal stagingPack, packThermal;
NpTsensThermal stagingTsens, tsensThermal;
uint64_t tsensAt;
NpConnectorThermal stagingConnector, connectorThermal;
uint64_t connectorAt;
unsigned connectorChannel;
bool connectorActive,connectorPending;
uint64_t connectorEpoch,connectorAttachment;
NpBoardThermal stagingBoard,boardThermal;
unsigned boardChannel;
bool boardActive,boardPending;
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
NpPowerHistoryRecord lastPersistentSample = {};
bool persistentSampleReported = false;
uint64_t detailedAttachment,detailedUntil;
NpJeitaVote lastJeitaVote = {};
bool jeitaReported;
NpBoardThermal lastLoggedBoard={};
NpConnectorThermal lastLoggedConnector={};
NpConnectorPolicyStatus lastLoggedConnectorPolicy={};
NpPackThermal lastLoggedPack={};
uint64_t lastBoardLogAt,lastConnectorLogAt,lastPackLogAt;
bool boardLogReported,connectorLogReported,packLogReported;
void connector_policy_locked(NpConnectorPolicyStatus &out,uint64_t now) {
    out=NpConnectorPolicyStatus{};
    out.epoch=control.epoch;out.attachment=control.attachment;out.now_ms=now;
    out.source_online_at_ms=control.source_online_at_ms;
    out.enabled=control.connector_policy_enabled;
    out.cid=control.meta[NP_META_CID];out.otg_plugin=control.meta[NP_META_OTG_PLUGIN];
    np_control_connector_signals(&control,now,&out.signals);
    np_control_connector_vote(&control,now,&out.vote);
    out.jeita=control.jeita.vote;
    NpBuckFacts facts={};np_control_facts(&control,now,&facts);
    out.effective_policy_valid=facts.policy_valid;
    out.effective_policy_enabled=facts.policy_enabled;
}
void connector_number(char *out,size_t cap,bool valid,int64_t value) {
    if(valid)snprintf(out,cap,"%lld",(long long)value);
    else snprintf(out,cap,"none");
}
static bool usable_log(const NpPowerLogStatus &log) {
    // A deliberate completed diagnostic budget is not a charging fault. The
    // production low-voltage policy retains all source/profile/IO checks;
    // actual VFS errors and failed startup still stop its control path.
    return log.phase==2||(io.control_mode==5&&log.phase==4&&log.capped&&!log.error);
}

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
    np_control_profile(&control,&gaugeIdentity,(int)io.country_cn);
    np_control_sample(&control,&snapshot,&pumpSample,pumpError,pumpAt);
    const NpJeitaVote jeitaVote=control.jeita.vote;
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
    const bool chargingSource=row.real_type==3||row.real_type==10||row.real_type==11||row.real_type==12;
    const bool sampleChange=!persistentSampleReported||row.attachment!=lastPersistentSample.attachment||
        row.valid!=lastPersistentSample.valid||row.pump_valid!=lastPersistentSample.pump_valid||
        row.pump_error!=lastPersistentSample.pump_error||row.real_type!=lastPersistentSample.real_type||
        row.state!=lastPersistentSample.state||row.blocked!=lastPersistentSample.blocked||
        row.owned!=lastPersistentSample.owned;
    // Keep charger samples, first samples and state/fault transitions. Stable
    // PC waiting needs one persistent snapshot per10s; RAM/UI/console still
    // receive every original sample, and cannot fill the charger log first.
    if((io.control_mode==5||io.control_mode==6)&&detailedAttachment!=row.attachment){
        detailedAttachment=row.attachment;detailedUntil=now+200000;
    }
    const bool detailed=(io.control_mode!=5&&io.control_mode!=6)||now<=detailedUntil||
        (io.control_mode==6&&(row.owned||control.buck.pending));
    const uint64_t samplePeriod=io.control_mode==5?60000:10000;
    if(persistentLog&&((chargingSource&&detailed)||sampleChange||now<lastPersistentSample.at_ms||
        now-lastPersistentSample.at_ms>=samplePeriod)) {
        char line[1024];int bytes=snprintf(line,sizeof(line),
          "P1 id=%llu ms=%llu gen=%llu attach=%llu soc=%d mv=%d ma=%d temp=%d rm=%d ext=%d charging=%d valid=%x vbus_uv=%d buck_ua=%d cp_valid=%x cp_error=%u cp_mode=%u cp_on=%u cp_ibus=%u type=%u icl=%u fcc=%u meta=%x state=%u blocked=%u owned=%u base_icl=%u base_fcc=%u target_icl=%u target_fcc=%u ctl_errors=%u backend=%u confirmation=%u vbus_confirmed=%u ovp_base=%u ovp_now=%u ovp_restored=%u\n",
          (unsigned long long)row.id,(unsigned long long)row.at_ms,(unsigned long long)row.generation,(unsigned long long)row.attachment,
          row.soc,row.battery_mv,row.battery_ma,row.temperature,row.remaining,row.external,row.charging,row.valid,row.vbus_uv,row.buck_ua,
          row.pump_valid,row.pump_error,row.pump_mode,row.pump_enable,row.pump_ibus_ma,row.real_type,row.icl_ma,row.fcc_ma,row.meta_valid,row.state,row.blocked,row.owned,
          row.baseline_icl,row.baseline_fcc,row.target_icl,row.target_fcc,row.control_errors,
          row.backend,row.confirmation,row.confirmed_vbus_mv,row.usb_ovp_baseline,row.usb_ovp_raw,row.protection_restored);
        if(bytes>0&&(size_t)bytes<sizeof(line)&&!np_power_log_push(line,(size_t)bytes)) {
            lastPersistentSample=row;persistentSampleReported=true;
        }
    }
    if(persistentLog&&(io.control_mode==5||io.control_mode==6)&&(!jeitaReported||
       jeitaVote.row!=lastJeitaVote.row||jeitaVote.valid!=lastJeitaVote.valid||
       jeitaVote.full_valid!=lastJeitaVote.full_valid||
       jeitaVote.fcc_ma!=lastJeitaVote.fcc_ma||jeitaVote.fv_mv!=lastJeitaVote.fv_mv||
       jeitaVote.enabled!=lastJeitaVote.enabled||jeitaVote.full!=lastJeitaVote.full||
       jeitaVote.recharging!=lastJeitaVote.recharging)){
        char line[320];int n=snprintf(line,sizeof(line),
          "J1 ms=%llu valid=%u full_valid=%u temp_c=%d row=%d raw_soc=%d fcc_ceiling=%d fv=%d iterm_nominal=%d iterm_firmware_managed=1 iterm_readback_supported=0 enabled=%u full=%u recharging=%u\n",
          (unsigned long long)now,jeitaVote.valid,jeitaVote.full_valid,jeitaVote.temperature_c,
          jeitaVote.row,jeitaVote.raw_soc,jeitaVote.fcc_ma,jeitaVote.fv_mv,jeitaVote.iterm_ma+30,
          jeitaVote.enabled,jeitaVote.full,jeitaVote.recharging);
        if(n>0&&(size_t)n<sizeof(line)&&!np_power_log_push(line,(size_t)n)){
            lastJeitaVote=jeitaVote;jeitaReported=true;
        }
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
        const unsigned scalarProperty=authCommand.property==0x21002||authCommand.property==0x2100b||
            authCommand.property==0x2100d||authCommand.property==0x21012||
            authCommand.property==0x2100f||authCommand.property==0x21015;
        const unsigned observedValid=!authCommand.write&&scalarProperty&&!error&&bytes&&length>=4;
        const uint32_t observed=observedValid?np_le32(bytes):0;
        char line[384];int size=snprintf(line,sizeof(line),
          "A2 ms=%llu id=%llu property=%x write=%u result=%d decoded=%d state=%u error=%u owned=%u verified=%u svid=%x adapter=%x role=%u role_requests=%u observed=%u observed_valid=%u\n",
          (unsigned long long)now,(unsigned long long)authCommand.id,authCommand.property,
          (unsigned)authCommand.write,error,decoded,state,authError,owned,verified,svid,adapter,role,roleRequests,observed,observedValid);
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
       auth.state!=NP_AUTH_FAILED&&auth.state!=NP_AUTH_STOPPED&&(!persistentLog||!usable_log(log))))
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
    if(np_auth_busy(&auth)||(persistentLog&&usable_log(log)&&!draining))
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
    control.allow_control=persistentLog&&usable_log(log)&&!manualDisable&&!draining&&!control.adc_error&&
        !(auth.owned&&auth.state!=NP_AUTH_DONE);
    if((io.control_mode==5||io.control_mode==6)&&!auth.enabled)control.allow_control=0;
    // Finish the PD catalog/state preflight before the sole auth worker claims
    // verify_process. Metadata can still refresh while this gate is pending.
    if(auth.enabled&&(!auth.battery_ok||
       ((facts.real_type==10||facts.real_type==11||io.control_mode==6)&&!auth.verified)))
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
    const unsigned writeBytes=auxiliaryCommand.property==NP_BUCK_ENABLE?1:4;
    auxiliaryActive=true;
    result=np_mca_begin(&mca,auxiliaryCommand.property,auxiliaryCommand.write?NP_MCA_WRITE:NP_MCA_READ,
      auxiliaryCommand.write?value:nullptr,auxiliaryCommand.write?writeBytes:0,auxiliaryCommand.reply_bytes,request);
    if(result!=NP_MCA_REQUEST){auxiliary_done(-1,nullptr,0,now);return;}
    queuedAt=now;outgoing=true;
}

void commit(uint64_t now) {
    stagingCharger.end_ms = now;
    if (refreshRequested) { stagingCharger.valid = 0; stagingCharger.fault |= NP_FAULT_STALE; }
    IOLockLock(snapshotLock);
    gauge = stagingGauge; charger = stagingCharger;
    if(packRequested)packThermal=stagingPack;
    IOLockUnlock(snapshotLock);
    control_publish(now);
    if(packRequested) {
        char line[320];int size=snprintf(line,sizeof(line),
          "T1 ms=%llu gen=%llu epoch=%llu valid_raw=%u error=%d pack_tbat_raw=%d pack_units_c=1 gauge_raw_temp=%u gauge_centi_c=%d cp_die_centi_c=%d cp_valid=%x cp_ms=%llu\n",
          (unsigned long long)stagingPack.sampled_ms,(unsigned long long)stagingPack.generation,
          (unsigned long long)stagingPack.epoch,stagingPack.valid_raw,stagingPack.error,
          stagingPack.pack_tbat_raw,(unsigned)stagingGauge.raw[2],stagingGauge.temperature_centi_c,
          pumpSample.die_centi_c,pumpSample.valid,(unsigned long long)pumpAt);
        if(size>0&&(size_t)size<sizeof(line)) {
            IOLog("NEZHA POWER THERMAL: %s",line);
            const bool changed=!packLogReported||stagingPack.epoch!=lastLoggedPack.epoch||
                stagingPack.valid_raw!=lastLoggedPack.valid_raw||stagingPack.error!=lastLoggedPack.error;
            if(persistentLog&&(io.control_mode!=6||changed||now<lastPackLogAt||now-lastPackLogAt>=10000)&&
               !np_power_log_push(line,(size_t)size)){
                lastLoggedPack=stagingPack;lastPackLogAt=now;packLogReported=true;
            }
        }
    }
    IOLog("NEZHA POWER SAMPLE: gen=%llu gauge=%x soc=%d mv=%d ma=%d centiC=%d remaining=%d full=%d design=%d cycle=%d charger=%x faults=%x link=%u replies_timeout=%u ignored=%u\n",
          (unsigned long long)stagingGauge.generation,stagingGauge.valid,stagingGauge.soc,
          stagingGauge.voltage_mv,stagingGauge.current_ma,stagingGauge.temperature_centi_c,
          stagingGauge.remaining_mah,stagingGauge.full_mah,stagingGauge.design_mah,stagingGauge.cycles,
          stagingCharger.valid,stagingCharger.fault,(unsigned)mca.up,mca.timeouts,mca.ignored);
    cycle = outgoing = false; refreshAt = now + (refreshRequested ? 250 : 2000);
    refreshRequested = false;
}
void next(uint64_t now) {
    while (cursor < 5 && !props[cursor]) ++cursor; // unverified fields stay absent
    if (cursor == 5) { commit(now); return; }
    if (np_mca_begin(&mca, props[cursor], NP_MCA_READ, nullptr, 0, 4, request) != NP_MCA_REQUEST) {
        if(cursor==4)stagingPack.error=-EIO;
        else stagingCharger.fault |= NP_FAULT_TRANSPORT;
        commit(now); return;
    }
    queuedAt = now; outgoing = true;
}
void begin(uint64_t now) {
    if (generation == UINT64_MAX) { __atomic_store_n(&stopRequested, 1u, __ATOMIC_RELEASE); return; }
    const uint64_t gen = ++generation;
    if(packRequested) {
        bzero(&stagingPack,sizeof(stagingPack));stagingPack.enabled=1;
        stagingPack.epoch=mca.epoch;stagingPack.generation=gen;
        stagingPack.error=-ECONNRESET;
    }
    uint8_t bytes[NP_GAUGE_WORDS][2] = {};
    uint32_t valid = 0;
    const int gaugeError = io.read_gauge(io.context, bytes, &valid);
    uint64_t end = io.now_ms(io.context);
    np_decode_gauge(&stagingGauge, bytes, valid, now, end, gen);
    if(io.control_mode>=2&&io.read_identity&&gaugeIdentity.valid!=3&&
       (stagingGauge.valid&NP_BIT(NP_PRESENT))&&stagingGauge.present) {
        NpGaugeIdentity identity={};
        int identityError=io.read_identity(io.context,&identity);
        if(identityError){identity.valid=0;identity.error=(uint32_t)identityError;}
        IOLockLock(snapshotLock);gaugeIdentity=identity;IOLockUnlock(snapshotLock);
        IOLog("NEZHA POWER IDENTITY: valid=%u error=%u cell=%u model=%s\n",
          identity.valid,identity.error,identity.cell_code,identity.device_name);
    }
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
        else if(cycle){if(cursor==4)stagingPack.error=-ETIMEDOUT;else stagingCharger.fault |= NP_FAULT_TRANSPORT; ++cursor; next(now);}
    }
    if (io.poll_link(io.context)) np_power27_link(0, transportEpoch);
    now = io.now_ms(io.context); // callbacks may have queued the NEXT request
    if (outgoing) {
        if (now < queuedAt || now - queuedAt >= 250) {
            np_mca_cancel(&mca); outgoing = false;
            if(auxiliaryActive)auxiliary_done(-ETIMEDOUT,nullptr,0,now);
            else{if(cursor==4)stagingPack.error=-ETIMEDOUT;else stagingCharger.fault |= NP_FAULT_TRANSPORT; ++cursor; next(now);}
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
                    else{if(cursor==4)stagingPack.error=-EIO;else stagingCharger.fault |= NP_FAULT_TRANSPORT; ++cursor; next(io.now_ms(io.context));}
                }
            }
            else if (r != -1) {
                np_mca_cancel(&mca); outgoing = false;
                if(auxiliaryActive)auxiliary_done(r,nullptr,0,now);
                else{if(cursor==4)stagingPack.error=r;else stagingCharger.fault |= NP_FAULT_TRANSPORT; ++cursor; next(now);}
            }
        }
    }
    if (!cycle&&!auxiliaryActive&&!mca.pending) {
        bool confirmingLimit=false;
        if(io.control_mode==5){
            IOLockLock(controlLock);
            confirmingLimit=np_buck_stock_confirming(&control.buck)!=0;
            IOLockUnlock(controlLock);
        }
        // VDM polling owns a live wire deadline. A due read cohort must wait
        // until this command is confirmed, rather than occupy its last ticks.
        // Between commands the ordinary cohort still refreshes gauge/facts.
        if((np_auth_busy(&auth)&&auth.state==NP_AUTH_POLL)||confirmingLimit)auxiliary_begin(io.now_ms(io.context));
        else if((!draining||((io.control_mode==4||io.control_mode==5||io.control_mode==6)&&(control.buck.armed||control.buck.owned))||np_auth_busy(&auth))&&now>=refreshAt)begin(io.now_ms(io.context));
        else auxiliary_begin(io.now_ms(io.context));
    }
    log_primary_auth(io.now_ms(io.context));
}
int drain_control(void) {
    if(io.control_mode<2)return 0;
    draining=true;IOLockLock(controlLock);np_control_stop(&control);np_auth_stop(&auth);IOLockUnlock(controlLock);
    uint64_t until=io.now_ms(io.context)+control_drain_budget_ms();
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
      control.buck.owned||control.buck.state==NP_BUCK_UNCERTAIN||np_auth_busy(&auth)||auth.owned;
    IOLockUnlock(controlLock);
    control_publish(io.now_ms(io.context));
    IOLog("NEZHA POWER CONTROL STOP: state=%u owned=%u pending_effect=%u; no replay\n",
      control.buck.state,control.buck.owned,control.buck.unverified_effect);
    if(persistentLog) {
        char line[384];int bytes=snprintf(line,sizeof(line),"S1 ms=%llu shutdown=%u state=%u owned=%u pending_effect=%u backend=%u confirmation=%u vbus_confirmed=%u ovp_restored=%u auth_state=%u auth_owned=%u auth_error=%u icl_vote_ma=%u icl_vote_withdrawal_supported=0 icl_hardware_limit_readback=0\n",
          (unsigned long long)io.now_ms(io.context),(unsigned)__atomic_load_n(&system_inshutdown,__ATOMIC_ACQUIRE),
          control.buck.state,control.buck.owned,control.buck.unverified_effect,control.buck.backend,control.buck.confirmation,
          control.buck.confirmed_vbus_mv,control.buck.protection_restored,auth.state,auth.owned,auth.error,
          io.control_mode==5?control.buck.stock.target[0]:0);
        if(bytes>0&&(size_t)bytes<sizeof(line))(void)np_power_log_push(line,(size_t)bytes);
    }
    return incomplete?EIO:0;
}
void log_boot_adc() {
    if(!io.adc_boot_probe)return;
    const NpAdcNativeBootProbe &p=*io.adc_boot_probe;
    const NpAdcNativeReading &a=p.reading[0],&b=p.reading[1];
    char line[512];int size=snprintf(line,sizeof(line),
      "T1 ms=%llu gen=0 epoch=0 valid_raw=0 error=%d adc_boot=1 adc_attempted=%u adc_init_error=%d adc_cleanup_error=%d adc0_valid=%u adc0_raw=%u adc0_c1024=%d adc0_centi_c=%d adc0_error=%d adc0_quiet=%u adc1_valid=%u adc1_raw=%u adc1_c1024=%d adc1_centi_c=%d adc1_error=%d adc1_quiet=%u\n",
      (unsigned long long)io.now_ms(io.context),-ENODATA,p.attempted,p.init_error,p.stop_error,
      a.converted,(unsigned)a.raw,a.c1024,a.centi_c,a.error,a.quiescent,
      b.converted,(unsigned)b.raw,b.c1024,b.centi_c,b.error,b.quiescent);
    if(size>0&&(size_t)size<sizeof(line)) {
        IOLog("NEZHA POWER ADC: %s",line);
        if(persistentLog)(void)np_power_log_push(line,(size_t)size);
    }
}
int quiesce_adc() {
    if(!io.quiesce_adc)return 0;
    int error=io.quiesce_adc(io.context);
    char line[160];int size=snprintf(line,sizeof(line),
      "T1 ms=%llu gen=0 epoch=0 valid_raw=0 error=%d adc_quiesce=1 adc_cleanup_error=%d\n",
      (unsigned long long)io.now_ms(io.context),-ENODATA,error);
    if(size>0&&(size_t)size<sizeof(line)) {
        IOLog("NEZHA POWER ADC: %s",line);
        if(persistentLog)(void)np_power_log_push(line,(size_t)size);
    }
    return error;
}
void invalidate_tsens(NpTsensThermal &t,int error) {
    t.error=error;t.sample.read_mask=t.sample.valid_mask=0;
    for(auto &r:t.sample.readings){r.read_ok=r.valid=0;r.deci_c=INT32_MIN;}
}
void invalidate_connector(NpConnectorThermal &t,int error) {
    t.error=error;t.valid_mask=0;
    for(auto &r:t.reading){r.converted=0;r.c1024=r.centi_c=INT32_MIN;}
}
void invalidate_board(NpBoardThermal &t,int error) {
    t.valid_mask=0;t.derived_valid=0;
    for(unsigned i=0;i<8;++i){if(!t.errors[i])t.errors[i]=error;t.input_milli_c[i]=INT32_MIN;}
}
int32_t adc_board_value(const NpAdcNativeReading &r) {
    /* The actual OS4 ADC getter hands TC1024 directly to the thermal zone.
     * Preserve that numeric coordinate for the original formula. Native
     * centi-C display remains the separately converted connector reading. */
    return r.c1024;
}
void publish_board() {
    stagingBoard.end_ms=io.now_ms(io.context);
    IOLockLock(controlLock);
    const bool sameSource=stagingBoard.epoch==control.epoch&&
        stagingBoard.attachment==control.attachment;
    IOLockUnlock(controlLock);
    for(unsigned i=0;i<8;++i) {
        if(!(stagingBoard.valid_mask&(1u<<i)))continue;
        const uint64_t sampled=stagingBoard.sampled_ms[i];
        if(!sameSource||sampled<stagingBoard.begin_ms||sampled>stagingBoard.end_ms||
           stagingBoard.end_ms-sampled>NP_MAX_SKEW_MS) {
            stagingBoard.valid_mask&=~(1u<<i);stagingBoard.errors[i]=ESTALE;
        }
    }
    /* mi_thermal_interface defaults the compensation to0; the captured
     * normal profile also reads0. This is the explicitly named normal
     * formula diagnostic, not the daemon's runtime-selected board cache. */
    np_board_thermal_calculate(&stagingBoard,0);
    IOLockLock(snapshotLock);boardThermal=stagingBoard;IOLockUnlock(snapshotLock);
    boardActive=boardPending=false;connectorAt=stagingBoard.end_ms+1000;
    if(!persistentLog)return;
    /* Keep live H4/thermal sampling at its original rate. Only successful
     * repeated diagnostic rows use10s persistence in the PPS trial; source,
     * mask, retention and error transitions are retained immediately. */
    const bool changed=!boardLogReported||stagingBoard.epoch!=lastLoggedBoard.epoch||
        stagingBoard.attachment!=lastLoggedBoard.attachment||
        stagingBoard.valid_mask!=lastLoggedBoard.valid_mask||stagingBoard.retained!=lastLoggedBoard.retained||
        stagingBoard.derived_valid!=lastLoggedBoard.derived_valid||
        memcmp(stagingBoard.errors,lastLoggedBoard.errors,sizeof(stagingBoard.errors));
    if(io.control_mode==6&&!changed&&stagingBoard.end_ms>=lastBoardLogAt&&
       stagingBoard.end_ms-lastBoardLogAt<10000)return;
    char line[1024];int size=snprintf(line,sizeof(line),
      "T3 board_diag=1 gen=%llu epoch=%llu attach=%llu begin_ms=%llu end_ms=%llu attempted=%u valid=%u retained=%u derived_valid=%u board_valid=0 global_comp_mc=%d",
      (unsigned long long)stagingBoard.generation,(unsigned long long)stagingBoard.epoch,
      (unsigned long long)stagingBoard.attachment,(unsigned long long)stagingBoard.begin_ms,
      (unsigned long long)stagingBoard.end_ms,stagingBoard.attempted_mask,stagingBoard.valid_mask,
      stagingBoard.retained,stagingBoard.derived_valid,stagingBoard.global_comp_milli_c);
    const char *labels[]={"raw","mc","sample_ms","io"};
    for(unsigned field=0;field<4&&size>0&&(size_t)size<sizeof(line);++field) {
        size+=snprintf(line+size,sizeof(line)-(size_t)size," %s=",labels[field]);
        for(unsigned i=0;i<8&&size>0&&(size_t)size<sizeof(line);++i) {
            if(field==2)size+=snprintf(line+size,sizeof(line)-(size_t)size,"%s%llu",i?",":"",
                (unsigned long long)stagingBoard.sampled_ms[i]);
            else if(field==1&&!(stagingBoard.valid_mask&(1u<<i)))
                size+=snprintf(line+size,sizeof(line)-(size_t)size,"%snull",i?",":"");
            else size+=snprintf(line+size,sizeof(line)-(size_t)size,"%s%d",i?",":"",
                field==0?(int)stagingBoard.raw[i]:field==1?stagingBoard.input_milli_c[i]:stagingBoard.errors[i]);
        }
    }
    if(size>0&&(size_t)size<sizeof(line)) {
        size+=snprintf(line+size,sizeof(line)-(size_t)size," directions_mc=");
        for(unsigned i=0;i<5&&size>0&&(size_t)size<sizeof(line);++i) {
            if(stagingBoard.derived_valid)size+=snprintf(line+size,sizeof(line)-(size_t)size,"%s%d",
                i?",":"",stagingBoard.direction_milli_c[i]);
            else size+=snprintf(line+size,sizeof(line)-(size_t)size,"%snull",i?",":"");
        }
    }
    if(size>0&&(size_t)size<sizeof(line)) {
        if(stagingBoard.derived_valid)size+=snprintf(line+size,sizeof(line)-(size_t)size,
            " formula_mc=%d charger_mc=%d\n",stagingBoard.formula_milli_c,stagingBoard.charger_milli_c);
        else size+=snprintf(line+size,sizeof(line)-(size_t)size," formula_mc=null charger_mc=null\n");
    }
    if(size>0&&(size_t)size<sizeof(line)&&!np_power_log_push(line,(size_t)size)){
        lastLoggedBoard=stagingBoard;lastBoardLogAt=stagingBoard.end_ms;boardLogReported=true;
    }
}
void begin_board(bool retained=false) {
    stagingBoard=NpBoardThermal{};stagingBoard.enabled=1;
    stagingBoard.generation=stagingConnector.generation;
    stagingBoard.begin_ms=stagingConnector.begin_ms;
    stagingBoard.epoch=connectorEpoch;stagingBoard.attachment=connectorAttachment;
    for(unsigned i=0;i<8;++i){stagingBoard.errors[i]=ENODATA;stagingBoard.input_milli_c[i]=INT32_MIN;}
    const auto &conn=stagingConnector.reading[0];
    if(stagingConnector.attempted&1)stagingBoard.attempted_mask|=1u<<2;
    stagingBoard.raw[2]=conn.raw;stagingBoard.sampled_ms[2]=stagingConnector.sampled_ms[0];
    stagingBoard.errors[2]=conn.error;
    if((stagingConnector.valid_mask&1)&&conn.converted&&!conn.error) {
        stagingBoard.valid_mask|=1u<<2;stagingBoard.input_milli_c[2]=adc_board_value(conn);
    }
    if(retained){stagingBoard.retained=1;publish_board();return;}
    NpBoardTemperatureReading r={};stagingBoard.attempted_mask|=1;
    const int error=io.read_original_temperature(io.context,&r);
    stagingBoard.raw[0]=r.raw;stagingBoard.sampled_ms[0]=r.end_ms;
    stagingBoard.errors[0]=error?(error<0?-error:error):r.error;
    if(!stagingBoard.errors[0]&&(!r.valid||r.begin_ms<stagingBoard.begin_ms||r.end_ms<r.begin_ms))
        stagingBoard.errors[0]=EIO;
    if(!stagingBoard.errors[0]){stagingBoard.valid_mask|=1;stagingBoard.input_milli_c[0]=r.milli_c;}
    boardActive=true;boardPending=false;boardChannel=2;
}
int poll_board() {
    if(!boardActive)return 0;
    NpAdcNativeReading r={};int result;
    const unsigned input=boardChannel==2?1:boardChannel;
    if(boardPending)result=io.poll_connector(io.context,&r);
    else {
        stagingBoard.attempted_mask|=1u<<input;
        result=io.begin_connector(io.context,boardChannel,&r);
    }
    if(result==1){boardPending=true;return 0;}
    boardPending=false;
    if(result<0&&!r.error)r.error=EIO;
    if(!r.error&&(!r.converted||!r.quiescent))r.error=EIO;
    stagingBoard.raw[input]=r.raw;stagingBoard.sampled_ms[input]=io.now_ms(io.context);
    stagingBoard.errors[input]=r.error;
    if(!r.error){stagingBoard.valid_mask|=1u<<input;stagingBoard.input_milli_c[input]=adc_board_value(r);}
    if(!r.quiescent) {
        stagingBoard.retained=1;publish_board();return r.error?r.error:EIO;
    }
    if(++boardChannel==8)publish_board();
    return 0;
}
void publish_connector() {
    stagingConnector.end_ms=io.now_ms(io.context);
    IOLockLock(snapshotLock);connectorThermal=stagingConnector;IOLockUnlock(snapshotLock);
    NpConnectorPolicyStatus policy={};
    IOLockLock(controlLock);
    np_control_connector_sample(&control,&stagingConnector,stagingConnector.end_ms);
    connector_policy_locked(policy,stagingConnector.end_ms);
    IOLockUnlock(controlLock);
    connectorActive=connectorPending=false;
    connectorAt=stagingConnector.end_ms+1000;
    if(!persistentLog)return;
    const auto &vote=policy.vote;
    const auto &previousVote=lastLoggedConnectorPolicy.vote;
    const bool changed=!connectorLogReported||policy.epoch!=lastLoggedConnectorPolicy.epoch||
        policy.attachment!=lastLoggedConnectorPolicy.attachment||
        stagingConnector.valid_mask!=lastLoggedConnector.valid_mask||stagingConnector.error!=lastLoggedConnector.error||
        stagingConnector.retained!=lastLoggedConnector.retained||policy.enabled!=lastLoggedConnectorPolicy.enabled||
        vote.charge_allowed!=previousVote.charge_allowed||vote.decision_complete!=previousVote.decision_complete||
        vote.latch!=previousVote.latch||vote.cooldown_wait!=previousVote.cooldown_wait||
        vote.reason!=previousVote.reason||vote.error!=previousVote.error||vote.last_error!=previousVote.last_error||
        policy.effective_policy_valid!=lastLoggedConnectorPolicy.effective_policy_valid||
        policy.effective_policy_enabled!=lastLoggedConnectorPolicy.effective_policy_enabled;
    if(io.control_mode==6&&!changed&&stagingConnector.end_ms>=lastConnectorLogAt&&
       stagingConnector.end_ms-lastConnectorLogAt<10000)return;
    const auto &a=stagingConnector.reading[0],&b=stagingConnector.reading[1];
    char cc0[16],cc1[16];
    connector_number(cc0,sizeof(cc0),(stagingConnector.valid_mask&1)&&a.converted&&!a.error,a.centi_c);
    connector_number(cc1,sizeof(cc1),(stagingConnector.valid_mask&2)&&b.converted&&!b.error,b.centi_c);
    char line[1024];int size=snprintf(line,sizeof(line),
      "T3 gen=%llu begin_ms=%llu end_ms=%llu attempted=%u valid=%u error=%d retained=%u t0_ms=%llu raw0=%u cc0=%s io0=%d quiet0=%u t1_ms=%llu raw1=%u cc1=%s io1=%d quiet1=%u thermal_vote=%u",
      (unsigned long long)stagingConnector.generation,(unsigned long long)stagingConnector.begin_ms,
      (unsigned long long)stagingConnector.end_ms,stagingConnector.attempted,stagingConnector.valid_mask,
      stagingConnector.error,stagingConnector.retained,
      (unsigned long long)stagingConnector.sampled_ms[0],(unsigned)a.raw,cc0,a.error,a.quiescent,
      (unsigned long long)stagingConnector.sampled_ms[1],(unsigned)b.raw,cc1,b.error,b.quiescent,policy.enabled);
    if(policy.enabled&&size>0&&(size_t)size<sizeof(line)) {
        const auto &v=policy.vote;const auto &s=policy.signals;
        char tc0[16],tc1[16],effective[16],rate0[32],rate1[32],selected[32];
        connector_number(tc0,sizeof(tc0),v.temperature_valid_mask&1,v.temperature_c[0]);
        connector_number(tc1,sizeof(tc1),v.temperature_valid_mask&2,v.temperature_c[1]);
        connector_number(effective,sizeof(effective),v.temperature_valid_mask==3,v.effective_temperature_c);
        connector_number(rate0,sizeof(rate0),(v.rate_valid_mask|v.rate_initialized_mask)&1,v.rate_c_per_s[0]);
        connector_number(rate1,sizeof(rate1),(v.rate_valid_mask|v.rate_initialized_mask)&2,v.rate_c_per_s[1]);
        connector_number(selected,sizeof(selected),v.selected_rate_valid||v.selected_rate_initialized,v.selected_rate_c_per_s);
        size+=snprintf(line+size,sizeof(line)-(size_t)size,
          " epoch=%llu attach=%llu vote_ms=%llu allowed=%u complete=%u latch=%u wait=%u reason=%u cooldown_reason=%u ab_error=%d last_error=%d temp_mask=%u tc0=%s tc1=%s effective_c=%s rate_mask=%u rate_init=%u dt_ms=%llu rate0=%s rate1=%s selected=%u selected_rate=%s selected_valid=%u selected_init=%u online=%d online_valid=%u cid=%d cid_valid=%u otg=%d otg_valid=%u board_valid=0 policy_valid=%u policy_enabled=%u",
          (unsigned long long)policy.epoch,(unsigned long long)policy.attachment,(unsigned long long)v.now_ms,
          v.charge_allowed,v.decision_complete,v.latch,v.cooldown_wait,v.reason,v.cooldown_reason,v.error,v.last_error,
          v.temperature_valid_mask,tc0,tc1,effective,v.rate_valid_mask,v.rate_initialized_mask,
          (unsigned long long)v.rate_interval_ms,rate0,rate1,v.selected_index,selected,
          v.selected_rate_valid,v.selected_rate_initialized,s.usb_online.value,s.usb_online.valid,
          s.cid.value,s.cid.valid,s.otg_plugin.value,s.otg_plugin.valid,
          policy.effective_policy_valid,policy.effective_policy_enabled);
    }
    if(size>0&&(size_t)size<sizeof(line))size+=snprintf(line+size,sizeof(line)-(size_t)size,"\n");
    if(size>0&&(size_t)size<sizeof(line)&&!np_power_log_push(line,(size_t)size)){
        lastLoggedConnector=stagingConnector;lastLoggedConnectorPolicy=policy;
        lastConnectorLogAt=stagingConnector.end_ms;connectorLogReported=true;
    }
}
int poll_connector(uint64_t now) {
    if(!io.begin_connector||boardActive)return 0;
    if(!connectorActive) {
        if(now<connectorAt)return 0;
        stagingConnector=NpConnectorThermal{};stagingConnector.enabled=1;
        stagingConnector.generation=connectorThermal.generation+1;stagingConnector.begin_ms=now;
        if(io.read_original_temperature) {
            IOLockLock(controlLock);connectorEpoch=control.epoch;
            connectorAttachment=control.attachment;IOLockUnlock(controlLock);
        }
        for(auto &r:stagingConnector.reading){r.error=ENODATA;r.c1024=r.centi_c=INT32_MIN;}
        connectorActive=true;connectorPending=false;connectorChannel=0;
    }
    NpAdcNativeReading r={};
    int result;
    if(connectorPending)result=io.poll_connector(io.context,&r);
    else {
        stagingConnector.attempted|=1u<<connectorChannel;
        result=io.begin_connector(io.context,connectorChannel,&r);
    }
    if(result==1){connectorPending=true;return 0;}
    connectorPending=false;
    if(result<0&&!r.error)r.error=EIO;
    if(!r.error&&(!r.converted||!r.quiescent))r.error=EIO;
    if(r.error){r.converted=0;r.c1024=r.centi_c=INT32_MIN;}
    stagingConnector.reading[connectorChannel]=r;
    stagingConnector.sampled_ms[connectorChannel]=io.now_ms(io.context);
    if(r.converted)stagingConnector.valid_mask|=1u<<connectorChannel;
    if(r.error&&!stagingConnector.error)stagingConnector.error=r.error;
    if(!r.quiescent) {
        stagingConnector.retained=1;publish_connector();
        if(io.read_original_temperature)begin_board(true);
        return r.error?r.error:EIO;
    }
    if(++connectorChannel==2) {
        publish_connector();
        if(io.read_original_temperature)begin_board();
    }
    return 0;
}
void poll_tsens(uint64_t now) {
    if(!io.read_tsens||now<tsensAt)return;
    tsensAt=now+10000;
    stagingTsens.enabled=1;
    stagingTsens.generation=tsensThermal.generation+1;
    stagingTsens.begin_ms=now;
    memset(&stagingTsens.sample,0,sizeof(stagingTsens.sample));
    invalidate_tsens(stagingTsens,-ENODATA);
    stagingTsens.error=io.read_tsens(io.context,&stagingTsens.sample);
    stagingTsens.end_ms=io.now_ms(io.context);
    IOLockLock(snapshotLock);tsensThermal=stagingTsens;IOLockUnlock(snapshotLock);
    if(!persistentLog)return;
    unsigned first=0;
    for(unsigned c=0;c<NP_TSENS_CONTROLLER_COUNT;++c) {
        const unsigned count=np_tsens_controllers[c].sensors;
        const uint64_t mask=(UINT64_C(1)<<count)-1;
        char line[768];int at=snprintf(line,sizeof(line),
          "T2 gen=%llu begin_ms=%llu end_ms=%llu controller=%u count=%u read=%llx valid=%llx last_status=1 error=%d raw=",
          (unsigned long long)stagingTsens.generation,(unsigned long long)stagingTsens.begin_ms,
          (unsigned long long)stagingTsens.end_ms,c,count,
          (unsigned long long)((stagingTsens.sample.read_mask>>first)&mask),
          (unsigned long long)((stagingTsens.sample.valid_mask>>first)&mask),stagingTsens.error);
        for(unsigned s=0;s<count&&at>0&&(size_t)at<sizeof(line);++s)
            at+=snprintf(line+at,sizeof(line)-(size_t)at,"%s%08x",s?",":"",stagingTsens.sample.readings[first+s].raw);
        if(at>0&&(size_t)at<sizeof(line))at+=snprintf(line+at,sizeof(line)-(size_t)at," dc=");
        for(unsigned s=0;s<count&&at>0&&(size_t)at<sizeof(line);++s) {
            const auto &r=stagingTsens.sample.readings[first+s];
            if(r.valid)at+=snprintf(line+at,sizeof(line)-(size_t)at,"%s%d",s?",":"",r.deci_c);
            else at+=snprintf(line+at,sizeof(line)-(size_t)at,"%snone",s?",":"");
        }
        if(at>0&&(size_t)at<sizeof(line))at+=snprintf(line+at,sizeof(line)-(size_t)at," io=");
        for(unsigned s=0;s<count&&at>0&&(size_t)at<sizeof(line);++s)
            at+=snprintf(line+at,sizeof(line)-(size_t)at,"%s%d",s?",":"",stagingTsens.sample.readings[first+s].io_error);
        if(at>0&&(size_t)at<sizeof(line))at+=snprintf(line+at,sizeof(line)-(size_t)at,"\n");
        if(at>0&&(size_t)at<sizeof(line))(void)np_power_log_push(line,(size_t)at);
        first+=count;
    }
}
void invalidate(uint32_t fault) {
    persistentSampleReported=false;
    np_mca_link(&mca, 0); cycle = outgoing = false;
    IOLockLock(snapshotLock);
    memset(&gaugeIdentity,0,sizeof(gaugeIdentity));
    charger.valid = gauge.valid = 0; charger.fault = fault;
    packThermal.valid_raw=0;packThermal.error=-EIO;
    if(tsensThermal.enabled)invalidate_tsens(tsensThermal,-ESTALE);
    if(connectorThermal.enabled)invalidate_connector(connectorThermal,-ESTALE);
    if(boardThermal.enabled)invalidate_board(boardThermal,-ESTALE);
    IOLockUnlock(snapshotLock);
    // The CPU diagnostic copy must withdraw the same stale sample as the OS
    // power source while its worker is suspended or has stopped.
    IOLockLock(controlLock);
    control.sample.valid = control.sample.gauge.valid = control.sample.charger.valid = 0;
    control.sample.faults |= fault;
    np_control_connector_invalidate(&control,-ESTALE);
    pumpSample.valid = pumpSample.controls_valid = pumpSample.switching_valid = 0;
    control.pump.valid = control.pump.controls_valid = control.pump.switching_valid = 0;
    pumpSample.protection.valid = control.pump.protection.valid = 0;
    IOLockUnlock(controlLock);
}
void worker(void *, wait_result_t) {
    __atomic_store_n(&lifecycle, 1, __ATOMIC_RELEASE);
    source = np_battery_service_create();
    if(io.control_mode>=2) {
        (void)PE_parse_boot_argn("nezha-power-log",&persistentLog,sizeof(persistentLog));
        if(persistentLog&&np_power_log_start())persistentLog=0;
        log_boot_adc();
        np_power_diagnostic_start();
    }
    bool suspended = false;
    bool controlDrainAttempted=false,quiesceAttempted=false;int drainError=0;
    bool adcQuiesceAttempted=false;
    int error = source ? 0 : -1;
    /* Normal Settings shutdown uses the same XNU flag as USB shutdown.
     * Finish controller/log work before the normal VFS unmount sequence. */
    while (!error && !__atomic_load_n(&stopRequested, __ATOMIC_ACQUIRE)&&
           !__atomic_load_n(&system_inshutdown,__ATOMIC_ACQUIRE)) {
        const bool requested = __atomic_load_n(&suspendRequested, __ATOMIC_ACQUIRE) != 0;
        if (requested && !suspended) {
            controlDrainAttempted=true;drainError=drain_control();error=drainError;
            invalidate(NP_FAULT_STALE);
            if(!error&&io.quiesce_adc){adcQuiesceAttempted=true;error=quiesce_adc();}
            if(!error){quiesceAttempted=true;error = io.quiesce(io.context);}
            if (!error) { suspended = true; __atomic_store_n(&lifecycle, 3, __ATOMIC_RELEASE); }
        } else if (!requested && suspended) {
            error = io.resume(io.context);
            if (!error) {
                suspended = false; refreshAt = 0;
                tsensAt=0;
                connectorAt=0;connectorActive=connectorPending=false;
                boardActive=boardPending=false;
                controlDrainAttempted=false;drainError=0;
                quiesceAttempted=false;
                adcQuiesceAttempted=false;
                draining=false;IOLockLock(controlLock);control.stopping=0;
                if(np_auth_resume(&auth))error=EIO;
                IOLockUnlock(controlLock);
                __atomic_store_n(&lifecycle, 1, __ATOMIC_RELEASE);
            }
        }
        if (!error && !suspended) {
            step();int adcError=0;
            if(!control.adc_error){
                adcError=poll_connector(io.now_ms(io.context));
                if(!adcError)adcError=poll_board();
            }
            if(adcError){
                /* Optional ADC failure fences only its sampling owner. Keep
                 * its mappings/unknown effect, withdraw thermal freshness
                 * and stop controlled charging. Gauge/MCA status and the
                 * existing controller's measured fallback keep running. */
                IOLockLock(controlLock);
                control.adc_error=adcError<0?(uint32_t)-adcError:(uint32_t)adcError;
                control.allow_control=0;np_buck_stop(&control.buck);np_auth_stop(&auth);
                np_control_connector_invalidate(&control,adcError);
                IOLockUnlock(controlLock);
                connectorActive=connectorPending=boardActive=boardPending=false;
                IOLockLock(snapshotLock);
                invalidate_connector(connectorThermal,adcError);
                invalidate_board(boardThermal,adcError);
                IOLockUnlock(snapshotLock);
                IOLog("NEZHA POWER ADC: sampling fenced error=%d; control stopped, gauge/MCA continue; mappings retained\n",adcError);
            }
            if(!error)poll_tsens(io.now_ms(io.context));
        }
        IOSleep(WORKER_TICK_MS);
    }
    if(!suspended&&!controlDrainAttempted)drainError=drain_control();
    if(!error)error=drainError;
    if(!adcQuiesceAttempted&&io.quiesce_adc) {
        adcQuiesceAttempted=true;const int adcError=quiesce_adc();
        if(!error)error=adcError;
    }
    if(persistentLog) {
        np_power_log_stop();uint64_t until=io.now_ms(io.context)+LOG_STOP_BUDGET_MS;NpPowerLogStatus log={};
        bool terminal=false;
        do {
            if(np_power_log_status(&log)&&(log.phase==3||log.phase==4)){terminal=true;break;}
            IOSleep(20);
        } while(io.now_ms(io.context)<until);
        if(!error) {
            if(!terminal)error=ETIMEDOUT;
            else if(log.phase!=4||log.error)error=log.error?(int)log.error:EIO;
        }
    }
    invalidate(NP_FAULT_TRANSPORT);
    // Auth failure need not keep idle board resources active. Unknown BUCK
    // effects still retain those resources/ADC protections, as before.
    // Keep either failure and all mappings; never retry a failed quiesce.
    const bool effectsReleased=!control.buck.pending&&!control.buck.owned&&
        !control.buck.unverified_effect&&control.buck.state!=NP_BUCK_UNCERTAIN;
    if (!suspended&&!quiesceAttempted&&effectsReleased) {
        quiesceAttempted=true;const int quietError=io.quiesce(io.context);
        if(!error)error=quietError;
    }
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
    if((ops->control_mode==4||ops->control_mode==6)&&!ops->set_pump_usb_ovp)return -1;
    if(ops->adc_boot_probe&&!ops->quiesce_adc)return -1;
    if((ops->begin_connector||ops->poll_connector)&&
       (!ops->begin_connector||!ops->poll_connector||!ops->quiesce_adc||ops->adc_boot_probe))return -1;
    if(ops->read_original_temperature&&!ops->begin_connector)return -1;
    if (ops->schema_verified && ops->property_status &&
        (!ops->status_valid_mask || (ops->status_charging_mask & ~ops->status_valid_mask))) return -1;
    int pack=0;(void)PE_parse_boot_argn("nezha-power-pack-tbat",&pack,sizeof(pack));
    if(pack&&(pack!=1||!ops->schema_verified||ops->property_online!=0x20001||
        ops->property_status!=0x2001||ops->property_input_uv!=0x20002||ops->property_input_ua!=0x20004))return -1;
    snapshotLock = IOLockAlloc();
    controlLock=IOLockAlloc();if (!snapshotLock||!controlLock) return -1;
    io = *ops; props[0] = io.property_online; props[1] = io.property_status;
    props[2] = io.property_input_uv; props[3] = io.property_input_ua;
    packRequested=pack==1;props[4]=packRequested?NP_PACK_TBAT_PROPERTY:0;
    packThermal.enabled=packRequested;
    tsensThermal.enabled=ops->read_tsens!=nullptr;
    invalidate_tsens(tsensThermal,-ENODATA);tsensAt=0;
    connectorThermal.enabled=ops->begin_connector!=nullptr;
    invalidate_connector(connectorThermal,-ENODATA);connectorAt=0;
    boardThermal.enabled=ops->read_original_temperature!=nullptr;
    invalidate_board(boardThermal,-ENODATA);boardActive=boardPending=false;
    np_mca_init(&mca);np_control_init(&control,io.control_mode);
    np_control_connector_policy_enable(&control,(io.control_mode==5||io.control_mode==6)&&io.begin_connector!=nullptr);
    np_auth_init(&auth,io.auth_material,&io.auth,io.auth_enabled);bound = 1; return 0;
}
extern "C" void nezha_power27_start(void) {
    int mode = 0;
    (void)PE_parse_boot_argn("nezha-power", &mode, sizeof(mode));
    if (!mode) return;
    if ((mode != 1&&mode!=2&&mode!=3&&mode!=4&&mode!=5&&mode!=6) || !bound) { IOLog("NEZHA POWER: binding unavailable or unsupported mode\n"); return; }
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
extern "C" int nezha_power27_prepare_shutdown(void) {
    if(!__atomic_load_n(&started,__ATOMIC_ACQUIRE)||
       __atomic_load_n(&workerExited,__ATOMIC_ACQUIRE))return 0;
    // Share the existing pending stop with PM/shutdown callbacks. Only its
    // worker executes drain, ADC/board cleanup and log stop.
    uint32_t expected=0;
    (void)__atomic_compare_exchange_n(&stopRequested,&expected,1u,false,
        __ATOMIC_RELEASE,__ATOMIC_RELAXED);
    const uint64_t until=io.now_ms(io.context)+control_drain_budget_ms()+
        LOG_STOP_BUDGET_MS+WORKER_TICK_MS;
    while(!__atomic_load_n(&workerExited,__ATOMIC_ACQUIRE)) {
        if(io.now_ms(io.context)>=until)return ETIMEDOUT;
        IOSleep(WORKER_TICK_MS);
    }
    // A terminal failure still means this worker has stopped touching IO.
    // Log integrity and successful power transitions remain separate facts.
    return 0;
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
extern "C" int nezha_power27_worker_exited(void) {
    return __atomic_load_n(&workerExited,__ATOMIC_ACQUIRE)!=0;
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
    packThermal.valid_raw=0;packThermal.error=-ECONNRESET;
    IOLockUnlock(snapshotLock); refreshAt = 0;
}
extern "C" void np_power27_receive(uint64_t epoch, const uint8_t *p, uint32_t n) {
    if (!epoch || epoch != transportEpoch || __atomic_load_n(&workerExited,__ATOMIC_ACQUIRE)) return;
    uint32_t note = 0, value = 0;
    if (np_mca_timeout(&mca, io.now_ms(io.context))) {
        if(auxiliaryActive)auxiliary_done(-ETIMEDOUT,nullptr,0,io.now_ms(io.context));
        else if(cycle){if(cursor==4)stagingPack.error=-ETIMEDOUT;else stagingCharger.fault |= NP_FAULT_TRANSPORT; ++cursor; next(io.now_ms(io.context));}
    }
    const int r = np_mca_receive(&mca, mca.epoch, io.now_ms(io.context), p, n, &note, &value);
    if(r==NP_MCA_NOTE&&io.control_mode>=2&&note==0) {
        // Current OS4 qcom_subpmic_notify_cb@256c/2578 consumes TWO u32:
        // online then real_type. A PD->PD_VERIFY notification is not unplug.
        const uint32_t realType=np_le32(p+20);
        IOLockLock(controlLock);
        int changed=np_control_usb_type(&control,value,realType,io.now_ms(io.context));
        if(changed<0)np_control_source_event(&control);
        np_auth_source(&auth,control.epoch,control.attachment);IOLockUnlock(controlLock);
        IOLog("NEZHA POWER USB TYPE: online=%u type=%u source_changed=%d\n",value,realType,changed);
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
        case 4:
            stagingPack.pack_tbat_raw=(int32_t)raw;stagingPack.valid_raw=1;
            stagingPack.error=0;stagingPack.sampled_ms=io.now_ms(io.context);
            break;
        }
    } else if(cursor==4)stagingPack.error=-EIO;
    else stagingCharger.fault |= NP_FAULT_CHARGER;
    ++cursor; next(io.now_ms(io.context));
}

extern "C" int np_power27_write_allowed(const uint8_t *p,uint32_t bytes) {
    if(authAuxiliary) {
        if(io.control_mode<2||!p||bytes!=NP_MCA_REQUEST||!auxiliaryActive||!outgoing||!mca.pending||
           !authCommand.write||np_le32(p)!=NP_MCA_OWNER||np_le32(p+4)!=1||
           np_le32(p+8)!=NP_MCA_WRITE||np_le32(p+12)!=authCommand.property||np_le32(p+16)!=mca.sequence)
            return 0;
        for(unsigned i=20+authCommand.write_bytes;i<bytes;++i)if(p[i])return 0;
        bool clear=authCommand.write_bytes==4&&(authCommand.property==0x21002||authCommand.property==0x21013)&&
            np_le32(p+20)==0;
        if(!clear) {
            NpPowerLogStatus log={};
            if(!persistentLog)return 0;
            if(!np_power_log_status(&log))return -1; /* no wire submission while the logger is busy */
            if(!usable_log(log))return 0;
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
    const unsigned payloadEnd=auxiliaryCommand.property==NP_BUCK_ENABLE?21:24;
    for(unsigned i=payloadEnd;i<bytes;++i)if(p[i])return 0;
    NpPowerLogStatus log={};
    if(control.buck.state==NP_BUCK_SET_ICL||control.buck.state==NP_BUCK_SET_FCC||
       control.buck.state==NP_BUCK_FCC_INITIALIZE||control.buck.state==NP_BUCK_PD_REQUEST_9V||
       (io.control_mode==5&&auxiliaryCommand.value&&
        (control.buck.state==NP_BUCK_POLICY_CHECK||control.buck.state==NP_BUCK_POLICY_FCC_CHECK))) {
        if(!np_power_log_status(&log))return -1; /* transient busy: no wire write or cancellation */
        if(!usable_log(log))return 0;
    }
    IOLockLock(controlLock);
    const bool ppsIncrease=io.control_mode==6&&(auxiliaryCommand.property==NP_BUCK_PPS_PD||
        (auxiliaryCommand.property==NP_BUCK_FCC&&auxiliaryCommand.value>500));
    const bool sameAuth=!ppsIncrease||(auth.enabled&&auth.battery_ok&&auth.verified&&
        auth.epoch==control.epoch&&auth.attachment==control.attachment&&
        !(auth.owned&&auth.state!=NP_AUTH_DONE));
    int allowed=sameAuth&&np_control_write_allowed(&control,&auxiliaryCommand,io.now_ms(io.context));
    IOLockUnlock(controlLock);return allowed;
}
extern "C" int np_power_control_copy(NpPowerControl *out) {
    if(!out||!controlLock||!IOLockTryLock(controlLock))return 0;
    *out=control;IOLockUnlock(controlLock);return 1;
}
extern "C" int np_power_connector_policy_copy(NpConnectorPolicyStatus *out) {
    if(!out||!bound||!controlLock||!IOLockTryLock(controlLock))return 0;
    connector_policy_locked(*out,io.now_ms(io.context));
    IOLockUnlock(controlLock);return 1;
}
extern "C" int np_power_thermal_copy(NpPackThermal *out) {
    if(!out||!bound||!snapshotLock)return 0;
    if(!packRequested){bzero(out,sizeof(*out));return 1;}
    if(!IOLockTryLock(snapshotLock))return 0;
    *out=packThermal;
    const uint64_t now=io.now_ms(io.context);
    if(out->valid_raw&&(__atomic_load_n(&lifecycle,__ATOMIC_ACQUIRE)!=1||
        now<out->sampled_ms||now-out->sampled_ms>NP_CHARGER_TTL_MS)) {
        out->valid_raw=0;out->error=-ETIMEDOUT;
    }
    IOLockUnlock(snapshotLock);return 1;
}
extern "C" int np_power_auth_copy(NpAuthStatus *out) {
    if(!out||!controlLock||!IOLockTryLock(controlLock))return 0;
    *out=NpAuthStatus{auth.epoch,auth.attachment,auth.verified_ms,
        auth.enabled,auth.state,auth.error,auth.owned,auth.attempts,auth.svid,auth.adapter_id,
        auth.battery_ok,auth.verified,auth.battery_checks,auth.battery_error,auth.discovery_until};
    IOLockUnlock(controlLock);return 1;
}
extern "C" int np_power_identity_copy(NpGaugeIdentity *out) {
    if(!out||!snapshotLock||!IOLockTryLock(snapshotLock))return 0;
    *out=gaugeIdentity;
    const uint64_t now=io.now_ms(io.context);
    if(__atomic_load_n(&lifecycle,__ATOMIC_ACQUIRE)!=1||
       !(gauge.valid&NP_BIT(NP_PRESENT))||!gauge.present||now<gauge.end_ms||
       now-gauge.end_ms>NP_GAUGE_TTL_MS)out->valid=0;
    IOLockUnlock(snapshotLock);return 1;
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
extern "C" int np_power_tsens_copy(NpTsensThermal *out) {
    if(!out||!snapshotLock||!IOLockTryLock(snapshotLock))return 0;
    *out=tsensThermal;
    const uint64_t now=io.now_ms(io.context);
    if(out->enabled&&(__atomic_load_n(&lifecycle,__ATOMIC_ACQUIRE)!=1||
       now<out->end_ms||now-out->end_ms>30000))invalidate_tsens(*out,-ESTALE);
    IOLockUnlock(snapshotLock);return 1;
}
extern "C" int np_power_connector_copy(NpConnectorThermal *out) {
    if(!out||!snapshotLock||!IOLockTryLock(snapshotLock))return 0;
    *out=connectorThermal;
    const uint64_t now=io.now_ms(io.context);
    if(out->enabled&&(__atomic_load_n(&lifecycle,__ATOMIC_ACQUIRE)!=1||
       now<out->end_ms||now-out->end_ms>NP_CONNECTOR_TTL_MS))invalidate_connector(*out,-ESTALE);
    IOLockUnlock(snapshotLock);return 1;
}
extern "C" int np_power_board_thermal_copy(NpBoardThermal *out) {
    if(!out||!snapshotLock||!IOLockTryLock(snapshotLock))return 0;
    *out=boardThermal;
    const uint64_t now=io.now_ms(io.context);
    if(out->enabled&&(__atomic_load_n(&lifecycle,__ATOMIC_ACQUIRE)!=1||
       now<out->end_ms||now-out->end_ms>NP_CONNECTOR_TTL_MS))invalidate_board(*out,-ESTALE);
    IOLockUnlock(snapshotLock);return 1;
}
extern "C" int np_power_control_request(unsigned mode) {
    if(io.control_mode<2||mode>1||!controlLock)return -1;
    NpPowerLogStatus log={};
    if(mode){if(!persistentLog)return -1;if(!np_power_log_status(&log))return 0;if(!usable_log(log))return -1;}
    if(!IOLockTryLock(controlLock))return 0;
    if(mode&&(control.buck.pending||control.buck.owned||control.buck.state==NP_BUCK_UNCERTAIN)){
        IOLockUnlock(controlLock);return 0;
    }
    manualDisable=mode?0:1;
    if(mode){control.last_armed_attachment=0;control.stopping=0;}
    else{control.allow_control=0;np_buck_stop(&control.buck);}
    IOLockUnlock(controlLock);return 1;
}
