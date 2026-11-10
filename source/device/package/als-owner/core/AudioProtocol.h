/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_PROTOCOL_H
#define NEZHA_AUDIO_PROTOCOL_H
#include <stdint.h>
#include "../upstream/ShengGpr.h"
#include "../upstream/ShengAudioReach.h"
#ifdef __cplusplus
extern "C" {
#endif

/* All methods run on one audio worker. GLINK callbacks only enqueue complete
 * packets. No implicit retry: a timeout leaves DSP ownership uncertain. */
enum { NA_OK = 0, NA_PENDING = 1, NA_IGNORED = 2,
       NA_ARGUMENT = -1, NA_BUSY = -2, NA_WIRE = -3, NA_REMOTE = -4,
       NA_TIMEOUT = -5, NA_OFFLINE = -6, NA_OVERFLOW = -7,
       NA_OWNERSHIP = -8, NA_MISSING = -9, NA_IO = -10 };
#define NA_TRANSACTIONS 8u
#define NA_RX_SLOTS 32u
typedef struct {
    uint32_t token, generation, port, peer, opcode, response;
    uint64_t deadline_ms;
    int state;                     /* zero unused, pending, or terminal error */
    uint32_t value;                /* handle, SPF state, or DSP error */
} NaTransaction;
typedef struct {
    uint32_t generation, next_token, online, exhausted, last_expected_token;
    uint32_t ignored, malformed, timeouts, remote_errors;
    NaTransaction transaction[NA_TRANSACTIONS];
} NaSession;
void na_session_init(NaSession *s);
/* Invalidate all in-flight requests; tokens NEVER restart on reconnect. */
int na_session_link(NaSession *s, int online);
uint32_t na_token(NaSession *s);
int na_expect(NaSession *s, uint32_t token, uint32_t port, uint32_t peer,
              uint32_t opcode, uint32_t response, uint64_t now_ms, uint32_t timeout_ms);
/* result consumes a completed transaction; NA_PENDING leaves it owned. */
int na_result(NaSession *s, uint32_t token, uint32_t *value);
int na_receive(NaSession *s, const uint8_t *data, uint32_t length);
void na_expire(NaSession *s, uint64_t now_ms);
/* Only for a send proved not queued, NOT for a timeout or missing ack. */
int na_cancel_unsent(NaSession *s, uint32_t token);
int na_packet(const uint8_t *data, uint32_t length, GprPacket *p);

/* Retain sheng wire encoding; replace topology-owned endpoint IDs explicitly. */
uint32_t na_buffer_packet(uint8_t out[GPR_MAX_PACKET], uint32_t port,
                         uint32_t endpoint, uint32_t token, int capture,
                         uint64_t dsp_address, uint32_t handle, uint32_t bytes);
/* APM out-of-band payload, already mapped and cache-cleaned by the owner.
 * Payload lifetime extends to the matching response or proven DSP quiescence. */
uint32_t na_oob_packet(uint8_t out[GPR_MAX_PACKET], uint32_t port, uint32_t token,
                      uint32_t opcode, uint64_t address, uint32_t handle, uint32_t bytes);
uint32_t na_oob_packet_to(uint8_t out[GPR_MAX_PACKET],uint32_t port,uint32_t peer,uint32_t token,
                          uint32_t opcode,uint64_t address,uint32_t handle,uint32_t bytes);
uint32_t na_subgraphs_packet(uint8_t out[GPR_MAX_PACKET], uint32_t port, uint32_t token,
                            uint32_t opcode, const uint32_t *ids, uint32_t count);

/* Bounded SPSC: producer is the sole GLINK worker; consumer is audio worker.
 * Overflow latches a fault instead of dropping an ownership-changing reply. */
typedef struct {
    uint32_t generation, length;
    uint8_t data[GPR_MAX_PACKET];
} NaRxPacket;
typedef struct {
    uint32_t head, tail, fault;
    NaRxPacket slots[NA_RX_SLOTS];
} NaRxQueue;
int na_rx_push(NaRxQueue *q, uint32_t generation, const uint8_t *data, uint32_t length);
/* Ordered link events share the packet FIFO; a rapid down/up is never
 * collapsed into one 'online' bit. length==0 is reserved for these events. */
int na_rx_link(NaRxQueue *q,uint32_t generation,int online);
int na_rx_pop(NaRxQueue *q, NaRxPacket *packet);
#ifdef __cplusplus
}
#endif
#endif
