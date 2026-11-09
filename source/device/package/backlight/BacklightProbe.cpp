// SPDX-License-Identifier: BSD-2-Clause-Patent
// Original27 inheritance + stock nezha DCS transport. Publish only after an
// actual initial command completes and the inherited command state is restored.
#include "OriginalAppleARMBacklightABI.h"
#include "NezhaBrightnessRequest.h"
#include "NezhaDsi0.h"
#include "NezhaBrightnessProfile.h"
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSData.h>
#include <mach/kmod.h>
extern kmod_info_t g_kernel_kmod_info;
extern "C" volatile uint32_t native_source_storage_boot_state;
extern "C" volatile unsigned nezha27_usb_reset_owner_ready;
extern "C" {
volatile uint32_t nezha_backlight_stage,nezha_backlight_calls;
volatile uint32_t nezha_backlight_base_bytes,nezha_backlight_owned_bytes;
volatile int32_t nezha_backlight_meta_status=-1;
volatile uint32_t nezha_backlight_conforms,nezha_backlight_init_ok;
volatile uint64_t nezha_backlight_object;
volatile uint32_t nezha_backlight_requests,nezha_backlight_confirmed,nezha_backlight_level;
volatile int32_t nezha_backlight_transport_result;
__attribute__((noinline)) void nezha_backlight_checkpoint(void) {__asm__ volatile("" ::: "memory");}
}
static uint64_t nowUs() {uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000;}
class NativeNezhaBacklight : public AppleARMBacklight {
    OSDeclareDefaultStructors(NativeNezhaBacklight)
    IOWorkLoop *loop=nullptr;
    IOTimerEventSource *tick=nullptr;
    NezhaDsi0 dsi;
    struct nezha_brightness_queue commands={};
    struct nezha_brightness_profile profile={};
    uint64_t inbox=0,activeGeneration=0,waitDeadline=0,refusalSince=0,fadeStart=0,fadeDuration=0;
    uint32_t fadeFrom=0,fadeTo=0,latestRequest=0,activeRequest=0,traceCount=0;
    unsigned traceEnabled=0;
    bool published=false,unavailable=false,startedBackend=false,attachedBackend=false;
    uint32_t unsupportedTraceCount=0;
    void trace(const char *phase,uint32_t request,uint64_t generation,uint32_t value,int result,bool unsupported=false) {
        if(!traceEnabled)return;
        uint32_t *counter=unsupported?&unsupportedTraceCount:&traceCount;
        const uint32_t limit=unsupported?8:8192;
        uint32_t before=__atomic_load_n(counter,__ATOMIC_RELAXED);
        // Saturate rather than advancing after exhaustion: a long-running
        // service cannot wrap the counter and start logging again.
        for(;;) {
            if(before>=limit)return;
            if(__atomic_compare_exchange_n(counter,&before,before+1,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED))break;
        }
        printf("NEZHA BACKLIGHT TRACE: us=%llu phase=%s req=%u gen=%llu value=%u rc=%d\n",
            (unsigned long long)nowUs(),phase,request,(unsigned long long)generation,value,result);
    }
    bool loadProfile() {
        IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
        if(!chosen)return false;
        OSData *data=OSDynamicCast(OSData,chosen->getProperty("nezha,backlight-profile"));
        bool valid=data && nezha_brightness_profile_decode(data->getBytesNoCopy(),data->getLength(),&profile);
        chosen->release();return valid;
    }
    bool publishProfile() {
        // Actual24A446 CBBacklightNode getBacklightCalibrationParser expects
        // an IOFixed dictionary. copyMarketingTable reads an IOFixed OSData.
        // A missing curve reaches the original DisplayCreate fallback with a
        // null table pointer;publish both inputs before making this discoverable.
        static const char *keys[]={"lMaxProduct","lMidProduct","lMinProduct","lMaxPanel","lMidPanel","lMinPanel"};
        OSDictionary *calibration=OSDictionary::withCapacity(6);
        OSData *marketing=OSData::withBytes(profile.marketing,profile.count*sizeof(uint32_t));
        // BrightnessControl reads these independently of CoreBrightness's
        // lower-case calibration dictionary. LmaxProduct is four IOFixed
        // bytes; LmidProduct/LminProduct are numbers in whole nits (sheng).
        OSData *productMax=OSData::withBytes(&profile.calibration[0],sizeof(uint32_t));
        OSNumber *productMid=OSNumber::withNumber((unsigned long long)((profile.calibration[1]+32768)/65536),32);
        OSNumber *productMin=OSNumber::withNumber((unsigned long long)((profile.calibration[2]+32768)/65536),32);
        bool valid=calibration && marketing && productMax && productMid && productMin;
        for(unsigned i=0;i<6 && valid;++i) {
            OSNumber *value=OSNumber::withNumber((unsigned long long)profile.calibration[i],32);
            valid=value && calibration->setObject(keys[i],value);OSSafeReleaseNULL(value);
        }
        if(valid)valid=setProperty("backlight-calibration-parameters",calibration) && setProperty("backlight-marketing-table",marketing) &&
            setProperty("LmaxProduct",productMax) && setProperty("LmidProduct",productMid) && setProperty("LminProduct",productMin);
        OSSafeReleaseNULL(calibration);OSSafeReleaseNULL(marketing);
        OSSafeReleaseNULL(productMax);OSSafeReleaseNULL(productMid);OSSafeReleaseNULL(productMin);return valid;
    }
    void fault(int result) {
        __atomic_store_n(&unavailable,true,__ATOMIC_RELEASE);
        trace("fault",activeRequest,activeGeneration,commands.sending,result);
        nezha_backlight_transport_result=result;nezha_backlight_stage=20;
        if(activeGeneration)nezha_brightness_finish(&commands,activeGeneration,false);
        activeGeneration=0;commands.blocked=true;
        if(tick)tick->cancelTimeout();
        printf("NEZHA BACKLIGHT: stopped result=%d last-confirmed=%u;no retry or forced controller reset\n",result,commands.acknowledged);
    }
    bool publishLevel(uint32_t value) {
        OSDictionary *brightness=OSDictionary::withCapacity(3),*milliNits=OSDictionary::withCapacity(3);
        OSDictionary *parameters=OSDictionary::withCapacity(2);
        OSNumber *min=OSNumber::withNumber((unsigned long long)profile.minimum,32),*max=OSNumber::withNumber((unsigned long long)profile.maximum,32);
        OSNumber *level=OSNumber::withNumber((unsigned long long)value,32);
        OSNumber *minMilli=OSNumber::withNumber((unsigned long long)nezha_brightness_millinits(profile.calibration[2]),32);
        OSNumber *maxMilli=OSNumber::withNumber((unsigned long long)nezha_brightness_millinits(profile.calibration[0]),32);
        OSNumber *levelMilli=OSNumber::withNumber((unsigned long long)nezha_brightness_millinits(nezha_brightness_nits_for_level(&profile,value)),32);
        bool ok=brightness && milliNits && parameters && min && max && level && minMilli && maxMilli && levelMilli;
        if(ok)ok=brightness->setObject("min",min) && brightness->setObject("max",max) && brightness->setObject("value",level) &&
            milliNits->setObject("min",minMilli) && milliNits->setObject("max",maxMilli) && milliNits->setObject("value",levelMilli) &&
            parameters->setObject("brightness",brightness) && parameters->setObject("BrightnessMilliNits",milliNits) &&
            setProperty("IODisplayParameters",parameters);
        OSSafeReleaseNULL(min);OSSafeReleaseNULL(max);OSSafeReleaseNULL(level);
        OSSafeReleaseNULL(minMilli);OSSafeReleaseNULL(maxMilli);OSSafeReleaseNULL(levelMilli);
        OSSafeReleaseNULL(brightness);OSSafeReleaseNULL(milliNits);OSSafeReleaseNULL(parameters);return ok;
    }
    void service() {
        if(commands.blocked || !attachedBackend)return;
        const uint64_t now=nowUs();
        if(!startedBackend) {
            // SourceStorage state3/4 is reached only after its existing real
            // USB ACTIVE gate. Keep the controlled return path available first.
            const uint32_t usbRoot=native_source_storage_boot_state;
            if((usbRoot!=3 && usbRoot!=4) || !nezha27_usb_reset_owner_ready) {
                if(now>=waitDeadline){fault(-37);return;}
                tick->setTimeoutMS(20);return;
            }
            if(!dsi.openInherited()){fault(-38);return;}
            startedBackend=true;nezha_backlight_stage=10;
            // Stock boot init level0x394;below the normal-range cap,not HBM.
            if(!nezha_brightness_offer(&commands,0x394)){fault(-39);return;}
        }
        if(activeGeneration) {
            int result=sm_dsi_poll(&dsi.transfer,dsi.proof.lease_epoch,now);
            nezha_backlight_transport_result=result;
            if(result<0){fault(result);return;}
            if(result==SM_DSI_COMPLETE) {
                if(!nezha_brightness_finish(&commands,activeGeneration,true)){fault(-30);return;}
                // Controller done plus restored command state, not panel DCS
                // readback or a photometric confirmation.
                trace("done-restored",activeRequest,activeGeneration,commands.acknowledged,result);
                activeGeneration=0;
                nezha_backlight_confirmed=nezha_backlight_confirmed+1;
                nezha_backlight_level=commands.acknowledged;
                if(!publishLevel(commands.acknowledged)){fault(-31);return;}
                if(!published) {
                    if(!publishProfile()){fault(-40);return;}
                    // This is a real original AppleARMBacklight superclass.
                    // No replacement vptr or substituted conformance result.
                    if(!setProperty("backlight-control",true)){fault(-32);return;}
                    __atomic_store_n(&published,true,__ATOMIC_RELEASE);
                    registerService();nezha_backlight_stage=12;
                    printf("NEZHA BACKLIGHT: original27 service published after real DCS confirmation;level=%u max=%u\n",
                        commands.acknowledged,NEZHA_BRIGHTNESS_NORMAL_MAX);
                }
            }
        }
        uint64_t request=__atomic_exchange_n(&inbox,0,__ATOMIC_ACQ_REL);
        if(request) {
            latestRequest=(uint32_t)(request>>32)&0x7fffffff;
            fadeFrom=commands.acknowledged;fadeTo=(uint32_t)((request>>16)&0xffff);
            fadeStart=now;fadeDuration=(request&0xffff)*1000;
            trace("dequeued",latestRequest,0,fadeTo,0);
        }
        if(published && (request || fadeDuration)) {
            const uint64_t elapsed=now-fadeStart;
            uint32_t next=nezha_brightness_ramp(fadeFrom,fadeTo,elapsed,fadeDuration);
            if(!nezha_brightness_offer(&commands,next)){fault(-33);return;}
            if(!fadeDuration || elapsed>=fadeDuration)fadeDuration=0;
            if(!activeGeneration && !commands.pending && !fadeDuration)
                trace("already-confirmed",latestRequest,0,commands.acknowledged,0);
        }
        if(!activeGeneration && commands.pending) {
            uint32_t sending=0;activeGeneration=nezha_brightness_take(&commands,&sending);
            activeRequest=latestRequest;
            if(!activeGeneration){fault(-34);return;}
            uint8_t packet[8];
            if(!nezha_brightness_packet(sending,0,packet,sizeof(packet))){fault(-35);return;}
            int result=sm_dsi_begin(&dsi.transfer,&dsi.proof,packet,now);
            if(result==SM_DSI_REFUSED) {
                // Refusal happens before any write. Preserve the target and
                // wait at most200ms for an idle window;uncertain sends stop.
                if(!refusalSince)trace("busy-before-write",activeRequest,activeGeneration,sending,result);
                commands.inflight=0;commands.pending=true;activeGeneration=0;
                if(!refusalSince)refusalSince=now;
                if(now-refusalSince>=200000){fault(-36);return;}
                tick->setTimeoutMS(1);return;
            }
            if(result!=SM_DSI_SENT){fault(result);return;}
            trace("sent",activeRequest,activeGeneration,sending,result);
            refusalSince=0;nezha_backlight_transport_result=result;
            nezha_backlight_stage=published?12:11;
        }
        if(activeGeneration || commands.pending)tick->setTimeoutUS(100);
        else if(fadeDuration)tick->setTimeoutMS(16);
    }
    static void fired(OSObject *owner,IOTimerEventSource *) {
        auto self=static_cast<NativeNezhaBacklight *>(owner);if(self)self->service();
    }
public:
    bool init(OSDictionary *properties=nullptr) override {
        if(!IOService::init(properties))return false;
        if(!loadProfile())return false;
        PE_parse_boot_argn("nezha-backlight-log",&traceEnabled,sizeof(traceEnabled));
        loop=IOWorkLoop::workLoop();return loop!=nullptr;
    }
    void free() override {
        if(tick){tick->cancelTimeout();if(loop)loop->removeEventSource(tick);OSSafeReleaseNULL(tick);}
        dsi.close();OSSafeReleaseNULL(loop);IOService::free();
    }
    bool start(IOService *provider) override {
        if(!provider || !IOService::start(provider) || !loop)return false;
        tick=IOTimerEventSource::timerEventSource(this,&NativeNezhaBacklight::fired);
        if(!tick || loop->addEventSource(tick)!=kIOReturnSuccess)return false;
        attachedBackend=true;waitDeadline=nowUs()+90000000;
        nezha_backlight_stage=9;tick->setTimeoutMS(1);return true;
    }
    IOWorkLoop *getWorkLoop() const override {return loop;}
    IOReturn setProperties(OSObject *properties) override {
        if(!__atomic_load_n(&published,__ATOMIC_ACQUIRE) || __atomic_load_n(&unavailable,__ATOMIC_ACQUIRE))return kIOReturnNotReady;
        auto dictionary=OSDynamicCast(OSDictionary,properties);if(!dictionary)return kIOReturnBadArgument;
        OSObject *nitsObject=dictionary->getObject("brightness-nits");
        uint32_t goal=0,input=0;uint64_t ms=0;
        if(nitsObject) {
            auto nits=OSDynamicCast(OSNumber,nitsObject);
            if(!nits){trace("bad-nits-type",0,0,0,kIOReturnBadArgument);return kIOReturnBadArgument;}
            const uint64_t raw=nits->unsigned64BitValue();
            // Allow a32-bit bit pattern or an exact sign extension, never
            // silently truncate a positive64-bit value into an off request.
            if(raw>UINT32_MAX && raw<UINT64_C(0xffffffff80000000)) {
                trace("bad-nits-width",0,0,0,kIOReturnBadArgument);return kIOReturnBadArgument;
            }
            input=nits->unsigned32BitValue();
            goal=nezha_brightness_level_for_nits(&profile,(int32_t)input);
            // 27 ramps nits in user space; FadePeriod belongs to level input.
        } else {
            auto brightness=OSDynamicCast(OSNumber,dictionary->getObject("brightness"));
            if(!brightness){trace("unsupported",0,0,0,kIOReturnUnsupported,true);return kIOReturnUnsupported;}
            if(brightness->unsigned64BitValue()>UINT32_MAX)return kIOReturnBadArgument;
            OSObject *periodObject=dictionary->getObject("FadePeriod");
            auto period=OSDynamicCast(OSNumber,periodObject);
            if((periodObject && !period) || (period && period->unsigned64BitValue()>UINT32_MAX))return kIOReturnBadArgument;
            input=brightness->unsigned32BitValue();goal=nezha_brightness_normal_goal((int32_t)input);
            ms=period?((uint64_t)period->unsigned32BitValue()*1000)>>16:0;
            if(ms>5000)ms=5000;
        }
        const uint32_t request=(__atomic_add_fetch(&nezha_backlight_requests,1,__ATOMIC_RELAXED)&0x7fffffff);
        // Reuse sheng's asynchronous mailbox:do not block backboardd on our
        // workloop gate. Properties change after controller completion and
        // restore. Request ID occupies unused mailbox bits for USB correlation.
        __atomic_store_n(&inbox,(UINT64_C(1)<<63)|((uint64_t)request<<32)|((uint64_t)goal<<16)|ms,__ATOMIC_RELEASE);
        trace(nitsObject?"accepted-nits16.16":"accepted-dbv",request,0,input,kIOReturnSuccess);
        tick->setTimeoutUS(1);return kIOReturnSuccess;
    }
    IOReturn configureReport(IOReportChannelList *c,uint32_t a,void *b,void *d) override {return IOService::configureReport(c,a,b,d);}
    IOReturn updateReport(IOReportChannelList *c,uint32_t a,void *b,void *d) override {return IOService::updateReport(c,a,b,d);}
    IOReturn callPlatformFunction(const OSSymbol *name,bool wait,void *a,void *b,void *c,void *d) override {return IOService::callPlatformFunction(name,wait,a,b,c,d);}
    IOReturn message(uint32_t type,IOService *provider,void *arg) override {return IOService::message(type,provider,arg);}
    IOReturn setPowerState(unsigned long state,IOService *) override {
        // This callback is a requested PM state, not evidence of panel power.
        trace("power-request",0,0,(uint32_t)state,kIOReturnNotReady);return kIOReturnNotReady;
    }
};
OSDefineMetaClassAndStructors(NativeNezhaBacklight,AppleARMBacklight)
extern "C" void nezha_backlight_static_init(void) __asm__("__GLOBAL__sub_I_BacklightProbe.cpp");
extern "C" void nezha_backlight_late_probe(void) {
    unsigned diagnostic=0,enabled=0;
    PE_parse_boot_argn("nezha-backlight-probe",&diagnostic,sizeof(diagnostic));
    PE_parse_boot_argn("nezha-backlight",&enabled,sizeof(enabled));
    if((enabled!=1 && diagnostic!=1) || nezha_backlight_calls)return;
    nezha_backlight_calls=nezha_backlight_calls+1;nezha_backlight_stage=1;
    const OSSymbol *name=OSSymbol::withCString("AppleARMBacklight");
    const OSMetaClass *base=name?OSMetaClass::getMetaClassWithName(name):nullptr;OSSafeReleaseNULL(name);
    if(!base || base->getClassSize()!=0x370){nezha_backlight_stage=2;nezha_backlight_checkpoint();return;}
    nezha_backlight_base_bytes=base->getClassSize();void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle){nezha_backlight_stage=3;nezha_backlight_checkpoint();return;}
    nezha_backlight_static_init();nezha_backlight_meta_status=OSMetaClass::postModLoad(handle);
    if(nezha_backlight_meta_status){nezha_backlight_stage=4;nezha_backlight_checkpoint();return;}
    NativeNezhaBacklight *object=new NativeNezhaBacklight;
    if(!object){nezha_backlight_stage=5;nezha_backlight_checkpoint();return;}
    nezha_backlight_object=(uintptr_t)object;nezha_backlight_owned_bytes=object->getMetaClass()->getClassSize();
    nezha_backlight_conforms=object->metaCast("AppleARMBacklight")!=nullptr && object->metaCast("IOService")!=nullptr;
    nezha_backlight_init_ok=object->init();nezha_backlight_stage=nezha_backlight_conforms && nezha_backlight_init_ok?6:7;
    printf("NEZHA BACKLIGHT ABI: stage=%u meta=%d bytes=%u/%u conforms=%u init=%u\n",nezha_backlight_stage,
        nezha_backlight_meta_status,nezha_backlight_base_bytes,nezha_backlight_owned_bytes,nezha_backlight_conforms,nezha_backlight_init_ok);
    if(enabled==1 && nezha_backlight_stage==6) {
        IOService *provider=IOService::getResourceService();
        if(!provider || !object->attach(provider) || !object->start(provider))nezha_backlight_stage=8;
    }
    nezha_backlight_checkpoint();
}
