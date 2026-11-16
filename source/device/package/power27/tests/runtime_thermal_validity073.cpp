// SPDX-License-Identifier: BSD-2-Clause-Patent
#define main prior_runtime_cases_not_executed
#include "runtime_control.cpp"
#undef main
int main(void)
{
 snapshotLock=IOLockAlloc();controlLock=IOLockAlloc();bound=1;io.now_ms=now;
 np_control_init(&control,9);np_control_link(&control,1,1);
 np_control_charger_thermal_enable(&control,1);lifecycle=1;test_ms=10000;
 NpBoardThermal b={};b.enabled=b.derived_valid=1;b.valid_mask=b.attempted_mask=255;
 b.epoch=b.attachment=1;b.generation=1;b.begin_ms=9950;b.end_ms=10000;b.charger_milli_c=39500;
 for(unsigned i=0;i<8;++i)b.sampled_ms[i]=9992+i;
 assert(!np_control_charger_thermal_sample(&control,&b,10000));
 NpPowerControl published={};assert(np_power_control_copy(&published)&&published.charger_thermal.valid);
 test_ms=12951;assert(np_power_control_copy(&published)&&!published.charger_thermal.valid);
 assert(control.charger_thermal.valid&&control.charger_thermal.current_ua==4300000);
 test_ms=10001;lifecycle=3;assert(np_power_control_copy(&published)&&!published.charger_thermal.valid);
 lifecycle=1;np_control_link(&control,1,0);assert(np_power_control_copy(&published)&&!published.charger_thermal.valid);
 puts("PASS actual thermal publication: expiry/PM/link withdraw validity while preserving prior completed softwarevote");return 0;
}
