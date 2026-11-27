/* SPDX-License-Identifier: MIT */
/* NezhaShaderCompiler client stub:
 * transport-agnostic sync (FG_SYNC), async (BG_ASYNC), prewarm (WARMUP_LOW),
 * cancellation, health ping, and circuit-breaker exponential backoff returning
 * A840_SC_DAEMON_UNAVAILABLE ("raw:compiler.daemon_unavailable") when the
 * background daemon is unreachable.
 */
#ifndef A840_SHADER_COMPILER_CLIENT_H
#define A840_SHADER_COMPILER_CLIENT_H

#include "a840_shader_compiler_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct A840ScClient A840ScClient;

/* Transport send callback invoked exclusively on the client's I/O thread.
 * Must honor timeout_ms. Returns:
 *   - A840_SC_OK: one complete frame transmitted
 *   - A840_SC_TIMED_OUT: transient timeout (no partial frame left on wire)
 *   - A840_SC_DAEMON_UNAVAILABLE (or any other error): peer unreachable
 */
typedef A840ScStatus (*A840ScSendFn)(void *user_ctx, const uint8_t *frame,
    size_t frame_bytes, uint32_t timeout_ms);

/* Transport receive callback invoked exclusively on the client's I/O thread.
 * Must honor timeout_ms (0 = nonblocking poll). On A840_SC_OK, transfers a
 * malloc-owned complete wire frame to (*out_frame, *out_frame_bytes).
 * Returns A840_SC_TIMED_OUT if no frame is available within timeout_ms, or
 * A840_SC_DAEMON_UNAVAILABLE (or any other error) if peer is unreachable.
 */
typedef A840ScStatus (*A840ScRecvFn)(void *user_ctx, uint8_t **out_frame,
    size_t *out_frame_bytes, uint32_t timeout_ms);

/* Completion callback for async requests, always invoked on the client's
 * dedicated completion thread (never on the caller or I/O thread).
 * record_blob is borrowed for the callback duration (NULL on non-OK status).
 */
typedef void (*A840ScClientCompletionFn)(void *user_ctx, uint64_t request_id,
    A840ScStatus status, const uint8_t *record_blob, size_t record_bytes);

typedef struct A840ScClientConfig {
    uint64_t stack_key;
    uint64_t abi_fingerprint;
    uint32_t client_pid;
    uint32_t initial_backoff_ms; /* 0 -> default 50 ms */
    uint32_t max_backoff_ms;     /* 0 -> default 1000 ms */
    A840ScSendFn send_fn;
    A840ScRecvFn recv_fn;
    void *user_ctx;
} A840ScClientConfig;

A840ScStatus a840_sc_client_create(const A840ScClientConfig *cfg,
    A840ScClient **out_client);

/* Foreground synchronous compile (FG_SYNC). Blocks until reply, timeout, or
 * daemon unavailability. timeout_ms == 0 uses 5000 ms default.
 * On A840_SC_OK, *out_record_blob is a malloc-owned validated A840PR01 blob
 * that the caller must free(). On failure, output pointers are untouched.
 */
A840ScStatus a840_sc_client_compile_sync(A840ScClient *client,
    const A840PipelineKey *pipeline_key, const void *payload,
    size_t payload_bytes, uint32_t timeout_ms,
    uint8_t **out_record_blob, size_t *out_record_bytes);

/* Background asynchronous compile (BG_ASYNC). Copies input payload and returns
 * immediately without performing transport I/O. On A840_SC_OK, writes the
 * assigned nonzero request_id and guarantees completion runs exactly once on
 * the completion thread.
 */
A840ScStatus a840_sc_client_compile_async(A840ScClient *client,
    const A840PipelineKey *pipeline_key, const void *payload,
    size_t payload_bytes, uint32_t timeout_ms,
    A840ScClientCompletionFn completion, void *user_ctx,
    uint64_t *out_request_id);

/* Low-priority prewarm hint (WARMUP_LOW, async). */
A840ScStatus a840_sc_client_prewarm_async(A840ScClient *client,
    const A840PipelineKey *pipeline_key, const void *payload,
    size_t payload_bytes, uint32_t timeout_ms,
    A840ScClientCompletionFn completion, void *user_ctx,
    uint64_t *out_request_id);

/* Cancel an in-flight request by request_id. */
A840ScStatus a840_sc_client_cancel(A840ScClient *client, uint64_t request_id);

/* Health probe (PING_STATUS, FG_SYNC). Returns A840_SC_OK if daemon responds,
 * or A840_SC_DAEMON_UNAVAILABLE / A840_SC_TIMED_OUT / A840_SC_SHUTDOWN.
 */
A840ScStatus a840_sc_client_ping(A840ScClient *client, uint32_t timeout_ms);

/* Returns true if the circuit breaker backoff window is currently open. */
bool a840_sc_client_is_circuit_open(A840ScClient *client);

/* Stop I/O and callback threads, fail pending calls with A840_SC_SHUTDOWN,
 * drain queued completions, and release all resources.
 */
void a840_sc_client_destroy(A840ScClient *client);

#ifdef __cplusplus
}
#endif
#endif /* A840_SHADER_COMPILER_CLIENT_H */
