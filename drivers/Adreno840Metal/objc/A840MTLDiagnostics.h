/* SPDX-License-Identifier: MIT */
/* Real-time iOS 27 diagnostics for pure-native Adreno 840 Metal:
 *  - SHENGTHROW uncaught ObjC exception record (read by XNU BootFaultProbe)
 *  - IOServiceOpen(port, task, 0x4147xxxx) progress markers & text rows */
#ifndef A840_MTL_DIAGNOSTICS_H
#define A840_MTL_DIAGNOSTICS_H

#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>

namespace a840diag {

inline std::atomic<unsigned> gAccelPort{0u};
inline std::atomic<unsigned> gTextRow{0u};
inline NSUncaughtExceptionHandler *gPrevHandler = nullptr;
inline char *gLastThrowText = nullptr;

inline void markPort(unsigned port, uint32_t value)
{
    if (!port) return;
    static int (*sOpenFn)(unsigned, unsigned, uint32_t, unsigned *) = nullptr;
    static int (*sCloseFn)(unsigned) = nullptr;
    static bool sResolved = false;
    if (!sResolved) {
        sResolved = true;
        if (void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL)) {
            sOpenFn = reinterpret_cast<int (*)(unsigned, unsigned, uint32_t, unsigned *)>(
                dlsym(iokit, "IOServiceOpen"));
            sCloseFn = reinterpret_cast<int (*)(unsigned)>(dlsym(iokit, "IOServiceClose"));
        }
    }
    if (!sOpenFn) return;
    unsigned conn = 0u;
    if (sOpenFn(port, mach_task_self(), 0x41470000u | (value & 0xffffu), &conn) == 0 && conn && sCloseFn) {
        sCloseFn(conn);
    }
}

inline void setAcceleratorPort(unsigned port)
{
    if (port) gAccelPort.store(port, std::memory_order_release);
}

inline void mark(uint32_t value)
{
    markPort(gAccelPort.load(std::memory_order_acquire), value);
}

inline void reportTableRow(unsigned port, unsigned row, const char *line)
{
    if (!port || !line || row >= 48u) return;
    markPort(port, 0xC000u | (row & 0x3fu));
    const size_t len = strnlen(line, 88u);
    for (size_t i = 0u; i < len; i += 2u) {
        const unsigned a = static_cast<unsigned char>(line[i]) & 0x7fu;
        const unsigned b = (i + 1u < len) ? (static_cast<unsigned char>(line[i + 1u]) & 0x7fu) : 0u;
        markPort(port, 0x8000u | (a << 7u) | b);
    }
}

inline void reportText(const char *text)
{
    if (!text || !text[0]) return;
    const unsigned port = gAccelPort.load(std::memory_order_acquire);
    if (!port) return;
    const unsigned row = gTextRow.fetch_add(1u, std::memory_order_relaxed) % 32u;
    reportTableRow(port, row, text);
}

inline void recordMissingSelector(const char *clsName, const char *selName)
{
    char buf[128] = {};
    snprintf(buf, sizeof(buf), "A840MISS %.32s %.80s", clsName ? clsName : "?", selName ? selName : "?");
    reportText(buf);
    char throwBuf[160] = {};
    snprintf(throwBuf, sizeof(throwBuf), "SHENGTHROW MissingSelector | %.110s |", buf);
    char *copy = strdup(throwBuf);
    if (copy) gLastThrowText = copy;
}

inline void uncaughtExceptionHandler(NSException *exception)
{
    @autoreleasepool {
        NSMutableString *text = [NSMutableString stringWithFormat:@"SHENGTHROW %@ | ", exception.name ?: @"?"];
        NSString *reason = exception.reason ?: @"";
        [text appendString:(reason.length > 96u ? [reason substringToIndex:96u] : reason)];
        [text appendString:@" |"];
        NSArray<NSNumber *> *frames = exception.callStackReturnAddresses;
        for (NSUInteger i = 0u; i < frames.count && i < 20u; ++i) {
            [text appendFormat:@" %llx", static_cast<unsigned long long>(frames[i].unsignedLongLongValue)];
        }
        for (NSUInteger i = 0u; i < text.length; ++i) {
            const unichar ch = [text characterAtIndex:i];
            if (ch < 32u || ch > 126u) {
                [text replaceCharactersInRange:NSMakeRange(i, 1u) withString:@"?"];
            }
        }
        if (const char *utf8 = text.UTF8String) {
            gLastThrowText = strdup(utf8);
            reportText(utf8);
        }
    }
    if (gPrevHandler) gPrevHandler(exception);
}

inline void installExceptionHandler()
{
    static bool installed = false;
    if (installed) return;
    installed = true;
    gPrevHandler = NSGetUncaughtExceptionHandler();
    NSSetUncaughtExceptionHandler(uncaughtExceptionHandler);
}

} /* namespace a840diag */

#endif /* A840_MTL_DIAGNOSTICS_H */
