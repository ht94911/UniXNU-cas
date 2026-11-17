// SPDX-License-Identifier: BSD-2-Clause-Patent
// Executable production-source cases; build/run only after a test baseline is selected.
#include "QcomSscClient.h"
#include "QcomSscMotion.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
static void w16(uint8_t *p,uint16_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void w32(uint8_t *p,uint32_t n){w16(p,(uint16_t)n);w16(p+2,(uint16_t)(n>>16));}
static void w64(uint8_t *p,uint64_t n){w32(p,(uint32_t)n);w32(p+4,(uint32_t)(n>>32));}
static size_t pbuid(uint8_t *p,QsscUid u){p[0]=9;w64(p+1,u.low);p[9]=17;w64(p+10,u.high);return 18;}
static size_t vector(uint8_t *p,const uint32_t *bits,unsigned count,uint32_t status){
    p[0]=10;p[1]=(uint8_t)(4*count);for(unsigned i=0;i<count;i++)w32(p+2+4*i,bits[i]);p[2+4*count]=16;p[3+4*count]=(uint8_t)status;return 4+4*count;
}
static size_t report(uint8_t *out,QsscUid source,uint32_t id,uint64_t ticks,const uint8_t *payload,size_t bytes){
    uint8_t pb[900];size_t at=0;pb[at++]=10;pb[at++]=18;at+=pbuid(pb+at,source);
    pb[at++]=18;pb[at++]=(uint8_t)(16+bytes);pb[at++]=13;w32(pb+at,id);at+=4;
    pb[at++]=17;w64(pb+at,ticks);at+=8;pb[at++]=26;pb[at++]=(uint8_t)bytes;memcpy(pb+at,payload,bytes);at+=bytes;
    out[0]=4;w16(out+1,0);w16(out+3,33);w16(out+5,(uint16_t)(16+at));out[7]=1;w16(out+8,8);w64(out+10,55);
    out[18]=2;w16(out+19,(uint16_t)(at+2));w16(out+21,(uint16_t)at);memcpy(out+23,pb,at);return 23+at;
}
static size_t ack(uint8_t *p,uint16_t txn){p[0]=2;w16(p+1,txn);w16(p+3,32);w16(p+5,18);p[7]=2;w16(p+8,4);w32(p+10,0);p[14]=16;w16(p+15,8);w64(p+17,55);return 25;}
static size_t cal(uint8_t *p,const uint32_t *bias){p[0]=10;p[1]=12;for(unsigned i=0;i<3;i++)w32(p+2+4*i,bias[i]);p[14]=32;p[15]=3;return 16;}
struct Mock {unsigned sends,samples,states,flushes;uint64_t serial;QsscSample last;int send_result;};
static int send(void *v,QsscEndpoint ep,uint32_t port,const uint8_t *p,size_t bytes){struct Mock *m=v;assert(ep.node==5&&port==0x4001&&bytes>=7&&p[0]==0);m->sends++;return m->send_result;}
static int sample(void *v,const QsscSample *s){struct Mock *m=v;m->samples++;m->last=*s;return 0;}
static int state(void *v,uint32_t kind,uint64_t epoch,bool ready,int error){struct Mock *m=v;(void)kind;(void)ready;(void)error;assert(epoch==1);m->states++;return 0;}
static int flush(void *v,uint32_t kind,uint64_t epoch,uint64_t serial){struct Mock *m=v;assert(kind==QsscProximityKind&&epoch==1);m->flushes++;m->serial=serial;return 0;}
static int identity(void *v,uint32_t kind,const QsscAttributes *a){(void)v;(void)kind;return a->available?0:QsscIdentity;}
static int physical_proximity(void *v,uint32_t kind,const QsscAttributes *a){
    (void)v;assert(kind==QsscProximityKind);
    return a->available&&!strcmp(a->name,"tcs3760")&&!strcmp(a->api,"sns_proximity.proto")?0:QsscIdentity;
}
static void begin(QsscClient *c,struct Mock *m){QsscClock clock={0};assert(!qssc_clock_anchor(&clock,1,19200000,1000000,19200000,19200000,19200000));QsscClientOps ops={m,send,sample,state,flush,identity,false};assert(!qssc_client_begin(c,&ops,(QsscEndpoint){5,17,0x101},0x4001,1,&clock,100));}
static void enabled(QsscClient *c,unsigned kind,QsscUid uid){QsscStream *s=&c->stream[kind];s->uid=uid;s->present=true;s->enabled=true;s->lookup_sent=true;s->attributes_seen=true;s->phase=QsscRunning;s->interval_us=s->configured_interval=20000;s->attributes.available=true;}
static void math(void){
    int64_t x;int32_t y;assert(!qssc_float_q32(0x3f800000,&x)&&x==INT64_C(4294967296));
    assert(!qssc_float_q32(0xbf800000,&x)&&x==-INT64_C(4294967296));
    assert(!qssc_float_q32(0x80000000,&x)&&x==0);assert(qssc_float_q32(0x7f800000,&x)<0);
    assert(!qssc_scaled_q16(INT64_C(4294967296),1,1,&y)&&y==65536);
    assert(!qssc_scaled_q16(-INT64_C(4294967296),1,1,&y)&&y==-65536);
    assert(qssc_scaled_q16(INT64_MAX,UINT32_MAX,1,&y)<0);
    QsscClock c={0};uint64_t t;assert(!qssc_clock_anchor(&c,1,19200000,1000000,19200000,19200000,19200000));
    assert(!qssc_clock_convert(&c,1,19190000,19202000,&t)&&t==990000);
    assert(!qssc_clock_convert(&c,1,19201000,19202000,&t)&&t==1001000);
    assert(qssc_clock_convert(&c,1,19203000,19202000,&t)==QsscTime);assert(qssc_clock_convert(&c,2,19201000,19202000,&t)==QsscTime);
}
static void wire(void){
    uint8_t p[1023],payload[64];uint32_t bits[3]={0x3f800000,0xbf000000,0};size_t n=vector(payload,bits,3,3);
    size_t bytes=report(p,(QsscUid){1,2},1025,19201000,payload,n);QsscReport r;assert(!qssc_decode_report(p,bytes,&r));
    QsscVector v;assert(!qssc_decode_vector(&r.events[0],&v)&&v.count==3&&v.bits[1]==0xbf000000&&v.status==3);
    assert(qssc_decode_report(p,bytes-1,&r)<0);
    bits[2]=0x7fc00000;n=vector(payload,bits,3,3);bytes=report(p,(QsscUid){1,2},1025,19201000,payload,n);
    assert(!qssc_decode_report(p,bytes,&r));assert(qssc_decode_vector(&r.events[0],&v)<0);
    uint8_t prox[]={8,1,16,123,24,3};bytes=report(p,(QsscUid){3,4},769,19201000,prox,sizeof prox);
    assert(!qssc_decode_report(p,bytes,&r));QsscProximity q;assert(!qssc_decode_proximity(&r.events[0],&q)&&q.near==1&&q.raw_adc==123&&q.status==3);
    prox[1]=2;bytes=report(p,(QsscUid){3,4},769,19201000,prox,sizeof prox);assert(!qssc_decode_report(p,bytes,&r));assert(qssc_decode_proximity(&r.events[0],&q)<0);
    uint8_t scalar[]={13,0,0,0,0,21,0,0,128,63,32,3};
    bytes=report(p,(QsscUid){3,4},1022,19201000,scalar,sizeof scalar);
    assert(!qssc_decode_report(p,bytes,&r));QsscCalibration calibration;
    assert(!qssc_decode_calibration(&r.events[0],&calibration)&&calibration.bias_count==1&&calibration.scale_count==1);
    size_t txbytes=0;assert(!qssc_encode_lookup("gyro_cal",7,p,sizeof p,&txbytes));assert(p[0]==0&&p[1]==7&&p[3]==32);
    QsscAttributes a={0},b={0};strcpy(b.name,"lsm6dsv");b.name_valid=true;b.present=1;assert(qssc_attributes_merge(&a,&b)==QsscNoData);
    b=(QsscAttributes){0};strcpy(b.datatype,"gyro");strcpy(b.api,"sns_gyro.proto");b.type_valid=b.available_valid=b.stream_valid=true;b.available=true;b.rate_count=1;b.rates[0]=0x42480000;b.present=4|8|(1u<<5)|(1u<<6)|(1u<<16);assert(!qssc_attributes_merge(&a,&b));
}
static void calibration_and_time(void){
    QsscClient c={0};struct Mock m={0};begin(&c,&m);enabled(&c,QsscGyro,(QsscUid){1,2});enabled(&c,QsscGyroCal,(QsscUid){3,4});
    uint8_t p[1023],payload[64];uint32_t zero[3]={0x3e800000,0,0},bits[3]={0x3f800000,0,0};
    size_t n=cal(payload,zero),bytes=report(p,(QsscUid){3,4},1022,19200010,payload,n);
    assert(!qssc_client_receive(&c,5,17,p,bytes,101,19201000));
    n=vector(payload,bits,3,3);bytes=report(p,(QsscUid){1,2},1025,19200200,payload,n);
    assert(!qssc_client_receive(&c,5,17,p,bytes,102,19202000));assert(m.samples==1&&m.last.value[0]==49152&&m.last.timestamp==1000200);
    assert(!qssc_client_receive(&c,5,17,p,bytes,103,19203000));assert(m.samples==1&&c.stale==1);
    assert(qssc_client_receive(&c,5,17,p,bytes-1,104,19204000)<0&&m.samples==1);
}
static void independent_consumers(void){
    QsscClient c={0};struct Mock m={0};begin(&c,&m);QsscDemand a={20000,10000,true,false},b={10000,0,true,true};
    assert(!qssc_client_demand(&c,QsscAccel,0,a));assert(!qssc_client_demand(&c,QsscAccel,1,b));
    assert(c.stream[0].interval_us==10000&&c.stream[0].batch_us==0&&c.stream[0].wakeup);
    b.active=false;assert(!qssc_client_demand(&c,QsscAccel,1,b));assert(c.stream[0].interval_us==20000&&c.stream[QsscAccelCal].interval_us==1);
    assert(!qssc_client_demand(&c,QsscGyro,0,a));a.active=false;assert(!qssc_client_demand(&c,QsscAccel,0,a));
    assert(c.stream[QsscAccelCal].interval_us==0&&c.stream[QsscGyro].interval_us==20000&&c.stream[QsscGyroCal].interval_us==1);
    m.send_result=QsscAgain;assert(qssc_client_poll(&c,200)==QsscAgain&&c.queued&&!c.awaiting);
    m.send_result=0;assert(!qssc_client_poll(&c,201)&&c.awaiting);unsigned sent=m.sends;
    assert(qssc_client_poll(&c,202)==QsscAgain&&m.sends==sent);assert(qssc_client_poll(&c,3000201)==QsscTimeout&&m.sends==sent);
}
static void early_enable_event(void){
    QsscClient c={0};struct Mock m={0};begin(&c,&m);enabled(&c,QsscProximityKind,(QsscUid){1,2});
    c.stream[QsscProximityKind].enabled=false;c.stream[QsscProximityKind].configured_interval=0;
    assert(!qssc_client_poll(&c,200));uint8_t p[1023];uint8_t prox[]={8,1,16,42,24,3};
    size_t n=report(p,(QsscUid){1,2},769,19200100,prox,sizeof prox);
    assert(!qssc_client_receive(&c,5,17,p,n,201,19201000));assert(!m.samples&&c.pending_count==1);
    n=ack(p,c.transaction);assert(!qssc_client_receive(&c,5,17,p,n,202,19202000));
    assert(m.samples==1&&m.last.near&&m.last.raw_adc==42&&c.pending_count==0);
}
static void flush_ack_order(void){
    for(unsigned order=0;order<2;order++){
        QsscClient c={0};struct Mock m={0};begin(&c,&m);enabled(&c,QsscProximityKind,(QsscUid){1,2});
        assert(!qssc_client_flush(&c,QsscProximityKind,99));assert(!qssc_client_poll(&c,200));uint8_t p[1023];size_t n;
        for(unsigned i=0;i<2;i++){
            if(i==order)n=ack(p,c.transaction);else n=report(p,(QsscUid){1,2},129,19200100,(const uint8_t *)"",0);
            assert(!qssc_client_receive(&c,5,17,p,n,201+i,19201000+i));if(!i)assert(!m.flushes);
        }assert(m.flushes==1&&m.serial==99);
    }
}
static void fusion(void){
    QsscMotion m={0};assert(!qssc_motion_begin(&m,1,19200000));QsscSample s={0};s.epoch=1;s.valid=true;s.status=3;s.timestamp=1000000;s.ssc_ticks=19200000;
    s.kind=QsscGyro;s.value[0]=65536;assert(!qssc_motion_add(&m,&s));
    s.kind=QsscGravity;memset(s.value,0,sizeof s.value);s.value[5]=65536;assert(!qssc_motion_add(&m,&s));
    s.kind=QsscGameRv;memset(s.value,0,sizeof s.value);s.value[3]=65536;assert(!qssc_motion_add(&m,&s));
    QsscMotionFrame f;assert(!qssc_motion_next(&m,&f)&&f.valid&&f.quaternion[3]==65536&&f.rotation_rate[0]==65536&&f.user_accel[2]<0);
    uint8_t p[60];assert(!qssc_motion_payload(&f,p,sizeof p)&&p[0]==4);assert(p[24]==0&&p[25]==0&&p[26]==0x80&&p[27]==0x3f);
    assert(qssc_motion_next(&m,&f)==QsscNoData);qssc_motion_invalidate(&m);assert(!m.count[0]&&!m.count[1]&&!m.count[2]);
}
static void sleep_and_stop(void){
    QsscClient c={0};struct Mock m={0};begin(&c,&m);
    enabled(&c,QsscGravity,(QsscUid){1,2});enabled(&c,QsscProximityKind,(QsscUid){3,4});
    c.stream[QsscProximityKind].wakeup=c.stream[QsscProximityKind].configured_wakeup=true;
    c.stream[QsscProximityKind].configured_interval=c.stream[QsscProximityKind].interval_us=1;
    assert(!qssc_client_pause(&c,true,0));assert(!qssc_client_poll(&c,200));
    assert(c.tx_kind==QsscGravity&&c.tx_id==QsscDisableRequest);
    uint8_t p[1023];size_t n=ack(p,c.transaction);
    assert(!qssc_client_receive(&c,5,17,p,n,201,19201000));
    assert(!c.stream[QsscGravity].enabled&&c.stream[QsscProximityKind].enabled);
    assert(qssc_client_poll(&c,202)==QsscNoData);
    uint8_t near[]={8,1,16,42,24,3};n=report(p,(QsscUid){3,4},769,19201100,near,sizeof near);
    assert(!qssc_client_receive(&c,5,17,p,n,203,19202000));assert(m.samples==1&&m.last.near);
    assert(c.stream[QsscProximityKind].ready);
    assert(!qssc_client_pause(&c,false,&c.clock)&&c.stream[QsscProximityKind].ready);
    assert(!qssc_client_stop(&c)&&!qssc_client_drained(&c));assert(!qssc_client_poll(&c,204));
    assert(c.tx_kind==QsscProximityKind&&c.tx_id==QsscDisableRequest);
    n=ack(p,c.transaction);assert(!qssc_client_receive(&c,5,17,p,n,205,19203000));
    assert(qssc_client_drained(&c));
}
static void acknowledged_delayed_suid(void){
    QsscClient c={0};struct Mock m={0};begin(&c,&m);
    assert(!qssc_client_demand(&c,QsscProximityKind,0,(QsscDemand){1,0,true,true}));
    assert(!qssc_client_poll(&c,200)&&c.tx_id==QsscLookupRequest);
    uint8_t p[1023];size_t n=ack(p,c.transaction);
    assert(!qssc_client_receive(&c,5,17,p,n,201,19201000));
    assert(!c.awaiting&&c.stream[3].lookup_sent);
    assert(qssc_client_poll(&c,5000000)==QsscNoData&&!c.error);
    uint8_t empty[]={10,9,'p','r','o','x','i','m','i','t','y'};
    n=report(p,(QsscUid){QSSC_LOOKUP_WORD,QSSC_LOOKUP_WORD},QsscLookupEvent,19202000,empty,sizeof empty);
    assert(!qssc_client_receive(&c,5,17,p,n,5000001,19203000));
    assert(!c.stream[3].present&&!c.stream[3].error);
    uint8_t lookup[64];memcpy(lookup,empty,sizeof empty);lookup[11]=18;lookup[12]=18;
    n=13+pbuid(lookup+13,(QsscUid){3,4});
    n=report(p,(QsscUid){QSSC_LOOKUP_WORD,QSSC_LOOKUP_WORD},QsscLookupEvent,19203100,lookup,n);
    assert(!qssc_client_receive(&c,5,17,p,n,5000002,19204000));
    assert(c.stream[3].present&&!c.stream[3].error);
    assert(!qssc_client_poll(&c,5000003)&&c.tx_id==QsscAttrRequest);
}
static void retire_decode_failure(void){
    QsscClient c={0};struct Mock m={0};begin(&c,&m);
    enabled(&c,QsscGyro,(QsscUid){1,2});enabled(&c,QsscGyroCal,(QsscUid){3,4});
    uint8_t p[1023],invalid[]={0};
    size_t n=report(p,(QsscUid){1,2},QsscSampleEvent,19200100,invalid,sizeof invalid);
    assert(qssc_client_receive(&c,5,17,p,n,101,19201000)==QsscWire);
    assert(c.error==QsscWire&&c.last_report_kind==QsscGyro&&c.last_event_id==QsscSampleEvent);
    assert(!m.samples&&!qssc_client_stop(&c)&&!qssc_client_drained(&c));
    assert(!qssc_client_poll(&c,102)&&c.tx_id==QsscDisableRequest&&c.tx_kind==QsscGyro);
    unsigned sends=m.sends;
    // Late malformed data is no longer consumed; an unrelated ACK cannot drain.
    n=report(p,(QsscUid){1,2},QsscSampleEvent,19200200,invalid,sizeof invalid);
    assert(qssc_client_receive(&c,5,17,p,n,103,19202000)==QsscNoData&&!m.samples);
    n=ack(p,c.transaction+1);assert(qssc_client_receive(&c,5,17,p,n,104,19203000)==QsscNoData);
    assert(!qssc_client_drained(&c)&&c.stream[QsscGyro].enabled&&c.awaiting&&m.sends==sends);
    n=ack(p,c.transaction);assert(!qssc_client_receive(&c,5,17,p,n,105,19204000));
    assert(!qssc_client_poll(&c,106)&&c.tx_kind==QsscGyroCal);
    n=ack(p,c.transaction);assert(!qssc_client_receive(&c,5,17,p,n,107,19205000));
    assert(c.error==QsscWire&&!c.stop_error&&qssc_client_drained(&c)&&!m.samples);
}
static void retire_pending_enable(void){
    QsscClient c={0};struct Mock m={0};begin(&c,&m);
    enabled(&c,QsscProximityKind,(QsscUid){3,4});c.stream[3].enabled=false;
    assert(!qssc_client_poll(&c,101)&&c.tx_id==QsscOnChangeRequest);
    unsigned sends=m.sends;qssc_client_lost(&c,QsscWire);
    assert(c.awaiting&&!qssc_client_stop(&c));
    assert(qssc_client_poll(&c,102)==QsscAgain&&m.sends==sends&&!qssc_client_drained(&c));
    uint8_t p[1023];size_t n=ack(p,c.transaction);
    assert(!qssc_client_receive(&c,5,17,p,n,103,19201000)&&c.stream[3].enabled);
    assert(!qssc_client_poll(&c,104)&&c.tx_id==QsscDisableRequest&&m.sends==sends+1);
    n=ack(p,c.transaction);assert(!qssc_client_receive(&c,5,17,p,n,105,19202000));
    assert(c.error==QsscWire&&qssc_client_drained(&c));
    // Unknown enable submission retains its transaction until a real ACK.
    c=(QsscClient){0};m=(struct Mock){0};begin(&c,&m);enabled(&c,3,(QsscUid){3,4});c.stream[3].enabled=false;
    m.send_result=QsscRemote;assert(qssc_client_poll(&c,101)==QsscRemote&&c.awaiting);
    m.send_result=0;assert(!qssc_client_stop(&c));n=ack(p,c.transaction);
    assert(!qssc_client_receive(&c,5,17,p,n,102,19201000));assert(!qssc_client_poll(&c,103)&&c.tx_id==QsscDisableRequest);
    n=ack(p,c.transaction);assert(!qssc_client_receive(&c,5,17,p,n,104,19202000));assert(qssc_client_drained(&c));
}
static void retirement_failure_is_not_success(void){
    QsscClient c={0};struct Mock m={0};begin(&c,&m);enabled(&c,3,(QsscUid){3,4});
    qssc_client_lost(&c,QsscWire);assert(!qssc_client_stop(&c));assert(!qssc_client_poll(&c,101));
    unsigned sends=m.sends;assert(qssc_client_poll(&c,3000101)==QsscTimeout);
    assert(c.error==QsscWire&&c.stop_error==QsscTimeout&&!qssc_client_drained(&c)&&m.sends==sends);
    uint8_t p[1023];size_t n=ack(p,c.transaction);assert(qssc_client_receive(&c,5,17,p,n,3000102,19201000)==QsscTimeout);
    c=(QsscClient){0};m=(struct Mock){0};begin(&c,&m);enabled(&c,3,(QsscUid){3,4});
    qssc_client_lost(&c,QsscWire);assert(!qssc_client_stop(&c));assert(!qssc_client_poll(&c,101));
    n=ack(p,c.transaction);p[10]=1;
    assert(qssc_client_receive(&c,5,17,p,n,102,19201000)==QsscRemote);
    assert(c.error==QsscWire&&c.stop_error==QsscRemote&&!qssc_client_drained(&c));
}
static size_t string_attr(uint8_t *p,unsigned id,const uint8_t *text,size_t bytes){
    p[0]=10;p[1]=(uint8_t)(8+bytes);p[2]=8;p[3]=(uint8_t)id;
    p[4]=18;p[5]=(uint8_t)(4+bytes);p[6]=10;p[7]=(uint8_t)(2+bytes);
    p[8]=18;p[9]=(uint8_t)bytes;memcpy(p+10,text,bytes);return 10+bytes;
}
static void vendor_text_is_not_identity(void){
    // The current virtual proximity packet has13-byte vendor text ending in
    // NUL then two non-text bytes; name/type/API remain strict identity fields.
    uint8_t p[64],vendor[]={'X','i','a','o','M','i','(','V','1','.','7',')',0,0xc9,0x32};
    QsscEvent e={QsscAttrEvent,1,p,string_attr(p,1,vendor,sizeof vendor)};QsscAttributes a;
    assert(!qssc_decode_attributes(&e,&a)&&!strcmp(a.vendor,"XiaoMi(V1.7)"));
    e.bytes=string_attr(p,0,vendor,sizeof vendor);assert(qssc_decode_attributes(&e,&a)==QsscWire);
    e.bytes=string_attr(p,5,vendor,sizeof vendor);assert(qssc_decode_attributes(&e,&a)==QsscWire);
}
static void select_physical_proximity(void){
    QsscClient c={0};struct Mock m={0};begin(&c,&m);c.ops.identity=physical_proximity;
    assert(!qssc_client_demand(&c,3,0,(QsscDemand){1,0,true,true}));
    assert(!qssc_client_poll(&c,101));
    assert(c.tx[c.tx_bytes-1]==0); // all SUIDs, not the virtual default.
    uint8_t p[1023],payload[120];size_t n=ack(p,c.transaction);
    assert(!qssc_client_receive(&c,5,17,p,n,102,19201000));
    uint8_t lookup[]={10,9,'p','r','o','x','i','m','i','t','y'};memcpy(payload,lookup,sizeof lookup);
    payload[11]=18;payload[12]=18;pbuid(payload+13,(QsscUid){3,4});payload[31]=18;payload[32]=18;pbuid(payload+33,(QsscUid){5,6});
    n=report(p,(QsscUid){QSSC_LOOKUP_WORD,QSSC_LOOKUP_WORD},768,19201010,payload,51);
    assert(!qssc_client_receive(&c,5,17,p,n,103,19202000)&&c.proximity_choice_count==2);
    assert(!qssc_client_poll(&c,104)&&c.tx_id==QsscAttrRequest&&c.stream[3].uid.low==3);
    n=ack(p,c.transaction);assert(!qssc_client_receive(&c,5,17,p,n,105,19203000));
    // Model previously decoded split attributes, then finish the available bit.
    QsscAttributes *a=&c.stream[3].attributes;strcpy(a->name,"proximity");strcpy(a->datatype,"proximity");strcpy(a->api,"sns_std_sensor.proto");
    a->name_valid=a->type_valid=a->stream_valid=true;a->stream_type=1;a->present=1|4|(1u<<5)|(1u<<16);
    uint8_t available[]={10,8,8,3,18,4,10,2,40,1};
    n=report(p,(QsscUid){3,4},128,19203010,available,sizeof available);
    assert(!qssc_client_receive(&c,5,17,p,n,106,19204000));assert(!c.error&&!c.stream[3].enabled&&c.stream[3].error==QsscIdentity);
    assert(!qssc_client_poll(&c,107)&&c.tx_id==QsscAttrRequest&&c.stream[3].uid.low==5);
    n=ack(p,c.transaction);assert(!qssc_client_receive(&c,5,17,p,n,108,19205000));
    a=&c.stream[3].attributes;strcpy(a->name,"tcs3760");strcpy(a->datatype,"proximity");strcpy(a->api,"sns_proximity.proto");
    a->name_valid=a->type_valid=a->stream_valid=true;a->stream_type=1;a->present=1|4|(1u<<5)|(1u<<16);
    n=report(p,(QsscUid){5,6},128,19205010,available,sizeof available);assert(!qssc_client_receive(&c,5,17,p,n,109,19206000));
    assert(!qssc_client_poll(&c,110)&&c.tx_id==QsscOnChangeRequest&&c.stream[3].uid.low==5);
    assert(!qssc_client_stop(&c));n=ack(p,c.transaction);assert(!qssc_client_receive(&c,5,17,p,n,111,19207000));
    assert(!qssc_client_poll(&c,112)&&c.tx_id==QsscDisableRequest);n=ack(p,c.transaction);
    assert(!qssc_client_receive(&c,5,17,p,n,113,19208000)&&qssc_client_drained(&c));
}
static void optional_absent_accel_cal(void){
    QsscClient c={0};struct Mock m={0};begin(&c,&m);enabled(&c,QsscAccel,(QsscUid){1,2});
    c.stream[QsscAccel].enabled=false;c.stream[QsscAccelCal].lookup_sent=true;
    c.stream[QsscAccel].attributes.rate_count=1;c.stream[QsscAccel].attributes.rates[0]=0x42480000;
    c.stream[QsscAccelCal].interval_us=1;
    // A board has to opt in; an unresolved lookup or an advertised calibration
    // provider cannot be treated as an absent calibration service.
    assert(qssc_client_poll(&c,101)==QsscNoData&&m.sends==0);
    c.ops.optional_accel_cal=true;c.stream[QsscAccelCal].lookup_sent=false;
    assert(!qssc_client_poll(&c,102)&&c.tx_kind==QsscAccelCal&&c.tx_id==QsscLookupRequest);
    c.awaiting=false;c.stream[QsscAccelCal].lookup_sent=true;
    assert(!qssc_client_poll(&c,103)&&c.tx_kind==QsscAccel&&c.tx_id==QsscStreamRequest);
    uint8_t p[1023],payload[64];size_t n=ack(p,c.transaction);
    assert(!qssc_client_receive(&c,5,17,p,n,104,19201000));
    uint32_t values[3]={0x3f800000,0,0};n=vector(payload,values,3,3);
    size_t bytes=report(p,(QsscUid){1,2},QsscSampleEvent,19200200,payload,n);
    assert(!qssc_client_receive(&c,5,17,p,bytes,105,19202000));
    assert(m.samples==1&&m.last.valid&&m.last.status==3&&m.last.value[0]==65536);
    c.stream[QsscAccelCal].present=true;c.stream[QsscAccelCal].uid=(QsscUid){3,4};
    bytes=report(p,(QsscUid){1,2},QsscSampleEvent,19200300,payload,n);
    assert(!qssc_client_receive(&c,5,17,p,bytes,106,19203000)&&m.samples==1&&c.pending_count==1);
    c.stream[QsscAccelCal].enabled=true;uint32_t bias[3]={0x3e800000,0,0};n=cal(payload,bias);
    bytes=report(p,(QsscUid){3,4},QsscCalibrationEvent,19200250,payload,n);
    assert(!qssc_client_receive(&c,5,17,p,bytes,107,19204000)&&m.samples==2&&m.last.value[0]==49152);
}
int main(void){math();wire();calibration_and_time();independent_consumers();flush_ack_order();early_enable_event();fusion();sleep_and_stop();acknowledged_delayed_suid();retire_decode_failure();retire_pending_enable();retirement_failure_is_not_success();vendor_text_is_not_identity();select_physical_proximity();optional_absent_accel_cal();puts("SSC production codec/client/math/fusion/error retirement, optional absent accel calibration and physical proximity selection scenarios passed");}
