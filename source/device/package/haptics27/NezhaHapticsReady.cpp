// SPDX-License-Identifier: BSD-2-Clause-Patent
// Original24A446 AHSFirmwareReadyNotifier is an IOService with no extra state.
// AudioToolbox waits for its class's first match after routing the haptics VAD.
// This marker belongs to the working HAP530 endpoint, not an Apple controller.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/kmod.h>
#include <sys/errno.h>

#ifndef NEZHA_HAPTICS_READY_HOST
static_assert(sizeof(IOService) == 0x88, "fixed24A446 readiness service layout");
extern "C" kmod_info_t g_kernel_kmod_info;
#endif
extern "C" void nezha_haptics_ready_static_init(void)
    __asm__("__GLOBAL__sub_I_NezhaHapticsReady.cpp");

class AHSFirmwareReadyNotifier : public IOService {
    OSDeclareDefaultStructors(AHSFirmwareReadyNotifier)
};
OSDefineMetaClassAndStructors(AHSFirmwareReadyNotifier, IOService)
#ifndef NEZHA_HAPTICS_READY_HOST
static_assert(sizeof(AHSFirmwareReadyNotifier) == sizeof(IOService),
              "original readiness marker owns no private fields");
#endif

namespace {
IOService *published;
bool constructorAttempted;
int constructorResult;
uint32_t readyState;
int readyError;
void status(uint32_t state, int error) {
    __atomic_store_n(&readyError, error, __ATOMIC_RELEASE);
    __atomic_store_n(&readyState, state, __ATOMIC_RELEASE);
}
int registerClass() {
    if (constructorAttempted) return constructorResult;
    constructorAttempted = true;
    // Use the accepted late-constructor path of the native ALS/AES providers.
    const OSSymbol *name = OSSymbol::withCString("AHSFirmwareReadyNotifier");
    if (!name) return constructorResult = ENOMEM;
    const OSMetaClass *existing = OSMetaClass::getMetaClassWithName(name);
    name->release();
    if (existing) return constructorResult = EEXIST;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return constructorResult = ENOMEM;
    nezha_haptics_ready_static_init();
    if (OSMetaClass::postModLoad(handle)) return constructorResult = EIO;
    name = OSSymbol::withCString("AHSFirmwareReadyNotifier");
    if (!name) return constructorResult = ENOMEM;
    existing = OSMetaClass::getMetaClassWithName(name);
    name->release();
    if (existing != &AHSFirmwareReadyNotifier::gMetaClass ||
        existing->getSuperClass() != &IOService::gMetaClass ||
        existing->getClassSize() != sizeof(AHSFirmwareReadyNotifier))
        return constructorResult = ENODEV;
    return 0;
}
}

// Only the serialized motor worker calls update. Its caller has already
// checked real initialization, endpoint ownership, HAL and preset visibility.
extern "C" int nezha_haptics_ready_update(int ready) {
    if (!ready) {
        if (!published) return 0;
        status(2, 0);
        if (!published->terminate(kIOServiceRequired | kIOServiceSynchronous)) {
            status(2, EIO);
            return EIO;
        }
        published->release();
        published = nullptr;
        status(0, 0);
        IOLog("NEZHA HAPTICS: execution readiness withdrawn\n");
        return 0;
    }
    if (published) return readyState == 1 ? 0 : EIO;
    int error = registerClass();
    if (error) {status(0, error); return error;}
    IOService *provider = IOService::getResourceService();
    auto service = new AHSFirmwareReadyNotifier;
    bool initialized = service && service->init();
    bool attached = initialized && provider && service->attach(provider);
    if (!attached) {
        if (service) service->release();
        status(0, ENXIO);
        return ENXIO;
    }
    service->setName("AHSFirmwareReadyNotifier");
    // Original start stores uptime in milliseconds as a 64-bit OSNumber.
    uint64_t nanoseconds = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
    if (!service->setProperty("FirmwareReadyTimestamp", nanoseconds / 1000000, 64) ||
        !service->start(provider)) {
        service->detach(provider);
        service->release();
        status(0, EIO);
        return EIO;
    }
    published = service; // Retain the allocation until synchronous withdrawal.
    service->registerService(kIOServiceSynchronous);
    status(1, 0);
    IOLog("NEZHA HAPTICS: execution readiness published after real endpoint and iOS assets\n");
    return 0;
}

extern "C" uint32_t nezha_haptics_ready_state(void) {
    return __atomic_load_n(&readyState, __ATOMIC_ACQUIRE);
}
extern "C" int nezha_haptics_ready_error(void) {
    return __atomic_load_n(&readyError, __ATOMIC_ACQUIRE);
}
