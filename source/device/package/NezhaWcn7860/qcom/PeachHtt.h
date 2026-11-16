/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PEACH_HTT_H
#define PEACH_HTT_H
#include "PeachHtc.h"
enum { PeachHttHwToSw, PeachHttSwToHw, PeachHttSwToSw };
#define PEACH_HTT_PEERS 8u

typedef struct {
    unsigned mapped, vdev, peer, ast, hash, ast_valid, hash_valid;
    uint32_t generation;
    unsigned security[2], security_valid[2]; /* multicast / unicast */
    uint32_t security_generation[2];
    unsigned char mac[6];
} PeachHttPeer;
typedef struct {
    /* All addresses are coherent, retained DMA, including BOTH remote
     * pointers. No CPU virtual address or windowed BAR address is accepted. */
    PeachMhiDma descriptors, head, tail;
    unsigned pdev, id, type, entries, entry_dwords;
} PeachHttRing;
typedef struct {
    PeachHtc *htc;
    unsigned requested, confirmed, major, minor, address_bits, metadata_version;
    unsigned setup_pending, setup_sent, setup_done, setup_pdev, setup_id, setup_status;
    uint32_t configured[4];
    unsigned rx_config_sent, rx_pdev, backpressure_reports;
    uint32_t peer_generation;
    PeachHttPeer peers[PEACH_HTT_PEERS];
    uint32_t last_message;
    int error;
} PeachHtt;
int peach_htt_init(PeachHtt *, PeachHtc *);
void peach_htt_receive(PeachHtt *, const void *, size_t);
/* Single worker, fixed table. Pointer is borrowed and invalidated by the
 * next receive. Legacy maps have no AST hash; never invent that value. */
const PeachHttPeer *peach_htt_peer(const PeachHtt *, unsigned vdev, const unsigned char mac[6]);
/* Real VERSION_REQ/CONF, retaining implicit V1 TCL metadata. A completed
 * negotiation is not a configured DP path or evidence of packet exchange. */
int peach_htt_negotiate(PeachHtt *);
/* HTT 3.143 follows the captured native submission-only setup stream;
 * other versions require a matching successful SRING_SETUP_DONE.
 * Neither result proves that RX or association is working. */
int peach_htt_setup_ring(PeachHtt *, const PeachHttRing *);
/* Full Peach RX_MSDU_END + RX_MPDU_START, 4-byte tags, no packet-header
 * TLV/word compaction. RXDMA ring 0 is distinct from host refill ring 5.
 * Firmware provides no ACK for this command; success means submitted. */
int peach_htt_config_rx(PeachHtt *, unsigned pdev, unsigned buffer_bytes);
#define PEACH_HTT_RX_MSDU_OFFSET 0u
#define PEACH_HTT_RX_MPDU_OFFSET 140u
#define PEACH_HTT_RX_PACKET_OFFSET 264u
#endif
