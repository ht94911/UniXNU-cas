/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Sm8850Resources.h"
#include <string.h>
static int lock(Sm8850Resources *r) {
    if (!r || !r->initialized) return Sm8850Invalid;
    return r->io.lock(r->io.context) ? Sm8850Busy:Sm8850Okay;
}
static void unlock(Sm8850Resources *r) {r->io.unlock(r->io.context);}
static int rd(Sm8850Resources *r,unsigned d,uint32_t o,uint32_t *v) {return r->io.read32(r->io.context,d,o,v);}
static int wr(Sm8850Resources *r,unsigned d,uint32_t o,uint32_t v) {return r->io.write32(r->io.context,d,o,v);}
static int fence(Sm8850Resources *r) {return r->io.barrier(r->io.context);}
int sm8850_resources_init(Sm8850Resources *r,const Sm8850ResourceIo *io) {
    if (!r || r->initialized || !io || !io->lock || !io->unlock || !io->read32 ||
        !io->write32 || !io->read64 || !io->barrier) return Sm8850Invalid;
    memset(r,0,sizeof *r);r->io=*io;r->initialized=1;return Sm8850Okay;
}
int sm8850_resource_acquire(Sm8850Resources *r,unsigned slot,uint32_t owner,uint64_t *lease) {
    if (slot>=Sm8850ResourceCount || !owner || !lease) return Sm8850Invalid;
    *lease=0;int e=lock(r);if (e) return e;
    if (r->owner[slot] || r->sequence==UINT64_MAX) e=Sm8850Busy;
    else {r->owner[slot]=owner;r->lease[slot]=++r->sequence;*lease=r->lease[slot];}
    unlock(r);return e;
}
int sm8850_resource_enter(Sm8850Resources *r,unsigned slot,uint32_t owner,uint64_t lease) {
    if (slot>=Sm8850ResourceCount || !owner || !lease) return Sm8850Invalid;
    int e=lock(r);if (e) return e;
    if (r->owner[slot]!=owner || r->lease[slot]!=lease || r->quarantined[slot]) {unlock(r);return Sm8850Conflict;}
    return Sm8850Okay;
}
void sm8850_resource_leave(Sm8850Resources *r) {unlock(r);}
int sm8850_resource_release(Sm8850Resources *r,unsigned slot,uint32_t owner,uint64_t lease,int quiet) {
    if (!quiet) return Sm8850Conflict;
    int e=sm8850_resource_enter(r,slot,owner,lease);if (e) return e;
    r->owner[slot]=0;r->lease[slot]=0;unlock(r);return Sm8850Okay;
}
int sm8850_resource_quarantine(Sm8850Resources *r,unsigned slot,uint32_t owner,uint64_t lease) {
    int e=sm8850_resource_enter(r,slot,owner,lease);if (e) return e;
    r->quarantined[slot]=1;unlock(r);return Sm8850Okay;
}
int sm8850_smmu_check_assignment(Sm8850Resources *r,const Sm8850SmmuAssignment *a) {
    uint32_t id0,id1,id2,cr,fsr;
    unsigned count=a && a->count ? a->count:4;
    if(count>4)return Sm8850Invalid;
    if (!r || !a || !a->asid || a->asid>65535) return Sm8850Invalid;
    if (rd(r,0,0x20,&id0) || rd(r,0,0x24,&id1) || rd(r,0,0x28,&id2) ||
        rd(r,0,0,&cr) || rd(r,0,0x48,&fsr)) return Sm8850Io;
    /* This platform uses the observed 15-bit SMR layout, not EXIDS. */
    unsigned slots=id0&255,banks=id1&255,stage2=(id1>>16)&255;
    unsigned shift=(id1>>31)?16:12,pages=2u<<((id1>>28)&7);
    if (!(id0&(1u<<27)) || (id0&(1u<<8)) || ((id0>>9)&15)!=15 ||
        !slots || slots>128 || !banks || banks>128 || banks>pages ||
        a->bank<stage2 || a->bank>=banks || shift!=12 || pages>128 ||
        !(id2&(1u<<12)) || (cr&0x40f)!=0x406 || fsr) return Sm8850Conflict;
    for (unsigned j=0;j<count;++j) {
        if (a->slot[j]>=slots || a->sid[j]>0x7fff) return Sm8850Invalid;
        for (unsigned k=0;k<j;++k) if (a->slot[j]==a->slot[k] || a->sid[j]==a->sid[k]) return Sm8850Invalid;
    }
    for (unsigned i=0;i<slots;++i) {
        uint32_t smr,s2cr;
        if (rd(r,0,0x800+4*i,&smr) || rd(r,0,0xc00+4*i,&s2cr)) return Sm8850Io;
        for (unsigned j=0;j<count;++j) if (i==a->slot[j] && ((smr&0x80000000u) || s2cr!=0x20000)) return Sm8850Conflict;
        if (!(smr&0x80000000u)) continue;
        if (((s2cr>>16)&3)==0 && (s2cr&255)==a->bank) return Sm8850Conflict;
        for (unsigned j=0;j<count;++j) if (!((a->sid[j]^smr)&~(smr>>16)&0x7fff)) return Sm8850Conflict;
    }
    for (unsigned i=0;i<banks;++i) {
        uint32_t cbar,sctlr;uint64_t tt0,tt1;
        if (rd(r,0,0x1000+4*i,&cbar)) return Sm8850Io;
        if (((cbar>>16)&3)==3 && ((cbar>>8)&255)==a->bank) return Sm8850Conflict;
        if (i<stage2) continue;
        uint32_t cb=(pages+i)<<shift;
        if (rd(r,0,cb,&sctlr) || r->io.read64(r->io.context,0,cb+0x20,&tt0) ||
            r->io.read64(r->io.context,0,cb+0x28,&tt1)) return Sm8850Io;
        /* Nonzero inactive SCTLR/TTBR reset bits are not active translation. */
        if (i==a->bank && (sctlr&1)) return Sm8850Conflict;
        if ((tt0>>48)==a->asid || (tt1>>48)==a->asid) return Sm8850Conflict;
    }
    if (rd(r,0,(pages+a->bank)*4096+0x58,&fsr)) return Sm8850Io;
    /* Follow the existing USB decoder's meaningful fault bits. */
    if (fsr&~0x600u) return Sm8850Conflict;
    return Sm8850Okay;
}
int sm8850_tcs_idle(Sm8850Resources *r,unsigned tcs) {
    if (!r || tcs>=3) return Sm8850Invalid;
    uint32_t id,config,status,control,enable,wait,irq,b=0xd00+0x2a0*tcs;
    if (rd(r,1,0,&id) || rd(r,1,0xc,&config) || rd(r,1,b+0x28,&status) ||
        rd(r,1,b+0x24,&control) || rd(r,1,b+0x2c,&enable) || rd(r,1,b+0x20,&wait) || rd(r,1,0xd04,&irq)) return Sm8850Io;
    if (id!=0x40300 || config!=0x800c8104 || status!=1 || control || enable || wait || (irq&(1u<<tcs))) return Sm8850Conflict;
    return Sm8850Okay;
}
int sm8850_tcs_write(Sm8850Resources *r,unsigned tcs,uint32_t offset,uint32_t value) {
    if (!r || tcs>=3 || (offset&3)) return Sm8850Invalid;
    uint32_t base=0xd00+tcs*0x2a0,rel=offset-base;
    int command=rel>=0x34 && rel<0x34+16*0x18 && ((rel-0x34)%0x18)<=8;
    int local=offset>=base && (rel==0x20 || rel==0x24 || rel==0x2c || command);
    if (!local && !(offset==0xd08 && value==(1u<<tcs))) return Sm8850Conflict;
    return wr(r,1,offset,value) || fence(r) ? Sm8850Io:Sm8850Okay;
}
int sm8850_tcs_irq_update(Sm8850Resources *r,unsigned tcs,unsigned enabled,unsigned *previous) {
    if (!r || tcs>=3 || enabled>1 || !previous) return Sm8850Invalid;
    uint32_t old,got,bit=1u<<tcs;
    if (rd(r,1,0xd00,&old)) return Sm8850Io;
    *previous=!!(old&bit);
    uint32_t value=(old&~bit)|(enabled ? bit:0);
    if (wr(r,1,0xd00,value) || fence(r) || rd(r,1,0xd00,&got)) return Sm8850Io;
    return got==value ? Sm8850Okay:Sm8850Conflict;
}
int sm8850_gcc_update(Sm8850Resources *r,uint32_t offset,uint32_t allowed,uint32_t mask,uint32_t value) {
    if (!r || (offset&3) || !mask || (mask&~allowed) || (value&~mask)) return Sm8850Invalid;
    uint32_t old,got;
    if (rd(r,2,offset,&old) || wr(r,2,offset,(old&~mask)|value) || fence(r) || rd(r,2,offset,&got)) return Sm8850Io;
    return got==((old&~mask)|value) ? Sm8850Okay:Sm8850Conflict;
}
int sm8850_smem_trylock(Sm8850Resources *r,uint32_t owner) {
    if (!owner || owner==UINT32_MAX) return Sm8850Invalid;
    int e=lock(r);if (e) return e;
    uint32_t v;
    if (r->smem_owner) e=Sm8850Busy;
    else if (wr(r,3,0,1) || fence(r) || rd(r,3,0,&v)) {r->smem_owner=UINT32_MAX;e=Sm8850Io;}
    else if (v!=1) e=Sm8850Busy;
    else r->smem_owner=owner;
    unlock(r);return e;
}
int sm8850_smem_unlock(Sm8850Resources *r,uint32_t owner) {
    if (!owner || owner==UINT32_MAX) return Sm8850Invalid;
    int e=lock(r);if (e) return e;uint32_t v;
    if (r->smem_owner!=owner) e=Sm8850Conflict;
    else if (rd(r,3,0,&v) || v!=1 || wr(r,3,0,0) || fence(r)) {r->smem_owner=UINT32_MAX;e=Sm8850Io;}
    else r->smem_owner=0;
    unlock(r);return e;
}
int sm8850_ipcc_register(Sm8850Resources *r,uint32_t id,void (*event)(void *,uint32_t),void *ctx) {
    if (id==UINT32_MAX || !event) return Sm8850Invalid;
    int e=lock(r);if (e) return e;
    if (r->ipcc_sealed) {unlock(r);return Sm8850Conflict;}
    for (unsigned i=0;i<r->ipcc_count;++i) if (r->ipcc[i].id==id) {unlock(r);return Sm8850Conflict;}
    if (r->ipcc_count==Sm8850IpccRoutes) e=Sm8850Busy;
    else {
        /* Retain the route even if enable is indeterminate. */
        r->ipcc[r->ipcc_count++]=(Sm8850IpccRoute){id,ctx,event};
        if (wr(r,4,0x14,id) || fence(r)) e=Sm8850Io;
    }
    unlock(r);return e;
}
int sm8850_ipcc_send(Sm8850Resources *r,uint32_t id) {
    int e=lock(r);if (e) return e;
    unsigned found=0;for (unsigned i=0;i<r->ipcc_count;++i) found|=r->ipcc[i].id==id;
    if (!found) e=Sm8850Conflict;
    else if (wr(r,4,0xc,id) || fence(r)) e=Sm8850Io;
    unlock(r);return e;
}
int sm8850_ipcc_dispatch(Sm8850Resources *r,unsigned budget) {
    if (!budget || budget>64) return Sm8850Invalid;
    int e=lock(r);if (e) return e;
    if (__atomic_exchange_n(&r->ipcc_busy,1,__ATOMIC_ACQ_REL)) {unlock(r);return Sm8850Busy;}
    r->ipcc_sealed=1;unlock(r);
    for (unsigned n=0;n<budget;++n) {
        /* Registration is initialization-only, before dispatch starts. No
         * mutation or deletion of published entries is permitted. */
        uint32_t id;Sm8850IpccRoute route={0};
        if (rd(r,4,0x10,&id)) {e=Sm8850Io;break;}
        if (id==UINT32_MAX) break;
        for (unsigned i=0;i<r->ipcc_count;++i) if (r->ipcc[i].id==id) {route=r->ipcc[i];break;}
        if (!route.event) {++r->ipcc_unknown_count;r->ipcc_last_unknown=id;}
        if (wr(r,4,0x1c,id) || fence(r)) {e=Sm8850Io;break;}
        if (route.event) route.event(route.context,id);
    }
    /* The single dispatcher flag does not protect a callback's own state. */
    __atomic_store_n(&r->ipcc_busy,0,__ATOMIC_RELEASE);return e;
}
