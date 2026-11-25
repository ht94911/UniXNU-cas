/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <CoreMedia/CoreMedia.h>
#include <objc/runtime.h>
#include <ptrauth.h>

static bool still_observers_installed;
static intptr_t capture_slide;
static IMP original_still_capture,original_still_initiate,original_still_prepare,original_still_complete;
uint64_t mach_absolute_time(void);
#include "../userland/NezhaCameraStillObservers.h"

static unsigned capture_calls,initiate_calls,prepare_calls,complete_calls,replacements;
static unsigned unavailable,wrong_type,wrong_address;
struct TestMethod { IMP implementation;const char *encoding; };
static struct TestMethod methods[4];
static const char *const classes[]={"BWFigVideoCaptureDevice","BWStillImageCoordinatorNode","BWMultiStreamCameraSourceNode","BWFigVideoCaptureDevice"};
static const char *const selectors[]={"captureStillImageNowWithSettings:captureSettings:assetBundle:","initiateStillImageCaptureNowWithSettings:","prepareForStillImageCaptureWithFirmwareStillImageOutputRetainedBufferCountOverride:clientBracketCount:enableSushiRawAttachmentOption:","captureStream:didCompleteStillImageCaptureWithPTS:error:"};
static const char *const encodings[]={"i40@0:8@16@24@32","i24@0:8@16","i28@0:8i16i20B24","v52@0:8@16{?=qiIq}24i48"};
static const uintptr_t addresses[]={0x1b089ae30ULL,0x1b079e290ULL,0x1b06b0128ULL,0x1b0898638ULL};
Class objc_getClass(const char *name)
{for(unsigned i=0;i<3;++i)if(!strcmp(name,classes[i]))return (Class)(uintptr_t)(i+1);assert(!"unexpected class");return NULL;}
SEL sel_registerName(const char *name)
{for(unsigned i=0;i<4;++i)if(!strcmp(name,selectors[i]))return (SEL)(uintptr_t)(i+1);assert(!"unexpected selector");return NULL;}
Method class_getInstanceMethod(Class type,SEL selector)
{unsigned i=(unsigned)(uintptr_t)selector-1;assert(i<4 && type==(Class)(uintptr_t)(i==3?1:i+1));return unavailable==i+1 ? NULL:&methods[i];}
IMP method_getImplementation(Method method){return method->implementation;}
const char *method_getTypeEncoding(Method method)
{return wrong_type==(unsigned)(method-methods)+1 ? "v16@0:8":method->encoding;}
IMP method_setImplementation(Method method,IMP replacement)
{IMP old=method->implementation;method->implementation=replacement;++replacements;return old;}
uint64_t mach_absolute_time(void){return 12345;}

static int32_t capture_fixture(id self,SEL selector,id settings,id capture,id asset)
{assert(self==(id)101 && selector==(SEL)1 && settings==(id)103 && capture==(id)104 && asset==(id)105);++capture_calls;return -12783;}
static int32_t initiate_fixture(id self,SEL selector,id settings)
{assert(self==(id)101 && selector==(SEL)2 && settings==(id)103);++initiate_calls;return -9;}
static int32_t prepare_fixture(id self,SEL selector,int32_t retained,int32_t brackets,bool sushi)
{assert(self==(id)101 && selector==(SEL)3 && retained==-1 && brackets==3 && sushi);++prepare_calls;return -12780;}
static void complete_fixture(id self,SEL selector,id stream,CMTime time,int32_t status)
{assert(self==(id)101 && selector==(SEL)4 && stream==(id)103 && time.value==-17 && time.timescale==600 && time.flags==3 && time.epoch==9 && status==-7);++complete_calls;}
static void reset_methods(void)
{for(unsigned i=0;i<4;++i){methods[i].implementation=(IMP)(uintptr_t)(addresses[i]+capture_slide+(wrong_address==i+1?4:0));methods[i].encoding=encodings[i];}}

int main(void)
{
    capture_slide=0x234000;
    for(unsigned i=1;i<=4;++i){unavailable=i;reset_methods();install_still_observers();assert(!still_observers_installed && !replacements);}
    unavailable=0;
    for(unsigned i=1;i<=4;++i){wrong_type=i;reset_methods();install_still_observers();assert(!still_observers_installed && !replacements);}
    wrong_type=0;
    for(unsigned i=1;i<=4;++i){wrong_address=i;reset_methods();install_still_observers();assert(!still_observers_installed && !replacements);}
    wrong_address=0;reset_methods();install_still_observers();assert(still_observers_installed && replacements==4);
    install_still_observers();assert(replacements==4);
    original_still_capture=(IMP)capture_fixture;original_still_initiate=(IMP)initiate_fixture;
    original_still_prepare=(IMP)prepare_fixture;original_still_complete=(IMP)complete_fixture;
    for(unsigned i=0;i<24;++i){
        assert(still_capture((id)101,(SEL)1,(id)103,(id)104,(id)105)==-12783);
        assert(still_initiate((id)101,(SEL)2,(id)103)==-9);
        assert(still_prepare((id)101,(SEL)3,-1,3,true)==-12780);
        still_complete((id)101,(SEL)4,(id)103,(CMTime){-17,600,3,9},-7);
    }
    assert(capture_calls==24 && initiate_calls==24 && prepare_calls==24 && complete_calls==24);
    puts("Camera still observers:12 install refusals, one atomic4-method install and96 original forwards preserve errors, counts, objects and CMTime");
}
