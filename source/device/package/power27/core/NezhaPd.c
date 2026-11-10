/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaPd.h"
#include "NezhaMca.h"
#include <string.h>
int np_pd_decode(NpPdCaps *caps,const uint8_t *data,size_t bytes,uint64_t epoch,uint64_t at)
{
    if(!caps)return -1;
    memset(caps,0,sizeof(*caps));caps->epoch=epoch;caps->at_ms=at;
    if(!data||bytes!=NP_PD_BYTES||!epoch)return -1;
    for(unsigned i=0;i<NP_PD_ENTRIES;++i) {
        NpPdEntry *p=&caps->entry[i];const uint8_t *b=data+16*i;
        p->min_mv=np_le32(b);p->max_mv=np_le32(b+4);p->max_ma=np_le32(b+8);p->type=np_le32(b+12);
        if(!(p->min_mv|p->max_mv|p->max_ma|p->type))continue;
        caps->present_mask|=1u<<i;
        if(p->min_mv<3000||p->max_mv<p->min_mv||p->max_mv>22000||!p->max_ma||p->max_ma>10000||
           (p->type!=NP_PD_FIXED&&p->type!=NP_PD_APDO)||
           (p->type==NP_PD_FIXED&&p->min_mv!=p->max_mv))caps->invalid_mask|=1u<<i;
    }
    caps->valid=!!caps->present_mask&&!caps->invalid_mask;
    return caps->valid?0:-1;
}
int np_pd_choose(const NpPdCaps *caps,uint64_t epoch,uint32_t mv,uint32_t limit,int allow_pps,NpPdChoice *out)
{
    if(!out)return -1;
    memset(out,0,sizeof(*out));
    if(!caps||!caps->valid||caps->epoch!=epoch||!epoch||mv<3000||mv>22000||!limit||limit>10000)return -1;
    /* Prefer a matching fixed PDO before requesting a programmable supply. */
    for(unsigned pass=0;pass<2;++pass)for(unsigned i=0;i<NP_PD_ENTRIES;++i) {
        const NpPdEntry *p=&caps->entry[i];
        if(!(caps->present_mask&(1u<<i))||p->type!=(pass?NP_PD_APDO:NP_PD_FIXED)||
           mv<p->min_mv||mv>p->max_mv||(pass&&(!allow_pps||mv%20)))continue;
        uint32_t ma=p->max_ma<limit?p->max_ma:limit;
        if(pass)ma-=ma%50;
        if(!ma)continue;
        out->mv=mv;out->ma=ma;out->index=i;out->pps=pass;return 0;
    }
    return -1;
}
int np_pd_status(uint32_t wire,uint32_t *mv,uint32_t *ma)
{
    if(!mv||!ma)return -1;
    *mv=*ma=0;
    uint32_t v=(wire>>16)*20u,a=(wire&65535u)*50u;
    if(v<3000||v>22000||a>10000)return -1;
    *mv=v;*ma=a;return 0;
}
