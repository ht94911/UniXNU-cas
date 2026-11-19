/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaBoardThermal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc,char **argv)
{
 if(argc==4){
  int32_t low[8],high[8];int error=np_charger_thermal_limits((unsigned)atoi(argv[1]),
    (unsigned)atoi(argv[2]),atoi(argv[3]),low,high);
  printf("%d",error);
  if(!error)for(unsigned i=0;i<8;++i)printf(" %d %d",low[i],high[i]);
  putchar('\n');return 0;
 }
 assert(argc==1);
 NpBoardThermal s={0};s.valid_mask=s.attempted_mask=255;s.enabled=s.derived_valid=1;
 s.epoch=s.attachment=1;s.generation=1;s.begin_ms=9950;s.end_ms=10000;s.charger_milli_c=41347;
 for(unsigned i=0;i<8;++i)s.sampled_ms[i]=9992+i;
 NpChargerThermalCurrent t={0};t.screen_valid=1;t.screen_on=0;t.charger_shift_milli_c=8000;
 assert(!np_charger_thermal_current(&t,&s,10001,3500000));
 assert(t.stage==4&&t.current_ua==5001900); /* Original target42000 recurrence, changed max12400. */
 s.generation=2;s.begin_ms+=1000;s.end_ms+=1000;
 for(unsigned i=0;i<8;++i)s.sampled_ms[i]+=1000;
 s.charger_milli_c=41088;
 assert(!np_charger_thermal_current(&t,&s,11001,12000000));
 assert(t.stage==4&&t.current_ua==12400000);
 t.screen_on=1;s.generation=3;s.begin_ms+=1000;s.end_ms+=1000;
 for(unsigned i=0;i<8;++i)s.sampled_ms[i]+=1000;
 assert(!np_charger_thermal_current(&t,&s,12001,12400000));
 assert(t.current_ua==5000000); /* Actual bright transition restores the unshifted bound. */
 t.screen_valid=0;t.screen_on=0;s.generation=4;s.begin_ms+=1000;s.end_ms+=1000;
 for(unsigned i=0;i<8;++i)s.sampled_ms[i]+=1000;
 assert(!np_charger_thermal_current(&t,&s,13001,12000000)&&t.current_ua==5000000);
 t.screen_valid=1;t.screen_on=0;s.charger_milli_c=44501;
 s.generation=5;s.begin_ms+=1000;s.end_ms+=1000;
 for(unsigned i=0;i<8;++i)s.sampled_ms[i]+=1000;
 assert(!np_charger_thermal_current(&t,&s,14001,12000000)&&t.current_ua==1000000);
 s.valid_mask=254;assert(np_charger_thermal_current(&t,&s,14002,1000000)==ENODATA&&!t.valid);
 puts("PASS098 original scene bounds/current recurrence, bright/unknown restore, hot and missing-input gates");
 return 0;
}
