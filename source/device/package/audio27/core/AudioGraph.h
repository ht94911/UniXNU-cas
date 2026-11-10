/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_GRAPH_H
#define NEZHA_AUDIO_GRAPH_H
#include "AudioStream.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Graph/calibration bytes come from the owner's nezha ACDB, never sheng's or
 * pandora's graph. Each offset is inside this graph's one DMA mapping. */
typedef struct { uint32_t offset,bytes; } NaBlob;
#define NA_GRAPH_CONFIGS 16u
typedef struct {
    NaBlob open,config[NA_GRAPH_CONFIGS];
    uint32_t configs,subgraphs[16],subgraph_count;
    uint32_t config_peer[NA_GRAPH_CONFIGS]; /*0=APM; shared-memory format goes to endpoint*/
} NaTopology;
typedef struct {
    void *context;
    /* 0 queued; any error MUST mean no bytes queued to GLINK. */
    int (*send)(void *,const uint8_t *,uint32_t);
    /* Synchronous, bounded, worker-only cache maintenance. */
    int (*sync)(void *,uint32_t offset,uint32_t bytes,int for_device);
    /* Request/verify exclusive endpoint resources and keep endpoint muted.
     * prepare/release may return NA_PENDING: poll again while servicing DSP. */
    int (*prepare)(void *);
    int (*mute)(void *);
    /* Called only after graph close/unmap, or before graph was ever submitted. */
    int (*release)(void *);
} NaGraphOps;
enum { NaGraphOff,NaGraphEndpoint,NaGraphMap,NaGraphOpen,NaGraphConfig,NaGraphPrepare,
       NaGraphStart,NaGraphRunning,NaGraphMuting,NaGraphStop,NaGraphClose,NaGraphUnmap,
       NaGraphRelease,NaGraphFault };
typedef struct {
    NaSession *session;NaStream *stream;NaTopology topology;NaGraphOps ops;
    uint32_t stage,token,config_index,want,endpoint_owned;
    uint32_t map_possible,open_possible,start_possible,close_ack,unmap_ack;
    uint64_t enqueue_deadline,endpoint_deadline;
    /* Optional worker-owned diagnostic time. The five-second endpoint budget
     * still counts hardware and scheduler time; DSP command deadlines do not
     * use this counter. Its owner must outlive the graph. */
    const uint64_t *endpoint_trace_wait_us;
    uint64_t endpoint_trace_base_us;
    int error,cleanup_error;
} NaGraph;
int na_graph_init(NaGraph *g,NaSession *session,NaStream *stream,
                  const NaTopology *topology,const NaGraphOps *ops);
int na_graph_request(NaGraph *g,int start);
/* Call each worker iteration after processing all received packets. No waits. */
int na_graph_poll(NaGraph *g,uint64_t now_ms);
#ifdef __cplusplus
}
#endif
#endif
