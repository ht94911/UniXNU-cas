/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Existing AudioReach parameter layout and pandora graph container, with
 * full bounds and explicit target endpoints. Firmware blobs remain private. */
#include "AudioTopology.h"
#include <string.h>
static uint32_t rd(const uint8_t *p) {return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void wr(uint8_t *p,uint32_t n) {p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24;}
static void w16(uint8_t *p,uint32_t n) {p[0]=n;p[1]=n>>8;}
int na_spf_validate(const void *data,uint32_t bytes) {
    if (!data || !bytes || bytes>0x100000 || (bytes&7)) return NA_ARGUMENT;
    const uint8_t *p=data;uint32_t at=0,count=0;
    while (at<bytes) {
        if (bytes-at<16 || ++count>8192) return NA_WIRE;
        uint32_t n=rd(p+at+8);
        if (!rd(p+at) || !rd(p+at+4) || rd(p+at+12) || n>bytes-at-16 || n>0x100000-7) return NA_WIRE;
        uint32_t pad=(n+7)&~7u;if (pad>bytes-at-16) return NA_WIRE;
        for (unsigned i=n;i<pad;++i) if (p[at+16+i]) return NA_WIRE;
        at+=16+pad;
    }
    return at==bytes ? NA_OK:NA_WIRE;
}
static uint32_t parameter(uint8_t *p,uint32_t iid,uint32_t pid,const uint8_t *body,uint32_t n) {
    uint32_t padded=(n+7)&~7u;memset(p,0,16+padded);wr(p,iid);wr(p+4,pid);wr(p+8,n);memcpy(p+16,body,n);return 16+padded;
}
static int find_module(const void *open,uint32_t bytes,uint32_t type,uint32_t wanted,uint32_t *instance) {
    if (!instance || (!type && !wanted) || na_spf_validate(open,bytes)) return NA_ARGUMENT;
    const uint8_t *b=open;uint32_t found=0,selected=0;
    for (uint32_t at=0;at<bytes;) {
        uint32_t size=rd(b+at+8);
        if (rd(b+at+4)==0x08001002) {
            const uint8_t *p=b+at+16;uint32_t offset=4;
            if (size<4 || rd(p)>64) return NA_WIRE;
            for (unsigned i=0;i<rd(p);++i) {
                if (offset>size || size-offset<12) return NA_WIRE;
                uint32_t n=rd(p+offset+8);offset+=12;
                if (n>1024 || n>(size-offset)/8) return NA_WIRE;
                for (unsigned j=0;j<n;++j) {
                    if (type ? rd(p+offset)==type:rd(p+offset+4)==wanted) {selected=rd(p+offset+4);++found;}
                    offset+=8;
                }
            }
            if (size-offset>7) return NA_WIRE;
            while (offset<size) if (p[offset++]) return NA_WIRE;
        }
        at+=16+((size+7)&~7u);
    }
    if (found!=1 || !selected) return NA_MISSING;
    *instance=selected;return NA_OK;
}
int na_topology_find_module(const void *open,uint32_t bytes,uint32_t type,uint32_t *instance) {
    return find_module(open,bytes,type,0,instance);
}
static uint32_t pcm(uint8_t *out,uint32_t iid,unsigned channels,unsigned interleaving) {
    uint8_t p[32]={0};wr(p,1);wr(p+4,0x09001000);wr(p+8,(16+channels+3)&~3u);
    w16(p+12,16);w16(p+14,1);w16(p+16,16);w16(p+18,15);w16(p+20,1);w16(p+22,interleaving);w16(p+26,channels);
    p[28]=channels==1 ? 3:1;if (channels==2) p[29]=2;
    return parameter(out,iid,0x08001008,p,(12+16+channels+3)&~3u);
}
static uint32_t formats(uint8_t *out,const NaMediaEndpoints *e,int shared_only) {
    uint8_t p[48]={0};uint32_t n=0;
    if (shared_only) {
        wr(p,1);wr(p+4,0x09001000);wr(p+8,16+e->channels);
        wr(p+12,48000);w16(p+16,16);w16(p+18,1);w16(p+20,16);w16(p+22,15);w16(p+24,1);w16(p+26,e->channels);
        p[28]=e->channels==1 ? 3:1;if (e->channels==2) p[29]=2;
        return parameter(out,e->shared,0x0800100c,p,28+e->channels);
    }
    wr(p,48000);w16(p+4,e->hardware_bits);w16(p+6,e->channels);wr(p+8,1);
    n+=parameter(out+n,e->hardware,0x08001017,p,12);
    memset(p,0,sizeof p);wr(p,1);n+=parameter(out+n,e->hardware,0x08001018,p,4);
    if (e->capture==1) {
        wr(p,1);wr(p+4,4);wr(p+8,4); /*LPAIF_RXTX,TX3,DEC2 mask*/
        n+=parameter(out+n,e->hardware,0x08001063,p,12);
        memset(p,0,sizeof p);n+=parameter(out+n,e->hardware,0x08001176,p,4);
    }
    if (e->capture!=2) {
        n+=pcm(out+n,e->pcm[0],e->channels,1);
        n+=pcm(out+n,e->pcm[1],e->channels,e->capture ? 1:3);
    }
    if (e->mfc) {
        memset(p,0,sizeof p);wr(p,48000);w16(p+4,e->hardware_bits);w16(p+6,e->channels);w16(p+8,e->channels==1 ? 3:1);
        if (e->channels==2) w16(p+10,2);
        n+=parameter(out+n,e->mfc,0x08001024,p,8+2*e->channels);
    }
    if (e->spr) {
        memset(p,0,sizeof p);wr(p,e->hardware);n+=parameter(out+n,e->spr,0x080010c4,p,4);
        wr(p,1);n+=parameter(out+n,e->spr,0x08001b07,p,4);
    }
    /* Preserve ACDB volume/protection calibration. HAL and the private kernel
     * PCM stage attenuate; don't overwrite unidentified device volume modules. */
    return n;
}
int na_topology_load(const void *data,uint32_t bytes,void *dma,uint32_t capacity,const NaMediaEndpoints *e,NaTopology *out) {
    if (!data || !dma || !e || !out || bytes<12 || bytes>0x100000 || capacity<65536 || capacity>0x1000000 ||
        e->capture>2 || e->channels!=(e->capture==1 ? 1u:2u) || e->hardware_bits!=(e->capture==1 ? 16u:32u) ||
        !e->hardware || (e->capture!=2 && (!e->shared || !e->pcm[0] || !e->pcm[1])) || (!e->capture && !e->mfc)) return NA_ARGUMENT;
    const uint8_t *b=data,*section[6]={0};uint32_t length[6]={0};
    if (memcmp(b,"PSPKGR01",8) || rd(b+8)!=5) return NA_WIRE;
    uint32_t pos=12;
    for (unsigned i=0;i<5;++i) {
        if (bytes-pos<8) return NA_WIRE;
        uint32_t kind=rd(b+pos),n=rd(b+pos+4);pos+=8;
        if (!kind || kind>5 || section[kind] || n>bytes-pos || n>0x100000-7 || ((n+7)&~7u)>bytes-pos) return NA_WIRE;
        for (uint32_t j=n;j<((n+7)&~7u);++j) if (b[pos+j]) return NA_WIRE;
        section[kind]=b+pos;length[kind]=n;pos+=(n+7)&~7u;
    }
    if (pos!=bytes || na_spf_validate(section[1],length[1]) || na_spf_validate(section[2],length[2]) ||
        na_spf_validate(section[3],length[3]) || length[4]<8 || length[5]<8) return NA_WIRE;
    NaTopology t={0};t.subgraph_count=rd(section[4]);
    if (!t.subgraph_count || t.subgraph_count>16 || length[4]!=4+4*t.subgraph_count) return NA_WIRE;
    for (unsigned i=0;i<t.subgraph_count;++i) {
        t.subgraphs[i]=rd(section[4]+4+4*i);if (!t.subgraphs[i]) return NA_WIRE;
        for (unsigned j=0;j<i;++j) if (t.subgraphs[j]==t.subgraphs[i]) return NA_WIRE;
    }
    uint32_t count=rd(section[5]+4);
    if (rd(section[5])!=e->channels || count>32 || length[5]!=8+8*count) return NA_WIRE;
    uint32_t expected[7]={e->shared,e->pcm[0],e->pcm[1],e->mfc,e->hardware,e->spr,e->volume};
    for (unsigned i=0;i<7;++i) if (expected[i]) {
        unsigned found=0;
        for (unsigned j=0;j<count;++j) if (rd(section[5]+12+8*j)==expected[i]) ++found;
        uint32_t actual=0;
        if (found!=1 || find_module(section[1],length[1],0,expected[i],&actual) || actual!=expected[i]) return NA_MISSING;
    }
    uint8_t format[512],shared[64];uint32_t fn=formats(format,e,0),sn=e->capture ? 0:formats(shared,e,1);
    if (fn>sizeof format || sn>sizeof shared || na_spf_validate(format,fn) || (sn && na_spf_validate(shared,sn))) return NA_WIRE;
    uint32_t total=32768+length[1]+length[2]+length[3]+fn+sn;
    if (total>capacity) return NA_OVERFLOW;
    /* Parse completed, write only to the caller's separate CPU-owned region. */
    uint8_t *dest=dma;uint32_t offset=32768;
    for (unsigned i=1;i<=3;++i) {
        memcpy(dest+offset,section[i],length[i]);
        if (i==1) t.open=(NaBlob){offset,length[i]};else t.config[t.configs++]=(NaBlob){offset,length[i]};
        offset+=length[i];
    }
    memcpy(dest+offset,format,fn);t.config[t.configs++]=(NaBlob){offset,fn};offset+=fn;
    if (sn) {memcpy(dest+offset,shared,sn);t.config_peer[t.configs]=e->shared;t.config[t.configs++]=(NaBlob){offset,sn};}
    *out=t;return NA_OK;
}
