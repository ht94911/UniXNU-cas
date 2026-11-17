/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved.
 * AudioServerPlugIn property model: FoXLinne/UniXNU_marble7a0d68ff.
 * A single Actuator device only. The system retains its original policies,
 * haptic synthesis, audio routing, settings and completion semantics. */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "../NezhaHapticsProtocol.h"
enum { kPlugin=kAudioObjectPlugInObject,kActuator=2,kStream=3,kPeriod=4096,kSafety=512,kClients=32 };
static AudioServerPlugInHostRef host;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static atomic_uint references=1;
static int endpoint=-1;
static atomic_int active=1;
static uint32_t session,epoch,sequence;
static atomic_uint users;
static uint64_t anchor,seed;
static double ticksPerFrame;
static struct {uint32_t id,count;} clients[kClients];
static atomic_uint writes,framesWritten,rejected,lastError;
static atomic_uint initialized,starts,stops,propertyQueries;
static atomic_uint lastUnknownObject,lastUnknownSelector;
static HRESULT QueryInterface(void *self,REFIID uuid,LPVOID *out) {
    if(!out)return E_POINTER;
    CFUUIDRef requested=CFUUIDCreateFromUUIDBytes(NULL,uuid);if(!requested)return E_NOINTERFACE;
    Boolean same=CFEqual(requested,IUnknownUUID)||CFEqual(requested,kAudioServerPlugInDriverInterfaceUUID);
    CFRelease(requested);*out=same?self:NULL;if(same)atomic_fetch_add(&references,1);
    return same?S_OK:E_NOINTERFACE;
}
static ULONG AddRef(void *self) {(void)self;return atomic_fetch_add(&references,1)+1;}
static ULONG Release(void *self) {
    (void)self;unsigned n=atomic_load(&references);
    while(n>1&&!atomic_compare_exchange_weak(&references,&n,n-1)){}
    return n>1?n-1:n;
}
static int state(NzHapticsStatus *s) {
    if(ioctl(endpoint,NZ_HAPTICS_GET_STATUS,s)<0)return errno;
    if(s->version!=NZ_HAPTICS_ABI || !s->session || !s->ready)return ENXIO;
    return 0;
}
static int writePacket(const NzHapticsPacket *p,size_t bytes) {
    ssize_t n=write(endpoint,p,bytes);return n==(ssize_t)bytes?0:n<0?errno:EIO;
}
static int startEndpoint(void) {
    if(endpoint<0)endpoint=open(NZ_HAPTICS_DEVICE,O_WRONLY|O_NONBLOCK|O_CLOEXEC);
    if(endpoint<0)return errno;
    NzHapticsStatus s={0};int e=state(&s);
    if(!e) {
        NzHapticsPacket p={NZ_HAPTICS_MAGIC,NZ_HAPTICS_ABI,NzHapticsStart,0,s.session,s.epoch,0,0,0};
        e=writePacket(&p,sizeof p);
    }
    if(!e)e=state(&s);
    if(!e){session=s.session;epoch=s.epoch;sequence=s.next_sequence;anchor=mach_absolute_time();++seed;}
    if(e){close(endpoint);endpoint=-1;session=epoch=sequence=0;}
    atomic_store(&lastError,(uint32_t)e);return e;
}
static int stopEndpoint(void) {
    if(endpoint<0)return 0;
    NzHapticsPacket p={NZ_HAPTICS_MAGIC,NZ_HAPTICS_ABI,NzHapticsStop,0,session,epoch,sequence,0,0};
    int e=writePacket(&p,sizeof p);close(endpoint);endpoint=-1;session=epoch=sequence=0;
    atomic_store(&lastError,(uint32_t)e);return e;
}
static OSStatus Initialize(AudioServerPlugInDriverRef d,AudioServerPlugInHostRef h) {
    (void)d;mach_timebase_info_data_t t;
    if(mach_timebase_info(&t)||!t.numer||!t.denom)return kAudioHardwareUnspecifiedError;
    pthread_mutex_lock(&gate);host=h;ticksPerFrame=1e9*t.denom/t.numer/NZ_HAPTICS_RATE;
    anchor=mach_absolute_time();seed=1;atomic_fetch_add(&initialized,1);pthread_mutex_unlock(&gate);
    /* Observe the same endpoint the HAL will use without starting playback.
     * The mature Pagani HAL checks its service at Initialize, before StartIO.
     * Closing this unused descriptor retires the session through normal close. */
    int descriptor=open(NZ_HAPTICS_DEVICE,O_WRONLY|O_NONBLOCK|O_CLOEXEC);
    int error=descriptor<0?errno:0;
    if(descriptor>=0) {
        NzHapticsStatus status={0};
        if(ioctl(descriptor,NZ_HAPTICS_GET_STATUS,&status)<0)error=errno;
        else if(status.version!=NZ_HAPTICS_ABI || !status.ready)error=ENXIO;
        close(descriptor);
    }
    atomic_store(&lastError,(uint32_t)error);return 0;
}
static OSStatus CreateDevice(AudioServerPlugInDriverRef d,CFDictionaryRef x,const AudioServerPlugInClientInfo *c,AudioObjectID *o) {
    (void)d;(void)x;(void)c;(void)o;return kAudioHardwareUnsupportedOperationError;
}
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d,AudioObjectID o) {(void)d;(void)o;return kAudioHardwareUnsupportedOperationError;}
static void removeClient(uint32_t id) {
    for(unsigned i=0;i<kClients;++i)if(clients[i].count&&clients[i].id==id) {
        users-=clients[i].count;clients[i].count=0;if(!users)stopEndpoint();return;
    }
}
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d,AudioObjectID o,const AudioServerPlugInClientInfo *c) {
    (void)d;if(o!=kActuator||!c)return kAudioHardwareBadObjectError;return 0;
}
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d,AudioObjectID o,const AudioServerPlugInClientInfo *c) {
    (void)d;if(o!=kActuator||!c)return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gate);removeClient(c->mClientID);pthread_mutex_unlock(&gate);return 0;
}
static OSStatus ConfigurationChange(AudioServerPlugInDriverRef d,AudioObjectID o,UInt64 action,void *data) {
    (void)d;(void)action;(void)data;return o==kActuator?0:kAudioHardwareBadObjectError;
}
static AudioStreamBasicDescription format(void) {
    return (AudioStreamBasicDescription){.mSampleRate=NZ_HAPTICS_RATE,.mFormatID=kAudioFormatLinearPCM,
        .mFormatFlags=kAudioFormatFlagIsFloat|kAudioFormatFlagsNativeEndian|kAudioFormatFlagIsPacked,
        .mBytesPerPacket=4,.mFramesPerPacket=1,.mBytesPerFrame=4,.mChannelsPerFrame=1,.mBitsPerChannel=32};
}
typedef struct {
    UInt32 size;Boolean found,string,diagnostic;
    union {UInt32 words[4];Float64 rate;CFStringRef text;AudioValueRange range;
        AudioStreamBasicDescription format;AudioStreamRangedDescription ranged;AudioChannelLayout layout;
        AudioServerPlugInCustomPropertyInfo custom;} data;
} Value;
static Value property(AudioObjectID o,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q) {
    Value v={0};if(!a||(o!=kPlugin&&o!=kActuator&&o!=kStream))return v;
    atomic_fetch_add(&propertyQueries,1);
#define WORD(x) do{v.found=1;v.size=4;v.data.words[0]=(x);}while(0)
#define EMPTY() do{v.found=1;v.size=0;}while(0)
#define TEXT(x) do{v.found=v.string=1;v.size=sizeof(CFStringRef);v.data.text=CFSTR(x);}while(0)
    switch(a->mSelector) {
        case kAudioObjectPropertyBaseClass:WORD(kAudioObjectClassID);return v;
        case kAudioObjectPropertyClass:WORD(o==kPlugin?kAudioPlugInClassID:o==kActuator?kAudioDeviceClassID:kAudioStreamClassID);return v;
        case kAudioObjectPropertyOwner:WORD(o==kPlugin?kAudioObjectUnknown:o==kActuator?kPlugin:kActuator);return v;
        case kAudioObjectPropertyManufacturer:TEXT("UniXNU");return v;
        case kAudioObjectPropertyName:case kAudioObjectPropertyModelName:TEXT("Actuator");return v;
        case kAudioObjectPropertyElementName:TEXT("");return v;
        case kAudioObjectPropertyIdentify:WORD(0);return v;
        case 'nhpt':
            v.found=v.diagnostic=1;v.size=sizeof(CFPropertyListRef);return v;
        case kAudioObjectPropertyCustomPropertyInfoList:
            v.found=1;v.size=sizeof(v.data.custom);
            v.data.custom=(AudioServerPlugInCustomPropertyInfo){'nhpt',
                kAudioServerPlugInCustomPropertyDataTypeCFPropertyList,
                kAudioServerPlugInCustomPropertyDataTypeNone};return v;
    }
    if(o==kPlugin) {
        switch(a->mSelector) {
            case kAudioObjectPropertyOwnedObjects:case kAudioPlugInPropertyDeviceList:WORD(kActuator);break;
            case kAudioPlugInPropertyBoxList:case kAudioPlugInPropertyClockDeviceList:EMPTY();break;
            case kAudioPlugInPropertyTranslateUIDToDevice:
                WORD(kAudioObjectUnknown);if(qs==sizeof(CFStringRef)&&q&&*(CFStringRef const *)q&&
                    CFEqual(*(CFStringRef const *)q,CFSTR("Actuator")))v.data.words[0]=kActuator;
                break;
            case kAudioPlugInPropertyTranslateUIDToBox:case kAudioPlugInPropertyTranslateUIDToClockDevice:WORD(kAudioObjectUnknown);break;
            case kAudioPlugInPropertyResourceBundle:TEXT("");break;
        }
    } else if(o==kActuator) {
        switch(a->mSelector) {
            case kAudioDevicePropertyDeviceUID:TEXT("Actuator");break;
            case kAudioDevicePropertyModelUID:TEXT("NezhaHaptics27");break;
            case kAudioDevicePropertyTransportType:WORD(kAudioDeviceTransportTypeBuiltIn);break;
            case kAudioObjectPropertySerialNumber:TEXT("nezha-haptics27");break;
            case kAudioObjectPropertyFirmwareVersion:TEXT("1");break;
            case kAudioDevicePropertyRelatedDevices:WORD(kActuator);break;
            case kAudioDevicePropertyConfigurationApplication:TEXT("com.apple.Preferences");break;
            case 'accs':case 'auto':WORD(0);break;
            case kAudioObjectPropertyOwnedObjects:case kAudioDevicePropertyStreams:
                if(a->mScope==kAudioObjectPropertyScopeInput)EMPTY();else WORD(kStream);break;
            case kAudioObjectPropertyControlList:EMPTY();break;
            case kAudioDevicePropertyDeviceIsAlive:WORD(1);break;
            case kAudioDevicePropertyDeviceIsRunning:WORD(users!=0);break;
            case kAudioDevicePropertyDeviceCanBeDefaultDevice:case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:WORD(0);break;
            case kAudioDevicePropertyLatency:case kAudioDevicePropertyClockDomain:case kAudioDevicePropertyIsHidden:WORD(0);break;
            case kAudioDevicePropertySafetyOffset:WORD(kSafety);break;
            case kAudioDevicePropertyClockIsStable:WORD(1);break;
            case kAudioDevicePropertyClockAlgorithm:WORD(kAudioDeviceClockAlgorithmRaw);break;
            case kAudioDevicePropertyZeroTimeStampPeriod:WORD(kPeriod);break;
            case kAudioDevicePropertyNominalSampleRate:v.found=1;v.size=sizeof(Float64);v.data.rate=NZ_HAPTICS_RATE;break;
            case kAudioDevicePropertyAvailableNominalSampleRates:v.found=1;v.size=sizeof(AudioValueRange);v.data.range=(AudioValueRange){NZ_HAPTICS_RATE,NZ_HAPTICS_RATE};break;
            case kAudioDevicePropertyPreferredChannelsForStereo:WORD(1);v.data.words[1]=1;v.size=8;break;
            case kAudioDevicePropertyPreferredChannelLayout:
                v.found=1;v.size=sizeof(AudioChannelLayout);v.data.layout.mChannelLayoutTag=kAudioChannelLayoutTag_UseChannelDescriptions;
                v.data.layout.mNumberChannelDescriptions=1;v.data.layout.mChannelDescriptions[0].mChannelLabel=kAudioChannelLabel_Mono;break;
        }
    } else {
        switch(a->mSelector) {
            case kAudioObjectPropertyOwnedObjects:case 'taps':EMPTY();break;
            case kAudioStreamPropertyIsActive:WORD(active);break;
            case kAudioStreamPropertyDirection:WORD(0);break;
            case kAudioStreamPropertyTerminalType:WORD(kAudioStreamTerminalTypeSpeaker);break;
            case kAudioStreamPropertyStartingChannel:WORD(1);break;
            case kAudioStreamPropertyLatency:WORD(0);break;
            case kAudioStreamPropertyVirtualFormat:case kAudioStreamPropertyPhysicalFormat:
                v.found=1;v.size=sizeof(AudioStreamBasicDescription);v.data.format=format();break;
            case kAudioStreamPropertyAvailableVirtualFormats:case kAudioStreamPropertyAvailablePhysicalFormats:
                v.found=1;v.size=sizeof(AudioStreamRangedDescription);v.data.ranged.mFormat=format();
                v.data.ranged.mSampleRateRange=(AudioValueRange){NZ_HAPTICS_RATE,NZ_HAPTICS_RATE};break;
        }
    }
#undef WORD
#undef EMPTY
#undef TEXT
    if(!v.found){atomic_store(&lastUnknownObject,o);atomic_store(&lastUnknownSelector,a->mSelector);}
    return v;
}
static Boolean HasProperty(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a) {
    (void)d;(void)pid;return property(o,a,0,NULL).found;
}
static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a,Boolean *out) {
    (void)d;(void)pid;if(!a||!out)return kAudioHardwareIllegalOperationError;
    if(!property(o,a,0,NULL).found)return kAudioHardwareUnknownPropertyError;
    *out=(o==kActuator&&a->mSelector==kAudioDevicePropertyNominalSampleRate)||(o==kStream&&
        (a->mSelector==kAudioStreamPropertyIsActive||a->mSelector==kAudioStreamPropertyVirtualFormat||a->mSelector==kAudioStreamPropertyPhysicalFormat));return 0;
}
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 *n) {
    (void)d;(void)pid;if(!n)return kAudioHardwareIllegalOperationError;Value v=property(o,a,qs,q);
    if(!v.found)return kAudioHardwareUnknownPropertyError;*n=v.size;return 0;
}
static CFDictionaryRef diagnostics(void) {
    const void *keys[]={CFSTR("initialized"),CFSTR("starts"),CFSTR("writes"),CFSTR("lastError"),
        CFSTR("stops"),CFSTR("propertyQueries"),CFSTR("lastUnknownObject"),CFSTR("lastUnknownSelector")};
    uint32_t counters[]={atomic_load(&initialized),atomic_load(&starts),atomic_load(&writes),atomic_load(&lastError),
        atomic_load(&stops),atomic_load(&propertyQueries),atomic_load(&lastUnknownObject),atomic_load(&lastUnknownSelector)};
    CFNumberRef values[sizeof(counters)/sizeof(counters[0])]={0};unsigned made=0;
    for(;made<sizeof(values)/sizeof(values[0]);++made) {
        int64_t value=counters[made];values[made]=CFNumberCreate(NULL,kCFNumberSInt64Type,&value);
        if(!values[made])break;
    }
    CFDictionaryRef result=made==sizeof(values)/sizeof(values[0])?CFDictionaryCreate(NULL,keys,
        (const void **)values,made,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks):NULL;
    while(made)CFRelease(values[--made]);return result;
}
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 capacity,UInt32 *n,void *data) {
    (void)d;(void)pid;if(!n)return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&gate);Value v=property(o,a,qs,q);pthread_mutex_unlock(&gate);
    if(!v.found)return kAudioHardwareUnknownPropertyError;if(capacity<v.size||(v.size&&!data))return kAudioHardwareBadPropertySizeError;
    *n=v.size;
    if(v.diagnostic) {
        CFDictionaryRef value=diagnostics();if(!value)return kAudioHardwareUnspecifiedError;
        memcpy(data,&value,sizeof(value));return 0;
    }
    if(v.size){memcpy(data,&v.data,v.size);if(v.string)CFRetain(v.data.text);}return 0;
}
static OSStatus SetPropertyData(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 qs,const void *q,UInt32 n,const void *data) {
    (void)qs;(void)q;Boolean settable=0;OSStatus e=IsPropertySettable(d,o,pid,a,&settable);
    if(e)return e;if(!settable||!data)return kAudioHardwareIllegalOperationError;
    if(o==kActuator)return n==sizeof(Float64)&&*(const Float64 *)data==NZ_HAPTICS_RATE?0:kAudioDeviceUnsupportedFormatError;
    if(a->mSelector==kAudioStreamPropertyIsActive) {
        if(n!=4)return kAudioHardwareBadPropertySizeError;
        pthread_mutex_lock(&gate);int next=*(const UInt32 *)data!=0,changed=next!=active;
        if(changed&&!next){stopEndpoint();users=0;memset(clients,0,sizeof clients);}active=next;
        AudioServerPlugInHostRef h=host;pthread_mutex_unlock(&gate);
        if(changed&&h&&h->PropertiesChanged)h->PropertiesChanged(h,kStream,1,a);return 0;
    }
    if(n!=sizeof(AudioStreamBasicDescription))return kAudioHardwareBadPropertySizeError;
    AudioStreamBasicDescription f=format();return !memcmp(data,&f,sizeof f)?0:kAudioDeviceUnsupportedFormatError;
}
static OSStatus StartIO(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 id) {
    (void)d;if(o!=kActuator)return kAudioHardwareBadObjectError;
    atomic_fetch_add(&starts,1);
    pthread_mutex_lock(&gate);unsigned slot=kClients;
    for(unsigned i=0;i<kClients;++i)if(clients[i].count&&clients[i].id==id){slot=i;break;}
    if(slot==kClients)for(unsigned i=0;i<kClients;++i)if(!clients[i].count){slot=i;break;}
    int e=slot==kClients||!active||users==UINT32_MAX||
        (slot<kClients && clients[slot].count==UINT32_MAX)?EBUSY:0;
    if(!e&&!users)e=startEndpoint();
    if(!e){clients[slot].id=id;++clients[slot].count;++users;}
    pthread_mutex_unlock(&gate);return e?kAudioHardwareNotRunningError:0;
}
static OSStatus StopIO(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 id) {
    (void)d;if(o!=kActuator)return kAudioHardwareBadObjectError;
    atomic_fetch_add(&stops,1);
    pthread_mutex_lock(&gate);int e=0;
    for(unsigned i=0;i<kClients;++i)if(clients[i].count&&clients[i].id==id) {
        --clients[i].count;--users;if(!users)e=stopEndpoint();break;
    }
    pthread_mutex_unlock(&gate);return e?kAudioHardwareUnspecifiedError:0;
}
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 id,Float64 *sample,UInt64 *hostTime,UInt64 *clockSeed) {
    (void)d;(void)id;if(o!=kActuator||!sample||!hostTime||!clockSeed)return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gate);uint64_t now=mach_absolute_time();double period=ticksPerFrame*kPeriod;
    uint64_t n=anchor&&now>anchor&&period>0?(uint64_t)((now-anchor)/period):0;
    *sample=(double)n*kPeriod;*hostTime=anchor+(uint64_t)(n*period);*clockSeed=seed;pthread_mutex_unlock(&gate);return 0;
}
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 id,UInt32 op,Boolean *will,Boolean *inPlace) {
    (void)d;(void)id;if(o!=kActuator||!will||!inPlace)return kAudioHardwareBadObjectError;
    *will=op==kAudioServerPlugInIOOperationWriteMix;*inPlace=1;return 0;
}
static OSStatus BeginEndIOOperation(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 id,UInt32 op,UInt32 n,const AudioServerPlugInIOCycleInfo *c) {
    (void)d;(void)id;(void)op;(void)n;(void)c;return o==kActuator?0:kAudioHardwareBadObjectError;
}
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d,AudioObjectID o,AudioObjectID stream,UInt32 id,UInt32 op,UInt32 n,const AudioServerPlugInIOCycleInfo *cycle,void *buffer,void *secondary) {
    (void)d;(void)secondary;if(o!=kActuator||stream!=kStream)return kAudioHardwareBadObjectError;
    if(op!=kAudioServerPlugInIOOperationWriteMix)return 0;
    if(!buffer||!cycle||!n||n>NZ_HAPTICS_MAX_FRAMES)return kAudioHardwareIllegalOperationError;
    if(pthread_mutex_trylock(&gate))return kAudioHardwareNotRunningError;
    if(!users||endpoint<0||!active){pthread_mutex_unlock(&gate);return kAudioHardwareNotRunningError;}
    Boolean owns=0;for(unsigned i=0;i<kClients;++i)if(clients[i].id==id&&clients[i].count){owns=1;break;}
    if(!owns){pthread_mutex_unlock(&gate);return kAudioHardwareNotRunningError;}
    struct {NzHapticsPacket header;int16_t pcm[NZ_HAPTICS_MAX_FRAMES];} packet;
    packet.header=(NzHapticsPacket){NZ_HAPTICS_MAGIC,NZ_HAPTICS_ABI,NzHapticsPCM,n,session,epoch,sequence,0,0};
    if(cycle->mOutputTime.mFlags&kAudioTimeStampHostTimeValid)packet.header.host_time=cycle->mOutputTime.mHostTime;
    else if((cycle->mOutputTime.mFlags&kAudioTimeStampSampleTimeValid)&&isfinite(cycle->mOutputTime.mSampleTime)&&
            cycle->mOutputTime.mSampleTime>=0) {
        double at=cycle->mOutputTime.mSampleTime*ticksPerFrame;
        if(at>(double)(UINT64_MAX-anchor)){pthread_mutex_unlock(&gate);return kAudioHardwareIllegalOperationError;}
        packet.header.host_time=anchor+(uint64_t)at;
    } else {pthread_mutex_unlock(&gate);return kAudioHardwareIllegalOperationError;}
    if(packet.header.host_time<anchor){pthread_mutex_unlock(&gate);return kAudioHardwareNotRunningError;}
    const float *pcm=buffer;
    for(unsigned i=0;i<n;++i)packet.pcm[i]=!isfinite(pcm[i])?0:pcm[i]>=1?INT16_MAX:pcm[i]<=-1?INT16_MIN:(int16_t)(pcm[i]*32767);
    int e=writePacket(&packet.header,sizeof packet.header+n*2);
    atomic_fetch_add(&writes,1);if(!e){++sequence;atomic_fetch_add(&framesWritten,n);}else{
        atomic_fetch_add(&rejected,1);atomic_store(&lastError,(uint32_t)e);
        /* Realtime failure only closes the descriptor. The kernel close queues
         * cancellation; the normal StopIO path may wait for a hardware stop. */
        close(endpoint);endpoint=-1;session=epoch=sequence=0;users=0;memset(clients,0,sizeof clients);
    }
    pthread_mutex_unlock(&gate);return e?kAudioHardwareUnspecifiedError:0;
}
static AudioServerPlugInDriverInterface driver={NULL,QueryInterface,AddRef,Release,Initialize,CreateDevice,DestroyDevice,
    AddDeviceClient,RemoveDeviceClient,ConfigurationChange,ConfigurationChange,HasProperty,IsPropertySettable,
    GetPropertyDataSize,GetPropertyData,SetPropertyData,StartIO,StopIO,GetZeroTimeStamp,WillDoIOOperation,
    BeginEndIOOperation,DoIOOperation,BeginEndIOOperation};
static AudioServerPlugInDriverInterface *driverPointer=&driver;
__attribute__((visibility("default"))) void *NezhaHaptics_Create(CFAllocatorRef allocator,CFUUIDRef type) {
    (void)allocator;return type&&CFEqual(type,kAudioServerPlugInTypeUUID)?&driverPointer:NULL;
}
