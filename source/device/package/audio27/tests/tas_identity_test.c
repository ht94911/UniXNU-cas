/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Production identity sequencer, synthetic GPIO/clock/I2C callbacks. */
#include "../platform/Tas25xxEndpoint.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    char events[64];unsigned calls,fail_at,busy,partial_claim,powered,asserted,released;
} Peer;
static int event(Peer *p,char code) {
    assert(p->calls<sizeof p->events);p->events[p->calls++]=code;
    return p->calls==p->fail_at ? NA_IO:NA_OK;
}
static int claim(void *v,uint64_t *lease) {
    Peer *p=v;int r=event(p,'C');
    if (p->busy) {--p->busy;return NA_BUSY;}
    if (!r || p->partial_claim) *lease=55;
    return r;
}
static int reset(void *v,uint64_t lease,int asserted) {
    Peer *p=v;assert(lease==55);p->asserted=!!asserted;
    return event(p,asserted ? 'R':'r');
}
static int power(void *v,uint64_t lease,int on) {
    Peer *p=v;assert(lease==55);p->powered=!!on;
    return event(p,on ? 'P':'p');
}
static int release(void *v,uint64_t lease) {
    Peer *p=v;assert(lease==55 && !p->powered && p->asserted);
    int r=event(p,'L');if (!r) p->released=1;return r;
}
static int read(void *v,uint64_t lease,uint8_t address,uint32_t reg,uint8_t *out) {
    Peer *p=v;assert(lease==55 && reg==0x78 && p->powered && !p->asserted);
    unsigned ch=address==(uint8_t)na_nezha_facts.amp_address[0] ? 0:1;
    assert(address==(uint8_t)na_nezha_facts.amp_address[ch]);
    int r=event(p,(char)('0'+ch));if (!r) *out=(uint8_t)(0x51+ch);return r;
}
static NaTasPowerOps ops(Peer *p) {
    return (NaTasPowerOps){p,claim,power,reset,release,{p,read,0},0};
}
static int run(NaTasIdentity *d,unsigned start) {
    int r=NA_PENDING;
    for (unsigned i=0;i<20 && r==NA_PENDING;++i) r=na_tas_identity_poll(d,start+10*i);
    return r;
}
int main(void) {
    Peer p={0};NaTasIdentity d={0};NaTasPowerOps io=ops(&p);
    assert(!na_tas_identity_init(&d,&io));
    assert(na_tas_identity_poll(&d,100)==NA_PENDING && p.calls==3);
    assert(na_tas_identity_poll(&d,119)==NA_PENDING && p.calls==3);
    assert(na_tas_identity_poll(&d,120)==NA_PENDING && p.calls==4);
    assert(na_tas_identity_poll(&d,139)==NA_PENDING && p.calls==4);
    assert(!na_tas_identity_poll(&d,140) && p.calls==9);
    assert(!memcmp(p.events,"CRPr01RpL",9) && d.phase==3 && d.revision[0]==0x51 && d.revision[1]==0x52);
    assert(!d.lease && !d.claimed && p.released && !p.powered && p.asserted);
    assert(!na_tas_identity_poll(&d,10000) && !na_tas_identity_stop(&d) && p.calls==9);
    assert(na_tas_identity_init(&d,&io)==NA_ARGUMENT);
    for (unsigned failure=1;failure<=9;++failure) {
        p=(Peer){.fail_at=failure};d=(NaTasIdentity){0};io=ops(&p);
        assert(!na_tas_identity_init(&d,&io) && run(&d,100)==NA_IO && d.error==NA_IO);
        unsigned before=p.calls;
        assert(na_tas_identity_poll(&d,500)==NA_IO && p.calls==before);
        (void)na_tas_identity_stop(&d);assert(p.calls==before);
        if (failure>=7) assert(d.lease==55 && !p.released && d.cleanup_error==NA_IO);
        else if (failure!=1) assert(!d.lease && p.released && !p.powered && p.asserted);
    }
    p=(Peer){.fail_at=1,.partial_claim=1};d=(NaTasIdentity){0};io=ops(&p);
    assert(!na_tas_identity_init(&d,&io) && run(&d,100)==NA_IO);
    assert(p.calls==1 && d.lease==55 && !d.claimed && d.claim_failed && d.cleanup_error==NA_OWNERSHIP);
    p=(Peer){.busy=3};d=(NaTasIdentity){0};io=ops(&p);
    assert(!na_tas_identity_init(&d,&io));
    for (unsigned i=0;i<3;++i) assert(na_tas_identity_poll(&d,100+10*i)==NA_PENDING && !d.lease && !p.powered);
    assert(!run(&d,130) && d.phase==3 && p.released);
    p=(Peer){.busy=10000};d=(NaTasIdentity){0};io=ops(&p);
    assert(!na_tas_identity_init(&d,&io) && na_tas_identity_poll(&d,100)==NA_PENDING);
    assert(na_tas_identity_poll(&d,5100)==NA_TIMEOUT && p.calls==1 && !d.lease);
    p=(Peer){0};d=(NaTasIdentity){0};io=ops(&p);
    assert(!na_tas_identity_init(&d,&io) && na_tas_identity_poll(&d,100)==NA_PENDING);
    assert(!na_tas_identity_stop(&d) && p.released && p.asserted && !p.powered);
    assert(na_tas_identity_poll(&d,200)==NA_OWNERSHIP);
    puts("TAS identity PASS: exact reset settles, real sequencer, nine I/O failures, partial claim retention, bounded busy and one cleanup; synthetic hardware only");
    return 0;
}
