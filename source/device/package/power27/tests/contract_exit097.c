/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Actual Buck exit with the 096 PMIC/CP readings and a new ordinary source. */
#define main retained_input_suspend_main
#include "input_suspend081.c"
#undef main

int main(int argc,char **argv)
{
 assert(argc==2);unsigned scenario=(unsigned)atoi(argv[1]);assert(scenario<14);
 NpBuck b;NpBuckFacts f;NpPdCaps pd;NpBuckCommand command;
 init(&b,&f,&pd);b.stock_current_enabled=b.stock_continuous=b.stop=1;
 b.state=NP_BUCK_PD_REQUEST_5V;b.owned=8u|16u|128u;
 b.input_suspend_confirmed=1;f.attachment=2;f.real_type=1;
 f.vbus_mv=4942;f.pump_bus_mv=4962;f.pd_state=f.data_role=0;
 static const unsigned ordinary[]={1,2,3,4,9,15};
 if(scenario<6)f.real_type=ordinary[scenario];
 if(scenario==6){f.online=0;f.vbus_mv=f.pump_bus_mv=0;}
 if(scenario==7)f.attachment=b.attachment;
 if(scenario==8)f.real_type=0;
 if(scenario==9)f.vbus_mv=6000;
 if(scenario==10)f.pump_bus_mv=6000;
 if(scenario==11)f.boost_enabled=1;
 if(scenario==12)f.gate_enabled=1;
 if(scenario==13)f.sampled_ms=f.now_ms-3001;
 assert(!np_buck_next(&b,&f,&command));
 if(scenario>=7){
  assert(b.state==NP_BUCK_PD_REQUEST_5V&&b.owned==(8u|16u|128u)&&!b.pending);
  puts("PASS097 unavailable/high/unknown/same source retains the old contract owner");return 0;
 }
 assert(b.state==NP_BUCK_PD_OBSERVE_5V&&!b.pending&&!b.voltage_samples);
 assert(!np_buck_next(&b,&f,&command)&&!b.voltage_samples);
 fresh(&b,&f,&pd,10100);
 assert(!np_buck_next(&b,&f,&command)&&b.voltage_samples==1&&(b.owned&8u));
 assert(!np_buck_next(&b,&f,&command)&&b.voltage_samples==1);
 fresh(&b,&f,&pd,10200);f.pump_ms=10100;
 assert(!np_buck_next(&b,&f,&command)&&b.voltage_samples==1&&(b.owned&8u));
 f.pump_ms=10200;
 assert(!np_buck_next(&b,&f,&command)&&b.state==NP_BUCK_PUMP_RESTORE_MODE&&!(b.owned&8u));
 assert(b.confirmed_vbus_mv==f.vbus_mv); /* Zero disappearance never becomes a fictional 5V. */
 assert(np_buck_next(&b,&f,&command)==1&&command.property==NP_BUCK_PUMP_MODE_RESTORE);
 assert(np_buck_complete(&b,command.id,0,0)==1&&b.state==NP_BUCK_PUMP_INPUT_RESUME);
 fresh(&b,&f,&pd,10400);
 assert(np_buck_next(&b,&f,&command)==1&&command.property==NP_BUCK_INPUT_SUSPEND&&!command.value);
 assert(np_buck_complete(&b,command.id,0,0)==1&&(b.owned&128u));
 fresh(&b,&f,&pd,b.input_after_ms+100);
 if(!f.online){f.main_buck_state=7;f.main_buck_bus_ma=0;f.battery_ma=-100;}
 assert(!np_buck_next(&b,&f,&command)&&b.input_samples==1&&(b.owned&128u));
 fresh(&b,&f,&pd,f.now_ms+100);
 assert(!np_buck_next(&b,&f,&command)&&b.state==NP_BUCK_FINISHED&&!b.owned&&!b.pending);
 puts("PASS097 two fresh low PMIC/CP cohorts, real measured voltage, restore mode/input, no PD to new ordinary source");
 return 0;
}
