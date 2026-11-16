/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Tests added for the local changes; no hardware, not compiled in source phase. */
#include "../upstream/QcomGlink.h"
#include <assert.h>
#include <string.h>
#include <stdint.h>
static QcomGlink g;static uint32_t th,tt,rh,rt,delivered;
static uint8_t tx[256],rx[256],storage[64];static QcomGlinkChannel *ch;
static unsigned headers,errors;static uint32_t last_attempt;
static void header(void *ctx,uint32_t attempt,uint16_t cmd,uint16_t p1,uint32_t p2,int result) {
    assert(ctx==&g);last_attempt=attempt;
    if (!result) {++headers;assert(headers<=16 && cmd==QCOM_GLINK_SIGNALS && p1==7 && p2==0x12345678);}
    else {++errors;assert(result==QcomGlinkCorrupt && cmd==0xffff && p1==9 && p2==0xfedcba98);}
}
static void received(void *ctx,QcomGlinkChannel *c,const uint8_t *p,uint32_t n) {
    (void)ctx;(void)c;(void)p;assert(n<=64);++delivered;
}
static void setup(void) {
    th=tt=rh=rt=delivered=0;memset(tx,0,sizeof tx);memset(rx,0,sizeof rx);
    QcomGlinkFifo a={&th,&tt,tx,sizeof tx},b={&rh,&rt,rx,sizeof rx};
    qcom_glink_init(&g,a,b,0,0);QcomGlinkClient cb={0,received};
    ch=qcom_glink_channel(&g,"adsp_apps",64,1,storage,&cb,0,0);assert(ch);
    g.version_sent=g.version_agreed=1;g.features=QCOM_GLINK_FEATURE_INTENT_REUSE;
    ch->rcid=7;ch->open_sent=ch->open_acked=ch->remote_open=ch->up=1;ch->local[0].advertised=1;
}
static void wr(uint8_t *p,uint32_t n) {p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24;}
static void fragment(uint32_t command,uint32_t chunk,uint32_t remaining) {
    uint8_t m[24]={0};assert(chunk<=8);m[0]=(uint8_t)command;m[2]=7;
    wr(m+4,1);wr(m+8,chunk);wr(m+12,remaining);
    for (unsigned i=0;i<sizeof m;++i) {rx[rh]=m[i];rh=(rh+1)%sizeof rx;}
}
int main(void) {
    setup();th=232; /* No room for RX_DONE; remote must still receive it later. */
    fragment(QCOM_GLINK_TX_DATA,8,0);
    assert(qcom_glink_poll(&g)>=0 && delivered==1 && ch->local[0].done_pending);
    tt=th;assert(qcom_glink_poll(&g)>=0 && delivered==1 && !ch->local[0].done_pending);
    assert(tx[232]==QCOM_GLINK_RX_DONE_W_REUSE);
    setup();fragment(QCOM_GLINK_TX_DATA,4,4);
    assert(qcom_glink_poll(&g)>=0 && !delivered && ch->local[0].remaining==4);
    fragment(QCOM_GLINK_TX_DATA_CONT,4,1); /* Must equal previous remaining. */
    assert(qcom_glink_poll(&g)==QcomGlinkCorrupt && !delivered);
    setup();QcomGlinkClient cb={0,received};
    assert(!qcom_glink_channel(&g,"adsp_apps",64,1,storage,&cb,0,0));
    assert(!qcom_glink_channel(&g,"overflow",UINT32_MAX,2,storage,&cb,0,0));
    th=3;assert(qcom_glink_poll(&g)==QcomGlinkCorrupt);
    setup();g.observe_context=&g;g.observe_header=header;headers=errors=0;
    for (unsigned i=0;i<20;++i) {unsigned at=8*i;rx[at]=QCOM_GLINK_SIGNALS;rx[at+2]=7;wr(rx+at+4,0x12345678);}
    rh=160;assert(qcom_glink_poll(&g)==20 && rt==160 && g.rx_messages==20);
    assert(headers==16 && !errors && g.header_attempts==20);
    rx[160]=rx[161]=0xff;rx[162]=9;wr(rx+164,0xfedcba98);rh=168;
    assert(qcom_glink_poll(&g)==QcomGlinkCorrupt && rt==160 && g.rx_messages==20);
    assert(headers==16 && errors==1 && last_attempt==21 && g.broken);
    assert(qcom_glink_poll(&g)==QcomGlinkCorrupt && errors==1); /* Terminal report only once. */
    setup();g.observe_header=header;g.observe_context=&g;headers=errors=0;
    rx[0]=QCOM_GLINK_SIGNALS;rx[2]=7;wr(rx+4,0x12345678);rh=0;
    assert(qcom_glink_poll(&g)==0 && !headers && !g.header_attempts); /* No complete header. */
    setup();rx[0]=QCOM_GLINK_INTENT;rx[2]=7;wr(rx+4,1);wr(rx+8,64);wr(rx+12,0);rh=16;
    assert(qcom_glink_poll(&g)==1 && ch->remote[0].valid && !ch->remote[0].id);
    unsigned sent_at=th;uint8_t payload[4]={1,2,3,4};
    assert(qcom_glink_send(&g,ch,payload,sizeof payload)==QcomGlinkOk && ch->remote[0].busy);
    assert(tx[sent_at]==QCOM_GLINK_TX_DATA && !tx[sent_at+4] && !tx[sent_at+5] && !tx[sent_at+6] && !tx[sent_at+7]);
    rx[16]=QCOM_GLINK_RX_DONE_W_REUSE;rx[18]=7;wr(rx+20,0);rh=24;
    assert(qcom_glink_poll(&g)==1 && ch->remote[0].valid && !ch->remote[0].busy);
    assert(qcom_glink_send(&g,ch,payload,sizeof payload)==QcomGlinkOk);
    rx[24]=QCOM_GLINK_RX_DONE;rx[26]=7;wr(rx+28,0);rh=32;
    assert(qcom_glink_poll(&g)==1 && !ch->remote[0].valid && !ch->remote[0].busy);
    setup();rx[0]=QCOM_GLINK_INTENT;rx[2]=7;wr(rx+4,1);wr(rx+8,0);wr(rx+12,0);rh=16;
    assert(qcom_glink_poll(&g)==QcomGlinkCorrupt && rt==0); /* Zero size remains invalid. */
    setup();ch->remote[0].id=0;ch->remote[0].size=64;ch->remote[0].valid=ch->remote[0].busy=1;
    rx[0]=QCOM_GLINK_INTENT;rx[2]=7;wr(rx+4,1);wr(rx+8,64);wr(rx+12,0);rh=16;
    assert(qcom_glink_poll(&g)==QcomGlinkCorrupt && rt==0); /* No replacement of a busy ID. */
    return 0;
}
