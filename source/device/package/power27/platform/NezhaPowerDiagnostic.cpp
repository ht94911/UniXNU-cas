// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <sys/systm.h>
#include <IOKit/IOLib.h>
#include <stdarg.h>
#include <string.h>
#include "NezhaPowerRuntime.h"
#include "NezhaPowerLog.h"
#include "NezhaPowerThermal.h"
extern "C" {
#include "QcomUsbDiagnostic.h"
}
namespace {
struct Output {
    char *data;size_t capacity,used;bool truncated;
    void put(const char *format,...) {
        if(truncated||used>=capacity){truncated=true;return;}
        va_list args;va_start(args,format);int n=vsnprintf(data+used,capacity-used,format,args);va_end(args);
        if(n<0||(size_t)n>=capacity-used){truncated=true;return;}used+=(size_t)n;
    }
};
const char *boolean(unsigned value) { return value?"true":"false"; }
void nullable_number(Output &out,unsigned valid,int64_t value) {
    if(valid)out.put("%lld",(long long)value);else out.put("null");
}
void connector_metadata(Output &out,const NpMeta &meta,unsigned fresh) {
    out.put("{\"value\":%u,\"valid\":%s,\"error\":%d,\"at_ms\":%llu,\"fresh\":%s}",
        meta.value,boolean(meta.valid),meta.error,(unsigned long long)meta.at_ms,boolean(fresh));
}
void connector_policy(Output &out,const NpConnectorPolicyStatus &policy) {
    const auto &s=policy.signals;const auto &v=policy.vote;const auto &c=v.cohort;const auto &j=policy.jeita;
    out.put("{\"type\":\"connector_policy\",\"enabled\":%s,\"epoch\":%llu,\"attachment\":%llu,\"now_ms\":%llu,\"signals\":{\"usb_online\":[%d,%s,%llu],\"cid\":",
        boolean(policy.enabled),(unsigned long long)policy.epoch,(unsigned long long)policy.attachment,
        (unsigned long long)policy.now_ms,s.usb_online.value,boolean(s.usb_online.valid),
        (unsigned long long)policy.source_online_at_ms);
    connector_metadata(out,policy.cid,s.cid.valid);
    out.put(",\"otg_plugin\":");connector_metadata(out,policy.otg_plugin,s.otg_plugin.valid);
    out.put(",\"board\":{\"valid\":false,\"c\":null}},\"vote\":{\"charge_allowed\":%s,\"decision_complete\":%s,\"latch\":%s,\"cooldown_wait\":%s,\"reason\":%u,\"cooldown_reason\":%u,\"error\":%d,\"last_error\":%d,\"now_ms\":%llu,\"temperature_valid_mask\":%u,\"temperature_c\":[",
        boolean(v.charge_allowed),boolean(v.decision_complete),boolean(v.latch),boolean(v.cooldown_wait),
        v.reason,v.cooldown_reason,v.error,v.last_error,(unsigned long long)v.now_ms,v.temperature_valid_mask);
    nullable_number(out,v.temperature_valid_mask&1,v.temperature_c[0]);out.put(",");
    nullable_number(out,v.temperature_valid_mask&2,v.temperature_c[1]);
    out.put("],\"effective_c\":");nullable_number(out,v.temperature_valid_mask==3,v.effective_temperature_c);
    out.put(",\"rate_valid_mask\":%u,\"rate_initialized_mask\":%u,\"rate_interval_ms\":%llu,\"rates_c_per_s\":[",
        v.rate_valid_mask,v.rate_initialized_mask,(unsigned long long)v.rate_interval_ms);
    nullable_number(out,(v.rate_valid_mask|v.rate_initialized_mask)&1,v.rate_c_per_s[0]);out.put(",");
    nullable_number(out,(v.rate_valid_mask|v.rate_initialized_mask)&2,v.rate_c_per_s[1]);
    out.put("],\"selected\":{\"index\":");nullable_number(out,v.temperature_valid_mask==3,v.selected_index);
    out.put(",\"rate\":");nullable_number(out,v.selected_rate_valid||v.selected_rate_initialized,v.selected_rate_c_per_s);
    out.put(",\"valid\":%s,\"initialized\":%s},\"cohort\":{\"generation\":%llu,\"begin_ms\":%llu,\"end_ms\":%llu,\"attempted\":%u,\"valid_mask\":%u,\"error\":%d,\"retained\":%u,\"rows\":[",
        boolean(v.selected_rate_valid),boolean(v.selected_rate_initialized),
        (unsigned long long)c.generation,(unsigned long long)c.begin_ms,(unsigned long long)c.end_ms,
        c.attempted,c.valid_mask,c.error,c.retained);
    for(unsigned channel=0;channel<2;++channel) {
        const auto &r=c.reading[channel];
        out.put("%s[%u,%llu,%u,",channel?",":"",channel,(unsigned long long)c.sampled_ms[channel],(unsigned)r.raw);
        nullable_number(out,c.valid_mask&(1u<<channel),r.c1024);out.put(",");
        nullable_number(out,c.valid_mask&(1u<<channel),r.centi_c);
        out.put(",%d,%u,%u]",r.error,r.converted,r.quiescent);
    }
    out.put("]}},\"jeita\":[%u,%u,%u,%d,%d,%d,%d,%d,%d,%u,%u],\"effective_policy\":{\"valid\":%s,\"enabled\":%s}}",
        j.valid,j.full_valid,j.enabled,j.temperature_c,j.row,j.raw_soc,j.fcc_ma,j.fv_mv,j.iterm_ma+30,
        j.full,j.recharging,boolean(policy.effective_policy_valid),boolean(policy.effective_policy_enabled));
}
int snapshot(void *,char *data,size_t cap,size_t *used,unsigned *trunc) {
    NpPowerControl c={};NpPowerLogStatus log={};NpAuthStatus a={};NpGaugeIdentity identity={};
    if(!np_power_control_copy(&c)||!np_power_auth_copy(&a)||!np_power_log_status(&log))return QUD_BUSY;
    if(a.epoch&&(a.epoch!=c.epoch||a.attachment!=c.attachment))return QUD_BUSY;
    if(!np_power_identity_copy(&identity))return QUD_BUSY;
    NpPackThermal thermal={};if(!np_power_thermal_copy(&thermal))return QUD_BUSY;
    if(thermal.valid_raw&&(thermal.epoch!=c.sample.charger.epoch||thermal.generation!=c.sample.generation))return QUD_BUSY;
    NpConnectorPolicyStatus connector={};
    NpBuckFacts connectorFacts={};
    if(c.connector_policy_enabled) {
        if(!np_power_connector_policy_copy(&connector))return QUD_BUSY;
        if(!connector.enabled||connector.epoch!=c.epoch||connector.attachment!=c.attachment)return QUD_BUSY;
        // Use the current clock to qualify the first Control copy. JEITA,
        // thermal vote and actual readbacks must all come from that same copy.
        np_control_facts(&c,connector.now_ms,&connectorFacts);
    }
    Output out={data,cap,0,false};
    out.put("{\"mode\":%u,\"lifecycle\":%d,\"epoch\":%llu,\"attachment\":%llu,\"ms\":%llu,\"soc\":%d,\"mv\":%d,\"ma\":%d,\"temp\":%d,\"valid\":%u,\"external\":%d,\"charging\":%d,\"vbus_uv\":%d,\"buck_ua\":%d,\"current_scope\":%u,",
        c.mode,nezha_power27_lifecycle(),(unsigned long long)c.epoch,(unsigned long long)c.attachment,(unsigned long long)c.sample.at_ms,
        c.sample.gauge.soc,c.sample.gauge.voltage_mv,c.sample.gauge.current_ma,c.sample.gauge.temperature_centi_c,
        c.sample.valid,c.sample.external,c.sample.charging,c.sample.charger.usb_uv,c.sample.charger.usb_ua,c.sample.charger.input_current_scope);
    out.put("\"pump\":{\"error\":%u,\"chip\":%u,\"valid\":%u,\"on\":%u,\"switching\":%u,\"mode\":%u,\"mv\":%d,\"ma\":%d,\"die\":%d},",
        c.pump_error,c.pump.chip,c.pump.valid,c.pump.enable,c.pump.switching,c.pump.mode,c.pump.bus_mv,c.pump.bus_ma,c.pump.die_centi_c);
    out.put("\"control\":{\"state\":%u,\"blocked\":%u,\"owned\":%u,\"pending\":%u,\"changes\":%u,\"failures\":%u,\"backend\":%u,\"confirmation\":%u,\"requested_fcc\":%u,\"feedback_base_ma\":%d,\"feedback_target_ma\":%d,\"requires_register_readback\":%s},",
        c.buck.state,c.buck.blocked,c.buck.owned,c.buck.pending,c.buck.changes,c.buck.failures,
        c.buck.backend,c.buck.confirmation,c.buck.target_fcc,c.buck.feedback_baseline_ma,c.buck.feedback_target_ma,
        c.buck.backend==NP_BUCK_READBACK?"true":"false");
    if(c.mode==5){
        out.put("\"icl_vote\":{\"readback_scope\":\"firmware_min_aggregate\",\"requested_ma\":");
        if(c.buck.stock.target[0])out.put("%u",c.buck.stock.target[0]);else out.put("null");
        out.put(",\"withdrawal_supported\":false,\"hardware_limit_readback\":false},");
    }
    out.put("\"fixed_pd\":{\"confirmed_mv\":%u,\"ovp_baseline_raw\":%u,\"ovp_actual_raw\":%u,\"ovp_restored\":%u,\"protection_requires_readback\":%s},\"meta\":[",
        c.buck.confirmed_vbus_mv,c.buck.usb_ovp_baseline,c.pump.control[2]&15,c.buck.protection_restored,
        c.buck.backend==NP_BUCK_OS4_FIXED_PD?"true":"false");
    for(unsigned i=0;i<NP_META_SNAPSHOT_COUNT;++i)out.put("%s[%u,%u,%d,%llu]",i?",":"",c.meta[i].value,c.meta[i].valid,c.meta[i].error,(unsigned long long)c.meta[i].at_ms);
    out.put("],\"pdo_valid\":%u,\"pdos\":[",c.caps.valid);
    for(unsigned i=0;i<NP_PD_ENTRIES;++i)out.put("%s[%u,%u,%u,%u]",i?",":"",c.caps.entry[i].min_mv,c.caps.entry[i].max_mv,c.caps.entry[i].max_ma,c.caps.entry[i].type);
    out.put("],\"auth\":{\"enabled\":%u,\"state\":%u,\"error\":%u,\"owned\":%u,\"attempts\":%u,\"svid\":%u,\"adapter_id\":%u,\"primary_gauge_verified\":%u,\"adapter_verified\":%u,\"verified_ms\":%llu,\"primary_checks\":%u,\"primary_error\":%u,\"discovery_until_ms\":%llu},",
        a.enabled,a.state,a.error,a.owned,a.attempts,a.svid,a.adapter_id,
        a.primary_gauge_verified,a.adapter_verified,(unsigned long long)a.verified_ms,
        a.primary_checks,a.primary_error,(unsigned long long)a.discovery_until_ms);
    out.put("\"gauge_id\":[%u,%u,%u,%u],",identity.valid,identity.error,identity.cell_code,
      identity.valid==3&&!strcmp(identity.device_name,"BM6N"));
    if(c.mode==5){const auto &j=c.jeita.vote;
        out.put("\"policy\":[%u,%u,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,null,%u],\"iterm\":{\"firmware_managed\":true,\"readback_supported\":false},",
          c.country_cn,j.valid&&j.full_valid,j.temperature_c,j.row,j.raw_soc,j.fcc_ma,j.fv_mv,
          j.iterm_ma+30,c.connector_policy_enabled?connectorFacts.policy_enabled:j.enabled,j.full,j.recharging,c.buck.stock.actual[1],
          c.buck.stock.actual[3]);
    }
    if(thermal.enabled)out.put("\"thermal\":{\"pack_valid_raw\":%u,\"pack_error\":%d,\"pack_raw\":%d,\"epoch\":%llu,\"generation\":%llu,\"sampled_ms\":%llu,\"units_confirmed\":true,\"units\":\"degC\"},",
      thermal.valid_raw,thermal.error,thermal.pack_tbat_raw,(unsigned long long)thermal.epoch,
      (unsigned long long)thermal.generation,(unsigned long long)thermal.sampled_ms);
    out.put("\"log\":{\"phase\":%u,\"error\":%u,\"lines\":%u,\"lost\":%u,\"bytes\":%u,\"capped\":%u,\"path\":\"%s\"}}",
      log.phase,log.error,log.written,log.dropped,log.bytes,log.capped,log.path);
    *used=out.used;*trunc=out.truncated;return out.truncated?QUD_FAILED:QUD_OK;
}
int submit(void *,const qud_diag_request *request,char *data,size_t cap,size_t *used,unsigned *trunc) {
    Output out={data,cap,0,false};
    if(request->op==QUD_HISTORY&&request->arg1==1) {
        if(request->arg0>=NP_TSENS_CONTROLLER_COUNT||request->cursor)return QUD_UNSUPPORTED;
        NpTsensThermal thermal={};if(!np_power_tsens_copy(&thermal))return QUD_BUSY;
        if(!thermal.enabled)return QUD_UNSUPPORTED;
        const unsigned group=request->arg0,count=np_tsens_controllers[group].sensors;
        unsigned first=0;for(unsigned c=0;c<group;++c)first+=np_tsens_controllers[c].sensors;
        out.put("{\"type\":\"tsens\",\"controller\":%u,\"generation\":%llu,\"begin_ms\":%llu,\"end_ms\":%llu,\"error\":%d,\"units\":\"deciC\",\"last_status_words\":true,\"thermal_vote\":false,\"rows\":[",
            group,(unsigned long long)thermal.generation,(unsigned long long)thermal.begin_ms,
            (unsigned long long)thermal.end_ms,thermal.error);
        for(unsigned s=0;s<count;++s) {
            const auto &r=thermal.sample.readings[first+s];
            out.put("%s[%u,%u,",s?",":"",s,r.raw);
            if(r.valid)out.put("%d",r.deci_c);else out.put("null");
            out.put(",%d,%u,%u]",r.io_error,r.read_ok,r.valid);
        }
        out.put("]}");
    } else if(request->op==QUD_HISTORY&&request->arg1==2) {
        if(request->arg0||request->cursor)return QUD_UNSUPPORTED;
        NpConnectorThermal thermal={};if(!np_power_connector_copy(&thermal))return QUD_BUSY;
        if(!thermal.enabled)return QUD_UNSUPPORTED;
        NpConnectorPolicyStatus policy={};if(!np_power_connector_policy_copy(&policy))return QUD_BUSY;
        out.put("{\"type\":\"connectors\",\"generation\":%llu,\"begin_ms\":%llu,\"end_ms\":%llu,\"error\":%d,\"attempted\":%u,\"valid_mask\":%u,\"retained\":%u,\"units\":\"centiC\",\"sequential\":true,\"thermal_vote\":%s,\"rows\":[",
            (unsigned long long)thermal.generation,(unsigned long long)thermal.begin_ms,
            (unsigned long long)thermal.end_ms,thermal.error,thermal.attempted,thermal.valid_mask,thermal.retained,boolean(policy.enabled));
        for(unsigned channel=0;channel<2;++channel) {
            const auto &r=thermal.reading[channel];
            out.put("%s[%u,%llu,%u,",channel?",":"",channel,
                (unsigned long long)thermal.sampled_ms[channel],(unsigned)r.raw);
            if(thermal.valid_mask&(1u<<channel))out.put("%d,%d",r.c1024,r.centi_c);
            else out.put("null,null");
            out.put(",%d,%u,%u]",r.error,r.converted,r.quiescent);
        }
        out.put("]}");
    } else if(request->op==QUD_HISTORY&&request->arg1==3) {
        if(request->arg0||request->cursor)return QUD_UNSUPPORTED;
        NpConnectorPolicyStatus policy={};if(!np_power_connector_policy_copy(&policy))return QUD_BUSY;
        if(!policy.enabled)return QUD_UNSUPPORTED;
        connector_policy(out,policy);
    } else if(request->op==QUD_HISTORY&&!request->arg1) {
        NpPowerHistoryRecord rows[8];unsigned count=0;uint64_t next=0,oldest=0;
        unsigned limit=request->arg0?request->arg0:4;
        int result=np_power_history_copy(request->cursor,limit,rows,&count,&next,&oldest);
        if(!result)return QUD_BUSY;
        if(result<0)return QUD_EXPIRED;
        out.put("{\"oldest\":%llu,\"next\":%llu,\"rows\":[",(unsigned long long)oldest,(unsigned long long)next);
        for(unsigned i=0;i<count;++i){const auto &v=rows[i];out.put("%s[%llu,%llu,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%u]",
            i?",":"",(unsigned long long)v.id,(unsigned long long)v.at_ms,v.soc,v.battery_mv,v.battery_ma,v.temperature,
            v.external,v.charging,v.vbus_uv,v.buck_ua,v.real_type,v.icl_ma,v.fcc_ma,v.state,v.blocked,v.pump_enable,v.pump_ibus_ma);}
        out.put("]}");
    } else if(request->op==QUD_CONFIG&&request->arg0<=3&&!request->arg1) {
        int result=request->arg0<2?np_power_control_request(request->arg0):
            np_power_lifecycle_request(request->arg0-2);
        if(result<=0)return result?QUD_FAILED:QUD_BUSY;
        if(request->arg0<2)
            out.put("{\"queued\":true,\"normal_control\":%u,\"effect_confirmed\":false}",request->arg0);
        else out.put("{\"queued\":true,\"power_state\":%u,\"effect_confirmed\":false}",request->arg0-2);
    } else return QUD_UNSUPPORTED;
    *used=out.used;*trunc=out.truncated;return out.truncated?QUD_FAILED:QUD_OK;
}
const qud_diag_provider provider={9,1,(1u<<QUD_SNAPSHOT)|(1u<<QUD_HISTORY)|(1u<<QUD_CONFIG),8,3,"nezhapower",nullptr,snapshot,submit};
}
extern "C" void np_power_diagnostic_start(void) {
    int registered=qcom_usb_diag_register(&provider);
    IOLog("NEZHA POWER DIAG: provider9 registered=%d; CPU snapshots/history, bounded control and worker suspend/resume\n",registered);
}
