// SPDX-License-Identifier: BSD-2-Clause-Patent
// Derived from sheng's registered service/user-client structure. ABI2 keeps all
// DMA, PRM and endpoint state private and enforces one mapped-ring owner.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <sys/proc.h>
#include "NezhaAudioService.h"
#include "../core/AudioHalDiagnostics.h"
#ifdef IOKIT_ENABLE_SHARED_PTR
#error This source requires the selected native build's raw OSPtr convention; audit before changing flags.
#endif
extern "C" IOReturn nezha_audio_fix_host_clock(task_t);
namespace {
IOBufferMemoryDescriptor *maps[3];
NaRuntime *runtime;
void *connection;
bool published;
IOLock *ownerGate;
IOLock *diagnosticGate;
NaHalHistory halHistory;
uint32_t halStats[16];
enum {OpenAttempts,OpenRejected,OpenConnected,ControlMaps,OutputMaps,InputMaps,
      ClockCalls,ClockResult,OutputStarts,OutputResult,InputStarts,InputResult,
      HalConnections,HalDeviceLists,HalPropertyErrors,HalRecords};
IOReturn result(int);
void increment(unsigned field) {__atomic_fetch_add(&halStats[field],1,__ATOMIC_RELAXED);}
void store(unsigned field,uint32_t value) {__atomic_store_n(&halStats[field],value,__ATOMIC_RELEASE);}
bool rejectOpen() {increment(OpenRejected);return false;}
IOReturn observe(const NaHalObservation *event) {
    IOLockLock(diagnosticGate);
    int r=na_hal_history_add(&halHistory,event);
    if (!r) {
        store(HalRecords,halHistory.count);
        if (event->kind==NaHalConnected && !event->result) increment(HalConnections);
        if (event->kind==NaHalDeviceList) increment(HalDeviceLists);
        if (event->kind==NaHalPropertyError) increment(HalPropertyErrors);
    }
    IOLockUnlock(diagnosticGate);return result(r);
}
int allocateMaps(IOBufferMemoryDescriptor **target,bool capture) {
    const vm_size_t sizes[3]={4096,NA_AUDIO_RING_BYTES,NA_AUDIO_RING_BYTES};
    for (unsigned i=0;i<(capture ? 3u:2u);++i) {
        target[i]=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,IOOptionBits(kIOMemoryKernelUserShared)|IOOptionBits(kIODirectionInOut),sizes[i],4096);
        if (!target[i] || !target[i]->getBytesNoCopy()) {
            for (unsigned j=0;j<3;++j) if (target[j]) {target[j]->release();target[j]=nullptr;}
            return NA_IO;
        }
        bzero(target[i]->getBytesNoCopy(),sizes[i]);
    }
    return NA_OK;
}
IOReturn result(int e) {
    if (!e) return kIOReturnSuccess;
    if (e==NA_ARGUMENT) return kIOReturnBadArgument;
    if (e==NA_BUSY) return kIOReturnBusy;
    return kIOReturnNotReady;
}
bool allowedCaller() {
    char name[32]={0};proc_selfname(name,sizeof name);
    // Same process list as the existing sandbox grant, plus the 27 audio
    // server name when present. No caller can supply an address to the clock fix.
    return !strcmp(name,"audiomxd") || !strcmp(name,"mediaserverd") || !strcmp(name,"coreaudiod");
}
}
class NezhaAudio27 : public IOService {
    OSDeclareDefaultStructors(NezhaAudio27);
public:
    bool publishAudio() {
        if (!setProperty("NativeSandboxGrantProcess","audiomxd,mediaserverd,coreaudiod") ||
            !setProperty("IOUserClientClass","NezhaAudio27UserClient")) return false;
        registerService();return true;
    }
};
OSDefineMetaClassAndStructors(NezhaAudio27,IOService);
class NezhaAudio27UserClient : public IOUserClient {
    OSDeclareDefaultStructors(NezhaAudio27UserClient);
    task_t owner=nullptr;
    uint32_t closed=0;
    bool claimed=false;
    IOLock *gate=nullptr;
    void closeAudio() {
        if (!gate) return;
        IOLockLock(gate);
        if (closed) {IOLockUnlock(gate);return;}
        closed=1;
        if (claimed) {
            na_runtime_request(runtime,NaAudioStopOutput);
            na_runtime_request(runtime,NaAudioStopInput);
            // Publish the stops before giving a new connection its ownership.
            void *expected=this;
            __atomic_compare_exchange_n(&connection,&expected,nullptr,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
            claimed=false;
        }
        IOLockUnlock(gate);
    }
public:
    bool initWithTask(task_t task,void *security,UInt32 type,OSDictionary *properties) override {
        increment(OpenAttempts);
        if (type || task!=current_task() || !allowedCaller() || !IOUserClient::initWithTask(task,security,type,properties)) return rejectOpen();
        gate=IOLockAlloc();if (!gate) return rejectOpen();owner=task;return true;
    }
    bool start(IOService *provider) override {
        if (!runtime || !OSDynamicCast(NezhaAudio27,provider) || !IOUserClient::start(provider)) return rejectOpen();
        void *expected=nullptr;
        claimed=__atomic_compare_exchange_n(&connection,&expected,this,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
        if (!claimed) {IOUserClient::stop(provider);return rejectOpen();}
        nezha_audio_owner_lock();
        IOBufferMemoryDescriptor *fresh[3]={};
        int r=na_runtime_quiescent(runtime) ? allocateMaps(fresh,runtime->input!=nullptr):NA_BUSY;
        if (!r) r=na_runtime_rebind(runtime,(NaAudioStatus *)fresh[0]->getBytesNoCopy(),
            (uint64_t *)fresh[1]->getBytesNoCopy(),fresh[2] ? (uint64_t *)fresh[2]->getBytesNoCopy():nullptr);
        if (!r) {
            for (unsigned i=0;i<3;++i) {
                // VM mappings retain their old descriptors. Releasing our
                // reference cannot turn an old writable map into the new ring.
                if (maps[i]) maps[i]->release();maps[i]=fresh[i];fresh[i]=nullptr;
            }
        }
        for (auto *memory:fresh) if (memory) memory->release();
        nezha_audio_owner_unlock();
        if (r) {closeAudio();IOUserClient::stop(provider);return rejectOpen();}
        increment(OpenConnected);
        return true;
    }
    IOReturn clientClose() override {closeAudio();terminate();return kIOReturnSuccess;}
    IOReturn clientDied() override {closeAudio();return IOUserClient::clientDied();}
    void stop(IOService *provider) override {closeAudio();IOUserClient::stop(provider);}
    void free() override {closeAudio();if (gate) {IOLockFree(gate);gate=nullptr;}IOUserClient::free();}
    IOReturn clientMemoryForType(UInt32 type,IOOptionBits *options,IOMemoryDescriptor **memory) override {
        if (!options || !memory || type>NaAudioInputMemory) return kIOReturnBadArgument;
        *memory=nullptr;
        IOLockLock(gate);
        if (closed || !claimed || !maps[type] ||
            (type==NaAudioInputMemory && !runtime->capture_proven)) {IOLockUnlock(gate);return kIOReturnNotReady;}
        *options=type==NaAudioOutputMemory ? 0:kIOMapReadOnly;
        maps[type]->retain();*memory=maps[type];increment(ControlMaps+type);IOLockUnlock(gate);return kIOReturnSuccess;
    }
    IOReturn externalMethod(uint32_t selector,IOExternalMethodArguments *args,IOExternalMethodDispatch *,OSObject *,void *) override {
        const bool observation=selector==NaAudioObserveHAL;
        if (!args || args->scalarInputCount || args->structureInputDescriptor ||
            args->scalarOutputCount || args->structureOutputSize || args->structureOutputDescriptor ||
            args->asyncWakePort || selector>NaAudioObserveHAL ||
            (observation ? args->structureInputSize!=sizeof(NaHalObservation) || !args->structureInput:args->structureInputSize!=0)) return kIOReturnBadArgument;
        IOLockLock(gate);
        if (closed || !claimed || owner!=current_task()) {IOLockUnlock(gate);return kIOReturnNotOpen;}
        IOReturn r;
        if (observation) r=observe(static_cast<const NaHalObservation *>(args->structureInput));
        else if (selector==NaAudioFixHostClock) {increment(ClockCalls);r=nezha_audio_fix_host_clock(owner);store(ClockResult,r);}
        else {
            r=result(na_runtime_request(runtime,selector));
            if (selector==NaAudioStartOutput) {increment(OutputStarts);store(OutputResult,r);}
            if (selector==NaAudioStartInput) {increment(InputStarts);store(InputResult,r);}
        }
        IOLockUnlock(gate);return r;
    }
};
OSDefineMetaClassAndStructors(NezhaAudio27UserClient,IOUserClient);
extern "C" int nezha_audio_allocate_maps(NaAudioStatus **status,uint64_t **output,uint64_t **input,int capture) {
    if (!status || !output || !input || maps[0] || runtime || published) return NA_ARGUMENT;
    if (!ownerGate) ownerGate=IOLockAlloc();if (!ownerGate) return NA_IO;
    if (!diagnosticGate) diagnosticGate=IOLockAlloc();if (!diagnosticGate) return NA_IO;
    int r=allocateMaps(maps,capture!=0);if (r) return r;
    *status=(NaAudioStatus *)maps[0]->getBytesNoCopy();*output=(uint64_t *)maps[1]->getBytesNoCopy();
    *input=maps[2] ? (uint64_t *)maps[2]->getBytesNoCopy():nullptr;return NA_OK;
}
extern "C" void nezha_audio_owner_lock(void) {IOLockLock(ownerGate);}
extern "C" int nezha_audio_owner_try_lock(void) {return ownerGate && IOLockTryLock(ownerGate);}
extern "C" void nezha_audio_owner_unlock(void) {IOLockUnlock(ownerGate);}
extern "C" void nezha_audio_hal_stats(uint32_t out[16]) {
    for (unsigned i=0;i<16;++i) out[i]=__atomic_load_n(&halStats[i],__ATOMIC_ACQUIRE);
}
extern "C" int nezha_audio_hal_history(uint32_t offset,uint32_t count,NaHalObservation *out,uint32_t *copied) {
    if (!out || !copied || !count || count>8 || offset>=NA_HAL_HISTORY_CAPACITY) return NA_ARGUMENT;
    *copied=0;
    if (!diagnosticGate) return NA_OK;
    if (!IOLockTryLock(diagnosticGate)) return NA_BUSY;
    *copied=na_hal_history_copy(&halHistory,offset,count,out);
    IOLockUnlock(diagnosticGate);return NA_OK;
}
extern "C" int nezha_audio_publish(NaRuntime *r) {
    if (!r || !maps[0] || r->shared!=maps[0]->getBytesNoCopy() || r->output!=maps[1]->getBytesNoCopy() ||
        !__atomic_load_n(&r->service_ready,__ATOMIC_ACQUIRE)) return NA_MISSING;
    if (published) return runtime==r ? NA_OK:NA_OWNERSHIP;
    // This worker is the sole publisher. Attached service/runtime/maps remain
    // retained for this boot, including fault cleanup; no DSP-visible free.
    NezhaAudio27 *service=OSTypeAlloc(NezhaAudio27);
    if (!service) return NA_IO;
    IOService *platform=IOService::getPlatform();bool attached=false,started=false;
    if (service->init() && (attached=service->attach(platform))) started=service->start(platform);
    if (started) {
        runtime=r;
        if (service->publishAudio()) {published=true;service->release();return NA_OK;}
        runtime=nullptr;service->stop(platform);
    }
    if (attached) service->detach(platform);service->release();return NA_IO;
}
