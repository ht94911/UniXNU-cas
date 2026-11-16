/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Prepared for the selected package stage; executes the production bus layer. */
#include "../Sm8850SpmiV8.h"
#include <assert.h>
#include <string.h>
static unsigned locked,commands;static uint32_t status,word;static uint64_t last_command;
static unsigned timed;
static uint32_t ownerValue;
static int acquire(void *v){(void)v;assert(!locked);locked=1;return 0;}
static void release(void *v){(void)v;assert(locked);locked=0;}
static void pause(void *v,unsigned us){(void)v;(void)us;assert(locked);}
static int read32(void *v,uint64_t a,uint32_t *out) {
    (void)v;assert(locked);*out=0;
    if(a==0x0c400000)*out=0x80000001;
    else if(a==0x0c400004 || a==0x0c400008)*out=2048;
    else if(a==0x0c403000+4u*2055)*out=0x800007f0;
    else if(a==0x0c43b000+4u*7)*out=ownerValue;
    else if(a==0x0c42a000+4u*2055)*out=3; /* wrong bus table must never authorize */
    else if(a==last_command+8)*out=status;
    else if(a==last_command+0x18)*out=word;
    return 0;
}
static int write32(void *v,uint64_t a,uint32_t value) {
    (void)v;assert(locked);
    if(a==0x0c4c0000+0x20u*2055 || a==0x0c900000+0x200u*2055) {
        ++commands;last_command=a;status=timed?0:1;word=0x55;
        assert(((value>>4)&255)==0x4c);
    }
    return 0;
}
int main(void) {
    Sm8850SpmiV8 s;Sm8850SpmiOps ops={0,read32,write32,pause,acquire,release};
    assert(!sm8850_spmi_v8_init(&s,&ops,0));
    Sm8850SpmiChannel c;assert(!sm8850_spmi_v8_find(&s,1,0x7f0,&c));
    assert(c.apid==2055 && c.owner==0 && c.irq_owner==0);
    uint8_t value=0x81;assert(!sm8850_spmi_v8_write(&s,&c,0x4c,&value,1));
    assert(last_command==0x0c900000+0x200u*2055);
    unsigned count=commands;ownerValue=3;
    assert(sm8850_spmi_v8_write(&s,&c,0x4c,&value,1)==-13 && commands==count);
    ownerValue=0;
    assert(!sm8850_spmi_v8_read(&s,&c,0x4c,&value,1) && value==0x55);
    assert(last_command==0x0c4c0000+0x20u*2055);
    timed=1;assert(sm8850_spmi_v8_write(&s,&c,0x4c,&value,1)==-110 && s.pending);
    unsigned before=commands;value=0;
    assert(sm8850_spmi_v8_write(&s,&c,0x4c,&value,1)==-110 && commands==before);
    status=1;timed=0;assert(!sm8850_spmi_v8_write(&s,&c,0x4c,&value,1) && commands==before+1);
    assert(sm8850_spmi_v8_find(&s,0,0x7f0,&c)==-19);
    return 0;
}
