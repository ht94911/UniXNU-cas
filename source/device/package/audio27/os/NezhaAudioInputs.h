/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_INPUTS_H
#define NEZHA_AUDIO_INPUTS_H
#include "../platform/AudioInputPack.h"
#define NA_INPUT_CAPACITY (34u * 1024u * 1024u)
#ifdef __cplusplus
extern "C" {
#endif
extern const uint8_t nezha_audio_inputs[NA_INPUT_CAPACITY];
extern const uint64_t nezha_audio_inputs_size;
/* Existing GPU private-resource delivery convention: CI carries zero bytes;
 * local finalization fills this owned section before strict prelink/HMAC.
 * No file read or partition fallback. Empty CI input never enables audio. */
int na_native_audio_inputs(NaInputPack *);
#ifdef __cplusplus
}
#endif
#endif
