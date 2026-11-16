/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Production request generator with synthetic CMDDB/RSC, not hardware audio. */
#include "../platform/NezhaMicPower.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t db[0x20000];
static uint32_t registers[0x2000/4];
static unsigned acquired,released,written,fail_write,busy;
static const uint32_t base=0xd00+2*0x2a0;
static void le16(uint8_t *p,uint16_t n) {p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void le32(uint8_t *p,uint32_t n) {le16(p,(uint16_t)n);le16(p+2,(uint16_t)(n>>16));}
static void database(void) {
    memset(db,0,sizeof db);le32(db+4,QCOM_CMD_DB_MAGIC);
    le16(db+8,4);le16(db+12,72);le16(db+14,3);le16(db+16,0x100);
    const char *names[]={"L15B_E0","B1B_E0","L1G_E0"};
    const uint32_t addr[]={0x40b00,0x40000,0x42300};
    for (unsigned i=0;i<3;++i) {
        uint8_t *e=db+144+24*i;memcpy(e,names[i],strlen(names[i]));le32(e+16,addr[i]);
        le16(e+20,4);le16(e+22,(uint16_t)(4*i));le32(db+216+4*i,0x4401+0x100*i);
    }
}
static int acquire(void *v) {(void)v;if (busy) {--busy;return NA_BUSY;}++acquired;return NA_OK;}
static int release(void *v) {(void)v;++released;return NA_OK;}
static int read_reg(void *v,uint32_t at,uint32_t *n) {(void)v;assert(at<sizeof registers);*n=registers[at/4];return NA_OK;}
static int write_reg(void *v,uint32_t at,uint32_t n) {
    (void)v;assert(at<sizeof registers);++written;
    if (at==0xd08) registers[0xd04/4]&=~n;else registers[at/4]=n;
    return written==fail_write ? NA_IO:NA_OK; /* an error may follow an applied write */
}
static NaMicPower fresh(void) {
    database();memset(registers,0,sizeof registers);acquired=released=written=fail_write=busy=0;
    registers[0]=0x40300;registers[3]=0x800c8104;registers[(base+0x28)/4]=1;
    NaMicPower p={0};NaMicPowerOps ops={0,acquire,release,read_reg,write_reg};
    assert(!na_mic_power_init(&p,db,sizeof db,&ops));return p;
}
static void complete(void) {
    registers[0xd04/4]|=4;
    for (unsigned i=0;i<3;++i) registers[(base+0x40+24*i)/4]=0x10100;
}
int main(void) {
    NaMicPower p=fresh();assert(!na_mic_power_request(&p,0));assert(!na_mic_power_poll(&p,0) && !written);
    assert(!na_mic_power_request(&p,1));busy=2;
    assert(na_mic_power_poll(&p,0)==NA_PENDING && !written && !acquired);
    assert(na_mic_power_poll(&p,1)==NA_PENDING && !written && !acquired);
    assert(na_mic_power_poll(&p,2)==NA_PENDING && acquired==1 && written==15 && p.busy && !p.held);
    assert(registers[(base+0x38)/4]==0x40b00 && registers[(base+0x3c)/4]==1800);
    assert(registers[(base+0x50)/4]==0x40b08 && registers[(base+0x54)/4]==5);
    assert(registers[(base+0x68)/4]==0x40b04 && registers[(base+0x6c)/4]==1);
    complete();registers[0xd04/4]=0;
    assert(na_mic_power_poll(&p,3)==NA_PENDING && !p.held && !released);
    complete();registers[(base+0x58)/4]=0;
    assert(na_mic_power_poll(&p,4)==NA_PENDING && !p.held && !released);
    complete();registers[(base+0x28)/4]=0;
    assert(na_mic_power_poll(&p,5)==NA_PENDING && !p.held && !released);
    registers[(base+0x28)/4]=1;
    assert(na_mic_power_poll(&p,102)==NA_PENDING && p.held==1 && released==1 && acquired==2);
    assert(registers[(base+0x38)/4]==0x40000 && registers[(base+0x3c)/4]==3400);
    assert(registers[(base+0x54)/4]==7 && registers[(base+0x6c)/4]==1);
    complete();assert(na_mic_power_poll(&p,103)==NA_PENDING && p.held==3 && released==2);
    assert(registers[(base+0x38)/4]==0x42300 && registers[(base+0x3c)/4]==1200);
    assert(registers[(base+0x54)/4]==5);
    complete();assert(!na_mic_power_poll(&p,104) && p.held==7 && p.possible==7 && released==3);
    unsigned before=written;
    assert(!na_mic_power_request(&p,0) && !na_mic_power_poll(&p,105) && written==before && p.held==7);
    assert(!na_mic_power_request(&p,1) && !na_mic_power_poll(&p,106) && written==before);
    for (unsigned fail=1;fail<=15;++fail) {
        p=fresh();fail_write=fail;assert(!na_mic_power_request(&p,1));
        assert(na_mic_power_poll(&p,10)==NA_IO && p.busy && p.possible==1 && !p.held && !released);
        before=written;assert(na_mic_power_poll(&p,11)==NA_IO && written==before);
        assert(na_mic_power_request(&p,0)==NA_IO && written==before);
    }
    p=fresh();assert(!na_mic_power_request(&p,1));assert(na_mic_power_poll(&p,10)==NA_PENDING);
    assert(na_mic_power_poll(&p,110)==NA_TIMEOUT && p.busy && p.possible==1 && !released);
    before=written;assert(na_mic_power_poll(&p,111)==NA_TIMEOUT && written==before);
    p=fresh();assert(!na_mic_power_request(&p,1));assert(na_mic_power_poll(&p,0)==NA_PENDING);
    assert(!na_mic_power_request(&p,0));complete();
    assert(!na_mic_power_poll(&p,1) && p.held==1 && released==1 && acquired==1);
    p=fresh();NaMicPowerOps ops=p.ops;NaMicPower bad={0};
    le32(db+144+16,0x40c00);assert(na_mic_power_init(&bad,db,sizeof db,&ops)==NA_MISSING && !written);
    database();registers[0]=0x30300;assert(na_mic_power_init(&bad,db,sizeof db,&ops)==NA_MISSING && !written);
    puts("PASS: exact VRM addresses/units/order, shared floor retained, fresh IRQ+all ACKs+idle, completion at deadline, lease wait, 15 partial-write failures, timeout and no mutating replay; RSC is synthetic");
}
