/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PEACH_RX_H
#define PEACH_RX_H
#include "PeachSrng.h"

#define PEACH_RX_BUFFERS 256u
#define PEACH_RX_BUFFER_BYTES 4096u
#define PEACH_RX_LINKS 1023u
#define PEACH_RX_QUEUE_BYTES (16u * 512u + 128u)
#define PEACH_RX_DMA_BYTES (PEACH_RX_LINKS * 128u + PEACH_RX_QUEUE_BYTES + \
                            PEACH_RX_BUFFERS * PEACH_RX_BUFFER_BYTES)

typedef struct {
    unsigned peer, tid, encrypted, cipher, key_index, multicast, decrypted;
    uint64_t pn;
    unsigned char transmitter[6];
} PeachRxInfo;
typedef int (*PeachReceive)(void *, const void *, size_t, const PeachRxInfo *);

typedef struct {
    PeachSrng *rings;
    PeachMhiDma memory, links, queues, buffers;
    unsigned prepared, started, repost, in_flight, peer_metadata_version;
    unsigned char owned[PEACH_RX_BUFFERS];
    uint32_t cookies[PEACH_RX_BUFFERS];
    uint64_t received, discarded, released;
    uint32_t last_register, last_value, last_descriptor[8];
    int error;
} PeachRx;

/* One worker; no transient allocation. Queue descriptors are unbound until
 * peer setup. Initial non-BA queues have PN disabled: protected peers need
 * explicit PN/BA configuration before they can be connected. */
int peach_rx_prepare(PeachRx *, PeachSrng rings[PeachSrngKinds], PeachMhiDma);
/* Host rings and the idle link pool are started/seeded before WMI INIT.
 * After HTT setup, this final step publishes packet buffers and filters. */
int peach_rx_start(PeachRx *, PeachHtt *, unsigned pdev, unsigned peer_metadata_version);
PeachMhiDma peach_rx_queue(const PeachRx *, unsigned tid);
/* After peer retirement only: use fresh retained storage. Old descriptors
 * may still be cached by REO and must never be cleared or reassigned. */
int peach_rx_new_queues(PeachRx *, PeachMhiDma);
/* Callback borrows a complete Ethernet MSDU only until it returns. No data
 * callback means discard; configuring RX does not publish a NIC. Linked
 * exceptions are dropped/reclaimed through SW2WBM. Unknown ownership stops
 * the DP with all DMA retained. */
int peach_rx_poll(PeachRx *, PeachReceive, void *);
#endif
