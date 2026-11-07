/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_TOPOLOGY_H
#define NEZHA_AUDIO_TOPOLOGY_H
#include "AudioGraph.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t shared,pcm[2],mfc,hardware,spr,volume;
    uint32_t capture,channels,hardware_bits; /*capture:0play,1mic,2DSP-only VI*/
} NaMediaEndpoints;
/* Read the existing PSPKGR01 graph container from the private input provider.
 * Section1 OPEN,2calibration,3hardware tag,4subgraphs,5tagged module IDs.
 * Explicit endpoint IDs must come from this ACDB's tagged graph; no sheng or
 * pandora fallback IID. No writes on parse failure. Destination is CPU-owned. */
int na_topology_load(const void *,uint32_t bytes,void *dma,uint32_t dma_bytes,
                      const NaMediaEndpoints *,NaTopology *);
int na_spf_validate(const void *,uint32_t bytes);
int na_topology_find_module(const void *open,uint32_t bytes,uint32_t module_type,uint32_t *instance);
#ifdef __cplusplus
}
#endif
#endif
