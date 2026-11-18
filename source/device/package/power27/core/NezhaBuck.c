/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaBuck.h"
#include "NezhaCharge.h"
#include <string.h>
static uint32_t lesser(uint32_t a,uint32_t b){return a<b?a:b;}
void np_buck_init(NpBuck *b){memset(b,0,sizeof(*b));}
int np_buck_arm(NpBuck *b,uint64_t now,uint32_t lease)
{
 const int continuous=b&&b->stock_continuous&&b->stock_current_enabled&&
   b->input_suspend_enabled&&(b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST||
                             b->backend==NP_BUCK_OS4_STOCK_POLICY);
 if(!b||b->armed||b->pending||b->owned||b->state==NP_BUCK_UNCERTAIN||
    (b->stock_continuous&&!continuous)||(continuous&&lease)||
    (!continuous&&b->backend!=NP_BUCK_OS4_STOCK_POLICY&&(lease<1000||lease>300000))||
    (b->backend==NP_BUCK_OS4_STOCK_POLICY&&lease)||now>UINT64_MAX-lease)return -1;
 b->armed=1;b->stop=0;b->failed_write=0;b->epoch=b->attachment=0;
 b->state=NP_BUCK_IDLE;b->lease_until=continuous?UINT64_MAX:now+lease;return 0;
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
 const uint32_t maximum_fv=backend==NP_BUCK_OS4_PPS_ADAPTER_TEST&&f->ffc_active?4530u:4490u;
 if(!f->valid||!f->epoch||!f->attachment||f->now_ms<f->sampled_ms||
    f->now_ms-f->sampled_ms>3000)return NP_BUCK_STALE;
 if(!f->online||f->boost_enabled||f->gate_enabled||f->input_suspended)return NP_BUCK_SOURCE;
 if(backend==NP_BUCK_OS4_PPS_OFF_TEST||backend==NP_BUCK_OS4_PPS_PUMP_TEST||(backend==NP_BUCK_OS4_PPS_CCCV_TEST||backend==NP_BUCK_OS4_PPS_ADAPTER_TEST)){
  /* Actual CN BM6N/F JEITA supplies the warm voltage/current ceilings.
   * This pump-off transport trial stays<=1500mA, requires the existing FV
   * to honor that ceiling, and keeps every source/ADC/OVP/current check. */
  if(f->temperature_centi_c<1800||!f->policy_valid||!f->policy_enabled||
     f->policy_fcc<1500||f->policy_fv<4100||f->policy_fv>maximum_fv||
     f->term_mv>f->policy_fv||f->battery_mv>=(int32_t)f->policy_fv)
   return NP_BUCK_TEMPERATURE;
 }else if(f->temperature_centi_c<1800||f->temperature_centi_c>=3500)return NP_BUCK_TEMPERATURE;
 if(f->vbus_mv<4500||f->vbus_mv>(backend==NP_BUCK_OS4_PPS_ADAPTER_TEST?20500u:(backend==NP_BUCK_OS4_FIXED_PD||backend==NP_BUCK_OS4_PPS_OFF_TEST||backend==NP_BUCK_OS4_PPS_PUMP_TEST||(backend==NP_BUCK_OS4_PPS_CCCV_TEST||backend==NP_BUCK_OS4_PPS_ADAPTER_TEST)?9500u:5500u))||f->battery_mv<3000||f->battery_mv>=(int32_t)((backend==NP_BUCK_OS4_PPS_CCCV_TEST||backend==NP_BUCK_OS4_PPS_ADAPTER_TEST)?maximum_fv:4460u)||
    f->term_mv<4100||f->term_mv>maximum_fv)return NP_BUCK_VOLTAGE;
 if(!f->pump_known_off&&backend!=NP_BUCK_OS4_PPS_PUMP_TEST&&backend!=NP_BUCK_OS4_PPS_CCCV_TEST&&backend!=NP_BUCK_OS4_PPS_ADAPTER_TEST)return NP_BUCK_PUMP_ACTIVE;
 if(f->icl_ma<100||f->icl_ma>((backend==NP_BUCK_OS4_PPS_OFF_TEST||backend==NP_BUCK_OS4_PPS_PUMP_TEST||(backend==NP_BUCK_OS4_PPS_CCCV_TEST||backend==NP_BUCK_OS4_PPS_ADAPTER_TEST))?10000u:5000u)||
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
static int pd_contract_gone(const NpBuck *b,const NpBuckFacts *f)
{
 if(!b->stock_continuous||!b->stop||!pd_low_voltage(f)||
    f->boost_enabled||f->gate_enabled)return 0;
 if(!f->online)return 1;
 if(f->epoch!=b->epoch||f->attachment==b->attachment)return 0;
 /* A new BC1.2/Type-C source cannot retain the detached adapter's PPS
  * contract. The matching factory USB_DISCONNECT clears that contract.
  * Keep unknown and PD sources on their existing negotiated exit path. */
 switch(f->real_type){case 1:case 2:case 3:case 4:case 9:case 15:return 1;default:return 0;}
}
static void pd_voltage_reset(NpBuck *b,uint64_t after)
{
 b->voltage_after_ms=after;b->voltage_last_ms=b->voltage_last_pump_ms=0;b->voltage_samples=0;
}
static int pd_voltage_feedback(NpBuck *b,const NpBuckFacts *f,unsigned high)
{
 if(f->charger_ms<=b->voltage_after_ms||f->charger_ms<=b->voltage_last_ms)return 0;
 if((b->backend==NP_BUCK_OS4_PPS_OFF_TEST||b->backend==NP_BUCK_OS4_PPS_PUMP_TEST||
     b->backend==NP_BUCK_OS4_PPS_CCCV_TEST||b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST)&&
    (f->pump_ms<=b->voltage_after_ms||f->pump_ms<=b->voltage_last_pump_ms))return 0;
 if(!f->valid||f->now_ms<f->sampled_ms||f->now_ms-f->sampled_ms>3000||
    !f->pump_known_off||!f->pump_protection_valid)return 0;
 if(high) {
  if(f->vbus_mv<8500||f->vbus_mv>9500||f->pump_bus_mv<8500||
     f->pump_bus_mv>9500)return 0;
 } else if(!pd_low_voltage(f))return 0;
 int32_t delta=(int32_t)f->vbus_mv-f->pump_bus_mv;
 if(delta>500||delta< -500)return -1;
 b->voltage_last_ms=f->charger_ms;b->voltage_last_pump_ms=f->pump_ms;++b->voltage_samples;
 return b->voltage_samples>=2?1:0;
}
static int pps_caps_ready(const NpBuckFacts *f)
{
 NpPdChoice choice;
 return f->icl_ma>=2000&&!np_pd_choose_pps(f->pd,f->epoch,9000,10000,&choice)&&
   choice.ma>=2000&&f->icl_ma<=choice.ma&&
   f->now_ms>=f->pd->at_ms&&f->now_ms-f->pd->at_ms<=3000;
}
static int fixed_pd_next(NpBuck *b,const NpBuckFacts *f,NpBuckCommand *out)
{
 const int pps=b->backend==NP_BUCK_OS4_PPS_OFF_TEST;
 uint32_t icl=0,fcc=0;unsigned reason=qualify(f,&icl,&fcc,b->backend);
 if(!reason&&((f->real_type!=10&&f->real_type!=11&&f->real_type!=12)||f->pd_state!=2||
    (f->data_role!=1&&f->data_role!=2)))reason=NP_BUCK_ROLE;
 if(!reason&&(!f->pump_protection_valid||f->pump_die_centi_c>=5000))
  reason=NP_BUCK_PROTECTION;
 if(!reason&&pps&&(b->state==NP_BUCK_SET_FCC||b->state==NP_BUCK_FCC_OBSERVE||b->state==NP_BUCK_RUNNING)&&
    (!pps_caps_ready(f)||f->vbus_mv<8500||f->vbus_mv>9500||
     f->pump_bus_mv<8500||f->pump_bus_mv>9500||
     (int32_t)f->vbus_mv-f->pump_bus_mv>500||f->pump_bus_mv-(int32_t)f->vbus_mv>500))
   reason=NP_BUCK_CAPS;
 b->blocked=reason;if(!b->armed)return 0;
 if(b->state==NP_BUCK_IDLE) {
  if(b->stop||f->now_ms>=b->lease_until){b->armed=0;b->state=NP_BUCK_FINISHED;return 0;}
  if(reason)return 0;
  NpPdChoice choice;
  const int selection=pps?!pps_caps_ready(f):
    np_pd_choose(f->pd,f->epoch,9000,1500,0,&choice)||choice.pps||choice.ma<1500;
  if(selection) {
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
   uint32_t value=9000;
   if(pps){
    if(!pps_caps_ready(f)||np_pps_encode(9000,2000,&value)){
     b->blocked=NP_BUCK_CAPS;return 0;
    }
   }
   int got=issue(b,f,out,1,pps?NP_BUCK_PPS_PD:NP_BUCK_FIXED_PD,value);
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
  if(high&&pps){
   if(!pps_caps_ready(f)||f->pd->at_ms<=b->voltage_after_ms)return 0;
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
static int pump_caps_ready(const NpBuckFacts *f,uint32_t mv)
{
 NpPdChoice choice;
 return f->icl_ma>=2000&&!np_pd_choose_pps(f->pd,f->epoch,mv,10000,&choice)&&
   choice.ma>=2000&&f->icl_ma<=choice.ma&&f->now_ms>=f->pd->at_ms&&
   f->now_ms-f->pd->at_ms<=3000;
}
static unsigned pump_ratio(const NpBuck *b)
{return b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST?b->adapter.ratio:2;}
static unsigned pump_mode(const NpBuck *b)
{return b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST?b->adapter.pump_mode:1;}
static uint32_t pump_request_ma(const NpBuck *b)
{return b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST?b->pump_request_ma:2000;}
uint32_t np_buck_pump_budget(const NpBuck *b,const NpBuckFacts *f)
{
 if(!b||!f)return 0;
 if(b->backend!=NP_BUCK_OS4_PPS_ADAPTER_TEST)return b->stock_current_enabled?0:4000;
 NpAdapterChoice choice;
 if(np_adapter_select(f->pd,f->epoch,f->real_type,f->stock_support100w,&choice)||
    !b->cccv.valid||b->cccv.current_max_ma<0)return 0;
 /* The OS4 curr_max channel and adapter votes are battery-side values.
  * The N0 single-channel non_mi_pps vote is11000/17600 for ratio2/4;
  * it cannot lower the corresponding single-channel11000/15600 vote.
  * Keep the4A experiment ceiling separately from the main-buck JEITA FCC. */
 uint32_t current=lesser(choice.single_battery_ma,choice.maximum_battery_ma);
 if(!b->stock_current_enabled)current=lesser(current,4000);
 current=lesser(current,choice.adapter_ma*choice.ratio);
 if(b->stock_current_enabled){
  /* This OS4 probe accepts only2x2 drop_fcc strings. N0 supplies six,
   * so the actual parser rejects that property and leaves the drop off.
   * Keep the working cycle-aware vstep/voltage rules separately. */
  if(!f->charger_thermal_enabled||!f->pump_controls_valid||
     !f->pump_bus_ocp_enabled||!f->pump_bus_ocp_ma)return 0;
  current=lesser(current,f->pump_bus_ocp_ma*choice.ratio);
 }
 current=lesser(current,(uint32_t)b->cccv.current_max_ma);
 if(f->charger_thermal_enabled)
  current=f->charger_thermal_valid?lesser(current,f->charger_thermal_ma):0;
 return current;
}
static uint32_t pump_adapter_budget(const NpBuck *b,const NpBuckFacts *f,uint32_t mv,int startup)
{
 const unsigned ratio=pump_ratio(b);
 if(!mv||(ratio!=2&&ratio!=4))return 0;
 uint32_t ma=np_buck_pump_budget(b,f)/ratio;
 if(b->stock_current_enabled){
  NpAdapterChoice choice;
  if(np_adapter_select(f->pd,f->epoch,f->real_type,f->stock_support100w,&choice)||
     !f->pump_bus_ocp_enabled||!f->pump_bus_ocp_ma)return 0;
  /* The matching N0 regulator writes the selected APDO current into its
   * PPS request (8098..80a8). curr_max remains the battery feedback limit;
   * dividing it by ratio would also subtract the system's supply budget.
   * Before confirmed FFC, retain the measured startup current bound. */
  if(!startup&&f->ffc_active)ma=choice.adapter_ma;
  ma=lesser(ma,f->pump_bus_ocp_ma);
  /* The N0 board's max_power=90 is an operating ceiling, not measured
   * wattage. Use the actual encoded voltage and floor the wire current. */
  ma=lesser(ma,90000000u/mv);
 }
 if(!b->stock_current_enabled||startup||!f->ffc_active)ma=lesser(ma,2000);
 return ma/50u*50u;
}
static int pump_current_changing(const NpBuck *b)
{
 return b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST&&
  (b->state==NP_BUCK_PUMP_REGULATE||b->state==NP_BUCK_PUMP_OBSERVE_REGULATION||
   b->state==NP_BUCK_PUMP_REQUEST_PPS||b->state==NP_BUCK_PUMP_OBSERVE_PPS)&&
  b->cccv_target_ma>=100&&b->cccv_target_ma!=b->pump_request_ma&&
  (b->stock_current_enabled||b->cccv_target_ma<b->pump_request_ma)&&
  b->cccv_target_mv&&(b->stock_current_enabled||!b->cccv_delta_mv);
}
static uint32_t pump_command_ma(const NpBuck *b)
{return pump_current_changing(b)?b->cccv_target_ma:pump_request_ma(b);}
static int selected_caps_ready(const NpBuck *b,const NpBuckFacts *f,uint32_t mv)
{
 if(b->backend!=NP_BUCK_OS4_PPS_ADAPTER_TEST)return pump_caps_ready(f,mv);
 NpAdapterChoice choice;
 /* GET2008 is the role0 main-buck limit. After our PPS ACK, use the fresh selected APDO for adapter current.
  * The actual N0 trace observed
  * the main-buck limit fall to100mA while that APDO still offered5A.
  * qualify() still requires a valid100..10000mA main-buck read. */
 if(np_adapter_select(f->pd,f->epoch,f->real_type,f->stock_support100w,&choice)||
    !b->adapter.valid||choice.ratio!=b->adapter.ratio||choice.pdo_index!=b->adapter.pdo_index||
    choice.minimum_mv>b->adapter.minimum_mv||choice.maximum_mv<b->adapter.maximum_mv||
    mv<choice.minimum_mv||mv>choice.maximum_mv||mv>20000||
    (!(b->owned&8)&&(f->icl_ma<2000||f->icl_ma>choice.adapter_ma))||
    pump_command_ma(b)<100||pump_command_ma(b)%50u||
    (b->stock_current_enabled&&(b->owned&64u)&&
     pump_command_ma(b)>b->pump_request_ma&&!f->ffc_active)||
    pump_command_ma(b)>pump_adapter_budget(b,f,mv,0)||
    f->now_ms<choice.sampled_ms||f->now_ms-choice.sampled_ms>3000)return 0;
 return 1;
}
int np_buck_pump_quiet(const NpBuck *b)
{return b&&(b->backend==NP_BUCK_OS4_PPS_PUMP_TEST||(b->backend==NP_BUCK_OS4_PPS_CCCV_TEST||b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST))&&
 (b->state==NP_BUCK_PUMP_QUIET||b->state==NP_BUCK_PUMP_READ_EXPIRY||b->state==NP_BUCK_UNCERTAIN);}
static unsigned pump_qualify(const NpBuck *b,const NpBuckFacts *f)
{
 const int suspended=b->input_suspend_enabled&&(b->owned&128u);
 NpBuckFacts charge=*f;
 /* Preserve raw telemetry: only this controller's input-suspend ownership
  * distinguishes a live sink with its main charging branch paused. */
 if(suspended){charge.input_suspended=0;charge.pmic_charging=1;}
 uint32_t icl=0,fcc=0;unsigned reason=qualify(&charge,&icl,&fcc,b->backend);
 if(reason)return reason;
 if((f->real_type!=10&&f->real_type!=11&&f->real_type!=12)||
    f->pd_state!=2||(f->data_role!=1&&f->data_role!=2))return NP_BUCK_ROLE;
 if(!f->pump_controls_valid||!f->pump_switching_valid||f->pump_faults||
    f->pump_die_centi_c>=(b->stock_continuous?12500:5000)||(f->pump_valid&22)!=22||
    f->now_ms<f->pump_ms||f->now_ms-f->pump_ms>3000)return NP_BUCK_PROTECTION;
 if(f->charger_thermal_enabled&&!f->charger_thermal_valid)return NP_BUCK_STALE;
 if((b->backend==NP_BUCK_OS4_PPS_CCCV_TEST||b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST)&&
    (!b->cccv.valid||b->cccv.epoch!=f->epoch||b->cccv.attachment!=f->attachment||
     f->now_ms<b->cccv.sampled_ms||f->now_ms-b->cccv.sampled_ms>3000||
     !f->main_buck_bus_valid||f->main_buck_bus_ma<0||f->pump_bus_ma<0||
     !(f->pump_valid&1)||f->pump_bus_mv<0||
     b->cccv.current_max_ma<=1500))return NP_BUCK_STALE;
 if((b->backend==NP_BUCK_OS4_PPS_CCCV_TEST||b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST)&&
    (int64_t)f->main_buck_bus_ma+f->pump_bus_ma>10000)return NP_BUCK_PROTECTION;
 if((b->state<=NP_BUCK_PUMP_START||b->state==NP_BUCK_PUMP_INPUT_SUSPEND||
     b->state==NP_BUCK_PUMP_OBSERVE_INPUT_SUSPEND)&&
    (!f->pump_known_off||f->pump_enable))return NP_BUCK_PUMP_ACTIVE;
 if(b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST&&!b->cccv.cycles_valid)return NP_BUCK_STALE;
 const uint32_t budget=np_buck_pump_budget(b,f);
 if(budget<=1500||(!b->stock_current_enabled&&
    (f->policy_fcc<4000||f->battery_ma>(int32_t)budget)))return NP_BUCK_TEMPERATURE;
 if(b->stock_current_enabled&&(!f->pump_bus_ocp_enabled||
    f->pump_bus_ma>(int32_t)f->pump_bus_ocp_ma||
    (b->adapter.valid&&f->battery_ma>(int32_t)b->adapter.single_battery_ma)))return NP_BUCK_PROTECTION;
 if(suspended&&b->input_suspend_confirmed&&
    (!f->main_buck_state_valid||!f->main_buck_bus_valid||
     f->main_buck_bus_ma<0||f->main_buck_bus_ma>150||
     (f->main_buck_state!=6&&f->main_buck_state!=7)))return NP_BUCK_BASELINE;
 const uint32_t command_mv=b->state==NP_BUCK_PUMP_REGULATE||
     b->state==NP_BUCK_PUMP_OBSERVE_REGULATION?b->cccv_target_mv:b->pump_target_mv;
 if(command_mv&&!selected_caps_ready(b,f,command_mv))return NP_BUCK_CAPS;
 if((b->state==NP_BUCK_PUMP_RUN||b->state==NP_BUCK_PUMP_REGULATE||
     b->state==NP_BUCK_PUMP_OBSERVE_REGULATION||
     (b->state==NP_BUCK_PUMP_OBSERVE_START&&f->pump_ms>b->pump_enabled_ms))&&
    (!f->pump_enable||f->pump_mode!=pump_mode(b)))return NP_BUCK_PUMP_ACTIVE;
 if((b->state==NP_BUCK_PUMP_RUN||b->state==NP_BUCK_PUMP_REGULATE||
     b->state==NP_BUCK_PUMP_OBSERVE_REGULATION)&&!f->pump_switching)return NP_BUCK_PUMP_ACTIVE;
 if(b->state==NP_BUCK_PUMP_OBSERVE_START||b->state==NP_BUCK_PUMP_RUN||
    b->state==NP_BUCK_PUMP_REGULATE||b->state==NP_BUCK_PUMP_OBSERVE_REGULATION){
  /*064 confirmed switching, then ordinary cable/load droop moved the PMIC
   *224mV below the acknowledged8980mV target. Keep the strict200mV startup
   * check; running observation uses the existing PPS500mV measured window. */
  const uint32_t target=b->state==NP_BUCK_PUMP_OBSERVE_REGULATION?b->cccv_target_mv:b->pump_target_mv;
  if(f->vbus_mv+500<target||f->vbus_mv>target+500||
     f->pump_bus_mv+500<(int32_t)target||
     f->pump_bus_mv>(int32_t)target+500||
     (int32_t)f->vbus_mv-f->pump_bus_mv>500||
     f->pump_bus_mv-(int32_t)f->vbus_mv>500)return NP_BUCK_VOLTAGE;
 }
 return NP_BUCK_READY;
}
static uint32_t cccv_target(const NpBuck *b,const NpBuckFacts *f)
{
 const int64_t bus=(int64_t)f->main_buck_bus_ma+f->pump_bus_ma;
 if(bus<0||bus>10000)return 0;
 /* The original regulator compares combined bus current with the selected
  * APDO maximum, independently of the current requested by this client.
  * The accepted request still bounds delivery; battery/curve/thermal votes
  * remain in limit_ma and govern every subsequent request. */
 NpAdapterChoice choice;
 if(b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST&&
    np_adapter_select(f->pd,f->epoch,f->real_type,f->stock_support100w,&choice))return 0;
 const uint32_t adapter_ma=b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST?
     choice.adapter_ma:pump_request_ma(b);
 const NpCccvInput input={f->battery_mv,f->battery_ma,(int32_t)bus,
  (int32_t)f->policy_fv,(int32_t)(b->stock_current_enabled?np_buck_pump_budget(b,f):
    lesser(f->policy_fcc,np_buck_pump_budget(b,f))),adapter_ma,f->real_type,
  f->now_ms,b->last_boost_ms,b->last_buck_ms,b->cccv_boost_done};
 const int32_t delta=np_cccv_delta_ratio(&b->cccv,&input,pump_ratio(b));
 const int32_t target=(int32_t)b->pump_target_mv+delta;
 const uint32_t minimum=b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST?b->adapter.minimum_mv:6500;
 const uint32_t maximum=b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST?lesser(b->adapter.maximum_mv,20000):9200;
 return !delta||target<(int32_t)minimum||target>(int32_t)maximum?0:(uint32_t)target/20*20;
}
static void input_reset(NpBuck *b)
{
 b->input_after_ms=b->command.deadline_ms;
 b->input_last_ms=b->input_last_gauge_ms=0;b->input_samples=0;
}
static int input_confirm(NpBuck *b,const NpBuckFacts *f,int suspended)
{
 if(f->now_ms>b->input_after_ms&&f->now_ms-b->input_after_ms>20000)return -1;
 const int detached=b->stock_continuous&&!f->online;
 if(!f->valid||(!f->online&&!detached)||!f->main_buck_state_valid||!f->main_buck_bus_valid||
    f->now_ms<f->sampled_ms||f->now_ms-f->sampled_ms>3000||
    f->charger_ms<=b->input_after_ms||f->charger_ms<=b->input_last_ms||
    f->gauge_ms<=b->input_after_ms||f->gauge_ms<=b->input_last_gauge_ms)return 0;
 if(suspended){
  if((f->main_buck_state!=6&&f->main_buck_state!=7)||
     f->main_buck_bus_ma<0||f->main_buck_bus_ma>150||f->battery_ma>150)return 0;
 }else if(detached){
  /* No adapter contract exists after unplug. The accepted suspend-clear
   * still owns its typed ACK; observe two fresh, deenergized cohorts before
   * ending that owner instead of waiting for charging to resume offline. */
  if(!pd_low_voltage(f)||f->main_buck_bus_ma<0||f->main_buck_bus_ma>150||
     f->battery_ma>150||f->boost_enabled||f->gate_enabled)return 0;
 }else{
  const int stopped_by_policy=b->stock_continuous&&f->policy_valid&&
    !f->policy_enabled&&(f->main_buck_state==5||f->main_buck_state==6||f->main_buck_state==7)&&
    f->main_buck_bus_ma<=150&&f->battery_ma<=150;
  if((!f->pmic_charging&&!stopped_by_policy)||f->input_suspended||f->main_buck_bus_ma<0||
     !pd_low_voltage(f)||f->boost_enabled||f->gate_enabled)return 0;
 }
 b->input_last_ms=f->charger_ms;b->input_last_gauge_ms=f->gauge_ms;
 return ++b->input_samples>=2;
}
static int stock_start_allowed(const NpBuck *b,const NpBuckFacts *f)
{
 const NpCccv *cc=&b->cccv;
 if(!cc->valid||!cc->cycles_valid||f->battery_soc<0||f->battery_soc>100)return 0;
 /* Original enable_cp_para: cycle upper bound, final-voltage margin,
  * exclusive SOC ceiling. The cycle drop is recovered in the same final
  * voltage coordinate already used by the original regulation table. */
 const int32_t cycles=cc->cycles;
 const int32_t margin=cycles<300?70:cycles<800?90:cycles<1200?200:250;
 const int32_t soc=cycles<300?90:cycles<800?85:cycles<1200?80:70;
 const int32_t last=cc->last_voltage_mv+(cycles>=300?20:0);
 return last-f->battery_mv>margin&&f->battery_soc<soc;
}
static int normal_source_ready(const NpBuckFacts *f)
{
 return f->valid&&f->epoch&&f->attachment&&f->online&&!f->fault&&
  f->policy_valid&&f->charger_thermal_enabled&&f->charger_thermal_valid&&
  !f->boost_enabled&&!f->gate_enabled&&f->now_ms>=f->sampled_ms&&
  f->now_ms-f->sampled_ms<=3000;
}
static int stock_normal_fallback(const NpBuck *b,const NpBuckFacts *f)
{
 if(f->real_type==1||f->real_type==2||f->real_type==3)return 1;
 const NpCccv *cc=&b->cccv;const NpPdCaps *pd=f->pd;
 /* Authentication clears its old catalog. Until a fresh catalog and
  * curve arrive, budget0 means unavailable, not an effective low vote.
  * Keep IDLE so the existing metadata worker can finish that refresh. */
 if(!pd||!pd->valid||pd->invalid_mask||pd->epoch!=f->epoch||
    f->now_ms<pd->at_ms||f->now_ms-pd->at_ms>3000||
    !cc->valid||!cc->cycles_valid||cc->epoch!=f->epoch||cc->attachment!=f->attachment||
    f->now_ms<cc->sampled_ms||f->now_ms-cc->sampled_ms>3000)return 0;
 NpAdapterChoice choice;
 if(np_adapter_select(pd,f->epoch,f->real_type,f->stock_support100w,&choice))return 1;
 if(!f->pump_controls_valid||!f->pump_bus_ocp_enabled||!f->pump_bus_ocp_ma)return 0;
 return !stock_start_allowed(b,f)||np_buck_pump_budget(b,f)<=1500;
}
int np_buck_prepare_reentry(const NpBuck *normal,const NpBuckFacts *f,
                          NpBuck *next,uint32_t *icl)
{
 if(!normal||!f||!next||!icl||normal->backend!=NP_BUCK_OS4_STOCK_POLICY||
    !normal->stock_current_enabled||!normal->normal_fallback||!normal->armed||
    normal->stop||normal->pending||normal->unverified_effect||normal->failures||
    normal->cleanup_failed||normal->state!=NP_BUCK_POLICY_MONITOR||
    normal->stock.confirm_until_ms||!normal->stock.configured||
    normal->epoch!=f->epoch||normal->attachment!=f->attachment||
    f->now_ms>=normal->lease_until||!normal_source_ready(f)||
    f->now_ms-f->sampled_ms>3000u-NP_BUCK_COMMAND_MS||
    !f->policy_enabled||f->full||!f->pump_known_off||
    f->vbus_mv<4500||f->vbus_mv>5500||f->pump_bus_mv<4500||f->pump_bus_mv>5500)
  return 0;
 NpPdChoice fixed;
 const uint32_t prior=normal->stock.baseline[0];
 /* Initial cache and an earlier source's100mA handoff are not capabilities.
  * The current5V PDO bounds release of this broker's ACKed ordinary HLOS
  * vote. Readback must expose any remaining limit before pump admission. */
 const uint32_t limit=normal->stock_continuous?5000u:prior;
 if(limit<2000||limit>5000||
    np_pd_choose(f->pd,f->epoch,5000,limit,0,&fixed)||
    fixed.pps||fixed.ma<2000)return 0;
 /* Only replace our own confirmed low ICL vote. A lower aggregate without
  * that ownership can belong to another client and cannot be raised here. */
 if(f->icl_ma<2000&&(!(normal->owned&1u)||
    normal->stock.target[0]>(normal->stock_continuous?1600u:1500u)||
    normal->stock.actual[0]>(normal->stock_continuous?1600u:1500u)||
    (normal->stock.reentry==4&&f->icl_ma<=normal->stock.actual[0])))return 0;
 NpBuck planned;np_buck_init(&planned);
 planned.backend=NP_BUCK_OS4_PPS_ADAPTER_TEST;
 planned.stock_current_enabled=planned.input_suspend_enabled=planned.armed=1;
 planned.stock_continuous=normal->stock_continuous;
 planned.lease_until=normal->lease_until;planned.next_id=normal->next_id;
 planned.changes=normal->changes;planned.cccv=normal->cccv;
 NpBuckFacts ready=*f;ready.icl_ma=fixed.ma;
 NpBuckCommand command;
 if(!stock_start_allowed(&planned,&ready)||
    np_buck_next(&planned,&ready,&command)!=1||
    command.property!=NP_BUCK_FCC||command.value!=500||planned.normal_fallback)
  return 0;
 /* The plan has issued nothing. The sole broker will create that FCC
  * command after ICL readback and another complete admission check. */
 planned.pending=planned.unverified_effect=planned.wire_accepted=0;
 planned.command=(NpBuckCommand){0};planned.next_id=normal->next_id;
 planned.request_id=0;planned.owned=normal->owned&2u;
 planned.stock=normal->stock;planned.stock.reentry=3;
 planned.stock.normal_owned=(uint8_t)(normal->owned&~2u);
 *icl=fixed.ma;*next=planned;return 1;
}
int np_buck_pump_next(NpBuck *b,const NpBuckFacts *f,NpBuckCommand *out)
{
 const unsigned exiting=b->state==NP_BUCK_PUMP_STOP||b->state==NP_BUCK_PUMP_PREPARE_EXIT||
  b->state==NP_BUCK_FCC_REDUCE||b->state==NP_BUCK_PD_REQUEST_5V||
  b->state==NP_BUCK_PD_OBSERVE_5V||b->state==NP_BUCK_PUMP_RESTORE_MODE||
  b->state==NP_BUCK_PUMP_INPUT_RESUME||b->state==NP_BUCK_PUMP_OBSERVE_INPUT_RESUME;
 if(b->stock_current_enabled&&b->armed&&!b->stop&&!exiting&&
    f->now_ms<b->lease_until&&normal_source_ready(f)&&
    (!b->epoch||b->epoch==f->epoch)&&(!b->attachment||b->attachment==f->attachment)){
  if(b->state==NP_BUCK_IDLE&&!b->owned&&f->pump_known_off&&
     f->vbus_mv>=4500&&f->vbus_mv<=5500&&
     stock_normal_fallback(b,f)){
   b->normal_fallback=1;b->armed=0;b->state=NP_BUCK_FINISHED;return 0;
  }
  if(b->state!=NP_BUCK_IDLE&&(b->owned&64u)&&b->cccv.valid&&
     f->pump_controls_valid&&f->pump_bus_ocp_enabled&&f->pump_bus_ocp_ma&&
     np_buck_pump_budget(b,f)<=1500){b->normal_fallback=1;}
 }
 /* Before the first PPS command, this is still a planned adapter current.
  * Lower it when a fresh budget changes during the FCC baseline or setup;
  * no accepted PPS request or voltage confirmation is replaced here. */
 if(b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST&&b->armed&&!b->stop&&
    f->now_ms<b->lease_until&&b->epoch==f->epoch&&b->attachment==f->attachment&&
    !(b->owned&8)&&b->adapter.valid&&(b->adapter.ratio==2||b->adapter.ratio==4)&&
    (b->state==NP_BUCK_FCC_INITIALIZE||b->state==NP_BUCK_FCC_BASELINE||
     b->state==NP_BUCK_PUMP_SET_MODE||b->state==NP_BUCK_PUMP_SET_WATCHDOG||
     b->state==NP_BUCK_PUMP_REQUEST_PPS||b->state==NP_BUCK_PUMP_INPUT_SUSPEND||
     b->state==NP_BUCK_PUMP_OBSERVE_INPUT_SUSPEND)){
  const uint32_t ma=pump_adapter_budget(b,f,b->pump_target_mv,1);
  if(ma>=100&&ma<b->pump_request_ma)b->pump_request_ma=ma;
 }
 /* The original PPS transaction can carry a lower current or the selected
  * APDO envelope after FFC. Voltage regulation still uses battery curr_max;
  * the accepted current changes after ACK and two new voltage cohorts. */
 if(b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST&&b->armed&&!b->pending&&
    (b->state==NP_BUCK_PUMP_RUN||b->state==NP_BUCK_PUMP_OBSERVE_REGULATION||
     b->state==NP_BUCK_PUMP_OBSERVE_PPS||b->state==NP_BUCK_PUMP_START)&&
    !b->stop&&f->now_ms<b->lease_until&&b->epoch==f->epoch&&b->attachment==f->attachment&&
    b->adapter.valid&&(b->adapter.ratio==2||b->adapter.ratio==4)){
  /* OBSERVE states follow an ACK. A lower request uses that acknowledged
   * voltage, without waiting for the older current to qualify again.
   * Repeated facts never resubmit the same reduction. */
  const uint32_t requested=pump_command_ma(b);
  uint32_t mv=b->state==NP_BUCK_PUMP_OBSERVE_REGULATION?b->cccv_target_mv:b->pump_target_mv;
  const uint32_t ma=pump_adapter_budget(b,f,mv,0);
  const int increase=b->stock_current_enabled&&f->ffc_active&&
     b->state==NP_BUCK_PUMP_RUN&&ma>requested;
  if(ma>=100&&(ma<requested||increase)){
   const unsigned before_start=b->state==NP_BUCK_PUMP_OBSERVE_PPS||b->state==NP_BUCK_PUMP_START;
   if(increase){const uint32_t regulated=cccv_target(b,f);if(regulated)mv=regulated;}
   NpBuck next=*b;next.state=before_start?NP_BUCK_PUMP_REQUEST_PPS:NP_BUCK_PUMP_REGULATE;
   next.cccv_target_ma=pump_adapter_budget(b,f,mv,0);next.cccv_target_mv=mv;
   next.cccv_delta_mv=increase?(int32_t)mv-(int32_t)b->pump_target_mv:0;
   if(pump_qualify(&next,f)==NP_BUCK_READY){
    b->state=next.state;b->cccv_target_ma=next.cccv_target_ma;
    b->cccv_target_mv=mv;b->cccv_delta_mv=next.cccv_delta_mv;
   }
  }
 }
 unsigned reason=np_buck_pump_quiet(b)?NP_BUCK_READY:pump_qualify(b,f);
 /* The original monitor returns to ordinary charging after recoverable
  * policy events. I/O failures and unknown effects still retain ownership. */
 if(b->stock_continuous&&b->armed&&!exiting&&!b->stop&&!b->failures&&
    !b->cleanup_failed&&b->epoch==f->epoch&&b->attachment==f->attachment&&
    f->valid&&f->online&&!f->fault&&(f->full||
    reason==NP_BUCK_TEMPERATURE||reason==NP_BUCK_STALE||reason==NP_BUCK_CAPS))
  b->normal_fallback=1;
 b->blocked=reason;if(!b->armed)return 0;
 if(b->state==NP_BUCK_IDLE){
  if(b->stop||f->now_ms>=b->lease_until){b->armed=0;b->state=NP_BUCK_FINISHED;return 0;}
  if(reason||f->vbus_mv>5500||f->pump_bus_mv>5500||!f->pump_known_off)return 0;
  if(f->pump_battery_mv<3000||f->pump_battery_mv>=(b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST?4490:4460)){b->blocked=NP_BUCK_VOLTAGE;return 0;}
  if(b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST){
   if(np_adapter_select(f->pd,f->epoch,f->real_type,f->stock_support100w,&b->adapter)||
      !b->cccv.valid||!b->cccv.cycles_valid){b->blocked=NP_BUCK_CAPS;return 0;}
   const uint32_t initial=((uint32_t)f->pump_battery_mv*b->adapter.ratio+
      b->adapter.initial_headroom_mv)/20u*20u;
   b->pump_request_ma=pump_adapter_budget(b,f,initial,1);
  }
  const uint32_t target=((uint32_t)f->pump_battery_mv*pump_ratio(b)+
    (b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST?b->adapter.initial_headroom_mv:300u))/20u*20u;
  if(target<(b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST?b->adapter.minimum_mv:6500u)||
     target>(b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST?lesser(b->adapter.maximum_mv,20000):9200u)||
     !selected_caps_ready(b,f,target)){b->blocked=NP_BUCK_CAPS;return 0;}
  b->epoch=f->epoch;b->attachment=f->attachment;b->pump_target_mv=target;
  b->baseline_icl=b->target_icl=f->icl_ma;b->target_fcc=500;
  b->confirmed_vbus_mv=5000;b->pump_shutdown_verified=0;
  b->cccv_gauge_ms=b->last_boost_ms=b->last_buck_ms=0;
  b->cccv_target_mv=b->cccv_updates=0;b->cccv_delta_mv=0;b->cccv_boost_done=0;
  b->cccv_target_ma=0;
  b->state=NP_BUCK_FCC_INITIALIZE;
 }
 if(!exiting&&(b->stop||f->now_ms>=b->lease_until||reason||b->attachment!=f->attachment)){
  b->stop=1;
  if(!b->owned&&!b->unverified_effect){b->armed=0;b->state=NP_BUCK_FINISHED;return 0;}
  b->state=NP_BUCK_PUMP_STOP;
 }
 switch(b->state){
 case NP_BUCK_FCC_INITIALIZE:return issue(b,f,out,1,NP_BUCK_FCC,500);
 case NP_BUCK_FCC_BASELINE:{
  b->blocked=NP_BUCK_FEEDBACK;
  if(f->now_ms>b->feedback_after_ms&&f->now_ms-b->feedback_after_ms>20000){
   ++b->failures;b->stop=1;b->state=NP_BUCK_PUMP_STOP;return 0;}
  int got=feedback(b,f);if(!got)return 0;
  if(got<0){++b->failures;b->stop=1;b->state=NP_BUCK_PUMP_STOP;return 0;}
  const int32_t mean=(int32_t)(b->feedback_sum/b->feedback_samples);
  if(mean<200||mean>650||(int64_t)b->feedback_max-b->feedback_min>150){feedback_window_reset(b);return 0;}
  b->feedback_baseline_ma=mean;b->confirmation=NP_BUCK_CONFIRM_BASELINE;
  b->unverified_effect=0;b->state=b->input_suspend_enabled?
      NP_BUCK_PUMP_INPUT_SUSPEND:NP_BUCK_PUMP_SET_MODE;return 0;
 }
 case NP_BUCK_PUMP_INPUT_SUSPEND:
  return issue(b,f,out,1,NP_BUCK_INPUT_SUSPEND,1);
 case NP_BUCK_PUMP_OBSERVE_INPUT_SUSPEND:{
  int got=input_confirm(b,f,1);
  if(got<0){b->stop=1;b->state=NP_BUCK_PUMP_STOP;return 0;}
  if(got){b->input_suspend_confirmed=1;b->unverified_effect=0;b->state=NP_BUCK_PUMP_SET_MODE;}
  return 0;
 }
 case NP_BUCK_PUMP_SET_MODE:return issue(b,f,out,1,NP_BUCK_PUMP_MODE1,pump_mode(b));
 case NP_BUCK_PUMP_SET_WATCHDOG:return issue(b,f,out,1,NP_BUCK_PUMP_WATCHDOG,5000);
 case NP_BUCK_PUMP_REQUEST_PPS:{
  uint32_t value=0;
  /* N0 pauses the main input before reading the CP battery voltage for
   * its first PPS request. That voltage can fall when the buck stops.
   * Replace only the unsent startup target; an ACKed request keeps its
   * original target and two measured voltage cohorts. */
  if(b->input_suspend_enabled&&b->input_suspend_confirmed&&!(b->owned&8u)){
   if(f->pump_ms<=b->input_after_ms){b->blocked=NP_BUCK_STALE;return 0;}
   if(f->pump_battery_mv<3000||f->pump_battery_mv>=4490){b->blocked=NP_BUCK_VOLTAGE;return 0;}
   b->pump_target_mv=((uint32_t)f->pump_battery_mv*pump_ratio(b)+
       b->adapter.initial_headroom_mv)/20u*20u;
  }
  if(f->pump_mode!=pump_mode(b)||!selected_caps_ready(b,f,b->pump_target_mv)||
     np_pps_encode(b->pump_target_mv,pump_command_ma(b),&value)){b->blocked=NP_BUCK_CAPS;return 0;}
  int got=issue(b,f,out,1,NP_BUCK_PPS_PD,value);
  if(got>0)pd_voltage_reset(b,b->command.deadline_ms);
  return got;
 }
 case NP_BUCK_PUMP_OBSERVE_PPS:{
  if(f->now_ms>b->voltage_after_ms&&f->now_ms-b->voltage_after_ms>20000){
   ++b->failures;b->stop=1;b->state=NP_BUCK_PUMP_STOP;return 0;}
  if(!f->pump_known_off||f->pd->at_ms<=b->voltage_after_ms||
     f->charger_ms<=b->voltage_after_ms||f->charger_ms<=b->voltage_last_ms||
     f->pump_ms<=b->voltage_after_ms||f->pump_ms<=b->voltage_last_pump_ms||
     f->vbus_mv+200<b->pump_target_mv||f->vbus_mv>b->pump_target_mv+200||
     f->pump_bus_mv+200<(int32_t)b->pump_target_mv||
     f->pump_bus_mv>(int32_t)b->pump_target_mv+200)return 0;
  b->voltage_last_ms=f->charger_ms;b->voltage_last_pump_ms=f->pump_ms;
  if(++b->voltage_samples>=2){b->confirmed_vbus_mv=b->pump_target_mv;
   if(pump_current_changing(b))b->pump_request_ma=b->cccv_target_ma;
   b->unverified_effect=0;b->state=NP_BUCK_PUMP_START;}return 0;
 }
 case NP_BUCK_PUMP_START:return issue(b,f,out,1,NP_BUCK_PUMP_ENABLE,b->pump_target_mv);
 case NP_BUCK_PUMP_OBSERVE_START:
  if(f->pump_ms>b->pump_enabled_ms&&f->pump_enable&&f->pump_switching&&
    (!b->input_suspend_enabled||b->stock_continuous||
      (f->gauge_ms>b->pump_enabled_ms&&f->battery_ma>0))){
   b->unverified_effect=0;b->state=NP_BUCK_PUMP_RUN;return 0;}
  if(f->now_ms>b->pump_enabled_ms&&f->now_ms-b->pump_enabled_ms>8000){
   ++b->failures;b->stop=1;b->state=NP_BUCK_PUMP_STOP;}return 0;
 case NP_BUCK_PUMP_RUN:
  if((b->backend==NP_BUCK_OS4_PPS_CCCV_TEST||b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST)){
   if(f->gauge_ms<=b->cccv_gauge_ms)return 0;
   b->cccv_gauge_ms=f->gauge_ms;
   /* CP and main-buck currents are distinct same-generation observations,
    * not simultaneous whole-port wattage. Both constrain this ratio2 step. */
   const uint32_t target=cccv_target(b,f);
   if(!target)return 0;
   uint32_t ma=pump_request_ma(b);
   if(b->stock_current_enabled)ma=lesser(ma,pump_adapter_budget(b,f,target,0));
   NpBuck next=*b;next.cccv_target_ma=ma;next.cccv_target_mv=target;
   next.cccv_delta_mv=(int32_t)target-(int32_t)b->pump_target_mv;next.state=NP_BUCK_PUMP_REGULATE;
   if(!selected_caps_ready(&next,f,target)){
    b->blocked=NP_BUCK_CAPS;b->stop=1;b->state=NP_BUCK_PUMP_STOP;return 0;
   }
   b->cccv_target_mv=target;b->cccv_target_ma=ma;
   b->cccv_delta_mv=(int32_t)b->cccv_target_mv-(int32_t)b->pump_target_mv;
   if(!b->cccv_delta_mv)return 0;
   b->state=NP_BUCK_PUMP_REGULATE;
  }else{
   if(f->now_ms>=b->pump_enabled_ms&&f->now_ms-b->pump_enabled_ms>=60000){
    b->pump_quiet_after_ms=f->now_ms;b->state=NP_BUCK_PUMP_QUIET;}return 0;
  }
  /* fall through */
 case NP_BUCK_PUMP_REGULATE:{
  uint32_t encoded;
  if((b->backend!=NP_BUCK_OS4_PPS_CCCV_TEST&&b->backend!=NP_BUCK_OS4_PPS_ADAPTER_TEST)||
     b->cccv_target_mv<(b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST?b->adapter.minimum_mv:6500u)||
     b->cccv_target_mv>(b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST?lesser(b->adapter.maximum_mv,20000):9200u)||
     ((!pump_current_changing(b)||b->cccv_delta_mv)&&cccv_target(b,f)!=b->cccv_target_mv)||
     !selected_caps_ready(b,f,b->cccv_target_mv)||np_pps_encode(b->cccv_target_mv,pump_command_ma(b),&encoded))return 0;
  return issue(b,f,out,1,NP_BUCK_PPS_PD,encoded);
 }
 case NP_BUCK_PUMP_OBSERVE_REGULATION:
  if(f->now_ms>b->voltage_after_ms&&f->now_ms-b->voltage_after_ms>20000){
   ++b->failures;b->stop=1;b->state=NP_BUCK_PUMP_STOP;return 0;}
  if(f->pd->at_ms<=b->voltage_after_ms||f->charger_ms<=b->voltage_after_ms||
     f->charger_ms<=b->voltage_last_ms||f->pump_ms<=b->voltage_after_ms||
     f->pump_ms<=b->voltage_last_pump_ms)return 0;
  b->voltage_last_ms=f->charger_ms;b->voltage_last_pump_ms=f->pump_ms;
  if(++b->voltage_samples>=2){
   if(pump_current_changing(b))b->pump_request_ma=b->cccv_target_ma;
   b->pump_target_mv=b->confirmed_vbus_mv=b->cccv_target_mv;
   b->unverified_effect=0;++b->cccv_updates;b->state=NP_BUCK_PUMP_RUN;
  }
  return 0;
 case NP_BUCK_PUMP_QUIET:
  if(f->now_ms>=b->pump_quiet_after_ms&&f->now_ms-b->pump_quiet_after_ms>=8000)
   b->state=NP_BUCK_PUMP_READ_EXPIRY;
  return 0;
 case NP_BUCK_PUMP_READ_EXPIRY:return issue(b,f,out,0,NP_BUCK_PUMP_EXPIRY,0);
 case NP_BUCK_PUMP_STOP:
  if(b->owned&64)return issue(b,f,out,1,NP_BUCK_PUMP_DISABLE,0);
  b->state=NP_BUCK_PUMP_PREPARE_EXIT;return 0;
 case NP_BUCK_PUMP_PREPARE_EXIT:
  if(b->owned&32)return issue(b,f,out,1,NP_BUCK_PUMP_EXIT_ADC,0);
  b->state=NP_BUCK_FCC_REDUCE;return 0;
 case NP_BUCK_FCC_REDUCE:
  if(b->owned&2)return issue(b,f,out,1,NP_BUCK_FCC,100);
  b->state=NP_BUCK_PD_REQUEST_5V;return 0;
 case NP_BUCK_PD_REQUEST_5V:
  if(!(b->owned&8)){b->state=NP_BUCK_PUMP_RESTORE_MODE;return 0;}
  if(pd_contract_gone(b,f)){
   /* Confirm disappearance with two new PMIC/CP cohorts even when a
    * computer is already connected. No PD request is sent to that source. */
   pd_voltage_reset(b,f->now_ms);b->state=NP_BUCK_PD_OBSERVE_5V;return 0;
  }
  if(!f->valid||!f->online||f->pd_state!=2||
     (f->real_type!=10&&f->real_type!=11&&f->real_type!=12)||
     (f->data_role!=1&&f->data_role!=2)||f->boost_enabled||f->gate_enabled){b->blocked=NP_BUCK_SOURCE;return 0;}
  return issue(b,f,out,1,NP_BUCK_FIXED_PD,5000);
 case NP_BUCK_PD_OBSERVE_5V:{
  if(f->now_ms>b->voltage_after_ms&&f->now_ms-b->voltage_after_ms>20000){
   b->state=NP_BUCK_UNCERTAIN;b->blocked=NP_BUCK_EFFECT_UNKNOWN;return -1;}
  const int got=pd_voltage_feedback(b,f,0);if(!got)return 0;
  if(got<0){b->state=NP_BUCK_UNCERTAIN;return -1;}
  b->owned&=~8u;b->confirmed_vbus_mv=b->stock_continuous?f->vbus_mv:5000;b->unverified_effect=0;
  b->state=NP_BUCK_PUMP_RESTORE_MODE;return 0;
 }
 case NP_BUCK_PUMP_RESTORE_MODE:
  if(!pd_low_voltage(f)){b->blocked=NP_BUCK_VOLTAGE;return 0;}
  if(b->owned&16)return issue(b,f,out,1,NP_BUCK_PUMP_MODE_RESTORE,0);
  b->protection_restored=1;
  if(b->owned&128u){b->state=NP_BUCK_PUMP_INPUT_RESUME;return 0;}
  b->armed=0;
  b->state=(b->owned||b->cleanup_failed)?NP_BUCK_UNCERTAIN:NP_BUCK_FINISHED;return 0;
 case NP_BUCK_PUMP_INPUT_RESUME:
  if(!(b->owned&128u)||!b->input_suspend_enabled||
     (f->attachment!=b->attachment&&!(b->stock_continuous&&b->stop))||
     !f->valid||(!f->online&&!b->stock_continuous)||!f->pump_known_off||!pd_low_voltage(f)||
     !b->protection_restored||f->boost_enabled||f->gate_enabled)return 0;
  return issue(b,f,out,1,NP_BUCK_INPUT_SUSPEND,0);
 case NP_BUCK_PUMP_OBSERVE_INPUT_RESUME:{
  int got=input_confirm(b,f,0);
  if(got<0){b->state=NP_BUCK_UNCERTAIN;b->blocked=NP_BUCK_EFFECT_UNKNOWN;return -1;}
  if(got){b->owned&=~128u;b->input_suspend_confirmed=0;b->unverified_effect=0;b->armed=0;
   b->state=(b->owned||b->cleanup_failed)?NP_BUCK_UNCERTAIN:NP_BUCK_FINISHED;}
  return 0;
 }
 default:return 0;
 }
}
int np_buck_pump_complete(NpBuck *b,int status,uint32_t value)
{
 const unsigned state=b->state;
 if(state==NP_BUCK_PUMP_INPUT_SUSPEND||state==NP_BUCK_PUMP_INPUT_RESUME){
  if(state==NP_BUCK_PUMP_INPUT_SUSPEND&&(!status||b->wire_accepted))b->owned|=128u;
  if(status){b->stop=1;b->state=NP_BUCK_UNCERTAIN;b->blocked=NP_BUCK_EFFECT_UNKNOWN;return -1;}
  ++b->changes;input_reset(b);
  b->state=state==NP_BUCK_PUMP_INPUT_SUSPEND?
      NP_BUCK_PUMP_OBSERVE_INPUT_SUSPEND:NP_BUCK_PUMP_OBSERVE_INPUT_RESUME;
  return 1;
 }
 if(state==NP_BUCK_PUMP_REGULATE){
  if(status){b->stop=1;b->state=NP_BUCK_PUMP_STOP;return 1;}
  ++b->changes;
  const uint64_t now=b->command.deadline_ms-NP_BUCK_COMMAND_MS;
  if(b->cccv_delta_mv>0)b->last_boost_ms=now;
  else if(b->cccv_delta_mv<0){b->last_buck_ms=now;b->cccv_boost_done=1;}
  pd_voltage_reset(b,b->command.deadline_ms);b->state=NP_BUCK_PUMP_OBSERVE_REGULATION;return 1;
 }
 if(state==NP_BUCK_PUMP_SET_MODE||state==NP_BUCK_PUMP_SET_WATCHDOG||state==NP_BUCK_PUMP_START){
  const unsigned bit=state==NP_BUCK_PUMP_SET_MODE?16u:state==NP_BUCK_PUMP_SET_WATCHDOG?32u:64u;
  if(!status||b->wire_accepted)b->owned|=bit;
  if(status){b->stop=1;b->unverified_effect=0;b->state=NP_BUCK_PUMP_STOP;return 1;}
  ++b->changes;
  if(state==NP_BUCK_PUMP_SET_MODE)b->state=NP_BUCK_PUMP_SET_WATCHDOG;
  else if(state==NP_BUCK_PUMP_SET_WATCHDOG)b->state=NP_BUCK_PUMP_REQUEST_PPS;
  else {b->pump_enabled_ms=b->command.deadline_ms-NP_BUCK_COMMAND_MS;
   b->state=NP_BUCK_PUMP_OBSERVE_START;return 1;}
  b->unverified_effect=0;return 1;
 }
 if(state==NP_BUCK_PUMP_REQUEST_PPS){
  if(!status||b->wire_accepted)b->owned|=8;
  if(status){b->stop=1;b->state=NP_BUCK_PUMP_STOP;return 1;}
  ++b->changes;pd_voltage_reset(b,b->command.deadline_ms);b->state=NP_BUCK_PUMP_OBSERVE_PPS;return 1;
 }
 if(state==NP_BUCK_PUMP_READ_EXPIRY){
  b->pump_shutdown_verified=!status&&value==1;
  if(!b->pump_shutdown_verified)++b->failures;
  b->stop=1;b->state=NP_BUCK_PUMP_STOP;return 1;
 }
 if(state==NP_BUCK_PUMP_STOP||state==NP_BUCK_PUMP_PREPARE_EXIT){
  if(status){b->state=NP_BUCK_UNCERTAIN;b->blocked=NP_BUCK_EFFECT_UNKNOWN;return -1;}
  b->owned&=~(state==NP_BUCK_PUMP_STOP?64u:32u);b->unverified_effect=0;++b->changes;
  b->state=state==NP_BUCK_PUMP_STOP?NP_BUCK_PUMP_PREPARE_EXIT:NP_BUCK_FCC_REDUCE;return 1;
 }
 if(state==NP_BUCK_FCC_INITIALIZE){
  if(!status||b->wire_accepted)b->owned|=2;
  if(status){b->stop=1;b->state=NP_BUCK_PUMP_STOP;return 1;}
  ++b->changes;feedback_reset(b);b->state=NP_BUCK_FCC_BASELINE;return 1;
 }
 if(state==NP_BUCK_FCC_REDUCE){
  if(status)b->cleanup_failed=1;
  else{b->owned&=~2u;b->unverified_effect=0;++b->changes;}
  b->state=NP_BUCK_PD_REQUEST_5V;return 1;
 }
 if(state==NP_BUCK_PD_REQUEST_5V){
  if(status){b->state=NP_BUCK_UNCERTAIN;return -1;}
  ++b->changes;pd_voltage_reset(b,b->command.deadline_ms);b->state=NP_BUCK_PD_OBSERVE_5V;return 1;
 }
 if(state==NP_BUCK_PUMP_RESTORE_MODE){
  if(status){b->state=NP_BUCK_UNCERTAIN;b->blocked=NP_BUCK_EFFECT_UNKNOWN;return -1;}
  b->owned&=~16u;b->unverified_effect=0;++b->changes;
  b->protection_restored=1;
  if(b->owned&128u){b->state=NP_BUCK_PUMP_INPUT_RESUME;return 1;}
  b->armed=0;
  b->state=(b->owned||b->cleanup_failed)?NP_BUCK_UNCERTAIN:NP_BUCK_FINISHED;return 1;
 }
 b->state=NP_BUCK_UNCERTAIN;return -1;
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
 if(b->backend==NP_BUCK_OS4_PPS_PUMP_TEST||(b->backend==NP_BUCK_OS4_PPS_CCCV_TEST||b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST))return np_buck_pump_next(b,f,out);
 if(b->backend==NP_BUCK_OS4_STOCK_POLICY)return np_buck_stock_next(b,f,out);
 if(b->backend==NP_BUCK_OS4_FIXED_PD||b->backend==NP_BUCK_OS4_PPS_OFF_TEST)return fixed_pd_next(b,f,out);
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
 if(b->backend==NP_BUCK_OS4_PPS_PUMP_TEST||(b->backend==NP_BUCK_OS4_PPS_CCCV_TEST||b->backend==NP_BUCK_OS4_PPS_ADAPTER_TEST))return np_buck_pump_complete(b,status,value);
 if(b->backend==NP_BUCK_OS4_STOCK_POLICY)return np_buck_stock_complete(b,status,value);
 if(b->backend==NP_BUCK_OS4_FIXED_PD||b->backend==NP_BUCK_OS4_PPS_OFF_TEST)return fixed_pd_complete(b,status,value);
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
