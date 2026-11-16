/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioClock.h"
#include <limits.h>
int na_clock_contract(uint32_t numer,uint32_t denom,uint32_t hz,uint64_t *q32) {
    if (!numer || !denom || !q32 || !hz) return NA_ARGUMENT;
    /* Supports the measured 625/12 and Apple's 125/3 without correcting twice. */
    if ((uint64_t)hz*numer!=1000000000ULL*denom) return NA_MISSING;
    /* The equality above makes hz/numer conversion exact. A uint32_t Hz
     * shifted by32 fits uint64_t; do not introduce an unavailable kernel
     * __udivti3 dependency merely to cancel numer/denom again. */
    uint64_t value=((uint64_t)hz<<32)/48000;
    if (!value) return NA_OVERFLOW;
    *q32=value;return NA_OK;
}
int na_clock_frames(uint64_t anchor,uint64_t host,uint64_t q32,uint64_t *frames) {
    if (!frames || !q32 || host<anchor) return NA_ARGUMENT;
    uint64_t ticks=host-anchor;
    /* Both measured host clocks have an integral ticks/frame value. */
    if (!(q32&UINT32_MAX)) {*frames=ticks/(q32>>32);return NA_OK;}
    uint64_t whole=ticks/q32,remainder=ticks%q32;
    if (whole>UINT32_MAX) return NA_OVERFLOW;
    uint32_t fraction=0;
    /* Exact floor((remainder<<32)/q32), without a128-bit division helper.
     * remainder<q32. Compare before doubling so neither branch overflows.
     * This is bounded to32 steps even for hostile public ABI inputs. */
    for (unsigned bit=0;bit<32;++bit) {
        fraction<<=1;
        if (remainder>=q32-remainder) {remainder-=q32-remainder;fraction|=1;}
        else remainder+=remainder;
    }
    *frames=(whole<<32)|fraction;return NA_OK;
}
int na_clock_host(uint64_t anchor,uint64_t frames,uint64_t q32,uint64_t *host) {
    if (!host || !q32) return NA_ARGUMENT;
    __uint128_t offset=((__uint128_t)frames*q32)>>32;
    if (offset>UINT64_MAX-anchor) return NA_OVERFLOW;
    *host=anchor+(uint64_t)offset;return NA_OK;
}
