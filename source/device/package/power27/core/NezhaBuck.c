/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaBuck.h"
#include <string.h>
static uint32_t lesser(uint32_t a,uint32_t b){return a<b?a:b;}
void np_buck_init(NpBuck *b){memset(b,0,sizeof(*b));}
int np_buck_arm(NpBuck *b,uint64_t now,uint32_t lease)
{
 if(!b||b->armed||b->pending||b->owned||b->state==NP_BUCK_UNCERTAIN||
    (b->backend!=NP_BUCK_OS4_STOCK_POLICY&&(lease<1000||lease>300000))||
    (b->backend==NP_BUCK_OS4_STOCK_POLICY&&lease)||now>UINT64_MAX-lease)return -1;
 b->armed=1;b->stop=0;b->failed_write=0;b->epoch=b->attachment=0;
 b->state=NP_BUCK_IDLE;b->lease_until=now+lease;return 0;
}
void np_buck_stop(NpBuck *b){if(b)b->stop=1;}
static uint32_t budget(const NpBuckFacts *f,uint32_t *fcc)
{
 /* Current nezha FDT: SDP500/800, CDP900/900, DCP1600/2000,
  * PD1600/3000mA. Conservative ceiling is1500, below failed-auth FCC2000. */
 switch(f->real_type) {
 case 1:*fcc=800;return 500;
 case 2:*fcc=900;return 900;
 case 3:*fcc=1500;return 1500;
 case 10:case 11:case 12: {
  NpPdChoice choice;
  if(np_pd_choose(f->pd,f->epoch,5000,1500,0,&choice))return 0;
  *fcc=1500;return choice.ma;
 }
 default:return 0;
 }
}
static unsigned qualify(const NpBuckFacts *f,uint32_t *icl,uint32_t *fcc,unsigned backend)
{
 if(!f->valid||!f->epoch||!f->attachment||f->now_ms<f->sampled_ms||
    f->now_ms-f->sampled_ms>3000)return NP_BUCK_STALE;
 if(!f->online||f->boost_enabled||f->gate_enabled||f->input_suspended)return NP_BUCK_SOURCE;
 if(f->temperature_centi_c<1800||f->temperature_centi_c>=3500)return NP_BUCK_TEMPERATURE;
 if(f->vbus_mv<4500||f->vbus_mv>(backend==NP_BUCK_OS4_FIXED_PD?9500u:5500u)||f->battery_mv<3000||f->battery_mv>=4460||
    f->term_mv<4100||f->term_mv>4490)return NP_BUCK_VOLTAGE;
 if(!f->pump_known_off)return NP_BUCK_PUMP_ACTIVE;
 if(f->icl_ma<100||f->icl_ma>5000||
    (backend==NP_BUCK_READBACK&&(f->fcc_ma<100||f->fcc_ma>10000)))return NP_BUCK_BASELINE;
 if(f->fault||f->full||!f->charge_enabled||!f->pmic_charging)return NP_BUCK_FAULT;
 *icl=budget(f,fcc);return *icl?NP_BUCK_READY:NP_BUCK_CAPS;
}
int np_buck_issue(NpBuck *b,const NpBuckFacts *f,NpBuckCommand *out,unsigned write,uint32_t property,uint32_t value)
{
 if(b->next_id==UINT64_MAX||f->now_ms>UINT64_MAX-NP_BUCK_COMMAND_MS){b->state=NP_BUCK_UNCERTAIN;return -1;}
 b->command=(NpBuckCommand){++b->next_id,b->epoch,f->now_ms+NP_BUCK_COMMAND_MS,property,value,(uint8_t)write};
 if(write)b->unverified_effect=1;
 b->wire_accepted=0;
 b->request_id=b->command.id;b->pending=1;*out=b->command;return 1;
}
#define issue np_buck_issue
/* OS4 exposes set_ichg without get_ichg. This separate backend never turns
 * READ0x2005=0 into a limit, or claims register readback. Initialize 500mA,
 * observe the battery current, then make one bounded 1000mA request. ICL,
 * voltage, enable, pump and protection settings remain with their owners. */
static void feedback_window_reset(NpBuck *b)
{
 b->feedback_sum=0;b->feedback_samples=0;
 b->feedback_min=INT32_MAX;b->feedback_max=INT32_MIN;
}
static void feedback_reset(NpBuck *b)
{
 b->feedback_after_ms=b->command.deadline_ms;
 b->feedback_last_ms=0;feedback_window_reset(b);
}
static int feedback(NpBuck *b,const NpBuckFacts *f)
{
 if(f->gauge_ms<=b->feedback_after_ms||f->gauge_ms<=b->feedback_last_ms)return 0;
 if(f->gauge_ms>f->now_ms||f->now_ms-f->gauge_ms>3000)return -1;
 b->feedback_last_ms=f->gauge_ms;b->feedback_sum+=f->battery_ma;++b->feedback_samples;
 if(f->battery_ma<b->feedback_min)b->feedback_min=f->battery_ma;
 if(f->battery_ma>b->feedback_max)b->feedback_max=f->battery_ma;
 return b->feedback_samples>=4?1:0;
}
static int setter_next(NpBuck *b,const NpBuckFacts *f,NpBuckCommand *out)
{
 uint32_t icl=0,fcc=0;unsigned reason=qualify(f,&icl,&fcc,NP_BUCK_OS4_SETTER);
 b->blocked=reason;
 if(!b->armed)return 0;
 if(b->state==NP_BUCK_IDLE) {
  if(b->stop||f->now_ms>=b->lease_until){b->armed=0;b->state=NP_BUCK_FINISHED;return 0;}
  if(reason)return 0;
  /* This trial does not change ICL. A real existing limit must cover the
   * requested ordinary charge budget before claiming the FCC setter. */
  if((f->real_type!=3&&f->real_type!=10)||f->icl_ma<icl){b->blocked=NP_BUCK_BASELINE;return 0;}
  b->epoch=f->epoch;b->attachment=f->attachment;
  b->baseline_icl=b->target_icl=f->icl_ma;b->baseline_fcc=0;
  b->target_fcc=lesser(fcc,1000);b->state=NP_BUCK_FCC_INITIALIZE;
 }
 if(b->stop||f->now_ms>=b->lease_until||reason||b->attachment!=f->attachment||
    f->battery_ma>1500) {
  b->stop=1;
  if(b->state==NP_BUCK_FCC_INITIALIZE&&!b->unverified_effect&&!b->owned) {
   b->armed=0;b->state=NP_BUCK_FINISHED;return 0;
  }
  uint32_t reduce=!reason&&b->attachment==f->attachment&&f->battery_ma<=1500&&
    !b->failures&&b->confirmation==NP_BUCK_CONFIRM_CURRENT?500:100;
  if(b->state==NP_BUCK_FCC_REDUCE&&b->restore_value==100)reduce=100;
  b->restore_value=reduce;
  b->state=NP_BUCK_FCC_REDUCE;
 }
 switch(b->state) {
 case NP_BUCK_FCC_INITIALIZE:return issue(b,f,out,1,NP_BUCK_FCC,500);
 case NP_BUCK_SET_FCC:return issue(b,f,out,1,NP_BUCK_FCC,b->target_fcc);
 case NP_BUCK_FCC_BASELINE:case NP_BUCK_FCC_OBSERVE: {
  b->blocked=NP_BUCK_FEEDBACK;
  if(f->now_ms>b->feedback_after_ms&&f->now_ms-b->feedback_after_ms>20000) {
   ++b->failures;b->stop=1;b->restore_value=100;b->state=NP_BUCK_FCC_REDUCE;return 0;
  }
  int got=feedback(b,f);if(!got)return 0;
  int32_t mean=b->feedback_samples?(int32_t)(b->feedback_sum/b->feedback_samples):0;
  if(got<0) {
   ++b->failures;b->stop=1;b->restore_value=100;b->state=NP_BUCK_FCC_REDUCE;return 0;
  }
  if((int64_t)b->feedback_max-b->feedback_min>150||
     (b->state==NP_BUCK_FCC_BASELINE?(mean<200||mean>650):
       (mean<b->feedback_baseline_ma+250||mean>1200))) {
   /* Load and taper can disturb the first cohort. Await another four real
    * samples inside the original deadline; no higher request is qualified. */
   feedback_window_reset(b);return 0;
  }
  b->unverified_effect=0;b->blocked=NP_BUCK_READY;
  if(b->state==NP_BUCK_FCC_BASELINE) {
   b->feedback_baseline_ma=mean;b->confirmation=NP_BUCK_CONFIRM_BASELINE;b->state=NP_BUCK_SET_FCC;
  }else{
   b->feedback_target_ma=mean;b->confirmation=NP_BUCK_CONFIRM_CURRENT;b->state=NP_BUCK_RUNNING;
  }
  return 0;
 }
 case NP_BUCK_FCC_REDUCE:return issue(b,f,out,1,NP_BUCK_FCC,b->restore_value);
 default:return 0;
 }
}
static int setter_complete(NpBuck *b,int status)
{
 switch(b->state) {
 case NP_BUCK_FCC_INITIALIZE:case NP_BUCK_SET_FCC:
  if(status&&!b->wire_accepted&&!b->owned) {
   b->unverified_effect=0;b->stop=1;b->armed=0;b->state=NP_BUCK_FINISHED;return 1;
  }
  b->owned|=2;
  if(status){b->stop=1;b->restore_value=100;b->state=NP_BUCK_FCC_REDUCE;return 1;}
  ++b->changes;feedback_reset(b);
  b->state=b->state==NP_BUCK_FCC_INITIALIZE?NP_BUCK_FCC_BASELINE:NP_BUCK_FCC_OBSERVE;return 1;
 case NP_BUCK_FCC_REDUCE:
  if(status){b->state=NP_BUCK_UNCERTAIN;b->blocked=NP_BUCK_EFFECT_UNKNOWN;return -1;}
  ++b->changes;b->unverified_effect=0;b->owned=0;b->armed=0;
  b->confirmation=NP_BUCK_CONFIRM_REDUCTION_ACK;b->state=NP_BUCK_FINISHED;return 1;
 default:b->state=NP_BUCK_UNCERTAIN;return -1;
 }
}
/* Fixed-PD is the stock buck path (OS4 WRITE2101b), not PPS or pump startup.
 * owned2=FCC, owned4=USB OVP, owned8=voltage request possibly accepted.
 * Voltage ACKs are followed by new measured cohorts, including the exit. */
static int pd_low_voltage(const NpBuckFacts *f)
{
 return f->valid&&f->now_ms>=f->sampled_ms&&f->now_ms-f->sampled_ms<=3000&&
        f->vbus_mv<=5500&&f->pump_protection_valid&&
        f->pump_bus_mv>=0&&f->pump_bus_mv<=5500&&f->pump_known_off;
}
static void pd_voltage_reset(NpBuck *b,uint64_t after)
{
 b->voltage_after_ms=after;b->voltage_last_ms=0;b->voltage_samples=0;
}
static int pd_voltage_feedback(NpBuck *b,const NpBuckFacts *f,unsigned high)
{
 if(f->charger_ms<=b->voltage_after_ms||f->charger_ms<=b->voltage_last_ms)return 0;
 if(!f->valid||f->now_ms<f->sampled_ms||f->now_ms-f->sampled_ms>3000||
    !f->pump_known_off||!f->pump_protection_valid)return 0;
 if(high) {
  if(f->vbus_mv<8500||f->vbus_mv>9500||f->pump_bus_mv<8500||
     f->pump_bus_mv>9500)return 0;
 } else if(!pd_low_voltage(f))return 0;
 int32_t delta=(int32_t)f->vbus_mv-f->pump_bus_mv;
 if(delta>500||delta< -500)return -1;
 b->voltage_last_ms=f->charger_ms;++b->voltage_samples;
 return b->voltage_samples>=2?1:0;
}
static int fixed_pd_next(NpBuck *b,const NpBuckFacts *f,NpBuckCommand *out)
{
 uint32_t icl=0,fcc=0;unsigned reason=qualify(f,&icl,&fcc,NP_BUCK_OS4_FIXED_PD);
 if(!reason&&((f->real_type!=10&&f->real_type!=11&&f->real_type!=12)||f->pd_state!=2||
    (f->data_role!=1&&f->data_role!=2)))reason=NP_BUCK_ROLE;
 if(!reason&&(!f->pump_protection_valid||f->pump_die_centi_c>=5000))
  reason=NP_BUCK_PROTECTION;
 b->blocked=reason;if(!b->armed)return 0;
 if(b->state==NP_BUCK_IDLE) {
  if(b->stop||f->now_ms>=b->lease_until){b->armed=0;b->state=NP_BUCK_FINISHED;return 0;}
  if(reason)return 0;
  NpPdChoice choice;
  if(np_pd_choose(f->pd,f->epoch,9000,1500,0,&choice)||choice.pps||choice.ma<1500) {
   b->blocked=NP_BUCK_CAPS;return 0;
  }
  if(f->icl_ma<1500||f->vbus_mv>5500||
     (f->usb_ovp_raw!=15&&f->usb_ovp_raw!=0)){b->blocked=NP_BUCK_PROTECTION;return 0;}
  b->epoch=f->epoch;b->attachment=f->attachment;
  b->baseline_icl=b->target_icl=f->icl_ma;b->target_fcc=1500;b->baseline_fcc=0;
  b->usb_ovp_baseline=f->usb_ovp_raw;b->protection_restored=f->usb_ovp_raw==0;
  b->cleanup_failed=0;b->confirmed_vbus_mv=5000;b->state=NP_BUCK_FCC_INITIALIZE;
 }
 const unsigned exiting=b->state==NP_BUCK_FCC_REDUCE||
   b->state==NP_BUCK_PD_REQUEST_5V||b->state==NP_BUCK_PD_OBSERVE_5V||
   b->state==NP_BUCK_PD_RESTORE_PROTECTION;
 if(!exiting&&(b->stop||f->now_ms>=b->lease_until||reason||
    b->attachment!=f->attachment||f->battery_ma>1700||
    (b->state==NP_BUCK_RUNNING&&(f->vbus_mv<8500||f->usb_ovp_raw!=0)))) {
  b->stop=1;
  if(!b->owned&&!b->unverified_effect){b->armed=0;b->state=NP_BUCK_FINISHED;return 0;}
  b->restore_value=!reason&&b->attachment==f->attachment&&!b->failures&&
    b->confirmation==NP_BUCK_CONFIRM_CURRENT?500:100;
  b->state=NP_BUCK_FCC_REDUCE;
 }
 switch(b->state) {
 case NP_BUCK_FCC_INITIALIZE:return issue(b,f,out,1,NP_BUCK_FCC,500);
 case NP_BUCK_FCC_BASELINE:case NP_BUCK_FCC_OBSERVE: {
  b->blocked=NP_BUCK_FEEDBACK;
  if(f->now_ms>b->feedback_after_ms&&f->now_ms-b->feedback_after_ms>20000) {
   ++b->failures;b->stop=1;b->restore_value=100;b->state=NP_BUCK_FCC_REDUCE;return 0;
  }
  int got=feedback(b,f);if(!got)return 0;
  int32_t mean=b->feedback_samples?(int32_t)(b->feedback_sum/b->feedback_samples):0;
  if(got<0) {
   ++b->failures;b->stop=1;b->restore_value=100;b->state=NP_BUCK_FCC_REDUCE;return 0;
  }
  if((int64_t)b->feedback_max-b->feedback_min>150||
     (b->state==NP_BUCK_FCC_BASELINE?(mean<200||mean>650):
       (mean<b->feedback_baseline_ma+500||mean<1000||mean>1650))) {
   feedback_window_reset(b);return 0;
  }
  b->unverified_effect=0;b->blocked=NP_BUCK_READY;
  if(b->state==NP_BUCK_FCC_BASELINE) {
   b->feedback_baseline_ma=mean;b->confirmation=NP_BUCK_CONFIRM_BASELINE;b->state=NP_BUCK_PD_PROTECT;
  } else {
   b->feedback_target_ma=mean;b->confirmation=NP_BUCK_CONFIRM_CURRENT;b->state=NP_BUCK_RUNNING;
  }
  return 0;
 }
 case NP_BUCK_PD_PROTECT:
  if(f->usb_ovp_raw==0){b->state=NP_BUCK_PD_REQUEST_9V;return 0;}
  return issue(b,f,out,1,NP_BUCK_USB_OVP,15u<<8);
 case NP_BUCK_PD_REQUEST_9V:
  if(!f->pump_protection_valid||f->usb_ovp_raw!=0){b->blocked=NP_BUCK_PROTECTION;return 0;}
  {
   int got=issue(b,f,out,1,NP_BUCK_FIXED_PD,9000);
   if(got>0)pd_voltage_reset(b,b->command.deadline_ms);
   return got;
  }
 case NP_BUCK_PD_OBSERVE_9V:case NP_BUCK_PD_OBSERVE_5V: {
  unsigned high=b->state==NP_BUCK_PD_OBSERVE_9V;
  if(f->now_ms>b->voltage_after_ms&&f->now_ms-b->voltage_after_ms>20000) {
   ++b->failures;
   if(!high){b->state=NP_BUCK_UNCERTAIN;b->blocked=NP_BUCK_EFFECT_UNKNOWN;return -1;}
   b->stop=1;b->restore_value=100;b->state=NP_BUCK_FCC_REDUCE;return 0;
  }
  int got=pd_voltage_feedback(b,f,high);if(!got)return 0;
  if(got<0) {
   if(!high){b->state=NP_BUCK_UNCERTAIN;b->blocked=NP_BUCK_EFFECT_UNKNOWN;return -1;}
   ++b->failures;b->stop=1;b->restore_value=100;b->state=NP_BUCK_FCC_REDUCE;return 0;
  }
  b->confirmed_vbus_mv=high?9000:5000;b->unverified_effect=0;
  if(high)b->state=NP_BUCK_SET_FCC;
  else {b->owned&=~8u;b->state=NP_BUCK_PD_RESTORE_PROTECTION;}
  return 0;
 }
 case NP_BUCK_SET_FCC:
  if(f->vbus_mv<8500||f->pump_bus_mv<8500||f->usb_ovp_raw!=0) {
   b->stop=1;b->restore_value=100;b->state=NP_BUCK_FCC_REDUCE;return 0;
  }
  return issue(b,f,out,1,NP_BUCK_FCC,1500);
 case NP_BUCK_FCC_REDUCE:return issue(b,f,out,1,NP_BUCK_FCC,b->restore_value);
 case NP_BUCK_PD_REQUEST_5V:
  /* A queued9V request can outlive its ACK timeout. On the same PD source,
   * supersede it with5V even if a pre-request cohort still reports5V.
   * A later measured non-PD attachment has lost the old physical contract. */
  if(pd_low_voltage(f)&&(!(b->owned&8)||
     (f->attachment!=b->attachment&&f->real_type!=10&&
      f->charger_ms>b->voltage_after_ms))) {
   pd_voltage_reset(b,f->now_ms);b->state=NP_BUCK_PD_OBSERVE_5V;return 0;
  }
  if(!f->valid||!f->online||(f->real_type!=10&&f->real_type!=11&&f->real_type!=12)||f->pd_state!=2||
     (f->data_role!=1&&f->data_role!=2)||
     f->boost_enabled||f->gate_enabled){b->blocked=NP_BUCK_SOURCE;return 0;}
  return issue(b,f,out,1,NP_BUCK_FIXED_PD,5000);
 case NP_BUCK_PD_RESTORE_PROTECTION:
  if(!(b->owned&4)) {
   b->armed=0;b->state=(b->owned||b->cleanup_failed)?NP_BUCK_UNCERTAIN:NP_BUCK_FINISHED;return 0;
  }
  if(!pd_low_voltage(f)){b->blocked=NP_BUCK_VOLTAGE;return 0;}
  return issue(b,f,out,1,NP_BUCK_USB_OVP,15);
 default:return 0;
 }
}
static int fixed_pd_complete(NpBuck *b,int status,uint32_t value)
{
 switch(b->state) {
 case NP_BUCK_FCC_INITIALIZE:case NP_BUCK_SET_FCC:
  if(status&&!b->wire_accepted&&!b->owned) {
   b->unverified_effect=0;b->armed=0;b->state=NP_BUCK_FINISHED;return 1;
  }
  b->owned|=2;
  if(status){b->stop=1;b->restore_value=100;b->state=NP_BUCK_FCC_REDUCE;return 1;}
  ++b->changes;feedback_reset(b);
  b->state=b->state==NP_BUCK_FCC_INITIALIZE?NP_BUCK_FCC_BASELINE:NP_BUCK_FCC_OBSERVE;return 1;
 case NP_BUCK_PD_PROTECT:
  if(status||value!=0) {
   if(!b->wire_accepted) {
    b->unverified_effect=0;b->protection_restored=1;b->stop=1;b->restore_value=100;
    b->state=NP_BUCK_FCC_REDUCE;return 1;
   }
   b->state=NP_BUCK_UNCERTAIN;b->blocked=NP_BUCK_EFFECT_UNKNOWN;return -1;
  }
  ++b->changes;b->owned|=4;b->unverified_effect=0;b->state=NP_BUCK_PD_REQUEST_9V;return 1;
 case NP_BUCK_PD_REQUEST_9V:
  if(!status||b->wire_accepted)b->owned|=8;
  if(status){b->stop=1;b->restore_value=100;b->state=NP_BUCK_FCC_REDUCE;return 1;}
  ++b->changes;pd_voltage_reset(b,b->command.deadline_ms);b->state=NP_BUCK_PD_OBSERVE_9V;return 1;
 case NP_BUCK_FCC_REDUCE:
  if(status)b->cleanup_failed=1;
  else{++b->changes;b->owned&=~2u;b->confirmation=NP_BUCK_CONFIRM_REDUCTION_ACK;b->unverified_effect=0;}
  b->state=NP_BUCK_PD_REQUEST_5V;return 1;
 case NP_BUCK_PD_REQUEST_5V:
  if(status){b->state=NP_BUCK_UNCERTAIN;b->blocked=NP_BUCK_EFFECT_UNKNOWN;return -1;}
  ++b->changes;pd_voltage_reset(b,b->command.deadline_ms);b->state=NP_BUCK_PD_OBSERVE_5V;return 1;
 case NP_BUCK_PD_RESTORE_PROTECTION:
  if(status||value!=b->usb_ovp_baseline){b->state=NP_BUCK_UNCERTAIN;b->blocked=NP_BUCK_EFFECT_UNKNOWN;return -1;}
  ++b->changes;b->owned&=~4u;b->unverified_effect=0;b->protection_restored=1;b->armed=0;
  b->state=(b->owned||b->cleanup_failed)?NP_BUCK_UNCERTAIN:NP_BUCK_FINISHED;return 1;
 default:b->state=NP_BUCK_UNCERTAIN;return -1;
 }
}
int np_buck_next(NpBuck *b,const NpBuckFacts *f,NpBuckCommand *out)
{
 if(!b||!f||!out)return -1;
 memset(out,0,sizeof(*out));
 if(b->state==NP_BUCK_UNCERTAIN)return -1;
 if(b->epoch&&b->epoch!=f->epoch) {
  b->pending=0;b->armed=0;
  b->state=(b->owned||b->unverified_effect)?NP_BUCK_UNCERTAIN:NP_BUCK_FINISHED;
  b->blocked=NP_BUCK_EFFECT_UNKNOWN;return b->state==NP_BUCK_UNCERTAIN?-1:0;
 }
 if(b->pending)return 0; /* broker completes each request, including timeout */
 if(b->backend==NP_BUCK_OS4_STOCK_POLICY)return np_buck_stock_next(b,f,out);
 if(b->backend==NP_BUCK_OS4_FIXED_PD)return fixed_pd_next(b,f,out);
 if(b->backend==NP_BUCK_OS4_SETTER)return setter_next(b,f,out);
 uint32_t icl=0,fcc=0;
 unsigned reason=qualify(f,&icl,&fcc,NP_BUCK_READBACK);b->blocked=reason;
 b->restore_icl=!reason&&f->attachment==b->attachment?b->baseline_icl:lesser(b->baseline_icl,100);
 b->restore_fcc=!reason&&f->attachment==b->attachment?b->baseline_fcc:lesser(b->baseline_fcc,100);
 if(b->armed&&(b->stop||f->now_ms>=b->lease_until||
    (b->state!=NP_BUCK_IDLE&&(reason||b->attachment!=f->attachment)))) {
  b->stop=1;
  if(b->state<NP_BUCK_RESTORE_FCC_READ&&b->state!=NP_BUCK_CHECK_ICL&&b->state!=NP_BUCK_CHECK_FCC)
   b->state=NP_BUCK_RESTORE_FCC_READ;
 }
 if(!b->armed)return 0;
 switch(b->state) {
 case NP_BUCK_IDLE:
  if(reason)return 0;
  b->epoch=f->epoch;b->attachment=f->attachment;
  b->baseline_icl=f->icl_ma;b->baseline_fcc=f->fcc_ma;b->target_icl=icl;b->target_fcc=fcc;
  b->state=NP_BUCK_SET_ICL;
  /* fall through */
 case NP_BUCK_SET_ICL:
  if(b->baseline_icl>=b->target_icl){b->state=NP_BUCK_SET_FCC;return 0;}
  return issue(b,f,out,1,NP_BUCK_ICL,b->target_icl);
 case NP_BUCK_CHECK_ICL:return issue(b,f,out,0,NP_BUCK_ICL,0);
 case NP_BUCK_SET_FCC:
  if(b->baseline_fcc>=b->target_fcc){b->state=NP_BUCK_RUNNING;return 0;}
  return issue(b,f,out,1,NP_BUCK_FCC,b->target_fcc);
 case NP_BUCK_CHECK_FCC:return issue(b,f,out,0,NP_BUCK_FCC,0);
 case NP_BUCK_RUNNING:return 0;
 case NP_BUCK_RESTORE_FCC_READ:
  if(!(b->owned&2)){b->state=NP_BUCK_RESTORE_ICL_READ;return 0;}
  return issue(b,f,out,0,NP_BUCK_FCC,0);
 case NP_BUCK_RESTORE_FCC_WRITE:
  b->restore_value=lesser(b->restore_value,b->restore_fcc);
  return issue(b,f,out,1,NP_BUCK_FCC,b->restore_value);
 case NP_BUCK_RESTORE_FCC_CHECK:return issue(b,f,out,0,NP_BUCK_FCC,0);
 case NP_BUCK_RESTORE_ICL_READ:
  if(!(b->owned&1)){b->state=NP_BUCK_FINISHED;b->armed=0;return 0;}
  return issue(b,f,out,0,NP_BUCK_ICL,0);
 case NP_BUCK_RESTORE_ICL_WRITE:
  b->restore_value=lesser(b->restore_value,b->restore_icl);
  return issue(b,f,out,1,NP_BUCK_ICL,b->restore_value);
 case NP_BUCK_RESTORE_ICL_CHECK:return issue(b,f,out,0,NP_BUCK_ICL,0);
 default:return 0;
 }
}
int np_buck_complete(NpBuck *b,uint64_t id,int status,uint32_t value)
{
 if(!b||!b->pending||b->request_id!=id)return 0;
 b->pending=0;b->last_value=value;
 if(status)++b->failures;
 if(b->backend==NP_BUCK_OS4_STOCK_POLICY)return np_buck_stock_complete(b,status,value);
 if(b->backend==NP_BUCK_OS4_FIXED_PD)return fixed_pd_complete(b,status,value);
 if(b->backend==NP_BUCK_OS4_SETTER)return setter_complete(b,status);
 switch(b->state) {
 case NP_BUCK_SET_ICL:b->failed_write=!!status;b->state=NP_BUCK_CHECK_ICL;return 1;
 case NP_BUCK_SET_FCC:b->failed_write=!!status;b->state=NP_BUCK_CHECK_FCC;return 1;
 case NP_BUCK_CHECK_ICL:case NP_BUCK_CHECK_FCC: {
  unsigned icl=b->state==NP_BUCK_CHECK_ICL;
  uint32_t target=icl?b->target_icl:b->target_fcc,base=icl?b->baseline_icl:b->baseline_fcc;
  if(status||(value!=target&&value!=base)){b->state=NP_BUCK_UNCERTAIN;b->blocked=NP_BUCK_EFFECT_UNKNOWN;return -1;}
  b->unverified_effect=0;
  if(value==target){b->owned|=icl?1u:2u;++b->changes;}
  if(value!=target||b->failed_write||b->stop){b->stop=1;b->state=NP_BUCK_RESTORE_FCC_READ;}
  else b->state=icl?NP_BUCK_SET_FCC:NP_BUCK_RUNNING;
  return 1;
 }
 case NP_BUCK_RESTORE_FCC_READ:case NP_BUCK_RESTORE_ICL_READ: {
  unsigned icl=b->state==NP_BUCK_RESTORE_ICL_READ;
  if(status){b->state=NP_BUCK_UNCERTAIN;return -1;}
  if(value!=(icl?b->target_icl:b->target_fcc)) {
   b->owned&=~(icl?1u:2u);b->state=icl?NP_BUCK_FINISHED:NP_BUCK_RESTORE_ICL_READ;
   if(icl)b->armed=0;
   return 1; /* another policy changed it: never overwrite */
  }
  /* On a detach/unknown source, restore at most100mA. This is only a
   * reduction of our still-present setting, never a new source's increase. */
  b->restore_value=icl?b->restore_icl:b->restore_fcc;
  b->state=icl?NP_BUCK_RESTORE_ICL_WRITE:NP_BUCK_RESTORE_FCC_WRITE;return 1;
 }
 case NP_BUCK_RESTORE_FCC_WRITE:b->failed_write=!!status;b->state=NP_BUCK_RESTORE_FCC_CHECK;return 1;
 case NP_BUCK_RESTORE_ICL_WRITE:b->failed_write=!!status;b->state=NP_BUCK_RESTORE_ICL_CHECK;return 1;
 case NP_BUCK_RESTORE_FCC_CHECK:case NP_BUCK_RESTORE_ICL_CHECK: {
  unsigned icl=b->state==NP_BUCK_RESTORE_ICL_CHECK;
  if(status||value!=b->restore_value){b->state=NP_BUCK_UNCERTAIN;return -1;}
  b->unverified_effect=0;
  b->owned&=~(icl?1u:2u);b->state=icl?NP_BUCK_FINISHED:NP_BUCK_RESTORE_ICL_READ;
  if(icl)b->armed=0;
   return 1;
 }
 default:b->state=NP_BUCK_UNCERTAIN;return -1;
 }
}
