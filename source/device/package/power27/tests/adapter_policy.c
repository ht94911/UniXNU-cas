/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaCharge.h"
#include "NezhaMca.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static NpPdCaps caps(void)
{
 const uint32_t values[7][4]={{5000,5000,3000,0},{9000,9000,3000,0},
  {15000,15000,3000,0},{20000,20000,3250,0},{3600,11000,6100,3},
  {3600,20000,5000,3},{3600,21000,4750,3}};
 uint8_t raw[NP_PD_BYTES];
 for(unsigned row=0;row<7;++row)for(unsigned i=0;i<4;++i)np_put32(raw+row*16+i*4,values[row][i]);
 NpPdCaps pd;assert(!np_pd_decode(&pd,raw,sizeof(raw),1,10000));return pd;
}
static NpChargeFacts facts(void)
{
 NpChargeFacts f={0};f.epoch=f.profile=f.attachment=1;f.now_ms=f.sample_ms=10000;
 f.wired=f.sink=f.external=f.sample_valid=f.protocol_verified=1;
 f.battery_authenticated=f.adapter_authenticated=f.cable_authenticated=1;
 f.protections_verified=f.autonomous_fault_shutdown=f.limits_valid=f.remote_ready=1;
 f.pump_off_confirmed=1;f.cable_max_ma=f.input_limit_ma=f.thermal_input_ma=5000;
 f.battery_limit_ma=15600;f.board_power_mw=90000;return f;
}
static NpCccv curve(void)
{
 NpCccv c={0};c.epoch=c.attachment=1;c.sampled_ms=10000;c.valid=1;c.cycles=331;c.cycles_valid=1;
 c.current_max_ma=15600;c.current_min_ma=7872;c.voltage_mv=4310;c.rows=2;return c;
}
int main(int argc,char **argv)
{
 NpPdCaps pd=caps();NpAdapterChoice choice;NpChargeFacts f=facts();NpCccv cc=curve();
 if(argc==3){
  int status=np_adapter_select(&pd,1,(unsigned)atoi(argv[1]),atoi(argv[2]),&choice);
  if(status){puts("invalid");return 0;}
  printf("%u %u %u %u %u %u\n",choice.ratio,choice.pdo_index,choice.minimum_mv,
   choice.maximum_mv,choice.adapter_ma,choice.maximum_battery_ma);return 0;
 }
 assert(np_adapter_stock_support100w(0,0,0)==0);
 assert(np_adapter_stock_support100w(1,0,0)==1);
 assert(np_adapter_stock_support100w(1,0,1)==0);
 assert(np_adapter_stock_support100w(2,0,0)==0);
 assert(np_adapter_stock_support100w(2,0,1)==1);
 assert(np_adapter_stock_support100w(3,0,0)==1);
 assert(np_adapter_stock_support100w(3,0,1)==1);
 assert(np_adapter_stock_support100w(0,1,0)==1);
 assert(np_adapter_stock_support100w(4,0,0)<0);
 assert(!np_adapter_select(&pd,1,11,0,&choice)&&choice.ratio==2&&choice.pump_mode==1&&choice.pdo_index==4);
 assert(!np_adapter_select(&pd,1,11,1,&choice)&&choice.ratio==4&&choice.pump_mode==0&&choice.pdo_index==5);
 assert(!np_adapter_select(&pd,1,12,-1,&choice)&&choice.ratio==4);
 assert(np_adapter_select(&pd,1,11,-1,&choice)<0&&!choice.valid);
 assert(np_adapter_select(&pd,2,11,1,&choice)<0&&!choice.valid);
 assert(np_adapter_select(&pd,1,10,1,&choice)<0&&!choice.valid);
 NpPdCaps fixed=pd;fixed.present_mask=15;
 assert(np_adapter_select(&fixed,1,11,1,&choice)<0&&!choice.valid);
 pd.entry[6].max_ma=5000;
 assert(!np_adapter_select(&pd,1,11,1,&choice)&&choice.pdo_index==6);
 pd=caps();
 NpChargePolicy p;NpChargeCommand out;np_charge_init(&p);
 int got=np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out);
 assert(got==1&&out.operation==NP_ACTION_CONFIGURE_DISABLED_PUMP&&out.mv==17400&&out.ma==3900&&out.battery_ma==15600);
 assert(p.adapter_ratio==4&&p.adapter_attachment==1);
 assert(np_charge_ack(&p,1,out.request_id,0,10001)==1);
 f.now_ms=f.sample_ms=10002;
 assert(np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)==1&&out.operation==NP_ACTION_REQUEST_PPS);
 uint64_t old_id=out.request_id;
 f.thermal_input_ma=500;
 assert(np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)==1&&out.operation==NP_ACTION_DISABLE_PUMP);
 assert(!np_charge_ack(&p,1,old_id,0,10003));
 assert(np_charge_ack(&p,1,out.request_id,0,10003)==1);
 f.now_ms=f.sample_ms=10004;
 assert(np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)==1&&out.operation==NP_ACTION_RELEASE_OWN_VOTES);
 assert(np_charge_ack(&p,1,out.request_id,0,10005)==1&&!p.owned);
 f=facts();f.requested_ma=2000;f.requested_battery_ma=4000;np_charge_init(&p);
 assert(np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)==1&&out.ma==1000&&out.battery_ma==4000);
 f=facts();f.board_power_mw=30000;np_charge_init(&p);
 assert(np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)==1&&out.ma==1700&&out.battery_ma==6800);
 f=facts();f.cable_max_ma=1000;np_charge_init(&p);
 assert(np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)==1&&out.ma==1000&&out.battery_ma==4000);
 f=facts();f.usb_data_budget_ma=1500;np_charge_init(&p);
 assert(np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)==1&&out.ma==1500&&out.battery_ma==6000);
 /* Full existing command/ACK lifetime with the selected input. Changing a
  * lower CV-row budget must invalidate an in-flight increase before its ACK. */
 f=facts();np_charge_init(&p);
 assert(np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)==1);
 assert(np_charge_ack(&p,1,out.request_id,0,10001)==1);
 f.now_ms=f.sample_ms=10002;
 assert(np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)==1&&out.operation==NP_ACTION_REQUEST_PPS);
 assert(np_charge_ack(&p,1,out.request_id,0,10003)==1);
 f.now_ms=f.sample_ms=10004;
 assert(np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)==1&&out.operation==NP_ACTION_VERIFY_CONTRACT);
 assert(np_charge_ack(&p,1,out.request_id,0,10005)==1);
 f.now_ms=f.sample_ms=10006;f.contract_valid=1;f.actual_mv=17400;f.actual_ma=3900;
 assert(np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)==1&&out.operation==NP_ACTION_ENABLE_PUMP);
 old_id=out.request_id;cc.current_max_ma=7872;
 f.now_ms=f.sample_ms=10007;
 assert(np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)==1&&out.operation==NP_ACTION_DISABLE_PUMP);
 assert(!np_charge_ack(&p,1,old_id,0,10008));
 cc=curve();
 f=facts();np_charge_init(&p);
 assert(np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)==1);
 assert(np_charge_ack(&p,1,out.request_id,0,10001)==1);
 f.now_ms=f.sample_ms=10002;f.attachment=2;cc.attachment=2;
 assert(np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)==1&&out.operation==NP_ACTION_DISABLE_PUMP);
 f=facts();cc=curve();np_charge_init(&p);cc.sampled_ms=6999;
 assert(!np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)&&!p.owned);
 cc=curve();pd.at_ms=6999;
 assert(!np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)&&!p.owned);
 pd=caps();f.cable_authenticated=0;
 assert(!np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)&&!p.owned);
 f=facts();cc=curve();cc.cycles_valid=0;np_charge_init(&p);
 assert(!np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)&&!p.owned);
 cc=curve();
 f=facts();f.battery_limit_ma=1500;
 assert(!np_charge_adapter_step(&p,&f,&pd,&cc,4200,11,1,&choice,&out)&&!p.owned);
 puts("PASS APDO mode choice and actual charge consumer: stock tie, budgets, taper, detach, no stale ACK increase");
 return 0;
}
