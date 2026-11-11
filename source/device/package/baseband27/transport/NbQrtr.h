/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The IPC router (QRTR) on the application processor's link to the modem (GLINK "IPCRTR" on the mpss edge).
 *
 * Generalizes sheng's QcomQrtr (one link, four local services, 512-byte packets) to what the modem needs: the modem's
 * 64 announced services, AP-side services (rmtfs, service locator, TFTP, memory share, IPA QMI), the QcomBaseband
 * bridge's client ports, and QMI messages of several KiB. The name-service duties are Linux net/qrtr/ns.c's for a
 * remote node: on the remote's HELLO answer HELLO and announce every local server; keep the servers the remote
 * announces; on BYE, or when the link drops, forget them and report each removal. Flow control is af_qrtr.c's
 * receiver side: a DATA packet with confirm_rx is answered with RESUME_TX after it was delivered.
 *
 * No allocation and no waiting. The owner (the baseband worker) feeds every received packet in and supplies a send
 * callback that queues one packet; a refused send is counted, never retried here. Every callback runs on that worker. */
#ifndef NB_QRTR_H
#define NB_QRTR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NB_QRTR_V1_HEADER 32u
#define NB_QRTR_V2_HEADER 16u
#define NB_QRTR_PORT_CTRL 0xfffffffeu
#define NB_QRTR_PORT_CTRL_LEGACY 0xffffu
#define NB_QRTR_NODE_BROADCAST 0xffffffffu
#define NB_QRTR_MAX_NODES 16u
#define NB_QRTR_MAX_LOCAL 16u
#define NB_QRTR_MAX_REMOTE 96u     /* the modem announced 64 on Android 2026-11-07 */
#define NB_QRTR_MAX_PACKET 65536u  /* GLINK IPCRTR intents reach 0x4400; QMI bodies stay well below this */

enum {
    NB_QRTR_DATA = 1, NB_QRTR_HELLO = 2, NB_QRTR_BYE = 3, NB_QRTR_NEW_SERVER = 4, NB_QRTR_DEL_SERVER = 5,
    NB_QRTR_DEL_CLIENT = 6, NB_QRTR_RESUME_TX = 7, NB_QRTR_EXIT = 8, NB_QRTR_PING = 9, NB_QRTR_NEW_LOOKUP = 10,
    NB_QRTR_DEL_LOOKUP = 11,
};
enum { NbQrtrOk = 0, NbQrtrBad = -1, NbQrtrFull = -2, NbQrtrRefused = -3, NbQrtrTooBig = -4, NbQrtrDown = -5 };

typedef struct NbQrtr NbQrtr;
typedef int (*NbQrtrSend)(void *context, const uint8_t *packet, uint32_t length);
/* One DATA packet for a local port. Return value ignored; the router sends RESUME_TX when asked, after this returns. */
typedef void (*NbQrtrReceive)(void *context, NbQrtr *q, uint32_t port, uint32_t node, uint32_t from_port,
                              const uint8_t *data, uint32_t length);
typedef struct {
    void *context;
    /* DATA for a port that is no local service (the bridge's QMI client ports). 1 = consumed. */
    int (*unrouted)(void *context, uint32_t port, uint32_t node, uint32_t from_port, const uint8_t *data, uint32_t length);
    void (*server_new)(void *context, uint32_t service, uint32_t instance, uint32_t node, uint32_t port);
    void (*server_del)(void *context, uint32_t service, uint32_t instance, uint32_t node, uint32_t port);
    /* A remote client went away (DEL_CLIENT, BYE): local services drop its per-client state. */
    void (*client_gone)(void *context, uint32_t node, uint32_t port);
} NbQrtrEvents;

struct NbQrtr {
    uint32_t local_node, remote_node;
    uint8_t up, hello_sent, hello_received;
    uint32_t nodes[NB_QRTR_MAX_NODES], node_count;
    NbQrtrSend send;
    void *send_context;
    NbQrtrEvents events;
    uint8_t *tx;                     /* NB_QRTR_MAX_PACKET bytes owned by the router's owner */
    struct { uint32_t service, instance, port; NbQrtrReceive receive; void *context; } local[NB_QRTR_MAX_LOCAL];
    uint32_t local_count;
    struct { uint32_t service, instance, node, port; } remote[NB_QRTR_MAX_REMOTE];
    uint32_t remote_count;
    uint64_t rx_packets, rx_data, rx_bad, rx_unrouted, rx_dropped_remote, tx_packets, tx_refused, resumes_sent,
             hellos, byes, link_resets;
};

/* tx: a buffer of NB_QRTR_MAX_PACKET bytes for building packets. */
int nb_qrtr_init(NbQrtr *q, uint32_t local_node, NbQrtrSend send, void *send_context, uint8_t *tx, const NbQrtrEvents *events);
/* instance as announced (version | instance << 8). Announced at once when the remote said hello. */
int nb_qrtr_add_service(NbQrtr *q, uint32_t service, uint32_t instance, uint32_t port, NbQrtrReceive receive, void *context);
/* The GLINK channel opened (up=1) or closed (up=0). Up says HELLO; down forgets the remote, reporting each server. */
void nb_qrtr_link(NbQrtr *q, int up);
int nb_qrtr_receive(NbQrtr *q, const uint8_t *packet, uint32_t length);
int nb_qrtr_send_data(NbQrtr *q, uint32_t from_port, uint32_t node, uint32_t port, const uint8_t *data, uint32_t length);
/* A local client port closed: DEL_CLIENT to the remote so its services free that client's state (af_qrtr qrtr_port_remove). */
int nb_qrtr_del_client(NbQrtr *q, uint32_t port);
/* The first remote server of service (and instance when instance != ~0u), -1 when not announced. */
int nb_qrtr_find_remote(const NbQrtr *q, uint32_t service, uint32_t instance);

/* Packet header fields, exposed for the host tests and the trace. */
typedef struct { uint32_t version, type, src_node, src_port, confirm_rx, size, dst_node, dst_port, header; } NbQrtrHeader;
int nb_qrtr_parse_header(const uint8_t *packet, uint32_t length, NbQrtrHeader *h);

#ifdef __cplusplus
}
#endif
#endif
