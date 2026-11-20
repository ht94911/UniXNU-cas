/* SPDX-License-Identifier: MIT */
#include "NezhaCameraFormats.h"
#include <CoreMedia/CoreMedia.h>
#include <stdbool.h>
#include <stddef.h>

/* FormatDescription and scaler keys follow HakiOS f128f1d camera_device.c.
 * Actual dimensions/rate/CFA/binning are nezha's stock OVX mode1. The
 * 24A446 source parser's crop keys were checked in the selected cache. */
static bool put_number(CFMutableDictionaryRef d, CFStringRef key, double value)
{
    CFNumberRef n=CFNumberCreate(NULL,kCFNumberDoubleType,&value);
    if (!n) return false;
    CFDictionarySetValue(d,key,n);
    CFRelease(n);
    return true;
}

static CFMutableDictionaryRef dictionary(void)
{
    return CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
}

static CFDictionaryRef output_format(bool primary_preview)
{
    CFMutableDictionaryRef d=dictionary();
    if (!d) return NULL;
    const double codes[]={0x42475241,0x34323066,0x34323076};
    const unsigned count=primary_preview ? 1u:3u;
    CFNumberRef n[3]={NULL,NULL,NULL};
    bool complete=true;
    for (unsigned i=0;i<count;++i) {
        n[i]=CFNumberCreate(NULL,kCFNumberDoubleType,&codes[i]);
        complete=complete && n[i]!=NULL;
    }
    CFArrayRef formats=complete ?
        CFArrayCreate(NULL,(const void **)n,count,&kCFTypeArrayCallBacks) : NULL;
    for (unsigned i=0;i<3;++i) if (n[i]) CFRelease(n[i]);
    if (!formats) { CFRelease(d);return NULL; }
    CFDictionarySetValue(d,CFSTR("SupportedPixelFormats"),formats);
    CFRelease(formats);
    if (!put_number(d,CFSTR("NativeWidth"),4096) ||
        !put_number(d,CFSTR("NativeHeight"),3072) ||
        !put_number(d,CFSTR("MaxDownscaleFactor"),32) ||
        !put_number(d,CFSTR("MaxScaleFactor"),1) ||
        !put_number(d,CFSTR("MinScaleFactor"),1.0/32) ||
        !put_number(d,CFSTR("OutputWidthAlignmentRequirement"),2)) {
        CFRelease(d);return NULL;
    }
    return d;
}

CFArrayRef nezha_camera_main_formats(void)
{
    CFMutableDictionaryRef d=dictionary(),outputs=dictionary();
    CMVideoFormatDescriptionRef description=NULL;
    /* PrimaryScaler delivers the existing BGRA renderer to the preview
     * consumer. Auxiliary and still outputs retain their three formats. */
    CFDictionaryRef primary=output_format(true),output=output_format(false);
    CFArrayRef result=NULL;
    if (!d || !outputs || !primary || !output ||
        CMVideoFormatDescriptionCreate(NULL,0x34323066,4096,3072,NULL,&description) || !description)
        goto done;
    CFDictionarySetValue(d,CFSTR("FormatDescription"),description);
    CFDictionarySetValue(d,CFSTR("Name"),CFSTR("nezha-main-4096x3072-30"));
    CFDictionarySetValue(d,CFSTR("VideoIsBinned"),kCFBooleanTrue);
    static const char *const dimensions[]={"SensorWidth","SensorHeight","VideoMaxWidth","VideoMaxHeight",
        "SensorCropWidth","SensorCropHeight","PreGDCSensorCropWidth","PreGDCSensorCropHeight",
        "PostGDCCropWidth","PostGDCCropHeight","GeometricDistortionCorrectedNativeWidth",
        "GeometricDistortionCorrectedNativeHeight"};
    for (unsigned i=0;i<sizeof(dimensions)/sizeof(dimensions[0]);++i) {
        CFStringRef key=CFStringCreateWithCString(NULL,dimensions[i],kCFStringEncodingUTF8);
        if (!key) goto done;
        const bool ok=put_number(d,key,(i&1) ? 3072:4096);
        CFRelease(key);
        if (!ok) goto done;
    }
    if (!put_number(d,CFSTR("VideoMinFrameRate"),30) ||
        !put_number(d,CFSTR("VideoMaxFrameRate"),30) ||
        !put_number(d,CFSTR("VideoRawBitDepth"),10) ||
        !put_number(d,CFSTR("VideoBinningFactorHorizontal"),2) ||
        !put_number(d,CFSTR("VideoBinningFactorVertical"),2) ||
        !put_number(d,CFSTR("VideoFieldOfView"),72.8)) goto done;
    CFDictionarySetValue(outputs,CFSTR("PrimaryScaler"),primary);
    CFDictionarySetValue(outputs,CFSTR("SecondaryScaler"),output);
    CFDictionarySetValue(outputs,CFSTR("StillImage"),output);
    CFDictionarySetValue(d,CFSTR("SupportedOutputs"),outputs);
    result=CFArrayCreate(NULL,(const void **)&d,1,&kCFTypeArrayCallBacks);
done:
    if (description) CFRelease(description);
    if (primary) CFRelease(primary);
    if (output) CFRelease(output);
    if (outputs) CFRelease(outputs);
    if (d) CFRelease(d);
    return result;
}
