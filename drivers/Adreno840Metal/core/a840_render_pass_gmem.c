/* SPDX-License-Identifier: MIT */
#define A840_EMIT_OPS_INTERNAL
#include "a840_emit_ops.h"
#include "a840_render_pass_gmem.h"
#include "Adreno840Regs.h"
#include <limits.h>

#define TRY A840_OPS_TRY
static A840EmitStatus draw_ib(A840CmdStream *cs, A840GmemRange ib)
{
    if (!ib.bytes) return A840_EMIT_OK;
    if (ib.bytes % 4 || ib.bytes / 4 >= (UINT32_C(1) << 20)) return A840_EMIT_RANGE_ERROR;
    TRY(a840_ops_va(ib.va,ib.bytes,4));
    A840OpsBatch b;
    TRY(a840_ops_batch_init(&b));
    uint32_t packet[] = {(uint32_t)ib.va,(uint32_t)(ib.va >> 32),(uint32_t)(ib.bytes/4)};
    TRY(a840_cs_emit_pkt7(&b.cs,A840_CP_INDIRECT_BUFFER,packet,3));
    TRY(a840_ops_add_reloc(&b,1,ib.bo,ib.va,A840_RELOC_READ));
    return a840_ops_commit(cs,&b);
}

A840EmitStatus a840_rt_gmem_encode(A840CmdStream *dst, const A840RtGmemPass *pass,
                                    A840GmemPlan *out_plan)
{
    A840GmemPlan p;
    TRY(a840_ops_input_storage(dst,pass,sizeof(*pass),_Alignof(A840RtGmemPass)));
    if (pass->vsc)
        TRY(a840_ops_input_storage(dst,pass->vsc,sizeof(*pass->vsc),_Alignof(A840GmemVsc)));
    if (out_plan) TRY(a840_ops_input_storage(dst,out_plan,sizeof(p),_Alignof(A840GmemPlan)));
    TRY(a840_gmem_plan(&pass->desc,&p));
    if (p.path != A840_PATH_GMEM) return A840_EMIT_UNSUPPORTED;
    if ((pass->desc.draw_count != 0) != (pass->tile_draws.bytes != 0) ||
        (pass->vsc != NULL) != (pass->bin_draws.bytes != 0) ||
        (pass->vsc && !pass->tile_draws.bytes)) return A840_EMIT_INVALID_ARG;

    /* A private cursor makes a failure even on the last tile retryable. */
    A840CmdStream cs = *dst;
    TRY(a840_emit_gmem_binning_begin(&cs,&p,pass->vsc));
    TRY(draw_ib(&cs,pass->bin_draws));
    TRY(a840_emit_gmem_binning_end(&cs,pass->vsc));
    for (uint32_t y = 0; y < p.tiles_y; ++y) {
        for (uint32_t x = 0; x < p.tiles_x; ++x) {
            TRY(a840_emit_gmem_tile_select(&cs,&p,x,y,pass->vsc));
            TRY(a840_emit_gmem_tile_load(&cs,&p,pass->bindings));
            TRY(a840_emit_gmem_tile_clear(&cs,&p,pass->bindings));
            TRY(draw_ib(&cs,pass->tile_draws));
            TRY(a840_emit_gmem_tile_store(&cs,&p,pass->bindings));
            if (x+1 < p.tiles_x || y+1 < p.tiles_y) TRY(a840_emit_gmem_tile_end(&cs));
        }
    }
    TRY(a840_emit_gmem_pass_end(&cs));
    dst->used_dwords = cs.used_dwords;
    dst->reloc_count = cs.reloc_count;
    if (out_plan) *out_plan = p;
    return A840_EMIT_OK;
}

/* Forward unmodified W03 operations; only queue_submit needs composition.
 * Preserve optional method absence (including v2-only transports) in open. */
#define FORWARD(name, args, ...) \
    static A840TransportStatus forward_##name args { \
        A840RtGmemQueue *q = ctx; \
        return q->downstream.ops->name(q->downstream.ctx,__VA_ARGS__); \
    }
FORWARD(query_caps,(void *ctx,A840TransportCaps *c),c)
FORWARD(bo_alloc,(void *ctx,const A840BoAllocDesc *d,uint64_t *h),d,h)
FORWARD(bo_import,(void *ctx,const A840BoImportDesc *d,uint64_t *h),d,h)
FORWARD(bo_map,(void *ctx,const A840BoMapDesc *d,A840BoMapReply *r),d,r)
FORWARD(bo_unmap,(void *ctx,const A840BoUnmapDesc *d),d)
FORWARD(bo_free,(void *ctx,uint64_t h),h)
FORWARD(queue_create,(void *ctx,uint32_t p,uint64_t *h),p,h)
FORWARD(queue_destroy,(void *ctx,uint64_t h),h)
FORWARD(fence_wait,(void *ctx,uint64_t qid,uint64_t f,uint64_t t,uint64_t *r),qid,f,t,r)
FORWARD(wait_any_v3,(void *ctx,const struct NativeAdrenoAbiWaitAnyV3 *d,struct NativeAdrenoAbiWaitAnyReplyV3 *r),d,r)
FORWARD(get_zero_page_v3,(void *ctx,struct NativeAdrenoAbiZeroPageReplyV3 *r),r)
FORWARD(surface_meta_v3,(void *ctx,struct NativeAdrenoAbiSurfaceMetaV3 *d),d)
#undef FORWARD

typedef struct PendingPrefix {
    A840RtGmemQueue *owner;
    A840SubmitIb ib;
    A840CmdStream cs;
} PendingPrefix;
/* W03 calls queue_submit synchronously from commit, before callbacks. A
 * thread-local, single-use prefix cannot attach to a different queue/CB. */
static _Thread_local PendingPrefix *pending;
static A840TransportStatus submit_batch(void *ctx,const A840SubmitDesc *d,A840SubmitReply *reply)
{
    A840RtGmemQueue *q = ctx;
    PendingPrefix *p = pending;
    if (!p || p->owner != q || d->queue != a840_rt_queue_handle(q->queue))
        return q->downstream.ops->queue_submit(q->downstream.ctx,d,reply);
    pending = NULL;
    if (d->ib_count != 1 || d->reloc_count > p->cs.reloc_capacity - p->cs.reloc_count ||
        p->cs.reloc_count > UINT32_MAX - d->reloc_count) return A840_TRANSPORT_OUT_OF_BOUNDS;
    A840SubmitIb ibs[] = {p->ib,d->ibs[0]};
    for (uint32_t i = 0; i < d->reloc_count; ++i) {
        A840Reloc r = d->relocs[i];
        if (p->cs.used_dwords > UINT32_MAX - r.dword_offset) return A840_TRANSPORT_OUT_OF_BOUNDS;
        r.dword_offset += (uint32_t)p->cs.used_dwords;
        p->cs.relocs[p->cs.reloc_count+i] = r;
    }
    A840SubmitDesc batch = *d;
    batch.ibs = ibs; batch.ib_count = 2;
    batch.relocs = p->cs.relocs; batch.reloc_count += (uint32_t)p->cs.reloc_count;
    return q->downstream.ops->queue_submit(q->downstream.ctx,&batch,reply);
}

A840RtStatus a840_rt_gmem_open(A840RtGmemQueue *q, A840BackendTransport downstream)
{
    if (!q || !downstream.ops || !downstream.ops->queue_submit) return A840_RT_INVALID_ARG;
    memset(q,0,sizeof(*q));
    q->downstream = downstream;
#define INSTALL(name) q->ops.name = downstream.ops->name ? forward_##name : NULL
    INSTALL(query_caps); INSTALL(bo_alloc); INSTALL(bo_import); INSTALL(bo_map);
    INSTALL(bo_unmap); INSTALL(bo_free); INSTALL(queue_create); INSTALL(queue_destroy);
    INSTALL(fence_wait); INSTALL(wait_any_v3); INSTALL(get_zero_page_v3); INSTALL(surface_meta_v3);
#undef INSTALL
    q->ops.queue_submit = submit_batch;
    A840BackendTransport bridge = {&q->ops,q};
    A840RtStatus st = a840_rt_device_create(&bridge,A840_ROUTE_PHASE2_ALL,&q->device);
    if (st != A840_RT_OK) return st;
    if (q->device->chip_facts.gmem_bytes != A840_KAANAPALI_GMEM_BYTES)
        st = A840_RT_UNSUPPORTED;
    else st = a840_rt_queue_create(q->device,A840_QUEUE_PRIORITY_NORMAL,&q->queue);
    if (st != A840_RT_OK) { a840_rt_device_destroy(q->device); q->device = NULL; }
    return st;
}

void a840_rt_gmem_close(A840RtGmemQueue *q)
{
    if (!q) return;
    a840_rt_queue_destroy(q->queue);
    a840_rt_device_destroy(q->device);
    memset(q,0,sizeof(*q));
}

static A840RtStatus resident_range(A840GmemRange r, uint32_t usage,
                                    A840RtBuffer *const buffers[], size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const A840RtBuffer *b = buffers[i];
        if (b->bo_handle != r.bo || r.va < b->gpu_va) continue;
        uint64_t off = r.va - b->gpu_va;
        if (!r.bytes || off > b->size_bytes || r.bytes > b->size_bytes-off)
            return A840_RT_RANGE_ERROR;
        return (b->usage_flags & usage) == usage ? A840_RT_OK : A840_RT_INVALID_ARG;
    }
    return A840_RT_INVALID_ARG;
}

static A840RtStatus check_surface(const A840GmemSurface *s, bool ubwc, uint32_t usage,
                                   A840RtBuffer *const b[], size_t n)
{
    A840RtStatus st = resident_range(s->data,usage,b,n);
    return st == A840_RT_OK && ubwc ? resident_range(s->flags,usage,b,n) : st;
}

static A840RtStatus residency(const A840RtGmemPass *p, A840RtBuffer *const b[], size_t n)
{
    for (unsigned i = 0; i < A840_GMEM_PLANES; ++i) {
        const A840GmemAttachment *a = &p->desc.attachments[i];
        if (!a->cpp || a->memoryless) continue;
        A840RtStatus st = A840_RT_OK;
        if (a->load == A840_LOAD_LOAD)
            st = check_surface(&p->bindings[i].image,a->ubwc,A840_BUF_USAGE_READ,b,n);
        if (st != A840_RT_OK) return st;
        if (a->store != A840_STORE_DONT_CARE) {
            const A840GmemSurface *dst = a->store == A840_STORE_RESOLVE ?
                &p->bindings[i].resolve : &p->bindings[i].image;
            st = check_surface(dst,a->ubwc,A840_BUF_USAGE_WRITE,b,n);
            if (st != A840_RT_OK) return st;
        }
    }
    if (p->vsc) {
        A840GmemRange ranges[] = {p->vsc->draw,p->vsc->primitive,p->vsc->sizes};
        for (unsigned i = 0; i < 3; ++i) {
            A840RtStatus st = resident_range(ranges[i],A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE,b,n);
            if (st != A840_RT_OK) return st;
        }
    }
    if (p->bin_draws.bytes) {
        A840RtStatus st = resident_range(p->bin_draws,A840_BUF_USAGE_READ,b,n);
        if (st != A840_RT_OK) return st;
    }
    return p->tile_draws.bytes ? resident_range(p->tile_draws,A840_BUF_USAGE_READ,b,n) : A840_RT_OK;
}

A840RtStatus a840_rt_gmem_submit(A840RtGmemQueue *q,const A840RtGmemPass *pass,
    A840RtBuffer *ib,A840Reloc *scratch,size_t capacity,A840RtBuffer *const resident[],
    size_t count,A840RtGmemFlight *flight)
{
    if (!q || !q->queue || !pass || !flight || !scratch || !capacity ||
        count > A840_GMEM_MAX_RESIDENT || (count && !resident)) return A840_RT_INVALID_ARG;
    if (flight->cb || pending) return A840_RT_STATE_ERROR;
    if (!a840_rt_device_owns_buffer(q->device,ib)) return A840_RT_WRONG_DEVICE;
    if (!ib->cpu_ptr || !(ib->usage_flags & A840_BUF_USAGE_READ) ||
        ib->size_bytes/4 > UINT32_MAX) return A840_RT_INVALID_ARG;
    for (size_t i = 0; i < count; ++i) {
        if (!a840_rt_device_owns_buffer(q->device,resident[i])) return A840_RT_WRONG_DEVICE;
        if (resident[i] == ib) return A840_RT_INVALID_ARG;
    }
    A840RtStatus st = residency(pass,resident,count);
    if (st != A840_RT_OK) return st;
    PendingPrefix prefix = {0};
    prefix.owner = q;
    A840EmitStatus est = a840_cs_init(&prefix.cs,ib->cpu_ptr,ib->size_bytes/4,scratch,capacity);
    if (est == A840_EMIT_OK) est = a840_rt_gmem_encode(&prefix.cs,pass,NULL);
    if (est != A840_EMIT_OK) return a840_rt_status_from_emit(est);
    if (prefix.cs.reloc_count >= capacity) return A840_RT_CAPACITY;
    prefix.ib = (A840SubmitIb){ib->bo_handle,ib->gpu_va,prefix.cs.used_dwords*sizeof(uint32_t)};

    /* The trailing W03 IB makes resolved writes visible to texture fetches
     * and supplies W03's ordinary RB_DONE_TS, fence and queue bookkeeping. */
    A840RtCmdBuffer *cb = NULL;
    st = a840_rt_cb_create(q->queue,NULL,&cb);
    if (st != A840_RT_OK) return st;
    st = a840_rt_cb_encode_barrier(cb,A840_CACHE_UCHE_INV,false,false);
    if (st == A840_RT_OK) {
        pending = &prefix;
        st = a840_rt_cb_commit(cb);
        pending = NULL;
    }
    if (st != A840_RT_OK) { a840_rt_cb_release(cb); return st; }
    flight->cb = cb;
    flight->resident[0] = ib;
    a840_rt_buffer_retain(ib);
    for (size_t i = 0; i < count; ++i) {
        flight->resident[i+1] = resident[i];
        a840_rt_buffer_retain(resident[i]);
    }
    flight->count = count+1;
    return A840_RT_OK;
}

A840RtStatus a840_rt_gmem_retire(A840RtGmemFlight *flight,uint64_t timeout_ns)
{
    if (!flight || !flight->cb) return A840_RT_STATE_ERROR;
    A840RtStatus st = a840_rt_cb_wait_until_completed(flight->cb,timeout_ns);
    if (st != A840_RT_OK) return st;
    a840_rt_cb_release(flight->cb);
    for (size_t i = 0; i < flight->count; ++i) a840_rt_buffer_release(flight->resident[i]);
    memset(flight,0,sizeof(*flight));
    return A840_RT_OK;
}
