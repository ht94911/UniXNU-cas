/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PEACH_RSN_H
#define PEACH_RSN_H
#include "PeachStation.h"

#define PEACH_RSN_PACKET 640u
#define PEACH_RSN_QUEUE 4u
enum { PeachRsnIdle, PeachRsnPrepared, PeachRsnM1, PeachRsnM3, PeachRsnKeys,
       PeachRsnConnected, PeachRsnFailed };

typedef struct {
    PeachStation *station;
    unsigned phase, head, count, tx_action, tx_sent, tx_tries;
    unsigned group_index, pair_changed, have_replay, have_response, response_group;
    uint64_t deadline, tx_deadline, replay, m1_replay, response_replay, group_rsc;
    uint32_t expected_keys[2], pn_generation[5]; /* confirmed WMI GTK/PTK epochs */
    uint64_t pn[5][17]; /* separate key generations and QoS/non-QoS replay spaces */
    unsigned char pmk[32], ptk[48], snonce[32], anonce[32], gtk[16];
    unsigned char ap_bssid[6], ap_rsn[257], response_digest[20];
    unsigned ap_rsn_bytes, reply_bytes, packet_bytes[PEACH_RSN_QUEUE];
    unsigned char reply[PEACH_RSN_PACKET], packets[PEACH_RSN_QUEUE][PEACH_RSN_PACKET];
    unsigned char plain[PEACH_RSN_PACKET];
    int error;
} PeachRsn;

/* WPA2-PSK/CCMP128, descriptor v2. PMK is supplied by the connection owner;
 * password derivation/UI are outside the firmware protocol. No WPA1/3, FT,
 * enterprise, PMF, aggregation or opportunistic security downgrade. */
/* Prepare before association so WMI waits can retain the AP's first EAPOL.
 * Start after association/TX binding; it preserves that queue and starts the
 * handshake deadline. Preparation never permits TX or ordinary data. */
int peach_rsn_prepare(PeachRsn *, PeachStation *, unsigned bss_index, const unsigned char pmk[32]);
int peach_rsn_start(PeachRsn *);
/* Called only by the single protocol worker. RX copies EAPOL into a bounded
 * queue; it never sends, installs a key, or recursively progresses HTC. The
 * return value is 1 only for ordinary, authenticated Ethernet data. */
int peach_rsn_receive(PeachRsn *, const void *, size_t, const PeachRxInfo *);
int peach_rsn_poll(PeachRsn *);
void peach_rsn_clear(PeachRsn *);
/* IEEE 802.11 PSK derivation on the connection worker, outside ioctl/locks. */
int peach_rsn_pmk(const void *passphrase, size_t bytes, const void *ssid,
                  size_t ssid_bytes, unsigned char pmk[32]);
#endif
