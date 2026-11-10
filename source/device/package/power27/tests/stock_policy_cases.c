/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Independent normal-buck register/charge peer; no phone or hardware IO. */
#include "NezhaBuck.h"
#include "NezhaMca.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
 uint32_t icl,fv,iterm,en,fcc;
 unsigned writes[4],fcc_writes,lost_fcc_ack,lost_reduce_ack,ignore_fv_write,taper;
 NpBuck b;NpBuckFacts f;NpPdCaps pd;
} Peer;
static unsigned slot(uint32_t p)
{switch(p){case NP_BUCK_ICL:return 0;case NP_BUCK_TERM_VOLT:return 1;
 case NP_BUCK_TERM_CURRENT:return 2;case NP_BUCK_ENABLE:return 3;default:return 4;}}
static void prepare(Peer *p)
{
 memset(p,0,sizeof(*p));p->icl=1600;p->fv=4400;p->iterm=100;p->en=1;p->fcc=500;
 np_buck_init(&p->b);p->b.backend=NP_BUCK_OS4_STOCK_POLICY;
 uint8_t raw[NP_PD_BYTES]={0};np_put32(raw,5000);np_put32(raw+4,5000);np_put32(raw+8,3000);
 np_put32(raw+16,9000);np_put32(raw+20,9000);np_put32(raw+24,3000);
 assert(!np_pd_decode(&p->pd,raw,sizeof(raw),1,1000));
 p->f.epoch=p->f.attachment=1;p->f.now_ms=1000;p->f.valid=p->f.online=1;
 p->f.real_type=11;p->f.pump_known_off=1;p->f.vbus_mv=5000;p->f.battery_mv=4300;
 p->f.data_role=2;p->f.pd_state=2;p->f.pd=&p->pd;
 p->f.policy_valid=p->f.policy_enabled=1;p->f.policy_fcc=4000;
 p->f.policy_fv=4480;p->f.policy_iterm=358;
 assert(!np_buck_arm(&p->b,p->f.now_ms,0));
}
static void step(Peer *p)
{
 NpBuckFacts *f=&p->f;f->now_ms+=100;f->sampled_ms=f->now_ms;
 f->gauge_ms=f->charger_ms=f->now_ms/2000*2000;
 f->icl_ma=p->icl;f->term_mv=p->fv;f->charge_enabled=p->en;f->pmic_charging=p->en;
 f->battery_ma=!p->en?-50:p->taper?100:(int32_t)p->fcc;
 NpBuckCommand c;int got=np_buck_next(&p->b,f,&c);
 if(got<=0)return;
 unsigned i=slot(c.property);int error=0;uint32_t value=0;
 assert(c.property!=NP_BUCK_FIXED_PD&&c.property!=NP_BUCK_USB_OVP);
 if(c.write){
  assert(np_buck_stock_write_allowed(&p->b,f,c.property,c.value));
  p->b.wire_accepted=1;
  if(i==4){assert(c.property==NP_BUCK_FCC&&c.value>=100&&c.value<=1500);
   ++p->fcc_writes;p->fcc=c.value;
   if(c.value>100&&p->lost_fcc_ack){p->lost_fcc_ack=0;error=-1;}
   if(c.value==100&&p->lost_reduce_ack){p->lost_reduce_ack=0;error=-1;}
  }else{
   ++p->writes[i];
   if(i==0){assert(c.value<=1500);p->icl=c.value;}
   if(i==1){assert(c.value<=4500);if(!p->ignore_fv_write)p->fv=c.value;}
   if(i==2)p->iterm=c.value;
   if(i==3){assert(c.value<=1);p->en=c.value;}
  }
 }else{
  assert(i<4);value=i==0?p->icl:i==1?p->fv:i==2?p->iterm:p->en;
 }
 assert(np_buck_complete(&p->b,c.id+1,0,value)==0);
 (void)np_buck_complete(&p->b,c.id,error,value);
}
static void until(Peer *p,unsigned state)
{
 for(unsigned i=0;i<800;++i){step(p);if(p->b.state==state)return;}
 fprintf(stderr,"state=%u expected=%u blocked=%u owned=%u fcc=%u enabled=%u\n",
  p->b.state,state,p->b.blocked,p->b.owned,p->fcc,p->en);
 assert(!"Normal policy did not reach expected state");
}
int main(void)
{
 Peer p;prepare(&p);until(&p,NP_BUCK_POLICY_MONITOR);
 assert(p.icl==1500&&p.fv==4480&&p.iterm==358&&p.fcc==1500&&p.en==1);
 assert(p.b.confirmation==NP_BUCK_CONFIRM_CURRENT&&p.b.owned);
 p.f.now_ms+=400000;step(&p);assert(p.b.state==NP_BUCK_POLICY_MONITOR&&p.b.armed);
 p.f.policy_enabled=0;until(&p,NP_BUCK_POLICY_MONITOR);assert(!p.en&&p.b.armed);
 p.f.policy_enabled=1;p.taper=1;until(&p,NP_BUCK_POLICY_MONITOR);assert(p.en);
 np_buck_stop(&p.b);until(&p,NP_BUCK_FINISHED);
 assert(!p.en&&p.fcc==100&&p.icl==100&&p.fv==4400&&p.iterm==100&&!p.b.owned&&!p.b.armed);

 prepare(&p);p.f.policy_enabled=0;p.f.policy_fv=4060;p.f.policy_fcc=3280;
 until(&p,NP_BUCK_POLICY_MONITOR);assert(!p.en&&!p.fcc_writes);
 assert(p.b.stock.stop_samples>=2);
 p.f.battery_mv=3900;p.f.policy_enabled=1;until(&p,NP_BUCK_POLICY_MONITOR);
 assert(p.en&&p.fv==4060&&p.fcc==1500);
 np_buck_stop(&p.b);until(&p,NP_BUCK_FINISHED);assert(p.fv==4060&&!p.en&&!p.b.owned);

 prepare(&p);p.lost_fcc_ack=1;until(&p,NP_BUCK_FINISHED);
 assert(p.fcc_writes==2&&!p.en&&!p.b.owned&&p.b.failures);
 prepare(&p);until(&p,NP_BUCK_POLICY_MONITOR);p.lost_reduce_ack=1;
 np_buck_stop(&p.b);until(&p,NP_BUCK_UNCERTAIN);
 assert(p.b.owned&2);assert(!p.en);unsigned writes=p.fcc_writes;
 for(unsigned i=0;i<30;++i)step(&p);
 assert(p.fcc_writes==writes);

 prepare(&p);p.ignore_fv_write=1;until(&p,NP_BUCK_UNCERTAIN);
 assert(p.fcc==500&&p.b.owned&&p.b.blocked==NP_BUCK_EFFECT_UNKNOWN);
 prepare(&p);until(&p,NP_BUCK_POLICY_MONITOR);p.f.epoch=2;step(&p);
 assert(p.b.state==NP_BUCK_UNCERTAIN&&p.b.owned);
 prepare(&p);p.f.policy_valid=0;
 for(unsigned i=0;i<40;++i)step(&p);
 assert(!p.fcc_writes&&!p.b.owned&&p.b.blocked==NP_BUCK_STALE);
 puts("PASS stock normal-buck policy: measured current ceilings, real readback, full/hot stop-restart, no five-minute lease, no request replay, monotonic safe handoff and retained uncertainty; host-only");
}
