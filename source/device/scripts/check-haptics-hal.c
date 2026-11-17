// SPDX-License-Identifier: BSD-2-Clause-Patent
// Execute production HAL property marshalling and Initialize without hardware IO.
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <CoreFoundation/CoreFoundation.h>
#include "../package/haptics27/NezhaHapticsProtocol.h"
static unsigned opens,closes,packets;
static int observe_open(const char *path,int flags,...)
{
    assert(!strcmp(path,NZ_HAPTICS_DEVICE));assert(flags&O_WRONLY);++opens;return 77;
}
static int observe_close(int descriptor) {assert(descriptor==77);++closes;return 0;}
static int observe_ioctl(int descriptor,unsigned long command,...)
{
    assert(descriptor==77 && command==NZ_HAPTICS_GET_STATUS);
    va_list list;va_start(list,command);NzHapticsStatus *state=va_arg(list,NzHapticsStatus *);va_end(list);
    *state=(NzHapticsStatus){.version=NZ_HAPTICS_ABI,.session=1,.epoch=1,.next_sequence=1,.ready=1};return 0;
}
static ssize_t observe_write(int descriptor,const void *data,size_t bytes)
{
    assert(descriptor==77 && data && bytes>=sizeof(NzHapticsPacket));++packets;return (ssize_t)bytes;
}
#define open observe_open
#define close observe_close
#define ioctl observe_ioctl
#define write observe_write
#include "../package/haptics27/hal/NezhaHapticsPlugIn.c"
#undef open
#undef close
#undef ioctl
#undef write
int main(void)
{
    assert(Initialize(&driverPointer,NULL)==0);
    assert(opens==1 && closes==1 && packets==0 && endpoint<0 && !users);
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
    puts("PASS production HAL: typed custom property, retained CF dictionary, exact Actuator UID, silent Initialize and StartIO/StopIO lifetime");
    return 0;
}
