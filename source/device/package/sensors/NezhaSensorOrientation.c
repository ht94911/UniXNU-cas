// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NezhaSensorOrientation.h"
#include <string.h>
static uint64_t magnitude(int32_t value){return value<0?(uint64_t)-(int64_t)value:(uint64_t)value;}
int nezha_orientation_begin(struct nezha_orientation_filter *f,uint64_t epoch,uint32_t hz){
    if(!f||!epoch||!hz)return QsscArgument;
    memset(f,0,sizeof *f);f->epoch=epoch;f->counter_hz=hz;return QsscOkay;
}
void nezha_orientation_invalidate(struct nezha_orientation_filter *f){
    if(!f)return;f->candidate=NezhaOrientationUnknown;f->candidate_since=0;
    f->published=NezhaOrientationUnknown;
}
int nezha_orientation_sample(struct nezha_orientation_filter *f,const QsscSample *s,uint32_t *out){
    if(!f||!s||!out||!f->counter_hz||s->kind!=QsscGravity)return QsscArgument;
    if(s->epoch!=f->epoch)return QsscIdentity;
    if(!s->valid){nezha_orientation_invalidate(f);return QsscNoData;}
    if(!s->ssc_ticks||s->ssc_ticks<=f->last_ticks)return QsscNoData;
    if(f->last_ticks&&s->ssc_ticks-f->last_ticks>(uint64_t)f->counter_hz/2){
        f->candidate=NezhaOrientationUnknown;f->candidate_since=0;
    }
    f->last_ticks=s->ssc_ticks;
    uint64_t axis[3]={magnitude(s->value[0]),magnitude(s->value[1]),magnitude(s->value[2])};
    unsigned largest=0;for(unsigned n=1;n<3;n++)if(axis[n]>axis[largest])largest=n;
    uint64_t second=0;for(unsigned n=0;n<3;n++)if(n!=largest&&axis[n]>second)second=axis[n];
    // At least0.65g along one axis and0.15g separation avoids selecting a
    // side at a diagonal boundary. A clear direction must persist250ms.
    uint32_t direction=NezhaOrientationUnknown;
    if(axis[largest]>=417750&&axis[largest]-second>=96403){
        if(largest==0)direction=s->value[0]>0?NezhaOrientationLandscapeLeft:NezhaOrientationLandscapeRight;
        else if(largest==1)direction=s->value[1]>0?NezhaOrientationPortrait:NezhaOrientationPortraitUpsideDown;
        else direction=s->value[2]>0?NezhaOrientationFaceUp:NezhaOrientationFaceDown;
    }
    if(direction==NezhaOrientationUnknown){f->candidate=direction;f->candidate_since=0;return QsscNoData;}
    if(direction!=f->candidate){f->candidate=direction;f->candidate_since=s->ssc_ticks;return QsscNoData;}
    if(direction==f->published||s->ssc_ticks-f->candidate_since<(uint64_t)f->counter_hz/4)return QsscNoData;
    f->published=direction;*out=direction;return QsscOkay;
}
