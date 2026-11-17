/* SPDX-License-Identifier: GPL-2.0-only */
/* Synthetic controller; literal frames are derived from the recorded HAL
 * layouts. This test never loads private firmware or opens a device. */
#include "qbt_peri.h"
#include "qbt_internal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS 1000000ULL
struct model {
    struct qbt_peri *p;
    struct qbt *core;
    uint64_t time, baud_written, drained_at, speed_at, unblock_at, cancel_at, ack_unblock_at, late_time;
    bool wake_ack, suppress, cc_only, status_only, wrong_opcode, wrong_sub, wrong_ss, short_reply;
    bool truncate_version, extra_wrong, hardware_error, cancel, final_only, drop_final, fail_final, fail_early;
    bool reset_ready, ready_wrong_ss, ready_error, lease, fail_release, flow, drop_reset_reply;
    bool baud_pending, no_baud_reply, fail_speed, fail_flow_on, no_drain, rx_blocked;
    bool incoming_wake_partial, partial_wake_sent, release_cancel, stall_tx, reenter, reentered, old_reply_tail;
    bool late_wake, late_ready, stale_ready_before_tx, stale_ready_sent;
    bool drop_activation_reply, drop_activation_notice, activation_notice_first, activation_vendor_reply, bad_activation_opcode;
    bool drop_bt_reply, bad_bt_opcode, bad_bt_subcommand, short_bt_reply, fail_bt_reply, zero_bt_soc, zero_bt_rom;
    bool old_bt_reply, reject_bt_identity, cancel_bt_verify, late_bt_verify, reenter_bt_verify;
    bool early_activation_notice, early_activation_sent, early_bt_reply, early_bt_sent;
    bool buffered_old_notice, buffered_old_bt, defer_response, legacy_rx, bad_rx_mark, fail_rx_snapshot;
    bool fast_activation_reply, fast_bt_reply, fence_probe_only, fence_bad_count, fence_bad_mark, fence_error_count, fence_positive_result, fence_cancel;
    bool mixed_hci, block_hci;
    uint64_t hci_unblock_at, fence_unblock_at;
    size_t hci_limit;
    uint8_t pending_response[512]; size_t pending_response_length;
    uint8_t activation_reply_value, activation_notice_ss, activation_notice_action;
    unsigned activation_commands, bt_probes, bt_verify_calls;
    unsigned fenced_calls, fenced_admissions, fenced_act_admissions, fenced_bt_admissions, fence_retries;
    unsigned fast_activation_returns, fast_bt_returns, rx_snapshot_calls;
    int reenter_result, fence_error;
    uint32_t expected_config;
    uint8_t status, arb_state, command[259], queue[65536], forwarded[8192], download[4096];
    size_t command_have, command_need, queue_length, queue_offset, forwarded_length, download_length, download_expected;
    size_t write_limit, rx_limit, forward_limit;
    unsigned allocations, released, commands, segments, configs, host_speed_calls, flow_off_calls, flow_on_calls, ack_tx, wake_tx, pumps;
};
static void queue_bytes(struct model *m, const uint8_t *b, size_t n) {
    if (m->defer_response) {
        assert(m->pending_response_length + n <= sizeof(m->pending_response));
        memcpy(m->pending_response + m->pending_response_length, b, n); m->pending_response_length += n; return;
    }
    assert(m->queue_length + n <= sizeof(m->queue));
    memcpy(m->queue + m->queue_length, b, n); m->queue_length += n;
}
static void produce_pending(struct model *m) {
    if (m->pending_response_length) {
        size_t n = m->pending_response_length; m->pending_response_length = 0;
        queue_bytes(m, m->pending_response, n);
    }
}
static void queue_ready(struct model *m, uint8_t status, uint8_t ss) {
    const uint8_t b[] = { 0x34, 0, 0xff, 4, 0xf0, 3, status, ss };
    queue_bytes(m, b, sizeof(b));
}
static void queue_cc(struct model *m, uint16_t opcode, uint8_t status) {
    const uint8_t b[] = { 0x34, 0, 0xff, 6, 0xf0, 0, status, 0, (uint8_t)opcode, (uint8_t)(opcode >> 8) };
    queue_bytes(m, b, sizeof(b));
}
static const uint8_t bt_version_reply[] = {
    4, 0x0e, 0x12, 0, 0, 0xfc, 0, 0x19, 0xa5,
    0x1e, 0, 0, 0, 0x34, 0x12, 0, 2, 0, 2, 0x21, 0x40
};
static void queue_bt_version(struct model *m) {
    if (m->drop_bt_reply) return;
    uint8_t b[sizeof(bt_version_reply)]; memcpy(b, bt_version_reply, sizeof(b)); size_t n = sizeof(b);
    if (m->bad_bt_opcode) b[4] = 0x48;
    if (m->bad_bt_subcommand) b[7] = 0x1e;
    if (m->zero_bt_soc) memset(b + 17, 0, 4);
    if (m->zero_bt_rom) memset(b + 15, 0, 2);
    if (m->short_bt_reply) n--;
    if (m->fail_bt_reply) { b[6] = 1; n = 7; }
    b[2] = (uint8_t)(n - 3); queue_bytes(m, b, n);
}
static void queue_activation_notice(struct model *m) {
    const uint8_t b[] = {0x34, 0, 0xff, 4, 0xf0, 2, m->activation_notice_ss, m->activation_notice_action};
    queue_bytes(m, b, sizeof(b));
}
static void reply(struct model *m, uint16_t opcode, uint8_t sub, uint8_t ss, uint8_t status) {
    if (m->suppress) return;
    if (m->hardware_error) {
        const uint8_t b[] = { 4, 0x10, 1, 0x11 }; queue_bytes(m, b, sizeof(b)); return;
    }
    if (m->cc_only) { queue_cc(m, opcode, status); return; }
    if (m->status_only) {
        const uint8_t b[] = { 0x34, 0, 0xff, 4, 0xf0, 2, 0, 0x17 };
        queue_bytes(m, b, sizeof(b)); return;
    }
    /* Real NC is retained as0 here, deliberately not fixed to1. */
    uint8_t b[40] = { 0x34, 0, 0xff, 8, 0xf0, 1, 0, (uint8_t)opcode, (uint8_t)(opcode >> 8), status, sub, ss };
    size_t n = 12;
    if (sub == 5) {
        const uint8_t version[] = { 0x1e, 0, 0, 0, 0x34, 0x12, 0, 2, 0, 2, 0x21, 0x40, 3 };
        memcpy(b + 12, version, sizeof(version)); n = 25;
        if (m->truncate_version) n--;
    } else if (sub == 0x0a) { b[11] = 2; b[12] = 0; b[13] = 0x45; n = 14; }
    else if (sub == 9) { b[12] = 7; memcpy(b + 13, "fixture", 7); n = 20; }
    if (m->wrong_opcode) b[7] ^= 4;
    if (m->wrong_sub) b[10] ^= 1;
    if (m->wrong_ss) b[11] = ss == 3 ? 0 : 3;
    if (m->short_reply) n = 11;
    b[3] = (uint8_t)(n - 4);
    if (m->extra_wrong) {
        b[11] = ss == 3 ? 0 : 3; queue_bytes(m, b, n); b[11] = ss;
    }
    queue_bytes(m, b, n);
}
static void command_sent(struct model *m) {
    const uint8_t *c = m->command;
    m->commands++;
    m->defer_response = true; /* write_fenced may produce these before returning */
    if (c[0] == 1) {
        if (c[1] == 3 && c[2] == 0x0c && c[3] == 0) {
            assert(m->core && !m->lease && m->command_have == 4);
            if (m->mixed_hci) {
                const uint8_t peri[] = {0x34, 0, 0xff, 4, 0xf0, 2, 1, 1};
                queue_bytes(m, peri, sizeof(peri));
                const uint8_t wake = 0xfd; queue_bytes(m, &wake, 1);
            }
            const uint8_t reset_ack[] = {4, 0x0e, 4, 1, 3, 0x0c, 0}; queue_bytes(m, reset_ack, sizeof(reset_ack));
            m->command_have = m->command_need = 0; m->defer_response = false; return;
        }
        /* Genuine first BT H4 probe must follow actual PERI access release;
         * it uses the same host rate and UART, not another channel. */
        const uint8_t expected[] = {1, 0, 0xfc, 1, 0x19};
        assert(!m->lease && m->activation_commands == 1 && m->command_have == sizeof(expected));
        assert(!memcmp(c, expected, sizeof(expected)));
        m->bt_probes++; queue_bt_version(m); m->command_have = m->command_need = 0; m->defer_response = false; return;
    }
    assert(c[0] == 0x31 && c[1] == 0 && c[3] == 0xff);
    assert(m->command_have == (size_t)c[4] + 5);
    if (c[2] == 0xf1 && c[5] == 2) {
        assert(m->command_have == 7 && c[4] == 2 && c[6] == 0x11 && !m->flow);
        m->baud_pending = true; m->baud_written = m->time;
    } else if (c[2] == 0xf1 && c[5] == 3) {
        assert(m->command_have == 6 && c[4] == 1);
        if (!m->drop_reset_reply) queue_cc(m, 0xfff1, m->status);
        if (m->reset_ready) queue_ready(m, m->ready_error ? 0x17 : 0, m->ready_wrong_ss ? 3 : 0);
    } else if (c[2] == 0xf1 && c[5] == 0) {
        const uint8_t expected[] = {0x31, 0, 0xf1, 0xff, 3, 0, 1, 1};
        assert(m->lease && m->command_have == sizeof(expected) && !memcmp(c, expected, sizeof(expected)));
        m->activation_commands++;
        if (m->activation_notice_first && !m->drop_activation_notice) queue_activation_notice(m);
        if (!m->drop_activation_reply) {
            if (m->activation_vendor_reply) reply(m, 0xfff1, 0, 1, m->activation_reply_value);
            else queue_cc(m, m->bad_activation_opcode ? 0xfff0 : 0xfff1, m->activation_reply_value);
        }
        if (!m->activation_notice_first && !m->drop_activation_notice) queue_activation_notice(m);
        if (m->old_bt_reply) queue_bytes(m, bt_version_reply, sizeof(bt_version_reply));
    } else {
        assert(c[2] == 0xf0);
        if (c[5] == 6) {
            assert(c[4] == c[7] + 3 && c[7] && c[7] <= 243);
            assert(m->download_length + c[7] <= sizeof(m->download));
            memcpy(m->download + m->download_length, c + 8, c[7]); m->download_length += c[7]; m->segments++;
            bool last = m->download_length == m->download_expected;
            if (m->fail_early && m->segments == 1) reply(m, 0xfff0, 6, c[6], 0x17);
            else if ((!m->final_only || last) && !(last && m->drop_final)) reply(m, 0xfff0, 6, c[6], last && m->fail_final ? 0x17 : 0);
        } else if (c[5] == 7) {
            assert(m->command_have == 11 && c[4] == 6); m->configs++;
            assert(c[7] == (uint8_t)m->expected_config && c[8] == (uint8_t)(m->expected_config >> 8) &&
                   c[9] == (uint8_t)(m->expected_config >> 16) && c[10] == (uint8_t)(m->expected_config >> 24));
            reply(m, 0xfff0, 7, c[6], m->status);
        } else if (c[5] == 0x0a) {
            assert(m->command_have == 6 && c[4] == 1); reply(m, 0xfff0, 0x0a, 0, m->status);
        } else if (c[5] == 8) {
            assert(m->command_have == 8 && c[4] == 3 && c[7] == 0); reply(m, 0xfff0, 8, c[6], m->arb_state);
        } else {
            assert(m->command_have == 7 && c[4] == 2 && (c[5] == 5 || c[5] == 9));
            reply(m, 0xfff0, c[5], c[6], m->status);
            if (m->old_reply_tail) { const uint8_t tail[] = {5, 0}; queue_bytes(m, tail, sizeof(tail)); }
        }
    }
    m->command_have = m->command_need = 0;
    m->defer_response = false;
}
static size_t write_uart(void *context, const uint8_t *b, size_t n) {
    struct model *m = context;
    if (m->write_limit && n > m->write_limit) n = m->write_limit;
    if (!m->command_have && b[0] == 0xfc && m->time < m->ack_unblock_at) return 0;
    if (m->stall_tx) return 0;
    if (m->cancel) { qbt_peri_fail(m->p, -ECANCELED); return 0; }
    for (size_t i = 0; i < n; i++) {
        if (!m->command_have && b[i] == 0xfd) {
            m->wake_tx++;
            if (m->wake_ack) { const uint8_t ack = 0xfc; queue_bytes(m, &ack, 1); }
            continue;
        }
        if (!m->command_have && b[i] == 0xfc) {
            assert(!m->baud_pending || (m->host_speed_calls && m->flow)); m->ack_tx++; continue;
        }
        assert(m->command_have < sizeof(m->command));
        m->command[m->command_have++] = b[i];
        if (m->command[0] == 1 && m->command_have == 4) m->command_need = m->command[3] + 4;
        else if (m->command[0] == 0x31 && m->command_have == 5) m->command_need = m->command[4] + 5;
        if (m->command_need && m->command_have == m->command_need) command_sent(m);
    }
    return n;
}
static int write_fenced(void *context, const uint8_t *b, size_t n, size_t *consumed, uint64_t *rx_before_admission) {
    struct model *m = context;
    bool activation = m->lease, faulty = !m->fence_probe_only || !activation;
    *consumed = 0; *rx_before_admission = m->queue_length; m->fenced_calls++;
    if (faulty) {
        if (m->time < m->fence_unblock_at || m->fence_retries) {
            if (m->fence_retries) m->fence_retries--;
            return -EAGAIN;
        }
        if (m->fence_cancel) { qbt_peri_fail(m->p, -ECANCELED); return -EAGAIN; }
        if (m->fence_error) { if (m->fence_error_count) *consumed = 1; return m->fence_error; }
        if (m->fence_bad_count) { *consumed = n + 1; return 0; }
        if (m->fence_positive_result) return 1;
    }
    /* Snapshot before this fragment's TX admission. The producer is independent
     * of parsing; do not invoke pump/receive within the fenced callback. */
    *consumed = write_uart(context, b, n);
    if (*consumed) {
        m->fenced_admissions++;
        if (activation) m->fenced_act_admissions++; else m->fenced_bt_admissions++;
        if (faulty && m->fence_bad_mark) *rx_before_admission = 0;
    }
    if (m->pending_response_length && (activation ? m->fast_activation_reply : m->fast_bt_reply)) {
        assert(*consumed && m->queue_length == *rx_before_admission);
        if (activation) m->fast_activation_returns++; else m->fast_bt_returns++;
        produce_pending(m); /* actual RX bytes exist before this call returns */
        assert(m->queue_length > *rx_before_admission);
    }
    return *consumed ? 0 : -EAGAIN;
}
static void *allocate(void *context, size_t n) { ((struct model *)context)->allocations++; return calloc(1, n); }
static void deallocate(void *context, void *p, size_t n) { (void)n; ((struct model *)context)->released++; free(p); }
static uint64_t clock_now(void *context) { return ((struct model *)context)->time; }
static void sleep_until(void *context, uint64_t until) {
    struct model *m = context; assert(until >= m->time);
    /* Queued DMA/space events wake the existing core before its timer. */
    if (m->core && (m->queue_offset < m->queue_length || m->pending_response_length) && until > m->time + MS) m->time += MS;
    else if (m->core && m->block_hci && m->time < m->hci_unblock_at && until > m->hci_unblock_at) m->time = m->hci_unblock_at;
    else m->time = until;
}
static size_t forward(void *context, const uint8_t *b, size_t n) {
    struct model *m = context;
    if (m->rx_blocked && m->time < m->unblock_at) return 0;
    if (m->forward_limit && n > m->forward_limit) n = m->forward_limit;
    assert(m->forwarded_length + n <= sizeof(m->forwarded));
    memcpy(m->forwarded + m->forwarded_length, b, n); m->forwarded_length += n;
    return n;
}
static void pump_model(void *context) {
    struct model *m = context;
    m->pumps++;
    produce_pending(m);
    if (m->early_activation_notice && !m->early_activation_sent && m->command_have && m->command[0] == 0x31 && !m->activation_commands) {
        if (m->buffered_old_notice) { const uint8_t blocker[] = {4, 0xff, 4, 0x99, 0, 0, 0}; queue_bytes(m, blocker, sizeof(blocker)); }
        queue_activation_notice(m); m->early_activation_sent = true;
    }
    if (m->early_bt_reply && !m->early_bt_sent && m->command_have && m->command[0] == 1 && !m->bt_probes) {
        if (m->buffered_old_bt) { const uint8_t blocker[] = {4, 0xff, 4, 0x99, 0, 0, 0}; queue_bytes(m, blocker, sizeof(blocker)); }
        queue_bytes(m, bt_version_reply, sizeof(bt_version_reply)); m->early_bt_sent = true;
    }
    if (m->late_wake && m->wake_tx && m->time) {
        m->late_wake = false; m->time = m->late_time;
        const uint8_t ack = 0xfc; queue_bytes(m, &ack, 1);
    }
    if (m->late_ready) { m->late_ready = false; m->time = m->late_time; queue_ready(m, 0, 3); }
    if (m->stale_ready_before_tx && m->pumps == 2 && !m->commands) {
        queue_ready(m, 0, 0); m->stale_ready_sent = true;
    }
    if (m->cancel_at && m->time >= m->cancel_at) qbt_peri_fail(m->p, -ECANCELED);
    if (m->reenter && !m->reentered) {
        m->reentered = true;
        const struct qbt_peri_request r = { .operation = QBT_PERI_VERSION, .subsystem = 0 };
        m->reenter_result = qbt_peri_command(m->p, &r, m->time + MS, NULL);
        assert(qbt_peri_destroy(m->p) == -EBUSY && qbt_peri_release(m->p, m->time + MS) == -EBUSY);
    }
    if (m->incoming_wake_partial && !m->partial_wake_sent && m->command_have) {
        const uint8_t wake = 0xfd; queue_bytes(m, &wake, 1); m->partial_wake_sent = true;
    }
    size_t n = m->queue_length - m->queue_offset;
    if (m->rx_limit && n > m->rx_limit) n = m->rx_limit;
    if (n) m->queue_offset += m->legacy_rx ? qbt_peri_receive(m->p, m->queue + m->queue_offset, n) :
        qbt_peri_receive_at(m->p, m->queue + m->queue_offset, n, m->queue_offset);
    if (m->core) qbt_peri_step(m->p);
}
static int rx_produced(void *context, uint64_t *count) {
    struct model *m = context;
    m->rx_snapshot_calls++;
    if (m->fail_rx_snapshot) return -EOPNOTSUPP;
    /* The producer appends independent of how far receive has parsed. */
    *count = m->bad_rx_mark ? 0 : m->queue_length;
    return 0;
}
static bool drained(void *context) {
    struct model *m = context;
    if (m->no_drain) return false;
    if (m->baud_pending) { assert(m->time >= m->baud_written + 20 * MS); m->drained_at = m->time; }
    return true;
}
static int speed(void *context, uint32_t baud) {
    struct model *m = context;
    assert(m->baud_pending && !m->flow && baud == 3200000);
    assert(m->time >= m->baud_written + 20 * MS && m->drained_at >= m->baud_written + 20 * MS);
    m->host_speed_calls++; m->speed_at = m->time;
    return m->fail_speed ? -EIO : 0;
}
static int flow(void *context, bool enabled) {
    struct model *m = context;
    m->flow = enabled;
    if (!enabled) { m->flow_off_calls++; return 0; }
    m->flow_on_calls++;
    if (m->fail_flow_on) return -EIO;
    if (m->baud_pending && m->host_speed_calls && !m->fail_speed && !m->no_baud_reply) reply(m, 0xfff1, 2, 0, m->status ? m->status : 1);
    return 0;
}
static int access(void *context, bool acquire, uint64_t deadline) {
    struct model *m = context;
    assert(deadline > m->time);
    if (acquire) { assert(!m->lease); m->lease = true; return 0; }
    assert(m->lease);
    if (m->fail_release) return -EBUSY;
    m->lease = false;
    if (m->release_cancel) qbt_peri_fail(m->p, -ECANCELED);
    return 0;
}
static int verify_bt(void *context, const struct qbt_peri_bt_identity *v, uint64_t deadline) {
    struct model *m = context;
    assert(!m->lease && m->bt_probes == 1 && deadline > m->time);
    assert(v->product == 0x1e && v->patch == 0x1234 && v->rom == 0x0200 && v->soc == 0x40210200);
    m->bt_verify_calls++;
    if (m->reenter_bt_verify) assert(qbt_peri_activate_bt(m->p, deadline) == -EBUSY);
    if (m->reenter_bt_verify) assert(qbt_peri_enter_hci(m->p, forward) == -EBUSY);
    if (m->cancel_bt_verify) qbt_peri_fail(m->p, -ECANCELED);
    if (m->late_bt_verify) m->time = deadline + MS;
    return m->reject_bt_identity ? -ENODEV : 0;
}
static void init_with_transport(struct model *m, bool awake, bool verifier, bool provenance, bool fencing) {
    memset(m, 0, sizeof(*m)); m->wake_ack = true; m->reset_ready = true; m->flow = true; m->rx_limit = 1;
    m->activation_notice_ss = m->activation_notice_action = 1;
    const struct qbt_peri_ops ops = { .uart = { .context = m, .allocate = allocate, .release = deallocate,
        .now_ns = clock_now, .uart_write = write_uart, .uart_drained = drained, .uart_speed = speed,
        .uart_flow = flow, .pump = pump_model, .wait_until = sleep_until, .receive = forward }, .access = access,
        .verify_bt = verifier ? verify_bt : NULL, .rx_produced = provenance ? rx_produced : NULL,
        .uart_write_fenced = fencing ? write_fenced : NULL };
    m->p = qbt_peri_create(&ops, 115200); assert(m->p);
    assert(qbt_peri_acquire(m->p, m->time + MS) == 0);
    if (awake) assert(qbt_peri_wake(m->p, m->time + 200 * MS) == 0);
}
static void init_with_source(struct model *m, bool awake, bool verifier, bool provenance) {
    init_with_transport(m, awake, verifier, provenance, true);
}
static void init_checked(struct model *m, bool awake, bool verifier) { init_with_source(m, awake, verifier, true); }
static void init(struct model *m, bool awake) { init_checked(m, awake, false); }
static void finish(struct model *m) {
    if (m->lease) assert(qbt_peri_destroy(m->p) == -EBUSY);
    assert(qbt_peri_release(m->p, m->time + MS) == 0);
    assert(qbt_peri_destroy(m->p) == 0);
    assert(!m->lease && m->allocations == m->released);
}
static int version(struct model *m, uint8_t ss) {
    struct qbt_peri_version v;
    int e = qbt_peri_read_version(m->p, ss, m->time + 100 * MS, &v);
    if (!e) {
        assert(v.product == 0x1e && v.soc == 0x40210200 && v.rom == 0x0200 && v.patch == 0x1234 && v.security == 3 && v.subsystem == ss);
        assert(qbt_peri_version_key(&v) == 0x40210200001e0200ULL);
    }
    return e;
}

static void encodings(void) {
    uint8_t b[259]; size_t n;
    struct qbt_peri_request r = { .operation = QBT_PERI_VERSION, .subsystem = 0 };
#define ENCODE(...) do { const uint8_t want[] = {__VA_ARGS__}; n = sizeof(b); assert(!qbt_peri_encode(&r, b, &n)); assert(n == sizeof(want) && !memcmp(b, want, n)); } while (0)
    ENCODE(0x31, 0, 0xf0, 0xff, 2, 5, 0);
    r.subsystem = 3; ENCODE(0x31, 0, 0xf0, 0xff, 2, 5, 3);
    r.operation = QBT_PERI_ARBITRATE; ENCODE(0x31, 0, 0xf0, 0xff, 3, 8, 3, 0);
    r.operation = QBT_PERI_PATCH_CONFIG; r.value = 0x78563412;
    ENCODE(0x31, 0, 0xf0, 0xff, 6, 7, 3, 0x12, 0x34, 0x56, 0x78);
    r.operation = QBT_PERI_BAUD; r.subsystem = 0; r.value = 3200000;
    ENCODE(0x31, 0, 0xf1, 0xff, 2, 2, 0x11);
    r.operation = QBT_PERI_BOARD_ID; ENCODE(0x31, 0, 0xf0, 0xff, 1, 0x0a);
    r.operation = QBT_PERI_RESET; ENCODE(0x31, 0, 0xf1, 0xff, 1, 3);
    r.operation = QBT_PERI_ACTIVATE_BT; r.subsystem = 1; r.value = 1;
    ENCODE(0x31, 0, 0xf1, 0xff, 3, 0, 1, 1);
    r.value = 0; ENCODE(0x31, 0, 0xf1, 0xff, 3, 0, 1, 0);
#undef ENCODE
    r.subsystem = 0; n = sizeof(b); assert(qbt_peri_encode(&r, b, &n) == -EOPNOTSUPP);
    r.operation = QBT_PERI_SEGMENT; r.data = b; r.length = 244; n = sizeof(b);
    assert(qbt_peri_encode(&r, b, &n) == -EMSGSIZE);
    r.operation = QBT_PERI_VERSION; n = 6; memset(b, 0xa5, sizeof(b));
    assert(qbt_peri_encode(&r, b, &n) == -EMSGSIZE && n == 7 && b[0] == 0xa5);
    struct qbt_peri_version v = { .product = 0x1e, .soc = 0x40210200, .rom = 0x0200, .subsystem = 3 };
    const char *name = NULL;
    assert(!qbt_peri_tme_filename(&v, &name) && !strcmp(name, "tmel_peach_20.elf"));
    v.product = 0x21; assert(!qbt_peri_tme_filename(&v, &name));
    v.soc = 0x40210100; v.rom = 0x0100; assert(!qbt_peri_tme_filename(&v, &name) && !strcmp(name, "tmel_peach_10.elf"));
    v.subsystem = 0; assert(qbt_peri_tme_filename(&v, &name) == -EOPNOTSUPP && !name);
    v.subsystem = 3; v.product = 0x22; assert(qbt_peri_tme_filename(&v, &name) == -EOPNOTSUPP && !name);
}
static void decode_cases(void) {
    const uint8_t good[] = { 0x34, 0, 0xff, 8, 0xf0, 1, 0, 0xf0, 0xff, 0, 8, 3 };
    struct qbt_peri_event e;
    for (size_t n = 0; n < sizeof(good); n++) assert(qbt_peri_decode(good, n, &e) == -EMSGSIZE);
    assert(!qbt_peri_decode(good, sizeof(good), &e));
    assert(e.opcode == 0xfff0 && e.subcommand == 8 && e.subsystem == 3 && e.credits == 0);
    uint8_t bad[sizeof(good)]; memcpy(bad, good, sizeof(bad)); bad[1] = 3;
    assert(qbt_peri_decode(bad, sizeof(bad), &e) == -EPROTO);
    memcpy(bad, good, sizeof(bad)); bad[0] = 0x32; assert(qbt_peri_decode(bad, sizeof(bad), &e) == -EPROTO);
    memcpy(bad, good, sizeof(bad)); bad[3] = 7; assert(qbt_peri_decode(bad, sizeof(bad), &e) == -EMSGSIZE);
    const uint8_t ready[] = { 0x34, 0, 0xff, 4, 0xf0, 3, 0x17, 3 };
    assert(!qbt_peri_decode(ready, sizeof(ready), &e) && e.status == 0x17 && !e.has_opcode && e.has_subsystem);
}
static void replies_and_wake(void) {
    struct model m; struct qbt_peri_info i;
    init(&m, true); m.write_limit = 2; m.rx_limit = 0; m.incoming_wake_partial = true; m.extra_wrong = true;
    assert(!version(&m, 3)); assert(m.ack_tx == 1);
    qbt_peri_get_info(m.p, &i); assert(i.matched == 1 && i.unmatched >= 1 && i.last_credits == 0);
    uint16_t board; char build[16]; m.extra_wrong = false;
    assert(!qbt_peri_read_board_id(m.p, m.time + 100 * MS, &board) && board == 0x45);
    assert(!qbt_peri_read_build_info(m.p, 3, m.time + 100 * MS, build, sizeof(build)) && !strcmp(build, "fixture"));
    assert(qbt_peri_read_build_info(m.p, 0, m.time + 100 * MS, build, 7) == -EMSGSIZE && !build[0]);
    finish(&m);
    for (unsigned test = 0; test < 9; test++) {
        init(&m, true);
        switch (test) {
        case 0: m.wrong_opcode = true; break; case 1: m.wrong_sub = true; break;
        case 2: m.wrong_ss = true; break; case 3: m.short_reply = true; break;
        case 4: m.cc_only = true; break; case 5: m.status_only = true; break;
        case 6: m.truncate_version = true; break; case 7: m.status = 0x17; break;
        case 8: m.hardware_error = true; break;
        }
        int expected = test < 6 ? -ETIMEDOUT : (test == 6 ? -EMSGSIZE : -EIO);
        assert(version(&m, 0) == expected); finish(&m);
    }
    init(&m, false); m.wake_ack = false;
    assert(qbt_peri_wake(m.p, m.time + 200 * MS) == -ETIMEDOUT);
    qbt_peri_get_info(m.p, &i); assert(i.wake_requests == 10 && !i.awake && m.time == 100 * MS);
    finish(&m);
    init(&m, false); m.cancel = true;
    assert(qbt_peri_wake(m.p, m.time + 200 * MS) == -ECANCELED); finish(&m);
}
static void h4_backpressure(void) {
    struct model m; init(&m, true);
    uint8_t acl[1029] = { 2, 0, 0, 0, 4 };
    for (size_t j = 5; j < sizeof(acl); j++) acl[j] = (uint8_t)(0xfc + j % 3);
    m.rx_blocked = true; m.unblock_at = m.time + 15 * MS; m.forward_limit = 3; m.rx_limit = 17;
    queue_bytes(&m, acl, sizeof(acl));
    const uint8_t h4[] = {4, 0x0e, 4, 1, 0xf0, 0xff, 0}; queue_bytes(&m, h4, sizeof(h4));
    assert(!version(&m, 0));
    assert(m.forwarded_length == sizeof(acl) + sizeof(h4));
    assert(!memcmp(m.forwarded, acl, sizeof(acl)) && !memcmp(m.forwarded + sizeof(acl), h4, sizeof(h4)));
    assert(m.ack_tx == 0); finish(&m);
}
static void baud_sequence(void) {
    struct model m; struct qbt_peri_info i;
    for (unsigned test = 0; test < 8; test++) {
        init(&m, true); m.write_limit = 2;
        if (test == 1) m.no_baud_reply = true;
        else if (test == 2) m.fail_speed = true;
        else if (test == 3) m.fail_flow_on = true;
        else if (test == 4) m.cancel = true;
        else if (test == 5) m.no_drain = true;
        else if (test == 6) m.cancel_at = m.time + 5 * MS;
        else if (test == 7) { m.no_drain = true; m.cancel_at = m.time + 25 * MS; }
        int expected = test == 0 ? 0 : (test == 1 || test == 5 ? -ETIMEDOUT : (test == 4 || test >= 6 ? -ECANCELED : -EIO));
        assert(qbt_peri_set_baud(m.p, 3200000, m.time + 80 * MS) == expected);
        qbt_peri_get_info(m.p, &i);
        assert(m.flow_off_calls == 1 && m.flow_on_calls == 1 && i.baud_verified == (test == 0));
        assert(m.host_speed_calls == (test == 4 || test == 5 || test >= 6 ? 0u : 1u));
        if (!test) assert(i.host_baud == 3200000 && m.speed_at >= m.baud_written + 20 * MS);
        finish(&m);
    }
}
static void downloads_and_ready(void) {
    struct model m;
    uint8_t blob[500]; memset(blob, 0x5a, sizeof(blob));
    blob[0] = 1; blob[1] = 0xf0; blob[2] = 1; blob[3] = 0; /* 496 + four-byte TLV header */
    for (unsigned mode = 0; mode < 2; mode++) for (unsigned fault = 0; fault < 3; fault++) {
        init(&m, true); m.final_only = mode != 0; m.download_expected = sizeof(blob); blob[14] = mode ? 3 : 0;
        if (fault == 1) m.drop_final = true; else if (fault == 2) m.fail_final = true;
        const struct qbt_peri_blob b = { .data = blob, .length = sizeof(blob) };
        int expected = fault == 0 ? 0 : (fault == 1 ? -ETIMEDOUT : -EIO);
        assert(qbt_peri_download(m.p, 0, &b, m.time + 200 * MS) == expected);
        assert(m.segments == 3 && m.download_length == sizeof(blob) && !memcmp(m.download, blob, sizeof(blob)));
        finish(&m);
    }
    uint8_t elf[500]; memset(elf, 0x5a, sizeof(elf)); memcpy(elf, "\177ELF", 4); elf[4] = elf[5] = elf[6] = 1;
    for (unsigned mode = 0; mode < 3; mode++) {
        init(&m, true); m.final_only = mode != 0; m.download_expected = sizeof(elf);
        m.expected_config = mode == 1 ? 1 : 0;
        const struct qbt_peri_blob b = { .data = elf, .length = sizeof(elf),
            .elf_config = mode == 2 ? QBT_PERI_ELF_NO_VALUE : QBT_PERI_ELF_EXPLICIT, .config_value = mode == 1 ? 1 : 0 };
        uint64_t floor = qbt_peri_sequence(m.p);
        assert(!qbt_peri_download(m.p, 3, &b, m.time + 200 * MS));
        assert(m.configs == (mode == 2 ? 0u : 1u) && m.segments == 3 && !memcmp(m.download, elf, sizeof(elf)));
        queue_ready(&m, 0, 3);
        assert(!qbt_peri_wait_ready(m.p, 3, floor, m.time + 100 * MS)); finish(&m);
    }
    init(&m, true); m.download_expected = sizeof(elf); m.expected_config = 2;
    const struct qbt_peri_blob parallel = { .data = elf, .length = sizeof(elf), .elf_config = QBT_PERI_ELF_EXPLICIT, .config_value = 2 };
    assert(!qbt_peri_download(m.p, 3, &parallel, m.time + 200 * MS) && m.configs == 1 && m.segments == 3); finish(&m);
    init(&m, true); blob[0] = 2; blob[14] = 3; m.download_expected = sizeof(blob);
    const struct qbt_peri_blob nv = { .data = blob, .length = sizeof(blob) };
    assert(!qbt_peri_download(m.p, 0, &nv, m.time + 200 * MS) && m.segments == 3); finish(&m);
    blob[0] = 1;
    init(&m, true); m.final_only = true; m.fail_early = true; m.download_expected = sizeof(elf); m.rx_limit = 0;
    const struct qbt_peri_blob b = { .data = elf, .length = sizeof(elf), .elf_config = QBT_PERI_ELF_NO_VALUE };
    assert(qbt_peri_download(m.p, 3, &b, m.time + 200 * MS) == -EIO); assert(m.segments < 3); finish(&m);
    init(&m, true);
    struct qbt_peri_blob bad = b; bad.elf_config = QBT_PERI_ELF_CONFIG_UNKNOWN;
    assert(qbt_peri_download(m.p, 3, &bad, m.time + MS) == -EOPNOTSUPP && !m.commands);
    bad.data = NULL; assert(qbt_peri_download(m.p, 3, &bad, m.time + MS) == -ENOENT && !m.commands);
    uint8_t nvm[] = { 2, 1, 0, 0, 0x55 }; bad.data = nvm; bad.length = sizeof(nvm);
    assert(qbt_peri_download(m.p, 3, &bad, m.time + MS) == -EOPNOTSUPP && !m.commands);
    bad.data = blob; bad.length = sizeof(blob) - 1;
    assert(qbt_peri_download(m.p, 0, &bad, m.time + MS) == -EMSGSIZE && !m.commands); finish(&m);
    for (unsigned fault = 0; fault < 3; fault++) {
        init(&m, true); queue_ready(&m, 0, 3);
        while (m.queue_offset < m.queue_length) pump_model(&m);
        uint64_t floor = qbt_peri_sequence(m.p);
        if (fault == 1) queue_ready(&m, 0, 0); else if (fault == 2) queue_ready(&m, 0x17, 3);
        assert(qbt_peri_wait_ready(m.p, 3, floor, m.time + 50 * MS) == (fault == 2 ? -EIO : -ETIMEDOUT));
        finish(&m);
    }
    init(&m, true); uint64_t floor = qbt_peri_sequence(m.p);
    queue_ready(&m, 0x17, 3); queue_ready(&m, 0, 3);
    while (m.queue_offset < m.queue_length) pump_model(&m);
    assert(qbt_peri_wait_ready(m.p, 3, floor, m.time + 50 * MS) == -EIO); finish(&m);
}
static void reset_and_lease(void) {
    struct model m; struct qbt_peri_info i; uint8_t state;
    for (unsigned s = 0; s < 6; s++) {
        init(&m, true); const uint8_t states[] = {0, 0x0b, 0x17, 5, 6, 0x18}; m.arb_state = states[s];
        assert(qbt_peri_arbitrate(m.p, 3, m.time + 100 * MS, &state) == (s == 5 ? -EOPNOTSUPP : 0) && state == states[s]);
        finish(&m);
    }
    for (unsigned fault = 0; fault < 6; fault++) {
        init(&m, true);
        if (fault == 1) m.reset_ready = false; else if (fault == 2) m.ready_wrong_ss = true; else if (fault == 3) m.ready_error = true;
        else if (fault == 4) m.drop_reset_reply = true; else if (fault == 5) m.status = 1;
        assert(qbt_peri_reset(m.p, m.time + 100 * MS) == (fault == 0 ? 0 : (fault == 3 || fault == 5 ? -EIO : -ETIMEDOUT)));
        qbt_peri_get_info(m.p, &i); assert(!i.awake); finish(&m);
    }
    init(&m, true); m.fail_release = true;
    assert(qbt_peri_release(m.p, m.time + MS) == -EBUSY);
    qbt_peri_get_info(m.p, &i); assert(i.access_held && m.lease && qbt_peri_destroy(m.p) == -EBUSY);
    m.fail_release = false; finish(&m);
    struct qbt_peri_ops ops = { .uart = { .context = &m, .allocate = allocate, .release = deallocate,
        .now_ns = clock_now, .uart_write = write_uart, .pump = pump_model, .wait_until = sleep_until } };
    m.p = qbt_peri_create(&ops, 115200); assert(m.p);
    assert(qbt_peri_acquire(m.p, m.time + MS) == -EOPNOTSUPP);
    assert(qbt_peri_wake(m.p, m.time + MS) == -EOPNOTSUPP);
    assert(qbt_peri_set_baud(m.p, 3200000, m.time + MS) == -EOPNOTSUPP);
    assert(qbt_peri_activate_bt(m.p, m.time + MS) == -EOPNOTSUPP);
    assert(!qbt_peri_destroy(m.p) && m.allocations == m.released);
}
static void cancellation_and_stale_fragments(void) {
    struct model m; struct qbt_peri_info i;
    init(&m, true); m.reenter = true; assert(!version(&m, 0) && m.reenter_result == -EBUSY); finish(&m);
    init(&m, true); m.stall_tx = true;
    assert(version(&m, 0) == -ETIMEDOUT && !m.commands);
    assert(version(&m, 0) == -ETIMEDOUT && !m.commands); finish(&m);
    init(&m, true); m.suppress = true; m.old_reply_tail = true;
    const uint8_t prefix[] = {0x34, 0, 0xff, 8, 0xf0, 1, 0, 0xf0, 0xff, 0};
    assert(qbt_peri_receive(m.p, prefix, sizeof(prefix)) == sizeof(prefix));
    assert(version(&m, 0) == -ETIMEDOUT);
    qbt_peri_get_info(m.p, &i); assert(!i.matched && i.unmatched); finish(&m);
    init(&m, true); m.suppress = true; m.cancel_at = m.time + 4 * MS;
    assert(version(&m, 0) == -ECANCELED && m.commands == 1); finish(&m);
    init(&m, true);
    const struct qbt_peri_request baud = { .operation = QBT_PERI_BAUD, .subsystem = 0, .value = 3200000 };
    assert(qbt_peri_command(m.p, &baud, m.time + MS, NULL) == -EOPNOTSUPP && !m.commands);
    assert(qbt_peri_set_baud(m.p, 115200, m.time + MS) == -EALREADY && !m.commands); finish(&m);
    init(&m, true); m.rx_limit = 0; m.reset_ready = false; m.stale_ready_before_tx = true; m.pumps = 0;
    assert(qbt_peri_reset(m.p, m.time + 50 * MS) == -ETIMEDOUT && m.stale_ready_sent && m.commands == 1); finish(&m);
}
static void late_replies_and_controls(void) {
    struct model m; struct qbt_peri_info i;
    init(&m, false); m.wake_ack = false; m.late_wake = true; m.late_time = m.time + 11 * MS;
    assert(qbt_peri_wake(m.p, m.time + 10 * MS) == -ETIMEDOUT);
    qbt_peri_get_info(m.p, &i); assert(!i.awake && i.wake_acks == 1); finish(&m);
    init(&m, true); m.rx_limit = 0; m.late_ready = true; m.late_time = m.time + 11 * MS;
    assert(qbt_peri_wait_ready(m.p, 3, qbt_peri_sequence(m.p), m.time + 10 * MS) == -ETIMEDOUT); finish(&m);
    for (unsigned block = 0; block < 2; block++) {
        init(&m, true); m.write_limit = 2; m.rx_limit = 0; m.incoming_wake_partial = true;
        m.ack_unblock_at = m.time + (block ? 101 : 3) * MS;
        assert(version(&m, 0) == (block ? -ETIMEDOUT : 0)); assert(m.ack_tx == (block ? 0u : 1u)); finish(&m);
    }
    init(&m, true); m.rx_limit = 0; m.write_limit = 2; m.incoming_wake_partial = true;
    assert(!qbt_peri_set_baud(m.p, 3200000, m.time + 100 * MS) && m.ack_tx == 1); finish(&m);
}
static void bt_version_and_activation(void) {
    struct qbt_peri_bt_identity v;
    assert(!qbt_peri_decode_bt_version(bt_version_reply, sizeof(bt_version_reply), &v));
    assert(v.product == 0x1e && v.rom == 0x0200 && v.patch == 0x1234 && v.soc == 0x40210200);
    for (size_t n = 0; n < sizeof(bt_version_reply); n++)
        assert(qbt_peri_decode_bt_version(bt_version_reply, n, &v) == -EMSGSIZE);
    struct qbt_peri_event e;
    const uint8_t notice[] = {0x34, 0, 0xff, 4, 0xf0, 2, 1, 1};
    assert(!qbt_peri_decode(notice, sizeof(notice), &e));
    assert(e.kind == QBT_PERI_ACTIVATE_NOTIFY && e.has_subsystem && e.subsystem == 1 && e.action == 1);
    assert(!e.has_status && !e.has_opcode && !e.has_subcommand);
    struct model m; struct qbt_peri_info i;
    for (unsigned scenario = 0; scenario < 17; scenario++) {
        init_checked(&m, true, true); m.rx_limit = 0; m.write_limit = 2;
        int expected = 0;
        switch (scenario) {
        case 0: break;
        case 1: m.activation_notice_first = true; break;
        case 2: m.activation_reply_value = 0x17; m.drop_activation_notice = true; break;
        case 3: m.activation_reply_value = 5; break; /* opaque result: still needs action1 + H4 proof */
        case 4: m.activation_vendor_reply = true; break;
        case 5: m.drop_activation_reply = true; expected = -ETIMEDOUT; break;
        case 6: m.drop_activation_notice = true; expected = -ETIMEDOUT; break;
        case 7: m.activation_notice_ss = 0; expected = -ETIMEDOUT; break;
        case 8: m.activation_notice_action = 0; expected = -ETIMEDOUT; break;
        case 9: m.drop_bt_reply = true; expected = -ETIMEDOUT; break;
        case 10: m.bad_bt_opcode = true; expected = -ETIMEDOUT; break;
        case 11: m.bad_bt_subcommand = true; expected = -ETIMEDOUT; break;
        case 12: m.short_bt_reply = true; expected = -EMSGSIZE; break;
        case 13: m.fail_bt_reply = true; expected = -EIO; break;
        case 14: m.zero_bt_soc = true; expected = -EILSEQ; break;
        case 15: m.zero_bt_rom = true; expected = -EILSEQ; break;
        case 16: m.bad_activation_opcode = true; expected = -ETIMEDOUT; break;
        }
        int result = qbt_peri_activate_bt(m.p, m.time + 100 * MS);
        qbt_peri_get_info(m.p, &i);
        if (result != expected) fprintf(stderr, "activation scenario%u result%d expected%d stage%u\n", scenario, result, expected, i.activation_stage);
        assert(result == expected && i.bt_activated == (expected == 0));
        if (!expected) {
            assert(i.activation_stage == QBT_PERI_ACT_VERIFIED && i.activation_reply_seen && i.bt_probe_seen && !i.access_held);
            assert(i.bt.product == 0x1e && i.bt.soc == 0x40210200 && i.bt.rom == 0x0200 && m.bt_probes == 1 && m.bt_verify_calls == 1);
        } else {
            assert(!m.bt_verify_calls);
            if ((scenario >= 5 && scenario <= 8) || scenario == 16) assert(!m.bt_probes && i.access_held);
            else assert(m.bt_probes == 1 && !i.access_held);
        }
        finish(&m);
    }
    init_checked(&m, true, true); m.rx_limit = 0; m.drop_activation_notice = true;
    queue_activation_notice(&m); /* stale full notice before the current send */
    assert(qbt_peri_activate_bt(m.p, m.time + 100 * MS) == -ETIMEDOUT && !m.bt_probes && !m.bt_verify_calls); finish(&m);
    init_checked(&m, true, true); m.rx_limit = 0; m.old_bt_reply = true; m.drop_bt_reply = true;
    assert(qbt_peri_activate_bt(m.p, m.time + 100 * MS) == -ETIMEDOUT && m.bt_probes == 1 && !m.bt_verify_calls);
    qbt_peri_get_info(m.p, &i); assert(!i.bt_probe_seen && !i.bt_activated && m.forwarded_length == sizeof(bt_version_reply)); finish(&m);
    init_checked(&m, true, true); m.rx_limit = 0; m.write_limit = 2; m.early_activation_notice = true; m.drop_activation_notice = true;
    assert(qbt_peri_activate_bt(m.p, m.time + 100 * MS) == -ETIMEDOUT && m.early_activation_sent && !m.bt_probes);
    qbt_peri_get_info(m.p, &i); assert(!i.activation_notice_seen && !i.bt_activated); finish(&m);
    init_checked(&m, true, true); m.rx_limit = 0; m.write_limit = 2; m.early_bt_reply = true; m.drop_bt_reply = true;
    assert(qbt_peri_activate_bt(m.p, m.time + 100 * MS) == -ETIMEDOUT && m.early_bt_sent && m.bt_probes == 1);
    qbt_peri_get_info(m.p, &i); assert(!i.bt_probe_seen && !i.bt_activated && m.forwarded_length == sizeof(bt_version_reply)); finish(&m);
    /* Old packets produced during fragmented TX can be hidden behind an
     * unfinished H4 event. Parser-start sequence alone falsely accepts them. */
    init_checked(&m, true, true); m.write_limit = 2; m.early_activation_notice = m.buffered_old_notice = true; m.drop_activation_notice = true;
    assert(qbt_peri_activate_bt(m.p, m.time + 100 * MS) == -ETIMEDOUT && m.early_activation_sent && !m.bt_probes);
    qbt_peri_get_info(m.p, &i); assert(!i.activation_notice_seen && !i.bt_activated); finish(&m);
    init_checked(&m, true, true); m.write_limit = 2; m.early_bt_reply = m.buffered_old_bt = true; m.drop_bt_reply = true;
    assert(qbt_peri_activate_bt(m.p, m.time + 100 * MS) == -ETIMEDOUT && m.early_bt_sent && m.bt_probes == 1 && !m.bt_verify_calls);
    qbt_peri_get_info(m.p, &i); assert(!i.bt_probe_seen && !i.bt_activated && m.forwarded_length == 7 + sizeof(bt_version_reply)); finish(&m);
    for (unsigned fault = 0; fault < 5; fault++) {
        init_checked(&m, true, true); m.rx_limit = 0;
        if (fault == 0) m.fail_release = true;
        else if (fault == 1) m.reject_bt_identity = true;
        else if (fault == 2) m.cancel_bt_verify = true;
        else if (fault == 3) m.late_bt_verify = true;
        else m.release_cancel = true;
        int expected = fault == 0 ? -EBUSY : (fault == 1 ? -ENODEV : (fault == 3 ? -ETIMEDOUT : -ECANCELED));
        assert(qbt_peri_activate_bt(m.p, m.time + 100 * MS) == expected);
        qbt_peri_get_info(m.p, &i); assert(!i.bt_activated && i.access_held == (fault == 0));
        if (fault == 0 || fault == 4) assert(!m.bt_probes && !m.bt_verify_calls);
        m.fail_release = false; finish(&m);
    }
    init_checked(&m, true, true); m.rx_limit = 0; m.reenter_bt_verify = true;
    assert(!qbt_peri_activate_bt(m.p, m.time + 100 * MS)); finish(&m);
    init(&m, true); m.rx_limit = 0;
    assert(!qbt_peri_activate_bt(m.p, m.time + 100 * MS) && m.bt_probes == 1 && !m.bt_verify_calls); finish(&m);
    for (unsigned missing = 0; missing < 3; missing++) {
        init_checked(&m, true, true); m.rx_limit = 0;
        if (!missing) m.legacy_rx = true; else if (missing == 1) m.fail_rx_snapshot = true; else m.bad_rx_mark = true;
        int expected = missing < 2 ? -EOPNOTSUPP : -EILSEQ;
        assert(qbt_peri_activate_bt(m.p, m.time + 100 * MS) == expected);
        qbt_peri_get_info(m.p, &i); assert(!i.bt_activated && !m.bt_verify_calls);
        assert(i.activation_gap == (missing == 0 ? QBT_PERI_ACT_RX_UNSTAMPED :
            (missing == 1 ? QBT_PERI_ACT_RX_SNAPSHOT_FAILED : QBT_PERI_ACT_RX_MARK_INVALID)));
        finish(&m);
    }
    init_with_source(&m, true, true, false);
    assert(qbt_peri_activate_bt(m.p, m.time + 100 * MS) == -EOPNOTSUPP && !m.activation_commands && !m.bt_probes);
    qbt_peri_get_info(m.p, &i); assert(i.activation_gap == QBT_PERI_ACT_NO_RX_PRODUCER && !i.bt_activated); finish(&m);
    init_checked(&m, true, true); m.legacy_rx = true; m.rx_limit = 0;
    const uint8_t diagnostic[] = {4, 0xff, 4, 0x99, 0, 0, 0}; queue_bytes(&m, diagnostic, sizeof(diagnostic)); pump_model(&m);
    m.legacy_rx = false; m.bad_rx_mark = true;
    assert(qbt_peri_activate_bt(m.p, m.time + 100 * MS) == -EILSEQ && !m.activation_commands && !m.bt_probes);
    qbt_peri_get_info(m.p, &i); assert(i.rx_bytes >= 8 && i.activation_gap == QBT_PERI_ACT_RX_MARK_INVALID && i.access_held); finish(&m);
    init(&m, true); m.rx_limit = 0;
    assert(!qbt_peri_set_baud(m.p, 3200000, m.time + 100 * MS));
    assert(!qbt_peri_activate_bt(m.p, m.time + 100 * MS));
    qbt_peri_get_info(m.p, &i); assert(i.host_baud == 3200000 && m.host_speed_calls == 1 && m.flow_off_calls == 1); finish(&m);
}
static void tx_admission_fence(void) {
    struct model m; struct qbt_peri_info i;
    for (unsigned fast = 0; fast < 4; fast++) for (unsigned fragmented = 0; fragmented < 2; fragmented++) {
        init_checked(&m, true, true); m.rx_limit = fragmented ? 1 : 0; m.write_limit = fragmented ? 2 : 0;
        m.fast_activation_reply = fast != 2; m.fast_bt_reply = fast != 1;
        m.activation_vendor_reply = fast == 3; m.activation_notice_first = fragmented != 0;
        assert(!qbt_peri_activate_bt(m.p, m.time + 100 * MS));
        qbt_peri_get_info(m.p, &i);
        assert(i.bt_activated && i.activation_reply_seen && i.activation_notice_seen && i.bt_probe_seen);
        assert(i.activation_gap == QBT_PERI_ACT_GAP_NONE && !i.access_held && m.bt_verify_calls == 1);
        assert(m.fast_activation_returns == (m.fast_activation_reply ? 1u : 0u));
        assert(m.fast_bt_returns == (m.fast_bt_reply ? 1u : 0u));
        assert(m.fenced_act_admissions == (fragmented ? 4u : 1u) && m.fenced_bt_admissions == (fragmented ? 3u : 1u));
        assert(m.fenced_calls == m.fenced_admissions && i.tx_bytes == 14);
        /* Only the initial capability/consistency snapshot is read. A later
         * producer snapshot would exclude the fast response's first byte. */
        assert(m.rx_snapshot_calls == 1); finish(&m);
    }
    for (unsigned probe = 0; probe < 2; probe++) {
        init_checked(&m, true, true); m.rx_limit = 0; m.write_limit = 2;
        m.fence_probe_only = probe != 0; m.fence_retries = 3;
        m.fast_activation_reply = m.fast_bt_reply = true;
        uint64_t bound = m.time + 50 * MS;
        assert(!qbt_peri_activate_bt(m.p, bound) && m.time < bound);
        assert(m.fenced_admissions == 7 && m.fenced_calls == 10 && !m.fence_retries && m.rx_snapshot_calls == 1);
        qbt_peri_get_info(m.p, &i); assert(i.bt_activated && i.tx_bytes == 14 && !i.access_held); finish(&m);
        init_checked(&m, true, true); m.rx_limit = 0; m.fence_probe_only = probe != 0;
        bound = m.time + 25 * MS; m.fence_unblock_at = bound + MS;
        assert(qbt_peri_activate_bt(m.p, bound) == -ETIMEDOUT && m.time == bound);
        qbt_peri_get_info(m.p, &i);
        assert(!i.bt_activated && !m.bt_probes && !m.bt_verify_calls && i.access_held == (probe == 0));
        assert(m.activation_commands == probe && m.fenced_admissions == probe && m.rx_snapshot_calls == 1);
        unsigned calls = m.fenced_calls; m.fence_unblock_at = 0;
        assert(qbt_peri_activate_bt(m.p, m.time + 100 * MS) == -ETIMEDOUT && m.fenced_calls == calls);
        finish(&m);
        for (unsigned fault = 0; fault < 8; fault++) {
            init_checked(&m, true, true); m.rx_limit = 0; m.fence_probe_only = probe != 0;
            int expected = -EIO;
            switch (fault) {
            case 0: m.fence_error = -EIO; break;
            case 1: m.fence_error = -EOPNOTSUPP; expected = -EOPNOTSUPP; break;
            case 2: m.fence_bad_count = true; break;
            case 3: m.fence_error = -EAGAIN; m.fence_error_count = true; break;
            case 4: m.fence_error = -EIO; m.fence_error_count = true; break;
            case 5: m.fence_positive_result = true; break;
            case 6: m.fence_bad_mark = true; expected = -EILSEQ; break;
            case 7: m.fence_cancel = true; expected = -ECANCELED; break;
            }
            assert(qbt_peri_activate_bt(m.p, m.time + 100 * MS) == expected);
            qbt_peri_get_info(m.p, &i);
            assert(!i.bt_activated && !m.bt_verify_calls && i.access_held == (probe == 0));
            assert(i.activation_gap == (fault == 6 ? QBT_PERI_ACT_RX_MARK_INVALID :
                (fault == 7 ? QBT_PERI_ACT_GAP_NONE : QBT_PERI_ACT_TX_FENCE_FAILED)));
            if (fault != 6) assert(m.activation_commands == probe && !m.bt_probes);
            else assert(m.activation_commands == 1 && m.bt_probes == probe);
            calls = m.fenced_calls;
            assert(qbt_peri_activate_bt(m.p, m.time + 100 * MS) == expected && m.fenced_calls == calls);
            finish(&m);
        }
    }
    init_with_transport(&m, true, true, true, false); m.rx_limit = 0;
    assert(!version(&m, 0)); /* other protocol helpers retain the ordinary UART API */
    assert(qbt_peri_activate_bt(m.p, m.time + 100 * MS) == -EOPNOTSUPP);
    qbt_peri_get_info(m.p, &i);
    assert(i.activation_gap == QBT_PERI_ACT_NO_TX_FENCE && !i.bt_activated && i.access_held);
    assert(!m.activation_commands && !m.bt_probes && !m.fenced_calls && !m.rx_snapshot_calls); finish(&m);
}
static int core_clock(void *context, bool on) { (void)context; (void)on; return 0; }
static int unused_firmware(void *context, const char *name, const uint8_t **data, size_t *length) {
    (void)context; (void)name; *data = NULL; *length = 0; return -ENOENT;
}
static void unused_firmware_put(void *context, const uint8_t *data, size_t length) { (void)context; (void)data; (void)length; }
static size_t hci_receiver(void *context, const uint8_t *b, size_t n) {
    struct model *m = context;
    if (m->block_hci && m->time < m->hci_unblock_at) return 0;
    if (m->hci_limit && n > m->hci_limit) n = m->hci_limit;
    return qbt_receive(m->core, b, n);
}
static void hci_phase(void) {
    struct model m; struct qbt_peri_info i;
    for (unsigned fragmented = 0; fragmented < 2; fragmented++) {
        init(&m, true); m.rx_limit = 0;
        assert(qbt_peri_enter_hci(m.p, hci_receiver) == -EBUSY); /* PERI access still held */
        assert(!qbt_peri_activate_bt(m.p, m.time + 100 * MS));
        struct qbt_platform ops = { .context = &m, .allocate = allocate, .release = deallocate,
            .now_ns = clock_now, .uart_write = write_uart, .uart_drained = drained, .uart_speed = speed,
            .uart_flow = flow, .clock = core_clock, .pump = pump_model, .wait_until = sleep_until,
            .receive = forward, .firmware_get = unused_firmware, .firmware_put = unused_firmware_put };
        m.core = qbt_create(&ops); assert(m.core);
        /* Receive-path model only; no simulated firmware boot is claimed. */
        m.core->info.state = 2; m.core->ibs = true; qbt_set_service(m.core, true);
        assert(!qbt_peri_enter_hci(m.p, hci_receiver));
        assert(!qbt_peri_enter_hci(m.p, hci_receiver) && qbt_peri_enter_hci(m.p, forward) == -EBUSY);
        assert(qbt_peri_acquire(m.p, m.time + MS) == -EBUSY);
        const struct qbt_peri_request version = { .operation = QBT_PERI_VERSION, .subsystem = 0 };
        assert(qbt_peri_command(m.p, &version, m.time + MS, NULL) == -EBUSY);
        m.rx_limit = fragmented ? 1 : 0; m.hci_limit = fragmented ? 2 : 0;
        m.block_hci = fragmented != 0; m.hci_unblock_at = m.time + 3 * MS; m.mixed_hci = true;
        unsigned before_ack = m.ack_tx; uint8_t response[16]; size_t length = sizeof(response);
        assert(!qbt_command(m.core, 0x0c03, NULL, 0, 0, response, &length) && length == 1 && response[0] == 0);
        assert(m.ack_tx == before_ack + 1); /* exactly the real core's FD acknowledgement */
        struct qbt_stats stats; qbt_get_stats(m.core, &stats);
        assert(stats.commands == 1 && stats.wake_ind == 1 && stats.wake_ack == 1 && !stats.malformed);
        /* The full last DMA packet can be buffered while core is blocked.
         * A space wake plus peri_step must flush it without more RX bytes. */
        m.mixed_hci = false; m.block_hci = true; m.hci_unblock_at = m.time + 3 * MS;
        length = sizeof(response);
        assert(!qbt_command(m.core, 0x0c03, NULL, 0, 0, response, &length) && length == 1 && response[0] == 0);
        assert(m.ack_tx == before_ack + 1);
        const uint8_t peri[] = {0x34, 0, 0xff, 4, 0xf0, 3, 0, 3}; queue_bytes(&m, peri, sizeof(peri));
        const uint8_t ordinary[] = {4, 0xff, 3, 0x55, 0xfe, 0xfc}; queue_bytes(&m, ordinary, sizeof(ordinary));
        const uint8_t control[] = {0xfc, 0xfe}; queue_bytes(&m, control, sizeof(control));
        while (m.queue_offset < m.queue_length) { pump_model(&m); qbt_step(m.core); }
        assert(m.forwarded_length == sizeof(ordinary) && !memcmp(m.forwarded, ordinary, sizeof(ordinary)));
        qbt_peri_get_info(m.p, &i); assert(i.hci_phase && i.bt_activated && !i.access_held);
        qbt_get_stats(m.core, &stats); assert(!stats.malformed);
        qbt_destroy(m.core); m.core = NULL; finish(&m);
    }
    /* A handoff cannot move a partially delivered packet to another parser. */
    init(&m, true); assert(!qbt_peri_release(m.p, m.time + MS));
    const uint8_t partial[] = {4, 0x0e};
    assert(qbt_peri_receive(m.p, partial, sizeof(partial)) == sizeof(partial));
    assert(qbt_peri_enter_hci(m.p, hci_receiver) == -EBUSY); finish(&m);
    init(&m, false); assert(!qbt_peri_release(m.p, m.time + MS));
    const uint8_t wake = 0xfd; assert(qbt_peri_receive(m.p, &wake, 1) == 1);
    assert(qbt_peri_enter_hci(m.p, forward) == -EBUSY);
    qbt_peri_step(m.p); assert(m.ack_tx == 1);
    assert(!qbt_peri_enter_hci(m.p, forward)); finish(&m);
}
int main(void) {
    encodings(); decode_cases(); replies_and_wake(); h4_backpressure(); baud_sequence(); downloads_and_ready(); reset_and_lease(); cancellation_and_stale_fragments(); late_replies_and_controls();
    bt_version_and_activation();
    tx_admission_fence();
    hci_phase();
    puts("PERI/TME framing, fenced TX admission and replies queued before return, producer-based BT activation proof, H4/IBS handoff to the existing core, backpressure and cleanup passed (synthetic controller).");
    return 0;
}
