/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_OVX10500U_H
#define NEZHA_CAMERA_OVX10500U_H

#include "NezhaCameraCci.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    const uint8_t *burst;
    size_t burst_bytes;
};

struct NezhaCameraOvxControls {
    uint32_t exposure_lines, frame_lines, gain_q8;
};

struct NezhaCameraOvxControlPlan {
    uint32_t exposure_lines, frame_lines;
    uint16_t analog_gain_q8, digital_gain_q10;
    struct NezhaCameraOvxModeWrite writes[14];
};

/* Pure mode1 control conversion. Addresses and byte order come from the
 * actual nezha HAL/sensor plugin; no bus or stream operation occurs here.
 * gain_q8 is 256 for 1x. This mode retains unity sensor digital gain and
 * the upstream piecewise analog quantization. Apply only under the same
 * sensor/CCI owner and after selecting this measured mode. The stock
 * mode1 group-hold commands bracket all eleven exposure/gain bytes. */
bool nezha_camera_ovx10500u_control_plan(const struct NezhaCameraOvxControls *controls,
    struct NezhaCameraOvxControlPlan *plan);

struct NezhaCameraOvxControlResult {
    struct NezhaCameraOvxControlPlan plan;
    struct NezhaCameraCciObservation transfer;
    size_t applied_writes;
    bool group_may_be_held;
};

/* Only the selected mode1 capture worker may apply this transaction.
 * Report the first CCI error without issuing a group release or retry;
 * the owner stops capture and recovers that master through its teardown. */
enum NezhaCameraCciStatus nezha_camera_ovx10500u_apply_controls(
    const struct NezhaCameraRegisterIo *cci, const struct NezhaCameraOvxControls *controls,
    struct NezhaCameraOvxControlResult *result);

struct NezhaCameraOvxControlReadback {
    uint32_t exposure_lines, frame_lines;
    uint16_t analog_gain_q8;
    uint32_t digital_gain_q10;
    uint8_t registers[11];
    unsigned bytes_read;
    struct NezhaCameraCciObservation transfer;
};

/* Read the original mode1 control registers under the same CCI owner.
 * Return only values actually read; an interrupted read is incomplete.
 * Current registers alone do not date the settings of a queued RAW frame. */
enum NezhaCameraCciStatus nezha_camera_ovx10500u_read_controls(
    const struct NezhaCameraRegisterIo *cci, struct NezhaCameraOvxControlReadback *result);

/* Use the powered, initialized and exclusively held nezha CCI0 master 1.
 * Obtain both identity bytes before matching the actual stock module. */
enum NezhaCameraCciStatus nezha_camera_ovx10500u_read_identity(
    const struct NezhaCameraRegisterIo *cci, struct NezhaCameraOvxIdentity *identity);

struct NezhaCameraOvxRevision {
    struct NezhaCameraCciObservation transfer;
    uint8_t vendor, version;
    bool vendor_read, version_read, stream_may_be_on;
};

/* Run the stock revision discriminator on an identified, exclusively
 * owned sensor in standby. CSI receiver and DMA are still stopped.
 * stream_may_be_on requires recovery/stream-off after a failed command. */
enum NezhaCameraCciStatus nezha_camera_ovx10500u_read_revision(
    const struct NezhaCameraRegisterIo *cci, struct NezhaCameraOvxRevision *revision);

/* Execute the caller's verified, ordered pre-stream profile. Stream state
 * stays under the capture engine. The original trace is not a mode profile. */
enum NezhaCameraCciStatus nezha_camera_ovx10500u_apply_mode(
    const struct NezhaCameraRegisterIo *cci, const struct NezhaCameraOvxModeWrite *writes,
    size_t count, unsigned burst_queue_words, size_t *applied,
    struct NezhaCameraCciObservation *transfer);

/* Assert stream-on only after receiver, DMA and buffers are ready. */
enum NezhaCameraCciStatus nezha_camera_ovx10500u_set_stream(
    const struct NezhaCameraRegisterIo *cci, bool running,
    struct NezhaCameraCciObservation *transfer);

#ifdef __cplusplus
}
#endif
#endif
