/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaPowerControl.h"
#include "NezhaMca.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void fresh(NpBuckFacts *f,NpPdCaps *pd,NpBuck *b,uint64_t ms)
{f->now_ms=f->sampled_ms=f->gauge_ms=f->charger_ms=f->pump_ms=ms;pd->at_ms=ms;b->cccv.sampled_ms=ms;}
int main(int argc,char **argv)
{
 assert(argc==2);unsigned scenario=(unsigned)atoi(argv[1]);assert(scenario<20);
 NpPdCaps pd={.epoch=1,.at_ms=10000,.valid=1,.present_mask=15};
 pd.entry[0]=(NpPdEntry){5000,5000,3000,NP_PD_FIXED};pd.entry[1]=(NpPdEntry){3600,11000,6100,NP_PD_APDO};pd.entry[2]=(NpPdEntry){3600,20000,5000,NP_PD_APDO};pd.entry[3]=(NpPdEntry){3600,21000,4750,NP_PD_APDO};
 NpBuck b={0};b.backend=NP_BUCK_OS4_PPS_ADAPTER_TEST;b.armed=1;b.epoch=b.attachment=1;b.state=NP_BUCK_PUMP_RUN;b.owned=122;b.pump_target_mv=17400;b.pump_request_ma=1000;b.lease_until=300000;
 b.cccv=(NpCccv){.epoch=1,.attachment=1,.sampled_ms=10000,.stage=2,.rows=2,.valid=1,.initialized=1,.voltage_mv=4464,.current_max_ma=7872,.current_min_ma=800,.cycles=331,.cycles_valid=1,.last_voltage_mv=4464};
 assert(!np_adapter_select(&pd,1,11,1,&b.adapter)&&b.adapter.ratio==4&&b.adapter.pump_mode==0);
 NpBuckFacts f={0};f.epoch=f.attachment=1;f.pd=&pd;fresh(&f,&pd,&b,10000);f.valid=f.online=f.charge_enabled=f.pmic_charging=f.policy_valid=f.policy_enabled=1;f.real_type=11;f.stock_support100w=1;f.pd_state=2;f.data_role=2;f.icl_ma=5000;f.term_mv=4400;f.policy_fcc=4000;f.policy_fv=4470;f.battery_mv=4300;f.pump_battery_mv=4200;f.battery_ma=3000;f.temperature_centi_c=3650;f.vbus_mv=f.pump_bus_mv=17400;f.pump_controls_valid=f.pump_switching_valid=f.pump_enable=f.pump_switching=1;f.pump_valid=23;f.pump_mode=0;f.pump_die_centi_c=4000;f.main_buck_bus_valid=1;f.main_buck_bus_ma=0;f.pump_bus_ma=500;
 if(scenario==1)b.cccv.cycles_valid=0;
 if(scenario==2)f.stock_support100w=-1;
 if(scenario==3)pd.entry[2].max_mv=17000;
 if(scenario==4)f.pump_mode=1;
 if(scenario==5){b.cccv.stage=3;f.battery_mv=4465;}
 if(scenario==6){b.state=NP_BUCK_IDLE;b.epoch=b.attachment=b.owned=0;b.pump_target_mv=0;f.pump_known_off=1;f.pump_enable=f.pump_switching=0;f.vbus_mv=f.pump_bus_mv=5000;}
 if(scenario==7){b.pump_target_mv=20000;f.vbus_mv=f.pump_bus_mv=20000;}
 if(scenario==8)f.attachment=2;
 if(scenario==9)f.main_buck_bus_valid=0;
 if(scenario==10){b.state=NP_BUCK_PUMP_SET_MODE;f.pump_enable=f.pump_switching=0;f.pump_known_off=1;}
 if(scenario==11){b.state=NP_BUCK_PUMP_START;f.pump_known_off=1;f.pump_enable=f.pump_switching=0;}
 if(scenario==12){b.state=NP_BUCK_PUMP_REGULATE;b.cccv_target_mv=17420;f.stock_support100w=0;}
 if(scenario==13){b.state=NP_BUCK_PUMP_REGULATE;b.cccv_target_mv=17420;f.pump_enable=f.pump_switching=0;f.pump_known_off=1;f.vbus_mv=f.pump_bus_mv=5000;}

 if(scenario>=14){f.charger_thermal_enabled=f.charger_thermal_valid=1;f.charger_thermal_ma=scenario==14?10000:scenario==15?3500:scenario==16?1000:4000;
  if(scenario==17)f.charger_thermal_valid=0;
  if(scenario==18||scenario==19){b.state=NP_BUCK_IDLE;b.epoch=b.attachment=b.owned=0;b.pump_target_mv=0;f.pump_known_off=1;f.pump_enable=f.pump_switching=0;f.vbus_mv=f.pump_bus_mv=5000;f.charger_thermal_ma=scenario==18?3000:1999;f.battery_ma=1000;}
 }
 NpBuckCommand cmd={0};int got=np_buck_next(&b,&f,&cmd);
 if(scenario>=14){
  if(scenario==14)assert(got==1&&cmd.property==NP_BUCK_PPS_PD&&b.pump_request_ma==1000);
  if(scenario>=15&&scenario<=17)assert(got==1&&cmd.property==NP_BUCK_PUMP_DISABLE&&b.stop);
  if(scenario==18)assert(got==1&&cmd.property==NP_BUCK_FCC&&cmd.value==500&&b.pump_request_ma==750);
  if(scenario==19)assert(got==1&&cmd.property==NP_BUCK_FCC&&cmd.value==500&&b.pump_request_ma==450);
  printf("PASS thermalBuck072case%u ma%u action%x\n",scenario,b.pump_request_ma,cmd.property);return 0;
 }
 if(scenario==10){assert(got==1&&cmd.property==NP_BUCK_PUMP_MODE1&&cmd.value==0);}
 else if(scenario==11){assert(got==1&&cmd.property==NP_BUCK_PUMP_ENABLE&&cmd.value==17400);}
 else if(scenario==12||scenario==13){assert(got==1&&cmd.property==NP_BUCK_PUMP_DISABLE&&b.stop);}
 else if((scenario>=1&&scenario<=4)||scenario==8||scenario==9){assert(got==1&&b.stop&&cmd.property==NP_BUCK_PUMP_DISABLE);}
 else if(scenario==6){assert(got==1&&cmd.property==NP_BUCK_FCC&&cmd.value==500&&b.adapter.ratio==4&&b.pump_target_mv==17400&&b.pump_request_ma==1000);}
 else if(scenario==7){assert(!got&&!cmd.property&&!b.stop);}
 else{
  assert(got==1&&cmd.property==NP_BUCK_PPS_PD&&(cmd.value&65535)==20);
  uint32_t target=(cmd.value>>16)*20;assert(target==(scenario==5?17380u:17420u));
  assert(np_buck_complete(&b,cmd.id,0,0)==1&&b.state==NP_BUCK_PUMP_OBSERVE_REGULATION);
  fresh(&f,&pd,&b,12000);(void)np_buck_next(&b,&f,&cmd);assert(b.voltage_samples==1&&!b.cccv_updates);
  (void)np_buck_next(&b,&f,&cmd);assert(b.voltage_samples==1&&!b.cccv_updates);
  fresh(&f,&pd,&b,12100);(void)np_buck_next(&b,&f,&cmd);assert(b.state==NP_BUCK_PUMP_RUN&&b.cccv_updates==1&&b.pump_target_mv==target);
 }
 printf("PASS adapter controller case%u, actual4:1 selection and ratio-budgeted1A, one ACK/two new voltage cohorts\n",scenario);
 return 0;
}
