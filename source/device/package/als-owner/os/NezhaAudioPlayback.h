/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_NATIVE_PLAYBACK_H
#define NEZHA_AUDIO_NATIVE_PLAYBACK_H
#include "NezhaAudioAdspInputs.h"
#include "NezhaAudioSmmu.h"
#include "NezhaAudioTasBoard.h"
#include "NezhaAudioMicBoard.h"
#include "../platform/AudioPlaybackInputs.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    NaTasSelection speaker;
    uint32_t iova[3],port[3]; /* output/VI/optional microphone; not physical addresses */
    NaPrmResource resources[NA_PRM_RESOURCES];
    uint32_t resource_count,output_votes,input_votes,timebase_numer,timebase_denom,capture_probe;
} NaNativePlaybackConfig;
typedef struct {
    NaNativeAdspInputs inputs;NaPlaybackInputs playback;NaCaptureInputs capture;
    NaNativeSmmu domain;NaNativeTasBoard board;
    NaNativeMicBoard microphone;
    NaNativeAdspWatchdog watchdog;
    NaNativeMemory dma[3],tables;
    NaNativeWindow smem,qmp,cmd_db;
    NaAssembly assembly;NaAssemblyPlan plan;
    uint32_t attempted,ready;int error;
} NaNativePlayback;
/* Complete the existing playback Assembly with GPU015 native owners. Does
 * not start its worker or install a boot mode/HAL. First use requires actual
 * TAS identity and reviewed PRM/format choices; this API supplies no defaults.
 * Caller supplies a fresh, zeroed, lifetime-stable object. Failed allocations
 * and leases remain visible in it, never reused by an implicit retry. */
int na_native_playback_prepare(NaNativePlayback *,const NaNativePlaybackConfig *);
/* Reviewed primary-TDM stereo route. The caller supplies BOTH observed TAS
 * revisions and the actual XNU timebase; no fabricated revision default. */
int na_native_speaker_config(NaNativePlaybackConfig *,const uint8_t revisions[2],
                              uint32_t timebase_numer,uint32_t timebase_denom);
/* Explicitly extend that speaker plan with the stock TX/VA clock/core votes
 * and a third, software-owned DMA address for the bounded capture probe.
 * The default speaker recipe and HAL input publication remain unchanged. */
int na_native_capture_config(NaNativePlaybackConfig *);
/* Explicit na=3 startup: discover both revisions while reset-muted, restore
 * the identity-only board lease, then initialize the existing playback
 * Assembly and start its sole native worker. No output is requested here. */
typedef struct {
    NaNativePlayback playback;
    NaNativeTasBoard identity_native;NaGeniBoard identity_board;NaTasIdentity identity;
    NaNativePlaybackConfig config;
    uint64_t deadline;
    uint32_t phase,capture_frames;
    int error;
} NaNativePlaybackStart;
int na_native_playback_start_step(NaNativePlaybackStart *,uint64_t now_ms,
                                  uint32_t timebase_numer,uint32_t timebase_denom);
void nezha_audio27_playback_start(void);
#ifdef __cplusplus
}
#endif
#endif
