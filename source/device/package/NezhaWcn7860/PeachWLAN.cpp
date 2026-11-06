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
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSDictionary.h>
#include <kern/task.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include "NezhaWifiRegistry.h"
#include <mach/kmod.h>

static IOReturn register_owned_interface(OSObject *);
extern "C" int peach_net_verify_attached(void);
static int registry_caller(task_t task)
{
    proc_t process = task ? static_cast<proc_t>(get_bsdtask_info(task)) : nullptr;
    return process ? proc_pid(process) : 0;
}

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
    IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE {
        IOReturn result = IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator);
        if (result == kIOReturnSuccess) result = register_owned_interface(properties);
        else result = kIOReturnNotPrivileged;
        nezha_wifi_diagnostic_registry_event(NezhaWifiRegistryName,
            registry_caller(current_task()), 0, static_cast<int32_t>(result));
        return result;
    }
};
class IONetworkStack : public IOService {
    OSDeclareDefaultStructors(IONetworkStack);
public:
    IOReturn newUserClient(task_t task, void *security, UInt32 type,
                          OSDictionary *, IOUserClient **out) APPLE_KEXT_OVERRIDE {
        IOReturn result = create_client(task, security, type, out);
        nezha_wifi_diagnostic_registry_event(NezhaWifiRegistryOpen,
            registry_caller(task), type, static_cast<int32_t>(result));
        return result;
    }
private:
    IOReturn create_client(task_t task, void *security, UInt32 type, IOUserClient **out) {
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
static IO80211Interface *published_interface; /* retained until reboot */
static IONetworkStack *published_stack;
static IO80211Controller *published_controller;
extern "C" void peach_wlan_registry_state(NezhaWifiRegistryState *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out)); out->version = 1;
    IOService *root = OSDynamicCast(IOService, IORegistryEntry::getRegistryRoot());
    if (root) { out->root_busy = root->getBusyState(); out->valid |= 1; }
    if (__atomic_load_n(&published, __ATOMIC_ACQUIRE)) {
        out->stack_busy = published_stack->getBusyState();
        out->controller_busy = published_controller->getBusyState();
        out->interface_busy = published_interface->getBusyState();
        out->valid |= 14;
    }
}
/* 24A446 configd 0x100032840: command0/1 + 8-byte registry ID + unit.
 * This acknowledges only the interface already created in the real BSD stack.
 * It never renames another interface or treats an unknown operation as done. */
static IOReturn register_owned_interface(OSObject *properties)
{
    OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
    if (!dict || !dict->getCount() || dict->getCount() > 4) return kIOReturnBadArgument;
    OSObject *command_object = dict->getObject("IONetworkStackUserCommand");
    OSNumber *command = OSDynamicCast(OSNumber, command_object);
    if (command_object && !command) return kIOReturnBadArgument;
    uint64_t operation = command ? command->unsigned64BitValue() : 1;
    if (operation > 2) return kIOReturnUnsupported;
    unsigned known = !!command_object + !!dict->getObject("IORegistryEntryID") +
        !!dict->getObject("IOPathMatch") + !!dict->getObject("IOInterfaceUnit");
    if (known != dict->getCount()) return kIOReturnUnsupported;
    if (!__atomic_load_n(&published, __ATOMIC_ACQUIRE) || !published_interface || peach_net_verify_attached()) return kIOReturnNotReady;
    if (operation == 2) return known == 1 ? kIOReturnSuccess : kIOReturnBadArgument;
    OSNumber *unit = OSDynamicCast(OSNumber, dict->getObject("IOInterfaceUnit"));
    if (!unit) return kIOReturnBadArgument;
    if (unit->unsigned64BitValue() != 0) return kIOReturnNoSpace;
    OSObject *id_object = dict->getObject("IORegistryEntryID"), *path_object = dict->getObject("IOPathMatch");
    if (!id_object && !path_object) return kIOReturnBadArgument;
    if (id_object) {
        OSData *id = OSDynamicCast(OSData, id_object);
        if (!id || id->getLength() != sizeof(uint64_t)) return kIOReturnBadArgument;
        uint64_t value; memcpy(&value, id->getBytesNoCopy(), sizeof(value));
        if (value != published_interface->getRegistryEntryID()) return kIOReturnNoDevice;
    }
    if (path_object) {
        OSString *path = OSDynamicCast(OSString, path_object);
        if (!path || path->getLength() > 1024) return kIOReturnBadArgument;
        IORegistryEntry *entry = IORegistryEntry::fromPath(path->getCStringNoCopy());
        bool same = entry == published_interface;
        OSSafeReleaseNULL(entry);
        if (!same) return kIOReturnNoDevice;
    }
    return kIOReturnSuccess; /* confirmed same en0, no hardware or naming side effect */
}
static int class_exists(const char *name)
{
    const OSSymbol *symbol = OSSymbol::withCStringNoCopy(name);
    if (!symbol) return -ENOMEM;
    int exists = OSMetaClass::getMetaClassWithName(symbol) != nullptr;
    symbol->release(); return exists;
}
static int initialize_classes(void)
{
    if (registered) return registered == 1 ? 0 : EIO;
    const char *names[] = {"IO80211Controller", "IONetworkInterface", "IO80211Interface",
                          "IONetworkStack", "IONetworkStackUserClient"};
    for (const char *name : names) {
        int exists = class_exists(name);
        if (exists < 0) return -exists;
        if (exists) return EEXIST;
    }
    registered = 2; /* one attempt, never rerun partially initialized globals */
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return ENOMEM;
    peach_wlan_static_init();
    OSReturn result = OSMetaClass::postModLoad(handle);
    if (result != kOSReturnSuccess) return EIO;
    for (const char *name : names) {
        int exists = class_exists(name);
        if (exists < 0) return -exists;
        if (!exists) return ENOENT;
    }
    registered = 1; return 0;
}
extern "C" int peach_wlan_publish(const char *name, const unsigned char *mac)
{
    if (!name || strcmp(name, "en0") || !mac || (mac[0] & 1) || __atomic_load_n(&published, __ATOMIC_ACQUIRE)) return EINVAL;
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
    // Publish the owned identity before notifications can reach configd.
    interface->retain(); controller->retain(); stack->retain();
    published_interface = interface; published_controller = controller; published_stack = stack;
    __atomic_store_n(&published, 1, __ATOMIC_RELEASE);
    stack->registerService(); controller->registerService(); interface->registerService();
    error = 0;
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

/* Called only by the explicitly selected synthetic-QEMU worker. Exercise the
 * actual owned metaclasses and allocate/init/virtual-call/release paths. */
extern "C" int peach_wlan_offline_check(void)
{
    if (!__atomic_load_n(&published, __ATOMIC_ACQUIRE) || registered != 1 ||
        IO80211Interface::gMetaClass.getClassSize() != sizeof(IO80211Interface) ||
        IONetworkStack::gMetaClass.getClassSize() != sizeof(IONetworkStack)) return EIO;
    NezhaWifiRegistryState registry;
    peach_wlan_registry_state(&registry);
    if (registry.version != 1 || (registry.valid & 14) != 14) return EIO;
    for (unsigned i = 0; i < 16; i++) {
        IO80211Interface *interface = OSTypeAlloc(IO80211Interface);
        if (!interface) return ENOMEM;
        bool initialized = interface->init();
        IOUserClient *client = reinterpret_cast<IOUserClient *>(1);
        IOReturn result = initialized ? interface->newUserClient(nullptr, nullptr, 0, nullptr, &client) : kIOReturnError;
        interface->release();
        if (!initialized || result != kIOReturnUnsupported || client) return EIO;
    }
    OSDictionary *request = OSDictionary::withCapacity(3);
    OSNumber *zero = OSNumber::withNumber(0ULL, 32), *one = OSNumber::withNumber(1ULL, 32);
    uint64_t id = published_interface->getRegistryEntryID();
    OSData *identity = OSData::withBytes(&id, sizeof(id));
    int error = ENOMEM;
    if (request && zero && one && identity &&
        request->setObject("IONetworkStackUserCommand", zero) &&
        request->setObject("IOInterfaceUnit", zero) && request->setObject("IORegistryEntryID", identity)) {
        error = register_owned_interface(request) == kIOReturnSuccess ? 0 : EIO;
        request->setObject("IONetworkStackUserCommand", one);
        if (register_owned_interface(request) != kIOReturnSuccess) error = EIO;
        request->setObject("IOInterfaceUnit", one);
        if (register_owned_interface(request) != kIOReturnNoSpace) error = EIO;
        request->setObject("IOInterfaceUnit", zero); request->removeObject("IORegistryEntryID");
        if (register_owned_interface(request) != kIOReturnBadArgument) error = EIO;
    }
    OSSafeReleaseNULL(identity); OSSafeReleaseNULL(one); OSSafeReleaseNULL(zero); OSSafeReleaseNULL(request);
    if (error) return error;
    const unsigned char test_mac[6] = {2, 0, 0, 0, 0, 1};
    return peach_wlan_publish("en0", test_mac) == EINVAL ? 0 : EIO;
}
