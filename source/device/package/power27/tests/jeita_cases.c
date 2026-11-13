/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaJeita.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static NpGaugeIdentity identity;
static NpSnapshot sample;
static void input(int raw,int mv)
{
    memset(&sample,0,sizeof(sample));
    sample.valid=NP_BIT(NP_PRESENT)|NP_BIT(NP_TEMPERATURE)|NP_BIT(NP_VOLTAGE);
    sample.gauge.present=1;sample.gauge.raw[2]=(uint16_t)raw;
    sample.gauge.voltage_mv=mv;sample.gauge.temperature_centi_c=raw*10-27315;
}
static void check(NpJeita *j,int raw,int mv,int row,int ma,int fv,int enabled)
{
    input(raw,mv);assert(!np_jeita_update(j,&sample,&identity,1));
    if(!(j->vote.valid&&j->vote.row==row&&j->vote.fcc_ma==ma&&j->vote.fv_mv==fv&&j->vote.enabled==enabled))
        fprintf(stderr,"raw=%d mv=%d actual=row%d ma%d fv%d en%d expected=row%d ma%d fv%d en%d\n",
            raw,mv,j->vote.row,j->vote.fcc_ma,j->vote.fv_mv,j->vote.enabled,row,ma,fv,enabled);
    assert(j->vote.valid&&j->vote.row==row&&j->vote.fcc_ma==ma&&j->vote.fv_mv==fv&&j->vote.enabled==enabled);
}
int main(void)
{
    identity.valid=3;identity.cell_code='F';memcpy(identity.device_name,"BM6N",5);
    NpJeita j;
    np_jeita_init(&j);check(&j,2910,4300,7,4000,4480,1);
    assert(sample.gauge.temperature_centi_c==1785&&j.vote.temperature_c==18&&j.vote.iterm_ma==328);
    check(&j,3080,4300,8,4000,4470,1);
    check(&j,3070,4300,7,4000,4480,1);
    check(&j,3130,4300,9,4000,4460,1);
    check(&j,3120,4300,8,4000,4470,1);
    np_jeita_init(&j);check(&j,2630,4300,0,0,4490,0);
    check(&j,2631,4300,0,0,4490,0);assert(j.vote.temperature_held);
    check(&j,2660,4300,1,131,4490,1);
    np_jeita_init(&j);
    check(&j,2631,4300,1,131,4490,1);
    np_jeita_init(&j);check(&j,2640,3990,1,262,4490,1);
    check(&j,2640,4000,1,262,4490,1);
    check(&j,2640,4001,1,131,4490,1);
    check(&j,2640,3999,1,131,4490,1);assert(j.vote.voltage_held);
    check(&j,2640,3950,1,131,4490,1);
    check(&j,2640,3949,1,262,4490,1);
    np_jeita_init(&j);check(&j,2780,4000,3,787,4500,1);
    check(&j,2780,4001,3,656,4500,1);
    check(&j,2780,4200,3,656,4500,1);
    check(&j,2780,4201,3,524,4500,1);
    np_jeita_init(&j);check(&j,3210,4300,10,3280,4060,0);
    check(&j,3200,3900,10,3280,4060,1);assert(j.vote.temperature_held);
    check(&j,3190,4300,9,4000,4460,1);
    check(&j,3280,4300,11,0,4060,0);
    check(&j,3270,3800,11,0,4060,0);
    check(&j,3260,3800,10,3280,4060,1);
    np_jeita_init(&j);check(&j,2910,4300,7,4000,4480,1);
    sample.valid&=~NP_BIT(NP_TEMPERATURE);
    assert(np_jeita_update(&j,&sample,&identity,1)==-1&&!j.vote.valid&&!j.vote.enabled);
    check(&j,2910,4300,7,4000,4480,1);
    identity.cell_code='L';assert(np_jeita_update(&j,&sample,&identity,1)==-1&&!j.vote.valid);
    identity.cell_code='F';assert(np_jeita_update(&j,&sample,&identity,0)==-1&&!j.vote.valid);
    memcpy(identity.device_name,"2@BP",5);assert(np_jeita_update(&j,&sample,&identity,1)==-1&&!j.vote.valid);
    memcpy(identity.device_name,"BM6N",5);np_jeita_init(&j);input(3080,4300);
    sample.valid|=NP_BIT(NP_REMAINING)|NP_BIT(NP_FULL_CAPACITY)|NP_BIT(NP_FULL_FLAG)|
        NP_BIT(NP_EXTERNAL)|NP_BIT(NP_CHARGE_DONE);
    sample.external=1;sample.gauge.remaining_mah=4900;sample.gauge.full_mah=5000;
    sample.gauge.soc=95;
    assert(!np_jeita_update(&j,&sample,&identity,1)&&!np_jeita_full_update(&j,&sample));
    assert(j.vote.full_valid&&j.vote.raw_soc==9800&&!j.vote.full&&j.vote.enabled);
    sample.gauge.full_flag=1;
    assert(!np_jeita_update(&j,&sample,&identity,1)&&!np_jeita_full_update(&j,&sample));
    assert(j.vote.full&&!j.vote.enabled);
    sample.gauge.remaining_mah=4851;
    assert(!np_jeita_update(&j,&sample,&identity,1)&&!np_jeita_full_update(&j,&sample));
    assert(j.vote.raw_soc==9702&&j.vote.full&&!j.vote.recharging);
    sample.gauge.remaining_mah=4850;
    assert(!np_jeita_update(&j,&sample,&identity,1)&&!np_jeita_full_update(&j,&sample));
    assert(j.vote.raw_soc==9700&&!j.vote.full&&j.vote.recharging&&j.vote.enabled);
    sample.gauge.remaining_mah=4860;
    assert(!np_jeita_update(&j,&sample,&identity,1)&&!np_jeita_full_update(&j,&sample));
    assert(j.vote.recharging&&!j.vote.full); // Do not re-stop on the sticky old full flag.
    sample.gauge.full_flag=0;
    assert(!np_jeita_update(&j,&sample,&identity,1)&&!np_jeita_full_update(&j,&sample));
    sample.gauge.remaining_mah=5000;sample.gauge.full_flag=1;
    assert(!np_jeita_update(&j,&sample,&identity,1)&&!np_jeita_full_update(&j,&sample));
    assert(j.vote.full&&!j.vote.recharging&&!j.vote.enabled);
    sample.gauge.full_mah=0;
    assert(np_jeita_full_update(&j,&sample)==-1&&!j.vote.full_valid);
    sample.gauge.full_mah=5000;sample.external=0;
    assert(!np_jeita_full_update(&j,&sample)&&!j.full_latched&&!j.recharging);
    puts("PASS actual CN/main JEITA values, raw-temperature coordinate, voltage/temperature hysteresis, hot stop/recovery and identity/invalid sample refusal; host-only");
}
