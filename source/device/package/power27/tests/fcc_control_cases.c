/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Real controller with ACK/current fixtures; no hardware qualification. */
#include "NezhaPowerControl.h"
#include <assert.h>
#include <stdio.h>
static NpBuckFacts facts(void)
{
 NpBuckFacts f={0};f.epoch=1;f.attachment=2;f.now_ms=f.sampled_ms=f.gauge_ms=1000;
 f.valid=f.online=f.charge_enabled=f.pmic_charging=f.pump_known_off=1;
 f.real_type=3;f.term_mv=4400;f.icl_ma=3000;f.fcc_ma=0;f.vbus_mv=4850;
 f.battery_mv=4250;f.temperature_centi_c=3000;f.battery_ma=490;return f;
}
static void init(NpBuck *b)
{np_buck_init(b);b->backend=NP_BUCK_OS4_SETTER;assert(!np_buck_arm(b,1000,240000));}
static NpBuckCommand command(NpBuck *b,NpBuckFacts *f)
{
 NpBuckCommand q;f->now_ms+=10;f->sampled_ms=f->gauge_ms=f->now_ms;
 assert(np_buck_next(b,f,&q)==1&&q.write&&q.property==NP_BUCK_FCC);
 assert(q.value==100||q.value==500||q.value==1000);return q;
}
static void observe(NpBuck *b,NpBuckFacts *f,int ma)
{
 for(unsigned i=0;i<4;++i){NpBuckCommand q;f->now_ms+=2500;f->gauge_ms=f->sampled_ms=f->now_ms;f->battery_ma=ma;
  assert(!np_buck_next(b,f,&q));}
}
static void deadline(NpBuck *b,NpBuckFacts *f)
{
 NpBuckCommand q;f->now_ms=b->feedback_after_ms+20001;
 f->gauge_ms=f->sampled_ms=f->now_ms;assert(!np_buck_next(b,f,&q));
 assert(b->state==NP_BUCK_FCC_REDUCE&&b->stop);
}
static void baseline(NpBuck *b,NpBuckFacts *f)
{
 init(b);NpBuckCommand q=command(b,f);assert(q.value==500);
 assert(!np_buck_complete(b,q.id+1,0,0)&&b->pending);
 assert(np_buck_complete(b,q.id,0,0)==1&&b->state==NP_BUCK_FCC_BASELINE);
 assert(b->unverified_effect&&b->confirmation==NP_BUCK_CONFIRM_NONE);
 /* Repeated calls with the same gauge do not manufacture fresh samples. */
 NpBuckCommand ignored;for(unsigned i=0;i<20;++i)assert(!np_buck_next(b,f,&ignored));
 assert(!b->feedback_samples);observe(b,f,490);
 assert(b->state==NP_BUCK_SET_FCC&&b->confirmation==NP_BUCK_CONFIRM_BASELINE);
 assert(b->baseline_fcc==0&&b->feedback_baseline_ma==490&&!b->unverified_effect);
}
int main(void)
{
 NpBuck b;NpBuckFacts f=facts();NpBuckCommand q;
 /* The legacy backend continues to reject the unknown0 getter. */
 np_buck_init(&b);np_buck_arm(&b,1000,120000);assert(!np_buck_next(&b,&f,&q)&&b.blocked==NP_BUCK_BASELINE);
 baseline(&b,&f);q=command(&b,&f);assert(q.value==1000);np_buck_complete(&b,q.id,0,0);
 assert(b.state==NP_BUCK_FCC_OBSERVE&&b.unverified_effect);observe(&b,&f,990);
 assert(b.state==NP_BUCK_RUNNING&&b.confirmation==NP_BUCK_CONFIRM_CURRENT&&b.feedback_target_ma==990);
 assert(b.owned==2&&b.target_icl==3000&&b.baseline_fcc==0);
 np_buck_stop(&b);q=command(&b,&f);assert(q.value==500);np_buck_complete(&b,q.id,0,0);
 assert(b.state==NP_BUCK_FINISHED&&!b.owned&&!b.armed&&b.confirmation==NP_BUCK_CONFIRM_REDUCTION_ACK);
 /* The lease uses monotonic time, independent of physical/chat replies. */
 f=facts();baseline(&b,&f);q=command(&b,&f);np_buck_complete(&b,q.id,0,0);observe(&b,&f,990);
 f.now_ms=b.lease_until;f.sampled_ms=f.gauge_ms=f.now_ms;q=command(&b,&f);assert(q.value==500);
 np_buck_complete(&b,q.id,0,0);assert(!b.armed&&!b.owned&&b.state==NP_BUCK_FINISHED);
 /* An ACK with unchanged battery current cannot qualify the increase. */
 f=facts();baseline(&b,&f);q=command(&b,&f);np_buck_complete(&b,q.id,0,0);observe(&b,&f,490);
 assert(b.state==NP_BUCK_FCC_OBSERVE&&!b.stop&&!b.failures&&b.unverified_effect);
 deadline(&b,&f);
 assert(b.state==NP_BUCK_FCC_REDUCE&&b.stop);q=command(&b,&f);assert(q.value==100);
 np_buck_complete(&b,q.id,0,0);assert(b.state==NP_BUCK_FINISHED&&b.failures);
 /* The actual019 first cohort (range183mA) waits without qualifying a raise.
  * A subsequent stable cohort can qualify before the original20s deadline. */
 f=facts();init(&b);q=command(&b,&f);np_buck_complete(&b,q.id,0,0);
 const int32_t actual019[]={495,417,312,476};
 uint64_t end=b.feedback_after_ms;
 for(unsigned i=0;i<4;++i){f.now_ms+=2500;f.gauge_ms=f.sampled_ms=f.now_ms;
  f.battery_ma=actual019[i];assert(!np_buck_next(&b,&f,&q));}
 assert(b.state==NP_BUCK_FCC_BASELINE&&!b.stop&&!b.failures&&b.unverified_effect);
 assert(b.feedback_after_ms==end&&b.feedback_samples==0);
 for(unsigned i=0;i<20;++i)assert(!np_buck_next(&b,&f,&q));
 assert(!b.feedback_samples);observe(&b,&f,490);
 assert(b.state==NP_BUCK_SET_FCC&&b.feedback_baseline_ma==490&&!b.unverified_effect);
 /* Persistent load oscillation remains unqualified and ends at the same
  * deadline, including after a discarded cohort. */
 f=facts();init(&b);q=command(&b,&f);np_buck_complete(&b,q.id,0,0);
 for(unsigned i=0;i<4;++i){f.now_ms+=2500;f.gauge_ms=f.sampled_ms=f.now_ms;
  f.battery_ma=actual019[i];assert(!np_buck_next(&b,&f,&q));}
 deadline(&b,&f);q=command(&b,&f);assert(q.value==100);np_buck_complete(&b,q.id,0,0);
 assert(b.state==NP_BUCK_FINISHED&&b.failures==1);
 /* Refusal before sending has no effect and requires no cleanup write. */
 f=facts();init(&b);q=command(&b,&f);np_buck_complete(&b,q.id,-1,0);
 assert(b.state==NP_BUCK_FINISHED&&!b.owned&&!b.unverified_effect);
 /* An accepted write with a lost reply gets one different reduction. */
 f=facts();baseline(&b,&f);q=command(&b,&f);b.wire_accepted=1;np_buck_complete(&b,q.id,-1,0);
 q=command(&b,&f);assert(q.value==100);b.wire_accepted=1;assert(np_buck_complete(&b,q.id,-1,0)<0);
 assert(b.state==NP_BUCK_UNCERTAIN&&np_buck_next(&b,&f,&q)<0);
 /* Restart after enqueue cannot target the new firmware epoch. */
 f=facts();init(&b);q=command(&b,&f);f.epoch=2;
 assert(np_buck_next(&b,&f,&q)<0&&b.state==NP_BUCK_UNCERTAIN);
 /* Source/temperature/pump/cap changes stop before an increase. */
 for(unsigned i=0;i<5;++i){f=facts();baseline(&b,&f);
  if(i==0)++f.attachment;
  if(i==1)f.temperature_centi_c=3500;
  if(i==2)f.pump_known_off=0;
  if(i==3)f.vbus_mv=9000;
  if(i==4)f.battery_ma=1600;
  q=command(&b,&f);assert(q.value==100);np_buck_complete(&b,q.id,0,0);}
 f=facts();init(&b);f.icl_ma=100;assert(!np_buck_next(&b,&f,&q)&&b.blocked==NP_BUCK_BASELINE);
 puts("PASS FCC setter: real current response, unsupported getter remains unknown, no ACK-only increase, refusal/timeout/epoch and single reduction");
}
