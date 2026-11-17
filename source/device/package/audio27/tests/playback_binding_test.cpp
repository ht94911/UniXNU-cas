// SPDX-License-Identifier: BSD-2-Clause-Patent
// Production native adapter linked against explicitly fake platform providers.
// Tests routing, dependency waits and every partial-setup failure. The Assembly
// call is inspected, not emulated: this is NOT a full XNU/DSP execution test.
#include "../os/NezhaAudioPlayback.h"
#include "../os/NezhaAudioService.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace {
NaInputPack pack;
unsigned calls,failAt,busy,syncs;
NaNativePlayback *current;
NaNativePlaybackConfig config;
NaNativePlaybackStart *starting;
Sm8850Resources common;
unsigned usbReady=1,identityReads,identityPowered,identityReset,identityReleased,unmaps,workerCalls;
int inputError,identityError,unmapFailure,workerError;
unsigned watchdogCalls;int watchdogError;
NaAudioStatus status;
alignas(4096) uint8_t fakeMemory[4][65536];
uint64_t ring[NA_AUDIO_RING_FRAMES],inputRing[NA_AUDIO_RING_FRAMES];
int step() {return ++calls==failAt ? NA_IO:NA_OK;}
int identity_claim(void *,uint64_t *lease) {*lease=55;return NA_OK;}
int identity_power(void *,uint64_t lease,int on) {assert(lease==55);identityPowered=!!on;return NA_OK;}
int identity_reset(void *,uint64_t lease,int on) {assert(lease==55);identityReset=!!on;return NA_OK;}
int identity_release(void *,uint64_t lease) {
    assert(lease==55 && !identityPowered && identityReset);++identityReleased;return NA_OK;
}
int identity_read(void *,uint64_t lease,uint8_t address,uint32_t reg,uint8_t *value) {
    assert(lease==55 && reg==0x78 && identityPowered && !identityReset);++identityReads;
    if (identityError) return identityError;
    unsigned ch=address==na_nezha_facts.amp_address[0] ? 0:1;
    assert(address==na_nezha_facts.amp_address[ch]);*value=(uint8_t)(0x51+ch);return NA_OK;
}
}
extern "C" {volatile int nezha_touch_stage;}
extern "C" unsigned sm8850_usb_domain_ready(void) {return usbReady;}
extern "C" Sm8850Resources *sm8850_native_resources(void) {return &common;}
extern "C" int na_native_audio_inputs(NaInputPack *p) {if (inputError) return inputError;*p=pack;return NA_OK;}
extern "C" int na_native_prepare_adsp_inputs(NaNativeAdspInputs *s,NaAssemblyPlan *p) {
    if (busy) {--busy;return NA_BUSY;}
    s->attempted=1;p->adsp_lease=42;return step();
}
extern "C" int na_native_map(NaNativeWindow *w,NaRange r,uint64_t lease) {
    assert(lease==42);int e=step();if (e) return e;
    w->va=fakeMemory[0];w->physical=r;w->lease=lease;return NA_OK;
}
extern "C" int na_native_allocate(NaNativeMemory *m,uint32_t bytes) {
    int e=step();if (e) return e;
    unsigned i=m==&current->dma[0] ? 0:m==&current->dma[1] ? 1:m==&current->dma[2] ? 2:3;
    m->va=fakeMemory[i];m->physical=0x60000000+0x100000*i;m->bytes=bytes;m->descriptor=m;return NA_OK;
}
extern "C" int na_native_smmu_bind(NaNativeSmmu *,NaResourceOwner *,NaSmmuOwner *owner) {owner->lease=43;return step();}
extern "C" int na_native_tas_board(NaNativeTasBoard *b,Sm8850Resources *owner,NaBoardIo *out) {
    if (starting && b==&starting->identity_native) {assert(owner==&common);return NA_OK;}
    out->context=b;
    return step();
}
extern "C" int na_native_mic_board(NaNativeMicBoard *b,Sm8850Resources *,NaBoardIo *out) {
    assert(config.capture_probe && b==&current->microphone);out->context=b;return step();
}
extern "C" int na_geni_board_init_identity(NaGeniBoard *,const NaBoardIo *,NaTasPowerOps *ops) {
    assert(starting);*ops={nullptr,identity_claim,identity_power,identity_reset,identity_release,{nullptr,identity_read,nullptr},nullptr};
    return NA_OK;
}
extern "C" int na_native_unmap(NaNativeWindow *window,int quiet) {
    assert(starting && quiet && identityReleased==1 && !identityPowered && identityReset);
    assert(window==&starting->identity_native.geni || window==&starting->identity_native.tlmm);
    ++unmaps;if ((int)unmaps==unmapFailure) return NA_IO;*window={};return NA_OK;
}
extern "C" int nezha_audio_worker_start(NaDriver *driver) {
    assert(starting && driver==&current->assembly.driver && current->ready && identityReleased==1 && unmaps==2);
    assert(driver->adsp && driver->runtime.shared && driver->feedback_attached);++workerCalls;return workerError;
}
extern "C" int nezha_audio_allocate_maps(NaAudioStatus **s,uint64_t **out,uint64_t **in,int capture) {
    assert((unsigned)capture==config.capture_probe);int e=step();if (e) return e;
    *s=&status;*out=ring;*in=capture ? inputRing:nullptr;return NA_OK;
}
extern "C" NaProxyOps na_resource_owner_proxy(NaResourceOwner *) {return {};}
extern "C" uint64_t na_native_now_us(void *) {return 123000;}
extern "C" uint64_t na_native_host_time(void *) {return 2361600;}
extern "C" int na_resource_owner_smem_lock(void *v) {assert(v==&current->inputs.resources);return NA_OK;}
extern "C" void na_resource_owner_smem_unlock(void *v) {assert(v==&current->inputs.resources);}
extern "C" void na_resource_owner_doorbell(void *v,uint32_t signal) {assert(v==&current->inputs.resources && signal==2);}
extern "C" int na_resource_owner_dispatch(NaResourceOwner *r) {assert(r==&current->inputs.resources);return NA_OK;}
extern "C" int na_native_smmu_faulted(const NaNativeSmmu *d) {return d->error;}
extern "C" int na_native_adsp_watchdog_prepare(NaNativeAdspWatchdog *w,NaResourceOwner *owner,NaAdsp *adsp,uint64_t lease) {
    assert(w==&current->watchdog && owner==&current->inputs.resources && adsp==&current->assembly.adsp);
    assert(lease==42 && adsp->masters_stopped && adsp->stage==NaAdspDma);++watchdogCalls;return watchdogError;
}
extern "C" int na_native_sync(NaNativeMemory *m,uint32_t offset,uint32_t bytes,int device) {
    assert(!offset);
    if (m==&current->dma[2]) assert(config.capture_probe && bytes==960 && !device);
    else assert((m==&current->dma[0] || m==&current->dma[1]) && bytes==1920 && device==1);
    ++syncs;return NA_OK;
}
extern "C" int na_assembly_init(NaAssembly *assembly,const NaAssemblyPlan *p,uint64_t now) {
    assert(assembly==&current->assembly && now==123);
    assert(p->tuning==&current->playback.tuning && p->calibration.valid && p->ti_instance);
    assert(p->tcs==1 && p->adsp_lease==42 && p->smmu.lease==43);
    assert(p->status==&status && p->output_ring==ring && p->input_ring==(config.capture_probe ? inputRing:nullptr));
    assert(p->capture_proven==config.input_ready && p->capture_probe_allowed==(config.capture_probe && !config.input_ready));
    assert(p->resource_count==config.resource_count && p->output_votes==config.output_votes && p->input_votes==config.input_votes);
    assert(p->board.context==&current->board);
    if (config.capture_probe) assert(p->capture_board.context==&current->microphone);
    else assert(!p->stream[1].graph && !p->capture_board.context);
    assert(!memcmp(p->resources,config.resources,config.resource_count*sizeof *p->resources));
    for (unsigned i=0;i<(config.capture_probe ? 3u:2u);++i) {
        const auto &stream=p->stream[i==2 ? 1:i ? 2:0];
        assert(stream.dma.cpu==current->dma[i].va && stream.dma.physical.base==current->dma[i].physical);
        assert(stream.iova==config.iova[i] && stream.port==config.port[i]);
        assert(stream.graph==(i==2 ? current->capture.graph.data:current->playback.graph[i].data));
        assert(!p->ops.sync(p->ops.context,&stream.dma,0,i==2 ? 960:1920,i==2 ? 0:1));
    }
    NaOwnedBuffer foreign=p->stream[0].dma;foreign.physical.base+=4096;
    assert(p->ops.sync(p->ops.context,&foreign,0,1920,1)==NA_OWNERSHIP);
    assert(!p->ops.smem_lock(p->ops.context));p->ops.smem_unlock(p->ops.context);p->ops.doorbell(p->ops.context,2);
    assert(!p->ops.poll_shared(p->ops.context));current->domain.error=NA_IO;
    assert(p->ops.poll_shared(p->ops.context)==NA_IO);current->domain.error=0;
    unsigned previous=watchdogCalls;assembly->adsp.stage=NaAdspDma;assembly->adsp.masters_stopped=1;
    assert(!p->ops.poll_shared(p->ops.context) && watchdogCalls==previous+1);
    watchdogError=NA_IO;
    assert(p->ops.poll_shared(p->ops.context)==NA_IO && watchdogCalls==previous+2);
    watchdogError=0;assembly->adsp.stage=NaAdspCold;
    int r=step();
    if (!r) {assembly->driver.adsp=&assembly->adsp;assembly->driver.runtime.shared=p->status;assembly->driver.feedback_attached=1;}
    return r;
}
extern "C" int na_resource_owner_ipcc(NaResourceOwner *owner,NaAdsp *adsp) {
    assert(owner==&current->inputs.resources && adsp==&current->assembly.adsp);
    assert(adsp->gpr_initiator==1 && adsp->glink_delay_ms==20000 && adsp->glink_observe_ms==100);return step();
}
int main(int argc,char **argv) {
    assert(argc==5);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);assert(n>0 && n<=0x8000000);rewind(f);
    void *blob=malloc((size_t)n);assert(blob && fread(blob,1,(size_t)n,f)==(size_t)n);fclose(f);
    assert(!na_input_pack(&pack,blob,(uint64_t)n));
    // Test-only revisions; real board recipe, never a candidate identity.
    const uint8_t revisions[]={0x51,0x52};
    assert(na_native_speaker_config(&config,nullptr,625,12)==NA_ARGUMENT);
    assert(!na_native_speaker_config(&config,revisions,625,12));
    assert(!strcmp(config.speaker.profile,argv[2]) && !strcmp(config.speaker.rx_format,argv[3]) && !strcmp(config.speaker.tx_format,argv[4]));
    assert(config.resource_count==1 && config.output_votes==1 && config.resources[0].kind==NaPrmClock &&
        config.resources[0].id==0x200 && config.resources[0].hz==3072000 && config.resources[0].attribute==4);
    // Bind the selected logical index to the actual stock coefficient bytes.
    // An index must never become a raw register value or exceed the stock gain.
    NaFirmwarePiece tuning;NaTasImage image;
    assert(!na_input_find(&pack,"tismartpa_driver_tuning.bin",&tuning));
    assert(!na_tas_image(&image,tuning.data,(uint32_t)tuning.bytes));
    for (unsigned channel=0;channel<2;++channel) {
        NaTasControl gain;uint8_t encoded[17];NaTasBytes program;
        assert(!na_tas_control(&image,channel ? "DVC_GAIN_BOTTOM":"DVC_GAIN_TOP",&gain));
        assert(config.speaker.dvc_index[channel]==gain.default_index && gain.default_index==55);
        assert(!na_tas_integer(&gain,config.speaker.dvc_index[channel],encoded,&program));
        const uint8_t expected[]={0x40,0x00,0x00,0x00};
        assert(program.bytes==17 && !memcmp(encoded+13,expected,sizeof expected));
    }
    for (unsigned failure=0;failure<=12;++failure) {
        current=new NaNativePlayback{};calls=syncs=0;failAt=failure;busy=2;
        assert(na_native_playback_prepare(current,&config)==NA_BUSY && !current->attempted && !current->playback.valid && !calls);
        assert(na_native_playback_prepare(current,&config)==NA_BUSY && !current->attempted && !calls);
        int r=na_native_playback_prepare(current,&config);
        assert(calls==(failure ? failure:12) && current->attempted);
        assert(failure ? r==NA_IO && !current->ready && current->error==NA_IO : !r && current->ready);
        unsigned before=calls;
        assert(na_native_playback_prepare(current,&config)==NA_ARGUMENT && calls==before);
        delete current;
    }
    current=new NaNativePlayback{};calls=0;config.iova[1]=config.iova[0];
    assert(na_native_playback_prepare(current,&config)==NA_ARGUMENT && !calls);
    config.iova[1]=0xfffff000;assert(na_native_playback_prepare(current,&config)==NA_ARGUMENT && !calls);
    delete current;
    assert(!na_native_speaker_config(&config,revisions,625,12));
    assert(!na_native_capture_config(&config));
    assert(config.capture_probe && config.resource_count==5 && config.output_votes==1 && config.input_votes==0x1e);
    assert(config.resources[1].kind==NaPrmCore && config.resources[1].id==1);
    assert(config.resources[2].kind==NaPrmCore && config.resources[2].id==2);
    assert(config.resources[3].id==0x30c && config.resources[4].id==0x307);
    for (unsigned i=3;i<5;++i) assert(config.resources[i].kind==NaPrmClock && config.resources[i].hz==19200000 && config.resources[i].attribute==1);
    assert(na_native_capture_config(&config)==NA_ARGUMENT);
    for (unsigned failure=0;failure<=14;++failure) {
        current=new NaNativePlayback{};calls=syncs=0;failAt=failure;busy=2;
        assert(na_native_playback_prepare(current,&config)==NA_BUSY && !current->attempted && !current->playback.valid && !current->capture.valid && !calls);
        assert(na_native_playback_prepare(current,&config)==NA_BUSY && !current->attempted && !calls);
        int r=na_native_playback_prepare(current,&config);
        assert(calls==(failure ? failure:14) && current->attempted);
        assert(failure ? r==NA_IO && !current->ready && current->error==NA_IO : !r && current->ready);
        unsigned before=calls;assert(na_native_playback_prepare(current,&config)==NA_ARGUMENT && calls==before);
        delete current;
    }
    current=new NaNativePlayback{};calls=0;uint32_t saved=config.iova[2];
    config.iova[2]=config.iova[1];assert(na_native_playback_prepare(current,&config)==NA_ARGUMENT && !calls);
    config.iova[2]=saved;saved=config.port[2];config.port[2]=config.port[0];
    assert(na_native_playback_prepare(current,&config)==NA_ARGUMENT && !calls);
    config.port[2]=saved;config.input_votes=0;assert(na_native_playback_prepare(current,&config)==NA_ARGUMENT && !calls);
    delete current;
    assert(!na_native_speaker_config(&config,revisions,625,12));common.initialized=1;
    for (unsigned scenario=0;scenario<6;++scenario) {
        starting=new NaNativePlaybackStart{};current=&starting->playback;
        calls=syncs=identityReads=identityPowered=identityReset=identityReleased=unmaps=workerCalls=0;
        failAt=scenario==3 ? 7:0;busy=2;usbReady=0;nezha_touch_stage=6;
        inputError=scenario==1 ? NA_WIRE:0;identityError=scenario==2 ? NA_IO:0;
        workerError=scenario==4 ? NA_IO:0;unmapFailure=scenario==5 ? 1:0;
        assert(na_native_playback_start_step(starting,100,625,12)==NA_PENDING && !calls && !identityReads);
        nezha_touch_stage=7;
        assert(na_native_playback_start_step(starting,110,625,12)==NA_PENDING && !calls && !identityReads);
        usbReady=1;int r=NA_PENDING;
        for (unsigned i=0;i<40 && r==NA_PENDING;++i) r=na_native_playback_start_step(starting,120+10*i,625,12);
        if (!scenario) {
            assert(!r && starting->phase==4 && calls==12 && workerCalls==1 && identityReads==2 && identityReleased==1);
            assert(!memcmp(starting->config.speaker.revision,revisions,2));
        } else {
            assert(r==(scenario==1 ? NA_WIRE:NA_IO) && starting->phase==5 && starting->error==r);
            if (scenario<3 || scenario==5) assert(!calls && !workerCalls);
            if (scenario==3) assert(calls==7 && current->attempted && !workerCalls);
            if (scenario==4) assert(calls==12 && workerCalls==1);
        }
        unsigned before=calls,reads=identityReads,workers=workerCalls,maps=unmaps;
        assert(na_native_playback_start_step(starting,2000,625,12)==r);
        assert(calls==before && identityReads==reads && workerCalls==workers && unmaps==maps);
        delete starting;starting=nullptr;
    }
    // Explicit bounded capture request flows through the same cold startup.
    assert(!na_native_speaker_config(&config,revisions,625,12) && !na_native_capture_config(&config));
    starting=new NaNativePlaybackStart{};current=&starting->playback;starting->capture_frames=48000;
    calls=syncs=identityReads=identityPowered=identityReset=identityReleased=unmaps=workerCalls=0;
    failAt=busy=0;usbReady=1;nezha_touch_stage=7;inputError=identityError=workerError=unmapFailure=0;
    int captureResult=NA_PENDING;
    for (unsigned i=0;i<40 && captureResult==NA_PENDING;++i)
        captureResult=na_native_playback_start_step(starting,120+10*i,625,12);
    assert(!captureResult && starting->phase==4 && calls==14 && workerCalls==1 && current->plan.capture_probe_allowed);
    assert(!current->plan.capture_proven && current->plan.stream[1].graph && starting->config.capture_probe);
    assert(!current->assembly.driver.runtime.output_held);
    delete starting;starting=nullptr;
    // System input is explicit and opens no automatic finite capture.
    assert(!na_native_speaker_config(&config,revisions,625,12) && !na_native_capture_config(&config));
    config.input_ready=1;
    starting=new NaNativePlaybackStart{};current=&starting->playback;starting->input_ready=1;
    calls=syncs=identityReads=identityPowered=identityReset=identityReleased=unmaps=workerCalls=0;
    failAt=busy=0;usbReady=1;nezha_touch_stage=7;inputError=identityError=workerError=unmapFailure=0;
    captureResult=NA_PENDING;
    for (unsigned i=0;i<40 && captureResult==NA_PENDING;++i)
        captureResult=na_native_playback_start_step(starting,120+10*i,625,12);
    assert(!captureResult && starting->phase==4 && workerCalls==1 && current->plan.capture_proven);
    assert(!current->plan.capture_probe_allowed && current->plan.stream[1].graph && starting->config.input_ready);
    assert(current->assembly.driver.runtime.output_held && !current->assembly.driver.runtime.request);
    delete starting;starting=nullptr;
    starting=new NaNativePlaybackStart{};starting->input_ready=1;starting->capture_frames=480;calls=0;
    assert(na_native_playback_start_step(starting,100,625,12)==NA_ARGUMENT && !calls);delete starting;
    starting=new NaNativePlaybackStart{};starting->input_ready=2;calls=0;
    assert(na_native_playback_start_step(starting,100,625,12)==NA_ARGUMENT && !calls);delete starting;
    starting=new NaNativePlaybackStart{};starting->capture_frames=2*NA_AUDIO_RATE+1;calls=0;
    assert(na_native_playback_start_step(starting,100,625,12)==NA_ARGUMENT && !calls);
    delete starting;starting=nullptr;
    assert(!na_native_speaker_config(&config,revisions,625,12));
    starting=new NaNativePlaybackStart{};current=&starting->playback;usbReady=0;calls=0;
    assert(na_native_playback_start_step(starting,100,0,12)==NA_ARGUMENT && !starting->phase);
    assert(na_native_playback_start_step(starting,100,625,12)==NA_PENDING);
    assert(na_native_playback_start_step(starting,120100,625,12)==NA_TIMEOUT && !calls);
    delete starting;starting=nullptr;free(blob);
    puts("native playback adapter/startup PASS: dependency waits, real reset-muted identity sequence, 12 setup failures, input/TAS/unmap/worker/timeout failures and no replay; platform and Assembly providers are fake");
}
