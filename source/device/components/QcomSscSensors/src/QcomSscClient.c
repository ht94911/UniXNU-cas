// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "QcomSscClient.h"
#include <limits.h>
#include <string.h>
static const char *const datatypes[QSSC_SENSOR_COUNT]={"accel","gyro","gravity","proximity","accel_cal","gyro_cal","game_rv"};
static const QsscUid lookup={QSSC_LOOKUP_WORD,QSSC_LOOKUP_WORD};
static int state(QsscClient *c,unsigned k,bool ready,int error){
    QsscStream *s=&c->stream[k];s->ready=ready;
    if((k<QSSC_OUTPUT_COUNT||k==QsscGameRv)&&c->ops.state(c->ops.context,k,c->epoch,ready,error))return QsscConsumer;
    return QsscOkay;
}
void qssc_client_lost(QsscClient *c,int error){
    if(!c||!c->epoch||c->error)return;
    c->error=error<0?error:QsscRemote;c->awaiting=false;c->queued=false;c->pending_count=0;
    for(unsigned k=0;k<QSSC_SENSOR_COUNT;k++){c->stream[k].phase=QsscFailed;c->stream[k].error=c->error;
        if(state(c,k,false,c->error))c->error=QsscConsumer;}
    memset(c->calibration_count,0,sizeof c->calibration_count);
}
static int fail(QsscClient *c,int error){qssc_client_lost(c,error);return c->error;}
int qssc_client_begin(QsscClient *c,const QsscClientOps *ops,QsscEndpoint ep,uint32_t port,
 uint64_t epoch,const QsscClock *clock,uint64_t now){
    if(!c||!ops||!ops->send||!ops->sample||!ops->state||!ops->flushed||!ops->identity||!port||port>=0xfffffffeu||
       !ep.node||!ep.port||ep.port>=0xfffffffeu||(ep.instance&255)!=QSSC_IDL_MAJOR||
       !clock||!clock->valid||clock->epoch!=epoch||!epoch||c->epoch)return QsscArgument;
    memset(c,0,sizeof *c);c->ops=*ops;c->endpoint=ep;c->local_port=port;c->epoch=epoch;c->clock=*clock;c->last_now_us=now;
    for(unsigned k=0;k<QSSC_SENSOR_COUNT;k++){c->stream[k].datatype=datatypes[k];c->stream[k].phase=QsscCold;}
    return QsscOkay;
}
int qssc_client_rebind(QsscClient *c,QsscEndpoint ep,uint32_t port,uint64_t epoch,
 const QsscClock *clock,uint64_t now){
    if(!c||!c->epoch||!c->error||c->stopped||epoch<=c->epoch||!clock||!clock->valid||clock->epoch!=epoch||
       !port||port>=0xfffffffeu||!ep.node||!ep.port||ep.port>=0xfffffffeu||(ep.instance&255)!=QSSC_IDL_MAJOR)return QsscArgument;
    QsscDemand demands[QSSC_OUTPUT_COUNT][QSSC_CONSUMERS];
    for(unsigned k=0;k<QSSC_OUTPUT_COUNT;k++)memcpy(demands[k],c->stream[k].demands,sizeof demands[k]);
    QsscClientOps ops=c->ops;c->epoch=0;int r=qssc_client_begin(c,&ops,ep,port,epoch,clock,now);if(r)return r;
    for(unsigned k=0;k<QSSC_OUTPUT_COUNT;k++)for(unsigned n=0;n<QSSC_CONSUMERS;n++){
        r=qssc_client_demand(c,k,n,demands[k][n]);if(r)return r;}
    return QsscOkay;
}
static void merge(QsscClient *c,unsigned k){
    QsscStream *s=&c->stream[k];uint32_t interval=0,batch=UINT32_MAX;bool wake=false;
    for(unsigned n=0;n<QSSC_CONSUMERS;n++){QsscDemand d=s->demands[n];if(!d.active)continue;
        if(!interval||d.interval_us<interval)interval=d.interval_us;if(d.batch_us<batch)batch=d.batch_us;wake|=d.wakeup;}
    s->interval_us=interval;s->batch_us=interval?batch:0;s->wakeup=wake;
    if(k==QsscAccel||k==QsscGyro){unsigned cal=k==QsscAccel?QsscAccelCal:QsscGyroCal;
        c->stream[cal].interval_us=interval?1:0;c->stream[cal].wakeup=wake;c->stream[cal].batch_us=0;}
}
int qssc_client_demand(QsscClient *c,uint32_t k,uint32_t consumer,QsscDemand d){
    if(!c||!c->epoch||c->stopped||c->error||!(k<QSSC_OUTPUT_COUNT||k==QsscGameRv)||consumer>=QSSC_CONSUMERS)return QsscArgument;
    if(d.active&&(k!=QsscProximityKind&&(d.interval_us<2000||d.interval_us>1000000)))return QsscArgument;
    if(d.active&&k==QsscProximityKind)d.interval_us=1;
    if(d.batch_us>1000000)return QsscArgument;
    c->stream[k].demands[consumer]=d;merge(c,k);
    if(k==QsscGravity){c->stream[QsscGameRv].demands[consumer]=d;merge(c,QsscGameRv);}
    if(c->queued&&(c->tx_kind==k||((k==QsscAccel||k==QsscGyro)&&c->tx_kind==k+QsscAccelCal)))c->queued=false;
    if(c->stream[k].error==QsscUnsupported&&d.active)c->stream[k].error=0;
    if(!c->stream[k].interval_us){c->retired_ticks[k]=c->last_receipt_counter;
        if(state(c,k,false,0))return fail(c,QsscConsumer);}
    return QsscOkay;
}
int qssc_client_flush(QsscClient *c,uint32_t k,uint64_t serial){
    if(!c||c->error||c->stopped||k>=QSSC_OUTPUT_COUNT||!serial||!c->stream[k].enabled||c->stream[k].flush_pending)return QsscArgument;
    c->stream[k].flush_pending=true;c->stream[k].flush_sent=false;
    c->stream[k].flush_ack=false;c->stream[k].flush_complete=false;
    c->stream[k].flush_serial=serial;return QsscOkay;
}
static int prepare(QsscClient *c,unsigned k,unsigned id){
    // Reuse transaction IDs only after the previous request completed; QMI's
    // wire identity is 16bit, separate from sample sequence and owner epoch.
    c->transaction++;if(!c->transaction)c->transaction=1;
    c->tx_kind=k;c->tx_id=id;c->lookup_ack=false;c->lookup_event=false;c->attribute_ack=false;c->attribute_event=false;
    QsscStream *s=&c->stream[k];int r;
    c->tx_interval=s->interval_us;c->tx_batch=s->batch_us;c->tx_wakeup=s->wakeup;
    if(id==QsscLookupRequest)r=qssc_encode_lookup(s->datatype,c->transaction,c->tx,sizeof c->tx,&c->tx_bytes);
    else if(id==QsscStreamRequest){
        uint32_t hz=(1000000u+s->interval_us-1)/s->interval_us,bits;
        r=qssc_integer_rate(hz,&bits);
        if(!r){uint32_t selected=0;for(unsigned n=0;n<s->attributes.rate_count;n++){
            uint32_t candidate=s->attributes.rates[n];if(candidate>=bits&&(!selected||candidate<selected))selected=candidate;}
            if(!selected){s->error=QsscUnsupported;s->phase=QsscFailed;
                if(state(c,k,false,s->error))return fail(c,QsscConsumer);return QsscNoData;}bits=selected;
            r=qssc_encode_stream(s->uid,bits,s->wakeup,s->batch_us,c->transaction,c->tx,sizeof c->tx,&c->tx_bytes);}
    }else r=qssc_encode_request(s->uid,id,s->wakeup,id==QsscOnChangeRequest?s->batch_us:0,0,0,c->transaction,c->tx,sizeof c->tx,&c->tx_bytes);
    if(r)return fail(c,r);c->queued=true;return QsscOkay;
}
static bool wanted(const QsscClient *c,unsigned k){return !c->stopped&&(!c->paused||c->stream[k].wakeup)&&c->stream[k].interval_us&&!c->stream[k].error;}
static bool starting(const QsscClient *c,unsigned k){
    return c->awaiting&&c->tx_kind==k&&(c->tx_id==QsscStreamRequest||c->tx_id==QsscOnChangeRequest);
}
static int choose(QsscClient *c){
    // Cancel unused hardware requests first; each SUID is disabled independently.
    for(unsigned k=0;k<QSSC_SENSOR_COUNT;k++)if(c->stream[k].enabled&&!wanted(c,k))return prepare(c,k,QsscDisableRequest);
    // Calibration subscriptions precede physical streams, preserving stock bias ownership.
    static const unsigned order[]={QsscAccelCal,QsscGyroCal,QsscAccel,QsscGyro,QsscGravity,QsscProximityKind,QsscGameRv};
    for(unsigned n=0;n<QSSC_SENSOR_COUNT;n++){unsigned k=order[n];QsscStream *s=&c->stream[k];if(!wanted(c,k))continue;
        if(!s->lookup_sent){s->phase=QsscLookup;return prepare(c,k,QsscLookupRequest);}
        if(!s->present)continue;
        if(!s->attributes_seen)return prepare(c,k,QsscAttrRequest);
        if(k==QsscAccel||k==QsscGyro){unsigned cal=k==QsscAccel?QsscAccelCal:QsscGyroCal;
            if(c->stream[cal].error){
                if(state(c,k,false,c->stream[cal].error))return fail(c,QsscConsumer);continue;}
            if(!c->stream[cal].enabled)continue;}
        if(!s->enabled||s->interval_us!=s->configured_interval||s->batch_us!=s->configured_batch||s->wakeup!=s->configured_wakeup)
            return prepare(c,k,k==QsscAccelCal||k==QsscGyroCal||k==QsscProximityKind?QsscOnChangeRequest:QsscStreamRequest);
        if(s->flush_pending&&!s->flush_serial)return fail(c,QsscArgument);
        if(s->flush_pending&&!s->flush_sent)return prepare(c,k,QsscFlushRequest);
    }return QsscNoData;
}
int qssc_client_poll(QsscClient *c,uint64_t now){
    if(!c||!c->epoch)return QsscArgument;if(c->error)return c->error;
    if(now<c->last_now_us)return fail(c,QsscTime);c->last_now_us=now;
    for(unsigned k=0;k<QSSC_OUTPUT_COUNT;k++)if(c->stream[k].flush_pending&&c->stream[k].flush_sent&&
        now>=c->stream[k].flush_deadline)return fail(c,QsscTimeout);
    if(c->awaiting){if(now>=c->deadline_us)return fail(c,QsscTimeout);return QsscAgain;}
    if(!c->queued){int r=choose(c);if(r)return r;}
    int r=c->ops.send(c->ops.context,c->endpoint,c->local_port,c->tx,c->tx_bytes);
    if(r==QsscAgain)return r;if(r)return fail(c,r<0?r:QsscRemote);
    c->queued=false;c->awaiting=true;
    if((c->tx_id==QsscStreamRequest||c->tx_id==QsscOnChangeRequest)&&!c->stream[c->tx_kind].enabled)
        c->subscription_floor[c->tx_kind]=c->last_receipt_counter;
    if(now>UINT64_MAX-3000000)return fail(c,QsscTime);c->deadline_us=now+3000000;
    if(c->tx_id==QsscFlushRequest){c->stream[c->tx_kind].flush_sent=true;c->stream[c->tx_kind].flush_deadline=c->deadline_us;}
    return QsscOkay;
}
static void lookup_complete(QsscClient *c){
    if(c->awaiting&&c->tx_id==QsscLookupRequest&&c->lookup_ack&&c->lookup_event){
        QsscStream *s=&c->stream[c->tx_kind];s->lookup_sent=true;s->phase=s->present?QsscIdle:QsscMissing;c->awaiting=false;}
    if(c->awaiting&&c->tx_id==QsscAttrRequest&&c->attribute_ack&&c->attribute_event)c->awaiting=false;
}
static int response(QsscClient *c,const uint8_t *raw,size_t bytes){
    if(!c->awaiting){c->stale++;return QsscNoData;}QsscResponse r;
    int e=qssc_decode_response(raw,bytes,c->transaction,&r);if(e==QsscIdentity){c->stale++;return QsscNoData;}if(e)return fail(c,e);
    if(r.client_id_valid){if(c->client_id_valid&&r.client_id!=c->client_id)return fail(c,QsscIdentity);c->client_id=r.client_id;c->client_id_valid=true;}
    c->responses++;QsscStream *s=&c->stream[c->tx_kind];
    if(c->tx_id==QsscLookupRequest){c->lookup_ack=true;lookup_complete(c);return QsscOkay;}
    if(c->tx_id==QsscAttrRequest){c->attribute_ack=true;lookup_complete(c);return QsscOkay;}
    if(c->tx_id==QsscDisableRequest){s->enabled=false;s->phase=QsscIdle;s->configured_interval=0;s->flush_pending=false;s->flush_sent=false;
        if(state(c,c->tx_kind,false,0))return fail(c,QsscConsumer);
        if(c->tx_kind==QsscAccelCal||c->tx_kind==QsscGyroCal)c->calibration_count[c->tx_kind-QsscAccelCal]=0;}
    else if(c->tx_id==QsscStreamRequest||c->tx_id==QsscOnChangeRequest){
        s->enabled=true;s->phase=QsscRunning;s->configured_interval=c->tx_interval;
        s->configured_batch=c->tx_batch;s->configured_wakeup=c->tx_wakeup;}
    else if(c->tx_id==QsscFlushRequest)s->flush_ack=true;
    c->awaiting=false;return QsscOkay;
}
static int lookup_report(QsscClient *c,const QsscReport *r){
    for(unsigned n=0;n<r->count;n++){
        const QsscEvent *e=&r->events[n];if(e->id!=QsscLookupEvent){if(e->id==QsscErrorEvent)return fail(c,QsscRemote);continue;}
        for(unsigned k=0;k<QSSC_SENSOR_COUNT;k++){QsscUid u={0};bool present=false;int x=qssc_decode_lookup(e,datatypes[k],&u,&present);
            if(x==QsscIdentity)continue;if(x)return fail(c,x);QsscStream *s=&c->stream[k];
            if(s->present&&(!present||!qssc_uid_equal(s->uid,u))){
                // A service update cannot silently replace an active subscription.
                // The actual owner must establish a fresh session and epoch.
                return fail(c,QsscIdentity);
            }
            s->present=present;if(present){s->uid=u;
                if(s->phase==QsscMissing){s->phase=QsscIdle;s->error=0;}}
            if(!present){s->error=QsscNoData;
                if(state(c,k,false,QsscNoData))return fail(c,QsscConsumer);}
            if(c->awaiting&&c->tx_id==QsscLookupRequest&&c->tx_kind==k)c->lookup_event=true;
            break;
        }
    }lookup_complete(c);return QsscOkay;
}
static int calibration(QsscClient *c,unsigned k,const QsscEvent *e){
    QsscCalibration v;int r=qssc_decode_calibration(e,&v);if(r)return r;
    if(!v.bias_count){c->calibration_count[k-QsscAccelCal]=0;
        if(state(c,k-QsscAccelCal,false,QsscNoData))return QsscConsumer;return QsscNoData;}
    if(v.bias_count!=3)return QsscUnsupported;
    // Dynamic cal service supplies bias; factory scale/matrix stays in physical SEE.
    // Stock calibration::get_bias consumes bias+status. Other fields are
    // validated as metadata and are not applied again in the client.
    unsigned j=k-QsscAccelCal;QsscBias b={0};b.ticks=e->ticks;b.status=v.status;
    for(unsigned a=0;a<3;a++){r=qssc_float_q32(v.bias[a],&b.bias[a]);if(r)return r;}
    unsigned count=c->calibration_count[j];
    if(count&&e->ticks<=c->calibration[j][count-1].ticks){c->stale++;return QsscNoData;}
    if(count==QSSC_CAL_HISTORY){
        for(unsigned n=0;n<c->pending_count;n++){QsscPending *p=&c->pending[(c->pending_head+n)%QSSC_PENDING_SAMPLES];
            if(p->kind==j&&p->ticks<c->calibration[j][1].ticks)return QsscCapacity;}
        memmove(&c->calibration[j][0],&c->calibration[j][1],(QSSC_CAL_HISTORY-1)*sizeof b);count--;}
    c->calibration[j][count++]=b;c->calibration_count[j]=count;
    if(!b.status&&state(c,j,false,QsscNoData))return QsscConsumer;return QsscOkay;
}
static const QsscBias *bias_for(const QsscClient *c,unsigned k,uint64_t ticks){
    unsigned j=k==QsscAccel?0:1;const QsscBias *selected=0;
    for(unsigned n=0;n<c->calibration_count[j];n++){
        const QsscBias *b=&c->calibration[j][n];if(b->ticks>ticks)break;selected=b;
    }return selected;
}
static int sample(QsscClient *c,unsigned k,const QsscEvent *e,const QsscVector *v,const QsscProximity *proximity,uint64_t receipt){
    QsscStream *s=&c->stream[k];if(!wanted(c,k)){c->stale++;return QsscNoData;}
    if(!s->enabled){if(starting(c,k))return QsscAgain;c->stale++;return QsscNoData;}
    if(e->ticks<=s->last_ticks||e->ticks<=c->retired_ticks[k]||e->ticks<c->subscription_floor[k]){c->stale++;return QsscNoData;}
    QsscSample a={0};a.epoch=c->epoch;a.kind=k;a.ssc_ticks=e->ticks;a.client_id=c->client_id;
    int r=qssc_clock_convert(&c->clock,c->epoch,e->ticks,receipt,&a.timestamp);if(r)return r;
    if(a.timestamp<=s->last_host)return QsscTime;
    if(k==QsscProximityKind){if(!proximity)return QsscArgument;QsscProximity p=*proximity;
        a.near=p.near!=0;a.raw_adc=p.raw_adc;a.status=p.status;
    }else{
        unsigned count=k==QsscGravity?6:k==QsscGameRv?4:3;if(v->count!=count)return QsscWire;a.status=v->status;
        const QsscBias *b=0;
        if((k==QsscAccel||k==QsscGyro)&&a.status){
            b=bias_for(c,k,e->ticks);
            if(!b){unsigned j=k==QsscAccel?0:1;
                if(c->calibration_count[j]&&c->calibration[j][0].ticks>e->ticks){c->stale++;return QsscNoData;}
                return QsscAgain;}
            if(b->status<a.status)a.status=b->status;}
        for(unsigned n=0;n<count;n++){int64_t si;r=qssc_float_q32(v->bits[n],&si);if(r)return r;
            if(b){int64_t bias=b->bias[n];if((bias>0&&si<INT64_MIN+bias)||(bias<0&&si>INT64_MAX+bias))return QsscCapacity;si-=bias;}
            r=qssc_scaled_q16(si,1,1,&a.value[n]);if(r)return r;
        }
    }
    if(s->sequence==UINT64_MAX)return QsscCapacity;
    a.valid=a.status!=0;a.sequence=++s->sequence;s->last_ticks=e->ticks;s->last_host=a.timestamp;
    if(!a.valid){c->invalid++;return state(c,k,false,QsscNoData);}
    if(!s->ready){r=state(c,k,true,0);if(r)return r;}
    if(c->ops.sample(c->ops.context,&a))return QsscConsumer;c->delivered++;return QsscOkay;
}
static int drain_pending(QsscClient *c){
    unsigned count=c->pending_count;
    bool blocked[QSSC_SENSOR_COUNT]={false};
    while(count--){QsscPending p=c->pending[c->pending_head];
        c->pending_head=(c->pending_head+1)%QSSC_PENDING_SAMPLES;c->pending_count--;
        QsscEvent e={QsscSampleEvent,p.ticks,0,0};
        int r=blocked[p.kind]?QsscAgain:sample(c,p.kind,&e,&p.vector,&p.proximity,c->last_receipt_counter);
        if(r==QsscAgain){blocked[p.kind]=true;unsigned at=(c->pending_head+c->pending_count)%QSSC_PENDING_SAMPLES;c->pending[at]=p;c->pending_count++;}
        else if(r<0)return r;}
    return QsscOkay;
}
static int complete_flushes(QsscClient *c){
    int r=drain_pending(c);if(r<0)return r;
    for(unsigned k=0;k<QSSC_OUTPUT_COUNT;k++){
        QsscStream *s=&c->stream[k];if(!s->flush_pending||!s->flush_ack||!s->flush_complete)continue;
        bool pending=false;for(unsigned n=0;n<c->pending_count;n++)
            pending|=c->pending[(c->pending_head+n)%QSSC_PENDING_SAMPLES].kind==k;
        if(pending)continue;
        uint64_t serial=s->flush_serial;s->flush_pending=false;s->flush_sent=false;
        if(c->ops.flushed(c->ops.context,k,c->epoch,serial))return QsscConsumer;
    }return QsscOkay;
}
int qssc_client_receive(QsscClient *c,uint32_t node,uint32_t port,const uint8_t *raw,size_t bytes,uint64_t now,uint64_t receipt){
    if(!c||!c->epoch||!raw||!bytes)return QsscArgument;if(c->error)return c->error;
    if(node!=c->endpoint.node||port!=c->endpoint.port){c->stale++;return QsscNoData;}
    if(now<c->last_now_us||receipt<c->last_receipt_counter)return fail(c,QsscTime);
    if(c->awaiting&&now>=c->deadline_us)return fail(c,QsscTimeout);
    c->last_now_us=now;c->last_receipt_counter=receipt;
    if(raw[0]==2){int r=response(c,raw,bytes);if(r<0)return r;
        int complete=complete_flushes(c);return complete<0?fail(c,complete):r;}
    QsscReport r;int result=qssc_decode_report(raw,bytes,&r);if(result<0)return fail(c,result);
    if(c->client_id_valid&&r.client_id!=c->client_id){c->stale++;return QsscNoData;}
    if(!c->client_id_valid){c->client_id_valid=true;c->client_id=r.client_id;}
    c->reports++;
    if(qssc_uid_equal(r.source,lookup))return lookup_report(c,&r);
    unsigned k=0;while(k<QSSC_SENSOR_COUNT&&(!c->stream[k].present||!qssc_uid_equal(c->stream[k].uid,r.source)))k++;
    if(k==QSSC_SENSOR_COUNT){c->stale++;return QsscNoData;}
    // Validate all data payloads before delivering any event from this report.
    for(unsigned n=0;n<r.count;n++){
        QsscEvent *e=&r.events[n];QsscVector v;
        if(e->id==QsscSampleEvent){result=qssc_decode_vector(e,&v);if(result<0)return fail(c,result);
            if(k<QsscGravity&&v.count!=3)return fail(c,QsscWire);if(k==QsscGravity&&v.count!=6)return fail(c,QsscWire);
            if(k==QsscGameRv&&v.count!=4)return fail(c,QsscWire);}
        else if(e->id==QsscProximityEvent&&k==QsscProximityKind){QsscProximity p;result=qssc_decode_proximity(e,&p);if(result)return fail(c,result);}
        else if(e->id==QsscCalibrationEvent){QsscCalibration a;result=qssc_decode_calibration(e,&a);if(result)return fail(c,result);}
        else if(e->id==QsscErrorEvent){uint32_t ecode;result=qssc_decode_error(e,&ecode);if(result||ecode)return fail(c,result?result:QsscRemote);}
        else if(e->id==QsscAttrEvent){QsscAttributes a;result=qssc_decode_attributes(e,&a);if(result)return fail(c,result);}
        else if(e->id==QsscFlushEvent&&e->bytes)return fail(c,QsscWire);
    }
    for(unsigned n=0;n<r.count;n++){
        QsscEvent *e=&r.events[n];
        if(e->id==QsscAttrEvent){
            QsscAttributes a;result=qssc_decode_attributes(e,&a);if(result)return fail(c,result);
            QsscStream *s=&c->stream[k];result=qssc_attributes_merge(&s->attributes,&a);
            if(result==QsscNoData)continue;if(result)return fail(c,result);
            if(strcmp(s->attributes.datatype,datatypes[k]))return fail(c,QsscIdentity);
            result=c->ops.identity(c->ops.context,k,&s->attributes);
            if(result){s->error=result;s->phase=QsscFailed;if(state(c,k,false,result))return fail(c,QsscConsumer);}
            else{s->attributes_seen=true;s->error=0;}
            if(c->awaiting&&c->tx_id==QsscAttrRequest&&c->tx_kind==k){c->attribute_event=true;lookup_complete(c);}
        }else if(e->id==QsscCalibrationEvent&&(k==QsscAccelCal||k==QsscGyroCal)){
            if(!wanted(c,k)||(!c->stream[k].enabled&&!starting(c,k))){c->stale++;continue;}
            result=calibration(c,k,e);if(result<0)return fail(c,result);}
        else if((e->id==QsscSampleEvent&&(k<=QsscGravity||k==QsscGameRv))||(e->id==QsscProximityEvent&&k==QsscProximityKind)){
            QsscVector v={0};QsscProximity proximity={0};
            if(e->id==QsscSampleEvent)qssc_decode_vector(e,&v);
            else qssc_decode_proximity(e,&proximity);
            bool earlier=false;for(unsigned j=0;j<c->pending_count;j++)
                if(c->pending[(c->pending_head+j)%QSSC_PENDING_SAMPLES].kind==k)earlier=true;
            result=earlier?QsscAgain:sample(c,k,e,&v,&proximity,receipt);
            if(result==QsscAgain){
                if(c->pending_count==QSSC_PENDING_SAMPLES){c->overflows++;return fail(c,QsscCapacity);}
                unsigned at=(c->pending_head+c->pending_count)%QSSC_PENDING_SAMPLES;
                c->pending[at]=(QsscPending){k,e->ticks,v,proximity};c->pending_count++;
            }else if(result<0)return fail(c,result);
        }else if(e->id==QsscFlushEvent&&k<QSSC_OUTPUT_COUNT&&c->stream[k].flush_pending&&c->stream[k].flush_sent){
            c->stream[k].flush_complete=true;
        }
    }
    result=complete_flushes(c);return result<0?fail(c,result):QsscOkay;
}
int qssc_client_pause(QsscClient *c,bool pause,const QsscClock *wake_clock){
    if(!c||c->error||c->stopped)return QsscArgument;
    if(!pause&&(!wake_clock||!wake_clock->valid||wake_clock->epoch!=c->epoch))return QsscTime;
    c->paused=pause;
    unsigned pending=c->pending_count;
    while(pending--){
        QsscPending p=c->pending[c->pending_head];
        c->pending_head=(c->pending_head+1)%QSSC_PENDING_SAMPLES;c->pending_count--;
        if(c->stream[p.kind].wakeup){c->pending[(c->pending_head+c->pending_count)%QSSC_PENDING_SAMPLES]=p;c->pending_count++;}
    }
    if(pause&&c->queued&&c->tx_id!=QsscDisableRequest&&!c->stream[c->tx_kind].wakeup)c->queued=false;
    if(pause)for(unsigned k=0;k<QSSC_SENSOR_COUNT;k++)if(!c->stream[k].wakeup)c->retired_ticks[k]=c->last_receipt_counter;
    for(unsigned k=0;k<QSSC_OUTPUT_COUNT;k++)if(!c->stream[k].wakeup&&state(c,k,false,0))return fail(c,QsscConsumer);
    if(!pause)c->clock=*wake_clock;return QsscOkay;
}
int qssc_client_stop(QsscClient *c){
    if(!c||!c->epoch)return QsscArgument;c->stopped=true;c->pending_count=0;
    if(c->queued&&c->tx_id!=QsscDisableRequest)c->queued=false;
    for(unsigned k=0;k<QSSC_OUTPUT_COUNT;k++)if(state(c,k,false,0))return fail(c,QsscConsumer);
    return c->error?c->error:QsscOkay;
}
bool qssc_client_drained(const QsscClient *c){
    if(!c||!c->epoch||!c->stopped||c->error||c->awaiting||c->queued)return false;
    for(unsigned k=0;k<QSSC_SENSOR_COUNT;k++)if(c->stream[k].enabled)return false;
    return true;
}
