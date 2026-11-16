// SPDX-License-Identifier: BSD-2-Clause-Patent
// Register the existing SoftwareAES only after its original parent kext loads.
// Same late-constructor contract as the qualified sheng/nezha HID provider.
#include "OriginalAESABI.h"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" void native_aes_static_init(void)
    __asm__("__GLOBAL__sub_I_NativeAES.cpp");
extern "C" {
volatile uint32_t native_aes_bootstrap_stage;
volatile int native_aes_bootstrap_status;
}

class NativeAESBootstrap : public IOService {
    OSDeclareDefaultStructors(NativeAESBootstrap)
public:
    bool start(IOService *provider) override;
};
OSDefineMetaClassAndStructors(NativeAESBootstrap, IOService)

static bool failure(unsigned stage, IOReturn status)
{
    native_aes_bootstrap_status = status;
    native_aes_bootstrap_stage = stage;
    IOLog("SoftwareAES: dependency setup failed at %u, status 0x%x\n", stage, status);
    return false;
}

bool NativeAESBootstrap::start(IOService *provider)
{
    static uint32_t attempted;
    uint32_t expected = 0;
    if (!IOService::start(provider) ||
        !__atomic_compare_exchange_n(&attempted, &expected, 1u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;
    native_aes_bootstrap_stage = 1;
    IOReturn status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOCryptoAcceleratorFamily");
    if (status) return failure(2, status);
    const OSSymbol *name = OSSymbol::withCString("IOAESAccelerator");
    const OSMetaClass *base = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    const OSMetaClass *parent = base ? base->getSuperClass() : nullptr;
    if (base != &IOAESAccelerator::gMetaClass || base->getClassSize() != sizeof(IOAESAccelerator) ||
        parent != &IOCryptoAccelerator::gMetaClass || parent->getClassSize() != sizeof(IOCryptoAccelerator) ||
        parent->getSuperClass() != &IOService::gMetaClass)
        return failure(3, kIOReturnUnsupported);
    native_aes_bootstrap_stage = 4;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return failure(5, kIOReturnNoResources);
    native_aes_static_init();
    status = OSMetaClass::postModLoad(handle);
    if (status) return failure(6, status);
    name = OSSymbol::withCString("NativeAES");
    OSObject *object = name ? OSMetaClass::allocClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    IOService *service = OSDynamicCast(IOService, object);
    if (!service) {
        OSSafeReleaseNULL(object);
        return failure(7, kIOReturnNoMemory);
    }
    if (!service->init()) {
        service->release();
        return failure(8, kIOReturnError);
    }
    if (!service->attach(provider)) {
        service->release();
        return failure(9, kIOReturnNotAttached);
    }
    if (!service->start(provider)) {
        service->detach(provider);
        service->release();
        return failure(10, kIOReturnError);
    }
    // The published provider keeps its lifetime reference, as the existing
    // late HID/Metal providers do. No constructor retry or unpublish occurs.
    native_aes_bootstrap_status = kIOReturnSuccess;
    native_aes_bootstrap_stage = 11;
    IOLog("SoftwareAES: original parent ABI verified; CBC provider started\n");
    return true;
}
