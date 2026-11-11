/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Only the new actual ITERM0 baseline and its affected error/readback paths. */
#define main legacy_stock_cases_not_run
#include "stock_policy_cases.c"
#undef main

static NpBuckCommand baseline_read(Peer *p)
{
 p->f.sampled_ms=p->f.now_ms;p->f.icl_ma=p->icl;p->f.term_mv=p->fv;p->f.charge_enabled=p->en;
 NpBuckCommand command;
 assert(np_buck_next(&p->b,&p->f,&command)==1);
 assert(!command.write&&command.property==NP_BUCK_TERM_CURRENT);
 return command;
}
int main(void)
{
 Peer p;prepare(&p);p.iterm=0;
 until(&p,NP_BUCK_POLICY_MONITOR);
 assert(p.b.stock.baseline[2]==0&&p.iterm==358&&p.icl==1500&&p.fv==4480&&p.fcc==1500);
 np_buck_stop(&p.b);until(&p,NP_BUCK_FINISHED);
 assert(p.iterm==0&&!p.en&&p.fcc==100&&!p.b.owned&&!p.b.unverified_effect);

 prepare(&p);NpBuckCommand command=baseline_read(&p);
 assert(np_buck_complete(&p.b,command.id,-1,0)==1);
 assert(p.b.state==NP_BUCK_FINISHED&&p.b.blocked==NP_BUCK_BASELINE&&!p.b.armed&&
        p.b.failures==1&&!p.b.owned&&!p.b.unverified_effect);
 unsigned writes=p.fcc_writes;for(unsigned i=0;i<20;++i)step(&p);assert(p.fcc_writes==writes);

 prepare(&p);command=baseline_read(&p);
 assert(np_buck_complete(&p.b,command.id,0,2001)==1);
 assert(p.b.state==NP_BUCK_FINISHED&&p.b.blocked==NP_BUCK_BASELINE&&!p.b.owned);

 prepare(&p);p.iterm=0;
 for(unsigned i=0;i<800&&!p.writes[2];++i)step(&p);
 assert(p.writes[2]==1&&p.iterm==358&&(p.b.owned&8));
 p.iterm=0;step(&p); /* The accepted write was ignored by this peer. */
 assert(p.b.state==NP_BUCK_UNCERTAIN&&(p.b.owned&8)&&p.b.unverified_effect);
 puts("PASS four new ITERM0 cases: captured-zero configure/restore, no-effect read failures and retained failed write readback; old suites not run");
 return 0;
}
