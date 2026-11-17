/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_METAL_H
#define NEZHA_CAMERA_METAL_H
#include <objc/runtime.h>
#include <mach-o/loader.h>
#include <ptrauth.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* GPU051's exact signed plugin has only isRequired. Original24A446 Core
 * Image asks MTLFunctionConstant for required and terminates cameracaptured.
 * Retain the fixedGPU051 binary and alias its real getter in this process;
 * do not replace its value, allocation, shader or rendering implementation. */
static inline bool nezha_camera_metal_uuid(const struct mach_header_64 *header)
{
    static const uint8_t uuid[16]={0x7e,0x7b,0x76,0xab,0x1e,0xd6,0x3a,0x33,
        0x81,0x78,0x77,0x9b,0xba,0x5e,0x8d,0x71};
    if (!header || header->magic!=MH_MAGIC_64) return false;
    const uint8_t *at=(const uint8_t *)(header+1),*end=at+header->sizeofcmds;
    for (unsigned i=0;i<header->ncmds;++i) {
        if ((size_t)(end-at)<sizeof(struct load_command)) return false;
        const struct load_command *command=(const void *)at;
        if (command->cmdsize<sizeof(*command) || command->cmdsize>(size_t)(end-at)) return false;
        if (command->cmd==LC_UUID)
            return command->cmdsize>=sizeof(struct uuid_command) &&
                !memcmp(((const struct uuid_command *)command)->uuid,uuid,sizeof(uuid));
        at+=command->cmdsize;
    }
    return false;
}

static inline bool nezha_camera_metal_alias(Class type)
{
    if (!type) return false;
    SEL required=sel_registerName("required"),original=sel_registerName("isRequired");
    Method method=class_getInstanceMethod(type,original);
    if (!method || !method_getTypeEncoding(method) ||
        strcmp(method_getTypeEncoding(method),"B16@0:8")) return false;
    Method present=class_getInstanceMethod(type,required);
    if (present) return method_getImplementation(present)==method_getImplementation(method);
    return class_addMethod(type,required,method_getImplementation(method),method_getTypeEncoding(method));
}

static inline bool nezha_camera_metal_install(const struct mach_header *header,intptr_t slide)
{
    if (!nezha_camera_metal_uuid((const struct mach_header_64 *)header)) return false;
    Class type=objc_getClass("NBFunctionConstant");
    Method getter=type ? class_getInstanceMethod(type,sel_registerName("isRequired")):NULL;
    IMP implementation=getter ? method_getImplementation(getter):NULL;
    uintptr_t address=(uintptr_t)ptrauth_strip(implementation,ptrauth_key_function_pointer);
    static const uint8_t code[8]={0x00,0x20,0x40,0x39,0xc0,0x03,0x5f,0xd6};
    if (!implementation || address!=(uintptr_t)(0x22890ULL+slide) ||
        memcmp((const void *)address,code,sizeof(code))) return false;
    return nezha_camera_metal_alias(type);
}
#endif
