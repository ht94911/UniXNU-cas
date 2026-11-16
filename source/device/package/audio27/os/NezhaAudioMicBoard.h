/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_NATIVE_MIC_BOARD_H
#define NEZHA_AUDIO_NATIVE_MIC_BOARD_H
#include "NezhaAudioNative.h"
#include "../platform/NezhaAudioBoard.h"
#include "../platform/NezhaMicPower.h"
#include <Sm8850Resources.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    Sm8850Resources *common;
    NaNativeWindow window[6];
    NaMicPower power;
    uint64_t lease,tcs_lease,trace_wait_us;
    uint32_t irq_previous,irq_saved,quarantined;
    int error;
} NaNativeMicBoard;
/* Binds the existing NaMicBoard/WCD9395 operations. This alone does not
 * enable a capture stream, issue PCM, or publish a HAL input device. */
int na_native_mic_board(NaNativeMicBoard *,Sm8850Resources *,NaBoardIo *);
#ifdef __cplusplus
}
#endif
#endif
