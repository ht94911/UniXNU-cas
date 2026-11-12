// SPDX-License-Identifier: BSD-2-Clause-Patent
// sheng AudioServerPlugIn architecture, with ABI2 ownership and independent
// input lifecycle. arm64e source only; original-donor load validation pending.
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <dlfcn.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <os/log.h>
#include "../os/NezhaAudioAbi.h"
#include "../core/AudioRing.h"
#include "AudioClock.h"
#include "AudioReference.h"
enum {kPlugIn=kAudioObjectPlugInObject,kDevice=2,kOutStream=3,kInStream=4,kVolume=5,kReferenceStream=6};
#define kDeviceUID "NezhaAudio27Device"
#define kModelUID "NezhaAudio27Model"
static const Float64 kRate=NA_AUDIO_RATE;
/* Zero timestamps follow the10ms DSP period, independently of ring capacity. */
static const UInt32 kZeroPeriod=NA_AUDIO_PERIOD_FRAMES;
static const Float32 kMinDb=-36.0f;
static pthread_mutex_t gLock=PTHREAD_MUTEX_INITIALIZER;
static atomic_uint gRefs=1;
static AudioServerPlugInHostRef gHost;
static IONotificationPortRef gNotifications;
static io_iterator_t gMatches;
static dispatch_queue_t gDiscoveryQueue;
static dispatch_source_t gRetry;
static uint8_t gDiscoveryQueueKey;
static atomic_uint gDiscoveryActive;
static io_connect_t gConnect;
static mach_vm_address_t gMapAddress[3];
static const NaAudioStatus *gControl;
static NaAudioStatus gLastStatus;
static uint64_t *gOutput;static const uint64_t *gInput;
static uint64_t gReference[NA_AUDIO_RING_FRAMES];
static UInt32 gIoUsers;static int gInputAvailable,gOutputSelected=1,gInputSelected=1,gCaptureIssued;
static int gReferenceSelected=1;
static uint64_t gCaptureTouch;
static Float32 gVolume=1; /* Upstream default; kernel and amplifier limits still apply. */
static CFNumberRef gLogicalLocation;
static uint64_t gZeroOrigin,gZeroSeed;
static uint32_t gEpoch;
static struct {UInt32 id,count;} gUsers[32];
static uint32_t gReportedDeviceList=UINT32_MAX,gObservationCount;
static UInt32 gSystemRouteSamples;
static NaHalObservation gObservations[NA_HAL_HISTORY_CAPACITY];
/* One handled scheduling miss and one remaining failure, forwarded on the
 * existing discovery timer. The realtime callback never makes a logging RPC. */
static struct {atomic_uint state;uint32_t reason,flags,frames;int32_t result,delta;} gIoNotes[2];
static void note_io(unsigned slot,uint32_t reason,uint32_t flags,uint32_t frames,int32_t result,int64_t delta) {
    unsigned expected=0;
    if (!atomic_compare_exchange_strong(&gIoNotes[slot].state,&expected,1)) return;
    gIoNotes[slot].reason=reason;gIoNotes[slot].flags=flags;gIoNotes[slot].frames=frames;gIoNotes[slot].result=result;
    gIoNotes[slot].delta=delta<INT32_MIN ? INT32_MIN:delta>INT32_MAX ? INT32_MAX:(int32_t)delta;
    atomic_store_explicit(&gIoNotes[slot].state,2,memory_order_release);
}
static void report_hal(uint32_t kind,uint32_t object,uint32_t selector,uint32_t scope,uint32_t element,int32_t result,uint32_t value) {
    if (!gConnect) return;
    NaHalObservation event={NA_HAL_OBSERVATION_VERSION,kind,object,selector,scope,element,result,value};
    (void)IOConnectCallStructMethod(gConnect,NaAudioObserveHAL,&event,sizeof event,NULL,NULL);
}
/* Callback observations are retained once per numeric event, before the
 * realtime IO path. A full history never changes a property or IO decision. */
static void observe_once(uint32_t kind,uint32_t object,uint32_t selector,uint32_t scope,uint32_t element,int32_t result,uint32_t value) {
    /* The retained kernel observation ABI accepts the original objects1..5.
     * Reference-stream construction is visible in stock VirtualAudio logs. */
    if (object>kVolume) return;
    if (!gConnect || gObservationCount==NA_HAL_HISTORY_CAPACITY) return;
    NaHalObservation event={NA_HAL_OBSERVATION_VERSION,kind,object,selector,scope,element,result,value};
    for (uint32_t i=0;i<gObservationCount;++i)
        if (!memcmp(&gObservations[i],&event,sizeof event)) return;
    gObservations[gObservationCount++]=event;
    report_hal(kind,object,selector,scope,element,result,value);
}
static int observed_property(AudioObjectID object,AudioObjectPropertySelector selector) {
    switch (selector) {
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwnedObjects:
        return 1;
    case kAudioDevicePropertyTransportType:
    case 'lcID':
    case kAudioDevicePropertyDeviceIsAlive:
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
    case kAudioDevicePropertyStreams:
    case kAudioObjectPropertyControlList:
    case kAudioDevicePropertyNominalSampleRate:
        return object==kDevice;
    case kAudioStreamPropertyIsActive:
    case kAudioStreamPropertyDirection:
    case kAudioStreamPropertyTerminalType:
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
        return object==kOutStream || object==kInStream;
    case kAudioControlPropertyScope:
    case kAudioControlPropertyElement:
    case kAudioLevelControlPropertyScalarValue:
        return object==kVolume;
    default:return 0;
    }
}
static uint32_t property_observation_value(AudioObjectID object,const AudioObjectPropertyAddress *address,uint32_t bytes,const void *data) {
    uint32_t value=bytes;
    switch (address->mSelector) {
    case kAudioObjectPropertyOwnedObjects:
    case kAudioDevicePropertyStreams:
    case kAudioObjectPropertyControlList:
        return bytes/(uint32_t)sizeof(AudioObjectID);
    case kAudioDevicePropertyNominalSampleRate:
        if (bytes==sizeof(Float64) && data) {
            Float64 rate;memcpy(&rate,data,sizeof rate);
            if (isfinite(rate) && rate>=0 && rate<=UINT32_MAX) return (uint32_t)rate;
        }
        return bytes;
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
        if (bytes==sizeof(AudioStreamBasicDescription) && data)
            return ((const AudioStreamBasicDescription *)data)->mChannelsPerFrame;
        return bytes;
    default:
        /* Scalar volume retains its Float32 bit pattern, not an address. */
        if (bytes==sizeof value && data) memcpy(&value,data,sizeof value);
        (void)object;return value;
    }
}
static kern_return_t call(uint32_t selector) {
    return gConnect ? IOConnectCallScalarMethod(gConnect,selector,NULL,0,NULL,NULL):KERN_FAILURE;
}
static void disconnect(void) {
    if (gConnect) {call(NaAudioStopOutput);call(NaAudioStopInput);}
    for (unsigned i=0;i<3;++i) if (gMapAddress[i]) {
        IOConnectUnmapMemory64(gConnect,i,mach_task_self(),gMapAddress[i]);gMapAddress[i]=0;
    }
    if (gConnect) IOServiceClose(gConnect);
    gConnect=0;gControl=NULL;gOutput=NULL;gInput=NULL;gInputAvailable=0;
    memset(&gLastStatus,0,sizeof gLastStatus);
    gIoUsers=0;memset(gUsers,0,sizeof gUsers);
    gCaptureIssued=0;gCaptureTouch=0;
    memset(gReference,0,sizeof gReference);
}
static int map_type(unsigned type,uint64_t expected) {
    mach_vm_size_t size=0;mach_vm_address_t address=0;
    if (IOConnectMapMemory64(gConnect,type,mach_task_self(),&address,&size,kIOMapAnywhere)!=KERN_SUCCESS) return -1;
    gMapAddress[type]=address;
    return !address || (address&7) || size<expected || size>expected+16384 ? -1:0;
}
static int clock_ready(const NaAudioStatus *s) {
    /* dlsym's signed function pointer is called directly on arm64e; no
     * instruction reads through a PAC pointer and no legacy rebinding hook. */
    Float64 (*frequency)(void)=(Float64 (*)(void))dlsym(RTLD_DEFAULT,"AudioGetHostClockFrequency");
    if (!frequency) {report_hal(NaHalClockResult,kPlugIn,0,0,0,-1,0);return -1;}
    Float64 observed=frequency();
    if (!isfinite(observed) || observed<1 || observed>UINT32_MAX || floor(observed)!=observed) {
        report_hal(NaHalClockResult,kPlugIn,0,0,0,-2,0);return -1;
    }
    if ((uint32_t)observed!=s->host_hz) {
        /* No caller-supplied address: kernel checks its exact donor UUID/code
         * and computes the one shared-cache literal from the task's slide. */
        kern_return_t fixed=call(NaAudioFixHostClock);
        if (fixed!=KERN_SUCCESS) {report_hal(NaHalClockResult,kPlugIn,0,0,0,fixed,(uint32_t)observed);return -1;}
        observed=frequency();
    }
    mach_timebase_info_data_t tb;
    if (mach_timebase_info(&tb)!=KERN_SUCCESS || !isfinite(observed) || observed!=s->host_hz) {
        report_hal(NaHalClockResult,kPlugIn,0,0,0,-3,s->host_hz);return -1;
    }
    uint64_t q=0;
    int result=na_clock_contract(tb.numer,tb.denom,(uint32_t)observed,&q) || q!=s->ticks_per_frame_q32 ? -1:0;
    report_hal(NaHalClockResult,kPlugIn,0,0,0,result ? -4:0,(uint32_t)observed);return result;
}
static int connect_service(void) {
    if (gControl) return 0;
    io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching(NA_AUDIO_CLASS));
    if (!service) return -1;
    kern_return_t result=IOServiceOpen(service,mach_task_self(),0,&gConnect);IOObjectRelease(service);
    if (result!=KERN_SUCCESS) {gConnect=0;return -1;}
    if (map_type(NaAudioControlMemory,4096)) goto bad;
    gControl=(const NaAudioStatus *)(uintptr_t)gMapAddress[0];NaAudioStatus s;
    if (na_audio_status_snapshot(gControl,&s) || !(s.state&NaAudioOutputReady) || (s.state&NaAudioFault) || clock_ready(&s)) goto bad;
    if (map_type(NaAudioOutputMemory,NA_AUDIO_RING_BYTES)) goto bad;
    gOutput=(uint64_t *)(uintptr_t)gMapAddress[1];gInputAvailable=(s.state&NaAudioInputReady)!=0;
    if (gInputAvailable) {
        if (map_type(NaAudioInputMemory,NA_AUDIO_RING_BYTES)) goto bad;
        gInput=(const uint64_t *)(uintptr_t)gMapAddress[2];
    }
    gEpoch=s.epoch;gLastStatus=s;gReportedDeviceList=UINT32_MAX;gSystemRouteSamples=0;
    memset(gReference,0,sizeof gReference);
    report_hal(NaHalConnected,kPlugIn,0,0,0,0,1);return 0;
bad:disconnect();return -1;
}
/* gLock protects the mapping and this last complete status. A busy publication
 * does not retire a live connection or its immutable clock. PCM callers must
 * use fresh counters and drop the period when the publication is busy. */
static int status_snapshot(NaAudioStatus *s,int allow_cached_clock) {
    if (!gControl) return NA_MISSING;
    int result=na_audio_status_snapshot(gControl,s);
    if (!result) gLastStatus=*s;
    else if (result==NA_BUSY && allow_cached_clock) {*s=gLastStatus;return NA_OK;}
    return result;
}
static int available(void) {
    NaAudioStatus s;
    return !status_snapshot(&s,1) && s.epoch==gEpoch &&
        (s.state&NaAudioOutputReady) && !(s.state&NaAudioFault);
}
static void check_service(void) {
    pthread_mutex_lock(&gLock);
    if (!atomic_load(&gDiscoveryActive)) {pthread_mutex_unlock(&gLock);return;}
    int previous=gControl!=NULL;
    if (gControl && !available()) disconnect();
    int now=connect_service()==0;
    AudioServerPlugInHostRef host=gHost;
    pthread_mutex_unlock(&gLock);
    if (previous!=now && host) {
        AudioObjectPropertyAddress changed[2]={
            {kAudioPlugInPropertyDeviceList,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain},
            {kAudioObjectPropertyOwnedObjects,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain}};
        OSStatus notification=host->PropertiesChanged(host,kPlugIn,2,changed);
        pthread_mutex_lock(&gLock);
        observe_once(NaHalPublication,kPlugIn,kAudioPlugInPropertyDeviceList,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain,notification,2);
        pthread_mutex_unlock(&gLock);
        if (previous) {
            AudioObjectPropertyAddress alive={kAudioDevicePropertyDeviceIsAlive,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
            host->PropertiesChanged(host,kDevice,1,&alive);
        }
    }
}
static void service_matched(void *context,io_iterator_t matches) {
    (void)context;io_object_t object;
    if (!atomic_load(&gDiscoveryActive)) return;
    while ((object=IOIteratorNext(matches))) IOObjectRelease(object);
    check_service();
}
/* Observe the registered physical device and the system defaults on the
 * existing discovery queue. The mobile VirtualAudio layer normally takes
 * the physical device's hog mode and routes its own virtual device to it.
 * HALS excludes hogged physical devices from its ordinary default catalog;
 * forcing our physical UID into that catalog made both defaults become 0
 * in056. Leave the system's mobile mixer selection in place. Global getters
 * can re-enter this plug-in's callbacks, so hold no gLock across them. */
/* Read the actual global device/stream values used by24A446's default-device
 * policy. These are observations; a full history cannot affect the policy. */
static void sample_default_eligibility(UInt32 epoch,io_connect_t connection,AudioObjectID target) {
    pthread_mutex_lock(&gLock);
    int current=atomic_load(&gDiscoveryActive) && gConnect==connection && gEpoch==epoch && available();
    pthread_mutex_unlock(&gLock);
    if (!current || target==kAudioObjectUnknown) return;
    NaHalObservation events[7];unsigned count=0;
    const AudioObjectPropertySelector scalar[3]={kAudioDevicePropertyDeviceIsAlive,
        kAudioDevicePropertyIsHidden,kAudioDevicePropertyHogMode};
    for (unsigned i=0;i<3;++i) {
        AudioObjectPropertyAddress a={scalar[i],kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
        UInt32 bytes=sizeof(UInt32),value=0;
        OSStatus r=AudioObjectGetPropertyData(target,&a,0,NULL,&bytes,&value);
        if (!r && bytes!=sizeof value) r=kAudioHardwareBadPropertySizeError;
        events[count++]=(NaHalObservation){NA_HAL_OBSERVATION_VERSION,NaHalSystemRoute,
            kDevice,a.mSelector,a.mScope,a.mElement,r,r ? 0:value};
    }
    AudioObjectPropertyAddress a={kAudioDevicePropertyStreams,kAudioObjectPropertyScopeOutput,kAudioObjectPropertyElementMain};
    UInt32 bytes=0;AudioObjectID streams[16]={0};
    OSStatus r=AudioObjectGetPropertyDataSize(target,&a,0,NULL,&bytes);
    if (!r && (bytes%sizeof(AudioObjectID) || bytes>sizeof streams)) r=kAudioHardwareBadPropertySizeError;
    if (!r && bytes) {
        UInt32 expected=bytes;r=AudioObjectGetPropertyData(target,&a,0,NULL,&bytes,streams);
        if (!r && bytes!=expected) r=kAudioHardwareBadPropertySizeError;
    }
    UInt32 stream_count=r ? 0:bytes/(UInt32)sizeof(AudioObjectID);
    events[count++]=(NaHalObservation){NA_HAL_OBSERVATION_VERSION,NaHalSystemRoute,
        kDevice,a.mSelector,a.mScope,a.mElement,r,stream_count};
    if (!r && stream_count && streams[0]!=kAudioObjectUnknown) {
        AudioStreamBasicDescription f={0};bytes=sizeof f;
        a=(AudioObjectPropertyAddress){kAudioStreamPropertyVirtualFormat,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
        r=AudioObjectGetPropertyData(streams[0],&a,0,NULL,&bytes,&f);
        if (!r && bytes!=sizeof f) r=kAudioHardwareBadPropertySizeError;
        events[count++]=(NaHalObservation){NA_HAL_OBSERVATION_VERSION,NaHalSystemRoute,
            kOutStream,a.mSelector,a.mScope,a.mElement,r,r ? 0:f.mFormatFlags};
        /* sfid is the format-ID companion to the same observed sfmt reply;
         * it is a diagnostic label, never a queried or answered property. */
        events[count++]=(NaHalObservation){NA_HAL_OBSERVATION_VERSION,NaHalSystemRoute,
            kOutStream,'sfid',a.mScope,a.mElement,r,r ? 0:f.mFormatID};
        a=(AudioObjectPropertyAddress){kAudioStreamPropertyTerminalType,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
        UInt32 value=0;bytes=sizeof value;
        r=AudioObjectGetPropertyData(streams[0],&a,0,NULL,&bytes,&value);
        if (!r && bytes!=sizeof value) r=kAudioHardwareBadPropertySizeError;
        events[count++]=(NaHalObservation){NA_HAL_OBSERVATION_VERSION,NaHalSystemRoute,
            kOutStream,a.mSelector,a.mScope,a.mElement,r,r ? 0:value};
    }
    pthread_mutex_lock(&gLock);
    if (atomic_load(&gDiscoveryActive) && gConnect==connection && gEpoch==epoch && available())
        for (unsigned i=0;i<count;++i) report_hal(events[i].kind,events[i].object,events[i].selector,
            events[i].scope,events[i].element,events[i].result,events[i].value);
    pthread_mutex_unlock(&gLock);
}
static void sample_system_route(void) {
    pthread_mutex_lock(&gLock);
    if (!atomic_load(&gDiscoveryActive) || !available() || gSystemRouteSamples>=8) {
        pthread_mutex_unlock(&gLock);return;
    }
    UInt32 epoch=gEpoch;io_connect_t connection=gConnect;
    UInt32 sample=++gSystemRouteSamples;
    pthread_mutex_unlock(&gLock);
    const AudioObjectPropertySelector selectors[4]={kAudioHardwarePropertyTranslateUIDToDevice,
        kAudioHardwarePropertyDefaultOutputDevice,kAudioHardwarePropertyDefaultSystemOutputDevice,
        kAudioHardwarePropertyDevices};
    NaHalObservation events[4];CFStringRef uid=CFSTR(kDeviceUID);
    for (unsigned i=0;i<4;++i) {
        AudioObjectPropertyAddress address={selectors[i],kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
        UInt32 bytes=sizeof(AudioObjectID);AudioObjectID value=kAudioObjectUnknown;OSStatus r;
        if (i==3) {
            bytes=0;r=AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&address,0,NULL,&bytes);
            if (!r && bytes%sizeof(AudioObjectID)) r=kAudioHardwareBadPropertySizeError;
            if (!r) value=bytes/(UInt32)sizeof(AudioObjectID);
        } else {
            r=AudioObjectGetPropertyData(kAudioObjectSystemObject,&address,
                i==0 ? (UInt32)sizeof(uid):0,i==0 ? &uid:NULL,&bytes,&value);
            if (!r && bytes!=sizeof(AudioObjectID)) r=kAudioHardwareBadPropertySizeError;
        }
        events[i]=(NaHalObservation){NA_HAL_OBSERVATION_VERSION,NaHalSystemRoute,
            kAudioObjectSystemObject,address.mSelector,address.mScope,address.mElement,r,r ? 0:value};
    }
    AudioObjectID target=events[0].value;
    pthread_mutex_lock(&gLock);
    if (atomic_load(&gDiscoveryActive) && gConnect==connection && gEpoch==epoch && available()) {
        for (unsigned i=0;i<4;++i) {
            /* The last sample must survive duplicate suppression: an earlier
             *36 or0 does not describe the final system default. */
            if (sample==8 && i<4) report_hal(events[i].kind,events[i].object,events[i].selector,
                events[i].scope,events[i].element,events[i].result,events[i].value);
            else observe_once(events[i].kind,events[i].object,events[i].selector,
                events[i].scope,events[i].element,events[i].result,events[i].value);
        }
    }
    pthread_mutex_unlock(&gLock);
    if (sample==8 && !events[0].result) sample_default_eligibility(epoch,connection,target);
}
static void retry_service(void *context) {
    (void)context;check_service();sample_system_route();
    pthread_mutex_lock(&gLock);
    if (gConnect) for (unsigned i=0;i<2;++i) if (atomic_load_explicit(&gIoNotes[i].state,memory_order_acquire)==2) {
        report_hal(NaHalPropertyError,kOutStream,'wMix',kAudioObjectPropertyScopeOutput,gIoNotes[i].reason,gIoNotes[i].result,gIoNotes[i].flags);
        report_hal(NaHalPropertyError,kOutStream,'wDel',kAudioObjectPropertyScopeOutput,gIoNotes[i].reason,gIoNotes[i].result,(uint32_t)gIoNotes[i].delta);
        report_hal(NaHalPropertyError,kOutStream,'wLen',kAudioObjectPropertyScopeOutput,gIoNotes[i].reason,gIoNotes[i].result,gIoNotes[i].frames);
        atomic_store_explicit(&gIoNotes[i].state,3,memory_order_release);
    }
    pthread_mutex_unlock(&gLock);
}
static UInt32 latencyFrames(void) {return NA_AUDIO_LEAD_FRAMES;}
static Float32 toDb(Float32 value) {if (!isfinite(value)) return kMinDb;Float32 r=1.0f-fminf(1,fmaxf(0,value));return kMinDb*r*r;}
static Float32 toScalar(Float32 db) {if (!isfinite(db) || db<=kMinDb) return 0;return db>=0 ? 1:1-sqrtf(db/kMinDb);}
static Float32 gain(void) {return gVolume<=0 ? 0:powf(10.0f,toDb(gVolume)/20.0f);}
static AudioStreamBasicDescription format(UInt32 channels) {
    AudioStreamBasicDescription f={0};f.mSampleRate=kRate;f.mFormatID=kAudioFormatLinearPCM;
    f.mFormatFlags=kAudioFormatFlagIsFloat|kAudioFormatFlagsNativeEndian|kAudioFormatFlagIsPacked;
    f.mBytesPerPacket=f.mBytesPerFrame=4*channels;f.mFramesPerPacket=1;f.mChannelsPerFrame=channels;f.mBitsPerChannel=32;return f;
}
#define REPLY(type,value) do {if (inDataSize<sizeof(type)) return kAudioHardwareBadPropertySizeError;*(type *)outData=(value);*outDataSize=sizeof(type);return 0;} while(0)
#define REPLY_STR(value) REPLY(CFStringRef,CFSTR(value))
#include "NezhaAudioProperties.inc"
static HRESULT QueryInterface(void *,REFIID,LPVOID *);
static ULONG AddRef(void *self) {(void)self;unsigned n=atomic_load(&gRefs);while (n!=UINT32_MAX && !atomic_compare_exchange_weak(&gRefs,&n,n+1)) {}return n==UINT32_MAX ? n:n+1;}
static void finish_discovery(void *context) {
    (void)context;
    if (gRetry) {dispatch_release(gRetry);gRetry=NULL;}
    if (gNotifications) {IONotificationPortDestroy(gNotifications);gNotifications=NULL;}
    if (gMatches) {IOObjectRelease(gMatches);gMatches=0;}
    pthread_mutex_lock(&gLock);gHost=NULL;disconnect();
    if (gLogicalLocation) {CFRelease(gLogicalLocation);gLogicalLocation=NULL;}
    pthread_mutex_unlock(&gLock);
}
static ULONG Release(void *self) {
    (void)self;unsigned n=atomic_load(&gRefs);while (n && !atomic_compare_exchange_weak(&gRefs,&n,n-1)) {}
    if (n==1) {
        atomic_store(&gDiscoveryActive,0);
        if (gRetry) dispatch_source_cancel(gRetry);
        if (gDiscoveryQueue && !dispatch_get_specific(&gDiscoveryQueueKey)) dispatch_sync_f(gDiscoveryQueue,NULL,finish_discovery);
        else finish_discovery(NULL);
        if (gDiscoveryQueue) {dispatch_release(gDiscoveryQueue);gDiscoveryQueue=NULL;}
    }return n ? n-1:0;
}
static OSStatus Initialize(AudioServerPlugInDriverRef d,AudioServerPlugInHostRef host) {
    (void)d;if (!host) return kAudioHardwareIllegalOperationError;gHost=host;
    atomic_store(&gDiscoveryActive,1);
    pthread_mutex_lock(&gLock);int r=connect_service();pthread_mutex_unlock(&gLock);
    /* Initialize can run on an audio-server worker with no running CFRunLoop.
     * IOKit notifications and retries must still deliver a late service. */
    if (!gDiscoveryQueue) {
        gDiscoveryQueue=dispatch_queue_create("org.unixnu.nezha.audio27.discovery",DISPATCH_QUEUE_SERIAL);
        if (!gDiscoveryQueue) return kAudioHardwareUnspecifiedError;
        dispatch_queue_set_specific(gDiscoveryQueue,&gDiscoveryQueueKey,(void *)1,NULL);
    }
    if (!gNotifications) {
        gNotifications=IONotificationPortCreate(kIOMainPortDefault);
        if (gNotifications) {
            IONotificationPortSetDispatchQueue(gNotifications,gDiscoveryQueue);
            kern_return_t kr=IOServiceAddMatchingNotification(gNotifications,kIOFirstMatchNotification,
                IOServiceMatching(NA_AUDIO_CLASS),service_matched,NULL,&gMatches);
            if (kr==KERN_SUCCESS) service_matched(NULL,gMatches); /* drain to arm */
            else {IONotificationPortDestroy(gNotifications);gNotifications=NULL;}
        }
    }
    /* A close may still be draining DSP ownership when the next client opens.
     * First-match notifications do not repeat for that case. Retry outside
     * all realtime callbacks, on the same serial discovery queue. */
    if (!gRetry) {
        gRetry=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,gDiscoveryQueue);
        if (!gRetry) return kAudioHardwareUnspecifiedError;
        dispatch_source_set_event_handler_f(gRetry,retry_service);
        dispatch_source_set_timer(gRetry,dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),NSEC_PER_SEC,10*NSEC_PER_MSEC);
        dispatch_resume(gRetry);
    }
    os_log_info(OS_LOG_DEFAULT,"NezhaAudio27: service %{public}s",r ? "unavailable":"connected");return 0;
}
static OSStatus CreateDevice(AudioServerPlugInDriverRef d,CFDictionaryRef p,const AudioServerPlugInClientInfo *c,AudioObjectID *o) {(void)d;(void)p;(void)c;(void)o;return kAudioHardwareUnsupportedOperationError;}
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d,AudioObjectID o) {(void)d;(void)o;return kAudioHardwareUnsupportedOperationError;}
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d,AudioObjectID o,const AudioServerPlugInClientInfo *c) {
    (void)d;OSStatus result=o==kDevice && c ? 0:kAudioHardwareBadObjectError;
    if (o==kDevice && c) {
        pthread_mutex_lock(&gLock);observe_once(NaHalDeviceClient,o,0,0,0,result,c->mClientID);pthread_mutex_unlock(&gLock);
    }
    return result;
}
static OSStatus StopIO(AudioServerPlugInDriverRef,AudioObjectID,UInt32);
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d,AudioObjectID o,const AudioServerPlugInClientInfo *c) {
    (void)d;if (o!=kDevice || !c) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    for (unsigned i=0;i<32;++i) if (gUsers[i].count && gUsers[i].id==c->mClientID) {gIoUsers-=gUsers[i].count;gUsers[i].count=0;break;}
    if (!gIoUsers) {call(NaAudioStopOutput);call(NaAudioStopInput);}
    observe_once(NaHalDeviceClient,o,1,0,0,0,c->mClientID);
    pthread_mutex_unlock(&gLock);return 0;
}
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d,AudioObjectID o,UInt64 action,void *info) {(void)d;(void)o;(void)action;(void)info;return kAudioHardwareUnsupportedOperationError;}
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d,AudioObjectID o,UInt64 action,void *info) {(void)d;(void)o;(void)action;(void)info;return 0;}
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 size,UInt32 *written,void *out) {
    (void)d;(void)pid;if (!a || !written || (!out && size)) return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gLock);
    if (o==kPlugIn && !gControl) connect_service();
    OSStatus r=propertyData(o,a,qs,q,size,written,out);
    if (!r && o==kPlugIn && a->mSelector==kAudioPlugInPropertyDeviceList) {
        uint32_t count=*written/sizeof(AudioObjectID);
        if (gConnect && count!=gReportedDeviceList) {
            report_hal(NaHalDeviceList,o,a->mSelector,a->mScope,a->mElement,0,count);gReportedDeviceList=count;
        }
    } else if (r) {
        observe_once(NaHalPropertyError,o,a->mSelector,a->mScope,a->mElement,r,0);
    }
    if (!r && observed_property(o,a->mSelector))
        observe_once(NaHalPropertyRead,o,a->mSelector,a->mScope,a->mElement,0,
                     property_observation_value(o,a,*written,out));
    pthread_mutex_unlock(&gLock);return r;
}
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 *size) {
    if (!size) return kAudioHardwareIllegalOperationError;
    /* HasProperty calls this function. A size probe must not allocate or
     * retain the custom CFNumber that a real data read transfers. */
    if (o==kDevice && a && a->mSelector=='lcID') {
        if (a->mScope!=kAudioObjectPropertyScopeGlobal || a->mElement!=kAudioObjectPropertyElementMain)
            return kAudioHardwareUnknownPropertyError;
        *size=sizeof(CFNumberRef);return 0;
    }
    _Alignas(16) uint8_t scratch[1024]={0};return GetPropertyData(d,o,pid,a,qs,q,sizeof scratch,size,scratch);
}
static Boolean HasProperty(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a) {UInt32 size=0;return GetPropertyDataSize(d,o,pid,a,0,NULL,&size)==0;}
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a,Boolean *value) {
    if (!a || !value) return kAudioHardwareIllegalOperationError;
    *value=(o==kVolume && (a->mSelector==kAudioLevelControlPropertyScalarValue || a->mSelector==kAudioLevelControlPropertyDecibelValue)) ||
        ((o==kOutStream || o==kInStream || o==kReferenceStream) && (a->mSelector==kAudioStreamPropertyIsActive || a->mSelector==kAudioStreamPropertyVirtualFormat || a->mSelector==kAudioStreamPropertyPhysicalFormat)) ||
        (o==kDevice && a->mSelector==kAudioDevicePropertyNominalSampleRate);
    return HasProperty(d,o,pid,a) ? 0:kAudioHardwareUnknownPropertyError;
}
static OSStatus SetPropertyData(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 size,const void *data) {
    (void)d;(void)pid;(void)qs;(void)q;if (!a || !data) return kAudioHardwareIllegalOperationError;
    OSStatus result=kAudioHardwareUnknownPropertyError;int volume_changed=0;
    pthread_mutex_lock(&gLock);
    if (o==kVolume && size==sizeof(Float32) && (a->mSelector==kAudioLevelControlPropertyScalarValue || a->mSelector==kAudioLevelControlPropertyDecibelValue)) {
        Float32 value=*(const Float32 *)data;
        if (!isfinite(value)) result=kAudioHardwareIllegalOperationError;
        else {if (a->mSelector==kAudioLevelControlPropertyDecibelValue) value=toScalar(value);gVolume=fminf(1,fmaxf(0,value));volume_changed=1;result=0;}
    } else if ((o==kOutStream || o==kInStream || o==kReferenceStream) && a->mSelector==kAudioStreamPropertyIsActive && size==sizeof(UInt32)) {
        UInt32 active=*(const UInt32 *)data;
        if (active>1 || (o==kInStream && !gInputAvailable)) result=kAudioHardwareIllegalOperationError;
        else if (o==kReferenceStream) {gReferenceSelected=active;result=0;}
        else {
            uint32_t selector=o==kOutStream ? (active ? NaAudioStartOutput:NaAudioStopOutput):(active ? NaAudioStartInput:NaAudioStopInput);
            if (gIoUsers && (o==kOutStream || !active) && call(selector)!=KERN_SUCCESS) result=kAudioHardwareNotRunningError;
            else {if (o==kOutStream) gOutputSelected=active;else gInputSelected=active;result=0;}
        }
    } else if ((o==kOutStream || o==kInStream || o==kReferenceStream) && (a->mSelector==kAudioStreamPropertyPhysicalFormat || a->mSelector==kAudioStreamPropertyVirtualFormat)) {
        AudioStreamBasicDescription expected=format(o==kInStream ? 1:2);
        result=size==sizeof expected && !memcmp(data,&expected,sizeof expected) ? 0:kAudioDeviceUnsupportedFormatError;
    } else if (o==kDevice && a->mSelector==kAudioDevicePropertyNominalSampleRate)
        result=size==sizeof(Float64) && *(const Float64 *)data==kRate ? 0:kAudioDeviceUnsupportedFormatError;
    if (observed_property(o,a->mSelector))
        observe_once(NaHalPropertyWrite,o,a->mSelector,a->mScope,a->mElement,result,
                     property_observation_value(o,a,size,data));
    pthread_mutex_unlock(&gLock);
    if (volume_changed && gHost) {
        AudioObjectPropertyAddress changed[2]={{kAudioLevelControlPropertyScalarValue,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain},{kAudioLevelControlPropertyDecibelValue,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain}};
        gHost->PropertiesChanged(gHost,kVolume,2,changed);
    }
    return result;
}
static OSStatus StartIO(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 client) {
    (void)d;if (o!=kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);OSStatus r=kAudioHardwareNotRunningError;
    if (connect_service() || !available() || gIoUsers>=1024) goto out;
    unsigned slot=32;
    for (unsigned i=0;i<32;++i) if (gUsers[i].count && gUsers[i].id==client) {slot=i;break;}
    if (slot==32) for (unsigned i=0;i<32;++i) if (!gUsers[i].count) {slot=i;break;}
    if (slot==32) goto out;
    if (!gIoUsers) {
        NaAudioStatus s;
        if (status_snapshot(&s,1) || clock_ready(&s)) goto out;
        if (gOutputSelected && call(NaAudioStartOutput)!=KERN_SUCCESS) goto out;
        gCaptureIssued=0;gCaptureTouch=0; /* Input starts on actual ReadInput. */
        if (na_clock_frames(s.anchor_host,mach_absolute_time(),s.ticks_per_frame_q32,&gZeroOrigin)) {call(NaAudioStopOutput);call(NaAudioStopInput);goto out;}
        ++gZeroSeed;
    }
    gUsers[slot].id=client;++gUsers[slot].count;++gIoUsers;r=0;
out:report_hal(NaHalStartIO,kDevice,0,0,0,r,gIoUsers);pthread_mutex_unlock(&gLock);return r;
}
static OSStatus StopIO(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 client) {
    (void)d;if (o!=kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    for (unsigned i=0;i<32;++i) if (gUsers[i].count && gUsers[i].id==client) {--gUsers[i].count;--gIoUsers;break;}
    if (!gIoUsers) {call(NaAudioStopOutput);call(NaAudioStopInput);}
    pthread_mutex_unlock(&gLock);return 0;
}
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 client,Float64 *sample,UInt64 *host,UInt64 *seed) {
    (void)d;(void)client;if (o!=kDevice || !sample || !host || !seed) return kAudioHardwareIllegalOperationError;
    /* Metadata/close operations share this lock. Contention is not a stopped
     * clock: serialize the scalar timestamp snapshot with those operations. */
    pthread_mutex_lock(&gLock);
    NaAudioStatus s;uint64_t frames,stamp;OSStatus r=kAudioHardwareNotRunningError;
    if (gControl && gIoUsers && !status_snapshot(&s,1) && s.epoch==gEpoch &&
        (s.state&NaAudioOutputReady) && !(s.state&NaAudioFault) &&
        !na_clock_frames(s.anchor_host,mach_absolute_time(),s.ticks_per_frame_q32,&frames)) {
        uint64_t wraps=frames>gZeroOrigin ? (frames-gZeroOrigin)/kZeroPeriod:0;
        stamp=gZeroOrigin+wraps*kZeroPeriod;
        uint64_t host_value;
        if (stamp<=NA_AUDIO_MAX_FRAME && !na_clock_host(s.anchor_host,stamp,s.ticks_per_frame_q32,&host_value)) {*host=(UInt64)host_value;*sample=(Float64)stamp;*seed=(UInt64)s.epoch+gZeroSeed;r=0;}
    }
    pthread_mutex_unlock(&gLock);return r;
}
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 client,UInt32 op,Boolean *will,Boolean *inPlace) {
    (void)d;(void)client;if (o!=kDevice || !will || !inPlace) return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gLock);*will=op==kAudioServerPlugInIOOperationWriteMix || op==kAudioServerPlugInIOOperationReadInput;*inPlace=true;pthread_mutex_unlock(&gLock);return 0;
}
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 c,UInt32 op,UInt32 frames,const AudioServerPlugInIOCycleInfo *i) {(void)d;(void)c;(void)op;return o==kDevice && frames<=4096 && i ? 0:kAudioHardwareIllegalOperationError;}
static int16_t s16(Float32 value) {if (!isfinite(value)) return 0;value=fmaxf(-1,fminf(1,value));return (int16_t)(value*32767.0f);}
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d,AudioObjectID o,AudioObjectID stream,UInt32 client,UInt32 op,UInt32 frames,const AudioServerPlugInIOCycleInfo *info,void *main,void *secondary) {
    (void)d;(void)client;(void)secondary;
    if (o!=kDevice || !info || !main || !frames || frames>4096) return kAudioHardwareIllegalOperationError;
    int input=op==kAudioServerPlugInIOOperationReadInput && stream==kInStream;
    int reference=op==kAudioServerPlugInIOOperationReadInput && stream==kReferenceStream;
    int output=op==kAudioServerPlugInIOOperationWriteMix && stream==kOutStream;
    if (!input && !reference && !output) return kAudioHardwareIllegalOperationError;
    if (input || reference) memset(main,0,frames*sizeof(Float32)*(reference ? 2:1));
    if (pthread_mutex_trylock(&gLock)) {
        note_io(0,1,output ? info->mOutputTime.mFlags:info->mInputTime.mFlags,frames,0,0);
        return 0; /* A dropped output period or silent input is an underrun. */
    }
    OSStatus result=kAudioHardwareNotRunningError;NaAudioStatus s={0};
    int snapshot=gControl && gIoUsers ? status_snapshot(&s,0):NA_MISSING;
    if (snapshot==NA_BUSY) {
        note_io(0,7,0,frames,0,snapshot);result=0;goto out;
    }
    if (!gControl || !gIoUsers || snapshot || s.epoch!=gEpoch ||
        !(s.state&NaAudioOutputReady) || (s.state&NaAudioFault)) {
        uint32_t flags=(gControl ? 1u:0u)|(gIoUsers ? 2u:0u)|(!snapshot ? 4u:0u)|
            (!snapshot && s.epoch==gEpoch ? 8u:0u)|(s.state<<8);
        note_io(1,2,flags,frames,result,snapshot);goto out;
    }
    const AudioTimeStamp *time=input || reference ? &info->mInputTime:&info->mOutputTime;
    Float64 sample=time->mSampleTime;
    if (!(time->mFlags&kAudioTimeStampSampleTimeValid) || !isfinite(sample) || sample<0 || floor(sample)!=sample || sample>NA_AUDIO_MAX_FRAME-frames) {
        result=kAudioHardwareIllegalOperationError;note_io(1,3,time->mFlags,frames,result,0);goto out;
    }
    uint64_t t=(uint64_t)sample;
    if (!input && op==kAudioServerPlugInIOOperationWriteMix && stream==kOutStream && gOutputSelected) {
        if (t+frames>s.out_consumed+NA_AUDIO_RING_FRAMES) {
            result=kAudioHardwareIllegalOperationError;note_io(1,5,time->mFlags,frames,result,(int64_t)t-(int64_t)s.out_consumed);goto out;
        }
        unsigned first=0;
        if (t<s.out_consumed) {
            uint64_t late=s.out_consumed-t;
            first=late<frames ? (unsigned)late:frames;
            note_io(0,4,time->mFlags,frames,0,-(int64_t)late);
        }
        Float32 g=gain();const Float32 *src=main;
        for (unsigned i=first;i<frames;++i) {
            int r=na_reference_mix(gOutput,gReference,t+i,s16(src[2*i]*g),s16(src[2*i+1]*g));
            if (r==NA_BUSY) {note_io(0,8,time->mFlags,frames,0,r);continue;}
            if (r && r!=NA_OWNERSHIP) {note_io(1,6,time->mFlags,frames,result,r);goto out;}
        }
        result=0;
    } else if (reference && gReferenceSelected) {
        Float32 *out=main;
        for (unsigned i=0;i<frames;++i) {
            int16_t left=0,right=0;
            if (!na_reference_read(gReference,t+i,&left,&right)) {
                out[2*i]=left/32768.0f;out[2*i+1]=right/32768.0f;
            }
        }
        result=0;
    } else if (input && gInputAvailable && gInputSelected) {
        uint64_t now=mach_absolute_time();
        if (!gCaptureIssued || now-gCaptureTouch>=s.host_hz/10) {
            /* This selector only updates a kernel request/lease, never waits
             * for ADC, DSP or DMA. Do not record on output-only StartIO. */
            kern_return_t requested=call(NaAudioStartInput);
            if (requested!=KERN_SUCCESS) {note_io(1,11,time->mFlags,frames,result,requested);goto out;}
            gCaptureIssued=1;gCaptureTouch=now;
        }
        Float32 *out=main;
        for (unsigned i=0;i<frames;++i) {
            int16_t value=0;
            if (t+i<s.in_produced && s.in_produced-(t+i)<=NA_AUDIO_RING_FRAMES && !na_ring_read(gInput,t+i,&value)) out[i]=value/32768.0f;
        }
        result=0;
    } else note_io(1,10,(gOutputSelected ? 1u:0u)|(gReferenceSelected ? 2u:0u)|
                        (gInputAvailable ? 4u:0u)|(gInputSelected ? 8u:0u),frames,result,stream);
out:pthread_mutex_unlock(&gLock);return result;
}
static OSStatus EndIOOperation(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 c,UInt32 op,UInt32 frames,const AudioServerPlugInIOCycleInfo *i) {(void)d;(void)c;(void)op;(void)frames;(void)i;return o==kDevice ? 0:kAudioHardwareBadObjectError;}
static AudioServerPlugInDriverInterface gInterface={NULL,QueryInterface,AddRef,Release,Initialize,CreateDevice,DestroyDevice,AddDeviceClient,RemoveDeviceClient,PerformDeviceConfigurationChange,AbortDeviceConfigurationChange,HasProperty,IsPropertySettable,GetPropertyDataSize,GetPropertyData,SetPropertyData,StartIO,StopIO,GetZeroTimeStamp,WillDoIOOperation,BeginIOOperation,DoIOOperation,EndIOOperation};
static AudioServerPlugInDriverInterface *gInterfacePtr=&gInterface;
static HRESULT QueryInterface(void *self,REFIID uuid,LPVOID *out) {
    if (!out) return E_NOINTERFACE;*out=NULL;CFUUIDRef requested=CFUUIDCreateFromUUIDBytes(NULL,uuid);
    if (!requested) return E_NOINTERFACE;HRESULT r=E_NOINTERFACE;
    if (CFEqual(requested,IUnknownUUID) || CFEqual(requested,kAudioServerPlugInDriverInterfaceUUID)) {AddRef(self);*out=&gInterfacePtr;r=S_OK;}
    CFRelease(requested);return r;
}
__attribute__((visibility("default"))) void *NezhaAudio27_Create(CFAllocatorRef allocator,CFUUIDRef type) {
    (void)allocator;return type && CFEqual(type,kAudioServerPlugInTypeUUID) ? &gInterfacePtr:NULL;
}
