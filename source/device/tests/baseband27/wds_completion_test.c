/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "NbWdsCompletion.h"
#include "coachz_bb_qmux.h"

static uint32_t failure(uint8_t *out, uint16_t result, uint16_t error,
                         uint16_t type, uint16_t code)
{
    uint8_t tlvs[19], res[4], why[2], verbose[4];
    cbd_wr16(res,result); cbd_wr16(res+2,error);
    cbd_wr16(why,1); cbd_wr16(verbose,type); cbd_wr16(verbose+2,code);
    uint32_t bytes=cbd_tlv(tlvs,sizeof tlvs,2,res,4);
    bytes+=cbd_tlv(tlvs+bytes,sizeof tlvs-bytes,0x10,why,2);
    bytes+=cbd_tlv(tlvs+bytes,sizeof tlvs-bytes,0x11,verbose,4);
    out[0]=2; cbd_wr16(out+1,7); cbd_wr16(out+3,CBP_START);
    cbd_wr16(out+5,bytes); memcpy(out+7,tlvs,bytes);
    return 7+bytes;
}

int main(void)
{
    uint8_t input[64], saved[64], tlvs[32], wire[64];
    struct cbp_client c={0}, ipv6={0};
    c.mux=2; c.family=4; c.start_txn=7; c.start_pending=1;
    ipv6.mux=2; ipv6.family=6; ipv6.active=1; ipv6.settings=2; ipv6.handle=808036768;
    const struct cbp_client original=c, original6=ipv6;
    uint32_t size=failure(input,1,14,6,51); memcpy(saved,input,size);
    assert(nb_wds_failure_completion(&c,input,size,tlvs,sizeof tlvs)==21);
    assert(!memcmp(input,saved,size)&&!memcmp(&c,&original,sizeof c));
    uint32_t length=0;
    /* Exact indication expected by the native27 WDS status parser. */
    const uint8_t expected[]={1,2,0,1,0,0x10,2,0,1,0,0x11,4,0,6,0,51,0,0x12,1,0,4};
    assert(!memcmp(tlvs,expected,sizeof expected));
    uint32_t n=cbq_private_indication(CBP_STATUS,1,8,tlvs,21,wire,sizeof wire);
    assert(n==34 && wire[4]==1 && wire[5]==8);
    assert(wire[CBQ_HDR]==4 && cbd_rd16(wire+CBQ_HDR+1)==0 && cbd_rd16(wire+CBQ_HDR+3)==CBP_STATUS);
    assert(!cbd_find(wire+CBQ_HDR,n-CBQ_HDR,2,&length));
    assert(cbp_observe(&c,input,size) && !c.start_pending && !c.active);
    assert(!nb_wds_failure_completion(&c,input,size,tlvs,sizeof tlvs));
    assert(cbp_observe(&c,wire+CBQ_HDR,n-CBQ_HDR) && !c.active);
    struct cbp_client both[]={c,ipv6};
    assert(cbp_active_families(both,2,2)==2 && !memcmp(&ipv6,&original6,sizeof ipv6));

    c=original;
    for (unsigned i=0;i<7;i++) {
        struct cbp_client rejected=c;
        if(i==0) rejected.start_pending=0;
        if(i==1) rejected.start_txn=8;
        if(i==2) rejected.active=1;
        if(i==3) rejected.mux=0;
        if(i==4) rejected.family=6;
        if(i==5) rejected.bind_pending=1;
        if(i==6) rejected.family_pending=1;
        assert(!nb_wds_failure_completion(&rejected,input,size,tlvs,sizeof tlvs));
    }
    for(unsigned i=0;i<4;i++) {
        size=failure(input,i==0?0:1,i==1?81:14,i==2?1:6,i==3?50:51);
        assert(!nb_wds_failure_completion(&c,input,size,tlvs,sizeof tlvs));
    }
    size=failure(input,1,14,6,51);
    assert(!nb_wds_failure_completion(&c,input,size,tlvs,20));
    assert(!nb_wds_failure_completion(0,input,size,tlvs,sizeof tlvs));
    for(uint32_t length=0;length<size;length++)
        assert(!nb_wds_failure_completion(&c,input,length,tlvs,sizeof tlvs));
    input[0]=4;assert(!nb_wds_failure_completion(&c,input,size,tlvs,sizeof tlvs));input[0]=2;
    cbd_wr16(input+3,CBP_SETTINGS);assert(!nb_wds_failure_completion(&c,input,size,tlvs,sizeof tlvs));cbd_wr16(input+3,CBP_START);
    /* Duplicate reason and malformed lengths must not produce a projection. */
    memcpy(input+size,input+14,5);cbd_wr16(input+5,size+5-7);
    assert(!nb_wds_failure_completion(&c,input,size+5,tlvs,sizeof tlvs));
    size=failure(input,1,14,6,51);cbd_wr16(input+8,63);
    assert(!nb_wds_failure_completion(&c,input,size,tlvs,sizeof tlvs));
    /* Actual040 IPv6 Start failure: reason1022, 3GPP43. Complete only that
     * pending inactive family; an already-active IPv4 remains usable. */
    c=original; c.family=6;
    struct cbp_client ipv4=original; ipv4.active=1; ipv4.settings=1; ipv4.handle=808036768;
    const struct cbp_client original4=ipv4;
    size=failure(input,1,14,6,43); cbd_wr16(input+17,1022); memcpy(saved,input,size);
    assert(nb_wds_failure_completion(&c,input,size,tlvs,sizeof tlvs)==21);
    const uint8_t expected6[]={1,2,0,1,0,0x10,2,0,0xfe,3,0x11,4,0,6,0,43,0,0x12,1,0,6};
    assert(!memcmp(tlvs,expected6,sizeof expected6)&&!memcmp(input,saved,size));
    n=cbq_private_indication(CBP_STATUS,1,9,tlvs,21,wire,sizeof wire);
    assert(n==34&&wire[5]==9&&cbp_observe(&c,input,size)&&!c.start_pending);
    assert(!nb_wds_failure_completion(&c,input,size,tlvs,sizeof tlvs));
    assert(cbp_observe(&c,wire+CBQ_HDR,n-CBQ_HDR)&&!c.active);
    struct cbp_client surviving[]={ipv4,c};
    assert(cbp_active_families(surviving,2,2)==1&&!memcmp(&ipv4,&original4,sizeof ipv4));
    c=original; c.family=6;
    for (unsigned i=0;i<8;i++) {
        struct cbp_client rejected=c;
        if(i==0) rejected.start_pending=0;
        if(i==1) rejected.start_txn=8;
        if(i==2) rejected.active=1;
        if(i==3) rejected.mux=0;
        if(i==4) rejected.family=4;
        if(i==5) rejected.bind_pending=1;
        if(i==6) rejected.family_pending=1;
        if(i==7) rejected.family=5;
        assert(!nb_wds_failure_completion(&rejected,input,size,tlvs,sizeof tlvs));
    }
    size=failure(input,1,14,6,51);
    assert(!nb_wds_failure_completion(&c,input,size,tlvs,sizeof tlvs));
    size=failure(input,1,14,6,43); c.family=4;
    assert(!nb_wds_failure_completion(&c,input,size,tlvs,sizeof tlvs));
    puts("PASS042 exact IPv4cause51/IPv6cause43 failed Start completion, original reasons and surviving family preserved; unrelated, repeated and malformed input rejected");
    return 0;
}
