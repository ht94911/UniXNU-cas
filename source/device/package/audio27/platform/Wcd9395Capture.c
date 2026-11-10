/* SPDX-License-Identifier: GPL-2.0-only */
/* WCD939x codec sequence adapted for nezha's bottom-main route from
 * OnePlus SM8850 5ab2a689ff87d7d28c511f1762cf41c1b90d965a.
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * Original bounded lifecycle/ownership glue is part of this adaptation.
 */
#include "Wcd9395Capture.h"
#include <string.h>
#include <limits.h>
typedef struct {uint16_t reg;uint8_t mask,value;uint32_t delay_ms;} WcdStep;
#include "Wcd9395Fields.inc"
static int tx_update(NaWcdCapture *c,uint32_t reg,uint32_t mask,uint32_t value) {
    uint32_t old=c->tx.read_tx(c->tx.context,reg);
    if (c->tx.error(c->tx.context)) return NA_IO;
    c->tx.write_tx(c->tx.context,reg,(old&~mask)|(value&mask));
    return c->tx.error(c->tx.context) ? NA_IO:NA_OK;
}
static uint16_t mic_reg(const NaWcdCapture *c) {
    static const uint16_t regs[4]={0x3022,0x3023,0x3025,0x3026};return regs[c->micbias-1];
}
int na_wcd_capture_mute(NaWcdCapture *c) {
    if (!c || !c->bus) return NA_ARGUMENT;
    int r=tx_update(c,0x500,0x10,0x10);
    if (!r) {c->muted=1;c->active=0;}return r;
}
static int failed(NaWcdCapture *c,int error) {
    na_wcd_capture_mute(c);c->error=error<0 ? error:NA_IO;return c->error;
}
int na_wcd_capture_init(NaWcdCapture *c,NaSwr *bus,const NaTxIo *tx,uint8_t bias,uint8_t gain) {
    if (!c || !bus || !bus->lease || !bus->device || bus->failed || !tx || !tx->read_tx || !tx->write_tx || !tx->error || bias!=1 || gain>10) return NA_ARGUMENT;
    memset(c,0,sizeof *c);c->bus=bus;c->tx=*tx;c->micbias=bias;c->adc_gain=gain;
    uint8_t id=0;int r=na_swr_read(bus,0x3402,&id);
    if (r || id!=1) return failed(c,r ? r:NA_MISSING); /* WCD939X_2_0 */
    c->version=id;return na_wcd_capture_mute(c);
}
static int ports(NaWcdCapture *c) {
    NaSwr *s=c->bus;int r;
    /* Isolated capture owns these ports in both banks. DT UC0 gives offset0,
     * lane1. ADC_LP data is4.8MHz: interval1 on the9.6MHz bus. Port numbers in
     * DT are zero based; slave logical0 becomes physical DP1. */
    for (unsigned bank=0;bank<2;++bank) {
        uint16_t b=(uint16_t)(bank*0x10);
        const uint16_t reg[7]={0x122,0x123,0x124,0x125,0x126,0x127,0x128};
        const uint8_t value[7]={1,0,0,0,0xff,1,1};
        for (unsigned i=0;i<7;++i) if ((r=na_swr_write(s,reg[i]+b,value[i]))) return r;
    }
    if ((r=na_swr_write(s,0x102,0)) || (r=na_swr_write(s,0x103,0)) || (r=na_swr_write(s,0x105,2))) return r;
    /* Master DP2 channel3 corresponds to SWR_MIC2; slave DP1 retains ADC2's
     * physical channel2. They are intentionally different masks. */
    for (unsigned bank=0;bank<2;++bank) {
        uint32_t b=bank*0x40;
        s->io.write(s->io.context,0x1224+b,1u|(4u<<24));
        s->io.write(s->io.context,0x1228+b,1);
        s->io.write(s->io.context,0x1230+b,0);
        s->io.write(s->io.context,0x1234+b,0xf0);
        s->io.write(s->io.context,0x1238+b,1);
    }
    s->io.write(s->io.context,0x122c,0);
    if ((r=na_swr_write(s,0x120,2)) || (r=na_swr_write(s,0x130,2))) return r;
    return NA_OK;
}
int na_wcd_capture_prepare(NaWcdCapture *c,uint64_t now) {
    if (!c || now>UINT64_MAX-5000) return NA_ARGUMENT;
    if (c->error) return c->error;
    if (c->prepared) return NA_OK;
    uint64_t trace=c->trace_wait_us ? *c->trace_wait_us:0;
    if (!c->deadline) {c->deadline=now+5000;c->trace_base_us=trace;}
    if (trace<c->trace_base_us) return failed(c,NA_IO);
    /* Same owned-counter accounting as TAS. Actual settles and the bounded
     * SoundWire FIFO waits still use real time. Never estimate from USB logs. */
    uint64_t paused_ms=(trace-c->trace_base_us)/1000;
    if (now>=c->deadline && now-c->deadline>=paused_ms) return failed(c,NA_TIMEOUT);
    if (now<c->wait_until) return NA_PENDING;
    int r=0;
    if (c->stage==0) {
        if (c->index<sizeof bias_setup/sizeof bias_setup[0]) {
            const WcdStep *p=&bias_setup[c->index++];r=na_swr_update(c->bus,p->reg,p->mask,p->value);
            c->wait_until=now+p->delay_ms;
        } else {c->stage=1;c->index=0;}
    } else if (c->stage==1) {
        WcdStep steps[]={
            {mic_reg(c),0x3f,0x23,0}, /*2750mV: (2750-1000)/50*/
            {mic_reg(c),0xc0,0x40,2},
            {0x3127,0x38,0x08,0}, /*ADC2 mux: AMIC1*/
            {0x3405,0xff,0x11,0}, /*both banks:4.8MHz*/
            {0x300f,0x20,0x20,0},
            {0x3410,0xf0,0x50,0}, /*ADC_LP, exact Android route*/
            {0x3409,0x20,0x20,0},
            {0x300f,0x20,0x00,0},
            {0x300f,0x1f,c->adc_gain,0}
        };
        if (c->index<sizeof steps/sizeof steps[0]) {
            WcdStep *p=&steps[c->index++];r=na_swr_update(c->bus,p->reg,p->mask,p->value);c->wait_until=now+p->delay_ms;
        } else {c->stage=2;c->index=0;}
    } else if (c->stage==2) {
        r=ports(c);if (!r) {c->stage=3;c->wait_until=now+1;}
    } else if (c->stage==3) {
        uint8_t pending=0;r=na_swr_read(c->bus,0x104,&pending);
        if (r) return failed(c,r);
        if (pending&2) return NA_PENDING;
        if ((r=tx_update(c,0x110,0x0f,3)) || /*SMIC2 enum3*/
            (r=tx_update(c,0x114,3,1)) ||    /*DEC2 source SWR_MIC*/
            (r=tx_update(c,0x504,0x66,0x40)) || /*150Hz HPF*/
            (r=tx_update(c,0x500,0x3f,0x34))) return failed(c,r);
        c->prepared=1;c->wait_until=now+300;return NA_OK;
    } else return failed(c,NA_OWNERSHIP);
    return r ? failed(c,r):NA_PENDING;
}
int na_wcd_capture_activate(NaWcdCapture *c,uint64_t now) {
    if (!c || !c->prepared || c->error) return c && c->error ? c->error:NA_OWNERSHIP;
    if (now<c->wait_until) return NA_PENDING;
    int r=tx_update(c,0x500,0x10,0);if (r) return failed(c,r);
    c->muted=0;c->active=1;return NA_OK;
}
int na_wcd_capture_shutdown(NaWcdCapture *c,int quiescent) {
    if (!c) return NA_ARGUMENT;
    if (!c->bus) return NA_ARGUMENT;
    int r=na_wcd_capture_mute(c);if (!quiescent) return NA_OWNERSHIP;
    if (r || (r=tx_update(c,0x500,0x20,0)) || (r=tx_update(c,0x110,0xf,0)) ||
        (r=tx_update(c,0x114,3,0))) return failed(c,r);
    for (unsigned bank=0;bank<2;++bank) c->bus->io.write(c->bus->io.context,0x1224+0x40*bank,1);
    r=na_swr_write(c->bus,0x120,0);
    if (!r) r=na_swr_write(c->bus,0x130,0);
    if (!r) r=na_swr_write(c->bus,0x105,0);
    if (!r) r=na_swr_update(c->bus,0x3410,0xf0,0);
    if (!r) r=na_swr_update(c->bus,0x3409,0xf0,0);
    if (!r) r=na_swr_update(c->bus,0x3408,0x18,0);
    if (!r) r=na_swr_update(c->bus,mic_reg(c),0xc0,0);
    if (r) return failed(c,r); /* external reset fallback retains resource lease */
    c->prepared=0;c->stage=0;c->index=0;c->deadline=0;return NA_OK;
}
