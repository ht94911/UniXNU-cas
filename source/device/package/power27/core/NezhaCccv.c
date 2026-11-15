/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Independent arithmetic from the saved N0 FDT and matching OS4 module.
 * The original module stays local; no Linux object is linked into XNU. */
#include "NezhaCccv.h"
#include <string.h>
typedef struct {int32_t mv,cc,cv;} Curve;
typedef struct {int32_t low,high,low_hys;const Curve *curve;unsigned rows;} Zone;
static const Curve cool[]={{3900,3936,2624},{4494,2624,800}};
static const Curve mild[]={{4000,7216,5248},{4150,5248,2624},{4494,2624,800}};
static const Curve normal[]={{3900,9840,2624},{4474,2624,800}};
static const Curve warm0[]={{4310,15600,5248},{4474,5248,800}};
static const Curve warm1[]={{4310,15600,7872},{4474,7872,800}};
static const Curve warm2[]={{4310,15600,7872},{4464,7872,800}};
static const Curve warm3[]={{4310,15600,7872},{4454,7872,800}};
static const Zone zones[]={
 {13,15,0,cool,2},{15,18,0,mild,3},{18,23,0,normal,2},
 {23,25,1,warm0,2},{25,35,0,warm1,2},{35,40,1,warm2,2},{40,48,1,warm3,2}
};
static const Curve ffc_normal[]={{3900,9840,7216},{4000,7216,5248},{4150,5248,2624},{4524,2624,722}};
static const Curve ffc_warm0[]={{4310,15600,12400},{4350,12400,9840},{4400,9840,7872},{4500,7872,5248},{4524,5248,787}};
static const Curve ffc_warm1[]={{4310,15600,12400},{4350,12400,11808},{4400,11808,9840},{4500,9840,7872},{4524,7872,918}};
static const Curve ffc_warm2[]={{4310,15600,12400},{4350,12400,11808},{4400,11808,9840},{4500,9840,7872},{4524,7872,1509}};
static const Curve ffc_warm3[]={{4310,15600,12400},{4350,12400,11808},{4400,11808,9840},{4500,9840,7872},{4524,7872,2230}};
static const Curve *const fast_curves[]={0,0,ffc_normal,ffc_warm0,ffc_warm1,ffc_warm2,ffc_warm3};
int np_cccv_update_profile(NpCccv *c,const NpSnapshot *s,const NpGaugeIdentity *id,int cn,
                   uint64_t epoch,uint64_t attachment,unsigned fast_mode)
{
 if(!c||fast_mode>1)return -1;
 c->valid=0;c->cycles_valid=0;
 const uint32_t need=NP_BIT(NP_PRESENT)|NP_BIT(NP_TEMPERATURE)|NP_BIT(NP_VOLTAGE)|NP_BIT(NP_CURRENT);
 if(!s||!id||cn!=1||id->valid!=3||id->error||id->cell_code!='F'||
    memcmp(id->device_name,"BM6N",5)||!epoch||!attachment||
    (s->valid&need)!=need||!s->gauge.present||s->gauge.current_ma<0)return -1;
 if(c->epoch!=epoch||c->attachment!=attachment||c->profile!=fast_mode)c->initialized=0;
 int32_t deci=(int32_t)s->gauge.raw[2]-2730;
 const int32_t temp=deci/10;int selected=-1;
 for(unsigned i=0;i<sizeof(zones)/sizeof(zones[0]);++i)
  if(temp>=zones[i].low&&temp<zones[i].high){selected=(int)i;break;}
 if(selected<0)return -1;
 /* The stock quick-charge temperature selector delays a warmer zone by
  * that zone's low hysteresis; cooling selects the actual cooler curve. */
 if(c->initialized&&selected>c->zone&&temp<zones[selected].low+zones[selected].low_hys)
  selected=c->zone;
 Zone chosen=zones[selected];
 if(fast_mode){
  if(!fast_curves[selected])return -1;
  chosen.curve=fast_curves[selected];chosen.rows=selected==2?4:5;
 }
 const Zone *z=&chosen;int stage=0;
 for(unsigned row=z->rows;row>0;--row){
  const Curve *p=&z->curve[row-1];
  if(s->gauge.voltage_mv>=p->mv){
   stage=(int)(2*(row-1)+1+((fast_mode?s->gauge.current_ma<p->cv:s->gauge.current_ma<=p->cv)));break;
  }
 }
 /* All five N0 ATL ffc_stage arrays skip the first CV stage. */
 if(fast_mode&&stage==1)stage=2;
 if(stage>=(int)(2*z->rows))stage=(int)(2*z->rows-1);
 if(c->initialized&&selected==c->zone&&stage<c->stage)stage=c->stage;
 const Curve *p=&z->curve[(unsigned)stage/2];
 c->epoch=epoch;c->attachment=attachment;c->sampled_ms=s->gauge.end_ms;
 c->temperature_c=temp;c->zone=selected;c->stage=stage;c->rows=z->rows;
 c->voltage_mv=p->mv;c->current_max_ma=p->cc;c->current_min_ma=p->cv;
 c->cycles=s->gauge.cycles;
 c->cycles_valid=(s->valid&NP_BIT(NP_CYCLES))&&c->cycles>=0&&c->cycles<=65535;
 c->last_voltage_mv=z->curve[z->rows-1].mv;
 c->profile=(uint8_t)fast_mode;c->initialized=1;c->valid=1;return 0;
}
int np_cccv_update(NpCccv *c,const NpSnapshot *s,const NpGaugeIdentity *id,int cn,
                   uint64_t epoch,uint64_t attachment)
{return np_cccv_update_profile(c,s,id,cn,epoch,attachment,0);}
int32_t np_cccv_delta_ratio(const NpCccv *c,const NpCccvInput *i,unsigned ratio)
{
 if(!c||!i||!c->valid||(ratio!=2&&ratio!=4)||i->adapter_ma<100||i->adapter_ma>10000||
    i->limit_ma<=200||i->limit_ma>15600||i->limit_mv<3500||i->limit_mv>4600||
    i->battery_mv<3000||i->battery_mv>4600||i->battery_ma<0||i->battery_ma>20000||
    i->bus_ma<0||i->bus_ma>10000)return 0;
 if(ratio==4&&!c->cycles_valid)return 0;
 const int32_t cycles=c->cycles_valid?c->cycles:0;
 int32_t max=c->current_max_ma;
 if(max>i->limit_ma)max=i->limit_ma;
 if(max>(int32_t)i->adapter_ma*(int32_t)ratio)max=(int32_t)i->adapter_ma*(int32_t)ratio;
 int32_t threshold=c->voltage_mv;
 if(threshold>i->limit_mv)threshold=i->limit_mv;
 const int32_t gap=max-i->battery_ma;
 /* Match original vstep order: ratio4 rows require cycles<=300 or<=800.
  * The original function uses20mV from800 cycles, and recovers its20mV
  * curve drop in the final-row margin check from300. FinalCV rises20mV;
  * intermediateCV halves an upward table multiplier before multiplying20. */
 int32_t step;
 if(ratio==2)step=gap>=2000?80:gap<=0?(gap<=-2000?80:40):20;
 else if(gap<=0)step=gap<=-8000?400:gap<=-4000?200:gap<=-1000?100:40;
 else if(cycles<=300)step=gap>=8000?600:gap>=3000?500:gap>=1500?260:gap>=1000?200:gap>=500?100:20;
 else step=gap>=8000?200:gap>=6000?100:gap>=4000?40:20;
 if(gap>0&&cycles>=800)step=20;
 if(gap>0&&c->stage==(int32_t)(2*c->rows-1))step=20;
 else if((c->stage&1)&&gap>0&&step>20)step=step/40*20;
 int32_t last=c->last_voltage_mv?c->last_voltage_mv:c->voltage_mv;
 if(cycles>=300)last+=20;
 if(gap>0&&i->battery_mv>=last-100)step=20;
 const int32_t compensation=i->real_type==12&&c->stage==(int32_t)(2*c->rows-1)?-100:0;
 int32_t delta=0;
 if((c->stage&1)&&i->battery_mv>=threshold){
  delta=(threshold-i->battery_mv)*(int32_t)ratio*2;
  if(delta<-100)delta=-100;
  if(delta>-20)delta=-20;
 }else if(i->bus_ma>=(int32_t)i->adapter_ma-200||
          i->bus_ma-compensation>max/(int32_t)ratio||i->battery_ma>=max)delta=-step;
 else if(i->battery_ma<max-200&&i->bus_ma-compensation<(max-200)/(int32_t)ratio&&
         i->battery_mv<threshold-5)delta=step;
 if(i->boost_done&&
    ((delta==20&&(i->now_ms<i->last_boost_ms||i->now_ms-i->last_boost_ms<2000||
                  i->now_ms<i->last_buck_ms||i->now_ms-i->last_buck_ms<1000))||
     (delta==-20&&(i->now_ms<i->last_buck_ms||i->now_ms-i->last_buck_ms<1000))))delta=0;
 return delta;
}
int32_t np_cccv_delta(const NpCccv *c,const NpCccvInput *i)
{return np_cccv_delta_ratio(c,i,2);}
