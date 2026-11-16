/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_TEST_CORE_MEDIA_H
#define NEZHA_TEST_CORE_MEDIA_H
#include <stdint.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
typedef struct { int64_t value; int32_t timescale; uint32_t flags; int64_t epoch; } CMTime;
typedef const struct opaqueCMClock *CMClockRef;
CMClockRef CMClockGetHostTimeClock(void);
CMTime CMClockMakeHostTimeFromSystemUnits(uint64_t);
CFDictionaryRef CMTimeCopyAsDictionary(CMTime, CFAllocatorRef);
typedef struct opaqueCMSampleBuffer *CMSampleBufferRef;
typedef uint32_t CMAttachmentMode;
enum { kCMAttachmentMode_ShouldPropagate=1 };
CVImageBufferRef CMSampleBufferGetImageBuffer(CMSampleBufferRef);
CFTypeRef CMGetAttachment(CMSampleBufferRef,CFStringRef,CMAttachmentMode *);
void CMSetAttachment(CMSampleBufferRef,CFStringRef,CFTypeRef,CMAttachmentMode);
typedef const struct opaqueCMFormatDescription *CMVideoFormatDescriptionRef;
int32_t CMVideoFormatDescriptionCreate(CFAllocatorRef, uint32_t, int32_t, int32_t,
    CFDictionaryRef, CMVideoFormatDescriptionRef *);
_Static_assert(sizeof(CMTime)==24,"public CMTime layout");
#endif
