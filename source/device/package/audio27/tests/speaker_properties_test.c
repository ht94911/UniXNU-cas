/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Execute the production HAL with the host Apple SDK, without Initialize or
 * any device connection. The stock24A446 Speaker/tap contract is the oracle. */
#include "../hal/NezhaAudioPlugIn.c"
#include <assert.h>
#include <stdio.h>

static uint64_t test_output[NA_AUDIO_RING_FRAMES];
static void fixture(void) {
    static NaAudioStatus status;
    status=(NaAudioStatus){.magic=NA_AUDIO_MAGIC,.version=NA_AUDIO_ABI_VERSION,
        .bytes=sizeof status,.rate=NA_AUDIO_RATE,.ring_frames=NA_AUDIO_RING_FRAMES,
        .period_frames=NA_AUDIO_PERIOD_FRAMES,.lead_frames=NA_AUDIO_LEAD_FRAMES,
        .state=NaAudioOutputReady,.epoch=7,.out_channels=2,.in_channels=0,
        .host_hz=19200000,.ticks_per_frame_q32=(uint64_t)400<<32};
    gControl=&status;gEpoch=7;gIoUsers=1;gOutput=test_output;gConnect=0;
    gInputAvailable=0;gReferenceSelected=1;gVolume=1;gCaptureIssued=0;
}
static UInt32 scalar(AudioObjectID object, UInt32 selector, UInt32 scope) {
    AudioObjectPropertyAddress address={selector,scope,0};UInt32 value=0,bytes=0;
    assert(propertyData(object,&address,0,NULL,sizeof value,&bytes,&value)==0);
    assert(bytes==sizeof value);return value;
}
int main(void) {
    fixture();
    AudioObjectID streams[4]={0};UInt32 bytes=0;
    AudioObjectPropertyAddress address={kAudioDevicePropertyStreams,kAudioObjectPropertyScopeInput,0};
    assert(propertyData(kDevice,&address,0,NULL,sizeof streams,&bytes,streams)==0);
    assert(bytes==sizeof(AudioObjectID) && streams[0]==kReferenceStream);
    assert(scalar(streams[0],'taps',kAudioObjectPropertyScopeGlobal)==1);
    assert(scalar(streams[0],kAudioStreamPropertyDirection,kAudioObjectPropertyScopeGlobal)==1);
    assert(scalar(kDevice,kAudioDevicePropertyDeviceCanBeDefaultDevice,kAudioObjectPropertyScopeInput)==0);
    assert(scalar(kDevice,kAudioDevicePropertyTransportType,kAudioObjectPropertyScopeGlobal)==kAudioDeviceTransportTypeBuiltIn);
    CFStringRef uid=NULL;address=(AudioObjectPropertyAddress){kAudioDevicePropertyDeviceUID,kAudioObjectPropertyScopeGlobal,0};
    assert(propertyData(kDevice,&address,0,NULL,sizeof uid,&bytes,&uid)==0);
    assert(CFEqual(uid,CFSTR("Speaker")));
    AudioStreamBasicDescription stream_format;
    address=(AudioObjectPropertyAddress){kAudioStreamPropertyVirtualFormat,kAudioObjectPropertyScopeGlobal,0};
    assert(propertyData(kReferenceStream,&address,0,NULL,sizeof stream_format,&bytes,&stream_format)==0);
    assert(stream_format.mChannelsPerFrame==2 && stream_format.mSampleRate==48000);

    /* A ready physical mic stays first; the tap stays last and stereo. */
    gInputAvailable=1;
    address=(AudioObjectPropertyAddress){kAudioDevicePropertyStreams,kAudioObjectPropertyScopeInput,0};
    assert(propertyData(kDevice,&address,0,NULL,sizeof streams,&bytes,streams)==0);
    assert(bytes==2*sizeof(AudioObjectID) && streams[0]==kInStream && streams[1]==kReferenceStream);
    assert(scalar(streams[1],kAudioStreamPropertyStartingChannel,kAudioObjectPropertyScopeGlobal)==2);
    gInputAvailable=0;
    address=(AudioObjectPropertyAddress){'taps',kAudioObjectPropertyScopeGlobal,0};
    UInt32 marker=0;
    assert(GetPropertyDataSize(NULL,kReferenceStream,0,&address,0,NULL,&bytes)==0 && bytes==sizeof marker);
    assert(propertyData(kReferenceStream,&address,0,NULL,1,&bytes,&marker)==kAudioHardwareBadPropertySizeError);

    AudioServerPlugInIOCycleInfo cycle={0};
    cycle.mInputTime.mSampleTime=cycle.mOutputTime.mSampleTime=100;
    cycle.mInputTime.mFlags=cycle.mOutputTime.mFlags=kAudioTimeStampSampleTimeValid;
    Float32 source[4]={0.5f,-0.25f,0.125f,-0.0625f},reference[4]={0};
    assert(DoIOOperation(NULL,kDevice,kOutStream,0,kAudioServerPlugInIOOperationWriteMix,2,&cycle,source,NULL)==0);
    int16_t left[2],right[2];
    for (unsigned i=0;i<2;++i) assert(na_ring_take(test_output,100+i,&left[i],&right[i])==0);
    assert(DoIOOperation(NULL,kDevice,kReferenceStream,0,kAudioServerPlugInIOOperationReadInput,2,&cycle,reference,NULL)==0);
    for (unsigned i=0;i<2;++i) {
        assert(reference[2*i]==left[i]/32768.0f && reference[2*i+1]==right[i]/32768.0f);
    }
    assert(gCaptureIssued==0 && gConnect==0);
    UInt32 active=0;address=(AudioObjectPropertyAddress){kAudioStreamPropertyIsActive,kAudioObjectPropertyScopeGlobal,0};
    assert(SetPropertyData(NULL,kReferenceStream,0,&address,0,NULL,sizeof active,&active)==0);
    assert(DoIOOperation(NULL,kDevice,kReferenceStream,0,kAudioServerPlugInIOOperationReadInput,2,&cycle,reference,NULL)==kAudioHardwareNotRunningError);
    for (unsigned i=0;i<4;++i) assert(reference[i]==0);
    assert(gCaptureIssued==0);
    disconnect();
    assert(na_reference_read(gReference,100,&left[0],&right[0])==NA_MISSING);
    puts("production Speaker HAL: UID, tap size/value, input order, format, accepted PCM, no mic request, active state, disconnect PASS");
    return 0;
}
