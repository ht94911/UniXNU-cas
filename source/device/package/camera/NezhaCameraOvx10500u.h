/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_OVX10500U_H
#define NEZHA_CAMERA_OVX10500U_H

#include "NezhaCameraCci.h"
#include <stddef.h>

enum { NezhaCameraOvx10500uId = 0x5058 };

struct NezhaCameraOvxIdentity {
    uint8_t bytes[2];
    unsigned bytes_read;
    struct NezhaCameraCciObservation transfer;
};

struct NezhaCameraOvxModeWrite {
    uint16_t reg;
    uint8_t value;
    uint16_t delay_ms;
};

/* Use the powered, initialized and exclusively held nezha CCI0 master 1.
 * Obtain both identity bytes before matching the actual stock module. */
enum NezhaCameraCciStatus nezha_camera_ovx10500u_read_identity(
    const struct NezhaCameraRegisterIo *cci, struct NezhaCameraOvxIdentity *identity);

/* Execute the caller's verified, ordered pre-stream profile. Stream state
 * stays under the capture engine. The original trace is not a mode profile. */
enum NezhaCameraCciStatus nezha_camera_ovx10500u_apply_mode(
    const struct NezhaCameraRegisterIo *cci, const struct NezhaCameraOvxModeWrite *writes,
    size_t count, size_t *applied, struct NezhaCameraCciObservation *transfer);

/* Assert stream-on only after receiver, DMA and buffers are ready. */
enum NezhaCameraCciStatus nezha_camera_ovx10500u_set_stream(
    const struct NezhaCameraRegisterIo *cci, bool running,
    struct NezhaCameraCciObservation *transfer);

#endif
