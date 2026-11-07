/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioAssembly.h"
#include <string.h>
static int owned(NaOwnedBuffer b) {
    return b.cpu && !((uintptr_t)b.cpu&4095) && na_range_valid(b.physical) &&
        !((b.physical.base|b.physical.bytes)&4095) && b.physical.bytes<=0x1000000;
}
static int failed(NaAssembly *a,int r) {a->error=r<0 ? r:NA_IO;return a->error;}
static int smem_lock(void *v) {NaAssembly *a=v;return a->plan.ops.smem_lock(a->plan.ops.context);}
static void smem_unlock(void *v) {NaAssembly *a=v;a->plan.ops.smem_unlock(a->plan.ops.context);}
static void doorbell(void *v,uint32_t signal) {NaAssembly *a=v;a->plan.ops.doorbell(a->plan.ops.context,signal);}
static int poll_shared(void *v) {NaAssembly *a=v;return a->plan.ops.poll_shared(a->plan.ops.context);}
static int proxy(void *v,int on) {
    NaAssembly *a=v;int r=na_proxy_request(&a->proxy,on);
    return r ? r:na_proxy_poll(&a->proxy,a->driver.now_ms);
}
static int dma_setup(void *v) {
    NaAssembly *a=v;
    if (!a->adsp.masters_stopped || a->error) return NA_OWNERSHIP;
    if (a->smmu.started) return NA_OK;
    if (a->dma_attempted) return NA_OWNERSHIP;
    a->dma_attempted=1;
    int r=na_smmu_start(&a->smmu,&a->plan.smmu,a->dma_range,a->iova,a->dma_count,
        a->plan.tables.cpu,a->plan.tables.physical.base,(uint32_t)(a->plan.tables.physical.bytes/4096),
        a->reserved,a->reserved_count);
    if (r==NA_BUSY && !a->smmu.quarantine) {a->dma_attempted=0;return r;}
    return r ? failed(a,r):NA_OK;
}
static int sync_dma(void *v,unsigned direction,uint32_t offset,uint32_t bytes,int for_device) {
    NaAssembly *a=v;
    if (direction>2 || !a->smmu.started || a->smmu.quarantine) return NA_OWNERSHIP;
    const NaOwnedBuffer *b=&a->plan.stream[direction].dma;
    if (!bytes || offset>b->physical.bytes || bytes>b->physical.bytes-offset) return NA_ARGUMENT;
    return a->plan.ops.sync(a->plan.ops.context,b,offset,bytes,for_device);
}
static int health(void *v) {
    NaAssembly *a=v;
    if (a->speaker_board.error || a->mic_board.error) return NA_IO;
    return na_smmu_observe(&a->smmu,0);
}
static int mic_prepare(void *v,NaWcdCapture *c) {return na_mic_board_prepare(&((NaAssembly *)v)->mic_board,c);}
static int mic_release(void *v) {return na_mic_board_release(&((NaAssembly *)v)->mic_board);}
static uint64_t host_time(void *v) {NaAssembly *a=v;return a->plan.ops.host_time(a->plan.ops.context);}
void na_assembly_smp2p(NaAssembly *a) {
    if (a && __atomic_load_n(&a->initialized,__ATOMIC_ACQUIRE)) na_adsp_smp2p_event(&a->adsp);
}
int na_assembly_init(NaAssembly *a,const NaAssemblyPlan *p,uint64_t now) {
    if (!a || !p || a->initialized || a->error || !p->adsp_lease || !p->smem ||
        !p->ops.smem_lock || !p->ops.smem_unlock || !p->ops.doorbell || !p->ops.sync || !p->ops.host_time || !p->ops.poll_shared ||
        !p->tuning || !p->calibration.valid || !p->ti_instance || !p->smmu.lease ||
        !owned(p->tables) || p->tables.physical.bytes>64*4096 ||
        p->reserved_count>126 || (p->reserved_count && !p->reserved)) return NA_ARGUMENT;
    /* Check the entire allocation set before writing any CPU buffer. Metadata,
     * page tables and PCM/topology allocations cannot alias one another. */
    NaOwnedBuffer buffers[6]={p->tables,p->metadata[0],p->metadata[1]};unsigned count=3;
    for (unsigned i=0;i<3;++i) {
        if (i==1 && !p->stream[i].graph) continue;
        if (!p->stream[i].graph || p->stream[i].media.capture!=(i==0 ? 0u:i==1 ? 1u:2u)) return NA_ARGUMENT;
        buffers[count++]=p->stream[i].dma;
    }
    for (unsigned i=0;i<count;++i) {
        NaOwnedBuffer b=buffers[i];
        if (!owned(b) || na_ranges_overlap(b.physical,na_nezha_facts.firmware) || na_ranges_overlap(b.physical,na_nezha_facts.dtb)) return NA_OWNERSHIP;
        for (unsigned j=0;j<p->reserved_count;++j) if (na_ranges_overlap(b.physical,p->reserved[j])) return NA_OWNERSHIP;
        for (unsigned j=0;j<i;++j) if (na_ranges_overlap(b.physical,buffers[j].physical) ||
            na_ranges_overlap((NaRange){(uintptr_t)b.cpu,b.physical.bytes},
                              (NaRange){(uintptr_t)buffers[j].cpu,buffers[j].physical.bytes})) return NA_OWNERSHIP;
    }
    memset(a,0,sizeof *a);a->plan=*p;
    for (unsigned i=0;i<p->reserved_count;++i) a->reserved[a->reserved_count++]=p->reserved[i];
    a->reserved[a->reserved_count++]=p->metadata[0].physical;
    a->reserved[a->reserved_count++]=p->metadata[1].physical;
    const NaRange region[2]={na_nezha_facts.firmware,na_nezha_facts.dtb};
    for (unsigned i=0;i<2;++i) {
        int r=na_split_firmware(&a->firmware[i],p->firmware[i],p->firmware_count[i]);if (r) return failed(a,r);
        r=na_pas_begin(&a->loader[i],&a->firmware[i],i ? 36:1,region[i],p->firmware_map[i],
            p->metadata[i].cpu,p->metadata[i].physical.base,p->metadata[i].physical.bytes,&p->pas,now);
        if (r) return failed(a,r);
    }
    int r=na_proxy_init(&a->proxy,p->qmp,0x400,p->cmd_db,0x20000,p->tcs,&p->proxy);if (r) return failed(a,r);
    NaTasPowerOps tas;
    r=na_geni_board_init(&a->speaker_board,&p->board,&tas);if (r) return failed(a,r);
    r=na_tas_endpoint_init(&a->speaker,p->tuning,&p->tas,&tas);if (r) return failed(a,r);
    if (p->stream[1].graph) {r=na_mic_board_init(&a->mic_board,&p->board,1,0);if (r) return failed(a,r);}
    NaAdspOps adsp={a,smem_lock,smem_unlock,doorbell,proxy,dma_setup,poll_shared};
    r=na_adsp_init(&a->adsp,p->smem,0x200000,&a->loader[0],&a->loader[1],&a->rx,&adsp);if (r) return failed(a,r);
    NaDriverStream stream[3]={0};
    for (unsigned i=0;i<3;++i) if (p->stream[i].graph) {
        const NaAssemblyStream *s=&p->stream[i];uint64_t encoded=0;
        r=na_dma_contract(s->dma.physical,s->iova,na_nezha_facts.sid[0],a->reserved,a->reserved_count,0,0,&encoded);
        if (r) return failed(a,r);
        for (unsigned j=0;j<a->dma_count;++j) if (na_ranges_overlap((NaRange){s->iova,s->dma.physical.bytes},
            (NaRange){a->iova[j],a->dma_range[j].bytes})) return failed(a,NA_OWNERSHIP);
        a->dma_range[a->dma_count]=s->dma.physical;a->iova[a->dma_count++]=s->iova;
        stream[i].memory=s->dma.cpu;stream[i].dsp_address=encoded;stream[i].bytes=(uint32_t)s->dma.physical.bytes;
        stream[i].port=s->port;stream[i].endpoint=i==2 ? s->media.hardware:s->media.shared;
        r=na_topology_load(s->graph,s->graph_bytes,s->dma.cpu,stream[i].bytes,&s->media,&stream[i].topology);
        if (r) return failed(a,r);
    }
    /* The shared SMMU builder expects ascending IOVA, independent of the
     * logical output/mic/VI order used by the period callbacks. */
    for (unsigned i=1;i<a->dma_count;++i) for (unsigned j=i;j && a->iova[j]<a->iova[j-1];--j) {
        NaRange range=a->dma_range[j];a->dma_range[j]=a->dma_range[j-1];a->dma_range[j-1]=range;
        uint32_t address=a->iova[j];a->iova[j]=a->iova[j-1];a->iova[j-1]=address;
    }
    NaDriverOps driver={a,host_time,sync_dma,health,mic_prepare,mic_release};
    r=na_driver_init(&a->driver,&a->adsp,&a->speaker,&stream[0],p->stream[1].graph ? &stream[1]:0,
        p->resources,p->resource_count,p->output_votes,p->input_votes,p->status,p->output_ring,p->input_ring,
        p->timebase_numer,p->timebase_denom,p->capture_proven,&driver);
    if (r) return failed(a,r);
    r=na_driver_attach_feedback(&a->driver,&stream[2],&p->calibration,p->ti_instance);if (r) return failed(a,r);
    a->driver.runtime.capture_probe_allowed=!!p->capture_probe_allowed;
    __atomic_store_n(&a->initialized,1,__ATOMIC_RELEASE);return NA_OK;
}
