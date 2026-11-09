// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native XNU IOPMPowerSource, following ShengBattery/BatteryProbe. Compile with
// the selected 27 kernel's recorded C++ flags; no old object or Apple vptr patch.
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include "NezhaPower.h"

extern kmod_info_t g_kernel_kmod_info;
extern "C" void np_battery_static_init(void) __asm__("__GLOBAL__sub_I_NezhaBatteryService.cpp");

class NezhaPowerSource : public IOPMPowerSource {
    OSDeclareDefaultStructors(NezhaPowerSource)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    bool published = false;
    bool timerAdded = false;
    bool pmInitialized = false;
    unsigned pmPending = 0, pmTarget = 1, shutdownKind = 0;
    bool pmErrorReported = false;

    void servicePowerTransition() {
        const int state = nezha_power27_lifecycle();
        const unsigned shutdown = __atomic_load_n(&shutdownKind, __ATOMIC_ACQUIRE);
        if (shutdown) {
            // The worker has stopped touching hardware and retained its
            // mappings. A diagnosed cleanup failure must not turn a normal
            // restart into IOPlatformExpert's halt/restart timeout panic.
            if (nezha_power27_worker_exited()&&
                __atomic_exchange_n(&shutdownKind, 0u, __ATOMIC_ACQ_REL)) {
                IOLog("NEZHA POWER: normal shutdown completion, lifecycle=%d; mappings retained\n",state);
                IOPMPowerSource::systemWillShutdown(shutdown);
            }
            return;
        }
        if (state < 0 && !pmErrorReported) {
            pmErrorReported = true;
            IOLog("NEZHA POWER: quiesce/resume failed; no completion acknowledgement, resources retained\n");
        }
        if (!__atomic_load_n(&pmPending, __ATOMIC_ACQUIRE)) return;
        const unsigned target = __atomic_load_n(&pmTarget, __ATOMIC_ACQUIRE);
        if ((target ? state == 1 : state == 3) &&
            __atomic_exchange_n(&pmPending, 0u, __ATOMIC_ACQ_REL)) acknowledgeSetPowerState();
        // A timeout/failure is not an acknowledged transition. The selected
        // platform must retain this client's mappings/votes and diagnose the
        // PM failure; this driver cannot guarantee whole-system recovery.
    }

    void unsignedNumber(const char *name, uint64_t v, unsigned width = 32) {
        const OSSymbol *k = OSSymbol::withCString(name);
        OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(v), width);
        if (k && n) setPSProperty(k, n);
        OSSafeReleaseNULL(n); OSSafeReleaseNULL(k);
    }
    void number(const char *name, int64_t v, unsigned width = 32) {
        unsignedNumber(name, static_cast<uint64_t>(v), width);
    }
    void flag(const char *name, bool value) {
        const OSSymbol *k = OSSymbol::withCString(name);
        if (k) setPSProperty(k, value ? kOSBooleanTrue : kOSBooleanFalse);
        OSSafeReleaseNULL(k);
    }
    void forget(const char *name) {
        // updateStatus copies existing dictionary keys; it does not remove a
        // key deleted from the dictionary. Remove it from BOTH stores.
        if (properties && properties->getObject(name)) {
            properties->removeObject(name); removeProperty(name);
            settingsChangedSinceUpdate = true;
        }
    }
    void optional(const char *key, uint32_t valid, enum np_field f, int64_t value) {
        if (valid & NP_BIT(f)) number(key, value); else forget(key);
    }
    void apply(const NpSnapshot &s) {
        const auto &g = s.gauge;
        const bool gauge = (s.valid & NP_GAUGE_REQUIRED) == NP_GAUGE_REQUIRED;
        if (!published && !gauge) return;
        flag("BatteryDataStale", !gauge);
        number("NezhaPowerValidFields", s.valid);
        number("NezhaPowerFaults", s.faults);
        unsignedNumber("NezhaPowerSampleMonotonicMs", s.at_ms, 64);
        unsignedNumber("NezhaPowerSampleGeneration", s.generation, 64);
        if (gauge) {
            setBatteryInstalled(g.present != 0);
            // powerd consumes this ratio. Use the measured RSOC in a 0..100
            // presentation scale, as the iOS power-source path expects; the
            // separately named AppleRaw* capacity properties below remain mAh.
            // No pandora97% rescale or unverified Xiaomi UI compensation.
            setCurrentCapacity(static_cast<unsigned>(g.soc)); setMaxCapacity(100);
            number("NezhaGaugePercent", g.soc);
        } else {
            // Withdraw the stale capacities. Keep presence as last observed;
            // transport loss is not proof of a physically removed battery.
            forget(kIOPMPSCurrentCapacityKey); forget(kIOPMPSMaxCapacityKey);
            forget("NezhaGaugePercent");
        }
        // An unqualified charger schema or a failed GET is not a physical
        // battery fault. Keep telemetry diagnostics separate from ErrorCondition.
        if (!gauge) {
            const OSSymbol *error = OSSymbol::withCString("Battery data unavailable");
            if (error) setErrorCondition(const_cast<OSSymbol *>(error));
            OSSafeReleaseNULL(error);
        } else forget(kIOPMPSErrorConditionKey);
        if (s.valid & NP_BIT(NP_EXTERNAL)) {
            setExternalConnected(s.external != 0);
            flag("AppleRawExternalConnected", s.external != 0);
        } else {
            forget(kIOPMPSExternalConnectedKey); forget("AppleRawExternalConnected");
        }
        // External supply alone does not prove it can charge the battery.
        forget(kIOPMPSExternalChargeCapableKey);
        if (s.valid & NP_BIT(NP_CHARGING)) setIsCharging(s.charging != 0);
        else forget(kIOPMPSIsChargingKey);
        if (s.valid & NP_BIT(NP_CHARGE_DONE)) flag("FullyCharged", s.fully_charged != 0);
        else forget("FullyCharged");
        // No invented 10%/2% thresholds. These require the selected stock low-
        // battery policy; the OS may still use the published real capacity.
        forget(kIOPMPSAtWarnLevelKey); forget(kIOPMPSAtCriticalLevelKey);
        optional(kIOPMPSVoltageKey, s.valid, NP_VOLTAGE, g.voltage_mv);
        optional(kIOPMPSAmperageKey, s.valid, NP_CURRENT, g.current_ma);
        optional("InstantAmperage", s.valid, NP_CURRENT, g.current_ma);
        optional("Temperature", s.valid, NP_TEMPERATURE, g.temperature_centi_c);
        optional("AppleRawCurrentCapacity", s.valid, NP_REMAINING, g.remaining_mah);
        optional("AppleRawMaxCapacity", s.valid, NP_FULL_CAPACITY, g.full_mah);
        optional("NominalChargeCapacity", s.valid, NP_FULL_CAPACITY, g.full_mah);
        optional("DesignCapacity", s.valid, NP_DESIGN, g.design_mah);
        optional(kIOPMPSCycleCountKey, s.valid, NP_CYCLES, g.cycles);
        setTimeRemaining(0xffff);
        // Measured input power is NOT AdapterDetails.Watts (rated power).
        if (s.usb_power_valid) number("NezhaMeasuredUsbMicrowatts", s.usb_power_uw, 64);
        else forget("NezhaMeasuredUsbMicrowatts");
        if (!published) {
            updateStatus(); // registry contains the first snapshot before discovery
            registerService(); published = true;
            IOLog("NEZHA POWER: native IOPMPowerSource published; soc=%d valid=%x generation=%llu\n",
                  g.soc,s.valid,(unsigned long long)s.generation);
        } else updateStatus();
    }
    static void tick(OSObject *owner, IOTimerEventSource *sender) {
        auto *self = OSDynamicCast(NezhaPowerSource, owner);
        if (!self) return;
        self->servicePowerTransition();
        NpSnapshot snapshot;
        if (np_snapshot_read(&snapshot)) self->apply(snapshot);
        sender->setTimeoutMS(1000); // reevaluate expiry even without a new sample
    }
public:
    IOReturn setPowerState(unsigned long ordinal, IOService *device) override {
        (void)device;
        if (ordinal > 1) return kIOPMAckImplied;
        __atomic_store_n(&pmTarget, static_cast<unsigned>(ordinal), __ATOMIC_RELEASE);
        if (ordinal) nezha_power27_resume(); else nezha_power27_suspend();
        const int state = nezha_power27_lifecycle();
        if (ordinal ? state == 1 : state == 3) return kIOPMAckImplied;
        __atomic_store_n(&pmPending, 1u, __ATOMIC_RELEASE);
        return 10000000; // microseconds, async ACK only after worker confirmation
    }
    void systemWillShutdown(IOOptionBits specifier) override {
        if (specifier != kIOMessageSystemWillPowerOff && specifier != kIOMessageSystemWillRestart) {
            IOPMPowerSource::systemWillShutdown(specifier); return;
        }
        nezha_power27_stop();
        __atomic_store_n(&shutdownKind, static_cast<unsigned>(specifier), __ATOMIC_RELEASE);
        servicePowerTransition();
    }
    bool start(IOService *provider) override {
        if (!IOPMPowerSource::start(provider)) return false;
        loop = IOWorkLoop::workLoop();
        timer = IOTimerEventSource::timerEventSource(this, &NezhaPowerSource::tick);
        if (!loop || !timer || loop->addEventSource(timer) != kIOReturnSuccess) {
            IOPMPowerSource::stop(provider); return false;
        }
        timerAdded = true;
        IOPMPowerState states[2] = {};
        states[0].version = states[1].version = 1;
        states[1].capabilityFlags = kIOPMDeviceUsable;
        states[1].outputPowerCharacter = states[1].inputPowerRequirement = kIOPMPowerOn;
        PMinit(); pmInitialized = true;
        provider->joinPMtree(this);
        if (registerPowerDriver(this, states, 2) != kIOReturnSuccess) {
            PMstop(); pmInitialized = false;
            loop->removeEventSource(timer); timerAdded = false;
            IOPMPowerSource::stop(provider); return false;
        }
        changePowerStateTo(1);
        timer->setTimeoutMS(1); return true;
    }
    void stop(IOService *provider) override {
        nezha_power27_stop();
        if (pmInitialized) { PMstop(); pmInitialized = false; }
        if (timer) timer->cancelTimeout();
        if (timerAdded) { loop->removeEventSource(timer); timerAdded = false; }
        IOPMPowerSource::stop(provider);
    }
    void free() override {
        if (pmInitialized) { PMstop(); pmInitialized = false; }
        if (timer) timer->cancelTimeout();
        if (timerAdded) { loop->removeEventSource(timer); timerAdded = false; }
        OSSafeReleaseNULL(timer); OSSafeReleaseNULL(loop);
        IOPMPowerSource::free();
    }
};
OSDefineMetaClassAndStructors(NezhaPowerSource, IOPMPowerSource)

// The module coordinator serializes this call AFTER native class registration
// and outside an unrelated kext's preModLoad/postModLoad critical section.
extern "C" IOService *np_battery_service_create(void) {
    static unsigned registration;
    if (registration == 2) return nullptr;
    if (!registration) {
        void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if (!handle) return nullptr;
        registration = 2; // do not rerun an initializer after a failed postModLoad
        np_battery_static_init();
        if (OSMetaClass::postModLoad(handle)) return nullptr;
        registration = 1;
    }
    auto *service = OSTypeAlloc(NezhaPowerSource);
    auto *provider = IOService::getPlatform();
    if (!service || !provider || !service->init()) { OSSafeReleaseNULL(service); return nullptr; }
    if (!service->attach(provider)) { service->release(); return nullptr; }
    if (!service->start(provider)) { service->detach(provider); service->release(); return nullptr; }
    return service; // coordinator owns reference, stop/detach/release at teardown
}
