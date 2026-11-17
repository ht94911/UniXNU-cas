// SPDX-License-Identifier: BSD-2-Clause-Patent
// Original24A446 IOHIDEventService ABI, reusing ALS017 class/lifetime pattern.
#ifdef NEZHA_SENSORS_HID_HOST
#include "sensors-hid-host-shim.h"
#else
#include <IOKit/hid/IOHIDEventService.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSData.h>
#include <mach/kmod.h>
#endif
#include "NezhaSensors.h"
extern "C" int printf(const char *,...);

// Original constructors own event memory. All arguments are integer ABI;
// local code never allocates a private IOHIDEvent layout or uses kernel FP.
extern "C" IOHIDEvent *nezha_sensor_original_accel(uint64_t,int32_t,int32_t,int32_t,uint32_t,uint32_t,uint32_t,uint32_t)
    __asm__("__ZN10IOHIDEvent18accelerometerEventEyiiijjjj");
extern "C" IOHIDEvent *nezha_sensor_original_gyro(uint64_t,int32_t,int32_t,int32_t,uint32_t,uint32_t,uint32_t,uint32_t)
    __asm__("__ZN10IOHIDEvent9gyroEventEyiiijjjj");
extern "C" IOHIDEvent *nezha_sensor_original_prox(uint64_t,uint32_t,uint32_t,uint32_t)
    __asm__("__ZN10IOHIDEvent14proximityEventEyjjj");
extern "C" IOHIDEvent *nezha_sensor_original_vendor(uint64_t,uint32_t,uint32_t,uint32_t,uint8_t *,uint32_t,uint32_t)
    __asm__("__ZN10IOHIDEvent18vendorDefinedEventEyjjjPhjj");
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void nezha_sensor_static_init(void) __asm__("__GLOBAL__sub_I_NezhaSensorHid.cpp");
#ifndef NEZHA_SENSORS_HID_HOST
static_assert(sizeof(IOService)==0x88,"selected27 IOService");
static_assert(sizeof(IOHIDEventService)==0xc8,"actual24A446 EventService");
#endif
namespace {
const uint32_t usage[QSSC_OUTPUT_COUNT]={3,9,0x70,5};
const uint32_t eventType[QSSC_OUTPUT_COUNT]={13,20,1,14};
const char *const product[QSSC_OUTPUT_COUNT]={"nezha LSM6DSV Accelerometer","nezha LSM6DSV Gyroscope",
    "nezha SSC Gravity","nezha SSC Proximity"};
struct GravityData {uint32_t version,bytes;uint64_t timestamp,sequence;int32_t gravity[3],linear[3];uint32_t status,reserved;};
IOHIDEvent *eventFor(const QsscSample &s){
    if(!s.valid)return nullptr;
    if(s.kind==QsscProximityKind){
        // Receiver mask is the optical face/ear proximity source. raw_adc
        // remains diagnostic, not a claimed calibrated centimetre distance.
        return nezha_sensor_original_prox(s.timestamp,s.near?64u:0u,s.near?1u:0u,0);
    }
    if(s.kind==QsscGravity){
        GravityData data={1,sizeof(GravityData),s.timestamp,s.sequence,
            {s.value[0],s.value[1],s.value[2]},{s.value[3],s.value[4],s.value[5]},s.status,0};
        // Explicit local diagnostic usage; the normal CoreMotion fusion path
        // consumes accel/gyro. This packet does not impersonate Apple SPU.
        return nezha_sensor_original_vendor(s.timestamp,0xff00,0x70,1,
            reinterpret_cast<uint8_t *>(&data),sizeof data,0);
    }
    int32_t v[3];
    for(unsigned i=0;i<3;i++){
        int64_t si=(int64_t)s.value[i]*65536;
        // SEE uses upward specific acceleration; Apple acceleration is g
        // with -Z for a face-up device. Gyro remains rad/s and the board axes
        // already come from SEE's stock -Y,+X,+Z mounting transform.
        if(s.kind==QsscAccel)si=-si;
        if(qssc_scaled_q16(si,s.kind==QsscAccel?100000u:1u,
                          s.kind==QsscAccel?980665u:1u,&v[i]))return nullptr;
    }
    if(s.kind==QsscAccel)return nezha_sensor_original_accel(s.timestamp,v[0],v[1],v[2],0,0,(uint32_t)s.sequence,0);
    if(s.kind==QsscGyro)return nezha_sensor_original_gyro(s.timestamp,v[0],v[1],v[2],0,0,(uint32_t)s.sequence,0);
    return nullptr;
}
void releaseEvent(IOHIDEvent *e){if(e)reinterpret_cast<OSObject *>(e)->release();}
}

class NativeNezhaSensor : public IOHIDEventService {
    OSDeclareDefaultStructors(NativeNezhaSensor)
    IOWorkLoop *loop=nullptr;
    IOCommandGate *gate=nullptr;
    IOTimerEventSource *timer=nullptr;
    IOLock *clientLock=nullptr;
    IOService *clients[QSSC_CONSUMERS]={};
    uint32_t clientCount=0;
    struct FastClient { IOService *client=nullptr;uint32_t consumer=0,interval=20000,batch=0;uint64_t sequence=0;bool active=false; };
    FastClient fast[QSSC_CONSUMERS-2];
    QsscMotionFrame motion={};
    struct nezha_sensor_hid_ops backend={};
    QsscSample latest={},queue[NEZHA_SENSOR_EVENTS]={};
    uint64_t epoch=0,dispatched=0,lastSequence=0,flushSerial=0;
    uint32_t kind=0,interval=20000,batch=0,head=0,count=0,detectionMode=1;
    bool ready=false,stopped=false,registered=false,wakeup=false,priming=true,copyLogged=false;
    int error=0;
    int32_t requestDemand(uint32_t consumer,QsscDemand d){
        return kind==NEZHA_SENSOR_FUSION_KIND?backend.motion(backend.context,consumer,d):backend.request(backend.context,kind,consumer,d);
    }
    struct Control {uint32_t interval,batch,mode;bool hasInterval,hasBatch,hasMode;FastClient *client=nullptr;};
    static IOReturn control(OSObject *object,void *value,void *,void *,void *){
        auto self=static_cast<NativeNezhaSensor *>(object);auto arg=static_cast<Control *>(value);
        if(self->stopped)return kIOReturnNotReady;
        IOLockLock(self->clientLock);
        if(arg->client&&!arg->client->active){IOLockUnlock(self->clientLock);return kIOReturnNotReady;}
        uint32_t currentInterval=arg->client?arg->client->interval:self->interval;
        uint32_t currentBatch=arg->client?arg->client->batch:self->batch;
        uint32_t nextInterval=arg->hasInterval?arg->interval:currentInterval;
        uint32_t nextBatch=arg->hasBatch?arg->batch:currentBatch;
        uint32_t nextMode=arg->hasMode?arg->mode:self->detectionMode;
        QsscDemand demand={nextInterval,nextBatch,nextInterval!=0&&(arg->client||self->clientCount||self->priming)&&
            (self->kind!=QsscProximityKind||nextMode!=0),self->wakeup};
        uint32_t consumer=self->kind==NEZHA_SENSOR_FUSION_KIND?1:0;
        if(arg->client)consumer=arg->client->consumer;
        int32_t r=self->requestDemand(consumer,demand);
        if(!r){if(arg->client){arg->client->interval=nextInterval;arg->client->batch=nextBatch;}
            else{self->interval=nextInterval;self->batch=nextBatch;self->detectionMode=nextMode;}}
        IOLockUnlock(self->clientLock);
        printf("NEZHA SENSORS HID: control kind=%u consumer=%u interval=%u batch=%u mode=%u rc=%d\n",
            self->kind,consumer,nextInterval,nextBatch,nextMode,r);
        if(r)return (IOReturn)r;
        // Actual readiness belongs to the aggregated SSC subscription. A
        // consumer's interval0 must not invalidate another active FastPath.
        if(arg->client)return kIOReturnSuccess;
        if(!self->setProperty("ReportInterval",self->interval,32)||!self->setProperty("BatchInterval",self->batch,32)||
           (self->kind==QsscProximityKind&&!self->setProperty("DetectionMode",self->detectionMode,32)))return kIOReturnNoMemory;
        return kIOReturnSuccess;
    }
    static IOReturn accept(OSObject *object,void *value,void *,void *,void *){
        auto self=static_cast<NativeNezhaSensor *>(object);auto s=static_cast<QsscSample *>(value);
        if(self->stopped||!self->ready||!s->valid||s->epoch!=self->epoch||s->kind!=self->kind||s->sequence<=self->lastSequence)
            return kIOReturnNotReady;
        if(self->count==NEZHA_SENSOR_EVENTS){self->ready=false;self->latest.valid=false;self->error=QsscCapacity;self->backend.fault(self->backend.context,self->error);return kIOReturnNoResources;}
        self->lastSequence=s->sequence;self->latest=*s;
        self->queue[(self->head+self->count)%NEZHA_SENSOR_EVENTS]=*s;self->count++;
        self->timer->setTimeoutUS(1);return kIOReturnSuccess;
    }
    struct State {uint64_t epoch;bool ready;int error;};
    static IOReturn state(OSObject *object,void *value,void *,void *,void *){
        auto self=static_cast<NativeNezhaSensor *>(object);auto s=static_cast<State *>(value);
        if(s->epoch!=self->epoch)return kIOReturnNotReady;
        if(self->stopped)return s->ready?kIOReturnNotReady:kIOReturnSuccess;
        if(self->ready!=s->ready||self->error!=s->error)
            printf("NEZHA SENSORS HID: state kind=%u ready=%u error=%d epoch=%llu\n",
                self->kind,s->ready,s->error,(unsigned long long)s->epoch);
        self->ready=s->ready;self->error=s->error;
        if(!s->ready){self->latest.valid=false;self->motion.valid=false;self->count=0;self->timer->cancelTimeout();}
        return kIOReturnSuccess;
    }
    IOHIDEvent *motionEvent(){
        uint8_t payload[60];if(!ready||stopped||!motion.valid||qssc_motion_payload(&motion,payload,sizeof payload))return nullptr;
        return nezha_sensor_original_vendor(motion.timestamp,0xff0c,5,1,payload,sizeof payload,0);
    }
    void finishPrime(){
        IOLockLock(clientLock);
        if(priming){priming=false;if(!clientCount){QsscDemand off={interval,batch,false,wakeup};
            if(requestDemand(kind==NEZHA_SENSOR_FUSION_KIND?1:0,off)){error=QsscConsumer;backend.fault(backend.context,error);}}}
        IOLockUnlock(clientLock);
    }
    static IOReturn acceptMotion(OSObject *object,void *value,void *,void *,void *){
        auto self=static_cast<NativeNezhaSensor *>(object);auto f=static_cast<QsscMotionFrame *>(value);
        if(self->kind!=NEZHA_SENSOR_FUSION_KIND||self->stopped||!self->ready||!f->valid||f->epoch!=self->epoch)return kIOReturnNotReady;
        self->motion=*f;
        IOHIDEvent *event=self->motionEvent();if(!event)return kIOReturnNoMemory;
        if(!self->registered){self->registered=true;self->registerService();self->finishPrime();
            printf("NEZHA SENSORS HID: registered kind=%u page=%u usage=%u seq=%llu clients=%u\n",
                self->kind,0xff0cu,5u,(unsigned long long)f->sequence,self->clientCount);}
        self->dispatchEvent(event);releaseEvent(event);self->dispatched++;return kIOReturnSuccess;
    }
    void drain(){
        for(unsigned n=0;n<NEZHA_SENSOR_EVENTS&&count;n++){
            QsscSample s=queue[head];head=(head+1)%NEZHA_SENSOR_EVENTS;count--;
            if(stopped||!ready||!s.valid)continue;
            IOHIDEvent *e=eventFor(s);if(!e){error=QsscConsumer;ready=false;latest.valid=false;count=0;backend.fault(backend.context,error);return;}
            if(!registered){registered=true;registerService();finishPrime();
                printf("NEZHA SENSORS HID: registered kind=%u page=%u usage=%u seq=%llu clients=%u\n",
                    kind,0xff00u,usage[kind],(unsigned long long)s.sequence,clientCount);}
            dispatchEvent(e);releaseEvent(e);dispatched++;
        }
    }
    static void fired(OSObject *object,IOTimerEventSource *){static_cast<NativeNezhaSensor *>(object)->drain();}
    static IOReturn flushDone(OSObject *object,void *serial,void *,void *,void *){
        auto self=static_cast<NativeNezhaSensor *>(object);if(self->stopped)return kIOReturnNotReady;
        self->drain();self->flushSerial=*static_cast<uint64_t *>(serial);
        return self->setProperty("NezhaSensorFlushSerial",self->flushSerial,64)?kIOReturnSuccess:kIOReturnNoMemory;
    }
    static IOReturn copy(OSObject *object,void *value,void *,void *,void *){
        auto self=static_cast<NativeNezhaSensor *>(object);auto out=static_cast<IOHIDEvent **>(value);
        *out=nullptr;if(self->stopped||!self->ready)return kIOReturnNotReady;
        *out=self->kind==NEZHA_SENSOR_FUSION_KIND?self->motionEvent():self->latest.valid?eventFor(self->latest):nullptr;
        if(*out&&!self->copyLogged){self->copyLogged=true;
            printf("NEZHA SENSORS HID: first-copy kind=%u seq=%llu\n",self->kind,
                (unsigned long long)(self->kind==NEZHA_SENSOR_FUSION_KIND?self->motion.sequence:self->latest.sequence));}
        return *out?kIOReturnSuccess:kIOReturnNoMemory;
    }
    struct FastCopy {FastClient *client;IOHIDEvent *event;};
    static IOReturn copyFast(OSObject *object,void *value,void *,void *,void *){
        auto self=static_cast<NativeNezhaSensor *>(object);auto copy=static_cast<FastCopy *>(value);copy->event=nullptr;
        IOLockLock(self->clientLock);
        bool active=copy->client->active&&copy->client->interval&&self->motion.sequence>copy->client->sequence;
        if(active){copy->event=self->motionEvent();if(copy->event){
            if(!copy->client->sequence)printf("NEZHA SENSORS HID: first-fast-copy kind=%u consumer=%u seq=%llu\n",
                self->kind,copy->client->consumer,(unsigned long long)self->motion.sequence);
            copy->client->sequence=self->motion.sequence;}}
        IOLockUnlock(self->clientLock);return copy->event?kIOReturnSuccess:kIOReturnNotReady;
    }
    static IOReturn shutdownAction(OSObject *object,void *,void *,void *,void *){
        auto self=static_cast<NativeNezhaSensor *>(object);
        IOLockLock(self->clientLock);self->stopped=true;IOLockUnlock(self->clientLock);
        self->ready=false;self->count=0;
        self->latest.valid=false;self->motion.valid=false;self->timer->cancelTimeout();return kIOReturnSuccess;
    }
    static IOReturn rebindAction(OSObject *object,void *value,void *,void *,void *){
        auto self=static_cast<NativeNezhaSensor *>(object);uint64_t next=*static_cast<uint64_t *>(value);
        if(self->stopped||next<=self->epoch)return kIOReturnNotReady;
        self->epoch=next;self->ready=false;self->latest.valid=false;self->motion.valid=false;
        self->count=0;self->lastSequence=0;self->timer->cancelTimeout();
        IOLockLock(self->clientLock);for(auto &f:self->fast)f.sequence=0;IOLockUnlock(self->clientLock);
        return kIOReturnSuccess;
    }
protected:
    UInt32 getPrimaryUsagePage() override{return kind==NEZHA_SENSOR_FUSION_KIND?0xff0c:0xff00;}
    UInt32 getPrimaryUsage() override{return kind==NEZHA_SENSOR_FUSION_KIND?5:usage[kind];}
    UInt32 getReportInterval() override{
        IOLockLock(clientLock);uint32_t value=interval;IOLockUnlock(clientLock);return value;
    }
    UInt32 getLocationID() override{return 0;}
    bool handleStart(IOService *provider) override{
        return IOHIDEventService::handleStart(provider)&&setProperty("Built-In",kOSBooleanTrue)&&
            setProperty("Product",kind==NEZHA_SENSOR_FUSION_KIND?"nezha SSC Device Motion":product[kind])&&setProperty("BatchInterval",batch,32)&&
            (kind!=QsscProximityKind||setProperty("DetectionMode",detectionMode,32))&&
            (kind!=NEZHA_SENSOR_FUSION_KIND||setProperty("SupportsIOFastPath",kOSBooleanTrue));
    }
    bool handleOpen(IOService *client,IOOptionBits options,void *argument) override{
        // IOService holds the arbitration lock. Only the short client/command
        // locks are taken here; no HID gate, owner worker or remote ACK wait.
        IOLockLock(clientLock);
        if(stopped){IOLockUnlock(clientLock);return false;}
        unsigned index=0;while(index<QSSC_CONSUMERS&&clients[index]!=client)index++;
        bool existing=index<QSSC_CONSUMERS;
        if(!existing){index=0;while(index<QSSC_CONSUMERS&&clients[index])index++;}
        IOLockUnlock(clientLock);
        if(index==QSSC_CONSUMERS||!IOHIDEventService::handleOpen(client,options,argument))return false;
        if(existing)return true;
        IOLockLock(clientLock);
        QsscDemand d={interval,batch,interval!=0&&(kind!=QsscProximityKind||detectionMode!=0),wakeup};
        int32_t r=requestDemand(kind==NEZHA_SENSOR_FUSION_KIND?1:0,d);
        if(!r){clients[index]=client;clientCount++;priming=false;}
        printf("NEZHA SENSORS HID: open kind=%u interval=%u clients=%u rc=%d\n",kind,interval,clientCount,r);
        IOLockUnlock(clientLock);
        if(r){IOHIDEventService::handleClose(client,options);return false;}
        return true;
    }
    void handleClose(IOService *client,IOOptionBits options) override{
        IOHIDEventService::handleClose(client,options);
        IOLockLock(clientLock);
        for(unsigned n=0;n<QSSC_CONSUMERS;n++)if(clients[n]==client){clients[n]=nullptr;clientCount--;break;}
        if(!clientCount&&!priming){QsscDemand d={interval,batch,false,wakeup};
            int32_t r=requestDemand(kind==NEZHA_SENSOR_FUSION_KIND?1:0,d);if(r){error=QsscConsumer;backend.fault(backend.context,error);}}
        printf("NEZHA SENSORS HID: close kind=%u clients=%u\n",kind,clientCount);
        IOLockUnlock(clientLock);
    }
public:
    bool init(OSDictionary *properties=nullptr) override{
        if(!IOHIDEventService::init(properties))return false;
        loop=IOWorkLoop::workLoop();clientLock=IOLockAlloc();if(!loop||!clientLock)return false;
        gate=IOCommandGate::commandGate(this);timer=IOTimerEventSource::timerEventSource(this,&NativeNezhaSensor::fired);
        return gate&&timer&&loop->addEventSource(gate)==kIOReturnSuccess&&loop->addEventSource(timer)==kIOReturnSuccess&&
            setProperty("RegisterService",kOSBooleanFalse);
    }
    bool configure(unsigned k,const struct nezha_sensor_hid_ops *ops,uint64_t e){
        if((k>=QSSC_OUTPUT_COUNT&&k!=NEZHA_SENSOR_FUSION_KIND)||!ops||!e)return false;kind=k;backend=*ops;epoch=e;wakeup=k==QsscProximityKind;
        interval=k==QsscProximityKind?1:20000;
        return setProperty("RegisterService",kOSBooleanFalse)&&
            setProperty("NezhaSensorKind",kind,32)&&setProperty("NezhaSensorEpoch",epoch,64);
    }
    IOWorkLoop *getWorkLoop() const override{return loop;}
    bool beginSampling(){QsscDemand d={interval,batch,true,wakeup};
        return requestDemand(kind==NEZHA_SENSOR_FUSION_KIND?1:0,d)==0;}
    int submit(const QsscSample *s){return gate->runAction(&NativeNezhaSensor::accept,const_cast<QsscSample *>(s))==kIOReturnSuccess?0:QsscConsumer;}
    int submitMotion(const QsscMotionFrame *f){return gate->runAction(&NativeNezhaSensor::acceptMotion,const_cast<QsscMotionFrame *>(f))==kIOReturnSuccess?0:QsscConsumer;}
    bool rebind(uint64_t e){return gate->runAction(&NativeNezhaSensor::rebindAction,&e)==kIOReturnSuccess;}
    int setReady(uint64_t e,bool r,int err){State s={e,r,err};return gate->runAction(&NativeNezhaSensor::state,&s)==kIOReturnSuccess?0:QsscConsumer;}
    int flushed(uint64_t e,uint64_t serial){if(e!=epoch)return QsscIdentity;return gate->runAction(&NativeNezhaSensor::flushDone,&serial)==kIOReturnSuccess?0:QsscConsumer;}
    IOHIDEvent *copyEvent(IOHIDEventType type,IOHIDEvent *,IOOptionBits) override{
        if(type!=(kind==NEZHA_SENSOR_FUSION_KIND?1:eventType[kind]))return nullptr;IOHIDEvent *e=nullptr;gate->runAction(&NativeNezhaSensor::copy,&e);return e;
    }
    bool openForClient(IOService *client,IOOptionBits,OSDictionary *,void **context) override{
        if(kind!=NEZHA_SENSOR_FUSION_KIND||!context||!client)return false;
        IOLockLock(clientLock);FastClient *slot=nullptr;
        if(stopped){IOLockUnlock(clientLock);return false;}
        for(auto &f:fast)if(f.active&&f.client==client){*context=&f;IOLockUnlock(clientLock);return true;}
        for(auto &f:fast)if(!f.active){slot=&f;break;}
        if(!slot){IOLockUnlock(clientLock);return false;}
        unsigned n=(unsigned)(slot-fast);QsscDemand d={interval,batch,true,wakeup};
        int32_t r=requestDemand(n+2,d);
        if(!r){slot->client=client;slot->consumer=n+2;slot->interval=interval;slot->batch=batch;
            slot->sequence=0;slot->active=true;*context=slot;}
        printf("NEZHA SENSORS HID: fast-open kind=%u consumer=%u interval=%u rc=%d\n",kind,n+2,interval,r);
        IOLockUnlock(clientLock);return !r;
    }
    void closeForClient(IOService *client,void *context,IOOptionBits) override{
        IOLockLock(clientLock);
        for(auto &f:fast)if(&f==context&&f.active&&f.client==client){
            QsscDemand off={f.interval,f.batch,false,wakeup};int32_t r=requestDemand(f.consumer,off);
            if(r){error=QsscConsumer;backend.fault(backend.context,error);}
            printf("NEZHA SENSORS HID: fast-close kind=%u consumer=%u rc=%d\n",kind,f.consumer,r);
            f.active=false;f.client=nullptr;break;}
        IOLockUnlock(clientLock);
    }
    IOHIDEvent *copyEventForClient(OSObject *,IOOptionBits,void *context) override{
        FastClient *slot=nullptr;for(auto &f:fast)if(&f==context)slot=&f;
        if(!slot)return nullptr;FastCopy copy={slot,nullptr};gate->runAction(&NativeNezhaSensor::copyFast,&copy);return copy.event;
    }
    OSObject *copyPropertyForClient(const char *key,void *context) const override{
        bool found=false;IOLockLock(clientLock);for(auto &f:fast)if(&f==context&&f.active)found=true;IOLockUnlock(clientLock);
        if(!found||!key)return nullptr;
        OSObject *value=getProperty(key);if(value)value->retain();return value;
    }
    IOReturn setPropertiesForClient(OSObject *properties,void *context) override{
        FastClient *slot=nullptr;for(auto &f:fast)if(&f==context)slot=&f;if(!slot)return kIOReturnBadArgument;
        auto d=OSDynamicCast(OSDictionary,properties);if(!d)return kIOReturnBadArgument;
        OSObject *i=d->getObject("ReportInterval"),*b=d->getObject("BatchInterval");if(!i&&!b)return kIOReturnUnsupported;
        Control c={};c.client=slot;
        if(i){auto n=OSDynamicCast(OSNumber,i);if(!n||n->unsigned64BitValue()>UINT32_MAX)return kIOReturnBadArgument;c.hasInterval=true;c.interval=n->unsigned32BitValue();}
        if(b){auto n=OSDynamicCast(OSNumber,b);if(!n||n->unsigned64BitValue()>1000000)return kIOReturnBadArgument;c.hasBatch=true;c.batch=n->unsigned32BitValue();}
        return gate->runAction(&NativeNezhaSensor::control,&c);
    }
    IOReturn setProperties(OSObject *properties) override{
        auto d=OSDynamicCast(OSDictionary,properties);if(!d)return kIOReturnBadArgument;
        OSObject *i=d->getObject("ReportInterval"),*b=d->getObject("BatchInterval");
        OSObject *m=kind==QsscProximityKind?d->getObject("DetectionMode"):nullptr;
        if(d->getObject("ReceiverProximityReset"))return kIOReturnUnsupported;
        if(!i&&!b&&!m)return kIOReturnUnsupported;Control c={};
        if(i){auto n=OSDynamicCast(OSNumber,i);if(!n||n->unsigned64BitValue()>UINT32_MAX)return kIOReturnBadArgument;c.hasInterval=true;c.interval=n->unsigned32BitValue();}
        if(b){auto n=OSDynamicCast(OSNumber,b);if(!n||n->unsigned64BitValue()>1000000)return kIOReturnBadArgument;c.hasBatch=true;c.batch=n->unsigned32BitValue();}
        if(m){auto n=OSDynamicCast(OSNumber,m);if(!n||n->unsigned64BitValue()>5)return kIOReturnBadArgument;c.hasMode=true;c.mode=n->unsigned32BitValue();}
        return gate->runAction(&NativeNezhaSensor::control,&c);
    }
    void shutdown(){if(gate)gate->runAction(&NativeNezhaSensor::shutdownAction);}
    void stop(IOService *provider) override{
        IOLockLock(clientLock);
        QsscDemand d={interval,batch,false,wakeup};int32_t r=requestDemand(kind==NEZHA_SENSOR_FUSION_KIND?1:0,d);
        if(kind==NEZHA_SENSOR_FUSION_KIND)for(auto &f:fast)if(f.active){
            int32_t result=requestDemand(f.consumer,d);if(result)r=result;f.active=false;}
        IOLockUnlock(clientLock);
        if(r){error=QsscConsumer;backend.fault(backend.context,error);}shutdown();IOHIDEventService::stop(provider);
    }
    void free() override{
        if(timer){timer->cancelTimeout();if(loop)loop->removeEventSource(timer);timer->release();}
        if(gate){if(loop)loop->removeEventSource(gate);gate->release();}
        if(loop)loop->release();if(clientLock)IOLockFree(clientLock);IOHIDEventService::free();
    }
};
OSDefineMetaClassAndStructors(NativeNezhaSensor,IOHIDEventService)
static NativeNezhaSensor *services[QSSC_OUTPUT_COUNT+1];
static bool attempted;
extern "C" bool nezha_sensor_hid_prepare(void *provider,const struct nezha_sensor_hid_ops *ops,uint64_t epoch){
    if(!provider||!ops||!ops->request||!ops->flush||!ops->motion||!ops->fault||!epoch||attempted)return false;attempted=true;
    auto name=OSSymbol::withCStringNoCopy("IOHIDEventService");if(!name)return false;
    const OSMetaClass *base=OSMetaClass::getMetaClassWithName(name);name->release();if(!base||base->getClassSize()!=0xc8)return false;
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);if(!handle)return false;
    nezha_sensor_static_init();if(OSMetaClass::postModLoad(handle))return false;
    for(unsigned index=0;index<QSSC_OUTPUT_COUNT+1;index++){
        unsigned k=index==QSSC_OUTPUT_COUNT?NEZHA_SENSOR_FUSION_KIND:index;
        auto s=new NativeNezhaSensor;if(!s){nezha_sensor_hid_stop();return false;}
        if(!s->init()||!s->configure(k,ops,epoch)||!s->attach(static_cast<IOService *>(provider))){s->release();nezha_sensor_hid_stop();return false;}
        if(!s->start(static_cast<IOService *>(provider))){s->detach(static_cast<IOService *>(provider));s->release();nezha_sensor_hid_stop();return false;}
        services[index]=s;
    }
    return true;
}
extern "C" int nezha_sensor_hid_sample(const QsscSample *s){return s&&s->kind<QSSC_OUTPUT_COUNT&&services[s->kind]?services[s->kind]->submit(s):QsscArgument;}
extern "C" int nezha_sensor_hid_motion(const QsscMotionFrame *f){return f&&services[QSSC_OUTPUT_COUNT]?services[QSSC_OUTPUT_COUNT]->submitMotion(f):QsscArgument;}
extern "C" int nezha_sensor_hid_state(uint32_t kind,uint64_t epoch,bool ready,int error){
    unsigned index=kind==NEZHA_SENSOR_FUSION_KIND?QSSC_OUTPUT_COUNT:kind;
    return index<QSSC_OUTPUT_COUNT+1&&services[index]?services[index]->setReady(epoch,ready,error):QsscArgument;
}
extern "C" int nezha_sensor_hid_flushed(uint32_t kind,uint64_t epoch,uint64_t serial){return kind<QSSC_OUTPUT_COUNT&&services[kind]?services[kind]->flushed(epoch,serial):QsscArgument;}
extern "C" void nezha_sensor_hid_stop(void){for(auto s:services)if(s)s->shutdown();}
extern "C" bool nezha_sensor_hid_activate(void){
    for(auto s:services)if(!s||!s->beginSampling())return false;
    return true;
}
extern "C" bool nezha_sensor_hid_rebind(uint64_t epoch){for(auto s:services)if(!s||!s->rebind(epoch))return false;return true;}
