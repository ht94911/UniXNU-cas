// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real TCS3760 integer-lux front end; original24A446 IOHIDEventService ABI.
#ifdef NEZHA_ALS_HOST_MODEL
#include "als-host-shim.h"
#else
#include <IOKit/hid/IOHIDEventService.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDPrivateKeys.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#endif
#include "NezhaAlsState.h"

// The selected public EventService header supplies an opaque IOHIDEvent and
// uint32_t event type. Its full generated event headers are not public 2238
// inputs. Bind only the proven original integer constructor; event storage
// stays owned by original IOHIDFamily.
static constexpr IOHIDEventType nezhaAlsEventType = 12;
// Actual 24A446 start reads this key and skips its initial asynchronous
// registration only when the value is kOSBooleanFalse.
static constexpr const char *nezhaAlsRegisterServiceKey = "RegisterService";
#ifndef NEZHA_ALS_HOST_MODEL
extern "C" IOHIDEvent *nezha_als_original_ambient_event(
    uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
    __asm__("__ZN10IOHIDEvent23ambientLightSensorEventEyjjjjjj");
#endif
static IOHIDEvent *makeAmbientEvent(uint64_t timestamp, uint32_t lux) {
#ifdef NEZHA_ALS_HOST_MODEL
    return IOHIDEvent::ambientLightSensorEvent(timestamp, lux);
#else
    return nezha_als_original_ambient_event(timestamp, lux, 0, 0, 0, 0, 0);
#endif
}
static void releaseAmbientEvent(IOHIDEvent *event) {
#ifdef NEZHA_ALS_HOST_MODEL
    event->release();
#else
    // Original IOHIDEvent has one public OSObject base at offset 0. Its release
    // uses OSMetaClassBase PAC: vptr 0xcda1, slot 5 with diversity 0x3a87.
    reinterpret_cast<OSObject *>(event)->release();
#endif
}

extern "C" {
volatile uint32_t nezha_als_stage, nezha_als_base_bytes, nezha_als_owned_bytes;
volatile uint32_t nezha_als_conforms, nezha_als_published;
volatile int32_t nezha_als_meta_status = -1;
}

#ifndef NEZHA_ALS_HOST_MODEL
static_assert(sizeof(IOService) == 0x88, "selected27 IOService layout");
static_assert(sizeof(IOHIDEventService) == 0xc8, "actual24A446 event-service layout");
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void nezha_als_static_init(void) __asm__("__GLOBAL__sub_I_NezhaAlsFrontend.cpp");
#else
extern "C" void nezha_als_static_init(void);
#endif

class NativeNezhaAls : public IOHIDEventService {
    OSDeclareDefaultStructors(NativeNezhaAls)
    IOWorkLoop *loop = nullptr;
    IOCommandGate *gate = nullptr;
    IOTimerEventSource *timer = nullptr;
    IOLock *stateLock = nullptr;
    struct nezha_als_state state = {};
    struct nezha_als_backend_ops backend = {};
    uint64_t dispatched = 0;
    uint32_t intervalUs = 0;
    bool published = false;
    bool stopped = false;
    bool paused = false;

    static IOReturn beginAction(OSObject *owner, void *arg, void *, void *, void *) {
        auto self = static_cast<NativeNezhaAls *>(owner);
        if (self->stopped) return kIOReturnNotReady;
        IOLockLock(self->stateLock);
        bool ok = nezha_als_begin(&self->state, *static_cast<uint64_t *>(arg));
        IOLockUnlock(self->stateLock);
        if (ok) self->timer->cancelTimeout();
        return ok ? kIOReturnSuccess : kIOReturnBadArgument;
    }
    struct ReadyArg { uint64_t epoch; bool ready; };
    static IOReturn readyAction(OSObject *owner, void *arg, void *, void *, void *) {
        auto self = static_cast<NativeNezhaAls *>(owner);
        auto ready = static_cast<ReadyArg *>(arg);
        if (self->stopped) return kIOReturnNotReady;
        IOLockLock(self->stateLock);
        bool ok = nezha_als_ready(&self->state, ready->epoch, ready->ready);
        IOLockUnlock(self->stateLock);
        if (ok && !ready->ready) self->timer->cancelTimeout();
        return ok ? kIOReturnSuccess : kIOReturnBadArgument;
    }
    static IOReturn sampleAction(OSObject *owner, void *arg, void *, void *, void *) {
        auto self = static_cast<NativeNezhaAls *>(owner);
        if (self->stopped || self->paused) return kIOReturnNotReady;
        IOLockLock(self->stateLock);
        int result = nezha_als_submit(&self->state, static_cast<nezha_als_sample *>(arg));
        IOLockUnlock(self->stateLock);
        if (result == 1) self->timer->setTimeoutUS(1);
        else if (result == -1) self->timer->cancelTimeout();
        return result == 1 ? kIOReturnSuccess : kIOReturnBadArgument;
    }
    struct WorkerArg { int (*action)(void *); void *context; int *result; };
    static IOReturn workerAction(OSObject *owner, void *arg, void *, void *, void *) {
        auto self = static_cast<NativeNezhaAls *>(owner);
        if (self->stopped) return kIOReturnNotReady;
        auto operation = static_cast<WorkerArg *>(arg);
        // Selected XNU IOCommandGate runs actions on the calling thread and
        // uses a recursive workloop lock. Nested ready/sample stay on the sole
        // transport worker. No state/queue lock is held across their gate entry.
        *operation->result = operation->action(operation->context);
        return kIOReturnSuccess;
    }
    static IOReturn intervalAction(OSObject *owner, void *arg, void *, void *, void *) {
        auto self = static_cast<NativeNezhaAls *>(owner);
        uint32_t interval = *static_cast<uint32_t *>(arg);
        if (self->stopped) return kIOReturnNotReady;
        if (!self->backend.request_interval) return kIOReturnUnsupported;
        IOReturn result = self->backend.request_interval(self->backend.context, interval);
        if (result != kIOReturnSuccess) return result;
        IOLockLock(self->stateLock);
        self->intervalUs = interval;
        self->paused = interval == 0;
        if (self->paused) {
            self->state.latest.valid = false;
            self->state.pending = false;
        }
        IOLockUnlock(self->stateLock);
        if (!interval) self->timer->cancelTimeout();
        return self->setProperty(kIOHIDReportIntervalKey, interval, 32) ?
            kIOReturnSuccess : kIOReturnNoMemory;
    }
    static IOReturn stopAction(OSObject *owner, void *, void *, void *, void *) {
        auto self = static_cast<NativeNezhaAls *>(owner);
        if (self->stopped) return kIOReturnNotReady;
        IOLockLock(self->stateLock);
        self->stopped = true;
        self->paused = true;
        self->state.ready = false;
        self->state.latest.valid = false;
        self->state.pending = false;
        IOLockUnlock(self->stateLock);
        self->timer->cancelTimeout();
        return kIOReturnSuccess;
    }
    void process() {
        struct nezha_als_sample sample;
        IOLockLock(stateLock);
        bool valid = !stopped && !paused && state.ready && state.latest.valid && state.pending;
        sample = state.latest;
        state.pending = false;
        IOLockUnlock(stateLock);
        if (!valid) return;
        IOHIDEvent *event = makeAmbientEvent(sample.timestamp, sample.integer_lux);
        if (!event) return;
        // Backend mutations and timer delivery share the workloop gate. The
        // sample is cached before publication so the original client's initial
        // copyEvent can obtain actual data even before its event callback opens.
        bool first = !published;
        if (first) {
            IOLockLock(stateLock);
            published = true;
            IOLockUnlock(stateLock);
            registerService();
            nezha_als_published = 1;
            nezha_als_stage = 2;
            printf("NEZHA ALS: real integer-lux service registered;epoch=%llu seq=%llu ticks=%llu lux=%u\n",
                (unsigned long long)sample.epoch, (unsigned long long)sample.sequence,
                (unsigned long long)sample.timestamp, sample.integer_lux);
        }
        dispatchEvent(event);
        releaseAmbientEvent(event);
        IOLockLock(stateLock);
        dispatched++;
        IOLockUnlock(stateLock);
    }
    static void fired(OSObject *owner, IOTimerEventSource *) {
        static_cast<NativeNezhaAls *>(owner)->process();
    }
protected:
    UInt32 getPrimaryUsagePage() override { return 0x20; }
    UInt32 getPrimaryUsage() override { return 0x41; }
    UInt32 getReportInterval() override {
        IOLockLock(stateLock);
        uint32_t value = intervalUs;
        IOLockUnlock(stateLock);
        return value;
    }
    bool handleStart(IOService *provider) override {
        return IOHIDEventService::handleStart(provider) &&
            setProperty(kIOHIDBuiltInKey, kOSBooleanTrue);
    }
public:
    bool init(OSDictionary *properties = nullptr) override {
        if (!IOHIDEventService::init(properties)) return false;
        stateLock = IOLockAlloc();
        loop = IOWorkLoop::workLoop();
        if (!stateLock || !loop) return false;
        gate = IOCommandGate::commandGate(this);
        timer = IOTimerEventSource::timerEventSource(this, &NativeNezhaAls::fired);
        return gate && timer && loop->addEventSource(gate) == kIOReturnSuccess &&
            loop->addEventSource(timer) == kIOReturnSuccess &&
            setProperty(nezhaAlsRegisterServiceKey, kOSBooleanFalse);
    }
    bool configure(const struct nezha_als_backend_ops *ops, uint64_t epoch) {
        backend = *ops;
        intervalUs = ops->initial_interval_us;
        return nezha_als_begin(&state, epoch);
    }
    IOWorkLoop *getWorkLoop() const override { return loop; }
    bool begin(uint64_t epoch) {
        return gate->runAction(&NativeNezhaAls::beginAction, &epoch) == kIOReturnSuccess;
    }
    bool ready(uint64_t epoch, bool ready) {
        ReadyArg arg = {epoch, ready};
        return gate->runAction(&NativeNezhaAls::readyAction, &arg) == kIOReturnSuccess;
    }
    bool submit(const struct nezha_als_sample *sample) {
        return gate->runAction(&NativeNezhaAls::sampleAction,
                               const_cast<nezha_als_sample *>(sample)) == kIOReturnSuccess;
    }
    bool worker(int (*action)(void *), void *context, int *result) {
        WorkerArg arg = {action, context, result};
        return gate->runAction(&NativeNezhaAls::workerAction, &arg) == kIOReturnSuccess;
    }
    void snapshot(struct nezha_als_status *status) {
        IOLockLock(stateLock);
        status->latest = state.latest;
        status->backend_ready = state.ready;
        status->dispatched = dispatched;
        status->interval_us = intervalUs;
        status->published = published;
        status->stopped = stopped;
        status->sampling_paused = paused;
        IOLockUnlock(stateLock);
    }
    IOHIDEvent *copyEvent(IOHIDEventType type, IOHIDEvent *, IOOptionBits) override {
        if (type != nezhaAlsEventType) return nullptr;
        IOLockLock(stateLock);
        struct nezha_als_sample sample = state.latest;
        bool valid = published && !stopped && !paused && state.ready && sample.valid;
        IOLockUnlock(stateLock);
        if (!valid) return nullptr;
        IOHIDEvent *event = makeAmbientEvent(sample.timestamp, sample.integer_lux);
        if (!event) return nullptr;
        IOLockLock(stateLock);
        valid = !stopped && !paused && state.ready && state.latest.valid &&
            state.latest.epoch == sample.epoch && state.latest.sequence == sample.sequence;
        IOLockUnlock(stateLock);
        if (!valid) { releaseAmbientEvent(event); return nullptr; }
        return event;
    }
    IOReturn setProperties(OSObject *properties) override {
        auto dictionary = OSDynamicCast(OSDictionary, properties);
        if (!dictionary) return kIOReturnBadArgument;
        auto interval = OSDynamicCast(OSNumber, dictionary->getObject(kIOHIDReportIntervalKey));
        if (!interval) return dictionary->getObject(kIOHIDReportIntervalKey) ?
            kIOReturnBadArgument : kIOReturnUnsupported;
        if (interval->unsigned64BitValue() > UINT32_MAX) return kIOReturnBadArgument;
        uint32_t value = interval->unsigned32BitValue();
        return gate->runAction(&NativeNezhaAls::intervalAction, &value);
    }
    void stop(IOService *provider) override {
        if (gate->runAction(&NativeNezhaAls::stopAction) == kIOReturnSuccess) {
            IOHIDEventService::stop(provider);
            nezha_als_stage = 3;
        }
    }
    void shutdown() { stop(getProvider()); }
    void free() override {
        if (timer) { timer->cancelTimeout(); if (loop) loop->removeEventSource(timer); timer->release(); timer = nullptr; }
        if (gate) { if (loop) loop->removeEventSource(gate); gate->release(); gate = nullptr; }
        if (loop) { loop->release(); loop = nullptr; }
        if (stateLock) IOLockFree(stateLock);
        IOHIDEventService::free();
    }
};
OSDefineMetaClassAndStructors(NativeNezhaAls, IOHIDEventService)

// The unique backend owner prepares once on a serialized control path. The
// boot-lifetime reference remains after stop; later backend callbacks reject
// stopped state and cannot dereference a freed singleton.
static NativeNezhaAls *frontend;
static bool preparationAttempted;
extern "C" bool nezha_als_frontend_prepare(void *provider,
                    const struct nezha_als_backend_ops *ops, uint64_t epoch) {
    if (!provider || !ops || !ops->initial_interval_us || !epoch || frontend || preparationAttempted) return false;
    const OSSymbol *name = OSSymbol::withCStringNoCopy("IOHIDEventService");
    if (!name) return false;
    const OSMetaClass *base = OSMetaClass::getMetaClassWithName(name);
    name->release();
    if (!base || base->getClassSize() != 0xc8) return false;
    preparationAttempted = true;
    nezha_als_base_bytes = base->getClassSize();
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return false;
    nezha_als_static_init();
    nezha_als_meta_status = OSMetaClass::postModLoad(handle);
    if (nezha_als_meta_status) return false;
    auto service = new NativeNezhaAls;
    if (!service) return false;
    nezha_als_owned_bytes = service->getMetaClass()->getClassSize();
    nezha_als_conforms = service->metaCast("IOHIDEventService") != nullptr;
    if (!nezha_als_conforms || !service->init() || !service->configure(ops, epoch) ||
        !service->attach(static_cast<IOService *>(provider))) {
        service->release();
        return false;
    }
    if (!service->start(static_cast<IOService *>(provider))) {
        service->detach(static_cast<IOService *>(provider));
        service->release();
        return false;
    }
    __atomic_store_n(&frontend, service, __ATOMIC_RELEASE);
    nezha_als_stage = 1;
    return true;
}
extern "C" bool nezha_als_frontend_begin(uint64_t epoch) {
    auto service = __atomic_load_n(&frontend, __ATOMIC_ACQUIRE);
    return service && service->begin(epoch);
}
extern "C" bool nezha_als_frontend_ready(uint64_t epoch, bool ready) {
    auto service = __atomic_load_n(&frontend, __ATOMIC_ACQUIRE);
    return service && service->ready(epoch, ready);
}
extern "C" bool nezha_als_frontend_sample(const struct nezha_als_sample *sample) {
    auto service = __atomic_load_n(&frontend, __ATOMIC_ACQUIRE);
    return service && sample && service->submit(sample);
}
extern "C" bool nezha_als_frontend_worker(int (*action)(void *), void *context, int *result) {
    auto service = __atomic_load_n(&frontend, __ATOMIC_ACQUIRE);
    return service && action && result && service->worker(action, context, result);
}
extern "C" bool nezha_als_frontend_status(struct nezha_als_status *status) {
    auto service = __atomic_load_n(&frontend, __ATOMIC_ACQUIRE);
    if (!service || !status) return false;
    service->snapshot(status);
    return true;
}
extern "C" void nezha_als_frontend_stop(void) {
    auto service = __atomic_load_n(&frontend, __ATOMIC_ACQUIRE);
    if (service) service->shutdown();
}
