/* SPDX-License-Identifier: GPL-2.0-only */
/* Registry-only bridge following sheng4601795 WifiNet for24A446. These owned
 * IOService subclasses are NOT the donor's IO80211/IONetworkingFamily ABI.
 * No pointer is passed to an original networking C++ class or user client.
 * The real interface, control, packets and link state belong to BSD/PeachNet.
 * The build must exclude conflicting native networking metaclasses. */
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <sys/errno.h>
#include <mach/kmod.h>

class IO80211Controller : public IOService { OSDeclareDefaultStructors(IO80211Controller); };
class IONetworkInterface : public IOService { OSDeclareDefaultStructors(IONetworkInterface); };
class IO80211Interface : public IONetworkInterface {
    OSDeclareDefaultStructors(IO80211Interface);
public:
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **out) APPLE_KEXT_OVERRIDE {
        if (out) *out = nullptr;
        return kIOReturnUnsupported; /* actual Apple80211 BSD ioctl fallback */
    }
};
class IONetworkStackUserClient : public IOUserClient {
    OSDeclareDefaultStructors(IONetworkStackUserClient);
public:
    IOReturn clientClose() APPLE_KEXT_OVERRIDE { if (!isInactive()) terminate(); return kIOReturnSuccess; }
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE {
        /* BSD already owns en0. Do not acknowledge an unimplemented naming
         * or hardware operation as success just to make registry discovery quiet. */
        return kIOReturnUnsupported;
    }
};
class IONetworkStack : public IOService {
    OSDeclareDefaultStructors(IONetworkStack);
public:
    IOReturn newUserClient(task_t task, void *security, UInt32 type,
                          OSDictionary *, IOUserClient **out) APPLE_KEXT_OVERRIDE {
        if (!out) return kIOReturnBadArgument;
        *out = nullptr;
        if (IOUserClient::clientHasPrivilege(security, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
            return kIOReturnNotPrivileged;
        IONetworkStackUserClient *client = OSTypeAlloc(IONetworkStackUserClient);
        if (!client) return kIOReturnNoMemory;
        if (!client->initWithTask(task, security, type)) { client->release(); return kIOReturnError; }
        if (!client->attach(this)) { client->release(); return kIOReturnError; }
        if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnError; }
        *out = client; return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(IO80211Controller, IOService);
OSDefineMetaClassAndStructors(IONetworkInterface, IOService);
OSDefineMetaClassAndStructors(IO80211Interface, IONetworkInterface);
OSDefineMetaClassAndStructors(IONetworkStackUserClient, IOUserClient);
OSDefineMetaClassAndStructors(IONetworkStack, IOService);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void peach_wlan_static_init(void) __asm__("__GLOBAL__sub_I_PeachWLAN.cpp");
static unsigned registered, published;
static int initialize_classes(void)
{
    if (registered) return registered == 1 ? 0 : EIO;
    const char *names[] = {"IO80211Controller", "IONetworkInterface", "IO80211Interface",
                          "IONetworkStack", "IONetworkStackUserClient"};
    for (const char *name : names)
        if (OSMetaClass::getMetaClassWithName(name)) return EEXIST;
    registered = 2; /* one attempt, never rerun partially initialized globals */
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return ENOMEM;
    peach_wlan_static_init();
    OSReturn result = OSMetaClass::postModLoad(handle);
    if (result != kOSReturnSuccess) return EIO;
    for (const char *name : names)
        if (!OSMetaClass::getMetaClassWithName(name)) return ENOENT;
    registered = 1; return 0;
}
extern "C" int peach_wlan_publish(const char *name, const unsigned char *mac)
{
    if (!name || strcmp(name, "en0") || !mac || (mac[0] & 1) || published) return EINVAL;
    int error = initialize_classes(); if (error) return error;
    IOService *platform = IOService::getPlatform();
    IONetworkStack *stack = OSTypeAlloc(IONetworkStack);
    IO80211Controller *controller = OSTypeAlloc(IO80211Controller);
    IO80211Interface *interface = OSTypeAlloc(IO80211Interface);
    OSData *address = OSData::withBytes(mac, 6);
    bool sa = false, ss = false, ca = false, cs = false, ia = false, is = false;
    error = ENOMEM;
    if (!platform || !stack || !controller || !interface || !address) goto out;
    if (!stack->init() || !controller->init() || !interface->init()) goto out;
    stack->setName("IONetworkStack"); controller->setName("NezhaPeachWLAN"); interface->setName("NezhaPeachInterface");
    if (!controller->setProperty("IOMACAddress", address) || !controller->setProperty("IOBuiltin", true) ||
        !interface->setProperty("IOInterfaceName", name) || !interface->setProperty("BSD Name", name) ||
        !interface->setProperty("IOInterfaceNamePrefix", "en") ||
        !interface->setProperty("IOInterfaceType", 6ULL, 32) || !interface->setProperty("IOInterfaceUnit", 0ULL, 32) ||
        !interface->setProperty("IOBuiltin", true) || !interface->setProperty("IOPrimaryInterface", true) ||
        !interface->setProperty("IO80211InterfaceRole", "Infrastructure")) goto out;
    error = EIO;
    if (!(sa = stack->attach(platform)) || !(ss = stack->start(platform))) goto out;
    if (!(ca = controller->attach(platform)) || !(cs = controller->start(platform))) goto out;
    if (!(ia = interface->attach(controller)) || !(is = interface->start(controller))) goto out;
    stack->registerService(); controller->registerService(); interface->registerService();
    published = 1; error = 0;
    IOLog("NEZHA WIFI: real BSD en0 registry bridge published; association/IP separate\n");
out:
    if (error) {
        if (is) interface->stop(controller);
        if (ia) interface->detach(controller);
        if (cs) controller->stop(platform);
        if (ca) controller->detach(platform);
        if (ss) stack->stop(platform);
        if (sa) stack->detach(platform);
    }
    OSSafeReleaseNULL(address); OSSafeReleaseNULL(interface);
    OSSafeReleaseNULL(controller); OSSafeReleaseNULL(stack);
    return error;
}
