/* SPDX-License-Identifier: MIT */
/* NezhaShaderCompiler background compilation service engine:
 * dual-priority scheduling (FG_SYNC > BG_ASYNC > WARMUP_LOW),
 * in-flight pipeline_key deduplication, atomic priority inheritance (boost),
 * multi-tier cache (memory LRU + shared A840PR01 disk store), fault isolation,
 * and deterministic shutdown.
 */
#ifndef A840_SHADER_COMPILER_SERVICE_H
#define A840_SHADER_COMPILER_SERVICE_H

#include "a840_shader_compiler_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_SC_MIN_WORKERS = 1u,
    A840_SC_MAX_WORKERS = 8u
};

typedef struct A840ScService A840ScService;

/* Backend compiler callback invoked on a worker thread outside the service lock.
 * On A840_SC_OK, *out_record_blob must be a malloc-owned valid A840PR01 blob
 * matching the service's stack_key, abi_fingerprint, and request pipeline_key.
 * Ownership transfers to the service (which also frees any non-NULL buffer on
 * error).
 */
typedef A840ScStatus (*A840ScCompileFn)(void *user_ctx, uint32_t worker_id,
    const A840ScFrameHeader *request, const uint8_t *payload,
    uint8_t **out_record_blob, size_t *out_record_bytes);

/* Optional hook invoked outside the service lock when an in-flight worker job
 * receives a priority boost (e.g., BG_ASYNC/WARMUP_LOW -> FG_SYNC). On XNU/iOS
 * this binds to turnstile / pthread QoS elevation. The worker is prevented from
 * transitioning to a new job until this hook returns.
 */
typedef void (*A840ScPriorityBoostFn)(void *user_ctx, uint32_t worker_id,
    A840ScPriority from_prio, A840ScPriority to_prio);

/* Request completion callback invoked outside the service lock.
 * record_blob is borrowed for the duration of the callback (NULL on error or
 * empty PING reply).
 */
typedef void (*A840ScServiceCompletionFn)(void *user_ctx,
    const A840ScFrameHeader *reply_hdr, const uint8_t *record_blob,
    size_t record_bytes);

/* Encoded wire-frame reply callback for transport-driven receive path. */
typedef void (*A840ScReplyFrameFn)(void *user_ctx,
    const uint8_t *frame, size_t frame_bytes);

typedef struct A840ScServiceConfig {
    uint64_t stack_key;
    uint64_t abi_fingerprint;
    uint32_t worker_count;                    /* 1..8 */
    size_t mem_cache_entries;                 /* LRU entry capacity (>= 1) */
    size_t mem_cache_bytes;                   /* LRU byte capacity (>= 1) */
    const char *disk_cache_dir;               /* Optional existing directory, or NULL */
    A840ScCompileFn compile_fn;               /* Required */
    A840ScPriorityBoostFn on_priority_boost;  /* Optional */
    void *user_ctx;
} A840ScServiceConfig;

typedef struct A840ScServiceStats {
    uint64_t fg_requests;
    uint64_t bg_requests;
    uint64_t cache_mem_hits;
    uint64_t cache_disk_hits;
    uint64_t dedup_hits;
    uint64_t priority_boosts;
    uint64_t compiled_ok;
    uint64_t compiled_fail;
    uint64_t canceled;
    uint64_t max_inflight_seen;
} A840ScServiceStats;

A840ScStatus a840_sc_service_create(const A840ScServiceConfig *cfg,
    A840ScService **out_service);

/* Submit a COMPILE_REQUEST or PREWARM_HINT directly in-process.
 * On A840_SC_OK, completion is guaranteed to be called exactly once outside the
 * service lock (either before return on cache hit, on a worker thread, or on a
 * cancel/shutdown caller). On immediate error return, completion is NOT called.
 */
A840ScStatus a840_sc_service_submit(A840ScService *svc,
    const A840ScFrameHeader *request, const void *payload,
    A840ScServiceCompletionFn completion, void *user_ctx);

/* Cancel a single pending waiter identified by (pipeline_key, client_pid,
 * request_id). If a queued job loses its last waiter, it is removed from the
 * queue immediately. A running job drains to completion and may still populate
 * the cache; its remaining waiters receive the result normally.
 */
A840ScStatus a840_sc_service_cancel(A840ScService *svc,
    const A840PipelineKey *pipeline_key, uint32_t client_pid,
    uint64_t request_id);

/* Transport entry point: decode one wire frame and dispatch it.
 * Valid PING_STATUS, COMPILE_REQUEST, and PREWARM_HINT frames receive an
 * encoded COMPILE_REPLY frame via reply_fn (even on immediate rejection such as
 * SHUTDOWN or DUPLICATE_REQUEST). CANCEL_REQUEST invokes cancel with no reply
 * frame. Malformed frames return an error status without invoking reply_fn.
 */
A840ScStatus a840_sc_service_handle_frame(A840ScService *svc,
    const uint8_t *frame, size_t frame_bytes,
    A840ScReplyFrameFn reply_fn, void *user_ctx);

void a840_sc_service_get_stats(A840ScService *svc, A840ScServiceStats *out_stats);

/* Deterministic graceful shutdown:
 * Rejects new submissions, cancels all queued jobs (notifying their waiters
 * outside the lock), drains running worker jobs, and joins all worker threads.
 * Safe to call multiple times or concurrently from external threads.
 */
void a840_sc_service_shutdown(A840ScService *svc);

/* Shuts down (if not already) and releases all resources. */
void a840_sc_service_destroy(A840ScService *svc);

#ifdef __cplusplus
}
#endif
#endif /* A840_SHADER_COMPILER_SERVICE_H */
