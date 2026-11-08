/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_NATIVE_PLAYBACK_H
#define NEZHA_AUDIO_NATIVE_PLAYBACK_H
#include "NezhaAudioAdspInputs.h"
#include "NezhaAudioSmmu.h"
#include "NezhaAudioTasBoard.h"
#include "../platform/AudioPlaybackInputs.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    NaTasSelection speaker;
    uint32_t iova[2],port[2]; /* explicit output/VI plan, never physical addresses */
    NaPrmResource resources[NA_PRM_RESOURCES];
    uint32_t resource_count,output_votes,timebase_numer,timebase_denom;
} NaNativePlaybackConfig;
typedef struct {
    NaNativeAdspInputs inputs;NaPlaybackInputs playback;
    NaNativeSmmu domain;NaNativeTasBoard board;
    NaNativeMemory dma[2],tables;
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
#ifdef __cplusplus
}
#endif
#endif
