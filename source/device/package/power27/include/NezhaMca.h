/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_MCA_H
#define NEZHA_MCA_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NP_MCA_OWNER 0x800au
#define NP_MCA_DATA 256u
#define NP_MCA_REQUEST 276u
#define NP_MCA_RESPONSE 280u
#define NP_MCA_NOTIFICATION 272u
enum { NP_MCA_READ = 1, NP_MCA_WRITE = 2 };
enum { NP_MCA_IGNORE = 0, NP_MCA_REPLY = 1, NP_MCA_NOTE = 2, NP_MCA_ERROR = -1 };
typedef struct {
    uint64_t epoch, deadline_ms, sent_ms;
    uint32_t sequence, property, opcode, retcode;
    uint16_t reply_bytes;
    uint8_t up, pending, exhausted;
    uint8_t reply[NP_MCA_DATA];
    uint32_t ignored, errors, timeouts;
} NpMca;
void np_mca_init(NpMca *);
void np_mca_link(NpMca *, int up);
/* Builds a message only; the owner arms the deadline when send is accepted.
 * There is deliberately no notification-enable opcode: MCA pushes notifications
 * after channel negotiation. ShengBattmgr's opcode4 belongs to another protocol. */
int np_mca_begin(NpMca *, uint32_t property, uint32_t opcode,
                 const void *data, uint32_t size, uint32_t reply_size,
                 uint8_t out[NP_MCA_REQUEST]);
int np_mca_sent(NpMca *, uint64_t now_ms, uint32_t timeout_ms);
void np_mca_cancel(NpMca *);
int np_mca_receive(NpMca *, uint64_t receive_epoch, uint64_t now_ms, const uint8_t *, uint32_t,
                   uint32_t *note, uint32_t *note_value);
int np_mca_timeout(NpMca *, uint64_t now_ms);
uint32_t np_le32(const uint8_t *);
void np_put32(uint8_t *, uint32_t);
#ifdef __cplusplus
}
#endif
#endif
