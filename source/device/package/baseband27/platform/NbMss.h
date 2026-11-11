/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The modem subsystem (MSS) lifecycle on nezha: cold check, memory assignment, SMEM/SMP2P/GLINK setup, proxy votes, DTB and
 * main image through PAS, ready/handover, crash and watchdog, and an orderly stop.
 *
 * Sequence: Linux qcom_q6v5_pas.c sm8750_mpss_resource (the closest mainline entry to the device's "qcom,canoe-modem-pas")
 * and the device tree read from the phone on 2026-11-07:
 *   remoteproc-mss@04080000, PAS 4, DTB PAS 0x26, firmware "modem.mdt" + "modem_dtb.mdt", QMP load_state "modem",
 *   cx-supply (RPMh level 384) + mx-supply (level 320, cx-uV-uA/mx-uV-uA; the mx-supply phandle is the rpmh msslvl rail, ARC
 *   resource "mss.lvl", not mx.lvl), interconnects rproc_ddr + crypto_ddr, xo clock;
 *   memory-region: mpss 0x8aa00000/0xe100000, dtb 0x99500000/0x80000, linux,cma (dynamic), DSM1 0x85000000/0x4f00000,
 *   DSM2 0x89f00000/0x100000; the two DSM regions are assigned to the MSS_MSA VM (region_assign_idx/count);
 *   interrupts: wdog GIC SPI 264 rising, SMP2P "slave-kernel" bit0 fatal, bit1 ready, bit2 handover, bit3 stop-ack,
 *   bit7 shutdown-ack; smem-state "stop" = "master-kernel" bit0;
 *   qcom,smp2p-modem: qcom,smem = <435 428>, which smp2p.ko reads as inbound 435, outbound 428 (its probe stores the pair
 *   at smem_items[0..1]; alloc_outbound_item allocates [1], the interrupt reads [0]; the ADSP edge <443 429> is used the same
 *   way by audio27 and the SOCCP edge by power27), local pid 0, remote pid 1, IPCC client 2 signal 2;
 *   glink-edge: remote pid 1, IPCC client 2 signal 0, channels "IPCRTR" (intents 0x800x5, 0x2000x3, 0x4400x2) and "DS".
 * Structure: audio27 platform/AdspLink.c (sheng SMEM/SMP2P/GLINK lifecycle with the SM8850 PAS and proxy sequence).
 *
 * Ownership rules shared with the other subsystems: the SMEM hardware mutex, the single IPCC dispatcher (only modem
 * client 2 signals 0 and 2 come here), the NezhaScm lock, the RSC/QMP lease and the firmware carveouts belong to the common
 * platform owners; this file only asks them through ops. A failure after any secure call leaves PAS, the carveouts and the
 * assignment owned: a generic error is never proof that the modem stopped doing DMA. */
#ifndef NB_MSS_H
#define NB_MSS_H
#include "NbPas.h"
#include "../upstream/QcomGlink.h"
#include "../upstream/QcomSmem.h"
#include "../upstream/QcomSmp2p.h"
#ifdef __cplusplus
extern "C" {
#endif

#define NB_MSS_PAS 4u
#define NB_MSS_DTB_PAS 0x26u
#define NB_MSS_SMEM_HOST 1u
#define NB_MSS_SMP2P_OUT 428u
#define NB_MSS_SMP2P_IN 435u
#define NB_MSS_GLINK_DESC 478u
#define NB_MSS_GLINK_TX 479u
#define NB_MSS_GLINK_RX 480u
#define NB_MSS_GLINK_TX_BYTES 0x4000u
#define NB_MSS_CRASH_REASON 421u
#define NB_MSS_IPCC_GLINK 0u
#define NB_MSS_IPCC_SMP2P 2u

enum {
    NB_MSS_BIT_FATAL = 1u << 0, NB_MSS_BIT_READY = 1u << 1, NB_MSS_BIT_HANDOVER = 1u << 2,
    NB_MSS_BIT_STOP_ACK = 1u << 3, NB_MSS_BIT_SHUTDOWN_ACK = 1u << 7,
};

typedef struct {
    void *context;
    int (*smem_lock)(void *context);
    void (*smem_unlock)(void *context);
    /* Shared IPCC send to modem client 2 with the given signal (0 GLINK, 2 SMP2P). */
    void (*doorbell)(void *context, uint32_t signal);
    /* on: xo, cx/mx levels, rproc_ddr/crypto_ddr bandwidth and QMP load_state "modem" on, in that order. off: removes only
     * our votes, after handover or after a proven stop. NB_PENDING while a lease or the AOP is busy. */
    int (*proxy)(void *context, int on);
    /* on: hypervisor assignment of DSM1 and DSM2 from HLOS to MSS_MSA. off: back to HLOS, only after a successful PAS
     * shutdown. NB_PENDING keeps the same request; an error is terminal (no replay of a half-done assignment). */
    int (*assign)(void *context, int on);
    void (*trace)(void *context, const char *stage, int result);
} NbMssOps;

enum {
    NbMssCold, NbMssCheck, NbMssAssign, NbMssSetup, NbMssPower, NbMssDtb, NbMssMain, NbMssWaitReady, NbMssOnline,
    NbMssStopRequest, NbMssStopWait, NbMssShutdownMain, NbMssShutdownDtb, NbMssUnassign, NbMssUnvote, NbMssStopped,
    NbMssCrashed, NbMssFailed,
};

typedef struct {
    NbMssOps ops;
    QcomSmem smem;
    QcomSmemPartition partition;
    QcomSmp2p p2p;
    QcomGlink glink;
    NbPas *main_image, *dtb_image;
    int stop_entry, ipa_entry, mailbox_entry;  /* outbound SMP2P entry indices */
    uint32_t stage, slave, ipa_in, ipa_in_changed;
    uint32_t doorbells, auth_doorbells, wdog_events;
    uint8_t assigned, voted, handed, glink_ready, glink_rx_attached, decrypt_retries, glink_polled;
    uint64_t deadline, retry_at;
    int error, cleanup_error;
    char crash_reason[96];
} NbMss;

/* Both loaders must already be begun (nb_pas_begin) for PAS 4 and 0x26 against the mpss and dtb carveouts. Nothing touches
 * hardware here. smem is the 2 MiB SMEM mapping. */
int nb_mss_init(NbMss *m, volatile void *smem, uint32_t smem_bytes, NbPas *main_image, NbPas *dtb_image, const NbMssOps *ops);
/* From the IPCC dispatcher for client 2 signal 2 (SMP2P). Only counts: all work happens in nb_mss_poll. */
void nb_mss_smp2p_event(NbMss *m);
/* From the modem watchdog interrupt (GIC SPI 264). */
void nb_mss_wdog_event(NbMss *m);
/* The only worker. Bounded; a timer also calls it, so no wake-up can be lost. NB_OK when online, NB_PENDING while
 * starting or stopping, NB_REMOTE after a crash, another error after a failure. */
int nb_mss_poll(NbMss *m, uint64_t now_ms);
/* Online only: starts an orderly stop (stop bit, stop-ack, PAS shutdown with the decrypt retry, unassign, unvote). The
 * owner should have sent the SSCTL shutdown request first so the modem flushed its state. */
int nb_mss_stop(NbMss *m, uint64_t now_ms);
/* The GLINK link over the IPCRTR/DS FIFOs, valid once nb_mss_poll returned NB_OK; channels are the owner's. */
QcomGlink *nb_mss_glink(NbMss *m);
/* Sets bits of an outbound entry ("ipa", "smem-mailbox") and rings the modem when the value changed. */
int nb_mss_smp2p_set(NbMss *m, int entry, uint32_t mask, uint32_t value);

#ifdef __cplusplus
}
#endif
#endif
