// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "QcomSscMath.h"
#include <limits.h>
int qssc_float_q32(uint32_t bits,int64_t *out){
    if(!out||!qssc_float_finite(bits))return QsscArgument;
    uint32_t e=(bits>>23)&255,m=bits&0x7fffff;
    if(e)m|=0x800000;
    int shift=(e?(int)e-127:-126)+9;
    uint64_t magnitude;
    if(shift>=0){if(shift>39||m>(uint64_t)INT64_MAX>>shift)return QsscCapacity;magnitude=(uint64_t)m<<shift;}
    else{unsigned right=(unsigned)-shift;magnitude=right>=64?0:((uint64_t)m+(right?UINT64_C(1)<<(right-1):0))>>right;}
    *out=bits>>31?-(int64_t)magnitude:(int64_t)magnitude;return QsscOkay;
}
int qssc_scaled_q16(int64_t value,uint32_t numerator,uint32_t denominator,int32_t *out){
    if(!out||!numerator||!denominator)return QsscArgument;
    bool negative=value<0;uint64_t n=negative?(uint64_t)(-(value+1))+1:(uint64_t)value;
    if(n>UINT64_MAX/numerator)return QsscCapacity;n*=numerator;
    uint64_t d=(uint64_t)denominator<<16,rounded=n/d;
    if(n%d>=(d+1)/2)rounded++;
    uint64_t limit=negative?(uint64_t)INT32_MAX+1:(uint64_t)INT32_MAX;
    if(rounded>limit)return QsscCapacity;
    *out=negative?(int32_t)-(int64_t)rounded:(int32_t)rounded;return QsscOkay;
}
int qssc_integer_rate(uint32_t hz,uint32_t *bits){
    if(!bits||!hz||hz>1000)return QsscArgument;
    unsigned e=0;for(uint32_t n=hz;n>1;n>>=1)e++;
    *bits=((127+e)<<23)|((hz<<(23-e))&0x7fffff);return QsscOkay;
}
int qssc_clock_anchor(QsscClock *c,uint64_t epoch,uint64_t before,uint64_t host,
 uint64_t after,uint32_t counter_hz,uint32_t host_hz){
    if(!c||!epoch||!host||!counter_hz||!host_hz||after<before||
       after-before>(uint64_t)counter_hz/1000)return QsscTime;
    *c=(QsscClock){epoch,before+(after-before)/2,host,counter_hz,host_hz,true};return QsscOkay;
}
int qssc_clock_convert(const QsscClock *c,uint64_t epoch,uint64_t sample,
 uint64_t receipt,uint64_t *out){
    if(!c||!out||!c->valid||!c->counter_hz||!c->host_hz||epoch!=c->epoch||!sample||sample>receipt)return QsscTime;
    bool before=sample<c->counter_anchor;
    uint64_t delta=before?c->counter_anchor-sample:sample-c->counter_anchor;
    uint64_t seconds=delta/c->counter_hz,part=delta%c->counter_hz;
    if(seconds>UINT64_MAX/c->host_hz)return QsscTime;
    uint64_t ticks=seconds*c->host_hz,fraction=part*c->host_hz/c->counter_hz;
    if(ticks>UINT64_MAX-fraction)return QsscTime;ticks+=fraction;
    if(before){if(ticks>=c->host_anchor)return QsscTime;*out=c->host_anchor-ticks;}
    else{if(ticks>UINT64_MAX-c->host_anchor)return QsscTime;*out=c->host_anchor+ticks;}
    return QsscOkay;
}
