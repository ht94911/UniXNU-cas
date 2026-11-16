/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_REFERENCE_H
#define NEZHA_AUDIO_REFERENCE_H
#include "../core/AudioRing.h"

/* Both rings belong to the same HAL epoch and their writers hold gLock.
 * Keep the accepted, post-gain S16 mix after the DSP consumes its own slot.
 * A late/rejected write must never enter the playback reference. */
static inline int na_reference_mix(uint64_t *output, uint64_t *reference,
                                   uint64_t frame, int16_t left, int16_t right) {
    int result = na_ring_mix(output, frame, left, right);
    return result ? result : na_ring_mix(reference, frame, left, right);
}

static inline int na_reference_read(const uint64_t *reference, uint64_t frame,
                                    int16_t *left, int16_t *right) {
    if (!reference || !left || !right || frame > NA_AUDIO_MAX_FRAME) return NA_ARGUMENT;
    uint64_t word = __atomic_load_n(reference + frame % NA_AUDIO_RING_FRAMES, __ATOMIC_ACQUIRE);
    *left = *right = 0;
    if ((uint32_t)(word >> 32) != (uint32_t)frame + 1) return NA_MISSING;
    *left = (int16_t)word;
    *right = (int16_t)(word >> 16);
    return NA_OK;
}
#endif
