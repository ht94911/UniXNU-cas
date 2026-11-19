/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Production runtime/engine/graph/TAS state machines. The GPR peer and bus
 * supply synthetic acknowledgements/registers; no device or real audio I/O. */
#include "../core/AudioDriver.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct {
    NaDriver driver;
    NaAdsp adsp;
    NaTasEndpoint speaker;
    NaRxQueue rx;
    NaTransport transport;
    NaAudioStatus status,fresh;
    uint64_t now;
    uint32_t commands,closes,unmaps,releases,muted,activated,writes;
    uint32_t fail_done,busy_write;
    uint32_t nonzero_periods,zero_periods;
    uint32_t pcm_peak;
    int health_error;
    unsigned suppress_reads,reads,close_failure,input_opens,input_maps,input_stops,input_closes,input_unmaps;
    unsigned input_unmap_failure,input_release_failure;
} Fixture;
static Fixture f;
static _Alignas(4096) uint8_t dma[3][65536];
static _Alignas(8) uint64_t ring[NA_AUDIO_RING_FRAMES],fresh_ring[NA_AUDIO_RING_FRAMES];
static void put(uint8_t *p,uint32_t v) {p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);}
static int peer_send(void *v,const uint8_t *packet,uint32_t bytes) {
    Fixture *x=v;GprPacket p;assert(!gpr_decode(packet,bytes,&p));
    uint8_t payload[32]={0},reply[GPR_MAX_PACKET];uint32_t n=8,op=GPR_OP_BASIC_RESULT;
    if (p.opcode==AR_OP_WRITE) {
        if (x->busy_write) return NA_BUSY;
        assert(p.payload_bytes>=16);
        uint64_t address=(uint64_t)p.payload[0]|(uint64_t)p.payload[1]<<8|(uint64_t)p.payload[2]<<16|
            (uint64_t)p.payload[3]<<24|(uint64_t)p.payload[4]<<32|(uint64_t)p.payload[5]<<40|
            (uint64_t)p.payload[6]<<48|(uint64_t)p.payload[7]<<56;
        uint32_t slot=(uint32_t)((address-x->driver.stream[0].dsp_base)/4096);
        assert(slot<NA_DMA_SLOTS && p.src_port==x->driver.stream[0].port);
        int nonzero=0;const int16_t *samples=(const int16_t *)(dma[0]+slot*4096);
        for (unsigned i=0;i<2*NA_AUDIO_PERIOD_FRAMES;++i) {
            nonzero|=samples[i]!=0;
            uint32_t amplitude=(uint32_t)(samples[i]<0 ? -(int32_t)samples[i]:samples[i]);
            if (amplitude>x->pcm_peak) x->pcm_peak=amplitude;
        }
        if (x->driver.runtime.engine.output_idle) assert(!nonzero);
        if (nonzero) ++x->nonzero_periods;else ++x->zero_periods;
        if (x->fail_done) {put(payload,AR_OP_WRITE);put(payload+4,x->fail_done);x->fail_done=0;}
        else {op=AR_OP_WRITE_DONE;n=32;memcpy(payload,p.payload,12);}
    } else if (p.opcode==AR_OP_READ) {
        ++x->reads;
        assert(x->driver.graph[0].stage==NaGraphRunning && x->driver.graph[2].stage==NaGraphRunning);
        assert(!x->driver.runtime.engine.output_idle ||
            (x->driver.runtime.output_idle_ready && !x->driver.runtime.engine.active[0]));
        if (x->suppress_reads) return NA_OK;
        uint32_t slot=(uint32_t)(x->driver.stream[1].next-1)%NA_DMA_SLOTS;
        int16_t *samples=(int16_t *)(dma[2]+slot*4096);
        for (unsigned i=0;i<NA_AUDIO_PERIOD_FRAMES;++i) samples[i]=(int16_t)(i%2 ? -1234:1234);
        op=AR_OP_READ_DONE;n=56;uint8_t captured[56]={0};
        memcpy(captured+4,p.payload,12);put(captured+16,960);
        GprPacket q={.src_domain=GPR_DOMAIN_ADSP,.dst_domain=GPR_DOMAIN_APPS,
            .src_port=p.dst_port,.dst_port=p.src_port,.token=p.token,.opcode=op};
        uint32_t size=gpr_encode(reply,sizeof reply,&q,captured,n);assert(size);
        assert(!na_rx_push(&x->rx,x->driver.session.generation,reply,size));return NA_OK;
    } else if (p.opcode==AR_OP_MEM_MAP) {
        if (p.src_port==x->driver.stream[1].port) ++x->input_maps;
        op=AR_OP_MEM_MAP_DONE;n=4;put(payload,0x7700+p.src_port);
    }
    else if (p.opcode==GPR_OP_APM_GET_SPF_STATE) {op=GPR_OP_APM_SPF_STATE;n=4;put(payload,1);}
    else if (p.opcode==AR_OP_PRM_REQUEST || p.opcode==AR_OP_PRM_RELEASE) {
        op=p.opcode==AR_OP_PRM_REQUEST ? AR_OP_PRM_REQUEST_DONE:AR_OP_PRM_RELEASE_DONE;n=4;
        if (p.opcode==AR_OP_PRM_RELEASE) ++x->releases;
    } else {
        put(payload,p.opcode);
        if (p.src_port==x->driver.stream[1].port) {
            if (p.opcode==AR_OP_GRAPH_OPEN) {
                ++x->input_opens;
                if (x->input_opens==2) {
                    NaRuntime *r=&x->driver.runtime;
                    assert(r->engine.output_idle && r->output_idle_ready && !r->engine.active[0]);
                    assert(r->input_first_error==NA_TIMEOUT && !x->driver.graph[1].cleanup_error);
                }
            }
            if (p.opcode==AR_OP_GRAPH_STOP) ++x->input_stops;
            if (p.opcode==AR_OP_GRAPH_CLOSE) ++x->input_closes;
            if (p.opcode==AR_OP_MEM_UNMAP) {
                ++x->input_unmaps;
                if (x->input_unmap_failure) put(payload+4,x->input_unmap_failure);
            }
        }
        if (p.opcode==AR_OP_GRAPH_CLOSE) {
            ++x->closes;
            if (p.src_port==x->driver.stream[1].port && x->close_failure) put(payload+4,x->close_failure);
        }
        if (p.opcode==AR_OP_MEM_UNMAP) ++x->unmaps;
    }
    ++x->commands;
    GprPacket q={.src_domain=GPR_DOMAIN_ADSP,.dst_domain=GPR_DOMAIN_APPS,
        .src_port=p.dst_port,.dst_port=p.src_port,.token=p.token,.opcode=op};
    n=gpr_encode(reply,sizeof reply,&q,payload,n);assert(n);
    assert(!na_rx_push(&x->rx,x->driver.session.generation,reply,n));return NA_OK;
}
static int sync_slot(void *v,uint32_t at,uint32_t n,int for_device) {
    (void)v;(void)for_device;assert(at<65536 && n<=65536-at);return NA_OK;
}
static int tas_read(void *v,uint64_t lease,uint8_t address,uint32_t reg,uint8_t *out) {
    (void)v;assert(lease==42 && (address==0x4a || address==0x48));
    *out=reg==0x78 ? (uint8_t)(address==0x4a ? 0x51:0x52):0;return NA_OK;
}
static int tas_write(void *v,uint64_t lease,uint8_t address,uint32_t reg,const uint8_t *data,uint32_t bytes) {
    Fixture *x=v;(void)reg;assert(lease==42 && (address==0x4a || address==0x48) && data && bytes);
    ++x->writes;return NA_OK;
}
static int tas_reset(void *v,uint64_t lease,int asserted) {(void)v;(void)asserted;assert(lease==42);return NA_OK;}
static int graph_prepare(void *v) {
    Fixture *x=v;int r=na_tas_endpoint_request(&x->speaker,NaTasPrepared);
    return r ? r:na_tas_endpoint_poll(&x->speaker,x->now);
}
static int vi_prepare(void *v) {
    Fixture *x=v;return x->speaker.state>=NaTasPrepared && !x->speaker.changing ? NA_OK:NA_PENDING;
}
static int graph_mute(void *v) {Fixture *x=v;return mute_endpoint(&x->driver.direction[0]);}
static int graph_release(void *v) {
    Fixture *x=v;if (x->driver.graph[2].stage!=NaGraphOff) return NA_PENDING;
    ++x->muted;return NA_OK;
}
static int nothing(void *v) {(void)v;return NA_OK;}
static int input_release(void *v) {return ((Fixture *)v)->input_release_failure ? NA_IO:NA_OK;}
static uint64_t host(void *v) {return ((Fixture *)v)->now*19200;}
static int dma_health(void *v) {return ((Fixture *)v)->health_error;}
static int runtime_activate(void *v,int direction) {
    Fixture *x=v;if (direction) return NA_OK; /* synthetic capture, separately tested WCD */
    int r=activate(&x->driver,direction);if (!r) ++x->activated;return r;
}
static int runtime_standby(void *v) {Fixture *x=v;return standby(&x->driver);}
static int runtime_health(void *v) {Fixture *x=v;return health(&x->driver);}
static void initialize_capture(int capture) {
    memset(&f,0,sizeof f);memset(dma,0,sizeof dma);memset(ring,0,sizeof ring);memset(fresh_ring,0,sizeof fresh_ring);
    f.now=100;f.driver.stage=DReady;f.driver.speaker=&f.speaker;f.driver.adsp=&f.adsp;
    f.driver.feedback_attached=1;f.driver.protection_instance=42;f.driver.output_votes=1;
    f.driver.ops=(NaDriverOps){.context=&f,.dma_health=dma_health};
    f.speaker.ops.io=(NaTasIo){&f,tas_read,tas_write};
    f.speaker.ops.context=&f;f.speaker.ops.reset=tas_reset;
    f.speaker.lease=42;f.speaker.powered=f.speaker.identified=1;
    f.speaker.selected.revision[0]=0x51;f.speaker.selected.revision[1]=0x52;
    f.speaker.selected.revision_mask[0]=f.speaker.selected.revision_mask[1]=0xff;
    const uint8_t *prog=(const uint8_t[]){6,0,0,0,0,2,0,0,0,1};
    static uint8_t program[10];memcpy(program,prog,10);
    for (unsigned phase=0;phase<3;++phase) {
        f.speaker.count[phase]=2;
        for (unsigned ch=0;ch<2;++ch) f.speaker.steps[phase][ch]=(NaTasStep){{program,sizeof program},(uint8_t)ch,0};
    }
    static uint8_t fault_entry[92];memset(fault_entry,0,sizeof fault_entry);put(fault_entry+64,0x64);put(fault_entry+68,1);
    for (unsigned ch=0;ch<2;++ch) f.speaker.faults[ch]=(NaTasFaultTable){.count=1,.entries={fault_entry,sizeof fault_entry}};
    na_session_init(&f.driver.session);assert(!na_session_link(&f.driver.session,1));
    NaTopology topology={.open={32768,8},.config={{32776,8}},.configs=1,.subgraphs={42},.subgraph_count=1};
    for (unsigned k=0;k<2;++k) {
        unsigned i=k ? 2:0;f.driver.direction[i]=(NaDriverDirection){&f.driver,i};
        assert(!na_stream_init(&f.driver.stream[i],dma[k],0x110000000ULL+k*65536,65536,1920,4,7+i,42+i,0));
        NaGraphOps ops={&f,peer_send,sync_slot,k ? vi_prepare:graph_prepare,k ? nothing:graph_mute,k ? nothing:graph_release};
        assert(!na_graph_init(&f.driver.graph[i],&f.driver.session,&f.driver.stream[i],&topology,&ops));
    }
    if (capture) {
        assert(!na_stream_init(&f.driver.stream[1],dma[2],0x110020000ULL,65536,960,2,8,43,1));
        NaGraphOps go={&f,peer_send,sync_slot,nothing,nothing,input_release};
        assert(!na_graph_init(&f.driver.graph[1],&f.driver.session,&f.driver.stream[1],&topology,&go));
    }
    NaPrmResource resources[2]={{.kind=NaPrmClock,.id=0x200,.hz=3072000,.attribute=4},{.kind=NaPrmCore,.id=2}};
    assert(!na_prm_init(&f.driver.prm,&f.driver.session,resources,2,peer_send,&f));
    NaRuntimeOps ro={&f,host,runtime_activate,runtime_health,runtime_standby};
    assert(!na_runtime_init(&f.driver.runtime,&f.driver.session,&f.rx,&f.driver.graph[0],capture ? &f.driver.graph[1]:NULL,&f.driver.prm,&f.transport,
        &f.status,ring,capture ? fresh_ring:NULL,625,12,1,capture ? 2:0,0,&ro));
    assert(!na_engine_attach_control(&f.driver.runtime.engine,&f.driver.graph[2]));
    f.driver.runtime.service_ready=1;
}
static void initialize(void) {initialize_capture(0);}
static int step(void) {
    f.driver.now_ms=f.now;int r=na_runtime_poll(&f.driver.runtime,f.now);f.now+=10;return r;
}
static void steps(unsigned n) {for (unsigned i=0;i<n;++i) assert(!step());}
static void start(void) {
    assert(!na_runtime_request(&f.driver.runtime,NaAudioStartOutput));
    for (unsigned i=0;i<200 && !f.driver.runtime.engine.active[0];++i) assert(!step());
    assert(f.driver.runtime.engine.active[0] && f.speaker.state==NaTasActive && !f.speaker.changing);
    assert(f.driver.graph[0].stage==NaGraphRunning && f.driver.graph[2].stage==NaGraphRunning);
}
static void write_next(int16_t value) {
    NaRuntime *r=&f.driver.runtime;
    uint64_t relative=f.driver.stream[0].next*NA_AUDIO_PERIOD_FRAMES;
    uint64_t frame=r->origin[0]+relative-r->relative_origin[0];
    for (unsigned i=0;i<NA_DMA_FLIGHT*NA_AUDIO_PERIOD_FRAMES;++i) assert(!na_ring_mix(r->output,frame+i,value,-value));
}
static void standby_complete(void) {
    NaRuntime *r=&f.driver.runtime;
    for (unsigned i=0;i<200 && !r->output_idle_ready;++i) assert(!step());
    assert(r->output_idle_ready && !r->output_stop_pending && !r->engine.active[0]);
    assert(f.speaker.state==NaTasOff && !f.speaker.changing && !r->fault);
    uint32_t retained_input=f.driver.graph[1].stage==NaGraphPaused ? r->votes[1]:0;
    assert(f.driver.prm.held==(1u|retained_input) && !f.releases && !f.closes && !f.unmaps);
}
static void repeat_and_rebind(void) {
    initialize();start();write_next(16000);steps(1);assert(f.nonzero_periods);
    uint32_t active=f.activated;uint64_t next=f.driver.stream[0].next;
    write_next(24000);assert(!na_runtime_request(&f.driver.runtime,NaAudioStopOutput));
    assert(!na_runtime_rebind_ready(&f.driver.runtime));standby_complete();
    uint32_t audible=f.nonzero_periods,underruns=f.driver.runtime.status.underruns;
    steps(60);assert(f.nonzero_periods==audible && f.driver.stream[0].next>next);
    assert(f.driver.runtime.status.underruns==underruns && !f.releases);
    start();write_next(8000);steps(1);assert(f.activated==active+1 && f.nonzero_periods>audible);
    // Stop followed immediately by Start must still mute, discard queued old
    // samples and drain their DMA sequence before reactivation.
    write_next(24000);assert(!na_runtime_request(&f.driver.runtime,NaAudioStopOutput));
    assert(!na_runtime_request(&f.driver.runtime,NaAudioStartOutput));
    assert(!step() && f.driver.runtime.engine.output_idle && !f.driver.runtime.engine.active[0]);
    for (unsigned i=0;i<200 && !f.driver.runtime.engine.active[0];++i) assert(!step());
    assert(f.driver.runtime.engine.active[0] && f.activated==active+2);
    assert(!na_runtime_request(&f.driver.runtime,NaAudioStopOutput));standby_complete();
    assert(na_runtime_rebind_ready(&f.driver.runtime));
    uint32_t epoch=f.driver.runtime.status.epoch;uint64_t sequence=f.driver.stream[0].next;
    uint64_t stale_frame=f.driver.runtime.status.out_consumed+NA_AUDIO_RING_FRAMES;
    assert(!na_runtime_rebind(&f.driver.runtime,&f.fresh,fresh_ring,NULL));
    assert(!f.status.state && f.fresh.epoch==epoch+1 && f.driver.stream[0].next==sequence);
    assert(!na_ring_mix(ring,stale_frame,32000,32000)); // old writable map cannot feed the new client
    steps(1);assert(f.driver.runtime.relative_origin[0]==sequence*NA_AUDIO_PERIOD_FRAMES);
    assert(f.driver.runtime.status.out_consumed==4*NA_AUDIO_PERIOD_FRAMES);
    audible=f.nonzero_periods;steps(2);assert(f.nonzero_periods==audible);
    start();write_next(12000);steps(1);assert(f.nonzero_periods>audible);
    // Real shutdown disables requests permanently and uses original CLOSE +
    // UNMAP receipts; retained silence never hides ownership failures.
    na_runtime_disable(&f.driver.runtime);
    assert(na_runtime_request(&f.driver.runtime,NaAudioStartOutput)==NA_MISSING);
    for (unsigned i=0;i<200 && !na_runtime_quiescent(&f.driver.runtime);++i) assert(!step());
    assert(na_runtime_quiescent(&f.driver.runtime) && !f.status.state && !f.fresh.state);
    assert(f.closes==2 && f.unmaps==2 && f.releases==1 && !f.speaker.changing && f.speaker.state==NaTasOff);
    steps(3);assert(!f.driver.runtime.service_ready && !na_runtime_rebind_ready(&f.driver.runtime));
}
static void staged_stop(void) {
    initialize();start();f.busy_write=1;write_next(24000);steps(1);
    assert(f.driver.runtime.engine.staged[0]);
    assert(!na_runtime_request(&f.driver.runtime,NaAudioStopOutput));f.busy_write=0;
    uint32_t audible=f.nonzero_periods;steps(1);assert(f.nonzero_periods==audible);
    standby_complete();
}
static void failures(void) {
    initialize();start();f.health_error=NA_IO;assert(step()==NA_IO);
    assert(f.status.state==NaAudioFault && f.driver.runtime.disabled);
    assert(na_runtime_request(&f.driver.runtime,NaAudioStartOutput)==NA_MISSING);
    initialize();start();f.fail_done=4;steps(1);assert(step()==NA_REMOTE);
    assert(f.driver.stream[0].quarantine && f.status.state==NaAudioFault);
    assert(!na_runtime_rebind_ready(&f.driver.runtime));
    // Disconnect preserves DMA quarantine rather than claiming an idle graph.
    initialize();start();assert(!na_rx_link(&f.rx,f.driver.session.generation+1,0));
    assert(step()<=0 && f.driver.stream[0].quarantine && !(f.status.state&NaAudioOutputReady));
    assert(!na_runtime_rebind_ready(&f.driver.runtime));
}
static void probe_after_playback(void) {
    initialize_capture(1);start();
    assert(!na_runtime_request(&f.driver.runtime,NaAudioStopOutput));standby_complete();
    NaRuntime *r=&f.driver.runtime;r->capture_probe_allowed=1;
    assert(!na_runtime_probe_capture(r,960,f.now));
    assert(na_runtime_request(r,NaAudioStartOutput)==NA_BUSY);
    for (unsigned i=0;i<200 && r->probe.state!=NaProbeComplete;++i) assert(!step());
    assert(r->probe.state==NaProbeComplete && r->probe.frames==960 && r->probe.peak==1234);
    assert(r->probe.pcm_frames==960 && r->probe.pcm[0]==1234 && r->probe.pcm[1]==-1234);
    assert(!r->capture_proven && !r->status.in_channels);
    assert(f.driver.graph[1].stage==NaGraphOff && f.driver.graph[0].stage==NaGraphRunning);
    for (unsigned i=0;i<10 && (f.driver.prm.held!=1 || !na_runtime_rebind_ready(r));++i) assert(!step());
    assert(f.driver.graph[2].stage==NaGraphRunning && f.driver.prm.held==1 && na_runtime_rebind_ready(r));
    start();assert(r->engine.active[0]);
}
static void full_PCM_range(void) {
    initialize();start();write_next(16000);steps(1);assert(f.pcm_peak==16000);
    NaRuntime *r=&f.driver.runtime;
    uint64_t frame=r->origin[0]+f.driver.stream[0].next*NA_AUDIO_PERIOD_FRAMES-r->relative_origin[0];
    for (unsigned i=0;i<NA_DMA_FLIGHT*NA_AUDIO_PERIOD_FRAMES;++i) {
        assert(!na_ring_mix(r->output,frame+i,32000,-32000));
        assert(!na_ring_mix(r->output,frame+i,32000,-32000));
    }
    steps(1);assert(f.pcm_peak==32768); // saturation, not wrap or extra attenuation
    assert(!na_runtime_request(r,NaAudioStopOutput));standby_complete();
}
static void cold_probe(unsigned no_pcm,unsigned bad_close) {
    initialize_capture(1);f.suppress_reads=no_pcm;f.close_failure=bad_close;
    NaRuntime *r=&f.driver.runtime;r->capture_probe_allowed=1;
    assert(!na_runtime_probe_capture(r,960,f.now));
    unsigned target=no_pcm ? NaProbeFailed:NaProbeComplete;
    for (unsigned i=0;i<600 && r->probe.state!=target;++i) assert(!step());
    assert(r->probe.state==target && !r->fault && !r->disabled && r->service_ready);
    assert(f.reads && f.driver.graph[0].stage==NaGraphRunning && f.driver.graph[2].stage==NaGraphRunning);
    assert(!f.activated && !f.nonzero_periods && !r->status.in_channels);
    if (no_pcm) {
        assert(r->probe.error==NA_TIMEOUT && !r->probe.frames && !r->probe.pcm_frames);
        assert(na_runtime_probe_capture(r,960,f.now)==NA_TIMEOUT);
    } else assert(r->probe.frames==960 && r->probe.pcm_frames==960);
    if (bad_close) {
        assert(f.driver.graph[1].stage==NaGraphFault && f.driver.stream[1].quarantine);
        assert(f.driver.graph[1].cleanup_error==NA_REMOTE && !na_runtime_rebind_ready(r));
        assert(f.driver.prm.possible==3); // uncertain input resources stay owned
    } else {
        assert(f.driver.graph[1].stage==NaGraphOff && !f.driver.stream[1].quarantine);
        for (unsigned i=0;i<20 && f.driver.prm.held!=1;++i) assert(!step());
        assert(f.driver.prm.held==1);
    }
    unsigned audible=f.nonzero_periods;start();write_next(8000);steps(2);
    assert(f.nonzero_periods>audible && r->engine.active[0]);
    na_runtime_disable(r);
    if (!bad_close) {
        for (unsigned i=0;i<200 && !na_runtime_quiescent(r);++i) assert(!step());
        assert(na_runtime_quiescent(r));
    }
}
static void outer_probe_fault(void) {
    initialize_capture(1);NaRuntime *r=&f.driver.runtime;r->capture_probe_allowed=1;
    assert(!na_runtime_probe_capture(r,960,f.now));
    assert(fail(&f.driver,NA_REMOTE)==NA_REMOTE);
    assert(r->probe.state==NaProbeFailed && r->probe.error==NA_REMOTE && r->disabled);
    assert(!na_runtime_rebind_ready(r));
}
static void system_input_only(void) {
    initialize_capture(1);
    NaRuntime *r=&f.driver.runtime;r->capture_proven=1;r->status.in_channels=1;
    assert(!na_runtime_request(r,NaAudioStartInput));
    for (unsigned i=0;i<350;++i) {
        if (!(i%10)) assert(!na_runtime_request(r,NaAudioStartInput));
        assert(!step());
    }
    assert(r->output_held && r->engine.output_idle && !r->engine.active[0] && !f.activated);
    assert(f.driver.graph[0].stage==NaGraphRunning && f.driver.graph[2].stage==NaGraphRunning);
    assert(f.driver.graph[1].stage==NaGraphRunning && r->engine.active[1] && r->status.in_produced);
    assert((r->status.state&NaAudioInputReady) && (r->status.state&NaAudioInputRunning));
    int16_t value=0;assert(!na_ring_read(fresh_ring,r->status.in_produced-1,&value) && value==-4936);
    assert(!na_runtime_request(r,NaAudioStopInput));
    for (unsigned i=0;i<20 && f.driver.graph[1].stage!=NaGraphPaused;++i) assert(!step());
    assert(f.driver.graph[1].stage==NaGraphPaused && !r->fault);
    assert(f.driver.graph[0].stage==NaGraphRunning && f.driver.graph[2].stage==NaGraphRunning);
    assert(!na_runtime_request(r,NaAudioStartInput));
    for (unsigned i=0;i<60;++i) {
        if (!(i%10)) assert(!na_runtime_request(r,NaAudioStartInput));
        assert(!step());
    }
    assert(r->engine.active[1] && !r->engine.active[0]);
    for (unsigned i=0;i<70;++i) assert(!step());
    assert(f.driver.graph[1].stage==NaGraphPaused && !r->fault && !f.activated);
    assert(f.driver.graph[0].stage==NaGraphRunning && f.driver.graph[2].stage==NaGraphRunning);
    assert(f.input_maps==1 && f.input_opens==1 && f.input_stops==2 && !f.input_closes && !f.input_unmaps);
    assert(!f.driver.stream[1].in_flight && f.driver.stream[1].next==f.driver.stream[1].returned);
    na_runtime_disable(r);for(unsigned i=0;i<30 && !na_runtime_quiescent(r);++i) assert(!step());
    assert(na_runtime_quiescent(r));
    assert(f.input_closes==1 && f.input_unmaps==1);
}
static void system_duplex_restart(void) {
    initialize_capture(1);NaRuntime *r=&f.driver.runtime;
    r->capture_proven=1;r->status.in_channels=1;
    for (unsigned run=0;run<3;++run) {
        assert(!na_runtime_request(r,NaAudioStartInput));
        for (unsigned i=0;i<120;++i) {
            if (!(i%10)) assert(!na_runtime_request(r,NaAudioStartInput));
            assert(!step());
        }
        assert(r->engine.active[1] && r->status.in_produced && !r->fault);
        assert(!na_runtime_request(r,NaAudioStopInput));
        for (unsigned i=0;i<30 && f.driver.graph[1].stage!=NaGraphPaused;++i) assert(!step());
        assert(f.driver.graph[1].stage==NaGraphPaused);
        uint32_t reads=f.reads;uint64_t completed=f.driver.stream[1].completions;
        start();write_next(9000);steps(30);
        assert(!na_runtime_request(r,NaAudioStopOutput));standby_complete();steps(30);
        assert(f.reads==reads && f.driver.stream[1].completions==completed && !r->engine.active[1]);
    }
    assert(f.input_opens==1 && f.input_maps==1 && !f.input_closes && !f.input_unmaps);
    assert(f.input_stops==3 && r->engine.activation_completed[1]==3);
    assert(na_runtime_rebind_ready(r));
    static NaAudioStatus mapped;
    static _Alignas(8) uint64_t mapped_out[NA_AUDIO_RING_FRAMES],mapped_in[NA_AUDIO_RING_FRAMES];
    assert(!na_runtime_rebind(r,&mapped,mapped_out,mapped_in));
    assert(!na_runtime_request(r,NaAudioStartInput));
    for(unsigned i=0;i<70;++i) {if(!(i%10)) assert(!na_runtime_request(r,NaAudioStartInput));assert(!step());}
    assert(r->engine.active[1] && !f.input_closes && f.input_maps==1);
    na_runtime_disable(r);for(unsigned i=0;i<60 && !na_runtime_quiescent(r);++i) assert(!step());
    assert(na_runtime_quiescent(r) && f.input_closes==1 && f.input_unmaps==1);
}
static void system_input_timeout(void) {
    initialize_capture(1);NaRuntime *r=&f.driver.runtime;r->capture_proven=1;r->status.in_channels=1;
    f.suppress_reads=1;assert(!na_runtime_request(r,NaAudioStartInput));
    for(unsigned i=0;i<650;++i) {
        if (!(i%10) && !f.driver.graph[1].error) assert(!na_runtime_request(r,NaAudioStartInput));
        assert(!step());
    }
    assert(f.driver.graph[1].error==NA_TIMEOUT && !r->fault && !r->disabled);
    assert((r->status.state&NaAudioOutputReady) && !(r->status.state&NaAudioInputReady));
    assert(r->input_recovery_attempted && r->input_first_error==NA_TIMEOUT);
    assert(na_runtime_request(r,NaAudioStartInput)==NA_TIMEOUT);
    assert(f.driver.graph[0].stage==NaGraphRunning && f.driver.graph[2].stage==NaGraphRunning);
    start();write_next(8000);steps(10);assert(r->engine.active[0]);
    na_runtime_disable(r);for(unsigned i=0;i<60 && !na_runtime_quiescent(r);++i) assert(!step());
    assert(na_runtime_quiescent(r));
}
static void cold_system_idle(void) {
    initialize_capture(1);NaRuntime *r=&f.driver.runtime;
    r->capture_proven=1;r->status.in_channels=1;r->output_held=1;
    /* Same board policy as the production input HAL, before any Start.
     * The original runtime opens the retained output/VI and sends silence. */
    for(unsigned i=0;i<120;++i)assert(!step());
    assert(!r->request&&!r->probe.state&&r->engine.output_idle&&!r->engine.active[0]);
    assert(r->output_idle_ready&&f.zero_periods&&!f.nonzero_periods&&!f.activated);
    assert(f.driver.graph[0].stage==NaGraphRunning&&f.driver.graph[2].stage==NaGraphRunning);
    assert(f.driver.graph[1].stage==NaGraphOff&&!f.reads&&!f.input_maps&&!f.input_opens);
    assert(f.driver.prm.held==1&&!f.releases&&!f.closes&&!f.unmaps);
    assert(na_runtime_rebind_ready(r)&&r->service_ready);
    start();write_next(8000);steps(3);assert(f.nonzero_periods&&r->engine.active[0]);
    assert(!na_runtime_request(r,NaAudioStopOutput));standby_complete();
    na_runtime_disable(r);
    for(unsigned i=0;i<120&&!na_runtime_quiescent(r);++i)assert(!step());
    assert(na_runtime_quiescent(r)&&f.closes==2&&f.unmaps==2&&f.releases==1);
    assert(!f.reads&&!f.input_maps&&!f.input_opens);
}
static void cold_duplex_after_ringtone(void) {
    initialize_capture(1);NaRuntime *r=&f.driver.runtime;
    r->capture_proven=1;r->status.in_channels=1;r->output_held=1;
    steps(120);
    start();write_next(8000);steps(10);
    assert(!na_runtime_request(r,NaAudioStopOutput));standby_complete();
    unsigned activations=f.activated;
    assert(!na_runtime_request(r,NaAudioStartOutput));
    assert(!na_runtime_request(r,NaAudioStartInput));
    unsigned first_input_silent=0;
    for (unsigned i=0;i<150;++i) {
        if (!(i%10)) assert(!na_runtime_request(r,NaAudioStartInput));
        /* First capture must use the measured silent output/VI state even
         * though the output request stays queued. PCM unlocks playback. */
        if (!r->status.in_produced)
            assert(!r->engine.active[0] && f.activated==activations);
        assert(!step());
        if (f.reads && !r->status.in_produced) {
            assert(r->engine.output_idle && r->output_idle_ready);
            first_input_silent=1;
        }
    }
    assert(first_input_silent && (r->request&1));
    assert(f.activated==activations+1 && r->engine.active[0] && r->engine.active[1]);
    assert(f.input_maps==1 && f.input_opens==1 && f.reads && r->status.in_produced);
    assert(!r->fault && !f.driver.graph[1].error);
    assert(!na_runtime_request(r,NaAudioStopInput));
    for(unsigned i=0;i<30 && f.driver.graph[1].stage!=NaGraphPaused;++i)assert(!step());
    assert(f.driver.graph[1].stage==NaGraphPaused);
    assert(!na_runtime_request(r,NaAudioStopOutput));standby_complete();
    assert(!na_runtime_request(r,NaAudioStartInput));
    for(unsigned i=0;i<80;++i) {if(!(i%10))assert(!na_runtime_request(r,NaAudioStartInput));assert(!step());}
    assert(r->engine.active[1] && !r->engine.active[0] && f.input_maps==1);
    na_runtime_disable(r);
    for(unsigned i=0;i<80 && !na_runtime_quiescent(r);++i)assert(!step());
    assert(na_runtime_quiescent(r));
}
static void first_duplex_timeout(void) {
    initialize_capture(1);NaRuntime *r=&f.driver.runtime;
    r->capture_proven=1;r->status.in_channels=1;r->output_held=1;
    steps(120);start();steps(10);
    assert(!na_runtime_request(r,NaAudioStopOutput));standby_complete();
    unsigned activations=f.activated;
    f.suppress_reads=1;
    assert(!na_runtime_request(r,NaAudioStartOutput));
    assert(!na_runtime_request(r,NaAudioStartInput));
    for (unsigned i=0;i<650;++i) {
        if (!(i%10) && !f.driver.graph[1].error)
            assert(!na_runtime_request(r,NaAudioStartInput));
        if (!f.driver.graph[1].error)
            assert(!r->engine.active[0] && f.activated==activations);
        assert(!step());
    }
    assert(f.reads==8 && f.driver.graph[1].error==NA_TIMEOUT);
    assert(r->input_recovery_attempted && r->input_first_error==NA_TIMEOUT);
    assert(!r->status.in_produced && !r->fault);
    assert(r->engine.active[0] && f.activated==activations+1);
    assert((r->request&1) && !(r->status.state&NaAudioInputReady));
    na_runtime_disable(r);
    for(unsigned i=0;i<80 && !na_runtime_quiescent(r);++i)assert(!step());
    assert(na_runtime_quiescent(r));
}
static void first_input_recovery(unsigned keep_suppressed,unsigned fail_after_PCM) {
    initialize_capture(1);NaRuntime *r=&f.driver.runtime;
    r->capture_proven=1;r->status.in_channels=1;r->output_held=1;
    steps(120);start();steps(10);
    assert(!na_runtime_request(r,NaAudioStopOutput));standby_complete();
    f.suppress_reads=1;
    assert(!na_runtime_request(r,NaAudioStartOutput));
    assert(!na_runtime_request(r,NaAudioStartInput));
    for (unsigned i=0;i<350 && !f.driver.graph[1].error;++i) {
        if (!(i%10)) assert(!na_runtime_request(r,NaAudioStartInput));
        assert(!step());
    }
    assert(f.driver.graph[1].error==NA_TIMEOUT && f.reads==4 && !r->fault);
    unsigned original_maps=f.input_maps;
    /* A live renewed request is held through the first failed graph's drain;
     * it never causes another MAP before that graph is completely released. */
    assert(!na_runtime_request(r,NaAudioStartInput));
    while (f.driver.graph[1].stage!=NaGraphOff) {
        assert(!step());assert(f.input_maps==original_maps);
    }
    assert(f.input_closes==1 && f.input_unmaps==1 && !f.driver.stream[1].handle);
    f.suppress_reads=keep_suppressed;
    for (unsigned i=0;i<350;++i) {
        if (!(i%10) && (!f.driver.graph[1].error || !r->input_recovery_attempted))
            assert(!na_runtime_request(r,NaAudioStartInput));
        assert(!step());
    }
    assert(r->input_recovery_attempted==1 && r->input_first_error==NA_TIMEOUT);
    assert(f.input_maps==2 && f.input_opens==2 && !r->fault && !r->disabled);
    assert(r->request&1);
    if (keep_suppressed) {
        assert(f.driver.graph[1].error==NA_TIMEOUT && f.reads==8 && !r->status.in_produced);
        assert(na_runtime_request(r,NaAudioStartInput)==NA_TIMEOUT);
        assert(!(r->status.state&NaAudioInputReady) && r->engine.active[0]);
    } else {
        assert(!f.driver.graph[1].error && r->input_had_pcm && r->status.in_produced);
        assert((r->status.state&NaAudioInputReady) && r->engine.active[0] && r->engine.active[1]);
        if (fail_after_PCM) {
            /* Once real PCM has worked, any later timeout remains terminal. */
            f.driver.graph[1].error=NA_TIMEOUT;
            assert(na_runtime_request(r,NaAudioStartInput)==NA_TIMEOUT);
        } else {
            /* The reopened input then uses the original STOP/Paused path,
             * without another codec reset or map on ordinary recording. */
            assert(!na_runtime_request(r,NaAudioStopInput));
            for(unsigned i=0;i<40 && f.driver.graph[1].stage!=NaGraphPaused;++i)assert(!step());
            assert(f.driver.graph[1].stage==NaGraphPaused);
            assert(!na_runtime_request(r,NaAudioStartInput));
            for(unsigned i=0;i<70;++i) {
                if(!(i%10))assert(!na_runtime_request(r,NaAudioStartInput));
                assert(!step());
            }
            assert(f.input_maps==2 && f.input_opens==2 && r->engine.active[1]);
        }
    }
    na_runtime_disable(r);
    for(unsigned i=0;i<120 && !na_runtime_quiescent(r);++i)assert(!step());
    assert(na_runtime_quiescent(r));
}
static void first_input_recovery_cleanup_failure(unsigned which) {
    initialize_capture(1);NaRuntime *r=&f.driver.runtime;
    r->capture_proven=1;r->status.in_channels=1;r->output_held=1;steps(120);
    f.suppress_reads=1;f.close_failure=which==0;f.input_unmap_failure=which==1;f.input_release_failure=which==2;
    assert(!na_runtime_request(r,NaAudioStartInput));
    for(unsigned i=0;i<350;++i) {
        if (!(i%10) && !f.driver.graph[1].error) assert(!na_runtime_request(r,NaAudioStartInput));
        assert(!step());
    }
    assert(f.driver.graph[1].error==NA_TIMEOUT && f.driver.graph[1].cleanup_error);
    assert(!r->input_recovery_attempted && f.input_maps==1);
    assert(na_runtime_request(r,NaAudioStartInput)==NA_TIMEOUT);
    assert((r->status.state&NaAudioOutputReady) && !r->fault);
}
int main(void) {
    first_input_recovery(0,0);first_input_recovery(0,1);first_input_recovery(1,0);
    for(unsigned i=0;i<3;++i)first_input_recovery_cleanup_failure(i);
    cold_duplex_after_ringtone();
    first_duplex_timeout();
    cold_system_idle();
    system_input_only();system_duplex_restart();system_input_timeout();
    repeat_and_rebind();staged_stop();failures();probe_after_playback();full_PCM_range();
    cold_probe(0,0);cold_probe(1,0);cold_probe(1,1);outer_probe_fault();
    puts("production idle/restart: first PCM, silent/off hold, rapid Stop/Start, reprepare/VI, staged discard, fresh maps/clock, shutdown, DMA fault and disconnect passed; synthetic peer only");
    return 0;
}
