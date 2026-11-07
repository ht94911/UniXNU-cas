/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AdspPower.h"
#include <string.h>
#include <limits.h>
enum {PIdle,PBandwidth,PLoadState,PLcx,PLmx,PHeld,PReleaseLcx,PReleaseLmx,PReleaseBandwidth,PHanded,PFault};
static int fail(NaProxyPower *p,int r) {p->error=r<0 ? r:NA_IO;p->stage=PFault;return p->error;}
int na_proxy_layout(uint32_t version,uint32_t config,QcomRscLayout *layout) {
    /* GPU015 console:0x40300,config0x800c8104,DRV2 has8 TCS/16 commands.
     * Both Linux and the retained touch driver use the v3 layout for this
     * revision. Accept this observed4.3 identity without relaxing geometry. */
    uint32_t major=(version>>16)&255,tcs=(config>>12)&63,commands=config>>27;
    if (!layout || (major!=3 && version!=0x40300) || tcs<3 || tcs>16 || !commands || commands>16) return NA_MISSING;
    *layout=qcom_rsc_layout(version);return NA_OK;
}
int na_proxy_init(NaProxyPower *p,volatile uint32_t *ram,uint32_t bytes,const uint8_t *db,uint32_t dbbytes,uint32_t tcs,const NaProxyOps *ops) {
    if (!p || !ram || ((uintptr_t)ram&3) || bytes!=0x400 || !db || dbbytes!=0x20000 || tcs>=3 || !ops ||
        !ops->acquire || !ops->release || !ops->read_rsc || !ops->write_rsc || !ops->kick_aop) return NA_ARGUMENT;
    memset(p,0,sizeof *p);p->ops=*ops;p->tcs=tcs;
    uint32_t version=ops->read_rsc(ops->context,0);
    if (na_proxy_layout(version,ops->read_rsc(ops->context,0x0c),&p->layout)) return NA_MISSING;
    const char *names[3]={"LP0","lcx.lvl","lmx.lvl"};
    for (unsigned i=0;i<3;++i) {
        if (qcom_cmd_db_find(db,dbbytes,names[i],&p->resource[i])) return NA_MISSING;
        if (i) {
            uint32_t index=0;
            if (qcom_rpmh_level_index(&p->resource[i],384,&index) || p->resource[i].levels[index]<384) return NA_MISSING;
            p->values[i]=index;
        } else p->values[i]=(1u<<30)|(1u<<29)|0x3fffu;
    }
    qcom_qmp_init(&p->qmp,ram,bytes,ops->kick_aop,ops->context);return NA_OK;
}
int na_proxy_request(NaProxyPower *p,int on) {
    if (!p || (on!=0 && on!=1)) return NA_ARGUMENT;
    if (p->error) return p->error;
    if (on && p->stage==PHanded) return NA_OWNERSHIP; /* no reload without a new stopped-DSP lease */
    p->want=!!on;return NA_OK;
}
static uint32_t read_tcs(NaProxyPower *p,uint32_t at) {return p->ops.read_rsc(p->ops.context,0xd00+at);}
static void write_tcs(NaProxyPower *p,uint32_t at,uint32_t value) {p->ops.write_rsc(p->ops.context,0xd00+at,value);}
static int vote(NaProxyPower *p,unsigned resource,int on,uint64_t now) {
    if (!p->busy) {
        int r=p->ops.acquire(p->ops.context,NaPowerTcs,&p->lease);if (r) return r;
        if (!p->lease) return fail(p,NA_OWNERSHIP);
        uint32_t b=p->tcs*p->layout.tcs_stride;
        if (!read_tcs(p,b+p->layout.status) || read_tcs(p,b+p->layout.control) || read_tcs(p,b+p->layout.cmd_enable)) return fail(p,NA_OWNERSHIP);
        QcomRpmhWrite w[12];uint32_t value=on ? p->values[resource]:resource ? 0:((1u<<30)|(1u<<29));
        unsigned n=qcom_rpmh_active_request(&p->layout,p->tcs,&p->resource[resource].address,&value,1,w,12);
        if (!n) return fail(p,NA_WIRE);
        for (unsigned i=0;i<n;++i) write_tcs(p,w[i].offset,w[i].value);
        p->busy=1;p->active_resource=resource;p->possible|=1u<<resource;p->deadline=now+100;
        return NA_PENDING;
    }
    if (p->active_resource!=resource) return fail(p,NA_OWNERSHIP);
    uint32_t b=p->tcs*p->layout.tcs_stride;
    /* The exclusive owner cleared this TCS's completion before triggering.
     * Require the new completion bit as well as its command ACK and idle:
     * stale CMD_STATUS from the previous vote is not a new completion.
     * Touch's real polling path observes IRQ_STATUS even with delivery masked.
     * This owner never overwrites the common interrupt-enable word. */
    p->last_irq=read_tcs(p,p->layout.irq_status);
    p->last_command_status=read_tcs(p,b+p->layout.cmd_data+4);
    p->last_status=read_tcs(p,b+p->layout.status);
    if (!(p->last_irq&(1u<<p->tcs)) || (p->last_command_status&0x10100u)!=0x10100u || !p->last_status) return NA_PENDING;
    QcomRpmhWrite w[4];unsigned n=qcom_rpmh_finish(&p->layout,p->tcs,w,4);
    for (unsigned i=0;i<n;++i) write_tcs(p,w[i].offset,w[i].value);
    write_tcs(p,b+p->layout.wait_for_cmpl,0);
    int r=p->ops.release(p->ops.context,NaPowerTcs,p->lease);if (r) return fail(p,r);
    p->lease=0;p->busy=0;p->deadline=0;
    if (on) p->held|=1u<<resource;else {p->held&=~(1u<<resource);p->possible&=~(1u<<resource);}
    return NA_OK;
}
static int load_state(NaProxyPower *p,uint64_t now) {
    if (!p->lease) {
        int r=p->ops.acquire(p->ops.context,NaPowerMailbox,&p->lease);if (r) return r;
        if (!p->lease) return fail(p,NA_OWNERSHIP);
        p->deadline=now+1000;
    }
    int state=qcom_qmp_step(&p->qmp);if (state<0) return fail(p,NA_IO);
    if (state!=QcomQmpOpen) return NA_PENDING;
    if (!p->busy) {
        if (qcom_qmp_send(&p->qmp,"{class: image, res: load_state, name: adsp, val: on}")!=QcomQmpSending) return fail(p,NA_IO);
        p->busy=1;return NA_PENDING;
    }
    int r=p->ops.release(p->ops.context,NaPowerMailbox,p->lease);if (r) return fail(p,r);
    p->busy=0;p->lease=0;p->deadline=0;p->load_state_on=1;return NA_OK;
}
int na_proxy_poll(NaProxyPower *p,uint64_t now) {
    if (!p || now>UINT64_MAX-5000) return NA_ARGUMENT;
    if (p->error) return p->error;
    if (p->deadline && now>=p->deadline) return fail(p,NA_TIMEOUT);
    if (p->stage==PIdle) {if (!p->want) return NA_OK;p->stage=PBandwidth;}
    if (p->stage==PHeld) {if (p->want) return NA_OK;p->stage=PReleaseLcx;}
    if (p->stage==PHanded) return p->want ? NA_OWNERSHIP:NA_OK;
    if (!p->deadline) p->deadline=now+1000; /* Includes time waiting for shared lease. */
    int r;
    switch (p->stage) {
    case PBandwidth:r=vote(p,0,1,now);break;
    case PLoadState:r=load_state(p,now);break;
    case PLcx:r=vote(p,1,1,now);break;
    case PLmx:r=vote(p,2,1,now);break;
    case PReleaseLcx:r=vote(p,1,0,now);break;
    case PReleaseLmx:r=vote(p,2,0,now);break;
    case PReleaseBandwidth:r=vote(p,0,0,now);break;
    default:return fail(p,NA_OWNERSHIP);
    }
    if (r==NA_BUSY || r==NA_PENDING) return NA_PENDING;
    if (r) return fail(p,r);
    ++p->stage;return p->stage==PHeld || p->stage==PHanded ? NA_OK:NA_PENDING;
}
