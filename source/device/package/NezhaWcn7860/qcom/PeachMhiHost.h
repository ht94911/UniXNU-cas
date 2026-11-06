/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PEACH_MHI_HOST_H
#define PEACH_MHI_HOST_H
#include "PeachMhiFirmware.h"

#define PEACH_MHI_HOST_BYTES 0x110000u
#define PEACH_MHI_IPCR_MTU 8192u

/* One worker owns host state, rings and receive callbacks. memory must be
 * coherent little-endian DMA belonging to SID 0x1401's isolated domain.
 * The caller masks INTx and routes PCI MSI into owned DMA while polling,
 * holds endpoint power, and
 * retains all storage until real endpoint containment, including on errors. */
typedef struct {
    PeachMhiDma memory;
    const PeachMhiIo *io;
    void (*barrier)(void *);
    void (*receive)(void *, const void *, size_t);
    void *receive_context;
    uint32_t channel_db, event_db, bhi;
    unsigned event_rp[2], event_wp[2];
    unsigned tx_rp, tx_wp, rx_rp, rx_wp, cmd_wp;
    uint64_t pending_cmd;
    unsigned pending_channel, command_done, environment, state, ipcr_up, poisoned;
    uint64_t last_event_pointer, last_ring_pointer;
    uint32_t last_event_data[2], last_status;
} PeachMhiHost;

/* After BHI SBL transfer: wait RESET clear/READY, publish actual contexts and
 * event rings, enter M0, and hold DEVICE_WAKE. No network interface is made. */
int peach_mhi_host_start(PeachMhiHost *, const PeachMhiIo *, PeachMhiDma,
                         void (*barrier)(void *));
/* Consume at most budget events; an empty ring returns immediately. The
 * caller sleeps between polls. Invalid device pointers permanently poison
 * this host. No packet or event pointer is trusted as a CPU address. */
int peach_mhi_host_poll(PeachMhiHost *, unsigned budget);
/* Called after firmware download reports success and BHI EE is AMSS. */
int peach_mhi_host_open_ipcr(PeachMhiHost *);
/* -6 is ring full: poll/sleep and retry the same packet without dropping it. */
int peach_mhi_host_send(PeachMhiHost *, const void *, size_t);
#endif
