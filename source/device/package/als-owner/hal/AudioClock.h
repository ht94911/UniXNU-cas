/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_CLOCK_H
#define NEZHA_AUDIO_CLOCK_H
#include "../core/AudioProtocol.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Integer-only shared timeline from sheng. The caller obtains numer/denom from
 * clock_timebase_info (kernel) or mach_timebase_info (HAL), never a board guess.
 * coreaudio_hz is read from the actual AudioGetHostClockFrequency API. A mismatch
 * blocks normal HAL use until a separately reviewed, exact-donor fix exists. */
int na_clock_contract(uint32_t numer,uint32_t denom,uint32_t coreaudio_hz,
                       uint64_t *ticks_per_frame_q32);
int na_clock_frames(uint64_t anchor,uint64_t host,uint64_t ticks_q32,uint64_t *frames);
int na_clock_host(uint64_t anchor,uint64_t frames,uint64_t ticks_q32,uint64_t *host);
#ifdef __cplusplus
}
#endif
#endif
