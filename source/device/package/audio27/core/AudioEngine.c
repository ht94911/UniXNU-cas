/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "AudioEngine.h"
#include <string.h>
int na_engine_init(NaEngine *e,NaSession *s,NaRxQueue *q,NaGraph *out,NaGraph *in,const NaEngineOps *ops) {
    if (!e || !s || !q || !ops || !ops->activate || (!out && !in) ||
        (out && (!ops->playback || out->session!=s || out->stream->capture)) ||
        (in && (!ops->capture || in->session!=s || !in->stream->capture)) ||
        (out && in && out->stream->port==in->stream->port)) return NA_ARGUMENT;
    memset(e,0,sizeof *e);e->session=s;e->rx=q;e->graph[0]=out;e->graph[1]=in;e->ops=*ops;
    return NA_OK;
}
int na_engine_attach_control(NaEngine *e,NaGraph *g) {
    if (!e || !g || e->graph[2] || g->session!=e->session || g->stage!=NaGraphOff || g->stream->running) return NA_ARGUMENT;
    for (unsigned i=0;i<2;++i) if (e->graph[i] && e->graph[i]->stream->port==g->stream->port) return NA_OWNERSHIP;
    e->graph[2]=g;return NA_OK;
}
void na_engine_link(NaEngine *e,int online) {
    if (!e) return;
    na_session_link(e->session,online);
    if (!online) for (unsigned i=0;i<3;++i) if (e->graph[i]) {
        NaGraph *g=e->graph[i];
        na_graph_request(g,0);
        /* A cold/off graph owns no DMA; preserve the ability to wait for the
         * first channel. A graph ever submitted is quarantined on disconnect. */
        if (g->map_possible) na_stream_lost(g->stream);
        if (g->endpoint_owned) {
            int r=g->ops.mute(g->ops.context);if (r) g->cleanup_error=r;
        }
        if (i<2) e->active[i]=0;
    }
}
static int fault(NaGraph *g,int result) {
    if (!g->error) g->error=result;
    na_stream_lost(g->stream);na_graph_request(g,0);
    return result;
}
static int periods(NaEngine *e,unsigned direction,uint64_t now) {
    NaGraph *g=e->graph[direction];NaStream *s=g->stream;
    uint8_t captured[4096];
    for (unsigned n=0;n<NA_DMA_SLOTS;++n) {
        NaBuffer *b=&s->slot[s->returned%NA_DMA_SLOTS];
        if (b->state!=NaBufferReturned || b->sequence!=s->returned) break;
        /* An authenticated DONE returned ownership; invalidate the whole
         * corresponding CPU slot before looking at the returned PCM. */
        uint32_t bytes=0;uint64_t frame=s->frames;
        int r=s->capture ? g->ops.sync(g->ops.context,
            (uint32_t)(s->returned%NA_DMA_SLOTS)*s->stride,s->period_bytes,0) : 0;
        if (r) return fault(g,r);
        r=na_stream_take(s,captured,sizeof captured,&bytes);
        if (r) return fault(g,r);
        if (s->capture && bytes) {
            r=e->ops.capture(e->ops.context,frame,captured,bytes);
            if (r) return fault(g,r);
        }
    }
    if (g->stage!=NaGraphRunning || !g->want || !s->running) return NA_OK;
    for (unsigned n=0;n<NA_DMA_FLIGHT;++n) {
        uint8_t packet[GPR_MAX_PACKET];unsigned index=0;uint32_t length=0;
        int r=na_stream_prepare(s,e->session,now,2000,&index,packet,&length);
        if (r==NA_BUSY) break;
        if (r) return fault(g,r);
        void *pcm=s->memory+index*s->stride;
        if (!e->staged[direction] || e->staged_sequence[direction]!=s->next) {
          memset(pcm,0,s->period_bytes);
          if (!s->capture) {
            if (s->next>UINT64_MAX/(s->period_bytes/s->frame_bytes)) {
                na_stream_unsent(s,index);return fault(g,NA_OVERFLOW);
            }
            r=e->ops.playback(e->ops.context,s->next*(s->period_bytes/s->frame_bytes),pcm,s->period_bytes);
          }
          if (!r) {e->staged[direction]=1;e->staged_sequence[direction]=s->next;}
        }
        if (!r) r=g->ops.sync(g->ops.context,index*s->stride,s->period_bytes,1);
        if (r) {na_stream_unsent(s,index);return fault(g,r);}
        r=na_stream_commit(s,index);
        if (r) {na_stream_unsent(s,index);return fault(g,r);}
        r=g->ops.send(g->ops.context,packet,length);
        if (r) {
            /* send errors guarantee no queued message; timeout has a separate
             * path and may never reach this undo. */
            na_stream_unsent(s,index);
            if (r==NA_BUSY) break;
            return fault(g,r);
        }
        ++e->periods_submitted[direction];
        e->staged[direction]=0;
    }
    if (!e->active[direction] && s->in_flight>=2) {
        if (!e->activation_deadline[direction]) e->activation_deadline[direction]=now+5000;
        if (now>=e->activation_deadline[direction]) return fault(g,NA_TIMEOUT);
        int r=e->ops.activate(e->ops.context,(int)direction);
        if (r==NA_PENDING) return NA_OK;
        if (r) return fault(g,r);
        e->active[direction]=1;
        ++e->activation_completed[direction];
    }
    return NA_OK;
}
int na_engine_poll(NaEngine *e,uint64_t now) {
    if (!e) return NA_ARGUMENT;
    int result=NA_OK;
    for (unsigned n=0;n<NA_RX_SLOTS;++n) {
        NaRxPacket packet;int r=na_rx_pop(e->rx,&packet);
        if (r==NA_PENDING) break;
        if (r) {++e->rx_faults;na_engine_link(e,0);result=r;break;}
        if (!packet.length) {
            na_engine_link(e,packet.data[0]);
            if (packet.generation!=e->session->generation) {
                ++e->rx_faults;na_engine_link(e,0);result=NA_OFFLINE;break;
            }
            continue;
        }
        if (packet.generation!=e->session->generation) {++e->stale_packets;continue;}
        r=na_receive(e->session,packet.data,packet.length);
        if (r==NA_WIRE) {na_engine_link(e,0);result=r;break;}
        for (unsigned i=0;i<3;++i) if (e->graph[i]) {
            NaGraph *g=e->graph[i];
            uint64_t first=g->stream->first_read_done;
            r=na_stream_done(g->stream,packet.data,packet.length);
            /* Same dispatch, before graph cleanup or PCM consumption changes
             * its stage. Existing first receipts stay fixed across replies. */
            if (!first && g->stream->first_read_done)
                g->stream->first_read_done|=(uint64_t)(g->stage&31u)<<27;
            if (r<0) result=fault(g,r);
        }
    }
    na_expire(e->session,now);
    for (unsigned i=0;i<3;++i) if (e->graph[i]) {
        NaGraph *g=e->graph[i];
        uint32_t was_quarantined=g->stream->quarantine;
        na_stream_expire(g->stream,now);
        if (!was_quarantined && g->stream->quarantine) result=fault(g,NA_TIMEOUT);
        int r=na_graph_poll(g,now);
        if (r<0) result=r;
        if (i<2 && g->stage!=NaGraphRunning) {e->active[i]=0;e->staged[i]=0;e->activation_deadline[i]=0;}
        if (i<2 && !g->stream->quarantine) {
            r=periods(e,i,now);
            if (r<0) result=r;
        }
    }
    return result;
}
