/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaPowerControl.h"
#include "NezhaMca.h"
#include <string.h>
static const uint32_t properties[NP_META_COUNT]={0x2008,0x2005,0x2007,0x2003,
 0x200d,0x200e,0x201b,0x2000c,0x21002,0x21014,0x21018,0x2101c,0x2100d,0x2100e};
static unsigned length(unsigned i){return i==NP_META_PDOS?NP_PD_BYTES:i==NP_META_ENABLED?1:4;}
void np_control_init(NpPowerControl *c,unsigned mode)
{memset(c,0,sizeof(*c));c->mode=mode;c->attachment=1;np_buck_init(&c->buck);
 if(mode==3)c->buck.backend=NP_BUCK_OS4_SETTER;
 if(mode==4)c->buck.backend=NP_BUCK_OS4_FIXED_PD;
 if(mode==5)c->buck.backend=NP_BUCK_OS4_STOCK_POLICY;
 np_jeita_init(&c->jeita);}
void np_control_profile(NpPowerControl *c,const NpGaugeIdentity *id,int country_cn)
{if(c){if(id)c->identity=*id;else memset(&c->identity,0,sizeof(c->identity));c->country_cn=country_cn==1;}}
void np_control_link(NpPowerControl *c,uint64_t epoch,int up)
{
 if(!up||c->epoch!=epoch){memset(c->meta,0,sizeof(c->meta));memset(&c->caps,0,sizeof(c->caps));np_buck_stop(&c->buck);c->source_online_valid=0;}
 c->epoch=up?epoch:0;
}
void np_control_source_event(NpPowerControl *c)
{
 if(c->attachment==UINT64_MAX){c->stopping=1;np_buck_stop(&c->buck);return;}
 ++c->attachment;memset(c->meta,0,sizeof(c->meta));memset(&c->caps,0,sizeof(c->caps));np_buck_stop(&c->buck);
}
int np_control_usb_type(NpPowerControl *c,uint32_t online,uint32_t type,uint64_t now)
{
 if(!c||online>1||type>20)return -1;
 const NpMeta previous=c->meta[NP_META_TYPE];
 const int same_pd=(previous.value==10||previous.value==11)&&(type==10||type==11);
 const int changed=!c->source_online_valid||c->source_online!=online||
   (online&&previous.valid&&previous.value!=type&&!same_pd);
 if(changed)np_control_source_event(c);
 else if(previous.value!=type) {
  /* Reclassifying the same PD adapter still needs a new capability catalog
   * before a voltage increase. It does not revoke the in-flight HMAC. */
  c->caps.valid=0;c->meta[NP_META_PDOS].valid=0;
 }
 c->source_online=online;c->source_online_valid=1;
 c->meta[NP_META_TYPE]=(NpMeta){type,0,now,0,1};
 return changed;
}
void np_control_sample(NpPowerControl *c,const NpSnapshot *s,const NpPump *p,int error,uint64_t at)
{
 if(s->valid&NP_BIT(NP_EXTERNAL)) {
  if(c->source_online_valid&&(uint32_t)s->external!=c->source_online)np_control_source_event(c);
  c->source_online=(uint32_t)s->external;c->source_online_valid=1;
 }
 c->sample=*s;c->pump=*p;c->pump_error=(uint32_t)error;c->pump_at=at;
 if(c->mode==5){np_jeita_update(&c->jeita,s,&c->identity,c->country_cn);np_jeita_full_update(&c->jeita,s);}
}
static uint64_t earlier(uint64_t a,uint64_t b){return a<b?a:b;}
void np_control_facts(const NpPowerControl *c,uint64_t now,NpBuckFacts *f)
{
 memset(f,0,sizeof(*f));f->epoch=c->epoch;f->attachment=c->attachment;f->now_ms=now;
 const NpSnapshot *s=&c->sample;const NpMeta *m=c->meta;
 uint32_t need=NP_BIT(NP_PRESENT)|NP_BIT(NP_SOC)|NP_BIT(NP_VOLTAGE)|NP_BIT(NP_TEMPERATURE)|
   NP_BIT(NP_EXTERNAL)|NP_BIT(NP_CHARGING)|NP_BIT(NP_USB_VOLTAGE);
 if(c->mode>=3)need|=NP_BIT(NP_CURRENT);
 f->valid=(s->valid&need)==need;f->sampled_ms=earlier(s->gauge.end_ms,s->charger.end_ms);
 for(unsigned i=NP_META_ICL;i<=NP_META_VERIFY;++i){
  if(c->mode>=3&&i==NP_META_FCC)continue; /* setter backend has no supported getter */
  if(!m[i].valid)f->valid=0;
  f->sampled_ms=earlier(f->sampled_ms,m[i].at_ms);
 }
 f->online=(uint32_t)s->external;f->real_type=m[NP_META_TYPE].value;
 f->boost_enabled=m[NP_META_BOOST].value;f->gate_enabled=m[NP_META_GATE].value;
 f->input_suspended=m[NP_META_SUSPEND].value;f->charge_enabled=m[NP_META_ENABLED].value;
 f->term_mv=m[NP_META_TERM].value;f->icl_ma=m[NP_META_ICL].value;f->fcc_ma=m[NP_META_FCC].value;
 f->pmic_charging=(uint32_t)s->charging;f->vbus_mv=s->charger.usb_uv>0?(uint32_t)s->charger.usb_uv/1000:0;
 f->battery_mv=s->gauge.voltage_mv;f->temperature_centi_c=s->gauge.temperature_centi_c;
 f->fault=s->faults|m[NP_META_VERIFY].value;
 f->full=s->fully_charged||s->gauge.full_flag||s->gauge.soc>=95;
 f->pump_known_off=!c->pump_error&&c->pump.controls_valid&&c->pump.chip==0x85&&
   c->pump.switching_valid&&!c->pump.switching&&!c->pump.enable&&c->pump.mode<=2&&
   now>=c->pump_at&&now-c->pump_at<=3000;
 f->pd=&c->caps;
 f->gauge_ms=s->gauge.end_ms;f->battery_ma=s->gauge.current_ma;
 f->charger_ms=s->charger.end_ms;f->data_role=m[NP_META_DATA_ROLE].value;
 f->pd_state=m[NP_META_PD_STATE].value;
 f->pump_protection_valid=f->pump_known_off&&(c->pump.valid&18)==18;
 f->usb_ovp_raw=c->pump.control[2]&15;f->pump_bus_mv=c->pump.bus_mv;
 f->pump_die_centi_c=c->pump.die_centi_c;
 if(c->mode==4||c->mode==5) {
  const NpMeta *role=&m[NP_META_DATA_ROLE];
  if(!role->valid||now<role->at_ms||now-role->at_ms>3000)f->valid=0;
  const NpMeta *pdstate=&m[NP_META_PD_STATE];
  if(!pdstate->valid||now<pdstate->at_ms||now-pdstate->at_ms>3000)f->valid=0;
 }
 if(c->mode==5){
  const NpJeitaVote *v=&c->jeita.vote;f->policy_valid=v->valid&&v->full_valid;
  f->policy_enabled=v->enabled;f->policy_fcc=(uint32_t)v->fcc_ma;
  f->policy_fv=(uint32_t)v->fv_mv;f->policy_iterm=(uint32_t)v->iterm_ma+30;
  /* The95% check belongs to the fixed trial. Stock policy stops at actual
   * full/JEITA votes and restarts with the original capacity-ratio rule. */
  f->full=v->full;
 }
}
int np_control_next(NpPowerControl *c,uint64_t now,NpAuxCommand *out)
{
 if(!c||!out||c->mode<2||!c->epoch||c->pending)return 0;
 memset(out,0,sizeof(*out));NpBuckFacts f;np_control_facts(c,now,&f);
 if(!c->allow_control)np_buck_stop(&c->buck);
 if(c->allow_control&&!c->stopping&&!c->buck.armed&&!c->buck.owned&&c->buck.state!=NP_BUCK_UNCERTAIN&&
    (c->last_armed_attachment!=c->attachment||
     (c->mode==5&&c->buck.state==NP_BUCK_FINISHED&&!c->buck.failures))&&f.valid&&f.online&&
    (f.real_type==3||f.real_type==10||f.real_type==11||f.real_type==12||
     (c->mode==5&&(f.real_type==1||f.real_type==2)))) {
  if(!np_buck_arm(&c->buck,now,c->mode==5?0:c->mode==4?300000:c->mode==3?240000:120000))c->last_armed_attachment=c->attachment;
 }
 NpBuckCommand control;int got=np_buck_next(&c->buck,&f,&control);
 if(got>0) {
  unsigned local=control.property==NP_BUCK_USB_OVP;
  *out=(NpAuxCommand){control.id,c->attachment,control.property,control.value,
       (uint16_t)(local?1:control.write?0:control.property==NP_BUCK_ENABLE?1:4),control.write,(uint8_t)(local?2:1),0};
 } else {
  /* Fixed-PD exit needs fresh role/limit/status facts through both measured
   * voltage cohorts. Stop reads as soon as that controller has finished. */
  if(c->stopping&&((c->mode!=4&&c->mode!=5)||
     (!c->buck.armed&&!c->buck.owned&&!c->buck.pending)||
     c->buck.state==NP_BUCK_UNCERTAIN))return 0;
  unsigned oldest=NP_META_COUNT;
  if(c->mode==5&&c->buck.state==NP_BUCK_POLICY_SYNC&&c->buck.blocked==NP_BUCK_STALE){
   /* A just-confirmed limit can leave old metadata close to its3s gate.
    * Refresh the oldest required field before selecting the next write. */
   for(unsigned i=0;i<NP_META_COUNT;++i){
    const int required=(i<=NP_META_VERIFY&&i!=NP_META_FCC)||
      i==NP_META_DATA_ROLE||i==NP_META_PD_STATE;
    const NpMeta *m=&c->meta[i];
    if(!required||now<m->retry_ms||(m->valid&&now>=m->at_ms&&now-m->at_ms<1000))continue;
    if(oldest==NP_META_COUNT||m->at_ms<c->meta[oldest].at_ms)oldest=i;
   }
  }
  for(unsigned count=0;count<NP_META_COUNT;++count) {
   unsigned i=!count&&oldest<NP_META_COUNT?oldest:c->cursor++%NP_META_COUNT;NpMeta *m=&c->meta[i];
   if(c->mode==5&&i==NP_META_FCC)continue; // Current OS4 has no supported FCC getter.
   uint64_t period=i>=NP_META_PDOS?30000:1000;
    if(i==NP_META_PD_STATE||((c->mode==4||c->mode==5)&&i==NP_META_DATA_ROLE))period=1000;
   if(now<m->retry_ms||(m->valid&&now>=m->at_ms&&now-m->at_ms<period))continue;
   if(c->next_id==UINT64_MAX)return -1;
   *out=(NpAuxCommand){++c->next_id,c->attachment,properties[i],0,(uint16_t)length(i),0,0,(uint8_t)i};got=1;break;
  }
 }
 if(got<=0)return 0;
 c->pending_command=*out;c->pending=1;++c->queries;return 1;
}
static int scalar_valid(unsigned i,uint32_t v)
{
 switch(i) {
 case NP_META_ICL:return v>=100&&v<=5000;
 case NP_META_FCC:return v>=100&&v<=10000;
 case NP_META_TERM:return v>=3500&&v<=4600;
 case NP_META_ENABLED:case NP_META_BOOST:case NP_META_GATE:case NP_META_SUSPEND:case NP_META_VERIFY:return v<=1;
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
 if(!error&&c->mode==5&&command->control&&!command->write&&
    np_buck_stock_confirming(&c->buck)&&now>=c->buck.command.deadline_ms)error=-4;
 if(!error&&(bytes<command->reply_bytes||(command->reply_bytes&&!data)))error=-3;
 if(error)++c->errors;
 uint32_t value=0;
 if(!error&&command->reply_bytes)value=command->reply_bytes==1?data[0]:np_le32(data);
 if(command->control) {
  int result=np_buck_complete(&c->buck,command->id,error,value);
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
   if(!error){c->meta[i].value=value;c->meta[i].at_ms=now;c->meta[i].valid=scalar_valid(i,value);}
  }
  return result;
 }
 unsigned i=command->index;if(i>=NP_META_COUNT)return -1;
 NpMeta *m=&c->meta[i];m->value=value;m->at_ms=now;
 if(!error&&i==NP_META_PDOS)error=np_pd_decode(&c->caps,data,bytes,c->epoch,now);
 else if(!error&&!scalar_valid(i,value))error=-4;
 m->error=error;m->valid=!error;m->retry_ms=now+(error?30000:0);return error?-1:0;
}
int np_control_write_allowed(NpPowerControl *c,const NpAuxCommand *command,uint64_t now)
{
 if(!c||!command||!c->pending||!command->control||!command->write||
    command->id!=c->pending_command.id||c->epoch!=c->buck.epoch||
    now>=c->buck.command.deadline_ms)return 0;
 uint32_t property=command->property,value=command->value;
 NpBuckFacts f;np_control_facts(c,now,&f);
 if(c->mode==5){
  if(command->control!=1||command->attachment!=c->attachment)return 0;
  return np_buck_stock_write_allowed(&c->buck,&f,property,value);
 }
 if(c->mode==4) {
  if(command->control!=1)return 0;
  if(property==NP_BUCK_FCC) {
   if(value<100||value>1500)return 0;
   if(c->buck.state==NP_BUCK_FCC_REDUCE)
    return value==c->buck.restore_value&&(value==100||value==500);
   if(c->buck.state!=NP_BUCK_FCC_INITIALIZE&&c->buck.state!=NP_BUCK_SET_FCC)return 0;
  } else if(property==NP_BUCK_FIXED_PD) {
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
 if(!c||!command||c->mode!=4||!c->pending||command->control!=2||!command->write||
    command->property!=NP_BUCK_USB_OVP||command->id!=c->pending_command.id||
    command->attachment!=c->attachment||c->epoch!=c->buck.epoch||
    now>=c->buck.command.deadline_ms)return 0;
 NpBuckFacts f;np_control_facts(c,now,&f);
 NpBuck copy=c->buck;copy.pending=0;NpBuckCommand check;
 return np_buck_next(&copy,&f,&check)==1&&check.write&&
        check.property==command->property&&check.value==command->value;
}
void np_control_stop(NpPowerControl *c){if(c){c->stopping=1;np_buck_stop(&c->buck);}}
