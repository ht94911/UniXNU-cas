/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaBoardThermal.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static NpBoardThermal sample(int32_t temperature,uint64_t generation,uint64_t at)
{
 NpBoardThermal s={0};s.valid_mask=s.attempted_mask=NP_BOARD_INPUT_MASK;
 s.derived_valid=s.enabled=1;s.epoch=s.attachment=1;s.generation=generation;
 s.begin_ms=at-50;s.end_ms=at;s.charger_milli_c=temperature;
 for(unsigned i=0;i<NP_BOARD_INPUT_COUNT;++i)s.sampled_ms[i]=at-8+i;
 return s;
}
int main(int argc,char **argv)
{
 if(argc==7){
  NpChargerThermalCurrent state={0};state.initialized=1;state.epoch=state.attachment=1;
  state.generation=1;state.sampled_ms=9000;state.stage=atoi(argv[1]);
  state.previous_milli_c[0]=atoi(argv[3]);state.previous_milli_c[1]=atoi(argv[4]);
  NpBoardThermal s=sample(atoi(argv[2]),2,10000);
  int error=np_charger_thermal_current(&state,&s,10001,atoi(argv[5]));
  assert(!error&&state.stage==atoi(argv[6]));
  printf("%d %d\n",state.stage,state.current_ua);return 0;
 }
 NpChargerThermalCurrent state={0};NpBoardThermal s=sample(39500,1,10000);
 assert(!np_charger_thermal_current(&state,&s,10001,8000000));
 assert(state.valid&&state.stage==3&&state.current_ua==8000000);
 s=sample(41000,2,11000);assert(!np_charger_thermal_current(&state,&s,11001,8000000));
 assert(state.stage==4&&state.current_ua==5000000);
 s=sample(42001,3,12000);assert(!np_charger_thermal_current(&state,&s,12001,5000000));
 assert(state.current_ua==4809512); /* -2300 -188*1001 */
 assert(np_charger_thermal_current(&state,&s,12002,4809512)==EAGAIN&&!state.valid);
 NpChargerThermalCurrent before=state;
 s=sample(42000,4,13000);s.valid_mask=0xfe;
 assert(np_charger_thermal_current(&state,&s,13001,4809512)==ENODATA&&!state.valid);
 assert(state.generation==before.generation&&state.current_ua==before.current_ua);
 s=sample(42000,4,13000);s.errors[3]=EIO;
 assert(np_charger_thermal_current(&state,&s,13001,4809512)==ENODATA);
 s=sample(42000,4,13000);s.retained=1;
 assert(np_charger_thermal_current(&state,&s,13001,4809512)==ENODATA);
 s=sample(42000,4,13000);assert(np_charger_thermal_current(&state,&s,16001,4809512)==ENODATA);
 s=sample(42000,4,13000);s.sampled_ms[1]=13001;
 assert(np_charger_thermal_current(&state,&s,13001,4809512)==ENODATA);
 s=sample(42000,4,13000);assert(np_charger_thermal_current(&state,&s,13001,-1)==ENODATA);
 s=sample(42000,4,13000);assert(np_charger_thermal_current(&state,&s,13001,22000001)==ENODATA);
 s=sample(42000,4,13000);s.derived_valid=0;
 assert(np_charger_thermal_current(&state,&s,13001,4000000)==ENODATA);
 s=sample(39000,4,13000);s.attachment=2;
 assert(!np_charger_thermal_current(&state,&s,13001,4300000));
 assert(state.attachment==2&&state.stage==3&&state.current_ua==5450000);
 s=sample(128001,5,14000);s.attachment=2;
 assert(np_charger_thermal_current(&state,&s,14001,5480000)==ERANGE&&!state.valid);
 s=sample(13000,6,15000);s.attachment=2;
 assert(!np_charger_thermal_current(&state,&s,15001,500000));
 assert(state.stage==-1&&state.current_ua==22000000);
 puts("PASS thermal current: original recurrence, complete fresh cohort, duplicate suppression and attachment history reset");
 return 0;
}
