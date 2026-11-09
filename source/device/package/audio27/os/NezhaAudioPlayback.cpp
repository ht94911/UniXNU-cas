// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NezhaAudioPlayback.h"
#include "NezhaAudioService.h"
#include <Sm8850ResourcesNative.h>
#include <string.h>
extern "C" volatile int nezha_touch_stage;
namespace {
int failed(NaNativePlayback *s,int r) {s->error=r;return r;}
NaOwnedBuffer owned(const NaNativeMemory &m) {return {m.va,{m.physical,m.bytes}};}
int smem_lock(void *v) {return na_resource_owner_smem_lock(&static_cast<NaNativePlayback *>(v)->inputs.resources);}
void smem_unlock(void *v) {na_resource_owner_smem_unlock(&static_cast<NaNativePlayback *>(v)->inputs.resources);}
void doorbell(void *v,uint32_t signal) {na_resource_owner_doorbell(&static_cast<NaNativePlayback *>(v)->inputs.resources,signal);}
int dispatch(void *v) {
    auto *s=static_cast<NaNativePlayback *>(v);int r=na_native_smmu_faulted(&s->domain);
    if (!r && s->assembly.adsp.stage==NaAdspDma)
        r=na_native_adsp_watchdog_prepare(&s->watchdog,&s->inputs.resources,&s->assembly.adsp,s->plan.adsp_lease);
    return r ? r:na_resource_owner_dispatch(&s->inputs.resources);
}
int sync(void *v,const NaOwnedBuffer *buffer,uint32_t offset,uint32_t bytes,int device) {
    auto *s=static_cast<NaNativePlayback *>(v);
    for (auto &m:s->dma) if (buffer->cpu==m.va && buffer->physical.base==m.physical && buffer->physical.bytes==m.bytes)
        return na_native_sync(&m,offset,bytes,device);
    return NA_OWNERSHIP;
}
}
extern "C" int na_native_speaker_config(NaNativePlaybackConfig *c,const uint8_t revisions[2],uint32_t numer,uint32_t denom) {
    if (!c || !revisions || !numer || !denom) return NA_ARGUMENT;
    NaNativePlaybackConfig plan={};
    plan.speaker.revision[0]=revisions[0];plan.speaker.revision[1]=revisions[1];
    plan.speaker.revision_mask[0]=plan.speaker.revision_mask[1]=0xff;
    // Stock mixer speaker path + actual saved Android hw_params/format logs.
    // TI intentionally labels 32-bit slots as24; this exact IV12/VBAT1 entry
    // was selected for BOTH amps, so do not use its near-match fallback.
    plan.speaker.profile="music_18dB_stereo";
    plan.speaker.rx_format="32BIT";plan.speaker.tx_format="24_I2S_02_12_1";
    plan.speaker.dvc_index[0]=plan.speaker.dvc_index[1]=0; // below stock55; not a raw register value
    // Software-owned GPR ports and IOVAs; these are not borrowed physical RAM.
    plan.port[0]=0x7000;plan.port[1]=0x7002;
    plan.iova[0]=0x40000000;plan.iova[1]=0x40010000;
    // Stock primary TDM,48k*2slots*32bits; DT attribute4. Downstream
    // msm_common_snd_hw_params uses no extra core vote for this48k PRI route.
    plan.resources[0]={NaPrmClock,0x200,48000*2*32,4};
    plan.resource_count=1;plan.output_votes=1;
    plan.timebase_numer=numer;plan.timebase_denom=denom;*c=plan;return NA_OK;
}
extern "C" int na_native_playback_prepare(NaNativePlayback *s,const NaNativePlaybackConfig *c) {
    if (!s || !c || s->attempted || s->ready || s->error ||
        !c->port[0] || !c->port[1] || c->port[0]==c->port[1] ||
        !c->iova[0] || !c->iova[1] || ((c->iova[0]|c->iova[1])&4095) ||
        (uint64_t)c->iova[0]+65536>UINT64_C(0x100000000) ||
        (uint64_t)c->iova[1]+65536>UINT64_C(0x100000000) ||
        na_ranges_overlap({c->iova[0],65536},{c->iova[1],65536}) ||
        !c->resource_count || c->resource_count>NA_PRM_RESOURCES || !c->output_votes ||
        (c->output_votes>>c->resource_count) || !c->timebase_numer || !c->timebase_denom ||
        c->speaker.revision_mask[0]!=0xff || c->speaker.revision_mask[1]!=0xff) return NA_ARGUMENT;
    for (unsigned i=0;i<c->resource_count;++i) {
        const auto &r=c->resources[i];
        if (r.kind>NaPrmClock || !r.id || (r.kind==NaPrmClock && !r.hz)) return NA_ARGUMENT;
        for (unsigned j=0;j<i;++j) if (r.kind==c->resources[j].kind && r.id==c->resources[j].id) return NA_ARGUMENT;
    }
    // Validate immutable private graph/tuning/calibration before claiming any
    // shared hardware resource. This never treats a zero-filled CI slot as input.
    NaInputPack pack;int r=na_native_audio_inputs(&pack);if (r) return r;
    r=na_playback_inputs(&s->playback,&pack,&c->speaker);if (r) return r;
    r=na_native_prepare_adsp_inputs(&s->inputs,&s->plan);
    if (r==NA_BUSY && !s->inputs.attempted) {
        // Dependency wait has acquired no resources. The retained input view
        // is safe to discard, allowing the same caller to wait for touch/USB.
        s->playback={};return r;
    }
    s->attempted=1;if (r) return failed(s,r);
    auto &p=s->plan;uint64_t lease=p.adsp_lease;
    r=na_playback_plan(&p,&s->playback);if (r) return failed(s,r);
    r=na_native_map(&s->smem,{0x81d00000,0x200000},lease);if (r) return failed(s,r);
    r=na_native_map(&s->qmp,{0x0c300000,0x1000},lease);if (r) return failed(s,r);
    r=na_native_map(&s->cmd_db,{0x81c60000,0x20000},lease);if (r) return failed(s,r);
    p.smem=s->smem.va;p.qmp=reinterpret_cast<volatile uint32_t *>(s->qmp.va);
    p.cmd_db=const_cast<const uint8_t *>(s->cmd_db.va);p.tcs=1;
    p.proxy=na_resource_owner_proxy(&s->inputs.resources);
    for (unsigned i=0;i<2;++i) {
        r=na_native_allocate(&s->dma[i],65536);if (r) return failed(s,r);
        auto &stream=p.stream[i ? 2:0];stream.dma=owned(s->dma[i]);stream.iova=c->iova[i];stream.port=c->port[i];
    }
    r=na_native_allocate(&s->tables,64*4096);if (r) return failed(s,r);p.tables=owned(s->tables);
    r=na_native_smmu_bind(&s->domain,&s->inputs.resources,&p.smmu);if (r) return failed(s,r);
    r=na_native_tas_board(&s->board,s->inputs.resources.common,&p.board);if (r) return failed(s,r);
    r=nezha_audio_allocate_maps(&p.status,&p.output_ring,&p.input_ring,0);if (r) return failed(s,r);
    memcpy(p.resources,c->resources,c->resource_count*sizeof *c->resources);
    p.resource_count=c->resource_count;p.output_votes=c->output_votes;
    p.timebase_numer=c->timebase_numer;p.timebase_denom=c->timebase_denom;
    p.ops={s,smem_lock,smem_unlock,doorbell,sync,na_native_host_time,dispatch};
    r=na_assembly_init(&s->assembly,&p,na_native_now_us(nullptr)/1000);if (r) return failed(s,r);
    // Carry the actual037 channel-opening path and diagnostic timing into
    // this first playback candidate; do not silently change DSP bring-up.
    s->assembly.adsp.gpr_initiator=1;
    s->assembly.adsp.glink_delay_ms=20000;
    s->assembly.adsp.glink_observe_ms=100;
    r=na_resource_owner_ipcc(&s->inputs.resources,&s->assembly.adsp);if (r) return failed(s,r);
    s->ready=1;return NA_OK;
}
extern "C" int na_native_playback_start_step(NaNativePlaybackStart *s,uint64_t now,uint32_t numer,uint32_t denom) {
    if (!s || !numer || !denom || now>UINT64_MAX-120000) return NA_ARGUMENT;
    if (s->error) return s->error;
    if (s->phase==4) return NA_OK;
    if (!s->deadline) s->deadline=now+120000;
    int r=now>=s->deadline ? NA_TIMEOUT:NA_OK;
    if (!r && !s->phase) {
        if (__atomic_load_n(&nezha_touch_stage,__ATOMIC_ACQUIRE)!=7 || !sm8850_usb_domain_ready()) return NA_PENDING;
        NaInputPack pack;
        r=na_native_audio_inputs(&pack);
        if (!r) {
            Sm8850Resources *common=sm8850_native_resources();NaBoardIo board;NaTasPowerOps ops;
            r=common ? na_native_tas_board(&s->identity_native,common,&board):NA_IO;
            if (!r) r=na_geni_board_init_identity(&s->identity_board,&board,&ops);
            if (!r) r=na_tas_identity_init(&s->identity,&ops);
            if (!r) s->phase=1;
        }
    } else if (!r && s->phase==1) {
        r=na_tas_identity_poll(&s->identity,now);
        if (r==NA_PENDING) return r;
        if (!r) {
            /* Identity completion proved the bus quiet and released its
             * GPIO/clock lease; drop those CPU aliases before playback maps. */
            r=na_native_unmap(&s->identity_native.geni,1);
            if (!r) r=na_native_unmap(&s->identity_native.tlmm,1);
            if (!r) r=na_native_speaker_config(&s->config,s->identity.revision,numer,denom);
            if (!r) s->phase=2;
        }
    } else if (!r && s->phase==2) {
        r=na_native_playback_prepare(&s->playback,&s->config);
        if (r==NA_BUSY && !s->playback.attempted && !s->playback.inputs.attempted) return NA_PENDING;
        if (!r) s->phase=3;
    } else if (!r && s->phase==3) {
        r=nezha_audio_worker_start(&s->playback.assembly.driver);
        if (!r) {s->phase=4;return NA_OK;}
    } else if (!r) r=NA_OWNERSHIP;
    if (r) {
        s->error=r<0 ? r:NA_IO;s->phase=5;
        (void)na_tas_identity_stop(&s->identity);
        /* Keep partial Assembly/PAS/DMA ownership for diagnosis. Never
         * restart a failed initialization or request automatic playback. */
        return s->error;
    }
    return NA_PENDING;
}
