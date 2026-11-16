/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Sm8850SpmiV8.h"
#define CORE 0x0c400000ull
#define MAP 0x0c403000ull
#define OBSERVER 0x0c4c0000ull
#define WRITER 0x0c900000ull
static const uint64_t owners[SM8850_SPMI_BUSES] = {0x0c42a000ull, 0x0c43b000ull};
enum { INVALID = -22, IO = -5, DENIED = -13, NODEV = -19, TIMEOUT = -110 };
static int neg(int e) { return e > 0 ? -e : e; }
static int rd(Sm8850SpmiV8 *s, uint64_t a, uint32_t *v) { return neg(s->io.read32(s->io.context,a,v)); }
static int wr(Sm8850SpmiV8 *s, uint64_t a, uint32_t v) { return neg(s->io.write32(s->io.context,a,v)); }
static int lock(Sm8850SpmiV8 *s) { return neg(s->io.acquire(s->io.context)); }
static int finish(Sm8850SpmiV8 *s,int e) { s->last_error=e;s->io.release(s->io.context);return e; }
static int scope(const Sm8850SpmiV8 *s,const Sm8850SpmiChannel *c) {
    return s && s->ready && c && c->bus<SM8850_SPMI_BUSES && c->ppid<=0xfff &&
        c->apid>=s->first[c->bus] && c->apid-s->first[c->bus]<s->count[c->bus];
}
static int identity(Sm8850SpmiV8 *s,const Sm8850SpmiChannel *c,int write) {
    uint32_t map=0,owner=0;
    int e=rd(s,MAP+4u*c->apid,&map);
    if (!e && (!map || (map&0x1fff)!=c->ppid)) e=NODEV;
    /* v8 owner tables are local to each bus; APIDs in CMD windows are global.
     * Using bus0 OWNER+4*global_apid for bus1 silently reads another table. */
    if (!e) e=rd(s,owners[c->bus]+4u*(c->apid-s->first[c->bus]),&owner);
    if (!e && write && (owner&7u)!=s->ee) e=DENIED;
    return e;
}
int sm8850_spmi_v8_init(Sm8850SpmiV8 *s,const Sm8850SpmiOps *io,unsigned ee) {
    if (!s || !io || !io->read32 || !io->write32 || !io->pause_us || !io->acquire || !io->release || ee>7) return INVALID;
    __builtin_memset(s,0,sizeof *s);s->io=*io;s->ee=(uint8_t)ee;
    int e=lock(s);if(e)return e;
    e=rd(s,CORE,&s->version);
    if (!e && (s->version<0x80000000u || s->version>=0x90000000u)) e=NODEV;
    unsigned first=0;
    for (unsigned b=0;!e && b<SM8850_SPMI_BUSES;++b) {
        uint32_t n=0;e=rd(s,CORE+4u+4u*b,&n);n&=0x1fff;
        if(!e && (!n || first+n>SM8850_SPMI_MAX_APIDS))e=NODEV;
        if(!e){s->first[b]=(uint16_t)first;s->count[b]=(uint16_t)n;first+=n;}
    }
    s->ready=(uint8_t)!e;return finish(s,e);
}
int sm8850_spmi_v8_find(Sm8850SpmiV8 *s,unsigned bus,uint16_t ppid,Sm8850SpmiChannel *out) {
    if(!s || !s->ready || !out || bus>=SM8850_SPMI_BUSES || ppid>0xfff)return INVALID;
    __builtin_memset(out,0,sizeof *out);out->bus=(uint8_t)bus;out->ppid=ppid;
    out->irq_owner=0xff;
    int e=lock(s);if(e)return e;int found=0;
    for(unsigned local=0;!e && local<s->count[bus];++local) {
        unsigned apid=s->first[bus]+local;uint32_t map=0,owner=0;e=rd(s,MAP+4u*apid,&map);
        if(e || !map || (map&0x1fff)!=ppid)continue;
        e=rd(s,owners[bus]+4u*local,&owner);if(e)break;
        if(out->mappings<255)++out->mappings;
        /* Linux v8 picks the last mapping owned by this EE. IRQ routing has a
         * separate owner bit; this code enables no interrupt. */
        if(!found || (owner&7u)==s->ee) {out->apid=(uint16_t)apid;out->owner=(uint8_t)(owner&7);found=1;}
        if(map&(1u<<31))out->irq_owner=(uint8_t)(owner&7);
    }
    return finish(s,e?e:found?0:NODEV);
}
int sm8850_spmi_v8_owns(Sm8850SpmiV8 *s,const Sm8850SpmiChannel *c) {
    if(!scope(s,c))return INVALID;
    int e=lock(s);if(e)return e;return finish(s,identity(s,c,1));
}
static int wait(Sm8850SpmiV8 *s,uint64_t base) {
    for(unsigned i=0;i<1000;++i) {
        int e=rd(s,base+8,&s->status);if(e)return e;
        if(s->status&1u){s->pending=0;return s->status&4u?DENIED:s->status&10u?IO:0;}
        s->io.pause_us(s->io.context,1);
    }
    return TIMEOUT;
}
static int transfer(Sm8850SpmiV8 *s,const Sm8850SpmiChannel *c,uint8_t reg,uint8_t *out,const uint8_t *data,unsigned n) {
    if(!scope(s,c) || !n || n>8 || (unsigned)reg+n>256 || (!out&&!data))return INVALID;
    int e=lock(s);if(e)return e;s->status=0;
    /* A timed-out CMD may still own the channel. Retire it before any later
     * command, including emergency STOP; elapsed time is not completion. */
    if(s->pending) {
        e=rd(s,s->pending_base+8,&s->status);
        if(!e && !(s->status&1u))e=TIMEOUT;
        if(e)return finish(s,e);
        s->pending=0;
    }
    e=identity(s,c,data!=0);
    uint64_t base=(data?WRITER:OBSERVER+(uint64_t)s->ee*0x40000u)+(uint64_t)c->apid*(data?0x200u:0x20u);
    uint32_t words[2]={0,0};
    if(data) {
        for(unsigned i=0;i<n;++i)words[i/4]|=(uint32_t)data[i]<<(8u*(i%4));
        if(!e)e=wr(s,base+0x10,words[0]);
        if(!e && n>4)e=wr(s,base+0x14,words[1]);
    }
    if(!e){s->pending=1;s->pending_base=base;e=wr(s,base,(data?0u:1u<<27)|((uint32_t)reg<<4)|(n-1));}
    if(!e)e=wait(s,base);
    if(!e && out) {
        e=rd(s,base+0x18,&words[0]);if(!e && n>4)e=rd(s,base+0x1c,&words[1]);
        if(!e)for(unsigned i=0;i<n;++i)out[i]=(uint8_t)(words[i/4]>>(8u*(i%4)));
    }
    return finish(s,e);
}
int sm8850_spmi_v8_read(Sm8850SpmiV8 *s,const Sm8850SpmiChannel *c,uint8_t reg,uint8_t *out,unsigned n) {
    if(!out)return INVALID;
    return transfer(s,c,reg,out,0,n);
}
int sm8850_spmi_v8_write(Sm8850SpmiV8 *s,const Sm8850SpmiChannel *c,uint8_t reg,const uint8_t *data,unsigned n) {
    if(!data)return INVALID;
    return transfer(s,c,reg,0,data,n);
}
