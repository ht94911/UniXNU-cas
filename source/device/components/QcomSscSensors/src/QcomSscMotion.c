// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "QcomSscMotion.h"
#include <string.h>
#include <limits.h>
static int motion_index(uint32_t kind){return kind==QsscGyro?0:kind==QsscGravity?1:kind==QsscGameRv?2:-1;}
int qssc_motion_begin(QsscMotion *m,uint64_t epoch,uint32_t counter_hz){
    if(!m||!epoch||!counter_hz)return QsscArgument;memset(m,0,sizeof *m);m->epoch=epoch;m->counter_hz=counter_hz;return QsscOkay;
}
void qssc_motion_invalidate(QsscMotion *m){if(m)memset(m->count,0,sizeof m->count);}
int qssc_motion_add(QsscMotion *m,const QsscSample *s){
    if(!m||!s||s->epoch!=m->epoch)return QsscIdentity;int i=motion_index(s->kind);if(i<0)return QsscArgument;
    if(!s->valid){m->invalid++;qssc_motion_invalidate(m);return QsscNoData;}
    unsigned count=m->count[i];
    if(count){QsscSample *last=&m->values[i][(m->head[i]+count-1)%QSSC_MOTION_HISTORY];if(s->ssc_ticks<=last->ssc_ticks)return QsscNoData;}
    if(count==QSSC_MOTION_HISTORY){
        m->dropped++;
        if(i==2)return QsscCapacity;
        m->head[i]=(m->head[i]+1)%QSSC_MOTION_HISTORY;count--;m->count[i]--;
    }
    m->values[i][(m->head[i]+count)%QSSC_MOTION_HISTORY]=*s;m->count[i]++;return QsscOkay;
}
static const QsscSample *at(const QsscMotion *m,unsigned i,unsigned n){return &m->values[i][(m->head[i]+n)%QSSC_MOTION_HISTORY];}
static void pop(QsscMotion *m,unsigned i){m->head[i]=(m->head[i]+1)%QSSC_MOTION_HISTORY;m->count[i]--;}
static const QsscSample *before(QsscMotion *m,unsigned i,uint64_t ticks){
    while(m->count[i]>1&&at(m,i,1)->ssc_ticks<=ticks)pop(m,i);
    return m->count[i]&&at(m,i,0)->ssc_ticks<=ticks?at(m,i,0):0;
}
int qssc_motion_next(QsscMotion *m,QsscMotionFrame *out){
    if(!m||!out||!m->epoch)return QsscArgument;
    while(m->count[2]){
        QsscSample q=*at(m,2,0);if(q.ssc_ticks<=m->last_frame){pop(m,2);continue;}
        const QsscSample *gyro=before(m,0,q.ssc_ticks),*gravity=before(m,1,q.ssc_ticks);
        if(!gyro||!gravity){
            if((m->count[0]&&at(m,0,0)->ssc_ticks>q.ssc_ticks)||(m->count[1]&&at(m,1,0)->ssc_ticks>q.ssc_ticks)){
                m->dropped++;pop(m,2);continue;}
            return QsscAgain;
        }
        uint64_t max_age=(uint64_t)m->counter_hz*40/1000;
        if(q.ssc_ticks-gyro->ssc_ticks>max_age||q.ssc_ticks-gravity->ssc_ticks>max_age){
            // Wait for delayed source reports while their timelines haven't
            // crossed this frame. Later data cannot repair an old stale frame.
            if(at(m,0,m->count[0]-1)->ssc_ticks<q.ssc_ticks||at(m,1,m->count[1]-1)->ssc_ticks<q.ssc_ticks)return QsscAgain;
            m->dropped++;pop(m,2);continue;
        }
        int64_t norm=0;for(unsigned n=0;n<4;n++){
            if(q.value[n]<-66500||q.value[n]>66500)return QsscWire;
            norm+=(int64_t)q.value[n]*q.value[n];
        }
        if(norm<INT64_C(4209067950)||norm>INT64_C(4381296648)){m->invalid++;pop(m,2);return QsscWire;}
        QsscMotionFrame f={0};f.epoch=m->epoch;f.timestamp=q.timestamp;f.ssc_ticks=q.ssc_ticks;f.client_id=q.client_id;
        f.gravity_ticks=gravity->ssc_ticks;f.gyro_ticks=gyro->ssc_ticks;f.status=q.status;
        if(gyro->status<f.status)f.status=gyro->status;if(gravity->status<f.status)f.status=gravity->status;
        // SEE specifies world->device xyzw. CoreMotion attitude represents
        // device->reference, so use the conjugate, retaining the same axes.
        for(unsigned n=0;n<4;n++){if(q.value[n]==INT32_MIN)return QsscCapacity;f.quaternion[n]=n<3?-q.value[n]:q.value[n];}
        for(unsigned n=0;n<3;n++){
            int64_t linear=-(int64_t)gravity->value[n+3]*65536;
            int r=qssc_scaled_q16(linear,100000,980665,&f.user_accel[n]);if(r)return r;
            f.rotation_rate[n]=gyro->value[n];
        }
        if(m->sequence==UINT64_MAX)return QsscCapacity;f.sequence=++m->sequence;f.valid=f.status!=0;
        m->last_frame=q.ssc_ticks;pop(m,2);*out=f;return QsscOkay;
    }return QsscNoData;
}
static void wr32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static void wr64(uint8_t *p,uint64_t n){wr32(p,(uint32_t)n);wr32(p+4,(uint32_t)(n>>32));}
static uint32_t binary32(int32_t q16){
    if(!q16)return 0;bool sign=q16<0;uint32_t n=sign?(uint32_t)(-(int64_t)q16):(uint32_t)q16;
    unsigned bit=0;for(uint32_t v=n;v>1;v>>=1)bit++;
    uint32_t mantissa;
    if(bit<=23)mantissa=n<<(23-bit);
    else{unsigned shift=bit-23;mantissa=(n+(1u<<(shift-1)))>>shift;if(mantissa==0x1000000){mantissa>>=1;bit++;}}
    return (sign?0x80000000u:0)|((bit+111u)<<23)|(mantissa&0x7fffffu);
}
int qssc_motion_payload(const QsscMotionFrame *f,uint8_t *out,size_t bytes){
    if(!f||!f->valid||!out||bytes<60)return QsscArgument;uint8_t p[60]={3};
    // A completed, time-aligned and reliable six-axis frame is initialized.
    // Type4's original parser clears the state word; type3 preserves bit8
    // from bytes1..2, required by CMMotionManager's public callback guard.
    p[2]=1;
    // Type3 parser stores clientID@4. Frame sequence remains diagnostic;
    // preserve the actual SSC client identity and the IOHIDEvent sample time.
    wr64(p+4,f->client_id);
    for(unsigned n=0;n<4;n++)wr32(p+12+4*n,binary32(f->quaternion[n]));
    for(unsigned n=0;n<3;n++){
        // The vendor motion translator and CMDeviceMotion setter preserve
        // radians/s. Only the separate raw gyro HID event uses degrees/s.
        wr32(p+28+4*n,binary32(f->rotation_rate[n]));wr32(p+40+4*n,binary32(f->user_accel[n]));
    }
    memcpy(out,p,sizeof p);return QsscOkay;
}
