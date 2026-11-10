/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaAudioInputs.h"
__attribute__((used,aligned(4096),section("__DATA_CONST,__nezaudio")))
const uint8_t nezha_audio_inputs[NA_INPUT_CAPACITY]={0};
const uint64_t nezha_audio_inputs_size=NA_INPUT_CAPACITY;
int na_native_audio_inputs(NaInputPack *pack) {
    int r=na_input_pack(pack,nezha_audio_inputs,nezha_audio_inputs_size);
    if (r) return r;
    if (pack->count!=63) {*pack=(NaInputPack){0};return NA_MISSING;}
    return NA_OK;
}
