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
static uint64_t reportTime(const HostOrientationReport &report){
    assert(report.bytes.size()==NezhaOrientationInputReportBytes);
    uint64_t time=0;for(unsigned n=0;n<8;n++)time|=(uint64_t)report.bytes[n+2]<<(8*n);
    return time;
}
static void feed(uint64_t ticks,int32_t x,int32_t y,int32_t z){
    QsscSample s={};s.kind=QsscGravity;s.epoch=1;s.ssc_ticks=ticks;s.timestamp=ticks+42;
    s.value[0]=x;s.value[1]=y;s.value[2]=z;s.valid=true;s.status=3;
    assert(!orientation->submit(&s));
}
static void feature(uint32_t interval){
    uint8_t r[5]={2};for(unsigned n=0;n<4;n++)r[n+1]=(uint8_t)(interval>>(8*n));
    auto b=IOBufferMemoryDescriptor::withBytes(r,sizeof r,0);assert(!orientation->setReport(b,kIOHIDReportTypeFeature,2));b->release();
}
int main(int argc,char **argv){
    hostBaseSize=208;IOService provider;nezha_sensor_hid_ops ops={};ops.request=request;ops.fault=fault;
    hostParserAllocator=hostAllocateOrientationParser;hostOrientationInterfaceHook=hostInstallOrientationInterface;
    if(argc>1){
        std::string scenario=argv[1];
        if(scenario=="missing-interface")hostOrientationInterfaceHook=nullptr;
        else if(scenario=="duplicate-interface")hostOrientationInterfaceHook=[&provider](IOHIDDevice *d){
            d->children.push_back(&hostOrientationInterface);d->children.push_back(&provider);
        };
        else if(scenario=="missing-parser")hostParserAllocator=nullptr;
        else if(scenario=="parser-start-failed")hostParserStartFailure=true;
        else if(scenario=="parser-without-orientation")hostParserOrientationMissing=true;
        else if(scenario=="parser-without-service-identity")hostParserServiceIdentityMissing=true;
        else assert(false);
        assert(nezha_sensor_orientation_prepare(&provider,&ops,1));
        assert(!nezha_sensor_orientation_activate());
        assert(hostParserStarts<=1&&hostParserAllocations<=1);
        puts("Original orientation parser setup failure is explicit; no fake ready event");return 0;
    }
    assert(nezha_sensor_orientation_prepare(&provider,&ops,1));feature(30000);assert(requests==0);
    feature(20000);
    hostOrientationPublishHook=[](IOHIDDevice *device){
        assert(requested.active);uint8_t r[5]={2,0x20,0x4e,0,0};auto b=IOBufferMemoryDescriptor::withBytes(r,5,0);
        assert(!device->setReport(b,kIOHIDReportTypeFeature,2));b->release();
    };
    assert(nezha_sensor_orientation_activate());assert(nezha_sensor_orientation_activate());hostOrientationPublishHook=nullptr;
    assert(requests==2&&requested.active&&requested.interval_us==20000);
    assert(hostOrientationInterfaceCreates==1&&hostParserStarts==1&&hostParserAllocations==1);
    assert(hostOrientationInterface.children.empty());
    OSDictionary kernelMatch,dextMatch;SInt32 score=0;
    assert(orientation->matchPropertyTable(&kernelMatch,&score));
    dextMatch.setObject("IOUserClass",kOSBooleanTrue);assert(!orientation->matchPropertyTable(&dextMatch,&score));
    OSDictionary serverMatch;serverMatch.setObject("IOUserServerName",kOSBooleanTrue);assert(!orientation->matchPropertyTable(&serverMatch,&score));
    auto className=OSString::withCString("IOHIDEventDriver");kernelMatch.setObject("IOClass",className);className->release();
    assert(!orientation->matchPropertyTable(&kernelMatch,&score));
    IOMemoryDescriptor *descriptor=nullptr;assert(!orientation->newReportDescriptor(&descriptor));
    assert(descriptor->getLength()==sizeof nezha_orientation_descriptor);
    assert(descriptor->bytes[0]==0x05&&descriptor->bytes[1]==0x20&&
           descriptor->bytes[2]==0x09&&descriptor->bytes[3]==0x8a);
    descriptor->release();
    uint64_t t=10000000;constexpr int32_t g=642689;
    const int32_t vectors[][3]={{0,g,0},{0,-g,0},{g,0,0},{-g,0,0},{0,0,g},{0,0,-g}};
    const unsigned expected[]={1,2,3,4,5,6};
    for(unsigned n=0;n<6;n++){
        feed(t,vectors[n][0],vectors[n][1],vectors[n][2]);assert(hostOrientationReports.size()==n);
        feed(t+4800000,vectors[n][0],vectors[n][1],vectors[n][2]);
        assert(hostOrientationReports.size()==n+1&&hostOrientationReports.back().bytes[0]==1&&
               hostOrientationReports.back().bytes[1]==(uint8_t)(1u<<expected[n]));
        assert(hostOrientationReports.back().timestamp==t+4800000+42);t+=9600000;
        assert(reportTime(hostOrientationReports.back())==hostOrientationReports.back().timestamp);
    }
    auto count=hostOrientationReports.size();feed(t,g*7/10,g*7/10,0);feed(t+4800000,g*7/10,g*7/10,0);
    assert(hostOrientationReports.size()==count);t+=9600000;
    feed(t,g,0,0);feed(t+19200000,g,0,0);assert(hostOrientationReports.size()==count);
    feed(t+24000000,g,0,0);assert(hostOrientationReports.size()==count+1);t+=28800000;
    feature(0);assert(!requested.active);feed(t,0,g,0);feed(t+4800000,0,g,0);assert(hostOrientationReports.size()==count+1);
    feature(20000);assert(requested.active);feed(t+9600000,0,g,0);feed(t+14400000,0,g,0);assert(hostOrientationReports.size()==count+3);
    assert(hostOrientationReports[hostOrientationReports.size()-2].bytes[0]==1&&
           hostOrientationReports[hostOrientationReports.size()-2].bytes[1]==0);
    assert(reportTime(hostOrientationReports[hostOrientationReports.size()-2])==
           reportTime(hostOrientationReports.back()));
    assert(!orientation->setReady(1,false,0));feed(t+19200000,0,g,0);feed(t+24000000,0,g,0);assert(hostOrientationReports.size()==count+5);
    assert(hostOrientationReports[hostOrientationReports.size()-2].bytes[0]==1&&
           hostOrientationReports[hostOrientationReports.size()-2].bytes[1]==0);
    assert(reportTime(hostOrientationReports.back())==hostOrientationReports.back().timestamp);
    uint8_t input[NezhaOrientationInputReportBytes]={};auto current=IOBufferMemoryDescriptor::withBytes(input,sizeof input,0);
    assert(!orientation->getReport(current,kIOHIDReportTypeInput,1));
    assert(current->bytes==hostOrientationReports.back().bytes);current->release();
    uint8_t bytes[5]={};auto b=IOBufferMemoryDescriptor::withBytes(bytes,5,0);assert(!orientation->getReport(b,kIOHIDReportTypeFeature,2));
    assert(b->bytes==std::vector<uint8_t>({2,0x20,0x4e,0,0}));
    // The system report interval is independent of the physical source
    // rate. Preserve all values in the descriptor range and keep source demand in
    // the measured gravity range;0 remains the normal disable request.
    for(uint32_t value:{1u,1000u,1999u,2000u,5000u,50000u,1000000u}){
        feature(value);
        assert(requested.active&&requested.interval_us>=5000&&requested.interval_us<=50000);
        assert(!orientation->getReport(b,kIOHIDReportTypeFeature,2));
        uint32_t reported=0;for(unsigned n=0;n<4;n++)reported|=(uint32_t)b->bytes[n+1]<<(8*n);
        assert(reported==value);
    }
    b->bytes={2,0x41,0x42,0x0f,0};
    assert(orientation->setReport(b,kIOHIDReportTypeFeature,2)==kIOReturnBadArgument);
    feature(0);assert(!requested.active);feature(20000);assert(requested.active);
    b->release();
    QsscSample wrong={};wrong.epoch=2;wrong.kind=QsscGravity;wrong.valid=true;assert(nezha_sensor_orientation_sample(&wrong)==QsscConsumer);
    feature(1000000);assert(requested.interval_us==50000);feature(20000);
    nezha_sensor_orientation_stop();feed(t+30000000,0,g,0);assert(hostOrientationReports.size()==count+5);
    puts("Production orientation HID: all six directions, stable timing, ambiguous poses, stale epoch, interval0/restart and real report bytes passed");
}
