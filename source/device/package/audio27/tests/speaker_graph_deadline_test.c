/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Run the production driver, TAS endpoint and output/VI graph waits with the
 * retained input pack. Transport, register I/O and elapsed time are synthetic. */
#define main tas_endpoint_deadline_checks
#include "tas_trace_deadline_test.c"
#undef main
#include "../core/AudioDriver.h"
#include "../core/AudioTopology.h"

typedef struct {Peer bus;unsigned maps[3];} GraphPeer;
static _Alignas(4096) uint8_t graph_dma[2][65536];
static _Alignas(8) uint64_t output_ring[NA_AUDIO_RING_FRAMES];
static NaAudioStatus shared;
static uint64_t host_ticks(void *v) {return ((GraphPeer *)v)->bus.now_us*192/10;}
static int sync_buffer(void *v,unsigned direction,uint32_t at,uint32_t n,int device) {
    (void)v;(void)direction;(void)at;(void)n;(void)device;return NA_OK;
}
static int dma_healthy(void *v) {(void)v;return NA_OK;}
static int observe_map(void *v,const uint8_t *packet,uint32_t bytes) {
    NaDriverDirection *direction=v;GraphPeer *peer=direction->driver->ops.context;
    GprPacket decoded;assert(!gpr_decode(packet,bytes,&decoded));
    assert(decoded.opcode==AR_OP_MEM_MAP && (direction->direction==0 || direction->direction==2));
    ++peer->maps[direction->direction];return NA_OK;
}
static void initialize(GraphPeer *peer,NaDriver *driver,NaAdsp *adsp,NaRxQueue *rx,
                       NaTasEndpoint *endpoint,const NaPlaybackInputs *input,uint64_t wait_us) {
    memset(peer,0,sizeof *peer);peer->bus.now_us=658000000;
    peer->bus.trace_us=166619983;peer->bus.wait_per_call=wait_us;
    NaTasPowerOps io=ops(&peer->bus,1);
    assert(!na_tas_endpoint_init(endpoint,&input->tuning,&input->selection,&io));
    memset(adsp,0,sizeof *adsp);memset(rx,0,sizeof *rx);adsp->rx=rx;
    NaDriverStream streams[2]={0};
    for (unsigned i=0;i<2;++i) {
        streams[i].memory=graph_dma[i];streams[i].bytes=sizeof graph_dma[i];
        streams[i].dsp_address=0x110000000ULL+65536*i;streams[i].port=0x7000+2*i;
        streams[i].endpoint=i ? input->media[i].hardware:input->media[i].shared;
        assert(!na_topology_load(input->graph[i].data,(uint32_t)input->graph[i].bytes,
            graph_dma[i],sizeof graph_dma[i],&input->media[i],&streams[i].topology));
    }
    const NaPrmResource clock={NaPrmClock,0x200,3072000,4};
    NaDriverOps driver_ops={.context=peer,.host_time=host_ticks,.sync_dma=sync_buffer,.dma_health=dma_healthy};
    assert(!na_driver_init(driver,adsp,endpoint,&streams[0],NULL,&clock,1,1,0,
        &shared,output_ring,NULL,625,12,0,&driver_ops));
    assert(!na_driver_attach_feedback(driver,&streams[1],&input->calibration,input->protection[0]));
    na_session_link(&driver->session,1);
    driver->graph[0].ops.send=observe_map;driver->graph[2].ops.send=observe_map;
    assert(!na_graph_request(&driver->graph[0],1) && !na_graph_request(&driver->graph[2],1));
}
static unsigned progressing(const NaPlaybackInputs *input,uint64_t wait_us,int expected) {
    GraphPeer peer;NaDriver driver;NaAdsp adsp;NaRxQueue rx;NaTasEndpoint endpoint;
    initialize(&peer,&driver,&adsp,&rx,&endpoint,input,wait_us);
    for (unsigned poll=0;poll<10000 && !driver.graph[0].error &&
         (!peer.maps[0] || !peer.maps[2]);++poll) {
        driver.now_ms=peer.bus.now_us/1000;
        int r=na_engine_poll(&driver.runtime.engine,driver.now_ms);
        assert(r==NA_OK || r==NA_PENDING || r==expected);
        peer.bus.now_us+=1000;
    }
    assert(driver.graph[0].error==expected);
    if (expected) {
        assert(!peer.maps[0] && !peer.maps[2] && !driver.graph[0].map_possible &&
               !driver.graph[2].map_possible);
    } else {
        assert(peer.maps[0]==1 && peer.maps[2]==1 && endpoint.state==NaTasPrepared && !endpoint.error);
        assert(driver.graph[0].stage==NaGraphMap && driver.graph[2].stage==NaGraphMap);
        assert(driver.graph[0].map_possible && driver.graph[2].map_possible);
    }
    printf("production output/VI: wait=%llu trace=%llu elapsed=%llu writes=%u maps=%u/%u error=%d\n",
        (unsigned long long)wait_us,(unsigned long long)(peer.bus.trace_us-166619983),
        (unsigned long long)(peer.bus.now_us-658000000),peer.bus.writes,peer.maps[0],peer.maps[2],driver.graph[0].error);
    return peer.bus.writes;
}
static void actual_stall(const NaPlaybackInputs *input) {
    GraphPeer peer;NaDriver driver;NaAdsp adsp;NaRxQueue rx;NaTasEndpoint endpoint;
    initialize(&peer,&driver,&adsp,&rx,&endpoint,input,100000);
    driver.now_ms=peer.bus.now_us/1000;
    assert(na_engine_poll(&driver.runtime.engine,driver.now_ms)==NA_OK);
    peer.bus.now_us+=5000000;driver.now_ms=peer.bus.now_us/1000;
    /* Graph timeout first enters asynchronous cleanup; the retained graph
     * error is visible before the engine returns a terminal error. */
    int result=na_engine_poll(&driver.runtime.engine,driver.now_ms);
    assert(result==NA_OK || result==NA_TIMEOUT);
    assert(driver.graph[0].error==NA_TIMEOUT && !peer.maps[0] && !peer.maps[2]);
    puts("genuine hardware elapsed5s still times out before any MAP");
}
int main(int argc,char **argv) {
    assert(argc==5);FILE *file=fopen(argv[1],"rb");assert(file);
    fseek(file,0,SEEK_END);long bytes=ftell(file);assert(bytes>0 && bytes<=0x8000000);rewind(file);
    void *blob=malloc((size_t)bytes);assert(blob && fread(blob,1,(size_t)bytes,file)==(size_t)bytes);fclose(file);
    NaInputPack pack;assert(!na_input_pack(&pack,blob,(uint64_t)bytes));
    NaTasSelection selection={.revision={0x51,0x52},.revision_mask={0xff,0xff},
        .profile=argv[2],.rx_format=argv[3],.tx_format=argv[4]};
    NaPlaybackInputs input={0};assert(!na_playback_inputs(&input,&pack,&selection));
    unsigned ordinary=progressing(&input,0,NA_OK);
#ifdef EXPECT_SPEAKER_TIMEOUT
    progressing(&input,100000,NA_TIMEOUT);
#else
    assert(progressing(&input,100000,NA_OK)==ordinary);
#endif
    (void)ordinary;actual_stall(&input);free(blob);
    puts("production driver/TAS/graph checks passed; no phone, real DSP or audio samples");
    return 0;
}
