/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_PHOTO_CODEC_H
#define NEZHA_CAMERA_PHOTO_CODEC_H
#include <CoreFoundation/CoreFoundation.h>
#include <stddef.h>
#include <stdint.h>

/* HakiOS f128f1d inject_media.c registers the system's VCPHEVC encoder
 * alongside VideoToolbox's existing encoders. The selected24A446 exports
 * retain this three-argument contract; VCPHEVCRegisterEncoder is a RET.
 * The create function is passed through with its original authentication.
 * Its library handle must remain loaded for the registration's lifetime. */
typedef int32_t (*NezhaCameraRegisterEncoder)(uint32_t, CFDictionaryRef, void *);

static inline int32_t nezha_camera_photo_codec_register(
    NezhaCameraRegisterEncoder register_encoder, void *create,
    CFStringRef identifier_key, CFStringRef name_key, CFStringRef hardware_key)
{
    if (!register_encoder || !create || !identifier_key || !name_key || !hardware_key)
        return -12783;
    const void *keys[] = {identifier_key, name_key, hardware_key};
    const void *values[] = {
        CFSTR("com.apple.videotoolbox.videoencoder.hevc.vcp"),
        CFSTR("HEVC (VCPHEVC, software)"), kCFBooleanFalse,
    };
    CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!info) return -12782;
    const int32_t status = register_encoder(UINT32_C(0x68766331), info, create);
    CFRelease(info);
    return status;
}
#endif
