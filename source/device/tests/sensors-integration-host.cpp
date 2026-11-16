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
static void ack_disable(){
    auto &c=caller.adapter.client;if(!c.awaiting)return;
    assert(c.tx_id==QsscDisableRequest);uint8_t p[25]={2};
    p[1]=(uint8_t)c.transaction;p[2]=(uint8_t)(c.transaction>>8);p[3]=32;p[5]=18;p[7]=2;p[8]=4;p[14]=16;p[15]=8;p[17]=55;
    sensors_host_counter+=19200;sensors_host_ticks+=19200;
    uint8_t packet[60]={};auto word=[&](unsigned at,uint32_t n){for(unsigned i=0;i<4;i++)packet[at+i]=(uint8_t)(n>>(i*8));};
    word(0,1);word(4,1);word(8,5);word(12,17);word(20,25);word(24,1);word(28,caller.adapter.local_port);
    memcpy(packet+32,p,sizeof p);
    assert(!qcom_qrtr_receive(caller.adapter.router,packet,sizeof packet));
    assert(!c.error);
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
    for(unsigned k=0;k<QSSC_SENSOR_COUNT;k++)online_stream(k);
    QsscSample s={};s.epoch=1;s.valid=true;s.status=3;s.sequence=1;s.timestamp=19200000;
    s.kind=QsscAccel;s.value[2]=642688;assert(!nezha_sensor_hid_state(s.kind,1,true,0));
    assert(!nezha_sensor_hid_sample(&s));
    // copyEvent converts real SI and uses the sample timestamp before timer dispatch.
    auto e=services[0]->copyEvent(13,nullptr,0);assert(e&&e->timestamp==s.timestamp&&e->xyz[2]<-65530&&e->xyz[2]>-65542);e->release();
    s.kind=QsscProximityKind;s.near=true;s.raw_adc=42;
    assert(!nezha_sensor_hid_state(s.kind,1,true,0));assert(!nezha_sensor_hid_sample(&s));
    e=services[3]->copyEvent(14,nullptr,0);assert(e&&e->level==1&&e->xyz[0]==64);e->release();
    QsscMotionFrame motion={};motion.epoch=1;motion.valid=true;motion.sequence=7;motion.timestamp=19300000;motion.quaternion[3]=65536;
    assert(!nezha_sensor_hid_state(7,1,true,0));
    IOService clients[2];void *contexts[2]={};auto fusion=services[4];
    assert(fusion->openForClient(&clients[0],0,nullptr,&contexts[0]));
    assert(fusion->openForClient(&clients[1],0,nullptr,&contexts[1]));
    assert(contexts[0]!=contexts[1]);assert(!nezha_sensor_hid_motion(&motion));
    e=fusion->copyEventForClient(nullptr,0,contexts[0]);assert(e&&e->payload.size()==60);e->release();
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
