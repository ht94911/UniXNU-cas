/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_PLATFORM_H
#define NEZHA_AUDIO_PLATFORM_H
#include "../core/AudioProtocol.h"
#include "../upstream/QcomMdt.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint64_t base,bytes; } NaRange;
typedef struct { uint16_t id,mask; } NaSid;
/* Descriptions from the saved nezha DTS. They confer no ownership: the chosen
 * package must reserve/lease every resource before any hardware operation. */
typedef struct {
    NaRange firmware,dtb;
    uint64_t amp_i2c,tx_macro,va_macro,swr_tx;
    uint32_t amp_address[2],amp_reset[2],amp_irq,codec_reset;
    uint32_t codec_tx_address[2],micbias_mv;
    NaSid sid[2];
} NaBoardFacts;
extern const NaBoardFacts na_nezha_facts;
int na_range_valid(NaRange);
int na_ranges_overlap(NaRange,NaRange);
int na_sid_overlap(NaSid,NaSid);
/* A supplied audio-owned IOVA and exclusive physical allocation. Reject all
 * overlaps with the selected package's reservations, including USB/GPU/Wi-Fi.
 * The high GPR word is SID & 0xf; no physical-address truncation or bypass. */
int na_dma_contract(NaRange physical,uint32_t iova,NaSid sid,
                    const NaRange *reserved,uint32_t count,
                    const NaSid *owned_streams,uint32_t stream_count,uint64_t *encoded);
/* Verify every ELF segment against the exact firmware carveout before PAS.
 * Parsing is reused from sheng; binary input stays outside all source repos. */
int na_firmware_contract(const QcomMdtImage *,NaRange,uint64_t *span);

enum { NaEndpointOff,NaEndpointMuted,NaEndpointActive,NaEndpointFault };
enum { NaProofIdentity=1u,NaProofPower=2u,NaProofCalibration=4u,
       NaProofTopology=8u,NaProofClock=16u,NaProofDma=32u,
       NaProofCaptureRoute=64u,NaProofCapturePcm=128u };
typedef struct {
    uint32_t proofs,capture,channels,rate,bits;
    uint32_t device_id[2],device_count;
    /* Both limits must come from the exact amplifier calibration. Q15 gain
     * above 4096 (-18 dBFS) is prohibited in the first diagnostic path. */
    uint32_t max_gain_q15;
} NaEndpointProfile;
typedef struct {
    void *context;
    /* Every callback runs on the worker with a bounded deadline. claim returns
     * an exclusive owner lease; a failed power/configure may have side effects
     * and is always followed by mute + shutdown. No optimistic success stubs. */
    int (*claim)(void *,uint64_t *lease);
    int (*power_muted)(void *,uint64_t lease);
    int (*identify)(void *,uint64_t lease,uint32_t *ids,uint32_t count);
    int (*configure_muted)(void *,uint64_t lease,const NaEndpointProfile *);
    int (*gain)(void *,uint64_t lease,uint32_t q15);
    int (*mute)(void *,uint64_t lease,int muted);
    int (*shutdown)(void *,uint64_t lease);
    int (*release)(void *,uint64_t lease);
} NaEndpointOps;
typedef struct {
    NaEndpointProfile profile;NaEndpointOps ops;
    uint64_t lease;uint32_t state,gain_q15;int error,cleanup_error;
} NaEndpoint;
int na_endpoint_init(NaEndpoint *,const NaEndpointProfile *,const NaEndpointOps *);
int na_endpoint_prepare(NaEndpoint *);
int na_endpoint_gain(NaEndpoint *,uint32_t q15);
int na_endpoint_activate(NaEndpoint *);
int na_endpoint_mute(NaEndpoint *);
/* Only graph CLOSE/UNMAP or PAS stop + revoked DMA permits normal release. */
int na_endpoint_release(NaEndpoint *,int dma_quiescent);
int na_endpoint_input_publishable(const NaEndpoint *);
#ifdef __cplusplus
}
#endif
#endif
