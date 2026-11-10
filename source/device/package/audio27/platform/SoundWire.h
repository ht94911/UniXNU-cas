/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_SOUNDWIRE_H
#define NEZHA_AUDIO_SOUNDWIRE_H
#include "NezhaAudioPlatform.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    void *context;
    uint32_t (*read)(void *,uint32_t reg);
    void (*write)(void *,uint32_t reg,uint32_t value);
    uint64_t (*now_us)(void *);void (*pause_us)(void *,uint32_t);
    int (*error)(void *); /* latched MMIO/lease error, including void writes */
} NaSwrIo;
typedef struct {
    NaSwrIo io;uint64_t lease;uint8_t next_id,device;
    uint32_t failed,reads,writes,last_status;
} NaSwr;
/* Requires the exclusive, clocked TX v2 master at0x07630000 and actual slave
 * identity, not 'first enumerated device'. The caller performs master startup. */
/* Stock DT2.0 layout with the upstream2.0 or actual nezha2.2 hardware ID. */
int na_swr_version_supported(uint32_t hardware_version);
int na_swr_bind(NaSwr *,const NaSwrIo *,uint64_t lease);
int na_swr_read(NaSwr *,uint16_t reg,uint8_t *);
int na_swr_write(NaSwr *,uint16_t reg,uint8_t value);
int na_swr_update(NaSwr *,uint16_t reg,uint8_t mask,uint8_t value);
#ifdef __cplusplus
}
#endif
#endif
