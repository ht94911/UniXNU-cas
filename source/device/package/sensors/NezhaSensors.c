// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board glue to an existing ALS/audio single-owner router; no transport creation.
#include "NezhaSensors.h"
#include <string.h>
extern int printf(const char *,...);
static bool announced(struct nezha_sensor_adapter *a,QsscEndpoint ep){
    if(!a->ops.online(a->ops.context)||!a->router->hello_received)return false;
    unsigned count=0;bool exact=false;
    for(unsigned i=0;i<a->router->remote_count;i++){
        if(a->router->remote[i].service!=QSSC_SERVICE||(a->router->remote[i].instance&255)!=QSSC_IDL_MAJOR)continue;
        count++;if(a->router->remote[i].node==ep.node&&a->router->remote[i].port==ep.port&&
            a->router->remote[i].instance==ep.instance)exact=true;}
    return count==1&&exact;
}
static int send(void *v,QsscEndpoint ep,uint32_t port,const uint8_t *qmi,size_t bytes){
    struct nezha_sensor_adapter *a=v;
    if(!announced(a,ep)||port!=a->local_port||!qmi||bytes>QSSC_REQUEST_BYTES)return QsscRemote;
    int r=a->ops.tx_room(a->ops.context,(uint32_t)bytes+32);if(r)return r;
    // qcom_qrtr_send_data calls the original mapper TX queue once; a refusal
    // after the room check has unknown submission and is terminal.
    return qcom_qrtr_send_data(a->router,port,ep.node,ep.port,qmi,(uint32_t)bytes)?QsscRemote:QsscOkay;
}
static int sample(void *v,const QsscSample *s){
    struct nezha_sensor_adapter *a=v;
    // Bounded console observations join raw SSC time to the actual HID sample.
    if(s->kind<QSSC_OUTPUT_COUNT&&(s->sequence<=2||s->sequence%50==0||s->kind==QsscProximityKind))
        printf("NEZHA SENSORS SAMPLE: kind=%u seq=%llu ssc=%llu host=%llu status=%u v=%d,%d,%d near=%u adc=%u\n",
            s->kind,(unsigned long long)s->sequence,(unsigned long long)s->ssc_ticks,(unsigned long long)s->timestamp,
            s->status,s->value[0],s->value[1],s->value[2],s->near,s->raw_adc);
    if(s->kind<QSSC_OUTPUT_COUNT){int r=nezha_sensor_hid_sample(s);if(r)return r;}
    if(s->kind==QsscGyro||s->kind==QsscGravity||s->kind==QsscGameRv){
        // Raw/diagnostic streams may remain active while fusion is off, or
        // while its gyro/calibration subscription is being restored. They
        // must not fill a motion queue waiting for an unavailable source.
        if(!a->client.stream[QsscGameRv].interval_us||
           !a->client.stream[QsscGyro].ready||!a->client.stream[QsscGravity].ready){
            qssc_motion_invalidate(&a->motion);return QsscOkay;
        }
        int r=qssc_motion_add(&a->motion,s);if(r<0)return r;
        for(unsigned n=0;n<QSSC_MOTION_HISTORY;n++){
            QsscMotionFrame frame;r=qssc_motion_next(&a->motion,&frame);if(r==QsscAgain||r==QsscNoData)break;
            if(r)return r;r=nezha_sensor_hid_state(NEZHA_SENSOR_FUSION_KIND,s->epoch,true,0);if(r)return r;
            r=nezha_sensor_hid_motion(&frame);if(r)return r;
        }
    }
    return QsscOkay;
}
static int state(void *v,uint32_t kind,uint64_t epoch,bool ready,int error){
    struct nezha_sensor_adapter *a=v;
    if(!ready&&(kind==QsscGyro||kind==QsscGravity||kind==QsscGameRv)){
        qssc_motion_invalidate(&a->motion);int r=nezha_sensor_hid_state(NEZHA_SENSOR_FUSION_KIND,epoch,false,error);if(r)return r;
    }
    return kind<QSSC_OUTPUT_COUNT?nezha_sensor_hid_state(kind,epoch,ready,error):QsscOkay;
}
static int flushed(void *v,uint32_t kind,uint64_t epoch,uint64_t serial){(void)v;return nezha_sensor_hid_flushed(kind,epoch,serial);}
static bool api_has(const char *apis,const char *name){
    // Existing native source exports strncmp, not strstr. API lists are
    // bounded strings with semicolon separators, not arbitrary substrings.
    size_t n=strlen(name);
    for(const char *p=apis;*p;){if(!strncmp(p,name,n)&&(p[n]==0||p[n]==';'))return true;
        while(*p&&*p!=';')p++;if(*p)p++;}
    return false;
}
static int identity(void *v,uint32_t kind,const QsscAttributes *a){
    (void)v;
    printf("NEZHA SENSORS ATTR: kind=%u model=%s type=%s api=%s available=%u stream=%u rates=%u\n",
        kind,a->name,a->datatype,a->api,a->available,a->stream_type,a->rate_count);
    if(!a->available||!a->name_valid||!a->type_valid||!a->stream_valid)return QsscIdentity;
    if(kind==QsscAccel||kind==QsscGyro){
        if(strcmp(a->name,"lsm6dsv")||a->stream_type!=0||!a->rate_count||
           !api_has(a->api,kind==QsscAccel?"sns_accel.proto":"sns_gyro.proto"))return QsscIdentity;
    }else if(kind==QsscProximityKind){
        bool physical=!strcmp(a->name,"tcs3760")&&api_has(a->api,"sns_proximity.proto");
        // This nezha firmware exposes the stock Xiaomi virtual proximity
        // service as its only on-change provider. Its original HAL decodes
        // the same769 sns_proximity_event; report the source explicitly.
        bool virtual_provider=!strcmp(a->name,"proximity")&&!strcmp(a->vendor,"XiaoMi(V1.7)")&&
            api_has(a->api,"sns_std_sensor.proto");
        if(a->stream_type!=1||(!physical&&!virtual_provider))return QsscIdentity;
        printf("NEZHA SENSORS: proximity provider=%s vendor=%s physical=%u\n",
            physical?"tcs3760":"xiaomi-virtual",a->vendor,physical);
    }else if(kind==QsscGravity){
        if(a->stream_type!=0||!a->rate_count||!api_has(a->api,"sns_gravity.proto"))return QsscIdentity;
    }else if(kind==QsscAccelCal||kind==QsscGyroCal){
        // The measured nezha qsh HAL still decodes1022 sns_cal_event from
        // the gyro_cal/accel_cal services advertising sns_oem/std_sensor.
        if(a->stream_type!=1||(!api_has(a->api,"sns_cal.proto")&&
           !(api_has(a->api,"sns_oem.proto")&&api_has(a->api,"sns_std_sensor.proto"))))return QsscIdentity;
    }else if(kind==QsscGameRv){
        if(a->stream_type!=0||!a->rate_count||!api_has(a->api,"sns_game_rv.proto"))return QsscIdentity;
    }else return QsscArgument;
    printf("NEZHA SENSORS: identity kind=%u model=%s type=%s stream=%u rates=%u\n",kind,a->name,a->datatype,a->stream_type,a->rate_count);
    return QsscOkay;
}
static int anchor(struct nezha_sensor_adapter *a){
    uint64_t before=a->ops.counter_ticks(a->ops.context),host=a->ops.host_ticks(a->ops.context),after=a->ops.counter_ticks(a->ops.context);
    return qssc_clock_anchor(&a->clock,a->epoch,before,host,after,a->ops.counter_hz,a->ops.host_hz);
}
void nezha_sensors_lost(struct nezha_sensor_adapter *a,int error){
    if(!a||!a->prepared||a->error)return;a->error=error<0?error:QsscRemote;
    a->ops.lock(a->ops.context);a->accepting=false;a->count=0;a->ops.unlock(a->ops.context);
    if(a->selected)qssc_client_lost(&a->client,a->error);
    else for(unsigned k=0;k<QSSC_OUTPUT_COUNT;k++)nezha_sensor_hid_state(k,a->epoch,false,a->error);
    qssc_motion_invalidate(&a->motion);nezha_sensor_hid_state(NEZHA_SENSOR_FUSION_KIND,a->epoch,false,a->error);
    printf("NEZHA SENSORS: lost epoch=%llu rc=%d\n",(unsigned long long)a->epoch,a->error);
}
static void receive(void *v,QcomQrtr *router,uint32_t local,uint32_t node,uint32_t port,const uint8_t *data,uint32_t bytes){
    struct nezha_sensor_adapter *a=v;
    if(router!=a->router||local!=a->local_port||!a->selected||(a->error&&!a->stopped))return;
    int r=qssc_client_receive(&a->client,node,port,data,bytes,a->ops.now_us(a->ops.context),a->ops.counter_ticks(a->ops.context));
    if(r<0){
        if(!a->error){
            // Save exactly the first rejected normal packet, after the parser
            // returned. Existing console only; no extra transport reader.
            if(bytes<=QSSC_REPORT_BYTES){char hex[2*QSSC_REPORT_BYTES+1];static const char digits[]="0123456789abcdef";
                for(unsigned n=0;n<bytes;n++){hex[2*n]=digits[data[n]>>4];hex[2*n+1]=digits[data[n]&15];}hex[2*bytes]=0;
                printf("NEZHA SENSORS WIRE: rc=%d kind=%u event=%u bytes=%u tx=%u/%u data=%s\n",
                    r,a->client.last_report_kind,a->client.last_event_id,bytes,a->client.tx_kind,a->client.tx_id,hex);
            }
        }
        nezha_sensors_lost(a,r);
        // Begin retirement in this same worker callback. Waiting until the
        // later USB shutdown would discard intervening ACKs and expire the
        // already submitted transaction while the client is faulted.
        if(!a->stopped)(void)nezha_sensors_stop(a);
    }
}
static uint32_t free_port(const QcomQrtr *q){
    if(q->local_count>=QCOM_QRTR_MAX_LOCAL)return 0;
    for(uint32_t port=0x4000;port<0x4000+QCOM_QRTR_MAX_LOCAL;port++){
        bool used=false;for(unsigned n=0;n<q->local_count;n++)used|=q->local[n].port==port;
        if(!used)return port;}
    return 0;
}
int nezha_sensors_prepare(struct nezha_sensor_adapter *a,void *provider,QcomQrtr *router,
 const struct nezha_sensor_owner_ops *ops,uint64_t epoch){
    if(!a||a->prepared||!provider||!router||!ops||!epoch||!ops->lock||!ops->unlock||!ops->wake||
       !ops->online||!ops->now_us||!ops->counter_ticks||!ops->host_ticks||!ops->tx_room||!ops->counter_hz||!ops->host_hz)return QsscArgument;
    memset(a,0,sizeof *a);a->ops=*ops;a->router=router;a->epoch=epoch;a->local_port=free_port(router);
    if(!a->local_port)return QsscCapacity;int r=anchor(a);if(r)return r;
    r=qssc_motion_begin(&a->motion,epoch,ops->counter_hz);if(r)return r;
    const struct nezha_sensor_hid_ops hid={a,nezha_sensors_request,nezha_sensors_request_flush,nezha_sensors_request_motion,nezha_sensors_consumer_fault};
    if(!nezha_sensor_hid_prepare(provider,&hid,epoch))return QsscConsumer;
    // Boot-lifetime callbacks retain a. Stop invalidates delivery without
    // removing/reinitializing another owner's client table or IPCRTR channel.
    if(qcom_qrtr_add_client(router,a->local_port,receive,a)){nezha_sensor_hid_stop();return QsscCapacity;}
    a->prepared=true;a->accepting=true;
    if(!nezha_sensor_hid_activate()){nezha_sensors_lost(a,QsscConsumer);return a->error;}
    return QsscOkay;
}
int nezha_sensors_rebind(struct nezha_sensor_adapter *a,QcomQrtr *router,uint64_t epoch){
    if(!a||!a->prepared||!a->selected||!a->error||a->stopped||!router||epoch<=a->epoch)return QsscArgument;
    // A fresh router comes only from the real owner after remote recovery;
    // never initialize/reset it here or clear another owner's table.
    uint32_t port=free_port(router);if(!port)return QsscCapacity;
    a->ops.lock(a->ops.context);a->accepting=false;a->count=0;a->ops.unlock(a->ops.context);
    a->router=router;a->local_port=port;a->epoch=epoch;int r=anchor(a);if(r)return r;
    r=qssc_motion_begin(&a->motion,epoch,a->ops.counter_hz);if(r)return r;
    if(!nezha_sensor_hid_rebind(epoch))return QsscConsumer;
    if(qcom_qrtr_add_client(router,port,receive,a))return QsscCapacity;
    a->selected=false;a->error=0;a->consumer_fault=0;
    a->ops.lock(a->ops.context);a->accepting=true;a->ops.unlock(a->ops.context);return QsscOkay;
}
static int32_t enqueue(struct nezha_sensor_adapter *a,struct nezha_sensor_command command){
    if(!a||!a->prepared)return (int32_t)0xe00002d8u;
    a->ops.lock(a->ops.context);
    if(!a->accepting){a->ops.unlock(a->ops.context);return (int32_t)0xe00002d8u;}
    if(a->count==NEZHA_SENSOR_COMMANDS){a->ops.unlock(a->ops.context);return (int32_t)0xe00002bdu;}
    a->commands[(a->head+a->count)%NEZHA_SENSOR_COMMANDS]=command;a->count++;
    a->ops.unlock(a->ops.context);a->ops.wake(a->ops.context);return 0;
}
int32_t nezha_sensors_request(void *v,uint32_t kind,uint32_t consumer,QsscDemand d){
    if(kind>=QSSC_OUTPUT_COUNT||consumer>=QSSC_CONSUMERS||d.batch_us>1000000||
       (d.active&&kind!=QsscProximityKind&&(d.interval_us<2000||d.interval_us>1000000)))return (int32_t)0xe00002c2u;
    struct nezha_sensor_command command={1,kind,consumer,d,0};return enqueue(v,command);
}
int32_t nezha_sensors_request_flush(void *v,uint32_t kind,uint64_t serial){
    if(kind>=QSSC_OUTPUT_COUNT||!serial)return (int32_t)0xe00002c2u;
    struct nezha_sensor_command command={2,kind,0,{0},serial};return enqueue(v,command);
}
int32_t nezha_sensors_request_motion(void *v,uint32_t consumer,QsscDemand d){
    if(consumer>=QSSC_CONSUMERS||d.batch_us>1000000||
       (d.active&&(d.interval_us<2000||d.interval_us>1000000)))return (int32_t)0xe00002c2u;
    struct nezha_sensor_command command={3,NEZHA_SENSOR_FUSION_KIND,consumer,d,0};return enqueue(v,command);
}
void nezha_sensors_consumer_fault(void *v,int error){
    struct nezha_sensor_adapter *a=v;if(!a||!a->prepared)return;
    a->ops.lock(a->ops.context);a->consumer_fault=error<0?error:QsscConsumer;
    a->accepting=false;a->ops.unlock(a->ops.context);a->ops.wake(a->ops.context);
}
int nezha_sensors_poll(struct nezha_sensor_adapter *a){
    if(!a||!a->prepared)return QsscArgument;if(a->error&&!a->stopped)return a->error;
    if(a->stopped&&a->selected){
        if(!announced(a,a->client.endpoint))return QsscRemote;
        return qssc_client_poll(&a->client,a->ops.now_us(a->ops.context));
    }
    a->ops.lock(a->ops.context);int fault=a->consumer_fault;a->ops.unlock(a->ops.context);
    if(fault){
        nezha_sensors_lost(a,fault);
        if(a->selected){(void)nezha_sensors_stop(a);return qssc_client_poll(&a->client,a->ops.now_us(a->ops.context));}
        return a->error;
    }
    if(a->stopped&&!a->selected)return QsscNoData;
    if(!a->selected){
        if(!a->ops.online(a->ops.context)||!a->router->hello_received)return QsscAgain;
        unsigned count=0;QsscEndpoint ep={0};
        for(unsigned n=0;n<a->router->remote_count;n++){
            if(a->router->remote[n].service!=QSSC_SERVICE||(a->router->remote[n].instance&255)!=QSSC_IDL_MAJOR)continue;
            count++;ep=(QsscEndpoint){a->router->remote[n].node,a->router->remote[n].port,a->router->remote[n].instance};}
        if(!count)return QsscAgain;if(count!=1){nezha_sensors_lost(a,QsscIdentity);return a->error;}
        // Actual stock accelerometer::get_pairedcalsuid falls back to physical
        // SEE data when no dynamic accel_cal provider exists on this board.
        QsscClientOps ops={a,send,sample,state,flushed,identity,true};
        int r=a->client.epoch?qssc_client_rebind(&a->client,ep,a->local_port,a->epoch,&a->clock,a->ops.now_us(a->ops.context)):
            qssc_client_begin(&a->client,&ops,ep,a->local_port,a->epoch,&a->clock,a->ops.now_us(a->ops.context));
        if(r){nezha_sensors_lost(a,r);return r;}a->selected=true;
        if(a->paused){r=qssc_client_pause(&a->client,true,0);if(r)return r;}
        printf("NEZHA SENSORS: SSC service=400 dst=%u/%u local=%u epoch=%llu\n",ep.node,ep.port,a->local_port,(unsigned long long)a->epoch);
    }
    if(!announced(a,a->client.endpoint)){nezha_sensors_lost(a,QsscRemote);return a->error;}
    for(unsigned n=0;n<NEZHA_SENSOR_COMMANDS;n++){
        struct nezha_sensor_command cmd;a->ops.lock(a->ops.context);
        if(!a->count){a->ops.unlock(a->ops.context);break;}
        cmd=a->commands[a->head];a->head=(a->head+1)%NEZHA_SENSOR_COMMANDS;a->count--;a->ops.unlock(a->ops.context);
        int r;
        if(cmd.operation==1)r=qssc_client_demand(&a->client,cmd.kind,cmd.consumer,cmd.demand);
        else if(cmd.operation==2)r=qssc_client_flush(&a->client,cmd.kind,cmd.serial);
        else r=qssc_client_demand(&a->client,QsscGameRv,cmd.consumer,cmd.demand);
        if(r){nezha_sensors_lost(a,r);return r;}
    }
    int r=qssc_client_poll(&a->client,a->ops.now_us(a->ops.context));
    if(r<0){nezha_sensors_lost(a,r);if(!a->stopped)(void)nezha_sensors_stop(a);}return r;
}
int nezha_sensors_sleep(struct nezha_sensor_adapter *a,bool sleeping){
    if(!a||!a->prepared||a->error||a->stopped)return QsscArgument;
    if(!a->selected){a->paused=sleeping;return QsscOkay;}
    int r=sleeping?0:anchor(a);if(r)return r;
    r=qssc_client_pause(&a->client,sleeping,sleeping?0:&a->clock);a->paused=sleeping;
    if(sleeping){qssc_motion_invalidate(&a->motion);nezha_sensor_hid_state(NEZHA_SENSOR_FUSION_KIND,a->epoch,false,0);}
    return r;
}
int nezha_sensors_stop(struct nezha_sensor_adapter *a){
    if(!a||!a->prepared)return QsscArgument;
    a->ops.lock(a->ops.context);a->accepting=false;a->count=0;a->ops.unlock(a->ops.context);
    int r=a->selected?qssc_client_stop(&a->client):QsscOkay;
    // Owner keeps polling to receive per-SUID disable ACKs before final shutdown.
    // HID objects remain retained until that drain is complete.
    a->stopped=true;nezha_sensor_hid_stop();return r;
}
void nezha_sensors_status(struct nezha_sensor_adapter *a,struct nezha_sensor_status *s){
    if(!a||!s)return;*s=(struct nezha_sensor_status){0};s->epoch=a->epoch;
    s->reports=a->client.reports;s->responses=a->client.responses;s->delivered=a->client.delivered;
    s->invalid=a->client.invalid;s->stale=a->client.stale;s->overflows=a->client.overflows;
    for(unsigned k=0;k<QSSC_SENSOR_COUNT;k++)s->phase[k]=a->client.stream[k].phase;
    for(unsigned k=0;k<QSSC_OUTPUT_COUNT;k++)s->samples[k]=a->client.stream[k].sequence;
    a->ops.lock(a->ops.context);s->queued_commands=a->count;a->ops.unlock(a->ops.context);
    s->prepared=a->prepared;s->selected=a->selected;s->stopped=a->stopped;s->error=a->error;
}
