/* SPDX-License-Identifier: GPL-2.0-only */
/* Independently implemented protocol helpers from the N0 QTI HAL, SHA256
 * d6d6c883ecb3f904fa3907d85f67b47396ee7e9d25231e7016754742d4519844.
 * Verified functions: ReadHciPeriEvent1294dc, ReadHciEvent12c890,
 * GetVsHciEvent12d4ac, ProcessPeriEdlPatchEvt12d85c, ReadVsAsyncEvent12d2c4,
 * SetPeriBaudRateReq12ae80, FramePeriHciPkt13003c, TlvDnldReq12f8c0,
 * PeriReset130e58, GetBoardIdReq12fae4, LoadTMELPatchMaptable128454.
 * BL names were resolved through ELF GOT relocations, not objdump PLT labels.
 * Type2 byte6=SsId/byte7=Action is confirmed by ReadVsAsyncEvent12d394 and
 * rodata6d099. ActivateSS12a978 skips its notice for reply17; it otherwise
 * waits for the notification without assigning an invented result enum.
 * PerformChipInit129d84 releases PERI access before ordinary BT init. The following
 * BT PatchVerReqda2a8 sends01 00 fc 01 19. Unified version validation follows
 * locked WCN7860 btqca.c:qca_read_soc_version (9321086f2e2234bfdf073ac3326bd88cb5053e4f).
 * Firmware and HAL bytes remain external board inputs. No device result is
 * implied by these helpers or by the synthetic host test. */
#include "qbt_peri.h"
#ifdef KERNEL
#include <sys/errno.h>
#include <sys/systm.h>
#else
#include <errno.h>
#include <string.h>
#endif

#define PERI_EDL 0xfff0u
#define PERI_CONTROL 0xfff1u
#define PERI_POLL_NS 1000000ULL
#define PERI_WAKE_NS 10000000ULL /* timeval at HAL VA48540: {0,10000} */
#define PERI_COMMAND_NS 10000000000ULL
#define PERI_BAUD_DELAY_NS 20000000ULL

struct qbt_peri {
    struct qbt_peri_ops ops;
    struct qbt_peri_info info;
    bool busy, waiting, complete, accept_complete, ack_pending, transmitting, wake_waiting, wake_ack;
    bool download_active, baud_changing;
    bool activating, probing, probe_complete, request_sending, probe_sending;
    bool proof_required, frame_stamped, rx_next_valid, rx_mark_valid;
    size_t (*hci_receiver)(void *, const uint8_t *, size_t);
    uint8_t download_subsystem;
    uint16_t expect_opcode;
    uint8_t expect_subcommand, expect_subsystem;
    bool expect_selector;
    enum qbt_peri_operation operation;
    uint64_t sequence, frame_sequence, command_floor, download_floor, probe_floor;
    uint64_t frame_position, rx_next, last_rx_mark, command_rx_floor, probe_rx_floor;
    struct { uint64_t success, failure; } ready[2];
    struct qbt_peri_event reply;
    uint8_t frame[QBT_PERI_PACKET_MAX];
    size_t have, header, remaining, forward, forwarded;
    bool streaming;
};

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool valid_ss(uint8_t ss) { return ss == QBT_PERI_SS || ss == QBT_PERI_TME; }
static int negative(int e) { return e < 0 ? e : -EIO; }
static uint64_t now(struct qbt_peri *p) { return p->ops.uart.now_ns(p->ops.uart.context); }
static uint64_t earlier(uint64_t a, uint64_t b) { return a < b ? a : b; }
static int baud_code(uint32_t baud) {
    /* HciUartTransport::find_baud_rate_code0xfcc54, including0xfce38. */
    switch (baud) {
    case 115200: return 0;
    case 3000000: return 0x0e;
    case 3200000: return 0x11;
    case 4000000: return 0x0f;
    default: return -EOPNOTSUPP;
    }
}

int qbt_peri_encode(const struct qbt_peri_request *r, uint8_t *out, size_t *length) {
    uint16_t opcode = PERI_EDL;
    uint8_t sub = 0, payload[246];
    size_t n = 0;
    int code;
    if (!r || !out || !length) return -EINVAL;
    if (r->operation == QBT_PERI_ACTIVATE_BT) {
        if (r->subsystem != QBT_PERI_BT || r->value > 1) return -EOPNOTSUPP;
    } else if (!valid_ss(r->subsystem)) return -EOPNOTSUPP;
    switch (r->operation) {
    case QBT_PERI_VERSION: sub = 5; payload[n++] = r->subsystem; break;
    case QBT_PERI_ARBITRATE: sub = 8; payload[n++] = r->subsystem; payload[n++] = 0; break;
    case QBT_PERI_PATCH_CONFIG:
        sub = 7; payload[n++] = r->subsystem;
        for (unsigned i = 0; i < 4; i++) payload[n++] = (uint8_t)(r->value >> (8 * i));
        break;
    case QBT_PERI_SEGMENT:
        if (!r->data || !r->length || r->length > QBT_PERI_SEGMENT_MAX) return -EMSGSIZE;
        sub = 6; payload[n++] = r->subsystem; payload[n++] = (uint8_t)r->length;
        memcpy(payload + n, r->data, r->length); n += r->length;
        break;
    case QBT_PERI_BAUD:
        if (r->subsystem != QBT_PERI_SS) return -EOPNOTSUPP;
        code = baud_code(r->value); if (code < 0) return code;
        opcode = PERI_CONTROL; sub = 2; payload[n++] = (uint8_t)code; break;
    case QBT_PERI_BOARD_ID:
        if (r->subsystem != QBT_PERI_SS) return -EOPNOTSUPP;
        sub = 0x0a; break; /* six bytes; no selector in the actual request */
    case QBT_PERI_BUILD_INFO: sub = 9; payload[n++] = r->subsystem; break;
    case QBT_PERI_RESET:
        if (r->subsystem != QBT_PERI_SS) return -EOPNOTSUPP;
        opcode = PERI_CONTROL; sub = 3; break; /* six bytes; no selector */
    case QBT_PERI_ACTIVATE_BT:
        opcode = PERI_CONTROL; sub = 0; payload[n++] = QBT_PERI_BT; payload[n++] = (uint8_t)r->value; break;
    default: return -EOPNOTSUPP;
    }
    size_t required = 6 + n, capacity = *length;
    *length = required;
    if (capacity < required) return -EMSGSIZE;
    out[0] = 0x31; out[1] = 0; out[2] = (uint8_t)opcode; out[3] = (uint8_t)(opcode >> 8);
    out[4] = (uint8_t)(n + 1); out[5] = sub;
    if (n) memcpy(out + 6, payload, n);
    return 0;
}

int qbt_peri_decode(const uint8_t *b, size_t n, struct qbt_peri_event *e) {
    if (!b || !e) return -EINVAL;
    if (n < 4 || n > QBT_PERI_PACKET_MAX || n != (size_t)b[3] + 4) return -EMSGSIZE;
    if (b[0] != 0x34 || b[1] != 0 || b[2] != 0xff) return -EPROTO;
    if (n < 6) return -EMSGSIZE;
    if (b[4] != 0xf0) return -EOPNOTSUPP;
    memset(e, 0, sizeof(*e));
    e->kind = b[5];
    switch (e->kind) {
    case QBT_PERI_COMPLETE:
        if (n < 10) return -EMSGSIZE;
        e->status = b[6]; e->credits = b[7]; e->opcode = le16(b + 8); e->has_opcode = e->has_status = true;
        break;
    case QBT_PERI_VENDOR:
        if (n < 11) return -EMSGSIZE;
        e->credits = b[6]; e->opcode = le16(b + 7); e->status = b[9]; e->subcommand = b[10];
        e->has_opcode = e->has_subcommand = e->has_status = true;
        if (e->opcode == PERI_EDL && e->subcommand >= 5 && e->subcommand <= 9) {
            if (n < 12) return -EMSGSIZE;
            e->subsystem = b[11]; e->has_subsystem = true;
        }
        /* For FFF0/0a, byte11 is the board-ID length, not SS. FFF1/02
         * carries the baud response without a proven selector field. */
        break;
    case QBT_PERI_ACTIVATE_NOTIFY:
        if (n < 8) return -EMSGSIZE;
        e->subsystem = b[6]; e->action = b[7]; e->has_subsystem = true;
        break; /* no opcode/subcommand/status: not a generic successful reply */
    case QBT_PERI_READY:
        if (n < 8) return -EMSGSIZE;
        e->status = b[6]; e->subsystem = b[7]; e->has_subsystem = e->has_status = true;
        break;
    default: return -EOPNOTSUPP;
    }
    e->length = n;
    memcpy(e->packet, b, n);
    return 0;
}

int qbt_peri_decode_bt_version(const uint8_t *b, size_t n, struct qbt_peri_bt_identity *v) {
    if (!b || !v) return -EINVAL;
    memset(v, 0, sizeof(*v));
    if (n < 7 || n != (size_t)b[2] + 3) return -EMSGSIZE;
    if (b[0] != 4 || b[1] != 0x0e || le16(b + 4) != 0xfc00) return -EPROTO;
    if (b[6]) return -EIO;
    if (n != 21) return -EMSGSIZE;
    if (b[7] != 0x19) return -EPROTO;
    /* btqca's edl_event_hdr has two bytes; for WCN7860 it skips one
     * additional byte before the twelve-byte qca_btsoc_version. Byte8 is
     * retained but is not given an invented status/security interpretation. */
    struct qbt_peri_bt_identity observed = {
        .product = le32(b + 9), .patch = le16(b + 13), .rom = le16(b + 15), .soc = le32(b + 17)
    };
    if (!observed.soc || !observed.rom) return -EILSEQ;
    *v = observed;
    return 0;
}

uint64_t qbt_peri_version_key(const struct qbt_peri_version *v) {
    return v ? (uint64_t)v->soc << 32 | (uint64_t)v->product << 16 | v->rom : 0;
}
int qbt_peri_tme_filename(const struct qbt_peri_version *v, const char **name) {
    if (!v || !name) return -EINVAL;
    *name = NULL;
    if (v->subsystem != QBT_PERI_TME || (v->product != 0x1e && v->product != 0x21)) return -EOPNOTSUPP;
    if (v->soc == 0x40210100 && v->rom == 0x0100) *name = "tmel_peach_10.elf";
    else if (v->soc == 0x40210200 && v->rom == 0x0200) *name = "tmel_peach_20.elf";
    else return -EOPNOTSUPP;
    return 0;
}

struct qbt_peri *qbt_peri_create(const struct qbt_peri_ops *ops, uint32_t baud) {
    if (!ops || !ops->uart.allocate || !ops->uart.release || !ops->uart.now_ns ||
        !ops->uart.uart_write || !ops->uart.pump || !ops->uart.wait_until || !baud) return NULL;
    struct qbt_peri *p = ops->uart.allocate(ops->uart.context, sizeof(*p));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p)); p->ops = *ops; p->info.host_baud = baud;
    return p;
}
int qbt_peri_destroy(struct qbt_peri *p) {
    if (!p) return 0;
    if (p->busy || p->info.access_held) return -EBUSY;
    p->ops.uart.release(p->ops.uart.context, p, sizeof(*p));
    return 0;
}
int qbt_peri_acquire(struct qbt_peri *p, uint64_t deadline) {
    if (!p) return -EINVAL;
    if (p->busy || p->info.hci_phase) return -EBUSY;
    if (p->info.error) return p->info.error;
    if (p->info.access_held) return 0;
    if (!p->ops.access) return -EOPNOTSUPP;
    if (now(p) >= deadline) return -ETIMEDOUT;
    p->busy = true;
    int e = p->ops.access(p->ops.uart.context, true, deadline);
    if (!e) p->info.access_held = true;
    if (!e && now(p) >= deadline) { qbt_peri_fail(p, -ETIMEDOUT); e = p->info.error; }
    p->busy = false;
    return e ? negative(e) : p->info.error;
}
static int release_access(struct qbt_peri *p, uint64_t deadline) {
    if (!p->info.access_held) return 0;
    if (now(p) >= deadline) return -ETIMEDOUT;
    int e = p->ops.access(p->ops.uart.context, false, deadline);
    if (!e) p->info.access_held = false;
    if (!e && now(p) >= deadline) e = -ETIMEDOUT;
    return e ? negative(e) : 0;
}
int qbt_peri_release(struct qbt_peri *p, uint64_t deadline) {
    if (!p) return -EINVAL;
    if (p->busy) return -EBUSY;
    p->busy = true;
    int e = release_access(p, deadline);
    p->busy = false;
    return e;
}
void qbt_peri_fail(struct qbt_peri *p, int error) {
    if (p && !p->info.error) { p->info.error = negative(error); p->info.bt_activated = false; }
}
void qbt_peri_get_info(const struct qbt_peri *p, struct qbt_peri_info *i) { if (p && i) *i = p->info; }
uint64_t qbt_peri_sequence(const struct qbt_peri *p) { return p ? p->sequence : 0; }
int qbt_peri_enter_hci(struct qbt_peri *p, size_t (*receiver)(void *, const uint8_t *, size_t)) {
    if (!p || !receiver) return -EINVAL;
    if (p->busy || p->waiting || p->activating || p->probing || p->proof_required || p->request_sending ||
        p->probe_sending || p->transmitting || p->info.access_held) return -EBUSY;
    if (p->info.error) return p->info.error;
    if (p->info.hci_phase) return receiver == p->hci_receiver ? 0 : -EBUSY;
    if (p->have || p->forward || p->ack_pending) return -EBUSY;
    p->hci_receiver = receiver; p->info.hci_phase = true;
    return 0;
}

static void frame_done(struct qbt_peri *p) {
    p->have = p->header = p->remaining = p->forward = p->forwarded = 0; p->streaming = false;
}
static size_t relay(struct qbt_peri *p, const uint8_t *bytes, size_t n) {
    size_t (*receiver)(void *, const uint8_t *, size_t) = p->info.hci_phase ? p->hci_receiver : p->ops.uart.receive;
    return receiver ? receiver(p->ops.uart.context, bytes, n) : n;
}
static bool flush_forward(struct qbt_peri *p) {
    while (p->forwarded < p->forward) {
        size_t n = p->forward - p->forwarded;
        n = relay(p, p->frame + p->forwarded, n);
        if (!n) return false;
        if (n > p->forward - p->forwarded) { qbt_peri_fail(p, -EIO); return false; }
        p->forwarded += n;
    }
    p->forward = p->forwarded = 0;
    if (!p->streaming || !p->remaining) frame_done(p);
    return true;
}
static void event_received(struct qbt_peri *p) {
    struct qbt_peri_event e;
    int error = qbt_peri_decode(p->frame, p->have, &e);
    p->info.packets++;
    if (error) {
        if (error == -EOPNOTSUPP) p->info.unmatched++; else p->info.malformed++;
        return;
    }
    e.sequence = p->frame_sequence;
    e.rx_position = p->frame_position; e.rx_position_valid = p->frame_stamped;
    p->info.last_kind = e.kind; if (e.has_status) p->info.last_status = e.status;
    if (e.has_opcode) p->info.last_credits = e.credits;
    if (e.kind == QBT_PERI_ACTIVATE_NOTIFY) {
        p->info.last_activation_subsystem = e.subsystem; p->info.last_activation_action = e.action;
        if (p->activating && !p->request_sending && e.rx_position_valid && e.rx_position >= p->command_rx_floor &&
            e.subsystem == QBT_PERI_BT && e.action == 1)
            p->info.activation_notice_seen = true;
    }
    if (e.kind == QBT_PERI_READY && valid_ss(e.subsystem)) {
        unsigned i = e.subsystem == QBT_PERI_TME;
        if (e.status) p->ready[i].failure = e.sequence; else p->ready[i].success = e.sequence;
    }
    if (p->download_active && e.sequence > p->download_floor && e.kind == QBT_PERI_VENDOR &&
        e.opcode == PERI_EDL && e.subcommand == 6 && e.has_subsystem &&
        e.subsystem == p->download_subsystem && e.status) qbt_peri_fail(p, -EIO);
    if (!p->waiting || p->request_sending || e.sequence <= p->command_floor || !e.has_opcode || e.opcode != p->expect_opcode) {
        p->info.unmatched++; return;
    }
    if (p->activating && (!e.rx_position_valid || e.rx_position < p->command_rx_floor)) {
        p->info.unmatched++; return;
    }
    if (e.kind == QBT_PERI_COMPLETE) {
        bool activation_reply = p->operation == QBT_PERI_ACTIVATE_BT;
        if (e.status && !(p->operation == QBT_PERI_BAUD && e.status == 1) && !activation_reply) {
            p->reply = e; p->info.matched++; qbt_peri_fail(p, -EIO);
        }
        else if (p->accept_complete) {
            p->reply = e; p->info.matched++; p->complete = true; p->waiting = false;
            if (p->activating) { p->info.activation_reply_seen = true; p->info.activation_reply_value = e.status; }
        }
        else p->info.unmatched++;
        return;
    }
    if (e.kind != QBT_PERI_VENDOR || e.subcommand != p->expect_subcommand ||
        (p->expect_selector && (!e.has_subsystem || e.subsystem != p->expect_subsystem))) {
        p->info.unmatched++; return;
    }
    p->reply = e; p->info.matched++;
    bool success = p->operation == QBT_PERI_ARBITRATE || p->operation == QBT_PERI_ACTIVATE_BT ||
                   e.status == (p->operation == QBT_PERI_BAUD ? 1 : 0);
    if (!success) qbt_peri_fail(p, -EIO); else { p->complete = true; p->waiting = false; }
    if (p->activating && success) { p->info.activation_reply_seen = true; p->info.activation_reply_value = e.status; }
}

static bool bt_version_received(struct qbt_peri *p) {
    if (!p->probing || p->probe_sending || p->probe_complete || p->info.error || !p->frame_stamped ||
        p->frame_position < p->probe_rx_floor || p->frame[1] != 0x0e ||
        p->have < 6 || le16(p->frame + 4) != 0xfc00) return false;
    /* A delayed download/board reply under FC00 cannot answer FC00/19.
     * Status-only real command failures carry no subcommand. */
    if (p->have >= 8 && p->frame[7] != 0x19) return false;
    p->info.bt_probe_seen = true;
    if (p->have >= 7) p->info.bt_probe_status = p->frame[6];
    int error = qbt_peri_decode_bt_version(p->frame, p->have, &p->info.bt);
    if (error) qbt_peri_fail(p, error); else p->probe_complete = true;
    return true; /* only this requested H4 response is consumed by the probe */
}

static size_t receive(struct qbt_peri *p, const uint8_t *data, size_t length, bool stamped, uint64_t position) {
    size_t used = 0;
    if (!p || (!data && length)) return 0;
    if (length && p->proof_required && !stamped) {
        p->info.activation_gap = QBT_PERI_ACT_RX_UNSTAMPED; qbt_peri_fail(p, -EOPNOTSUPP); return 0;
    }
    if (length && stamped) {
        if (length > UINT64_MAX - position || (p->rx_next_valid && position != p->rx_next)) {
            p->info.activation_gap = QBT_PERI_ACT_RX_UNORDERED; qbt_peri_fail(p, -EILSEQ); return 0;
        }
        if (!p->rx_next_valid) { p->rx_next = position; p->rx_next_valid = true; }
    } else if (length) p->rx_next_valid = false;
    for (;;) {
        if (p->forward && !flush_forward(p)) break;
        if (used == length) break;
        if (!p->have) {
            uint8_t type = data[used++]; p->info.rx_bytes++;
            if (p->info.hci_phase && (type == 0xfd || type == 0xfc || type == 0xfe)) {
                if (type == 0xfd) p->info.wake_indications++;
                else if (type == 0xfc) p->info.wake_acks++; else p->info.sleep_indications++;
                p->frame[0] = type; p->have = p->header = p->forward = 1;
                p->remaining = p->forwarded = 0; p->streaming = false;
                continue; /* core owns IBS; do not enqueue a PERI FC reply */
            }
            if (type == 0xfd) { p->ack_pending = true; p->info.wake_indications++; continue; }
            if (type == 0xfc) {
                p->info.wake_acks++; if (p->wake_waiting) p->wake_ack = true;
                continue;
            }
            if (type == 0xfe) { p->info.sleep_indications++; p->info.awake = false; continue; }
            if (type != 0x34 && type != 4 && type != 2 && type != 3 && type != 5) {
                p->info.malformed++; continue;
            }
            p->frame[0] = type; p->have = 1; p->frame_sequence = ++p->sequence;
            p->frame_stamped = stamped; p->frame_position = stamped ? position + used - 1 : 0;
            p->header = type == 4 ? 3 : (type == 0x34 || type == 3 ? 4 : 5);
            p->streaming = type == 2 || type == 5;
        }
        if (p->have < p->header) {
            size_t n = earlier(length - used, p->header - p->have);
            memcpy(p->frame + p->have, data + used, n); p->have += n; used += n; p->info.rx_bytes += n;
            if (p->have < p->header) break;
            if (p->frame[0] == 4) p->remaining = p->frame[2];
            else if (p->frame[0] == 0x34 || p->frame[0] == 3) p->remaining = p->frame[3];
            else p->remaining = le16(p->frame + 3) & (p->frame[0] == 5 ? 0x3fff : 0xffff);
            if (p->streaming) { if (!p->remaining) p->info.packets++; p->forward = p->header; continue; }
        }
        if (p->streaming) {
            size_t n = earlier(p->remaining, length - used), accepted = n;
            accepted = relay(p, data + used, n);
            if (!accepted) break;
            if (accepted > n) { qbt_peri_fail(p, -EIO); break; }
            used += accepted; p->info.rx_bytes += accepted; p->remaining -= accepted;
            if (!p->remaining) { p->info.packets++; frame_done(p); }
            continue;
        }
        size_t n = earlier(p->remaining, length - used);
        memcpy(p->frame + p->have, data + used, n); p->have += n; p->remaining -= n;
        used += n; p->info.rx_bytes += n;
        if (p->remaining) break;
        if (p->frame[0] == 0x34) { event_received(p); frame_done(p); }
        else {
            p->info.packets++;
            if (p->frame[0] == 4 && p->frame[1] == 0x10) qbt_peri_fail(p, -EIO);
            if (p->frame[0] == 4 && bt_version_received(p)) frame_done(p);
            else p->forward = p->have;
        }
    }
    if (stamped && length) p->rx_next += used;
    return used;
}
size_t qbt_peri_receive(struct qbt_peri *p, const uint8_t *data, size_t length) {
    return receive(p, data, length, false, 0);
}
size_t qbt_peri_receive_at(struct qbt_peri *p, const uint8_t *data, size_t length, uint64_t position) {
    return receive(p, data, length, true, position);
}

void qbt_peri_step(struct qbt_peri *p) {
    if (!p) return;
    qbt_peri_receive(p, NULL, 0);
    if (p->ack_pending && !p->info.hci_phase && !p->transmitting && !p->baud_changing && !p->info.error) {
        const uint8_t ack = 0xfc;
        size_t n = p->ops.uart.uart_write(p->ops.uart.context, &ack, 1);
        if (n == 1) { p->info.tx_bytes++; p->ack_pending = false; }
        else if (n > 1) qbt_peri_fail(p, -EIO);
    }
}
static void pump(struct qbt_peri *p) {
    p->ops.uart.pump(p->ops.uart.context);
    qbt_peri_step(p);
}
static int check(struct qbt_peri *p, uint64_t deadline) {
    if (p->info.error) return p->info.error;
    if (now(p) >= deadline) { qbt_peri_fail(p, -ETIMEDOUT); return p->info.error; }
    return 0;
}
static void wait_tick(struct qbt_peri *p, uint64_t deadline) {
    p->ops.uart.wait_until(p->ops.uart.context, earlier(deadline, now(p) + PERI_POLL_NS));
}
static int finish_controls(struct qbt_peri *p, uint64_t deadline) {
    if (check(p, deadline)) return p->info.error;
    while (p->ack_pending && !p->info.error) {
        pump(p);
        if (check(p, deadline)) break;
        if (p->ack_pending) wait_tick(p, deadline);
    }
    return p->info.error;
}
static int enter(struct qbt_peri *p, uint64_t deadline, bool require_awake) {
    if (!p) return -EINVAL;
    if (p->busy || p->info.hci_phase) return -EBUSY;
    if (p->info.error) return p->info.error;
    if (!p->info.access_held) return -EOPNOTSUPP;
    if (require_awake && !p->info.awake) return -EHOSTDOWN;
    if (check(p, deadline)) return p->info.error;
    p->busy = true; return 0;
}
static int validate_rx_mark(struct qbt_peri *p, uint64_t mark) {
    /* The callback does not parse RX. Already delivered positions therefore
     * remain a valid lower bound even if a new reply was produced before
     * the fenced callback returned. Do not compare with a later snapshot. */
    if (mark < p->info.rx_bytes || (p->rx_next_valid && mark < p->rx_next) ||
        (p->rx_mark_valid && mark < p->last_rx_mark)) {
        p->info.activation_gap = QBT_PERI_ACT_RX_MARK_INVALID;
        qbt_peri_fail(p, -EILSEQ); return p->info.error;
    }
    p->last_rx_mark = mark; p->rx_mark_valid = true;
    return 0;
}
static int producer_mark(struct qbt_peri *p, uint64_t *mark) {
    int error = p->ops.rx_produced(p->ops.uart.context, mark);
    if (error) {
        p->info.activation_gap = QBT_PERI_ACT_RX_SNAPSHOT_FAILED;
        qbt_peri_fail(p, error); return p->info.error;
    }
    return validate_rx_mark(p, *mark);
}
static int write_all(struct qbt_peri *p, const uint8_t *bytes, size_t length, uint64_t deadline) {
    size_t offset = 0;
    p->transmitting = true;
    while (offset < length) {
        if (check(p, deadline)) break;
        size_t n = 0;
        uint64_t admission = 0;
        if (p->proof_required) {
            int error = p->ops.uart_write_fenced(p->ops.uart.context, bytes + offset, length - offset, &n, &admission);
            if (n > length - offset || (error && n) || error > 0) {
                p->info.activation_gap = QBT_PERI_ACT_TX_FENCE_FAILED;
                qbt_peri_fail(p, -EIO); break;
            }
            if (error && error != -EAGAIN) {
                p->info.activation_gap = QBT_PERI_ACT_TX_FENCE_FAILED;
                qbt_peri_fail(p, error); break;
            }
        } else n = p->ops.uart.uart_write(p->ops.uart.context, bytes + offset, length - offset);
        if (n > length - offset) { qbt_peri_fail(p, -EIO); break; }
        offset += n; p->info.tx_bytes += n;
        if (n && p->proof_required && validate_rx_mark(p, admission)) break;
        if (p->info.error) break;
        if (offset == length) {
            /* Use the final fragment's admission mark. Reading rx_produced
             * here would wrongly exclude a real reply already queued while
             * the write callback was returning. */
            if (p->request_sending) {
                if (p->proof_required) p->command_rx_floor = admission;
                p->command_floor = p->sequence; p->request_sending = false;
            }
            if (p->probe_sending) {
                p->probe_rx_floor = admission;
                p->probe_floor = p->sequence; p->probe_sending = false;
            }
        }
        pump(p);
        if (!n && !p->info.error) wait_tick(p, deadline);
    }
    p->transmitting = false;
    if (!p->info.error) check(p, deadline);
    return p->info.error;
}
static int start_request(struct qbt_peri *p, const struct qbt_peri_request *r,
                          uint8_t *packet, size_t *length) {
    memset(&p->reply, 0, sizeof(p->reply));
    int e = qbt_peri_encode(r, packet, length);
    if (e) return e;
    pump(p); /* prior complete or partially received events cannot satisfy this request */
    if (p->info.error) return p->info.error;
    p->command_floor = p->sequence; p->operation = r->operation;
    p->expect_opcode = le16(packet + 2); p->expect_subcommand = packet[5]; p->expect_subsystem = r->subsystem;
    p->expect_selector = p->expect_opcode == PERI_EDL && packet[5] >= 5 && packet[5] <= 9;
    p->accept_complete = r->operation == QBT_PERI_RESET || r->operation == QBT_PERI_ACTIVATE_BT;
    p->complete = false; p->waiting = true;
    p->request_sending = true;
    return 0;
}
static int wait_reply(struct qbt_peri *p, uint64_t deadline) {
    if (check(p, deadline)) { p->waiting = false; return p->info.error; }
    while (!p->complete && !p->info.error) {
        if (check(p, deadline)) break;
        pump(p); if (!p->complete && !p->info.error) wait_tick(p, deadline);
    }
    p->waiting = false;
    if (!p->info.error) finish_controls(p, deadline);
    return p->info.error;
}
static int issue(struct qbt_peri *p, const struct qbt_peri_request *r, uint64_t deadline, bool reply) {
    uint8_t packet[QBT_PERI_PACKET_MAX]; size_t length = sizeof(packet);
    int e = start_request(p, r, packet, &length);
    if (e) return e;
    if (!reply) p->waiting = false;
    uint64_t command_deadline = earlier(deadline, now(p) + PERI_COMMAND_NS);
    e = write_all(p, packet, length, command_deadline);
    if (!e && reply) e = wait_reply(p, command_deadline);
    p->waiting = false;
    p->request_sending = false;
    return e;
}

int qbt_peri_wake(struct qbt_peri *p, uint64_t deadline) {
    int e = enter(p, deadline, false);
    if (e) return e;
    pump(p); p->wake_ack = false; p->wake_waiting = true; p->info.awake = false;
    const uint8_t wake = 0xfd;
    for (unsigned i = 0; i < 10 && !p->wake_ack && !p->info.error; i++) {
        /* UART backpressure uses the total bound; an unsent FD is not a retry. */
        p->info.wake_requests++;
        e = write_all(p, &wake, 1, deadline);
        if (e) break;
        uint64_t retry = earlier(deadline, now(p) + PERI_WAKE_NS);
        while (!p->wake_ack && !p->info.error && now(p) < retry) { pump(p); if (!p->wake_ack) wait_tick(p, retry); }
        if (now(p) >= deadline && !p->wake_ack) qbt_peri_fail(p, -ETIMEDOUT);
    }
    if (!p->wake_ack && !p->info.error) qbt_peri_fail(p, -ETIMEDOUT);
    if (!p->info.error) finish_controls(p, deadline);
    if (!p->info.error) p->info.awake = true;
    p->wake_waiting = false; p->busy = false;
    return p->info.error;
}
int qbt_peri_command(struct qbt_peri *p, const struct qbt_peri_request *r,
                     uint64_t deadline, struct qbt_peri_event *reply) {
    if (reply) memset(reply, 0, sizeof(*reply));
    if (!r) return -EINVAL;
    if (r->operation == QBT_PERI_BAUD || r->operation == QBT_PERI_RESET || r->operation == QBT_PERI_ACTIVATE_BT)
        return -EOPNOTSUPP;
    int e = enter(p, deadline, true); if (e) return e;
    e = issue(p, r, deadline, true);
    if (reply) *reply = p->reply;
    p->busy = false;
    return e;
}
int qbt_peri_set_baud(struct qbt_peri *p, uint32_t baud, uint64_t deadline) {
    uint8_t packet[QBT_PERI_PACKET_MAX]; size_t length = sizeof(packet);
    const struct qbt_peri_request r = { .operation = QBT_PERI_BAUD, .subsystem = QBT_PERI_SS, .value = baud };
    int e = qbt_peri_encode(&r, packet, &length); if (e) return e;
    if (!p) return -EINVAL;
    if (!p->ops.uart.uart_flow || !p->ops.uart.uart_speed || !p->ops.uart.uart_drained) return -EOPNOTSUPP;
    if (p->busy || p->info.hci_phase) return -EBUSY;
    if (p->info.error) return p->info.error;
    if (baud == p->info.host_baud) return -EALREADY;
    e = enter(p, deadline, true); if (e) return e;
    p->info.baud_verified = false; p->info.cleanup_error = 0;
    bool flow_off = false;
    e = start_request(p, &r, packet, &length);
    if (!e) {
        e = p->ops.uart.uart_flow(p->ops.uart.context, false);
        if (e) e = negative(e); else { flow_off = true; p->baud_changing = true; }
    }
    if (!e) e = write_all(p, packet, length, deadline);
    if (!e) {
        uint64_t settled = now(p) + PERI_BAUD_DELAY_NS;
        while (!p->info.error && now(p) < settled) {
            if (check(p, deadline)) break;
            pump(p); if (!p->info.error) wait_tick(p, earlier(deadline, settled));
        }
        e = p->info.error;
    }
    if (!e) {
        while (!p->ops.uart.uart_drained(p->ops.uart.context) && !p->info.error) {
            if (check(p, deadline)) break;
            pump(p); if (!p->info.error) wait_tick(p, deadline);
        }
        e = p->info.error;
    }
    if (!e && !(e = check(p, deadline))) {
        e = p->ops.uart.uart_speed(p->ops.uart.context, baud);
        if (!e) p->info.host_baud = baud; else e = negative(e);
    }
    if (flow_off) {
        int restore = p->ops.uart.uart_flow(p->ops.uart.context, true);
        if (restore) { p->info.cleanup_error = negative(restore); if (!e) e = p->info.cleanup_error; }
    }
    p->baud_changing = false;
    if (e) qbt_peri_fail(p, e);
    else e = wait_reply(p, earlier(deadline, now(p) + PERI_COMMAND_NS));
    if (!e) p->info.baud_verified = true;
    p->waiting = false; p->request_sending = false; p->busy = false;
    return e;
}

int qbt_peri_read_version(struct qbt_peri *p, uint8_t ss, uint64_t deadline, struct qbt_peri_version *v) {
    if (!v) return -EINVAL;
    memset(v, 0, sizeof(*v));
    const struct qbt_peri_request r = { .operation = QBT_PERI_VERSION, .subsystem = ss };
    struct qbt_peri_event e;
    int error = qbt_peri_command(p, &r, deadline, &e); if (error) return error;
    if (e.length < 25) { qbt_peri_fail(p, -EMSGSIZE); return -EMSGSIZE; }
    v->product = le32(e.packet + 12); v->patch = le16(e.packet + 16); v->rom = le16(e.packet + 18);
    v->soc = le32(e.packet + 20); v->security = e.packet[24]; v->subsystem = ss;
    return 0;
}
int qbt_peri_arbitrate(struct qbt_peri *p, uint8_t ss, uint64_t deadline, uint8_t *state) {
    if (!state) return -EINVAL;
    *state = 0xff;
    const struct qbt_peri_request r = { .operation = QBT_PERI_ARBITRATE, .subsystem = ss };
    struct qbt_peri_event e;
    int error = qbt_peri_command(p, &r, deadline, &e); if (error) return error;
    *state = e.status;
    if (*state == 0 || *state == 0x0b || *state == 0x17 || (ss == QBT_PERI_TME && (*state == 5 || *state == 6))) return 0;
    qbt_peri_fail(p, -EOPNOTSUPP); return -EOPNOTSUPP;
}
int qbt_peri_read_board_id(struct qbt_peri *p, uint64_t deadline, uint16_t *board) {
    if (!board) return -EINVAL;
    const struct qbt_peri_request r = { .operation = QBT_PERI_BOARD_ID, .subsystem = QBT_PERI_SS };
    struct qbt_peri_event e;
    int error = qbt_peri_command(p, &r, deadline, &e); if (error) return error;
    if (e.length < 14 || e.packet[11] != 2) { qbt_peri_fail(p, -EMSGSIZE); return -EMSGSIZE; }
    *board = (uint16_t)((uint16_t)e.packet[12] << 8 | e.packet[13]);
    return 0;
}
int qbt_peri_read_build_info(struct qbt_peri *p, uint8_t ss, uint64_t deadline, char *out, size_t capacity) {
    if (!out || !capacity) return -EINVAL;
    out[0] = 0;
    const struct qbt_peri_request r = { .operation = QBT_PERI_BUILD_INFO, .subsystem = ss };
    struct qbt_peri_event e;
    int error = qbt_peri_command(p, &r, deadline, &e); if (error) return error;
    if (e.length < 13 || (size_t)e.packet[12] > e.length - 13) { qbt_peri_fail(p, -EMSGSIZE); return -EMSGSIZE; }
    size_t n = e.packet[12];
    if (n >= capacity) return -EMSGSIZE;
    memcpy(out, e.packet + 13, n); out[n] = 0;
    return 0;
}

static int blob_policy(uint8_t ss, const struct qbt_peri_blob *b, bool *final_only, bool *elf) {
    if (!valid_ss(ss)) return -EOPNOTSUPP;
    if (!b || !b->data || !b->length) return -ENOENT;
    const uint8_t *d = b->data; size_t n = b->length;
    *elf = n >= 4 && !memcmp(d, "\177ELF", 4);
    if (*elf) {
        if (n < 16 || d[5] != 1 || d[6] != 1 || (d[4] != 1 && d[4] != 2) || n < (d[4] == 1 ? 52u : 64u)) return -EMSGSIZE;
        if (b->elf_config == QBT_PERI_ELF_NO_VALUE) *final_only = true;
        else if (b->elf_config == QBT_PERI_ELF_EXPLICIT) *final_only = (b->config_value & 1) != 0;
        else return -EOPNOTSUPP;
        return 0;
    }
    if (n < 4 || n - 4 != ((size_t)d[1] | (size_t)d[2] << 8 | (size_t)d[3] << 16)) return -EMSGSIZE;
    if (d[0] == 1) {
        if (n < 28) return -EMSGSIZE;
        if (d[14] != 0 && d[14] != 3) return -EOPNOTSUPP;
        *final_only = d[14] == 3;
    } else if (d[0] == 2) {
        if (ss == QBT_PERI_TME) return -EOPNOTSUPP;
        *final_only = false;
    } else return -EOPNOTSUPP;
    return 0;
}
int qbt_peri_download(struct qbt_peri *p, uint8_t ss, const struct qbt_peri_blob *b, uint64_t deadline) {
    bool final_only, elf;
    int error = blob_policy(ss, b, &final_only, &elf); if (error) return error;
    error = enter(p, deadline, true); if (error) return error;
    pump(p);
    p->download_active = true; p->download_subsystem = ss; p->download_floor = p->sequence;
    if (elf && b->elf_config == QBT_PERI_ELF_EXPLICIT) {
        const struct qbt_peri_request r = { .operation = QBT_PERI_PATCH_CONFIG, .subsystem = ss, .value = b->config_value };
        error = issue(p, &r, deadline, true);
    }
    for (size_t offset = 0; !error && offset < b->length; ) {
        size_t n = earlier(b->length - offset, QBT_PERI_SEGMENT_MAX);
        const struct qbt_peri_request r = { .operation = QBT_PERI_SEGMENT, .subsystem = ss, .data = b->data + offset, .length = n };
        error = issue(p, &r, deadline, !final_only || offset + n == b->length);
        offset += n;
    }
    p->waiting = false; p->download_active = false; p->busy = false;
    return error;
}
static int wait_ready(struct qbt_peri *p, uint8_t ss, uint64_t floor, uint64_t deadline) {
    unsigned i = ss == QBT_PERI_TME;
    for (;;) {
        if (check(p, deadline)) return p->info.error;
        if (p->ready[i].failure > floor) { qbt_peri_fail(p, -EIO); return p->info.error; }
        if (p->ready[i].success > floor) return finish_controls(p, deadline);
        pump(p); if (!p->info.error && p->ready[i].success <= floor && p->ready[i].failure <= floor) wait_tick(p, deadline);
    }
}
int qbt_peri_wait_ready(struct qbt_peri *p, uint8_t ss, uint64_t floor, uint64_t deadline) {
    if (!valid_ss(ss)) return -EOPNOTSUPP;
    int e = enter(p, deadline, false); if (e) return e;
    e = wait_ready(p, ss, floor, deadline); p->busy = false;
    return e;
}
int qbt_peri_reset(struct qbt_peri *p, uint64_t deadline) {
    int e = enter(p, deadline, true); if (e) return e;
    pump(p);
    const struct qbt_peri_request r = { .operation = QBT_PERI_RESET, .subsystem = QBT_PERI_SS };
    e = issue(p, &r, deadline, true);
    if (!e) e = wait_ready(p, QBT_PERI_SS, p->command_floor, deadline);
    p->info.awake = false; p->busy = false;
    return e;
}
static int quiesce(struct qbt_peri *p, uint64_t deadline) {
    for (;;) {
        if (check(p, deadline)) return p->info.error;
        uint64_t received = p->info.rx_bytes;
        pump(p);
        if (finish_controls(p, deadline)) return p->info.error;
        /* An RX-idle pump, no partial/blocked frame and drained TX form the
         * phase boundary. A source that keeps delivering cannot masquerade
         * as drained; the common deadline still bounds this wait. */
        if (received == p->info.rx_bytes && !p->have && !p->forward &&
            p->ops.uart.uart_drained(p->ops.uart.context)) return check(p, deadline);
        wait_tick(p, deadline);
    }
}
int qbt_peri_activate_bt(struct qbt_peri *p, uint64_t deadline) {
    if (!p) return -EINVAL;
    if (!p->ops.uart.uart_drained) { p->info.activation_gap = QBT_PERI_ACT_NO_UART_DRAIN; return -EOPNOTSUPP; }
    if (!p->ops.rx_produced) { p->info.activation_gap = QBT_PERI_ACT_NO_RX_PRODUCER; return -EOPNOTSUPP; }
    if (!p->ops.uart_write_fenced) { p->info.activation_gap = QBT_PERI_ACT_NO_TX_FENCE; return -EOPNOTSUPP; }
    int error = enter(p, deadline, true); if (error) return error;
    p->info.activation_stage = QBT_PERI_ACT_COMMAND;
    p->info.activation_gap = QBT_PERI_ACT_GAP_NONE; p->proof_required = true;
    p->info.activation_reply_seen = p->info.activation_notice_seen = p->info.bt_probe_seen = p->info.bt_activated = false;
    p->info.activation_reply_value = p->info.bt_probe_status = 0xff;
    memset(&p->info.bt, 0, sizeof(p->info.bt));
    error = quiesce(p, deadline);
    uint64_t initial_mark;
    if (!error) error = producer_mark(p, &initial_mark);
    const struct qbt_peri_request r = { .operation = QBT_PERI_ACTIVATE_BT, .subsystem = QBT_PERI_BT, .value = 1 };
    uint8_t request[QBT_PERI_PACKET_MAX]; size_t length = sizeof(request);
    if (!error) error = start_request(p, &r, request, &length);
    if (!error) {
        p->activating = true;
        uint64_t bound = earlier(deadline, now(p) + PERI_COMMAND_NS);
        error = write_all(p, request, length, bound);
        if (!error) error = wait_reply(p, bound);
        if (!error && p->info.activation_reply_value != 0x17) {
            p->info.activation_stage = QBT_PERI_ACT_NOTICE;
            while (!p->info.activation_notice_seen && !p->info.error) {
                if (check(p, bound)) break;
                pump(p); if (!p->info.activation_notice_seen && !p->info.error) wait_tick(p, bound);
            }
            error = finish_controls(p, bound);
        }
    }
    p->waiting = false; p->activating = false;
    if (!error) error = quiesce(p, deadline);
    if (!error) {
        p->info.activation_stage = QBT_PERI_ACT_RELEASE;
        error = release_access(p, deadline);
        if (error) qbt_peri_fail(p, error);
        else error = check(p, deadline);
    }
    if (!error) {
        /* This real query is required even after skip-notice reply17;
         * an opcode-less notification can be delayed from another request. */
        const uint8_t version_request[] = {1, 0, 0xfc, 1, 0x19};
        p->info.activation_stage = QBT_PERI_ACT_PROBE;
        p->probe_floor = p->sequence; p->probe_complete = false; p->probing = p->probe_sending = true;
        uint64_t bound = earlier(deadline, now(p) + PERI_COMMAND_NS);
        error = write_all(p, version_request, sizeof(version_request), bound);
        while (!error && !p->probe_complete) {
            error = check(p, bound); if (error) break;
            pump(p); error = p->info.error;
            if (!error && !p->probe_complete) wait_tick(p, bound);
        }
        if (!error) error = finish_controls(p, bound);
        p->probing = false;
    }
    if (!error) error = quiesce(p, deadline);
    if (!error && p->ops.verify_bt) {
        p->info.activation_stage = QBT_PERI_ACT_VERIFY;
        error = p->ops.verify_bt(p->ops.uart.context, &p->info.bt, deadline);
        if (error) { error = negative(error); qbt_peri_fail(p, error); }
        else error = check(p, deadline);
    }
    if (!error) {
        p->info.activation_stage = QBT_PERI_ACT_VERIFIED;
        p->info.bt_activated = true;
    }
    p->waiting = p->activating = p->probing = p->request_sending = p->probe_sending = p->proof_required = false; p->busy = false;
    return error;
}
