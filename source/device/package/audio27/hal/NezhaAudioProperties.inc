// SPDX-License-Identifier: BSD-2-Clause-Patent
// Derived from sheng 4601795 hal/ShengAudioPlugIn.c property implementation.
static UInt32 objects(AudioObjectID o, AudioObjectPropertyScope scope, AudioObjectID *list) {
    UInt32 n = 0;
    if (o == kPlugIn && available()) list[n++] = kDevice;
    else if (o == kDevice) {
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kOutStream;
        if (scope != kAudioObjectPropertyScopeOutput && gInputAvailable) list[n++] = kInStream;
        /* Original24A446 finds the tap at the last input-stream index. */
        if (scope != kAudioObjectPropertyScopeOutput) list[n++] = kReferenceStream;
        if (scope != kAudioObjectPropertyScopeInput) {
            list[n++] = kVolume;list[n++] = kReceiverVolume;
            list[n++] = kReceiverMute;list[n++] = kSpeakerMute;
        }
        if (scope != kAudioObjectPropertyScopeOutput && gInputAvailable) {
            list[n++] = kInputMute;list[n++] = kInputVolume;list[n++] = kInputSource;
        }
    }
    return n;
}

static OSStatus propertyData(AudioObjectID o, const AudioObjectPropertyAddress *a, UInt32 qs, const void *q, UInt32 inDataSize,
                             UInt32 *outDataSize, void *outData) {
    const AudioObjectPropertySelector s = a->mSelector;
    /* Device_USB_Aspen reads lcID as a CFNumber before constructing ports.
     * This software device has no physical USB location. */
    if (o == kDevice && s == kAudioObjectPropertyCustomPropertyInfoList) {
        AudioServerPlugInCustomPropertyInfo info[2] = {
            {'lcID', kAudioServerPlugInCustomPropertyDataTypeCFPropertyList,
             kAudioServerPlugInCustomPropertyDataTypeNone},
            {'MCCs', kAudioServerPlugInCustomPropertyDataTypeCFPropertyList,
             kAudioServerPlugInCustomPropertyDataTypeNone}
        };
        if (inDataSize<sizeof info) return kAudioHardwareBadPropertySizeError;
        memcpy(outData,info,sizeof info);*outDataSize=sizeof info;return 0;
    }
    // Lists every object answers, empty here: custom properties ('cust'), and on the plug-in its boxes and clock devices. The audio
    // server's device manager reads them for each new device and drops a device whose answer is an unknown-property error (i525).
    if (o == kReferenceStream && s == 'taps' && a->mScope == kAudioObjectPropertyScopeGlobal && a->mElement == kAudioObjectPropertyElementMain)
        REPLY(UInt32, 1);
    if (o <= kInputSource && (s == kAudioObjectPropertyCustomPropertyInfoList || ((s == kAudioPlugInPropertyBoxList ||
        s == kAudioPlugInPropertyClockDeviceList) && o == kPlugIn) || ((s == 'taps') && (o == kOutStream || o == kInStream)))) {
        *outDataSize = 0;
        return 0;
    }
    switch (o) {
    case kPlugIn:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            if (!available() || inDataSize < sizeof(AudioObjectID)) {*outDataSize = 0; return 0;}
            ((AudioObjectID *)outData)[0] = kDevice; *outDataSize = sizeof(AudioObjectID); return 0;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CFStringRef uid = q && qs >= sizeof(CFStringRef) ? *(const CFStringRef *)q : NULL;
            REPLY(AudioObjectID, uid && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo ? kDevice : kAudioObjectUnknown);
        }
        case kAudioPlugInPropertyResourceBundle: REPLY_STR("");
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kDevice:
        switch (s) {
        case 'MCCs': {
            if (a->mScope!=kAudioObjectPropertyScopeGlobal || a->mElement!=0)
                return kAudioHardwareUnknownPropertyError;
            if (inDataSize<sizeof(CFDictionaryRef)) return kAudioHardwareBadPropertySizeError;
            SInt32 out=2,in=gInputAvailable ? 1:0;
            CFNumberRef output=CFNumberCreate(kCFAllocatorDefault,kCFNumberSInt32Type,&out);
            CFNumberRef input=CFNumberCreate(kCFAllocatorDefault,kCFNumberSInt32Type,&in);
            if (!output || !input) {
                if (output) CFRelease(output);
                if (input) CFRelease(input);
                return kAudioHardwareUnspecifiedError;
            }
            const void *keys[]={CFSTR("MaxOutputChannelCount"),CFSTR("MaxInputChannelCount")};
            const void *values[]={output,input};
            CFDictionaryRef counts=CFDictionaryCreate(kCFAllocatorDefault,keys,values,2,
                &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
            CFRelease(output);CFRelease(input);
            if (!counts) return kAudioHardwareUnspecifiedError;
            REPLY(CFDictionaryRef,counts);
        }
        case kAudioDevicePropertyDataSource:
            if (a->mScope!=kAudioObjectPropertyScopeInput || a->mElement!=0 || !gInputAvailable)
                return kAudioHardwareUnknownPropertyError;
            if (!gSelectedMicSource) {*outDataSize=0;return 0;}
            REPLY(UInt32,gSelectedMicSource);
        case kAudioDevicePropertyDataSources:
            if (a->mScope!=kAudioObjectPropertyScopeInput || a->mElement!=0 || !gInputAvailable)
                return kAudioHardwareUnknownPropertyError;
            REPLY(UInt32,kMicSource);
        case kAudioDevicePropertyDataSourceNameForIDCFString:
        case kAudioDevicePropertyDataSourceKindForID: {
            if (a->mScope!=kAudioObjectPropertyScopeInput || a->mElement!=0 || !gInputAvailable)
                return kAudioHardwareUnknownPropertyError;
            if (inDataSize<sizeof(AudioValueTranslation)) return kAudioHardwareBadPropertySizeError;
            AudioValueTranslation *translation=outData;
            if (!translation->mInputData || translation->mInputDataSize!=sizeof(UInt32) ||
                !translation->mOutputData) return kAudioHardwareIllegalOperationError;
            UInt32 source;memcpy(&source,translation->mInputData,sizeof source);
            if (source!=kMicSource) return kAudioHardwareIllegalOperationError;
            if (s==kAudioDevicePropertyDataSourceNameForIDCFString) {
                if (translation->mOutputDataSize<sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
                *(CFStringRef *)translation->mOutputData=(CFStringRef)CFRetain(CFSTR("Microphone"));
                translation->mOutputDataSize=sizeof(CFStringRef);
            } else {
                if (translation->mOutputDataSize<sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                memcpy(translation->mOutputData,&kMicSourceKind,sizeof kMicSourceKind);
                translation->mOutputDataSize=sizeof(UInt32);
            }
            *outDataSize=sizeof *translation;return 0;
        }
        case 'lcID':
            if (a->mScope != kAudioObjectPropertyScopeGlobal || a->mElement != kAudioObjectPropertyElementMain)
                return kAudioHardwareUnknownPropertyError;
            if (inDataSize < sizeof(CFNumberRef)) return kAudioHardwareBadPropertySizeError;
            if (!gLogicalLocation) {
                SInt32 location = 0;
                gLogicalLocation = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &location);
                if (!gLogicalLocation) return kAudioHardwareUnspecifiedError;
            }
            CFRetain(gLogicalLocation);
            REPLY(CFNumberRef, gLogicalLocation);
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kPlugIn);
        case kAudioObjectPropertyName: REPLY_STR("Nezha Audio");
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioDevicePropertyDeviceUID: REPLY_STR(kDeviceUID);
        case kAudioDevicePropertyModelUID: REPLY_STR(kModelUID);
        case kAudioObjectPropertyModelName: REPLY_STR("Nezha Audio");
        case kAudioObjectPropertySerialNumber: REPLY_STR("0001");
        case kAudioObjectPropertyFirmwareVersion: REPLY_STR("1.0");
        case kAudioObjectPropertyIdentify: REPLY(UInt32, 0);
        // Private device properties the audio server reads for each new device (i526: 'accs' and 'auto' were its last unanswered ones).
        case 'accs': REPLY(UInt32, 0);
        case 'auto': REPLY(UInt32, 0);
        case kAudioObjectPropertyElementName: REPLY_STR("");
        case kAudioDevicePropertyConfigurationApplication: REPLY_STR("com.apple.Preferences");
        /* UID Codec uses the stock codec class and its prec/pspk/pmwi ports.
         * Native TAS protection/VI remains in the DSP rather than an Apple
         * speaker-telemetry input stream. */
        case kAudioDevicePropertyTransportType: REPLY(UInt32, kAudioDeviceTransportTypeBuiltIn);
        case kAudioDevicePropertyMute:
            if (a->mScope==kAudioObjectPropertyScopeOutput && a->mElement==3) REPLY(UInt32,output_mute(kReceiverMute));
            if (a->mScope==kAudioObjectPropertyScopeOutput && a->mElement==4) REPLY(UInt32,output_mute(kSpeakerMute));
            if (a->mScope==kAudioObjectPropertyScopeInput && a->mElement==0 && gInputAvailable) REPLY(UInt32,gInputMuted);
            return kAudioHardwareUnknownPropertyError;
        case kAudioDevicePropertyVolumeScalar:
            if (a->mScope==kAudioObjectPropertyScopeInput && a->mElement==0 && gInputAvailable) REPLY(Float32,gInputVolume);
            if (a->mScope!=kAudioObjectPropertyScopeOutput) return kAudioHardwareUnknownPropertyError;
            if (a->mElement==3) REPLY(Float32,gReceiverVolume);
            if (a->mElement==4) REPLY(Float32,gVolume);
            return kAudioHardwareUnknownPropertyError;
        case kAudioDevicePropertyVolumeDecibels:
            if (a->mScope==kAudioObjectPropertyScopeInput && a->mElement==0 && gInputAvailable) REPLY(Float32,toDb(gInputVolume));
            if (a->mScope!=kAudioObjectPropertyScopeOutput) return kAudioHardwareUnknownPropertyError;
            if (a->mElement==3) REPLY(Float32,toDb(gReceiverVolume));
            if (a->mElement==4) REPLY(Float32,toDb(gVolume));
            return kAudioHardwareUnknownPropertyError;
        case kAudioDevicePropertyVolumeRangeDecibels:
            if ((a->mScope==kAudioObjectPropertyScopeOutput && (a->mElement==3 || a->mElement==4)) ||
                (a->mScope==kAudioObjectPropertyScopeInput && a->mElement==0 && gInputAvailable))
                REPLY(AudioValueRange,((AudioValueRange){kMinDb,0.0}));
            return kAudioHardwareUnknownPropertyError;
        case kAudioDevicePropertyRelatedDevices: REPLY(AudioObjectID, kDevice);
        case kAudioDevicePropertyClockDomain: REPLY(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: REPLY(UInt32, available());
        case kAudioDevicePropertyDeviceIsRunning: REPLY(UInt32, gIoUsers > 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: REPLY(UInt32, available() && (a->mScope != kAudioObjectPropertyScopeInput || gInputAvailable));
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: REPLY(UInt32, available() && a->mScope != kAudioObjectPropertyScopeInput);
        case kAudioDevicePropertyLatency: REPLY(UInt32, latencyFrames());
        // The service takes a period lead_frames before it plays (its own queue and the DSP's); the service measures and publishes it, so
        // the audio server writes each frame at least that early.
        case kAudioDevicePropertySafetyOffset: REPLY(UInt32, latencyFrames() + 2 * NA_AUDIO_PERIOD_FRAMES);
        case kAudioDevicePropertyNominalSampleRate: REPLY(Float64, kRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: REPLY(AudioValueRange, ((AudioValueRange){kRate, kRate}));
        case kAudioDevicePropertyIsHidden: REPLY(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: REPLY(UInt32, kZeroPeriod);
        case kAudioDevicePropertyClockIsStable: REPLY(UInt32, 1);
        // Raw: the zero time stamps are exact (one host clock), so smoothing only delays the audio server's view of them.
        case kAudioDevicePropertyClockAlgorithm: REPLY(UInt32, kAudioDeviceClockAlgorithmRaw);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            UInt32 first = a->mScope == kAudioObjectPropertyScopeInput && gInputAvailable ? 2 : 1;
            ((UInt32 *)outData)[0] = first; ((UInt32 *)outData)[1] = first + 1;
            *outDataSize = 2 * sizeof(UInt32); return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            const UInt32 channels = a->mScope == kAudioObjectPropertyScopeInput && gInputAvailable ? 3 : 2;
            const UInt32 bytes = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + channels * sizeof(AudioChannelDescription));
            if (inDataSize < bytes) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *l = outData;
            memset(l, 0, bytes);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = channels;
            UInt32 first = channels == 3 ? 1 : 0;
            if (first) l->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Mono;
            l->mChannelDescriptions[first].mChannelLabel = kAudioChannelLabel_Left;
            l->mChannelDescriptions[first+1].mChannelLabel = kAudioChannelLabel_Right;
            *outDataSize = bytes; return 0;
        }
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList: {
            AudioObjectID list[10];
            UInt32 n = objects(kDevice, a->mScope, list), out = 0;
            for (UInt32 i = 0; i < n; ++i) {
                int control=list[i]==kVolume || list[i]>=kReceiverMute;
                if (s == kAudioDevicePropertyStreams && control) continue;
                if (s == kAudioObjectPropertyControlList && !control) continue;
                if ((out + 1) * sizeof(AudioObjectID) > inDataSize) break;
                ((AudioObjectID *)outData)[out++] = list[i];
            }
            *outDataSize = out * (UInt32)sizeof(AudioObjectID); return 0;
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kOutStream:
    case kInStream:
    case kReferenceStream: {
        if (o == kInStream && !gInputAvailable) return kAudioHardwareBadObjectError;
        const UInt32 channels = o == kInStream ? 1 : 2;
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyName:
            if (o == kOutStream) REPLY_STR("Speaker");
            if (o == kReferenceStream) REPLY_STR("Speaker Reference");
            REPLY_STR("Microphone");
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioStreamPropertyIsActive: REPLY(UInt32, o == kOutStream ? gOutputSelected : o == kReferenceStream ? gReferenceSelected : gInputSelected);
        case kAudioStreamPropertyDirection: REPLY(UInt32, o != kOutStream);
        case kAudioStreamPropertyTerminalType: REPLY(UInt32, o == kOutStream ? kAudioStreamTerminalTypeSpeaker : o == kReferenceStream ? kAudioStreamTerminalTypeUnknown : kAudioStreamTerminalTypeMicrophone);
        case kAudioStreamPropertyStartingChannel: REPLY(UInt32, o == kReferenceStream && gInputAvailable ? 2 : 1);
        case kAudioStreamPropertyLatency: REPLY(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: REPLY(AudioStreamBasicDescription, format(channels));
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription r = {format(channels), {kRate, kRate}};
            REPLY(AudioStreamRangedDescription, r);
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    }
    case kVolume:
    case kReceiverVolume:
    case kInputVolume:
        if (o==kInputVolume && !gInputAvailable) return kAudioHardwareBadObjectError;
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioLevelControlClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioVolumeControlClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioControlPropertyScope: REPLY(AudioObjectPropertyScope, o==kInputVolume ? kAudioObjectPropertyScopeInput:kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: REPLY(AudioObjectPropertyElement,o==kInputVolume ? 0:o==kReceiverVolume ? 3:4);
        case kAudioLevelControlPropertyScalarValue: REPLY(Float32, o==kInputVolume ? gInputVolume:o==kReceiverVolume ? gReceiverVolume:gVolume);
        case kAudioLevelControlPropertyDecibelValue: REPLY(Float32, toDb(o==kInputVolume ? gInputVolume:o==kReceiverVolume ? gReceiverVolume:gVolume));
        case kAudioLevelControlPropertyDecibelRange: REPLY(AudioValueRange, ((AudioValueRange){kMinDb, 0.0}));
        case kAudioLevelControlPropertyConvertScalarToDecibels:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = toDb(*(Float32 *)outData); *outDataSize = sizeof(Float32); return 0;
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = toScalar(*(Float32 *)outData); *outDataSize = sizeof(Float32); return 0;
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kReceiverMute:
    case kSpeakerMute:
    case kInputMute:
        if (o==kInputMute && !gInputAvailable) return kAudioHardwareBadObjectError;
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID,kAudioBooleanControlClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID,kAudioMuteControlClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID,kDevice);
        case kAudioObjectPropertyOwnedObjects: *outDataSize=0;return 0;
        case kAudioControlPropertyScope: REPLY(AudioObjectPropertyScope,o==kInputMute ? kAudioObjectPropertyScopeInput:kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: REPLY(AudioObjectPropertyElement,o==kReceiverMute ? 3:o==kSpeakerMute ? 4:0);
        case kAudioBooleanControlPropertyValue: REPLY(UInt32,o==kInputMute ? gInputMuted:output_mute(o));
        default:return kAudioHardwareUnknownPropertyError;
        }
    case kInputSource:
        if (!gInputAvailable) return kAudioHardwareBadObjectError;
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID,kAudioSelectorControlClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID,kAudioDataSourceControlClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID,kDevice);
        case kAudioObjectPropertyOwnedObjects: *outDataSize=0;return 0;
        case kAudioControlPropertyScope: REPLY(AudioObjectPropertyScope,kAudioObjectPropertyScopeInput);
        case kAudioControlPropertyElement: REPLY(AudioObjectPropertyElement,0);
        case kAudioSelectorControlPropertyCurrentItem:
            REPLY(UInt32,gSelectedMicSource);
        case kAudioSelectorControlPropertyAvailableItems: REPLY(UInt32,kMicSource);
        case kAudioSelectorControlPropertyItemName:
            if (!q || qs!=sizeof(UInt32) || *(const UInt32 *)q!=kMicSource)
                return kAudioHardwareIllegalOperationError;
            REPLY(CFStringRef,(CFStringRef)CFRetain(CFSTR("Microphone")));
        case kAudioSelectorControlPropertyItemKind:
            if (!q || qs!=sizeof(UInt32) || *(const UInt32 *)q!=kMicSource)
                return kAudioHardwareIllegalOperationError;
            REPLY(UInt32,kMicSourceKind);
        default:return kAudioHardwareUnknownPropertyError;
        }
    default:
        return kAudioHardwareBadObjectError;
    }
}
