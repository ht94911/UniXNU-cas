/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_NATIVE_DEVICE_H
#define NEZHA_CAMERA_NATIVE_DEVICE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Called after the board's storage, USB and touch prerequisites. Keep the
 * validated profile and command database alive for this device's lifetime.
 * Publishing the mobile-owned node starts no camera hardware. */
bool nezha_camera_native_device_init(const uint8_t *profile,size_t profile_bytes,
    const uint8_t *database,size_t database_bytes,bool AF_readonly_probe);

#ifdef __cplusplus
}
#endif
#endif
