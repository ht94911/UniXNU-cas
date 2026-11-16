/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host tests of the REAL C implementation. Source-only delivery: these have
 * not been compiled/run. Fixtures never access MMIO, ADB, USB or firmware. */
#include "../core/AudioEngine.h"
#include "../platform/NezhaAudioPlatform.h"
#include "../hal/AudioClock.h"
#include <assert.h>
#include <limits.h>
#include <string.h>
static void wr(uint8_t *p,uint32_t n) {p[0]=n;p[1]=n>>8;p[2]=n>>16;p[3]=n>>24;}
static uint32_t reply(uint8_t *b,uint32_t token,uint32_t port,uint32_t peer,
                       uint32_t opcode,const void *payload,uint32_t bytes) {
    GprPacket p={.src_domain=GPR_DOMAIN_ADSP,.dst_domain=GPR_DOMAIN_APPS,
        .src_port=peer,.dst_port=port,.token=token,.opcode=opcode};
    return gpr_encode(b,GPR_MAX_PACKET,&p,payload,bytes);
}
static void transactions(void) {
    NaSession s;na_session_init(&s);assert(!na_session_link(&s,1));
    uint32_t token=na_token(&s),value=0;uint8_t b[GPR_MAX_PACKET],p[8];
    assert(!na_expect(&s,token,7,AR_APM_IID,AR_OP_GRAPH_OPEN,GPR_OP_BASIC_RESULT,0,20));
    wr(p,AR_OP_GRAPH_OPEN);wr(p+4,0);
    uint32_t n=reply(b,token+1,7,AR_APM_IID,GPR_OP_BASIC_RESULT,p,8);
    assert(na_receive(&s,b,n)==NA_IGNORED);
    n=reply(b,token,8,AR_APM_IID,GPR_OP_BASIC_RESULT,p,8);
    assert(na_receive(&s,b,n)==NA_IGNORED);
    n=reply(b,token,7,99,GPR_OP_BASIC_RESULT,p,8);
    assert(na_receive(&s,b,n)==NA_IGNORED);
    n=reply(b,token,7,AR_APM_IID,GPR_OP_BASIC_RESULT,p,8);
    assert(!na_receive(&s,b,n));assert(!na_result(&s,token,&value));
    assert(na_result(&s,token,&value)==NA_MISSING);
    assert(na_expect(&s,token,7,AR_APM_IID,AR_OP_GRAPH_OPEN,GPR_OP_BASIC_RESULT,0,20)==NA_ARGUMENT);
    token=na_token(&s);
    assert(!na_expect(&s,token,7,AR_APM_IID,AR_OP_MEM_MAP,AR_OP_MEM_MAP_DONE,1,20));
    wr(p,AR_OP_MEM_MAP);n=reply(b,token,7,AR_APM_IID,GPR_OP_BASIC_RESULT,p,8);
    assert(na_receive(&s,b,n)==NA_IGNORED); /* basic OK is not a map handle */
    na_expire(&s,21);assert(na_result(&s,token,&value)==NA_TIMEOUT);
    uint32_t oldgen=s.generation;assert(!na_session_link(&s,0));assert(!na_session_link(&s,1));
    assert(s.generation!=oldgen && na_token(&s)>token);
    s.next_token=UINT32_MAX;assert(!na_token(&s));assert(s.exhausted);
    assert(na_session_link(&s,1)==NA_OVERFLOW);
}
static _Alignas(4096) uint8_t dma[65536];
static uint32_t done(uint8_t *packet,const NaStream *s,unsigned i,uint32_t off,uint32_t bytes) {
    uint8_t p[60]={0};uint32_t a=s->capture ? 4:0;
    uint64_t addr=s->dsp_base+i*s->stride;
    wr(p+a,(uint32_t)addr);wr(p+a+4,(uint32_t)(addr>>32));wr(p+a+8,s->handle);
    if (s->capture) {wr(p+16,bytes);wr(p+20,off);}
    return reply(packet,s->slot[i].token,s->port,s->endpoint,
        s->capture ? AR_OP_READ_DONE:AR_OP_WRITE_DONE,p,s->capture ? 60:32);
}
static void stream(void) {
    NaStream s;NaSession session;na_session_init(&session);na_session_link(&session,1);
    assert(!na_stream_init(&s,dma,0x110000000ULL,sizeof dma,960,2,7,42,1));
    assert(!na_stream_mapped(&s,9,session.generation));assert(!na_stream_start(&s));
    unsigned i[2];uint32_t n,got;uint8_t b[GPR_MAX_PACKET],output[960];
    for (unsigned k=0;k<2;++k) {
        assert(!na_stream_prepare(&s,&session,0,20,&i[k],b,&n));
        assert(!na_stream_commit(&s,i[k]));
    }
    memset(dma,0x35,sizeof dma);
    n=done(b,&s,i[1],4,8);assert(!na_stream_done(&s,b,n));
    assert(na_stream_take(&s,output,sizeof output,&got)==NA_PENDING);
    n=done(b,&s,i[0],2,10);assert(!na_stream_done(&s,b,n));
    assert(na_stream_done(&s,b,n)==NA_IGNORED); /* duplicate cannot decrement flight */
    assert(!na_stream_take(&s,output,sizeof output,&got) && got==10 && output[0]==0x35);
    assert(!na_stream_take(&s,output,sizeof output,&got) && got==8);
    assert(!na_stream_prepare(&s,&session,0,20,&i[0],b,&n));
    na_stream_stop(&s);assert(na_stream_commit(&s,i[0])==NA_OWNERSHIP);
    assert(!na_stream_unsent(&s,i[0]));
    assert(na_stream_reclaim(&s,1,0,0)==NA_OWNERSHIP);
    assert(!na_stream_reclaim(&s,1,1,0));
    assert(!na_stream_mapped(&s,10,session.generation));assert(!na_stream_start(&s));
    assert(!na_stream_prepare(&s,&session,0,20,&i[0],b,&n));assert(!na_stream_commit(&s,i[0]));
    n=done(b,&s,i[0],958,4);assert(na_stream_done(&s,b,n)==NA_WIRE && s.quarantine);
    assert(na_stream_reclaim(&s,0,0,0)==NA_OWNERSHIP);
    assert(!na_stream_reclaim(&s,0,0,1));
}
static void queues(void) {
    NaRxQueue q={0};NaRxPacket packet;uint8_t b[GPR_MAX_PACKET]={0};
    for (unsigned i=0;i<NA_RX_SLOTS;++i) assert(!na_rx_push(&q,1,b,GPR_HEADER_BYTES));
    assert(na_rx_push(&q,1,b,GPR_HEADER_BYTES)==NA_OVERFLOW);
    assert(na_rx_pop(&q,&packet)==NA_OVERFLOW);
    memset(&q,0,sizeof q);q.head=q.tail=UINT32_MAX;
    assert(!na_rx_push(&q,2,b,GPR_HEADER_BYTES));assert(!na_rx_pop(&q,&packet));
    assert(packet.generation==2 && na_rx_pop(&q,&packet)==NA_PENDING);
}
typedef struct {unsigned send,prepare,mute,release;int busy;uint8_t last[GPR_MAX_PACKET];uint32_t length;} Fake;
static int send(void *v,const uint8_t *p,uint32_t n) {Fake *f=v;if(f->busy)return NA_BUSY;++f->send;memcpy(f->last,p,n);f->length=n;return 0;}
static int sync_dma(void *v,uint32_t o,uint32_t b,int d) {(void)v;(void)o;(void)b;(void)d;return 0;}
static int prepare(void *v) {++((Fake *)v)->prepare;return 0;}
static int mute(void *v) {++((Fake *)v)->mute;return 0;}
static int release(void *v) {++((Fake *)v)->release;return 0;}
static void graph(void) {
    NaSession session;NaStream s;NaGraph g;Fake f={0};
    NaGraphOps ops={&f,send,sync_dma,prepare,mute,release};
    NaTopology t={.open={32768,8},.config={{32776,8}},.configs=1,.subgraphs={0x4001},.subgraph_count=1};
    na_session_init(&session);na_session_link(&session,1);
    assert(!na_stream_init(&s,dma,0x110000000ULL,sizeof dma,1920,4,7,42,0));
    assert(!na_graph_init(&g,&session,&s,&t,&ops));
    assert(!na_graph_request(&g,0) && !s.retiring);
    assert(!na_graph_request(&g,1));f.busy=1;
    assert(na_graph_poll(&g,1)==NA_PENDING);assert(f.send==0);
    assert(na_graph_poll(&g,2001)==NA_PENDING && g.stage==NaGraphMuting);
    assert(na_graph_poll(&g,2002)==NA_TIMEOUT);assert(f.release==1 && !s.quarantine);
    /* Once MAP was queued, timeout without a handle must keep DMA pinned. */
    memset(&f,0,sizeof f);memset(&g,0,sizeof g);
    assert(!na_graph_init(&g,&session,&s,&t,&ops));assert(!na_graph_request(&g,1));
    assert(na_graph_poll(&g,3000)==NA_PENDING && f.send==1);
    na_expire(&session,5000);assert(na_graph_poll(&g,5000)==NA_PENDING);
    assert(na_graph_poll(&g,5001)==NA_OWNERSHIP);
    assert(g.stage==NaGraphFault && s.quarantine && f.release==0);
}
static void resources(void) {
    uint64_t encoded=0;NaRange mem={0x180000000ULL,65536};NaSid sid={0x1001,0x80};
    assert(!na_dma_contract(mem,0x10000000,sid,0,0,0,0,&encoded) && encoded==0x110000000ULL);
    assert(na_dma_contract(mem,0xfffff000,sid,0,0,0,0,&encoded)==NA_ARGUMENT);
    assert(na_dma_contract(mem,0x10000000,sid,&mem,1,0,0,&encoded)==NA_OWNERSHIP);
    NaSid other={0x1081,0};
    assert(na_sid_overlap(sid,other));
    assert(na_dma_contract(mem,0x10000000,sid,0,0,&other,1,&encoded)==NA_OWNERSHIP);
    assert(na_ranges_overlap((NaRange){UINT64_MAX,1},mem));
    assert(na_nezha_facts.codec_reset==161); /* USBSS gpio162 must never be used */
}
static void clock_contract(void) {
    uint64_t q,t,f;
    assert(!na_clock_contract(625,12,19200000,&q) && q==(400ULL<<32));
    assert(na_clock_contract(625,12,24000000,&q)==NA_MISSING);
    assert(!na_clock_host(10,48000,q,&t) && t==19200010);
    assert(!na_clock_frames(10,t,q,&f) && f==48000);
    assert(na_clock_frames(10,9,q,&f)==NA_ARGUMENT);
    assert(na_clock_host(UINT64_MAX,1,q,&t)==NA_OVERFLOW);
    assert(!na_clock_contract(125,3,24000000,&q) && q==(500ULL<<32));
}
int main(void) {transactions();stream();queues();graph();resources();clock_contract();return 0;}
