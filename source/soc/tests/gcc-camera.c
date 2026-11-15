/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Sm8850Resources.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t word;
static unsigned written, dynamic, drop_enable, fail_io;
static int lock(void *p) { (void)p;return 0; }
static void unlock(void *p) { (void)p; }
static int barrier(void *p) { (void)p;return 0; }
static int read64(void *p,unsigned d,uint32_t o,uint64_t *v)
{ (void)p;(void)d;(void)o;*v=0;return 0; }
static int read32(void *p,unsigned d,uint32_t o,uint32_t *v)
{ (void)p;assert(d==Sm8850Gcc && !(o&3));if (fail_io) return -1;*v=word;return 0; }
static int write32(void *p,unsigned d,uint32_t o,uint32_t value)
{
    (void)p;assert(d==Sm8850Gcc && !(o&3));++written;
    if (fail_io) return -1;
    word=value;
    if (dynamic) word=(value&~0x80000000u)|((value&1u) ? 0u:0x80000000u);
    if (drop_enable) word&=~1u;
    return 0;
}
int main(void)
{
    Sm8850Resources resources={0};
    const Sm8850ResourceIo io={NULL,lock,unlock,read32,write32,read64,barrier};
    assert(!sm8850_resources_init(&resources,&io));
    static const uint32_t gates[]={0x26004,0x2603c,0xa0004,0x26014,0x26028};
    dynamic=1;
    for (unsigned i=0;i<sizeof(gates)/sizeof(gates[0]);++i) {
        word=0x80000002u;
        assert(sm8850_gcc_update(&resources,gates[i],1,1,1)==Sm8850Okay);
        assert(word==3); /* HWCG unchanged; hardware cleared CLK_OFF. */
        assert(sm8850_gcc_update(&resources,gates[i],1,1,0)==Sm8850Okay);
        assert(word==0x80000002u);
    }
    drop_enable=1;
    assert(sm8850_gcc_update(&resources,gates[0],1,1,1)==Sm8850Conflict);
    drop_enable=0;fail_io=1;
    assert(sm8850_gcc_update(&resources,gates[0],1,1,1)==Sm8850Io);
    fail_io=0;dynamic=0;word=0x07c00000u;
    assert(!sm8850_gcc_update(&resources,0x52018,0x300,0x300,0x200));
    assert(word==0x07c00200u);
    unsigned before=written;
    assert(sm8850_gcc_update(&resources,gates[0],1,2,0)==Sm8850Invalid && written==before);
    assert(sm8850_gcc_update(&resources,gates[0],1,1,2)==Sm8850Invalid && written==before);
    puts("GCC camera: all five gates accept dynamic CLK_OFF; owned-bit, I/O and mask failures retained; HWCG/unrelated bits preserved");
}
