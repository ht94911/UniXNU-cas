/* SPDX-License-Identifier: MIT */
/* Compile without ARC: the original _newFigCaptureSources returns a
 * caller-owned CFArray and the original _init consumes self and returns
 * an owned object. A plain IMP observer must preserve both conventions. */
#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include "NezhaCameraDiscovery.h"
#include "NezhaFigCapture24A446.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <ptrauth.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

typedef struct OpaqueFigCaptureSource *CameraSource;
static IMP original_new_sources,original_video_init,original_video_devices,original_filter,original_access,original_copy_property,original_connection_init;
static IMP original_input_init,original_session_start,original_session_commit,original_session_add,original_session_can_add;
static IMP original_capabilities_init,original_preferred_device,original_engine_init,original_engine_device,original_engine_input,original_engine_start,original_capture_available;
static IMP original_capabilities_get;
static const struct NezhaFigDeviceVTable *(*source_vtable)(NezhaFigObject);
static FILE *observations;
static intptr_t av_slide,camera_ui_slide;
static bool av_present,observers_installed,connection_observer_installed,session_observers_installed;
static bool camera_ui_present,camera_ui_observers_installed,original_camera_client;
static const uint8_t av_uuid[16]={0xf2,0x7f,0x14,0xdb,0xc4,0xd4,0x3d,0xff,
    0x85,0x2b,0x2f,0x19,0x61,0xc9,0x44,0x35};
static const uint8_t camera_ui_uuid[16]={0x91,0x02,0xbd,0xfc,0xac,0xcf,0x3a,0xcb,
    0xa9,0x10,0x6f,0xa1,0x39,0xd0,0xa5,0x5d};

/* The original Camera080 constructs its real main device but never starts
 * our graph. Observe its next native boundaries, without selecting devices,
 * adding inputs, calling Start ourselves, or changing completion blocks. */
static void report_device(const char *stage,id device)
{
    NSString *identifier=device ? ((id (*)(id,SEL))objc_msgSend)
        (device,sel_registerName("uniqueID")):nil;
    fprintf(observations,"nezha-camera-discovery: %s device=%u id=%s\n",stage,
        (unsigned)(device!=nil),identifier.UTF8String ?: "<absent>");
}

static id input_init(id self,SEL selector,id device,NSError **error)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<8;
    if (report) report_device("original input init entered",device);
    id input=((id (*)(id,SEL,id,NSError **))original_input_init)(self,selector,device,error);
    if (report) fprintf(observations,"nezha-camera-discovery: original input init returned input=%u error=%ld domain=%s\n",
        (unsigned)(input!=nil),(long)(error ? (*error).code:0),
        error && *error ? (*error).domain.UTF8String : "<none>");
    /* Native init consumes self. Preserve its owned result and NSError*. */
    return input;
}

static void session_start(id self,SEL selector)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<8;
    if (report) fputs("nezha-camera-discovery: original session start entered\n",observations);
    ((void (*)(id,SEL))original_session_start)(self,selector);
    if (report) fprintf(observations,"nezha-camera-discovery: original session start returned running=%u interrupted=%u\n",
        (unsigned)((BOOL (*)(id,SEL))objc_msgSend)(self,sel_registerName("isRunning")),
        (unsigned)((BOOL (*)(id,SEL))objc_msgSend)(self,sel_registerName("isInterrupted")));
}

static void session_commit(id self,SEL selector)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<8;
    if (report) fputs("nezha-camera-discovery: original session commit entered\n",observations);
    ((void (*)(id,SEL))original_session_commit)(self,selector);
    if (report) fputs("nezha-camera-discovery: original session commit returned\n",observations);
}

static void session_add(id self,SEL selector,id input)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<8;
    if (report) fprintf(observations,"nezha-camera-discovery: original session add entered input=%u\n",(unsigned)(input!=nil));
    ((void (*)(id,SEL,id))original_session_add)(self,selector,input);
    if (report) fputs("nezha-camera-discovery: original session add returned\n",observations);
}

static BOOL session_can_add(id self,SEL selector,id input)
{
    const BOOL allowed=((BOOL (*)(id,SEL,id))original_session_can_add)(self,selector,input);
    static unsigned reported;
    if (__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<8)
        fprintf(observations,"nezha-camera-discovery: original session canAddInput input=%u allowed=%u\n",
            (unsigned)(input!=nil),(unsigned)allowed);
    return allowed;
}

static id capabilities_init(id self,SEL selector,long host)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4;
    if (report) fprintf(observations,"nezha-camera-discovery: original capabilities init entered host=%ld\n",host);
    id capabilities=((id (*)(id,SEL,long))original_capabilities_init)(self,selector,host);
    if (report) fprintf(observations,"nezha-camera-discovery: original capabilities init returned object=%u back=%u front=%u multiCam=%u\n",
        (unsigned)(capabilities!=nil),
        (unsigned)((BOOL (*)(id,SEL))objc_msgSend)(capabilities,sel_registerName("isBackCameraSupported")),
        (unsigned)((BOOL (*)(id,SEL))objc_msgSend)(capabilities,sel_registerName("isFrontCameraSupported")),
        (unsigned)((BOOL (*)(id,SEL))objc_msgSend)(capabilities,sel_registerName("useMultiCamSession")));
    return capabilities;
}

static id capabilities_get(id self,SEL selector)
{
    id capabilities=((id (*)(id,SEL))original_capabilities_get)(self,selector);
    static unsigned reported;
    if (__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4) {
        fprintf(observations,"nezha-camera-discovery: original capabilities object=%u back=%u front=%u multiCam=%u\n",
            (unsigned)(capabilities!=nil),
            (unsigned)((BOOL (*)(id,SEL))objc_msgSend)(capabilities,sel_registerName("isBackCameraSupported")),
            (unsigned)((BOOL (*)(id,SEL))objc_msgSend)(capabilities,sel_registerName("isFrontCameraSupported")),
            (unsigned)((BOOL (*)(id,SEL))objc_msgSend)(capabilities,sel_registerName("useMultiCamSession")));
        char target[64]={0};size_t length=sizeof(target);
        const int target_status=sysctlbyname("hw.target",target,&length,NULL,0);
        id (*model)(void)=(id (*)(void))dlsym(RTLD_DEFAULT,"FigCaptureGetModelSpecificName");
        id name=model ? model():nil;
        fprintf(observations,"nezha-camera-discovery: original camera model target_status=%d target=%.63s captureModel=%s\n",
            target_status,target,[name UTF8String] ?: "<absent>");
    }
    return capabilities;
}

static long preferred_device(id self,SEL selector,long position,long mode,long configuration,long strength,BOOL simultaneous)
{
    const long selected=((long (*)(id,SEL,long,long,long,long,BOOL))original_preferred_device)
        (self,selector,position,mode,configuration,strength,simultaneous);
    static unsigned reported;
    if (__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<16)
        fprintf(observations,"nezha-camera-discovery: original preferred device position=%ld mode=%ld configuration=%ld strength=%ld simultaneous=%u selected=%ld\n",
            position,mode,configuration,strength,(unsigned)simultaneous,selected);
    return selected;
}

static id engine_init(id self,SEL selector,id power,id controller,long options)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4;
    if (report) fprintf(observations,"nezha-camera-discovery: original engine init entered options=%ld\n",options);
    id engine=((id (*)(id,SEL,id,id,long))original_engine_init)(self,selector,power,controller,options);
    if (report) fprintf(observations,"nezha-camera-discovery: original engine init returned object=%u\n",(unsigned)(engine!=nil));
    return engine;
}

static id engine_device(id self,SEL selector,NSString *type,long position)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<16;
    if (report) fprintf(observations,"nezha-camera-discovery: original engine device entered type=%s position=%ld\n",type.UTF8String ?: "<absent>",position);
    id device=((id (*)(id,SEL,id,long))original_engine_device)(self,selector,type,position);
    if (report) fprintf(observations,"nezha-camera-discovery: original engine device returned object=%u\n",(unsigned)(device!=nil));
    return device;
}

static id engine_input(id self,SEL selector)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<8;
    if (report) fputs("nezha-camera-discovery: original engine input entered\n",observations);
    id input=((id (*)(id,SEL))original_engine_input)(self,selector);
    if (report) fprintf(observations,"nezha-camera-discovery: original engine input returned input=%u\n",(unsigned)(input!=nil));
    return input;
}

static void engine_start(id self,SEL selector,unsigned long retries,double interval,id reason,id completion)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<8;
    if (report) fprintf(observations,"nezha-camera-discovery: original engine start entered retries=%lu interval=%.3f completion=%u\n",retries,interval,(unsigned)(completion!=nil));
    ((void (*)(id,SEL,unsigned long,double,id,id))original_engine_start)(self,selector,retries,interval,reason,completion);
    if (report) fputs("nezha-camera-discovery: original engine start request returned\n",observations);
}

static void capture_available(id self,SEL selector,BOOL available)
{
    static unsigned reported;
    if (__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<8)
        fprintf(observations,"nezha-camera-discovery: original capture available=%u\n",(unsigned)available);
    ((void (*)(id,SEL,BOOL))original_capture_available)(self,selector,available);
}

static id source_property(CameraSource source,CFStringRef key,int32_t *error)
{
    CFTypeRef value=NULL;
    const struct NezhaFigDeviceVTable *table=source && source_vtable ?
        source_vtable((NezhaFigObject)source):NULL;
    *error=table && table->base && table->base->copy_property ?
        table->base->copy_property((NezhaFigObject)source,key,NULL,&value):-12782;
    return (id)value;
}

static void report_source(CameraSource source,unsigned index)
{
    int32_t attributes_error=0,formats_error=0,connected_error=0;
    id attributes=source_property(source,CFSTR("Attributes"),&attributes_error);
    id formats=source_property(source,CFSTR("Formats"),&formats_error);
    id connected=source_property(source,CFSTR("Connected"),&connected_error);
    NSString *identifier=attributes ? ((id (*)(id,SEL))objc_msgSend)
        (attributes,sel_registerName("uniqueID")):nil;
    int32_t type=attributes ? ((int32_t (*)(id,SEL))objc_msgSend)
        (attributes,sel_registerName("deviceType")):0;
    int32_t position=attributes ? ((int32_t (*)(id,SEL))objc_msgSend)
        (attributes,sel_registerName("position")):0;
    fprintf(observations,"nezha-camera-discovery: remote source index=%u object=%u attributes=%u error=%d type=%d position=%d id=%s formats=%lu error=%d connected=%u error=%d\n",
        index,(unsigned)(source!=NULL),(unsigned)(attributes!=nil),attributes_error,type,position,
        identifier.UTF8String ?: "<absent>",(unsigned long)[formats count],formats_error,
        (unsigned)[connected boolValue],connected_error);
    [connected release];[formats release];[attributes release];
}

static CFArrayRef new_sources(id self,SEL selector)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4;
    if (report) fputs("nezha-camera-discovery: original remote query entered\n",observations);
    CFArrayRef sources=((CFArrayRef (*)(id,SEL))original_new_sources)(self,selector);
    if (report) {
        const CFIndex count=sources ? CFArrayGetCount(sources):0;
        fprintf(observations,"nezha-camera-discovery: original remote query returned array=%u count=%ld\n",
            (unsigned)(sources!=NULL),count);
        for (CFIndex i=0;i<count && i<4;++i)
            report_source((CameraSource)CFArrayGetValueAtIndex(sources,i),(unsigned)i);
    }
    return sources;
}

static id video_init(id self,SEL selector,CameraSource source)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4;
    if (report) fprintf(observations,"nezha-camera-discovery: original video init entered source=%u\n",
        (unsigned)(source!=NULL));
    id device=((id (*)(id,SEL,CameraSource))original_video_init)(self,selector,source);
    /* The initializer may have released self. Read only its returned object. */
    if (report) fprintf(observations,"nezha-camera-discovery: original video init returned device=%u\n",
        (unsigned)(device!=nil));
    return device;
}

static id copy_property(id self,SEL selector,CFStringRef key)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<32;
    const char *name=[(id)key UTF8String] ?: "<absent>";
    if (report) fprintf(observations,"nezha-camera-discovery: original property entered key=%s\n",name);
    id value=((id (*)(id,SEL,CFStringRef))original_copy_property)(self,selector,key);
    if (report) {
        fprintf(observations,"nezha-camera-discovery: original property returned key=%s value=%u\n",
            name,(unsigned)(value!=nil));
        if (value && (CFEqual(key,CFSTR("Hidden")) || CFEqual(key,CFSTR("ReadyToUnhide"))))
            fprintf(observations,"nezha-camera-discovery: original visibility property key=%s boolean=%u\n",
                name,(unsigned)[value boolValue]);
    }
    /* _copy returns an owned object. Return it untouched to the native caller. */
    return value;
}

static id connection_init(id self,SEL selector,NSString *name,unsigned long options)
{
    static unsigned reported;
    const bool report=[name hasPrefix:@"com.apple.lsd."] &&
        __atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<12;
    if (report) fprintf(observations,"nezha-camera-discovery: original LS connection entered name=%s options=%lu uid=%u\n",
        name.UTF8String,options,(unsigned)geteuid());
    id connection=((id (*)(id,SEL,id,unsigned long))original_connection_init)(self,selector,name,options);
    if (report) fprintf(observations,"nezha-camera-discovery: original LS connection returned name=%s object=%u\n",
        name.UTF8String,(unsigned)(connection!=nil));
    return connection;
}

static id video_devices(id self,SEL selector,id prior)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4;
    if (report) fputs("nezha-camera-discovery: original video device construction entered\n",observations);
    id devices=((id (*)(id,SEL,id))original_video_devices)(self,selector,prior);
    if (report) fprintf(observations,"nezha-camera-discovery: original video device construction returned count=%lu\n",
        (unsigned long)[devices count]);
    return devices;
}

static BOOL camera_access(id self,SEL selector)
{
    const BOOL allowed=((BOOL (*)(id,SEL))original_access)(self,selector);
    static unsigned reported;
    if (__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<8)
        fprintf(observations,"nezha-camera-discovery: original camera access enabled=%u\n",(unsigned)allowed);
    return allowed;
}

static void filter_devices(id self,SEL selector,id devices,id types,id media,long position)
{
    static unsigned reported;
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<8;
    if (report) {
        fprintf(observations,"nezha-camera-discovery: original filter entered input=%lu types=%lu position=%ld\n",
            (unsigned long)[devices count],(unsigned long)[types count],position);
        for (id device in devices) {
            NSString *identifier=((id (*)(id,SEL))objc_msgSend)(device,sel_registerName("uniqueID"));
            NSString *type=((id (*)(id,SEL))objc_msgSend)(device,sel_registerName("deviceType"));
            BOOL connected=((BOOL (*)(id,SEL))objc_msgSend)(device,sel_registerName("isConnected"));
            fprintf(observations,"nezha-camera-discovery: original filter device id=%s type=%s connected=%u\n",
                identifier.UTF8String ?: "<absent>",type.UTF8String ?: "<absent>",(unsigned)connected);
        }
    }
    /* v48@0:8@16@24@32q40: this filters the mutable input and returns void. */
    ((void (*)(id,SEL,id,id,id,long))original_filter)(self,selector,devices,types,media,position);
    if (report) fprintf(observations,"nezha-camera-discovery: original filter returned output=%lu\n",
        (unsigned long)[devices count]);
}

static bool matches_uuid(const struct mach_header_64 *header,const uint8_t expected[16])
{
    if (!header || header->magic!=MH_MAGIC_64 || header->ncmds>256 || header->sizeofcmds>65536) return false;
    const uint8_t *cursor=(const uint8_t *)(header+1),*end=cursor+header->sizeofcmds;
    for (unsigned i=0;i<header->ncmds;++i) {
        if ((size_t)(end-cursor)<sizeof(struct load_command)) return false;
        const struct load_command *command=(const void *)cursor;
        if (command->cmdsize<sizeof(*command) || command->cmdsize>(size_t)(end-cursor)) return false;
        if (command->cmd==LC_UUID)
            return command->cmdsize>=sizeof(struct uuid_command) &&
                !memcmp(((const struct uuid_command *)command)->uuid,expected,16);
        cursor+=command->cmdsize;
    }
    return false;
}

static void install_session_observers(void)
{
    if (!original_camera_client || !av_present || session_observers_installed) return;
    Class input=objc_getClass("AVCaptureDeviceInput"),session=objc_getClass("AVCaptureSession");
    struct {Class type;const char *selector,*encoding;uintptr_t address;IMP replacement;IMP *original;} rows[]={
        {input,"initWithDevice:error:","@32@0:8@16^@24",0x1ad8f580cULL,(IMP)input_init,&original_input_init},
        {session,"startRunning","v16@0:8",0x1ad8c62ecULL,(IMP)session_start,&original_session_start},
        {session,"commitConfiguration","v16@0:8",0x1ad85f188ULL,(IMP)session_commit,&original_session_commit},
        {session,"addInput:","v24@0:8@16",0x1ad883104ULL,(IMP)session_add,&original_session_add},
        {session,"canAddInput:","B24@0:8@16",0x1ad882b1cULL,(IMP)session_can_add,&original_session_can_add},
    };
    Method methods[5];
    for (unsigned i=0;i<5;++i) {
        methods[i]=rows[i].type ? class_getInstanceMethod(rows[i].type,sel_registerName(rows[i].selector)):NULL;
        IMP current=methods[i] ? method_getImplementation(methods[i]):NULL;
        const char *encoding=methods[i] ? method_getTypeEncoding(methods[i]):NULL;
        if (!current || !encoding || strcmp(encoding,rows[i].encoding) ||
            (uintptr_t)ptrauth_strip(current,ptrauth_key_function_pointer)!=(uintptr_t)(rows[i].address+av_slide)) return;
    }
    for (unsigned i=0;i<5;++i) {
        *rows[i].original=method_getImplementation(methods[i]);
        method_setImplementation(methods[i],rows[i].replacement);
    }
    session_observers_installed=true;
    fputs("nezha-camera-discovery: original input/session observers installed\n",observations);
}

static void install_camera_ui_observers(void)
{
    if (!original_camera_client || !camera_ui_present || camera_ui_observers_installed) return;
    Class capabilities=objc_getClass("CAMCaptureCapabilities"),engine=objc_getClass("CAMCaptureEngine");
    Class device=objc_getClass("CAMCaptureEngineDevice"),controller=objc_getClass("CUCaptureController");
    struct {Class type;const char *selector,*encoding;uintptr_t address;IMP replacement;IMP *original;} rows[]={
        {capabilities,"initWithHostProcess:","@24@0:8q16",0x1bea06828ULL,(IMP)capabilities_init,&original_capabilities_init},
        {capabilities,"preferredDeviceForPosition:mode:videoConfiguration:videoStabilizationStrength:frontRearSimultaneousVideoEnabled:",
            "q52@0:8q16q24q32q40B48",0x1bec40894ULL,(IMP)preferred_device,&original_preferred_device},
        {engine,"initWithPowerController:captureController:options:","@40@0:8@16@24q32",0x1bec8bfc0ULL,(IMP)engine_init,&original_engine_init},
        {engine,"_captureEngineDeviceForDeviceType:position:","@32@0:8@16q24",0x1bea4f794ULL,(IMP)engine_device,&original_engine_device},
        {device,"captureDeviceInput","@16@0:8",0x1beb2c014ULL,(IMP)engine_input,&original_engine_input},
        {engine,"startWithRetryCount:retryInterval:logReason:completion:","v48@0:8Q16d24@32@?40",0x1bea2a0fcULL,(IMP)engine_start,&original_engine_start},
        {controller,"_setCaptureAvailable:","v20@0:8B16",0x1beac0520ULL,(IMP)capture_available,&original_capture_available},
    };
    Method methods[7];
    Method getter=capabilities ? class_getClassMethod(capabilities,sel_registerName("capabilities")):NULL;
    IMP get=getter ? method_getImplementation(getter):NULL;
    const char *getter_encoding=getter ? method_getTypeEncoding(getter):NULL;
    if (!get || !getter_encoding || strcmp(getter_encoding,"@16@0:8") ||
        (uintptr_t)ptrauth_strip(get,ptrauth_key_function_pointer)!=(uintptr_t)(0x1bea066c4ULL+camera_ui_slide)) return;
    for (unsigned i=0;i<7;++i) {
        methods[i]=rows[i].type ? class_getInstanceMethod(rows[i].type,sel_registerName(rows[i].selector)):NULL;
        IMP current=methods[i] ? method_getImplementation(methods[i]):NULL;
        const char *encoding=methods[i] ? method_getTypeEncoding(methods[i]):NULL;
        if (!current || !encoding || strcmp(encoding,rows[i].encoding) ||
            (uintptr_t)ptrauth_strip(current,ptrauth_key_function_pointer)!=(uintptr_t)(rows[i].address+camera_ui_slide)) return;
    }
    original_capabilities_get=get;method_setImplementation(getter,(IMP)capabilities_get);
    for (unsigned i=0;i<7;++i) {
        *rows[i].original=method_getImplementation(methods[i]);
        method_setImplementation(methods[i],rows[i].replacement);
    }
    camera_ui_observers_installed=true;
    fputs("nezha-camera-discovery: original CameraUI selection/engine observers installed\n",observations);
}

static void image_arrived(const struct mach_header *header,intptr_t slide)
{
    if (observers_installed && connection_observer_installed &&
        (!original_camera_client || (session_observers_installed && camera_ui_observers_installed))) return;
    if (matches_uuid((const struct mach_header_64 *)header,av_uuid)) {av_present=true;av_slide=slide;}
    if (matches_uuid((const struct mach_header_64 *)header,camera_ui_uuid)) {camera_ui_present=true;camera_ui_slide=slide;}
    install_camera_ui_observers();
    install_session_observers();
    if (!av_present) return;
    if (!connection_observer_installed) {
        Class connection=objc_getClass("NSXPCConnection");
        Method method=connection ? class_getInstanceMethod(connection,
            sel_registerName("initWithMachServiceName:options:")):NULL;
        IMP initializer=method ? method_getImplementation(method):NULL;
        const char *encoding=method ? method_getTypeEncoding(method):NULL;
        if (initializer && encoding && !strcmp(encoding,"@32@0:8@16Q24") &&
            (uintptr_t)ptrauth_strip(initializer,ptrauth_key_function_pointer)==(uintptr_t)(0x180b9d3f4ULL+av_slide)) {
            original_connection_init=initializer;method_setImplementation(method,(IMP)connection_init);
            connection_observer_installed=true;
            fputs("nezha-camera-discovery: original LS connection observer installed\n",observations);
        }
    }
    if (observers_installed) return;
    Class video=objc_getClass("AVCaptureFigVideoDevice"),device=objc_getClass("AVCaptureDevice");
    struct {Class type;bool class_method;const char *selector,*encoding;uintptr_t address;IMP replacement;IMP *original;} rows[]={
        {video,true,"_newFigCaptureSources","@16@0:8",0x1ad9298c8ULL,(IMP)new_sources,&original_new_sources},
        {video,false,"_initWithFigCaptureSource:","@24@0:8^{OpaqueFigCaptureSource=}16",0x1ad865c18ULL,(IMP)video_init,&original_video_init},
        {video,true,"_devicesWithPriorRegisteredDevices:","@24@0:8@16",0x1ad92992cULL,(IMP)video_devices,&original_video_devices},
        {device,true,"_filterConnectedDevices:withDeviceTypes:mediaType:position:","v48@0:8@16@24@32q40",0x1ad8763c0ULL,(IMP)filter_devices,&original_filter},
        {video,false,"_cameraAccessIsEnabled","B16@0:8",0x1ad880f74ULL,(IMP)camera_access,&original_access},
        {video,false,"_copyFigCaptureSourceProperty:","@24@0:8^{__CFString=}16",0x1ad882a3cULL,(IMP)copy_property,&original_copy_property},
    };
    const unsigned count=sizeof(rows)/sizeof(rows[0]);
    Method methods[6];
    for (unsigned i=0;i<count;++i) {
        SEL selector=sel_registerName(rows[i].selector);
        methods[i]=rows[i].type ? (rows[i].class_method ? class_getClassMethod(rows[i].type,selector):
            class_getInstanceMethod(rows[i].type,selector)):NULL;
        IMP original=methods[i] ? method_getImplementation(methods[i]):NULL;
        const char *encoding=methods[i] ? method_getTypeEncoding(methods[i]):NULL;
        if (!original || !encoding || strcmp(encoding,rows[i].encoding) ||
            (uintptr_t)ptrauth_strip(original,ptrauth_key_function_pointer)!=(uintptr_t)(rows[i].address+av_slide)) return;
    }
    void *core=dlopen("/System/Library/Frameworks/CoreMedia.framework/CoreMedia",RTLD_NOW|RTLD_LOCAL);
    source_vtable=core ? (const struct NezhaFigDeviceVTable *(*)(NezhaFigObject))dlsym(core,"CMBaseObjectGetVTable"):NULL;
    if (!source_vtable) return;
    for (unsigned i=0;i<count;++i) {
        *rows[i].original=method_getImplementation(methods[i]);
        method_setImplementation(methods[i],rows[i].replacement);
    }
    observers_installed=true;
    fputs("nezha-camera-discovery: original AVFCapture source/init/filter observers installed\n",observations);
}

void nezha_camera_discovery_observe(void)
{
    /* The launcher already owns a launchd stderr file. */
    if (!observations) {
        int fd=dup(STDERR_FILENO);
        if (fd>=0) {
            observations=fdopen(fd,"a");
            if (!observations) close(fd);
        }
    }
    if (!observations) return;
    setvbuf(observations,NULL,_IONBF,0);
    _dyld_register_func_for_add_image(image_arrived);
}

__attribute__((constructor)) static void initialize_camera_client(void)
{
    const char *name=getprogname();
    /* The kernel separately checks the original Camera CDHash, parent,
     * signature and explicit camera mode6 before permitting insertion. */
    if (!name || strcmp(name,"Camera") || geteuid()!=501) return;
    /* The original Camera's MobileSlideShow profile already permits /tmp.
     * Actual079 denied creation in mobile/Library/Logs before installation. */
    int fd=open("/tmp/nezha-camera-client-081.stderr",
        O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0644);
    if (fd<0) return;
    observations=fdopen(fd,"a");
    if (!observations) {close(fd);return;}
    setvbuf(observations,NULL,_IONBF,0);
    original_camera_client=true;
    fprintf(observations,"nezha-camera-discovery: original Camera client pid=%u\n",(unsigned)getpid());
    nezha_camera_discovery_observe();
}
