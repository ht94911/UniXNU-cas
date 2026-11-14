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
