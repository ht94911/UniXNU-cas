// SPDX-License-Identifier: BSD-2-Clause-Patent
// Qualify a real27 subclass before selecting a brightness transport. It is
// never published and never acknowledges a hardware brightness request.
#include "OriginalAppleARMBacklightABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
extern kmod_info_t g_kernel_kmod_info;
extern "C" {
volatile uint32_t nezha_backlight_stage,nezha_backlight_calls;
volatile uint32_t nezha_backlight_base_bytes,nezha_backlight_owned_bytes;
volatile int32_t nezha_backlight_meta_status=-1;
volatile uint32_t nezha_backlight_conforms,nezha_backlight_init_ok;
volatile uint64_t nezha_backlight_object;
__attribute__((noinline)) void nezha_backlight_checkpoint(void) {__asm__ volatile("" ::: "memory");}
}

class NativeNezhaBacklight : public AppleARMBacklight {
    OSDeclareDefaultStructors(NativeNezhaBacklight)
    IOWorkLoop *loop=nullptr;
public:
    bool init(OSDictionary *properties=nullptr) override {
        if(!IOService::init(properties))return false;
        loop=IOWorkLoop::workLoop();return loop!=nullptr;
    }
    void free() override {OSSafeReleaseNULL(loop);IOService::free();}
    bool start(IOService *) override {return false;} // hardware is not qualified
    IOWorkLoop *getWorkLoop() const override {return loop;}
    IOReturn setProperties(OSObject *) override {return kIOReturnNotReady;}
    IOReturn configureReport(IOReportChannelList *c,uint32_t a,void *b,void *d) override {
        return IOService::configureReport(c,a,b,d);
    }
    IOReturn updateReport(IOReportChannelList *c,uint32_t a,void *b,void *d) override {
        return IOService::updateReport(c,a,b,d);
    }
    IOReturn callPlatformFunction(const OSSymbol *name,bool wait,void *a,void *b,void *c,void *d) override {
        return IOService::callPlatformFunction(name,wait,a,b,c,d);
    }
    IOReturn message(uint32_t type,IOService *provider,void *arg) override {
        return IOService::message(type,provider,arg);
    }
    IOReturn setPowerState(unsigned long,IOService *) override {return kIOReturnNotReady;}
};
OSDefineMetaClassAndStructors(NativeNezhaBacklight,AppleARMBacklight)

extern "C" void nezha_backlight_static_init(void) __asm__("__GLOBAL__sub_I_BacklightProbe.cpp");
extern "C" void nezha_backlight_late_probe(void) {
    unsigned enabled=0;PE_parse_boot_argn("nezha-backlight-probe",&enabled,sizeof(enabled));
    if(enabled!=1 || nezha_backlight_calls)return;
    nezha_backlight_calls=nezha_backlight_calls+1;nezha_backlight_stage=1;
    const OSSymbol *name=OSSymbol::withCString("AppleARMBacklight");
    const OSMetaClass *base=name?OSMetaClass::getMetaClassWithName(name):nullptr;
    OSSafeReleaseNULL(name);
    if(!base || base->getClassSize()!=0x370){nezha_backlight_stage=2;nezha_backlight_checkpoint();return;}
    nezha_backlight_base_bytes=base->getClassSize();
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle){nezha_backlight_stage=3;nezha_backlight_checkpoint();return;}
    nezha_backlight_static_init();nezha_backlight_meta_status=OSMetaClass::postModLoad(handle);
    if(nezha_backlight_meta_status){nezha_backlight_stage=4;nezha_backlight_checkpoint();return;}
    NativeNezhaBacklight *object=new NativeNezhaBacklight;
    if(!object){nezha_backlight_stage=5;nezha_backlight_checkpoint();return;}
    nezha_backlight_object=(uintptr_t)object;
    nezha_backlight_owned_bytes=object->getMetaClass()->getClassSize();
    nezha_backlight_conforms=object->metaCast("AppleARMBacklight")!=nullptr && object->metaCast("IOService")!=nullptr;
    nezha_backlight_init_ok=object->init();
    nezha_backlight_stage=nezha_backlight_conforms && nezha_backlight_init_ok?6:7;
    printf("NEZHA BACKLIGHT ABI: stage=%u meta=%d bytes=%u/%u conforms=%u init=%u;unpublished,no hardware access\n",
        nezha_backlight_stage,nezha_backlight_meta_status,nezha_backlight_base_bytes,nezha_backlight_owned_bytes,
        nezha_backlight_conforms,nezha_backlight_init_ok);
    // Retain only in this opt-in diagnostic;the owned class is not attached,
    // started or registered. No brightness changes or conformance substitution.
    nezha_backlight_checkpoint();
}
