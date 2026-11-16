/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_AK7316_H
#define NEZHA_CAMERA_AK7316_H

#include "NezhaCameraCci.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { NezhaCameraAk7316MaxDac = 4095 };

struct NezhaCameraAk7316Result {
    struct NezhaCameraCciObservation transfer;
    unsigned completed_writes, completed_reads;
    bool position_read;
    uint16_t position_dac, register_value;
    uint8_t control;
};

/* Four original nezha OVX modules use the same AK7316 CCI0/master1/0x0c
 * tables. Their VAF and CCI ownership must already belong to the caller.
 * No GPIO, rail, MCLK or controller recovery is performed by this helper.
 * These operations are not enabled in a shipped camera candidate. */
enum NezhaCameraCciStatus nezha_camera_ak7316_initialize(
    const struct NezhaCameraRegisterIo *cci, struct NezhaCameraAk7316Result *result);

/* Report only the programmed DAC actually read from registers00/01.
 * Mechanical lens settling and per-frame focus remain separate. */
enum NezhaCameraCciStatus nezha_camera_ak7316_read_position(
    const struct NezhaCameraRegisterIo *cci, struct NezhaCameraAk7316Result *result);

/* Original CreateMoveFocusCmd writes DAC<<4 with one address byte and
 * two high-byte-first data bytes. Readback confirms the register value,
 * not focus quality. Caller provides an OEM-calibrated bounded target. */
enum NezhaCameraCciStatus nezha_camera_ak7316_set_position(
    const struct NezhaCameraRegisterIo *cci, uint16_t dac, struct NezhaCameraAk7316Result *result);

/* Original deInitSettings only. The owner must finish the separately
 * calibrated parking sequence before calling it or releasing VAF. */
enum NezhaCameraCciStatus nezha_camera_ak7316_deinitialize(
    const struct NezhaCameraRegisterIo *cci, struct NezhaCameraAk7316Result *result);

#ifdef __cplusplus
}
#endif
#endif
