/* SPDX-License-Identifier: MIT */
#include "NezhaCameraDeviceBackend.h"
#include "NezhaCameraFactory.h"
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <ptrauth.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* HakiOS inject_media.c supplies the image-arrival method installation.
 * All method identity and ABI data here belong to the measured 24A446
 * CMCapture UUID. The factory signature and current default initializer
 * were checked in that image, not inherited from its iOS16 shim. */
static bool installed;
static bool capture_present;
static bool model_installed;
static bool device_ids_installed;
static bool backings_init_installed,backings_list_installed;
static bool sources_installed;
static bool source_init_installed;
static bool publishing_installed;
static bool stream_request_installed;
static intptr_t capture_slide;
static IMP original;
static IMP original_model_resource;
static IMP original_device_ids;
static IMP original_backings_init,original_backings_list;
static IMP original_sources;
static IMP original_source_init;
static pthread_mutex_t main_backings_lock=PTHREAD_MUTEX_INITIALIZER;
static CFArrayRef main_backings;
static IMP original_publishing;
static IMP original_stream_request;
static const struct NezhaFigDeviceVTable *(*source_vtable)(NezhaFigObject);
static const char selector_name[]="sharedCaptureDeviceVendorWithDefaultDeviceCreateFunction:";
static const char model_selector_name[]="URLForResource:withExtension:subdirectory:";
static const char device_ids_selector_name[]="deviceIDs";
static const char backings_init_selector[]="initWithSourceInfoDictionaries:commonSettings:";
static const char backings_list_selector[]="sourceBackings";
static const char sources_selector[]="copySourcesWithType:clientAuditToken:clientIsBincompat:";
static const char source_init_selector[]="initWithBackings:fineGrainThermalMonitoringEnabled:apsCalibrationMonitorEnabled:useSharedCaptureSourceBackingsProvider:";
static const char publishing_selector[]="copyDeviceForPublishingWithID:error:";
static const char stream_request_selector[]="copyStreamsFromDevice:positions:deviceTypes:deviceClientPriority:allowsStreamControlLoss:error:";
static const uint8_t capture_uuid[16]={0x99,0x5f,0xc2,0x2b,0x41,0xbd,0x37,0xb4,
    0x81,0x2f,0x26,0x86,0xb8,0x1c,0x26,0x94};
typedef NezhaFigStatus (*CameraCreate)(NezhaFigAllocator,NezhaFigString,NezhaFigObject *);
/* Original @56@0:8i16{?=[8I]}20B52. ARM64 passes this 32-byte value
 * indirectly in x3, but its C/ObjC contract is a by-value audit token. */
typedef struct {uint32_t val[8];} CameraAuditToken;

static bool single_main_backing(id array)
{
    if (!array || ((unsigned long (*)(id,SEL))objc_msgSend)(array,sel_registerName("count"))!=1) return false;
    id backing=((id (*)(id,SEL,unsigned long))objc_msgSend)(array,sel_registerName("objectAtIndex:"),0);
    id attributes=((id (*)(id,SEL))objc_msgSend)(backing,sel_registerName("attributes"));
    id identifier=((id (*)(id,SEL))objc_msgSend)(attributes,sel_registerName("uniqueID"));
    return ((uint32_t (*)(id,SEL))objc_msgSend)(backing,sel_registerName("mediaType"))==0x76696465u &&
        ((int32_t (*)(id,SEL))objc_msgSend)(attributes,sel_registerName("deviceType"))==2 &&
        ((int32_t (*)(id,SEL))objc_msgSend)(attributes,sel_registerName("position"))==1 &&
        identifier && ((bool (*)(id,SEL,id))objc_msgSend)(identifier,sel_registerName("isEqualToString:"),
            (id)CFSTR("com.apple.avfoundation.avcapturedevice.built-in_video:0"));
}

static void remember_main_backings(id array)
{
    if (!single_main_backing(array)) return;
    CFArrayRef copy=(CFArrayRef)CFRetain((CFTypeRef)array);
    pthread_mutex_lock(&main_backings_lock);
    CFArrayRef previous=main_backings;main_backings=copy;
    pthread_mutex_unlock(&main_backings_lock);
    if (previous) CFRelease(previous);
}

/* _fcsbp_plistCreateAndPreprocess calls the original NSBundle lookup with
 * AVCaptureSession.plist, including a NULL subdirectory in actual24A446. Redirect only
 * this exact camera bundle lookup to the matching single-main model; every
 * other resource still uses its unchanged bundle and original method. */
static id model_resource(id self,SEL selector,id resource,id extension,id subdirectory)
{
    static unsigned reported;
    const id bundle_id=((id (*)(id,SEL))objc_msgSend)(self,sel_registerName("bundleIdentifier"));
    const bool camera_bundle=bundle_id && ((bool (*)(id,SEL,id))objc_msgSend)
        (bundle_id,sel_registerName("isEqualToString:"),(id)CFSTR("com.apple.CMCapture"));
    const bool camera_file=resource && ((bool (*)(id,SEL,id))objc_msgSend)
        (resource,sel_registerName("isEqualToString:"),(id)CFSTR("AVCaptureSession.plist"));
    const bool selected=camera_bundle && camera_file && !extension;
    const bool readable=selected && access("/System/Library/MediaCapture/NezhaCameraModel.plist",R_OK)==0;
    if (camera_file && __atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4)
        fprintf(stderr,"nezha-camera: model lookup camera=%u extension=%u directory=%u readable=%u\n",
            (unsigned)camera_bundle,(unsigned)(extension!=NULL),(unsigned)(subdirectory!=NULL),(unsigned)readable);
    if (readable) {
        static unsigned model_read_reported;
        if (__atomic_fetch_add(&model_read_reported,1u,__ATOMIC_RELAXED)<2) {
            Class dictionary=objc_getClass("NSDictionary");
            id model=dictionary ? ((id (*)(id,SEL,id))objc_msgSend)(dictionary,
                sel_registerName("dictionaryWithContentsOfFile:"),(id)CFSTR("/System/Library/MediaCapture/NezhaCameraModel.plist")) : NULL;
            id devices=model ? ((id (*)(id,SEL,id))objc_msgSend)(model,sel_registerName("objectForKey:"),(id)CFSTR("AVCaptureDevices")) : NULL;
            const unsigned long count=devices ? ((unsigned long (*)(id,SEL))objc_msgSend)(devices,sel_registerName("count")) : 0;
            fprintf(stderr,"nezha-camera: model parsed=%u AVCaptureDevices=%lu\n",(unsigned)(model!=NULL),count);
        }
        Class url=objc_getClass("NSURL");
        if (url) return ((id (*)(id,SEL,id))objc_msgSend)(url,sel_registerName("fileURLWithPath:"),
            (id)CFSTR("/System/Library/MediaCapture/NezhaCameraModel.plist"));
    }
    return ((id (*)(id,SEL,id,id,id))original_model_resource)(self,selector,resource,extension,subdirectory);
}

static void install_model_resource(void)
{
    if (model_installed) return;
    Class bundle=objc_getClass("NSBundle");
    Method method=bundle ? class_getInstanceMethod(bundle,sel_registerName(model_selector_name)) : NULL;
    IMP current=method ? method_getImplementation(method) : NULL;
    const uintptr_t address=(uintptr_t)ptrauth_strip(current,ptrauth_key_function_pointer);
    if (!current || address!=(uintptr_t)(0x180b9cf90ULL+capture_slide)) return;
    original_model_resource=current;
    method_setImplementation(method,(IMP)model_resource);
    model_installed=true;
    fprintf(stderr,"nezha-camera: measured main sensor model installed\n");
}

static id shared_vendor(id self,SEL selector,CameraCreate factory)
{
    static unsigned reported;
    const int status=nezha_camera_device_backend_bind();
    if (status==0)
        factory=nezha_camera_device_create;
    const id vendor=((id (*)(id,SEL,CameraCreate))original)(self,selector,factory);
    if (__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4) {
        Class type=objc_getClass("BWFigCaptureDeviceVendor");
        Ivar ivar=type ? class_getInstanceVariable(type,"_defaultDeviceCreateFunction") : NULL;
        const bool layout=vendor && object_getClass(vendor)==type && ivar && ivar_getOffset(ivar)==8;
        CameraCreate saved=NULL;
        if (layout) memcpy(&saved,(const uint8_t *)vendor+8,sizeof(saved));
        const uintptr_t stored=(uintptr_t)ptrauth_strip(saved,ptrauth_key_function_pointer);
        const CameraCreate ours_factory=nezha_camera_device_create;
        const uintptr_t ours=(uintptr_t)ptrauth_strip(ours_factory,ptrauth_key_function_pointer);
        fprintf(stderr,"nezha-camera: vendor request backend=%d deviceVendor=%u savedLayout=%u savedFactory=%u savedOurs=%u\n",
            status,(unsigned)(vendor!=NULL),(unsigned)layout,(unsigned)(stored!=0),(unsigned)(stored==ours));
    }
    return vendor;
}

static id device_ids(id self,SEL selector)
{
    static unsigned reported;
    id ids=((id (*)(id,SEL))original_device_ids)(self,selector);
    if (__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4) {
        const unsigned long count=ids ? ((unsigned long (*)(id,SEL))objc_msgSend)(ids,sel_registerName("count")) : 0;
        const bool main=ids && ((bool (*)(id,SEL,id))objc_msgSend)(ids,sel_registerName("containsObject:"),(id)CFSTR("Default"));
        fprintf(stderr,"nezha-camera: vendor deviceIDs count=%lu default=%u\n",count,(unsigned)main);
    }
    return ids;
}

static void install_device_ids(void)
{
    if (device_ids_installed) return;
    Class type=objc_getClass("BWFigCaptureDeviceVendor");
    Method method=type ? class_getInstanceMethod(type,sel_registerName(device_ids_selector_name)) : NULL;
    IMP current=method ? method_getImplementation(method) : NULL;
    const uintptr_t address=(uintptr_t)ptrauth_strip(current,ptrauth_key_function_pointer);
    if (!current || address!=(uintptr_t)(0x1b080b2b8ULL+capture_slide)) return;
    original_device_ids=current;
    method_setImplementation(method,(IMP)device_ids);
    device_ids_installed=true;
    fprintf(stderr,"nezha-camera: measured device list observer installed\n");
}

static id publishing_device(id self,SEL selector,id device_id,int32_t *error)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4;
    if (report) fprintf(stderr,"nezha-camera: publishing request default=%u errorOut=%u\n",
        (unsigned)(device_id && ((bool (*)(id,SEL,id))objc_msgSend)(device_id,sel_registerName("isEqualToString:"),(id)CFSTR("Default"))),
        (unsigned)(error!=NULL));
    id device=((id (*)(id,SEL,id,int32_t *))original_publishing)(self,selector,device_id,error);
    if (report) {
        id streams=device ? ((id (*)(id,SEL))objc_msgSend)(device,sel_registerName("streams")) : NULL;
        const unsigned long count=streams ? ((unsigned long (*)(id,SEL))objc_msgSend)(streams,sel_registerName("count")) : 0;
        fprintf(stderr,"nezha-camera: publishing result device=%u error=%d streams=%lu\n",
            (unsigned)(device!=NULL),error?*error:0,count);
    }
    return device;
}

static id request_streams(id self,SEL selector,id device,id positions,id types,
    int32_t priority,bool allows_loss,int32_t *error)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4;
    id streams=((id (*)(id,SEL,id,id,id,int32_t,bool,int32_t *))original_stream_request)
        (self,selector,device,positions,types,priority,allows_loss,error);
    if (report) {
        const unsigned long count=streams ? ((unsigned long (*)(id,SEL))objc_msgSend)
            (streams,sel_registerName("count")) : 0;
        fprintf(stderr,"nezha-camera: original stream request priority=%d loss=%u streams=%lu error=%d\n",
            priority,(unsigned)allows_loss,count,error?*error:0);
    }
    return streams;
}

static void install_stream_request(void)
{
    if (stream_request_installed) return;
    Class type=objc_getClass("BWFigCaptureDeviceVendor");
    Method method=type ? class_getInstanceMethod(type,sel_registerName(stream_request_selector)) : NULL;
    IMP current=method ? method_getImplementation(method) : NULL;
    if (!current || (uintptr_t)ptrauth_strip(current,ptrauth_key_function_pointer)!=(uintptr_t)(0x1b0436bf8ULL+capture_slide)) return;
    original_stream_request=current;method_setImplementation(method,(IMP)request_streams);
    stream_request_installed=true;
    fprintf(stderr,"nezha-camera: measured original stream request observer installed\n");
}

static void install_publishing(void)
{
    if (publishing_installed) return;
    Class type=objc_getClass("BWFigCaptureDeviceVendor");
    Method method=type ? class_getInstanceMethod(type,sel_registerName(publishing_selector)) : NULL;
    IMP current=method ? method_getImplementation(method) : NULL;
    if (!current || (uintptr_t)ptrauth_strip(current,ptrauth_key_function_pointer)!=(uintptr_t)(0x1b080b458ULL+capture_slide)) return;
    original_publishing=current;method_setImplementation(method,(IMP)publishing_device);
    publishing_installed=true;
    fprintf(stderr,"nezha-camera: measured publishing request observer installed\n");
}

static id backings_init(id self,SEL selector,id dictionaries,id common)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4;
    if (report) {
        const unsigned long count=dictionaries ? ((unsigned long (*)(id,SEL))objc_msgSend)(dictionaries,sel_registerName("count")) : 0;
        fprintf(stderr,"nezha-camera: sourceInfo input count=%lu common=%u\n",count,(unsigned)(common!=NULL));
    }
    id provider=((id (*)(id,SEL,id,id))original_backings_init)(self,selector,dictionaries,common);
    id backings=provider && original_backings_list ?
        ((id (*)(id,SEL))original_backings_list)(provider,sel_registerName(backings_list_selector)):NULL;
    remember_main_backings(backings);
    if (report) {
        const unsigned long count=backings ? ((unsigned long (*)(id,SEL))objc_msgSend)
            (backings,sel_registerName("count")):0;
        fprintf(stderr,"nezha-camera: sourceInfo provider created=%u backings_after_init=%lu\n",
            (unsigned)(provider!=NULL),count);
    }
    return provider;
}

static id source_init(id self,SEL selector,id backings,bool thermal,bool calibration,bool shared)
{
    CFArrayRef selected=NULL;
    if (shared) {
        pthread_mutex_lock(&main_backings_lock);
        if (main_backings) selected=(CFArrayRef)CFRetain(main_backings);
        pthread_mutex_unlock(&main_backings_lock);
    }
    /* Actual077 emitted one real main backing, then the shared source
     * provider removed it and Camera's query observed an empty shared list.
     * The original manager supports an independent immutable backing list.
     * Bind that list at native initialization; all original source creation,
     * client filters, activation and graph lifecycle remain its responsibility. */
    id manager=((id (*)(id,SEL,id,bool,bool,bool))original_source_init)
        (self,selector,selected ? (id)selected:backings,thermal,calibration,selected ? false:shared);
    static unsigned reported;
    if (__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4)
        fprintf(stderr,"nezha-camera: original source manager init shared=%u selected_main=%u created=%u\n",
            (unsigned)shared,(unsigned)(selected!=NULL),(unsigned)(manager!=NULL));
    if (selected) CFRelease(selected);
    return manager;
}

static id backings_list(id self,SEL selector)
{
    static unsigned reported;
    id array=((id (*)(id,SEL))original_backings_list)(self,selector);
    if (__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4) {
        const unsigned long count=array ? ((unsigned long (*)(id,SEL))objc_msgSend)(array,sel_registerName("count")) : 0;
        fprintf(stderr,"nezha-camera: sourceBackings count=%lu\n",count);
    }
    return array;
}

static void install_backings(void)
{
    Class type=objc_getClass("FigCaptureSourceBackingsProvider");
    if (!type) return;
    if (!backings_init_installed) {
        Method method=class_getInstanceMethod(type,sel_registerName(backings_init_selector));
        IMP current=method ? method_getImplementation(method) : NULL;
        if (current && (uintptr_t)ptrauth_strip(current,ptrauth_key_function_pointer)==(uintptr_t)(0x1b07a9d9cULL+capture_slide)) {
            original_backings_init=current;method_setImplementation(method,(IMP)backings_init);
            backings_init_installed=true;
        }
    }
    if (!backings_list_installed) {
        Method method=class_getInstanceMethod(type,sel_registerName(backings_list_selector));
        IMP current=method ? method_getImplementation(method) : NULL;
        if (current && (uintptr_t)ptrauth_strip(current,ptrauth_key_function_pointer)==(uintptr_t)(0x1b07a9e60ULL+capture_slide)) {
            original_backings_list=current;method_setImplementation(method,(IMP)backings_list);
            backings_list_installed=true;
            fprintf(stderr,"nezha-camera: measured sourceInfo observers installed\n");
        }
    }
}

static void report_returned_source(NezhaFigObject source,unsigned index)
{
    const struct NezhaFigDeviceVTable *table=source && source_vtable ? source_vtable(source):NULL;
    if (!table || !table->base || !table->base->copy_property) return;
    const CFStringRef keys[]={CFSTR("SourceToken"),CFSTR("ExtendedAttributes"),
        CFSTR("Formats"),CFSTR("Connected")};
    CFTypeRef values[4]={NULL,NULL,NULL,NULL};
    int32_t status[4];
    for (unsigned i=0;i<4;++i)
        status[i]=table->base->copy_property(source,keys[i],NULL,&values[i]);
    id attributes=(id)values[1],formats=(id)values[2];
    id identifier=attributes ? ((id (*)(id,SEL))objc_msgSend)(attributes,sel_registerName("uniqueID")):NULL;
    const char *identifier_text=identifier ? ((const char *(*)(id,SEL))objc_msgSend)
        (identifier,sel_registerName("UTF8String")):NULL;
    fprintf(stderr,"nezha-camera: original returned source index=%u token=%u err=%d attributes=%u err=%d sourceType=%d deviceType=%d position=%d id=%s formats=%lu err=%d connected=%u err=%d\n",
        index,(unsigned)(values[0]!=NULL),status[0],(unsigned)(attributes!=NULL),status[1],
        attributes ? ((int32_t (*)(id,SEL))objc_msgSend)(attributes,sel_registerName("sourceType")):0,
        attributes ? ((int32_t (*)(id,SEL))objc_msgSend)(attributes,sel_registerName("deviceType")):0,
        attributes ? ((int32_t (*)(id,SEL))objc_msgSend)(attributes,sel_registerName("position")):0,
        identifier_text ?: "<absent>",formats ? ((unsigned long (*)(id,SEL))objc_msgSend)(formats,sel_registerName("count")):0,
        status[2],values[3] ? (unsigned)((bool (*)(id,SEL))objc_msgSend)((id)values[3],sel_registerName("boolValue")):0,status[3]);
    for (unsigned i=0;i<4;++i) if (values[i]) CFRelease(values[i]);
}

static id copy_sources(id self,SEL selector,int32_t type,CameraAuditToken audit,bool bincompat)
{
    static unsigned reported;
    const bool report=type==1 && __atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<12;
    id sources=((id (*)(id,SEL,int32_t,CameraAuditToken,bool))original_sources)
        (self,selector,type,audit,bincompat);
    if (report) {
        const unsigned long count=sources ? ((unsigned long (*)(id,SEL))objc_msgSend)
            (sources,sel_registerName("count")):0;
        fprintf(stderr,"nezha-camera: original sources type=%d pid=%u bincompat=%u returned=%u count=%lu\n",
            type,audit.val[5],(unsigned)bincompat,(unsigned)(sources!=NULL),count);
        for (unsigned i=0;source_vtable && i<count && i<4;++i) {
            NezhaFigObject source=(NezhaFigObject)((id (*)(id,SEL,unsigned long))objc_msgSend)
                (sources,sel_registerName("objectAtIndex:"),(unsigned long)i);
            report_returned_source(source,i);
        }
        Class manager=objc_getClass("FigCaptureSourceManager");
        Ivar ivar=manager ? class_getInstanceVariable(manager,"_captureSourceBackings"):NULL;
        Ivar shared=manager ? class_getInstanceVariable(manager,"_useSharedCaptureSourceBackingsProvider"):NULL;
        if (object_getClass(self)==manager && ivar && shared && ivar_getOffset(ivar)==8 &&
            ivar_getOffset(shared)==16) {
            id backings=NULL;uint8_t uses_shared=0;
            memcpy(&uses_shared,(const uint8_t *)self+16,sizeof(uses_shared));
            if (!uses_shared) memcpy(&backings,(const uint8_t *)self+8,sizeof(backings));
            else {
                Class provider=objc_getClass("FigCaptureSourceBackingsProvider");
                id singleton=provider ? ((id (*)(id,SEL))objc_msgSend)
                    (provider,sel_registerName("sharedCaptureSourceBackingsProvider")):NULL;
                backings=singleton ? ((id (*)(id,SEL))objc_msgSend)
                    (singleton,sel_registerName(backings_list_selector)):NULL;
            }
            const unsigned long backing_count=backings ? ((unsigned long (*)(id,SEL))objc_msgSend)
                (backings,sel_registerName("count")):0;
            fprintf(stderr,"nezha-camera: original source manager shared=%u backings=%lu\n",
                (unsigned)uses_shared,backing_count);
            if (backing_count==1) {
                id backing=((id (*)(id,SEL,unsigned long))objc_msgSend)
                    (backings,sel_registerName("objectAtIndex:"),0);
                id attributes=((id (*)(id,SEL))objc_msgSend)(backing,sel_registerName("attributes"));
                id formats=((id (*)(id,SEL))objc_msgSend)(backing,sel_registerName("formats"));
                id missing=((id (*)(id,SEL))objc_msgSend)(backing,sel_registerName("missingFormatNames"));
                fprintf(stderr,"nezha-camera: original backing media=0x%x deviceType=%d position=%d formats=%lu missing=%lu\n",
                    ((uint32_t (*)(id,SEL))objc_msgSend)(backing,sel_registerName("mediaType")),
                    ((int32_t (*)(id,SEL))objc_msgSend)(attributes,sel_registerName("deviceType")),
                    ((int32_t (*)(id,SEL))objc_msgSend)(attributes,sel_registerName("position")),
                    formats ? ((unsigned long (*)(id,SEL))objc_msgSend)(formats,sel_registerName("count")):0,
                    missing ? ((unsigned long (*)(id,SEL))objc_msgSend)(missing,sel_registerName("count")):0);
            }
        }
    }
    return sources;
}

static void install_sources(void)
{
    Class type=objc_getClass("FigCaptureSourceManager");
    if (!type) return;
    if (!source_init_installed) {
        Method init=class_getInstanceMethod(type,sel_registerName(source_init_selector));
        IMP initializer=init ? method_getImplementation(init):NULL;
        const char *encoding=init ? method_getTypeEncoding(init):NULL;
        if (initializer && encoding && !strcmp(encoding,"@36@0:8@16B24B28B32") &&
            (uintptr_t)ptrauth_strip(initializer,ptrauth_key_function_pointer)==(uintptr_t)(0x1b06e2928ULL+capture_slide)) {
            original_source_init=initializer;method_setImplementation(init,(IMP)source_init);
            source_init_installed=true;
            fputs("nezha-camera: measured independent main backing binding installed\n",stderr);
        }
    }
    if (sources_installed) return;
    Method method=type ? class_getInstanceMethod(type,sel_registerName(sources_selector)):NULL;
    IMP current=method ? method_getImplementation(method):NULL;
    const char *encoding=method ? method_getTypeEncoding(method):NULL;
    if (!current || !encoding || strcmp(encoding,"@56@0:8i16{?=[8I]}20B52") ||
        (uintptr_t)ptrauth_strip(current,ptrauth_key_function_pointer)!=(uintptr_t)(0x1b0451d48ULL+capture_slide)) return;
    original_sources=current;method_setImplementation(method,(IMP)copy_sources);
    sources_installed=true;
    fputs("nezha-camera: measured original sources observer installed\n",stderr);
}

static bool uuid_matches(const struct mach_header_64 *header)
{
    if (!header || header->magic!=MH_MAGIC_64 || header->ncmds>256 || header->sizeofcmds>65536) return false;
    const uint8_t *cursor=(const uint8_t *)(header+1),*end=cursor+header->sizeofcmds;
    for (unsigned i=0;i<header->ncmds;++i) {
        if ((size_t)(end-cursor)<sizeof(struct load_command)) return false;
        const struct load_command *command=(const void *)cursor;
        if (command->cmdsize<sizeof(*command) || command->cmdsize>(size_t)(end-cursor)) return false;
        if (command->cmd==LC_UUID)
            return command->cmdsize>=sizeof(struct uuid_command) &&
                !memcmp(((const struct uuid_command *)command)->uuid,capture_uuid,16);
        cursor+=command->cmdsize;
    }
    return false;
}

static void image_added(const struct mach_header *header,intptr_t slide)
{
    if (installed && model_installed && device_ids_installed && publishing_installed && stream_request_installed && backings_init_installed && backings_list_installed && sources_installed && source_init_installed) return;
    if (uuid_matches((const struct mach_header_64 *)header)) {
        capture_present=true;capture_slide=slide;
    }
    if (!capture_present) return;
    if (!source_vtable)
        source_vtable=(const struct NezhaFigDeviceVTable *(*)(NezhaFigObject))dlsym(RTLD_DEFAULT,"CMBaseObjectGetVTable");
    install_model_resource();
    install_device_ids();
    install_publishing();
    install_stream_request();
    install_backings();
    install_sources();
    if (installed) return;
    Class type=objc_getClass("BWFigCaptureDeviceVendor");
    Method method=type ? class_getClassMethod(type,sel_registerName(selector_name)) : NULL;
    IMP current=method ? method_getImplementation(method) : NULL;
    const uintptr_t address=(uintptr_t)ptrauth_strip(current,ptrauth_key_function_pointer);
    if (!current || address!=(uintptr_t)(0x1b05ce8e4ULL+capture_slide)) {
        fprintf(stderr,"nezha-camera: measured vendor method unavailable\n");return;
    }
    original=current;
    method_setImplementation(method,(IMP)shared_vendor);
    installed=true;
    fprintf(stderr,"nezha-camera: 24A446 device factory installed\n");
}

__attribute__((constructor)) static void initialize(void)
{
    const char *process=getprogname();
    if (!process || strcmp(process,"cameracaptured")) return;
    _dyld_register_func_for_add_image(image_added);
}
