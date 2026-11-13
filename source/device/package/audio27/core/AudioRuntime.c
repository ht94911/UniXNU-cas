/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioRuntime.h"
#include <string.h>
#include <limits.h>
static int current_frame(NaRuntime *r,uint64_t *frame) {
    int e=na_clock_frames(r->status.anchor_host,r->ops.host_time(r->ops.context),r->status.ticks_per_frame_q32,frame);
    return e ? e : *frame<=NA_AUDIO_MAX_FRAME-NA_AUDIO_RING_FRAMES ? NA_OK:NA_OVERFLOW;
}
static int origin(NaRuntime *r,unsigned d) {
    if (r->origin_set[d]) return NA_OK;
    NaStream *s=r->engine.graph[d]->stream;
    if (!d && s->next>UINT64_MAX/NA_AUDIO_PERIOD_FRAMES) return NA_OVERFLOW;
    int e=current_frame(r,&r->origin[d]);
    if (!e) {r->relative_origin[d]=d ? 0:s->next*NA_AUDIO_PERIOD_FRAMES;r->origin_set[d]=1;}
    return e;
}
static int playback(void *v,uint64_t relative,void *pcm,uint32_t bytes) {
    NaRuntime *r=v;
    if (!pcm || bytes!=NA_AUDIO_PERIOD_FRAMES*4) return NA_ARGUMENT;
    int e=origin(r,0);if (e) return e;
    if (relative<r->relative_origin[0]) return NA_OWNERSHIP;
    relative-=r->relative_origin[0];
    if (relative>NA_AUDIO_MAX_FRAME-r->origin[0] || r->origin[0]+relative>NA_AUDIO_MAX_FRAME-bytes/4) return NA_OVERFLOW;
    uint64_t start=r->origin[0]+relative;int16_t *out=pcm;unsigned missing=0;
    for (unsigned i=0;i<bytes/4;++i) {
        int16_t l=0,rr=0;e=na_ring_take(r->output,start+i,&l,&rr);
        if (e==NA_MISSING || e==NA_BUSY) ++missing;else if (e) return e;
        /* HAL and tagged-ring mixing already saturate to S16. Use their
         * full signal range; factory TAS gain/calibration and running VI
         * retain speaker protection. Idle still discards PCM unconditionally. */
        out[2*i]=r->engine.output_idle ? 0:l;
        out[2*i+1]=r->engine.output_idle ? 0:rr;
    }
    if (missing && !r->engine.output_idle && r->status.underruns!=UINT32_MAX) ++r->status.underruns;
    r->status.out_consumed=start+bytes/4;
    return NA_OK;
}
static int capture(void *v,uint64_t relative,const void *pcm,uint32_t bytes) {
    NaRuntime *r=v;
    if (!r->input || !pcm || bytes>NA_AUDIO_PERIOD_FRAMES*2 || (bytes&1)) return NA_ARGUMENT;
    if (!r->capture_proven) {
        uint32_t state=__atomic_load_n(&r->probe.state,__ATOMIC_ACQUIRE);
        if (state==NaProbeDraining) return NA_OK; /* discard bounded drain */
        if (state!=NaProbeStarting && state!=NaProbeCollecting) return NA_OWNERSHIP;
        const int16_t *samples=pcm;
        for (unsigned i=0;i<bytes/2 && r->probe.frames<r->probe.limit_frames;++i) {
            int32_t value=samples[i];uint32_t absolute=(uint32_t)(value<0 ? -value:value);
            if (r->probe.pcm_frames<NA_CAPTURE_PROBE_PCM_FRAMES)
                r->probe.pcm[r->probe.pcm_frames++]=samples[i];
            ++r->probe.frames;if (value) ++r->probe.nonzero;
            if (absolute>r->probe.peak) r->probe.peak=absolute;
            r->probe.squares+=(uint64_t)(value*value);
        }
        ++r->probe.periods;
        __atomic_store_n(&r->probe.state,r->probe.frames==r->probe.limit_frames ? NaProbeDraining:NaProbeCollecting,__ATOMIC_RELEASE);
        return NA_OK;
    }
    int e=origin(r,1);if (e) return e;
    if (relative>NA_AUDIO_MAX_FRAME-r->origin[1] || r->origin[1]+relative>NA_AUDIO_MAX_FRAME-bytes/2) return NA_OVERFLOW;
    const int16_t *in=pcm;uint64_t first=r->origin[1]+relative;
    for (unsigned i=0;i<bytes/2;++i) {e=na_ring_capture(r->input,first+i,in[i]);if (e) return e;}
    r->status.in_produced=first+bytes/2;return NA_OK;
}
static int activate(void *v,int direction) {
    NaRuntime *r=v;return r->ops.activate(r->ops.context,direction);
}
int na_runtime_init(NaRuntime *r,NaSession *s,NaRxQueue *rx,NaGraph *out,NaGraph *in,NaPrm *prm,NaTransport *transport,
                    NaAudioStatus *shared,uint64_t *output,uint64_t *input,
                    uint32_t numer,uint32_t denom,uint32_t outvotes,uint32_t invotes,int capture_proven,const NaRuntimeOps *ops) {
    if (!r || !s || !rx || !out || !prm || !transport || !shared || !output || !ops || !ops->host_time || !ops->activate || !ops->health || !ops->standby ||
        ((uintptr_t)shared&7) || ((uintptr_t)output&7) || (in && !input) ||
        out->stream->period_bytes!=NA_AUDIO_PERIOD_FRAMES*4 || out->stream->frame_bytes!=4 ||
        (in && (in->stream->period_bytes!=NA_AUDIO_PERIOD_FRAMES*2 || in->stream->frame_bytes!=2)) ||
        !outvotes || ((outvotes|invotes)>>prm->count) || (capture_proven && (!in || !invotes)) || !numer || !denom) return NA_ARGUMENT;
    uint64_t hz=1000000000ULL*denom/numer,q=0;
    if (hz>UINT32_MAX || na_clock_contract(numer,denom,(uint32_t)hz,&q)) return NA_MISSING;
    memset(r,0,sizeof *r);r->ops=*ops;r->prm=prm;r->transport=transport;r->shared=shared;r->output=output;r->input=input;
    r->votes[0]=outvotes;r->votes[1]=invotes;r->capture_proven=!!capture_proven;
    NaEngineOps engine={r,playback,capture,activate};
    int e=na_engine_init(&r->engine,s,rx,out,in,&engine);if (e) return e;
    na_engine_output_idle(&r->engine,1);
    r->status=(NaAudioStatus){.magic=NA_AUDIO_MAGIC,.version=NA_AUDIO_ABI_VERSION,.bytes=sizeof(NaAudioStatus),
        .rate=NA_AUDIO_RATE,.ring_frames=NA_AUDIO_RING_FRAMES,.period_frames=NA_AUDIO_PERIOD_FRAMES,.lead_frames=NA_AUDIO_LEAD_FRAMES,
        .epoch=1,.out_channels=2,.in_channels=capture_proven ? 1u:0u,.host_hz=(uint32_t)hz,.ticks_per_frame_q32=q,
        .anchor_host=ops->host_time(ops->context)};
    /* Caller allocates fresh zeroed maps; no epoch reuse of writable mappings. */
    na_audio_status_publish(shared,&r->status);return NA_OK;
}
int na_runtime_request(NaRuntime *r,uint32_t selector) {
    if (!r || selector>NaAudioStopInput) return NA_ARGUMENT;
    uint32_t bit=selector<2 ? 1:2;int start=!(selector&1);
    uint32_t probe=__atomic_load_n(&r->probe.state,__ATOMIC_ACQUIRE);
    if (start && probe>=NaProbeStarting && probe<=NaProbeDraining) return NA_BUSY;
    if (start && (__atomic_load_n(&r->disabled,__ATOMIC_ACQUIRE) || !__atomic_load_n(&r->service_ready,__ATOMIC_ACQUIRE) || (bit==2 && !r->capture_proven))) return NA_MISSING;
    if (start) {
        if (bit==2) __atomic_fetch_add(&r->input_heartbeat,1,__ATOMIC_ACQ_REL);
        __atomic_fetch_or(&r->request,bit,__ATOMIC_ACQ_REL);
    }
    else {
        __atomic_fetch_and(&r->request,~bit,__ATOMIC_ACQ_REL);
        if (bit==1) {
            na_ring_silence(r->output);
            /* Retain a Stop even when Start follows before the next poll. */
            __atomic_fetch_add(&r->output_stops,1,__ATOMIC_ACQ_REL);
        }
    }
    return NA_OK;
}
void na_runtime_disable(NaRuntime *r) {
    if (!r) return;
    __atomic_store_n(&r->disabled,1,__ATOMIC_RELEASE);
    __atomic_store_n(&r->service_ready,0,__ATOMIC_RELEASE);
    __atomic_store_n(&r->request,0,__ATOMIC_RELEASE);
}
int na_runtime_quiescent(const NaRuntime *r) {
    if (!r || r->prm->possible || r->prm->token) return 0;
    for (unsigned i=0;i<3;++i) if (r->engine.graph[i]) {
        const NaGraph *g=r->engine.graph[i];
        if (g->stage!=NaGraphOff || g->endpoint_owned || g->map_possible || g->stream->quarantine) return 0;
    }
    return 1;
}
static int cpu_maps_quiet(const NaRuntime *r) {
    if (na_runtime_quiescent(r)) return 1;
    if (!r->output_held || !r->engine.output_idle || !r->output_idle_ready || r->output_stop_pending ||
        r->output_seen_stop!=__atomic_load_n(&r->output_stops,__ATOMIC_ACQUIRE) ||
        !na_prm_ready(r->prm,r->votes[0])) return 0;
    for (unsigned i=0;i<3;++i) if (r->engine.graph[i]) {
        const NaGraph *g=r->engine.graph[i];
        if (i==1) {
            if (g->stage!=NaGraphOff || g->endpoint_owned || g->map_possible || g->stream->quarantine) return 0;
        } else if (g->stage!=NaGraphRunning || g->error || g->cleanup_error || !g->want ||
                   !g->stream->running || g->stream->quarantine) return 0;
    }
    return 1;
}
int na_runtime_rebind_ready(const NaRuntime *r) {
    if (!r || r->fault || __atomic_load_n(&r->disabled,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(&r->request,__ATOMIC_ACQUIRE)) return 0;
    uint32_t probe=__atomic_load_n(&r->probe.state,__ATOMIC_ACQUIRE);
    if (probe>=NaProbeStarting && probe<=NaProbeDraining) return 0;
    return cpu_maps_quiet(r);
}
int na_runtime_rebind(NaRuntime *r,NaAudioStatus *status,uint64_t *output,uint64_t *input) {
    if (!r || !status || !output || ((uintptr_t)status&7) || ((uintptr_t)output&7) ||
        (r->input && (!input || ((uintptr_t)input&7))) || status==r->shared || output==r->output ||
        (input && input==r->input)) return NA_ARGUMENT;
    if (r->fault || r->status.epoch==UINT32_MAX) return NA_OWNERSHIP;
    if (!na_runtime_rebind_ready(r)) return NA_BUSY;
    NaAudioStatus retired=r->status;retired.state=0;
    na_audio_status_publish(r->shared,&retired);
    r->shared=status;r->output=output;r->input=input;
    ++r->status.epoch;r->status.anchor_host=r->ops.host_time(r->ops.context);
    r->status.out_consumed=r->status.in_produced=0;
    r->status.state&=~(NaAudioInputRunning|NaAudioOutputRunning);
    memset(r->origin_set,0,sizeof r->origin_set);
    r->engine.staged[0]=0; /* never carry an old client's unsent PCM into fresh maps */
    r->input_seen=__atomic_load_n(&r->input_heartbeat,__ATOMIC_ACQUIRE);r->input_deadline=0;
    na_audio_status_publish(status,&r->status);return NA_OK;
}
int na_runtime_probe_capture(NaRuntime *r,uint32_t frames,uint64_t now) {
    if (!r || !frames || frames>2*NA_AUDIO_RATE || now>UINT64_MAX-30000) return NA_ARGUMENT;
    if (!r->capture_probe_allowed || r->capture_proven || !r->engine.graph[1] || !r->votes[1] ||
        __atomic_load_n(&r->disabled,__ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&r->service_ready,__ATOMIC_ACQUIRE) || r->fault) return NA_MISSING;
    uint32_t state=__atomic_load_n(&r->probe.state,__ATOMIC_ACQUIRE);
    if (!cpu_maps_quiet(r) || __atomic_load_n(&r->request,__ATOMIC_ACQUIRE) ||
        (state>=NaProbeStarting && state<=NaProbeDraining)) return NA_BUSY;
    r->probe.limit_frames=frames;r->probe.frames=r->probe.periods=r->probe.nonzero=r->probe.peak=0;
    r->probe.squares=0;r->probe.deadline=now+30000;r->probe.error=0;
    r->probe.pcm_frames=0;memset(r->probe.pcm,0,sizeof r->probe.pcm);
    __atomic_store_n(&r->probe.state,NaProbeStarting,__ATOMIC_RELEASE);return NA_OK;
}
int na_runtime_poll(NaRuntime *r,uint64_t now) {
    if (!r || now>UINT64_MAX-10000) return NA_ARGUMENT;
    uint32_t wanted=__atomic_load_n(&r->request,__ATOMIC_ACQUIRE),mask=0;
    if (wanted&2) {
        uint64_t heartbeat=__atomic_load_n(&r->input_heartbeat,__ATOMIC_ACQUIRE);
        if (heartbeat!=r->input_seen) {r->input_seen=heartbeat;r->input_deadline=now+500;}
        if (!r->input_deadline || now>=r->input_deadline) {
            /* Expiration is a worker-local decision. Clearing the shared bit
             * here could erase a renewal racing with this snapshot. The next
             * poll observes that renewal; explicit Stop still clears it. */
            wanted&=~2u;
        }
    }
    uint64_t frame=0;int e=current_frame(r,&frame);
    if (!e) e=r->ops.health(r->ops.context);
    if (e<0 && !r->fault) r->fault=e;
    if (r->fault) {na_runtime_disable(r);wanted=0;}
    int disabled=__atomic_load_n(&r->disabled,__ATOMIC_ACQUIRE);
    if (disabled) wanted=0;
    NaGraph *out=r->engine.graph[0],*vi=r->engine.graph[2];
    uint64_t stops=__atomic_load_n(&r->output_stops,__ATOMIC_ACQUIRE);
    if (stops!=r->output_seen_stop) {
        r->output_seen_stop=stops;
        if (r->output_held) {
            r->output_stop_pending=1;r->output_idle_ready=0;
            r->output_idle_fence=out->stream->next;
        }
    }
    if ((wanted&1) && !disabled) r->output_held=1;
    int keep_output=r->output_held && !disabled && !r->fault;
    int idle=!keep_output || !(wanted&1) || r->output_stop_pending;
    na_engine_output_idle(&r->engine,idle);
    if (!idle) r->output_idle_ready=0;
    if (idle && keep_output && !r->output_idle_ready && out->stage==NaGraphRunning &&
        (!vi || vi->stage==NaGraphRunning)) {
        e=r->ops.standby(r->ops.context);
        if (e<0 && !r->fault) {r->fault=e;na_runtime_disable(r);wanted=0;keep_output=0;}
        if (!e && out->stream->returned>=r->output_idle_fence) {
            r->output_stop_pending=0;r->output_idle_ready=1;
        }
    }
    uint32_t probe=__atomic_load_n(&r->probe.state,__ATOMIC_ACQUIRE);
    if (probe==NaProbeStarting || probe==NaProbeCollecting) {
        if (r->fault || disabled || now>=r->probe.deadline) {
            r->probe.error=r->fault ? r->fault:disabled ? NA_OFFLINE:NA_TIMEOUT;
            __atomic_store_n(&r->probe.state,NaProbeDraining,__ATOMIC_RELEASE);
        } else if (!disabled) wanted|=2u;
    }
    if (r->engine.graph[2]) {
        int draining=out->stage>=NaGraphMuting;
        /* A new Start while the prior stream drains stays queued. Keeping VI
         * alive from that new request would deadlock output's release gate. */
        int keep=(!draining && keep_output) || (!r->fault && out->map_possible && out->stage<NaGraphRelease);
        if (keep || vi->stage!=NaGraphOff) mask|=r->votes[0];
        if (!keep) na_graph_request(vi,0);
        else if (na_prm_ready(r->prm,r->votes[0])) {
            int vr=na_graph_request(vi,1);if (vr<0 && vr!=NA_BUSY && !r->fault) r->fault=vr;
        }
    }
    for (unsigned i=0;i<2;++i) if (r->engine.graph[i]) {
        NaGraph *g=r->engine.graph[i];
        int keep=i ? !!(wanted&2):keep_output;
        if (keep || g->stage!=NaGraphOff || g->endpoint_owned) mask|=r->votes[i];
        if (!keep) na_graph_request(g,0);
        else if (na_prm_ready(r->prm,r->votes[i])) {
            e=na_graph_request(g,1);if (e<0 && e!=NA_BUSY && !r->fault) r->fault=e;
        }
        if (g->stage==NaGraphOff) r->origin_set[i]=0;
        /* Capture starts at the first submitted period. The callback uses the
         * actual returned byte count; it cannot fabricate a full-period read. */
        if (i==1 && g->stage==NaGraphRunning && !r->origin_set[i]) origin(r,i);
    }
    na_prm_request(r->prm,mask);
    e=na_engine_poll(&r->engine,now);
    if (e<0 && e!=NA_OFFLINE && !r->fault) r->fault=e;
    e=na_prm_poll(r->prm,now);
    if (e<0 && e!=NA_OFFLINE && !r->fault) r->fault=e;
    if (!r->fault && !__atomic_load_n(&r->disabled,__ATOMIC_ACQUIRE) && r->engine.session->online) {
        r->status.state=NaAudioOutputReady|(r->capture_proven ? NaAudioInputReady:0);
        __atomic_store_n(&r->service_ready,1,__ATOMIC_RELEASE);
    } else {r->status.state=r->fault ? NaAudioFault:0;__atomic_store_n(&r->service_ready,0,__ATOMIC_RELEASE);}
    for (unsigned i=0;i<2;++i) if (r->engine.graph[i]) {
        NaGraph *g=r->engine.graph[i];
        if (!r->fault && !__atomic_load_n(&r->disabled,__ATOMIC_ACQUIRE) && r->engine.active[i])
            r->status.state|=i ? NaAudioInputRunning:NaAudioOutputRunning;
        if (i) r->status.input_periods=g->stream->returned;else r->status.output_periods=g->stream->returned;
    }
    if (!r->origin_set[0]) r->status.out_consumed=frame;
    probe=__atomic_load_n(&r->probe.state,__ATOMIC_ACQUIRE);
    if (probe>=NaProbeStarting && probe<=NaProbeDraining && r->fault) {
        r->probe.error=r->fault;
        __atomic_store_n(&r->probe.state,NaProbeDraining,__ATOMIC_RELEASE);
    }
    if (__atomic_load_n(&r->probe.state,__ATOMIC_ACQUIRE)==NaProbeDraining && cpu_maps_quiet(r))
        __atomic_store_n(&r->probe.state,r->probe.error ? NaProbeFailed:NaProbeComplete,__ATOMIC_RELEASE);
    r->status.last_error=(uint32_t)r->fault;
    na_audio_status_publish(r->shared,&r->status);return r->fault;
}
