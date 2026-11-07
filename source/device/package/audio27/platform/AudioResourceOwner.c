/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioResourceOwner.h"
#include <string.h>
#define AUDIO_OWNER 0x4e41u
static int error(int e) {
    return e==Sm8850Okay ? NA_OK:e==Sm8850Busy ? NA_BUSY:e==Sm8850Io ? NA_IO:NA_OWNERSHIP;
}
static int fail(NaResourceOwner *o,int e) {o->error=error(e);return o->error;}
int na_resource_owner_init(NaResourceOwner *o,Sm8850Resources *common) {
    if (!o || o->common || !common || !common->initialized) return NA_ARGUMENT;
    memset(o,0,sizeof *o);o->common=common;
    /* 004 showed these exact entries invalid/faulted and bank7 disabled.
     * This is board allocation policy, never a claim based on idleness alone.
     * Runtime checks and the shared lease still precede every first write. */
    o->assignment=(Sm8850SmmuAssignment){7,0xfe00,{9,10,11,12},{0x1001,0x1081,0x1041,0x1061}};
    return NA_OK;
}
static int enter(void *v,uint64_t lease) {
    NaResourceOwner *o=v;
    if (o->error || lease!=o->smmu_lease) return NA_OWNERSHIP;
    int r=sm8850_resource_enter(o->common,NaResourceSmmu,AUDIO_OWNER,lease);if (r) return error(r);
    if (!o->smmu_attempted) {
        r=sm8850_smmu_check_assignment(o->common,&o->assignment);
        if (r) {sm8850_resource_leave(o->common);return fail(o,r);}
        o->smmu_attempted=1;
    }
    return NA_OK;
}
static void leave(void *v) {sm8850_resource_leave(((NaResourceOwner *)v)->common);}
int na_resource_owner_smmu(NaResourceOwner *o,NaSmmuOwner *smmu) {
    if (!o || !o->common || !smmu || o->smmu_lease || o->error) return NA_ARGUMENT;
    int r=sm8850_resource_acquire(o->common,NaResourceSmmu,AUDIO_OWNER,&o->smmu_lease);if (r) return error(r);
    smmu->lease=o->smmu_lease;smmu->bank=o->assignment.bank;smmu->asid=o->assignment.asid;
    memcpy(smmu->stream_slot,o->assignment.slot,sizeof smmu->stream_slot);
    smmu->config_context=o;smmu->enter=enter;smmu->leave=leave;
    return NA_OK;
}
static int acquire(void *v,uint32_t kind,uint64_t *lease) {
    NaResourceOwner *o=v;
    if (!lease || kind>NaPowerTcs || o->error) return NA_OWNERSHIP;
    unsigned slot=kind==NaPowerTcs ? NaResourceAdspTcs:NaResourceQmp;
    uint64_t *owned=kind==NaPowerTcs ? &o->tcs_lease:&o->qmp_lease;
    if (*owned) return NA_OWNERSHIP;
    int r=sm8850_resource_acquire(o->common,slot,AUDIO_OWNER,owned);if (r) return error(r);
    *lease=*owned;
    if (kind==NaPowerTcs) {
        r=sm8850_resource_enter(o->common,slot,AUDIO_OWNER,*owned);
        if (!r) {r=sm8850_tcs_idle(o->common,1);sm8850_resource_leave(o->common);}
        if (r) {sm8850_resource_quarantine(o->common,slot,AUDIO_OWNER,*owned);return fail(o,r);}
    }
    return NA_OK;
}
static int release(void *v,uint32_t kind,uint64_t lease) {
    NaResourceOwner *o=v;
    if (kind>NaPowerTcs || o->error) return NA_OWNERSHIP;
    unsigned slot=kind==NaPowerTcs ? NaResourceAdspTcs:NaResourceQmp;
    uint64_t *owned=kind==NaPowerTcs ? &o->tcs_lease:&o->qmp_lease;
    if (!lease || *owned!=lease) return NA_OWNERSHIP;
    if (kind==NaPowerTcs) {
        int r=sm8850_resource_enter(o->common,slot,AUDIO_OWNER,lease);if (r) return error(r);
        r=sm8850_tcs_idle(o->common,1);sm8850_resource_leave(o->common);
        if (r) {sm8850_resource_quarantine(o->common,slot,AUDIO_OWNER,lease);return fail(o,r);}
    }
    /* QMP release follows the existing parser's real remote acknowledgement. */
    int r=sm8850_resource_release(o->common,slot,AUDIO_OWNER,lease,1);
    if (!r) *owned=0;return error(r);
}
static uint32_t read_rsc(void *v,uint32_t offset) {
    NaResourceOwner *o=v;uint32_t value=UINT32_MAX;
    if (o->error || (offset&3) || offset>=0x10000) return value;
    Sm8850ResourceIo *io=&o->common->io;
    if (io->lock(io->context)) {o->error=NA_BUSY;return value;}
    int r=io->read32(io->context,Sm8850Rsc,offset,&value);io->unlock(io->context);
    if (r) o->error=NA_IO;return r ? UINT32_MAX:value;
}
static void write_rsc(void *v,uint32_t offset,uint32_t value) {
    NaResourceOwner *o=v;if (o->error) return;
    int r=sm8850_resource_enter(o->common,NaResourceAdspTcs,AUDIO_OWNER,o->tcs_lease);
    if (!r) {r=sm8850_tcs_write(o->common,1,offset,value);sm8850_resource_leave(o->common);}
    if (r) fail(o,r);
}
static void kick_aop(void *v) {
    NaResourceOwner *o=v;if (o->error || !o->qmp_lease) {o->error=NA_OWNERSHIP;return;}
    int r=sm8850_ipcc_send(o->common,0);if (r) fail(o,r);
}
NaProxyOps na_resource_owner_proxy(NaResourceOwner *o) {
    return (NaProxyOps){o,acquire,release,read_rsc,write_rsc,kick_aop};
}
int na_resource_owner_smem_lock(void *v) {
    NaResourceOwner *o=v;if (!o || !o->common || o->error) return NA_OWNERSHIP;
    return error(sm8850_smem_trylock(o->common,AUDIO_OWNER));
}
void na_resource_owner_smem_unlock(void *v) {
    NaResourceOwner *o=v;int r=sm8850_smem_unlock(o->common,AUDIO_OWNER);if (r) fail(o,r);
}
void na_resource_owner_doorbell(void *v,uint32_t signal) {
    NaResourceOwner *o=v;if (o->error) return;
    if (signal!=0 && signal!=2) {o->error=NA_ARGUMENT;return;}
    int r=sm8850_ipcc_send(o->common,0x30000|signal);if (r) fail(o,r);
}
static void event(void *v,uint32_t id) {
    NaResourceOwner *o=v;
    if (id==0x30002 && o->adsp) na_adsp_smp2p_event(o->adsp);
    /* GLINK and QMP use their shared-memory producer/consumer indices. */
}
int na_resource_owner_ipcc(NaResourceOwner *o,NaAdsp *adsp) {
    if (!o || !o->common || !adsp || o->registered || o->error) return NA_ARGUMENT;
    o->adsp=adsp;o->registered=1;
    const uint32_t ids[]={0,0x30000,0x30002};
    for (unsigned i=0;i<3;++i) {
        int r=sm8850_ipcc_register(o->common,ids[i],event,o);if (r) return fail(o,r);
    }
    return NA_OK;
}
int na_resource_owner_dispatch(NaResourceOwner *o) {
    if (!o || !o->common || !o->registered || o->error) return NA_OWNERSHIP;
    int r=sm8850_ipcc_dispatch(o->common,32);return r ? fail(o,r):NA_OK;
}
