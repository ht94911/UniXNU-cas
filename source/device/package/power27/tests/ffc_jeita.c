/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaJeita.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static NpGaugeIdentity id;
static NpSnapshot sample(int temp,int mv)
{
 NpSnapshot s={0};s.valid=NP_BIT(NP_PRESENT)|NP_BIT(NP_TEMPERATURE)|NP_BIT(NP_VOLTAGE);
 s.gauge.present=1;s.gauge.raw[2]=(uint16_t)(2730+10*temp);s.gauge.voltage_mv=mv;return s;
}
static void check(NpJeita *j,int temp,int row,int ma,int mv,int iterm,int enabled)
{
 NpSnapshot s=sample(temp,4300);assert(!np_jeita_update_profile(j,&s,&id,1,1));
 assert(j->profile==1&&j->vote.valid&&j->vote.row==row&&j->vote.fcc_ma==ma&&
  j->vote.fv_mv==mv&&j->vote.iterm_ma==iterm&&j->vote.enabled==enabled);
}
int main(void)
{
 id.valid=3;id.cell_code='F';memcpy(id.device_name,"BM6N",5);
 const int temps[]={18,23,25,35,40,48,55};
 const int iterms[]={722,787,918,1509,2230,328,328};
 for(unsigned i=0;i<7;++i){NpJeita j;np_jeita_init(&j);
  check(&j,temps[i],(int)i,i<5?4000:i==5?3280:0,i<5?4530:4060,iterms[i],i<5);}
 NpJeita j;np_jeita_init(&j);check(&j,22,0,4000,4530,722,1);
 check(&j,23,0,4000,4530,722,1);assert(j.vote.temperature_held);
 check(&j,24,1,4000,4530,787,1);check(&j,25,1,4000,4530,787,1);
 check(&j,26,2,4000,4530,918,1);check(&j,35,3,4000,4530,1509,1);
 check(&j,34,2,4000,4530,918,1);
 NpSnapshot s=sample(40,4300);assert(!np_jeita_update(&j,&s,&id,1));
 assert(!j.profile&&j.vote.row==9&&j.vote.fcc_ma==4000&&j.vote.fv_mv==4460);
 assert(!np_jeita_update_profile(&j,&s,&id,1,1)&&j.vote.fv_mv==4530);
 s=sample(17,4300);assert(np_jeita_update_profile(&j,&s,&id,1,1)==-1&&!j.vote.valid);
 s=sample(30,4300);s.valid=0;assert(np_jeita_update_profile(&j,&s,&id,1,1)==-1&&!j.vote.valid);
 puts("PASS N0 FFC JEITA five charging zones, hot stop, hysteresis, profile reset and normal FV/current retention");
 return 0;
}
