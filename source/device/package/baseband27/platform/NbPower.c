/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Board vote policy, using QcomRpmh/QcomQmp and the package's common owner.
 * Hardware sources: stock canoe DTS and Linux kaanapali/clk-rpmh tables.
 * GPU uses its GMU voter; touch's regulator vote and audio's LP0/LCX/LMX
 * resources are distinct. Shared IPA/MSS votes are aggregated here. */
#include "NbPower.h"
#include <string.h>
static uint32_t le16(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8;}
static uint32_t le32(const uint8_t *p){return le16(p)|le16(p+2)<<16;}
static int error(int e){return !e?NB_OK:e==Sm8850Busy?NB_BUSY:e==Sm8850Io?NB_IO:NB_OWNERSHIP;}
static int fail(NbPower *p,int e){p->error=e<0?e:NB_IO;return p->error;}
static void kick(void *v){NbPower *p=v;if(sm8850_ipcc_send(p->common,0))p->error=NB_IO;}
static uint32_t bcm(unsigned x,unsigned y){if(x>0x3fff)x=0x3fff;if(y>0x3fff)y=0x3fff;return (1u<<30)|((x||y)?1u<<29:0)|(x<<14)|y;}
static unsigned ceildiv(uint64_t a,uint64_t b){uint64_t q=a/b+!!(a%b);return q>0x3fff?0x3fff:(unsigned)q;}
static uint32_t bandwidth(const QcomRpmhResource *r,uint32_t avg,uint32_t peak,unsigned width,unsigned channels){
    if(r->aux_bytes<8 || !le32(r->aux) || !le16(r->aux+4))return 0;
    uint64_t unit=le32(r->aux),w=le16(r->aux+4);
    return bcm(ceildiv((uint64_t)avg*1000*w,unit*width*channels),ceildiv((uint64_t)peak*1000*w,unit*width));
}
int nb_power_init(NbPower *p,Sm8850Resources *common,volatile uint32_t *ram,const uint8_t *db,size_t dbbytes){
    if(!p||!common||!common->initialized||!ram||!db)return NB_ARGUMENT;
    *p=(NbPower){0};p->common=common;uint32_t version,config;
    if(common->io.lock(common->io.context))return NB_BUSY;
    int rc=common->io.read32(common->io.context,Sm8850Rsc,0,&version)||common->io.read32(common->io.context,Sm8850Rsc,0xc,&config);
    common->io.unlock(common->io.context);
    if(rc||version!=0x40300||config!=0x800c8104)return NB_OWNERSHIP;
    p->layout=qcom_rsc_layout(version);
    static const char *names[]={"xo.lvl","cx.lvl","mss.lvl","IP0","MC0","SH0","SH1","SN0","SN3","CE0","CN0"};
    for(unsigned i=0;i<NB_POWER_RESOURCES;i++)if(qcom_cmd_db_find(db,dbbytes,names[i],&p->resource[i]))return NB_MISSING;
    p->vote[NB_POWER_MSS][0]=3; // clk-rpmh ARC enable value
    for(unsigned i=1;i<3;i++){
        unsigned level=i==1?384:320,index=0;
        if(qcom_rpmh_level_index(&p->resource[i],level,&index)||p->resource[i].levels[index]<level)return NB_MISSING;
        p->vote[NB_POWER_MSS][i]=index;
    }
    if(p->resource[3].aux_bytes<4||!le32(p->resource[3].aux))return NB_MISSING;
    p->vote[NB_POWER_IPA][3]=bcm(0,ceildiv(120000000,le32(p->resource[3].aux)));
    /* Stock SVS three paths: 1.2GB/s average, 2.8GB/s memory peak,
     * and 150MB/s AP configuration peak. Keepalive/mask BCMs follow their table. */
    const unsigned widths[]={4,16,0,16,16,8,0},channels[]={4,4,0,1,1,1,0};
    for(unsigned i=4;i<NB_POWER_RESOURCES;i++){
        if(i==6||i==10){p->vote[NB_POWER_IPA][i]=bcm(1,1);p->vote[NB_POWER_MSS][i]=bcm(1,1);continue;}
        p->vote[NB_POWER_IPA][i]=bandwidth(&p->resource[i],i==9?0:1200000,i==4?2800000:i==9?0:1200000,widths[i-4],channels[i-4]);
        if(!p->vote[NB_POWER_IPA][i])return NB_MISSING;
        p->vote[NB_POWER_MSS][i]=bcm(0x3fff,0x3fff);
    }
    /* No clear command for resources this owner has not voted. */
    for(unsigned i=3;i<NB_POWER_RESOURCES;i++)p->sent[i]=bcm(0,0);
    qcom_qmp_init(&p->qmp,ram,0x400,kick,p);return NB_OK;
}
static int read_rsc(NbPower *p,uint32_t off,uint32_t *value){
    Sm8850ResourceIo *io=&p->common->io;if(io->lock(io->context))return NB_BUSY;
    int r=io->read32(io->context,Sm8850Rsc,off,value);io->unlock(io->context);return r?NB_IO:NB_OK;
}
static int transaction(NbPower *p,uint64_t now){
    const unsigned tcs=1;const uint32_t b=0xd00+p->layout.tcs_stride;
    if(!p->busy){
        int r=sm8850_resource_acquire(p->common,NB_RES_TCS,NB_OWNER,&p->lease);if(r)return error(r);
        r=sm8850_resource_enter(p->common,NB_RES_TCS,NB_OWNER,p->lease);if(r)return fail(p,error(r));
        r=sm8850_tcs_idle(p->common,tcs);
        if(!r)r=sm8850_tcs_irq_update(p->common,tcs,1,&p->irq_previous);
        QcomRpmhWrite writes[12];unsigned n=qcom_rpmh_active_request(&p->layout,tcs,&p->resource[p->cursor].address,&p->desired[p->cursor],1,writes,12);
        if(!n)r=Sm8850Invalid;
        for(unsigned i=0;!r&&i<n;i++)r=sm8850_tcs_write(p->common,tcs,0xd00+writes[i].offset,writes[i].value);
        sm8850_resource_leave(p->common);
        if(r)return fail(p,error(r));
        p->busy=1;p->deadline=now+100;return NB_PENDING;
    }
    uint32_t irq,status,ack;
    if(read_rsc(p,0xd00+p->layout.irq_status,&irq)||read_rsc(p,b+p->layout.status,&status)||read_rsc(p,b+p->layout.cmd_data+4,&ack))return fail(p,NB_IO);
    if(!(irq&(1u<<tcs))||status!=1||(ack&0x10100)!=0x10100)return now>=p->deadline?fail(p,NB_TIMEOUT):NB_PENDING;
    int r=sm8850_resource_enter(p->common,NB_RES_TCS,NB_OWNER,p->lease);if(r)return error(r);
    QcomRpmhWrite writes[4];unsigned n=qcom_rpmh_finish(&p->layout,tcs,writes,4);
    for(unsigned i=0;!r&&i<n;i++)r=sm8850_tcs_write(p->common,tcs,0xd00+writes[i].offset,writes[i].value);
    if(!r)r=sm8850_tcs_write(p->common,tcs,b+p->layout.wait_for_cmpl,0);
    unsigned old;if(!r)r=sm8850_tcs_irq_update(p->common,tcs,p->irq_previous,&old);
    sm8850_resource_leave(p->common);if(r)return fail(p,error(r));
    r=sm8850_resource_release(p->common,NB_RES_TCS,NB_OWNER,p->lease,1);if(r)return fail(p,error(r));
    p->sent[p->cursor]=p->desired[p->cursor];p->busy=0;p->lease=0;return NB_OK;
}
int nb_power_message(NbPower *p,const char *text,uint64_t now){
    if(!p||!text||strlen(text)>=sizeof p->message||now>UINT64_MAX-1000)return NB_ARGUMENT;
    if(p->error)return p->error;
    if(!p->qmp_busy){
        int r=sm8850_resource_acquire(p->common,NB_RES_QMP,NB_OWNER,&p->qmp_lease);if(r)return error(r);
        memcpy(p->message,text,strlen(text)+1);/* length checked above */p->qmp_busy=1;p->qmp_deadline=now+1000;p->qmp_sent=0;
    }else if(strcmp(text,p->message))return NB_BUSY;
    if(now>=p->qmp_deadline)return fail(p,NB_TIMEOUT);
    int state=qcom_qmp_step(&p->qmp);if(state<0||p->error)return fail(p,NB_IO);
    if(state!=QcomQmpOpen)return NB_PENDING;
    if(!p->qmp_sent){
        if(qcom_qmp_send(&p->qmp,text)!=QcomQmpSending||p->error)return fail(p,NB_IO);
        p->qmp_sent=1;return NB_PENDING;
    }
    int r=sm8850_resource_release(p->common,NB_RES_QMP,NB_OWNER,p->qmp_lease,1);if(r)return fail(p,error(r));
    p->qmp_busy=0;p->qmp_lease=0;return NB_OK;
}
int nb_power_set(NbPower *p,unsigned kind,int on,uint64_t now){
    if(!p||kind>1||(on!=0&&on!=1)||now>UINT64_MAX-1000)return NB_ARGUMENT;
    if(p->error)return p->error;
    if(!p->active){
        if(p->want[kind]==(unsigned)on && !(kind==NB_POWER_MSS&&on&&!p->load_state))return NB_OK;
        p->want[kind]=(unsigned)on;p->active=kind+1;p->cursor=0;p->request_deadline=now+1000;
        for(unsigned i=0;i<NB_POWER_RESOURCES;i++){
            uint32_t a=p->want[0]?p->vote[0][i]:0,b=p->want[1]?p->vote[1][i]:0;
            if(i<3)p->desired[i]=a>b?a:b;
            else{unsigned ax=(a>>14)&0x3fff,bx=(b>>14)&0x3fff,ay=a&0x3fff,by=b&0x3fff;p->desired[i]=bcm(ax>bx?ax:bx,ay>by?ay:by);}
        }
    }else if(p->active!=kind+1 || p->want[kind]!=(unsigned)on)return NB_BUSY;
    if(now>=p->request_deadline)return fail(p,NB_TIMEOUT);
    while(p->cursor<NB_POWER_RESOURCES && !p->busy && p->sent[p->cursor]==p->desired[p->cursor])p->cursor++;
    if(p->cursor<NB_POWER_RESOURCES){int r=transaction(p,now);if(r==NB_BUSY)return NB_PENDING;return r<0?r:NB_PENDING;}
    if(kind==NB_POWER_MSS&&on&&!p->load_state){
        int r=nb_power_message(p,"{class: image, res: load_state, name: modem, val: on}",now);
        if(r==NB_BUSY)return NB_PENDING;
        if(r)return r;
        p->load_state=1;
    }
    p->active=0;return NB_OK;
}
