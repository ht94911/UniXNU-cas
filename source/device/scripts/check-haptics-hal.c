// SPDX-License-Identifier: BSD-2-Clause-Patent
// Execute production HAL property marshalling and Initialize without hardware IO.
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <errno.h>
#include <dlfcn.h>
#include "../package/haptics27/NezhaHapticsProtocol.h"
static unsigned opens,closes,packets;
static int write_error;
static NzHapticsPacket last_packet;
static unsigned clock_fixes;
static double observed_hz=24000000;
static unsigned clock_numer=125,clock_denom=3;
static int clock_fix_error;
static double observe_frequency(void) {return observed_hz;}
static uint64_t observe_to_nanos(uint64_t ticks) {return (uint64_t)((__uint128_t)ticks*clock_numer/clock_denom);}
static uint64_t observe_to_host(uint64_t nanos) {return (uint64_t)((__uint128_t)nanos*clock_denom/clock_numer);}
static void *observe_symbol(void *handle,const char *name) {
    (void)handle;
    if(!strcmp(name,"AudioGetHostClockFrequency"))return (void *)observe_frequency;
    if(!strcmp(name,"AudioConvertHostTimeToNanos"))return (void *)observe_to_nanos;
    if(!strcmp(name,"AudioConvertNanosToHostTime"))return (void *)observe_to_host;
    return NULL;
}
static kern_return_t observe_timebase(mach_timebase_info_t timebase) {
    timebase->numer=625;timebase->denom=12;return 0;
}
static int observe_open(const char *path,int flags,...)
{
    assert(!strcmp(path,NZ_HAPTICS_DEVICE));assert(flags&O_WRONLY);++opens;return 77;
}
static int observe_close(int descriptor) {assert(descriptor==77);++closes;return 0;}
static int observe_ioctl(int descriptor,unsigned long command,...)
{
    assert(descriptor==77);
    if(command==NZ_HAPTICS_FIX_HOST_CLOCK) {
        ++clock_fixes;
        if(clock_fix_error){errno=clock_fix_error;return -1;}
        observed_hz=19200000;clock_numer=625;clock_denom=12;return 0;
    }
    assert(command==NZ_HAPTICS_GET_STATUS);
    va_list list;va_start(list,command);NzHapticsStatus *state=va_arg(list,NzHapticsStatus *);va_end(list);
    *state=(NzHapticsStatus){.version=NZ_HAPTICS_ABI,.session=1,.epoch=1,.next_sequence=1,.ready=1};return 0;
}
static ssize_t observe_write(int descriptor,const void *data,size_t bytes)
{
    assert(descriptor==77 && data && bytes>=sizeof(NzHapticsPacket));++packets;
    memcpy(&last_packet,data,sizeof(last_packet));
    if(write_error){errno=write_error;return -1;}
    return (ssize_t)bytes;
}
#define open observe_open
#define close observe_close
#define ioctl observe_ioctl
#define write observe_write
#define dlsym observe_symbol
#define mach_timebase_info observe_timebase
#include "../package/haptics27/hal/NezhaHapticsPlugIn.c"
#undef open
#undef close
#undef ioctl
#undef write
#undef dlsym
#undef mach_timebase_info
int main(void)
{
    assert(Initialize(&driverPointer,NULL)==0);
    assert(opens==1 && closes==1 && packets==0 && endpoint<0 && !users);
    assert(clock_fixes==1 && clockReady && clockHz==19200000 && clockNanos==1000000000 && clockTicks==19200000);
    assert(prepareClock(77,625,12)==0 && clock_fixes==2); /* verify inlined renderer too */
    clock_fix_error=EPERM;observed_hz=24000000;clock_numer=125;clock_denom=3;
    assert(prepareClock(77,625,12)==EPERM && clock_fixes==3);
    clock_fix_error=0;assert(prepareClock(77,625,12)==0 && clock_fixes==4);
    AudioObjectPropertyAddress address={kAudioObjectPropertyCustomPropertyInfoList,kAudioObjectPropertyScopeGlobal,0};
    UInt32 size=0;
    assert(GetPropertyDataSize(&driverPointer,kActuator,0,&address,0,NULL,&size)==0);
    assert(size==sizeof(AudioServerPlugInCustomPropertyInfo));
    AudioServerPlugInCustomPropertyInfo custom={0};
    assert(GetPropertyData(&driverPointer,kActuator,0,&address,0,NULL,sizeof(custom),&size,&custom)==0);
    assert(custom.mSelector=='nhpt' && custom.mPropertyDataType==kAudioServerPlugInCustomPropertyDataTypeCFPropertyList);
    assert(custom.mQualifierDataType==kAudioServerPlugInCustomPropertyDataTypeNone);
    address.mSelector='nhpt';assert(GetPropertyDataSize(&driverPointer,kActuator,0,&address,0,NULL,&size)==0);
    assert(size==sizeof(CFPropertyListRef));CFPropertyListRef values=NULL;
    assert(GetPropertyData(&driverPointer,kActuator,0,&address,0,NULL,1,&size,&values)==kAudioHardwareBadPropertySizeError);
    assert(values==NULL);
    assert(GetPropertyData(&driverPointer,kActuator,0,&address,0,NULL,sizeof(values),&size,&values)==0);
    assert(values && CFGetTypeID(values)==CFDictionaryGetTypeID());
    int64_t count=-1;
    assert(CFNumberGetValue(CFDictionaryGetValue(values,CFSTR("initialized")),kCFNumberSInt64Type,&count) && count==1);
    assert(CFNumberGetValue(CFDictionaryGetValue(values,CFSTR("starts")),kCFNumberSInt64Type,&count) && count==0);
    assert(CFNumberGetValue(CFDictionaryGetValue(values,CFSTR("lastError")),kCFNumberSInt64Type,&count) && count==0);
    CFRelease(values);
    address.mSelector=kAudioPlugInPropertyTranslateUIDToDevice;CFStringRef uid=CFSTR("Actuator");AudioObjectID object=0;
    assert(GetPropertyData(&driverPointer,kPlugin,0,&address,sizeof(uid),&uid,sizeof(object),&size,&object)==0 && object==kActuator);
    address.mSelector=kAudioPlugInPropertyDeviceList;
    assert(GetPropertyData(&driverPointer,kPlugin,0,&address,0,NULL,sizeof(object),&size,&object)==0 && object==kActuator);
    address.mSelector='taps';assert(GetPropertyDataSize(&driverPointer,kStream,0,&address,0,NULL,&size)==0 && size==0);
    assert(StartIO(&driverPointer,kActuator,5)==0 && endpoint==77 && users==1 && packets==1);
    assert(StopIO(&driverPointer,kActuator,5)==0 && endpoint<0 && users==0 && packets==2);
    assert(opens==2 && closes==2);
    // Exercise the actual WriteMix path after original CoreAudio submits a
    // late/catch-up cycle. Transient refusals leave the same live IOProc and
    // sequence available; the next timely buffer still reaches the driver.
    assert(StartIO(&driverPointer,kActuator,6)==0);
    AudioServerPlugInIOCycleInfo cycle={0};
    cycle.mOutputTime.mFlags=kAudioTimeStampHostTimeValid;
    cycle.mOutputTime.mHostTime=anchor+32;
    float buffer[256]={0};buffer[0]=0.5f;
    UInt32 before_sequence=sequence;unsigned before_closes=closes;
    for(unsigned index=0;index<3;++index) {
        const int errors[]={ERANGE,EBUSY,EAGAIN};write_error=errors[index];
        assert(DoIOOperation(&driverPointer,kActuator,kStream,6,
            kAudioServerPlugInIOOperationWriteMix,256,&cycle,buffer,NULL)==0);
        assert(endpoint==77 && users==1 && sequence==before_sequence && closes==before_closes);
        assert(last_packet.operation==NzHapticsPCM && last_packet.sequence==before_sequence);
    }
    write_error=0;
    assert(DoIOOperation(&driverPointer,kActuator,kStream,6,
        kAudioServerPlugInIOOperationWriteMix,256,&cycle,buffer,NULL)==0);
    assert(sequence==before_sequence+1 && endpoint==77 && users==1);
    write_error=EIO;
    assert(DoIOOperation(&driverPointer,kActuator,kStream,6,
        kAudioServerPlugInIOOperationWriteMix,256,&cycle,buffer,NULL)==kAudioHardwareUnspecifiedError);
    assert(endpoint<0 && users==0 && closes==before_closes+1);
    write_error=0;
    assert(StopIO(&driverPointer,kActuator,6)==0);
    puts("PASS production HAL: typed custom property, retained CF dictionary, exact Actuator UID, silent Initialize and StartIO/StopIO lifetime");
    puts("PASS production WriteMix: late/busy/rest preserves stream and sequence; next timely buffer accepted; hard IO fault closes and cancels");
    puts("PASS production clock: frequency and both nanosecond converters aligned before streaming; original/corrected/denied paths");
    return 0;
}
