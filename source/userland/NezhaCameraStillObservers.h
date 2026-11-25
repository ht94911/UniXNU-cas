/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_STILL_OBSERVERS_H
#define NEZHA_CAMERA_STILL_OBSERVERS_H

/* Existing request/prepare/completion boundaries, exact24A446 own methods.
 * No block or settings are replaced and every original is called once. */
static int32_t still_capture(id self,SEL selector,id settings,id capture,id asset)
{
    static unsigned reports;
    const bool report=__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)<16;
    if (report) fprintf(stderr,"nezha-camera: original still device-request entered ticks=%llu settings=%u capture=%u asset=%u\n",
        (unsigned long long)mach_absolute_time(),(unsigned)(settings!=NULL),(unsigned)(capture!=NULL),(unsigned)(asset!=NULL));
    const int32_t status=((int32_t (*)(id,SEL,id,id,id))original_still_capture)(self,selector,settings,capture,asset);
    if (report) fprintf(stderr,"nezha-camera: original still device-request returned ticks=%llu status=%d\n",
        (unsigned long long)mach_absolute_time(),status);
    return status;
}

static int32_t still_initiate(id self,SEL selector,id settings)
{
    static unsigned reports;
    const bool report=__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)<16;
    if (report) fprintf(stderr,"nezha-camera: original still coordinator-request entered ticks=%llu settings=%u\n",
        (unsigned long long)mach_absolute_time(),(unsigned)(settings!=NULL));
    const int32_t status=((int32_t (*)(id,SEL,id))original_still_initiate)(self,selector,settings);
    if (report) fprintf(stderr,"nezha-camera: original still coordinator-request returned ticks=%llu status=%d\n",
        (unsigned long long)mach_absolute_time(),status);
    return status;
}

static int32_t still_prepare(id self,SEL selector,int32_t retained,int32_t brackets,bool sushi)
{
    const int32_t status=((int32_t (*)(id,SEL,int32_t,int32_t,bool))original_still_prepare)
        (self,selector,retained,brackets,sushi);
    static unsigned reports;
    if (__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)<16)
        fprintf(stderr,"nezha-camera: original still source-prepare ticks=%llu retained=%d brackets=%d sushi=%u status=%d\n",
            (unsigned long long)mach_absolute_time(),retained,brackets,(unsigned)sushi,status);
    return status;
}

static void still_complete(id self,SEL selector,id stream,CMTime time,int32_t status)
{
    ((void (*)(id,SEL,id,CMTime,int32_t))original_still_complete)(self,selector,stream,time,status);
    static unsigned reports;
    if (__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)<16)
        fprintf(stderr,"nezha-camera: original still device-complete ticks=%llu stream=%u status=%d pts=%lld/%d flags=%u epoch=%lld\n",
            (unsigned long long)mach_absolute_time(),(unsigned)(stream!=NULL),status,
            (long long)time.value,time.timescale,(unsigned)time.flags,(long long)time.epoch);
}

static void install_still_observers(void)
{
    if (still_observers_installed) return;
    struct {const char *class_name,*selector,*encoding;uintptr_t address;IMP replacement;IMP *original;} rows[]={
        {"BWFigVideoCaptureDevice","captureStillImageNowWithSettings:captureSettings:assetBundle:",
            "i40@0:8@16@24@32",0x1b089ae30ULL,(IMP)still_capture,&original_still_capture},
        {"BWStillImageCoordinatorNode","initiateStillImageCaptureNowWithSettings:",
            "i24@0:8@16",0x1b079e290ULL,(IMP)still_initiate,&original_still_initiate},
        {"BWMultiStreamCameraSourceNode","prepareForStillImageCaptureWithFirmwareStillImageOutputRetainedBufferCountOverride:clientBracketCount:enableSushiRawAttachmentOption:",
            "i28@0:8i16i20B24",0x1b06b0128ULL,(IMP)still_prepare,&original_still_prepare},
        {"BWFigVideoCaptureDevice","captureStream:didCompleteStillImageCaptureWithPTS:error:",
            "v52@0:8@16{?=qiIq}24i48",0x1b0898638ULL,(IMP)still_complete,&original_still_complete},
    };
    Method methods[4];
    for (unsigned i=0;i<4;++i) {
        Class type=objc_getClass(rows[i].class_name);
        methods[i]=type ? class_getInstanceMethod(type,sel_registerName(rows[i].selector)):NULL;
        IMP current=methods[i] ? method_getImplementation(methods[i]):NULL;
        const char *encoding=methods[i] ? method_getTypeEncoding(methods[i]):NULL;
        if (!current || !encoding || strcmp(encoding,rows[i].encoding) ||
            (uintptr_t)ptrauth_strip(current,ptrauth_key_function_pointer)!=(uintptr_t)(rows[i].address+capture_slide)) return;
    }
    for (unsigned i=0;i<4;++i) {
        *rows[i].original=method_getImplementation(methods[i]);
        method_setImplementation(methods[i],rows[i].replacement);
    }
    still_observers_installed=true;
    fputs("nezha-camera: original still request/prepare/completion observers installed\n",stderr);
}

#endif
