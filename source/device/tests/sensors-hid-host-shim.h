// SPDX-License-Identifier: BSD-2-Clause-Patent
// Kernel-interface model only. Production builds do not include this header.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <cassert>
using UInt32 = uint32_t;
using UInt64 = uint64_t;
using IOOptionBits = uint32_t;
using IOHIDEventType = uint32_t;
using IOReturn = int32_t;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnBadArgument = -1;
constexpr IOReturn kIOReturnNotReady = -2, kIOReturnUnsupported = -3, kIOReturnNoMemory = -4, kIOReturnNoResources = -5;
constexpr IOHIDEventType kIOHIDEventTypeAmbientLightSensor = 12;
constexpr const char *kIOHIDRegisterServiceKey = "RegisterService";
constexpr const char *kIOHIDBuiltInKey = "Built-In";
constexpr const char *kIOHIDReportIntervalKey = "ReportInterval";
inline unsigned hostRegistrations, hostStops, hostDetached, hostStaticInitializers;
inline uint32_t hostRegisteredUsagePage, hostRegisteredUsage;
inline uint32_t hostBaseSize = 200;
inline uint32_t hostDeviceBaseSize = 208;
inline int32_t hostPostResult;
inline bool hostLoopFailure, hostStartFailure, hostIntervalPropertyFailure;
inline std::atomic<bool> hostEventFailure{false};
inline std::function<void()> hostConstructorHook;
inline std::function<void()> hostGateEntryHook;
inline std::function<void()> hostDispatchHook;
struct OSSymbol;
struct OSMetaClass {
    uint32_t size;
    uint32_t getClassSize() const { return size; }
    static const OSMetaClass *getMetaClassWithName(const OSSymbol *);
    static void *preModLoad(const char *) { return reinterpret_cast<void *>(1); }
    static int32_t postModLoad(void *) { return hostPostResult; }
};
struct kmod_info_t { const char *name; };
extern "C" kmod_info_t g_kernel_kmod_info;
struct OSObject {
    unsigned references = 1;
    virtual ~OSObject() = default;
    virtual void free() { delete this; }
    void retain() { references++; }
    void release() { if (--references == 0) free(); }
    virtual const OSMetaClass *getMetaClass() const {
        static OSMetaClass m{200}; return &m;
    }
};
struct OSSymbol : OSObject {
    std::string name;
    static const OSSymbol *withCStringNoCopy(const char *name) { auto s=new OSSymbol;s->name=name;return s; }
    void release() const { const_cast<OSSymbol *>(this)->OSObject::release(); }
};
inline const OSMetaClass *OSMetaClass::getMetaClassWithName(const OSSymbol *symbol){
    static OSMetaClass meta{200};meta.size=symbol->name=="IOHIDDevice"?hostDeviceBaseSize:hostBaseSize;return &meta;
}
struct OSBoolean : OSObject { bool value; explicit OSBoolean(bool v) : value(v) {} };
inline OSBoolean hostTrue{true}, hostFalse{false};
inline OSBoolean *kOSBooleanTrue = &hostTrue, *kOSBooleanFalse = &hostFalse;
struct OSNumber : OSObject {
    uint64_t value;
    explicit OSNumber(uint64_t n) : value(n) {}
    static OSNumber *withNumber(uint64_t v, unsigned) { return new OSNumber(v); }
    uint64_t unsigned64BitValue() const { return value; }
    uint32_t unsigned32BitValue() const { return static_cast<uint32_t>(value); }
};
struct OSDictionary : OSObject {
    std::map<std::string, OSObject *> objects;
    ~OSDictionary() override { for (auto &entry : objects) entry.second->release(); }
    OSObject *getObject(const char *key) { auto i = objects.find(key); return i == objects.end() ? nullptr : i->second; }
    void setObject(const char *key, OSObject *object) {
        object->retain(); auto old = getObject(key); if (old) old->release(); objects[key] = object;
    }
};
#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
#define OSDeclareDefaultStructors(type) public: const OSMetaClass *getMetaClass() const override { static OSMetaClass meta{sizeof(type)}; return &meta; } private:
#define OSDefineMetaClassAndStructors(type, base)
using IOLock = std::mutex;
inline IOLock *IOLockAlloc() { return new IOLock; }
inline void IOLockFree(IOLock *lock) { delete lock; }
inline void IOLockLock(IOLock *lock) { lock->lock(); }
inline void IOLockUnlock(IOLock *lock) { lock->unlock(); }
struct IOWorkLoop;
struct HostEventSource : OSObject { IOWorkLoop *loop = nullptr; };
struct IOWorkLoop : OSObject {
    std::recursive_mutex mutex;
    static IOWorkLoop *workLoop() { return hostLoopFailure ? nullptr : new IOWorkLoop; }
    IOReturn addEventSource(HostEventSource *source) { source->loop = this; return 0; }
    void removeEventSource(HostEventSource *source) { source->loop = nullptr; }
};
struct IOCommandGate : HostEventSource {
    using Action = IOReturn (*)(OSObject *, void *, void *, void *, void *);
    OSObject *owner;
    explicit IOCommandGate(OSObject *o) : owner(o) {}
    static IOCommandGate *commandGate(OSObject *o) { return new IOCommandGate(o); }
    IOReturn runAction(Action action, void *a = nullptr, void *b = nullptr,
                       void *c = nullptr, void *d = nullptr) {
        if (hostGateEntryHook) hostGateEntryHook();
        std::lock_guard<std::recursive_mutex> held(loop->mutex);
        return action(owner, a, b, c, d);
    }
};
struct IOTimerEventSource;
inline IOTimerEventSource *hostTimer;
struct IOTimerEventSource : HostEventSource {
    using Action = void (*)(OSObject *, IOTimerEventSource *);
    OSObject *owner;
    Action action;
    std::atomic<bool> scheduled{false};
    IOTimerEventSource(OSObject *o, Action a) : owner(o), action(a) { hostTimer = this; }
    static IOTimerEventSource *timerEventSource(OSObject *o, Action a) { return new IOTimerEventSource(o, a); }
    void setTimeoutUS(unsigned) { scheduled.store(true); }
    void cancelTimeout() { scheduled.store(false); }
    void fire() {
        if (!scheduled.exchange(false)) return;
        std::lock_guard<std::recursive_mutex> held(loop->mutex);
        action(owner, this);
    }
};
struct IOHIDEvent : OSObject {
    uint64_t timestamp;
    uint32_t level;
    uint32_t type = 12;
    int32_t xyz[3]={};
    std::vector<uint8_t> payload;
    IOHIDEvent(uint64_t t, uint32_t l) : timestamp(t), level(l) {}
    static IOHIDEvent *ambientLightSensorEvent(uint64_t t, uint32_t l,
        uint32_t = 0, uint32_t = 0, uint32_t = 0, uint32_t = 0, uint32_t = 0) {
        if (hostConstructorHook) hostConstructorHook();
        return hostEventFailure.load() ? nullptr : new IOHIDEvent(t, l);
    }
};
struct HostDispatch { uint64_t timestamp; uint32_t lux; };
inline std::vector<HostDispatch> hostDispatches;
struct IOService : OSObject {
    OSDictionary properties;
    IOService *provider = nullptr;
    virtual bool init(OSDictionary * = nullptr) { return true; }
    virtual bool attach(IOService *p) { provider = p; return true; }
    virtual void detach(IOService *) { provider = nullptr; hostDetached++; }
    virtual bool start(IOService *) { return true; }
    virtual void stop(IOService *) { hostStops++; }
    virtual IOWorkLoop *getWorkLoop() const { return nullptr; }
    static IOService *getPlatform() { static IOService platform; return &platform; }
    virtual IOReturn setProperties(OSObject *) { return kIOReturnUnsupported; }
    IOService *getProvider() { return provider; }
    virtual void *metaCast(const char *) { return this; }
    bool setProperty(const char *key, OSObject *object) { properties.setObject(key, object); return true; }
    bool setProperty(const char *key, uint64_t value, unsigned width) {
        if (hostIntervalPropertyFailure && std::string(key) == kIOHIDReportIntervalKey) return false;
        auto n = OSNumber::withNumber(value, width); properties.setObject(key, n); n->release(); return true;
    }
    bool setProperty(const char *key,const char *) { return setProperty(key,kOSBooleanTrue); }
    OSObject *getProperty(const char *key) const { return const_cast<OSDictionary &>(properties).getObject(key); }
    void registerService() {
        hostRegistrations++;
        auto page = dynamic_cast<OSNumber *>(getProperty("PrimaryUsagePage"));
        auto usage = dynamic_cast<OSNumber *>(getProperty("PrimaryUsage"));
        hostRegisteredUsagePage = page ? page->unsigned32BitValue() : 0;
        hostRegisteredUsage = usage ? usage->unsigned32BitValue() : 0;
    }
};
struct IOHIDEventService : IOService {
protected:
    virtual UInt32 getPrimaryUsagePage() { return 0; }
    virtual UInt32 getPrimaryUsage() { return 0; }
    virtual UInt32 getLocationID() { return 0; }
    virtual bool handleOpen(IOService *,IOOptionBits,void *) { return true; }
    virtual void handleClose(IOService *,IOOptionBits) {}
    virtual UInt32 getReportInterval() { return 8000; }
    virtual bool handleStart(IOService *) { return true; }
    virtual void dispatchEvent(IOHIDEvent *event, IOOptionBits = 0) {
        if (hostDispatchHook) hostDispatchHook();
        hostDispatches.push_back({event->timestamp, event->level});
    }
public:
    bool start(IOService *p) override {
        if (hostStartFailure || !handleStart(p)) return false;
        setProperty("PrimaryUsagePage", getPrimaryUsagePage(), 32);
        setProperty("PrimaryUsage", getPrimaryUsage(), 32);
        setProperty(kIOHIDReportIntervalKey, getReportInterval(), 32);
        if (getProperty(kIOHIDRegisterServiceKey) != kOSBooleanFalse) registerService();
        return true;
    }
    virtual IOHIDEvent *copyEvent(IOHIDEventType, IOHIDEvent *, IOOptionBits) { return nullptr; }
    virtual bool openForClient(IOService *,IOOptionBits,OSDictionary *,void **) { return false; }
    virtual void closeForClient(IOService *,void *,IOOptionBits) {}
    virtual IOHIDEvent *copyEventForClient(OSObject *,IOOptionBits,void *) { return nullptr; }
    virtual OSObject *copyPropertyForClient(const char *,void *) const { return nullptr; }
    virtual IOReturn setPropertiesForClient(OSObject *,void *) { return kIOReturnUnsupported; }
    bool open(IOService *c) { return handleOpen(c,0,nullptr); }
    void close(IOService *c) { handleClose(c,0); }
};
