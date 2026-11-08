/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaBuck.h"
#include <string.h>
static uint32_t lesser(uint32_t a,uint32_t b){return a<b?a:b;}
void np_buck_init(NpBuck *b){memset(b,0,sizeof(*b));}
int np_buck_arm(NpBuck *b,uint64_t now,uint32_t lease)
{
 if(!b||b->armed||b->pending||b->owned||b->state==NP_BUCK_UNCERTAIN||
    lease<1000||lease>300000||now>UINT64_MAX-lease)return -1;
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
static unsigned qualify(const NpBuckFacts *f,uint32_t *icl,uint32_t *fcc)
{
 if(!f->valid||!f->epoch||!f->attachment||f->now_ms<f->sampled_ms||
    f->now_ms-f->sampled_ms>3000)return NP_BUCK_STALE;
 if(!f->online||f->boost_enabled||f->gate_enabled||f->input_suspended)return NP_BUCK_SOURCE;
 if(f->temperature_centi_c<1800||f->temperature_centi_c>=3500)return NP_BUCK_TEMPERATURE;
 if(f->vbus_mv<4500||f->vbus_mv>5500||f->battery_mv<3000||f->battery_mv>=4460||
    f->term_mv<4100||f->term_mv>4490)return NP_BUCK_VOLTAGE;
 if(!f->pump_known_off)return NP_BUCK_PUMP_ACTIVE;
 if(f->icl_ma<100||f->icl_ma>5000||f->fcc_ma<100||f->fcc_ma>10000)return NP_BUCK_BASELINE;
 if(f->fault||f->full||!f->charge_enabled||!f->pmic_charging)return NP_BUCK_FAULT;
 *icl=budget(f,fcc);return *icl?NP_BUCK_READY:NP_BUCK_CAPS;
}
static int issue(NpBuck *b,const NpBuckFacts *f,NpBuckCommand *out,unsigned write,uint32_t property,uint32_t value)
{
 if(b->next_id==UINT64_MAX||f->now_ms>UINT64_MAX-1500){b->state=NP_BUCK_UNCERTAIN;return -1;}
 b->command=(NpBuckCommand){++b->next_id,b->epoch,f->now_ms+1500,property,value,(uint8_t)write};
 if(write)b->unverified_effect=1;
 b->request_id=b->command.id;b->pending=1;*out=b->command;return 1;
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
 uint32_t icl=0,fcc=0;
 unsigned reason=qualify(f,&icl,&fcc);b->blocked=reason;
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
