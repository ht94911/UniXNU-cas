/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PEACH_TX_H
#define PEACH_TX_H
#include "PeachSrng.h"

#define PEACH_TX_BUFFERS 64u
#define PEACH_TX_BUFFER_BYTES 2048u
#define PEACH_TX_DMA_BYTES (PEACH_TX_BUFFERS * PEACH_TX_BUFFER_BYTES)
typedef struct {
    PeachSrng *ring, *completion;
    PeachHtt *htt;
    PeachMhiDma memory;
    unsigned prepared, started, bound, authorized, lmac, pdev, vdev, peer, ast, hash, qos;
    uint32_t peer_generation;
    unsigned char peer_mac[6];
    unsigned in_flight, next;
    unsigned control_pending, control_done, control_acked;
    uint32_t control_cookie;
    unsigned char owned[PEACH_TX_BUFFERS];
    uint32_t cookies[PEACH_TX_BUFFERS];
    uint64_t submitted_at[PEACH_TX_BUFFERS];
    uint64_t submitted, acked, dropped;
    uint32_t last_descriptor[8], last_status;
    int error;
} PeachTx;
int peach_tx_prepare(PeachTx *, PeachSrng rings[PeachSrngKinds], PeachHtt *,
                     PeachMhiDma, unsigned lmac, unsigned pdev);
/* Host SRNGs precede WMI INIT; enable packet TX only after HTT confirms
 * the metadata ABI and the firmware has completed initialization. */
int peach_tx_start(PeachTx *);
/* Binding must use a real HTT peer map after successful association. The
 * caller may authorize only after the required security/key setup succeeds.
 * Unbind needs all outstanding completions; no timeout-based DMA freeing. */
int peach_tx_bind(PeachTx *, unsigned vdev, const unsigned char mac[6]);
int peach_tx_unbind(PeachTx *);
int peach_tx_authorize(PeachTx *, unsigned enabled);
int peach_tx_send(PeachTx *, const void *, size_t, unsigned tid);
/* One retained EAPOL packet, also allowed before ordinary data authorization.
 * Destination/source must match the selected BSS and local station. Completion
 * is a real TQM/FW result; a successful enqueue is not a transmitted handshake. */
int peach_tx_eapol(PeachTx *, const unsigned char local[6], const void *, size_t);
int peach_tx_poll(PeachTx *);
#endif
