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
static intptr_t capture_slide;
uint64_t mach_absolute_time(void);
#include "NezhaCameraPhotoObservers.h"

static unsigned emit_calls,render_calls,encode_calls,sink_calls,replacements;
static unsigned unavailable,wrong_type,wrong_address;
struct TestMethod {IMP implementation;const char *encoding;};
static struct TestMethod methods[4];
static const char *const classes[]={"BWFigVideoCaptureDevice","BWPhotoEncoderNode","BWPhotoEncoderController","BWStillImageSampleBufferSinkNode"};
static const char *const selectors[]={"captureStream:willEmitStillImageSampleBuffer:cachedStreamingBuffer:","renderSampleBuffer:forInput:","_encodePrimarySbuf:metadata:processingFlags:primaryImageHandleInOut:"};
static const char *const encodings[]={"v36@0:8@16^{opaqueCMSampleBuffer=}24B32","v32@0:8^{opaqueCMSampleBuffer=}16@24","i44@0:8^{opaqueCMSampleBuffer=}16@24I32^q36","v32@0:8^{opaqueCMSampleBuffer=}16@24"};
static const uintptr_t addresses[]={0x1b069669cULL,0x1b06a9aa0ULL,0x1b065c5c8ULL,0x1b062f864ULL};
static const CMSampleBufferRef expected_sample=(CMSampleBufferRef)(uintptr_t)0x314;
static const CVImageBufferRef expected_image=(CVImageBufferRef)(uintptr_t)0x315;
static CFStringRef seen_key;
static uint32_t expected_flags;
static int32_t expected_status;

Class objc_getClass(const char *name)
{for(unsigned i=0;i<4;++i)if(!strcmp(name,classes[i]))return (Class)(uintptr_t)(i+1);assert(!"unexpected class");return NULL;}
SEL sel_registerName(const char *name)
{for(unsigned i=0;i<3;++i)if(!strcmp(name,selectors[i]))return (SEL)(uintptr_t)(i+1);assert(!"unexpected selector");return NULL;}
Method class_getInstanceMethod(Class type,SEL selector)
{unsigned i=(unsigned)(uintptr_t)type-1;assert(i<4 && selector==(SEL)(uintptr_t)(i==3?2:i+1));return unavailable==i+1 ? NULL:&methods[i];}
IMP method_getImplementation(Method method){return method->implementation;}
const char *method_getTypeEncoding(Method method)
{return wrong_type==(unsigned)(method-methods)+1 ? "v16@0:8":method->encoding;}
IMP method_setImplementation(Method method,IMP replacement)
{IMP old=method->implementation;method->implementation=replacement;++replacements;return old;}
uint64_t mach_absolute_time(void){return 12345;}
CVImageBufferRef CMSampleBufferGetImageBuffer(CMSampleBufferRef sample)
{assert(sample==expected_sample);return expected_image;}
size_t CVPixelBufferGetWidth(CVPixelBufferRef image){assert(image==expected_image);return 4096;}
size_t CVPixelBufferGetHeight(CVPixelBufferRef image){assert(image==expected_image);return 3072;}
CMTime CMSampleBufferGetPresentationTimeStamp(CMSampleBufferRef sample)
{assert(sample==expected_sample);return (CMTime){-17,600,3,9};}
CFTypeRef CMGetAttachment(CMSampleBufferRef sample,CFStringRef key,CMAttachmentMode *mode)
{assert(sample==expected_sample && !mode);(void)key;return NULL;}
CFTypeID CFGetTypeID(CFTypeRef value){assert(value);return 2;}
CFTypeID CFDictionaryGetTypeID(void){return 1;}
CFTypeID CFNumberGetTypeID(void){return 2;}
CFTypeRef CFDictionaryGetValue(CFDictionaryRef dictionary,const void *key)
{assert(dictionary && key);return NULL;}
unsigned char CFNumberGetValue(CFNumberRef value,int type,void *output)
{assert(value && type==kCFNumberDoubleType && output);*(double *)output=7;return 1;}
CFStringRef nezha_test_cfstr(const char *text){seen_key=(CFStringRef)text;return seen_key;}

static void emit_fixture(id self,SEL selector,id stream,CMSampleBufferRef sample,bool cached)
{assert(self==(id)101 && selector==(SEL)1 && stream==(id)102 && sample==expected_sample && cached);++emit_calls;}
static void render_fixture(id self,SEL selector,CMSampleBufferRef sample,id input)
{assert(self==(id)101 && selector==(SEL)2 && sample==expected_sample && input==(id)102);++render_calls;}
static int32_t encode_fixture(id self,SEL selector,CMSampleBufferRef sample,id metadata,uint32_t flags,int64_t *handle)
{assert(self==(id)101 && selector==(SEL)3 && sample==expected_sample && metadata==(id)102 && flags==expected_flags && handle);*handle=-17;++encode_calls;return expected_status;}
static void sink_fixture(id self,SEL selector,CMSampleBufferRef sample,id input)
{assert(self==(id)101 && selector==(SEL)2 && sample==expected_sample && input==(id)102);++sink_calls;}
static void reset_methods(void)
{for(unsigned i=0;i<4;++i){methods[i].implementation=(IMP)(uintptr_t)(addresses[i]+capture_slide+(wrong_address==i+1?4:0));methods[i].encoding=encodings[i];}}
int main(void)
{
    capture_slide=0x234000;
    for(unsigned i=1;i<=4;++i){unavailable=i;reset_methods();install_photo_observers();assert(!photo_observers_installed && !replacements);}
    unavailable=0;
    for(unsigned i=1;i<=4;++i){wrong_type=i;reset_methods();install_photo_observers();assert(!photo_observers_installed && !replacements);}
    wrong_type=0;
    for(unsigned i=1;i<=4;++i){wrong_address=i;reset_methods();install_photo_observers();assert(!photo_observers_installed && !replacements);}
    wrong_address=0;reset_methods();install_photo_observers();assert(photo_observers_installed && replacements==4);
    install_photo_observers();assert(replacements==4);
    original_photo_emit=(IMP)emit_fixture;original_photo_render=(IMP)render_fixture;
    original_photo_encode=(IMP)encode_fixture;original_photo_sink=(IMP)sink_fixture;
    for(unsigned i=0;i<24;++i) {
        expected_flags=i%2 ? UINT32_MAX:0;
        expected_status=i%2 ? -12908:0;
        photo_emit((id)101,(SEL)1,(id)102,expected_sample,true);
        photo_render((id)101,(SEL)2,expected_sample,(id)102);
        int64_t handle=0;
        assert(photo_encode((id)101,(SEL)3,expected_sample,(id)102,expected_flags,&handle)==expected_status && handle==-17);
        photo_sink((id)101,(SEL)2,expected_sample,(id)102);
    }
    assert(emit_calls==24 && render_calls==24 && encode_calls==24 && sink_calls==24);
    puts("Photo observers:12 install refusals and96 original forwards preserve sample, input, cached flag, encoding flags, errors and output handle");
}
