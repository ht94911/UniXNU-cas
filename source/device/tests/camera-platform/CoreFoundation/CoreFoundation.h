/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_TEST_CORE_FOUNDATION_H
#define NEZHA_TEST_CORE_FOUNDATION_H
#include <stdint.h>
typedef const struct __CFAllocator *CFAllocatorRef;
typedef const struct __CFString *CFStringRef;
typedef const void *CFTypeRef;
typedef uintptr_t CFTypeID;
typedef long CFIndex;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef struct __CFDictionary *CFMutableDictionaryRef;
typedef const struct __CFArray *CFArrayRef;
typedef const struct __CFNumber *CFNumberRef;
typedef const struct __CFBoolean *CFBooleanRef;
typedef uint32_t CFStringEncoding;
typedef struct { CFIndex version; const void *retain, *release, *copyDescription, *equal, *hash; } CFDictionaryKeyCallBacks;
typedef struct { CFIndex version; const void *retain, *release, *copyDescription, *equal; } CFDictionaryValueCallBacks;
typedef struct { CFIndex version; const void *retain, *release, *copyDescription, *equal; } CFArrayCallBacks;
enum { kCFNumberDoubleType=13, kCFStringEncodingUTF8=0x08000100 };
extern const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
extern const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
extern const CFArrayCallBacks kCFTypeArrayCallBacks;
extern CFBooleanRef kCFBooleanFalse, kCFBooleanTrue;
CFTypeID CFGetTypeID(CFTypeRef);
CFTypeID CFDictionaryGetTypeID(void);
CFTypeID CFArrayGetTypeID(void);
CFTypeID CFNumberGetTypeID(void);
CFTypeID CFStringGetTypeID(void);
CFTypeID CFBooleanGetTypeID(void);
CFTypeRef CFRetain(CFTypeRef);
void CFRelease(CFTypeRef);
CFArrayRef CFArrayCreate(CFAllocatorRef, const void **, CFIndex, const CFArrayCallBacks *);
CFIndex CFArrayGetCount(CFArrayRef);
const void *CFArrayGetValueAtIndex(CFArrayRef, CFIndex);
CFDictionaryRef CFDictionaryCreate(CFAllocatorRef, const void **, const void **, CFIndex,
    const CFDictionaryKeyCallBacks *, const CFDictionaryValueCallBacks *);
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef, CFIndex,
    const CFDictionaryKeyCallBacks *, const CFDictionaryValueCallBacks *);
CFTypeRef CFDictionaryGetValue(CFDictionaryRef, const void *);
void CFDictionarySetValue(CFMutableDictionaryRef, const void *, const void *);
void CFDictionaryApplyFunction(CFDictionaryRef, void (*)(const void *,const void *,void *),void *);
CFNumberRef CFNumberCreate(CFAllocatorRef, int, const void *);
unsigned char CFNumberGetValue(CFNumberRef, int, void *);
CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char *, CFStringEncoding);
unsigned char CFStringGetCString(CFStringRef, char *, CFIndex, CFStringEncoding);
unsigned char CFBooleanGetValue(CFBooleanRef);
CFMutableDictionaryRef CFDictionaryCreateMutableCopy(CFAllocatorRef, CFIndex, CFDictionaryRef);
CFStringRef nezha_test_cfstr(const char *);
#define CFSTR(s) nezha_test_cfstr(s)
#endif
