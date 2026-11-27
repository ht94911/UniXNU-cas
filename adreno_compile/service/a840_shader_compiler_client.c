/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "a840_shader_compiler_client.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    SC_CLIENT_DEFAULT_TIMEOUT_MS = 5000u,
    SC_CLIENT_DEFAULT_INIT_BACKOFF_MS = 50u,
    SC_CLIENT_DEFAULT_MAX_BACKOFF_MS = 1000u,
    SC_CLIENT_RECV_POLL_MS = 5u,
    SC_CLIENT_SEND_SLICE_MS = 25u
};

typedef struct ScPending {
    A840ScFrameHeader request;
    uint64_t deadline_ms;
    uint8_t *payload;
    uint8_t *record;
    size_t record_bytes;
    A840ScClientCompletionFn completion;
    void *user_ctx;
    A840ScStatus result;
    bool sent;
    bool done;
    bool canceled;
    bool abandoned;
    struct ScPending *next;
} ScPending;

struct A840ScClient {
    A840ScClientConfig cfg;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t io_thread;
    pthread_t cb_thread;
    ScPending *pending_head;
    ScPending **pending_tail;
    ScPending *completed_head;
    ScPending **completed_tail;
    uint64_t retry_after_ms;
    uint32_t current_backoff_ms;
    uint32_t init_backoff_ms;
    uint32_t max_backoff_ms;
    bool stopping;
    bool io_exited;
};

static _Atomic uint64_t g_next_request_id = 1u;

static uint64_t now_monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Darwin does not expose Linux pthread_condattr_setclock(CLOCK_MONOTONIC);
 * use pthread_cond_timedwait_relative_np on __APPLE__ and CLOCK_MONOTONIC
 * condattr on Linux/POSIX.
 */
static int client_cond_init(pthread_cond_t *cond)
{
#if defined(__APPLE__)
    return pthread_cond_init(cond, NULL);
#else
    pthread_condattr_t attr;
    int rc = pthread_condattr_init(&attr);
    if (rc != 0)
        return rc;
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0)
        rc = pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
#endif
}

static void client_cond_wait_until(A840ScClient *client, uint64_t deadline_ms,
    uint64_t now_ms)
{
    if (now_ms >= deadline_ms)
        return;
#if defined(__APPLE__)
    uint64_t delta = deadline_ms - now_ms;
    struct timespec rel;
    rel.tv_sec = (time_t)(delta / 1000u);
    rel.tv_nsec = (long)((delta % 1000u) * 1000000u);
    (void)pthread_cond_timedwait_relative_np(&client->cond, &client->mutex, &rel);
#else
    (void)now_ms;
    struct timespec abs_ts;
    abs_ts.tv_sec = (time_t)(deadline_ms / 1000u);
    abs_ts.tv_nsec = (long)((deadline_ms % 1000u) * 1000000u);
    (void)pthread_cond_timedwait(&client->cond, &client->mutex, &abs_ts);
#endif
}

static void pending_free(ScPending *p)
{
    if (!p)
        return;
    free(p->payload);
    free(p->record);
    free(p);
}

static ScPending **find_pending_slot(A840ScClient *client, uint64_t request_id)
{
    ScPending **slot = &client->pending_head;
    while (*slot && (*slot)->request.request_id != request_id)
        slot = &(*slot)->next;
    return slot;
}

/* Must be called with client->mutex held. Only io_thread removes entries from
 * pending_head; sync callers own their ScPending once done==true unless they
 * abandoned it on timeout.
 */
static void complete_pending_locked(A840ScClient *client, ScPending **slot,
    A840ScStatus status, uint8_t *record, size_t record_bytes)
{
    ScPending *p = *slot;
    *slot = p->next;
    if (!*slot)
        client->pending_tail = slot;
    p->next = NULL;

    free(p->payload);
    p->payload = NULL;
    p->result = status;
    p->record = record;
    p->record_bytes = record_bytes;
    p->done = true;

    if (p->abandoned) {
        pending_free(p);
    } else if (p->completion) {
        *client->completed_tail = p;
        client->completed_tail = &p->next;
    }
    pthread_cond_broadcast(&client->cond);
}

static A840ScStatus pending_expiry_status(const ScPending *p, uint64_t now_ms)
{
    if (p->canceled)
        return A840_SC_CANCELED;
    if (p->abandoned || now_ms >= p->deadline_ms)
        return A840_SC_TIMED_OUT;
    return A840_SC_OK;
}

static void fail_all_pending_locked(A840ScClient *client, A840ScStatus fallback,
    uint64_t now_ms)
{
    while (client->pending_head) {
        A840ScStatus exp = pending_expiry_status(client->pending_head, now_ms);
        complete_pending_locked(client, &client->pending_head,
            exp == A840_SC_OK ? fallback : exp, NULL, 0u);
    }
}

static void trip_circuit_breaker_locked(A840ScClient *client)
{
    uint64_t now = now_monotonic_ms();
    client->retry_after_ms = now + client->current_backoff_ms;
    if (client->current_backoff_ms < client->max_backoff_ms) {
        uint64_t doubled = (uint64_t)client->current_backoff_ms * 2u;
        client->current_backoff_ms = doubled < client->max_backoff_ms
                                         ? (uint32_t)doubled
                                         : client->max_backoff_ms;
    }
    fail_all_pending_locked(client, A840_SC_DAEMON_UNAVAILABLE, now);
}

static A840ScStatus io_send_frame(A840ScClient *client,
    const A840ScFrameHeader *hdr, const uint8_t *payload, uint64_t deadline_ms)
{
    uint8_t stack_buf[A840_SC_HEADER_BYTES];
    size_t cap = (size_t)A840_SC_HEADER_BYTES + (size_t)hdr->payload_bytes;
    uint8_t *wire = hdr->payload_bytes ? (uint8_t *)malloc(cap) : stack_buf;
    if (!wire)
        return A840_SC_OOM;

    size_t wire_len = 0u;
    A840ScStatus st = a840_sc_encode_frame(hdr, payload, wire, cap, &wire_len);
    if (st == A840_SC_OK) {
        uint64_t now = now_monotonic_ms();
        if (now >= deadline_ms) {
            st = A840_SC_TIMED_OUT;
        } else {
            uint64_t rem = deadline_ms - now;
            uint32_t budget = rem < SC_CLIENT_SEND_SLICE_MS
                                  ? (uint32_t)rem
                                  : SC_CLIENT_SEND_SLICE_MS;
            st = client->cfg.send_fn(client->cfg.user_ctx, wire, wire_len, budget);
            if (st != A840_SC_OK && st != A840_SC_TIMED_OUT)
                st = A840_SC_DAEMON_UNAVAILABLE;
        }
    }
    if (wire != stack_buf)
        free(wire);
    return st;
}

static A840ScStatus io_recv_reply(A840ScClient *client, uint32_t timeout_ms,
    A840ScFrameHeader *out_reply, uint8_t **out_record)
{
    *out_record = NULL;
    uint8_t *wire = NULL;
    size_t wire_len = 0u;
    A840ScStatus st = client->cfg.recv_fn(client->cfg.user_ctx, &wire, &wire_len, timeout_ms);
    if (st != A840_SC_OK) {
        free(wire);
        return st == A840_SC_TIMED_OUT ? A840_SC_TIMED_OUT : A840_SC_DAEMON_UNAVAILABLE;
    }

    const uint8_t *payload = NULL;
    st = a840_sc_decode_frame(wire, wire_len, client->cfg.stack_key,
        client->cfg.abi_fingerprint, out_reply, &payload);
    if (st != A840_SC_OK || out_reply->msg_type != A840_SC_MSG_COMPILE_REPLY) {
        free(wire);
        return A840_SC_PROTO_ERROR;
    }

    if (out_reply->payload_bytes != 0u) {
        A840PipelineRecord parsed;
        memset(&parsed, 0, sizeof(parsed));
        A840RecordStatus rst = a840_record_deserialize(payload, out_reply->payload_bytes,
            out_reply->stack_key, out_reply->abi_fingerprint,
            &out_reply->pipeline_key, &parsed);
        a840_record_free(&parsed);
        if (rst == A840_RECORD_OK) {
            memmove(wire, payload, out_reply->payload_bytes);
            *out_record = wire;
            return A840_SC_OK;
        }
        out_reply->status = (rst == A840_RECORD_OOM) ? A840_SC_OOM : A840_SC_PROTO_ERROR;
        out_reply->payload_bytes = 0u;
    }
    free(wire);
    return A840_SC_OK;
}

static bool reply_matches_pending(const ScPending *p, const A840ScFrameHeader *reply)
{
    return p && p->sent &&
           p->request.client_pid == reply->client_pid &&
           p->request.priority_class == reply->priority_class &&
           memcmp(p->request.pipeline_key.sha256, reply->pipeline_key.sha256, 32u) == 0;
}

static void *io_thread_main(void *arg)
{
    A840ScClient *client = (A840ScClient *)arg;
    bool prefer_recv = false;

    pthread_mutex_lock(&client->mutex);
    while (!client->stopping) {
        if (!client->pending_head) {
            pthread_cond_wait(&client->cond, &client->mutex);
            continue;
        }

        uint64_t now = now_monotonic_ms();
        ScPending **expired_slot = NULL;
        ScPending **send_slot = NULL;
        bool has_inflight_sent = false;
        A840ScStatus exp_status = A840_SC_OK;

        for (ScPending **slot = &client->pending_head; *slot; slot = &(*slot)->next) {
            ScPending *p = *slot;
            A840ScStatus st = pending_expiry_status(p, now);
            if (st != A840_SC_OK) {
                expired_slot = slot;
                exp_status = st;
                break;
            }
            if (p->sent) {
                has_inflight_sent = true;
            } else if (!send_slot ||
                       p->request.priority_class < (*send_slot)->request.priority_class) {
                send_slot = slot;
            }
        }

        if (expired_slot) {
            ScPending *p = *expired_slot;
            bool should_cancel_remote = p->sent &&
                (p->request.msg_type == A840_SC_MSG_COMPILE_REQUEST ||
                 p->request.msg_type == A840_SC_MSG_PREWARM_HINT);
            A840ScFrameHeader cancel_hdr = p->request;
            complete_pending_locked(client, expired_slot, exp_status, NULL, 0u);
            if (should_cancel_remote) {
                cancel_hdr.msg_type = A840_SC_MSG_CANCEL_REQUEST;
                cancel_hdr.payload_bytes = 0u;
                cancel_hdr.status = A840_SC_OK;
                pthread_mutex_unlock(&client->mutex);
                A840ScStatus cst = io_send_frame(client, &cancel_hdr, NULL,
                    now_monotonic_ms() + SC_CLIENT_SEND_SLICE_MS);
                pthread_mutex_lock(&client->mutex);
                if (cst == A840_SC_DAEMON_UNAVAILABLE)
                    trip_circuit_breaker_locked(client);
            }
            continue;
        }

        if (send_slot && (!has_inflight_sent || !prefer_recv)) {
            ScPending *p = *send_slot;
            pthread_mutex_unlock(&client->mutex);
            A840ScStatus sst = io_send_frame(client, &p->request, p->payload, p->deadline_ms);
            pthread_mutex_lock(&client->mutex);
            if (sst == A840_SC_OK) {
                p->sent = true;
                free(p->payload);
                p->payload = NULL;
            } else if (sst == A840_SC_DAEMON_UNAVAILABLE) {
                trip_circuit_breaker_locked(client);
            } else if (sst == A840_SC_TIMED_OUT) {
                /* Retry until deadline expires. */
                A840ScStatus est = pending_expiry_status(p, now_monotonic_ms());
                if (est != A840_SC_OK) {
                    ScPending **cur = find_pending_slot(client, p->request.request_id);
                    if (*cur == p)
                        complete_pending_locked(client, cur, est, NULL, 0u);
                }
            } else {
                ScPending **cur = find_pending_slot(client, p->request.request_id);
                if (*cur == p)
                    complete_pending_locked(client, cur, sst, NULL, 0u);
            }
            prefer_recv = true;
            continue;
        }

        A840ScFrameHeader reply;
        memset(&reply, 0, sizeof(reply));
        uint8_t *record = NULL;
        uint32_t poll_ms = send_slot ? 0u : SC_CLIENT_RECV_POLL_MS;

        pthread_mutex_unlock(&client->mutex);
        A840ScStatus rst = io_recv_reply(client, poll_ms, &reply, &record);
        pthread_mutex_lock(&client->mutex);

        if (rst == A840_SC_OK) {
            ScPending **slot = find_pending_slot(client, reply.request_id);
            if (*slot && reply_matches_pending(*slot, &reply) &&
                pending_expiry_status(*slot, now_monotonic_ms()) == A840_SC_OK) {
                client->retry_after_ms = 0u;
                client->current_backoff_ms = client->init_backoff_ms;
                complete_pending_locked(client, slot, (A840ScStatus)reply.status,
                    record, reply.payload_bytes);
                record = NULL;
            }
        } else if (rst == A840_SC_DAEMON_UNAVAILABLE) {
            trip_circuit_breaker_locked(client);
        }
        free(record);
        prefer_recv = false;
    }

    fail_all_pending_locked(client, A840_SC_SHUTDOWN, now_monotonic_ms());
    client->io_exited = true;
    pthread_cond_broadcast(&client->cond);
    pthread_mutex_unlock(&client->mutex);
    return NULL;
}

static void *cb_thread_main(void *arg)
{
    A840ScClient *client = (A840ScClient *)arg;

    pthread_mutex_lock(&client->mutex);
    while (!client->io_exited || client->completed_head) {
        ScPending *p = client->completed_head;
        if (!p) {
            pthread_cond_wait(&client->cond, &client->mutex);
            continue;
        }
        client->completed_head = p->next;
        if (!client->completed_head)
            client->completed_tail = &client->completed_head;
        p->next = NULL;
        pthread_mutex_unlock(&client->mutex);

        p->completion(p->user_ctx, p->request.request_id, p->result,
            p->record, p->record_bytes);
        pending_free(p);

        pthread_mutex_lock(&client->mutex);
    }
    pthread_mutex_unlock(&client->mutex);
    return NULL;
}

A840ScStatus a840_sc_client_create(const A840ScClientConfig *cfg,
    A840ScClient **out_client)
{
    if (!cfg || !out_client || cfg->stack_key == 0u || cfg->abi_fingerprint == 0u ||
        cfg->client_pid == 0u || !cfg->send_fn || !cfg->recv_fn)
        return A840_SC_INVALID_ARG;
    if (cfg->abi_fingerprint != a840_compile_abi_fingerprint())
        return A840_SC_ABI_MISMATCH;

    uint32_t init_bo = cfg->initial_backoff_ms ? cfg->initial_backoff_ms : SC_CLIENT_DEFAULT_INIT_BACKOFF_MS;
    uint32_t max_bo = cfg->max_backoff_ms ? cfg->max_backoff_ms : SC_CLIENT_DEFAULT_MAX_BACKOFF_MS;
    if (max_bo < init_bo)
        max_bo = init_bo;

    A840ScClient *client = (A840ScClient *)calloc(1, sizeof(*client));
    if (!client)
        return A840_SC_OOM;

    client->cfg = *cfg;
    client->pending_tail = &client->pending_head;
    client->completed_tail = &client->completed_head;
    client->init_backoff_ms = init_bo;
    client->max_backoff_ms = max_bo;
    client->current_backoff_ms = init_bo;

    if (pthread_mutex_init(&client->mutex, NULL) != 0) {
        free(client);
        return A840_SC_OOM;
    }
    if (client_cond_init(&client->cond) != 0) {
        pthread_mutex_destroy(&client->mutex);
        free(client);
        return A840_SC_OOM;
    }
    if (pthread_create(&client->io_thread, NULL, io_thread_main, client) != 0) {
        pthread_cond_destroy(&client->cond);
        pthread_mutex_destroy(&client->mutex);
        free(client);
        return A840_SC_OOM;
    }
    if (pthread_create(&client->cb_thread, NULL, cb_thread_main, client) != 0) {
        pthread_mutex_lock(&client->mutex);
        client->stopping = true;
        pthread_cond_broadcast(&client->cond);
        pthread_mutex_unlock(&client->mutex);
        pthread_join(client->io_thread, NULL);
        pthread_cond_destroy(&client->cond);
        pthread_mutex_destroy(&client->mutex);
        free(client);
        return A840_SC_OOM;
    }

    *out_client = client;
    return A840_SC_OK;
}

static A840ScStatus create_pending(A840ScClient *client, A840ScMessageType msg_type,
    A840ScPriority prio, const A840PipelineKey *pipeline_key,
    const void *payload, size_t payload_bytes, uint32_t timeout_ms,
    ScPending **out_pending)
{
    if (!client || payload_bytes > A840_SC_MAX_PAYLOAD_BYTES ||
        (payload_bytes != 0u && !payload))
        return A840_SC_INVALID_ARG;
    if (msg_type != A840_SC_MSG_PING_STATUS &&
        (!pipeline_key || a840_sc_pipeline_key_is_zero(pipeline_key)))
        return A840_SC_INVALID_ARG;

    ScPending *p = (ScPending *)calloc(1, sizeof(*p));
    if (!p)
        return A840_SC_OOM;

    if (payload_bytes != 0u) {
        p->payload = (uint8_t *)malloc(payload_bytes);
        if (!p->payload) {
            free(p);
            return A840_SC_OOM;
        }
        memcpy(p->payload, payload, payload_bytes);
    }

    p->request.magic = A840_SC_PROTO_MAGIC;
    p->request.version = A840_SC_PROTO_VERSION;
    p->request.msg_type = (uint32_t)msg_type;
    p->request.priority_class = (uint32_t)prio;
    p->request.stack_key = client->cfg.stack_key;
    p->request.abi_fingerprint = client->cfg.abi_fingerprint;
    p->request.client_pid = client->cfg.client_pid;
    p->request.status = A840_SC_OK;
    p->request.payload_bytes = (uint32_t)payload_bytes;
    if (pipeline_key)
        p->request.pipeline_key = *pipeline_key;

    do {
        p->request.request_id = atomic_fetch_add_explicit(&g_next_request_id, 1u,
            memory_order_relaxed);
    } while (p->request.request_id == 0u);

    uint32_t effective_to = timeout_ms ? timeout_ms : SC_CLIENT_DEFAULT_TIMEOUT_MS;
    p->deadline_ms = now_monotonic_ms() + effective_to;
    *out_pending = p;
    return A840_SC_OK;
}

static A840ScStatus enqueue_pending(A840ScClient *client, ScPending *p,
    uint64_t *out_request_id)
{
    A840ScStatus st = A840_SC_OK;
    pthread_mutex_lock(&client->mutex);
    if (client->stopping) {
        st = A840_SC_SHUTDOWN;
    } else if (now_monotonic_ms() < client->retry_after_ms) {
        st = A840_SC_DAEMON_UNAVAILABLE;
    } else {
        *client->pending_tail = p;
        client->pending_tail = &p->next;
        if (out_request_id)
            *out_request_id = p->request.request_id;
        pthread_cond_broadcast(&client->cond);
    }
    pthread_mutex_unlock(&client->mutex);

    if (st != A840_SC_OK)
        pending_free(p);
    return st;
}

static A840ScStatus execute_sync(A840ScClient *client, ScPending *p,
    uint8_t **out_record_blob, size_t *out_record_bytes)
{
    A840ScStatus st = enqueue_pending(client, p, NULL);
    if (st != A840_SC_OK)
        return st;

    pthread_mutex_lock(&client->mutex);
    while (!p->done) {
        uint64_t now = now_monotonic_ms();
        if (now >= p->deadline_ms) {
            p->abandoned = true;
            pthread_cond_broadcast(&client->cond);
            pthread_mutex_unlock(&client->mutex);
            return A840_SC_TIMED_OUT;
        }
        client_cond_wait_until(client, p->deadline_ms, now);
    }
    pthread_mutex_unlock(&client->mutex);

    st = p->result;
    if (st == A840_SC_OK && out_record_blob && out_record_bytes) {
        *out_record_blob = p->record;
        *out_record_bytes = p->record_bytes;
        p->record = NULL;
    }
    pending_free(p);
    return st;
}

A840ScStatus a840_sc_client_compile_sync(A840ScClient *client,
    const A840PipelineKey *pipeline_key, const void *payload,
    size_t payload_bytes, uint32_t timeout_ms,
    uint8_t **out_record_blob, size_t *out_record_bytes)
{
    if (!out_record_blob || !out_record_bytes)
        return A840_SC_INVALID_ARG;
    ScPending *p = NULL;
    A840ScStatus st = create_pending(client, A840_SC_MSG_COMPILE_REQUEST,
        A840_SC_PRIO_FG_SYNC, pipeline_key, payload, payload_bytes, timeout_ms, &p);
    if (st != A840_SC_OK)
        return st;
    return execute_sync(client, p, out_record_blob, out_record_bytes);
}

A840ScStatus a840_sc_client_compile_async(A840ScClient *client,
    const A840PipelineKey *pipeline_key, const void *payload,
    size_t payload_bytes, uint32_t timeout_ms,
    A840ScClientCompletionFn completion, void *user_ctx,
    uint64_t *out_request_id)
{
    if (!completion || !out_request_id)
        return A840_SC_INVALID_ARG;
    ScPending *p = NULL;
    A840ScStatus st = create_pending(client, A840_SC_MSG_COMPILE_REQUEST,
        A840_SC_PRIO_BG_ASYNC, pipeline_key, payload, payload_bytes, timeout_ms, &p);
    if (st != A840_SC_OK)
        return st;
    p->completion = completion;
    p->user_ctx = user_ctx;
    return enqueue_pending(client, p, out_request_id);
}

A840ScStatus a840_sc_client_prewarm_async(A840ScClient *client,
    const A840PipelineKey *pipeline_key, const void *payload,
    size_t payload_bytes, uint32_t timeout_ms,
    A840ScClientCompletionFn completion, void *user_ctx,
    uint64_t *out_request_id)
{
    if (!completion || !out_request_id)
        return A840_SC_INVALID_ARG;
    ScPending *p = NULL;
    A840ScStatus st = create_pending(client, A840_SC_MSG_PREWARM_HINT,
        A840_SC_PRIO_WARMUP_LOW, pipeline_key, payload, payload_bytes, timeout_ms, &p);
    if (st != A840_SC_OK)
        return st;
    p->completion = completion;
    p->user_ctx = user_ctx;
    return enqueue_pending(client, p, out_request_id);
}

A840ScStatus a840_sc_client_cancel(A840ScClient *client, uint64_t request_id)
{
    if (!client || request_id == 0u)
        return A840_SC_INVALID_ARG;
    A840ScStatus st = A840_SC_NOT_FOUND;
    pthread_mutex_lock(&client->mutex);
    ScPending *p = *find_pending_slot(client, request_id);
    if (p && !p->abandoned && !p->canceled && now_monotonic_ms() < p->deadline_ms) {
        p->canceled = true;
        st = A840_SC_OK;
        pthread_cond_broadcast(&client->cond);
    }
    pthread_mutex_unlock(&client->mutex);
    return st;
}

A840ScStatus a840_sc_client_ping(A840ScClient *client, uint32_t timeout_ms)
{
    ScPending *p = NULL;
    A840ScStatus st = create_pending(client, A840_SC_MSG_PING_STATUS,
        A840_SC_PRIO_FG_SYNC, NULL, NULL, 0u, timeout_ms, &p);
    if (st != A840_SC_OK)
        return st;
    return execute_sync(client, p, NULL, NULL);
}

bool a840_sc_client_is_circuit_open(A840ScClient *client)
{
    if (!client)
        return false;
    pthread_mutex_lock(&client->mutex);
    bool open = now_monotonic_ms() < client->retry_after_ms;
    pthread_mutex_unlock(&client->mutex);
    return open;
}

void a840_sc_client_destroy(A840ScClient *client)
{
    if (!client)
        return;
    pthread_mutex_lock(&client->mutex);
    client->stopping = true;
    pthread_cond_broadcast(&client->cond);
    pthread_mutex_unlock(&client->mutex);

    pthread_join(client->io_thread, NULL);
    pthread_join(client->cb_thread, NULL);
    pthread_cond_destroy(&client->cond);
    pthread_mutex_destroy(&client->mutex);
    free(client);
}
