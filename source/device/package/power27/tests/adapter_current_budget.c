/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaCharge.h"
#include <assert.h>
#include <stdio.h>

static NpPdCaps caps(void)
{
 NpPdCaps p={0};p.epoch=1;p.at_ms=10000;p.valid=1;p.present_mask=3;
 p.entry[0]=(NpPdEntry){3600,11000,6100,NP_PD_APDO};
 p.entry[1]=(NpPdEntry){3600,20000,5000,NP_PD_APDO};
 return p;
}
static NpChargeFacts facts(void)
{
 NpChargeFacts f={0};f.epoch=f.profile=f.attachment=1;f.now_ms=f.sample_ms=10000;
 f.wired=f.sink=f.external=f.sample_valid=f.protocol_verified=1;
 f.battery_authenticated=f.adapter_authenticated=f.cable_authenticated=1;
 f.protections_verified=f.autonomous_fault_shutdown=f.limits_valid=f.remote_ready=1;
 f.pump_off_confirmed=1;f.cable_max_ma=f.input_limit_ma=f.thermal_input_ma=6100;
 f.battery_limit_ma=15600;f.board_power_mw=90000;return f;
}
static NpCccv curve(void)
{
 NpCccv c={0};c.epoch=c.attachment=1;c.sampled_ms=10000;c.valid=1;
 c.cycles=331;c.cycles_valid=1;c.current_max_ma=15600;
 c.current_min_ma=7872;c.voltage_mv=4310;c.rows=2;return c;
}
static void expect(NpChargeFacts f,NpCccv c,int support,
                    uint32_t input,uint32_t battery)
{
 NpPdCaps pd=caps();NpChargePolicy p;NpAdapterChoice choice;NpChargeCommand cmd;
 np_charge_init(&p);
 int result=np_charge_adapter_step(&p,&f,&pd,&c,4200,11,support,&choice,&cmd);
 if(!input){assert(result==0&&!p.owned);return;}
 assert(result==1&&cmd.operation==NP_ACTION_CONFIGURE_DISABLED_PUMP);
 assert(cmd.ma==input&&cmd.battery_ma==battery);
 assert(cmd.ma%50==0&&cmd.ma*choice.ratio<=f.battery_limit_ma);
 assert(cmd.ma*choice.ratio<=(uint32_t)c.current_max_ma);
 if(f.requested_battery_ma)assert(cmd.ma*choice.ratio<=f.requested_battery_ma);
 uint32_t encoded;assert(!np_pps_encode(cmd.mv,cmd.ma,&encoded));
 assert((encoded&65535)*50==cmd.ma);
}
int main(void)
{
 NpChargeFacts f=facts();NpCccv c=curve();
 expect(f,c,1,3900,15600);expect(f,c,0,5500,11000);
 f.requested_ma=2000;f.requested_battery_ma=4000;
 expect(f,c,1,1000,4000);expect(f,c,0,2000,4000);
 f=facts();f.battery_limit_ma=4099;expect(f,c,1,1000,4000);
 f.battery_limit_ma=3999;expect(f,c,1,950,3800);
 f=facts();c.current_max_ma=7872;expect(f,c,1,1950,7800);
 c=curve();f.board_power_mw=30000;expect(f,c,1,1700,6800);
 f=facts();f.cable_max_ma=1000;expect(f,c,1,1000,4000);
 f=facts();f.input_limit_ma=750;expect(f,c,1,750,3000);
 f=facts();f.thermal_input_ma=1123;expect(f,c,1,1100,4400);
 f=facts();f.usb_data_budget_ma=949;expect(f,c,1,900,3600);
 f=facts();f.requested_ma=901;expect(f,c,1,900,3600);
 f=facts();f.requested_battery_ma=1500;expect(f,c,1,0,0);
 f=facts();f.battery_limit_ma=0;expect(f,c,1,0,0);
 f=facts();f.cable_authenticated=0;expect(f,c,1,0,0);
 puts("PASS 16 adapter current budgets: ratio, curve, wire rounding and all existing lower ceilings");
 return 0;
}
