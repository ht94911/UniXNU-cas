/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaCccv.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv){assert(argc==4);int temp=atoi(argv[1]),mv=atoi(argv[2]),ma=atoi(argv[3]);NpSnapshot s={0};s.valid=NP_BIT(NP_PRESENT)|NP_BIT(NP_VOLTAGE)|NP_BIT(NP_CURRENT)|NP_BIT(NP_TEMPERATURE)|NP_BIT(NP_CYCLES);s.gauge.present=1;s.gauge.voltage_mv=mv;s.gauge.current_ma=ma;s.gauge.raw[2]=(uint16_t)(2730+temp*10);s.gauge.end_ms=10000;s.gauge.cycles=331;NpGaugeIdentity id={0};id.valid=3;id.cell_code='F';memcpy(id.device_name,"BM6N",5);NpCccv c={0};int r=np_cccv_update_profile(&c,&s,&id,1,1,1,1);if(r){puts("invalid");return 0;}printf("%d %d %d %d %u\n",c.stage,c.voltage_mv,c.current_max_ma,c.current_min_ma,c.rows);return 0;}
