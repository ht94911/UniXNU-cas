// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NezhaAlsQrtrBackend.h"
// The existing XNU console export; no host stdio header in the kernel recipe.
extern int printf(const char *, ...);

static bool endpoint_equal(struct nezha_als_endpoint a, struct nezha_als_endpoint b) {
    return a.node == b.node && a.port == b.port && a.instance == b.instance;
}
static bool announced(struct nezha_als_qrtr_backend *b, struct nezha_als_endpoint endpoint) {
    QcomQrtr *q = b->router;
    if (!b->ops.online(b->ops.context) || !q->hello_received ||
        !endpoint.node || endpoint.node == QCOM_QRTR_NODE_BROADCAST ||
        !endpoint.port || endpoint.port >= QCOM_QRTR_PORT_CTRL ||
        (endpoint.instance & 0xff) != TCS3760_SNS_IDL_MAJOR) return false;
    unsigned matches = 0;
    bool exact = false;
    for (uint32_t i = 0; i < q->remote_count; ++i) {
        if (q->remote[i].service != TCS3760_SNS_SERVICE_ID ||
            q->remote[i].instance != endpoint.instance) continue;
        matches++;
        if (q->remote[i].node == endpoint.node && q->remote[i].port == endpoint.port) exact = true;
    }
    return matches == 1 && exact;
}
static void close_queue(struct nezha_als_qrtr_backend *b) {
    b->ops.lock(b->ops.context);
    b->accepting = false;
    b->worker_interval_us = b->accepted_interval_us;
    b->count = 0;
    b->ops.unlock(b->ops.context);
}
void nezha_als_qrtr_lost(struct nezha_als_qrtr_backend *b, int error) {
    if (!b || !b->epoch || b->stopped) return;
    b->error = error < 0 ? error : NezhaAlsClientTransport;
    close_queue(b);
    if (b->client) nezha_als_ssc_lost(b->client, b->error);
    else if (!nezha_als_frontend_ready(b->epoch, false)) b->error = NezhaAlsClientFrontend;
}
static int fail(struct nezha_als_qrtr_backend *b, int error) {
    nezha_als_qrtr_lost(b, error);
    return b->error;
}
static int send(void *context, const struct nezha_als_endpoint *endpoint, uint32_t local_port,
                const uint8_t *qmi, size_t bytes) {
    struct nezha_als_qrtr_backend *b = context;
    if (b->stopped || b->error || !endpoint_equal(*endpoint, b->endpoint) ||
        local_port != b->local_port || !announced(b, b->endpoint) ||
        !qmi || !bytes || bytes > TCS3760_SNS_REQUEST_CAPACITY) return NezhaAlsClientTransport;
    uint32_t packet_bytes = QCOM_QRTR_HEADER + (((uint32_t)bytes + 3u) & ~3u);
    int room = b->ops.tx_room(b->ops.context, packet_bytes);
    if (room == NezhaAlsClientAgain) return room;
    if (room) return NezhaAlsClientTransport;
    return qcom_qrtr_send_data(b->router, local_port, endpoint->node, endpoint->port,
                                qmi, (uint32_t)bytes) ? NezhaAlsClientTransport : NezhaAlsClientOkay;
}
static int ready(void *context, uint64_t epoch, bool value) {
    struct nezha_als_qrtr_backend *b = context;
    return epoch == b->epoch && nezha_als_frontend_ready(epoch, value) ?
        NezhaAlsClientOkay : NezhaAlsClientFrontend;
}
static int sample(void *context, const struct nezha_als_sample *value) {
    struct nezha_als_qrtr_backend *b = context;
    return value && value->epoch == b->epoch && nezha_als_frontend_sample(value) ?
        NezhaAlsClientOkay : NezhaAlsClientFrontend;
}
static int drain_intervals(struct nezha_als_qrtr_backend *b);
struct ReceiveArg {
    struct nezha_als_qrtr_backend *backend;
    uint32_t node, port, bytes;
    const uint8_t *qmi;
    uint64_t now_us, receipt_ticks;
};
static int receive_action(void *context) {
    struct ReceiveArg *arg = context;
    struct nezha_als_qrtr_backend *b = arg->backend;
    int result = drain_intervals(b);
    return result ? result : nezha_als_ssc_receive(b->client, arg->node, arg->port,
        arg->qmi, arg->bytes, arg->now_us, arg->receipt_ticks);
}
static void wire_failure(const struct ReceiveArg *arg, int result) {
    // ALS011 bound the real sensor and ACK, then rejected its next indication.
    // Retain that borrowed frame before the owner reuses its receive buffer.
    // One terminal report, in 64-byte console rows with explicit offsets.
    static const char digits[] = "0123456789abcdef";
    const uint32_t limit = arg->bytes < TCS3760_SNS_QMI_REPORT_MAX ?
        arg->bytes : TCS3760_SNS_QMI_REPORT_MAX;
    for (uint32_t offset = 0; offset < limit; offset += 64) {
        char hex[129];
        uint32_t count = limit - offset;
        if (count > 64) count = 64;
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t byte = arg->qmi[offset + i];
            hex[2*i] = digits[byte >> 4]; hex[2*i+1] = digits[byte & 15];
        }
        hex[2*count] = 0;
        printf("NEZHA ALS WIRE: rc=%d bytes=%u captured=%u offset=%u hex=%s\n",
               result, arg->bytes, limit, offset, hex);
    }
}
static void receive(void *context, QcomQrtr *q, uint32_t local, uint32_t node, uint32_t from,
                     const uint8_t *qmi, uint32_t bytes) {
    struct nezha_als_qrtr_backend *b = context;
    if (b->stopped || b->error || !b->client) return;
    if (q != b->router || local != b->local_port || !announced(b, b->endpoint)) {
        fail(b, NezhaAlsClientIdentity); return;
    }
    // This callback executes on the existing owner worker. The borrowed QRTR
    // buffer is decoded before return; the client retains only decoded fields.
    struct ReceiveArg arg = {b, node, from, bytes, qmi,
        b->ops.now_us(b->ops.context), b->ops.receipt_ticks(b->ops.context)};
    int result = NezhaAlsClientFrontend;
    if (!nezha_als_frontend_worker(receive_action, &arg, &result)) result = NezhaAlsClientFrontend;
    if (result < 0) {
        if (result == NezhaAlsClientWire) wire_failure(&arg, result);
        fail(b, result);
    }
}
int32_t nezha_als_qrtr_request_interval(void *context, uint32_t interval) {
    struct nezha_als_qrtr_backend *b = context;
    b->ops.lock(b->ops.context);
    int32_t result = 0;
    if (!b->accepting) result = b->ops.interval_not_ready;
    else if (b->count == NEZHA_ALS_INTERVAL_SLOTS) result = b->ops.interval_no_space;
    else {
        b->intervals[(b->head + b->count) % NEZHA_ALS_INTERVAL_SLOTS] = interval;
        b->count++;
        b->accepted_interval_us = interval;
    }
    b->ops.unlock(b->ops.context);
    if (!result) b->ops.wake(b->ops.context);
    return result;
}
int nezha_als_qrtr_prepare(struct nezha_als_qrtr_backend *b, void *provider,
                          QcomQrtr *q, uint32_t local_port,
                          const struct nezha_als_qrtr_ops *ops, uint64_t epoch,
                          uint32_t initial_interval) {
    if (!b || !provider || !q || !q->send || !ops || !ops->lock || !ops->unlock ||
        !ops->wake || !ops->online || !ops->now_us || !ops->receipt_ticks || !ops->tx_room ||
        !ops->interval_not_ready || !ops->interval_no_space || !epoch || !initial_interval ||
        b->epoch || b->router || !local_port || local_port >= QCOM_QRTR_PORT_CTRL)
        return NezhaAlsClientArgument;
    *b = (struct nezha_als_qrtr_backend){0};
    b->ops = *ops; b->router = q; b->local_port = local_port; b->epoch = epoch;
    b->initial_interval_us = initial_interval; b->worker_interval_us = initial_interval;
    b->accepted_interval_us = initial_interval;
    if (qcom_qrtr_add_client(q, local_port, receive, b)) {
        b->error = NezhaAlsClientArgument; b->stopped = true; return b->error;
    }
    const struct nezha_als_backend_ops frontend_ops = {b, nezha_als_qrtr_request_interval, initial_interval};
    if (!nezha_als_frontend_prepare(provider, &frontend_ops, epoch)) {
        b->error = NezhaAlsClientFrontend; b->stopped = true; return b->error;
    }
    b->ops.lock(b->ops.context); b->accepting = true; b->ops.unlock(b->ops.context);
    return NezhaAlsClientOkay;
}
int nezha_als_qrtr_select(struct nezha_als_qrtr_backend *b,
                         struct nezha_als_ssc_client *client,
                         struct nezha_als_endpoint endpoint, uint64_t epoch) {
    if (!b || !b->epoch || b->stopped || !client || client->epoch ||
        client == b->client || !epoch || epoch < b->epoch ||
        (b->client && (!b->error || epoch == b->epoch))) return NezhaAlsClientArgument;
    if (!announced(b, endpoint)) return NezhaAlsClientIdentity;
    if (epoch > b->epoch && !nezha_als_frontend_begin(epoch)) return fail(b, NezhaAlsClientFrontend);
    b->epoch = epoch; b->endpoint = endpoint; b->client = client; b->error = 0;
    const struct nezha_als_ssc_ops client_ops = {b, send, ready, sample};
    int result = nezha_als_ssc_begin(client, &client_ops, endpoint, b->local_port, epoch,
                                    b->initial_interval_us, b->ops.now_us(b->ops.context));
    if (result) return fail(b, result);
    b->ops.lock(b->ops.context); b->accepting = true; b->ops.unlock(b->ops.context);
    return NezhaAlsClientOkay;
}
static int drain_intervals(struct nezha_als_qrtr_backend *b) {
    // HID intervalAction uses this same frontend gate. Queue entries cannot be
    // accepted between this bounded drain and the client's ready/sample calls.
    for (unsigned i = 0; i < NEZHA_ALS_INTERVAL_SLOTS; ++i) {
        uint32_t interval = 0;
        b->ops.lock(b->ops.context);
        bool present = b->count != 0;
        if (present) {
            interval = b->intervals[b->head];
            b->head = (b->head + 1) % NEZHA_ALS_INTERVAL_SLOTS; b->count--;
        }
        b->ops.unlock(b->ops.context);
        if (!present) break;
        b->worker_interval_us = interval;
        if (b->client) {
            int result = nezha_als_ssc_interval(b->client, interval);
            if (result) return result;
        }
    }
    if (!b->client) return NezhaAlsClientOkay;
    // A new owned session retains the last accepted requested interval, but
    // the client and frontend still require a newly received valid sample.
    return nezha_als_ssc_interval(b->client, b->worker_interval_us);
}
static int poll_action(void *context) {
    struct nezha_als_qrtr_backend *b = context;
    int result = drain_intervals(b);
    if (result) return result;
    return b->client ? nezha_als_ssc_poll(b->client, b->ops.now_us(b->ops.context)) : NezhaAlsClientAgain;
}
int nezha_als_qrtr_poll(struct nezha_als_qrtr_backend *b) {
    if (!b || !b->epoch || b->stopped) return NezhaAlsClientArgument;
    if (b->error) return b->error;
    if (b->client && !announced(b, b->endpoint)) return fail(b, NezhaAlsClientIdentity);
    int result = NezhaAlsClientFrontend;
    if (!nezha_als_frontend_worker(poll_action, b, &result)) return fail(b, NezhaAlsClientFrontend);
    return result < 0 ? fail(b, result) : result;
}
void nezha_als_qrtr_stop(struct nezha_als_qrtr_backend *b) {
    if (!b || !b->epoch || b->stopped) return;
    nezha_als_qrtr_lost(b, NezhaAlsClientTransport);
    b->stopped = true;
    nezha_als_frontend_stop();
}
