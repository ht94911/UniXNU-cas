/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* CN stock JEITA/full/recharge ceilings on the existing single MCA broker.
 * This normal-buck policy never requests voltage or enables the charge pump. */
#include "NezhaBuck.h"
#include <string.h>

/* Slot2 is reserved: this OS4 firmware has no ITERM getter. Its successful
 * empty response is neither a baseline nor a hardware readback. Leave the
 * firmware's termination current untouched, including during cleanup. */
static const uint32_t fields[4]={NP_BUCK_ICL,NP_BUCK_TERM_VOLT,0,NP_BUCK_ENABLE};
static const uint32_t owner[4]={1,4,0,16};
static uint32_t minimum(uint32_t a,uint32_t b){return a<b?a:b;}
static int valid_field(unsigned i,uint32_t value)
{
 /* Min aggregation can legitimately be0. It blocks ordinary increases;
  * ffffffff is the firmware's invalid sentinel, never a default limit. */
 if(i==0)return value<=5000;
 if(i==1)return value>=3500&&value<=4500;
 return i==3&&value<=1;
}
static unsigned qualified(const NpBuckFacts *f,uint32_t *icl,uint32_t *fcc)
{
 if(!f->valid||!f->policy_valid||!f->epoch||!f->attachment||
    f->now_ms<f->sampled_ms||f->now_ms-f->sampled_ms>3000)return NP_BUCK_STALE;
 if(!f->online||f->boost_enabled||f->gate_enabled||f->input_suspended)return NP_BUCK_SOURCE;
 if(!f->pump_known_off)return NP_BUCK_PUMP_ACTIVE;
 if(f->icl_ma<100||f->icl_ma>5000)return NP_BUCK_BASELINE;
 if(f->vbus_mv<4500||f->vbus_mv>5500||f->battery_mv<2500||f->battery_mv>4600||
    f->policy_fv<3500||f->policy_fv>4500)return NP_BUCK_VOLTAGE;
 if(f->fault)return NP_BUCK_FAULT;
 switch(f->real_type){
 case 1:*icl=500;*fcc=800;break;
 case 2:*icl=900;*fcc=900;break;
 case 3:*icl=1500;*fcc=1500;break;
 case 10:case 11:case 12:{
  NpPdChoice choice;
  if(f->pd_state!=2||(f->data_role!=1&&f->data_role!=2))return NP_BUCK_ROLE;
  if(np_pd_choose(f->pd,f->epoch,5000,1500,0,&choice)||choice.pps)return NP_BUCK_CAPS;
  *icl=choice.ma;*fcc=1500;break;
 }
 default:return NP_BUCK_SOURCE;
 }
 *fcc=minimum(*fcc,f->policy_fcc);
 if(f->policy_enabled&&*fcc<100)return NP_BUCK_BASELINE;
 return NP_BUCK_READY;
}
static int uncertainty(NpBuck *b)
{b->state=NP_BUCK_UNCERTAIN;b->blocked=NP_BUCK_EFFECT_UNKNOWN;return -1;}
static int fresh_for_write(NpBuck *b,const NpBuckFacts *f)
{
 /* Keep the existing3s send-time freshness gate valid throughout the
  * existing command window. Stop/reduction cleanup uses its own path. */
 if(f->now_ms-f->sampled_ms>3000u-NP_BUCK_COMMAND_MS){b->blocked=NP_BUCK_STALE;return 0;}
 return 1;
}
int np_buck_stock_confirming(const NpBuck *b)
{
 return b&&b->backend==NP_BUCK_OS4_STOCK_POLICY&&b->stock.confirm_until_ms&&
  (b->state==NP_BUCK_POLICY_CHECK||b->state==NP_BUCK_POLICY_DISABLED_CHECK||
   b->state==NP_BUCK_POLICY_RESTORE_CHECK);
}
static int confirm_read(NpBuck *b,const NpBuckFacts *f,NpBuckCommand *out,uint32_t property)
{
 if(b->stock.confirm_until_ms&&f->now_ms>=b->stock.confirm_until_ms)return uncertainty(b);
 int result=np_buck_issue(b,f,out,0,property,0);
 if(result>0&&b->stock.confirm_until_ms&&b->command.deadline_ms>b->stock.confirm_until_ms){
  b->command.deadline_ms=b->stock.confirm_until_ms;*out=b->command;
 }
 return result;
}
static int set_field(NpBuck *b,const NpBuckFacts *f,NpBuckCommand *out,unsigned i,uint32_t value)
{
 if(value&&!fresh_for_write(b,f))return 0;
 b->stock.index=i;b->stock.target[i]=value;b->state=NP_BUCK_POLICY_CHECK;
 return np_buck_issue(b,f,out,1,fields[i],value);
}
static void stop_begin(NpBuck *b)
{b->stop=1;b->state=NP_BUCK_POLICY_DISABLE;b->stock.stop_samples=0;b->stock.confirm_until_ms=0;}
int np_buck_stock_next(NpBuck *b,const NpBuckFacts *f,NpBuckCommand *out)
{
 NpBuckStock *s=&b->stock;uint32_t icl=0,fcc=0;unsigned reason=qualified(f,&icl,&fcc);
 b->blocked=reason;if(!b->armed)return 0;
 if(b->state==NP_BUCK_IDLE){
  if(b->stop){b->armed=0;b->state=NP_BUCK_FINISHED;return 0;}
  if(reason)return 0;
  memset(s,0,sizeof(*s));
  b->epoch=f->epoch;b->attachment=f->attachment;
  s->baseline[0]=s->actual[0]=f->icl_ma;s->baseline[1]=s->actual[1]=f->term_mv;
  s->baseline[3]=s->actual[3]=f->charge_enabled;s->seen=11;s->configured=1;
  b->baseline_icl=f->icl_ma;b->state=NP_BUCK_POLICY_SYNC;
 }
 const int exiting=b->state>=NP_BUCK_POLICY_DISABLE;
 if(!exiting&&!np_buck_stock_confirming(b)&&(b->stop||reason||b->attachment!=f->attachment)){
  if(!b->owned&&!b->unverified_effect){b->armed=0;b->state=NP_BUCK_FINISHED;return 0;}
  stop_begin(b);
 }
 switch(b->state){
 case NP_BUCK_POLICY_CHECK:
  return confirm_read(b,f,out,fields[s->index]);
 case NP_BUCK_POLICY_SYNC:{
  if(!f->policy_enabled){
   if(s->actual[3])return set_field(b,f,out,3,0);
   b->state=NP_BUCK_POLICY_MONITOR;return 0;
  }
  /* Reduce/init the unknown FCC before other limits or enabling charging.
   * Later increases follow actual ICL/FV readback and four cohorts. */
  const uint32_t target=minimum(fcc,1500);
  if(!(b->owned&2)||b->target_fcc>target){
   if(!fresh_for_write(b,f))return 0;
   b->target_fcc=(b->owned&2)?target:minimum(target,500);
   b->state=NP_BUCK_POLICY_FCC_CHECK;
   return np_buck_issue(b,f,out,1,NP_BUCK_FCC,b->target_fcc);
  }
  const uint32_t wanted_icl=minimum(icl,1500);
  /* GET2008 is the Min budget, not this HLOS client's vote. Keep an ACKed
   * vote when another client clamps the budget instead of resending it. */
  const uint32_t requested_icl=(b->owned&1)?s->target[0]:s->actual[0];
  if(requested_icl!=wanted_icl)return set_field(b,f,out,0,wanted_icl);
  if(s->actual[1]!=f->policy_fv)return set_field(b,f,out,1,f->policy_fv);
  if(!s->actual[3])return set_field(b,f,out,3,1);
  if(b->target_fcc!=target){
   if(!fresh_for_write(b,f))return 0;
   b->target_fcc=target;b->state=NP_BUCK_POLICY_FCC_CHECK;
   return np_buck_issue(b,f,out,1,NP_BUCK_FCC,target);
  }
  b->state=NP_BUCK_POLICY_MONITOR;return 0;
 }
 case NP_BUCK_POLICY_FCC_CHECK:
  if(f->now_ms>b->feedback_after_ms&&f->now_ms-b->feedback_after_ms>20000){++b->failures;stop_begin(b);return 0;}
  if(f->gauge_ms<=b->feedback_after_ms||f->gauge_ms<=b->feedback_last_ms)return 0;
  if(f->battery_ma>(int32_t)b->target_fcc+150){++b->failures;stop_begin(b);return 0;}
  b->feedback_last_ms=f->gauge_ms;b->feedback_sum+=f->battery_ma;++b->feedback_samples;
  if(b->feedback_samples<4)return 0;
  b->feedback_target_ma=(int32_t)(b->feedback_sum/b->feedback_samples);
  b->confirmation=NP_BUCK_CONFIRM_CURRENT;b->unverified_effect=0;
  b->state=NP_BUCK_POLICY_SYNC;return 0;
 case NP_BUCK_POLICY_MONITOR:
  if(f->policy_enabled&&f->icl_ma>minimum(icl,1500)){++b->failures;stop_begin(b);return 0;}
  if(f->policy_enabled&&f->battery_ma>(int32_t)b->target_fcc+150){++b->failures;stop_begin(b);return 0;}
  if(f->policy_enabled!=(uint32_t)s->actual[3]||
     (f->policy_enabled&&(((b->owned&1)?s->target[0]:s->actual[0])!=minimum(icl,1500)||s->actual[1]!=f->policy_fv||
       b->target_fcc!=minimum(fcc,1500))))b->state=NP_BUCK_POLICY_SYNC;
  return 0;
 case NP_BUCK_POLICY_DISABLE:
  s->index=3;s->target[3]=0;b->state=NP_BUCK_POLICY_DISABLED_CHECK;
  if(s->actual[3]==0&&f->valid&&f->charge_enabled==0)
   return np_buck_issue(b,f,out,0,NP_BUCK_ENABLE,0);
  return np_buck_issue(b,f,out,1,NP_BUCK_ENABLE,0);
 case NP_BUCK_POLICY_DISABLED_CHECK:
  return confirm_read(b,f,out,NP_BUCK_ENABLE);
 case NP_BUCK_POLICY_STOP_OBSERVE:
  if(f->now_ms>s->stop_after_ms&&f->now_ms-s->stop_after_ms>20000)return uncertainty(b);
  if(!f->valid||f->gauge_ms<=s->stop_after_ms||f->gauge_ms<=s->stop_last_ms||
     f->pmic_charging||f->battery_ma>150)return 0;
  s->stop_last_ms=f->gauge_ms;if(++s->stop_samples<2)return 0;
  if(!b->stop){b->state=NP_BUCK_POLICY_MONITOR;return 0;}
  if(b->owned&2){
   b->restore_value=minimum(b->target_fcc,100);b->stock.check_stopping=1;
   return np_buck_issue(b,f,out,1,NP_BUCK_FCC,b->restore_value);
  }
  s->index=0;b->state=NP_BUCK_POLICY_RESTORE_READ;return 0;
 case NP_BUCK_POLICY_RESTORE_READ:
  while(s->index<3&&!(b->owned&owner[s->index]))++s->index;
  if(s->index==3){
   /* A confirmed disabled buck is handed to firmware; no stale full/hot
    * condition can restore enable1 during system shutdown or source loss. */
   b->owned&=~16u;b->armed=0;b->unverified_effect=0;
   b->confirmation=NP_BUCK_CONFIRM_REDUCTION_ACK;b->state=NP_BUCK_FINISHED;return 0;
  }
  if(s->index==0){
   /* There is no HLOS vote getter or MCA withdrawal operation. Hand back
    * the disabled device with the documented100mA HLOS vote, not a guessed
    * client baseline or a claim that a changed aggregate withdrew our vote. */
   b->restore_value=100;b->state=NP_BUCK_POLICY_RESTORE_WRITE;
   return np_buck_issue(b,f,out,1,NP_BUCK_ICL,100);
  }
  return np_buck_issue(b,f,out,0,fields[s->index],0);
 case NP_BUCK_POLICY_RESTORE_WRITE:
  return np_buck_issue(b,f,out,1,fields[s->index],b->restore_value);
 case NP_BUCK_POLICY_RESTORE_CHECK:
  return confirm_read(b,f,out,fields[s->index]);
 default:return uncertainty(b);
 }
}
int np_buck_stock_complete(NpBuck *b,int status,uint32_t value)
{
 NpBuckStock *s=&b->stock;
 if(b->command.property==NP_BUCK_FCC){
  if(status&&!b->wire_accepted){
   b->unverified_effect=0;
   if(!b->owned){b->armed=0;b->state=NP_BUCK_FINISHED;return 1;}
   if(s->check_stopping)return uncertainty(b);
   stop_begin(b);return 1;
  }
  b->owned|=2;
  if(s->check_stopping){
   s->check_stopping=0;if(status)return uncertainty(b);
   ++b->changes;b->owned&=~2u;b->unverified_effect=0;
   s->index=0;b->state=NP_BUCK_POLICY_RESTORE_READ;return 1;
  }
  if(status){stop_begin(b);return 1;}
  ++b->changes;b->feedback_after_ms=b->command.deadline_ms;b->feedback_last_ms=0;
  b->feedback_sum=0;b->feedback_samples=0;b->state=NP_BUCK_POLICY_FCC_CHECK;return 1;
 }
 unsigned i=s->index;
 if(i>3)return uncertainty(b);
 if(b->command.write){
  if(!status||b->wire_accepted)b->owned|=owner[i];
  if(i==0&&status&&b->wire_accepted){
   /* A lower aggregate can exist without this write. Only an ACKed vote
    * can be handed back; compensate a lost configuration ACK once with100,
    * but never replay a lost100mA handoff write. */
   if(b->state==NP_BUCK_POLICY_RESTORE_WRITE)return uncertainty(b);
   stop_begin(b);return 1;
  }
  if(i==0&&!status&&b->state==NP_BUCK_POLICY_RESTORE_WRITE)s->target[0]=b->restore_value;
  s->confirm_until_ms=(!status||b->wire_accepted)?b->command.deadline_ms:0;
  if(status)b->stop=1;
  if(!status)++b->changes;
  if(status&&!b->wire_accepted){
   b->unverified_effect=0;
   if(!b->owned){b->armed=0;b->state=NP_BUCK_FINISHED;}
   else if(b->state>=NP_BUCK_POLICY_DISABLE)return uncertainty(b);
   else stop_begin(b);
  }
  else if(b->state==NP_BUCK_POLICY_RESTORE_WRITE)b->state=NP_BUCK_POLICY_RESTORE_CHECK;
  return 1;
 }
 if(status||!valid_field(i,value))return uncertainty(b);
 if(b->state==NP_BUCK_POLICY_RESTORE_READ){
  if(value!=s->actual[i]){b->owned&=~owner[i];++s->index;return 1;}
  b->restore_value=i==0?minimum(minimum(s->baseline[i],value),100):minimum(s->baseline[i],value);
  b->state=NP_BUCK_POLICY_RESTORE_WRITE;return 1;
 }
 if(b->state==NP_BUCK_POLICY_RESTORE_CHECK){
  if(i==0?value>b->restore_value:value!=b->restore_value)return s->confirm_until_ms?1:uncertainty(b);
  s->confirm_until_ms=0;
  b->owned&=~owner[i];b->unverified_effect=0;s->actual[i]=value;
  ++s->index;b->state=NP_BUCK_POLICY_RESTORE_READ;return 1;
 }
 if(i==0?value>s->target[i]:value!=s->target[i])return s->confirm_until_ms?1:uncertainty(b);
 s->confirm_until_ms=0;
 s->actual[i]=value;b->unverified_effect=0;
 if(i!=0&&value==s->baseline[i])b->owned&=~owner[i];
 if(b->state==NP_BUCK_POLICY_DISABLED_CHECK||
    (b->state==NP_BUCK_POLICY_CHECK&&i==3&&value==0)){
  s->stop_after_ms=b->command.deadline_ms;s->stop_last_ms=0;s->stop_samples=0;
  b->state=NP_BUCK_POLICY_STOP_OBSERVE;
 }else if(b->stop)stop_begin(b);
 else if(i==3&&value==1){
  b->feedback_after_ms=b->command.deadline_ms;b->feedback_last_ms=0;
  b->feedback_sum=0;b->feedback_samples=0;b->confirmation=NP_BUCK_CONFIRM_NONE;
  b->state=NP_BUCK_POLICY_FCC_CHECK;
 }
 else b->state=NP_BUCK_POLICY_SYNC;
 return 1;
}
int np_buck_stock_write_allowed(const NpBuck *b,const NpBuckFacts *f,uint32_t property,uint32_t value)
{
 if(!b||!f||!b->pending||!b->command.write||property!=b->command.property||
    value!=b->command.value||f->now_ms>=b->command.deadline_ms)return 0;
 const NpBuckStock *s=&b->stock;
 if(property==NP_BUCK_ENABLE&&value==0)
  return b->state==NP_BUCK_POLICY_DISABLED_CHECK||
    (b->state==NP_BUCK_POLICY_CHECK&&s->index==3&&s->target[3]==0);
 if(property==NP_BUCK_FCC&&s->check_stopping)
  return b->state==NP_BUCK_POLICY_STOP_OBSERVE&&value==b->restore_value&&value<=b->target_fcc;
 if(b->state==NP_BUCK_POLICY_RESTORE_WRITE)
  return s->index<3&&property==fields[s->index]&&value==b->restore_value&&
    value<=(s->index==0?s->target[0]:s->actual[s->index])&&(b->owned&owner[s->index]);
 uint32_t icl=0,fcc=0;
 if(b->stop||qualified(f,&icl,&fcc)||!f->policy_enabled)return 0;
 if(property==NP_BUCK_FCC)
  return b->state==NP_BUCK_POLICY_FCC_CHECK&&value==b->target_fcc&&value>=100&&value<=minimum(fcc,1500);
 if(b->state!=NP_BUCK_POLICY_CHECK||s->index>3||property!=fields[s->index]||value!=s->target[s->index])return 0;
 if(s->index==0)return value>=100&&value<=minimum(icl,1500);
 if(s->index==1)return value>=3500&&value<=f->policy_fv;
 return s->index==3&&value==1&&s->configured&&
   s->actual[0]>=100&&s->actual[0]<=minimum(icl,1500)&&
   (!(b->owned&1)||s->target[0]==minimum(icl,1500))&&s->actual[1]==f->policy_fv;
}
