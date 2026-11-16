// SPDX-License-Identifier: BSD-2-Clause-Patent
// Private board data arrives only through the local ADT. This source contains
// the format/validation contract,never factory curves or device identities.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
enum {NEZHA_BL_PROFILE_MAX_POINTS=257,NEZHA_BL_PROFILE_HEADER_BYTES=44};
struct nezha_brightness_profile {
    uint32_t minimum,maximum,count;
    // Original27 dictionary order:product max/mid/min,panel max/mid/min.
    uint32_t calibration[6],marketing[NEZHA_BL_PROFILE_MAX_POINTS];
};
static inline uint32_t nezha_profile_u32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static inline bool nezha_brightness_profile_decode(const void *raw,size_t bytes,
                                                   struct nezha_brightness_profile *out) {
    if(!raw || !out || bytes<NEZHA_BL_PROFILE_HEADER_BYTES)return false;
    const uint8_t *p=(const uint8_t *)raw;const uint8_t magic[8]={'N','B','L','N','C','0','0','1'};
    for(unsigned i=0;i<8;++i)if(p[i]!=magic[i])return false;
    uint32_t minimum=nezha_profile_u32(p+8),maximum=nezha_profile_u32(p+12),count=nezha_profile_u32(p+16);
    if(minimum!=7 || maximum!=5460 || count<3 || count>NEZHA_BL_PROFILE_MAX_POINTS ||
       !(count&1) || bytes!=NEZHA_BL_PROFILE_HEADER_BYTES+(size_t)count*4)return false;
    // IOFixed values are signed16.16. This SDR backend admits only positive,
    // strictly increasing configured nits and no high-brightness extension.
    uint32_t first=0,last=0,middle=0;
    for(uint32_t i=0;i<count;++i) {
        uint32_t value=nezha_profile_u32(p+NEZHA_BL_PROFILE_HEADER_BYTES+4*i);
        if(!value || value>UINT32_C(4096)*65536 || (i && value<=last))return false;
        if(!i)first=value;
        if(i==count/2)middle=value;
        last=value;
    }
    for(unsigned i=0;i<6;++i) {
        uint32_t want=i%3==0?last:i%3==1?middle:first;
        if(nezha_profile_u32(p+20+4*i)!=want)return false;
    }
    // Publish into the output only after every byte and relationship passes.
    out->minimum=minimum;out->maximum=maximum;out->count=count;
    for(unsigned i=0;i<6;++i)out->calibration[i]=nezha_profile_u32(p+20+4*i);
    for(uint32_t i=0;i<count;++i)out->marketing[i]=nezha_profile_u32(p+NEZHA_BL_PROFILE_HEADER_BYTES+4*i);
    return true;
}

// The validated NBLNC001 samples were generated at equally spaced DBV
// positions between minimum and maximum (069/070), not at equal nits. Invert
// that same piecewise curve for the 27 signed16.16 brightness-nits request.
// Keep the conservative profile endpoints; do not rescale to the14-bit field
// maximum or the active panel's separate8191 normal limit. Call after decode.
static inline uint32_t nezha_brightness_level_for_nits(
    const struct nezha_brightness_profile *p,int32_t fixed_nits) {
    if(fixed_nits<=0)return 0;
    const uint32_t nits=(uint32_t)fixed_nits,segments=p->count-1;
    if(nits<=p->marketing[0])return p->minimum;
    if(nits>=p->marketing[segments])return p->maximum;
    for(uint32_t i=1;i<=segments;++i) {
        if(nits>p->marketing[i])continue;
        const uint32_t low=p->marketing[i-1],span=p->marketing[i]-low;
        const uint64_t denominator=(uint64_t)segments*span;
        const uint64_t position=(uint64_t)(i-1)*span+nits-low;
        return p->minimum+(uint32_t)(((uint64_t)(p->maximum-p->minimum)*position+
                                     denominator/2)/denominator);
    }
    return p->maximum;
}

// Publish measured command state in the same curve's units. Zero is the
// existing off command; all positive levels are within decoded endpoints.
static inline uint32_t nezha_brightness_nits_for_level(
    const struct nezha_brightness_profile *p,uint32_t level) {
    if(!level)return 0;
    if(level<=p->minimum)return p->marketing[0];
    if(level>=p->maximum)return p->marketing[p->count-1];
    const uint32_t range=p->maximum-p->minimum;
    const uint64_t position=(uint64_t)(level-p->minimum)*(p->count-1);
    const uint32_t i=(uint32_t)(position/range),remainder=(uint32_t)(position%range);
    return p->marketing[i]+(uint32_t)(((uint64_t)(p->marketing[i+1]-p->marketing[i])*
                                      remainder+range/2)/range);
}
static inline uint32_t nezha_brightness_millinits(uint32_t fixed_nits) {
    return (uint32_t)(((uint64_t)fixed_nits*1000+32768)/65536);
}
