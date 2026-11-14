/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_NATIVE_TAS_BOARD_H
#define NEZHA_AUDIO_NATIVE_TAS_BOARD_H
#include "NezhaAudioNative.h"
#include "../platform/NezhaAudioBoard.h"
#include <Sm8850Resources.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    Sm8850Resources *common;NaNativeWindow geni,tlmm;uint64_t lease,trace_wait_us;int error;
} NaNativeTasBoard;
/* Speaker GENI/GPIO owner only. Capture remains unavailable until its common
 * VRM owner is bound; this provider never touches codec/USBSS GPIO161/162. */
int na_native_tas_board(NaNativeTasBoard *,Sm8850Resources *,NaBoardIo *);
#ifdef __cplusplus
}
#endif
#endif
