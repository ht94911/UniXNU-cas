/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Test the real common-owner/proxy callbacks, not a substitute acquire stub. */
#include "../platform/AudioResourceOwner.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t reg[0x2000/4];
static unsigned locked,deny_irq_write;
static int lock(void *v) {(void)v;assert(!locked);locked=1;return 0;}
static void unlock(void *v) {(void)v;assert(locked);locked=0;}
static int read32(void *v,unsigned domain,uint32_t at,uint32_t *out) {
    (void)v;assert(locked && domain==Sm8850Rsc && at<sizeof reg);*out=reg[at/4];return 0;
}
static int write32(void *v,unsigned domain,uint32_t at,uint32_t value) {
    (void)v;assert(locked && domain==Sm8850Rsc && at<sizeof reg);
    if (at==0xd00 && deny_irq_write) return -1;
    if (at==0xd08) reg[0xd04/4]&=~value;
    else reg[at/4]=value;
    if (at==0xfc4 && (value&(1u<<24))) {reg[0xfc8/4]=0;reg[0xfe0/4]=0;}
    return 0;
}
static int read64(void *v,unsigned d,uint32_t at,uint64_t *out) {(void)v;(void)d;(void)at;(void)out;assert(0);return -1;}
static int fence(void *v) {(void)v;assert(locked);return 0;}
static void begin(Sm8850Resources *common,NaResourceOwner *owner,NaProxyPower *p,unsigned mask) {
    memset(common,0,sizeof *common);memset(owner,0,sizeof *owner);memset(p,0,sizeof *p);memset(reg,0,sizeof reg);
    locked=deny_irq_write=0;
    Sm8850ResourceIo io={0,lock,unlock,read32,write32,read64,fence};
    assert(!sm8850_resources_init(common,&io));assert(!na_resource_owner_init(owner,common));
    reg[0]=0x40300;reg[3]=0x800c8104;reg[0xd00/4]=mask;reg[0xfc8/4]=1;reg[0xfe0/4]=0x10101;
    reg[0xd04/4]=1; /* foreign TCS0 completion must not be cleared */
    p->ops=na_resource_owner_proxy(owner);p->tcs=1;p->layout=qcom_rsc_layout(0x40300);
    p->resource[0].address=0x123400;p->values[0]=0x60003fff;assert(!na_proxy_request(p,1));
}
int main(void) {
    const unsigned masks[]={0,1,3,0xa0000005};
    for (unsigned i=0;i<4;++i) {
        Sm8850Resources common;NaResourceOwner owner;NaProxyPower p;begin(&common,&owner,&p,masks[i]);
        assert(na_proxy_poll(&p,0)==NA_PENDING && p.lease && !p.held);
        assert(reg[0xd00/4]==(masks[i]|2) && reg[0xd04/4]==1 && owner.tcs_irq_saved);
        assert(na_proxy_poll(&p,1)==NA_PENDING && !p.held);
        reg[0xfc8/4]=1;reg[0xfe0/4]=0x10101; /* same ACK/idle observed in010 */
        assert(na_proxy_poll(&p,2)==NA_PENDING && !p.held && owner.tcs_lease);
        reg[0xd04/4]|=2;reg[0xd00/4]|=0x80; /* unrelated bit changes while lease is held */
        assert(na_proxy_poll(&p,3)==NA_PENDING && p.held==1 && !p.lease && !owner.tcs_lease);
        assert(reg[0xd00/4]==(masks[i]|0x80) && reg[0xd04/4]==1 && !owner.tcs_irq_saved);
    }
    Sm8850Resources common;NaResourceOwner owner;NaProxyPower p;begin(&common,&owner,&p,1);
    assert(na_proxy_poll(&p,10)==NA_PENDING);assert(na_proxy_poll(&p,110)==NA_TIMEOUT);
    assert(owner.tcs_lease && owner.tcs_irq_saved && reg[0xd00/4]==3 && !p.held);
    assert(na_proxy_poll(&p,111)==NA_TIMEOUT && owner.tcs_lease);
    begin(&common,&owner,&p,1);deny_irq_write=1;
    assert(na_proxy_poll(&p,0)==NA_IO && owner.tcs_lease && !p.held && reg[0xd00/4]==1);
    puts("PASS: TCS1 IRQ lease preserves other bits, waits for fresh completion, restores on success and retains uncertain ownership");
}
