/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* sheng QRTR/QMI service locator, bound to the stock nezha protection domains. */
#include "AudioPdMapper.h"
#include <string.h>
static const char *const audio_services[]={"avs/audio",0};
static const char *const modem_services[]={"gps/gps_service",0};
static const char *const oem_services[]={"oempd/srv1",0};
static const QcomPdDomain domains[]={
    {"msm/adsp/audio_pd",74,audio_services},
    {"msm/adsp/root_pd",74,0},
    {"msm/adsp/sensor_pd",74,0},
    {"msm/cdsp/root_pd",76,audio_services},
    {"msm/modem/root_pd",180,modem_services},
    {"msm/modem/oem_pd",180,oem_services},
};
static int queue_packet(void *v,const uint8_t *packet,uint32_t bytes) {
    NaPdMapper *p=v;
    if (p->error || !p->online || !packet || !bytes || bytes>NA_PD_PACKET_BYTES) return -1;
    if (p->head-p->tail==NA_PD_TX_SLOTS) {p->error=NA_OVERFLOW;return -1;}
    unsigned at=p->head%NA_PD_TX_SLOTS;
    memcpy(p->tx[at].data,packet,bytes);p->tx[at].bytes=bytes;++p->head;
    return 0;
}
static void locator(void *v,QcomQrtr *q,uint32_t port,uint32_t node,uint32_t from,const uint8_t *data,uint32_t bytes) {
    NaPdMapper *p=v;QcomQmiMessage message;uint32_t count=0;
    if (qcom_qmi_parse(data,bytes,&message) || message.type!=QCOM_QMI_REQUEST ||
        message.message!=QCOM_SERVLOC_GET_DOMAIN_LIST) return;
    const uint8_t *name=qcom_qmi_tlv(&message,1,&count);
    if (!name || count>64) return;
    ++p->requests;p->last_transaction=message.transaction;
    unsigned n=0;
    while (n<count && name[n]) {p->last_service[n]=name[n]>=32 && name[n]<=126 ? (char)name[n]:'.';++n;}
    p->last_service[n]=0;
    uint8_t answer[NA_PD_PACKET_BYTES-QCOM_QRTR_HEADER];
    uint32_t length=qcom_servloc_answer(domains,sizeof domains/sizeof domains[0],data,bytes,answer,sizeof answer);
    if (length && !qcom_qrtr_send_data(q,port,node,from,answer,length)) ++p->answers;
}
static void link_state(void *v,QcomGlinkChannel *channel,int up) {
    NaPdMapper *p=v;(void)channel;
    if (!up) {if (p->online) p->error=NA_REMOTE;p->online=0;return;}
    if (p->online || p->error) return;
    p->online=1;qcom_qrtr_start(&p->qrtr);
}
static void receive(void *v,QcomGlinkChannel *channel,const uint8_t *packet,uint32_t bytes) {
    NaPdMapper *p=v;(void)channel;
    if (p->error) return;
    unsigned hello=p->qrtr.hello_received;
    if (qcom_qrtr_receive(&p->qrtr,packet,bytes)) p->error=NA_WIRE;
    else if (hello && !p->qrtr.hello_received) p->error=NA_REMOTE;
}
int na_pd_mapper_init(NaPdMapper *p,QcomGlink *g) {
    if (!p || !g) return NA_ARGUMENT;
    if (p->glink || p->channel || p->error) return NA_OWNERSHIP;
    memset(p,0,sizeof *p);p->glink=g;
    qcom_qrtr_init(&p->qrtr,1,queue_packet,p);
    if (qcom_qrtr_add_service(&p->qrtr,QCOM_SERVLOC_SERVICE,QCOM_SERVLOC_INSTANCE,1,locator,p)) {
        p->error=NA_MISSING;return p->error;
    }
    const QcomGlinkClient client={link_state,receive};
    p->channel=qcom_glink_channel(g,"IPCRTR",1024,8,p->intents,&client,p,0);
    /* This fresh owned GLINK has only adsp_apps registered. A registration
     * failure is terminal, not a reason to reset already published indices. */
    if (!p->channel) p->error=NA_MISSING;
    return p->error;
}
int na_pd_mapper_poll(NaPdMapper *p) {
    if (!p || !p->channel) return NA_ARGUMENT;
    if (p->error) return p->error;
    if (!p->online) return NA_PENDING;
    for (unsigned i=0;i<NA_PD_TX_SLOTS && p->tail!=p->head;++i) {
        unsigned at=p->tail%NA_PD_TX_SLOTS;
        int r=qcom_glink_send(p->glink,p->channel,p->tx[at].data,p->tx[at].bytes);
        if (r==QcomGlinkAgain) return NA_PENDING;
        if (r) {p->error=r==QcomGlinkNotOpen ? NA_REMOTE:NA_IO;return p->error;}
        ++p->tail;++p->tx_sent;
    }
    return NA_OK;
}
