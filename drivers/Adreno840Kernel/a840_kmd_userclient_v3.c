/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "a840_kmd_userclient_v3.h"
#include <string.h>

#define A840_KMD_UC_MAGIC 1430471475u

#if !defined(A840_KMD_CUSTOM_SYNC)
static void uc_lock(A840KmdUserClientV3 *uc)
{
    (void)pthread_mutex_lock(&uc->lock);
}

static void uc_unlock(A840KmdUserClientV3 *uc)
{
    (void)pthread_mutex_unlock(&uc->lock);
}
#else
static void uc_lock(A840KmdUserClientV3 *uc) { (void)uc; }
static void uc_unlock(A840KmdUserClientV3 *uc) { (void)uc; }
#endif

static A840KmdStatus map_abi_status(A840AbiV3Status st)
{
    switch (st) {
    case A840_ABI_V3_OK: return A840_KMD_OK;
    case A840_ABI_V3_INVALID_ARG: return A840_KMD_INVALID_ARG;
    case A840_ABI_V3_RANGE: return A840_KMD_RANGE;
    case A840_ABI_V3_ALIGNMENT: return A840_KMD_ALIGNMENT;
    case A840_ABI_V3_CAPACITY: return A840_KMD_CAPACITY;
    case A840_ABI_V3_NOT_FOUND: return A840_KMD_NOT_FOUND;
    case A840_ABI_V3_STATE_CONFLICT: return A840_KMD_STATE_CONFLICT;
    case A840_ABI_V3_STACK_KEY_MISMATCH: return A840_KMD_STACK_KEY_MISMATCH;
    case A840_ABI_V3_UNSUPPORTED: return A840_KMD_UNSUPPORTED;
    default: return A840_KMD_INVALID_ARG;
    }
}

static A840KmdStatus check_init(const A840KmdUserClientV3 *uc)
{
    if (uc == NULL)
        return A840_KMD_INVALID_ARG;
    if (uc->initialized != A840_KMD_UC_MAGIC)
        return A840_KMD_STATE_CONFLICT;
    return A840_KMD_OK;
}

static uint64_t make_scoped_queue(uint32_t scope_id, uint64_t local_queue)
{
    return (((uint64_t)scope_id) << 32u) | (local_queue & UINT64_C(4294967295));
}

static A840KmdClientSession *find_client_locked(
    A840KmdUserClientV3 *uc, uint64_t client_id)
{
    size_t i;
    if (client_id == 0u)
        return NULL;
    for (i = 0u; i < A840_KMD_MAX_CLIENTS; ++i) {
        if (uc->clients[i].state != A840_KMD_CLIENT_FREE &&
            uc->clients[i].client_id == client_id)
            return &uc->clients[i];
    }
    return NULL;
}

static int scope_in_use_locked(const A840KmdUserClientV3 *uc, uint32_t scope_id)
{
    size_t i;
    for (i = 0u; i < A840_KMD_MAX_CLIENTS; ++i) {
        if (uc->clients[i].state != A840_KMD_CLIENT_FREE &&
            uc->clients[i].scope_id == scope_id)
            return 1;
    }
    return 0;
}

static uint32_t alloc_scope_id_locked(A840KmdUserClientV3 *uc)
{
    uint32_t tries;
    for (tries = 0u; tries < 256u; ++tries) {
        uc->next_scope_id++;
        if (uc->next_scope_id == 0u)
            uc->next_scope_id = 1u;
        if (!scope_in_use_locked(uc, uc->next_scope_id))
            return uc->next_scope_id;
    }
    return 0u;
}

/* Fired by FaultCapture OUTSIDE fc->lock. Preserves strict lock order:
 * uc->lock -> wait_any->lock.
 */
static void on_channel_poisoned(
    void *cookie,
    uint64_t client_id,
    uint64_t queue_id,
    const A840KmdFaultSnapshot *snapshot)
{
    A840KmdUserClientV3 *uc = (A840KmdUserClientV3 *)cookie;
    A840KmdClientSession *session;
    (void)snapshot;

    if (uc == NULL || queue_id > UINT64_C(4294967295))
        return;

    uc_lock(uc);
    if (uc->initialized == A840_KMD_UC_MAGIC) {
        session = find_client_locked(uc, client_id);
        if (session != NULL && session->state == A840_KMD_CLIENT_OPEN) {
            uint64_t scoped_q = make_scoped_queue(session->scope_id, queue_id);
            (void)a840_kmd_wait_any_poison_queue(&uc->wait_any, scoped_q);
        }
    }
    uc_unlock(uc);
}

A840KmdStatus a840_kmd_userclient_v3_init(
    A840KmdUserClientV3 *uc,
    const A840KmdUserClientConfig *config)
{
    A840KmdStatus status;
    A840AbiV3Status abi_st;
    uint32_t caps;

    if (uc == NULL || config == NULL)
        return A840_KMD_INVALID_ARG;

    caps = (config->cap_flags != 0u) ? config->cap_flags : A840_KMD_V3_DEFAULT_CAPS;
    memset(uc, 0, sizeof(*uc));

    abi_st = a840_abi_v3_init(&uc->abi_state, caps);
    if (abi_st != A840_ABI_V3_OK)
        return map_abi_status(abi_st);

    abi_st = a840_abi_v3_init(&uc->wait_abi_state, caps);
    if (abi_st != A840_ABI_V3_OK)
        return map_abi_status(abi_st);

    if ((uc->abi_state.cap_flags & NativeAdrenoParamsV3SharedZeroPage) != 0u) {
        status = a840_kmd_zero_page_init(&uc->zero_page, &config->smmu_ops,
            config->zero_page_default_va);
        if (status != A840_KMD_OK)
            return status;
        uc->zero_page_enabled = 1u;
    }

    status = a840_kmd_wait_any_init(&uc->wait_any, config->wait_backend);
    if (status != A840_KMD_OK) {
        if (uc->zero_page_enabled)
            a840_kmd_zero_page_destroy(&uc->zero_page);
        return status;
    }

    status = a840_kmd_fault_init(&uc->fault_capture);
    if (status != A840_KMD_OK) {
        a840_kmd_wait_any_destroy(&uc->wait_any);
        if (uc->zero_page_enabled)
            a840_kmd_zero_page_destroy(&uc->zero_page);
        return status;
    }

#if !defined(A840_KMD_CUSTOM_SYNC)
    if (pthread_mutex_init(&uc->lock, NULL) != 0) {
        a840_kmd_fault_destroy(&uc->fault_capture);
        a840_kmd_wait_any_destroy(&uc->wait_any);
        if (uc->zero_page_enabled)
            a840_kmd_zero_page_destroy(&uc->zero_page);
        return A840_KMD_NO_MEMORY;
    }
    if (pthread_cond_init(&uc->drain_cond, NULL) != 0) {
        (void)pthread_mutex_destroy(&uc->lock);
        a840_kmd_fault_destroy(&uc->fault_capture);
        a840_kmd_wait_any_destroy(&uc->wait_any);
        if (uc->zero_page_enabled)
            a840_kmd_zero_page_destroy(&uc->zero_page);
        return A840_KMD_NO_MEMORY;
    }
#endif

    uc->bo_size_lookup = config->bo_size_lookup;
    uc->bo_size_cookie = config->bo_size_cookie;

    if (config->promise_observer != NULL &&
        (uc->abi_state.cap_flags & NativeAdrenoParamsV3WriterPromise) != 0u) {
        (void)a840_abi_v3_set_promise_observer(&uc->abi_state,
            config->promise_observer, config->promise_observer_cookie);
    }

    (void)a840_kmd_fault_set_poison_callback(&uc->fault_capture,
        on_channel_poisoned, uc);

    uc->initialized = A840_KMD_UC_MAGIC;
    return A840_KMD_OK;
}

void a840_kmd_userclient_v3_destroy(A840KmdUserClientV3 *uc)
{
    uint64_t ids[A840_KMD_MAX_CLIENTS];
    size_t count = 0u;
    size_t i;

    if (check_init(uc) != A840_KMD_OK)
        return;

    /* Detach poison callback first so no new callbacks enter uc during teardown. */
    (void)a840_kmd_fault_set_poison_callback(&uc->fault_capture, NULL, NULL);

    uc_lock(uc);
    for (i = 0u; i < A840_KMD_MAX_CLIENTS; ++i) {
        if (uc->clients[i].state == A840_KMD_CLIENT_OPEN)
            ids[count++] = uc->clients[i].client_id;
    }
    uc_unlock(uc);

    for (i = 0u; i < count; ++i)
        (void)a840_kmd_userclient_v3_close_client(uc, ids[i], NULL);

    uc_lock(uc);
    uc->initialized = 0u;
    uc_unlock(uc);

    a840_kmd_fault_destroy(&uc->fault_capture);
    a840_kmd_wait_any_destroy(&uc->wait_any);
    if (uc->zero_page_enabled)
        a840_kmd_zero_page_destroy(&uc->zero_page);

#if !defined(A840_KMD_CUSTOM_SYNC)
    (void)pthread_cond_destroy(&uc->drain_cond);
    (void)pthread_mutex_destroy(&uc->lock);
#endif
}

uint32_t a840_kmd_userclient_v3_advertised_caps(const A840KmdUserClientV3 *uc)
{
    if (check_init(uc) != A840_KMD_OK)
        return 0u;
    return uc->abi_state.cap_flags;
}

A840KmdStatus a840_kmd_userclient_v3_configure_shared_pack(
    A840KmdUserClientV3 *uc,
    const struct NativeAdrenoAbiSharedPackReplyV3 *pack,
    uint32_t gpu_readonly,
    uint32_t cpu_readonly)
{
    A840KmdStatus status = check_init(uc);
    A840AbiV3Status abi_st;

    if (status != A840_KMD_OK)
        return status;

    uc_lock(uc);
    abi_st = a840_abi_v3_set_shared_pack(&uc->abi_state, pack,
        gpu_readonly, cpu_readonly);
    uc_unlock(uc);
    return map_abi_status(abi_st);
}

A840KmdStatus a840_kmd_userclient_v3_set_promise_observer(
    A840KmdUserClientV3 *uc,
    A840AbiV3PromiseObserver observer,
    void *cookie)
{
    A840KmdStatus status = check_init(uc);
    A840AbiV3Status abi_st;

    if (status != A840_KMD_OK)
        return status;

    uc_lock(uc);
    abi_st = a840_abi_v3_set_promise_observer(&uc->abi_state, observer, cookie);
    uc_unlock(uc);
    return map_abi_status(abi_st);
}

A840KmdStatus a840_kmd_userclient_v3_open_client(
    A840KmdUserClientV3 *uc,
    uint64_t client_id,
    void *smmu_ctx,
    uint64_t requested_zero_page_va)
{
    A840KmdStatus status = check_init(uc);
    A840KmdClientSession *free_slot = NULL;
    struct NativeAdrenoAbiZeroPageReplyV3 zp_reply;
    uint32_t scope_id;
    size_t i;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u)
        return A840_KMD_INVALID_ARG;

    uc_lock(uc);

    for (i = 0u; i < A840_KMD_MAX_CLIENTS; ++i) {
        if (uc->clients[i].state != A840_KMD_CLIENT_FREE) {
            if (uc->clients[i].client_id == client_id) {
                uc_unlock(uc);
                return A840_KMD_STATE_CONFLICT;
            }
        } else if (free_slot == NULL) {
            free_slot = &uc->clients[i];
        }
    }

    if (free_slot == NULL) {
        uc_unlock(uc);
        return A840_KMD_CAPACITY;
    }

    scope_id = alloc_scope_id_locked(uc);
    if (scope_id == 0u) {
        uc_unlock(uc);
        return A840_KMD_CAPACITY;
    }

    status = a840_kmd_wait_any_register_scope(&uc->wait_any, scope_id);
    if (status != A840_KMD_OK) {
        uc_unlock(uc);
        return status;
    }

    memset(&zp_reply, 0, sizeof(zp_reply));
    if (uc->zero_page_enabled) {
        status = a840_kmd_zero_page_bind_client(&uc->zero_page, client_id,
            smmu_ctx, requested_zero_page_va, A840_KMD_SMMU_PERM_READ_ONLY,
            &zp_reply);
        if (status != A840_KMD_OK) {
            (void)a840_kmd_wait_any_reclaim_scope(&uc->wait_any, scope_id);
            uc_unlock(uc);
            return status;
        }
        /* Cross-validate mapping against frozen ABI v3 host validator. */
        if (a840_abi_v3_set_zero_page(&uc->abi_state, &zp_reply, 1u) != A840_ABI_V3_OK) {
            (void)a840_kmd_zero_page_unbind_client(&uc->zero_page, client_id);
            (void)a840_kmd_wait_any_reclaim_scope(&uc->wait_any, scope_id);
            uc_unlock(uc);
            return A840_KMD_STATE_CONFLICT;
        }
    }

    /* Fresh client incarnation starts free of any prior channel poison marks. */
    (void)a840_kmd_fault_clear_client(&uc->fault_capture, client_id);

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->state = A840_KMD_CLIENT_OPEN;
    free_slot->scope_id = scope_id;
    free_slot->client_id = client_id;
    free_slot->smmu_ctx = smmu_ctx;
    free_slot->zero_page_bound = uc->zero_page_enabled;
    free_slot->zero_page = zp_reply;
    uc->open_client_count++;

    uc_unlock(uc);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_userclient_v3_close_client(
    A840KmdUserClientV3 *uc,
    uint64_t client_id,
    A840KmdClientCloseSummary *out_summary)
{
    A840KmdStatus status = check_init(uc);
    A840KmdStatus zp_status = A840_KMD_OK;
    A840KmdClientSession *session;
    A840KmdClientCloseSummary summary;
    uint32_t scope_id;
    uint32_t zp_bound;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u)
        return A840_KMD_INVALID_ARG;

    memset(&summary, 0, sizeof(summary));

    uc_lock(uc);
    session = find_client_locked(uc, client_id);
    if (session == NULL) {
        uc_unlock(uc);
        return A840_KMD_NOT_FOUND;
    }
    if (session->state == A840_KMD_CLIENT_CLOSING) {
        uc_unlock(uc);
        return A840_KMD_STATE_CONFLICT;
    }

    session->state = A840_KMD_CLIENT_CLOSING;
    scope_id = session->scope_id;
    zp_bound = session->zero_page_bound;

    /* Immediately wake all blocked Method 20 waiters on this client. */
    (void)a840_kmd_wait_any_disconnect_scope(&uc->wait_any, scope_id);

    /* Wait for all in-flight WaitAny calls to unwind cleanly (zero orphans). */
#if !defined(A840_KMD_CUSTOM_SYNC)
    while (session->active_waiters > 0u)
        (void)pthread_cond_wait(&uc->drain_cond, &uc->lock);
#endif

    /* One-stop deterministic cleanup of Promises, SurfaceMeta, ZeroPage,
     * active Fault poison marks, and WaitAny timelines.
     */
    summary.canceled_promises =
        a840_abi_v3_cancel_all_for_client(&uc->abi_state, client_id);

    if (zp_bound) {
        zp_status = a840_kmd_zero_page_unbind_client(&uc->zero_page, client_id);
        if (zp_status == A840_KMD_OK)
            summary.zero_page_unbound = 1u;
    }

    summary.cleared_poisoned_channels =
        a840_kmd_fault_clear_client(&uc->fault_capture, client_id);

    (void)a840_kmd_wait_any_reclaim_scope(&uc->wait_any, scope_id);

    memset(session, 0, sizeof(*session));
    if (uc->open_client_count > 0u)
        uc->open_client_count--;

    uc_unlock(uc);

    if (out_summary != NULL)
        *out_summary = summary;
    return zp_status;
}

static A840KmdStatus dispatch_wait_any(
    A840KmdUserClientV3 *uc,
    uint64_t client_id,
    const void *input,
    void *output,
    size_t *out_written_bytes)
{
    struct NativeAdrenoAbiWaitAnyV3 req;
    struct NativeAdrenoAbiWaitAnyV3 scoped_req;
    struct NativeAdrenoAbiWaitAnyReplyV3 reply;
    A840KmdClientSession *session;
    A840KmdStatus status;
    uint32_t scope_id;
    uint32_t i;

    memcpy(&req, input, sizeof(req));
    if (req.count == 0u || req.count > A840_KMD_WAIT_ANY_MAX_FENCES ||
        (req.flags & ~UINT32_C(1)) != 0u)
        return A840_KMD_INVALID_ARG;

    for (i = 0u; i < req.count; ++i) {
        if (req.queues[i] > UINT64_C(4294967295))
            return A840_KMD_RANGE;
    }

    uc_lock(uc);
    session = find_client_locked(uc, client_id);
    if (session == NULL) {
        uc_unlock(uc);
        return A840_KMD_NOT_FOUND;
    }
    if (session->state != A840_KMD_CLIENT_OPEN) {
        uc_unlock(uc);
        return A840_KMD_DISCONNECTED;
    }

    for (i = 0u; i < req.count; ++i) {
        if (a840_kmd_fault_is_channel_poisoned(&uc->fault_capture,
                client_id, req.queues[i])) {
            uc_unlock(uc);
            return A840_KMD_POISONED;
        }
    }

    scope_id = session->scope_id;
    scoped_req = req;
    for (i = 0u; i < req.count; ++i)
        scoped_req.queues[i] = make_scoped_queue(scope_id, req.queues[i]);

    session->active_waiters++;
    uc_unlock(uc);

    /* Sleep outside uc->lock so healthy clients and close()/IRQ never stall. */
    memset(&reply, 0, sizeof(reply));
    status = a840_kmd_wait_any_wait_scoped(&uc->wait_any, &uc->wait_abi_state,
        scope_id, &scoped_req, &reply);

    uc_lock(uc);
    if (session->active_waiters > 0u)
        session->active_waiters--;
    if (session->state == A840_KMD_CLIENT_CLOSING && session->active_waiters == 0u) {
#if !defined(A840_KMD_CUSTOM_SYNC)
        (void)pthread_cond_broadcast(&uc->drain_cond);
#endif
    }
    uc_unlock(uc);

    if (status == A840_KMD_OK) {
        memcpy(output, &reply, sizeof(reply));
        if (out_written_bytes != NULL)
            *out_written_bytes = sizeof(reply);
    }
    return status;
}

A840KmdStatus a840_kmd_userclient_v3_dispatch(
    A840KmdUserClientV3 *uc,
    uint64_t client_id,
    uint32_t method,
    const void *input,
    size_t input_bytes,
    void *output,
    size_t output_bytes,
    size_t *out_written_bytes)
{
    A840KmdStatus status = check_init(uc);
    A840AbiV3Status abi_st;
    A840KmdClientSession *session;

    if (out_written_bytes != NULL)
        *out_written_bytes = 0u;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u)
        return A840_KMD_INVALID_ARG;

    /* Strict 3104-byte IOKit external-method envelope boundary check. */
    if (input_bytes > NativeAdrenoAbiEnvelopeBytes ||
        output_bytes > NativeAdrenoAbiEnvelopeBytes)
        return A840_KMD_RANGE;

    if ((input_bytes > 0u && input == NULL) ||
        (output_bytes > 0u && output == NULL))
        return A840_KMD_INVALID_ARG;

    /* Verify client session is open before routing v2 or v3 methods. */
    uc_lock(uc);
    session = find_client_locked(uc, client_id);
    if (session == NULL) {
        uc_unlock(uc);
        return A840_KMD_NOT_FOUND;
    }
    if (session->state != A840_KMD_CLIENT_OPEN) {
        uc_unlock(uc);
        return A840_KMD_DISCONNECTED;
    }

    /* v2 methods 1..19 passthrough contract: unchanged and untouched. */
    if (method >= A840_KMD_V2_METHOD_MIN && method <= A840_KMD_V2_METHOD_MAX) {
        uc_unlock(uc);
        return A840_KMD_PASS_TO_V2;
    }

    /* Explicit UNSUPPORTED contract for Phase 4/5 reserved methods 22 and 26
     * as well as any unknown selector outside v3 core methods.
     */
    abi_st = a840_abi_v3_check_method(&uc->abi_state, method);
    if (abi_st != A840_ABI_V3_OK) {
        uc_unlock(uc);
        return map_abi_status(abi_st);
    }

    abi_st = a840_abi_v3_validate_envelope(method, input_bytes, output_bytes);
    if (abi_st != A840_ABI_V3_OK) {
        uc_unlock(uc);
        return map_abi_status(abi_st);
    }

    if (method == NativeAdrenoMethodWaitAnyV3) {
        uc_unlock(uc);
        return dispatch_wait_any(uc, client_id, input, output, out_written_bytes);
    }

    switch (method) {
    case NativeAdrenoMethodGetZeroPageV3: {
        struct NativeAdrenoAbiZeroPageReplyV3 reply;
        memset(&reply, 0, sizeof(reply));
        status = a840_kmd_zero_page_query_client(&uc->zero_page, client_id, &reply);
        uc_unlock(uc);
        if (status == A840_KMD_OK) {
            memcpy(output, &reply, sizeof(reply));
            if (out_written_bytes != NULL)
                *out_written_bytes = sizeof(reply);
        }
        return status;
    }
    case NativeAdrenoMethodMapSharedPackV3: {
        struct NativeAdrenoAbiSharedPackReqV3 req;
        struct NativeAdrenoAbiSharedPackReplyV3 reply;
        memcpy(&req, input, sizeof(req));
        memset(&reply, 0, sizeof(reply));
        abi_st = a840_abi_v3_map_shared_pack(&uc->abi_state, &req, &reply);
        uc_unlock(uc);
        if (abi_st == A840_ABI_V3_OK || abi_st == A840_ABI_V3_STACK_KEY_MISMATCH) {
            memcpy(output, &reply, sizeof(reply));
            if (out_written_bytes != NULL)
                *out_written_bytes = sizeof(reply);
        }
        return map_abi_status(abi_st);
    }
    case NativeAdrenoMethodWriterPromiseV3: {
        struct NativeAdrenoAbiWriterPromiseV3 req;
        struct NativeAdrenoAbiWriterPromiseReplyV3 reply;
        memcpy(&req, input, sizeof(req));
        memset(&reply, 0, sizeof(reply));
        if (req.op != NativeAdrenoPromiseCancel &&
            a840_kmd_fault_is_channel_poisoned(&uc->fault_capture, client_id, req.queue)) {
            uc_unlock(uc);
            return A840_KMD_POISONED;
        }
        abi_st = a840_abi_v3_writer_promise(&uc->abi_state, client_id, &req, &reply);
        uc_unlock(uc);
        if (abi_st == A840_ABI_V3_OK) {
            memcpy(output, &reply, sizeof(reply));
            if (out_written_bytes != NULL)
                *out_written_bytes = sizeof(reply);
        }
        return map_abi_status(abi_st);
    }
    case NativeAdrenoMethodSurfaceMetaV3: {
        struct NativeAdrenoAbiSurfaceMetaV3 req;
        struct NativeAdrenoAbiSurfaceMetaV3 reply;
        uint64_t bo_bytes = 0u;
        memcpy(&req, input, sizeof(req));
        memset(&reply, 0, sizeof(reply));
        if (uc->bo_size_lookup == NULL) {
            uc_unlock(uc);
            return A840_KMD_UNSUPPORTED;
        }
        status = uc->bo_size_lookup(uc->bo_size_cookie, client_id,
            req.bo_handle, &bo_bytes);
        if (status != A840_KMD_OK) {
            uc_unlock(uc);
            return status;
        }
        abi_st = a840_abi_v3_surface_meta(&uc->abi_state, client_id,
            &req, bo_bytes, &reply);
        uc_unlock(uc);
        if (abi_st == A840_ABI_V3_OK) {
            memcpy(output, &reply, sizeof(reply));
            if (out_written_bytes != NULL)
                *out_written_bytes = sizeof(reply);
        }
        return map_abi_status(abi_st);
    }
    default:
        uc_unlock(uc);
        return A840_KMD_UNSUPPORTED;
    }
}

A840KmdStatus a840_kmd_userclient_v3_signal_fence(
    A840KmdUserClientV3 *uc,
    uint64_t client_id,
    uint64_t local_queue,
    uint64_t fence,
    uint64_t ticks)
{
    A840KmdStatus status = check_init(uc);
    A840KmdClientSession *session;
    uint64_t scoped_q;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u)
        return A840_KMD_INVALID_ARG;
    if (local_queue > UINT64_C(4294967295))
        return A840_KMD_RANGE;

    uc_lock(uc);
    session = find_client_locked(uc, client_id);
    if (session == NULL) {
        uc_unlock(uc);
        return A840_KMD_NOT_FOUND;
    }
    if (session->state != A840_KMD_CLIENT_OPEN) {
        uc_unlock(uc);
        return A840_KMD_DISCONNECTED;
    }
    scoped_q = make_scoped_queue(session->scope_id, local_queue);
    status = a840_kmd_wait_any_signal_fence(&uc->wait_any, scoped_q, fence, ticks);
    uc_unlock(uc);
    return status;
}

A840KmdStatus a840_kmd_userclient_v3_check_submit_channel(
    A840KmdUserClientV3 *uc,
    uint64_t client_id,
    uint64_t local_queue)
{
    A840KmdStatus status = check_init(uc);
    A840KmdClientSession *session;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u)
        return A840_KMD_INVALID_ARG;
    if (local_queue > UINT64_C(4294967295))
        return A840_KMD_RANGE;

    uc_lock(uc);
    session = find_client_locked(uc, client_id);
    if (session == NULL) {
        uc_unlock(uc);
        return A840_KMD_NOT_FOUND;
    }
    if (session->state != A840_KMD_CLIENT_OPEN) {
        uc_unlock(uc);
        return A840_KMD_DISCONNECTED;
    }
    status = a840_kmd_fault_check_channel(&uc->fault_capture, client_id, local_queue);
    uc_unlock(uc);
    return status;
}

A840KmdStatus a840_kmd_userclient_v3_report_fault(
    A840KmdUserClientV3 *uc,
    const A840KmdFaultInput *input,
    A840KmdFaultSnapshot *out_snapshot)
{
    A840KmdStatus status = check_init(uc);
    if (status != A840_KMD_OK)
        return status;
    return a840_kmd_fault_record(&uc->fault_capture, input, out_snapshot);
}

A840KmdStatus a840_kmd_userclient_v3_queue_key(
    A840KmdUserClientV3 *uc,
    uint64_t client_id,
    uint64_t local_queue,
    uint64_t *out_scoped_queue)
{
    A840KmdStatus status = check_init(uc);
    A840KmdClientSession *session;

    if (status != A840_KMD_OK)
        return status;
    if (client_id == 0u || out_scoped_queue == NULL)
        return A840_KMD_INVALID_ARG;
    if (local_queue > UINT64_C(4294967295))
        return A840_KMD_RANGE;

    uc_lock(uc);
    session = find_client_locked(uc, client_id);
    if (session == NULL) {
        uc_unlock(uc);
        return A840_KMD_NOT_FOUND;
    }
    if (session->state != A840_KMD_CLIENT_OPEN) {
        uc_unlock(uc);
        return A840_KMD_DISCONNECTED;
    }
    *out_scoped_queue = make_scoped_queue(session->scope_id, local_queue);
    uc_unlock(uc);
    return A840_KMD_OK;
}

size_t a840_kmd_userclient_v3_pending_promises(
    A840KmdUserClientV3 *uc,
    uint64_t client_id)
{
    size_t pending;
    if (check_init(uc) != A840_KMD_OK)
        return 0u;
    uc_lock(uc);
    pending = a840_abi_v3_pending_promises(&uc->abi_state, client_id);
    uc_unlock(uc);
    return pending;
}

uint32_t a840_kmd_userclient_v3_open_client_count(A840KmdUserClientV3 *uc)
{
    uint32_t count;
    if (check_init(uc) != A840_KMD_OK)
        return 0u;
    uc_lock(uc);
    count = uc->open_client_count;
    uc_unlock(uc);
    return count;
}
