// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit offline keyboard/consumer input source. Uses the unchanged original
// HID parser and event dispatch; no user-client entitlement is substituted.
#include <IOKit/hid/IOHIDDevice.h>
#include <pexpert/pexpert.h>
static_assert(sizeof(IOHIDDevice)==208,"selected27 IOHIDDevice base layout");
// nezha: umi package/touch/NativeHidInput.cpp with the nezha producer (NezhaInput.c: Synaptics TCM touch frames in
// 1200 x 2608 panel pixels, PMK8850/TLMM key masks) and report 2 = Home, Power, Volume Increment, Volume Decrement (sheng bits).
#include "NezhaInput.h"
#include "NezhaHidPacing.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSIterator.h>
extern "C" void native_hid_input_observed(unsigned,int);
extern "C" void native_hid_live_checkpoint(IOService *);
extern "C" bool native_hid_input_requested(void);
// Optional touch digitizer fed by the board touch probe (kits/sheng/TouchProbe.cpp): finger set published with a seqlock.
extern "C" {
extern volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
extern volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
}
// Keyboard Application collection ensures the original generic keyboard
// personality can match. Report 2 carries Consumer Menu, not a claimed result
// that the system accepted Home. Report 1 is an ordinary boot keyboard report.
static const unsigned char descriptor[]={
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,
    0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x08,0x81,0x02,
    0x75,0x08,0x95,0x01,0x81,0x03,
    0x19,0x00,0x29,0x65,0x15,0x00,0x25,0x65,
    0x75,0x08,0x95,0x06,0x81,0x00,
    0x85,0x02,0x05,0x0c,0x09,0x40,0x09,0x30,0x09,0xe9,0x09,0xea,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x04,0x81,0x02,
    0x75,0x04,0x95,0x01,0x81,0x03,0xc0
};
// Report 3: ten-finger Touch Screen collection (Tip Switch, TouchValid/confidence, Contact Identifier, X, Y per finger, then Contact Count).
// Coordinates are normalised to 0..32767 on both axes by the producer.
#define FINGER_DESC \
    0x09,0x22,0xa1,0x02, 0x09,0x42,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x01,0x81,0x02, 0x09,0x47,0x81,0x02, 0x75,0x06,0x95,0x01,0x81,0x03, \
    0x09,0x51,0x75,0x08,0x95,0x01,0x15,0x00,0x26,0xff,0x00,0x81,0x02, \
    0x05,0x01,0x09,0x30,0x15,0x00,0x26,0xff,0x7f,0x75,0x10,0x95,0x01,0x81,0x02, 0x09,0x31,0x81,0x02, 0x05,0x0d, 0xc0,
static const unsigned char touchDescriptor[]={
    0x05,0x0d,0x09,0x04,0xa1,0x01,0x85,0x03,
    FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC
    FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC FINGER_DESC
    0x09,0x54,0x75,0x08,0x95,0x01,0x15,0x00,0x26,0xff,0x00,0x81,0x02,0xc0
};
#define TOUCH_FINGERS 10
//27 uses the original public HID dispatch. The17 unsigned-vptr edge patch is removed.

class NativeHidInput : public IOHIDDevice {
    OSDeclareDefaultStructors(NativeHidInput)
    IOWorkLoop *inputLoop=nullptr;
    IOTimerEventSource *timer=nullptr;
    IOTimerEventSource *touchTimer=nullptr;
    bool down=false;
    bool touchEnabled=false;
    // The digitizer is a separate service from the keyboard: backboardd matches digitizers by primary usage page/usage
    // (Digitizer/Touch Screen) and the Built-In flag, which a combined keyboard-first descriptor would not provide.
    bool touchOnly=false;
    bool eventDriverFound=false;
    struct nezha_hid_pacer touchState={};
    IOBufferMemoryDescriptor *touchBuffer=nullptr;
    bool touchFailed=false;
    unsigned touchLogged=0;
    uint32_t keyMask=0;            // report 2 bits of the physical keys currently down
    uint32_t sent=0;               // report 2 bits last accepted by handleReport
    unsigned homeHold=0;           // ticks left for a synthetic Home press
    static bool readTouch(void *,struct nezha_touch_frame *out) {return nezha_touch_next(out);}
    static void touchTick(OSObject *owner,IOTimerEventSource *source) {
        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return;
        if(self->touchFailed)return;
        source->setTimeoutMS(16);
        if(!self->hasEventDriverCached() || !self->touchBuffer)return;
        if(!nezha_hid_pick(&self->touchState,readTouch,nullptr))return;
        if(nezha_hid_same_contacts(&self->touchState.delivered,&self->touchState.pending)) {
            nezha_hid_commit(&self->touchState);return;
        }
        unsigned char report[62];unsigned count=touchReport(&self->touchState.delivered,&self->touchState.pending,report);
        if(!count){nezha_hid_commit(&self->touchState);return;}
        IOReturn result=kIOReturnNoMemory;
        if(self->touchBuffer->writeBytes(0,report,sizeof(report))==sizeof(report))
            result=self->handleReport(self->touchBuffer,kIOHIDReportTypeInput,0);
        native_hid_input_observed(6,result);native_touch_hid_reports=native_touch_hid_reports+1;
        if(self->touchLogged<12 || result!=kIOReturnSuccess) {
            ++self->touchLogged;
            printf("NEZHA HID: paced touch seq=%u active=%x records=%u result=%x\n",self->touchState.pending.sequence,
                   self->touchState.pending.active,count,result);
        }
        if(result!=kIOReturnSuccess) {
            self->touchFailed=true;source->cancelTimeout();
            printf("NEZHA HID: touch report stopped after error;pending preserved,no automatic replay\n");return;
        }
        nezha_hid_commit(&self->touchState);
        // handleReport synchronously reads this board buffer;DriverKit owns a
        // separate copied pool buffer (selected IOHIDInterface source). Do not
        // reuse that pool or infer its completion from this return code.
        source->setTimeoutMS(16);
    }
    // Report 3 from two contact sets: active contacts with tip, lifted ones once with tip clear; 0..32767 on both axes.
    static unsigned touchReport(const nezha_touch_frame *previous,const nezha_touch_frame *next,unsigned char report[62]) {
        memset(report,0,62);report[0]=3;unsigned count=0;
        for(unsigned id=0;id<TOUCH_FINGERS;++id) {
            bool now=next->active>>id&1,before=previous->active>>id&1;
            unsigned char *out=report+1+id*6;out[1]=(unsigned char)id;
            if(!now && !before)continue;
            unsigned px=now?next->x[id]:previous->x[id],py=now?next->y[id]:previous->y[id];
            unsigned x=px*32767u/(NEZHA_PANEL_W-1),y=py*32767u/(NEZHA_PANEL_H-1);
            if(x>32767)x=32767;if(y>32767)y=32767;
            out[0]=now?3:2;out[2]=(unsigned char)x;out[3]=(unsigned char)(x>>8);out[4]=(unsigned char)y;out[5]=(unsigned char)(y>>8);++count;
        }
        report[61]=(unsigned char)count;return count;
    }
    bool hasEventDriverCached() {
        if(!eventDriverFound) {
            IORegistryEntry *driver=eventDriver(this,4);
            eventDriverFound=driver!=nullptr;
            if(driver && (touchEnabled || touchOnly)){native_hid_live_checkpoint(this);}
        }
        return eventDriverFound;
    }
    static bool hasEventDriver(IORegistryEntry *parent,unsigned depth) {return eventDriver(parent,depth)!=nullptr;}
    // The IOHIDEventDriver below this device, if one has attached.
    static IORegistryEntry *eventDriver(IORegistryEntry *parent,unsigned depth) {
        if(!depth)return nullptr;
        OSIterator *it=parent->getChildIterator(gIOServicePlane);if(!it)return nullptr;
        IORegistryEntry *found=nullptr;
        while(OSObject *child=it->getNextObject()) {
            IORegistryEntry *entry=OSDynamicCast(IORegistryEntry,child);
            if(!entry)continue;
            found=entry->metaCast("IOHIDEventDriver")?entry:eventDriver(entry,depth-1);
            if(found)break;
        }
        it->release();return found;
    }
    // Keys (NezhaInput.c press/release masks) and the mailbox/auto Home press, as report 2, every 10 ms.
    static void tick(OSObject *owner,IOTimerEventSource *source) {
        NativeHidInput *self=OSDynamicCast(NativeHidInput,owner);if(!self)return;
        if(self->touchOnly){native_hid_live_checkpoint(self);source->setTimeoutMS(1000);return;}
        if(!self->hasEventDriverCached()) {
            native_hid_input_observed(3,0);native_hid_live_checkpoint(self);
            source->setTimeoutMS(500);return;
        }
        static unsigned beat;if((++beat&127)==0)native_hid_live_checkpoint(self);
        uint32_t keys=0;
        while(nezha_keys_next(&keys))self->keyMask=keys&0xf;     // physical keys: latest press/release mask
        if(self->homeHold)--self->homeHold;
        else if(native_hid_input_requested())self->homeHold=10;  // mailbox or offline-hid-auto-home: Home for 100 ms
        uint32_t want=self->keyMask|(self->homeHold?NEZHA_KEY_HOME:0);
        if(want!=self->sent) {
            unsigned char report[]={2,static_cast<unsigned char>(want&0xf)};
            IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
            IOReturn result=buffer?self->handleReport(buffer,kIOHIDReportTypeInput,0):kIOReturnNoMemory;
            OSSafeReleaseNULL(buffer);
            native_hid_input_observed(want?4:5,result);
            if(result==kIOReturnSuccess)self->sent=want;
        }
        native_hid_input_observed(3,1);
        source->setTimeoutMS(10);
    }
public:
    void setTouchOnly() {touchOnly=true;}
    IOWorkLoop *getWorkLoop() const override {return inputLoop;}
    OSString *newTransportString() const override {return OSString::withCString("SPI");}
    OSString *newProductString() const override {return OSString::withCString(touchOnly?"nezha Synaptics TCM touchscreen":"nezha Synaptics TCM HID");}
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        if(!out)return kIOReturnBadArgument;
        unsigned char combined[sizeof(descriptor)+sizeof(touchDescriptor)];
        size_t size;
        if(touchOnly){size=sizeof(touchDescriptor);memcpy(combined,touchDescriptor,size);}
        else{
            size=sizeof(descriptor);memcpy(combined,descriptor,size);
            // Default: one device, keyboard collection first, digitizer second (the arrangement whose event driver
            // and user client attach on hardware); the separate digitizer device is the opt-in sheng-touch-split.
            if(touchEnabled){memcpy(combined+size,touchDescriptor,sizeof(touchDescriptor));size+=sizeof(touchDescriptor);}
        }
        *out=IOBufferMemoryDescriptor::withBytes(combined,size,kIODirectionNone);
        return *out?kIOReturnSuccess:kIOReturnNoMemory;
    }
    bool start(IOService *provider) override {
        inputLoop=IOWorkLoop::workLoop();if(!inputLoop)return false;
        // Request the normal generic HID event personality for this owned
        // keyboard. This is a matching property, not a client entitlement.
        if(!setProperty("HIDDefaultBehavior",true))return false;
        unsigned touch=0;
        touchEnabled=PE_parse_boot_argn("sheng-touch-hid",&touch,sizeof(touch)) && touch==1;
        if(touchOnly||touchEnabled){setProperty("Built-In",true);setProperty("DisplayIntegrated",true);setProperty("DigitizerCollectionDispatch",true);}
        if(!IOHIDDevice::start(provider))return false;
        timer=IOTimerEventSource::timerEventSource(this,tick);
        if(!timer || inputLoop->addEventSource(timer)!=kIOReturnSuccess) {
            OSSafeReleaseNULL(timer);IOHIDDevice::stop(provider);return false;
        }
        if(touchOnly||touchEnabled) {
            unsigned char initial[62]={};touchBuffer=IOBufferMemoryDescriptor::withBytes(initial,sizeof(initial),kIODirectionNone);
            if(!touchBuffer){inputLoop->removeEventSource(timer);OSSafeReleaseNULL(timer);IOHIDDevice::stop(provider);return false;}
            touchTimer=IOTimerEventSource::timerEventSource(this,touchTick);
            if(touchTimer && inputLoop->addEventSource(touchTimer)==kIOReturnSuccess)touchTimer->setTimeoutMS(1000);
            else OSSafeReleaseNULL(touchTimer);
        }
        setProperty("OfflineInputProbe",true);
        native_hid_input_observed(2,0);
        timer->setTimeoutMS(1000);return true;
    }
    void stop(IOService *provider) override {
        if(touchTimer){touchTimer->cancelTimeout();inputLoop->removeEventSource(touchTimer);OSSafeReleaseNULL(touchTimer);}
        if(timer){timer->cancelTimeout();inputLoop->removeEventSource(timer);OSSafeReleaseNULL(timer);}
        OSSafeReleaseNULL(touchBuffer);IOHIDDevice::stop(provider);
    }
    void free() override {OSSafeReleaseNULL(touchBuffer);OSSafeReleaseNULL(inputLoop);IOHIDDevice::free();}
};
OSDefineMetaClassAndStructors(NativeHidInput,IOHIDDevice)
extern "C" IOService *native_hid_input_allocate(void) {return new NativeHidInput;}
extern "C" IOService *native_hid_touch_allocate(void) {NativeHidInput *device=new NativeHidInput;if(device)device->setTouchOnly();return device;}
