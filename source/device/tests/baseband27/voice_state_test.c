/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../package/baseband27/bridge/NbVoice.h"
static uint8_t wire[256], tlv[128];
static unsigned message(unsigned type, unsigned id, unsigned txn, const uint8_t *t, unsigned n)
{
    wire[0] = (uint8_t)type; cbd_wr16(wire + 1, txn); cbd_wr16(wire + 3, id); cbd_wr16(wire + 5, n);
    memcpy(wire + 7, t, n); return 7 + n;
}
static void bind(struct nb_voice *v, unsigned slot, unsigned channel, unsigned cid, unsigned sub)
{
    uint8_t t[] = {1,1,0,(uint8_t)sub}; unsigned n = message(0,0x44,123,t,sizeof t);
    nb_voice_request(v,slot,channel,cid,0,212,wire,n);
    cbq_tlv_result(tlv,0); n = message(2,0x44,123,tlv,7);
    assert(nb_voice_message(v,slot,channel,cid,0,212,wire,n));
}
static unsigned calls(unsigned id, unsigned state)
{
    uint8_t t[] = {1,8,0,1,(uint8_t)id,(uint8_t)state,2,1,4,0,0};
    return message(4,0x2e,0,t,sizeof t);
}
static unsigned codec(unsigned id, unsigned value, unsigned rate)
{
    unsigned at = 0;
    tlv[at++] = 0x13; cbd_wr16(tlv+at,1); at+=2; tlv[at++] = (uint8_t)id;
    tlv[at++] = 0x11; cbd_wr16(tlv+at,4); at+=2; cbd_wr32(tlv+at,value); at += 4;
    tlv[at++] = 0x12; cbd_wr16(tlv+at,4); at+=2; cbd_wr32(tlv+at,rate); at += 4;
    return message(4,0x53,0,tlv,at);
}
static int observe(struct nb_voice *v, unsigned slot, unsigned channel, unsigned cid, unsigned n)
{ return nb_voice_message(v,slot,channel,cid,0,212,wire,n); }
int main(void)
{
    struct nb_voice v; nb_voice_reset(&v); uint8_t out[16];
    assert(!nb_voice_vocoder(&v,0,out,sizeof out));
    bind(&v,0,0,1,0); bind(&v,1,1,2,1);
    unsigned n = calls(1,3); assert(observe(&v,0,0,1,n));
    n = codec(1,7,16000); assert(observe(&v,0,0,1,n));
    assert(nb_voice_vocoder(&v,0,out,sizeof out)==5 && out[3]==0x14 && out[4]==16);
    assert(!nb_voice_vocoder(&v,1,out,sizeof out));
    n = calls(1,3); assert(observe(&v,1,1,2,n)); n = codec(1,20,48000); assert(observe(&v,1,1,2,n));
    assert(nb_voice_vocoder(&v,1,out,sizeof out)==5 && out[3]==0x30 && out[4]==48);
    assert(nb_voice_vocoder(&v,0,out,sizeof out)==5 && out[3]==0x14);
    /* Query replies use distinct call-list TLV10, then the same codec fields. */
    n=calls(1,3); wire[0]=2; cbd_wr16(wire+3,0x2f); wire[7]=0x10;
    cbq_tlv_result(wire+n,0); cbd_wr16(wire+5,n-7+7); n+=7;
    assert(!observe(&v,0,0,1,n));
    n=codec(1,7,16000); wire[0]=2; cbd_wr16(wire+3,0x6e);
    cbq_tlv_result(wire+n,0); cbd_wr16(wire+5,n-7+7); n+=7;
    assert(!observe(&v,0,0,1,n));
    /* No inferred rate, unsupported codec, unrelated call, bad frame or tag. */
    n=codec(2,6,8000); assert(!observe(&v,0,0,1,n));
    n=codec(1,1,8000); assert(observe(&v,0,0,1,n)); assert(!nb_voice_vocoder(&v,0,out,sizeof out));
    n=codec(1,6,12000); assert(observe(&v,0,0,1,n)); assert(!nb_voice_vocoder(&v,0,out,sizeof out));
    n=codec(1,6,8000); assert(observe(&v,0,0,1,n));
    assert(nb_voice_vocoder(&v,0,out,sizeof out)==5 && out[3]==0x13 && out[4]==8);
    uint8_t good[256]; memcpy(good,wire,n);
    for(unsigned cut=0;cut<n;++cut) assert(!observe(&v,0,0,1,cut));
    assert(!nb_voice_vocoder(&v,0,out,4));
    memcpy(wire,good,n); wire[n++]=0; assert(!observe(&v,0,0,1,n));
    n=codec(1,7,16000); wire[n]=0x11; wire[n+1]=0; wire[n+2]=0; cbd_wr16(wire+5,n-7+3);
    assert(!observe(&v,0,0,1,n+3));
    /* End/hold withdraws the foreground format; a reused call id is fresh. */
    n=calls(1,6); assert(observe(&v,0,0,1,n)); assert(!nb_voice_vocoder(&v,0,out,sizeof out));
    n=calls(1,9); assert(observe(&v,0,0,1,n)); assert(!nb_voice_vocoder(&v,0,out,sizeof out));
    n=codec(1,7,16000); assert(!observe(&v,0,0,1,n));
    n=calls(1,3); assert(observe(&v,0,0,1,n)); assert(!nb_voice_vocoder(&v,0,out,sizeof out));
    n=codec(1,7,16000); assert(observe(&v,0,0,1,n));
    /* Rebinding is not complete until its own valid successful response. */
    uint8_t sub[]={1,1,0,1}; n=message(0,0x44,456,sub,sizeof sub);
    nb_voice_request(&v,0,0,1,0,212,wire,n); assert(!nb_voice_vocoder(&v,0,out,sizeof out));
    cbq_tlv_result(tlv,0); n=message(2,0x44,457,tlv,7); assert(!observe(&v,0,0,1,n));
    n=message(2,0x44,456,tlv,7); assert(observe(&v,0,0,1,n)); assert(!nb_voice_vocoder(&v,0,out,sizeof out));
    /* A deleted real endpoint/client clears observations before ID reuse. */
    struct cbq_bridge b; cbq_init(&b); nb_voice_sync(&v,&b); assert(!v.client[0].used && !v.client[1].used);
    bind(&v,0,0,1,0); n=calls(1,3); assert(observe(&v,0,0,1,n)); n=codec(1,7,16000); assert(observe(&v,0,0,1,n));
    assert(!nb_voice_message(&v,0,0,1,0,213,wire,n)); assert(!nb_voice_vocoder(&v,0,out,sizeof out));
    /* Two foreground calls on one channel cannot impersonate a known route. */
    nb_voice_reset(&v); bind(&v,0,0,1,0); bind(&v,1,0,2,1);
    n=calls(1,3); assert(observe(&v,0,0,1,n)); assert(observe(&v,1,0,2,n));
    n=codec(1,7,16000); assert(observe(&v,0,0,1,n)); assert(observe(&v,1,0,2,n));
    assert(!nb_voice_vocoder(&v,0,out,sizeof out));
    puts("PASS VOICE native audio format: real bind/call/codec/rate, two SIMs, termination, endpoint lifetime, malformed and ambiguous input");
}
