/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Real state machines with synthetic shared state; no hardware/firmware I/O. */
#include "../platform/AdspLink.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static NaAdsp a;static NaRxQueue queue;
static uint32_t th,tt,rh,rt,off,events[10];static int off_result;
static uint8_t tx[512],rx[512];
static int proxy(void *v,int on) {(void)v;assert(!on);++off;return off_result;}
static void observed(void *v,uint32_t point,int result) {
    (void)v;(void)result;assert(point>0 && point<10);++events[point];
}
static void setup(unsigned retain) {
    memset(&a,0,sizeof a);memset(&queue,0,sizeof queue);memset(events,0,sizeof events);
    th=tt=rh=rt=off=0;off_result=0;
    a.ops.proxy=proxy;a.ops.observe=observed;a.stage=NaAdspOnline;
    a.retain_proxy=retain;a.proxy_possible=1;a.doorbells=1;
    QcomGlinkFifo t={&th,&tt,tx,sizeof tx},r={&rh,&rt,rx,sizeof rx};
    qcom_glink_init(&a.glink,t,r,0,0);assert(!na_transport_init(&a.transport,&a.glink,&queue,0));
    assert(!na_pd_mapper_init(&a.mapper,&a.glink));
    a.glink_initialized=1; /* This fixture represents an already established link. */
}
int main(void) {
    setup(1);a.slave=2;
    for (unsigned n=0;n<100;++n) assert(!na_adsp_poll(&a,100+n));
    assert(!off && !a.handed && !a.handover_seen && a.proxy_possible);
    assert(events[NaAdspGlinkBegin]==1 && events[NaAdspGlinkReturn]==1);
    assert(!events[NaAdspProxyRetained] && a.glink.version_sent); /* Communication continues. */
    a.slave=6;
    for (unsigned n=0;n<100;++n) assert(!na_adsp_poll(&a,200+n));
    assert(a.handover_seen && !off && !a.handed && a.proxy_possible);
    assert(events[NaAdspProxyRetained]==1 && !events[NaAdspProxyOffBegin]);
    a.slave=7;assert(na_adsp_poll(&a,300)==NA_REMOTE && a.stage==NaAdspFailed);
    assert(!off && a.proxy_possible); /* Failure must not drop retained power. */
    setup(0);a.slave=6;off_result=NA_PENDING;
    assert(!na_adsp_poll(&a,100) && off==1 && !a.handed && a.proxy_possible);
    off_result=0;assert(!na_adsp_poll(&a,101) && off==2 && a.handed && !a.proxy_possible);
    assert(!na_adsp_poll(&a,102) && off==2);
    assert(events[NaAdspProxyOffBegin]==1 && events[NaAdspProxyOffReturn]==1);
    setup(0);a.slave=6;off_result=NA_IO;
    assert(na_adsp_poll(&a,100)==NA_IO && !a.handed && a.proxy_possible && a.cleanup_error==NA_IO);
    assert(na_adsp_poll(&a,101)==NA_IO && off==1); /* No replay. */
    setup(1);a.stage=NaAdspWaitReady;a.slave=6;a.doorbells=a.auth_doorbells;
    assert(na_adsp_poll(&a,100)==NA_PENDING && !a.handover_seen && a.stage==NaAdspWaitReady);
    na_adsp_smp2p_event(&a);
    assert(!na_adsp_poll(&a,101) && a.handover_seen && !a.handed && !off);
    puts("ADSP post-ready: retention, fresh handover, normal release, errors and bounded observation PASS");
    return 0;
}
