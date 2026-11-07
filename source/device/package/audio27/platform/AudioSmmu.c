/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioSmmu.h"
#include <string.h>
static int failure(NaSmmu *s,int r) {s->error=r;s->quarantine=1;return r;}
int na_smmu_start(NaSmmu *s,const NaSmmuOwner *o,const NaRange *physical,const uint32_t *iova,uint32_t count,
                  void *tables,uint64_t table_pa,uint32_t pages,const NaRange *reserved,uint32_t nreserved) {
    if (!s || !o || !o->lease || !o->powered || !o->fault_handler_installed || !o->global_lease_held || !o->clean ||
        !physical || !iova || !tables || ((uintptr_t)tables&4095) || !pages || pages>64 || !count || count>3 ||
        nreserved>128 || (nreserved && !reserved)) return NA_ARGUMENT;
    memset(s,0,sizeof *s);s->owner=*o;
    NaRange pool={table_pa,(uint64_t)pages*4096};
    uint64_t encoded;ArmSmmu4KRange range[3];
    for (unsigned i=0;i<count;++i) {
        if (na_dma_contract(physical[i],iova[i],na_nezha_facts.sid[0],reserved,nreserved,0,0,&encoded) ||
            na_ranges_overlap(physical[i],pool)) return NA_OWNERSHIP;
        for (unsigned j=0;j<i;++j) if (na_ranges_overlap(physical[i],physical[j]) ||
            (uint64_t)iova[i]<iova[j]+physical[j].bytes) return NA_OWNERSHIP;
        range[i]=(ArmSmmu4KRange){iova[i],physical[i].base,physical[i].bytes,ArmSmmu4KRead|ArmSmmu4KWrite,1,3};
    }
    if (!na_range_valid(pool) || (table_pa&4095) || na_ranges_overlap(pool,na_nezha_facts.firmware) || na_ranges_overlap(pool,na_nezha_facts.dtb)) return NA_OWNERSHIP;
    for (unsigned i=0;i<nreserved;++i) if (na_ranges_overlap(pool,reserved[i])) return NA_OWNERSHIP;
    /* Mask expansion is exact: 1001/80 ->1001,1081;1041/20 ->1041,1061.
     * Existing stream checker rejects every live overlapping route and bank.
     * No SMR mask probe, no global reset, no route discovered as 'unused'. */
    const uint16_t sid[4]={0x1001,0x1081,0x1041,0x1061};ArmSmmu4KStream stream[4];
    for (unsigned i=0;i<4;++i) {
        stream[i]=(ArmSmmu4KStream){o->stream_slot[i],sid[i]};
        for (unsigned j=0;j<i;++j) if (o->stream_slot[j]==o->stream_slot[i]) return NA_ARGUMENT;
    }
    if (ArmSmmu4KCheckFaultedStreams(&o->io,stream,4,o->bank,&s->stream_result)) return failure(s,NA_OWNERSHIP);
    s->tables_config=(ArmSmmu4KConfig){32,o->output_bits,table_pa,4096};
    if (ArmSmmu4KBuild(&s->tables_config,range,count,tables,pages,&s->tables_result)) return failure(s,NA_WIRE);
    if (o->clean(o->sync_context,tables,table_pa,s->tables_result.table_pages*4096)) return failure(s,NA_IO);
    s->context=(ArmSmmu4KContextConfig){.lower=&s->tables_config,.lower_tables=&s->tables_result,
        .asid=o->asid,.coherent_walk=1,.stall_on_fault=0,.continue_after_fault=0,.ttbr0_switched=0};
    if (ArmSmmu4KProgramDisabledContext(&o->io,o->bank,&s->context,10000,&s->program_result)) return failure(s,NA_IO);
    s->context.id0=s->program_result.id0;s->context.id2=s->program_result.id2;
    if (ArmSmmu4KAttachStreams(&o->io,&s->streams,stream,4,o->bank,&s->context,&s->stream_result)) return failure(s,NA_IO);
    s->started=1;return NA_OK;
}
int na_smmu_observe(NaSmmu *s,int full) {
    if (!s || !s->started || s->quarantine || !s->owner.global_lease_held) return NA_OWNERSHIP;
    ArmSmmu4KStatus r=full ? ArmSmmu4KObserveStreams(&s->owner.io,&s->streams,&s->context,&s->stream_result):
        ArmSmmu4KObserveOwnedStreams(&s->owner.io,&s->streams,&s->context,&s->stream_result);
    return r ? failure(s,NA_IO):NA_OK;
}
int na_smmu_stop(NaSmmu *s,int quiet) {
    if (!s || !quiet || !s->owner.global_lease_held || !s->started || s->quarantine) return NA_OWNERSHIP;
    if (ArmSmmu4KFaultStreams(&s->owner.io,&s->streams,&s->stream_result) ||
        ArmSmmu4KDisableOwnedContext(&s->owner.io,s->owner.bank,&s->context,10000,&s->program_result)) return failure(s,NA_IO);
    s->started=0;return NA_OK; /* Caller still owns pool/lease; no implicit free. */
}
