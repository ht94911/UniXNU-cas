// SPDX-License-Identifier: BSD-2-Clause-Patent
// sheng AudioServerPlugIn architecture, with ABI2 ownership and independent
// input lifecycle. arm64e source only; original-donor load validation pending.
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <dlfcn.h>
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
enum {kPlugIn=kAudioObjectPlugInObject,kDevice=2,kOutStream=3,kInStream=4,kVolume=5};
#define kDeviceUID "NezhaAudio27Device"
#define kModelUID "NezhaAudio27Model"
static const Float64 kRate=NA_AUDIO_RATE;
static const UInt32 kZeroPeriod=NA_AUDIO_RING_FRAMES;
static const Float32 kMinDb=-36.0f;
static pthread_mutex_t gLock=PTHREAD_MUTEX_INITIALIZER;
static atomic_uint gRefs=1;
static AudioServerPlugInHostRef gHost;
static IONotificationPortRef gNotifications;
static io_iterator_t gMatches;
static CFRunLoopTimerRef gRetry;
static io_connect_t gConnect;
static mach_vm_address_t gMapAddress[3];
static const NaAudioStatus *gControl;
static uint64_t *gOutput;static const uint64_t *gInput;
static UInt32 gIoUsers;static int gInputAvailable,gOutputSelected=1,gInputSelected=1,gCaptureIssued;
static uint64_t gCaptureTouch;
static Float32 gVolume=0;
static uint64_t gZeroOrigin,gZeroSeed;
static uint32_t gEpoch;
static struct {UInt32 id,count;} gUsers[32];
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
    gIoUsers=0;memset(gUsers,0,sizeof gUsers);
    gCaptureIssued=0;gCaptureTouch=0;
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
    if (!frequency) return -1;
    Float64 observed=frequency();
    if (!isfinite(observed) || observed<1 || observed>UINT32_MAX || floor(observed)!=observed) return -1;
    if ((uint32_t)observed!=s->host_hz) {
        /* No caller-supplied address: kernel checks its exact donor UUID/code
         * and computes the one shared-cache literal from the task's slide. */
        if (call(NaAudioFixHostClock)!=KERN_SUCCESS) return -1;
        observed=frequency();
    }
    mach_timebase_info_data_t tb;
    if (mach_timebase_info(&tb)!=KERN_SUCCESS || !isfinite(observed) || observed!=s->host_hz) return -1;
    uint64_t q=0;
    return na_clock_contract(tb.numer,tb.denom,(uint32_t)observed,&q) || q!=s->ticks_per_frame_q32 ? -1:0;
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
    gEpoch=s.epoch;return 0;
bad:disconnect();return -1;
}
static int available(void) {
    NaAudioStatus s;
    return gControl && !na_audio_status_snapshot(gControl,&s) && s.epoch==gEpoch &&
        (s.state&NaAudioOutputReady) && !(s.state&NaAudioFault);
}
static void check_service(void) {
    pthread_mutex_lock(&gLock);
    int previous=gControl!=NULL;
    if (gControl && !available()) disconnect();
    int now=connect_service()==0;
    AudioServerPlugInHostRef host=gHost;
    pthread_mutex_unlock(&gLock);
    if (previous!=now && host) {
        AudioObjectPropertyAddress changed[2]={
            {kAudioPlugInPropertyDeviceList,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain},
            {kAudioObjectPropertyOwnedObjects,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain}};
        host->PropertiesChanged(host,kPlugIn,2,changed);
        if (previous) {
            AudioObjectPropertyAddress alive={kAudioDevicePropertyDeviceIsAlive,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
            host->PropertiesChanged(host,kDevice,1,&alive);
        }
    }
}
static void service_matched(void *context,io_iterator_t matches) {
    (void)context;io_object_t object;
    while ((object=IOIteratorNext(matches))) IOObjectRelease(object);
    check_service();
}
static void retry_service(CFRunLoopTimerRef timer,void *context) {(void)timer;(void)context;check_service();}
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
static ULONG Release(void *self) {
    (void)self;unsigned n=atomic_load(&gRefs);while (n && !atomic_compare_exchange_weak(&gRefs,&n,n-1)) {}
    if (n==1) {
        if (gRetry) {CFRunLoopTimerInvalidate(gRetry);CFRelease(gRetry);gRetry=NULL;}
        if (gNotifications) {IONotificationPortDestroy(gNotifications);gNotifications=NULL;}
        if (gMatches) {IOObjectRelease(gMatches);gMatches=0;}
        pthread_mutex_lock(&gLock);gHost=NULL;disconnect();pthread_mutex_unlock(&gLock);
    }return n ? n-1:0;
}
static OSStatus Initialize(AudioServerPlugInDriverRef d,AudioServerPlugInHostRef host) {
    (void)d;if (!host) return kAudioHardwareIllegalOperationError;gHost=host;
    pthread_mutex_lock(&gLock);int r=connect_service();pthread_mutex_unlock(&gLock);
    if (!gNotifications) {
        gNotifications=IONotificationPortCreate(kIOMainPortDefault);
        if (gNotifications) {
            CFRunLoopSourceRef source=IONotificationPortGetRunLoopSource(gNotifications);
            if (source) CFRunLoopAddSource(CFRunLoopGetCurrent(),source,kCFRunLoopCommonModes);
            kern_return_t kr=IOServiceAddMatchingNotification(gNotifications,kIOFirstMatchNotification,
                IOServiceMatching(NA_AUDIO_CLASS),service_matched,NULL,&gMatches);
            if (kr==KERN_SUCCESS) service_matched(NULL,gMatches); /* drain to arm */
            else {IONotificationPortDestroy(gNotifications);gNotifications=NULL;}
        }
    }
    /* A close may still be draining DSP ownership when the next client opens.
     * First-match notifications do not repeat for that case. Retry outside
     * all realtime callbacks, on the existing service-discovery run loop. */
    if (!gRetry) {
        CFRunLoopTimerContext context={0};
        gRetry=CFRunLoopTimerCreate(kCFAllocatorDefault,CFAbsoluteTimeGetCurrent()+1,1,0,0,retry_service,&context);
        if (gRetry) CFRunLoopAddTimer(CFRunLoopGetCurrent(),gRetry,kCFRunLoopCommonModes);
    }
    os_log_info(OS_LOG_DEFAULT,"NezhaAudio27: service %{public}s",r ? "unavailable":"connected");return 0;
}
static OSStatus CreateDevice(AudioServerPlugInDriverRef d,CFDictionaryRef p,const AudioServerPlugInClientInfo *c,AudioObjectID *o) {(void)d;(void)p;(void)c;(void)o;return kAudioHardwareUnsupportedOperationError;}
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d,AudioObjectID o) {(void)d;(void)o;return kAudioHardwareUnsupportedOperationError;}
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d,AudioObjectID o,const AudioServerPlugInClientInfo *c) {(void)d;return o==kDevice && c ? 0:kAudioHardwareBadObjectError;}
static OSStatus StopIO(AudioServerPlugInDriverRef,AudioObjectID,UInt32);
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d,AudioObjectID o,const AudioServerPlugInClientInfo *c) {
    (void)d;if (o!=kDevice || !c) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    for (unsigned i=0;i<32;++i) if (gUsers[i].count && gUsers[i].id==c->mClientID) {gIoUsers-=gUsers[i].count;gUsers[i].count=0;break;}
    if (!gIoUsers) {call(NaAudioStopOutput);call(NaAudioStopInput);}
    pthread_mutex_unlock(&gLock);return 0;
}
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d,AudioObjectID o,UInt64 action,void *info) {(void)d;(void)o;(void)action;(void)info;return kAudioHardwareUnsupportedOperationError;}
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d,AudioObjectID o,UInt64 action,void *info) {(void)d;(void)o;(void)action;(void)info;return 0;}
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 size,UInt32 *written,void *out) {
    (void)d;(void)pid;if (!a || !written || (!out && size)) return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gLock);
    if (o==kPlugIn && !gControl) connect_service();
    OSStatus r=propertyData(o,a,qs,q,size,written,out);pthread_mutex_unlock(&gLock);return r;
}
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 *size) {
    if (!size) return kAudioHardwareIllegalOperationError;
    _Alignas(16) uint8_t scratch[1024]={0};return GetPropertyData(d,o,pid,a,qs,q,sizeof scratch,size,scratch);
}
static Boolean HasProperty(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a) {UInt32 size=0;return GetPropertyDataSize(d,o,pid,a,0,NULL,&size)==0;}
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a,Boolean *value) {
    if (!a || !value) return kAudioHardwareIllegalOperationError;
    *value=(o==kVolume && (a->mSelector==kAudioLevelControlPropertyScalarValue || a->mSelector==kAudioLevelControlPropertyDecibelValue)) ||
        ((o==kOutStream || o==kInStream) && (a->mSelector==kAudioStreamPropertyIsActive || a->mSelector==kAudioStreamPropertyVirtualFormat || a->mSelector==kAudioStreamPropertyPhysicalFormat)) ||
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
    } else if ((o==kOutStream || o==kInStream) && a->mSelector==kAudioStreamPropertyIsActive && size==sizeof(UInt32)) {
        UInt32 active=*(const UInt32 *)data;
        if (active>1 || (o==kInStream && !gInputAvailable)) result=kAudioHardwareIllegalOperationError;
        else {
            uint32_t selector=o==kOutStream ? (active ? NaAudioStartOutput:NaAudioStopOutput):(active ? NaAudioStartInput:NaAudioStopInput);
            if (gIoUsers && (o==kOutStream || !active) && call(selector)!=KERN_SUCCESS) result=kAudioHardwareNotRunningError;
            else {if (o==kOutStream) gOutputSelected=active;else gInputSelected=active;result=0;}
        }
    } else if ((o==kOutStream || o==kInStream) && (a->mSelector==kAudioStreamPropertyPhysicalFormat || a->mSelector==kAudioStreamPropertyVirtualFormat)) {
        AudioStreamBasicDescription expected=format(o==kOutStream ? 2:1);
        result=size==sizeof expected && !memcmp(data,&expected,sizeof expected) ? 0:kAudioDeviceUnsupportedFormatError;
    } else if (o==kDevice && a->mSelector==kAudioDevicePropertyNominalSampleRate)
        result=size==sizeof(Float64) && *(const Float64 *)data==kRate ? 0:kAudioDeviceUnsupportedFormatError;
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
        if (na_audio_status_snapshot(gControl,&s) || clock_ready(&s)) goto out;
        if (gOutputSelected && call(NaAudioStartOutput)!=KERN_SUCCESS) goto out;
        gCaptureIssued=0;gCaptureTouch=0; /* Input starts on actual ReadInput. */
        if (na_clock_frames(s.anchor_host,mach_absolute_time(),s.ticks_per_frame_q32,&gZeroOrigin)) {call(NaAudioStopOutput);call(NaAudioStopInput);goto out;}
        ++gZeroSeed;
    }
    gUsers[slot].id=client;++gUsers[slot].count;++gIoUsers;r=0;
out:pthread_mutex_unlock(&gLock);return r;
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
    if (pthread_mutex_trylock(&gLock)) return kAudioHardwareNotRunningError;
    NaAudioStatus s;uint64_t frames,stamp;OSStatus r=kAudioHardwareNotRunningError;
    if (gControl && gIoUsers && !na_audio_status_snapshot(gControl,&s) && s.epoch==gEpoch &&
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
    pthread_mutex_lock(&gLock);*will=op==kAudioServerPlugInIOOperationWriteMix || (op==kAudioServerPlugInIOOperationReadInput && gInputAvailable);*inPlace=true;pthread_mutex_unlock(&gLock);return 0;
}
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 c,UInt32 op,UInt32 frames,const AudioServerPlugInIOCycleInfo *i) {(void)d;(void)c;(void)op;return o==kDevice && frames<=4096 && i ? 0:kAudioHardwareIllegalOperationError;}
static int16_t s16(Float32 value) {if (!isfinite(value)) return 0;value=fmaxf(-1,fminf(1,value));return (int16_t)(value*32767.0f);}
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d,AudioObjectID o,AudioObjectID stream,UInt32 client,UInt32 op,UInt32 frames,const AudioServerPlugInIOCycleInfo *info,void *main,void *secondary) {
    (void)d;(void)client;(void)secondary;
    if (o!=kDevice || !info || !main || !frames || frames>4096) return kAudioHardwareIllegalOperationError;
    int input=op==kAudioServerPlugInIOOperationReadInput && stream==kInStream;
    if (input) memset(main,0,frames*sizeof(Float32));
    if (pthread_mutex_trylock(&gLock)) return kAudioHardwareNotRunningError;
    OSStatus result=kAudioHardwareNotRunningError;NaAudioStatus s;
    if (!gControl || !gIoUsers || na_audio_status_snapshot(gControl,&s) || s.epoch!=gEpoch || (s.state&NaAudioFault)) goto out;
    const AudioTimeStamp *time=input ? &info->mInputTime:&info->mOutputTime;
    Float64 sample=time->mSampleTime;
    if (!(time->mFlags&kAudioTimeStampSampleTimeValid) || !isfinite(sample) || sample<0 || floor(sample)!=sample || sample>NA_AUDIO_MAX_FRAME-frames) goto out;
    uint64_t t=(uint64_t)sample;
    if (!input && op==kAudioServerPlugInIOOperationWriteMix && stream==kOutStream && gOutputSelected) {
        if (t<s.out_consumed || t+frames>s.out_consumed+NA_AUDIO_RING_FRAMES) goto out;
        Float32 g=gain();const Float32 *src=main;
        for (unsigned i=0;i<frames;++i) {
            int r=na_ring_mix(gOutput,t+i,s16(src[2*i]*g),s16(src[2*i+1]*g));
            if (r && r!=NA_OWNERSHIP) goto out;
        }
        result=0;
    } else if (input && gInputAvailable && gInputSelected) {
        uint64_t now=mach_absolute_time();
        if (!gCaptureIssued || now-gCaptureTouch>=s.host_hz/10) {
            /* This selector only updates a kernel request/lease, never waits
             * for ADC, DSP or DMA. Do not record on output-only StartIO. */
            if (call(NaAudioStartInput)!=KERN_SUCCESS) goto out;
            gCaptureIssued=1;gCaptureTouch=now;
        }
        Float32 *out=main;
        for (unsigned i=0;i<frames;++i) {
            int16_t value=0;
            if (t+i<s.in_produced && s.in_produced-(t+i)<=NA_AUDIO_RING_FRAMES && !na_ring_read(gInput,t+i,&value)) out[i]=value/32768.0f;
        }
        result=0;
    }
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
