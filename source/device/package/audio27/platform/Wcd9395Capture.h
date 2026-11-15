/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NEZHA_WCD9395_CAPTURE_H
#define NEZHA_WCD9395_CAPTURE_H
#include "SoundWire.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    void *context;uint32_t (*read_tx)(void *,uint32_t);void (*write_tx)(void *,uint32_t,uint32_t);
    int (*error)(void *);
} NaTxIo;
#define NA_MIC_CAPTURE_REG_WORDS 39u
typedef struct {
    NaSwr *bus;NaTxIo tx;uint32_t stage,index,muted,prepared,active;
    uint64_t wait_until,deadline;uint8_t micbias,adc_gain,version;
    const uint64_t *trace_wait_us;
    uint64_t trace_base_us;
    int error;
    /* Existing MIC console records copy each phase once. Values are captured
     * while its original TX/SWR lease is powered, before stop changes them. */
    uint32_t registers[2][NA_MIC_CAPTURE_REG_WORDS],registers_done,registers_valid[2];
    int registers_error[2];
} NaWcdCapture;
/* ADC2 <- AMIC1, slave DP1 channel2 -> master DP2 channel3 -> SWR_MIC2 ->
 * TX DEC2, 48k/S16/mono. micbias is resolved from the actual board supply path;
 * no default assumption from sheng's AMIC3. All related clocks/rails are held
 * before this call, using the selected package's exclusive audio lease. */
int na_wcd_capture_init(NaWcdCapture *,NaSwr *,const NaTxIo *,uint8_t micbias,uint8_t adc_gain);
int na_wcd_capture_prepare(NaWcdCapture *,uint64_t now_ms);
int na_wcd_capture_activate(NaWcdCapture *,uint64_t now_ms);
int na_wcd_capture_mute(NaWcdCapture *);
/* Graph/DMA must quiesce before shutdown; power/resource release comes last. */
int na_wcd_capture_shutdown(NaWcdCapture *,int dma_quiescent);
#ifdef __cplusplus
}
#endif
#endif
