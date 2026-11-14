/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QBT_INTERNAL_H
#define QBT_INTERNAL_H
#include "qbt.h"
#ifdef KERNEL
#include <sys/errno.h>
#include <sys/systm.h>
#else
#include <errno.h>
#include <string.h>
#include <stdio.h>
#endif
#define QBT_TX_CAPACITY 32768u
#define QBT_CHUNK 2048u
#define QBT_COMMAND_TIMEOUT 10000000000ULL
#define QBT_WAKE_RETRY 100000000ULL
#define QBT_TX_POLL 1000000ULL
#define QBT_SPEED_REPLY_MS 100u
#define QBT_GANGES_SPEED_REPLY_MS 1000u /* public QTI timer; observation bound, not a verified live-HAL timer */
#define QBT_SPEED_SETTLE_MS 5u
#define QBT_TX_IDLE 2000000000ULL
enum { QBT_ASLEEP, QBT_WAKING, QBT_AWAKE };
struct qbt_tx_frame {
    uint8_t type, header[4], have, need, value;
    unsigned body;
    uint16_t link_handle;
    uint32_t link_value;
};
#define QBT_ACL_OBSERVER_SLOTS 4u
/* Bounded observation of Basic L2CAP SMP, not a protocol implementation.
 * Only the four-byte L2CAP header and four non-secret pairing parameters
 * are retained. Other SMP bodies are skipped even across ACL fragments. */
struct qbt_acl_observer {
    uint16_t handle, length, remaining, cid;
    uint8_t header[4], header_have, code, parameters[4];
    unsigned position;
    bool active;
};
struct qbt_acl_direction {
    struct qbt_acl_observer slots[QBT_ACL_OBSERVER_SLOTS];
    unsigned current, packet_remaining;
    bool complete;
};
struct qbt {
    struct qbt_platform ops;
    struct qbt_info info;
    struct qbt_stats stats;
    uint8_t tx[QBT_TX_CAPACITY];
    struct qbt_tx_frame tx_frame;
    struct qbt_acl_direction tx_acl, rx_acl;
    unsigned tx_head, tx_length;
    bool service, ibs, rx_awake, clock_on;
    unsigned tx_state, wake_attempts;
    uint64_t retry_at, idle_at;
    uint8_t control[16]; unsigned control_length;
    uint8_t command[259]; unsigned command_length, command_offset;
    bool waiting, complete, speed_reply;
    uint8_t wait_subcommand, speed_status;
    const char *startup_nvm;
    uint16_t expected_board;
    bool check_board;
    uint16_t wait_opcode;
    uint8_t wait_event;
    uint8_t reply[258]; unsigned reply_length;
    uint8_t type, header[4]; unsigned header_length, header_need, body;
    uint8_t event[258]; unsigned event_length;
    bool drop;
    uint8_t forward[QBT_CHUNK]; unsigned forward_length, forward_offset;
};
int qbt_command(struct qbt *, uint16_t, const void *, unsigned, uint8_t,
                uint8_t *, size_t *);
int qbt_send_command(struct qbt *, uint16_t, const void *, unsigned);
int qbt_drain(struct qbt *, uint64_t);
void qbt_delay(struct qbt *, unsigned);
void qbt_linux_log(struct qbt *, int, const char *, ...);
#endif
