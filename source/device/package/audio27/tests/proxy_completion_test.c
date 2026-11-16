/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../platform/AdspPower.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t registers[0x2000/4];static unsigned released;
static int acquire(void *v,uint32_t r,uint64_t *lease) {(void)v;assert(r==NaPowerTcs);*lease=1;return 0;}
static int release(void *v,uint32_t r,uint64_t lease) {(void)v;assert(r==NaPowerTcs && lease==1);++released;return 0;}
static uint32_t read_reg(void *v,uint32_t at) {(void)v;assert(at<sizeof registers);return registers[at/4];}
static void write_reg(void *v,uint32_t at,uint32_t value) {(void)v;assert(at<sizeof registers);if(at==0xd08)registers[0xd04/4]&=~value;else registers[at/4]=value;}
static void kick(void *v) {(void)v;assert(0);}
static void init(NaProxyPower *p) {
    memset(p,0,sizeof *p);memset(registers,0,sizeof registers);released=0;
    p->ops=(NaProxyOps){0,acquire,release,read_reg,write_reg,kick};p->tcs=1;p->layout=qcom_rsc_layout(0x40300);
    p->resource[0].address=0x123400;p->values[0]=0x60003fff;
    registers[(0xd00+0x2a0+0x28)/4]=1;
    registers[(0xd00+0x2a0+0x40)/4]=0x10100; /* stale previous command ACK */
    registers[0xd04/4]=2; /* stale previous completion; request must clear it */
    assert(!na_proxy_request(p,1));
}
int main(void) {
    NaProxyPower p;init(&p);
    assert(na_proxy_poll(&p,0)==NA_PENDING && p.busy && p.lease && !p.held);
    assert(registers[0xd04/4]==0);
    assert(na_proxy_poll(&p,1)==NA_PENDING && p.busy && !p.held && !released);
    registers[0xd04/4]=2;registers[(0xd00+0x2a0+0x40)/4]=0;
    assert(na_proxy_poll(&p,2)==NA_PENDING && p.busy && !p.held && !released);
    registers[(0xd00+0x2a0+0x40)/4]=0x10100;
    assert(na_proxy_poll(&p,3)==NA_PENDING && !p.busy && p.held==1 && !p.lease && released==1);
    init(&p);assert(na_proxy_poll(&p,10)==NA_PENDING);assert(na_proxy_poll(&p,110)==NA_TIMEOUT);
    assert(p.lease && !p.held && !released);assert(na_proxy_poll(&p,111)==NA_TIMEOUT && !released);
    puts("PASS: stale ACK/completion cannot advance proxy; new completion plus ACK required; timeout retains lease");
}
