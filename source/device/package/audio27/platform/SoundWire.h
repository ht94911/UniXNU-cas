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
/* Stock TX MCLK is9.6MHz; a50x16 frame needs6 SSPs for the4kHz sync.
 * ADC_LP4.8MHz has interval (2*9.6/4.8)-1, independently of PCM48kHz. */
enum {NA_SWR_TX_FRAME_SHAPE=15,NA_SWR_TX_FRAME_CTRL=(5u<<16)|15u,
      NA_SWR_TX_ADC_LP_INTERVAL=3};
/* Complete the stock broadcast bank-switch command and confirm the active
 * bank before a capture graph can submit DMA. No paging-device registers. */
int na_swr_select_tx_bank(NaSwr *,unsigned bank);
#ifdef __cplusplus
}
#endif
#endif
