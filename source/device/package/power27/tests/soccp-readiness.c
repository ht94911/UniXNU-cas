/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaSoccp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t memory[0x200000/4],before[0x200000/4];
static NpSoccp link_state;
static QcomSmem smem;
static QcomSmemPartition part;
static unsigned locks,bells;
static int deny_lock;
static int lock(void *ctx){(void)ctx;++locks;return !deny_lock;}
static void unlock(void *ctx){(void)ctx;}
static void kick(void *ctx,uint32_t client,uint32_t signal){(void)ctx;assert(client==13 && (signal==0||signal==2));++bells;}
static uint64_t now(void *ctx){(void)ctx;return 1234;}
static void state(void *ctx,int up,uint64_t epoch){(void)ctx;(void)up;(void)epoch;}
static void receive(void *ctx,uint64_t epoch,const uint8_t *p,uint32_t n){(void)ctx;(void)epoch;(void)p;(void)n;}
static const NpSoccpIo io={0,lock,unlock,kick,now,state,receive,0};
static void setup(void) {
    memset(memory,0,sizeof(memory));memset(&link_state,0,sizeof(link_state));locks=bells=0;deny_lock=0;
    uint32_t toc=0x1ff000,e=toc+32,off=0x10000,size=0x80000,hosts=19u<<16;
    memory[toc/4]=0x434f5424;memory[(toc+4)/4]=1;memory[(toc+8)/4]=1;
    memory[e/4]=off;memory[(e+4)/4]=size;memory[(e+12)/4]=hosts;memory[(e+16)/4]=64;
    memory[off/4]=0x54525024;memory[(off+4)/4]=hosts;memory[(off+8)/4]=size;
    memory[(off+12)/4]=32;memory[(off+16)/4]=size;
    smem=(QcomSmem){(volatile uint8_t *)memory,sizeof(memory)};
    assert(qcom_smem_partition(&smem,0,19,&part)==0);
}
static volatile uint32_t *add_item(uint16_t id,uint32_t bytes) {
    uint32_t off=0,size=0;
    assert(qcom_smem_alloc(&smem,&part,id,bytes)==0);
    assert(qcom_smem_find(&smem,&part,id,&off,&size)==0 && size==bytes);
    return (volatile uint32_t *)(smem.base+off);
}
static void unchanged(int expected) {
    memcpy(before,memory,sizeof(memory));unsigned l=locks,b=bells;
    assert(np_soccp_attach(&link_state,smem.base,smem.size,&io)==expected);
    assert(!memcmp(before,memory,sizeof(memory)) && locks==l && bells==b);
}
static volatile uint32_t *inbound(uint32_t value) {
    volatile uint32_t *in=add_item(617,QCOM_SMP2P_ITEM_BYTES);
    in[0]=QCOM_SMP2P_MAGIC;in[1]=QCOM_SMP2P_VERSION;in[2]=19;
    in[3]=QCOM_SMP2P_MAX_ENTRIES|(1u<<16);
    memcpy((void *)(in+5),"slave-kernel",13);in[9]=value;return in;
}
int main(void) {
    setup();unchanged(NP_SOCCP_WAIT_REMOTE); /* inbound not allocated */
    volatile uint32_t *in=add_item(617,QCOM_SMP2P_ITEM_BYTES);
    unchanged(NP_SOCCP_WAIT_REMOTE); /* zero header cannot cause outbound writes */
    in[0]=QCOM_SMP2P_MAGIC;in[1]=1;in[2]=19;in[3]=QCOM_SMP2P_MAX_ENTRIES;
    unchanged(NP_SOCCP_WAIT_REMOTE); /* slave entry not announced */
    setup();in=inbound(0);unchanged(NP_SOCCP_WAIT_REMOTE);
    in[9]=1;unchanged(-1); /* remote fatal */
    in[9]=2;in[2]=2;unchanged(-1); /* wrong remote */
    in[2]=19;in[1]=3;unchanged(-1); /* unsupported protocol */
    setup();in=inbound(2);(void)add_item(480,0x4000);
    assert(np_soccp_attach(&link_state,smem.base,smem.size,&io)==0);
    assert(link_state.attached && link_state.active && locks==3 && bells==1);
    unchanged(-1); /* a live attachment is never initialized twice */
    setup();in=inbound(2);in[1]=0x102; /* power007 real SOCCP header */
    (void)add_item(480,0x4000);
    assert(np_soccp_attach(&link_state,smem.base,smem.size,&io)==0);
    assert(link_state.smp2p.out[1]==0x102 && link_state.smp2p.in==in);
    uint32_t value=0,changed=0;
    qcom_smp2p_service(&link_state.smp2p,"slave-kernel",&value,&changed);
    assert(link_state.smp2p.negotiated && link_state.smp2p.ssr_ack_enabled && value==2 && changed==2);
    assert(!qcom_smp2p_service(&link_state.smp2p,"slave-kernel",&value,&changed) && changed==0);
    in[4]=QCOM_SMP2P_FLAG_RESTART_DONE;
    assert(qcom_smp2p_service(&link_state.smp2p,"slave-kernel",&value,&changed)==1);
    assert(changed==2 && value==2 && (link_state.smp2p.out[4]&QCOM_SMP2P_FLAG_RESTART_ACK));
    assert(!qcom_smp2p_service(&link_state.smp2p,"slave-kernel",&value,&changed) && changed==0);
    memcpy(before,memory,sizeof(memory));
    assert(qcom_smp2p_init_out_version(&link_state.smp2p,link_state.smp2p.out,0,19,3)==-2);
    assert(!memcmp(before,memory,sizeof(memory)));
    setup();in=inbound(2);in[1]=0x102;(void)add_item(480,0x4000);
    assert(np_soccp_attach(&link_state,smem.base,smem.size,&io)==0);
    in[1]=1; /* explicit peer downgrade before feature negotiation */
    assert(qcom_smp2p_service(&link_state.smp2p,"slave-kernel",&value,&changed)==1);
    assert((link_state.smp2p.out[1]&255)==1 && !link_state.smp2p.negotiated);
    qcom_smp2p_service(&link_state.smp2p,"slave-kernel",&value,&changed);
    assert(link_state.smp2p.negotiated && !link_state.smp2p.ssr_ack_enabled);
    setup();(void)inbound(2);deny_lock=1;
    memcpy(before,memory,sizeof(memory));assert(np_soccp_attach(&link_state,smem.base,smem.size,&io)==-1);
    assert(!memcmp(before,memory,sizeof(memory)) && locks==1 && !bells);
    puts("PASS: pending/fatal/invalid remote never writes; ready attaches once; lock failure does not mutate SMEM");
    return 0;
}
