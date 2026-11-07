/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* sheng APM lifecycle, with explicit partial-open and timeout ownership. */
#include "AudioGraph.h"
#include <string.h>
static int blob_ok(NaBlob b,uint32_t bytes) {
    /* PCM owns the first eight pages; topology follows them. */
    return b.bytes && !(b.offset&7) && !(b.bytes&3) && b.offset>=NA_DMA_SLOTS*4096 &&
        b.offset<=bytes && b.bytes<=bytes-b.offset;
}
int na_graph_init(NaGraph *g,NaSession *session,NaStream *stream,
                  const NaTopology *t,const NaGraphOps *ops) {
    if (!g || !session || !stream || !t || !ops || !ops->send || !ops->sync ||
        !ops->prepare || !ops->mute || !ops->release || !t->configs || t->configs>NA_GRAPH_CONFIGS ||
        !t->subgraph_count || t->subgraph_count>16 || !blob_ok(t->open,stream->bytes)) return NA_ARGUMENT;
    for (unsigned i=0;i<t->configs;++i) if (!blob_ok(t->config[i],stream->bytes) ||
        (t->config_peer[i] && t->config_peer[i]!=AR_APM_IID && t->config_peer[i]!=stream->endpoint)) return NA_ARGUMENT;
    for (unsigned i=0;i<t->subgraph_count;++i) {
        if (!t->subgraphs[i]) return NA_ARGUMENT;
        for (unsigned j=0;j<i;++j) if (t->subgraphs[i]==t->subgraphs[j]) return NA_ARGUMENT;
    }
    memset(g,0,sizeof *g);g->session=session;g->stream=stream;g->topology=*t;g->ops=*ops;
    return NA_OK;
}
int na_graph_request(NaGraph *g,int start) {
    if (!g) return NA_ARGUMENT;
    if (start && (g->error || g->stage==NaGraphFault || g->stream->quarantine)) return NA_OWNERSHIP;
    if (start && g->stage>=NaGraphMuting) return NA_BUSY;
    g->want=!!start;
    if (!start && g->stage!=NaGraphOff) na_stream_stop(g->stream);
    return NA_OK;
}
static int finish(NaGraph *g,uint64_t now) {
    if (g->stage!=NaGraphRelease) g->endpoint_deadline=now+5000;
    g->stage=NaGraphRelease;
    int r=now>=g->endpoint_deadline ? NA_TIMEOUT:g->ops.release(g->ops.context);
    if (r==NA_PENDING) return r;
    if (r) {g->cleanup_error=r;g->stage=NaGraphFault;return r;}
    g->endpoint_owned=0;g->stage=NaGraphOff;
    g->map_possible=g->open_possible=g->start_possible=0;
    g->enqueue_deadline=0;
    return g->error;
}
static int cleanup(NaGraph *g) {
    g->enqueue_deadline=0;g->endpoint_deadline=0;
    na_stream_stop(g->stream);g->want=0;
    g->stage=NaGraphMuting;
    return NA_PENDING;
}
static int muted(NaGraph *g,uint64_t now) {
    if (!g->endpoint_deadline) g->endpoint_deadline=now+5000;
    int r=now>=g->endpoint_deadline ? NA_TIMEOUT:g->endpoint_owned ? g->ops.mute(g->ops.context) : 0;
    if (r==NA_PENDING) return r;
    if (r) g->cleanup_error=r;
    if (g->map_possible && (!g->stream->handle || !g->session->online ||
        g->stream->generation!=g->session->generation)) {
        na_stream_lost(g->stream);g->stage=NaGraphFault;return NA_OWNERSHIP;
    }
    if (g->start_possible) g->stage=NaGraphStop;
    else if (g->open_possible) g->stage=NaGraphClose;
    else if (g->stream->handle) g->stage=NaGraphUnmap;
    else return finish(g,now);
    return NA_PENDING;
}
static int send_command(NaGraph *g,uint64_t now) {
    if (now>UINT64_MAX-2000) return NA_ARGUMENT;
    if (!g->enqueue_deadline) g->enqueue_deadline=now+2000;
    if (now>=g->enqueue_deadline) return NA_TIMEOUT;
    uint8_t packet[GPR_MAX_PACKET];uint32_t n=0,op=0,rsp=GPR_OP_BASIC_RESULT,peer=AR_APM_IID;
    NaStream *s=g->stream;
    uint32_t token=na_token(g->session);if (!token) return NA_OFFLINE;
    switch (g->stage) {
    case NaGraphMap:
        op=AR_OP_MEM_MAP;rsp=AR_OP_MEM_MAP_DONE;
        n=ar_map(packet,sizeof packet,s->port,token,s->dsp_base,s->bytes);break;
    case NaGraphOpen:
    case NaGraphConfig: {
        NaBlob b=g->stage==NaGraphOpen ? g->topology.open : g->topology.config[g->config_index];
        int r=g->ops.sync(g->ops.context,b.offset,b.bytes,1);if (r) return r;
        op=g->stage==NaGraphOpen ? AR_OP_GRAPH_OPEN : AR_OP_SET_CFG;
        if (g->stage==NaGraphConfig && g->topology.config_peer[g->config_index]) peer=g->topology.config_peer[g->config_index];
        n=na_oob_packet_to(packet,s->port,peer,token,op,s->dsp_base+b.offset,s->handle,b.bytes);break;
    }
    case NaGraphPrepare:op=AR_OP_GRAPH_PREPARE;break;
    case NaGraphStart:op=AR_OP_GRAPH_START;break;
    case NaGraphStop:op=AR_OP_GRAPH_STOP;break;
    case NaGraphClose:op=AR_OP_GRAPH_CLOSE;break;
    case NaGraphUnmap:
        op=AR_OP_MEM_UNMAP;n=ar_unmap(packet,sizeof packet,s->port,token,s->handle);break;
    default:return NA_ARGUMENT;
    }
    if (!n && (op==AR_OP_GRAPH_PREPARE || op==AR_OP_GRAPH_START || op==AR_OP_GRAPH_STOP || op==AR_OP_GRAPH_CLOSE))
        n=na_subgraphs_packet(packet,s->port,token,op,g->topology.subgraphs,g->topology.subgraph_count);
    if (!n) return NA_ARGUMENT;
    int r=na_expect(g->session,token,s->port,peer,op,rsp,now,2000);
    if (r) return r;
    r=g->ops.send(g->ops.context,packet,n);
    if (r) {na_cancel_unsent(g->session,token);return r;}
    g->token=token;
    g->enqueue_deadline=0;
    if (op==AR_OP_MEM_MAP) g->map_possible=1;
    if (op==AR_OP_GRAPH_OPEN) g->open_possible=1;
    if (op==AR_OP_GRAPH_START) g->start_possible=1;
    return NA_PENDING;
}
int na_graph_poll(NaGraph *g,uint64_t now) {
    if (!g || now>UINT64_MAX-5000) return NA_ARGUMENT;
    NaStream *s=g->stream;
    if (g->stage==NaGraphFault) return g->error ? g->error : NA_OWNERSHIP;
    if (g->stage==NaGraphRelease) return finish(g,now);
    if (g->stage==NaGraphMuting) return muted(g,now);
    if (g->stage==NaGraphOff) {
        if (!g->want) return g->error;
        if (!g->session->online) return NA_OFFLINE;
        g->endpoint_owned=1;g->stage=NaGraphEndpoint;g->endpoint_deadline=now+5000;g->config_index=0;
        g->close_ack=g->unmap_ack=0;s->retiring=0;
    }
    if (g->stage==NaGraphEndpoint) {
        if (!g->want || !g->session->online) return cleanup(g);
        int r=now>=g->endpoint_deadline ? NA_TIMEOUT:g->ops.prepare(g->ops.context);
        if (r==NA_PENDING) return r;
        if (r) {g->error=r;return cleanup(g);}
        g->stage=NaGraphMap;
    }
    if (g->token) {
        uint32_t value=0;int r=na_result(g->session,g->token,&value);
        if (r==NA_PENDING) return r;
        g->token=0;
        if (r) {
            if (!g->error) g->error=r;
            if (g->stage>=NaGraphStop) {
                /* A failed STOP may still CLOSE. A failed CLOSE/UNMAP may
                 * never free or reuse a DSP-visible buffer. */
                if (g->stage==NaGraphStop) g->stage=NaGraphClose;
                else {g->cleanup_error=r;na_stream_lost(s);g->stage=NaGraphFault;return r;}
            } else return cleanup(g);
        } else switch (g->stage) {
        case NaGraphMap:
            r=na_stream_mapped(s,value,g->session->generation);
            if (r) {g->error=r;return cleanup(g);}g->stage=NaGraphOpen;break;
        case NaGraphOpen:g->stage=NaGraphConfig;break;
        case NaGraphConfig:
            if (++g->config_index==g->topology.configs) g->stage=NaGraphPrepare;break;
        case NaGraphPrepare:g->stage=NaGraphStart;break;
        case NaGraphStart:
            r=na_stream_start(s);if (r) {g->error=r;return cleanup(g);}
            g->stage=NaGraphRunning;break;
        case NaGraphStop:g->start_possible=0;g->stage=NaGraphClose;break;
        case NaGraphClose:g->close_ack=1;g->open_possible=0;g->stage=NaGraphUnmap;break;
        case NaGraphUnmap:
            g->unmap_ack=1;
            /* Mapping before OPEN failed never transferred any buffer. */
            r=na_stream_reclaim(s,g->close_ack || !g->open_possible,g->unmap_ack,0);
            if (r) {g->cleanup_error=r;g->stage=NaGraphFault;return r;}
            return finish(g,now);
        default:return NA_OWNERSHIP;
        }
    }
    if (g->stage<NaGraphStop && (!g->want || s->quarantine || !g->session->online)) return cleanup(g);
    if (g->stage==NaGraphRunning) return NA_OK;
    int r=send_command(g,now);
    if (r==NA_BUSY) return NA_PENDING; /* not queued; bounded owner deadline */
    if (r<0) {
        if (!g->error) g->error=r;
        if (g->stage>=NaGraphStop) {na_stream_lost(s);g->stage=NaGraphFault;return r;}
        return cleanup(g);
    }
    return r;
}
