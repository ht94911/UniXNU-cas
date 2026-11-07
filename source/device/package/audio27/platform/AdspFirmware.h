/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_ADSP_FIRMWARE_H
#define NEZHA_ADSP_FIRMWARE_H
#include "NezhaAudioPlatform.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const uint8_t *data;uint64_t bytes;
} NaFirmwarePiece;
typedef struct {
    NaFirmwarePiece piece[QCOM_MDT_MAX_SEGMENTS];QcomMdtSplit split;QcomMdtImage image;
} NaSplitFirmware;
/* Pinned bytes from the private source input provider, not user memory or a
 * raw partition fallback. Verify file SHA in the selected build's input lock. */
int na_split_firmware(NaSplitFirmware *,const NaFirmwarePiece *,uint32_t count);
int na_split_read(void *,uint64_t offset,void *to,size_t bytes);
typedef struct {
    void *context;
    /* Shared NezhaScm PAS wrapper: same lock + WAITQ owner as GPU/Wi-Fi.
     * command {1 init-image,2 mem-setup,5 auth/reset,6 shutdown}; PAS {1,36}.
     * Failure may have changed secure state: never automatically replay. */
    int (*pas)(void *,uint32_t command,uint32_t pas,uint64_t arg1,uint64_t arg2);
    int (*sync)(void *,void *cpu,uint64_t physical,uint64_t bytes);
} NaPasOps;
enum {NaPasIdle,NaPasInit,NaPasMemory,NaPasCopy,NaPasAuthenticate,NaPasReady,NaPasFault};
typedef struct {
    NaSplitFirmware *firmware;NaPasOps ops;
    NaRange region;uint8_t *mapped,*metadata;
    uint64_t metadata_pa,metadata_capacity,span,offset,deadline;
    uint32_t pas,stage,segment,possible;int error;
} NaPasLoad;
/* Begin prepares only private metadata; it does not touch the firmware region.
 * AdspLink proves both PAS masters stopped and installs DMA before poll.
 * Both images prevalidate all segments and metadata before any secure call.
 * Metadata must be contiguous and remains retained through failure/quiescence. */
int na_pas_begin(NaPasLoad *,NaSplitFirmware *,uint32_t pas,NaRange,void *mapped,
                 void *metadata,uint64_t metadata_pa,uint64_t metadata_capacity,const NaPasOps *,uint64_t now_ms);
/* Bounded 64KiB copy per iteration; no synchronous 85MiB memcpy on audio worker. */
int na_pas_poll(NaPasLoad *,uint64_t now_ms);
#ifdef __cplusplus
}
#endif
#endif
