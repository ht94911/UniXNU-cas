// SPDX-License-Identifier: BSD-2-Clause-Patent
#define NEZHA_SENSORS_ORIENTATION_HOST 1
#include "../package/sensors/NezhaSensorOrientationHid.cpp"
#include <climits>
extern "C" {
kmod_info_t g_kernel_kmod_info={"kernel"};
void nezha_orientation_static_init(){}
}
static QsscDemand requested;
static unsigned requests;
static int32_t request(void *,uint32_t kind,uint32_t consumer,QsscDemand d){
    assert(kind==QsscGravity&&consumer==1);requested=d;requests++;return 0;
}
static void fault(void *,int){assert(false);}
static void feed(uint64_t ticks,int32_t x,int32_t y,int32_t z){
    QsscSample s={};s.kind=QsscGravity;s.epoch=1;s.ssc_ticks=ticks;s.timestamp=ticks+42;
    s.value[0]=x;s.value[1]=y;s.value[2]=z;s.valid=true;s.status=3;
    assert(!orientation->submit(&s));
}
static void feature(uint32_t interval){
    uint8_t r[5]={2};for(unsigned n=0;n<4;n++)r[n+1]=(uint8_t)(interval>>(8*n));
    auto b=IOBufferMemoryDescriptor::withBytes(r,sizeof r,0);assert(!orientation->setReport(b,kIOHIDReportTypeFeature,2));b->release();
}
int main(){
    hostBaseSize=208;IOService provider;nezha_sensor_hid_ops ops={};ops.request=request;ops.fault=fault;
    assert(nezha_sensor_orientation_prepare(&provider,&ops,1));feature(30000);assert(requests==0);
    feature(20000);
    hostOrientationPublishHook=[](IOHIDDevice *device){
        assert(requested.active);uint8_t r[5]={2,0x20,0x4e,0,0};auto b=IOBufferMemoryDescriptor::withBytes(r,5,0);
        assert(!device->setReport(b,kIOHIDReportTypeFeature,2));b->release();
    };
    assert(nezha_sensor_orientation_activate());assert(nezha_sensor_orientation_activate());hostOrientationPublishHook=nullptr;
    assert(requests==2&&requested.active&&requested.interval_us==20000);
    IOMemoryDescriptor *descriptor=nullptr;assert(!orientation->newReportDescriptor(&descriptor));
    assert(descriptor->getLength()==sizeof nezha_orientation_descriptor);descriptor->release();
    uint64_t t=10000000;constexpr int32_t g=642689;
    const int32_t vectors[][3]={{0,g,0},{0,-g,0},{g,0,0},{-g,0,0},{0,0,g},{0,0,-g}};
    const unsigned expected[]={1,2,3,4,5,6};
    for(unsigned n=0;n<6;n++){
        feed(t,vectors[n][0],vectors[n][1],vectors[n][2]);assert(hostOrientationReports.size()==n);
        feed(t+4800000,vectors[n][0],vectors[n][1],vectors[n][2]);
        assert(hostOrientationReports.size()==n+1&&hostOrientationReports.back().bytes==std::vector<uint8_t>({1,(uint8_t)(1u<<expected[n])}));
        assert(hostOrientationReports.back().timestamp==t+4800000+42);t+=9600000;
    }
    auto count=hostOrientationReports.size();feed(t,g*7/10,g*7/10,0);feed(t+4800000,g*7/10,g*7/10,0);
    assert(hostOrientationReports.size()==count);t+=9600000;
    feed(t,g,0,0);feed(t+19200000,g,0,0);assert(hostOrientationReports.size()==count);
    feed(t+24000000,g,0,0);assert(hostOrientationReports.size()==count+1);t+=28800000;
    feature(0);assert(!requested.active);feed(t,0,g,0);feed(t+4800000,0,g,0);assert(hostOrientationReports.size()==count+1);
    feature(20000);assert(requested.active);feed(t+9600000,0,g,0);feed(t+14400000,0,g,0);assert(hostOrientationReports.size()==count+3);
    assert(hostOrientationReports[hostOrientationReports.size()-2].bytes==std::vector<uint8_t>({1,0}));
    assert(!orientation->setReady(1,false,0));feed(t+19200000,0,g,0);feed(t+24000000,0,g,0);assert(hostOrientationReports.size()==count+5);
    assert(hostOrientationReports[hostOrientationReports.size()-2].bytes==std::vector<uint8_t>({1,0}));
    uint8_t bytes[5]={};auto b=IOBufferMemoryDescriptor::withBytes(bytes,5,0);assert(!orientation->getReport(b,kIOHIDReportTypeFeature,2));
    assert(b->bytes==std::vector<uint8_t>({2,0x20,0x4e,0,0}));
    b->bytes={2,1,0,0,0};assert(orientation->setReport(b,kIOHIDReportTypeFeature,2)==kIOReturnBadArgument);
    b->release();
    QsscSample wrong={};wrong.epoch=2;wrong.kind=QsscGravity;wrong.valid=true;assert(nezha_sensor_orientation_sample(&wrong)==QsscConsumer);
    feature(1000000);assert(requested.interval_us==50000);feature(20000);
    nezha_sensor_orientation_stop();feed(t+30000000,0,g,0);assert(hostOrientationReports.size()==count+5);
    puts("Production orientation HID: all six directions, stable timing, ambiguous poses, stale epoch, interval0/restart and real report bytes passed");
}
