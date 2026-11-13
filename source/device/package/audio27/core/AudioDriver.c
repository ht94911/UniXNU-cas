/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioDriver.h"
#include "AudioTopology.h"
#include <string.h>
/* Bootstrap performs a muted speaker identification/configuration before HAL
 * publication. It does not open a graph or produce automatic test audio. */
enum {DLink,DVotes,DProbe,DProbeRelease,DVoteRelease,DReady,DFault};
static int send_packet(void *v,const uint8_t *p,uint32_t n) {
    NaDriver *d=v;return na_transport_send(&d->adsp->transport,d->session.generation,p,n);
}
static int graph_send(void *v,const uint8_t *p,uint32_t n) {return send_packet(((NaDriverDirection *)v)->driver,p,n);}
static int sync_dma(void *v,uint32_t at,uint32_t n,int for_device) {
    NaDriverDirection *x=v;return x->driver->ops.sync_dma(x->driver->ops.context,x->direction,at,n,for_device);
}
static int prepare_endpoint(void *v) {
    NaDriverDirection *x=v;NaDriver *d=x->driver;
    if (x->direction==2) {
        if (d->speaker->error) return d->speaker->error;
        return d->speaker->lease && d->speaker->state>=NaTasPrepared && !d->speaker->changing ? NA_OK:NA_PENDING;
    }
    if (!x->direction) {
        int r=na_tas_endpoint_request(d->speaker,NaTasPrepared);
        return r ? r:na_tas_endpoint_poll(d->speaker,d->now_ms);
    }
    if (!d->mic_initialized) {
        d->mic_owned=1;
        int r=d->ops.mic_prepare(d->ops.context,&d->microphone);
        if (r) return r;d->mic_initialized=1;
    }
    return na_wcd_capture_prepare(&d->microphone,d->now_ms);
}
static int mute_endpoint(void *v) {
    NaDriverDirection *x=v;NaDriver *d=x->driver;
    if (x->direction==2) return NA_OK; /* DSP-only VI graph has no separate analog mute */
    if (x->direction) return d->mic_initialized ? na_wcd_capture_mute(&d->microphone):NA_OK;
    int r=na_tas_endpoint_request(d->speaker,NaTasOff);
    if (r) return r;
    r=na_tas_endpoint_poll(d->speaker,d->now_ms);
    /* The TAS fault handler asserts its private resets; its error remains
     * visible while graph close/unmap proceeds. */
    return r;
}
static int release_endpoint(void *v) {
    NaDriverDirection *x=v;NaDriver *d=x->driver;
    if (x->direction==2) return NA_OK; /* shared hardware lease remains with output graph */
    if (!x->direction) {
        if (d->feedback_attached && d->graph[2].stage!=NaGraphOff) return NA_PENDING;
        return na_tas_endpoint_release(d->speaker,1);
    }
    if (d->mic_initialized) {
        int r=na_wcd_capture_shutdown(&d->microphone,1);if (r) return r;
        d->mic_initialized=0;
    }
    if (d->mic_owned) {
        int r=d->ops.mic_release(d->ops.context);if (r) return r;
        d->mic_owned=0;
    }
    return NA_OK;
}
static uint64_t host_time(void *v) {NaDriver *d=v;return d->ops.host_time(d->ops.context);}
static int activate(void *v,int direction) {
    NaDriver *d=v;
    if (direction) return na_wcd_capture_activate(&d->microphone,d->now_ms);
    if (!d->feedback_attached || !d->protection_instance) return NA_MISSING;
    if (d->graph[2].error || d->graph[2].stage==NaGraphFault) return NA_REMOTE;
    if (d->graph[2].stage!=NaGraphRunning) return NA_PENDING;
    if (d->speaker->state==NaTasOff || (d->speaker->changing && d->speaker->target==NaTasPrepared)) {
        int r=na_tas_endpoint_request(d->speaker,NaTasPrepared);
        if (r) return r;
        r=na_tas_endpoint_poll(d->speaker,d->now_ms);
        return r ? r:NA_PENDING; /* Active follows a completed muted preparation */
    }
    int r=na_tas_endpoint_request(d->speaker,NaTasActive);
    return r ? r:na_tas_endpoint_poll(d->speaker,d->now_ms);
}
static int standby(void *v) {return mute_endpoint(&((NaDriver *)v)->direction[0]);}
static int health(void *v) {
    NaDriver *d=v;
    int r=d->ops.dma_health(d->ops.context);if (r) return r;
    if (d->speaker->state==NaTasActive && (!d->feedback_attached || d->graph[2].stage!=NaGraphRunning || d->graph[2].error)) return NA_REMOTE;
    if (d->speaker->state==NaTasActive && !d->speaker->changing) return na_tas_endpoint_poll(d->speaker,d->now_ms);
    return d->stage==DReady ? NA_OK:NA_PENDING;
}
int na_driver_init(NaDriver *d,NaAdsp *adsp,NaTasEndpoint *speaker,const NaDriverStream *out,const NaDriverStream *in,
                   const NaPrmResource *resources,uint32_t count,uint32_t outvotes,uint32_t invotes,
                   NaAudioStatus *status,uint64_t *out_ring,uint64_t *in_ring,uint32_t numer,uint32_t denom,int capture_proven,const NaDriverOps *ops) {
    if (!d || !adsp || !adsp->rx || !speaker || !out || !ops || !ops->host_time || !ops->sync_dma || !ops->dma_health ||
        (in && (!ops->mic_prepare || !ops->mic_release)) || (capture_proven && !in)) return NA_ARGUMENT;
    memset(d,0,sizeof *d);d->adsp=adsp;d->speaker=speaker;d->ops=*ops;d->input_enabled=in!=0;d->output_votes=outvotes;d->input_votes=invotes;
    na_session_init(&d->session);
    const NaDriverStream *streams[2]={out,in};
    for (unsigned i=0;i<2;++i) if (streams[i]) {
        const NaDriverStream *s=streams[i];d->direction[i]=(NaDriverDirection){d,i};
        int r=na_stream_init(&d->stream[i],s->memory,s->dsp_address,s->bytes,NA_AUDIO_PERIOD_FRAMES*(i ? 2:4),i ? 2:4,s->port,s->endpoint,i);
        if (r) return r;
        NaGraphOps go={&d->direction[i],graph_send,sync_dma,prepare_endpoint,mute_endpoint,release_endpoint};
        r=na_graph_init(&d->graph[i],&d->session,&d->stream[i],&s->topology,&go);if (r) return r;
        /* Use the TAS endpoint's existing diagnostic wait accounting. */
        if (!i) d->graph[i].endpoint_trace_wait_us=speaker->ops.trace_wait_us;
    }
    int r=na_prm_init(&d->prm,&d->session,resources,count,send_packet,d);if (r) return r;
    NaRuntimeOps ro={d,host_time,activate,health,standby};
    return na_runtime_init(&d->runtime,&d->session,adsp->rx,&d->graph[0],in ? &d->graph[1]:0,&d->prm,&adsp->transport,
        status,out_ring,in_ring,numer,denom,outvotes,invotes,capture_proven,&ro);
}
int na_driver_ready(const NaDriver *d) {return d && d->stage==DReady && !d->error && __atomic_load_n(&d->runtime.service_ready,__ATOMIC_ACQUIRE);}
int na_driver_attach_feedback(NaDriver *d,const NaDriverStream *s,const NaTiCalibration *calibration,uint32_t ti_instance) {
    if (!d || !s || !s->memory || s->bytes<65536 || s->bytes>0x1000000 || !calibration || !ti_instance || d->feedback_attached || d->stage!=DLink || d->session.online) return NA_ARGUMENT;
    NaTopology *t=&d->graph[0].topology;
    if (t->configs>=NA_GRAPH_CONFIGS) return NA_OVERFLOW;
    uint32_t actual=0,vi=0,hardware=0;
    if (na_topology_find_module(d->stream[0].memory+t->open.offset,t->open.bytes,0x870010a8,&actual) || actual!=ti_instance ||
        s->topology.open.offset>s->bytes || s->topology.open.bytes>s->bytes-s->topology.open.offset ||
        na_topology_find_module((const uint8_t *)s->memory+s->topology.open.offset,s->topology.open.bytes,0x870010a9,&vi) ||
        na_topology_find_module((const uint8_t *)s->memory+s->topology.open.offset,s->topology.open.bytes,0x0700100f,&hardware) ||
        s->endpoint!=hardware) return NA_MISSING;
    uint8_t payload[88];uint32_t n=0;
    int r=na_ti_calibration_payload(calibration,ti_instance,payload,&n);if (r) return r;
    uint32_t end=t->open.offset+t->open.bytes;
    for (unsigned i=0;i<t->configs;++i) {
        uint32_t e=t->config[i].offset+t->config[i].bytes;if (e>end) end=e;
    }
    if ((end&7) || end>d->stream[0].bytes || n>d->stream[0].bytes-end) return NA_OVERFLOW;
    d->direction[2]=(NaDriverDirection){d,2};
    r=na_stream_init(&d->stream[2],s->memory,s->dsp_address,s->bytes,1920,4,s->port,s->endpoint,0);if (r) return r;
    NaGraphOps go={&d->direction[2],graph_send,sync_dma,prepare_endpoint,mute_endpoint,release_endpoint};
    r=na_graph_init(&d->graph[2],&d->session,&d->stream[2],&s->topology,&go);if (r) return r;
    d->graph[2].endpoint_trace_wait_us=d->speaker->ops.trace_wait_us;
    r=na_engine_attach_control(&d->runtime.engine,&d->graph[2]);if (r) return r;
    memcpy(d->stream[0].memory+end,payload,n);t->config_peer[t->configs]=AR_APM_IID;t->config[t->configs++]=(NaBlob){end,n};
    d->protection_instance=ti_instance;d->feedback_attached=1;return NA_OK;
}
static int fail(NaDriver *d,int error) {
    d->error=error<0 ? error:NA_IO;d->stage=DFault;
    /* The outer ADSP/board failure bypasses normal runtime publication. Retire
     * the mapped ready/running state before the HAL can attempt another start. */
    if (!d->runtime.fault) d->runtime.fault=d->error;
    na_runtime_disable(&d->runtime);
    d->runtime.status.state=NaAudioFault;
    d->runtime.status.last_error=(uint32_t)d->runtime.fault;
    na_audio_status_publish(d->runtime.shared,&d->runtime.status);
    na_graph_request(&d->graph[0],0);if (d->input_enabled) na_graph_request(&d->graph[1],0);
    if (d->feedback_attached) na_graph_request(&d->graph[2],0);
    /* Also mute bootstrap's endpoint when no graph owns it yet. */
    na_tas_endpoint_request(d->speaker,NaTasOff);na_tas_endpoint_poll(d->speaker,d->now_ms);
    return d->error;
}
int na_driver_poll(NaDriver *d,uint64_t now) {
    if (!d) return NA_ARGUMENT;d->now_ms=now;
    if (d->stage==DLink && !d->feedback_attached) return NA_MISSING;
    int r=na_adsp_poll(d->adsp,now);
    if (r<0) {
        if (!d->error) fail(d,r);
        /* Continue ordered loss/cleanup processing. No PAS or firmware replay. */
    }
    if (d->stage==DReady) {
        r=na_runtime_poll(&d->runtime,now);return r ? fail(d,r):NA_OK;
    }
    r=na_engine_poll(&d->runtime.engine,now);
    if (r<0 && r!=NA_OFFLINE && !d->error) fail(d,r);
    if (d->stage==DFault) {
        uint32_t votes=0;
        for (unsigned i=0;i<3;++i) if (d->runtime.engine.graph[i] &&
            (d->graph[i].stage!=NaGraphOff || d->graph[i].endpoint_owned || d->graph[i].map_possible))
            votes|=i==1 ? d->input_votes:d->output_votes;
        if (d->speaker->lease && !d->graph[0].endpoint_owned && !d->graph[0].map_possible) {
            na_tas_endpoint_request(d->speaker,NaTasOff);na_tas_endpoint_poll(d->speaker,now);
            if (na_tas_endpoint_release(d->speaker,1)) votes|=d->output_votes;
        }
        na_prm_request(&d->prm,votes);na_prm_poll(&d->prm,now);
        return d->error;
    }
    if (!d->session.online) return NA_PENDING;
    if (d->stage==DLink) {na_prm_request(&d->prm,d->output_votes);d->stage=DVotes;}
    r=na_prm_poll(&d->prm,now);
    if (r<0) return fail(d,r);
    if (d->stage==DVotes && na_prm_ready(&d->prm,d->output_votes)) d->stage=DProbe;
    if (d->stage==DProbe) {
        r=prepare_endpoint(&d->direction[0]);
        if (r<0) return fail(d,r);if (r==NA_OK) d->stage=DProbeRelease;
    }
    if (d->stage==DProbeRelease) {
        r=mute_endpoint(&d->direction[0]);
        if (r==NA_OK) r=release_endpoint(&d->direction[0]);
        if (r<0) return fail(d,r);
        if (r==NA_OK) {na_prm_request(&d->prm,0);d->stage=DVoteRelease;}
    }
    if (d->stage==DVoteRelease && !d->prm.possible && d->prm.state==NaPrmStable) d->stage=DReady;
    if (d->stage==DReady) return na_runtime_poll(&d->runtime,now);
    return NA_PENDING;
}
