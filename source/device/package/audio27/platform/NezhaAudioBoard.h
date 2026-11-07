/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_BOARD_H
#define NEZHA_AUDIO_BOARD_H
#include "GeniTasBus.h"
#include "Tas25xxEndpoint.h"
#include "Wcd9395Capture.h"
#ifdef __cplusplus
extern "C" {
#endif
enum {NaBoardTas=1,NaBoardCapture=2};
typedef struct {
    void *context;
    int (*claim)(void *,uint32_t set,uint64_t *lease);
    int (*release)(void *,uint32_t set,uint64_t lease);
    /* Common shared owner serializes RMW against OTHER subsystems too; the
     * mask only changes leased bits. I/O error must never be hidden. */
    int (*read)(void *,uint64_t lease,uint64_t pa,uint32_t *);
    int (*update)(void *,uint64_t lease,uint64_t pa,uint32_t mask,uint32_t value);
    /* Exact rail references from DT: L15B_E0=1.8V, B1B_E0=3.4V. The common
     * VRM owner keeps other consumers' votes and returns real completion. */
    int (*mic_rails)(void *,uint64_t lease,int on);
    uint64_t (*now_us)(void *);void (*pause_us)(void *,uint32_t);
} NaBoardIo;
typedef struct {
    NaBoardIo io;NaGeniTasBus bus;uint64_t lease;
    uint32_t error,powered,saved_pins[9],saved_vote,clock_index,saved_clock_index,saved_config,saved_scl,clocks_on,configured,saved_receiver_output;
} NaGeniBoard;
int na_geni_board_init(NaGeniBoard *,const NaBoardIo *,NaTasPowerOps *);
typedef struct {
    NaBoardIo io;NaSwr swr;uint64_t lease,deadline,wait_until;
    uint32_t stage,error,saved_lpi[4],saved_reset,rails,macro_on,snapshot_valid;
    uint8_t micbias,adc_gain;
} NaMicBoard;
int na_mic_board_init(NaMicBoard *,const NaBoardIo *,uint8_t micbias,uint8_t gain);
int na_mic_board_prepare(void *,NaWcdCapture *);
int na_mic_board_release(void *);
#ifdef __cplusplus
}
#endif
#endif
