/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Run the actual initial lowFCC/profile admission on the original controller
 * fixture, including delayed main-state samples and stale command horizon. */
#define NP_STOCK_CURRENT_MAIN retained_stockcurrent105_main
#include "stock_current083.c"
#undef NP_STOCK_CURRENT_MAIN
static void ready_normal(NpPowerControl *c)
{
 controller(c);c->buck.backend=NP_BUCK_OS4_STOCK_POLICY;c->buck.armed=1;
 c->buck.stop=0;c->adapter_verified=1;np_control_adapter_support(c,1);
 c->buck.epoch=1;c->buck.attachment=c->attachment;c->buck.owned=7;
 c->buck.stock.configured=1;c->buck.stock.seen=11;
 c->buck.stock.baseline[0]=3000;c->buck.stock.actual[0]=c->buck.stock.target[0]=1500;
 c->buck.stock.baseline[1]=4400;c->buck.stock.actual[1]=c->buck.stock.target[1]=4470;
 c->buck.stock.baseline[3]=c->buck.stock.actual[3]=c->buck.stock.target[3]=1;
 c->caps.present_mask=3;c->caps.entry[1]=(NpPdEntry){3600,20000,5000,NP_PD_APDO};
 c->charger_thermal.current_ua=3500000;c->charger_thermal_device_ma=3500;
 c->sample.gauge.soc=24;c->sample.gauge.raw[2]=3095;c->sample.gauge.cycles=334;
 c->sample.gauge.full_mah=5700;c->sample.gauge.remaining_mah=1368;
 c->sample.gauge.voltage_mv=c->pump.battery_mv=3600;
 c->sample.charger.epoch=1;c->sample.charger.state_valid=1;c->sample.charger.state_raw=3;
 c->sample.charger.usb_ua=500000;c->sample.charger.input_current_scope=NP_INPUT_CURRENT_MAIN_BUCK;
 c->pump.bus_mv=5000;c->pump.die_centi_c=4000;c->pump.protection.valid=c->pump.protection.bus_ocp_enabled=1;
 c->pump.protection.bus_ocp_ma=3300;c->pump.controls_valid=c->pump.switching_valid=1;
 np_control_profile(c,&(NpGaugeIdentity){.cell_code='F',.device_name="BM6N",.valid=3},1);
 np_control_sample(c,&c->sample,&c->pump,0,10000);
}

#ifndef NP_LOW_START105_MAIN
#define NP_LOW_START105_MAIN main
#endif
int NP_LOW_START105_MAIN(int argc,char **argv)
{
 assert(argc==2);unsigned variant=(unsigned)atoi(argv[1]);assert(variant<8);
 NpPowerControl c;ready_normal(&c);
 c.stock_continuous=c.buck.stock_continuous=1;
 c.buck.stock.actual[0]=c.buck.stock.target[0]=1600;
 c.meta[NP_META_ICL].value=1600;
 c.buck.target_fcc=500;c.buck.state=NP_BUCK_POLICY_SYNC;
 c.buck.confirmation=NP_BUCK_CONFIRM_REDUCTION_ACK;
 c.sample.gauge.current_ma=-585;
 c.sample.gauge.end_ms=c.sample.charger.end_ms=10000;
 c.meta[NP_META_ENABLED].at_ms=9900;
 if(variant==1)c.sample.gauge.end_ms=c.sample.charger.end_ms=8499;
 if(variant==2)c.sample.gauge.end_ms=c.sample.charger.end_ms=8500;
 if(variant==3){c.sample.charging=0;c.sample.charger.end_ms=9800;}
 if(variant==4){c.sample.charging=0;c.sample.charger.end_ms=10000;}
 if(variant==5)c.adapter_verified=0;
 if(variant==6)c.charger_thermal.valid=0;
 if(variant==7){c.sample.fully_charged=1;c.jeita.vote.full=1;c.jeita.vote.enabled=0;}
 NpAuxCommand command;int got=np_control_next(&c,10000,&command);
 if(variant==0||variant==2){
  assert(got==1&&command.control==1&&command.write&&command.property==NP_BUCK_ICL&&command.value==3000);
  assert(c.buck.target_fcc==500&&c.buck.stock.reentry==1);
 }else if(variant==1||variant==3){
  assert(!got||!command.write);
  assert(c.buck.target_fcc==500&&c.buck.state==NP_BUCK_POLICY_SYNC);
 }else{
  assert(!got||command.property!=NP_BUCK_ICL||command.value!=3000);
  assert(!got||command.control!=2);
 }
 printf("PASS lowStart105 case%u oldcohort/commandhorizon/negativecurrent and existingauth/thermal/full gates\n",variant);
 return 0;
}
