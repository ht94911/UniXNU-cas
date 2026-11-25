/* SPDX-License-Identifier: MIT */
#include "../package/camera/userland/NezhaCameraPhotoCodec.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned calls, allocations, releases;
#ifndef __APPLE__
static bool fail_allocation;
#endif
static int32_t expected_status;
static void *expected_create;
static CFStringRef identifier_key, name_key, hardware_key;

#ifndef __APPLE__
struct __CFDictionary {const void *keys[3], *values[3];};
const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks={0};
const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks={0};
CFBooleanRef kCFBooleanFalse=(CFBooleanRef)"false";
CFStringRef nezha_test_cfstr(const char *value){return (CFStringRef)value;}
CFDictionaryRef CFDictionaryCreate(CFAllocatorRef allocator,const void **keys,
    const void **values,CFIndex count,const CFDictionaryKeyCallBacks *key_callbacks,
    const CFDictionaryValueCallBacks *value_callbacks)
{
    assert(!allocator && count==3 && key_callbacks==&kCFTypeDictionaryKeyCallBacks &&
        value_callbacks==&kCFTypeDictionaryValueCallBacks);
    if (fail_allocation) return NULL;
    struct __CFDictionary *result=malloc(sizeof(*result));assert(result);
    memcpy(result->keys,keys,sizeof result->keys);memcpy(result->values,values,sizeof result->values);
    ++allocations;return result;
}
void CFRelease(CFTypeRef value){assert(value);++releases;free((void *)value);}
CFTypeRef CFDictionaryGetValue(CFDictionaryRef dictionary,const void *key)
{for(unsigned i=0;i<3;++i)if(dictionary->keys[i]==key)return dictionary->values[i];return NULL;}
static bool strings_equal(CFTypeRef value,const char *expected)
{return value && !strcmp((const char *)value,expected);}
#else
static bool strings_equal(CFTypeRef value,const char *expected)
{
    CFStringRef string=CFStringCreateWithCString(NULL,expected,kCFStringEncodingUTF8);
    const bool equal=value && string && CFEqual(value,string);
    if (string) CFRelease(string);
    return equal;
}
#endif

static int32_t register_fixture(uint32_t codec,CFDictionaryRef info,void *create)
{
    assert(codec==UINT32_C(0x68766331) && info && create==expected_create);
    assert(strings_equal(CFDictionaryGetValue(info,identifier_key),
        "com.apple.videotoolbox.videoencoder.hevc.vcp"));
    assert(strings_equal(CFDictionaryGetValue(info,name_key),"HEVC (VCPHEVC, software)"));
    assert(CFDictionaryGetValue(info,hardware_key)==kCFBooleanFalse);
    ++calls;return expected_status;
}

int main(void)
{
    identifier_key=CFSTR("EncoderID");name_key=CFSTR("EncoderName");hardware_key=CFSTR("IsHardwareAccelerated");
    int callback_identity=7;expected_create=&callback_identity;
    for(unsigned absent=0;absent<5;++absent) {
        assert(nezha_camera_photo_codec_register(absent==0 ? NULL:register_fixture,
            absent==1 ? NULL:expected_create,absent==2 ? NULL:identifier_key,
            absent==3 ? NULL:name_key,absent==4 ? NULL:hardware_key)==-12783);
        assert(!calls && !allocations && !releases);
    }
#ifndef __APPLE__
    fail_allocation=true;
    assert(nezha_camera_photo_codec_register(register_fixture,expected_create,
        identifier_key,name_key,hardware_key)==-12782 && !calls);
    fail_allocation=false;
#endif
    const int32_t outcomes[]={0,-12908,-12904,17};
    for(unsigned i=0;i<sizeof outcomes/sizeof outcomes[0];++i) {
        expected_status=outcomes[i];
        assert(nezha_camera_photo_codec_register(register_fixture,expected_create,
            identifier_key,name_key,hardware_key)==outcomes[i]);
    }
    assert(calls==4);
#ifndef __APPLE__
    assert(allocations==4 && releases==4);
#endif
    puts("Photo codec: missing exports and allocation refuse registration; software hvc1 identity, original create callback and4 registration outcomes preserved");
}
