/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PEACH_NET_H
#define PEACH_NET_H
#include "PeachControl.h"
#include "PeachStation.h"
typedef struct {
    unsigned action, apple_layout, radio_power;
    uint64_t request_id;
    PeachControlJoinData join;
    unsigned passphrase_bytes;
    unsigned char passphrase[63];
} PeachNetRequest;
typedef struct {
    unsigned state, result, security, powered;
    int error;
    uint32_t country;
    PeachWmiBss current;
} PeachNetInfo;
typedef struct {
    uint64_t tx_frames, rx_frames, tx_bytes, rx_bytes;
    unsigned dhcp_tx, dhcp_rx, dhcp_tx_type, dhcp_rx_type, dhcp_ack, valid_ipv4;
    unsigned char ipv4[4], offered[4], mask[4], router[4], dns[4];
} PeachNetNetwork;
/* Read the actual BSD address list; never derive an installed IP from DHCP. */
void peach_net_network(PeachNetNetwork *);
int peach_net_init(void);
int peach_net_power(unsigned enabled);
void peach_net_power_complete(unsigned enabled, int error);
int peach_net_request(const PeachNetRequest *);
int peach_net_take_request(PeachNetRequest *);
void peach_net_status(const PeachWmi *, unsigned state, int error);
void peach_net_selection(const PeachWmiBss *, unsigned security);
void peach_net_info(PeachNetInfo *);
int peach_net_bss(unsigned index, PeachWmiBss *);
unsigned peach_net_channels(PeachWmiScanChannel *, unsigned capacity);
int peach_net_attach(const unsigned char mac[6]);
int peach_net_verify_attached(void);
void peach_net_link(unsigned up);
void peach_net_leave(void);
int peach_net_transmit(PeachTx *);
void peach_net_receive(const void *, size_t);
void peach_net_stop(int error);
/* Keep the registered interface and administrative radio state during a
 * real firmware restart. Link/data stay down until a fresh manual join. */
void peach_net_recovering(int error);
#endif
