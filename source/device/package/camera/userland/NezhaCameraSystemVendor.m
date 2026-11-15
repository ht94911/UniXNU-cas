/* SPDX-License-Identifier: MIT */
#include "NezhaCameraDeviceBackend.h"
#include "NezhaCameraFactory.h"
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <ptrauth.h>
#include <stdint.h>
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
static intptr_t capture_slide;
static IMP original;
static IMP original_model_resource;
static const char selector_name[]="sharedCaptureDeviceVendorWithDefaultDeviceCreateFunction:";
static const char model_selector_name[]="URLForResource:withExtension:subdirectory:";
static const uint8_t capture_uuid[16]={0x99,0x5f,0xc2,0x2b,0x41,0xbd,0x37,0xb4,
    0x81,0x2f,0x26,0x86,0xb8,0x1c,0x26,0x94};
typedef NezhaFigStatus (*CameraCreate)(NezhaFigAllocator,NezhaFigDictionary,NezhaFigObject *);

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
    if (__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4)
        fprintf(stderr,"nezha-camera: vendor request backend=%d deviceVendor=%u\n",status,(unsigned)(vendor!=NULL));
    return vendor;
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
    if (installed && model_installed) return;
    if (uuid_matches((const struct mach_header_64 *)header)) {
        capture_present=true;capture_slide=slide;
    }
    if (!capture_present) return;
    install_model_resource();
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
