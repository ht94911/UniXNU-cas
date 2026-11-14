/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef QBT_H
#define QBT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct qbt;
/* All protocol entry points run on one executor. IRQ and socket producers use
 * their own queues and wake it; callbacks never reenter a command operation. */
struct qbt_platform {
    void *context;
    void *(*allocate)(void *, size_t);
    void (*release)(void *, void *, size_t);
    uint64_t (*now_ns)(void *);
    size_t (*uart_write)(void *, const uint8_t *, size_t);
    bool (*uart_drained)(void *);
    int (*uart_speed)(void *, uint32_t);
    int (*uart_flow)(void *, bool);
    /* clock(false) succeeds only with a real RX wake source armed. */
    int (*clock)(void *, bool);
    void (*pump)(void *);
    void (*wait_until)(void *, uint64_t);
    size_t (*receive)(void *, const uint8_t *, size_t);
    /* Returned firmware remains valid until firmware_put; NVM is copied. */
    int (*firmware_get)(void *, const char *, const uint8_t **, size_t *);
    void (*firmware_put)(void *, const uint8_t *, size_t);
    void (*log)(void *, int, const char *);
};
#define QBT_HCI_RECENT_CAPACITY 24u
enum qbt_hci_kind {
    QBT_HCI_TX = 1, QBT_HCI_CC = 2, QBT_HCI_CS = 3,
    QBT_HCI_EVENT = 4, QBT_HCI_LE_REPORT = 5
};
/* Metadata only, observed on the existing executor while service/state2 are
 * active. opcode is 0 for unbound events. code is H4 type1 for TX, the event
 * code for CC/CS/EVENT, or the LE subevent for LE_REPORT. Missing status/value
 * is ff. TX value is the first parameter only for 2042/200c/0c1a; RX value
 * contains only command credits or the announced response/report count.
 * No address, name, key, or other command/event payload is retained here. */
struct qbt_hci_recent {
    uint64_t sequence, ns;
    uint16_t opcode;
    uint8_t kind, code, status, value;
};
struct qbt_hci_stats {
    uint64_t commands_tx, command_complete, command_status, command_errors;
    uint64_t inquiry_tx, inquiry_cancel_tx, inquiry_complete;
    uint64_t inquiry_result_events, announced_devices;
    uint64_t remote_name_completions, remote_name_errors;
    uint64_t le_scan_parameters_tx, le_scan_enable_tx, le_scan_disable_tx;
    /* Scan responses/errors cover LE 200b/200c/2041/2042 only. Announcement
     * counts include repeats and are not distinct-device/address counts. */
    uint64_t scan_responses, scan_errors;
    uint64_t legacy_report_events, extended_report_events, announced_advertisements;
    uint64_t recent_total;
    uint32_t recent_next, recent_count; /* next overwrite index, valid entries */
    struct qbt_hci_recent recent[QBT_HCI_RECENT_CAPACITY];
};
#define QBT_LINK_RECENT_CAPACITY 32u
enum qbt_link_kind {
    QBT_LINK_TX = 1, QBT_LINK_CC = 2, QBT_LINK_CS = 3,
    QBT_LINK_EVENT = 4, QBT_LINK_LE_EVENT = 5,
    QBT_LINK_SMP_TX = 6, QBT_LINK_SMP_RX = 7
};
/* Connection/security metadata has its own ring so advertisements cannot
 * evict a pairing result. A handle is a temporary controller handle, or ffff
 * when the event has no handle. value contains only whitelisted protocol
 * flags/reasons (SMP request/response: IO|OOB<<8|AuthReq<<16|key-size<<24).
 * PINs, passkeys, addresses, names, randoms and key material are never copied.
 * Successful command status is acceptance, not connection/authentication. */
struct qbt_link_recent {
    uint64_t sequence, ns;
    uint32_t value;
    uint16_t opcode, handle;
    uint8_t kind, code, status;
};
struct qbt_link_stats {
    uint64_t command_errors, classic_connections, classic_connection_errors;
    uint64_t le_connections, le_connection_errors;
    uint64_t authentication_complete, authentication_errors;
    uint64_t pairing_complete, pairing_errors;
    uint64_t encryption_change, encryption_errors, disconnects;
    uint64_t smp_packets, smp_failures, acl_observation_dropped, recent_total;
    uint32_t recent_next, recent_count;
    struct qbt_link_recent recent[QBT_LINK_RECENT_CAPACITY];
};
struct qbt_stats {
    uint64_t tx_bytes, rx_bytes, host_bytes, delivered_bytes;
    uint64_t commands, timeouts, malformed, unmatched_events;
    uint64_t wake_ind, wake_ack, sleep_ind, wake_retries, clock_sleeps;
    uint64_t backpressure, tx_rejected, resets;
    uint64_t tlv_sent, tlv_acks, tlv_final_status, hardware_errors, last_hardware_error;
    uint64_t command_credit_events, command_credits, completed_packets;
    struct qbt_hci_stats hci;
    struct qbt_link_stats link;
};
struct qbt_info {
    unsigned state; /* 0 off, 1 initializing, 2 ready, 3 failed */
    int error;
    uint32_t baud, product, soc;
    uint16_t rom, patch;
    uint16_t board;
    bool board_valid;
    char firmware[64];
};
enum qbt_controller { QBT_WCN7850, QBT_WCN7860 };
enum qbt_baud_method { QBT_BAUD_FC48, QBT_BAUD_CURRENT, QBT_BAUD_PERI };
/* Board-owned, immutable input. CURRENT means the version request and the
 * entire download use the same already-established operating rate. It does
 * not implement the separate QTI PERI baud negotiation. PERI is consumed by
 * the board before this HCI core; the board passes CURRENT only after the
 * real PERI/TME sequence establishes baud and a fresh BT version reply. */
struct qbt_boot_options {
    enum qbt_controller controller;
    enum qbt_baud_method baud_method;
    uint32_t initial_baud, operating_baud;
    const char *rampatch, *nvm; /* exact basenames in the verified board manifest */
    uint32_t expected_product, expected_soc; /* zero: observe, do not assume */
    uint16_t expected_rom;
    uint16_t expected_board;
    bool check_board;
    bool nvm_by_board; /* use the locked Linux ROM/board selector; loader rejects unlisted inputs */
};
struct qbt *qbt_create(const struct qbt_platform *);
void qbt_destroy(struct qbt *);
/* Returns consumed input; the UART retains unconsumed bytes under backpressure. */
size_t qbt_receive(struct qbt *, const uint8_t *, size_t);
/* Atomic enqueue: 0 accepted, negative errno leaves ownership with the caller. */
int qbt_transmit(struct qbt *, const uint8_t *, size_t);
size_t qbt_transmit_capacity(const struct qbt *);
void qbt_step(struct qbt *);
uint64_t qbt_deadline(const struct qbt *);
void qbt_set_service(struct qbt *, bool);
void qbt_rx_wake(struct qbt *);
/* Abort command waiters promptly after a UART/DMA fault on the executor. */
void qbt_fail(struct qbt *, int error);
/* address uses display/network order; the Linux adapter converts to bdaddr_t. */
int qbt_boot(struct qbt *, uint32_t baud, const uint8_t address[6]);
int qbt_boot_controller(struct qbt *, const struct qbt_boot_options *, const uint8_t address[6]);
int qbt_shutdown(struct qbt *);
void qbt_get_info(const struct qbt *, struct qbt_info *);
void qbt_get_stats(const struct qbt *, struct qbt_stats *);
#ifdef __cplusplus
}
#endif
#endif
