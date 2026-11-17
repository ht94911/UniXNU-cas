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
 np_control_link(&c,1,0);np_control_facts(&c,14001,&f);assert(!f.charger_thermal_valid);
 np_control_init(&c,8);np_control_charger_thermal_enable(&c,1);assert(!c.charger_thermal_enabled);
 puts("PASS thermalControl072 original softwarevote0 baseline, one integration per cohort, current-source/age/invalidwithdrawal");return 0;
}
