/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Independent evaluation of the actual OS4 CN/main BM6N JEITA table. The
 * original GPL MCA sources remain research references, not copied drivers. */
#include "NezhaJeita.h"
#include <string.h>

typedef struct { int32_t mv, ma; } Curve;
typedef struct {
    int32_t low, high, low_hys, high_hys, ma, mv, iterm;
    const Curve *curve;
    unsigned count;
} Row;
static const Curve cold0[]={{4000,262},{4490,131}};
static const Curve cold1[]={{4000,524},{4490,328}};
static const Curve cold2[]={{4000,787},{4200,656},{4500,524}};
static const Curve mild0[]={{3900,1968},{4500,1312}};
static const Curve mild1[]={{3900,3936},{4500,2624}};
static const Curve mild2[]={{4150,4000},{4500,2624}};
static const Row normal[]={
    {-65535,-10,0,2,0,4490,66,0,0},
    {-10,-2,0,2,262,4490,66,cold0,2},
    {-2,3,0,2,524,4490,131,cold1,2},
    {3,8,0,2,787,4500,328,cold2,3},
    {8,13,0,1,1968,4500,328,mild0,2},
    {13,15,0,1,3936,4500,328,mild1,2},
    {15,18,0,2,4000,4500,328,mild2,2},
    {18,35,0,0,4000,4480,328,0,0},
    {35,40,1,0,4000,4470,328,0,0},
    {40,48,1,0,4000,4460,328,0,0},
    {48,55,2,0,3280,4060,328,0,0},
    {55,65535,2,2,0,4060,328,0,0}
};
void np_jeita_init(NpJeita *j)
{ if(j){memset(j,0,sizeof(*j));j->row=-1;} }
static int32_t curve_current(const Row *r, int32_t mv, int *index)
{
    int32_t least=r->ma;*index=-1;
    for(unsigned i=0;i<r->count;++i){
        if(r->curve[i].ma<least)least=r->curve[i].ma;
        if(mv<=r->curve[i].mv){*index=(int)i;return r->curve[i].ma;}
    }
    return least;
}
int np_jeita_update(NpJeita *j, const NpSnapshot *s, const NpGaugeIdentity *id, int country_cn)
{
    if(!j)return -1;
    memset(&j->vote,0,sizeof(j->vote));
    const uint32_t need=NP_BIT(NP_PRESENT)|NP_BIT(NP_TEMPERATURE)|NP_BIT(NP_VOLTAGE);
    if(!s||!id||country_cn!=1||id->valid!=3||id->error||id->cell_code!='F'||
       memcmp(id->device_name,"BM6N",5)||(s->valid&need)!=need||!s->gauge.present)return -1;
    /* The stock FG uses raw kelvin -2730 in deci-C. Its JEITA caller uses
     * -(abs(t)/10)-1 for negative values, including exact multiples. Keep
     * that table coordinate without changing the accurate centi-C publisher. */
    int32_t deci=(int32_t)s->gauge.raw[2]-2730;
    int32_t temp=deci<0?-((-deci)/10)-1:deci/10;
    int candidate=-1;
    for(unsigned i=0;i<sizeof(normal)/sizeof(normal[0]);++i)
        if(temp>=normal[i].low&&temp<normal[i].high){candidate=(int)i;break;}
    if(candidate<0)return -1;
    int selected=candidate,held=0;
    if(j->initialized&&candidate!=j->row){
        const Row *old=&normal[j->row];
        if((candidate>j->row&&temp<old->high+old->high_hys)||
           (candidate<j->row&&temp>old->low-old->low_hys)){selected=j->row;held=1;}
    }
    const Row *row=&normal[selected];
    int point;int32_t current=curve_current(row,s->gauge.voltage_mv,&point);
    int voltage_held=0;
    if(!j->initialized||selected!=j->row)j->current_ma=current;
    else if(current!=j->current_ma){
        const int32_t mv=s->gauge.voltage_mv;
        if((current>j->current_ma&&point>=0&&mv<row->curve[point].mv-50)||
           (current<j->current_ma&&point>0&&mv>row->curve[point-1].mv))j->current_ma=current;
        else voltage_held=1;
    }
    j->row=selected;j->initialized=1;
    if(row->mv>4200)j->hot_stopped=0;
    else if(s->gauge.voltage_mv>=row->mv)j->hot_stopped=1;
    else if(s->gauge.voltage_mv<row->mv-150)j->hot_stopped=0;
    j->vote=(NpJeitaVote){.temperature_c=temp,.fcc_ma=j->current_ma,.fv_mv=row->mv,
        .iterm_ma=row->iterm,.row=selected,.valid=1,
        .enabled=(uint8_t)(j->current_ma>0&&!j->hot_stopped),
        .temperature_held=(uint8_t)held,.voltage_held=(uint8_t)voltage_held};
    return 0;
}
int np_jeita_full_update(NpJeita *j, const NpSnapshot *s)
{
    if(!j)return -1;
    j->vote.full_valid=0;j->vote.full=j->vote.recharging=0;
    const uint32_t need=NP_BIT(NP_PRESENT)|NP_BIT(NP_REMAINING)|
        NP_BIT(NP_FULL_CAPACITY)|NP_BIT(NP_FULL_FLAG)|NP_BIT(NP_EXTERNAL)|NP_BIT(NP_CHARGE_DONE);
    if(!s||(s->valid&need)!=need||!s->gauge.present||s->gauge.remaining_mah<0||
       s->gauge.full_mah<=0)return -1;
    int64_t ratio=(int64_t)s->gauge.remaining_mah*10000/s->gauge.full_mah;
    if(ratio<0||ratio>INT32_MAX)return -1;
    j->vote.raw_soc=(int32_t)ratio;
    const int observed_full=s->gauge.full_flag||s->fully_charged;
    if(!s->external){j->full_latched=j->recharging=j->full_cleared=0;}
    else if(j->full_latched&&ratio<=9700){
        /* The original CN strategy explicitly starts recharge at <=97%.
         * Its existing full flag may clear only after charging resumes. */
        j->full_latched=0;j->recharging=1;j->full_cleared=!observed_full;
    }else if(j->recharging){
        if(!observed_full)j->full_cleared=1;
        else if(j->full_cleared){j->full_latched=1;j->recharging=0;}
    }else if(observed_full)j->full_latched=1;
    j->vote.full_valid=1;j->vote.full=j->full_latched;j->vote.recharging=j->recharging;
    if(j->vote.full)j->vote.enabled=0;
    return 0;
}
