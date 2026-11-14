// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NezhaAlsSscClient.h"

// The portable reference uses a synchronous 2s request receipt timeout.
// This owner-driven client retains bounded asynchronous requests instead.
// An acknowledged SUID lookup with register_updates remains subscribed while
// sensors are registering; later SUID events are not pending QMI receipts.
#define RESPONSE_US UINT64_C(2000000)
#define CTRL_PORT UINT32_C(0xfffffffe)
#define MAX_LUX_BITS UINT32_C(0x47811f00) // IEEE binary32 66110.0 lux

bool nezha_als_integer_lux(uint32_t bits, uint32_t *lux) {
    if (!lux) return false;
    uint32_t magnitude = bits & UINT32_C(0x7fffffff);
    if (((bits >> 31) && magnitude) || magnitude > MAX_LUX_BITS) return false;
    uint32_t exponent = magnitude >> 23;
    if (exponent < 126) { *lux = 0; return true; }
    uint32_t significand = (magnitude & UINT32_C(0x7fffff)) | UINT32_C(0x800000);
    // Within66110lux the binary exponent is126..143; the shift is7..24.
    uint32_t shift = 150 - exponent;
    *lux = (significand + (UINT32_C(1) << (shift - 1))) >> shift;
    return true;
}

static bool same_uid(Tcs3760SnsUid a, Tcs3760SnsUid b) {
    return a.low == b.low && a.high == b.high;
}
static int readiness(struct nezha_als_ssc_client *c, bool ready) {
    if (c->ready == ready) return NezhaAlsClientOkay;
    c->ready = ready;
    return c->ops.ready(c->ops.context, c->epoch, ready) ? NezhaAlsClientFrontend : NezhaAlsClientOkay;
}
void nezha_als_ssc_lost(struct nezha_als_ssc_client *c, int error) {
    if (!c || !c->epoch) return;
    c->error = error < 0 ? error : NezhaAlsClientTransport;
    c->phase = NezhaAlsFailed;
    c->pending = false;
    c->latest.valid = false;
    readiness(c, false);
}
static int fail(struct nezha_als_ssc_client *c, int error) {
    nezha_als_ssc_lost(c, error);
    return c->error;
}
static int queue_request(struct nezha_als_ssc_client *c, uint32_t phase, uint64_t now) {
    if (c->transaction == UINT16_MAX || now > UINT64_MAX - RESPONSE_US)
        return fail(c, NezhaAlsClientArgument);
    uint16_t transaction = c->transaction + 1;
    int result;
    if (phase == NezhaAlsLookup) {
        result = c->match.calibrated_stream_lookup ?
            tcs3760_sns_encode_calibrated_lookup(transaction, c->tx, sizeof c->tx, &c->tx_bytes) :
            tcs3760_sns_encode_lookup(transaction, c->tx, sizeof c->tx, &c->tx_bytes);
    } else if (phase == NezhaAlsEnable || phase == NezhaAlsConfigure) {
        result = c->match.calibrated_stream_lookup ?
            tcs3760_sns_encode_streaming(c->match.sensor_uid, c->interval_us, transaction,
                                        c->tx, sizeof c->tx, &c->tx_bytes) :
            tcs3760_sns_encode_on_change(c->match.sensor_uid, transaction,
                                         c->tx, sizeof c->tx, &c->tx_bytes);
        c->configured_interval_us = c->interval_us;
        c->stream_target = c->match.calibrated_stream_lookup;
    }
    else
        result = tcs3760_sns_encode_disable(c->match.sensor_uid, transaction,
                                           c->tx, sizeof c->tx, &c->tx_bytes);
    if (result != Tcs3760SnsOkay) return fail(c, NezhaAlsClientWire);
    c->transaction = transaction;
    c->phase = phase;
    c->tx_state = NezhaAlsTxQueued;
    c->deadline_us = now + RESPONSE_US;
    return NezhaAlsClientOkay;
}
int nezha_als_ssc_begin(struct nezha_als_ssc_client *c, const struct nezha_als_ssc_ops *ops,
                       struct nezha_als_endpoint endpoint, uint32_t local_port,
                       uint64_t epoch, uint32_t interval_us, uint64_t now) {
    if (!c || !ops || !ops->send || !ops->ready || !ops->sample || !epoch ||
        !interval_us || !endpoint.node || endpoint.node == UINT32_MAX ||
        !endpoint.port || endpoint.port >= CTRL_PORT || !local_port ||
        local_port >= CTRL_PORT || (endpoint.instance & 0xff) != TCS3760_SNS_IDL_MAJOR ||
        now > UINT64_MAX - RESPONSE_US) return NezhaAlsClientArgument;
    // Fresh storage belongs to one session. A failed/lost session is retained;
    // restarting requires new storage and a newer owner epoch, not replay.
    if (c->epoch) return NezhaAlsClientArgument;
    *c = (struct nezha_als_ssc_client){0};
    c->ops = *ops; c->endpoint = endpoint; c->local_port = local_port;
    c->epoch = epoch; c->interval_us = interval_us;
    if (c->ops.ready(c->ops.context, epoch, false)) return fail(c, NezhaAlsClientFrontend);
    return queue_request(c, NezhaAlsLookup, now);
}
int nezha_als_ssc_sampling(struct nezha_als_ssc_client *c, uint32_t interval, bool continuous) {
    if (!c || !c->epoch) return NezhaAlsClientArgument;
    if (c->error) return c->error;
    c->interval_us = interval;
    c->continuous = continuous;
    if (!interval) {
        c->pending = false;
        c->latest.valid = false;
        if (readiness(c, false)) return fail(c, NezhaAlsClientFrontend);
    }
    return NezhaAlsClientOkay;
}
int nezha_als_ssc_interval(struct nezha_als_ssc_client *c, uint32_t interval) {
    return nezha_als_ssc_sampling(c, interval, c ? c->continuous : false);
}
static int change_lookup(struct nezha_als_ssc_client *c, uint64_t now) {
    c->match.sensor_uid_valid=false;
    c->match.sensor_uid=(Tcs3760SnsUid){0,0};
    c->match.calibrated_stream_lookup=c->continuous;
    c->switch_lookup_pending=false;
    c->pending=false; c->latest.valid=false;
    c->last_ssc_ticks=0;
    if (readiness(c,false)) return fail(c,NezhaAlsClientFrontend);
    return queue_request(c,NezhaAlsLookup,now);
}
int nezha_als_ssc_poll(struct nezha_als_ssc_client *c, uint64_t now) {
    if (!c || !c->epoch) return NezhaAlsClientArgument;
    if (c->error) return c->error;
    if (c->tx_state != NezhaAlsTxNone && now >= c->deadline_us)
        return fail(c, NezhaAlsClientTimeout);
    if (c->phase == NezhaAlsLookup && c->tx_state == NezhaAlsTxNone && c->match.sensor_uid_valid &&
        c->continuous != c->match.calibrated_stream_lookup) {
        if (change_lookup(c,now)) return c->error;
    } else if (c->phase == NezhaAlsRunning && c->tx_state == NezhaAlsTxNone &&
        c->interval_us && c->continuous != c->stream_active) {
        c->switch_lookup_pending=true;
        c->pending=false; c->latest.valid=false;
        if (readiness(c,false)) return fail(c,NezhaAlsClientFrontend);
        if (queue_request(c,NezhaAlsDisable,now)) return c->error;
    } else if (c->phase == NezhaAlsDisabled && c->tx_state == NezhaAlsTxNone && c->interval_us &&
               (c->switch_lookup_pending || c->continuous != c->match.calibrated_stream_lookup)) {
        if (change_lookup(c,now)) return c->error;
    } else if (c->phase == NezhaAlsLookup && c->tx_state == NezhaAlsTxNone && c->match.sensor_uid_valid) {
        if (c->interval_us) { if (queue_request(c, NezhaAlsEnable, now)) return c->error; }
        else c->phase = NezhaAlsDisabled;
    } else if (c->phase == NezhaAlsRunning && !c->interval_us) {
        if (queue_request(c, NezhaAlsDisable, now)) return c->error;
    } else if (c->phase == NezhaAlsDisabled && c->interval_us) {
        if (queue_request(c, NezhaAlsEnable, now)) return c->error;
    } else if (c->phase == NezhaAlsRunning && c->tx_state == NezhaAlsTxNone &&
               c->stream_active && c->configured_interval_us != c->interval_us) {
        if (queue_request(c,NezhaAlsConfigure,now)) return c->error;
    }
    if (c->tx_state == NezhaAlsTxQueued) {
        int result = c->ops.send(c->ops.context, &c->endpoint, c->local_port, c->tx, c->tx_bytes);
        if (result == NezhaAlsClientAgain) return result;
        if (result) return fail(c, NezhaAlsClientTransport);
        c->tx_state = NezhaAlsTxAwaiting;
    }
    if (c->phase == NezhaAlsRunning && c->interval_us && c->pending && c->latest.valid &&
        (!c->delivered || (now >= c->last_delivery_us && now - c->last_delivery_us >= c->interval_us))) {
        if (readiness(c, true) || c->ops.sample(c->ops.context, &c->latest))
            return fail(c, NezhaAlsClientFrontend);
        c->pending = false;
        c->last_delivery_us = now;
        c->delivered++;
    }
    return NezhaAlsClientOkay;
}
static int response(struct nezha_als_ssc_client *c, const uint8_t *qmi, size_t bytes) {
    if (c->tx_state != NezhaAlsTxAwaiting) { c->stale++; return NezhaAlsClientOkay; }
    Tcs3760SnsResponse ack = {0};
    int result = tcs3760_sns_decode_response(qmi, bytes, c->transaction, &ack);
    if (result == Tcs3760SnsCorrelation) { c->stale++; return NezhaAlsClientOkay; }
    if (result == Tcs3760SnsRemote) return fail(c, NezhaAlsClientRemote);
    if (result != Tcs3760SnsOkay) return fail(c, NezhaAlsClientWire);
    c->responses++;
    if (ack.qmi_result || ack.qmi_error || (ack.result_valid && ack.result))
        return fail(c, NezhaAlsClientRemote);
    if (ack.client_id_valid) {
        if (c->match.client_id_valid && c->match.client_id != ack.client_id)
            return fail(c, NezhaAlsClientIdentity);
        c->match.client_id = ack.client_id; c->match.client_id_valid = true;
    }
    c->tx_state = NezhaAlsTxNone;
    if (c->phase == NezhaAlsEnable || c->phase == NezhaAlsConfigure) {
        c->sensor_enabled = true;
        c->stream_active = c->stream_target;
        c->phase = NezhaAlsRunning;
    } else if (c->phase == NezhaAlsDisable) {
        c->sensor_enabled = false;
        c->phase = NezhaAlsDisabled;
        c->pending = false;
        c->latest.valid = false;
        if (readiness(c, false)) return fail(c, NezhaAlsClientFrontend);
    }
    return NezhaAlsClientOkay;
}
int nezha_als_ssc_receive(struct nezha_als_ssc_client *c, uint32_t node, uint32_t port,
                         const uint8_t *qmi, size_t bytes, uint64_t now, uint64_t receipt) {
    if (!c || !c->epoch || !qmi || !bytes || !receipt) return NezhaAlsClientArgument;
    if (c->error) return c->error;
    if (node != c->endpoint.node || port != c->endpoint.port) {
        c->stale++; return NezhaAlsClientOkay;
    }
    if (c->tx_state != NezhaAlsTxNone && now >= c->deadline_us)
        return fail(c, NezhaAlsClientTimeout);
    if (qmi[0] == 2) return response(c, qmi, bytes);
    Tcs3760SnsReport report = {0};
    int result = tcs3760_sns_decode_report(qmi, bytes, &c->match, &report);
    if (result == Tcs3760SnsCorrelation) { c->stale++; return NezhaAlsClientOkay; }
    if (result == Tcs3760SnsRemote) return fail(c, NezhaAlsClientRemote);
    if (result != Tcs3760SnsOkay && result != Tcs3760SnsNoData)
        return fail(c, NezhaAlsClientWire);
    c->reports++;
    if (report.error_valid && report.error) return fail(c, NezhaAlsClientRemote);
    if (!c->match.client_id_valid) {
        c->match.client_id = report.client_id; c->match.client_id_valid = true;
    }
    if (report.uid_event && !report.uid_valid && c->match.sensor_uid_valid)
        return fail(c, NezhaAlsClientIdentity);
    if (report.uid_valid) {
        if (c->match.calibrated_stream_lookup != c->continuous && !c->sensor_enabled && c->tx_state == NezhaAlsTxNone) {
            return change_lookup(c,now);
        }
        if (c->match.sensor_uid_valid && !same_uid(c->match.sensor_uid, report.uid))
            return fail(c, NezhaAlsClientIdentity);
        c->match.sensor_uid = report.uid; c->match.sensor_uid_valid = true;
    }
    if (!report.sample_valid || (c->phase != NezhaAlsEnable && c->phase != NezhaAlsRunning && c->phase != NezhaAlsConfigure) || !c->interval_us)
        return NezhaAlsClientOkay;
    const Tcs3760SnsSample *sample = &report.sample;
    if (!sample->timestamp || sample->timestamp <= c->last_ssc_ticks || receipt < c->last_receipt_ticks) {
        c->stale++; return NezhaAlsClientOkay;
    }
    c->last_ssc_ticks = sample->timestamp; c->last_receipt_ticks = receipt;
    if (c->sequence == UINT64_MAX) return fail(c, NezhaAlsClientArgument);
    c->latest = (struct nezha_als_sample){c->epoch, ++c->sequence, receipt, 0, false};
    if (!sample->status || sample->status > 3 || !nezha_als_integer_lux(sample->lux_bits, &c->latest.integer_lux)) {
        c->invalid_samples++; c->pending = false;
        if (readiness(c, false)) return fail(c, NezhaAlsClientFrontend);
        return NezhaAlsClientOkay;
    }
    c->latest.valid = true; c->pending = true;
    // Delivery is deferred to poll on the same owner worker. now carries no
    // sensor-clock conversion and is used only for cadence/deadline accounting.
    return NezhaAlsClientOkay;
}
