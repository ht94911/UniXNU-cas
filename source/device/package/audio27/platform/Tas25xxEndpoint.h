/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_TAS25XX_ENDPOINT_H
#define NEZHA_TAS25XX_ENDPOINT_H
#include "Tas25xxTables.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NA_TAS_STEPS 64u
enum {NaTasOff,NaTasPrepared,NaTasActive};
enum {NaTasSpeaker,NaTasReceiver,NaTasRoutes};
typedef struct {NaTasBytes bytes;uint8_t channel,check;} NaTasStep;
typedef struct {
    /* Entire plan and input remain private and pinned for the endpoint life.
     * Identity is an observed register 0x78 revision, not tuning device_type.
     * Zero mask is forbidden; unknown revision cannot silently enable audio. */
    uint8_t revision[2],revision_mask[2];
    uint32_t dvc_index[2];
    const char *profile,*rx_format,*tx_format;
    uint32_t calibrated; /* caller verified exact per-device ACDB/calibration */
} NaTasSelection;
typedef struct {
    void *context;
    /* Common exclusive GPIO/clock/power owner. reset() is available even when
     * GENI failed; asserting both audio reset pins is the mute fallback. */
    int (*claim)(void *,uint64_t *lease);
    int (*power)(void *,uint64_t lease,int on);
    int (*reset)(void *,uint64_t lease,int asserted);
    int (*release)(void *,uint64_t lease);
    NaTasIo io;
    const uint64_t *trace_wait_us;
    /* Stock GPIO174 selector. Caller has muted both amps before switching. */
    int (*route)(void *,uint64_t lease,int speaker);
} NaTasPowerOps;
/* Reset-muted identity read used by the DSP diagnostic and playback startup.
 * It never executes tuning or starts a stream. All GPIO/clock restoration
 * finishes before a new playback owner can claim the board. */
typedef struct {
    NaTasPowerOps ops;
    uint64_t lease,deadline,wait_until;
    uint32_t initialized,phase,claimed,power_attempted,claim_failed,cleanup_attempted;
    uint8_t revision[2];
    int error,cleanup_error;
} NaTasIdentity;
int na_tas_identity_init(NaTasIdentity *,const NaTasPowerOps *);
int na_tas_identity_poll(NaTasIdentity *,uint64_t now_ms);
int na_tas_identity_stop(NaTasIdentity *);
typedef struct {
    NaTasPowerOps ops;NaTasSelection selected;
    const NaTasImage *image;
    NaTasStep steps[3][NA_TAS_STEPS];uint32_t count[3];
    uint8_t integers[2][17];NaTasFaultTable faults[2];
    NaTasRun run;
    uint64_t lease,deadline,wait_until,next_health,trace_base_us;
    uint32_t state,target,phase,index,changing,powered,asserted,identified,claim_failed;
    uint8_t observed_revision[2];uint32_t reported_faults[2];
    NaTasStep receiver_steps[3][NA_TAS_STEPS];uint32_t receiver_count[3];
    uint32_t route,receiver_ready;
    int error,cleanup_error;
} NaTasEndpoint;
int na_tas_endpoint_init(NaTasEndpoint *,const NaTasImage *,const NaTasSelection *,const NaTasPowerOps *);
/* Resolve the same private tuning's voice profile once, before worker start.
 * Receiver keeps the existing two-slot TDM/VI clock, powers only the top amp,
 * and uses the stock RCV0 selector. No synthetic tuning or new board owner. */
int na_tas_endpoint_receiver(NaTasEndpoint *,const NaTasImage *);
/* Worker only, after mute has completed. Neither frees nor reclaims its lease. */
int na_tas_endpoint_route(NaTasEndpoint *,uint32_t route);
/* prepare(), activate(), mute() are repeated by graph/engine on NA_PENDING.
 * poll owns sequencing; request never replays a failed register operation. */
int na_tas_endpoint_request(NaTasEndpoint *,uint32_t target);
int na_tas_endpoint_poll(NaTasEndpoint *,uint64_t now_ms);
/* Only call release after graph CLOSE+UNMAP (or proved PAS+DMA revocation).
 * Hardware mute and reset can happen before DSP quiescence; resource release
 * cannot. A failed claim/release keeps its lease quarantined. */
int na_tas_endpoint_release(NaTasEndpoint *,int dma_quiescent);
#ifdef __cplusplus
}
#endif
#endif
