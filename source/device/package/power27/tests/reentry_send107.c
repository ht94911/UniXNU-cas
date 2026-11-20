/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Consume the actual low-current startup plan through its wire predicate. */
#include <errno.h>
#define NP_LOW_START105_MAIN retained_low_start105_main
#include "low_start105.c"
#undef NP_LOW_START105_MAIN

static void captured_normal(NpPowerControl *out)
{
 NpPowerControl c;ready_normal(&c);
 c.stock_continuous=c.buck.stock_continuous=1;
 c.buck.stock.actual[0]=c.buck.stock.target[0]=1600;
 c.meta[NP_META_ICL].value=1600;
 c.buck.target_fcc=500;c.buck.state=NP_BUCK_POLICY_SYNC;
 c.buck.confirmation=NP_BUCK_CONFIRM_REDUCTION_ACK;
 c.sample.gauge.current_ma=-214;c.sample.gauge.voltage_mv=3660;
 c.sample.gauge.cycles=338;c.sample.gauge.raw[2]=3087;
 c.pump.battery_mv=3717;
 c.charger_thermal.current_ua=15600000;c.charger_thermal_device_ma=15600;
 c.sample.gauge.end_ms=c.sample.charger.end_ms=10000;
 c.meta[NP_META_ENABLED].at_ms=9900;
 /* Captured734.325s boost metadata was still1454ms old at planning,
  * then1506ms old at the next50ms dispatch. It remains within3s. */
 c.meta[NP_META_BOOST].at_ms=8548;
 *out=c;
}
int main(int argc,char **argv)
{
 assert(argc==2);unsigned scenario=(unsigned)atoi(argv[1]);assert(scenario<15);
 setbuf(stdout,NULL);NpPowerControl c;captured_normal(&c);
 NpAuxCommand command;assert(np_control_next(&c,10000,&command)==1);
 assert(command.control==1&&command.write&&command.property==NP_BUCK_ICL&&command.value==3000);
 const uint64_t dispatch=10052;
 if(scenario==2)c.charger_thermal_device_ma=1500;
 if(scenario==3){c.jeita.vote.full=1;c.jeita.vote.enabled=0;}
 if(scenario==4)c.meta[NP_META_BOOST].valid=0;
 if(scenario==5)c.adapter_verified=0;
 if(scenario==6)c.ffc_owned=1;
 if(scenario==7)c.caps.entry[0].max_ma=2000;
 if(scenario==8)c.buck.stop=1;
 if(scenario==9)c.sample.gauge.end_ms=7000;
 if(scenario==10)++command.value;
 if(scenario==11){c.pump.protection.faults=1;c.pump_at=10000;}
 if(scenario==12)++c.attachment;
 NpBuckFacts f;np_control_facts(&c,dispatch,&f);
 NpBuck normal=c.buck,planned;uint32_t icl=0;
 normal.pending=normal.unverified_effect=0;normal.state=NP_BUCK_POLICY_MONITOR;
 normal.stock.target[0]=normal.stock.actual[0];
 printf("created state=%u reentry=%u FCC=%u confirmation=%u fresh=%llu prepare=%d stockallow=%d wireallow=%d\n",
  c.buck.state,c.buck.stock.reentry,c.buck.target_fcc,c.buck.confirmation,
  (unsigned long long)(f.now_ms-f.sampled_ms),np_buck_prepare_reentry(&normal,&f,&planned,&icl),
  np_buck_stock_write_allowed(&c.buck,&f,command.property,command.value),np_control_write_allowed(&c,&command,dispatch));
 const int initial_allowed=np_control_write_allowed(&c,&command,scenario==1?11500:dispatch);
 if(scenario>=1&&scenario<=12){assert(!initial_allowed);puts("PASS107 rejection of actual changed source/facts/deadline");return 0;}
 if(scenario==13){
  assert(initial_allowed);c.buck.wire_accepted=1;
  assert(np_control_complete(&c,&command,-ETIMEDOUT,NULL,0,10075)>0);
  assert(c.buck.stop&&c.buck.owned&&c.buck.unverified_effect);
  puts("PASS107 accepted unknown write remains pending cleanup");return 0;
 }
 assert(np_control_complete(&c,&command,-2,NULL,0,10075)>0);
 c.buck.stop=1;c.buck.state=NP_BUCK_POLICY_RESTORE_WRITE;
 c.buck.stock.index=0;c.buck.stock.check_stopping=0;
 c.buck.restore_value=100;c.buck.stock.target[0]=3000;
 c.buck.stock.actual[0]=1600;c.buck.owned=5;
 np_control_facts(&c,10076,&f);NpBuckCommand restore;
 assert(np_buck_issue(&c.buck,&f,&restore,1,NP_BUCK_ICL,100)==1);
 c.pending_command=(NpAuxCommand){restore.id,c.attachment,restore.property,restore.value,0,1,1,0};
 c.pending=1;
 if(scenario==14)c.pending_command.value=1600;
 const int restore_allowed=np_control_write_allowed(&c,&c.pending_command,10077);
 printf("initial_wire=%d restore100_wire=%d current_metadata_age=%llu\n",initial_allowed,restore_allowed,
  (unsigned long long)(10077-f.sampled_ms));
 assert(initial_allowed&&(scenario==14?!restore_allowed:restore_allowed));
 printf("PASS107 scenario%u existing command horizon and typed cleanup\n",scenario);
 return 0;
}
