/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Production endpoint with saved local tuning and explicit synthetic I/O.
 * Diagnostic waits advance real time; hardware work and settling are counted
 * independently. No device, power operation or audio samples are executed. */
#include "../platform/AudioPlaybackInputs.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    uint64_t now_us,trace_us,wait_per_call;
    uint32_t calls,writes,reset,claims,releases;
} Peer;
static void advance(Peer *p) {++p->calls;p->trace_us+=p->wait_per_call;p->now_us+=p->wait_per_call+1000;}
static int claim(void *v,uint64_t *lease) {Peer *p=v;advance(p);++p->claims;*lease=42;return NA_OK;}
static int power(void *v,uint64_t lease,int on) {Peer *p=v;assert(lease==42);(void)on;advance(p);return NA_OK;}
static int reset(void *v,uint64_t lease,int asserted) {Peer *p=v;assert(lease==42);advance(p);p->reset=!!asserted;return NA_OK;}
static int release(void *v,uint64_t lease) {Peer *p=v;assert(lease==42 && p->reset);advance(p);++p->releases;return NA_OK;}
static int read_reg(void *v,uint64_t lease,uint8_t address,uint32_t reg,uint8_t *out) {
    Peer *p=v;assert(lease==42 && reg<0x800000);advance(p);
    assert(address==0x4a || address==0x48);
    *out=reg==0x78 ? (uint8_t)(address==0x4a ? 0x51:0x52):0;return NA_OK;
}
static int write_reg(void *v,uint64_t lease,uint8_t address,uint32_t reg,const uint8_t *data,uint32_t bytes) {
    Peer *p=v;assert(lease==42 && (address==0x4a || address==0x48) && data && bytes && bytes<=128-(reg&127));
    advance(p);++p->writes;return NA_OK;
}
static NaTasPowerOps ops(Peer *p,int timed) {
    return (NaTasPowerOps){p,claim,power,reset,release,{p,read_reg,write_reg},timed ? &p->trace_us:0,0};
}
int main(int argc,char **argv) {
    assert(argc==5);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long bytes=ftell(f);assert(bytes>0 && bytes<=0x8000000);rewind(f);
    void *blob=malloc((size_t)bytes);assert(blob && fread(blob,1,(size_t)bytes,f)==(size_t)bytes);fclose(f);
    NaInputPack pack;assert(!na_input_pack(&pack,blob,(uint64_t)bytes));
    NaTasSelection selection={.revision={0x51,0x52},.revision_mask={0xff,0xff},.profile=argv[2],.rx_format=argv[3],.tx_format=argv[4]};
    NaPlaybackInputs inputs={0};assert(!na_playback_inputs(&inputs,&pack,&selection));
    unsigned counts[3]={0};
    for (unsigned timed=0;timed<2;++timed) {
        Peer p={.now_us=1000000,.wait_per_call=100000};NaTasPowerOps io=ops(&p,(int)timed);NaTasEndpoint e;
        assert(!na_tas_endpoint_init(&e,&inputs.tuning,&inputs.selection,&io));
        assert(!na_tas_endpoint_request(&e,NaTasPrepared));int result=NA_PENDING;
        for (unsigned poll=0;poll<10000 && result==NA_PENDING;++poll) {
            result=na_tas_endpoint_poll(&e,p.now_us/1000);p.now_us+=1000;
        }
        if (timed) {
            assert(result==NA_OK && e.state==NaTasPrepared && !e.changing && !e.error && e.identified);
            assert(e.index==e.count[0] && p.writes && p.now_us>e.deadline*1000);
            uint32_t writes=p.writes;
            assert(na_tas_endpoint_poll(&e,p.now_us/1000)==NA_OK && writes==p.writes);
            printf("timed preparation: %u table steps, %u writes, %llu diagnostic us, %llu real us\n",e.count[0],p.writes,(unsigned long long)p.trace_us,(unsigned long long)p.now_us);
        } else assert(result==NA_TIMEOUT && e.error==NA_TIMEOUT && p.reset && !p.releases);
        counts[timed]=p.writes;
    }
    assert(counts[1]>counts[0]);
    /* Ordinary preparation without diagnostic waits retains its old timing. */
    Peer ordinary={.now_us=1000000};NaTasPowerOps plain=ops(&ordinary,0);NaTasEndpoint normal;
    assert(!na_tas_endpoint_init(&normal,&inputs.tuning,&inputs.selection,&plain));
    assert(!na_tas_endpoint_request(&normal,NaTasPrepared));int complete=NA_PENDING;
    for (unsigned poll=0;poll<10000 && complete==NA_PENDING;++poll) {
        complete=na_tas_endpoint_poll(&normal,ordinary.now_us/1000);ordinary.now_us+=1000;
    }
    assert(complete==NA_OK && normal.state==NaTasPrepared && ordinary.writes==counts[1] && !ordinary.trace_us);
    /* Genuine elapsed time with no matching diagnostic wait remains bounded. */
    Peer p={.now_us=1000000};NaTasPowerOps io=ops(&p,1);NaTasEndpoint e;
    assert(!na_tas_endpoint_init(&e,&inputs.tuning,&inputs.selection,&io));
    assert(!na_tas_endpoint_request(&e,NaTasPrepared));
    assert(na_tas_endpoint_poll(&e,p.now_us/1000)==NA_PENDING);
    p.now_us=e.deadline*1000;
    assert(na_tas_endpoint_poll(&e,p.now_us/1000)==NA_TIMEOUT && e.error==NA_TIMEOUT && p.reset);
    uint32_t writes=p.writes;assert(na_tas_endpoint_poll(&e,(p.now_us+1000)/1000)==NA_TIMEOUT && p.writes==writes);
    /* A decreasing owner counter cannot extend a phase. */
    memset(&p,0,sizeof p);p.now_us=1000000;p.trace_us=5000;io=ops(&p,1);
    assert(!na_tas_endpoint_init(&e,&inputs.tuning,&inputs.selection,&io));assert(!na_tas_endpoint_request(&e,NaTasPrepared));
    assert(na_tas_endpoint_poll(&e,p.now_us/1000)==NA_PENDING);p.trace_us=0;
    assert(na_tas_endpoint_poll(&e,p.now_us/1000)==NA_IO && e.error==NA_IO && p.reset);
    free(blob);puts("five endpoint cases passed: progressing trace wait, original timeout, ordinary preparation, genuine stall and counter decrease; synthetic I/O only");return 0;
}
