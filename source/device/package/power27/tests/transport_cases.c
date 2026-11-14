/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Prepared, NOT executed. Link with the existing host runner after package selection. */
#include "QcomGlink.h"
#include "QcomSmem.h"
#include "NezhaMca.h"
#include "NezhaPowerGpi.h"
#include "NezhaCharge.h"
#include <assert.h>
#include <string.h>

static unsigned received;
static void receive(void *ctx, QcomGlinkChannel *ch, const uint8_t *p, uint32_t n)
{ (void)ctx; (void)ch; assert(n == 4 && p[0] == 42); ++received; }
static void done_backpressure(void)
{
    uint32_t th = 0, tt = 8, rh = 24, rt = 0;
    uint8_t tx[128] = {0}, rx[128] = {0}, storage[64] = {0};
    QcomGlink g;
    QcomGlinkFifo tf = {&th,&tt,tx,sizeof(tx)}, rf = {&rh,&rt,rx,sizeof(rx)};
    qcom_glink_attach(&g, tf, rf, 0, 0);
    assert(tt == 8 && rh == 24 && rt == 0); /* attachment preserves indices */
    g.version_sent = g.version_agreed = 1;
    QcomGlinkClient client = {0,receive};
    QcomGlinkChannel *c = qcom_glink_channel(&g, "test", 64, 1, storage, &client, 0, 1);
    assert(c); c->rcid = 9; c->up = c->open_acked = c->remote_open = c->open_sent = 1;
    c->local[0].advertised = 1;
    np_put32(rx, QCOM_GLINK_TX_DATA | (9u << 16)); np_put32(rx + 4, 1);
    np_put32(rx + 8, 4); rx[16] = 42;
    received = 0;
    assert(qcom_glink_poll(&g) == 1 && received == 1);
    assert(c->local[0].done_pending && c->local[0].busy && rt == 24);
    tt = th; /* remote drained the TX FIFO; retry the ACK, never the callback */
    assert(qcom_glink_poll(&g) == 0 && received == 1);
    assert(!c->local[0].done_pending && !c->local[0].busy);
    assert((np_le32(tx) & 0xffff) == QCOM_GLINK_RX_DONE_W_REUSE);
    rh = sizeof(rx); /* remote corrupt index must not become 'empty' */
    assert(qcom_glink_poll(&g) == QcomGlinkCorrupt);
}
static void corrupted_smem(void)
{
    uint32_t words[0x2000 / 4] = {0};
    QcomSmem s = {(volatile uint8_t *)words, sizeof(words)};
    QcomSmemPartition p = {0x100,0x100,8,0,19};
    words[0x100 / 4] = 0x54525024; words[0x108 / 4] = 0x100;
    words[0x10c / 4] = 0x40000000; words[0x110 / 4] = 0x100;
    uint32_t offset = 0, size = 0;
    assert(qcom_smem_find(&s,&p,478,&offset,&size) == QcomSmemCorrupt);
    assert(qcom_smem_alloc(&s,&p,478,32) == QcomSmemCorrupt);
}
static void remote_intent_zero(void)
{
    uint32_t th=0,tt=0,rh=96,rt=0;
    uint8_t tx[256]={0},rx[256]={0},storage[64]={0},message[64]={0};
    QcomGlink g;
    QcomGlinkFifo tf={&th,&tt,tx,sizeof(tx)},rf={&rh,&rt,rx,sizeof(rx)};
    qcom_glink_attach(&g,tf,rf,0,0);g.version_sent=g.version_agreed=1;
    const QcomGlinkClient client={0,0};
    QcomGlinkChannel *c=qcom_glink_channel(&g,"test",64,1,storage,&client,0,1);
    assert(c);c->rcid=9;c->up=c->open_acked=c->remote_open=c->open_sent=1;
    c->local[0].advertised=1;
    /* Eleven legal intents, including opaque remote ID0. The 96B size also
     * exercises a full table announcement; it is not a captured phone packet. */
    np_put32(rx,QCOM_GLINK_INTENT|(9u<<16));np_put32(rx+4,11);
    for(unsigned i=0;i<11;++i){np_put32(rx+8+i*8,1024);np_put32(rx+12+i*8,i);}
    assert(qcom_glink_poll(&g)==1 && !g.broken && rt==96 && g.remote_zero_intents==1);
    assert(qcom_glink_send(&g,c,message,sizeof(message))==0);
    assert(np_le32(tx+4)==0 && c->remote[0].busy);
    tt=th;
    np_put32(rx+96,QCOM_GLINK_INTENT|(9u<<16));np_put32(rx+100,1);
    np_put32(rx+104,1024);np_put32(rx+108,0);rh=112;
    assert(qcom_glink_poll(&g)==1 && c->remote[0].busy && g.remote_zero_intents==1);
    np_put32(rx+112,QCOM_GLINK_RX_DONE_W_REUSE|(9u<<16));np_put32(rx+116,0);rh=120;
    assert(qcom_glink_poll(&g)==1 && !c->remote[0].busy && c->remote[0].valid);
    assert(qcom_glink_send(&g,c,message,sizeof(message))==0 && c->remote[0].busy);
    tt=th;
    np_put32(rx+120,QCOM_GLINK_INTENT|(9u<<16));np_put32(rx+124,1);
    np_put32(rx+128,512);np_put32(rx+132,0);rh=136;
    assert(qcom_glink_poll(&g)==QcomGlinkCorrupt);
    assert(!strcmp(g.fault_reason,"remote-intent-resize") && g.fault_a==0 && g.fault_b==512);
    assert(rt==120 && c->remote[0].busy); /* refused packet never releases the in-flight buffer */
    unsigned last=(g.trace_next+15)%16;
    assert(g.trace[last].cmd==QCOM_GLINK_INTENT && g.trace[last].available==16);
}
static void pump_and_tre(void)
{
    PgpiI2cClock clock = {2,5,11,22,3};
    PgpiTre t = pgpi_i2c_config0(&clock);
    assert(t.w[0] == (3u << 24 | 22u << 16 | 5u << 8 | 11u));
    assert(t.w[2] == (3u << 16 | 2));
    const uint8_t command = 0x08;
    t = pgpi_imm(&command,1); assert(t.w[0] == 8 && t.w[2] == 1);
    uint8_t control[13] = {0}, adc[18] = {0}; NpPump p;
    uint16_t valid = (1u<<1)|(1u<<4)|(1u<<11)|(1u<<12);
    assert(np_sc8585_decode(&p,0x41,control,valid,adc) < 0);
    assert(np_sc8585_decode(&p,0x85,control,valid,adc) == 0 && !p.valid);
    control[11] = 0x80; adc[0] = 4; adc[1] = 0; /* IBUS1024 * 1.875mA */
    adc[2] = 4; adc[3] = 0; /* VBUS1024 * 6.25mV */
    assert(np_sc8585_decode(&p,0x85,control,valid,adc) == 0);
    assert(p.bus_ma == 1920 && p.bus_mv == 6400 && !p.switching_valid);
    control[12] = 0xff;
    assert(np_sc8585_decode(&p,0x85,control,valid,adc) == 0 && p.valid == 1);
}
int main(void) { done_backpressure(); corrupted_smem(); remote_intent_zero(); pump_and_tre(); return 0; }
