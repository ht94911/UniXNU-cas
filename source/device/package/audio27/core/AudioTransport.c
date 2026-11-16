/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioTransport.h"
#include <string.h>
#include <limits.h>
static void state(void *v,QcomGlinkChannel *channel,int online) {
    NaTransport *t=v;(void)channel;
    uint32_t old=__atomic_load_n(&t->online,__ATOMIC_RELAXED);
    if (old==(uint32_t)!!online) return;
    if (t->epoch==UINT32_MAX) {__atomic_store_n(&t->fault,1,__ATOMIC_RELEASE);online=0;}
    else ++t->epoch;
    __atomic_store_n(&t->online,(uint32_t)!!online,__ATOMIC_RELEASE);
    if (na_rx_link(t->rx,t->epoch,online)) __atomic_store_n(&t->fault,1,__ATOMIC_RELEASE);
}
static void receive(void *v,QcomGlinkChannel *channel,const uint8_t *data,uint32_t bytes) {
    NaTransport *t=v;(void)channel;
    if (!__atomic_load_n(&t->online,__ATOMIC_ACQUIRE) || na_rx_push(t->rx,t->epoch,data,bytes))
        __atomic_store_n(&t->fault,1,__ATOMIC_RELEASE);
}
int na_transport_init(NaTransport *t,QcomGlink *g,NaRxQueue *rx,unsigned initiate) {
    if (!t || !g || !rx || initiate>1) return NA_ARGUMENT;
    memset(t,0,sizeof *t);t->glink=g;t->rx=rx;t->epoch=1;
    const QcomGlinkClient client={state,receive};
    t->channel=qcom_glink_channel(g,"adsp_apps",GPR_MAX_PACKET,8,t->intents,&client,t,(int)initiate);
    return t->channel ? NA_OK : NA_BUSY;
}
int na_transport_send(NaTransport *t,uint32_t generation,const uint8_t *data,uint32_t bytes) {
    if (!t || !data || !generation || bytes<GPR_HEADER_BYTES || bytes>GPR_MAX_PACKET) return NA_ARGUMENT;
    /* epoch is consumed on the ADSP worker; comparing here would race its
     * callback. Queue the caller's immutable generation and check it there. */
    if (__atomic_load_n(&t->fault,__ATOMIC_ACQUIRE) || !__atomic_load_n(&t->online,__ATOMIC_ACQUIRE)) return NA_OFFLINE;
    uint32_t h=__atomic_load_n(&t->head,__ATOMIC_RELAXED),tail=__atomic_load_n(&t->tail,__ATOMIC_ACQUIRE);
    if (h-tail>=NA_TX_SLOTS) return NA_BUSY;
    NaRxPacket *p=&t->tx[h%NA_TX_SLOTS];p->generation=generation;p->length=bytes;
    memcpy(p->data,data,bytes);__atomic_store_n(&t->head,h+1,__ATOMIC_RELEASE);return NA_OK;
}
void na_transport_lost(NaTransport *t) {
    if (!t) return;
    state(t,t->channel,0);
    /* Keep the registration pinned. Reconnect requires a fresh platform-owned
     * GLINK setup after DMA quiescence; never erase stale firmware ownership. */
    __atomic_store_n(&t->fault,1,__ATOMIC_RELEASE);
}
int na_transport_poll(NaTransport *t) {
    if (!t || !t->channel) return NA_ARGUMENT;
    if (__atomic_load_n(&t->fault,__ATOMIC_ACQUIRE)) {na_transport_lost(t);return NA_OFFLINE;}
    int r=qcom_glink_poll(t->glink);
    if (r<0 || __atomic_load_n(&t->fault,__ATOMIC_ACQUIRE)) {na_transport_lost(t);return NA_WIRE;}
    for (unsigned i=0;i<NA_TX_SLOTS;++i) {
        uint32_t tail=__atomic_load_n(&t->tail,__ATOMIC_RELAXED);
        if (tail==__atomic_load_n(&t->head,__ATOMIC_ACQUIRE)) break;
        NaRxPacket *p=&t->tx[tail%NA_TX_SLOTS];
        if (p->generation==t->epoch && __atomic_load_n(&t->online,__ATOMIC_ACQUIRE)) {
            r=qcom_glink_send(t->glink,t->channel,p->data,p->length);
            if (r==QcomGlinkAgain) return NA_PENDING;
            if (r) {na_transport_lost(t);return NA_IO;}
        }
        /* Old generation has a preceding ordered disconnect event. It cannot
         * be sent on the new channel, and its engine must invalidate handles. */
        __atomic_store_n(&t->tail,tail+1,__ATOMIC_RELEASE);
    }
    return NA_OK;
}
