/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaPowerControl.h"
#include "NezhaMca.h"
#include <sys/errno.h>
#include <string.h>
static const uint32_t properties[NP_META_COUNT]={0x2008,0x2005,0x2007,0x2003,
 0x200d,0x200e,0x201b,0x2000c,0x21002,0x21014,0x21018,0x2101c,0x2100d,0x2100e,
 0x21022,0x21023};
static unsigned length(unsigned i)
{return i==NP_META_PDOS?NP_PD_BYTES:
  i==NP_META_ENABLED||i==NP_META_CID||i==NP_META_OTG_PLUGIN?1:4;}
static unsigned metadata_count(const NpPowerControl *c)
{return c->connector_policy_enabled?NP_META_COUNT:NP_META_SNAPSHOT_COUNT;}
unsigned np_control_refresh_due(const NpPowerControl *c,uint64_t now)
{
 const int startup=c&&c->stock_continuous&&c->buck.state==NP_BUCK_PUMP_OBSERVE_PPS;
 if(!c||c->mode!=9||!c->buck.stock_current_enabled||c->stopping||c->buck.stop||
    (!(c->buck.owned&64u)&&!startup)||(c->buck.state!=NP_BUCK_PUMP_OBSERVE_START&&
    c->buck.state!=NP_BUCK_PUMP_RUN&&c->buck.state!=NP_BUCK_PUMP_REGULATE&&
    c->buck.state!=NP_BUCK_PUMP_OBSERVE_REGULATION&&!startup))return NP_META_COUNT;
 const uint64_t gauge=c->sample.gauge.end_ms,charger=c->sample.charger.end_ms;
 if(!gauge||!charger||now<gauge||now<charger||now-gauge>=2000||now-charger>=2000)return NP_META_COUNT;
 /* Running voltage confirmation already requires a catalog newer than its
  * ACK. Fetch it before the first cohort instead of discarding that cohort
  * and waiting for an ordinary round-robin catalog refresh. */
 if((c->buck.state==NP_BUCK_PUMP_OBSERVE_REGULATION||startup)&&c->buck.voltage_after_ms&&
    c->caps.at_ms<=c->buck.voltage_after_ms&&now>=c->meta[NP_META_PDOS].retry_ms)
  return NP_META_PDOS;
 const unsigned slots=metadata_count(c);unsigned oldest=NP_META_COUNT;
 for(unsigned i=0;i<slots;++i){
  const int required=(i<=NP_META_VERIFY&&i!=NP_META_FCC&&i!=NP_META_SUSPEND)||
    i==NP_META_PDOS||i==NP_META_DATA_ROLE||i==NP_META_PD_STATE||
    (c->connector_policy_enabled&&(i==NP_META_CID||i==NP_META_OTG_PLUGIN));
  const NpMeta *m=&c->meta[i];
  /* Leave the ordinary1s reads on their existing round robin. Only an
   * entry approaching the actual3s freshness gate preempts a fast cohort. */
  if(!required||now<m->retry_ms||(m->valid&&now>=m->at_ms&&now-m->at_ms<2000))continue;
  if(oldest==NP_META_COUNT||m->at_ms<c->meta[oldest].at_ms)oldest=i;
 }
 return oldest;
}
int np_control_refresh_next(NpPowerControl *c,uint64_t now,NpAuxCommand *out)
{
 if(!c||!out||!c->epoch||c->pending)return 0;
 const unsigned i=np_control_refresh_due(c,now);
 if(i==NP_META_COUNT)return 0;
 if(c->next_id==UINT64_MAX)return -1;
 *out=(NpAuxCommand){++c->next_id,c->attachment,properties[i],0,(uint16_t)length(i),0,0,(uint8_t)i};
 c->pending_command=*out;c->pending=1;++c->queries;return 1;
}
void np_control_init(NpPowerControl *c,unsigned mode)
{memset(c,0,sizeof(*c));c->mode=mode;c->attachment=1;c->stock_support100w=-1;np_buck_init(&c->buck);
 if(mode==3)c->buck.backend=NP_BUCK_OS4_SETTER;
 if(mode==4)c->buck.backend=NP_BUCK_OS4_FIXED_PD;
 if(mode==5)c->buck.backend=NP_BUCK_OS4_STOCK_POLICY;
 if(mode==6)c->buck.backend=NP_BUCK_OS4_PPS_OFF_TEST;
 if(mode==7)c->buck.backend=NP_BUCK_OS4_PPS_PUMP_TEST;
 if(mode==8)c->buck.backend=NP_BUCK_OS4_PPS_CCCV_TEST;
 if(mode==9)c->buck.backend=NP_BUCK_OS4_PPS_ADAPTER_TEST;
 np_jeita_init(&c->jeita);np_connector_antiburn_init(&c->connector_antiburn);}
void np_control_connector_policy_enable(NpPowerControl *c,int enabled)
{if(c)c->connector_policy_enabled=(c->mode==5||c->mode==6||c->mode==7||(c->mode==8||c->mode==9))&&enabled;}
static int signal_fresh(const NpPowerControl *c,const NpMeta *m,uint64_t now)
{return c->epoch&&m->valid&&!m->error&&now>=m->at_ms&&now-m->at_ms<=NP_CONNECTOR_TTL_MS;}
void np_control_connector_signals(const NpPowerControl *c,uint64_t now,NpConnectorAntiburnSignals *s)
{
 if(!s)return;
 memset(s,0,sizeof(*s));if(!c)return;
 s->usb_online.value=(int32_t)c->source_online;
 s->usb_online.valid=c->epoch&&c->source_online_valid&&now>=c->source_online_at_ms&&
   now-c->source_online_at_ms<=NP_CONNECTOR_TTL_MS;
 s->cid.value=(int32_t)c->meta[NP_META_CID].value;
 s->cid.valid=signal_fresh(c,&c->meta[NP_META_CID],now);
 s->otg_plugin.value=(int32_t)c->meta[NP_META_OTG_PLUGIN].value;
 s->otg_plugin.valid=signal_fresh(c,&c->meta[NP_META_OTG_PLUGIN],now);
 /* Board0x3c is a Linux local thermal notifier, not a proven MCA packet.
  * Leave its validity false; zero is never a substituted temperature. */
}
static void connector_update(NpPowerControl *c,uint64_t now)
{
 if(!c->connector_policy_enabled)return;
 NpConnectorAntiburnSignals signals;np_control_connector_signals(c,now,&signals);
 (void)np_connector_antiburn_step(&c->connector_antiburn,&c->connector_sample,&signals,now);
}
void np_control_connector_sample(NpPowerControl *c,const NpConnectorThermal *sample,uint64_t now)
{if(c&&sample){c->connector_sample=*sample;connector_update(c,now);}}
void np_control_connector_invalidate(NpPowerControl *c,int error)
{
 if(!c||!c->connector_policy_enabled)return;
 c->connector_sample.valid_mask=0;c->connector_sample.error=error;
 for(unsigned i=0;i<2;++i){
  c->connector_sample.reading[i].converted=0;
  c->connector_sample.reading[i].c1024=c->connector_sample.reading[i].centi_c=INT32_MIN;
 }
 c->source_online_valid=0;c->source_online_at_ms=0;
 for(unsigned i=NP_META_CID;i<=NP_META_OTG_PLUGIN;++i){
  c->meta[i].valid=0;c->meta[i].error=error;c->meta[i].retry_ms=0;
 }
 np_connector_antiburn_invalidate(&c->connector_antiburn,error);
}
void np_control_connector_vote(const NpPowerControl *c,uint64_t now,NpConnectorAntiburnVote *v)
{
 if(!c||!v)return;
 NpConnectorAntiburn copy=c->connector_antiburn;
 if(c->connector_policy_enabled){
  NpConnectorAntiburnSignals signals;np_control_connector_signals(c,now,&signals);
  (void)np_connector_antiburn_step(&copy,&c->connector_sample,&signals,now);
 }
 (void)np_connector_antiburn_copy(&copy,v);
}
void np_control_profile(NpPowerControl *c,const NpGaugeIdentity *id,int country_cn)
{if(c){if(id)c->identity=*id;else memset(&c->identity,0,sizeof(c->identity));c->country_cn=country_cn==1;}}
void np_control_adapter_support(NpPowerControl *c,int value)
{if(c)c->stock_support100w=value==0||value==1?value:-1;}
int np_control_adapter_verified(NpPowerControl *c,uint64_t epoch,uint64_t attachment,int verified)
{
 if(!c||!epoch||epoch!=c->epoch||attachment!=c->attachment||verified<0||verified>1)return -1;
 c->adapter_verified=(uint8_t)verified;return 0;
}
uint32_t np_control_charge_type(const NpPowerControl *c,uint64_t now)
{
 if(!c)return 0;
 const NpMeta *m=&c->meta[NP_META_TYPE];
 /* Actual factory3b4..3c8: raw11 + acknowledged cached report ->12.
  * Authentication and source lifetime own the report; this lookup neither
  * changes raw metadata nor renews its original3s timestamp. */
 return c->mode==9&&c->buck.stock_current_enabled&&c->adapter_verified&&c->epoch&&
   m->value==11&&m->valid&&!m->error&&now>=m->at_ms&&now-m->at_ms<=3000?12:m->value;
}
void np_control_charger_thermal_enable(NpPowerControl *c,int enabled)
{
 if(!c)return;
 c->charger_thermal_enabled=c->mode==9&&enabled;
 memset(&c->charger_thermal,0,sizeof(c->charger_thermal));c->charger_thermal_begin_ms=0;
 c->charger_thermal_initial_ma=c->charger_thermal_device_ma=c->charger_thermal_request_ma=0;
 c->charger_thermal_initial_type=0;
}
static void thermal_source_invalidate(NpPowerControl *c)
{
 /* USB disconnect clears kernel current votes, not thermald's sensor
  * temperature/history and hysteresis flags. A new source must still
  * publish its own complete current cohort before that history can act. */
 if(c->stock_continuous)c->charger_thermal.valid=0;
 else memset(&c->charger_thermal,0,sizeof(c->charger_thermal));
 c->charger_thermal_begin_ms=0;
 c->charger_thermal_initial_ma=c->charger_thermal_device_ma=c->charger_thermal_request_ma=0;
 c->charger_thermal_initial_type=0;
}
static void thermal_source_power(NpPowerControl *c,uint64_t now)
{
 if(!c->stock_continuous||!c->charger_thermal_enabled)return;
 const uint32_t type=np_control_charge_type(c,now);
 const NpMeta *source=&c->meta[NP_META_TYPE];
 if((type!=11&&type!=12)||type==c->charger_thermal_initial_type||
    !source->valid||source->error||now<source->at_ms||now-source->at_ms>3000||
    !c->caps.valid||c->caps.invalid_mask||c->caps.epoch!=c->epoch||
    now<c->caps.at_ms||now-c->caps.at_ms>3000)return;
 uint32_t power=0;
 for(unsigned i=0;i<NP_PD_ENTRIES;++i){
  const NpPdEntry *p=&c->caps.entry[i];
  if(!(c->caps.present_mask&(1u<<i))||p->type!=NP_PD_APDO||
     p->min_mv==p->max_mv||(type==11&&c->stock_support100w==0&&p->max_mv>=11001))continue;
  const uint32_t watts=(uint32_t)((uint64_t)p->max_mv*p->max_ma/1000000u);
  if(watts>power)power=watts;
 }
 if(!power)return;
 /* Fixed N0 device_max_power90. Its original power classifier selects
  * one of the same33/67/90 bands after advertised APDO evaluation. */
 if(power>90)power=90;
 /* Actual N0 PD class returns raw power for type11; type12 maps the
  * intermediate67..89W band to67W before the thermal init table. */
 c->charger_thermal_initial_ma=power<=33?6600:
  (type==12?power<90:power<=67)?12400:15600;
 c->charger_thermal_initial_type=(uint8_t)type;
 const uint32_t sic=c->charger_thermal_request_ma;
 c->charger_thermal_device_ma=!sic||sic>c->charger_thermal_initial_ma?
  c->charger_thermal_initial_ma:sic;
}
int np_control_charger_thermal_sample(NpPowerControl *c,const NpBoardThermal *s,uint64_t now)
{
 if(!c||!c->charger_thermal_enabled)return EINVAL;
 NpChargerThermalCurrent *t=&c->charger_thermal;
 if(!s||s->epoch!=c->epoch||s->attachment!=c->attachment){t->valid=0;return ESTALE;}
 /* Repeated consumers cannot integrate the same completed board sample.
  * The previous value is this software thermal vote, not MCA FCC/current. */
 if(t->initialized&&t->epoch==s->epoch&&t->attachment==s->attachment){
  if(s->generation<t->generation){t->valid=0;return ESTALE;}
  if(s->generation==t->generation&&s->derived_valid&&s->valid_mask==NP_BOARD_INPUT_MASK&&!s->retained)return EAGAIN;
 }
 /* wired_chg_curr stores the accepted request in integer mA and returns
  * that value multiplied by1000. Use that same readback coordinate in the
  * next recurrence instead of accumulating discarded sub-mA fractions. */
 thermal_source_power(c,now);
 const int32_t previous=c->stock_continuous?
   (int32_t)c->charger_thermal_device_ma*1000:
   t->initialized?(t->current_ua/1000)*1000:0;
 NpChargerThermalCurrent next=*t;
 if(c->stock_continuous&&next.initialized&&
    (next.epoch!=s->epoch||next.attachment!=s->attachment)){
  next.epoch=s->epoch;next.attachment=s->attachment;
  next.generation=next.sampled_ms=0;next.valid=0;
 }
 int error=np_charger_thermal_current(&next,s,now,previous);
 if(!error){
  *t=next;c->charger_thermal_begin_ms=s->begin_ms;
  if(c->stock_continuous){
   c->charger_thermal_request_ma=c->charger_thermal_device_ma=(uint32_t)t->current_ua/1000u;
   if(c->charger_thermal_initial_ma&&
      c->charger_thermal_device_ma>c->charger_thermal_initial_ma)
    c->charger_thermal_device_ma=c->charger_thermal_initial_ma;
  }
 }else t->valid=0;
 return error;
}
static void update_profiles(NpPowerControl *c)
{
 const uint64_t previous_curve_ms=c->buck.cccv.sampled_ms;
 if(c->mode==8||c->mode==9){
  const int paused_start=c->buck.input_suspend_enabled&&(c->buck.owned&128u)&&
      (c->buck.state==NP_BUCK_PUMP_OBSERVE_INPUT_SUSPEND||
       c->buck.state==NP_BUCK_PUMP_SET_MODE||c->buck.state==NP_BUCK_PUMP_SET_WATCHDOG||
       c->buck.state==NP_BUCK_PUMP_REQUEST_PPS||c->buck.state==NP_BUCK_PUMP_OBSERVE_PPS||
       c->buck.state==NP_BUCK_PUMP_START||c->buck.state==NP_BUCK_PUMP_OBSERVE_START);
  const int low_current_start=c->stock_continuous&&c->adapter_verified&&
      c->allow_control&&!c->stopping&&!c->buck.stop&&!c->buck.pending&&
      !c->buck.unverified_effect&&!c->buck.failures&&!c->buck.cleanup_failed&&
      c->buck.backend==NP_BUCK_OS4_STOCK_POLICY&&
      c->buck.state==NP_BUCK_POLICY_SYNC&&(c->buck.owned&3u)==3u&&
      c->buck.target_fcc==500&&c->buck.stock.actual[3]==1&&
      (c->buck.confirmation==NP_BUCK_CONFIRM_REDUCTION_ACK||
       c->buck.confirmation==NP_BUCK_CONFIRM_CURRENT)&&
      c->buck.epoch==c->epoch&&c->buck.attachment==c->attachment&&
      !c->pump_error&&c->pump.controls_valid&&c->pump.switching_valid&&
      !c->pump.enable&&!c->pump.switching;
  /* Original stage selection can initialize before input suspend while
   * the accepted low500mA main vote leaves net battery current negative.
   * Use the existing startup-profile operation only on that confirmed
   * source; running CC/CV still consumes the measured current normally. */
  if(paused_start||low_current_start)(void)np_cccv_prepare_input_suspended(&c->buck.cccv,&c->sample,
      &c->identity,(int)c->country_cn,c->epoch,c->attachment,c->ffc_enabled?1:0);
  else (void)np_cccv_update_profile(&c->buck.cccv,&c->sample,
      &c->identity,(int)c->country_cn,c->epoch,c->attachment,c->ffc_enabled?1:0);
 }
 if(c->mode==5||c->mode==6||c->mode==7||c->mode==8||c->mode==9){
  (void)np_jeita_update_profile(&c->jeita,&c->sample,&c->identity,(int)c->country_cn,
   c->ffc_enabled&&c->ffc_valid&&c->ffc_active);
  (void)np_jeita_full_update(&c->jeita,&c->sample);
  if(c->stock_continuous&&!c->stopping&&!c->pump_error&&
     c->pump.controls_valid&&c->pump.switching_valid&&c->pump.enable&&c->pump.switching&&
     !c->pump.protection.faults&&(c->buck.owned&64u)&&
     c->sample.gauge.end_ms>previous_curve_ms&&
     np_cccv_termination_update(&c->buck.cccv,&c->sample)){
   c->jeita.full_latched=1;c->jeita.recharging=0;
   c->jeita.vote.full=1;c->jeita.vote.recharging=0;c->jeita.vote.enabled=0;
  }
 }
}
void np_control_ffc_result(NpPowerControl *c,unsigned requested,const NpFastModeReading *r,int error)
{
 if(!c||!c->ffc_enabled||requested>1||!r)return;
 const int confirmed=!error&&r->valid&&r->enabled==requested;
 c->ffc_valid=(uint8_t)confirmed;c->ffc_active=confirmed?r->enabled:0;
 c->ffc_owned=r->owned||(r->attempted&&!confirmed);
 if(!confirmed){c->ffc_failed=1;if(!requested)c->ffc_cleanup_failed=1;np_buck_stop(&c->buck);}
 update_profiles(c);
}
int np_control_ffc_desired(const NpPowerControl *c,uint64_t now)
{
 if(!c||!c->ffc_enabled||!c->ffc_valid||c->ffc_failed||!c->allow_control||
    c->stopping||c->buck.stop||!(c->buck.owned&64)||
    c->buck.epoch!=c->epoch||c->buck.attachment!=c->attachment||now>=c->buck.lease_until)return 0;
 if(c->buck.input_suspend_enabled&&c->buck.state==NP_BUCK_PUMP_OBSERVE_START)return 0;
 const NpCccv *cc=&c->buck.cccv;
 if(!cc->valid||cc->profile!=1||cc->epoch!=c->epoch||cc->attachment!=c->attachment||
    now<cc->sampled_ms||now-cc->sampled_ms>3000)return 0;
 NpBuckFacts f;np_control_facts(c,now,&f);
 if(c->pump_error||c->pump.chip!=0x85||!c->pump.controls_valid||!c->pump.switching_valid||
  !c->pump.protection.valid||c->pump.protection.faults||!c->pump.enable||!c->pump.switching||
  c->pump.mode>1||now<c->pump_at||now-c->pump_at>3000)return 0;
 NpBuck copy=c->buck;copy.pending=0;NpBuckCommand command;
 (void)np_buck_pump_next(&copy,&f,&command);
 return copy.armed&&!copy.stop&&copy.blocked==NP_BUCK_READY;
}
void np_control_link(NpPowerControl *c,uint64_t epoch,int up)
{
 if(!up||c->epoch!=epoch){memset(c->meta,0,sizeof(c->meta));memset(&c->caps,0,sizeof(c->caps));np_buck_stop(&c->buck);c->adapter_verified=0;c->source_online_valid=0;c->source_online_at_ms=0;np_control_connector_invalidate(c,NP_AB_STALE_TEMPERATURE);thermal_source_invalidate(c);}
 c->epoch=up?epoch:0;
}
void np_control_source_event(NpPowerControl *c)
{
 if(c->attachment==UINT64_MAX){c->stopping=1;np_buck_stop(&c->buck);return;}
 ++c->attachment;c->adapter_verified=0;memset(c->meta,0,sizeof(c->meta));memset(&c->caps,0,sizeof(c->caps));np_buck_stop(&c->buck);c->source_online_at_ms=0;thermal_source_invalidate(c);
}
int np_control_usb_type(NpPowerControl *c,uint32_t online,uint32_t type,uint64_t now)
{
 if(!c||online>1||type>20)return -1;
 const NpMeta previous=c->meta[NP_META_TYPE];
 const int same_pd=((previous.value==10||previous.value==11)&&(type==10||type==11))||
   ((c->mode==6||c->mode==7||(c->mode==8||c->mode==9))&&previous.value>=10&&previous.value<=12&&type>=10&&type<=12);
 const int changed=!c->source_online_valid||c->source_online!=online||
   (online&&previous.valid&&previous.value!=type&&!same_pd);
 if(changed)np_control_source_event(c);
 else if(previous.value!=type) {
  /* Reclassifying the same PD adapter still needs a new capability catalog
   * before a voltage increase. It does not revoke the in-flight HMAC. */
  /* In the pump-off PPS test, normal PD->PPS classification follows the
   * accepted request. Keep the same-epoch catalog for observation; force a
   * refresh before the new contract can authorize the FCC increase. */
  if(c->mode!=6&&c->mode!=7&&c->mode!=8&&c->mode!=9)c->caps.valid=0;
  c->meta[NP_META_PDOS].valid=0;
 }
 c->source_online=online;c->source_online_valid=1;c->source_online_at_ms=now;
 c->meta[NP_META_TYPE]=(NpMeta){type,0,now,0,1};
 return changed;
}
void np_control_sample(NpPowerControl *c,const NpSnapshot *s,const NpPump *p,int error,uint64_t at)
{
 if(s->valid&NP_BIT(NP_EXTERNAL)) {
  if(c->source_online_valid&&(uint32_t)s->external!=c->source_online)np_control_source_event(c);
  c->source_online=(uint32_t)s->external;c->source_online_valid=1;c->source_online_at_ms=at;
 }
 c->sample=*s;c->pump=*p;c->pump_error=(uint32_t)error;c->pump_at=at;
 update_profiles(c);
 /* at belongs to the earlier CP sample. During MCA reads the connector
  * worker can publish a newer cohort. Reevaluate that policy at the current
  * completed snapshot time; keep the CP's original time for freshness. */
 connector_update(c,s->at_ms>at?s->at_ms:at);
}
static uint64_t earlier(uint64_t a,uint64_t b){return a<b?a:b;}
void np_control_facts(const NpPowerControl *c,uint64_t now,NpBuckFacts *f)
{
 memset(f,0,sizeof(*f));f->epoch=c->epoch;f->attachment=c->attachment;f->now_ms=now;
 const NpSnapshot *s=&c->sample;const NpMeta *m=c->meta;
 const uint32_t charge_type=np_control_charge_type(c,now);
 const int ordinary=c->stock_continuous&&(charge_type==1||charge_type==2||charge_type==3||
   charge_type==4||charge_type==9||charge_type==15);
 uint32_t need=NP_BIT(NP_PRESENT)|NP_BIT(NP_SOC)|NP_BIT(NP_VOLTAGE)|NP_BIT(NP_TEMPERATURE)|
   NP_BIT(NP_EXTERNAL)|NP_BIT(NP_CHARGING)|NP_BIT(NP_USB_VOLTAGE);
 if(c->mode>=3)need|=NP_BIT(NP_CURRENT);
 f->valid=(s->valid&need)==need;f->sampled_ms=earlier(s->gauge.end_ms,s->charger.end_ms);
 for(unsigned i=NP_META_ICL;i<=NP_META_VERIFY;++i){
  if(c->mode>=3&&i==NP_META_FCC)continue; /* setter backend has no supported getter */
  if(c->buck.input_suspend_enabled&&i==NP_META_SUSPEND)continue; /* OS4 has no getter */
  if(!m[i].valid)f->valid=0;
  f->sampled_ms=earlier(f->sampled_ms,m[i].at_ms);
 }
 f->online=(uint32_t)s->external;f->real_type=charge_type;
 f->boost_enabled=m[NP_META_BOOST].value;f->gate_enabled=m[NP_META_GATE].value;
 f->input_suspended=m[NP_META_SUSPEND].value;f->charge_enabled=m[NP_META_ENABLED].value;
 if(c->buck.input_suspend_enabled)f->input_suspended=0;
 f->term_mv=m[NP_META_TERM].value;f->icl_ma=m[NP_META_ICL].value;f->fcc_ma=m[NP_META_FCC].value;
 f->pmic_charging=(uint32_t)s->charging;f->vbus_mv=s->charger.usb_uv>0?(uint32_t)s->charger.usb_uv/1000:0;
 f->main_buck_state=s->charger.state_raw;f->main_buck_state_valid=s->charger.state_valid;
 f->battery_mv=s->gauge.voltage_mv;f->temperature_centi_c=s->gauge.temperature_centi_c;
 f->fault=s->faults|m[NP_META_VERIFY].value;
 if(c->adc_error)f->fault|=NP_FAULT_ADC;
 if(!c->pump_error&&c->pump.controls_valid&&c->pump.protection.valid&&
    now>=c->pump_at&&now-c->pump_at<=3000&&c->pump.protection.faults)
  f->fault|=NP_FAULT_PUMP;
 /* 068 observed raw PMIC7 with CPmode0 enable1/switching1 and1664mA
  * battery input. Valid noncharging buck states6/7 must not stop that pump.
  * Unknown/failed state, disabled buck, hardware fault and full votes retain
  * their existing exits. Full gaugeflag remains an actual completion vote. */
 if(c->mode==9&&!c->pump_error&&c->pump.chip==0x85&&c->pump.controls_valid&&c->pump.switching_valid&&
    c->pump.protection.valid&&!c->pump.protection.faults&&c->pump.enable&&c->pump.switching&&
    (c->pump.mode<=1)&&s->charger.state_valid&&
    (s->charger.state_raw==6||s->charger.state_raw==7)&&
    now>=c->pump_at&&now-c->pump_at<=3000)f->pmic_charging=1;
 f->full=s->fully_charged||s->gauge.full_flag||s->gauge.soc>=95;
 f->pump_known_off=!c->pump_error&&c->pump.controls_valid&&c->pump.chip==0x85&&
   c->pump.switching_valid&&!c->pump.switching&&!c->pump.enable&&c->pump.mode<=2&&
   now>=c->pump_at&&now-c->pump_at<=3000;
 f->pd=&c->caps;f->stock_support100w=c->stock_support100w;
 f->ffc_active=c->ffc_enabled&&c->ffc_valid&&c->ffc_active;
 f->charger_thermal_enabled=c->charger_thermal_enabled;
 const NpChargerThermalCurrent *thermal=&c->charger_thermal;
 f->charger_thermal_valid=thermal->valid&&thermal->epoch==c->epoch&&thermal->attachment==c->attachment&&
  now>=thermal->sampled_ms&&now>=c->charger_thermal_begin_ms&&
  now-c->charger_thermal_begin_ms<=3000&&thermal->current_ua>=0&&thermal->current_ua<=22000000;
 if(f->charger_thermal_valid)f->charger_thermal_ma=c->stock_continuous?
   c->charger_thermal_device_ma:(uint32_t)thermal->current_ua/1000u;
 f->gauge_ms=s->gauge.end_ms;f->battery_ma=s->gauge.current_ma;
 f->battery_soc=s->gauge.soc;
 f->charger_ms=s->charger.end_ms;f->pump_ms=c->pump_at;f->data_role=m[NP_META_DATA_ROLE].value;
 f->pd_state=m[NP_META_PD_STATE].value;
 f->pump_protection_valid=f->pump_known_off&&(c->pump.valid&18)==18;
 f->usb_ovp_raw=c->pump.control[2]&15;f->pump_bus_mv=c->pump.bus_mv;
 f->pump_die_centi_c=c->pump.die_centi_c;
 f->pump_valid=c->pump_error?0:c->pump.valid;
 f->pump_controls_valid=c->pump.controls_valid;f->pump_enable=c->pump.enable;
 f->pump_switching=c->pump.switching;f->pump_switching_valid=c->pump.switching_valid;
 f->pump_faults=c->pump.protection.faults;f->pump_mode=c->pump.mode;
 f->pump_bus_ocp_ma=c->pump.protection.valid?c->pump.protection.bus_ocp_ma:0;
 f->pump_bus_ocp_enabled=c->pump.protection.valid&&c->pump.protection.bus_ocp_enabled;
 f->pump_battery_mv=c->pump.battery_mv;
 f->pump_bus_ma=c->pump.bus_ma;
 f->main_buck_bus_valid=(s->valid&NP_BIT(NP_USB_CURRENT))&&s->charger.usb_ua>=0&&
   s->charger.input_current_scope==NP_INPUT_CURRENT_MAIN_BUCK;
 f->main_buck_bus_ma=s->charger.usb_ua/1000;
 if(!ordinary&&(c->mode==4||c->mode==5||c->mode==6||c->mode==7||(c->mode==8||c->mode==9))) {
  const NpMeta *role=&m[NP_META_DATA_ROLE];
  if(!role->valid||now<role->at_ms||now-role->at_ms>3000)f->valid=0;
  const NpMeta *pdstate=&m[NP_META_PD_STATE];
  if(!pdstate->valid||now<pdstate->at_ms||now-pdstate->at_ms>3000)f->valid=0;
 }
 /* Actual OS4 firmware GET2101c jumps to its common epilogue without
  * writing output. Mode6 validates the acknowledged request using current
  * PD role/APDO and two new measured PMIC/SC8585 voltage cohorts instead. */
 if(c->mode==5||c->mode==6||c->mode==7||(c->mode==8||c->mode==9)){
  const NpJeitaVote *v=&c->jeita.vote;f->policy_valid=v->valid&&v->full_valid;
  f->policy_enabled=v->enabled;f->policy_fcc=(uint32_t)v->fcc_ma;
  if(c->connector_policy_enabled){
   NpConnectorAntiburnVote connectorVote;np_control_connector_vote(c,now,&connectorVote);
   f->policy_enabled=f->policy_enabled&&connectorVote.charge_allowed;
  }
  f->policy_fv=(uint32_t)v->fv_mv;f->policy_iterm=(uint32_t)v->iterm_ma+30;
  /* The95% check belongs to the fixed trial. Stock policy stops at actual
   * full/JEITA votes and restarts with the original capacity-ratio rule. */
  if(c->mode==5||(c->mode==8||c->mode==9))f->full=v->full;
  else f->full=f->full||v->full;
  if(c->buck.stock_current_enabled&&c->buck.backend==NP_BUCK_OS4_STOCK_POLICY){
   /* Ordinary charging takes the same fresh software thermal vote in
    * addition to its own JEITA FCC. It does not inherit pump curr_max. */
   if(!f->charger_thermal_valid){f->policy_valid=0;f->policy_enabled=0;}
   else{
    if(f->policy_fcc>f->charger_thermal_ma)f->policy_fcc=f->charger_thermal_ma;
    if(f->policy_fcc<100)f->policy_enabled=0;
   }
  }
 }
}
void np_control_snapshot(const NpPowerControl *c,uint64_t now,NpSnapshot *out)
{
 np_snapshot(out,&c->sample.gauge,&c->sample.charger,now,c->sample.generation);
 if(c->mode!=9)return;
 if(!c->epoch||out->charger.epoch!=c->epoch){
  out->valid&=~(NP_BIT(NP_EXTERNAL)|NP_BIT(NP_CHARGING)|NP_BIT(NP_CHARGE_DONE)|
   NP_BIT(NP_USB_VOLTAGE)|NP_BIT(NP_USB_CURRENT));
  out->external=out->charging=out->fully_charged=0;
  out->usb_power_valid=0;out->faults|=NP_FAULT_REMOTE_RESET;return;
 }
 if(c->stock_continuous&&c->jeita.vote.full_valid&&c->jeita.vote.full){
  out->fully_charged=1;out->charging=0;
  return;
 }
 if(out->charging)return;
 const unsigned state=c->buck.state;
 if(!(c->buck.owned&64)||(state!=NP_BUCK_PUMP_OBSERVE_START&&
    state!=NP_BUCK_PUMP_RUN&&state!=NP_BUCK_PUMP_REGULATE&&
    state!=NP_BUCK_PUMP_OBSERVE_REGULATION))return;
 const uint32_t need=NP_BIT(NP_PRESENT)|NP_BIT(NP_CURRENT)|NP_BIT(NP_EXTERNAL)|
  NP_BIT(NP_CHARGING)|NP_BIT(NP_CHARGE_DONE)|NP_BIT(NP_FULL_FLAG);
 if((out->valid&need)!=need||out->faults||!out->gauge.present||!out->external||
    out->gauge.current_ma<=0||out->fully_charged||out->gauge.full_flag||
    out->charger.epoch!=c->epoch)return;
 NpBuckFacts f;np_control_facts(c,now,&f);
 if(f.valid&&now>=f.sampled_ms&&now-f.sampled_ms<=3000&&f.pmic_charging&&
    f.online&&f.charge_enabled&&!f.boost_enabled&&!f.gate_enabled&&!f.input_suspended&&
    f.policy_valid&&f.policy_enabled&&!f.fault&&!f.full)out->charging=1;
}
int np_control_next(NpPowerControl *c,uint64_t now,NpAuxCommand *out)
{
 if(!c||!out)return 0;
 connector_update(c,now);
 if(c->mode<2||!c->epoch||c->pending)return 0;
 thermal_source_power(c,now);
 /* The final enable readback can finish between gauge cohorts. Rebuild
  * the same measured startup profile before this SYNC pass selects a
  * normal-current increase; the low-current admission above owns the gate. */
 if(c->stock_continuous&&c->buck.backend==NP_BUCK_OS4_STOCK_POLICY&&
    c->buck.state==NP_BUCK_POLICY_SYNC&&c->buck.target_fcc==500)
  update_profiles(c);
 memset(out,0,sizeof(*out));NpBuckFacts f;np_control_facts(c,now,&f);
 int startup_wait=0;
 if(c->buck.stock_current_enabled&&c->buck.backend==NP_BUCK_OS4_PPS_ADAPTER_TEST&&
    c->buck.stock.reentry==3&&!c->buck.armed&&!c->buck.pending&&!c->buck.owned&&
    !c->buck.unverified_effect&&c->buck.state==NP_BUCK_FINISHED){
  /* Pump cleanup has ended. The same broker still owns the earlier normal
   * ICL/FV/enable votes; restore their bookkeeping before normal monitoring
   * or final shutdown. Neither ownership nor the original lease disappears. */
  const NpBuck old=c->buck;
  const int stopping=c->stopping||!c->allow_control||now>=old.lease_until||
      old.failures||old.cleanup_failed||!old.normal_fallback;
  np_buck_init(&c->buck);c->buck.backend=NP_BUCK_OS4_STOCK_POLICY;
  c->buck.stock_continuous=c->stock_continuous;
  c->buck.stock_current_enabled=c->buck.input_suspend_enabled=c->buck.normal_fallback=1;
  c->buck.armed=1;c->buck.stop=(uint8_t)stopping;
  c->buck.epoch=old.epoch;c->buck.attachment=old.attachment;
  c->buck.lease_until=old.lease_until;c->buck.next_id=old.next_id;c->buck.changes=old.changes;
  c->buck.failures=old.failures;c->buck.stock=old.stock;c->buck.stock.reentry=5;
  c->buck.owned=old.stock.normal_owned;c->buck.stock.normal_owned=0;
  c->buck.target_fcc=100;c->buck.cccv=old.cccv;
  if(c->stock_continuous)c->buck.cccv.initialized=0;
  c->buck.state=stopping?NP_BUCK_POLICY_DISABLE:NP_BUCK_POLICY_SYNC;
  np_control_facts(c,now,&f);
 }
 /* Every new stock source first establishes the original ordinary votes.
  * Unplug leaves the acknowledged HLOS100 vote and a disabled main buck;
  * an authenticated adapter must not wait for those old settings to satisfy
  * pump admission. The ordinary broker confirms ICL/FV/enable and current
  * before the complete reentry check can transfer this connection to PPS. */
 if(c->stock_continuous&&c->allow_control&&!c->stopping&&
    c->buck.backend==NP_BUCK_OS4_PPS_ADAPTER_TEST&&
    c->buck.state==NP_BUCK_IDLE&&!c->buck.pending&&!np_buck_all_owned(&c->buck)&&
    f.valid&&f.online&&f.pump_known_off&&f.vbus_mv>=4500&&f.vbus_mv<=5500){
  const NpCccv curve=c->buck.cccv;const uint64_t id=c->buck.next_id;
  np_buck_init(&c->buck);c->buck.backend=NP_BUCK_OS4_STOCK_POLICY;
  c->buck.stock_continuous=c->buck.stock_current_enabled=c->buck.input_suspend_enabled=1;
  c->buck.normal_fallback=1;c->buck.cccv=curve;c->buck.next_id=id;
  (void)np_buck_arm(&c->buck,now,0);c->last_armed_attachment=c->attachment;
  np_control_facts(c,now,&f);
 }
 if(c->buck.stock_current_enabled&&c->buck.backend==NP_BUCK_OS4_PPS_ADAPTER_TEST&&
    c->buck.normal_fallback&&c->allow_control&&!c->stopping&&
    !c->buck.armed&&!c->buck.pending&&!c->buck.owned&&!c->buck.unverified_effect&&
    !c->buck.failures&&!c->buck.cleanup_failed&&c->buck.state==NP_BUCK_FINISHED&&
    now<c->buck.lease_until&&(!c->buck.epoch||c->buck.epoch==c->epoch)&&
    (!c->buck.attachment||c->buck.attachment==c->attachment)&&f.valid&&f.online&&
    f.pump_known_off&&f.vbus_mv>=4500&&f.vbus_mv<=5500&&
    (!c->ffc_enabled||(c->ffc_valid&&!c->ffc_active&&!c->ffc_owned&&!c->ffc_failed))){
  /* The same broker continues only after the pump's 5V/input/FFC cleanup
   * actually finished. Preserve the original 300s lease and request IDs. */
  const uint64_t lease=c->buck.lease_until,id=c->buck.next_id;
  const uint32_t changes=c->buck.changes;
  np_buck_init(&c->buck);c->buck.backend=NP_BUCK_OS4_STOCK_POLICY;
  c->buck.stock_continuous=c->stock_continuous;
  c->buck.stock_current_enabled=c->buck.input_suspend_enabled=c->buck.normal_fallback=1;
  c->buck.armed=1;c->buck.lease_until=lease;c->buck.next_id=id;c->buck.changes=changes;
  np_control_facts(c,now,&f);
 }
 if(c->mode==9&&c->buck.backend==NP_BUCK_OS4_STOCK_POLICY&&c->allow_control&&
    !c->stopping&&(!c->stock_continuous||c->adapter_verified)&&
    (!c->ffc_enabled||(c->ffc_valid&&!c->ffc_active&&!c->ffc_owned&&!c->ffc_failed))){
  NpBuck planned,normal=c->buck;uint32_t icl=0;
  int low_start=0;
  if(c->stock_continuous&&normal.state==NP_BUCK_POLICY_SYNC&&
     (normal.confirmation==NP_BUCK_CONFIRM_CURRENT||
      normal.confirmation==NP_BUCK_CONFIRM_REDUCTION_ACK)&&(normal.owned&3u)==3u&&
     normal.target_fcc==500&&normal.stock.actual[3]==1&&
     normal.stock.actual[1]==f.policy_fv){
   /* ICL/FV/enable and the initial low FCC are already confirmed. An FV
    * already equal to the original profile need not be owned or rewritten.
    * Factory pump admission can run now, before the normal branch raises FCC3A
    * only to lower it to500mA again immediately before input suspend. */
   normal.state=NP_BUCK_POLICY_MONITOR;
   low_start=1;
  }
  const int planned_ready=np_buck_prepare_reentry(&normal,&f,&planned,&icl);
  if(low_start&&!planned_ready&&f.valid&&f.online&&!f.fault&&!f.full&&
     f.policy_valid&&f.policy_enabled&&f.charger_thermal_valid&&
     c->caps.valid&&c->buck.cccv.valid&&now>=f.sampled_ms&&
     (now-f.sampled_ms>3000u-NP_BUCK_COMMAND_MS||
      (!f.pmic_charging&&f.charger_ms<=c->meta[NP_META_ENABLED].at_ms))){
   /* Pump admission needs room for the full command freshness window.
    * Hold the accepted500mA vote while the existing round-robin obtains
    * that same new source's samples; do not enter an ordinary3A feedback
    * calibration just because the previous cohort is too old to send. */
   c->buck.blocked=NP_BUCK_STALE;
   startup_wait=1;
  }
  if(planned_ready){
   if(f.icl_ma<2000){
    /* Release the normal branch's own1500mA clamp by setting a verified
     * fixed-PDO/baseline limit. The old branch remains responsible until
     * its existing limit-readback path confirms this request. */
    c->buck.stock.reentry=1;c->buck.stock.index=0;
    c->buck.stock.target[0]=icl;c->buck.state=NP_BUCK_POLICY_CHECK;
    NpBuckCommand command;int got=np_buck_issue(&c->buck,&f,&command,1,NP_BUCK_ICL,icl);
    if(got>0){
     *out=(NpAuxCommand){command.id,c->attachment,command.property,command.value,0,1,1,0};
     c->pending_command=*out;c->pending=1;return 1;
    }
    return got;
   }
   c->buck=planned;np_control_facts(c,now,&f);
  }else if(c->buck.stock.reentry==2){
   /* A lower aggregate after our completed ICL write belongs to another
    * active limit. Keep ordinary monitoring without replaying that raise. */
   if(f.icl_ma<2000)c->buck.stock.reentry=4;
   else if(!c->stock_continuous)c->buck.stock.reentry=0;
   else if(f.valid&&now>=f.sampled_ms&&now-f.sampled_ms<=3000u-NP_BUCK_COMMAND_MS&&
           c->caps.valid&&now>=c->caps.at_ms&&now-c->caps.at_ms<=3000&&
           c->buck.cccv.valid&&now>=c->buck.cccv.sampled_ms&&
           now-c->buck.cccv.sampled_ms<=3000){
    /* Fresh source facts rejected a new pump start. Restore ordinary
     * limits through SYNC instead of diagnosing the acknowledged raise. */
    c->buck.stock.reentry=0;c->buck.state=NP_BUCK_POLICY_SYNC;
   }
  }
 }
 if(c->buck.stock_current_enabled&&c->buck.backend==NP_BUCK_OS4_STOCK_POLICY&&
    now>=c->buck.lease_until)np_buck_stop(&c->buck);
 if(!c->allow_control&&!np_buck_pump_quiet(&c->buck))np_buck_stop(&c->buck);
 if(c->allow_control&&!c->stopping&&!c->buck.armed&&!c->buck.owned&&c->buck.state!=NP_BUCK_UNCERTAIN&&
    (c->last_armed_attachment!=c->attachment||
     (c->mode==5&&c->buck.state==NP_BUCK_FINISHED&&!c->buck.failures))&&f.valid&&f.online&&
    (f.real_type==3||f.real_type==10||f.real_type==11||f.real_type==12||
     ((c->mode==5||c->stock_continuous)&&(f.real_type==1||f.real_type==2))||
     (c->stock_continuous&&(f.real_type==4||f.real_type==9||f.real_type==15)))) {
  if(c->stock_continuous){
   /* All prior effects are released. A new/resumed connection starts with
    * a fresh adapter plan; an old accepted PPS current is not its baseline. */
   const uint64_t id=c->buck.next_id;const uint32_t changes=c->buck.changes;
   np_buck_init(&c->buck);c->buck.backend=NP_BUCK_OS4_PPS_ADAPTER_TEST;
   c->buck.next_id=id;c->buck.changes=changes;
   c->buck.stock_current_enabled=c->buck.input_suspend_enabled=c->buck.stock_continuous=1;
   update_profiles(c);np_control_facts(c,now,&f);
  }else if(c->buck.stock_current_enabled&&c->buck.backend==NP_BUCK_OS4_STOCK_POLICY){
   const uint64_t id=c->buck.next_id;np_buck_init(&c->buck);
   c->buck.next_id=id;c->buck.backend=NP_BUCK_OS4_PPS_ADAPTER_TEST;
   c->buck.stock_current_enabled=c->buck.input_suspend_enabled=1;update_profiles(c);
   np_control_facts(c,now,&f);
  }
  c->buck.stock_continuous=c->stock_continuous;
  if(c->stock_continuous)c->buck.normal_fallback=0;
  if(!np_buck_arm(&c->buck,now,c->stock_continuous||c->mode==5?0:(c->mode==4||c->mode==6||c->mode==7||(c->mode==8||c->mode==9))?300000:c->mode==3?240000:120000))c->last_armed_attachment=c->attachment;
 }
 NpBuckCommand control;int got=startup_wait?0:np_buck_next(&c->buck,&f,&control);
 if(got>0) {
  unsigned local=(control.property&0xff000000u)==0xff000000u;
  *out=(NpAuxCommand){control.id,c->attachment,control.property,control.value,
       (uint16_t)(local?1:control.write?0:control.property==NP_BUCK_ENABLE?1:4),control.write,(uint8_t)(local?2:1),0};
 } else {
  /* Fixed-PD exit needs fresh role/limit/status facts through both measured
   * voltage cohorts. Stop reads as soon as that controller has finished. */
  if(c->stopping&&((c->mode!=4&&c->mode!=5&&c->mode!=6&&c->mode!=7&&c->mode!=8&&c->mode!=9)||
     (!c->buck.armed&&!c->buck.owned&&!c->buck.pending)||
     c->buck.state==NP_BUCK_UNCERTAIN))return 0;
  const unsigned slots=metadata_count(c);
  const unsigned due=np_control_refresh_due(c,now);
  unsigned oldest=due<slots?due:slots;
  if(c->buck.backend==NP_BUCK_OS4_STOCK_POLICY&&
     ((c->buck.state==NP_BUCK_POLICY_SYNC&&c->buck.blocked==NP_BUCK_STALE)||
      (c->stock_continuous&&c->buck.stock.reentry==2))){
   /* A just-confirmed limit can leave old metadata close to its3s gate.
    * Refresh the oldest required field before selecting the next write. */
   for(unsigned i=0;i<slots;++i){
    const int required=(i<=NP_META_VERIFY&&i!=NP_META_FCC)||
      i==NP_META_DATA_ROLE||i==NP_META_PD_STATE;
    const NpMeta *m=&c->meta[i];
    if(!required||now<m->retry_ms||(m->valid&&now>=m->at_ms&&now-m->at_ms<1000))continue;
    if(oldest==slots||m->at_ms<c->meta[oldest].at_ms)oldest=i;
   }
  }
  for(unsigned count=0;count<slots;++count) {
   unsigned i=!count&&oldest<slots?oldest:c->cursor++%slots;NpMeta *m=&c->meta[i];
   if(c->stock_continuous&&(f.real_type==1||f.real_type==2||f.real_type==3||
      f.real_type==4||f.real_type==9||f.real_type==15)&&
      i>=NP_META_PDOS&&i<=NP_META_PD_STATE)continue;
   if(c->buck.backend==NP_BUCK_OS4_STOCK_POLICY&&i==NP_META_FCC)continue; // Current OS4 has no supported FCC getter.
   if(c->buck.input_suspend_enabled&&i==NP_META_SUSPEND){m->valid=0;m->error=-ENOTSUP;continue;}
   if((c->mode==6||c->mode==7||(c->mode==8||c->mode==9))&&i==NP_META_PPS){m->valid=0;m->error=-ENOTSUP;continue;}
   uint64_t period=i>=NP_META_PDOS?30000:1000;
    if(i==NP_META_PD_STATE||i==NP_META_CID||i==NP_META_OTG_PLUGIN||
       ((c->mode==4||c->mode==5||c->mode==6||c->mode==7||(c->mode==8||c->mode==9))&&i==NP_META_DATA_ROLE)||
       ((c->mode==6||c->mode==7||(c->mode==8||c->mode==9))&&(i==NP_META_PDOS||i==NP_META_PPS)))period=1000;
   if(now<m->retry_ms||(m->valid&&now>=m->at_ms&&now-m->at_ms<period))continue;
   if(c->next_id==UINT64_MAX)return -1;
   *out=(NpAuxCommand){++c->next_id,c->attachment,properties[i],0,(uint16_t)length(i),0,0,(uint8_t)i};got=1;break;
  }
 }
 if(got<=0)return 0;
 c->pending_command=*out;c->pending=1;++c->queries;return 1;
}
static int scalar_valid(unsigned i,uint32_t v,unsigned mode)
{
 switch(i) {
 case NP_META_ICL:
  /*051's PPS contract changed the firmware aggregate from3000 to6100mA,
   * matching the live9V APDO. This is a read, never our requested current.
   * The PPS backend separately bounds it to that actual selected APDO. */
  return v<=((mode==6||mode==7||(mode==8||mode==9))?10000u:5000u)&&(mode==5||v>=100);
 case NP_META_FCC:return v>=100&&v<=10000;
 case NP_META_TERM:return v>=3500&&v<=4600;
 case NP_META_ENABLED:case NP_META_BOOST:case NP_META_GATE:case NP_META_SUSPEND:case NP_META_VERIFY:
 case NP_META_CID:case NP_META_OTG_PLUGIN:return v<=1;
 case NP_META_TYPE:return v<=20;
 case NP_META_DATA_ROLE:case NP_META_PD_STATE:return v<=2;
 default:return 1;
 }
}
int np_control_complete(NpPowerControl *c,const NpAuxCommand *command,int error,const uint8_t *data,size_t bytes,uint64_t now)
{
 if(!c||!command||!c->pending||command->id!=c->pending_command.id||
    command->control!=c->pending_command.control||command->property!=c->pending_command.property||
    command->write!=c->pending_command.write||command->index!=c->pending_command.index)return -1;
 c->pending=0;
 if(command->attachment!=c->attachment) {
  /* The old source's read must not install a30s retry on the new source.
   * Accepted writes still complete as uncertain and enter compensation. */
  if(!command->control){++c->errors;return -1;}
  error=-2;
 }
 if(!error&&c->buck.backend==NP_BUCK_OS4_STOCK_POLICY&&command->control&&!command->write&&
    np_buck_stock_confirming(&c->buck)&&now>=c->buck.command.deadline_ms)error=-4;
 if(!error&&(bytes<command->reply_bytes||(command->reply_bytes&&!data)))error=-3;
 if(error)++c->errors;
 uint32_t value=0;
 if(!error&&command->reply_bytes)value=command->reply_bytes==1?data[0]:np_le32(data);
 if(command->control) {
  const int regulation_ack=c->mode==9&&c->buck.stock_current_enabled&&
   command->control==1&&command->write&&command->property==NP_BUCK_PPS_PD&&
   c->buck.state==NP_BUCK_PUMP_REGULATE;
  int result=np_buck_complete(&c->buck,command->id,error,value);
  if(result>0&&!error&&c->buck.backend==NP_BUCK_OS4_STOCK_POLICY&&
     c->buck.stock.reentry==5&&!command->write&&command->property==NP_BUCK_ENABLE&&
     value==0&&c->buck.state==NP_BUCK_POLICY_STOP_OBSERVE){
   /* The actual disabled readback completed. Observe fresh samples after
    * this time; the read command's unused timeout is not a settling delay. */
   c->buck.stock.stop_after_ms=now;
  }
  /* The broker supplies the actual completed ACK time. The command deadline
   * is its timeout, not a settling delay. Mode11 still needs two new PMIC/CP
   * cohorts after this ACK before it can accept or raise the next target. */
  if(regulation_ack&&!error&&result>0&&c->buck.state==NP_BUCK_PUMP_OBSERVE_REGULATION)
   c->buck.voltage_after_ms=now;
  /* A completed exit ACK starts the original fresh hardware observations.
   * Its unused wire timeout is not a settling delay. Keep both cohorts and
   * the original16s drain when the ordinary connection carries its votes. */
  if(c->stock_continuous&&result>0&&!error&&command->control==1&&command->write){
   if(command->property==NP_BUCK_FIXED_PD&&c->buck.state==NP_BUCK_PD_OBSERVE_5V)
    c->buck.voltage_after_ms=now;
   if(command->property==NP_BUCK_INPUT_SUSPEND&&command->value==0&&
      c->buck.state==NP_BUCK_PUMP_OBSERVE_INPUT_RESUME)c->buck.input_after_ms=now;
   if(command->property==NP_BUCK_INPUT_SUSPEND&&command->value==1&&
      c->buck.state==NP_BUCK_PUMP_OBSERVE_INPUT_SUSPEND)c->buck.input_after_ms=now;
   if(command->property==NP_BUCK_PPS_PD&&
      c->buck.state==NP_BUCK_PUMP_OBSERVE_PPS)c->buck.voltage_after_ms=now;
   if(command->property==NP_BUCK_FCC&&c->buck.backend==NP_BUCK_OS4_STOCK_POLICY&&
      c->buck.state==NP_BUCK_POLICY_FCC_CHECK){
    c->buck.feedback_after_ms=now;
    if(command->value==500&&c->adapter_verified&&c->allow_control&&!c->stopping){
     /* A typed low-current ACK on the authenticated source permits the
      * original ICL/FV/enable setup before pump admission. That low vote
      * needs no four-point current calibration; every increase still uses
      * normal readbacks and full pump/thermal/source checks. */
     c->buck.unverified_effect=0;c->buck.confirmation=NP_BUCK_CONFIRM_REDUCTION_ACK;
     c->buck.state=NP_BUCK_POLICY_SYNC;
    }
   }
  }
  if(c->stock_continuous&&c->mode==9&&result>0&&!error&&
     command->control==1&&!command->write&&command->property==NP_BUCK_ENABLE&&
     value==1&&c->buck.backend==NP_BUCK_OS4_STOCK_POLICY&&
     c->buck.state==NP_BUCK_POLICY_FCC_CHECK&&c->buck.target_fcc==500&&
     (c->buck.owned&2u)&&c->adapter_verified&&c->allow_control&&!c->stopping&&
     c->buck.epoch==c->epoch&&c->buck.attachment==c->attachment){
   /* The new source's low FCC ACK and ICL/FV/enable readbacks completed.
    * Enabling the main buck must not discard that ACK and start another
    * four-point calibration before the original pump admission. SYNC
    * still consumes fresh source/thermal/CP facts; all later FCC increases
    * retain ordinary feedback, and pump start retains both input cohorts. */
   c->buck.unverified_effect=0;c->buck.confirmation=NP_BUCK_CONFIRM_REDUCTION_ACK;
   c->buck.state=NP_BUCK_POLICY_SYNC;
  }
  if(command->control==2&&command->property==NP_BUCK_USB_OVP) {
   if(!error&&value<=15)c->pump.control[2]=(c->pump.control[2]&0xf0)|(uint8_t)value;
   return result;
  }
  unsigned i=command->property==NP_BUCK_ICL?NP_META_ICL:
    command->property==NP_BUCK_TERM_VOLT?NP_META_TERM:
    command->property==NP_BUCK_ENABLE?NP_META_ENABLED:
    command->property==NP_BUCK_FCC?NP_META_FCC:NP_META_COUNT;
  if(!command->write) {
   if(i==NP_META_COUNT)return result;
   c->meta[i].valid=0;c->meta[i].retry_ms=0;
   if(!error){c->meta[i].value=value;c->meta[i].at_ms=now;c->meta[i].valid=scalar_valid(i,value,
       c->buck.backend==NP_BUCK_OS4_STOCK_POLICY?5:c->mode);}
  }
  return result;
 }
 unsigned i=command->index;if(i>=NP_META_COUNT)return -1;
 NpMeta *m=&c->meta[i];m->value=value;m->at_ms=now;
 if(!error&&i==NP_META_PDOS)error=np_pd_decode(&c->caps,data,bytes,c->epoch,now);
 else if(!error&&!(c->stock_continuous&&i==NP_META_ICL&&value==UINT32_MAX)&&
    !scalar_valid(i,value,c->buck.backend==NP_BUCK_OS4_STOCK_POLICY?5:c->mode))error=-4;
 m->error=error;m->valid=!error;m->retry_ms=now+(error?30000:0);return error?-1:0;
}
int np_control_replan_unsent_pps(NpPowerControl *c,const NpAuxCommand *command,uint64_t now)
{
 if(!c||!command||!c->stock_continuous||c->mode!=9||!c->pending||
    !c->buck.pending||c->buck.wire_accepted||c->stopping||c->buck.stop||
    c->buck.failures||c->buck.cleanup_failed||
    c->buck.backend!=NP_BUCK_OS4_PPS_ADAPTER_TEST||
    c->buck.state!=NP_BUCK_PUMP_REGULATE||!c->buck.armed||
    c->buck.voltage_samples<2||
    c->buck.confirmed_vbus_mv!=c->buck.pump_target_mv||
    !c->adapter_verified||!c->allow_control||command->control!=1||
    !command->write||command->property!=NP_BUCK_PPS_PD||
    command->id!=c->pending_command.id||command->id!=c->buck.request_id||
    c->pending_command.control!=1||!c->pending_command.write||
    c->pending_command.property!=NP_BUCK_PPS_PD||
    c->buck.command.property!=NP_BUCK_PPS_PD||!c->buck.command.write||
    command->value!=c->pending_command.value||command->value!=c->buck.command.value||
    command->attachment!=c->attachment||c->buck.attachment!=c->attachment||
    c->buck.epoch!=c->epoch||now>=c->buck.command.deadline_ms||
    np_control_write_allowed(c,command,now))return 0;
 /* The broker has not accepted this request. Its unverified_effect marks
  * the planned write, not a change to hardware; the previous PPS contract
  * and all pump/input owners remain confirmed. Do not count a failed
  * transaction or let its stale target drive FFC/watchdog shutdown. The
  * existing monitor evaluates the next cohort and any real protection exit. */
 c->pending=0;c->buck.pending=0;c->buck.unverified_effect=0;
 c->buck.cccv_target_mv=c->buck.pump_target_mv;
 c->buck.cccv_target_ma=c->buck.pump_request_ma;c->buck.cccv_delta_mv=0;
 c->buck.state=NP_BUCK_PUMP_RUN;
 return 1;
}
int np_control_write_allowed(NpPowerControl *c,const NpAuxCommand *command,uint64_t now)
{
 if(!c||!command||!c->pending||!command->control||!command->write||
    command->id!=c->pending_command.id||c->epoch!=c->buck.epoch||
    now>=c->buck.command.deadline_ms)return 0;
 uint32_t property=command->property,value=command->value;
 NpBuckFacts f;np_control_facts(c,now,&f);
 if(c->buck.backend==NP_BUCK_OS4_STOCK_POLICY){
  if(command->control!=1||command->attachment!=c->attachment)return 0;
  if(property==NP_BUCK_ICL&&c->buck.stock.reentry==1&&
     c->buck.state==NP_BUCK_POLICY_CHECK){
   /* Recheck the complete pump admission immediately before replacing
    * this branch's low vote. A newer hot/full/stale sample cancels it. */
   if((c->stock_continuous&&!c->adapter_verified)||
      (c->ffc_enabled&&(!c->ffc_valid||c->ffc_active||c->ffc_owned||c->ffc_failed))||
      !np_buck_reentry_write_allowed(&c->buck,&f,value))return 0;
  }
  return np_buck_stock_write_allowed(&c->buck,&f,property,value);
 }
 if(c->mode==7||(c->mode==8||c->mode==9)) {
  if(property!=NP_BUCK_FCC&&property!=NP_BUCK_PPS_PD&&property!=NP_BUCK_FIXED_PD&&
     !(c->mode==9&&c->buck.input_suspend_enabled&&property==NP_BUCK_INPUT_SUSPEND&&value<=1))return 0;
  NpBuck copy=c->buck;copy.pending=0;NpBuckCommand check;
  if(np_buck_next(&copy,&f,&check)!=1)return 0;
  return check.write&&check.property==property&&check.value==value;
 }
 if(c->mode==4||c->mode==6) {
  if(command->control!=1)return 0;
  if(property==NP_BUCK_FCC) {
   if(value<100||value>1500)return 0;
   if(c->buck.state==NP_BUCK_FCC_REDUCE)
    return value==c->buck.restore_value&&(value==100||value==500);
   if(c->buck.state!=NP_BUCK_FCC_INITIALIZE&&c->buck.state!=NP_BUCK_SET_FCC)return 0;
  } else if(property==NP_BUCK_PPS_PD&&c->mode==6) {
   uint32_t encoded=0;
   if(c->buck.state!=NP_BUCK_PD_REQUEST_9V||np_pps_encode(9000,2000,&encoded)||value!=encoded)return 0;
  } else if(property==NP_BUCK_FIXED_PD) {
   if(c->mode==6&&value!=5000)return 0;
   if((value==9000&&c->buck.state!=NP_BUCK_PD_REQUEST_9V)||
      (value==5000&&c->buck.state!=NP_BUCK_PD_REQUEST_5V)||(value!=9000&&value!=5000))return 0;
   if(value==5000)return f.valid&&f.online&&(f.real_type==10||f.real_type==11||f.real_type==12)&&f.pd_state==2&&
                          (f.data_role==1||f.data_role==2)&&
                          !f.boost_enabled&&!f.gate_enabled;
  } else return 0;
  NpBuck copy=c->buck;copy.pending=0;NpBuckCommand check;
  if(np_buck_next(&copy,&f,&check)!=1)return 0;
  return check.write&&check.property==property&&check.value==value;
 }
 if((property!=NP_BUCK_ICL&&property!=NP_BUCK_FCC)||value<100||value>1500)return 0;
 if(c->mode==3) {
  if(property!=NP_BUCK_FCC||value>1000)return 0;
  if(c->buck.state==NP_BUCK_FCC_REDUCE)
   return value==c->buck.restore_value&&(value==100||value==500);
  if(c->buck.state!=NP_BUCK_FCC_INITIALIZE&&c->buck.state!=NP_BUCK_SET_FCC)return 0;
  NpBuck copy=c->buck;copy.pending=0;NpBuckCommand check;
  if(c->buck.stop||c->stopping||command->attachment!=c->attachment||np_buck_next(&copy,&f,&check)!=1)return 0;
  return check.write&&check.property==property&&check.value==value;
 }
 if(c->buck.state==NP_BUCK_SET_ICL||c->buck.state==NP_BUCK_SET_FCC) {
  /* Same facts must still qualify immediately before the single wire write. */
  NpBuck copy=c->buck;copy.pending=0;NpBuckCommand check;
  if(c->buck.stop||c->stopping||command->attachment!=c->attachment||np_buck_next(&copy,&f,&check)!=1)return 0;
  return check.write&&check.property==property&&check.value==value;
 }
 if(c->buck.state==NP_BUCK_RESTORE_ICL_WRITE||c->buck.state==NP_BUCK_RESTORE_FCC_WRITE)
  return value==c->buck.restore_value&&value<=(property==NP_BUCK_ICL?c->buck.target_icl:c->buck.target_fcc);
 return 0;
}
int np_control_register_allowed(NpPowerControl *c,const NpAuxCommand *command,uint64_t now)
{
 if(!c||!command||(c->mode!=4&&c->mode!=6&&c->mode!=7&&c->mode!=8&&c->mode!=9)||!c->pending||command->control!=2||
    (command->property&0xff000000u)!=0xff000000u||command->id!=c->pending_command.id||
    command->attachment!=c->attachment||c->epoch!=c->buck.epoch||
    now>=c->buck.command.deadline_ms)return 0;
 NpBuckFacts f;np_control_facts(c,now,&f);
 NpBuck copy=c->buck;copy.pending=0;NpBuckCommand check;
 return np_buck_next(&copy,&f,&check)==1&&check.write==command->write&&
        check.property==command->property&&check.value==command->value;
}
void np_control_stop(NpPowerControl *c){if(c){c->stopping=1;np_buck_stop(&c->buck);}}
