/* SPDX-License-Identifier: BSD-2-Clause */
/* The baseband worker: the single owner of the IPA bring-up, the modem lifecycle, the QRTR router over GLINK "IPCRTR" and the
 * application-processor services the modem needs (rmtfs, service locator, TFTP, memory share, IPA QMI).
 *
 * Order, on one thread with a bounded poll: stop both MSS images; IPA power, statistics SMEM and SMMU lease; ipa_config;
 * ipa_fws through PAS 15; ipa_setup; modem PAS and lifecycle; then every service runs while the modem is online. Statistics
 * storage is allocated or reused while MSS is stopped. IPA setup and its QMI server are ready before the modem restarts.
 *
 * A failure stops this sequence for the boot and is printed; nothing else in the system depends on it. A modem crash is reported
 * and not restarted in this first phase: the IPA endpoints and their DMA memory stay as they are until the next boot. */
#ifndef NB_BASEBAND_H
#define NB_BASEBAND_H
#include "NbMss.h"
#include "NbIpaSmem.h"
#include "../ipa/nb_ipa_board.h"
#include "../services/NbMemshare.h"
#include "../services/NbRmtfs.h"
#include "../services/NbTftp.h"
#ifdef __cplusplus
extern "C" {
#endif

#define NB_IPA_FWS_PAS 15u
#define NB_IPCRTR_INTENT_BYTES 0x4400u
#define NB_IPCRTR_INTENTS 10u
#define NB_DS_INTENT_BYTES 0x4000u
#define NB_DS_INTENTS 2u
#define NB_TX_QUEUE 32u
#define NB_TX_SLOT_BYTES 8192u
#define NB_TFTP_MAX_BLKSIZE 7168u   /* the OACK carries it; the frame must fit one queue slot */
#define NB_REPLY_BYTES 4096u

/* Local QRTR ports. The bridge's client ports start at NB_PORT_BRIDGE. */
enum {
    NB_PORT_RMTFS = 0x4000, NB_PORT_SERVLOC, NB_PORT_MEMSHARE, NB_PORT_IPA_HOST, NB_PORT_IPA_CLIENT, NB_PORT_TFTP,
    NB_PORT_TFTP_SESSION = 0x4010, NB_PORT_BRIDGE = 0x4100,
};

typedef struct {
    void *context;
    /* on: IPA clock and interconnect votes. off is never asked in this phase. 0 done, NB_PENDING busy, < 0 failed. */
    int (*ipa_power)(void *context, int on);
    /* on: the IPA's SMMU context banks (SIDs 0x4a0, 0x4a2) with the DMA memory, the SMEM item and the mappings the device tree
     * lists. 0 done, NB_PENDING busy, < 0 failed. */
    int (*ipa_smmu)(void *context, int on, const NbMss *stopped,
                    const NbIpaMemory *memory, uint32_t *smem_address);
    int (*qmp_send)(const char *text);
} NbBasebandPlatform;

/* The QcomBaseband bridge hooks: its QMI client ports and the modem's services. */
typedef struct {
    void *context;
    int (*unrouted)(void *context, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length);
    void (*server_new)(void *context, uint32_t service, uint32_t instance, uint32_t node, uint32_t port);
    void (*server_del)(void *context, uint32_t service, uint32_t instance, uint32_t node, uint32_t port);
    void (*online)(void *context, int online);
} NbBasebandBridge;

typedef struct {
    NbBasebandPlatform platform;
    NbBasebandBridge bridge;
    NbMssOps mss;
    NbMemshareOps memshare;
    NbTftpStore tftp;
    NbIpaMemory ipa_memory;
    volatile void *smem;
    uint32_t smem_bytes;
    uint64_t smem_physical;
    /* Item497, allotted/reused after both initial MSS stops and before
     * IPA context activation; exact original address is used in QMI. */
    uint32_t ipa_smem_bytes, ipa_smem_address;
    /* Begun with nb_pas_begin against their carveouts: ipa_fws (PAS 15), modem (PAS 4) and modem_dtb (PAS 0x26). */
    NbPas *ipa_fws, *modem, *modem_dtb;
    NbRmtfs *rmtfs;                /* initialized with its images and shared buffer */
    /* Caller-owned storage. */
    uint8_t *qrtr_tx;              /* NB_QRTR_MAX_PACKET */
    uint8_t *tftp_packet;          /* 4 + NB_TFTP_MAX_BLKSIZE */
    uint8_t *tftp_files;           /* NB_TFTP_RW_FILES * NB_TFTP_RW_FILE_BYTES */
    uint8_t *ipcrtr_intents;       /* NB_IPCRTR_INTENTS * NB_IPCRTR_INTENT_BYTES */
    uint8_t *ds_intents;           /* NB_DS_INTENTS * NB_DS_INTENT_BYTES */
    uint8_t *tx_queue;             /* NB_TX_QUEUE * NB_TX_SLOT_BYTES */
} NbBasebandConfig;

enum {
    NbBasebandPower, NbBasebandSmmu, NbBasebandConfigure, NbBasebandFirmware, NbBasebandSetup, NbBasebandModem, NbBasebandDown,
    NbBasebandFailed,
    NbBasebandInitialStop,
};

typedef struct {
    NbBasebandConfig c;
    NbMss mss;
    NbQrtr qrtr;
    NbTftp tftp;
    NbMemshare memshare;
    QcomGlinkChannel *ipcrtr, *ds;
    uint8_t reply[NB_REPLY_BYTES];
    uint32_t tx_head, tx_count, tx_length[NB_TX_QUEUE];
    uint32_t stage;
    uint8_t reported_online, link_up;
    int error;
    uint64_t tx_queued, tx_dropped, ds_packets;
} NbBaseband;

/* Nothing touches hardware here. 0 or a negative error. */
int nb_baseband_init(NbBaseband *b, const NbBasebandConfig *config);
/* The only worker: bounded, called from a timer and after every event. NB_OK while the modem is online, NB_PENDING while
 * starting, NB_REMOTE after a crash, another error after a failure. */
int nb_baseband_poll(NbBaseband *b, uint64_t now_ms);
/* From the IPCC dispatcher (client 2, signal 2) and the modem watchdog interrupt. */
void nb_baseband_smp2p_event(NbBaseband *b);
void nb_baseband_wdog_event(NbBaseband *b);
/* For the bridge's QMI ports. */
NbQrtr *nb_baseband_qrtr(NbBaseband *b);

#ifdef __cplusplus
}
#endif
#endif
