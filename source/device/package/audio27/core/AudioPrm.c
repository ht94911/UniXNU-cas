/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioPrm.h"
#include <string.h>
#include <limits.h>
int na_prm_init(NaPrm *p,NaSession *s,const NaPrmResource *r,uint32_t n,
                 int (*send)(void *,const uint8_t *,uint32_t),void *context) {
    if (!p || !s || !r || !n || n>NA_PRM_RESOURCES || !send) return NA_ARGUMENT;
    for (unsigned i=0;i<n;++i) {
        if (r[i].kind>NaPrmClock || !r[i].id || (r[i].kind==NaPrmClock && !r[i].hz)) return NA_ARGUMENT;
        for (unsigned j=0;j<i;++j) if (r[i].kind==r[j].kind && r[i].id==r[j].id) return NA_ARGUMENT;
    }
    memset(p,0,sizeof *p);p->session=s;p->count=n;p->send=send;p->context=context;memcpy(p->resource,r,n*sizeof *r);return NA_OK;
}
int na_prm_request(NaPrm *p,uint32_t mask) {
    if (!p || (mask>>p->count)) return NA_ARGUMENT;
    if (p->error && mask) return p->error;
    p->want=mask;return NA_OK;
}
int na_prm_ready(const NaPrm *p,uint32_t mask) {
    return p && p->session->online && p->generation==p->session->generation &&
        !p->error && p->state==NaPrmStable && (p->held&mask)==mask;
}
static int fail(NaPrm *p,int error) {p->error=error;p->state=NaPrmFault;return error;}
int na_prm_poll(NaPrm *p,uint64_t now) {
    if (!p || now>UINT64_MAX-5000) return NA_ARGUMENT;
    if (p->state==NaPrmFault) return p->error;
    if (!p->session->online || (p->generation && p->generation!=p->session->generation)) {
        if (p->possible || p->token) return fail(p,NA_OFFLINE);
        p->state=NaPrmOffline;p->generation=0;return NA_OFFLINE;
    }
    if (p->state==NaPrmOffline) {
        if (!p->want) return NA_OK;
        p->generation=p->session->generation;p->state=NaPrmSpf;p->deadline=now+5000;p->queries=0;
    }
    if (p->token) {
        uint32_t value=0;int r=na_result(p->session,p->token,&value);
        if (r==NA_PENDING) return r;
        p->token=0;
        if (p->state==NaPrmSpf) {
            if (r==NA_OK) {p->state=NaPrmChanging;p->deadline=0;}
            else if ((r==NA_REMOTE && value==0) || r==NA_TIMEOUT) {
                /* SPF is read-only; bounded readiness queries don't replay a
                 * mutating PRM command or manufacture an acknowledgement. */
                p->retry_at=now+100;
            } else return fail(p,r);
        } else {
            if (r) return fail(p,r); /* keep possible bits on uncertain votes */
            if (p->acquire) p->held|=p->bit;
            else {p->held&=~p->bit;p->possible&=~p->bit;}
            p->state=NaPrmChanging;p->deadline=0;
        }
    }
    uint8_t packet[GPR_MAX_PACKET];uint32_t n=0,op,rsp,peer,port;
    if (p->state==NaPrmSpf) {
        if (now>=p->deadline || p->queries>=8) return fail(p,NA_TIMEOUT);
        if (now<p->retry_at) return NA_PENDING;
        op=GPR_OP_APM_GET_SPF_STATE;rsp=GPR_OP_APM_SPF_STATE;peer=port=AR_APM_IID;
    } else {
        uint32_t release=p->held&~p->want,acquire=p->want&~p->held;
        if (!release && !acquire) {p->state=NaPrmStable;p->deadline=0;return NA_OK;}
        p->state=NaPrmChanging;
        if (!p->deadline) p->deadline=now+2000;
        if (now>=p->deadline) return fail(p,NA_TIMEOUT);
        p->acquire=!release;unsigned index;
        if (release) {index=p->count-1;while (!(release&(1u<<index))) --index;}
        else {index=0;while (!(acquire&(1u<<index))) ++index;}
        p->bit=1u<<index;op=p->acquire ? AR_OP_PRM_REQUEST:AR_OP_PRM_RELEASE;
        rsp=p->acquire ? AR_OP_PRM_REQUEST_DONE:AR_OP_PRM_RELEASE_DONE;peer=port=AR_PRM_IID;
    }
    uint32_t token=na_token(p->session);if (!token) return fail(p,NA_OFFLINE);
    if (p->state==NaPrmSpf) {
        GprPacket q={.src_domain=GPR_DOMAIN_APPS,.dst_domain=GPR_DOMAIN_ADSP,
            .src_port=port,.dst_port=peer,.opcode=op,.token=token};
        n=gpr_encode(packet,sizeof packet,&q,0,0);
    } else {
        unsigned index=0;while (p->bit!=(1u<<index)) ++index;
        NaPrmResource *r=&p->resource[index];
        n=r->kind==NaPrmCore ? ar_prm_vote(packet,sizeof packet,port,token,r->id,p->acquire):
            ar_prm_clock_attr(packet,sizeof packet,port,token,r->id,p->acquire ? r->hz:0,r->attribute);
    }
    if (!n) return fail(p,NA_ARGUMENT);
    int r=na_expect(p->session,token,port,peer,op,rsp,now,p->state==NaPrmSpf ? 400:2000);
    if (r==NA_BUSY) return NA_PENDING;
    if (r) return fail(p,r);
    r=p->send(p->context,packet,n);
    if (r) {na_cancel_unsent(p->session,token);return r==NA_BUSY ? NA_PENDING:fail(p,r);}
    p->token=token;
    if (p->state==NaPrmSpf) ++p->queries;
    else if (p->acquire) p->possible|=p->bit;
    return NA_PENDING;
}
