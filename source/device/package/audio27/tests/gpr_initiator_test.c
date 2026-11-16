/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Production transport/GLINK with a peer that waits for the local OPEN. */
#include "../core/AudioTransport.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static QcomGlink apps,dsp;static NaTransport transport;static NaRxQueue inbox;
static uint32_t ah,at,dh,dt;static uint8_t a[4096],d[4096],storage[4096];
static unsigned remote_up;
static void state(void *v,QcomGlinkChannel *c,int up) {(void)v;(void)c;remote_up=!!up;}
static void receive(void *v,QcomGlinkChannel *c,const uint8_t *data,uint32_t bytes) {(void)v;(void)c;(void)data;(void)bytes;}
static void setup(unsigned initiate) {
    ah=at=dh=dt=remote_up=0;memset(&inbox,0,sizeof inbox);
    QcomGlinkFifo tx={&ah,&at,a,sizeof a},rx={&dh,&dt,d,sizeof d};
    qcom_glink_init(&apps,tx,rx,0,0);qcom_glink_init(&dsp,rx,tx,0,0);
    assert(!na_transport_init(&transport,&apps,&inbox,initiate));
    const QcomGlinkClient client={state,receive};
    assert(qcom_glink_channel(&dsp,"adsp_apps",512,8,storage,&client,0,0));
    for (unsigned i=0;i<32;++i) {assert(na_transport_poll(&transport)>=0);assert(qcom_glink_poll(&dsp)>=0);}
}
int main(void) {
    setup(0);assert(apps.version_agreed && dsp.version_agreed && !transport.online && !remote_up);
    setup(1);assert(transport.online && remote_up && transport.channel->initiator && transport.epoch==2);
    NaRxPacket event;assert(!na_rx_pop(&inbox,&event) && !event.length && event.data[0]==1 && event.generation==2);
    assert(na_rx_pop(&inbox,&event)==NA_PENDING);
    assert(na_transport_init(&transport,&apps,&inbox,2)==NA_ARGUMENT);
    puts("GPR initiator PASS: production version/open/ack/intent and ordered link event with passive peer; no hardware");
    return 0;
}
