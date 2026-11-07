/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_TI_CALIBRATION_H
#define NEZHA_TI_CALIBRATION_H
#include "Tas25xxTables.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {const uint8_t *data;uint32_t bytes;} NaCalibrationFile;
typedef struct {uint32_t r0_q19[2],temperature[2],iv_width,vbat,driver_mode,valid;} NaTiCalibration;
/* Consume the owner's frozen copies of ti_calr.bin, calib_re(_r), amb_temp(_r).
 * No file I/O or calibration trigger here. Binary IEEE754 ->Q19 uses integer
 * operations; ASCII cross-check detects mismatched generations/order. */
int na_ti_calibration_parse(NaTiCalibration *,NaCalibrationFile binary,
                             const NaCalibrationFile resistance[2],const NaCalibrationFile temperature[2],
                             const NaTasImage *,const char *profile);
/* Public AudioReach TI SmartPA ABI (TIPayloadBuilder.h): SET_CALIB_DATA3859,
 * IV_WIDTH_VBAT_MON3840, SET_DRV_OP_MODE3854. Target must resolve the actual TI
 * module tag in its graph and match this ABI; no guessed fallback IID. Both
 * graph config acknowledgement and a running VI graph precede amp activation. */
int na_ti_calibration_payload(const NaTiCalibration *,uint32_t instance,uint8_t out[88],uint32_t *bytes);
#ifdef __cplusplus
}
#endif
#endif
