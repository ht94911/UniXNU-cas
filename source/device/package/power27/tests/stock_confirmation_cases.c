/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* New delayed readback cases on the existing independent register peer. */
#include "NezhaPowerControl.h"
static int observed_complete(NpBuck *,uint64_t,int,uint32_t);
#define np_buck_complete observed_complete
#define main legacy_stock_policy_cases_not_run
#include "stock_policy_cases.c"
#undef main
#undef np_buck_complete

static Peer *active;
static uint32_t previous_iterm,next_iterm,override_iterm;
static uint64_t apply_at,write_deadline;
static unsigned delay_ms,delayed_reads,write_count;
static int pending_iterm,forever_iterm,read_failure,invalid_read;

static int observed_complete(NpBuck *b,uint64_t id,int status,uint32_t value)
{
 if(!active||!b->pending||id!=b->request_id)return np_buck_complete(b,id,status,value);
 if(b->command.property==NP_BUCK_TERM_CURRENT){
  if(b->command.write){
   ++write_count;next_iterm=b->command.value;
   active->iterm=previous_iterm;pending_iterm=1;
   apply_at=active->f.now_ms+delay_ms;write_deadline=b->command.deadline_ms;
  }else if(pending_iterm){
   ++delayed_reads;
   if(read_failure)status=-3;
   if(invalid_read)value=3001;
   assert(b->command.deadline_ms<=write_deadline);
  }
 }
 return np_buck_complete(b,id,status,value);
}
static void new_prepare(Peer *p,unsigned delay,int forever)
{
 prepare(p);p->iterm=0;
 active=0;previous_iterm=next_iterm=override_iterm=0;
 apply_at=write_deadline=0;delay_ms=delay;delayed_reads=write_count=0;
 pending_iterm=read_failure=invalid_read=0;forever_iterm=forever;
}
static void tick(Peer *p)
{
 if(pending_iterm&&!forever_iterm&&p->f.now_ms+100>=apply_at){
  p->iterm=override_iterm?override_iterm:next_iterm;pending_iterm=0;
 }
 previous_iterm=p->iterm;active=p;step(p);active=0;
}
static void reach(Peer *p,unsigned state)
{
 for(unsigned i=0;i<800;++i){tick(p);if(p->b.state==state)return;}
 fprintf(stderr,"confirmation state=%u expected=%u owned=%u reads=%u writes=%u now=%llu until=%llu\n",
  p->b.state,state,p->b.owned,delayed_reads,write_count,
  (unsigned long long)p->f.now_ms,(unsigned long long)p->b.stock.confirm_until_ms);
 assert(!"New confirmation case did not reach its expected state");
}
static void first_mismatch(Peer *p)
{
 for(unsigned i=0;i<800;++i){tick(p);if(delayed_reads)return;}
 assert(!"No post-write ITERM observation");
}
static NpPowerControl broker_at_next_read(Peer *p)
{
 p->f.now_ms+=100;p->f.sampled_ms=p->f.now_ms;
 NpBuckCommand command={};assert(np_buck_next(&p->b,&p->f,&command)>0);
 assert(!command.write&&command.property==NP_BUCK_TERM_CURRENT);
 assert(command.deadline_ms==write_deadline);
 NpPowerControl c={};c.mode=5;c.epoch=p->f.epoch;c.attachment=p->f.attachment;c.buck=p->b;
 c.pending_command=(NpAuxCommand){command.id,c.attachment,command.property,0,4,0,1,0};
 c.pending=1;return c;
}
int main(void)
{
 Peer p;
 new_prepare(&p,600,0);reach(&p,NP_BUCK_POLICY_MONITOR);
 assert(delayed_reads>=2&&write_count==1&&p.iterm==358&&p.fcc==1500);
 assert(p.b.stock.baseline[2]==0&&!p.b.unverified_effect&&!p.b.stock.confirm_until_ms);
 np_buck_stop(&p.b);reach(&p,NP_BUCK_FINISHED);
 assert(write_count==2&&p.iterm==0&&!p.b.owned&&!p.b.unverified_effect);
 assert(!p.en&&p.fcc==100);

 new_prepare(&p,600,0);first_mismatch(&p);
 assert(p.b.state==NP_BUCK_POLICY_CHECK&&p.b.stock.index==2&&p.b.unverified_effect);
 const uint64_t deadline=write_deadline;
 np_buck_stop(&p.b);tick(&p);
 assert(p.b.stock.index==2&&p.b.state==NP_BUCK_POLICY_CHECK&&p.en==1);
 assert(write_deadline==deadline&&write_count==1);
 reach(&p,NP_BUCK_FINISHED);
 assert(!p.en&&p.iterm==0&&!p.b.owned&&p.fcc_writes==2);

 new_prepare(&p,600,0);first_mismatch(&p);p.f.policy_enabled=0;tick(&p);
 assert(p.b.stock.index==2&&p.b.state==NP_BUCK_POLICY_CHECK&&write_count==1);
 reach(&p,NP_BUCK_POLICY_MONITOR);assert(!p.en&&p.fcc==500);

 new_prepare(&p,600,1);reach(&p,NP_BUCK_UNCERTAIN);
 assert(delayed_reads>1&&write_count==1&&p.b.owned&&p.b.unverified_effect&&p.fcc==500);
 assert(p.f.now_ms==write_deadline);
 const unsigned reads=delayed_reads,writes=write_count,all_writes=p.fcc_writes;
 for(unsigned i=0;i<30;++i)tick(&p);
 assert(delayed_reads==reads&&write_count==writes&&p.fcc_writes==all_writes);

 new_prepare(&p,500,0);override_iterm=350;reach(&p,NP_BUCK_UNCERTAIN);
 assert(p.iterm==350&&write_count==1&&p.b.unverified_effect&&p.fcc==500);

 new_prepare(&p,600,0);read_failure=1;reach(&p,NP_BUCK_UNCERTAIN);
 assert(delayed_reads==1&&write_count==1&&p.b.failures==1&&p.b.unverified_effect);
 new_prepare(&p,600,0);invalid_read=1;reach(&p,NP_BUCK_UNCERTAIN);
 assert(delayed_reads==1&&write_count==1&&p.b.unverified_effect);

 new_prepare(&p,600,0);
 for(unsigned i=0;i<800;++i){
  tick(&p);
  if(p.b.stock.actual[2]==358&&p.b.state==NP_BUCK_POLICY_SYNC)break;
 }
 assert(p.b.stock.actual[2]==358&&p.fcc==500);
 p.f.sampled_ms=p.f.now_ms-2000;
 NpBuckCommand next={};
 assert(!np_buck_next(&p.b,&p.f,&next)&&!p.b.pending&&p.b.blocked==NP_BUCK_STALE);
 p.f.sampled_ms=p.f.now_ms;
 assert(np_buck_next(&p.b,&p.f,&next)>0&&next.write&&next.property==NP_BUCK_FCC&&next.value==1500);
 assert(next.deadline_ms==p.f.now_ms+NP_BUCK_COMMAND_MS);

 new_prepare(&p,600,0);first_mismatch(&p);
 NpPowerControl c=broker_at_next_read(&p),before_deadline=c;
 uint8_t answer[4];np_put32(answer,358);
 assert(np_control_complete(&c,&c.pending_command,0,answer,4,write_deadline)<0);
 assert(c.buck.state==NP_BUCK_UNCERTAIN&&c.buck.unverified_effect&&c.errors==1);
 assert(np_control_complete(&before_deadline,&before_deadline.pending_command,
  0,answer,4,write_deadline-1)>0);
 assert(before_deadline.buck.stock.actual[2]==358&&!before_deadline.buck.unverified_effect);
 assert(!before_deadline.buck.stock.confirm_until_ms);
 puts("PASS10 new stock confirmation cases: delayed configuration and zero restore, stop/hot during confirmation, exact deadline, persistent zero, quantization mismatch, failed reads, fresh queue horizon and broker late-response rejection; one write per change, host only");
 return 0;
}
