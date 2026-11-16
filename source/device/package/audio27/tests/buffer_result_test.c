/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Known GPR error receipts must not turn into a misleading period timeout. */
#include "../core/AudioEngine.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static _Alignas(64) uint8_t memory[32768];
static NaSession session;
static NaStream stream;
static uint8_t packet[GPR_MAX_PACKET];
static unsigned slot;
#ifndef LEGACY_048
static unsigned cases;
#endif

static void put(uint8_t *p,uint32_t v) {
    p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);p[2]=(uint8_t)(v>>16);p[3]=(uint8_t)(v>>24);
}
static void setup(int capture) {
    na_session_init(&session);na_session_link(&session,1);
    assert(!na_stream_init(&stream,memory,0x100040000ULL,sizeof memory,
                          capture ? 960:1920,capture ? 2:4,17,29,capture));
    assert(!na_stream_mapped(&stream,0x77,session.generation));
    assert(!na_stream_start(&stream));
    uint32_t bytes=0;
    assert(!na_stream_prepare(&stream,&session,1000,2000,&slot,packet,&bytes));
    assert(bytes && !na_stream_commit(&stream,slot));
}
static uint32_t response(uint32_t opcode,uint32_t token,uint32_t peer,uint32_t port,
                         const uint8_t *payload,uint32_t bytes) {
    GprPacket p={.dst_domain=GPR_DOMAIN_APPS,.src_domain=GPR_DOMAIN_ADSP,
        .src_port=peer,.dst_port=port,.token=token,.opcode=opcode};
    uint32_t n=gpr_encode(packet,sizeof packet,&p,payload,bytes);assert(n);return n;
}
static uint32_t basic(uint32_t command,uint32_t result,uint32_t token,
                      uint32_t peer,uint32_t port,uint32_t bytes) {
    uint8_t payload[8];put(payload,command);put(payload+4,result);
    return response(GPR_OP_BASIC_RESULT,token,peer,port,payload,bytes);
}
#ifndef LEGACY_048
static uint32_t read_done(uint32_t status,uint32_t bytes) {
    uint8_t payload[60]={0};
    put(payload,status);put(payload+4,(uint32_t)stream.dsp_base);
    put(payload+8,(uint32_t)(stream.dsp_base>>32));put(payload+12,stream.handle);
    put(payload+16,bytes);
    return response(0x05001005,stream.slot[slot].token,29,17,payload,sizeof payload);
}

typedef struct {unsigned busy,sent,activations,pending;} Peer;
static int send_packet(void *v,const uint8_t *data,uint32_t bytes) {
    Peer *p=v;GprPacket parsed;assert(!gpr_decode(data,bytes,&parsed));
    assert(parsed.dst_domain==GPR_DOMAIN_ADSP && parsed.src_domain==GPR_DOMAIN_APPS);
    assert(parsed.opcode==0x0400100B);
    if (p->busy) return NA_BUSY;
    ++p->sent;return NA_OK;
}
static int sync_memory(void *v,uint32_t offset,uint32_t bytes,int for_device) {
    (void)v;assert(offset<sizeof memory && bytes<=sizeof memory-offset && for_device);
    return NA_OK;
}
static int endpoint(void *v) {(void)v;return NA_OK;}
static int activate(void *v,int direction) {
    Peer *p=v;assert(direction==1);++p->activations;return p->pending ? NA_PENDING:NA_OK;
}
static int capture(void *v,uint64_t frame,const void *pcm,uint32_t bytes) {
    (void)v;(void)frame;(void)pcm;(void)bytes;assert(0);return NA_IO;
}
static void engine_counts(void) {
    setup(1);
    assert(!na_stream_unsent(&stream,slot));
    NaGraph graph={.session=&session,.stream=&stream,.stage=NaGraphRunning,.want=1};
    Peer peer={.busy=1,.pending=1};
    graph.ops=(NaGraphOps){&peer,send_packet,sync_memory,endpoint,endpoint,endpoint};
    NaEngine engine;NaRxQueue queue={0};NaEngineOps ops={&peer,NULL,capture,activate};
    assert(!na_engine_init(&engine,&session,&queue,NULL,&graph,&ops));
    assert(!na_engine_poll(&engine,1000));
    assert(!peer.sent && !engine.periods_submitted[1] && !stream.in_flight);
    peer.busy=0;assert(!na_engine_poll(&engine,1001));
    assert(!peer.sent && !engine.periods_submitted[1] && !stream.in_flight);
    assert(!engine.activation_completed[1]);
    peer.pending=0;assert(!na_engine_poll(&engine,1002));
    assert(engine.activation_completed[1]==1 && engine.periods_submitted[1]==4);
    assert(peer.sent==4 && stream.in_flight==4 && stream.slot[0].deadline_ms==3002);
    assert(!na_engine_poll(&engine,1003));
    assert(engine.activation_completed[1]==1 && peer.sent==4);++cases;
}
#endif

int main(void) {
    setup(1);
    uint32_t n=basic(0x0400100B,4,stream.slot[slot].token,29,17,8);
#ifdef LEGACY_048
    assert(na_stream_done(&stream,packet,n)==NA_IGNORED && !stream.quarantine);
    na_stream_expire(&stream,3000);assert(stream.quarantine);
    puts("Original048 ignores an owned READ error receipt and reaches the period timeout");
    return 0;
#else
    assert(na_stream_done(&stream,packet,n)==NA_REMOTE);
    assert(stream.quarantine && !stream.running && stream.in_flight==1);
    assert(stream.remote_errors==1 && stream.last_remote_status==4);
    n=basic(0x0400100B,2,stream.slot[slot].token,29,17,8);
    assert(na_stream_done(&stream,packet,n)==NA_REMOTE && stream.last_remote_status==4);
    assert(na_stream_reclaim(&stream,1,0,0)==NA_OWNERSHIP);
    assert(!na_stream_reclaim(&stream,1,1,0));
    assert(stream.remote_errors==2 && stream.last_remote_status==4 && !stream.handle);++cases;

    setup(1);n=basic(0x0400100B,0,stream.slot[slot].token,29,17,8);
    assert(na_stream_done(&stream,packet,n)==NA_IGNORED);
    assert(stream.in_flight==1 && !stream.completions && !stream.quarantine);
    na_stream_expire(&stream,3000);assert(stream.quarantine);++cases;

    setup(1);n=basic(0x0400100B,4,stream.slot[slot].token+100,29,17,8);
    assert(na_stream_done(&stream,packet,n)==NA_IGNORED && !stream.bad_done && !stream.remote_errors);++cases;
    n=basic(0x0400100B,4,stream.slot[slot].token,30,17,8);
    assert(na_stream_done(&stream,packet,n)==NA_IGNORED && !stream.remote_errors);++cases;
    n=basic(0x0400100B,4,stream.slot[slot].token,29,18,8);
    assert(na_stream_done(&stream,packet,n)==NA_IGNORED && !stream.remote_errors);++cases;
    n=basic(0x01001000,4,stream.slot[slot].token,29,17,8);
    assert(na_stream_done(&stream,packet,n)==NA_IGNORED && !stream.remote_errors);++cases;
    ++stream.generation;n=basic(0x0400100B,4,stream.slot[slot].token,29,17,8);
    assert(na_stream_done(&stream,packet,n)==NA_IGNORED && !stream.quarantine);++cases;

    setup(1);n=basic(0x0400100B,4,stream.slot[slot].token,29,17,4);
    assert(na_stream_done(&stream,packet,n)==NA_WIRE && stream.bad_done==1 && stream.quarantine);++cases;

    setup(0);n=basic(AR_OP_WRITE,6,stream.slot[slot].token,29,17,8);
    assert(na_stream_done(&stream,packet,n)==NA_REMOTE && stream.last_remote_status==6);++cases;

    setup(1);n=read_done(0,960);
    assert(!na_stream_done(&stream,packet,n) && stream.completions==1 && !stream.in_flight);
    int16_t pcm[480];uint32_t bytes=0;assert(!na_stream_take(&stream,pcm,sizeof pcm,&bytes) && bytes==960);
    assert(na_stream_done(&stream,packet,n)==NA_IGNORED && stream.completions==1);
    na_stream_stop(&stream);assert(!na_stream_reclaim(&stream,1,1,0) && stream.completions==1);++cases;
    setup(1);n=read_done(0,0);
    assert(!na_stream_done(&stream,packet,n) && stream.completions==1 && !stream.frames);++cases;
    setup(1);n=read_done(9,0);
    assert(na_stream_done(&stream,packet,n)==NA_REMOTE && stream.last_remote_status==9);++cases;

    engine_counts();
    printf("%u buffer receipt/ownership/normal completion/submission/activation scenarios pass; DSP peers modeled\n",cases);
    return 0;
#endif
}
