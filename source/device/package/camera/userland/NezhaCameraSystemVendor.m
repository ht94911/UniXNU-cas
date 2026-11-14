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

/* HakiOS inject_media.c supplies the image-arrival method installation.
 * All method identity and ABI data here belong to the measured 24A446
 * CMCapture UUID. The factory signature and current default initializer
 * were checked in that image, not inherited from its iOS16 shim. */
static bool installed;
static bool capture_present;
static intptr_t capture_slide;
static IMP original;
static const char selector_name[]="sharedCaptureDeviceVendorWithDefaultDeviceCreateFunction:";
static const uint8_t capture_uuid[16]={0x99,0x5f,0xc2,0x2b,0x41,0xbd,0x37,0xb4,
    0x81,0x2f,0x26,0x86,0xb8,0x1c,0x26,0x94};
typedef NezhaFigStatus (*CameraCreate)(NezhaFigAllocator,NezhaFigDictionary,NezhaFigObject *);

static id shared_vendor(id self,SEL selector,CameraCreate factory)
{
    if (nezha_camera_device_backend_bind()==0)
        factory=nezha_camera_device_create;
    return ((id (*)(id,SEL,CameraCreate))original)(self,selector,factory);
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
    if (installed) return;
    if (uuid_matches((const struct mach_header_64 *)header)) {
        capture_present=true;capture_slide=slide;
    }
    if (!capture_present) return;
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
