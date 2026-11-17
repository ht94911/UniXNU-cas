// SPDX-License-Identifier: BSD-2-Clause-Patent
// Compile the actual HID, adapter and native caller against software interfaces.
#include "sensors-caller-host-shim.h"
#include <cstring>
#define NEZHA_SENSORS_HID_HOST 1
#define NEZHA_SENSORS_CALLER_HOST 1
#include "../package/sensors/NezhaSensorHid.cpp"
#include "../package/sensors/NezhaSensorNativeCaller.cpp"
extern "C" {
kmod_info_t g_kernel_kmod_info={"kernel"};
void nezha_sensor_static_init(){hostStaticInitializers++;}
IOHIDEvent *nezha_sensor_original_accel(uint64_t t,int32_t x,int32_t y,int32_t z,uint32_t,uint32_t,uint32_t,uint32_t){
    auto e=new IOHIDEvent(t,0);e->type=13;e->xyz[0]=x;e->xyz[1]=y;e->xyz[2]=z;return e;
}
IOHIDEvent *nezha_sensor_original_gyro(uint64_t t,int32_t x,int32_t y,int32_t z,uint32_t,uint32_t,uint32_t,uint32_t){
    auto e=nezha_sensor_original_accel(t,x,y,z,0,0,0,0);e->type=20;return e;
}
IOHIDEvent *nezha_sensor_original_prox(uint64_t t,uint32_t mask,uint32_t level,uint32_t){
    auto e=new IOHIDEvent(t,level);e->type=14;e->xyz[0]=(int32_t)mask;return e;
}
IOHIDEvent *nezha_sensor_original_vendor(uint64_t t,uint32_t,uint32_t,uint32_t,uint8_t *p,uint32_t n,uint32_t){
    auto e=new IOHIDEvent(t,0);e->type=1;e->payload.assign(p,p+n);return e;
}
}
static int queued(void *,const uint8_t *,uint32_t){return 0;}
static void priorClient(void *,QcomQrtr *,uint32_t,uint32_t,uint32_t,const uint8_t *,uint32_t){}
static void online_stream(unsigned k){
    auto &s=caller.adapter.client.stream[k];s.present=true;s.enabled=true;s.phase=QsscRunning;
    s.lookup_sent=true;s.attributes_seen=true;s.uid={k+1,k+8};s.attributes.available=true;
    s.interval_us=s.configured_interval=k==QsscProximityKind?1:20000;
    s.wakeup=s.configured_wakeup=k==QsscProximityKind;
}
static void deliver_qmi(const uint8_t *p,size_t bytes){
    uint8_t packet[160]={};auto word=[&](unsigned at,uint32_t n){for(unsigned i=0;i<4;i++)packet[at+i]=(uint8_t)(n>>(i*8));};
    assert(bytes<=sizeof(packet)-32);word(0,1);word(4,1);word(8,5);word(12,17);word(20,(uint32_t)bytes);
    word(24,1);word(28,caller.adapter.local_port);memcpy(packet+32,p,bytes);
    assert(!qcom_qrtr_receive(caller.adapter.router,packet,(uint32_t)(32+((bytes+3)&~3u))));
}
static void ack_disable(bool require_disable=true){
    auto &c=caller.adapter.client;if(!c.awaiting)return;
    if(require_disable)assert(c.tx_id==QsscDisableRequest);int original_error=c.error;uint8_t p[25]={2};
    p[1]=(uint8_t)c.transaction;p[2]=(uint8_t)(c.transaction>>8);p[3]=32;p[5]=18;p[7]=2;p[8]=4;p[14]=16;p[15]=8;p[17]=55;
    sensors_host_counter+=19200;sensors_host_ticks+=19200;
    deliver_qmi(p,sizeof p);assert(c.error==original_error&&!c.stop_error);
}
int main(int argc,char **argv){
    if(argc>1&&std::string(argv[1])=="stop-before-install"){
        assert(!nezha_sensors_stop_and_wait(10));NaAdsp late={};late.stage=NaAdspPower;
        assert(!nezha_sensors_native_install(&late,&late,1));late.stage=NaAdspOnline;
        assert(!nezha_sensors_native_prepared(&late)&&!caller.adapter.prepared);
        puts("Stop-before-install never starts sensor requests");return 0;
    }
    NaAdsp owner={};owner.stage=NaAdspPower;
    assert(!nezha_sensors_native_install(&owner,&owner,1));
    owner.stage=NaAdspOnline;owner.glink_initialized=owner.dma_ready=1;owner.mapper.online=true;
    qcom_qrtr_init(&owner.mapper.qrtr,1,queued,nullptr);
    assert(!qcom_qrtr_add_client(&owner.mapper.qrtr,0x4000,priorClient,nullptr));
    assert(!nezha_sensors_native_prepared(&owner));assert(hostRegistrations==0&&caller.adapter.local_port==0x4001);
    auto &router=owner.mapper.qrtr;router.hello_received=true;router.remote_count=1;
    router.remote[0]={400,0x101,5,17};
    auto &adapter=caller.adapter;adapter.count=0;
    assert(nezha_sensors_native_after_receive(&owner)==QsscNoData&&adapter.selected);
    QsscAttributes calibrationAttributes={};calibrationAttributes.available=true;
    calibrationAttributes.name_valid=calibrationAttributes.type_valid=calibrationAttributes.stream_valid=true;
    calibrationAttributes.stream_type=1;
    strcpy(calibrationAttributes.name,"gyro_cal");strcpy(calibrationAttributes.datatype,"gyro_cal");
    strcpy(calibrationAttributes.api,"sns_oem.proto;sns_std_sensor.proto");
    assert(!adapter.client.ops.identity(adapter.client.ops.context,QsscGyroCal,&calibrationAttributes));
    strcpy(calibrationAttributes.api,"sns_oem.proto");
    assert(adapter.client.ops.identity(adapter.client.ops.context,QsscGyroCal,&calibrationAttributes)==QsscIdentity);
    QsscAttributes proxAttributes={};proxAttributes.available=true;
    proxAttributes.name_valid=proxAttributes.type_valid=proxAttributes.stream_valid=true;
    proxAttributes.stream_type=1;strcpy(proxAttributes.name,"proximity");
    strcpy(proxAttributes.datatype,"proximity");strcpy(proxAttributes.vendor,"XiaoMi(V1.7)");
    strcpy(proxAttributes.api,"sns_std_sensor.proto");
    assert(!adapter.client.ops.identity(adapter.client.ops.context,QsscProximityKind,&proxAttributes));
    strcpy(proxAttributes.vendor,"unknown");
    assert(adapter.client.ops.identity(adapter.client.ops.context,QsscProximityKind,&proxAttributes)==QsscIdentity);
    for(unsigned k=0;k<QSSC_SENSOR_COUNT;k++)online_stream(k);
    if(argc>1&&std::string(argv[1])=="parser-error-retirement"){
        auto &c=adapter.client;
        // Retain one already submitted stream request while an attribute event
        // from another stream fails on the actual QRTR callback.
        c.stream[QsscGyro].enabled=false;c.awaiting=true;c.tx_kind=QsscGyro;c.tx_id=QsscStreamRequest;
        c.transaction=37;c.tx_interval=20000;c.deadline_us=sensors_host_us+3000000;
        uint8_t p[63]={4};auto w16=[&](unsigned at,uint16_t x){p[at]=(uint8_t)x;p[at+1]=(uint8_t)(x>>8);};
        auto w32=[&](unsigned at,uint32_t x){for(unsigned n=0;n<4;n++)p[at+n]=(uint8_t)(x>>(8*n));};
        auto w64=[&](unsigned at,uint64_t x){for(unsigned n=0;n<8;n++)p[at+n]=(uint8_t)(x>>(8*n));};
        w16(3,33);w16(5,56);p[7]=1;w16(8,8);w64(10,55);p[18]=2;w16(19,42);w16(21,40);
        p[23]=10;p[24]=18;p[25]=9;w64(26,c.stream[QsscAccel].uid.low);p[34]=17;w64(35,c.stream[QsscAccel].uid.high);
        p[43]=18;p[44]=18;p[45]=13;w32(46,QsscAttrEvent);p[50]=17;w64(51,19200100);p[59]=26;p[60]=2;p[61]=10;p[62]=0;
        deliver_qmi(p,sizeof p);
        assert(adapter.error==QsscWire&&c.error==QsscWire&&c.awaiting&&c.stopped&&adapter.stopped);
        assert(c.last_report_kind==QsscAccel&&c.last_event_id==QsscAttrEvent);
        assert(nezha_sensor_hid_sample(nullptr)==QsscArgument);
        unsigned acknowledged=0;
        sensors_host_worker=[&](){
            (void)nezha_sensors_native_after_receive(&owner);
            if(c.awaiting){ack_disable(false);acknowledged++;}
            (void)nezha_sensors_native_after_receive(&owner);
        };
        // Hardware ACKs arrive on the existing worker before any USB return.
        for(unsigned i=0;i<16&&!qssc_client_drained(&c);i++)sensors_host_worker();
        assert(acknowledged==QSSC_SENSOR_COUNT+1&&qssc_client_drained(&c));
        sensors_host_us+=10000000;
        assert(!nezha_sensors_stop_and_wait(30000));
        assert(acknowledged==QSSC_SENSOR_COUNT+1&&nezha_sensors_native_drained(&owner));
        assert(adapter.error==QsscWire&&c.error==QsscWire&&!c.stop_error);
        puts("Actual QRTR parser failure preserved while original enable and all disable ACKs drain");return 0;
    }
    QsscSample s={};s.epoch=1;s.valid=true;s.status=3;s.sequence=1;s.timestamp=19200000;
    s.kind=QsscAccel;s.value[2]=642688;assert(!nezha_sensor_hid_state(s.kind,1,true,0));
    assert(!nezha_sensor_hid_sample(&s));
    // copyEvent converts real SI and uses the sample timestamp before timer dispatch.
    auto e=services[0]->copyEvent(13,nullptr,0);assert(e&&e->timestamp==s.timestamp&&e->xyz[2]<-65530&&e->xyz[2]>-65542);e->release();
    s.kind=QsscGyro;s.value[0]=65536;s.value[1]=-65536;s.value[2]=0;
    assert(!nezha_sensor_hid_state(s.kind,1,true,0));assert(!nezha_sensor_hid_sample(&s));
    e=services[1]->copyEvent(20,nullptr,0);
    assert(e&&e->timestamp==s.timestamp&&e->xyz[0]==3754936&&e->xyz[1]==-3754936&&e->xyz[2]==0);
    // Apply the actual original public CMGyroData getter's float pi/180.
    constexpr float degreesToRadians=0.01745329238474369f;
    float publicRate=(float(e->xyz[0])/65536.0f)*degreesToRadians;
    assert(publicRate>0.99999f&&publicRate<1.00001f);e->release();
    s.kind=QsscProximityKind;s.near=true;s.raw_adc=42;
    assert(!nezha_sensor_hid_state(s.kind,1,true,0));assert(!nezha_sensor_hid_sample(&s));
    e=services[3]->copyEvent(14,nullptr,0);assert(e&&e->level==1&&e->xyz[0]==64);e->release();
    QsscMotionFrame motion={};motion.epoch=1;motion.valid=true;motion.sequence=7;motion.timestamp=19300000;motion.quaternion[3]=65536;
    motion.rotation_rate[0]=65536;motion.rotation_rate[1]=-131072;motion.rotation_rate[2]=196608;
    motion.user_accel[0]=16384;motion.user_accel[1]=-32768;
    assert(!nezha_sensor_hid_state(7,1,true,0));
    IOService clients[2];void *contexts[2]={};auto fusion=services[4];
    assert(fusion->openForClient(&clients[0],0,nullptr,&contexts[0]));
    assert(fusion->openForClient(&clients[1],0,nullptr,&contexts[1]));
    assert(contexts[0]!=contexts[1]);assert(!nezha_sensor_hid_motion(&motion));
    e=fusion->copyEventForClient(nullptr,0,contexts[0]);assert(e&&e->payload.size()==60);
    assert(e->payload[0]==3&&e->payload[1]==0&&e->payload[2]==1);
    float rates[3],linear[3];memcpy(rates,e->payload.data()+28,sizeof rates);memcpy(linear,e->payload.data()+40,sizeof linear);
    assert(rates[0]==1.0f&&rates[1]==-2.0f&&rates[2]==3.0f);
    assert(linear[0]==0.25f&&linear[1]==-0.5f&&linear[2]==0.0f);e->release();
    assert(!fusion->copyEventForClient(nullptr,0,contexts[0]));
    e=fusion->copyEventForClient(nullptr,0,contexts[1]);assert(e);e->release();
    fusion->closeForClient(&clients[0],contexts[0],0);
    motion.sequence++;assert(!nezha_sensor_hid_motion(&motion));
    e=fusion->copyEventForClient(nullptr,0,contexts[1]);assert(e);e->release();
    assert(!fusion->copyEventForClient(nullptr,0,contexts[0]));
    // Ignore software demand setup; this case isolates worker drain and PM callbacks.
    adapter.count=0;for(unsigned k=0;k<QSSC_SENSOR_COUNT;k++)online_stream(k);
    sensors_host_power(nullptr,nullptr,kIOMessageSystemWillSleep,nullptr,(void *)1,0);
    assert(!nezha_sensors_native_after_receive(&owner));assert(sensors_host_acks==0);
    while(adapter.client.awaiting){ack_disable();int r=nezha_sensors_native_after_receive(&owner);assert(r>=0);}
    assert(sensors_host_acks==1&&adapter.client.stream[3].enabled);
    sensors_host_worker=[&](){ack_disable();(void)nezha_sensors_native_after_receive(&owner);};
    assert(!nezha_sensors_stop_and_wait(30000));assert(nezha_sensors_native_drained(&owner));
    assert(sensors_host_wakes>0&&!services[4]->copyEvent(1,nullptr,0));
    puts("Production sensor HID/FastPath/shared router/sleep/disable drain scenarios passed");
}
