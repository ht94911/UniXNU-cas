/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_ADSP_LINK_H
#define NEZHA_ADSP_LINK_H
#include "AdspFirmware.h"
#include "../core/AudioTransport.h"
#include "../upstream/QcomSmem.h"
#include "../upstream/QcomSmp2p.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    void *context;
    int (*smem_lock)(void *);void (*smem_unlock)(void *);
    void (*doorbell)(void *,uint32_t signal); /* shared IPCC send, LPASS client3 */
    /* Acquire LP0 bandwidth, load_state, LCX/LMX in the SM8850 sequence.
     * off after real handover removes only OUR votes; errors are not success. */
    int (*proxy)(void *,int on);
    /* Called only after BOTH PAS shutdowns succeeded, before any firmware
     * authentication. Install the assigned DMA translations here, never in
     * the pre-worker initializer. Pending retains the same setup operation;
     * an error is terminal and cannot cause PAS or SMMU replay. */
    int (*dma_setup)(void *);
} NaAdspOps;
enum {NaAdspCold,NaAdspStopMain,NaAdspStopDtb,NaAdspDma,NaAdspSetup,NaAdspPower,
      NaAdspDtb,NaAdspMain,NaAdspWaitReady,NaAdspOnline,NaAdspFailed};
typedef struct {
    NaAdspOps ops;QcomSmem smem;QcomSmemPartition partition;QcomSmp2p p2p;
    QcomGlink glink;NaTransport transport;NaRxQueue *rx;
    NaPasLoad *main_image,*dtb_image;
    uint32_t stage,doorbells,auth_doorbells,slave,handed,proxy_possible,glink_initialized;
    uint32_t masters_stopped,dma_ready;
    uint64_t deadline;int error,cleanup_error;
} NaAdsp;
/* Common platform must own ADSP PAS1/36, SMEM apps-2, LPASS doorbell signals
 * 0/2 and firmware carveouts. Never attach to a bootloader's live DSP session. */
int na_adsp_init(NaAdsp *,volatile void *smem,uint32_t bytes,NaPasLoad *main_image,NaPasLoad *dtb_image,
                  NaRxQueue *,const NaAdspOps *);
/* Original shared IPCC interrupt dispatcher forwards only LPASS signal2 here.
 * It must not introduce a second reader clearing the global RECV_ID FIFO. */
void na_adsp_smp2p_event(NaAdsp *);
/* Sole ADSP worker. A timer also polls: no unbounded wait, no lost-wake dependency. */
int na_adsp_poll(NaAdsp *,uint64_t now_ms);
#ifdef __cplusplus
}
#endif
#endif
