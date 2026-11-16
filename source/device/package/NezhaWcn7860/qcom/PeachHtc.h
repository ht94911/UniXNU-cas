/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PEACH_HTC_H
#define PEACH_HTC_H
#include "PeachCe.h"
#define PEACH_HTC_ENDPOINTS 32u
typedef struct {
    void *context;
    int (*progress)(void *); /* MHI/QRTR service, no recursive HTC calls */
    uint64_t (*now_us)(void *);
    void (*sleep_us)(void *, unsigned);
    void (*log)(void *, const char *, uint32_t, uint32_t);
    /* Borrowed service payload, after validated HTC header/trailer removal. */
    int (*receive)(void *, unsigned service, const void *, size_t);
} PeachHtcOps;
typedef struct {
    unsigned service, tx_pipe, rx_pipe, max_bytes;
    unsigned credits, allocation, flow_control, sequence;
} PeachHtcEndpoint;
typedef struct {
    PeachCe *ce;
    PeachHtcOps ops;
    PeachHtcEndpoint endpoint[PEACH_HTC_ENDPOINTS];
    unsigned ready, started, receiving, credit_size, credit_count, max_endpoints;
    unsigned pending_service, pending_credits, response_ready, wmi_endpoint, htt_endpoint;
    int error;
    unsigned char packet[3520];
} PeachHtc;
int peach_htc_init(PeachHtc *, PeachCe *, const PeachHtcOps *);
/* Install as PeachCeOps.receive before CE starts. Never waits or allocates. */
void peach_htc_receive(void *, unsigned, const void *, size_t);
int peach_htc_poll(PeachHtc *);
/* Wait for real READY, connect WMI/HTT, then SETUP_COMPLETE_EX. */
int peach_htc_bootstrap(PeachHtc *);
/* No implicit wait; PeachCeFull means retry after progress/sleep. WMI credits
 * remain enabled; HTT disables HTC credits as in vendor dp_htt.c. */
int peach_htc_send(PeachHtc *, unsigned service, const void *, size_t);
#endif
