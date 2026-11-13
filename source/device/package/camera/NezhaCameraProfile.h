/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_PROFILE_H
#define NEZHA_CAMERA_PROFILE_H

#include "NezhaCameraCsiphy24.h"
#include "NezhaCameraOvx10500u.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { NezhaCameraProfileCapacity = 128 * 1024, NezhaCameraProfileHeader = 192 };

struct NezhaCameraProfile {
    const uint8_t *bytes;
    size_t used_bytes, initialization_count, mode_count;
    uint32_t width, height;
    uint64_t data_rate;
    size_t alternate_initialization_count;
    uint8_t vendor, other_vendor, as1_version;
    struct NezhaCameraCsiphy24Profile phy;
};

/* View only: no allocation, register access or sensor writes. Local final
 * packaging supplies stock data after source-only CI. The chosen module
 * still requires a real hardware match before executing its initialization.
 * The buffer is immutable, 4-byte aligned and remains alive with the view. */
bool nezha_camera_profile_open(const uint8_t *bytes, size_t capacity,
    struct NezhaCameraProfile *profile);

/* Materialize the validated initialization followed by mode1 into caller
 * storage. Burst pointers refer to the immutable resource, never to kernel
 * addresses copied from a host. The capture engine retains stream-on. */
bool nezha_camera_profile_writes(const struct NezhaCameraProfile *profile,
    struct NezhaCameraOvxModeWrite *out, size_t capacity);

/* Paired resource. Require the measured vendor and a matching revision. */
bool nezha_camera_profile_revision_writes(const struct NezhaCameraProfile *profile,
    uint8_t vendor, uint8_t revision, struct NezhaCameraOvxModeWrite *out,
    size_t capacity, size_t *count);

extern const uint8_t nezha_camera_profile_slot[];
extern const uint64_t nezha_camera_profile_slot_size;

#ifdef __cplusplus
}
#endif
#endif
