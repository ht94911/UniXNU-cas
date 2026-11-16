/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../hal/AudioClock.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
static uint64_t seed=0xbad50f327065ULL;
static uint64_t random64(void) {
    seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;
}
static void check(uint64_t ticks,uint64_t q32) {
    uint64_t actual=0xdecafbadULL;
    int result=na_clock_frames(0,ticks,q32,&actual);
    if (!q32) {assert(result==NA_ARGUMENT);return;}
    __uint128_t reference=((__uint128_t)ticks<<32)/q32;
    if (reference>UINT64_MAX) {assert(result==NA_OVERFLOW);assert(actual==0xdecafbadULL);}
    else {assert(result==NA_OK);assert(actual==(uint64_t)reference);}
}
int main(void) {
    const uint64_t edges[]={0,1,2,3,UINT32_MAX,1ULL<<32,(1ULL<<32)+1,
        400ULL<<32,500ULL<<32,1ULL<<63,UINT64_MAX-1,UINT64_MAX};
    for(unsigned i=0;i<sizeof edges/sizeof edges[0];++i)
        for(unsigned j=0;j<sizeof edges/sizeof edges[0];++j)check(edges[i],edges[j]);
    for(unsigned i=0;i<200000;++i)check(random64(),random64());
    uint64_t q,frames,host;
    assert(na_clock_contract(625,12,19200000,&q)==NA_OK && q==(400ULL<<32));
    assert(na_clock_contract(125,3,24000000,&q)==NA_OK && q==(500ULL<<32));
    assert(na_clock_contract(1000000000,1,1,&q)==NA_OK && q==(1ULL<<32)/48000);
    assert(na_clock_contract(1,4,4000000000u,&q)==NA_OK && q==(4000000000ULL<<32)/48000);
    assert(na_clock_contract(625,12,24000000,&q)==NA_MISSING);
    for(unsigned hz=19200000;hz<=24000000;hz+=4800000) {
        assert(na_clock_contract(hz==19200000?625:125,hz==19200000?12:3,hz,&q)==NA_OK);
        for(unsigned i=0;i<10000;++i) {
            uint64_t count=random64()&((1ULL<<40)-1);
            assert(na_clock_host(100,count,q,&host)==NA_OK);
            assert(na_clock_frames(100,host,q,&frames)==NA_OK && frames==count);
        }
    }
    assert(na_clock_frames(1,0,q,&frames)==NA_ARGUMENT);
    assert(na_clock_host(UINT64_MAX,1,q,&host)==NA_OVERFLOW);
    puts("clock:200144 exact128-bit oracle cases and20000 measured-clock roundtrips passed");
    return 0;
}
