// SPDX-License-Identifier: BSD-2-Clause-Patent
// Passive listener on the selected original HID event service.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include "InputChainABI.h"
#include <kern/clock.h>
#include <libkern/c++/OSIterator.h>

static unsigned eventCount;
static unsigned lastTouch;
static uint64_t lastDump;
static unsigned observerBusy;
static unsigned observerDropped;

static void observeEvent(OSObject *, OSObject *, void *, OSObject *object, IOOptionBits) {
    if (!object || !object->metaCast("IOHIDEvent")) return;
    if (__atomic_exchange_n(&observerBusy, 1, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&observerDropped, 1, __ATOMIC_RELAXED);
        return;
    }
    void *event = object;
    unsigned ordinal = ++eventCount;
    unsigned type = chainType(event);
    unsigned touch = type == 11 ? chainInteger(event, 0xb0009) : 0;
    uint64_t now = mach_absolute_time();
    uint64_t second;
    nanoseconds_to_absolutetime(1000000000ULL, &second);
    bool dump = ordinal <= 24 || touch != lastTouch || now - lastDump >= second;
    lastTouch = touch;
    if (dump) {
        lastDump = now;
        unsigned char bytes[4096];
        IOByteCount wanted = chainLength(event);
        IOByteCount copied = wanted <= sizeof(bytes) ? chainRead(event, bytes, sizeof(bytes)) : 0;
        if (copied > sizeof(bytes)) copied = 0;
        printf("NEZHA INPUT EVENT: n=%u type=%u touch=%u mask=%x id=%d x=%d y=%d ticks=%llu bytes=%lu wanted=%lu dropped=%u\n",
            ordinal, type, touch,
            type == 11 ? chainInteger(event, 0xb0007) : 0,
            type == 11 ? chainInteger(event, 0xb0005) : 0,
            type == 11 ? chainFixed(event, 0xb0000) : 0,
            type == 11 ? chainFixed(event, 0xb0001) : 0,
            (unsigned long long)now, (unsigned long)copied, (unsigned long)wanted, observerDropped);
        for (IOByteCount at = 0; at < copied; at += 64) {
            char line[129];
            unsigned count = copied - at < 64 ? copied - at : 64;
            const char hex[] = "0123456789abcdef";
            for (unsigned i = 0; i < count; ++i) {
                line[i * 2] = hex[bytes[at+i] >> 4];
                line[i * 2 + 1] = hex[bytes[at+i] & 15];
            }
            line[count * 2] = 0;
            printf("NEZHA INPUT EVENTBYTES: n=%u at=%lu %s\n", ordinal, (unsigned long)at, line);
        }
    }
    __atomic_store_n(&observerBusy, 0, __ATOMIC_RELEASE);
}

extern "C" void nezha_input_chain_attach(IORegistryEntry *entry, IOService *client) {
    static bool attempted;
    if (attempted || !entry || !client || !entry->metaCast("IOHIDEventService")) return;
    attempted = true;
    auto *service = static_cast<IOService *>(entry);
    bool opened = chainOpen(service, client, observeEvent);
    printf("NEZHA INPUT LISTENER: opened=%u service=%s\n", (unsigned)opened, service->getName());
}
