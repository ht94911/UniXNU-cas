// SPDX-License-Identifier: BSD-2-Clause-Patent
// Device-mode subset of UniXNU/QcomDwc3 e4efe95c. SM8850 board resources
// and the inherited state / exact SID lease come from the nezha USB backend.
#include "OriginalUsbXdciABI.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/OSKextLib.h>
#include <kern/cpu_number.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

extern "C" {
int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);
int armcpu_spi_mask(uint32_t, bool);
int qcom_usb_apple_device_prepare(uint64_t, uint64_t);
int qcom_usb_apple_device_vbus(int);
void qcom_usb_apple_device_abort(void);
processor_t thread_bind(processor_t);
extern unsigned long gPhysBase, gPhysSize;
extern kmod_info_t g_kernel_kmod_info;
void initializeAppleUsbClasses() __asm__("__GLOBAL__sub_I_NezhaAppleUsbDevice.cpp");

// ABI leaves used through authenticated original AppleUSBXDCI fixups.
__attribute__((used, noinline)) void nezha_usb_store_le32(volatile uint32_t *where, uint32_t value) { *where = value; }
__attribute__((used, noinline)) void nezha_usb_store_le64(volatile uint64_t *where, uint64_t value) { *where = value; }
}

// Stock nezha DT /soc/ssusb@a600000/dwc3@a600000: core, SPI 0x85, SID 0x40.
// The base class expects the DWC3 global window, 0xc000 into this aperture.
constexpr uint64_t kWindow = 0x0a60c000;
constexpr uint32_t kInterrupt = 0x85 + 32;
constexpr unsigned kMap = 0x448, kRegisters = 0x450, kPeriodic = 0x58c, kParameters = 0x2628;

class NezhaAppleUsbNub : public IOService {
    OSDeclareDefaultStructors(NezhaAppleUsbNub)
    IOInterruptAction action = nullptr;
    OSObject *target = nullptr;
    void *refcon = nullptr;
    bool attached = false;
    static void primary(void *context)
    {
        auto self = static_cast<NezhaAppleUsbNub *>(context);
        auto handler = self->action;
        if (handler) handler(self->target, self->refcon, self, 0);
    }
public:
    IOReturn getInterruptType(int source, int *type) override
    {
        if (source != 0 || !type) return kIOReturnNoInterrupt;
        *type = kIOInterruptTypeLevel;
        return kIOReturnSuccess;
    }
    IOReturn registerInterrupt(int source, OSObject *owner, IOInterruptAction handler, void *context) override
    {
        if (source != 0 || !handler) return kIOReturnNoInterrupt;
        if (action) return kIOReturnNoResources;
        target = owner;
        refcon = context;
        action = handler;
        __asm__ volatile("dmb ish" ::: "memory");
        if (!attached) {
            if (armcpu_spi_attach(kInterrupt, false, primary, this, nullptr, nullptr) != kIOReturnSuccess) {
                action = nullptr;
                return kIOReturnNoResources;
            }
            attached = true;
        }
        armcpu_spi_mask(kInterrupt, true);
        return kIOReturnSuccess;
    }
    IOReturn unregisterInterrupt(int source) override
    {
        if (source != 0) return kIOReturnNoInterrupt;
        armcpu_spi_mask(kInterrupt, true);
        action = nullptr;
        __asm__ volatile("dmb ish" ::: "memory");
        return kIOReturnSuccess;
    }
    IOReturn enableInterrupt(int source) override
    {
        if (source != 0 || !attached || !action) return kIOReturnNoInterrupt;
        return armcpu_spi_mask(kInterrupt, false);
    }
    IOReturn disableInterrupt(int source) override
    {
        if (source != 0 || !attached) return kIOReturnNoInterrupt;
        return armcpu_spi_mask(kInterrupt, true);
    }
};
OSDefineMetaClassAndStructors(NezhaAppleUsbNub, IOService)

class NezhaAppleUsbXdci : public AppleUSBXDCI {
    OSDeclareDefaultStructors(NezhaAppleUsbXdci)
    template <class T> T &field(unsigned offset)
    {
        return *reinterpret_cast<T *>(reinterpret_cast<uint8_t *>(this) + offset);
    }
    volatile uint32_t &reg(unsigned offset) { return field<volatile uint32_t *>(kRegisters)[offset / 4]; }
    void write(unsigned offset, uint32_t value)
    {
        reg(offset) = value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
public:
    bool start(IOService *provider) override
    {
        auto map = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
        if (!map) return false;
        field<IOMemoryMap *>(kMap) = map;
        field<uint64_t>(kRegisters) = map->getVirtualAddress();
        return AppleUSBXDCI::start(provider);
    }
    bool XDCI_KEEP_CONFIGURATIONS() override { return false; }
    bool XDCI_TRANSPORT_ACTIVE() override { return true; }
    IOReturn XDCI_POWER_ON() override
    {
        return qcom_usb_apple_device_vbus(1) ? kIOReturnSuccess : kIOReturnIOError;
    }
    IOReturn XDCI_POWER_OFF() override
    {
        return qcom_usb_apple_device_vbus(0) ? kIOReturnSuccess : kIOReturnIOError;
    }
    void XDCI_PARAMETERS() override
    {
        auto p = &field<uint32_t>(kParameters);
        p[0] = 1; p[1] = 0; p[2] = 4; p[3] = 4;
    }
    IOReturn XDCI_CONTROLLER_RESET() override
    {
        const auto result = AppleUSBXDCI::XDCI_CONTROLLER_RESET();
        if (result != kIOReturnSuccess) return result;
        auto p = &field<uint32_t>(kParameters);
        const auto gctl = reg(0x110);
        p[1] = gctl >> 19;
        write(0x110, (gctl & ~(3u << 12)) | 2u << 12);
        const auto mdwidth = ((reg(0x140) >> 8) & 0xffu) | ((reg(0x158) >> 8) & 3u) << 8;
        const auto ram2_bytes = (reg(0x15c) >> 16) * mdwidth / 8;
        const auto packets = ram2_bytes > 40 ? (ram2_bytes - 40) / 1024 : 1;
        p[2] = packets > 16 ? 16 : packets ? packets : 1;
        write(0x200, reg(0x200) & ~(1u << 6 | 1u << 8));
        write(0x2c0, reg(0x2c0) & ~(1u << 17));
        // First profile is high speed on the inherited eUSB2 PHY.
        write(0x700, reg(0x700) & ~7u);
        return result;
    }
    uint32_t XDCI_PERIODIC_USAGE() override { return field<uint32_t>(kPeriodic) != 0; }
    void XDCI_PERIODIC_SET(uint32_t) override {}
    IOReturn activateCable() { return changePowerStateTo(4); }
};
OSDefineMetaClassAndStructors(NezhaAppleUsbXdci, AppleUSBXDCI)

namespace {
bool publishDevice()
{
    auto platform = IOService::getPlatform();
    auto nub = OSTypeAlloc(NezhaAppleUsbNub);
    auto range = IODeviceMemory::withRange(kWindow, 0x1000);
    auto ranges = range ? OSArray::withObjects((const OSObject **)&range, 1) : nullptr;
    const uint8_t zero = 0;
    auto bus = OSData::withBytes(&zero, sizeof(zero));
    const bool haveNub = platform && nub && range && ranges && bus && nub->init();
    if (haveNub) {
        nub->setDeviceMemory(ranges);
        nub->setName("usb-drd0");
        nub->setProperty("bus-number", bus);
        char configuration[] = "standardMuxOnly";
        nub->setProperty("configuration-string", configuration, sizeof(configuration));
        nub->setProperty("u1-init-disable", true);
        nub->setProperty("u2-init-disable", true);
    }
    OSSafeReleaseNULL(bus);
    OSSafeReleaseNULL(ranges);
    OSSafeReleaseNULL(range);
    if (!haveNub || !nub->attach(platform)) {
        OSSafeReleaseNULL(nub);
        return false;
    }
    nub->registerService();
    auto controller = OSTypeAlloc(NezhaAppleUsbXdci);
    if (!controller || !controller->init() || !controller->attach(nub)) {
        OSSafeReleaseNULL(controller);
        return false;
    }
    if (!controller->start(nub)) {
        nub->disableInterrupt(0);
        return false;
    }
    IOService::getResourceService()->setProperty("AppleUSBCableType", "USBHost");
    const auto power = controller->activateCable();
    IOLog("nezha-apple-usb: AppleUSBXDCI start succeeded, cable power result=%#x\n", static_cast<unsigned>(power));
    return true;
}

void appleUsbWorker(void *, wait_result_t)
{
    // The shared Qualcomm backend permits register and DMA setup only on the
    // master CPU. Match the normal CDC worker's migration before using it.
    const auto processor = cpu_to_processor(boot_cpu_id);
    if (processor != PROCESSOR_NULL) {
        thread_bind(processor);
        thread_block(THREAD_CONTINUE_NULL);
        if (cpu_number() == boot_cpu_id) {
            const auto load = OSKextLoadKextWithIdentifier("com.apple.driver.AppleUSBXDCI");
            bool started = false;
            if (load == kOSReturnSuccess || load == kOSKextReturnStartStopError) {
                auto context = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
                if (context) {
                    initializeAppleUsbClasses();
                    if (OSMetaClass::postModLoad(context) == kOSReturnSuccess &&
                        qcom_usb_apple_device_prepare(gPhysBase, gPhysSize)) {
                        started = publishDevice();
                        if (!started) qcom_usb_apple_device_abort();
                    }
                }
            }
            if (!started)
                IOLog("nezha-apple-usb: device startup failed, AppleUSBXDCI load=%#x\n", static_cast<unsigned>(load));
        } else {
            IOLog("nezha-apple-usb: could not enter the boot CPU context\n");
        }
        thread_bind(PROCESSOR_NULL);
    } else {
        IOLog("nezha-apple-usb: boot CPU processor is unavailable\n");
    }
    thread_terminate(current_thread());
}
} // namespace

extern "C" void nezha_apple_usb_start(void)
{
    int appsync = 0, apple = 0, debug = 0;
    if (!PE_parse_boot_argn("appsync", &appsync, sizeof(appsync)) || appsync != 1 ||
        !PE_parse_boot_argn("appsync-usb", &apple, sizeof(apple)) || apple != 1 ||
        (PE_parse_boot_argn("qcom-usb-debug", &debug, sizeof(debug)) && debug == 1))
        return;
    thread_t worker = THREAD_NULL;
    if (kernel_thread_start(appleUsbWorker, nullptr, &worker) == KERN_SUCCESS)
        thread_deallocate(worker);
}
