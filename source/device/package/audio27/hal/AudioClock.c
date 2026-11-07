/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioClock.h"
#include <limits.h>
int na_clock_contract(uint32_t numer,uint32_t denom,uint32_t hz,uint64_t *q32) {
    if (!numer || !denom || !q32 || !hz) return NA_ARGUMENT;
    /* Supports the measured 625/12 and Apple's 125/3 without correcting twice. */
    if ((uint64_t)hz*numer!=1000000000ULL*denom) return NA_MISSING;
    __uint128_t value=((__uint128_t)1000000000*denom<<32)/((uint64_t)numer*48000);
    if (!value || value>UINT64_MAX) return NA_OVERFLOW;
    *q32=(uint64_t)value;return NA_OK;
}
int na_clock_frames(uint64_t anchor,uint64_t host,uint64_t q32,uint64_t *frames) {
    if (!frames || !q32 || host<anchor) return NA_ARGUMENT;
    __uint128_t value=((__uint128_t)(host-anchor)<<32)/q32;
    if (value>UINT64_MAX) return NA_OVERFLOW;
    *frames=(uint64_t)value;return NA_OK;
}
int na_clock_host(uint64_t anchor,uint64_t frames,uint64_t q32,uint64_t *host) {
    if (!host || !q32) return NA_ARGUMENT;
    __uint128_t offset=((__uint128_t)frames*q32)>>32;
    if (offset>UINT64_MAX-anchor) return NA_OVERFLOW;
    *host=anchor+(uint64_t)offset;return NA_OK;
}
