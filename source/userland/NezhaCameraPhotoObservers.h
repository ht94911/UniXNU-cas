/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_PHOTO_OBSERVERS_H
#define NEZHA_CAMERA_PHOTO_OBSERVERS_H

/* Original24A446 photo boundaries, following HakiOS's encoder observation.
 * Only this sample's existing attachments are read. Settings, buffers,
 * output handles, errors and the original calls retain their ownership. */
static bool photo_observers_installed;
static IMP original_photo_emit, original_photo_render, original_photo_encode, original_photo_sink;

static double photo_value(CFTypeRef value)
{
    double result;
    return value && CFGetTypeID(value)==CFNumberGetTypeID() &&
        CFNumberGetValue(value,kCFNumberDoubleType,&result) ? result:-1;
}

static double photo_number(CFTypeRef dictionary, CFStringRef key)
{
    if (!dictionary || CFGetTypeID(dictionary)!=CFDictionaryGetTypeID()) return -1;
    return photo_value(CFDictionaryGetValue(dictionary,key));
}

static void photo_sample(const char *stage,CMSampleBufferRef sample)
{
    CVImageBufferRef image=sample ? CMSampleBufferGetImageBuffer(sample):NULL;
    CFTypeRef metadata=sample ? CMGetAttachment(sample,CFSTR("MetadataDictionary"),NULL):NULL;
    const CMTime pts=sample ? CMSampleBufferGetPresentationTimeStamp(sample):(CMTime){0};
    fprintf(stderr,"nezha-camera: photo %s entered ticks=%llu sample=%p image=%u size=%llux%llu "
        "metadata=%u settings=%u flags=%.0f sequence=%.0f generation=%.0f pts=%lld/%d/%u/%lld\n",
        stage,(unsigned long long)mach_absolute_time(),(void *)sample,(unsigned)(image!=NULL),
        (unsigned long long)(image ? CVPixelBufferGetWidth(image):0),
        (unsigned long long)(image ? CVPixelBufferGetHeight(image):0),(unsigned)(metadata!=NULL),
        (unsigned)(sample && CMGetAttachment(sample,CFSTR("StillSettings"),NULL)!=NULL),
        photo_value(sample ? CMGetAttachment(sample,CFSTR("StillImageProcessingFlags"),NULL):NULL),
        photo_number(metadata,CFSTR("FrameSequence")),photo_number(metadata,CFSTR("NezhaCameraGraphGeneration")),
        (long long)pts.value,pts.timescale,(unsigned)pts.flags,(long long)pts.epoch);
}

static void photo_emit(id self,SEL selector,id stream,CMSampleBufferRef sample,bool cached)
{
    static unsigned reports;
    const bool report=__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)<16;
    if (report) photo_sample("device-emit",sample);
    ((void (*)(id,SEL,id,CMSampleBufferRef,bool))original_photo_emit)(self,selector,stream,sample,cached);
    if (report) fprintf(stderr,"nezha-camera: photo device-emit returned ticks=%llu cached=%u\n",
        (unsigned long long)mach_absolute_time(),(unsigned)cached);
}

static void photo_render(id self,SEL selector,CMSampleBufferRef sample,id input)
{
    static unsigned reports;
    const bool report=__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)<16;
    if (report) photo_sample("encoder",sample);
    ((void (*)(id,SEL,CMSampleBufferRef,id))original_photo_render)(self,selector,sample,input);
    if (report) fprintf(stderr,"nezha-camera: photo encoder returned ticks=%llu\n",
        (unsigned long long)mach_absolute_time());
}

static int32_t photo_encode(id self,SEL selector,CMSampleBufferRef sample,id metadata,
    uint32_t flags,int64_t *handle)
{
    static unsigned reports;
    const bool report=__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)<16;
    if (report) photo_sample("encode-primary",sample);
    const int32_t status=((int32_t (*)(id,SEL,CMSampleBufferRef,id,uint32_t,int64_t *))original_photo_encode)
        (self,selector,sample,metadata,flags,handle);
    if (report) fprintf(stderr,"nezha-camera: photo encode-primary returned ticks=%llu status=%d flags=%u handle-present=%u\n",
        (unsigned long long)mach_absolute_time(),status,flags,(unsigned)(handle!=NULL));
    return status;
}

static void photo_sink(id self,SEL selector,CMSampleBufferRef sample,id input)
{
    static unsigned reports;
    const bool report=__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)<16;
    if (report) photo_sample("sink",sample);
    ((void (*)(id,SEL,CMSampleBufferRef,id))original_photo_sink)(self,selector,sample,input);
    if (report) fprintf(stderr,"nezha-camera: photo sink returned ticks=%llu\n",
        (unsigned long long)mach_absolute_time());
}

static void install_photo_observers(void)
{
    if (photo_observers_installed) return;
    struct {const char *class_name,*selector,*encoding;uintptr_t address;IMP replacement;IMP *original;} rows[]={
        {"BWFigVideoCaptureDevice","captureStream:willEmitStillImageSampleBuffer:cachedStreamingBuffer:",
            "v36@0:8@16^{opaqueCMSampleBuffer=}24B32",0x1b069669cULL,(IMP)photo_emit,&original_photo_emit},
        {"BWPhotoEncoderNode","renderSampleBuffer:forInput:",
            "v32@0:8^{opaqueCMSampleBuffer=}16@24",0x1b06a9aa0ULL,(IMP)photo_render,&original_photo_render},
        {"BWPhotoEncoderController","_encodePrimarySbuf:metadata:processingFlags:primaryImageHandleInOut:",
            "i44@0:8^{opaqueCMSampleBuffer=}16@24I32^q36",0x1b065c5c8ULL,(IMP)photo_encode,&original_photo_encode},
        {"BWStillImageSampleBufferSinkNode","renderSampleBuffer:forInput:",
            "v32@0:8^{opaqueCMSampleBuffer=}16@24",0x1b062f864ULL,(IMP)photo_sink,&original_photo_sink},
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
    photo_observers_installed=true;
    fputs("nezha-camera: original photo emission/encoder/sink observers installed\n",stderr);
}
#endif
