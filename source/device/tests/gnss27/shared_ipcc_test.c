/* SPDX-License-Identifier: BSD-2-Clause */
/* GPU051's early ADSP and later MSS share one append-only IPCC dispatcher. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <Sm8850Resources.h>
static unsigned held,callbacks,clears;
static uint32_t next=UINT32_MAX;
static int lock(void *p){(void)p;assert(!held);held=1;return 0;}
static void unlock(void *p){(void)p;assert(held);held=0;}
static int read32(void *p,unsigned d,uint32_t at,uint32_t *v){(void)p;assert(d==4&&at==0x10);*v=next;return 0;}
static int write32(void *p,unsigned d,uint32_t at,uint32_t v){(void)p;assert(d==4);if(at==0x1c){assert(v==next);next=UINT32_MAX;clears++;}return 0;}
static int read64(void *p,unsigned d,uint32_t at,uint64_t *v){(void)p;(void)d;(void)at;*v=0;return 0;}
static int fence(void *p){(void)p;return 0;}
static void event(void *p,uint32_t id){assert(!held&&id==(uint32_t)(uintptr_t)p);callbacks++;}
int main(void)
{
    Sm8850Resources r;Sm8850ResourceIo io={0,lock,unlock,read32,write32,read64,fence};
    assert(!sm8850_resources_init(&r,&io));
    assert(!sm8850_ipcc_register(&r,0x30002,event,(void *)(uintptr_t)0x30002));
    next=0x30002;assert(!sm8850_ipcc_dispatch(&r,32));assert(callbacks==1&&clears==1&&r.ipcc_sealed);
    assert(!sm8850_ipcc_register(&r,0x20002,event,(void *)(uintptr_t)0x20002));
    assert(sm8850_ipcc_register(&r,0x20002,event,(void *)(uintptr_t)0x20002)==Sm8850Conflict);
    next=0x20002;assert(!sm8850_ipcc_dispatch(&r,32));assert(callbacks==2&&clears==2);
    next=0x30002;assert(!sm8850_ipcc_dispatch(&r,32));assert(callbacks==3&&clears==3);
    assert(!held&&r.ipcc_count==2);puts("PASS shared IPCC: ADSP dispatch then MSS append, both callbacks and duplicate rejection");
}
