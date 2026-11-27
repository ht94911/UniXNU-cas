/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_shader_compiler_service.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

enum {
    SC_OFF_MAGIC = 0u,
    SC_OFF_VERSION = 4u,
    SC_OFF_MSG_TYPE = 8u,
    SC_OFF_PRIORITY = 12u,
    SC_OFF_STACK_KEY = 16u,
    SC_OFF_ABI_FP = 24u,
    SC_OFF_PIPELINE_KEY = 32u,
    SC_OFF_REQUEST_ID = 64u,
    SC_OFF_CLIENT_PID = 72u,
    SC_OFF_STATUS = 76u,
    SC_OFF_PAYLOAD_BYTES = 80u,
    SC_OFF_RESERVED0 = 84u,
    SC_OFF_RESERVED1 = 88u,
    SC_OFF_PAYLOAD_SHA256 = 96u
};

static void put_le32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xffu);
    dst[1] = (uint8_t)((v >> 8) & 0xffu);
    dst[2] = (uint8_t)((v >> 16) & 0xffu);
    dst[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_le64(uint8_t *dst, uint64_t v)
{
    put_le32(dst, (uint32_t)(v & UINT32_MAX));
    put_le32(dst + 4, (uint32_t)(v >> 32));
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static uint64_t get_le64(const uint8_t *src)
{
    return (uint64_t)get_le32(src) | ((uint64_t)get_le32(src + 4) << 32);
}

static bool regions_overlap(const void *a, size_t a_len, const void *b, size_t b_len)
{
    uintptr_t xa = (uintptr_t)a;
    uintptr_t xb = (uintptr_t)b;
    return a_len != 0u && b_len != 0u &&
           (xa <= xb ? (xb - xa) < a_len : (xa - xb) < b_len);
}

const char *a840_sc_status_string(A840ScStatus status)
{
    static const char *const names[] = {
        "OK",
        "INVALID_ARG",
        "PROTO_ERROR",
        "TRUNCATED",
        "PAYLOAD_TOO_LARGE",
        "STACK_MISMATCH",
        "ABI_MISMATCH",
        "HASH_MISMATCH",
        "RESERVED_NONZERO",
        "UNKNOWN_PRIORITY",
        "UNKNOWN_MSG_TYPE",
        "COMPILE_FAILED",
        "TIMED_OUT",
        "CANCELED",
        "SHUTDOWN",
        "raw:compiler.daemon_unavailable",
        "NOT_FOUND",
        "DUPLICATE_REQUEST",
        "OOM"
    };
    return (unsigned)status < (sizeof(names) / sizeof(names[0])) ? names[status] : "UNKNOWN";
}

bool a840_sc_pipeline_key_is_zero(const A840PipelineKey *key)
{
    if (!key)
        return true;
    uint8_t acc = 0u;
    for (size_t i = 0; i < sizeof(key->sha256); ++i)
        acc |= key->sha256[i];
    return acc == 0u;
}

A840ScStatus a840_sc_validate_header(const A840ScFrameHeader *hdr,
    uint64_t expected_stack_key, uint64_t expected_abi_fingerprint)
{
    if (!hdr || expected_stack_key == 0u || expected_abi_fingerprint == 0u)
        return A840_SC_INVALID_ARG;
    if (hdr->magic != A840_SC_PROTO_MAGIC || hdr->version != A840_SC_PROTO_VERSION)
        return A840_SC_PROTO_ERROR;
    if (hdr->reserved0 != 0u || hdr->reserved1 != 0u)
        return A840_SC_RESERVED_NONZERO;
    if (hdr->msg_type < A840_SC_MSG_COMPILE_REQUEST ||
        hdr->msg_type > A840_SC_MSG_PREWARM_HINT)
        return A840_SC_UNKNOWN_MSG_TYPE;
    if (hdr->priority_class > A840_SC_PRIO_WARMUP_LOW)
        return A840_SC_UNKNOWN_PRIORITY;
    if (hdr->payload_bytes > A840_SC_MAX_PAYLOAD_BYTES)
        return A840_SC_PAYLOAD_TOO_LARGE;
    if (hdr->stack_key != expected_stack_key)
        return A840_SC_STACK_MISMATCH;
    if (hdr->abi_fingerprint != expected_abi_fingerprint)
        return A840_SC_ABI_MISMATCH;
    if (hdr->request_id == 0u || hdr->client_pid == 0u || hdr->status > A840_SC_OOM)
        return A840_SC_PROTO_ERROR;

    const bool keyed = !a840_sc_pipeline_key_is_zero(&hdr->pipeline_key);
    const bool has_payload = hdr->payload_bytes != 0u;
    const bool status_ok = hdr->status == A840_SC_OK;

    switch (hdr->msg_type) {
    case A840_SC_MSG_COMPILE_REQUEST:
        return (keyed && status_ok) ? A840_SC_OK : A840_SC_PROTO_ERROR;
    case A840_SC_MSG_PREWARM_HINT:
        return (keyed && status_ok && hdr->priority_class == A840_SC_PRIO_WARMUP_LOW)
                   ? A840_SC_OK
                   : A840_SC_PROTO_ERROR;
    case A840_SC_MSG_CANCEL_REQUEST:
        return (keyed && status_ok && !has_payload) ? A840_SC_OK : A840_SC_PROTO_ERROR;
    case A840_SC_MSG_PING_STATUS:
        return (!keyed && status_ok && !has_payload &&
                hdr->priority_class == A840_SC_PRIO_FG_SYNC)
                   ? A840_SC_OK
                   : A840_SC_PROTO_ERROR;
    case A840_SC_MSG_COMPILE_REPLY:
        if (keyed)
            return (has_payload == status_ok) ? A840_SC_OK : A840_SC_PROTO_ERROR;
        return (!has_payload && hdr->priority_class == A840_SC_PRIO_FG_SYNC)
                   ? A840_SC_OK
                   : A840_SC_PROTO_ERROR;
    default:
        return A840_SC_UNKNOWN_MSG_TYPE;
    }
}

A840ScStatus a840_sc_encode_frame(const A840ScFrameHeader *hdr,
    const void *payload, uint8_t *out_frame, size_t out_capacity,
    size_t *out_bytes)
{
    if (!hdr || !out_frame || !out_bytes || (hdr->payload_bytes != 0u && !payload))
        return A840_SC_INVALID_ARG;

    A840ScFrameHeader normalized = *hdr;
    if (normalized.magic == 0u)
        normalized.magic = A840_SC_PROTO_MAGIC;
    if (normalized.version == 0u)
        normalized.version = A840_SC_PROTO_VERSION;

    A840ScStatus st = a840_sc_validate_header(&normalized, normalized.stack_key,
        normalized.abi_fingerprint);
    if (st != A840_SC_OK)
        return st;

    const size_t total = (size_t)A840_SC_HEADER_BYTES + (size_t)normalized.payload_bytes;
    if (out_capacity < total)
        return A840_SC_INVALID_ARG;
    if (regions_overlap(hdr, sizeof(*hdr), out_frame, total) ||
        regions_overlap(payload, normalized.payload_bytes, out_frame, total) ||
        regions_overlap(out_bytes, sizeof(*out_bytes), out_frame, total))
        return A840_SC_INVALID_ARG;

    uint8_t digest[32];
    if (a840_record_sha256(normalized.payload_bytes ? payload : "",
            normalized.payload_bytes, digest) != A840_RECORD_OK)
        return A840_SC_INVALID_ARG;

    put_le32(out_frame + SC_OFF_MAGIC, normalized.magic);
    put_le32(out_frame + SC_OFF_VERSION, normalized.version);
    put_le32(out_frame + SC_OFF_MSG_TYPE, normalized.msg_type);
    put_le32(out_frame + SC_OFF_PRIORITY, normalized.priority_class);
    put_le64(out_frame + SC_OFF_STACK_KEY, normalized.stack_key);
    put_le64(out_frame + SC_OFF_ABI_FP, normalized.abi_fingerprint);
    memcpy(out_frame + SC_OFF_PIPELINE_KEY, normalized.pipeline_key.sha256, 32u);
    put_le64(out_frame + SC_OFF_REQUEST_ID, normalized.request_id);
    put_le32(out_frame + SC_OFF_CLIENT_PID, normalized.client_pid);
    put_le32(out_frame + SC_OFF_STATUS, normalized.status);
    put_le32(out_frame + SC_OFF_PAYLOAD_BYTES, normalized.payload_bytes);
    put_le32(out_frame + SC_OFF_RESERVED0, 0u);
    put_le64(out_frame + SC_OFF_RESERVED1, 0u);
    memcpy(out_frame + SC_OFF_PAYLOAD_SHA256, digest, 32u);
    if (normalized.payload_bytes != 0u)
        memcpy(out_frame + A840_SC_HEADER_BYTES, payload, normalized.payload_bytes);
    *out_bytes = total;
    return A840_SC_OK;
}

A840ScStatus a840_sc_peek_frame_length(const uint8_t *prefix, size_t prefix_bytes,
    size_t *out_total_bytes)
{
    if (!prefix || !out_total_bytes)
        return A840_SC_INVALID_ARG;
    if (prefix_bytes < A840_SC_HEADER_BYTES)
        return A840_SC_TRUNCATED;
    if (get_le32(prefix + SC_OFF_MAGIC) != A840_SC_PROTO_MAGIC ||
        get_le32(prefix + SC_OFF_VERSION) != A840_SC_PROTO_VERSION)
        return A840_SC_PROTO_ERROR;
    uint32_t payload_bytes = get_le32(prefix + SC_OFF_PAYLOAD_BYTES);
    if (payload_bytes > A840_SC_MAX_PAYLOAD_BYTES)
        return A840_SC_PAYLOAD_TOO_LARGE;
    *out_total_bytes = (size_t)A840_SC_HEADER_BYTES + (size_t)payload_bytes;
    return A840_SC_OK;
}

A840ScStatus a840_sc_decode_frame(const uint8_t *frame, size_t frame_bytes,
    uint64_t expected_stack_key, uint64_t expected_abi_fingerprint,
    A840ScFrameHeader *out_hdr, const uint8_t **out_payload)
{
    if (!frame || !out_hdr || !out_payload ||
        expected_stack_key == 0u || expected_abi_fingerprint == 0u)
        return A840_SC_INVALID_ARG;
    if (frame_bytes < A840_SC_HEADER_BYTES)
        return A840_SC_TRUNCATED;

    A840ScFrameHeader decoded;
    memset(&decoded, 0, sizeof(decoded));
    decoded.magic = get_le32(frame + SC_OFF_MAGIC);
    decoded.version = get_le32(frame + SC_OFF_VERSION);
    decoded.msg_type = get_le32(frame + SC_OFF_MSG_TYPE);
    decoded.priority_class = get_le32(frame + SC_OFF_PRIORITY);
    decoded.stack_key = get_le64(frame + SC_OFF_STACK_KEY);
    decoded.abi_fingerprint = get_le64(frame + SC_OFF_ABI_FP);
    memcpy(decoded.pipeline_key.sha256, frame + SC_OFF_PIPELINE_KEY, 32u);
    decoded.request_id = get_le64(frame + SC_OFF_REQUEST_ID);
    decoded.client_pid = get_le32(frame + SC_OFF_CLIENT_PID);
    decoded.status = get_le32(frame + SC_OFF_STATUS);
    decoded.payload_bytes = get_le32(frame + SC_OFF_PAYLOAD_BYTES);
    decoded.reserved0 = get_le32(frame + SC_OFF_RESERVED0);
    decoded.reserved1 = get_le64(frame + SC_OFF_RESERVED1);
    memcpy(decoded.payload_sha256, frame + SC_OFF_PAYLOAD_SHA256, 32u);

    A840ScStatus st = a840_sc_validate_header(&decoded, expected_stack_key,
        expected_abi_fingerprint);
    if (st != A840_SC_OK)
        return st;

    const size_t avail_payload = frame_bytes - A840_SC_HEADER_BYTES;
    if (avail_payload < decoded.payload_bytes)
        return A840_SC_TRUNCATED;
    if (avail_payload > decoded.payload_bytes)
        return A840_SC_PROTO_ERROR;

    const uint8_t *body = decoded.payload_bytes ? (frame + A840_SC_HEADER_BYTES) : NULL;
    uint8_t digest[32];
    if (a840_record_sha256(body ? (const void *)body : (const void *)"",
            decoded.payload_bytes, digest) != A840_RECORD_OK)
        return A840_SC_PROTO_ERROR;
    if (memcmp(digest, decoded.payload_sha256, 32u) != 0)
        return A840_SC_HASH_MISMATCH;

    *out_hdr = decoded;
    *out_payload = body;
    return A840_SC_OK;
}

/* ---------------- Service Core Engine ---------------- */

enum {
    SC_INFLIGHT_BUCKETS = 256u
};

typedef struct ScWaiter {
    A840ScFrameHeader request;
    A840ScServiceCompletionFn completion;
    A840ScReplyFrameFn reply_frame;
    void *user_ctx;
    struct ScWaiter *next;
} ScWaiter;

typedef struct ScJob {
    A840ScFrameHeader request;
    struct ScJob *hash_next;
    struct ScJob *queue_prev;
    struct ScJob *queue_next;
    ScWaiter *waiters;
    A840ScPriority priority;
    uint32_t worker_id;
    bool running;
    bool in_queue;
    uint32_t active_boosts;
    uint8_t payload[];
} ScJob;

typedef struct ScQueue {
    ScJob *head;
    ScJob *tail;
} ScQueue;

typedef struct ScBucket {
    ScJob *jobs;
    uint64_t disk_epoch;
} ScBucket;

typedef struct ScWorker {
    A840ScService *svc;
    uint32_t id;
    pthread_t thread;
} ScWorker;

struct A840ScService {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    A840ScServiceConfig cfg;
    char *disk_cache_dir;
    A840PipelineMemCache *mem_cache;
    ScQueue fg_queue;
    ScQueue bg_queue;
    ScQueue warmup_queue;
    ScBucket inflight[SC_INFLIGHT_BUCKETS];
    uint64_t inflight_count;
    A840ScServiceStats stats;
    ScWorker workers[A840_SC_MAX_WORKERS];
    uint32_t started_workers;
    bool stopping;
    bool stopped;
};

static bool key_equal(const A840PipelineKey *a, const A840PipelineKey *b)
{
    return memcmp(a->sha256, b->sha256, sizeof(a->sha256)) == 0;
}

static ScBucket *bucket_for(A840ScService *svc, const A840PipelineKey *key)
{
    size_t idx = ((size_t)key->sha256[0] ^ ((size_t)key->sha256[1] << 1)) &
                 (SC_INFLIGHT_BUCKETS - 1u);
    return &svc->inflight[idx];
}

static ScJob **find_job_slot(ScBucket *bucket, const A840PipelineKey *key)
{
    ScJob **slot = &bucket->jobs;
    while (*slot && !key_equal(&(*slot)->request.pipeline_key, key))
        slot = &(*slot)->hash_next;
    return slot;
}

static ScQueue *queue_for_priority(A840ScService *svc, A840ScPriority prio)
{
    if (prio == A840_SC_PRIO_FG_SYNC)
        return &svc->fg_queue;
    if (prio == A840_SC_PRIO_BG_ASYNC)
        return &svc->bg_queue;
    return &svc->warmup_queue;
}

static void queue_remove(ScQueue *q, ScJob *job)
{
    if (!job->in_queue)
        return;
    if (job->queue_prev)
        job->queue_prev->queue_next = job->queue_next;
    else
        q->head = job->queue_next;
    if (job->queue_next)
        job->queue_next->queue_prev = job->queue_prev;
    else
        q->tail = job->queue_prev;
    job->queue_prev = NULL;
    job->queue_next = NULL;
    job->in_queue = false;
}

static void queue_push_front(ScQueue *q, ScJob *job)
{
    job->queue_prev = NULL;
    job->queue_next = q->head;
    if (q->head)
        q->head->queue_prev = job;
    else
        q->tail = job;
    q->head = job;
    job->in_queue = true;
}

static void queue_push_back(ScQueue *q, ScJob *job)
{
    job->queue_next = NULL;
    job->queue_prev = q->tail;
    if (q->tail)
        q->tail->queue_next = job;
    else
        q->head = job;
    q->tail = job;
    job->in_queue = true;
}

static ScJob *dequeue_next_job(A840ScService *svc)
{
    ScQueue *queues[3] = {&svc->fg_queue, &svc->bg_queue, &svc->warmup_queue};
    for (size_t i = 0; i < 3u; ++i) {
        if (queues[i]->head) {
            ScJob *job = queues[i]->head;
            queue_remove(queues[i], job);
            return job;
        }
    }
    return NULL;
}

static void unlink_inflight(A840ScService *svc, ScJob *job)
{
    ScBucket *b = bucket_for(svc, &job->request.pipeline_key);
    ScJob **slot = find_job_slot(b, &job->request.pipeline_key);
    if (*slot == job) {
        *slot = job->hash_next;
        job->hash_next = NULL;
        --svc->inflight_count;
    }
}

static void notify_waiter(const ScWaiter *waiter, A840ScStatus status,
    const uint8_t *record_blob, size_t record_bytes)
{
    A840ScFrameHeader reply = waiter->request;
    reply.magic = A840_SC_PROTO_MAGIC;
    reply.version = A840_SC_PROTO_VERSION;
    reply.msg_type = A840_SC_MSG_COMPILE_REPLY;
    reply.status = (uint32_t)status;
    reply.payload_bytes = (status == A840_SC_OK) ? (uint32_t)record_bytes : 0u;
    reply.reserved0 = 0u;
    reply.reserved1 = 0u;
    memset(reply.payload_sha256, 0, sizeof(reply.payload_sha256));

    const uint8_t *body = (status == A840_SC_OK && record_bytes > 0u) ? record_blob : NULL;
    const size_t body_len = body ? record_bytes : 0u;

    if (waiter->completion) {
        if (body_len > 0u) {
            (void)a840_record_sha256(body, body_len, reply.payload_sha256);
        } else {
            (void)a840_record_sha256("", 0u, reply.payload_sha256);
        }
        waiter->completion(waiter->user_ctx, &reply, body, body_len);
        return;
    }
    if (!waiter->reply_frame)
        return;

    uint8_t stack_buf[A840_SC_HEADER_BYTES];
    size_t cap = (size_t)A840_SC_HEADER_BYTES + body_len;
    uint8_t *wire = body_len ? (uint8_t *)malloc(cap) : stack_buf;
    if (!wire) {
        wire = stack_buf;
        cap = sizeof(stack_buf);
        reply.status = A840_SC_OOM;
        reply.payload_bytes = 0u;
        body = NULL;
    }
    size_t wire_len = 0u;
    if (a840_sc_encode_frame(&reply, body, wire, cap, &wire_len) == A840_SC_OK)
        waiter->reply_frame(waiter->user_ctx, wire, wire_len);
    if (wire != stack_buf)
        free(wire);
}

static void notify_and_free_waiters(ScWaiter *list, A840ScStatus status,
    const uint8_t *record_blob, size_t record_bytes)
{
    while (list) {
        ScWaiter *next = list->next;
        notify_waiter(list, status, record_blob, record_bytes);
        free(list);
        list = next;
    }
}

static A840ScStatus map_record_status(A840RecordStatus st)
{
    if (st == A840_RECORD_OK)
        return A840_SC_OK;
    return st == A840_RECORD_OOM ? A840_SC_OOM : A840_SC_COMPILE_FAILED;
}

static A840ScStatus run_backend_compile(A840ScService *svc, const ScJob *job,
    uint8_t **out_wire, size_t *out_bytes, A840PipelineRecord *out_rec)
{
    *out_wire = NULL;
    *out_bytes = 0u;
    A840ScStatus st = svc->cfg.compile_fn(svc->cfg.user_ctx, job->worker_id,
        &job->request, job->request.payload_bytes ? job->payload : NULL,
        out_wire, out_bytes);
    if (st != A840_SC_OK) {
        free(*out_wire);
        *out_wire = NULL;
        *out_bytes = 0u;
        if (st == A840_SC_TIMED_OUT || st == A840_SC_CANCELED || st == A840_SC_OOM)
            return st;
        return A840_SC_COMPILE_FAILED;
    }
    if (!*out_wire || *out_bytes == 0u || *out_bytes > A840_SC_MAX_PAYLOAD_BYTES) {
        free(*out_wire);
        *out_wire = NULL;
        *out_bytes = 0u;
        return A840_SC_COMPILE_FAILED;
    }
    A840RecordStatus rst = a840_record_deserialize(*out_wire, *out_bytes,
        svc->cfg.stack_key, svc->cfg.abi_fingerprint,
        &job->request.pipeline_key, out_rec);
    if (rst != A840_RECORD_OK) {
        free(*out_wire);
        *out_wire = NULL;
        *out_bytes = 0u;
        return map_record_status(rst);
    }
    return A840_SC_OK;
}

static void *worker_thread_main(void *arg)
{
    ScWorker *worker = (ScWorker *)arg;
    A840ScService *svc = worker->svc;

    pthread_mutex_lock(&svc->mutex);
    for (;;) {
        if (svc->stopping)
            break;
        ScJob *job = dequeue_next_job(svc);
        if (!job) {
            pthread_cond_wait(&svc->cond, &svc->mutex);
            continue;
        }
        job->running = true;
        job->worker_id = worker->id;
        pthread_mutex_unlock(&svc->mutex);

        uint8_t *wire = NULL;
        size_t wire_bytes = 0u;
        A840PipelineRecord record;
        memset(&record, 0, sizeof(record));

        A840ScStatus st = run_backend_compile(svc, job, &wire, &wire_bytes, &record);
        if (st == A840_SC_OK && svc->disk_cache_dir)
            (void)a840_record_save_file(svc->disk_cache_dir, &record);

        pthread_mutex_lock(&svc->mutex);
        while (job->active_boosts > 0u)
            pthread_cond_wait(&svc->cond, &svc->mutex);

        if (st == A840_SC_OK) {
            (void)a840_pipeline_mem_cache_put(svc->mem_cache, &record);
            if (svc->disk_cache_dir)
                ++bucket_for(svc, &job->request.pipeline_key)->disk_epoch;
            ++svc->stats.compiled_ok;
        } else {
            ++svc->stats.compiled_fail;
            if (st == A840_SC_CANCELED) {
                for (ScWaiter *w = job->waiters; w; w = w->next)
                    ++svc->stats.canceled;
            }
        }
        ScWaiter *waiters = job->waiters;
        job->waiters = NULL;
        unlink_inflight(svc, job);
        pthread_mutex_unlock(&svc->mutex);

        /* All completion callbacks execute outside the service mutex. */
        notify_and_free_waiters(waiters, st, st == A840_SC_OK ? wire : NULL,
            st == A840_SC_OK ? wire_bytes : 0u);
        a840_record_free(&record);
        free(wire);
        free(job);

        pthread_mutex_lock(&svc->mutex);
    }
    pthread_mutex_unlock(&svc->mutex);
    return NULL;
}

A840ScStatus a840_sc_service_create(const A840ScServiceConfig *cfg,
    A840ScService **out_service)
{
    if (!cfg || !out_service || !cfg->compile_fn ||
        cfg->stack_key == 0u || cfg->abi_fingerprint == 0u ||
        cfg->worker_count < A840_SC_MIN_WORKERS ||
        cfg->worker_count > A840_SC_MAX_WORKERS ||
        (cfg->disk_cache_dir && cfg->disk_cache_dir[0] == '\0'))
        return A840_SC_INVALID_ARG;
    if (cfg->abi_fingerprint != a840_compile_abi_fingerprint())
        return A840_SC_ABI_MISMATCH;

    A840ScService *svc = (A840ScService *)calloc(1, sizeof(*svc));
    if (!svc)
        return A840_SC_OOM;
    svc->cfg = *cfg;

    if (cfg->disk_cache_dir) {
        size_t len = strlen(cfg->disk_cache_dir);
        svc->disk_cache_dir = (char *)malloc(len + 1u);
        if (!svc->disk_cache_dir) {
            free(svc);
            return A840_SC_OOM;
        }
        memcpy(svc->disk_cache_dir, cfg->disk_cache_dir, len + 1u);
    }

    A840RecordStatus cst = a840_pipeline_mem_cache_create(cfg->mem_cache_entries,
        cfg->mem_cache_bytes, cfg->stack_key, cfg->abi_fingerprint, &svc->mem_cache);
    if (cst != A840_RECORD_OK) {
        free(svc->disk_cache_dir);
        free(svc);
        return cst == A840_RECORD_OOM ? A840_SC_OOM : A840_SC_INVALID_ARG;
    }

    if (pthread_mutex_init(&svc->mutex, NULL) != 0) {
        a840_pipeline_mem_cache_destroy(svc->mem_cache);
        free(svc->disk_cache_dir);
        free(svc);
        return A840_SC_OOM;
    }
    if (pthread_cond_init(&svc->cond, NULL) != 0) {
        pthread_mutex_destroy(&svc->mutex);
        a840_pipeline_mem_cache_destroy(svc->mem_cache);
        free(svc->disk_cache_dir);
        free(svc);
        return A840_SC_OOM;
    }

    for (uint32_t i = 0; i < cfg->worker_count; ++i) {
        svc->workers[i].svc = svc;
        svc->workers[i].id = i;
        if (pthread_create(&svc->workers[i].thread, NULL, worker_thread_main,
                &svc->workers[i]) != 0) {
            a840_sc_service_destroy(svc);
            return A840_SC_OOM;
        }
        ++svc->started_workers;
    }

    *out_service = svc;
    return A840_SC_OK;
}

static A840ScStatus schedule_request(A840ScService *svc, const ScWaiter *tmpl,
    const void *payload)
{
    const A840ScFrameHeader *req = &tmpl->request;
    const A840PipelineKey *key = &req->pipeline_key;
    const A840ScPriority prio = (A840ScPriority)req->priority_class;
    ScBucket *bucket = bucket_for(svc, key);

    bool disk_checked = false;
    bool have_disk = false;
    uint64_t seen_epoch = 0u;
    A840PipelineRecord disk_rec;
    memset(&disk_rec, 0, sizeof(disk_rec));

    uint8_t *hit_wire = NULL;
    size_t hit_bytes = 0u;

    ScJob *boost_job = NULL;
    uint32_t boost_worker_id = 0u;
    A840ScPriority boost_from = A840_SC_PRIO_FG_SYNC;
    A840ScStatus status = A840_SC_OK;

    pthread_mutex_lock(&svc->mutex);
    for (;;) {
        if (svc->stopping) {
            status = A840_SC_SHUTDOWN;
            break;
        }

        /* 1. Memory LRU cache lookup */
        const A840PipelineRecord *mem_rec = a840_pipeline_mem_cache_get(svc->mem_cache, key);
        if (mem_rec || have_disk) {
            const A840PipelineRecord *src = mem_rec ? mem_rec : &disk_rec;
            status = map_record_status(a840_record_serialize(src, &hit_wire, &hit_bytes));
            if (status == A840_SC_OK) {
                if (mem_rec) {
                    ++svc->stats.cache_mem_hits;
                } else {
                    (void)a840_pipeline_mem_cache_put(svc->mem_cache, &disk_rec);
                    ++svc->stats.cache_disk_hits;
                }
            }
            break;
        }

        /* 2. Shared A840PR01 disk cache lookup (performed outside the lock) */
        ScJob **slot = find_job_slot(bucket, key);
        if (svc->disk_cache_dir &&
            (!disk_checked || (!*slot && seen_epoch != bucket->disk_epoch))) {
            disk_checked = true;
            seen_epoch = bucket->disk_epoch;
            pthread_mutex_unlock(&svc->mutex);
            have_disk = (a840_record_load_file(svc->disk_cache_dir, key,
                             svc->cfg.stack_key, svc->cfg.abi_fingerprint,
                             &disk_rec) == A840_RECORD_OK);
            pthread_mutex_lock(&svc->mutex);
            continue;
        }

        /* 3. In-flight deduplication table */
        ScJob *job = *slot;
        if (job) {
            for (ScWaiter *w = job->waiters; w; w = w->next) {
                if (w->request.client_pid == req->client_pid &&
                    w->request.request_id == req->request_id) {
                    status = A840_SC_DUPLICATE_REQUEST;
                    break;
                }
            }
            if (status != A840_SC_OK)
                break;
        }

        ScWaiter *waiter = (ScWaiter *)malloc(sizeof(*waiter));
        if (!waiter) {
            status = A840_SC_OOM;
            break;
        }
        *waiter = *tmpl;
        waiter->next = NULL;

        if (!job) {
            /* 4. Enqueue new compile job */
            job = (ScJob *)calloc(1, sizeof(*job) + (size_t)req->payload_bytes);
            if (!job) {
                free(waiter);
                status = A840_SC_OOM;
                break;
            }
            job->request = *req;
            job->priority = prio;
            job->waiters = waiter;
            if (req->payload_bytes != 0u)
                memcpy(job->payload, payload, req->payload_bytes);

            *slot = job;
            ++svc->inflight_count;
            if (svc->inflight_count > svc->stats.max_inflight_seen)
                svc->stats.max_inflight_seen = svc->inflight_count;

            queue_push_back(queue_for_priority(svc, prio), job);
            pthread_cond_broadcast(&svc->cond);
            break;
        }

        /* Attach to existing in-flight slot */
        waiter->next = job->waiters;
        job->waiters = waiter;
        ++svc->stats.dedup_hits;

        /* Atomic priority inheritance / boost when higher priority arrives */
        if (prio < job->priority) {
            boost_from = job->priority;
            job->priority = prio;
            ++svc->stats.priority_boosts;
            if (!job->running && job->in_queue) {
                queue_remove(queue_for_priority(svc, boost_from), job);
                if (prio == A840_SC_PRIO_FG_SYNC)
                    queue_push_front(&svc->fg_queue, job);
                else
                    queue_push_front(queue_for_priority(svc, prio), job);
                pthread_cond_broadcast(&svc->cond);
            } else if (job->running && svc->cfg.on_priority_boost) {
                ++job->active_boosts;
                boost_job = job;
                boost_worker_id = job->worker_id;
            }
        }
        break;
    }

    if (status == A840_SC_OK) {
        if (prio == A840_SC_PRIO_FG_SYNC)
            ++svc->stats.fg_requests;
        else
            ++svc->stats.bg_requests;
    }
    pthread_mutex_unlock(&svc->mutex);

    if (boost_job) {
        svc->cfg.on_priority_boost(svc->cfg.user_ctx, boost_worker_id, boost_from, prio);
        pthread_mutex_lock(&svc->mutex);
        --boost_job->active_boosts;
        if (boost_job->active_boosts == 0u)
            pthread_cond_broadcast(&svc->cond);
        pthread_mutex_unlock(&svc->mutex);
    }

    if (hit_wire) {
        notify_waiter(tmpl, A840_SC_OK, hit_wire, hit_bytes);
        free(hit_wire);
    }
    a840_record_free(&disk_rec);
    return status;
}

A840ScStatus a840_sc_service_submit(A840ScService *svc,
    const A840ScFrameHeader *request, const void *payload,
    A840ScServiceCompletionFn completion, void *user_ctx)
{
    if (!svc || !request || !completion ||
        (request->payload_bytes != 0u && !payload))
        return A840_SC_INVALID_ARG;

    A840ScFrameHeader normalized = *request;
    if (normalized.magic == 0u)
        normalized.magic = A840_SC_PROTO_MAGIC;
    if (normalized.version == 0u)
        normalized.version = A840_SC_PROTO_VERSION;
    if (normalized.msg_type != A840_SC_MSG_COMPILE_REQUEST &&
        normalized.msg_type != A840_SC_MSG_PREWARM_HINT)
        return A840_SC_INVALID_ARG;

    A840ScStatus st = a840_sc_validate_header(&normalized, svc->cfg.stack_key,
        svc->cfg.abi_fingerprint);
    if (st != A840_SC_OK)
        return st;

    ScWaiter tmpl;
    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.request = normalized;
    tmpl.completion = completion;
    tmpl.user_ctx = user_ctx;
    return schedule_request(svc, &tmpl, payload);
}

A840ScStatus a840_sc_service_cancel(A840ScService *svc,
    const A840PipelineKey *pipeline_key, uint32_t client_pid,
    uint64_t request_id)
{
    if (!svc || !pipeline_key || a840_sc_pipeline_key_is_zero(pipeline_key) ||
        client_pid == 0u || request_id == 0u)
        return A840_SC_INVALID_ARG;

    pthread_mutex_lock(&svc->mutex);
    ScBucket *bucket = bucket_for(svc, pipeline_key);
    ScJob *job = *find_job_slot(bucket, pipeline_key);
    if (!job) {
        pthread_mutex_unlock(&svc->mutex);
        return A840_SC_NOT_FOUND;
    }

    ScWaiter **wslot = &job->waiters;
    while (*wslot &&
           ((*wslot)->request.client_pid != client_pid ||
            (*wslot)->request.request_id != request_id)) {
        wslot = &(*wslot)->next;
    }
    ScWaiter *waiter = *wslot;
    if (!waiter) {
        pthread_mutex_unlock(&svc->mutex);
        return A840_SC_NOT_FOUND;
    }

    *wslot = waiter->next;
    waiter->next = NULL;
    ++svc->stats.canceled;

    ScJob *drop_job = NULL;
    if (!job->waiters && !job->running) {
        queue_remove(queue_for_priority(svc, job->priority), job);
        unlink_inflight(svc, job);
        drop_job = job;
    }
    pthread_mutex_unlock(&svc->mutex);

    notify_waiter(waiter, A840_SC_CANCELED, NULL, 0u);
    free(waiter);
    free(drop_job);
    return A840_SC_OK;
}

A840ScStatus a840_sc_service_handle_frame(A840ScService *svc,
    const uint8_t *frame, size_t frame_bytes,
    A840ScReplyFrameFn reply_fn, void *user_ctx)
{
    if (!svc)
        return A840_SC_INVALID_ARG;

    ScWaiter tmpl;
    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.reply_frame = reply_fn;
    tmpl.user_ctx = user_ctx;

    const uint8_t *payload = NULL;
    A840ScStatus st = a840_sc_decode_frame(frame, frame_bytes,
        svc->cfg.stack_key, svc->cfg.abi_fingerprint, &tmpl.request, &payload);
    if (st != A840_SC_OK)
        return st;

    if (tmpl.request.msg_type == A840_SC_MSG_CANCEL_REQUEST) {
        return a840_sc_service_cancel(svc, &tmpl.request.pipeline_key,
            tmpl.request.client_pid, tmpl.request.request_id);
    }

    if (!reply_fn)
        return A840_SC_INVALID_ARG;

    if (tmpl.request.msg_type == A840_SC_MSG_PING_STATUS) {
        pthread_mutex_lock(&svc->mutex);
        st = svc->stopping ? A840_SC_SHUTDOWN : A840_SC_OK;
        pthread_mutex_unlock(&svc->mutex);
        notify_waiter(&tmpl, st, NULL, 0u);
        return st;
    }

    if (tmpl.request.msg_type == A840_SC_MSG_COMPILE_REQUEST ||
        tmpl.request.msg_type == A840_SC_MSG_PREWARM_HINT) {
        st = schedule_request(svc, &tmpl, payload);
        if (st != A840_SC_OK)
            notify_waiter(&tmpl, st, NULL, 0u);
        return st;
    }

    return A840_SC_PROTO_ERROR;
}

void a840_sc_service_get_stats(A840ScService *svc, A840ScServiceStats *out_stats)
{
    if (!svc || !out_stats)
        return;
    pthread_mutex_lock(&svc->mutex);
    *out_stats = svc->stats;
    pthread_mutex_unlock(&svc->mutex);
}

void a840_sc_service_shutdown(A840ScService *svc)
{
    if (!svc)
        return;

    pthread_mutex_lock(&svc->mutex);
    if (svc->stopping) {
        while (!svc->stopped)
            pthread_cond_wait(&svc->cond, &svc->mutex);
        pthread_mutex_unlock(&svc->mutex);
        return;
    }
    svc->stopping = true;

    ScJob *canceled_jobs = NULL;
    ScQueue *queues[3] = {&svc->fg_queue, &svc->bg_queue, &svc->warmup_queue};
    for (size_t i = 0; i < 3u; ++i) {
        while (queues[i]->head) {
            ScJob *job = queues[i]->head;
            queue_remove(queues[i], job);
            unlink_inflight(svc, job);
            for (ScWaiter *w = job->waiters; w; w = w->next)
                ++svc->stats.canceled;
            job->queue_next = canceled_jobs;
            canceled_jobs = job;
        }
    }
    pthread_cond_broadcast(&svc->cond);
    pthread_mutex_unlock(&svc->mutex);

    while (canceled_jobs) {
        ScJob *next = canceled_jobs->queue_next;
        notify_and_free_waiters(canceled_jobs->waiters, A840_SC_CANCELED, NULL, 0u);
        free(canceled_jobs);
        canceled_jobs = next;
    }

    for (uint32_t i = 0; i < svc->started_workers; ++i)
        pthread_join(svc->workers[i].thread, NULL);

    pthread_mutex_lock(&svc->mutex);
    svc->stopped = true;
    pthread_cond_broadcast(&svc->cond);
    pthread_mutex_unlock(&svc->mutex);
}

void a840_sc_service_destroy(A840ScService *svc)
{
    if (!svc)
        return;
    a840_sc_service_shutdown(svc);
    pthread_cond_destroy(&svc->cond);
    pthread_mutex_destroy(&svc->mutex);
    a840_pipeline_mem_cache_destroy(svc->mem_cache);
    free(svc->disk_cache_dir);
    free(svc);
}
