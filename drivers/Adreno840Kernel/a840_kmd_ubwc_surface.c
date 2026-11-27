/* SPDX-License-Identifier: MIT */
#include "a840_kmd_ubwc_surface.h"
#include "chip_facts_a840.h"
#include "Adreno840Caps.h"
#include <string.h>

A840KmdStatus a840_kmd_surface_init(A840KmdSurfaceManager *m, const A840KmdSurfaceOps *ops)
{
    if (!m || !ops || !ops->lock || !ops->unlock)
        return A840_KMD_INVALID_ARG;
    memset(m, 0, sizeof(*m));
    m->ops = *ops;
    return A840_KMD_OK;
}

static A840KmdSurfaceBinding *binding(A840KmdSurfaceManager *m, uint64_t client, uint64_t handle)
{
    for (size_t i = 0; i < A840_SURFACE_BINDINGS; ++i) {
        A840KmdSurfaceBinding *b = &m->bindings[i];
        if (b->client == client && b->handle == handle)
            return b;
    }
    return NULL;
}

static A840KmdSurfaceRecord *record(A840KmdSurfaceManager *m, uint64_t identity)
{
    for (size_t i = 0; i < A840_ABI_V3_SURFACE_SLOTS; ++i)
        if (m->records[i].identity == identity)
            return &m->records[i];
    return NULL;
}

static A840KmdStatus attach(A840KmdSurfaceManager *m, uint64_t client, uint64_t handle,
    uint64_t identity, const A840KmdSurfaceLayout *creator)
{
    if (!m || !client || !handle || !identity)
        return A840_KMD_INVALID_ARG;
    m->ops.lock(m->ops.cookie);
    A840KmdStatus status = A840_KMD_OK;
    A840KmdSurfaceRecord *r = record(m, identity);
    A840KmdSurfaceBinding *slot = binding(m, 0, 0);
    if (binding(m, client, handle) || (creator && r))
        status = A840_KMD_STATE_CONFLICT;
    else if (!creator && !r)
        status = A840_KMD_NOT_FOUND;
    else {
        if (!r)
            r = record(m, 0);
        if (!r || !slot)
            status = A840_KMD_CAPACITY;
    }
    if (status == A840_KMD_OK) {
        if (creator) {
            r->identity = identity;
            r->layout = *creator;
        }
        *slot = (A840KmdSurfaceBinding){client, handle, (size_t)(r - m->records), creator != NULL};
        ++r->bindings;
    }
    m->ops.unlock(m->ops.cookie);
    return status;
}

A840KmdStatus a840_kmd_surface_create(A840KmdSurfaceManager *m, uint64_t client,
    uint64_t handle, uint64_t identity, const A840KmdSurfaceLayout *layout)
{
    if (!layout || !layout->width || !layout->height || !layout->bytes ||
        layout->bytes > A840_ABI_V3_GPU_VA_LIMIT)
        return A840_KMD_INVALID_ARG;
    return attach(m, client, handle, identity, layout);
}

A840KmdStatus a840_kmd_surface_import(A840KmdSurfaceManager *m, uint64_t client,
    uint64_t handle, uint64_t identity)
{
    return attach(m, client, handle, identity, NULL);
}

static void detach(A840KmdSurfaceManager *m, A840KmdSurfaceBinding *b)
{
    A840KmdSurfaceRecord *r = &m->records[b->record];
    if (--r->bindings == 0)
        memset(r, 0, sizeof(*r));
    memset(b, 0, sizeof(*b));
}

void a840_kmd_surface_release(A840KmdSurfaceManager *m, uint64_t client, uint64_t handle)
{
    if (!m || !client || !handle)
        return;
    m->ops.lock(m->ops.cookie);
    A840KmdSurfaceBinding *b = binding(m, client, handle);
    if (b)
        detach(m, b);
    m->ops.unlock(m->ops.cookie);
}

void a840_kmd_surface_close_client(A840KmdSurfaceManager *m, uint64_t client)
{
    if (!m || !client)
        return;
    m->ops.lock(m->ops.cookie);
    for (size_t i = 0; i < A840_SURFACE_BINDINGS; ++i)
        if (m->bindings[i].client == client)
            detach(m, &m->bindings[i]);
    m->ops.unlock(m->ops.cookie);
}

void a840_kmd_surface_destroy_bo(A840KmdSurfaceManager *m, uint64_t identity)
{
    if (!m || !identity)
        return;
    m->ops.lock(m->ops.cookie);
    for (size_t i = 0; i < A840_SURFACE_BINDINGS; ++i) {
        A840KmdSurfaceBinding *b = &m->bindings[i];
        if (b->client && m->records[b->record].identity == identity)
            detach(m, b);
    }
    m->ops.unlock(m->ops.cookie);
}

static A840KmdStatus validate(const struct NativeAdrenoAbiSurfaceMetaV3 *s,
    const A840KmdSurfaceLayout *layout)
{
    const A840ChipFacts *chip = a840_chip_facts_kaanapali_default();
    if (s->ubwc_enabled > 1 || s->ubwc_version != chip->ubwc_version ||
        s->highest_bank_bit != A840_KAANAPALI_HIGHEST_BANK_BIT ||
        s->swizzle_levels != chip->ubwc_swizzle || s->macrotile_mode != chip->macrotile_mode ||
        (s->flags & ~(uint32_t)A840_SURFACE_SCANOUT_ELIGIBLE))
        return A840_KMD_INVALID_ARG;
    if (s->ubwc_enabled) {
        const A840FormatCaps *fmt = a840_format_caps_lookup(layout->format);
        if (!fmt || !fmt->ubwc_supported)
            return A840_KMD_UNSUPPORTED;
        if (!s->meta_pitch || !layout->meta_rows)
            return A840_KMD_INVALID_ARG;
    }
    uint64_t rows = s->ubwc_enabled ? layout->meta_rows : 1u;
    if (s->meta_offset > layout->bytes ||
        rows * s->meta_pitch > layout->bytes - s->meta_offset)
        return A840_KMD_RANGE;
    if (s->meta_offset % A840_ABI_V3_PAGE_BYTES ||
        s->meta_pitch % A840_ABI_V3_META_PITCH_ALIGNMENT)
        return A840_KMD_ALIGNMENT;
    return A840_KMD_OK;
}

static int mdss_accepts(const A840KmdMdssCaps *caps, const A840KmdSurfaceRecord *r)
{
    const A840KmdSurfaceLayout *l = &r->layout;
    if (!caps || !(caps->flags & A840_MDSS_UBWC6) || !caps->formats ||
        l->width > caps->max_width || l->height > caps->max_height ||
        !l->display_base || l->display_base % A840_ABI_V3_PAGE_BYTES ||
        l->bytes - 1 > UINT64_MAX - l->display_base)
        return 0;
    if (caps->base_alignment && (l->display_base % caps->base_alignment ||
        r->meta.meta_offset % caps->base_alignment))
        return 0;
    if (caps->pitch_alignment && r->meta.meta_pitch % caps->pitch_alignment)
        return 0;
    for (size_t i = 0; i < caps->format_count; ++i)
        if (caps->formats[i] == l->format)
            return 1;
    return 0;
}

static A840KmdStatus negotiate(A840KmdSurfaceManager *m, uint64_t client,
    const struct NativeAdrenoAbiSurfaceMetaV3 *req, uint32_t scan,
    const A840KmdMdssCaps *caps, A840KmdSurfaceResult *out)
{
    A840KmdSurfaceBinding *b = binding(m, client, req->bo_handle);
    if (!b)
        return A840_KMD_NOT_FOUND;
    A840KmdSurfaceRecord *r = &m->records[b->record];
    if (req->op == 1) {
        if (!b->owner)
            return A840_KMD_PERMISSION_DENIED;
        A840KmdStatus rc = validate(req, &r->layout);
        if (rc != A840_KMD_OK) {
            if (scan && req->ubwc_enabled && (rc == A840_KMD_UNSUPPORTED || rc == A840_KMD_ALIGNMENT)) {
                out->meta = *req;
                out->meta.flags = 0;
                out->fallback_linear_required = 1;
                if (rc == A840_KMD_ALIGNMENT)
                    return A840_KMD_OK;
            }
            return rc;
        }
        r->meta = *req;
        r->meta.flags = 0;
        r->has_meta = 1;
    } else if (!r->has_meta) {
        return A840_KMD_NOT_FOUND;
    }
    out->layout_valid = 1;
    out->meta = r->meta;
    out->meta.bo_handle = b->handle;
    out->meta.op = req->op;
    if (scan && out->meta.ubwc_enabled) {
        out->scanout_eligible = (uint32_t)mdss_accepts(caps, r);
        out->fallback_linear_required = !out->scanout_eligible;
        out->meta.flags = out->scanout_eligible ? A840_SURFACE_SCANOUT_ELIGIBLE : 0;
    }
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_surface_meta(A840KmdSurfaceManager *m, uint64_t client,
    const struct NativeAdrenoAbiSurfaceMetaV3 *request, uint32_t scan,
    const A840KmdMdssCaps *caps, A840KmdSurfaceResult *out)
{
    if (!out || !request)
        return A840_KMD_INVALID_ARG;
    struct NativeAdrenoAbiSurfaceMetaV3 req = *request;
    memset(out, 0, sizeof(*out));
    if (!m || !client || !req.bo_handle || req.op > 1 || scan > 1)
        return A840_KMD_INVALID_ARG;
    m->ops.lock(m->ops.cookie);
    A840KmdStatus rc = negotiate(m, client, &req, scan, caps, out);
    m->ops.unlock(m->ops.cookie);
    return rc;
}
