/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Execute the production HAL with the host Apple SDK, without Initialize or
 * any device connection. Stock24A446 Codec binds prec/pspk and requires input
 * vold/inpt/0 to construct the microphone port and its volume command. */
#define IOConnectCallScalarMethod property_test_scalar
#include "../hal/NezhaAudioPlugIn.c"
#undef IOConnectCallScalarMethod
#include <assert.h>
#include <stdio.h>

static uint64_t test_output[NA_AUDIO_RING_FRAMES],test_input[NA_AUDIO_RING_FRAMES];
static unsigned test_routes,test_starts,test_stops,test_inputs;
static uint32_t test_last_route;
static unsigned test_property_notifications;
static OSStatus reference_change_callback(AudioServerPlugInHostRef host,AudioObjectID object,
                                         UInt32 count,const AudioObjectPropertyAddress *addresses) {
    (void)host;(void)object;(void)count;(void)addresses;
    ++test_property_notifications;
    return 0;
}
kern_return_t property_test_scalar(mach_port_t connection,uint32_t selector,const uint64_t *input,
                                    uint32_t count,uint64_t *output,uint32_t *out_count) {
    assert(!input && !count && !output && !out_count);
    if (connection!=42) return KERN_FAILURE;
    if (selector==NaAudioRouteReceiver || selector==NaAudioRouteSpeaker) {++test_routes;test_last_route=selector;}
    else if (selector==NaAudioStartOutput) ++test_starts;
    else if (selector==NaAudioStopOutput) ++test_stops;
    else if (selector==NaAudioStartInput) ++test_inputs;
    else assert(selector==NaAudioStopInput);
    return KERN_SUCCESS;
}
static void fixture(void) {
    static NaAudioStatus status;
    status=(NaAudioStatus){.magic=NA_AUDIO_MAGIC,.version=NA_AUDIO_ABI_VERSION,
        .bytes=sizeof status,.rate=NA_AUDIO_RATE,.ring_frames=NA_AUDIO_RING_FRAMES,
        .period_frames=NA_AUDIO_PERIOD_FRAMES,.lead_frames=NA_AUDIO_LEAD_FRAMES,
        .state=NaAudioOutputReady,.epoch=7,.out_channels=2,.in_channels=0,
        .host_hz=19200000,.ticks_per_frame_q32=(uint64_t)400<<32};
    gControl=&status;gEpoch=7;gIoUsers=1;gOutput=test_output;gConnect=0;
    gInputAvailable=0;gReferenceSelected=1;gVolume=1;gInputVolume=1;gCaptureIssued=0;
    gSpeakerSelected=1;gReceiverSelected=0;gInputMuted=0;gReceiverVolume=0.5f;
    gSelectedMicSource=kMicSource;
}
static UInt32 scalar(AudioObjectID object, UInt32 selector, UInt32 scope) {
    AudioObjectPropertyAddress address={selector,scope,0};UInt32 value=0,bytes=0;
    assert(propertyData(object,&address,0,NULL,sizeof value,&bytes,&value)==0);
    assert(bytes==sizeof value);return value;
}
static void check_port_controls(UInt32 scope,UInt32 element) {
    AudioObjectID controls[11]={0};UInt32 bytes=0;
    AudioObjectPropertyAddress address={kAudioObjectPropertyControlList,scope,0};
    assert(GetPropertyData(NULL,kDevice,0,&address,0,NULL,sizeof controls,&bytes,controls)==0);
    unsigned mute=0,volume=0;
    for (unsigned i=0;i<bytes/sizeof(AudioObjectID);++i) {
        if (scalar(controls[i],kAudioControlPropertyScope,kAudioObjectPropertyScopeGlobal)!=scope ||
            scalar(controls[i],kAudioControlPropertyElement,kAudioObjectPropertyScopeGlobal)!=element) continue;
        UInt32 kind=scalar(controls[i],kAudioObjectPropertyClass,kAudioObjectPropertyScopeGlobal);
        if (kind==kAudioMuteControlClassID) ++mute;
        if (kind==kAudioVolumeControlClassID) ++volume;
    }
    assert(mute==1 && volume==1);
    address=(AudioObjectPropertyAddress){kAudioDevicePropertyVolumeDecibels,scope,element};
    assert(HasProperty(NULL,kDevice,0,&address));
    Boolean settable=0;
    assert(IsPropertySettable(NULL,kDevice,0,&address,&settable)==0 && settable);
    Float32 db=0;
    assert(GetPropertyData(NULL,kDevice,0,&address,0,NULL,sizeof db,&bytes,&db)==0 && bytes==sizeof db);
    address.mSelector=kAudioDevicePropertyVolumeRangeDecibels;
    AudioValueRange range={0};
    assert(GetPropertyData(NULL,kDevice,0,&address,0,NULL,sizeof range,&bytes,&range)==0);
    assert(bytes==sizeof range && range.mMinimum==-36 && range.mMaximum==0);
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
    assert(CFEqual(uid,CFSTR("Codec")));
    check_port_controls(kAudioObjectPropertyScopeOutput,3);
    check_port_controls(kAudioObjectPropertyScopeOutput,4);
    for (unsigned element=3;element<=4;++element) {
        address=(AudioObjectPropertyAddress){kAudioDevicePropertyMute,kAudioObjectPropertyScopeOutput,element};
        UInt32 mute=9;
        assert(propertyData(kDevice,&address,0,NULL,sizeof mute,&bytes,&mute)==0 && bytes==4);
        assert(mute==(element==3));
        address.mElement=5;
        assert(propertyData(kDevice,&address,0,NULL,sizeof mute,&bytes,&mute)==kAudioHardwareUnknownPropertyError);
    }
    address=(AudioObjectPropertyAddress){kAudioDevicePropertyVolumeScalar,kAudioObjectPropertyScopeOutput,3};
    Float32 volume=0;
    assert(propertyData(kDevice,&address,0,NULL,sizeof volume,&bytes,&volume)==0 && volume==0.5f);
    address.mElement=4;
    assert(propertyData(kDevice,&address,0,NULL,sizeof volume,&bytes,&volume)==0 && volume==1.0f);
    AudioStreamBasicDescription stream_format;
    address=(AudioObjectPropertyAddress){kAudioStreamPropertyVirtualFormat,kAudioObjectPropertyScopeGlobal,0};
    assert(propertyData(kReferenceStream,&address,0,NULL,sizeof stream_format,&bytes,&stream_format)==0);
    assert(stream_format.mChannelsPerFrame==2 && stream_format.mSampleRate==48000);

    /* A ready physical mic stays first; the tap stays last and stereo. */
    gInputAvailable=1;
    address=(AudioObjectPropertyAddress){kAudioDevicePropertyMute,kAudioObjectPropertyScopeInput,0};
    UInt32 muted_input=9;
    assert(propertyData(kDevice,&address,0,NULL,sizeof muted_input,&bytes,&muted_input)==0 && muted_input==0);
    address=(AudioObjectPropertyAddress){kAudioDevicePropertyStreams,kAudioObjectPropertyScopeInput,0};
    assert(propertyData(kDevice,&address,0,NULL,sizeof streams,&bytes,streams)==0);
    assert(bytes==2*sizeof(AudioObjectID) && streams[0]==kInStream && streams[1]==kReferenceStream);
    assert(scalar(streams[1],kAudioStreamPropertyStartingChannel,kAudioObjectPropertyScopeGlobal)==2);
    check_port_controls(kAudioObjectPropertyScopeInput,0);
    /* Follow the stock Codec data-source discovery through its control and
     * direct device properties before using the same mono PCM stream. */
    address=(AudioObjectPropertyAddress){kAudioDevicePropertyDataSources,kAudioObjectPropertyScopeInput,0};
    UInt32 mic_source=0;
    UInt32 source_ids[sizeof kMicSources/sizeof kMicSources[0]]={0};
    assert(HasProperty(NULL,kDevice,0,&address));
    assert(GetPropertyDataSize(NULL,kDevice,0,&address,0,NULL,&bytes)==0 && bytes==sizeof source_ids);
    assert(GetPropertyData(NULL,kDevice,0,&address,0,NULL,sizeof source_ids,&bytes,source_ids)==0);
    assert(bytes==sizeof source_ids && !memcmp(source_ids,kMicSources,sizeof source_ids));
    mic_source=source_ids[0];assert(mic_source=='imic');
    address.mSelector=kAudioDevicePropertyDataSource;
    assert(SetPropertyData(NULL,kDevice,0,&address,0,NULL,sizeof mic_source,&mic_source)==0);
    assert(SetPropertyData(NULL,kDevice,0,&address,0,NULL,0,NULL)==0);
    assert(GetPropertyDataSize(NULL,kDevice,0,&address,0,NULL,&bytes)==0 && bytes==0);
    assert(GetPropertyData(NULL,kDevice,0,&address,0,NULL,0,&bytes,NULL)==0 && bytes==0);
    address.mSelector=kAudioDevicePropertyDataSources;
    assert(GetPropertyDataSize(NULL,kDevice,0,&address,0,NULL,&bytes)==0 && bytes==sizeof source_ids);
    address.mSelector=kAudioDevicePropertyDataSource;
    UInt32 duplicate_sources[2]={'imic','imic'};
    assert(SetPropertyData(NULL,kDevice,0,&address,0,NULL,3,&mic_source)==kAudioHardwareBadPropertySizeError);
    assert(SetPropertyData(NULL,kDevice,0,&address,0,NULL,sizeof duplicate_sources,duplicate_sources)==0);
    assert(scalar(kDevice,kAudioDevicePropertyDataSource,kAudioObjectPropertyScopeInput)=='imic');
    assert(SetPropertyData(NULL,kDevice,0,&address,0,NULL,sizeof mic_source,&mic_source)==0);
    UInt32 fake_source='imi4';
    assert(SetPropertyData(NULL,kDevice,0,&address,0,NULL,sizeof fake_source,&fake_source)==kAudioHardwareIllegalOperationError);
    assert(scalar(kInputSource,kAudioObjectPropertyClass,kAudioObjectPropertyScopeGlobal)==kAudioDataSourceControlClassID);
    address=(AudioObjectPropertyAddress){kAudioSelectorControlPropertyItemName,kAudioObjectPropertyScopeGlobal,0};
    CFStringRef mic_name=NULL;
    assert(HasProperty(NULL,kInputSource,0,&address));
    assert(GetPropertyData(NULL,kInputSource,0,&address,sizeof mic_source,&mic_source,sizeof mic_name,&bytes,&mic_name)==0);
    assert(CFEqual(mic_name,CFSTR("Microphone")));CFRelease(mic_name);
    address.mSelector=kAudioSelectorControlPropertyItemKind;
    UInt32 mic_kind=0;
    assert(HasProperty(NULL,kInputSource,0,&address));
    assert(GetPropertyData(NULL,kInputSource,0,&address,sizeof mic_source,&mic_source,sizeof mic_kind,&bytes,&mic_kind)==0 && mic_kind=='omni');
    assert(GetPropertyData(NULL,kInputSource,0,&address,sizeof fake_source,&fake_source,sizeof mic_kind,&bytes,&mic_kind)==kAudioHardwareIllegalOperationError);
    address.mSelector=kAudioSelectorControlPropertyCurrentItem;
    assert(SetPropertyData(NULL,kInputSource,0,&address,0,NULL,0,NULL)==0);
    assert(GetPropertyDataSize(NULL,kInputSource,0,&address,0,NULL,&bytes)==0 && bytes==0);
    assert(GetPropertyData(NULL,kInputSource,0,&address,0,NULL,0,&bytes,NULL)==0 && bytes==0);
    AudioObjectPropertyAddress device_selected={kAudioDevicePropertyDataSource,kAudioObjectPropertyScopeInput,0};
    assert(GetPropertyDataSize(NULL,kDevice,0,&device_selected,0,NULL,&bytes)==0 && bytes==0);
    assert(SetPropertyData(NULL,kInputSource,0,&address,0,NULL,3,&mic_source)==kAudioHardwareBadPropertySizeError);
    assert(SetPropertyData(NULL,kInputSource,0,&address,0,NULL,sizeof duplicate_sources,duplicate_sources)==kAudioHardwareIllegalOperationError);
    assert(SetPropertyData(NULL,kInputSource,0,&address,0,NULL,sizeof fake_source,&fake_source)==kAudioHardwareIllegalOperationError);
    assert(GetPropertyDataSize(NULL,kInputSource,0,&address,0,NULL,&bytes)==0 && bytes==0);
    assert(SetPropertyData(NULL,kInputSource,0,&address,0,NULL,sizeof mic_source,&mic_source)==0);
    assert(scalar(kInputSource,kAudioSelectorControlPropertyCurrentItem,kAudioObjectPropertyScopeGlobal)=='imic');
    assert(scalar(kDevice,kAudioDevicePropertyDataSource,kAudioObjectPropertyScopeInput)=='imic');
    address=(AudioObjectPropertyAddress){kAudioDevicePropertyDataSourceNameForIDCFString,kAudioObjectPropertyScopeInput,0};
    AudioValueTranslation translation={&mic_source,sizeof mic_source,&mic_name,sizeof mic_name};
    assert(GetPropertyDataSize(NULL,kDevice,0,&address,0,NULL,&bytes)==0 && bytes==sizeof translation);
    assert(GetPropertyData(NULL,kDevice,0,&address,0,NULL,sizeof translation,&bytes,&translation)==0);
    assert(CFEqual(mic_name,CFSTR("Microphone")));CFRelease(mic_name);
    address.mSelector=kAudioDevicePropertyDataSourceKindForID;
    translation=(AudioValueTranslation){&mic_source,sizeof mic_source,&mic_kind,sizeof mic_kind};
    assert(GetPropertyData(NULL,kDevice,0,&address,0,NULL,sizeof translation,&bytes,&translation)==0 && mic_kind=='omni');
    /* Logical names normalize to the one published physical input. Neither
     * API advertises additional hardware sources or additional streams. */
    for (unsigned i=0;i<sizeof kLogicalMicSources/sizeof kLogicalMicSources[0];++i) {
        mic_source=kLogicalMicSources[i];
        address=(AudioObjectPropertyAddress){kAudioDevicePropertyDataSource,kAudioObjectPropertyScopeInput,0};
        assert(SetPropertyData(NULL,kDevice,0,&address,0,NULL,sizeof mic_source,&mic_source)==0);
        assert(scalar(kInputSource,kAudioSelectorControlPropertyCurrentItem,kAudioObjectPropertyScopeGlobal)=='imic');
        address=(AudioObjectPropertyAddress){kAudioSelectorControlPropertyCurrentItem,kAudioObjectPropertyScopeGlobal,0};
        assert(SetPropertyData(NULL,kInputSource,0,&address,0,NULL,sizeof mic_source,&mic_source)==0);
        assert(scalar(kDevice,kAudioDevicePropertyDataSource,kAudioObjectPropertyScopeInput)=='imic');
        address.mSelector=kAudioSelectorControlPropertyItemName;
        assert(GetPropertyData(NULL,kInputSource,0,&address,sizeof mic_source,&mic_source,sizeof mic_name,&bytes,&mic_name)==0);
        assert(CFEqual(mic_name,CFSTR("Microphone")));CFRelease(mic_name);
    }
    UInt32 logical_group[]={'btm1','fnt1','bak1','btm2'};
    address=(AudioObjectPropertyAddress){kAudioDevicePropertyDataSource,kAudioObjectPropertyScopeInput,0};
    assert(SetPropertyData(NULL,kDevice,0,&address,0,NULL,sizeof logical_group,logical_group)==0);
    assert(scalar(kDevice,kAudioDevicePropertyDataSource,kAudioObjectPropertyScopeInput)=='imic');
    logical_group[2]='Usb0';
    assert(SetPropertyData(NULL,kDevice,0,&address,0,NULL,sizeof logical_group,logical_group)==kAudioHardwareIllegalOperationError);
    assert(scalar(kDevice,kAudioDevicePropertyDataSource,kAudioObjectPropertyScopeInput)=='imic');
    /* The evis control owns the existing accepted-output reference. The
     * server then retains two input streams rather than generating a third. */
    address=(AudioObjectPropertyAddress){kAudioObjectPropertyControlList,kAudioObjectPropertyScopeGlobal,0};
    AudioObjectID all_controls[11]={0};
    assert(GetPropertyData(NULL,kDevice,0,&address,0,NULL,sizeof all_controls,&bytes,all_controls)==0);
    unsigned reference_controls=0;
    for (unsigned i=0;i<bytes/sizeof(AudioObjectID);++i)
        reference_controls+=scalar(all_controls[i],kAudioObjectPropertyClass,kAudioObjectPropertyScopeGlobal)=='evis';
    assert(reference_controls==1);
    assert(scalar(kReferenceControl,kAudioObjectPropertyBaseClass,kAudioObjectPropertyScopeGlobal)==kAudioBooleanControlClassID);
    address=(AudioObjectPropertyAddress){kAudioBooleanControlPropertyValue,kAudioObjectPropertyScopeGlobal,0};
    UInt32 reference_off=0,reference_on=1;
    Boolean reference_settable=0;
    assert(IsPropertySettable(NULL,kReferenceControl,0,&address,&reference_settable)==0 && reference_settable);
    AudioServerPlugInHostInterface reference_host={0};
    reference_host.PropertiesChanged=reference_change_callback;gHost=&reference_host;
    test_property_notifications=0;
    assert(SetPropertyData(NULL,kReferenceControl,0,&address,0,NULL,sizeof reference_off,&reference_off)==0);
    assert(scalar(kReferenceStream,kAudioStreamPropertyIsActive,kAudioObjectPropertyScopeGlobal)==0);
    assert(SetPropertyData(NULL,kReferenceControl,0,&address,0,NULL,sizeof reference_on,&reference_on)==0);
    assert(scalar(kReferenceStream,kAudioStreamPropertyIsActive,kAudioObjectPropertyScopeGlobal)==1);
    assert(SetPropertyData(NULL,kReferenceControl,0,&address,0,NULL,sizeof reference_on,&reference_on)==0);
    assert(test_property_notifications==0);gHost=NULL;
    gSelectedMicSource=kMicSource;
    address=(AudioObjectPropertyAddress){'MCCs',kAudioObjectPropertyScopeGlobal,0};
    CFDictionaryRef counts=NULL;
    assert(HasProperty(NULL,kDevice,0,&address));
    assert(GetPropertyData(NULL,kDevice,0,&address,0,NULL,sizeof counts,&bytes,&counts)==0);
    SInt32 input_count=0,output_count=0;
    assert(CFNumberGetValue(CFDictionaryGetValue(counts,CFSTR("MaxInputChannelCount")),kCFNumberSInt32Type,&input_count));
    assert(CFNumberGetValue(CFDictionaryGetValue(counts,CFSTR("MaxOutputChannelCount")),kCFNumberSInt32Type,&output_count));
    assert(input_count==1 && output_count==2);CFRelease(counts);
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
    /* The real mic path keeps unity PCM at default volume. The input volume
     * setter scales only this path, never the accepted playback reference. */
    NaAudioStatus *status=(NaAudioStatus *)(uintptr_t)gControl;
    status->state|=NaAudioInputReady;status->in_channels=1;status->in_produced=102;
    gInputAvailable=1;gInput=test_input;gConnect=42;
    assert(na_ring_capture(test_input,100,8192)==0 && na_ring_capture(test_input,101,-4096)==0);
    Float32 mic[2]={0};
    assert(DoIOOperation(NULL,kDevice,kInStream,0,kAudioServerPlugInIOOperationReadInput,2,&cycle,mic,NULL)==0);
    assert(mic[0]==0.25f && mic[1]==-0.125f && test_inputs==1);
    /* A selected N71 logical source must reach the same existing capture
     * callback; a successful property setter alone does not prove this. */
    gSelectedMicSource='btm1';
    assert(DoIOOperation(NULL,kDevice,kInStream,0,kAudioServerPlugInIOOperationReadInput,2,&cycle,mic,NULL)==0);
    assert(mic[0]==0.25f && mic[1]==-0.125f && test_inputs==1);
    gSelectedMicSource=kMicSource;
    Float32 db=-12;
    address=(AudioObjectPropertyAddress){kAudioDevicePropertyVolumeDecibels,kAudioObjectPropertyScopeInput,0};
    assert(SetPropertyData(NULL,kDevice,0,&address,0,NULL,sizeof db,&db)==0);
    assert(DoIOOperation(NULL,kDevice,kInStream,0,kAudioServerPlugInIOOperationReadInput,2,&cycle,mic,NULL)==0);
    assert(fabsf(mic[0]-0.25f*powf(10.0f,-12.0f/20.0f))<1e-6f && mic[1]<0);
    assert(gVolume==1 && gReceiverVolume==0.5f);
    Float32 unity=1;
    address=(AudioObjectPropertyAddress){kAudioLevelControlPropertyScalarValue,kAudioObjectPropertyScopeGlobal,0};
    assert(SetPropertyData(NULL,kInputVolume,0,&address,0,NULL,sizeof unity,&unity)==0 && gInputVolume==1);
    test_inputs=0;gCaptureIssued=0;gConnect=0;gInputAvailable=0;
    /* Real setter queues the native route. Report success to listeners after
     * the worker publishes applied mode; changing output never starts input. */
    gConnect=42;UInt32 unmute=0,mute=1;
    address=(AudioObjectPropertyAddress){kAudioDevicePropertyMute,kAudioObjectPropertyScopeOutput,3};
    status->route_pending=1;
    assert(SetPropertyData(NULL,kDevice,0,&address,0,NULL,4,&unmute)==0);
    assert(test_routes==1 && test_last_route==NaAudioRouteReceiver && test_starts==1 && !test_inputs);
    UInt32 value=9;
    assert(propertyData(kDevice,&address,0,NULL,4,&bytes,&value)==0 && value==1);
    status->route=NaAudioReceiver;status->route_pending=0;
    assert(propertyData(kDevice,&address,0,NULL,4,&bytes,&value)==0 && value==0);
    address.mElement=4;status->route_pending=1;
    assert(SetPropertyData(NULL,kDevice,0,&address,0,NULL,4,&unmute)==0);
    assert(test_routes==2 && test_last_route==NaAudioRouteSpeaker && test_starts==2 && !test_inputs);
    status->route=NaAudioSpeaker;status->route_pending=0;
    assert(propertyData(kDevice,&address,0,NULL,4,&bytes,&value)==0 && value==0);
    assert(SetPropertyData(NULL,kDevice,0,&address,0,NULL,4,&mute)==0 && test_stops==1);
    UInt32 invalid=2;
    assert(SetPropertyData(NULL,kDevice,0,&address,0,NULL,4,&invalid)==kAudioHardwareIllegalOperationError);
    assert(test_routes==2 && !test_inputs);gConnect=0;
    UInt32 active=0;address=(AudioObjectPropertyAddress){kAudioStreamPropertyIsActive,kAudioObjectPropertyScopeGlobal,0};
    assert(SetPropertyData(NULL,kReferenceStream,0,&address,0,NULL,sizeof active,&active)==0);
    assert(DoIOOperation(NULL,kDevice,kReferenceStream,0,kAudioServerPlugInIOOperationReadInput,2,&cycle,reference,NULL)==kAudioHardwareNotRunningError);
    for (unsigned i=0;i<4;++i) assert(reference[i]==0);
    assert(gCaptureIssued==0);
    disconnect();
    assert(na_reference_read(gReference,100,&left[0],&right[0])==NA_MISSING);
    puts("production Codec HAL: prec/pspk/pmwi mute and volume bindings, input unity/scaling, native route request, applied mute, separate volume, tap, accepted PCM, disconnect PASS");
    return 0;
}
