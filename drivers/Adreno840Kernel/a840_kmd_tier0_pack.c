/* SPDX-License-Identifier: MIT */
#include "a840_kmd_tier0_pack.h"
#include <string.h>

A840KmdStatus a840_kmd_pack_init(A840KmdPackManager *m, const A840KmdPackMapOps *ops)
{
    if (!m || !ops || !ops->lock || !ops->unlock || !ops->acquire || !ops->release ||
        !ops->wire_ro || !ops->unwire || !ops->bind_ro || !ops->revoke)
        return A840_KMD_INVALID_ARG;
    memset(m, 0, sizeof(*m));
    m->ops = *ops;
    return A840_KMD_OK;
}

static A840KmdPackClient *slot(A840KmdPackManager *m, uint64_t id)
{
    for (size_t i = 0; i < A840_PACK_MAX_CLIENTS; ++i)
        if (m->clients[i].client == id)
            return &m->clients[i];
    return NULL;
}

static void release_image(A840KmdPackManager *m, A840KmdPackImage *image)
{
    if (image->source)
        m->ops.release(m->ops.cookie, image->source);
    memset(image, 0, sizeof(*image));
}

static void retire_active(A840KmdPackManager *m)
{
    if (m->backing) {
        m->ops.unwire(m->ops.cookie, m->backing);
        m->backing = NULL;
    }
    release_image(m, &m->active);
}

static void activate_if_drained(A840KmdPackManager *m)
{
    if (m->pending.source && !m->bound_clients) {
        retire_active(m);
        m->active = m->pending;
        memset(&m->pending, 0, sizeof(m->pending));
        m->warm = m->boot_complete ? A840_WARM_ARMED : A840_WARM_IDLE;
    }
}

static int same_image(const A840KmdPackIdentity *a, const A840KmdPackIdentity *b)
{
    return a->actual_stack_key == b->actual_stack_key && a->bytes == b->bytes &&
        !memcmp(a->sha256, b->sha256, sizeof(a->sha256));
}

A840KmdStatus a840_kmd_pack_publish(A840KmdPackManager *m, void *source)
{
    if (!m || !source)
        return A840_KMD_INVALID_ARG;
    m->ops.lock(m->ops.cookie);
    A840KmdPackImage image = {0};
    A840KmdStatus rc = m->ops.acquire(m->ops.cookie, source, &image);
    if (rc == A840_KMD_OK) {
        const A840KmdPackImage *target = m->pending.source ? &m->pending : &m->active;
        if (!image.source || !image.identity.actual_stack_key)
            rc = A840_KMD_INVALID_ARG;
        else if (!image.identity.bytes || image.identity.bytes > A840_PACK_MAX_BYTES)
            rc = A840_KMD_RANGE;
        else if (!(target->source && same_image(&target->identity, &image.identity))) {
            if (m->pending.source)
                rc = A840_KMD_STATE_CONFLICT;
            else {
                m->pending = image;
                image.source = NULL;
                activate_if_drained(m);
            }
        }
        release_image(m, &image);
    }
    m->ops.unlock(m->ops.cookie);
    return rc;
}

A840KmdStatus a840_kmd_pack_open_client(A840KmdPackManager *m, uint64_t id)
{
    if (!m || !id)
        return A840_KMD_INVALID_ARG;
    m->ops.lock(m->ops.cookie);
    A840KmdStatus rc = A840_KMD_OK;
    A840KmdPackClient *s = slot(m, 0);
    if (slot(m, id))
        rc = A840_KMD_STATE_CONFLICT;
    else if (!s)
        rc = A840_KMD_CAPACITY;
    else
        s->client = id;
    m->ops.unlock(m->ops.cookie);
    return rc;
}

/* Successful revoke is the only path which forgets a backend mapping. */
static A840KmdStatus revoke(A840KmdPackManager *m, A840KmdPackClient *s)
{
    if (s->bound) {
        A840KmdStatus rc = m->ops.revoke(m->ops.cookie, m->backing, s->client);
        if (rc != A840_KMD_OK)
            return rc;
        s->bound = 0;
        --m->bound_clients;
        memset(&s->views, 0, sizeof(s->views));
    }
    m->total_refs -= s->refs;
    s->refs = 0;
    activate_if_drained(m);
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_pack_close_client(A840KmdPackManager *m, uint64_t id)
{
    if (!m || !id)
        return A840_KMD_INVALID_ARG;
    m->ops.lock(m->ops.cookie);
    A840KmdPackClient *s = slot(m, id);
    A840KmdStatus rc = A840_KMD_NOT_FOUND;
    if (s) {
        s->closing = 1;
        rc = revoke(m, s);
        if (rc == A840_KMD_OK)
            memset(s, 0, sizeof(*s));
    }
    m->ops.unlock(m->ops.cookie);
    return rc;
}

A840KmdStatus a840_kmd_pack_destroy(A840KmdPackManager *m)
{
    if (!m)
        return A840_KMD_INVALID_ARG;
    m->ops.lock(m->ops.cookie);
    release_image(m, &m->pending);
    A840KmdStatus rc = A840_KMD_OK;
    for (size_t i = 0; i < A840_PACK_MAX_CLIENTS; ++i) {
        A840KmdPackClient *s = &m->clients[i];
        if (s->client) {
            s->closing = 1;
            A840KmdStatus drop = revoke(m, s);
            if (drop == A840_KMD_OK)
                memset(s, 0, sizeof(*s));
            else
                rc = drop;
        }
    }
    if (rc == A840_KMD_OK)
        retire_active(m);
    m->ops.unlock(m->ops.cookie);
    return rc;
}

static int valid_views(A840KmdPackViews views, uint64_t bytes, uint32_t cpu)
{
    return views.gpu_va && views.gpu_va % A840_ABI_V3_PAGE_BYTES == 0 &&
        views.gpu_va < A840_KMD_GPU_VA_LIMIT && bytes <= A840_KMD_GPU_VA_LIMIT - views.gpu_va &&
        (cpu ? views.cpu_va && views.cpu_va % A840_ABI_V3_PAGE_BYTES == 0 &&
            bytes <= UINT64_MAX - views.cpu_va : views.cpu_va == 0);
}

static A840KmdStatus map_locked(A840KmdPackManager *m, uint64_t id,
    const struct NativeAdrenoAbiSharedPackReqV3 *req, struct NativeAdrenoAbiSharedPackReplyV3 *out)
{
    A840KmdPackClient *s = slot(m, id);
    if (!s || s->closing)
        return A840_KMD_DISCONNECTED;
    if (!m->active.source)
        return A840_KMD_NOT_FOUND;
    const A840KmdPackIdentity *image = &m->active.identity;
    if (req->expected_stack_key != image->actual_stack_key) {
        out->actual_stack_key = image->actual_stack_key;
        return A840_KMD_STACK_KEY_MISMATCH;
    }
    if (m->pending.source)
        return A840_KMD_STATE_CONFLICT;
    if (s->refs == UINT32_MAX)
        return A840_KMD_CAPACITY;
    if (!m->backing) {
        void *backing = NULL;
        A840KmdStatus rc = m->ops.wire_ro(m->ops.cookie, &m->active,
            A840_KMD_SMMU_PERM_READ_ONLY, &backing);
        if (rc != A840_KMD_OK)
            return rc;
        if (!backing)
            return A840_KMD_IO_ERROR;
        m->backing = backing;
    }
    if (!s->bound || (req->map_cpu && !s->views.cpu_va)) {
        A840KmdPackViews views = {0};
        A840KmdStatus rc = m->ops.bind_ro(m->ops.cookie, m->backing, id,
            A840_KMD_SMMU_PERM_READ_ONLY, req->map_cpu, &views);
        if (rc != A840_KMD_OK)
            return rc;
        if (!s->bound) {
            s->bound = 1;
            ++m->bound_clients;
        }
        if (!valid_views(views, image->bytes, req->map_cpu) ||
            (s->refs && s->views.gpu_va != views.gpu_va)) {
            /* Deny more binds. A failed upgrade cannot retire earlier
             * leases; leave them pinned until their unmaps or client close. */
            s->closing = 1;
            if (!s->refs)
                (void)revoke(m, s);
            return A840_KMD_IO_ERROR;
        }
        s->views = views;
    }
    ++s->refs;
    ++m->total_refs;
    out->gpu_va = s->views.gpu_va;
    out->cpu_va = req->map_cpu ? s->views.cpu_va : 0;
    out->bytes = image->bytes;
    out->actual_stack_key = image->actual_stack_key;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_pack_map_shared_pack(A840KmdPackManager *m, uint64_t id,
    const struct NativeAdrenoAbiSharedPackReqV3 *req, uint32_t perm,
    struct NativeAdrenoAbiSharedPackReplyV3 *out)
{
    if (!out)
        return A840_KMD_INVALID_ARG;
    struct NativeAdrenoAbiSharedPackReqV3 input = {0};
    if (req)
        memcpy(&input, req, sizeof(input));
    memset(out, 0, sizeof(*out));
    if (!m || !id || !req || input.map_cpu > 1 || input.reserved)
        return A840_KMD_INVALID_ARG;
    if (perm != A840_KMD_SMMU_PERM_READ_ONLY)
        return A840_KMD_PERMISSION_DENIED;
    m->ops.lock(m->ops.cookie);
    A840KmdStatus rc = map_locked(m, id, &input, out);
    m->ops.unlock(m->ops.cookie);
    return rc;
}

A840KmdStatus a840_kmd_pack_unmap(A840KmdPackManager *m, uint64_t id)
{
    if (!m || !id)
        return A840_KMD_INVALID_ARG;
    m->ops.lock(m->ops.cookie);
    A840KmdPackClient *s = slot(m, id);
    A840KmdStatus rc = A840_KMD_NOT_FOUND;
    if (s && s->refs) {
        if (s->refs == 1)
            rc = revoke(m, s);
        else {
            --s->refs;
            --m->total_refs;
            rc = A840_KMD_OK;
        }
    }
    m->ops.unlock(m->ops.cookie);
    return rc;
}

static A840KmdWarmState warm_state(const A840KmdPackManager *m)
{
    if (m->pressure && (m->warm == A840_WARM_ARMED || m->warm == A840_WARM_WARMING))
        return A840_WARM_PAUSED_MEMORY_PRESSURE;
    return m->warm;
}

A840KmdStatus a840_kmd_pack_stats(A840KmdPackManager *m, uint64_t id, A840KmdPackStats *out)
{
    if (!m || !out)
        return A840_KMD_INVALID_ARG;
    m->ops.lock(m->ops.cookie);
    const A840KmdPackClient *s = id ? slot(m, id) : NULL;
    *out = (A840KmdPackStats){m->active.identity, m->total_refs, m->backing != NULL,
        m->bound_clients, m->pending.source != NULL, s ? s->refs : 0, warm_state(m)};
    m->ops.unlock(m->ops.cookie);
    return A840_KMD_OK;
}

void a840_kmd_pack_boot_complete(A840KmdPackManager *m)
{
    if (!m)
        return;
    m->ops.lock(m->ops.cookie);
    m->boot_complete = 1;
    if (m->warm == A840_WARM_IDLE)
        m->warm = A840_WARM_ARMED;
    m->ops.unlock(m->ops.cookie);
}

void a840_kmd_pack_set_params(A840KmdPackManager *m, uint32_t flags)
{
    if (!m)
        return;
    m->ops.lock(m->ops.cookie);
    m->pressure = !!(flags & NativeAdrenoParamsV2MemoryPressure);
    m->ops.unlock(m->ops.cookie);
}

A840KmdStatus a840_kmd_pack_warm_step(A840KmdPackManager *m, A840KmdWarmUnit unit, void *cookie)
{
    if (!m || !unit)
        return A840_KMD_INVALID_ARG;
    m->ops.lock(m->ops.cookie);
    A840KmdStatus rc = A840_KMD_STATE_CONFLICT;
    A840KmdWarmState state = warm_state(m);
    if (!m->pending.source && (state == A840_WARM_ARMED || state == A840_WARM_WARMING)) {
        uint32_t done = 0;
        rc = unit(cookie, &done);
        if (rc == A840_KMD_OK)
            m->warm = done ? A840_WARM_READY : A840_WARM_WARMING;
    }
    m->ops.unlock(m->ops.cookie);
    return rc;
}
