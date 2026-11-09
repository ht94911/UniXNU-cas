/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host model of protocol/control contracts. It does not certify hardware. */
#include "NezhaPowerControl.h"
#include "NezhaMca.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void pd_cases(void) {
 uint8_t data[NP_PD_BYTES]={0};NpPdCaps caps;NpPdChoice choice;uint32_t mv,ma;
 np_put32(data,5000);np_put32(data+4,5000);np_put32(data+8,3000);
 np_put32(data+16,3300);np_put32(data+20,21000);np_put32(data+24,6100);np_put32(data+28,NP_PD_APDO);
 assert(!np_pd_decode(&caps,data,sizeof(data),7,100));
 assert(!np_pd_choose(&caps,7,5000,1500,1,&choice)&&!choice.pps&&choice.ma==1500);
 assert(!np_pd_choose(&caps,7,18400,1049,1,&choice)&&choice.pps&&choice.ma==1000);
 assert(np_pd_choose(&caps,8,5000,1500,0,&choice)<0);
 assert(np_pd_choose(&caps,7,18401,1000,1,&choice)<0);
 assert(np_pd_decode(&caps,data,NP_PD_BYTES-1,7,100)<0&&!caps.valid);
 np_put32(data+24,0xffffffff);assert(np_pd_decode(&caps,data,sizeof(data),7,100)<0&&caps.invalid_mask==2);
 assert(!np_pd_status((920u<<16)|20,&mv,&ma)&&mv==18400&&ma==1000);
 assert(np_pd_status(0xffffffff,&mv,&ma)<0);
}
static NpBuckFacts facts(void) {
 NpBuckFacts f={0};f.epoch=1;f.attachment=2;f.now_ms=f.sampled_ms=1000;
 f.valid=f.online=f.charge_enabled=f.pmic_charging=f.pump_known_off=1;
 f.real_type=3;f.term_mv=4480;f.icl_ma=500;f.fcc_ma=500;f.vbus_mv=5000;
 f.battery_mv=4250;f.temperature_centi_c=3000;return f;
}
static NpBuckCommand next(NpBuck *b,NpBuckFacts *f) {
 NpBuckCommand q;for(unsigned i=0;i<5;++i){f->now_ms+=10;int r=np_buck_next(b,f,&q);assert(r>=0);if(r)return q;}
 assert(!"expected command");return q;
}
static void buck_cases(void) {
 NpBuck b;NpBuckFacts f=facts();NpBuckCommand q;np_buck_init(&b);assert(!np_buck_arm(&b,1000,120000));
 q=next(&b,&f);assert(q.write&&q.property==NP_BUCK_ICL&&q.value==1500);
 assert(!np_buck_complete(&b,q.id+1,0,0)&&b.pending);
 assert(np_buck_complete(&b,q.id,0,0)==1);
 q=next(&b,&f);assert(!q.write&&q.property==NP_BUCK_ICL);
 assert(np_buck_complete(&b,q.id,0,1500)==1);f.icl_ma=1500;
 q=next(&b,&f);assert(q.write&&q.property==NP_BUCK_FCC&&q.value==1500);
 np_buck_complete(&b,q.id,0,0);q=next(&b,&f);assert(!q.write);
 np_buck_complete(&b,q.id,0,1500);f.fcc_ma=1500;assert(b.owned==3&&b.state==NP_BUCK_RUNNING);
 np_buck_stop(&b);q=next(&b,&f);assert(!q.write&&q.property==NP_BUCK_FCC);
 np_buck_complete(&b,q.id,0,1500);q=next(&b,&f);assert(q.write&&q.value==500);
 np_buck_complete(&b,q.id,0,0);q=next(&b,&f);np_buck_complete(&b,q.id,0,500);f.fcc_ma=500;
 q=next(&b,&f);assert(!q.write&&q.property==NP_BUCK_ICL);np_buck_complete(&b,q.id,0,1500);
 q=next(&b,&f);assert(q.write&&q.value==500);np_buck_complete(&b,q.id,0,0);
 q=next(&b,&f);np_buck_complete(&b,q.id,0,500);assert(!b.owned&&!b.armed&&b.state==NP_BUCK_FINISHED);
 /* Temperature change between WRITE and READBACK may not lose the effect. */
 f=facts();np_buck_init(&b);np_buck_arm(&b,1000,120000);q=next(&b,&f);
 np_buck_complete(&b,q.id,-1,0);f.temperature_centi_c=4000;
 q=next(&b,&f);assert(!q.write&&q.property==NP_BUCK_ICL);np_buck_complete(&b,q.id,0,1500);
 assert(b.owned==1&&b.stop);q=next(&b,&f);assert(!q.write);np_buck_complete(&b,q.id,0,1500);
 q=next(&b,&f);assert(q.write&&q.value==100);np_buck_complete(&b,q.id,0,0);
 q=next(&b,&f);np_buck_complete(&b,q.id,0,100);assert(!b.owned);
 /* Remote restart after an attempted write is not a clean idle state. */
 f=facts();np_buck_init(&b);np_buck_arm(&b,1000,120000);q=next(&b,&f);f.epoch=2;
 assert(np_buck_next(&b,&f,&q)<0&&b.state==NP_BUCK_UNCERTAIN&&b.unverified_effect);
 /* Unsupported source, active pump and invalid baselines cannot write. */
 for(unsigned test=0;test<5;++test){f=facts();np_buck_init(&b);np_buck_arm(&b,1000,120000);
  if(test==0)f.real_type=0;
  if(test==1)f.pump_known_off=0;
  if(test==2)f.icl_ma=0;
  if(test==3)f.vbus_mv=18400;
  if(test==4)f.term_mv=4500;
  assert(!np_buck_next(&b,&f,&q));}
}
static void broker_cases(void) {
 NpPowerControl c;np_control_init(&c,2);np_control_link(&c,1,1);c.allow_control=1;
 NpSnapshot s={0};s.generation=1;s.valid=0x7fff;s.gauge.generation=1;
 s.gauge.soc=79;s.gauge.voltage_mv=4250;s.gauge.temperature_centi_c=3000;
 s.external=s.charging=1;s.charger.usb_uv=5000000;
 NpPump pump={0};pump.chip=0x85;pump.controls_valid=pump.switching_valid=1;
 uint32_t registers[NP_META_COUNT]={500,500,4480,1,0,0,0,3,0,0,0,0,1};
 uint64_t now=1000;unsigned writes=0;
 for(unsigned n=0;n<160;++n){
  now+=20;s.at_ms=s.gauge.end_ms=s.charger.end_ms=now;np_control_sample(&c,&s,&pump,0,now);
  NpAuxCommand q;if(np_control_next(&c,now,&q)<=0)continue;
  uint8_t reply[NP_PD_BYTES]={0};
  if(q.control) {
   unsigned i=q.property==NP_BUCK_ICL?NP_META_ICL:NP_META_FCC;
   if(q.write){assert(np_control_write_allowed(&c,&q,now));NpAuxCommand bad=q;bad.property=0x2101a;assert(!np_control_write_allowed(&c,&bad,now));registers[i]=q.value;++writes;}
   else np_put32(reply,registers[i]);
  }else if(q.index!=NP_META_PDOS)np_put32(reply,registers[q.index]);
  np_control_complete(&c,&q,0,reply,q.reply_bytes,now);
  if(c.buck.state==NP_BUCK_RUNNING)break;
 }
 assert(writes==2&&registers[0]==1500&&registers[1]==1500&&c.buck.owned==3);
 /* An event on a different source invalidates cached capability immediately. */
 np_control_source_event(&c);assert(!c.caps.valid&&!c.meta[0].valid&&c.buck.stop);
}
int main(void){pd_cases();buck_cases();broker_cases();puts("PASS PD ABI/caps, real readback control, stale ACK/epoch, guarded limits and restore");return 0;}
