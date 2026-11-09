/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef QBT_PERI_H
#define QBT_PERI_H
#include "qbt.h"
#ifdef __cplusplus
extern "C" {
#endif

#define QBT_PERI_PACKET_MAX 259u
#define QBT_PERI_SEGMENT_MAX 243u
enum qbt_peri_subsystem { QBT_PERI_SS = 0, QBT_PERI_BT = 1, QBT_PERI_TME = 3 };
enum qbt_peri_operation {
    QBT_PERI_VERSION, QBT_PERI_ARBITRATE, QBT_PERI_PATCH_CONFIG,
    QBT_PERI_SEGMENT, QBT_PERI_BAUD, QBT_PERI_BOARD_ID,
    QBT_PERI_BUILD_INFO, QBT_PERI_RESET, QBT_PERI_ACTIVATE_BT
};
struct qbt_peri_request {
    enum qbt_peri_operation operation;
    uint8_t subsystem;
    uint32_t value; /* baud in bps, ELF configuration, or activation value */
    const uint8_t *data;
    size_t length; /* segment data only */
};
enum qbt_peri_event_kind {
    QBT_PERI_COMPLETE = 0, QBT_PERI_VENDOR = 1,
    QBT_PERI_ACTIVATE_NOTIFY = 2, QBT_PERI_READY = 3
};
struct qbt_peri_event {
    uint64_t sequence; /* assigned by the stream parser; zero for pure decode */
    uint64_t rx_position; /* producer stream position of the first packet byte */
    uint16_t opcode;
    uint8_t kind, status, credits, subcommand, subsystem, action;
    bool has_opcode, has_subcommand, has_subsystem, has_status, rx_position_valid;
    size_t length;
    uint8_t packet[QBT_PERI_PACKET_MAX];
};
struct qbt_peri_version {
    uint32_t product, soc;
    uint16_t patch, rom;
    uint8_t security, subsystem;
};
/* Genuine BT H4 version has no security-version field. */
struct qbt_peri_bt_identity {
    uint32_t product, soc;
    uint16_t patch, rom;
};
enum qbt_peri_activation_stage {
    QBT_PERI_ACT_IDLE, QBT_PERI_ACT_COMMAND, QBT_PERI_ACT_NOTICE,
    QBT_PERI_ACT_RELEASE, QBT_PERI_ACT_PROBE, QBT_PERI_ACT_VERIFY, QBT_PERI_ACT_VERIFIED
};
enum qbt_peri_activation_gap {
    QBT_PERI_ACT_GAP_NONE, QBT_PERI_ACT_NO_UART_DRAIN,
    QBT_PERI_ACT_NO_RX_PRODUCER, QBT_PERI_ACT_RX_SNAPSHOT_FAILED,
    QBT_PERI_ACT_RX_MARK_INVALID, QBT_PERI_ACT_RX_UNSTAMPED,
    QBT_PERI_ACT_RX_UNORDERED, QBT_PERI_ACT_NO_TX_FENCE,
    QBT_PERI_ACT_TX_FENCE_FAILED
};
enum qbt_peri_elf_config {
    QBT_PERI_ELF_CONFIG_UNKNOWN,
    QBT_PERI_ELF_NO_VALUE, /* actual Android property was "no_value" */
    QBT_PERI_ELF_EXPLICIT
};
struct qbt_peri_blob {
    const uint8_t *data;
    size_t length;
    enum qbt_peri_elf_config elf_config;
    uint32_t config_value;
};
struct qbt_peri_ops {
    struct qbt_platform uart; /* reuse the board's executor and GENI DMA */
    /* Board implementation must acquire/release exclusive PERI SoC access
     * within deadline. Success on acquire owns a lease; failed acquire
     * owns none. Failed release retains the lease. No Android ioctl is issued
     * by this component. Missing callback returns EOPNOTSUPP. This arbitration
     * is separate from caller-owned power/UART/phone reservation. Release must
     * preserve those owners, baud, pins, clocks and DMA for subsequent BT I/O. */
    int (*access)(void *, bool acquire, uint64_t deadline_ns);
    /* Optional board identity check, called only after a fresh, valid H4
     * FC00/19 response has been received. It may reject the observed hardware;
     * returning zero cannot replace the response or complete a missing phase.
     * It runs on the same executor without issuing a second UART operation. */
    int (*verify_bt)(void *, const struct qbt_peri_bt_identity *, uint64_t deadline_ns);
    /* Nonblocking ingress snapshot: total RX bytes produced in this UART
     * session, including unread ready buffers and pending/in-flight bytes
     * already received by hardware. Positions start at0 and never reset
     * during PERI->BT handoff. The callback does not deliver/parse RX.
     * A delivered-byte count or parser sequence is not this watermark.
     * Return EOPNOTSUPP if the board cannot prove the complete snapshot. */
    int (*rx_produced)(void *, uint64_t *byte_count);
    /* Nonblocking TX admission on uart.context. Capture the complete RX
     * producer position immediately before admitting this TX fragment; the
     * board must serialize snapshot->admission against other UART work. It
     * must not deliver/parse RX or pump the executor inside this callback.
     * Return 0 with *consumed<=length on success. A negative errno means no
     * TX was admitted and *consumed must be 0; -EAGAIN with *consumed==0
     * retries within the caller's original deadline. With no consumed bytes
     * the mark is ignored. A fast
     * reply may already be queued when the callback returns: its bytes must
     * not be included in rx_before_admission. This uses the same positions
     * as rx_produced/receive_at, including unread and in-flight RX. */
    int (*uart_write_fenced)(void *, const uint8_t *, size_t length,
                             size_t *consumed, uint64_t *rx_before_admission);
};
struct qbt_peri_info {
    int error, cleanup_error;
    uint32_t host_baud;
    bool access_held, awake, baud_verified;
    uint8_t last_kind, last_status, last_credits, last_activation_subsystem, last_activation_action;
    uint8_t activation_stage, activation_gap, activation_reply_value, bt_probe_status;
    bool activation_reply_seen, activation_notice_seen, bt_probe_seen, bt_activated, hci_phase;
    struct qbt_peri_bt_identity bt; /* boot proof; core owns current runtime state */
    uint64_t tx_bytes, rx_bytes, packets, matched, unmatched, malformed;
    uint64_t wake_requests, wake_acks, wake_indications, sleep_indications;
};
struct qbt_peri;

/* All methods, RX and cancellation run on the same existing UART executor.
 * While PERI owns RX, route DMA input to qbt_peri_receive. uart.receive, when
 * supplied, receives ordinary H4 frames with its existing backpressure
 * contract. PERI events never enter the native H4 pipe. No new worker, power
 * owner, firmware loader, or runtime IBS service is created here. */
struct qbt_peri *qbt_peri_create(const struct qbt_peri_ops *, uint32_t initial_baud);
/* Detach RX on its executor after the board retires its DMA, and release the
 * board lease explicitly first. Busy/owned objects are retained. */
int qbt_peri_destroy(struct qbt_peri *);
int qbt_peri_acquire(struct qbt_peri *, uint64_t deadline_ns);
int qbt_peri_release(struct qbt_peri *, uint64_t deadline_ns);
size_t qbt_peri_receive(struct qbt_peri *, const uint8_t *, size_t);
/* position is the producer-assigned absolute position of data[0], preserved
 * while queued or blocked behind other H4 data. Retries use position+consumed.
 * Chunks are in stream order, without gaps. Activation requires this entry
 * point; the unstamped receive entry remains for diagnostics/other helpers. */
size_t qbt_peri_receive_at(struct qbt_peri *, const uint8_t *, size_t, uint64_t position);
/* Bounded executor service for buffered RX and an outstanding PERI IBS ACK.
 * It does not call uart.pump and can follow GENI service in the board's existing
 * pump. Keep servicing after RX space becomes available even without new DMA
 * bytes. HCI phase sends no IBS controls here; core owns their replies. */
void qbt_peri_step(struct qbt_peri *);
/* One-way handoff to the existing HCI receiver on uart.context. No command or
 * probe may be active, PERI access must be released, and no partial/blocked
 * packet or PERI ACK may remain. This only changes RX routing, not activation
 * proof. H4 is relayed through the existing stream/backpressure contract;
 * IBS FD/FC/FE go unchanged to core, which alone replies. 34 remains here.
 * Keep this parser through BT download/reset/runtime/shutdown until the board
 * has safely retired DMA. The core retains HCI shutdown/power ownership. */
int qbt_peri_enter_hci(struct qbt_peri *, size_t (*receiver)(void *, const uint8_t *, size_t));
void qbt_peri_fail(struct qbt_peri *, int error);
void qbt_peri_get_info(const struct qbt_peri *, struct qbt_peri_info *);
uint64_t qbt_peri_sequence(const struct qbt_peri *);

/* Pure helpers. length is output capacity on entry and encoded size on return.
 * Request host ID is always zero. SS is a payload field, when that command
 * has one: board-ID/reset requests omit it, and the baud byte is a baud code. */
int qbt_peri_encode(const struct qbt_peri_request *, uint8_t *, size_t *length);
int qbt_peri_decode(const uint8_t *, size_t, struct qbt_peri_event *);
/* The locked WCN7860 qca_read_soc_version unified H4 CC layout, including
 * opcode, subcommand, actual status, declared length and nonzero SOC/ROM. */
int qbt_peri_decode_bt_version(const uint8_t *, size_t, struct qbt_peri_bt_identity *);
uint64_t qbt_peri_version_key(const struct qbt_peri_version *);
/* Requires a genuine target-3 version. Unknown tuples have no fallback. */
int qbt_peri_tme_filename(const struct qbt_peri_version *, const char **);

/* Every I/O wait uses an absolute monotonic deadline and honors fail/cancel.
 * Wake requires a fresh FC ACK (at most ten FD attempts, ten ms per attempt).
 * command requires a matching vendor reply and its real status. Baud, reset
 * and activation cannot bypass their separate completion contracts through
 * command. Type2 is an activation notice: byte6=SS, byte7=action, with no
 * status or opcode. Activation uses producer positions, not parser timing,
 * to reject bytes already received before admission of its final TX fragment.
 * Failed/ambiguous I/O leaves a sticky error; recreate after the
 * board has stopped and established a fresh controller session. */
int qbt_peri_wake(struct qbt_peri *, uint64_t deadline_ns);
int qbt_peri_command(struct qbt_peri *, const struct qbt_peri_request *,
                     uint64_t deadline_ns, struct qbt_peri_event *);
int qbt_peri_set_baud(struct qbt_peri *, uint32_t baud, uint64_t deadline_ns);
int qbt_peri_read_version(struct qbt_peri *, uint8_t subsystem,
                          uint64_t deadline_ns, struct qbt_peri_version *);
/* Reports the real arbitration state: 0 needs download, 0b waits for another
 * subsystem, 17 already patched; TME 5/6 permit skipping TME in the HAL but
 * do not prove TME patch success. Other states return EOPNOTSUPP. */
int qbt_peri_arbitrate(struct qbt_peri *, uint8_t subsystem,
                       uint64_t deadline_ns, uint8_t *state);
int qbt_peri_read_board_id(struct qbt_peri *, uint64_t deadline_ns, uint16_t *);
int qbt_peri_read_build_info(struct qbt_peri *, uint8_t subsystem,
                             uint64_t deadline_ns, char *, size_t capacity);
/* Transfers the entire hash-verified board input, without modifying it.
 * TLV patch mode comes from byte14 (0 or3); NVM always waits per segment.
 * ELF requires the actual config choice. Even skip-ACK mode waits for the
 * final 06/status0 reply. This is download ACK, not asynchronous patch ready.
 * TME cannot download NVM. Caller selects PERI NVM using the fresh board ID. */
int qbt_peri_download(struct qbt_peri *, uint8_t subsystem,
                      const struct qbt_peri_blob *, uint64_t deadline_ns);
/* Capture sequence before the operation that can produce ready. Cached
 * notifications older than that floor cannot satisfy this wait. Wire status
 * is 0; arbitration's internal state17 is never a ready success status. */
int qbt_peri_wait_ready(struct qbt_peri *, uint8_t subsystem, uint64_t after_sequence,
                        uint64_t deadline_ns);
/* Reset requires both the FFF1 reply and a fresh target-0 async status0. */
int qbt_peri_reset(struct qbt_peri *, uint64_t deadline_ns);
/* Own exclusive PERI access and retain caller's UART reservation; no other H4
 * command may be pending. Drain before sending, then require a FFF1 reply
 * plus a new type2 SS1/action1. Reply value17 is the HAL's skip-notice path;
 * other activation reply values are opaque results, not normal HCI statuses.
 * Release PERI access successfully without changing UART/power, as the HAL
 * does, before the actual first BT PatchVerReq (01 00 fc 01 19). Because the
 * notice has no transaction ID, both paths require a fresh valid unified H4 version.
 * Only that proof, and the optional board check, sets bt_activated. This does
 * not download BT firmware or prove HCI ready/pairing. uart_drained is required
 * (EOPNOTSUPP if absent). rx_produced, receive_at and uart_write_fenced are
 * required: missing ingress provenance or TX admission fence returns
 * EOPNOTSUPP and activation_gap gives the reason. ACT and the internal H4
 * probe use the final admitted fragment's RX position, accepting fast replies
 * already queued before the fenced write returns.
 * Other H4 traffic keeps the existing RX callback.
 * Failures before a successful release leave access held for caller cleanup;
 * after a successful release, later failures keep access_held=false. */
int qbt_peri_activate_bt(struct qbt_peri *, uint64_t deadline_ns);

#ifdef __cplusplus
}
#endif
#endif
