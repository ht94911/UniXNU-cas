// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifdef NEZHA_SENSORS_ORIENTATION_HOST
#include "sensors-orientation-host-shim.h"
#else
#include <IOKit/hid/IOHIDDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <mach/kmod.h>
#endif
#include "NezhaSensors.h"
#include "NezhaSensorOrientation.h"
#include "NezhaSensorOrientationDescriptor.h"
#include <string.h>
extern "C" int printf(const char *,...);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void nezha_orientation_static_init(void) __asm__("__GLOBAL__sub_I_NezhaSensorOrientationHid.cpp");
#ifndef NEZHA_SENSORS_ORIENTATION_HOST
static_assert(sizeof(IOHIDDevice)==208,"selected27 original HID device");
#endif
class NativeNezhaOrientation : public IOHIDDevice {
    OSDeclareDefaultStructors(NativeNezhaOrientation)
    IOWorkLoop *loop=nullptr;
    IOCommandGate *gate=nullptr;
    IOLock *settings=nullptr;
    IOService *kernelParser=nullptr;
    struct nezha_sensor_hid_ops backend={};
    struct nezha_orientation_filter filter={};
    uint32_t interval=20000,direction=0;
    uint64_t reportedTimestamp=0;
    bool ready=false,stopped=false,clearBeforeReport=false,activated=false,published=false;
    bool attachKernelParser(){
        if(kernelParser)return true;
        OSIterator *children=getChildIterator(gIOServicePlane);if(!children)return false;
        IOService *interface=nullptr;
        while(OSObject *child=children->getNextObject()){
            auto entry=OSDynamicCast(IOService,child);
            if(!entry||!entry->metaCast("IOHIDInterface"))continue;
            if(interface){children->release();return false;}
            interface=entry;
        }
        children->release();if(!interface)return false;
        auto name=OSSymbol::withCStringNoCopy("IOHIDEventDriver");if(!name)return false;
        const OSMetaClass *meta=OSMetaClass::getMetaClassWithName(name);name->release();if(!meta)return false;
        OSObject *object=meta->alloc();auto parser=OSDynamicCast(IOService,object);
        if(!parser||!parser->metaCast("IOHIDEventDriver")||!parser->init()){
            if(object)object->release();return false;
        }
        if(!parser->attach(interface)){parser->release();return false;}
        if(!parser->start(interface)){
            interface->close(parser);parser->detach(interface);parser->release();return false;
        }
        auto mask=OSDynamicCast(OSNumber,parser->getProperty("SupportedHIDEventMask"));
        uint64_t supported=mask?mask->unsigned64BitValue():0;
        auto page=OSDynamicCast(OSNumber,parser->getProperty("PrimaryUsagePage"));
        auto usage=OSDynamicCast(OSNumber,parser->getProperty("PrimaryUsage"));
        if(!(supported&(UINT64_C(1)<<10))||!parser->getProperty("DeviceOrientation")||
           !page||!usage||page->unsigned64BitValue()!=NezhaOrientationUsagePage||
           usage->unsigned64BitValue()!=NezhaOrientationUsage){
            interface->close(parser);parser->stop(interface);parser->detach(interface);parser->release();return false;
        }
        // Retain the real original parser for this boot, like the sensor
        // EventServices. Its own report callback and copyEvent own delivery.
        kernelParser=parser;
        printf("NEZHA SENSORS ORIENTATION: kernel-parser bytes=%u mask=%llu attached=1 started=1 page=%u usage=%u\n",
            meta->getClassSize(),(unsigned long long)supported,NezhaOrientationUsagePage,NezhaOrientationUsage);
        return true;
    }
    int32_t demand(uint32_t value){
        // Preserve every interval allowed by the report descriptor. The discrete
        // classifier uses the measured200Hz gravity source at most, and
        // needs at least20Hz to evaluate250ms direction stability.
        uint32_t source=value?(value<5000?5000:value>50000?50000:value):0;
        QsscDemand d={source,0,value!=0,false};
        return backend.request(backend.context,QsscGravity,1,d);
    }
    static IOReturn control(OSObject *object,void *value,void *,void *,void *){
        auto self=static_cast<NativeNezhaOrientation *>(object);uint32_t next=*static_cast<uint32_t *>(value);
        if(self->stopped)return kIOReturnNotReady;
        // Initial feature probes can run during original IOHIDDevice::start,
        // before the shared board adapter has enabled its command queue.
        int32_t result=next>1000000?kIOReturnBadArgument:self->activated?self->demand(next):0;
        printf("NEZHA SENSORS ORIENTATION: interval=%u source=%u rc=%d\n",next,
            next?(next<5000?5000:next>50000?50000:next):0,result);
        if(result)return result;
        IOLockLock(self->settings);self->interval=next;IOLockUnlock(self->settings);
        if(!next){self->clearBeforeReport|=self->ready;self->ready=false;nezha_orientation_invalidate(&self->filter);}
        return kIOReturnSuccess;
    }
    static IOReturn activate(OSObject *object,void *,void *,void *,void *){
        auto self=static_cast<NativeNezhaOrientation *>(object);if(self->stopped)return kIOReturnNotReady;
        if(self->activated)return kIOReturnSuccess;
        int32_t result=self->demand(self->interval);if(result)return result;
        self->activated=true;return kIOReturnSuccess;
    }
    static IOReturn receive(OSObject *object,void *value,void *,void *,void *){
        auto self=static_cast<NativeNezhaOrientation *>(object);auto sample=static_cast<QsscSample *>(value);
        if(self->stopped)return kIOReturnSuccess;
        IOLockLock(self->settings);uint32_t interval=self->interval;IOLockUnlock(self->settings);
        if(!interval)return kIOReturnSuccess;
        uint32_t direction;int result=nezha_orientation_sample(&self->filter,sample,&direction);
        if(result==QsscNoData)return kIOReturnSuccess;
        if(result)return kIOReturnBadArgument;
        // The original parser emits a CMUsage event on a0->1 bit edge.
        // Clear retained input state first when resuming at the same pose.
        if(self->clearBeforeReport){
            uint8_t clear[NezhaOrientationInputReportBytes];nezha_orientation_report(clear,0,sample->timestamp);
            auto buffer=IOBufferMemoryDescriptor::withBytes(clear,sizeof clear,kIODirectionNone);
            IOReturn status=buffer?self->handleReportWithTime(sample->timestamp,buffer,kIOHIDReportTypeInput,0):kIOReturnNoMemory;
            if(buffer)buffer->release();if(status!=kIOReturnSuccess){nezha_orientation_invalidate(&self->filter);return status;}
            self->clearBeforeReport=false;
        }
        uint8_t report[NezhaOrientationInputReportBytes];nezha_orientation_report(report,(uint8_t)(1u<<direction),sample->timestamp);
        auto buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof report,kIODirectionNone);
        IOReturn status=buffer?self->handleReportWithTime(sample->timestamp,buffer,kIOHIDReportTypeInput,0):kIOReturnNoMemory;
        if(buffer)buffer->release();
        if(status==kIOReturnSuccess){self->direction=direction;self->reportedTimestamp=sample->timestamp;self->ready=true;}
        else{self->ready=false;nezha_orientation_invalidate(&self->filter);}
        printf("NEZHA SENSORS ORIENTATION: direction=%u ssc=%llu host=%llu rc=%d\n",direction,
            (unsigned long long)sample->ssc_ticks,(unsigned long long)sample->timestamp,status);
        return status;
    }
    struct State {uint64_t epoch;bool ready;int error;};
    static IOReturn state(OSObject *object,void *value,void *,void *,void *){
        auto self=static_cast<NativeNezhaOrientation *>(object);auto next=static_cast<State *>(value);
        if(next->epoch!=self->filter.epoch)return kIOReturnNotReady;
        if(!next->ready){self->clearBeforeReport|=self->ready;self->ready=false;nezha_orientation_invalidate(&self->filter);}
        return kIOReturnSuccess;
    }
    static IOReturn rebindAction(OSObject *object,void *value,void *,void *,void *){
        auto self=static_cast<NativeNezhaOrientation *>(object);uint64_t epoch=*static_cast<uint64_t *>(value);
        if(self->stopped||epoch<=self->filter.epoch)return kIOReturnNotReady;
        self->clearBeforeReport|=self->ready;self->ready=false;self->direction=0;self->reportedTimestamp=0;
        return nezha_orientation_begin(&self->filter,epoch,19200000)?kIOReturnBadArgument:kIOReturnSuccess;
    }
    static IOReturn shutdownAction(OSObject *object,void *,void *,void *,void *){
        auto self=static_cast<NativeNezhaOrientation *>(object);self->stopped=true;self->ready=false;
        nezha_orientation_invalidate(&self->filter);return kIOReturnSuccess;
    }
    struct Read {IOHIDReportType type;uint32_t id;uint8_t bytes[NezhaOrientationInputReportBytes];size_t count;};
    static IOReturn readReport(OSObject *object,void *value,void *,void *,void *){
        auto self=static_cast<NativeNezhaOrientation *>(object);auto r=static_cast<Read *>(value);
        if(self->stopped)return kIOReturnNotReady;
        if(r->type==kIOHIDReportTypeFeature&&r->id==2){
            r->bytes[0]=2;IOLockLock(self->settings);uint32_t interval=self->interval;IOLockUnlock(self->settings);
            for(unsigned n=0;n<4;n++)r->bytes[n+1]=(uint8_t)(interval>>(8*n));r->count=5;
        }else if(r->type==kIOHIDReportTypeInput&&r->id==1){
            nezha_orientation_report(r->bytes,self->ready?(uint8_t)(1u<<self->direction):0,
                                     self->ready?self->reportedTimestamp:0);r->count=NezhaOrientationInputReportBytes;
        }else return kIOReturnUnsupported;
        return kIOReturnSuccess;
    }
protected:
    OSString *newTransportString() const override{return OSString::withCString("Virtual");}
    OSString *newProductString() const override{return OSString::withCString("nezha SEE Device Orientation");}
public:
    bool matchPropertyTable(OSDictionary *table,SInt32 *score) override{
        // Sensor009 selected AppleUserHIDEventDriver, whose measured embedded
        // parser did not publish orientation or its supported event mask.
        // Keep this owned report device on the original kernel parser.
        if(table&&(table->getObject("IOUserClass")||table->getObject("IOUserServerName")))return false;
        auto name=table?OSDynamicCast(OSString,table->getObject("IOClass")):nullptr;
        if(name&&name->isEqualTo("IOHIDEventDriver"))return false;
        return IOHIDDevice::matchPropertyTable(table,score);
    }
    bool init(OSDictionary *properties=nullptr) override{
        if(!IOHIDDevice::init(properties))return false;
        loop=IOWorkLoop::workLoop();settings=IOLockAlloc();if(!loop||!settings)return false;
        gate=IOCommandGate::commandGate(this);return gate&&loop->addEventSource(gate)==kIOReturnSuccess;
    }
    bool configure(const struct nezha_sensor_hid_ops *ops,uint64_t epoch){
        if(!ops||!epoch)return false;backend=*ops;
        return !nezha_orientation_begin(&filter,epoch,19200000)&&
            setProperty("Built-In",kOSBooleanTrue)&&setProperty("HIDDefaultBehavior",kOSBooleanTrue)&&
            setProperty("RegisterService",kOSBooleanFalse)&&setProperty("ReportInterval",interval,32)&&
            setProperty("NezhaSensorEpoch",epoch,64);
    }
    IOWorkLoop *getWorkLoop() const override{return loop;}
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override{
        if(!out)return kIOReturnBadArgument;
        *out=IOBufferMemoryDescriptor::withBytes(nezha_orientation_descriptor,sizeof nezha_orientation_descriptor,kIODirectionNone);
        return *out?kIOReturnSuccess:kIOReturnNoMemory;
    }
    IOReturn getReport(IOMemoryDescriptor *report,IOHIDReportType type,IOOptionBits options) override{
        if(!report)return kIOReturnBadArgument;
        Read r={type,options&255,{0},0};IOReturn status=gate->runAction(&NativeNezhaOrientation::readReport,&r);
        if(status)return status;if(report->getLength()<r.count)return kIOReturnBadArgument;
        return report->writeBytes(0,r.bytes,r.count)==r.count?kIOReturnSuccess:kIOReturnBadArgument;
    }
    IOReturn setReport(IOMemoryDescriptor *report,IOHIDReportType type,IOOptionBits options) override{
        if(!report||type!=kIOHIDReportTypeFeature||(options&255)!=2||report->getLength()!=5)return kIOReturnUnsupported;
        uint8_t bytes[5];if(report->readBytes(0,bytes,sizeof bytes)!=sizeof bytes||bytes[0]!=2)return kIOReturnBadArgument;
        uint32_t value=0;for(unsigned n=0;n<4;n++)value|=(uint32_t)bytes[n+1]<<(8*n);
        return gate->runAction(&NativeNezhaOrientation::control,&value);
    }
    bool beginSampling(){return gate->runAction(&NativeNezhaOrientation::activate)==kIOReturnSuccess;}
    bool publish(IOService *provider){
        if(published)return true;
        // With this source unregistered, original start prepares report
        // storage without relying on a catalogue notification to create the
        // interface. Explicitly attach/start the original kernel parser once.
        if(!IOHIDDevice::start(provider))return false;
        published=IOHIDDevice::createInterface(0)&&attachKernelParser();
        return published;
    }
    int submit(const QsscSample *sample){
        return gate->runAction(&NativeNezhaOrientation::receive,const_cast<QsscSample *>(sample))==kIOReturnSuccess?0:QsscConsumer;
    }
    int setReady(uint64_t epoch,bool value,int error){
        State next={epoch,value,error};return gate->runAction(&NativeNezhaOrientation::state,&next)==kIOReturnSuccess?0:QsscConsumer;
    }
    bool rebind(uint64_t epoch){return gate->runAction(&NativeNezhaOrientation::rebindAction,&epoch)==kIOReturnSuccess;}
    void shutdown(){if(gate)gate->runAction(&NativeNezhaOrientation::shutdownAction);}
    void stop(IOService *provider) override{
        int result=activated?demand(0):0;if(result)backend.fault(backend.context,QsscConsumer);
        shutdown();IOHIDDevice::stop(provider);
    }
    void free() override{
        if(gate){if(loop)loop->removeEventSource(gate);gate->release();}
        if(loop)loop->release();if(settings)IOLockFree(settings);IOHIDDevice::free();
    }
};
OSDefineMetaClassAndStructors(NativeNezhaOrientation,IOHIDDevice)
static NativeNezhaOrientation *orientation;
static void *orientation_provider;
extern "C" bool nezha_sensor_orientation_prepare(void *provider,const struct nezha_sensor_hid_ops *ops,uint64_t epoch){
    if(!provider||!ops||!ops->request||!ops->fault||orientation)return false;
    auto name=OSSymbol::withCStringNoCopy("IOHIDDevice");if(!name)return false;
    const OSMetaClass *base=OSMetaClass::getMetaClassWithName(name);name->release();
    if(!base||base->getClassSize()!=208)return false;
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);if(!handle)return false;
    nezha_orientation_static_init();if(OSMetaClass::postModLoad(handle))return false;
    auto service=new NativeNezhaOrientation;if(!service)return false;
    if(!service->init()||!service->configure(ops,epoch)||!service->attach(static_cast<IOService *>(provider))){service->release();return false;}
    orientation=service;orientation_provider=provider;return true;
}
extern "C" bool nezha_sensor_orientation_activate(void){
    return orientation&&orientation->beginSampling()&&orientation->publish(static_cast<IOService *>(orientation_provider));
}
extern "C" int nezha_sensor_orientation_sample(const QsscSample *s){return orientation?orientation->submit(s):QsscArgument;}
extern "C" int nezha_sensor_orientation_state(uint64_t epoch,bool ready,int error){return orientation?orientation->setReady(epoch,ready,error):QsscArgument;}
extern "C" void nezha_sensor_orientation_stop(void){if(orientation)orientation->shutdown();}
extern "C" bool nezha_sensor_orientation_rebind(uint64_t epoch){return orientation&&orientation->rebind(epoch);}
