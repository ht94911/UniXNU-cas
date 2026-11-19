/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* N0 OS4 decoded NORMAL Virtual coefficients and actual mi_thermald reader
 * a8dcc/formula_max_temp bce9c. No fitted weights or unconditional cache alias. */
#include "NezhaBoardThermal.h"
#include <sys/errno.h>
#include <string.h>
#include <limits.h>

int np_bq_original_temperature_milli_c(uint16_t raw,int32_t *out)
{
    if(!out)return EINVAL;
    *out=0;
    const int32_t tenths_c=(int32_t)raw-2730;
    /* Actual bq27z561 fg_read_original_temperature range, also preserved in
     * the public implementation. It caches out-of-range samples; we reject. */
    if(tenths_c < -1280 || tenths_c > 1280)return ERANGE;
    *out=tenths_c*100;
    return 0;
}

/* Avoid C signed-overflow/implementation-defined casts while keeping the
 * actual AArch64 W-register wrap before its signed division and comparison. */
static int32_t signed_word(uint32_t value)
{
    return value <= INT32_MAX ? (int32_t)value :
        (int32_t)((int64_t)value-INT64_C(4294967296));
}

static int32_t virtual_temperature(const int32_t inputs[NP_BOARD_INPUT_COUNT],
    const int32_t weights[NP_BOARD_INPUT_COUNT],int32_t compensation,int32_t global)
{
    uint32_t accumulated=0;
    for(unsigned i=0;i<NP_BOARD_INPUT_COUNT;++i)
        accumulated+=(uint32_t)weights[i]*(uint32_t)inputs[i];
    /* The decoded weight_sum is1000 even where the sum of weights differs. */
    const int32_t divided=signed_word(accumulated)/1000;
    return signed_word((uint32_t)divided+(uint32_t)compensation+(uint32_t)global);
}

int np_board_thermal_calculate(NpBoardThermal *out,int32_t global)
{
    if(!out)return EINVAL;
    out->derived_valid=0;
    memset(out->direction_milli_c,0,sizeof(out->direction_milli_c));
    out->formula_milli_c=out->charger_milli_c=0;
    out->global_comp_milli_c=global;
    if(out->valid_mask!=NP_BOARD_INPUT_MASK)return ENODATA;
    for(unsigned i=0;i<NP_BOARD_INPUT_COUNT;++i)
        if(out->errors[i])return out->errors[i] < 0 ? -out->errors[i] : out->errors[i];
    static const int32_t weights[NP_BOARD_DIRECTION_COUNT+1][NP_BOARD_INPUT_COUNT]={
        {271,-11,305,142,409,-157,1,1},
        {-84,292,361,70,481,-134,1,1},
        {363,-148,315,157,546,-245,1,1},
        {235,-118,387,129,360,16,1,1},
        {106,-286,348,191,1060,-469,1,1},
        {58,-385,689,200,-1097,500,500,500}
    };
    static const int32_t compensation[NP_BOARD_DIRECTION_COUNT+1]={1181,-860,448,-745,1611,1307};
    int32_t maximum=INT32_MIN;
    for(unsigned i=0;i<NP_BOARD_DIRECTION_COUNT;++i){
        const int32_t value=virtual_temperature(out->input_milli_c,weights[i],compensation[i],global);
        out->direction_milli_c[i]=value;
        if(value>maximum)maximum=value;
    }
    out->formula_milli_c=maximum;
    out->charger_milli_c=virtual_temperature(out->input_milli_c,weights[NP_BOARD_DIRECTION_COUNT],
        compensation[NP_BOARD_DIRECTION_COUNT],global);
    out->derived_valid=1;
    return 0;
}

int np_charger_thermal_limits(unsigned screen_valid,unsigned screen_on,int32_t shift,
                             int32_t low[8],int32_t high[8])
{
    static const int32_t minimum[]={15600,12400,5800,4300,3500,2000,1000,500};
    static const int32_t maximum[]={15600,12400,10000,8000,5000,3000,1000,500};
    static const int32_t target[]={0,32500,37500,39500,42000,44000,44500,45000};
    if(!low||!high||screen_valid>1||screen_on>1||shift<0||shift>8000)return EINVAL;
    memcpy(low,minimum,sizeof(minimum));memcpy(high,maximum,sizeof(maximum));
    if(!screen_valid||screen_on||!shift)return 0;
    /* The first nonzero target is32500 and its original max is12400.
     * b0870..b0900 copies both prior-stage limits below target+offset;
     * at the crossing stage it copies only that first target's max. */
    const int32_t boundary=target[1]+shift;
    for(unsigned i=1;i<8;++i){
        if(target[i]<boundary){low[i]=low[i-1];high[i]=high[i-1];}
        else if(target[i-1]&&target[i-1]<boundary)high[i]=maximum[1];
    }
    return 0;
}

int np_charger_thermal_current(NpChargerThermalCurrent *state,const NpBoardThermal *sample,
                              uint64_t now,int32_t previous_current_ua)
{
    if(!state)return EINVAL;
    state->valid=0;
    if(!sample||!sample->derived_valid||sample->valid_mask!=NP_BOARD_INPUT_MASK||
       !sample->generation||!sample->epoch||!sample->attachment||sample->retained||
       sample->end_ms<sample->begin_ms||sample->end_ms-sample->begin_ms>2000||
       now<sample->end_ms||now-sample->begin_ms>3000||
       previous_current_ua<0||previous_current_ua>22000000)return ENODATA;
    for(unsigned i=0;i<NP_BOARD_INPUT_COUNT;++i)
        if(sample->errors[i]||sample->sampled_ms[i]<sample->begin_ms||
           sample->sampled_ms[i]>sample->end_ms)return ENODATA;
    if(state->epoch!=sample->epoch||state->attachment!=sample->attachment)
        state->initialized=0;
    if(state->initialized&&(sample->generation<=state->generation||
       sample->end_ms<=state->sampled_ms))return EAGAIN;
    static const int32_t trigger[]={15000,31000,34000,39000,40500,44000,44500,45000};
    static const int32_t clear[]={14000,30000,31000,37700,39000,43500,44000,44500};
    static const int32_t target[]={0,32500,37500,39500,42000,44000,44500,45000};
    static const int32_t ks[]={0,2300000,2300000,2300000,2300000,2300000,2300000,2300000};
    static const int32_t ki[]={0,188000,188000,188000,188000,188000,188000,188000};
    int32_t minimum[8],maximum[8];
    int error=np_charger_thermal_limits(state->screen_valid,state->screen_on,
        state->charger_shift_milli_c,minimum,maximum);
    if(error)return error;
    const int32_t temperature=sample->charger_milli_c;
    if(temperature < -40000 || temperature > 128000)return ERANGE;
    int stage=state->initialized?state->stage:-1;
    while(stage+1<8&&temperature>=trigger[stage+1])++stage;
    while(stage>=0&&temperature<=clear[stage])--stage;
    /* The actual thermal_fcc_override device supplies def_target22000000
     * when every trigger is clear; it does not retain a hot-stage limit. */
    int32_t current=22000000;
    if(stage>=0) {
        /* Actual b1274..b12f0 uses W-register differences and signed
         * 64-bit coefficient accumulation, then truncates division by1000. */
        const int32_t prior1=state->initialized?state->previous_milli_c[0]:temperature;
        const int32_t e0=target[stage]-temperature,e1=target[stage]-prior1;
        /* Every actual N0 kc is0. Keep its second temperature history for
         * the verified recurrence without adding a configurable controller. */
        /* Actual b11f4..b1278 loads parsed ks into X5 and ki into X6.
         * b1294 then multiplies X6 by e0, and b12a4 applies X5 to
         * e0-e1. The earlier arithmetic-only oracle supplied them reversed. */
        const int64_t delta=((int64_t)ki[stage]*e0+(int64_t)ks[stage]*(e0-e1))/1000;
        int32_t candidate=signed_word((uint32_t)previous_current_ua+(uint32_t)delta);
        const int32_t low=minimum[stage]*1000,high=maximum[stage]*1000;
        if(candidate<low)candidate=low;
        if(candidate>high)candidate=high;
        current=candidate;
    }
    state->previous_milli_c[1]=state->initialized?state->previous_milli_c[0]:temperature;
    state->previous_milli_c[0]=temperature;
    state->generation=sample->generation;state->epoch=sample->epoch;
    state->attachment=sample->attachment;state->sampled_ms=sample->end_ms;
    state->current_ua=current;state->stage=stage;state->initialized=state->valid=1;
    return 0;
}
