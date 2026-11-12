/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomPdr.h"
#include <string.h>
#define RESPONSE_US UINT64_C(5000000) /* Linux pdr_register_listener: 5 * HZ */
static uint16_t get16(const uint8_t *p) {return (uint16_t)(p[0]|(uint16_t)p[1]<<8);}
static uint32_t get32(const uint8_t *p) {return get16(p)|(uint32_t)get16(p+2)<<16;}
static void put16(uint8_t *p,uint32_t x) {p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);}
static int state_valid(uint32_t s) {
    return s==QCOM_PDR_DOWN || s==QCOM_PDR_UP || s==QCOM_PDR_EARLY_DOWN || s==QCOM_PDR_UNINIT;
}
void qcom_pdr_lost(QcomPdr *p,int error) {
    if (!p || !p->router || p->error) return;
    p->error=error<0 ? error:QcomPdrLost;p->state=QCOM_PDR_DOWN;p->registered=0;++p->revision;
}
static void receive(void *v,QcomQrtr *q,uint32_t local,uint32_t node,uint32_t port,
                    const uint8_t *data,uint32_t bytes) {
    QcomPdr *p=v;QcomQmiMessage m;uint32_t n=0;
    if (p->error) return;
    if (q!=p->router || local!=p->local_port || !p->selected || node!=p->node || port!=p->port) {++p->stale;return;}
    if (qcom_qmi_parse(data,bytes,&m)) {qcom_pdr_lost(p,QcomPdrWire);return;}
    if (m.type==QCOM_QMI_RESPONSE) {
        uint32_t expected=p->pending_kind==1 ? QCOM_PDR_REGISTER:QCOM_PDR_ACK;
        if (!p->pending_kind || m.transaction!=p->pending_transaction || m.message!=expected) {++p->stale;return;}
        const uint8_t *result=qcom_qmi_tlv(&m,2,&n);
        if (!result || n!=4) {qcom_pdr_lost(p,QcomPdrWire);return;}
        if (get16(result) || get16(result+2)) {qcom_pdr_lost(p,QcomPdrRemote);return;}
        if (p->pending_kind==1) {
            const uint8_t *state=qcom_qmi_tlv(&m,0x10,&n);
            if (!state || n!=4 || !state_valid(get32(state))) {qcom_pdr_lost(p,QcomPdrWire);return;}
            p->registered=1;
            /* A notification received while REGISTER was pending is newer
             * than the state captured by its reply. Do not roll it back. */
            if (p->indications==p->register_indications) {p->state=get32(state);++p->revision;}
        } else {++p->tail;++p->acks;}
        p->pending_kind=0;return;
    }
    if (m.type!=QCOM_QMI_INDICATION || m.message!=QCOM_PDR_UPDATED) {++p->stale;return;}
    const uint8_t *path=qcom_qmi_tlv(&m,2,&n);
    if (!path || n!=strlen(p->path) || memcmp(path,p->path,n)) {++p->stale;return;}
    const uint8_t *state=qcom_qmi_tlv(&m,1,&n);
    if (!state || n!=4 || !state_valid(get32(state))) {qcom_pdr_lost(p,QcomPdrWire);return;}
    uint32_t value=get32(state);
    const uint8_t *transaction=qcom_qmi_tlv(&m,3,&n);
    if (!transaction || n!=2) {qcom_pdr_lost(p,QcomPdrWire);return;}
    uint16_t id=get16(transaction);
    for (uint32_t i=p->tail;i!=p->head;++i) if (p->ack[i%QCOM_PDR_ACK_SLOTS]==id) return;
    if (p->head-p->tail==QCOM_PDR_ACK_SLOTS) {qcom_pdr_lost(p,QcomPdrFull);return;}
    p->ack[p->head++%QCOM_PDR_ACK_SLOTS]=id;
    p->state=value;++p->indications;++p->revision;
}
int qcom_pdr_prepare(QcomPdr *p,QcomQrtr *q,uint32_t local,const char *path,
                     uint32_t instance,const QcomPdrOps *ops) {
    if (!p || p->router || !q || !q->send || !path || !*path || strlen(path)>64 ||
        !instance || instance>0xffffff || !ops || !ops->room) return QcomPdrArgument;
    memset(p,0,sizeof *p);p->router=q;p->ops=*ops;p->local_port=local;
    p->instance=instance<<8|1;p->state=QCOM_PDR_UNINIT;memcpy(p->path,path,strlen(path)+1);
    if (qcom_qrtr_add_client(q,local,receive,p)) {qcom_pdr_lost(p,QcomPdrFull);return p->error;}
    return QcomPdrOkay;
}
static uint32_t request(QcomPdr *p,uint32_t kind,uint8_t out[96],uint16_t transaction) {
    uint32_t at=QCOM_QMI_HEADER,n=(uint32_t)strlen(p->path);
    memset(out,0,96);put16(out+1,transaction);put16(out+3,kind==1 ? QCOM_PDR_REGISTER:QCOM_PDR_ACK);
    if (kind==1) {out[at++]=1;put16(out+at,1);at+=2;out[at++]=1;}
    out[at++]=(uint8_t)(kind==1 ? 2:1);put16(out+at,n);at+=2;memcpy(out+at,p->path,n);at+=n;
    if (kind==2) {out[at++]=2;put16(out+at,2);at+=2;put16(out+at,p->ack[p->tail%QCOM_PDR_ACK_SLOTS]);at+=2;}
    put16(out+5,at-QCOM_QMI_HEADER);return at;
}
int qcom_pdr_poll(QcomPdr *p,uint64_t now) {
    if (!p || !p->router) return QcomPdrArgument;
    if (p->error) return p->error;
    QcomQrtr *q=p->router;
    if (!q->hello_received) {
        if (p->selected) qcom_pdr_lost(p,QcomPdrLost);
        return p->error ? p->error:QcomPdrAgain;
    }
    unsigned matches=0;uint32_t node=0,port=0;
    for (uint32_t i=0;i<q->remote_count;++i) if (q->remote[i].service==QCOM_PDR_SERVICE && q->remote[i].instance==p->instance) {
        ++matches;node=q->remote[i].node;port=q->remote[i].port;
    }
    if ((!p->selected && matches>1) || (p->selected && (matches!=1 || node!=p->node || port!=p->port))) {
        qcom_pdr_lost(p,QcomPdrLost);return p->error;
    }
    if (!matches) return QcomPdrAgain;
    if (!p->selected) {p->selected=1;p->node=node;p->port=port;}
    if (p->pending_kind) {
        if (now>=p->deadline_us) {qcom_pdr_lost(p,QcomPdrTimeout);return p->error;}
        return QcomPdrAgain;
    }
    uint32_t kind=!p->registered ? 1:p->head!=p->tail ? 2:0;
    if (!kind) return QcomPdrOkay;
    if (p->transaction==UINT16_MAX || now>UINT64_MAX-RESPONSE_US) {qcom_pdr_lost(p,QcomPdrArgument);return p->error;}
    uint8_t message[96];uint16_t transaction=p->transaction+1;uint32_t bytes=request(p,kind,message,transaction);
    int r=p->ops.room(p->ops.context,QCOM_QRTR_HEADER+((bytes+3)&~3u));
    if (r==QcomPdrAgain) return r;
    if (r || qcom_qrtr_send_data(q,p->local_port,p->node,p->port,message,bytes)) {qcom_pdr_lost(p,QcomPdrLost);return p->error;}
    p->transaction=transaction;p->pending_transaction=transaction;p->pending_kind=kind;
    p->deadline_us=now+RESPONSE_US;
    if (kind==1) p->register_indications=p->indications;
    return QcomPdrOkay;
}
int qcom_pdr_is_up(const QcomPdr *p) {
    return p && p->registered && !p->error && p->state==QCOM_PDR_UP;
}
