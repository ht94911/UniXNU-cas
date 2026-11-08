/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_ADSP_LINK_H
#define NEZHA_ADSP_LINK_H
#include "AdspFirmware.h"
#include "AudioPdMapper.h"
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
    /* Existing common IPCC owner and deferred I/O errors, serviced by the
     * worker before advancing PAS/SMEM/GLINK. No second FIFO consumer. */
    int (*poll_shared)(void *);
    /* Optional bounded diagnostic notification, on the sole worker. */
    void (*observe)(void *,uint32_t point,int result);
} NaAdspOps;
enum {NaAdspRxLookup=1,NaAdspRxBound,NaAdspGlinkBegin,NaAdspGlinkReturn,
      NaAdspProxyOffBegin,NaAdspProxyOffReturn,NaAdspProxyRetained,
      NaAdspGlinkSetupBegin,NaAdspGlinkSetupReturn};
enum {NaAdspCold,NaAdspStopMain,NaAdspStopDtb,NaAdspDma,NaAdspSetup,NaAdspPower,
      NaAdspDtb,NaAdspMain,NaAdspWaitReady,NaAdspOnline,NaAdspFailed};
typedef struct {
    NaAdspOps ops;QcomSmem smem;QcomSmemPartition partition;QcomSmp2p p2p;
    QcomGlink glink;NaTransport transport;NaPdMapper mapper;NaRxQueue *rx;
    NaPasLoad *main_image,*dtb_image;
    uint32_t stage,doorbells,auth_doorbells,slave,handed,proxy_possible,glink_initialized;
    uint32_t masters_stopped,dma_ready;
    uint32_t initial_stop_mask;int initial_stop[2]; /* bit0/main,bit1/DTB actually called */
    /* Diagnostic retention never means the real handover or release occurred.
     * Default zero preserves normal proxy release; only na=2 sets retention. */
    uint32_t retain_proxy,handover_seen,observe_mask;
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
