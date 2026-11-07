/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_RESOURCE_PROBE_H
#define NEZHA_AUDIO_RESOURCE_PROBE_H
#include "NezhaAudioPlatform.h"
#ifdef __cplusplus
extern "C" {
#endif
enum {NaProbeSmmu,NaProbeRsc,NaProbeGcc};
enum {NA_PROBE_STREAMS=128,NA_PROBE_BANKS=128};
typedef struct {
    void *context;
    /* Read-only, fixed controller domains. No write, power, FIFO, interrupt
     * acknowledgement or PAS operation is part of this interface. */
    int (*read32)(void *,unsigned domain,uint32_t offset,uint32_t *);
    int (*read64)(void *,unsigned domain,uint32_t offset,uint64_t *);
} NaResourceReader;
typedef struct {uint32_t smr,s2cr;} NaProbeStream;
typedef struct {uint32_t sctlr,actlr,cbar,cba2r,asid0,asid1;} NaProbeBank;
typedef struct {
    uint32_t phase,domain,offset,reads;int error;
    uint32_t id[3],stream_count,bank_count,stage2_banks,page_bytes,context_base;
    uint32_t rsc_id,rsc_config,gcc_vote,tcs[3][4];
    NaProbeStream stream[NA_PROBE_STREAMS];NaProbeBank bank[NA_PROBE_BANKS];
} NaResourceSnapshot;
/* Caller has established that UFS/input keep the apps SMMU/RSC/GCC powered,
 * and that their initial configuration has finished. Capture once to private
 * storage; publish only after completion. Retain partial data and error.
 * Even a complete, unchanged snapshot NEVER grants ownership of idle slots,
 * banks, TCSes or ASIDs. The selected platform must assign explicit leases. */
int na_resource_capture(NaResourceSnapshot *,const NaResourceReader *);
#ifdef __cplusplus
}
#endif
#endif
