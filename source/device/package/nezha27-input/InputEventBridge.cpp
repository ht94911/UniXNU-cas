// SPDX-License-Identifier: BSD-2-Clause-Patent
// Copy/authenticate procedure from ShengHidInput71a40e7. Installed on one
// owned touchscreen event service only after the075 event contract is verified.
#include "InputChainABI.h"
#include "NezhaInput.h"
#include "NezhaTouchEdge.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSArray.h>
#include <libkern/libkern.h>

namespace {
constexpr unsigned dispatchSlot=198;
using Dispatch=void (*)(IOService *,OSObject *,IOOptionBits);
static_assert(sizeof(OSObject)==16,"24A446 event data begins after the16-byte OSObject");
static Dispatch originalDispatch;
static unsigned calls,corrected;
static IOLock *edgeLock;
static nezha_touch_edge_state edgeState={};
static unsigned edgeBegins,edgeFromBorder,edgePatched,edgeRefused;
static constexpr uint16_t kSlotDiversity[]={
    0x3771,0x56b7,0xbaa2,0x3607,0x2e4a,0x3a87,0x89a9,0xfffc,0xfc74,0x5635,0xbe60,0x32e5,
    0x9209,0x4a6a,0xedc5,0x5c68,0x6a10,0x7a2a,0xaf75,0x137e,0x0655,0x43aa,0x12e9,0x4578,
    0x4275,0xff53,0x1814,0x122e,0x13f6,0x1d35,0xacb1,0x7eb0,0x1262,0x82eb,0x164e,0x37a5,
    0xb659,0x6c51,0xa20f,0xb3b6,0x6bcb,0x5a20,0x5062,0x00d7,0x7c85,0x8a26,0x3539,0x688b,
    0x1e60,0x1955,0x0689,0xc256,0xa383,0xf021,0x1f0a,0xb4bb,0x8ffc,0xb5b9,0x8764,0x5d96,
    0x80d9,0x0c9c,0x5d0a,0xcbcc,0x617d,0x848a,0x2312,0x3540,0xc257,0x3025,0x9fc2,0x5038,
    0xc666,0x6cc3,0x550c,0xa19a,0xa51b,0x4577,0x573c,0x1a4e,0x6c3d,0xb049,0xc4b2,0xc90d,
    0x7d59,0x4897,0x3c68,0xb085,0x4529,0x639f,0xccfb,0x55eb,0xe933,0xaec3,0x5ec5,0x5219,
    0xc6b2,0x8a43,0x4a20,0xd9f2,0x981a,0xa27f,0xc4f9,0x6b87,0x60a1,0x7e78,0x36aa,0x86ef,
    0x9be9,0x7318,0x93b7,0x638e,0x61a6,0x9175,0x136b,0xdb58,0x4a31,0x0988,0x5393,0xabe0,
    0x0ad9,0x6c99,0xd52d,0xe213,0x308f,0xd78d,0x3a1d,0xa390,0x240b,0x1b89,0x8d3c,0x2652,
    0x7f14,0x0759,0x63c4,0x800f,0x9cc2,0x02ac,0x785f,0xcc6b,0x82cd,0x808e,0x37ce,0xa4c7,
    0xe8de,0xa343,0x4bc0,0xf8a6,0xac7f,0x7974,0xea1b,0x4b35,0x9eb4,0x595a,0x5b2b,0x699e,
    0x2b52,0xf40e,0x0ddb,0x0f88,0x8700,0x36c3,0x058e,0xf16e,0x3a71,0xda1e,0x10b6,0x8654,
    0x96c3,0x4cb5,0x19b8,0x6518,0x5717,0xd2bf,0x09fa,0xad91,0xd335,0x4902,0xae57,0xaad9,
    0x5299,0xe604,0x8660,0x0189,0xc3de,0x41f6,0x5c1e,0xb54e,0xef25,0x4bdd,0xefa8,0x9835,
    0xae71,0x67cf,0xa85e,0x7b4f,0x4abb,0xc0d0,0x1134,0x7eca,0xf128,0xc748,0xa49a,0xde1d,
    0x90ba,0xea29,0xce88,0xc62d,0x0078,0x0dce,0x454b,0xc58b,0xe23b,0x2684,0xcca1,0xfe11,
    0x69fc,0xaaca,0x6518,0x7547,
};

static uint32_t markEdge(OSObject *event) {
    if (!event || !event->metaCast("IOHIDEvent") || chainType(event)!=11)
        return 0;
    const int32_t index=chainInteger(event,0xb0005);
    const uint32_t before=(uint32_t)chainInteger(event,0xb0007);
    if (index<0 || (unsigned)index>=NZ_EDGE_CONTACTS || !edgeLock) {
        __atomic_fetch_add(&edgeRefused,1,__ATOMIC_RELAXED);
        return before&NZ_EDGE_TYPES;
    }
    const bool touching=chainInteger(event,0xb0009)!=0;
    const int32_t x=chainFixed(event,0xb0000),y=chainFixed(event,0xb0001);
    // No virtual calls, allocation or original dispatch while holding the lock.
    IOLockLock(edgeLock);
    const uint32_t after=nezha_touch_edge_mask(&edgeState,(unsigned)index,before,
        touching,x,y,NEZHA_PANEL_W,NEZHA_PANEL_H);
    if (touching && (before&NZ_EDGE_TOUCH_CHANGED)) {
        __atomic_fetch_add(&edgeBegins,1,__ATOMIC_RELAXED);
        if (after&NZ_EDGE_TYPES)__atomic_fetch_add(&edgeFromBorder,1,__ATOMIC_RELAXED);
    }
    if (after!=before)__atomic_fetch_add(&edgePatched,1,__ATOMIC_RELAXED);
    IOLockUnlock(edgeLock);
    if (after!=before)
        chainCall<void,uint32_t,int32_t,IOOptionBits>(event,24,0x130e,
            0xb0007,(int32_t)after,0);
    return after&NZ_EDGE_TYPES;
}

static void markCollectionEdges(OSObject *event) {
    // Exact24A446 IOHIDEvent::appendChild, slot15/diversity0x2c9e:
    // ldr x0,[x0,#0x18] and str x0,[x19,#0x18]. The original getType
    // also proves _data at0x10. This is an audited data member read, not
    // the old17 unsigned-vptr replacement. Children and their original PAC
    // method entries remain owned by IOHIDFamily.
    OSArray *children=*reinterpret_cast<OSArray **>(
        reinterpret_cast<uint8_t *>(event)+0x18);
    uint32_t edges=0;
    if (!children) {
        edges=markEdge(event);
    } else if (OSDynamicCast(OSArray,children) && children->getCount()<=32) {
        for (unsigned i=0;i<children->getCount();++i)
            edges|=markEdge(children->getObject(i));
    } else {
        __atomic_fetch_add(&edgeRefused,1,__ATOMIC_RELAXED);
    }
    if (edges) {
        const uint32_t mask=(uint32_t)chainInteger(event,0xb0007);
        chainCall<void,uint32_t,int32_t,IOOptionBits>(event,24,0x130e,
            0xb0007,(int32_t)(mask|edges),0);
    }
}

static void dispatch(IOService *service,OSObject *event,IOOptionBits options) {
    if (event && event->metaCast("IOHIDEvent") && chainType(event)==11) {
        int before=chainInteger(event,0xb0019);
        ++calls;
        if (!before) {
            chainCall<void,uint32_t,int32_t,IOOptionBits>(event,24,0x130e,0xb0019,1,0);
            ++corrected;
        }
        markCollectionEdges(event);
        if (calls<=8 || (calls%128)==0) {
            printf("NEZHA INPUT INTEGRATION: calls=%u corrected=%u before=%d after=%d\n",
                calls,corrected,before,chainInteger(event,0xb0019));
            printf("NEZHA INPUT EDGE: began=%u from_edge=%u patched=%u refused=%u\n",
                __atomic_load_n(&edgeBegins,__ATOMIC_RELAXED),
                __atomic_load_n(&edgeFromBorder,__ATOMIC_RELAXED),
                __atomic_load_n(&edgePatched,__ATOMIC_RELAXED),
                __atomic_load_n(&edgeRefused,__ATOMIC_RELAXED));
        }
    }
    originalDispatch(service,event,options);
}
}

extern "C" void nezha_input_integration_install(IORegistryEntry *driver) {
    if (!driver || originalDispatch) return;
    const OSMetaClass *meta=driver->getMetaClass();
    if (!meta || strcmp(meta->getClassName(),"AppleUserHIDEventService")) return;
    if (driver->getProperty("DisplayIntegrated")!=kOSBooleanTrue ||
        driver->getProperty("Built-In")!=kOSBooleanTrue) return;
    constexpr unsigned slots=sizeof(kSlotDiversity)/sizeof(kSlotDiversity[0]);
    static_assert(slots==220 && kSlotDiversity[dispatchSlot]==0x1134,"Exact24A446 service table");
    void **table=reinterpret_cast<void **>(ptrauth_auth_data(
        *reinterpret_cast<void **>(driver),ptrauth_key_process_independent_data,
        ptrauth_blend_discriminator(driver,0xcda1)));
    void **copy=static_cast<void **>(IOMalloc(slots*sizeof(void *)));
    if (!copy) return;
    Dispatch saved=nullptr;
    for (unsigned index=0;index<slots;++index) {
        void *entry=table[index];
        void *function=entry?ptrauth_strip(entry,ptrauth_key_function_pointer):nullptr;
        void *check=ptrauth_sign_unauthenticated(function,ptrauth_key_function_pointer,
            ptrauth_blend_discriminator(&table[index],kSlotDiversity[index]));
        if (!entry || check!=entry) {
            printf("NEZHA INPUT INTEGRATION INSTALL: refused_slot=%u\n",index);
            IOFree(copy,slots*sizeof(void *));
            return;
        }
        if (index==dispatchSlot) {
            saved=reinterpret_cast<Dispatch>(ptrauth_sign_unauthenticated(
                function,ptrauth_key_function_pointer,0));
            function=ptrauth_strip(reinterpret_cast<void *>(&dispatch),ptrauth_key_function_pointer);
        }
        copy[index]=ptrauth_sign_unauthenticated(function,ptrauth_key_function_pointer,
            ptrauth_blend_discriminator(&copy[index],kSlotDiversity[index]));
    }
    IOLock *lock=IOLockAlloc();
    if (!lock) {
        printf("NEZHA INPUT INTEGRATION INSTALL: edge lock allocation failed\n");
        IOFree(copy,slots*sizeof(void *));
        return;
    }
    edgeLock=lock;
    originalDispatch=saved;
    __asm__ volatile("dmb ish":::"memory");
    *reinterpret_cast<void **>(driver)=ptrauth_sign_unauthenticated(
        copy,ptrauth_key_process_independent_data,ptrauth_blend_discriminator(driver,0xcda1));
    printf("NEZHA INPUT INTEGRATION INSTALL: slots=%u dispatch=%u\n",slots,dispatchSlot);
}
