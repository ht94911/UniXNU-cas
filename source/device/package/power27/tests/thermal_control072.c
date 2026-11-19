/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaPowerControl.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
static NpBoardThermal board(int32_t mc,uint64_t generation,uint64_t now)
{
 NpBoardThermal b={0};b.enabled=b.derived_valid=1;b.valid_mask=b.attempted_mask=255;
 b.epoch=b.attachment=1;b.generation=generation;b.begin_ms=now-50;b.end_ms=now;
 b.charger_milli_c=mc;for(unsigned i=0;i<8;++i)b.sampled_ms[i]=now-8+i;return b;
}
int main(void)
{
 NpPowerControl c;np_control_init(&c,9);np_control_link(&c,1,1);np_control_charger_thermal_enable(&c,1);
 NpBuckFacts f;np_control_facts(&c,1000,&f);assert(f.charger_thermal_enabled&&!f.charger_thermal_valid);
 NpBoardThermal b=board(39500,1,10000);assert(!np_control_charger_thermal_sample(&c,&b,10001));
 assert(c.charger_thermal.valid&&c.charger_thermal.stage==3&&c.charger_thermal.current_ua==4300000);
 NpChargerThermalCurrent prior=c.charger_thermal;
 assert(np_control_charger_thermal_sample(&c,&b,10002)==EAGAIN);
 assert(c.charger_thermal.valid&&c.charger_thermal.current_ua==prior.current_ua&&c.charger_thermal.generation==prior.generation);
 np_control_facts(&c,10002,&f);assert(f.charger_thermal_valid&&f.charger_thermal_ma==4300);
 np_control_facts(&c,12951,&f);assert(!f.charger_thermal_valid);
 b=board(44700,2,11000);assert(!np_control_charger_thermal_sample(&c,&b,11001));
 assert(c.charger_thermal.current_ua==1000000&&c.charger_thermal.stage==6);
 np_control_facts(&c,11001,&f);assert(f.charger_thermal_valid&&f.charger_thermal_ma==1000);
 b=board(44700,3,12000);b.valid_mask=254;
 assert(np_control_charger_thermal_sample(&c,&b,12001)==ENODATA&&!c.charger_thermal.valid);
 b=board(35000,4,13000);assert(!np_control_charger_thermal_sample(&c,&b,13001));
 assert(c.charger_thermal.current_ua<=10000000&&c.charger_thermal.current_ua>=5800000);
 b.epoch=2;assert(np_control_charger_thermal_sample(&c,&b,13002)==ESTALE&&!c.charger_thermal.valid);
 np_control_source_event(&c);assert(!c.charger_thermal.initialized&&!c.charger_thermal.valid);
 b=board(13000,1,14000);b.attachment=c.attachment;
 assert(!np_control_charger_thermal_sample(&c,&b,14001)&&c.charger_thermal.current_ua==22000000);
 /* The original kernel discards fractional mA before the next sysfs read. */
 c.charger_thermal.screen_valid=1;c.charger_thermal.screen_on=0;
 c.charger_thermal.charger_shift_milli_c=8000;
 b=board(42000,2,15000);b.attachment=c.attachment;
 assert(!np_control_charger_thermal_sample(&c,&b,15001));
 c.charger_thermal.current_ua=5000999;
 b=board(42000,3,16000);b.attachment=c.attachment;
 assert(!np_control_charger_thermal_sample(&c,&b,16001));
 assert(c.charger_thermal.current_ua==5000000);
 np_control_link(&c,1,0);np_control_facts(&c,14001,&f);assert(!f.charger_thermal_valid);
 np_control_init(&c,8);np_control_charger_thermal_enable(&c,1);assert(!c.charger_thermal_enabled);
 /* Original thermal device and sensor history have separate lifetimes. */
 np_control_init(&c,9);c.stock_continuous=c.buck.stock_continuous=1;
 c.buck.stock_current_enabled=1;np_control_link(&c,1,1);
 np_control_charger_thermal_enable(&c,1);c.meta[NP_META_TYPE]=(NpMeta){.value=11,.valid=1,.at_ms=17000};
 c.caps.valid=1;c.caps.epoch=1;c.caps.at_ms=17000;c.caps.present_mask=2;
 c.caps.entry[1]=(NpPdEntry){3600,20000,5000,NP_PD_APDO};
 c.charger_thermal.screen_valid=1;c.charger_thermal.screen_on=0;
 c.charger_thermal.charger_shift_milli_c=8000;
 b=board(41347,1,17000);assert(!np_control_charger_thermal_sample(&c,&b,17001));
 assert(c.charger_thermal_initial_ma==15600&&c.charger_thermal.current_ua==12400000);
 assert(c.charger_thermal_device_ma==12400&&c.charger_thermal_request_ma==12400);
 b=board(39000,2,18000);assert(!np_control_charger_thermal_sample(&c,&b,18001));
 assert(c.charger_thermal.stage==3);const int32_t previous_temp=c.charger_thermal.previous_milli_c[0];
 np_control_source_event(&c);
 assert(c.charger_thermal.initialized&&!c.charger_thermal.valid&&
  c.charger_thermal.previous_milli_c[0]==previous_temp&&c.charger_thermal.stage==3);
 assert(!c.charger_thermal_device_ma&&!c.charger_thermal_initial_ma&&!c.charger_thermal_request_ma);
 c.meta[NP_META_TYPE]=(NpMeta){.value=11,.valid=1,.at_ms=19000};
 c.caps.valid=1;c.caps.epoch=1;c.caps.at_ms=19000;c.caps.present_mask=2;
 c.caps.entry[1]=(NpPdEntry){3600,20000,5000,NP_PD_APDO};
 b=board(38000,1,19000);b.attachment=c.attachment;
 assert(!np_control_charger_thermal_sample(&c,&b,19001));
 assert(c.charger_thermal.stage==3&&c.charger_thermal.current_ua==15600000);
 np_control_facts(&c,19001,&f);assert(f.charger_thermal_valid&&f.charger_thermal_ma==15600);
 puts("PASS thermalControl072 original softwarevote0 baseline, one integration per cohort, current-source/age/invalidwithdrawal");return 0;
}
