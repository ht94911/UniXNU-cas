/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Deferred host tests: source only until the human selects a test package. */
#include "../core/AudioRing.h"
#include "../core/AudioEngine.h"
#include "../core/AudioPrm.h"
#include <assert.h>
#include <string.h>
static _Alignas(4096) uint8_t memory[65536];
static _Alignas(8) uint64_t ring[NA_AUDIO_RING_FRAMES];
static void rings(void) {
    int16_t l=99,r=99;
    memset(ring,0,sizeof ring);
    assert(na_ring_take(ring,10,&l,&r)==NA_MISSING && !l && !r);
    assert(na_ring_mix(ring,10,100,200)==NA_OWNERSHIP); /* missing frames tombstoned */
    assert(!na_ring_mix(ring,11,30000,-30000));assert(!na_ring_mix(ring,11,10000,-10000));
    assert(!na_ring_take(ring,11,&l,&r) && l==32767 && r== -32768);
    assert(na_ring_mix(ring,11,1,1)==NA_OWNERSHIP);
    assert(!na_ring_mix(ring,11+NA_AUDIO_RING_FRAMES,2,3));
    assert(na_ring_take(ring,11,&l,&r)==NA_MISSING);
    assert(!na_ring_take(ring,11+NA_AUDIO_RING_FRAMES,&l,&r) && l==2 && r==3);
    assert(na_ring_mix(ring,NA_AUDIO_MAX_FRAME+1,1,1)==NA_ARGUMENT);
    assert(!na_ring_capture(ring,100,-17));assert(!na_ring_read(ring,100,&l) && l== -17);
    assert(na_ring_read(ring,100+NA_AUDIO_RING_FRAMES,&l)==NA_MISSING && l==0);
}
typedef struct {unsigned busy,rendered,sent,activated,prepare_pending,release_pending;} Fake;
static int send_packet(void *v,const uint8_t *p,uint32_t n) {
    Fake *f=v;(void)p;(void)n;if (f->busy) return NA_BUSY;++f->sent;return NA_OK;
}
static int sync_buffer(void *v,uint32_t o,uint32_t n,int device) {(void)v;(void)o;(void)n;(void)device;return NA_OK;}
static int prepare(void *v) {return ((Fake *)v)->prepare_pending ? NA_PENDING:NA_OK;}
static int mute(void *v) {(void)v;return NA_OK;}
static int release(void *v) {return ((Fake *)v)->release_pending ? NA_PENDING:NA_OK;}
static int activate(void *v,int capture) {assert(!capture);++((Fake *)v)->activated;return NA_OK;}
static int render(void *v,uint64_t frame,void *pcm,uint32_t bytes) {
    Fake *f=v;(void)frame;++f->rendered;memset(pcm,(int)f->rendered,bytes);return NA_OK;
}
static void staged_pcm(void) {
    Fake f={.busy=1};NaSession session;NaStream stream;NaGraph graph;NaEngine engine;NaRxQueue rx={0};
    na_session_init(&session);na_session_link(&session,1);
    assert(!na_stream_init(&stream,memory,0x110000000ULL,sizeof memory,1920,4,7,42,0));
    assert(!na_stream_mapped(&stream,77,session.generation));assert(!na_stream_start(&stream));
    NaTopology topology={.open={32768,8},.config={{32776,8}},.configs=1,.subgraphs={0x4001},.subgraph_count=1};
    NaGraphOps go={&f,send_packet,sync_buffer,prepare,mute,release};
    assert(!na_graph_init(&graph,&session,&stream,&topology,&go));
    graph.stage=NaGraphRunning;graph.want=1;graph.endpoint_owned=1;graph.map_possible=1;
    NaEngineOps eo={&f,render,0,activate};assert(!na_engine_init(&engine,&session,&rx,&graph,0,&eo));
    assert(!na_engine_poll(&engine,10));assert(f.rendered==1 && f.sent==0 && stream.next==0 && memory[0]==1);
    f.busy=0;assert(!na_engine_poll(&engine,11));
    /* Slot0 reuses the staged bytes; it must not consume the HAL ring twice. */
    assert(f.rendered==4 && f.sent==4 && memory[0]==1 && stream.next==4 && f.activated==1);
}
static void endpoint_deadlines(void) {
    Fake f={.prepare_pending=1};NaSession session;NaStream stream;NaGraph graph;
    na_session_init(&session);na_session_link(&session,1);
    assert(!na_stream_init(&stream,memory,0x110000000ULL,sizeof memory,1920,4,7,42,0));
    NaTopology topology={.open={32768,8},.config={{32776,8}},.configs=1,.subgraphs={0x4001},.subgraph_count=1};
    NaGraphOps go={&f,send_packet,sync_buffer,prepare,mute,release};
    assert(!na_graph_init(&graph,&session,&stream,&topology,&go));assert(!na_graph_request(&graph,1));
    assert(na_graph_poll(&graph,1)==NA_PENDING);
    assert(na_graph_poll(&graph,5001)==NA_PENDING && graph.stage==NaGraphMuting);
    f.release_pending=1;
    assert(na_graph_poll(&graph,5002)==NA_PENDING && graph.stage==NaGraphRelease);
    assert(na_graph_poll(&graph,10002)==NA_TIMEOUT && graph.stage==NaGraphFault && graph.endpoint_owned);
    assert(f.sent==0);
}
typedef struct {uint64_t ready_at;unsigned attempts,active,reads;int error;} Capture;
static int capture_activate(void *v,int direction) {
    Capture *c=v;assert(direction==1 && !c->reads);++c->attempts;
    if (c->error) return c->error;
    if (c->ready_at) return NA_PENDING;
    ++c->active;return NA_OK;
}
static int capture_send(void *v,const uint8_t *p,uint32_t n) {
    Capture *c=v;GprPacket packet;assert(!gpr_decode(p,n,&packet));
    assert(packet.opcode==AR_OP_READ && c->active==1);++c->reads;return NA_OK;
}
static int capture_take(void *v,uint64_t frame,const void *pcm,uint32_t bytes) {
    (void)v;(void)frame;(void)pcm;(void)bytes;return NA_OK;
}
static void capture_ready(Capture *c,NaEngine *e,NaGraph *g,NaStream *s,NaSession *session,NaRxQueue *rx) {
    na_session_init(session);na_session_link(session,1);memset(rx,0,sizeof *rx);
    assert(!na_stream_init(s,memory,0x110000000ULL,sizeof memory,960,2,8,43,1));
    assert(!na_stream_mapped(s,77,session->generation));assert(!na_stream_start(s));
    NaTopology t={.open={32768,8},.config={{32776,8}},.configs=1,.subgraphs={0x4001},.subgraph_count=1};
    NaGraphOps go={c,capture_send,sync_buffer,mute,mute,mute};
    assert(!na_graph_init(g,session,s,&t,&go));g->stage=NaGraphRunning;g->want=1;
    NaEngineOps eo={c,0,capture_take,capture_activate};assert(!na_engine_init(e,session,rx,0,g,&eo));
}
static void capture_activation_order(void) {
    Capture c={.ready_at=310};NaEngine e;NaGraph g;NaStream s;NaSession session;NaRxQueue rx;
    capture_ready(&c,&e,&g,&s,&session,&rx);
    assert(!na_engine_poll(&e,10) && !s.next && !s.in_flight && !c.reads);
    assert(!na_engine_poll(&e,309) && !s.next && !c.reads && c.attempts==2);
    c.ready_at=0;assert(!na_engine_poll(&e,310));
    assert(c.active==1 && c.reads==4 && s.next==4 && s.slot[0].deadline_ms==2310);
    assert(!na_engine_poll(&e,311) && c.attempts==3 && c.reads==4);
    c=(Capture){.error=NA_IO};capture_ready(&c,&e,&g,&s,&session,&rx);
    assert(na_engine_poll(&e,10)==NA_IO && g.error==NA_IO && !s.next && !c.reads);
    c=(Capture){.ready_at=1};capture_ready(&c,&e,&g,&s,&session,&rx);
    assert(!na_engine_poll(&e,10));
    assert(na_engine_poll(&e,5010)==NA_TIMEOUT && g.error==NA_TIMEOUT && !s.next && !c.reads);
}
int main(void) {rings();staged_pcm();endpoint_deadlines();capture_activation_order();return 0;}
