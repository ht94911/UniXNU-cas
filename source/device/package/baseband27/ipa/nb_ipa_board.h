/* SPDX-License-Identifier: BSD-2-Clause */
/* The nezha IPA bring-up in mainline's order, derived from QcomBaseband platform/coachz/coachz_modem.c (dd0199c, BSD-2-Clause,
 * HoshinoYumeka): nb_ipa_configure (ipa_config), the AP loads ipa_fws through PAS 15, nb_ipa_setup (ipa_setup), then the QMI
 * handshake over QRTR when the modem announces its IPA service, and finally the data endpoints.
 *
 * Differences from the Coachz file: the modem does not load the GSI firmware here (downstream ipa3_pil_load_ipa_fws, mainline
 * loader "self"), so setup does not wait for ipa-setup-ready; addresses given to the hardware are IOVAs of the IPA's SMMU context
 * bank (SIDs 0x4a0 and 0x4a2; the Coachz platform runs in bypass); QMI goes through NbQrtr; the modem's clock query on the inbound
 * "ipa" SMP2P entry is answered (downstream ipa3_freeze_clock_vote_and_notify_modem).
 *
 * Everything runs on the baseband worker thread. A failure turns cellular data off for this boot and is printed; a modem restart
 * is not supported in this first phase (see nb_ipa_modem_down). */
#ifndef NB_IPA_BOARD_H
#define NB_IPA_BOARD_H
#include <stdint.h>
#include "../transport/NbQrtr.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Write-combined or uncached memory the IPA can reach at iova. */
typedef struct { void *virt; uint64_t iova; uint32_t bytes; } NbIpaDma;
typedef struct {
    NbIpaDma zero;      /* 0x2000 bytes */
    NbIpaDma ring;      /* 64 KiB, 64 KiB aligned: GSI rings, then the command pool at +0xc000 */
    NbIpaDma lan_rx;    /* AP_LAN_RX buffers */
    NbIpaDma modem_rx;  /* AP_MODEM_RX buffers */
    NbIpaDma modem_tx;  /* AP_MODEM_TX bounce slots */
} NbIpaMemory;

typedef struct {
    NbQrtr *qrtr;
    uint32_t host_port, client_port;   /* local QRTR ports for the IPA QMI server and the INIT_DRIVER client */
    int (*qmp_send)(const char *text);
    /* Sets bits of the outbound "ipa" SMP2P entry. */
    int (*smp2p_set)(void *context, uint32_t mask, uint32_t value);
    void *context;
} NbIpaBoard;

/* ipa_config. The IPA clock, interconnect votes and the SMMU lease must already be held. 0 or a negative error. */
int nb_ipa_configure(const NbIpaBoard *board, const NbIpaMemory *memory);
/* The modem is about to power up (ipa_uc_power on QCOM_SSR_BEFORE_POWERUP). */
void nb_ipa_modem_starting(void);
/* ipa_setup, once ipa_fws is authenticated (GSI_STATUS enabled); registers the QMI server. 0 or a negative error. */
int nb_ipa_setup(void);
/* The SMEM_IPA_FILTER_TABLE item: its size and the address the modem uses for the first 2 KiB (0 leaves the block out). */
void nb_ipa_set_smem(uint32_t size, uint32_t per_stats_addr);
/* QRTR NEW_SERVER; the modem's IPA service starts the handshake. */
void nb_ipa_server_new(uint32_t service, uint32_t instance, uint32_t node, uint32_t port);
/* A DATA packet for a port that is no local service. 1 when it was the INIT_DRIVER client port. */
int nb_ipa_client_packet(uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length);
/* A rising edge of bit 0 of the inbound "ipa" SMP2P entry: answer once with the clock on. */
void nb_ipa_clock_query(void);
/* Interrupts and completion rings, polled. */
void nb_ipa_poll(void);
/* The modem stopped or crashed: data off for this boot. The endpoints are not torn down and DMA memory stays mapped. */
void nb_ipa_modem_down(void);
/* Data path, owner thread only: per-mux ingress callback, and egress of one IP packet. Errors are positive errno values. */
void nb_ipa_data_receiver(void (*receiver)(uint32_t mux, const uint8_t *ip, uint32_t length, uint32_t ip_version));
int nb_ipa_data_send(uint32_t mux, const uint8_t *ip, uint32_t length);
int nb_ipa_data_ready(void);
void nb_ipa_report(unsigned second);

#ifdef __cplusplus
}
#endif
#endif
