/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Execute the production board, WCD, graph and runtime paths with the existing
 * register fixture. Diagnostic delivery and hardware time advance separately.
 * EXPECT_MIC_FAILURE compiles the same experiment against unmodified046. */
#define main mic_revision_checks
#include "mic_revision_test.c"
#undef main
#include "../core/AudioRuntime.h"
typedef struct {
    Fixture bus;
    NaMicBoard board;
    NaWcdCapture codec;
    uint64_t trace_us,wait_per_io;
    unsigned initialized,sent,muted,released;
} Timed;
static void io_time(Timed *f) {
    f->trace_us+=f->wait_per_io;
    f->bus.now+=f->wait_per_io+100;
}
static int timed_claim(void *v,uint32_t kind,uint64_t *lease) {
    Timed *f=v;return claim(&f->bus,kind,lease);
}
static int timed_release(void *v,uint32_t kind,uint64_t lease) {
    Timed *f=v;return release(&f->bus,kind,lease);
}
static int timed_read(void *v,uint64_t lease,uint64_t address,uint32_t *out) {
    Timed *f=v;io_time(f);return read_register(&f->bus,lease,address,out);
}
static int timed_update(void *v,uint64_t lease,uint64_t address,uint32_t mask,uint32_t value) {
    Timed *f=v;io_time(f);return update_register(&f->bus,lease,address,mask,value);
}
static int timed_rails(void *v,uint64_t lease,int on) {
    Timed *f=v;return rails(&f->bus,lease,on);
}
static uint64_t timed_now(void *v) {return ((Timed *)v)->bus.now;}
static void timed_pause(void *v,uint32_t us) {((Timed *)v)->bus.now+=us;}
static int endpoint_prepare(void *v) {
    Timed *f=v;
    if (!f->initialized) {
        int r=na_mic_board_prepare(&f->board,&f->codec);
        if (r) return r;
        f->initialized=1;
    }
    return na_wcd_capture_prepare(&f->codec,f->bus.now/1000);
}
static int endpoint_mute(void *v) {
    Timed *f=v;++f->muted;
    return f->initialized ? na_wcd_capture_mute(&f->codec):NA_OK;
}
static int endpoint_release(void *v) {
    Timed *f=v;
    if (f->initialized) {
        int r=na_wcd_capture_shutdown(&f->codec,1);if (r) return r;
        f->initialized=0;
    }
    int r=na_mic_board_release(&f->board);if (!r) ++f->released;return r;
}
static int map_send(void *v,const uint8_t *packet,uint32_t bytes) {
    Timed *f=v;GprPacket decoded;
    assert(!gpr_decode(packet,bytes,&decoded) && decoded.opcode==AR_OP_MEM_MAP);
    ++f->sent;return NA_OK;
}
static int buffer_sync(void *v,uint32_t at,uint32_t bytes,int device) {
    (void)v;(void)at;(void)bytes;(void)device;return NA_OK;
}
static _Alignas(4096) uint8_t dma[65536];
static NaTopology topology={.open={32768,8},.config={{32776,8}},.configs=1,
    .subgraphs={42},.subgraph_count=1};
static void init_graph(Timed *f,NaGraph *g,NaSession *session,NaStream *stream,
                       uint64_t wait_per_io,int account) {
    memset(f,0,sizeof *f);f->bus.version=0x02020000;f->bus.now=1000000;
    f->bus.reset=0xffff03ff;f->bus.reset_output=2;f->wait_per_io=wait_per_io;
    NaBoardIo io={f,timed_claim,timed_release,timed_read,timed_update,timed_rails,
        timed_now,timed_pause,account ? &f->trace_us:NULL};
    assert(!na_mic_board_init(&f->board,&io,1,0));
    na_session_init(session);na_session_link(session,1);
    assert(!na_stream_init(stream,dma,0x110000000ULL,sizeof dma,960,2,8,43,1));
    NaGraphOps go={f,map_send,buffer_sync,endpoint_prepare,endpoint_mute,endpoint_release};
    assert(!na_graph_init(g,session,stream,&topology,&go));
#ifndef EXPECT_MIC_FAILURE
    if (account) g->endpoint_trace_wait_us=&f->trace_us;
#endif
    assert(!na_graph_request(g,1));
}
static unsigned run_graph(uint64_t wait_per_io,int account,int expected) {
    Timed f;NaGraph graph;NaSession session;NaStream stream;
    init_graph(&f,&graph,&session,&stream,wait_per_io,account);
    for (unsigned turn=0;turn<1000 && !f.sent && !graph.error;++turn) {
        int r=na_graph_poll(&graph,f.bus.now/1000);
        assert(r==NA_OK || r==NA_PENDING);f.bus.now+=1000;
    }
    assert(graph.error==expected);
    if (expected) {
        assert(!f.sent && graph.stage==NaGraphMuting);
        assert(na_graph_poll(&graph,f.bus.now/1000)==expected);
        assert(graph.stage==NaGraphOff && !graph.endpoint_owned && f.released==1);
    } else {
        assert(f.sent==1 && f.codec.prepared && f.board.stage==5);
        assert(graph.stage==NaGraphMap && graph.map_possible && graph.token);
        if (wait_per_io) assert(f.bus.now>graph.endpoint_deadline*1000);
    }
    printf("board/WCD/graph: account=%d trace=%llu real=%llu reads=%u writes=%u original=%d cleanup=%d\n",
        account,(unsigned long long)f.trace_us,(unsigned long long)f.bus.now,
        f.board.swr.reads,f.board.swr.writes,graph.error,graph.cleanup_error);
    return f.board.swr.writes;
}
static void real_stall(void) {
    Timed f;NaGraph graph;NaSession session;NaStream stream;
    init_graph(&f,&graph,&session,&stream,15000,1);
    assert(na_graph_poll(&graph,f.bus.now/1000)==NA_PENDING);
    f.bus.now+=5000000;
    assert(na_graph_poll(&graph,f.bus.now/1000)==NA_PENDING && graph.error==NA_TIMEOUT && !f.sent);
#ifndef EXPECT_MIC_FAILURE
    init_graph(&f,&graph,&session,&stream,15000,1);f.trace_us=1000;
    assert(na_graph_poll(&graph,f.bus.now/1000)==NA_PENDING);
    f.trace_us=0;
    assert(na_graph_poll(&graph,f.bus.now/1000)==NA_PENDING && graph.error==NA_IO && !f.sent);
#endif
}
static _Alignas(8) uint64_t out_ring[NA_AUDIO_RING_FRAMES],in_ring[NA_AUDIO_RING_FRAMES];
static _Alignas(4096) uint8_t runtime_dma[2][65536];
static int timeout_prepare(void *v) {(void)v;return NA_TIMEOUT;}
static int okay(void *v) {(void)v;return NA_OK;}
static int runtime_send(void *v,const uint8_t *p,uint32_t n) {(void)v;(void)p;(void)n;return NA_OK;}
static uint64_t host(void *v) {(void)v;return 1920000;}
static int runtime_activate(void *v,int direction) {(void)v;(void)direction;return NA_OK;}
static void original_error(void) {
    NaSession session;na_session_init(&session);na_session_link(&session,1);
    NaStream streams[2];NaGraph graphs[2];NaRuntime runtime;NaAudioStatus shared;
    NaRxQueue queue={0};NaTransport transport={0};
    NaPrm prm={.session=&session,.count=2,.state=NaPrmStable,
        .generation=session.generation,.send=runtime_send};
    NaGraphOps go={NULL,runtime_send,buffer_sync,timeout_prepare,okay,okay};
    for (unsigned i=0;i<2;++i) {
        assert(!na_stream_init(&streams[i],runtime_dma[i],0x110000000ULL+i*65536,
            65536,i ? 960:1920,i ? 2:4,7+i,42+i,i));
        assert(!na_graph_init(&graphs[i],&session,&streams[i],&topology,&go));
    }
    NaRuntimeOps ro={NULL,host,runtime_activate,okay,okay,0};
    assert(!na_runtime_init(&runtime,&session,&queue,&graphs[0],&graphs[1],&prm,&transport,
        &shared,out_ring,in_ring,625,12,1,2,0,&ro));
    runtime.capture_probe_allowed=1;runtime.service_ready=1;
    assert(!na_runtime_probe_capture(&runtime,480,100));
    prm.held=prm.possible=3; /* synthetic healthy retained output + capture vote */
    graphs[0].stage=NaGraphRunning;graphs[0].want=1;
    runtime.output_idle_ready=1;
    assert(!na_runtime_poll(&runtime,100));
    assert(graphs[1].error==NA_TIMEOUT && graphs[1].stage==NaGraphMuting && !runtime.fault);
    assert(!na_runtime_poll(&runtime,101) && runtime.probe.error==NA_TIMEOUT && !runtime.fault);
    assert(graphs[1].error==NA_TIMEOUT && !runtime.probe.frames && !runtime.capture_proven);
    printf("original endpoint=%d runtime=%d probe=%d\n",graphs[1].error,runtime.fault,runtime.probe.error);
}
int main(void) {
    original_error();
    unsigned ordinary=run_graph(0,0,NA_OK);
    (void)ordinary;
    run_graph(15000,0,NA_TIMEOUT);
#ifdef EXPECT_MIC_FAILURE
    run_graph(15000,1,NA_TIMEOUT);
#else
    assert(run_graph(15000,1,NA_OK)==ordinary);
#endif
    real_stall();
    puts("Production paths checked; register/time/GPR peers are synthetic, no capture or phone actions");
    return 0;
}
