/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_PLAYBACK_INPUTS_H
#define NEZHA_AUDIO_PLAYBACK_INPUTS_H
#include "AudioInputPack.h"
#include "AudioAssembly.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    NaTasImage tuning;NaTasSelection selection;NaTiCalibration calibration;
    NaFirmwarePiece graph[2];NaMediaEndpoints media[2]; /* speaker, DSP-only VI */
    uint32_t protection[2],valid;
} NaPlaybackInputs;
typedef struct {
    NaFirmwarePiece graph;NaMediaEndpoints media;uint32_t valid;
} NaCaptureInputs;
/* Bind the existing sealed PANDFW01 input set to the existing Assembly.
 * selection must carry exact, independently observed revisions (mask=0xff),
 * reviewed profile/format names and conservative gain indices. calibrated is
 * derived from the retained binary/ASCII factory records, never trusted.
 * No allocation, MMIO, DSP request, microphone, HAL publication or playback.
 * Keep the pack, selection strings and this object immutable and alive until
 * the complete assembly is retired. An already valid object cannot be reused. */
int na_playback_inputs(NaPlaybackInputs *,const NaInputPack *,const NaTasSelection *);
/* Fill only immutable playback/feedback inputs of a fresh plan; resources,
 * DMA, votes and shared maps remain the native owner's responsibility. */
int na_playback_plan(NaAssemblyPlan *,const NaPlaybackInputs *);
/* Resolve only this pack's mono capture.graph. Attach it for the existing
 * bounded probe; this never marks capture proven or publishes HAL input. */
int na_capture_inputs(NaCaptureInputs *,const NaInputPack *);
int na_capture_plan(NaAssemblyPlan *,const NaCaptureInputs *);
#ifdef __cplusplus
}
#endif
#endif
