/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_AF_CALIBRATION_H
#define NEZHA_CAMERA_AF_CALIBRATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NezhaCameraAfSteps = 900 };

struct NezhaCameraAfCalibration {
    int16_t initial_dac;
    uint16_t minimum_dac, maximum_dac;
    int16_t dac[NezhaCameraAfSteps];
};

/* Original nezha CamX InitializeStepTable uses the EEPROM macro/infinity
 * DACs, +10% macro/-10% infinity margins, 12-bit VCM bounds and900 steps.
 * Step899 is infinity; step0 is macro. Preserve its float accumulation
 * before int16_t truncation. This is a user-space calculation, not XNU
 * general-register code. Caller must validate its actual EEPROM first.
 * No generic/golden calibration is selected by this function. */
bool nezha_camera_af_calibrate(uint16_t infinity_dac, uint16_t macro_dac,
    struct NezhaCameraAfCalibration *out);

/* Original OFILM and SEMCO EEPROM plugins require AF flag27=1 and the
 * sum of bytes28..42 modulo255 plus1 at43. Decode big-endian macro2d
 * and infinity31 only after that actual input passes. No golden fallback. */
bool nezha_camera_af_calibrate_eeprom(const uint8_t *bytes, size_t length,
    struct NezhaCameraAfCalibration *out);

#ifdef __cplusplus
}
#endif
#endif
