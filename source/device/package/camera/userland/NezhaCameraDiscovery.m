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
#include <unistd.h>

typedef struct OpaqueFigCaptureSource *CameraSource;
static IMP original_new_sources,original_video_init,original_video_devices,original_filter,original_access;
static const struct NezhaFigDeviceVTable *(*source_vtable)(NezhaFigObject);
static FILE *observations;
static intptr_t av_slide;
static bool av_present,observers_installed;
static const uint8_t av_uuid[16]={0xf2,0x7f,0x14,0xdb,0xc4,0xd4,0x3d,0xff,
    0x85,0x2b,0x2f,0x19,0x61,0xc9,0x44,0x35};

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

static bool matches(const struct mach_header_64 *header)
{
    if (!header || header->magic!=MH_MAGIC_64 || header->ncmds>256 || header->sizeofcmds>65536) return false;
    const uint8_t *cursor=(const uint8_t *)(header+1),*end=cursor+header->sizeofcmds;
    for (unsigned i=0;i<header->ncmds;++i) {
        if ((size_t)(end-cursor)<sizeof(struct load_command)) return false;
        const struct load_command *command=(const void *)cursor;
        if (command->cmdsize<sizeof(*command) || command->cmdsize>(size_t)(end-cursor)) return false;
        if (command->cmd==LC_UUID)
            return command->cmdsize>=sizeof(struct uuid_command) &&
                !memcmp(((const struct uuid_command *)command)->uuid,av_uuid,sizeof(av_uuid));
        cursor+=command->cmdsize;
    }
    return false;
}

static void image_arrived(const struct mach_header *header,intptr_t slide)
{
    if (observers_installed) return;
    if (matches((const struct mach_header_64 *)header)) {av_present=true;av_slide=slide;}
    if (!av_present) return;
    Class video=objc_getClass("AVCaptureFigVideoDevice"),device=objc_getClass("AVCaptureDevice");
    struct {Class type;bool class_method;const char *selector,*encoding;uintptr_t address;IMP replacement;IMP *original;} rows[]={
        {video,true,"_newFigCaptureSources","@16@0:8",0x1ad9298c8ULL,(IMP)new_sources,&original_new_sources},
        {video,false,"_initWithFigCaptureSource:","@24@0:8^{OpaqueFigCaptureSource=}16",0x1ad865c18ULL,(IMP)video_init,&original_video_init},
        {video,true,"_devicesWithPriorRegisteredDevices:","@24@0:8@16",0x1ad92992cULL,(IMP)video_devices,&original_video_devices},
        {device,true,"_filterConnectedDevices:withDeviceTypes:mediaType:position:","v48@0:8@16@24@32q40",0x1ad8763c0ULL,(IMP)filter_devices,&original_filter},
        {video,false,"_cameraAccessIsEnabled","B16@0:8",0x1ad880f74ULL,(IMP)camera_access,&original_access},
    };
    const unsigned count=sizeof(rows)/sizeof(rows[0]);
    Method methods[5];
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
    int fd=open("/private/var/mobile/Library/Logs/nezha-camera-client-079.stderr",
        O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0644);
    if (fd<0) return;
    observations=fdopen(fd,"a");
    if (!observations) {close(fd);return;}
    setvbuf(observations,NULL,_IONBF,0);
    fprintf(observations,"nezha-camera-discovery: original Camera client pid=%u\n",(unsigned)getpid());
    nezha_camera_discovery_observe();
}
