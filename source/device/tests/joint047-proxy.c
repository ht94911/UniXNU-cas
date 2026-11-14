/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Real ALS proxy and common lease code; simulated RSC completion only. */
#include "../package/als-owner/platform/AdspPower.h"
#include "../package/NezhaJointResources.h"
#include <Sm8850Resources.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t registers[0x2000/4], floor_level;
static unsigned released, write_count;
static Sm8850Resources resources;
static int locked;
unsigned nezha_wcn_lmx_level(void) { return floor_level; }
static int lock(void *p) { (void)p; assert(!locked); locked=1; return 0; }
static void unlock(void *p) { (void)p; assert(locked); locked=0; }
static int read32(void *p,unsigned d,uint32_t at,uint32_t *v)
{ (void)p; (void)d; *v=registers[at/4]; return 0; }
static int read64(void *p,unsigned d,uint32_t at,uint64_t *v)
{ (void)p; (void)d; (void)at; *v=0; return 0; }
static int write32(void *p,unsigned d,uint32_t at,uint32_t value)
{
    (void)p; (void)d; ++write_count;
    if (at==0xd08) registers[0xd04/4]&=~value;
    else registers[at/4]=value;
    return 0;
}
static int barrier(void *p) { (void)p; return 0; }
static int acquire(void *p,uint32_t kind,uint64_t *lease)
{
    (void)p; assert(kind==NaPowerTcs);
    int e=sm8850_resource_acquire(&resources,NezhaJointRscTransaction,0x4e41,lease);
    return e==Sm8850Busy ? NA_BUSY : e;
}
static int release(void *p,uint32_t kind,uint64_t lease)
{
    (void)p; assert(kind==NaPowerTcs); ++released;
    return sm8850_resource_release(&resources,NezhaJointRscTransaction,0x4e41,lease,1);
}
static uint32_t read_reg(void *p,uint32_t at)
{ (void)p; assert(at<sizeof registers); return registers[at/4]; }
static void write_reg(void *p,uint32_t at,uint32_t value)
{ (void)write32(p,0,at,value); }
static void kick(void *p) { (void)p; assert(0); }
static void init(NaProxyPower *p)
{
    memset(p,0,sizeof *p); memset(registers,0,sizeof registers);
    memset(&resources,0,sizeof resources); released=write_count=0; locked=0;
    Sm8850ResourceIo io={0,lock,unlock,read32,write32,read64,barrier};
    assert(!sm8850_resources_init(&resources,&io));
    p->ops=(NaProxyOps){0,acquire,release,read_reg,write_reg,kick};
    p->tcs=1; p->layout=qcom_rsc_layout(0x40300);
    p->resource[2].address=0x30040;
    p->resource[2].level_count=4;
    p->resource[2].levels[0]=0; p->resource[2].levels[1]=128;
    p->resource[2].levels[2]=256; p->resource[2].levels[3]=384;
    p->values[2]=3;
    registers[(0xd00+0x2a0+0x28)/4]=1;
    registers[(0xd00+0x2a0+0x40)/4]=0x10100;
    registers[0xd04/4]=2;
}
int main(void)
{
    NaProxyPower p; init(&p); floor_level=384;
    /* The production state values are entered through normal request/poll;
     * PReleaseLmx is selected after the complete preceding lcx release. */
    p.stage=5; p.want=1; p.held=6;
    assert(!na_proxy_request(&p,0));
    assert(na_proxy_poll(&p,0)==NA_PENDING && p.busy);
    registers[0xd04/4]=2;
    assert(na_proxy_poll(&p,1)==NA_PENDING && !p.busy);
    assert(na_proxy_poll(&p,2)==NA_PENDING && p.busy);
    assert(registers[(0xd00+0x2a0+0x3c)/4]==3);
    uint64_t radio_lease=0;
    assert(sm8850_resource_acquire(&resources,NezhaJointRscTransaction,
                                  NezhaWcnResourceOwner,&radio_lease)==Sm8850Busy);
    registers[0xd04/4]=2;
    assert(na_proxy_poll(&p,3)==NA_PENDING && !p.busy);
    assert(!sm8850_resource_acquire(&resources,NezhaJointRscTransaction,
                                    NezhaWcnResourceOwner,&radio_lease));
    unsigned before=write_count;
    assert(na_proxy_poll(&p,4)==NA_PENDING && write_count==before);
    assert(!sm8850_resource_release(&resources,NezhaJointRscTransaction,
                                   NezhaWcnResourceOwner,radio_lease,1));
    assert(na_proxy_poll(&p,5)==NA_PENDING && p.busy);
    assert(na_proxy_poll(&p,105)==NA_TIMEOUT && p.lease);
    radio_lease=0;
    assert(sm8850_resource_acquire(&resources,NezhaJointRscTransaction,
                                  NezhaWcnResourceOwner,&radio_lease)==Sm8850Busy);

    init(&p); floor_level=0; p.stage=5; p.want=1; p.held=6;
    assert(!na_proxy_request(&p,0));
    assert(na_proxy_poll(&p,0)==NA_PENDING);
    registers[0xd04/4]=2; assert(na_proxy_poll(&p,1)==NA_PENDING);
    assert(na_proxy_poll(&p,2)==NA_PENDING);
    assert(registers[(0xd00+0x2a0+0x3c)/4]==0);
    puts("PASS: real ALS proxy preserves WCN floor, mutual transaction leases, pending-no-write, timeout retention and original zero-floor behavior");
}
