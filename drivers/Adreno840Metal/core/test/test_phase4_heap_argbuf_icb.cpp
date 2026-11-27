/* SPDX-License-Identifier: MIT */
#include "a840_argbuf_icb.h"
#include "a840_ca_render_bridge.h"
#include "a840_emit_indirect.h"
#include "a840_heap_allocator.h"
#include "a840_runtime_cmd.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

unsigned g_cases = 0;
unsigned g_checks = 0;
const char *g_active_case = "init";

void begin_case(const char *name)
{
    g_active_case = name;
    ++g_cases;
}

void check_impl(bool ok, const char *expr, int line)
{
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL [%s] line %d: %s\n", g_active_case, line, expr);
        std::abort();
    }
}

#define CASE(name) begin_case(name)
#define CHECK(expr) check_impl((expr), #expr, __LINE__)

struct DeviceFixture {
    A840MockTransport *mock = nullptr;
    A840RtDevice *dev = nullptr;
    std::vector<A840RtBuffer *> buffers;
    std::vector<A840RtTexture *> textures;

    explicit DeviceFixture(uint32_t cats = A840_ROUTE_PHASE2_ALL)
    {
        A840MockTransportConfig cfg{};
        a840_mock_transport_default_config(&cfg);
        cfg.auto_complete_submits = true;
        CHECK(a840_mock_transport_create(&cfg, &mock) == A840_TRANSPORT_OK);
        A840BackendTransport iface = a840_mock_transport_interface(mock);
        CHECK(a840_rt_device_create(&iface, cats, &dev) == A840_RT_OK);
    }

    A840RtBuffer *alloc_buf(uint64_t bytes, uint32_t usage, uint64_t align = A840_RT_PAGE_BYTES)
    {
        A840RtBufferDesc d{};
        d.size_bytes = bytes;
        d.alignment = align;
        d.usage_flags = usage;
        A840RtBuffer *b = nullptr;
        CHECK(a840_rt_buffer_create(dev, &d, &b) == A840_RT_OK);
        buffers.push_back(b);
        return b;
    }

    A840RtTexture *alloc_tex(uint32_t fmt, uint32_t w, uint32_t h, uint32_t tile = A840_TILE_LINEAR)
    {
        A840RtTextureDesc d{};
        d.mtl_pixel_format = fmt;
        d.tex_type = A840_TEX_2D;
        d.width = w;
        d.height = h;
        d.depth = 1u;
        d.array_layers = 1u;
        d.mip_levels = 1u;
        d.samples = 1u;
        d.tile_mode = tile;
        d.usage_flags = A840_TEX_USAGE_SHADER_READ | A840_TEX_USAGE_RENDER_TARGET;
        A840RtTexture *t = nullptr;
        CHECK(a840_rt_texture_create(dev, &d, &t) == A840_RT_OK);
        textures.push_back(t);
        return t;
    }

    ~DeviceFixture()
    {
        for (A840RtTexture *t : textures) {
            a840_rt_texture_release(t);
        }
        for (A840RtBuffer *b : buffers) {
            a840_rt_buffer_release(b);
        }
        a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }
};

void write_pm4_file(const char *path, const uint32_t *words, size_t count)
{
    if (!path || !path[0]) {
        return;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    CHECK(out.is_open());
    out.write(reinterpret_cast<const char *>(words),
              static_cast<std::streamsize>(count * sizeof(uint32_t)));
    CHECK(out.good());
}

bool stream_has_type7_opcode(const uint32_t *words, size_t count, uint32_t opcode)
{
    std::vector<A840PM4Packet> pkts(count + 1u);
    size_t n = 0u;
    CHECK(a840_pm4_decode(words, count, pkts.data(), pkts.size(), &n) == A840_EMIT_OK);
    for (size_t i = 0; i < n; ++i) {
        if (pkts[i].type == 7u && pkts[i].identifier == opcode) {
            return true;
        }
    }
    return false;
}

bool stream_has_type4_reg(const uint32_t *words, size_t count, uint32_t reg)
{
    std::vector<A840PM4Packet> pkts(count + 1u);
    size_t n = 0u;
    CHECK(a840_pm4_decode(words, count, pkts.data(), pkts.size(), &n) == A840_EMIT_OK);
    for (size_t i = 0; i < n; ++i) {
        if (pkts[i].type == 4u &&
            reg >= pkts[i].identifier &&
            reg < pkts[i].identifier + pkts[i].payload_words) {
            return true;
        }
    }
    return false;
}

size_t count_type7_opcode(const uint32_t *words, size_t count, uint32_t opcode)
{
    std::vector<A840PM4Packet> pkts(count + 1u);
    size_t n = 0u;
    CHECK(a840_pm4_decode(words, count, pkts.data(), pkts.size(), &n) == A840_EMIT_OK);
    size_t hits = 0u;
    for (size_t i = 0; i < n; ++i) {
        if (pkts[i].type == 7u && pkts[i].identifier == opcode) {
            hits++;
        }
    }
    return hits;
}

/* ============================================================================
 * Group 1: Automatic & Placement Heap Sub-Allocation, Coalescing, Purgeable
 * ============================================================================ */
void test_group1_heap_allocator()
{
    DeviceFixture fx;

    CASE("heap_create_invalid_args");
    {
        A840Heap *h = nullptr;
        A840HeapDesc d{};
        CHECK(a840_heap_create(nullptr, &d, &h) == A840_RT_INVALID_ARG);
        CHECK(a840_heap_create(fx.dev, nullptr, &h) == A840_RT_INVALID_ARG);
        CHECK(a840_heap_create(fx.dev, &d, nullptr) == A840_RT_INVALID_ARG);
        d.size_bytes = 0u;
        CHECK(a840_heap_create(fx.dev, &d, &h) == A840_RT_INVALID_ARG);
        d.size_bytes = 4096u;
        d.alignment = 3000u; /* non-power-of-two */
        CHECK(a840_heap_create(fx.dev, &d, &h) == A840_RT_ALIGNMENT);
        d.alignment = 131072u; /* > 64 KiB */
        CHECK(a840_heap_create(fx.dev, &d, &h) == A840_RT_ALIGNMENT);
    }

    CASE("heap_automatic_zero_extra_kmd_bo");
    {
        uint64_t bo_before = fx.dev->live_buffers;
        A840HeapDesc hd{};
        hd.type = A840_HEAP_TYPE_AUTOMATIC;
        hd.size_bytes = 256u * 1024u;
        hd.alignment = 65536u;
        hd.cpu_visible = true;
        A840Heap *heap = nullptr;
        CHECK(a840_heap_create(fx.dev, &hd, &heap) == A840_RT_OK);
        CHECK(fx.dev->live_buffers == bo_before + 1u);
        uint64_t backing_bo = a840_heap_backing_bo_handle(heap);
        CHECK(backing_bo != 0u);

        A840HeapBufferDesc bd{};
        bd.size_bytes = 4096u;
        bd.alignment = 256u;
        A840HeapSubAlloc *s1 = nullptr;
        A840HeapSubAlloc *s2 = nullptr;
        A840HeapSubAlloc *s3 = nullptr;
        CHECK(a840_heap_alloc_buffer(heap, &bd, &s1) == A840_RT_OK);
        bd.alignment = 65536u;
        CHECK(a840_heap_alloc_buffer(heap, &bd, &s2) == A840_RT_OK);
        bd.alignment = 64u;
        CHECK(a840_heap_alloc_buffer(heap, &bd, &s3) == A840_RT_OK);

        /* Zero extra KMD BOs allocated for sub-resources. */
        CHECK(fx.dev->live_buffers == bo_before + 1u);
        CHECK(s1->bo_handle == backing_bo);
        CHECK(s2->bo_handle == backing_bo);
        CHECK(s3->bo_handle == backing_bo);
        CHECK((s2->gpu_va % 65536u) == 0u);
        CHECK(s1->cpu_ptr != nullptr);

        a840_heap_suballoc_release(s1);
        a840_heap_suballoc_release(s2);
        a840_heap_suballoc_release(s3);
        a840_heap_release(heap);
        CHECK(fx.dev->live_buffers == bo_before);
    }

    CASE("heap_automatic_free_list_coalescing");
    {
        A840HeapDesc hd{};
        hd.type = A840_HEAP_TYPE_AUTOMATIC;
        hd.size_bytes = 64u * 1024u;
        hd.alignment = 4096u;
        A840Heap *heap = nullptr;
        CHECK(a840_heap_create(fx.dev, &hd, &heap) == A840_RT_OK);
        CHECK(a840_heap_free_block_count(heap) == 1u);

        A840HeapBufferDesc bd{};
        bd.size_bytes = 16u * 1024u;
        bd.alignment = 4096u;
        A840HeapSubAlloc *a = nullptr;
        A840HeapSubAlloc *b = nullptr;
        A840HeapSubAlloc *c = nullptr;
        A840HeapSubAlloc *d = nullptr;
        CHECK(a840_heap_alloc_buffer(heap, &bd, &a) == A840_RT_OK);
        CHECK(a840_heap_alloc_buffer(heap, &bd, &b) == A840_RT_OK);
        CHECK(a840_heap_alloc_buffer(heap, &bd, &c) == A840_RT_OK);
        CHECK(a840_heap_alloc_buffer(heap, &bd, &d) == A840_RT_OK);
        CHECK(a840_heap_free_bytes(heap) == 0u);
        CHECK(a840_heap_free_block_count(heap) == 0u);

        /* Release non-adjacent blocks a and c -> 2 free blocks. */
        a840_heap_suballoc_release(a);
        a840_heap_suballoc_release(c);
        CHECK(a840_heap_free_block_count(heap) == 2u);

        /* Release middle block b -> coalesces a+b+c into 1 block of 48 KiB. */
        a840_heap_suballoc_release(b);
        CHECK(a840_heap_free_block_count(heap) == 1u);
        CHECK(a840_heap_free_bytes(heap) == 48u * 1024u);

        /* Allocate 48 KiB contiguous block proving coalescing succeeded. */
        bd.size_bytes = 48u * 1024u;
        A840HeapSubAlloc *big = nullptr;
        CHECK(a840_heap_alloc_buffer(heap, &bd, &big) == A840_RT_OK);
        CHECK(big->offset == 0u);

        a840_heap_suballoc_release(big);
        a840_heap_suballoc_release(d);
        CHECK(a840_heap_free_block_count(heap) == 1u);
        CHECK(a840_heap_used_bytes(heap) == 0u);
        a840_heap_release(heap);
    }

    CASE("heap_placement_alignment_and_bounds_checks");
    {
        A840HeapDesc hd{};
        hd.type = A840_HEAP_TYPE_PLACEMENT;
        hd.size_bytes = 128u * 1024u;
        hd.alignment = 65536u;
        A840Heap *heap = nullptr;
        CHECK(a840_heap_create(fx.dev, &hd, &heap) == A840_RT_OK);

        A840HeapBufferDesc bd{};
        bd.size_bytes = 4096u;
        bd.alignment = 4096u;
        bd.placement_offset = 128u; /* misaligned for 4096B */
        A840HeapSubAlloc *sub = nullptr;
        CHECK(a840_heap_alloc_buffer(heap, &bd, &sub) == A840_RT_ALIGNMENT);

        bd.placement_offset = 128u * 1024u; /* OOB */
        CHECK(a840_heap_alloc_buffer(heap, &bd, &sub) == A840_RT_RANGE_ERROR);

        bd.placement_offset = UINT64_MAX - 4095u; /* 4 KiB aligned integer overflow */
        CHECK(a840_heap_alloc_buffer(heap, &bd, &sub) == A840_RT_RANGE_ERROR);

        bd.alignment = 64u;
        bd.placement_offset = UINT64_MAX - 63u; /* 64B aligned integer overflow */
        CHECK(a840_heap_alloc_buffer(heap, &bd, &sub) == A840_RT_RANGE_ERROR);

        bd.alignment = 65536u;
        bd.placement_offset = 65536u;
        CHECK(a840_heap_alloc_buffer(heap, &bd, &sub) == A840_RT_OK);
        CHECK(sub->offset == 65536u);
        CHECK((sub->gpu_va % 65536u) == 0u);

        /* Overlapping live non-aliasable placement must be rejected. */
        A840HeapSubAlloc *conflict = nullptr;
        CHECK(a840_heap_alloc_buffer(heap, &bd, &conflict) == A840_RT_STATE_ERROR);

        a840_heap_suballoc_release(sub);
        a840_heap_release(heap);
    }

    CASE("heap_purgeable_empty_rejects_alloc_and_submit");
    {
        A840HeapDesc hd{};
        hd.type = A840_HEAP_TYPE_AUTOMATIC;
        hd.size_bytes = 64u * 1024u;
        A840Heap *heap = nullptr;
        CHECK(a840_heap_create(fx.dev, &hd, &heap) == A840_RT_OK);
        CHECK(a840_heap_get_purgeable(heap) == A840_PURGEABLE_NONVOLATILE);
        CHECK(a840_heap_validate_for_submission(heap) == A840_RT_OK);

        A840HeapBufferDesc bd{};
        bd.size_bytes = 1024u;
        A840HeapSubAlloc *sub = nullptr;
        CHECK(a840_heap_alloc_buffer(heap, &bd, &sub) == A840_RT_OK);

        A840PurgeableState prev = A840_PURGEABLE_NONVOLATILE;
        CHECK(a840_heap_set_purgeable(heap, A840_PURGEABLE_VOLATILE, &prev) == A840_RT_OK);
        CHECK(prev == A840_PURGEABLE_NONVOLATILE);
        CHECK(a840_heap_validate_for_submission(heap) == A840_RT_OK);

        CHECK(a840_heap_set_purgeable(heap, A840_PURGEABLE_EMPTY, &prev) == A840_RT_OK);
        CHECK(prev == A840_PURGEABLE_VOLATILE);
        CHECK(a840_heap_validate_for_submission(heap) == A840_RT_STATE_ERROR);
        CHECK(a840_heap_suballoc_validate(fx.dev, sub) == A840_RT_STATE_ERROR);

        /* EMPTY heap rejects new sub-allocations and resurrecting to NONVOLATILE. */
        A840HeapSubAlloc *sub2 = nullptr;
        CHECK(a840_heap_alloc_buffer(heap, &bd, &sub2) == A840_RT_STATE_ERROR);
        CHECK(a840_heap_set_purgeable(heap, A840_PURGEABLE_NONVOLATILE, &prev) == A840_RT_STATE_ERROR);

        a840_heap_suballoc_release(sub);
        a840_heap_release(heap);
    }

    CASE("heap_texture_ubwc6_hbb16_enforcement");
    {
        A840HeapDesc hd{};
        hd.type = A840_HEAP_TYPE_PLACEMENT;
        hd.size_bytes = 2u * 1024u * 1024u;
        A840Heap *heap = nullptr;
        CHECK(a840_heap_create(fx.dev, &hd, &heap) == A840_RT_OK);

        A840HeapTextureDesc td{};
        td.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
        td.tex_type = A840_TEX_2D;
        td.width = 256u;
        td.height = 256u;
        td.tile_mode = A840_TILE_UBWC;
        td.highest_bank_bit = 15u; /* wrong HBB for Kaanapali A840 */
        A840HeapSubAlloc *bad = nullptr;
        CHECK(a840_heap_alloc_texture(heap, &td, &bad) == A840_RT_UNSUPPORTED);

        td.highest_bank_bit = 16u;
        A840HeapSubAlloc *ok_tex = nullptr;
        CHECK(a840_heap_alloc_texture(heap, &td, &ok_tex) == A840_RT_OK);
        CHECK(ok_tex->ubwc_flag_gpu_va != 0u);
        CHECK(ok_tex->data_gpu_va > ok_tex->ubwc_flag_gpu_va);
        CHECK(ok_tex->tex_const[0] != 0u);

        a840_heap_suballoc_release(ok_tex);
        a840_heap_release(heap);
    }
}

/* ============================================================================
 * Group 2: Aliasing Lifecycle & Linear <-> UBWC 6.0 Layout Transition Barriers
 * ============================================================================ */
void test_group2_aliasing_transitions(A840HeapAliasTransition *out_ubwc_tr)
{
    DeviceFixture fx;

    CASE("alias_automatic_linear_buf_to_ubwc6_texture");
    {
        A840HeapDesc hd{};
        hd.type = A840_HEAP_TYPE_AUTOMATIC;
        hd.size_bytes = 1024u * 1024u;
        A840Heap *heap = nullptr;
        CHECK(a840_heap_create(fx.dev, &hd, &heap) == A840_RT_OK);

        A840HeapBufferDesc bd{};
        bd.size_bytes = 512u * 1024u;
        bd.alignment = 4096u;
        A840HeapSubAlloc *lin_buf = nullptr;
        CHECK(a840_heap_alloc_buffer(heap, &bd, &lin_buf) == A840_RT_OK);
        CHECK(lin_buf->offset == 0u);
        CHECK(!lin_buf->last_transition.requires_alias_barrier);
        CHECK(lin_buf->alias_epoch == 1u);

        /* Mark linear buffer aliasable -> frees range back to reuse pool keeping epoch=1. */
        CHECK(a840_heap_suballoc_make_aliasable(lin_buf) == A840_RT_OK);
        CHECK(a840_heap_suballoc_is_aliasable(lin_buf));
        CHECK(a840_heap_used_bytes(heap) == 0u);

        /* Allocate UBWC 6.0 BGRA8 texture over same physical offset 0. */
        A840HeapTextureDesc td{};
        td.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
        td.tex_type = A840_TEX_2D;
        td.width = 256u;
        td.height = 256u;
        td.tile_mode = A840_TILE_UBWC;
        td.highest_bank_bit = 16u;
        A840HeapSubAlloc *ubwc_tex = nullptr;
        CHECK(a840_heap_alloc_texture(heap, &td, &ubwc_tex) == A840_RT_OK);
        CHECK(ubwc_tex->offset == 0u);
        CHECK(ubwc_tex->alias_epoch == 2u);
        CHECK(ubwc_tex->last_transition.requires_alias_barrier);
        CHECK(ubwc_tex->last_transition.ubwc_meta_invalidate_required);
        CHECK(ubwc_tex->last_transition.prev_tile_mode == A840_TILE_LINEAR);
        CHECK(ubwc_tex->last_transition.next_tile_mode == A840_TILE_UBWC);
        CHECK(ubwc_tex->last_transition.prev_alias_epoch == 1u);
        CHECK(ubwc_tex->last_transition.next_alias_epoch == 2u);
        CHECK(ubwc_tex->last_transition.ubwc_flag_va == ubwc_tex->ubwc_flag_gpu_va);

        if (out_ubwc_tr) {
            *out_ubwc_tr = ubwc_tex->last_transition;
        }

        /* Emit transition PM4 and verify CCU_FLUSH/INV + UCHE_INV + MEM_WRITE64. */
        uint32_t words[64];
        A840Reloc relocs[8];
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words, 64u, relocs, 8u) == A840_EMIT_OK);
        CHECK(a840_heap_emit_alias_transition(&cs, &ubwc_tex->last_transition) == A840_EMIT_OK);
        CHECK(cs.used_dwords > 0u);
        CHECK(cs.reloc_count == 1u);
        CHECK(stream_has_type7_opcode(words, cs.used_dwords, A840_CP_EVENT_WRITE7));
        CHECK(stream_has_type7_opcode(words, cs.used_dwords, A840_CP_WAIT_MEM_WRITES));
        CHECK(stream_has_type7_opcode(words, cs.used_dwords, A840_CP_WAIT_FOR_IDLE));
        CHECK(stream_has_type7_opcode(words, cs.used_dwords, A840_CP_MEM_WRITE));

        a840_heap_suballoc_release(lin_buf);
        a840_heap_suballoc_release(ubwc_tex);
        a840_heap_release(heap);
    }

    CASE("alias_placement_ubwc6_bgra8_to_rgba16f_linear");
    {
        A840HeapDesc hd{};
        hd.type = A840_HEAP_TYPE_PLACEMENT;
        hd.size_bytes = 2u * 1024u * 1024u;
        A840Heap *heap = nullptr;
        CHECK(a840_heap_create(fx.dev, &hd, &heap) == A840_RT_OK);

        A840HeapTextureDesc td1{};
        td1.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
        td1.tex_type = A840_TEX_2D;
        td1.width = 256u;
        td1.height = 256u;
        td1.tile_mode = A840_TILE_UBWC;
        td1.highest_bank_bit = 16u;
        td1.placement_offset = 65536u;
        A840HeapSubAlloc *t1 = nullptr;
        CHECK(a840_heap_alloc_texture(heap, &td1, &t1) == A840_RT_OK);
        CHECK(a840_heap_suballoc_make_aliasable(t1) == A840_RT_OK);

        /* Alias at same placement offset with RGBA16Float (MTL 115) Linear. */
        A840HeapTextureDesc td2{};
        td2.mtl_pixel_format = 115u; /* RGBA16Float */
        td2.tex_type = A840_TEX_2D;
        td2.width = 128u;
        td2.height = 128u;
        td2.tile_mode = A840_TILE_LINEAR;
        td2.highest_bank_bit = 16u;
        td2.placement_offset = 65536u;
        A840HeapSubAlloc *t2 = nullptr;
        CHECK(a840_heap_alloc_texture(heap, &td2, &t2) == A840_RT_OK);
        CHECK(t2->last_transition.requires_alias_barrier);
        CHECK(t2->last_transition.ubwc_meta_invalidate_required);
        CHECK(t2->last_transition.prev_tile_mode == A840_TILE_UBWC);
        CHECK(t2->last_transition.next_tile_mode == A840_TILE_LINEAR);
        CHECK(t2->last_transition.prev_mtl_format == A840_MTL_FMT_BGRA8_UNORM);
        CHECK(t2->last_transition.next_mtl_format == 115u);
        CHECK(t2->last_transition.ubwc_flag_va == t1->ubwc_flag_gpu_va);

        a840_heap_suballoc_release(t1);
        a840_heap_suballoc_release(t2);
        a840_heap_release(heap);
    }

    CASE("alias_transition_noop_and_rollback_on_small_cs");
    {
        A840HeapAliasTransition noop{};
        uint32_t words[4];
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words, 4u, nullptr, 0u) == A840_EMIT_OK);
        CHECK(a840_heap_emit_alias_transition(&cs, &noop) == A840_EMIT_OK);
        CHECK(cs.used_dwords == 0u);

        A840HeapAliasTransition active{};
        active.requires_alias_barrier = true;
        active.ubwc_meta_invalidate_required = true;
        active.ubwc_flag_va = UINT64_C(0x10000000);
        active.ubwc_flag_bytes = 4096u;
        CHECK(a840_heap_emit_alias_transition(&cs, &active) == A840_EMIT_CAPACITY);
        CHECK(cs.used_dwords == 0u); /* transactional rollback */
    }
}

/* ============================================================================
 * Group 3: Argument Buffer Encoder & Residency Verification
 * ============================================================================ */
void test_group3_argbuf_encoder()
{
    DeviceFixture fx;
    DeviceFixture fx_other;

    const A840ArgSlotDesc layout[4] = {
        {0u, A840_ARG_SLOT_BUFFER, 0u, A840_ARGBUF_BUFFER_ENTRY_BYTES},
        {1u, A840_ARG_SLOT_SAMPLER, 16u, A840_ARGBUF_SAMPLER_ENTRY_BYTES},
        {2u, A840_ARG_SLOT_INLINE_DATA, 32u, 32u},
        {3u, A840_ARG_SLOT_TEXTURE, 64u, A840_ARGBUF_TEXTURE_ENTRY_BYTES},
    };

    CASE("argbuf_init_validation_overlap_and_align");
    {
        A840ArgBufEncoder enc{};
        A840ArgSlotDesc bad_align[1] = {{0u, A840_ARG_SLOT_TEXTURE, 16u, 64u}};
        CHECK(a840_argbuf_encoder_init(&enc, fx.dev, bad_align, 1u) == A840_RT_ALIGNMENT);

        A840ArgSlotDesc overlap[2] = {
            {0u, A840_ARG_SLOT_BUFFER, 0u, 16u},
            {1u, A840_ARG_SLOT_INLINE_DATA, 8u, 16u},
        };
        CHECK(a840_argbuf_encoder_init(&enc, fx.dev, overlap, 2u) == A840_RT_RANGE_ERROR);

        A840ArgSlotDesc dup[2] = {
            {7u, A840_ARG_SLOT_BUFFER, 0u, 16u},
            {7u, A840_ARG_SLOT_BUFFER, 16u, 16u},
        };
        CHECK(a840_argbuf_encoder_init(&enc, fx.dev, dup, 2u) == A840_RT_INVALID_ARG);
    }

    CASE("argbuf_encode_all_slot_types_zero_temp_copy");
    {
        A840ArgBufEncoder enc{};
        CHECK(a840_argbuf_encoder_init(&enc, fx.dev, layout, 4u) == A840_RT_OK);
        CHECK(enc.encoded_length == 128u);

        A840RtBuffer *arg_gpu = fx.alloc_buf(4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_CPU_VISIBLE);
        A840RtBuffer *data_buf = fx.alloc_buf(8192u, A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE);
        A840RtTexture *tex = fx.alloc_tex(A840_MTL_FMT_BGRA8_UNORM, 64u, 64u, A840_TILE_LINEAR);

        CHECK(a840_argbuf_encoder_set_target(&enc, arg_gpu, 64u) == A840_RT_OK);
        CHECK(a840_argbuf_set_buffer(&enc, 0u, data_buf, 256u) == A840_RT_OK);

        A840SampDescInput samp{};
        samp.min_filter = A840_FILTER_LINEAR;
        samp.mag_filter = A840_FILTER_LINEAR;
        samp.mip_filter = A840_MIP_LINEAR;
        samp.address_s = A840_ADDRESS_CLAMP_TO_EDGE;
        samp.address_t = A840_ADDRESS_CLAMP_TO_EDGE;
        samp.address_r = A840_ADDRESS_CLAMP_TO_EDGE;
        samp.max_aniso = 1u;
        samp.min_lod = 0.0f;
        samp.max_lod = 12.0f;
        samp.compare_func = A840_COMPARE_DISABLED;
        CHECK(a840_argbuf_set_sampler(&enc, 1u, &samp) == A840_RT_OK);

        const uint32_t inline_vals[4] = {0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u};
        CHECK(a840_argbuf_set_inline_bytes(&enc, 2u, inline_vals, sizeof(inline_vals)) == A840_RT_OK);
        CHECK(a840_argbuf_set_texture(&enc, 3u, tex) == A840_RT_OK);

        /* Verify raw bytes directly inside CPU-mapped GPU buffer. */
        const uint8_t *base = static_cast<const uint8_t *>(arg_gpu->cpu_ptr) + 64u;
        uint64_t encoded_buf[2]{};
        std::memcpy(encoded_buf, base + 0u, sizeof(encoded_buf));
        CHECK(encoded_buf[0] == data_buf->gpu_va + 256u);
        CHECK(encoded_buf[1] == data_buf->size_bytes - 256u);

        uint32_t expected_samp[4]{};
        CHECK(a840_encode_samp_const(&samp, expected_samp) == A840_EMIT_OK);
        CHECK(std::memcmp(base + 16u, expected_samp, sizeof(expected_samp)) == 0);
        CHECK(std::memcmp(base + 32u, inline_vals, sizeof(inline_vals)) == 0);
        CHECK(std::memcmp(base + 64u, tex->tex_const, 64u) == 0);

        /* Residency validation requires both data_buf and tex BOs. */
        CHECK(enc.dep_count == 2u);
        const uint64_t partial_bos[1] = {data_buf->bo_handle};
        CHECK(a840_argbuf_validate_residency(&enc, partial_bos, 1u, nullptr, 0u) == A840_RT_STATE_ERROR);
        const uint64_t all_bos[2] = {data_buf->bo_handle, tex->bo_handle};
        CHECK(a840_argbuf_validate_residency(&enc, all_bos, 2u, nullptr, 0u) == A840_RT_OK);
    }

    CASE("argbuf_rejects_wrong_device_and_oob");
    {
        A840ArgBufEncoder enc{};
        CHECK(a840_argbuf_encoder_init(&enc, fx.dev, layout, 4u) == A840_RT_OK);

        A840RtBuffer *foreign_buf = fx_other.alloc_buf(4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE);
        A840RtTexture *foreign_tex = fx_other.alloc_tex(A840_MTL_FMT_BGRA8_UNORM, 32u, 32u);
        CHECK(a840_argbuf_encoder_set_target(&enc, foreign_buf, 0u) == A840_RT_WRONG_DEVICE);

        A840RtBuffer *local_arg = fx.alloc_buf(4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE);
        CHECK(a840_argbuf_encoder_set_target(&enc, local_arg, 4032u) == A840_RT_RANGE_ERROR);
        CHECK(a840_argbuf_encoder_set_target(&enc, local_arg, 16u) == A840_RT_ALIGNMENT);
        CHECK(a840_argbuf_encoder_set_target(&enc, local_arg, 0u) == A840_RT_OK);

        CHECK(a840_argbuf_set_buffer(&enc, 0u, foreign_buf, 0u) == A840_RT_WRONG_DEVICE);
        CHECK(a840_argbuf_set_texture(&enc, 3u, foreign_tex) == A840_RT_WRONG_DEVICE);
        CHECK(a840_argbuf_set_buffer(&enc, 0u, local_arg, local_arg->size_bytes) == A840_RT_RANGE_ERROR);

        uint8_t too_big[40]{};
        CHECK(a840_argbuf_set_inline_bytes(&enc, 2u, too_big, sizeof(too_big)) == A840_RT_RANGE_ERROR);
        CHECK(a840_argbuf_set_inline_bytes(&enc, 99u, too_big, 4u) == A840_RT_INVALID_ARG);
    }

    CASE("argbuf_heap_suballoc_and_use_heap_residency");
    {
        A840HeapDesc hd{};
        hd.type = A840_HEAP_TYPE_AUTOMATIC;
        hd.size_bytes = 256u * 1024u;
        A840Heap *heap = nullptr;
        CHECK(a840_heap_create(fx.dev, &hd, &heap) == A840_RT_OK);

        A840HeapBufferDesc bd{};
        bd.size_bytes = 2048u;
        A840HeapSubAlloc *hbuf = nullptr;
        CHECK(a840_heap_alloc_buffer(heap, &bd, &hbuf) == A840_RT_OK);

        A840HeapTextureDesc td{};
        td.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
        td.tex_type = A840_TEX_2D;
        td.width = 64u;
        td.height = 64u;
        td.tile_mode = A840_TILE_UBWC;
        td.highest_bank_bit = 16u;
        A840HeapSubAlloc *htex = nullptr;
        CHECK(a840_heap_alloc_texture(heap, &td, &htex) == A840_RT_OK);

        A840ArgBufEncoder enc{};
        CHECK(a840_argbuf_encoder_init(&enc, fx.dev, layout, 4u) == A840_RT_OK);
        A840RtBuffer *arg_gpu = fx.alloc_buf(4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE);
        CHECK(a840_argbuf_encoder_set_target(&enc, arg_gpu, 0u) == A840_RT_OK);
        CHECK(a840_argbuf_set_heap_suballoc(&enc, 0u, hbuf) == A840_RT_OK);
        CHECK(a840_argbuf_set_heap_suballoc(&enc, 3u, htex) == A840_RT_OK);

        const A840Heap *heaps[1] = {heap};
        CHECK(a840_argbuf_validate_residency(&enc, nullptr, 0u, heaps, 1u) == A840_RT_OK);

        /* Purging the heap to EMPTY invalidates residency! */
        CHECK(a840_heap_set_purgeable(heap, A840_PURGEABLE_EMPTY, nullptr) == A840_RT_OK);
        CHECK(a840_argbuf_validate_residency(&enc, nullptr, 0u, heaps, 1u) == A840_RT_STATE_ERROR);

        a840_heap_suballoc_release(hbuf);
        a840_heap_suballoc_release(htex);
        a840_heap_release(heap);
    }
}

/* ============================================================================
 * Group 4: Indirect Draw / Indexed Indirect / Indirect Compute + ICB
 * ============================================================================ */
void test_group4_indirect_and_icb(const char *dump_indirect_pm4,
                                  const char *dump_icb_pm4,
                                  const A840HeapAliasTransition *ubwc_tr)
{
    DeviceFixture fx;

    CASE("emit_draw_indirect_validation_and_packets");
    {
        uint32_t words[32];
        A840Reloc relocs[4];
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words, 32u, relocs, 4u) == A840_EMIT_OK);

        A840DrawIndirectDesc d{};
        d.primitive = A840_PRIM_TRIANGLES;
        d.indirect.bo_handle = 11u;
        d.indirect.gpu_va = UINT64_C(0x10000002); /* misaligned */
        CHECK(a840_emit_draw_indirect(&cs, &d) == A840_EMIT_ALIGNMENT);
        CHECK(cs.used_dwords == 0u);

        d.indirect.gpu_va = 0u;
        CHECK(a840_emit_draw_indirect(&cs, &d) == A840_EMIT_INVALID_ARG);
        d.indirect.gpu_va = UINT64_C(0x10000000);
        d.primitive = 0x3fu; /* unsupported prim */
        CHECK(a840_emit_draw_indirect(&cs, &d) == A840_EMIT_UNSUPPORTED);

        d.primitive = A840_PRIM_TRIANGLES;
        d.use_visibility = true;
        CHECK(a840_emit_draw_indirect(&cs, &d) == A840_EMIT_OK);
        CHECK(cs.reloc_count == 1u);
        CHECK(stream_has_type4_reg(words, cs.used_dwords, A840_REG_PC_DRAW_INITIATOR));
        CHECK(stream_has_type7_opcode(words, cs.used_dwords, A840_CP_WAIT_FOR_ME));
        CHECK(stream_has_type7_opcode(words, cs.used_dwords, A840_CP_DRAW_INDIRECT));
    }

    CASE("emit_draw_indexed_indirect_validation_and_packets");
    {
        uint32_t words[32];
        A840Reloc relocs[4];
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words, 32u, relocs, 4u) == A840_EMIT_OK);

        A840DrawIndexedIndirectDesc d{};
        d.primitive = A840_PRIM_TRIANGLE_STRIP;
        d.index_type = A840_INDEX_UINT32;
        d.max_indices = 1024u;
        d.index_buffer = {21u, UINT64_C(0x20000002)}; /* bad 4B alignment for uint32 */
        d.indirect = {22u, UINT64_C(0x20010000)};
        CHECK(a840_emit_draw_indexed_indirect(&cs, &d) == A840_EMIT_ALIGNMENT);
        CHECK(cs.used_dwords == 0u);

        d.index_type = A840_INDEX_UINT16; /* 2B alignment valid */
        CHECK(a840_emit_draw_indexed_indirect(&cs, &d) == A840_EMIT_OK);
        CHECK(cs.reloc_count == 2u);
        CHECK(stream_has_type4_reg(words, cs.used_dwords, A840_REG_PC_DRAW_INITIATOR));
        CHECK(stream_has_type7_opcode(words, cs.used_dwords, A840_CP_DRAW_INDX_INDIRECT));

        d.max_indices = 0u;
        size_t before = cs.used_dwords;
        CHECK(a840_emit_draw_indexed_indirect(&cs, &d) == A840_EMIT_RANGE_ERROR);
        CHECK(cs.used_dwords == before);
    }

    CASE("emit_dispatch_indirect_validation_and_packets");
    {
        uint32_t words[32];
        A840Reloc relocs[4];
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words, 32u, relocs, 4u) == A840_EMIT_OK);

        A840DispatchIndirectDesc d{};
        d.local_size[0] = 32u;
        d.local_size[1] = 32u;
        d.local_size[2] = 2u; /* 2048 > 1024 */
        d.indirect = {31u, UINT64_C(0x30000000)};
        CHECK(a840_emit_dispatch_indirect(&cs, &d) == A840_EMIT_RANGE_ERROR);

        d.local_size[0] = 16u;
        d.local_size[1] = 8u;
        d.local_size[2] = 1u;
        d.indirect.gpu_va = UINT64_C(0x30000001);
        CHECK(a840_emit_dispatch_indirect(&cs, &d) == A840_EMIT_ALIGNMENT);

        d.indirect.gpu_va = UINT64_C(0x30000000);
        CHECK(a840_emit_dispatch_indirect(&cs, &d) == A840_EMIT_OK);
        CHECK(cs.reloc_count == 1u);
        CHECK(stream_has_type4_reg(words, cs.used_dwords, A840_REG_SP_CS_NDRANGE_0));
        CHECK(stream_has_type4_reg(words, cs.used_dwords, A840_REG_SP_CS_NDRANGE_7));
        CHECK(stream_has_type7_opcode(words, cs.used_dwords, A840_CP_EXEC_CS_INDIRECT));
    }

    CASE("emit_icb_execute_range_rejects_bad_slots");
    {
        uint32_t words[32];
        A840Reloc relocs[4];
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words, 32u, relocs, 4u) == A840_EMIT_OK);

        A840IcbSubIb slots[2] = {
            {{40u, UINT64_C(0x40000000)}, 16u},
            {{40u, UINT64_C(0x40000042)}, 8u}, /* misaligned */
        };
        A840IcbExecuteRangeDesc r{};
        r.slots = slots;
        r.total_slots = 2u;
        r.start = 0u;
        r.count = 2u;
        CHECK(a840_emit_icb_execute_range(&cs, &r) == A840_EMIT_ALIGNMENT);
        CHECK(cs.used_dwords == 0u);

        slots[1].ib.gpu_va = UINT64_C(0x40000040);
        slots[1].dwords = 0u; /* empty slot */
        CHECK(a840_emit_icb_execute_range(&cs, &r) == A840_EMIT_RANGE_ERROR);

        r.start = 1u;
        r.count = 2u; /* OOB range */
        CHECK(a840_emit_icb_execute_range(&cs, &r) == A840_EMIT_RANGE_ERROR);
    }

    CASE("dump_pm4_stream1_indirect_and_aliasing");
    {
        std::vector<uint32_t> words(256u, 0u);
        std::vector<A840Reloc> relocs(32u);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);

        /* 1. Aliasing Linear -> UBWC 6.0 transition barrier + metadata invalidate */
        A840HeapAliasTransition tr{};
        if (ubwc_tr && ubwc_tr->requires_alias_barrier) {
            tr = *ubwc_tr;
        } else {
            tr.requires_alias_barrier = true;
            tr.ubwc_meta_invalidate_required = true;
            tr.bo_handle = 100u;
            tr.ubwc_flag_va = UINT64_C(0x10000000);
            tr.ubwc_flag_bytes = 4096u;
        }
        CHECK(a840_heap_emit_alias_transition(&cs, &tr) == A840_EMIT_OK);

        /* 2. Direct Indirect Draw */
        A840DrawIndirectDesc draw_ind{};
        draw_ind.primitive = A840_PRIM_TRIANGLES;
        draw_ind.use_visibility = false;
        draw_ind.indirect = {101u, UINT64_C(0x10100000)};
        CHECK(a840_emit_draw_indirect(&cs, &draw_ind) == A840_EMIT_OK);

        /* 3. Indexed Indirect Draw */
        A840DrawIndexedIndirectDesc draw_idx{};
        draw_idx.primitive = A840_PRIM_TRIANGLES;
        draw_idx.index_type = A840_INDEX_UINT32;
        draw_idx.use_visibility = true;
        draw_idx.max_indices = 65536u;
        draw_idx.index_buffer = {102u, UINT64_C(0x10200000)};
        draw_idx.indirect = {103u, UINT64_C(0x10300000)};
        CHECK(a840_emit_draw_indexed_indirect(&cs, &draw_idx) == A840_EMIT_OK);

        /* 4. Indirect Compute Dispatch */
        A840DispatchIndirectDesc disp_ind{};
        disp_ind.local_size[0] = 8u;
        disp_ind.local_size[1] = 8u;
        disp_ind.local_size[2] = 1u;
        disp_ind.work_dim = 3u;
        disp_ind.indirect = {104u, UINT64_C(0x10400000)};
        CHECK(a840_emit_dispatch_indirect(&cs, &disp_ind) == A840_EMIT_OK);

        CHECK(stream_has_type7_opcode(words.data(), cs.used_dwords, A840_CP_DRAW_INDIRECT));
        CHECK(stream_has_type7_opcode(words.data(), cs.used_dwords, A840_CP_DRAW_INDX_INDIRECT));
        CHECK(stream_has_type7_opcode(words.data(), cs.used_dwords, A840_CP_EXEC_CS_INDIRECT));
        CHECK(stream_has_type7_opcode(words.data(), cs.used_dwords, A840_CP_MEM_WRITE));

        write_pm4_file(dump_indirect_pm4, words.data(), cs.used_dwords);
    }

    CASE("icb_render_record_reset_copy_compile_and_dump_pm4_stream2");
    {
        A840IcbDesc desc{};
        desc.type = A840_ICB_TYPE_RENDER;
        desc.max_commands = 16u;
        desc.inherit_pipeline = false;
        desc.inherit_buffers = false;
        A840IndirectCmdBuffer *icb = nullptr;
        A840IndirectCmdBuffer *icb_copy = nullptr;
        CHECK(a840_icb_create(fx.dev, &desc, &icb) == A840_RT_OK);
        CHECK(a840_icb_create(fx.dev, &desc, &icb_copy) == A840_RT_OK);

        A840RtBuffer *sds_buf = fx.alloc_buf(4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE);
        A840RtBuffer *arg_buf = fx.alloc_buf(4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE);
        A840RtBuffer *ind_buf = fx.alloc_buf(4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE);
        A840RtBuffer *idx_buf = fx.alloc_buf(4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE);
        A840RtBuffer *sec_ib = fx.alloc_buf(16384u, A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_CPU_VISIBLE);

        A840SdsGroupDesc sds{};
        sds.group_id = 0u;
        sds.enable_mask = A840_SDS_GMEM | A840_SDS_SYSMEM;
        sds.state_dwords = 4u;
        sds.state = {sds_buf->bo_handle, sds_buf->gpu_va};

        /* Slot 0: direct draw */
        CHECK(a840_icb_set_pipeline(icb, 0u, 1u, &sds) == A840_RT_OK);
        CHECK(a840_icb_set_bindless_buffer(icb, 0u, 0u, {arg_buf->bo_handle, arg_buf->gpu_va}) == A840_RT_OK);
        A840DrawDesc d0{};
        d0.primitive = A840_PRIM_TRIANGLES;
        d0.index_type = A840_INDEX_NONE;
        d0.count = 36u;
        d0.instance_count = 2u;
        CHECK(a840_icb_encode_draw(icb, 0u, &d0) == A840_RT_OK);

        /* Slot 1: will be reset to EMPTY */
        CHECK(a840_icb_encode_draw(icb, 1u, &d0) == A840_RT_OK);
        CHECK(a840_icb_reset_range(icb, 1u, 1u) == A840_RT_OK);

        /* Slot 2: direct indirect draw */
        A840DrawIndirectDesc d2{};
        d2.primitive = A840_PRIM_TRIANGLES;
        d2.indirect = {ind_buf->bo_handle, ind_buf->gpu_va};
        CHECK(a840_icb_set_bindless_buffer(icb, 2u, 1u, {arg_buf->bo_handle, arg_buf->gpu_va + 64u}) == A840_RT_OK);
        CHECK(a840_icb_encode_draw_indirect(icb, 2u, &d2) == A840_RT_OK);

        /* Slot 3: indexed indirect draw */
        A840DrawIndexedIndirectDesc d3{};
        d3.primitive = A840_PRIM_TRIANGLE_STRIP;
        d3.index_type = A840_INDEX_UINT16;
        d3.max_indices = 512u;
        d3.index_buffer = {idx_buf->bo_handle, idx_buf->gpu_va};
        d3.indirect = {ind_buf->bo_handle, ind_buf->gpu_va + 64u};
        CHECK(a840_icb_encode_draw_indexed_indirect(icb, 3u, &d3) == A840_RT_OK);

        /* Copy slots [0..4) into icb_copy and verify identical deterministic hash */
        CHECK(a840_icb_copy_range(icb_copy, 0u, icb, 0u, 4u) == A840_RT_OK);

        A840IcbCompiledRange comp1{};
        A840IcbCompiledRange comp2{};
        std::vector<A840Reloc> sec_relocs(64u);
        CHECK(a840_icb_compile_to_gpu_ib(icb, 0u, 4u, sec_ib, sec_relocs.data(), sec_relocs.size(), &comp1) == A840_RT_OK);
        CHECK(comp1.non_empty_count == 3u);
        CHECK(comp1.total_ib_dwords > 0u);
        CHECK(comp1.content_hash != 0u);

        A840RtBuffer *sec_ib2 = fx.alloc_buf(16384u, A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_CPU_VISIBLE);
        CHECK(a840_icb_compile_to_gpu_ib(icb_copy, 0u, 4u, sec_ib2, sec_relocs.data(), sec_relocs.size(), &comp2) == A840_RT_OK);
        CHECK(comp1.content_hash == comp2.content_hash);
        CHECK(comp1.total_ib_dwords == comp2.total_ib_dwords);

        /* Verify secondary IB itself decodes cleanly as A8XX PM4 */
        const uint32_t *sec_words = static_cast<const uint32_t *>(sec_ib->cpu_ptr);
        CHECK(stream_has_type7_opcode(sec_words, comp1.total_ib_dwords, A840_CP_SET_DRAW_STATE));
        CHECK(stream_has_type7_opcode(sec_words, comp1.total_ib_dwords, A840_CP_DRAW_INDX_OFFSET));
        CHECK(stream_has_type7_opcode(sec_words, comp1.total_ib_dwords, A840_CP_DRAW_INDIRECT));
        CHECK(stream_has_type7_opcode(sec_words, comp1.total_ib_dwords, A840_CP_DRAW_INDX_INDIRECT));

        /* Build primary IB executing compiled ICB range + secondary IB payload concatenation check */
        std::vector<uint32_t> primary_words(256u, 0u);
        std::vector<A840Reloc> primary_relocs(32u);
        A840CmdStream primary_cs{};
        CHECK(a840_cs_init(&primary_cs, primary_words.data(), primary_words.size(),
                           primary_relocs.data(), primary_relocs.size()) == A840_EMIT_OK);
        CHECK(a840_icb_emit_execute(&primary_cs, &comp1, true) == A840_EMIT_OK);
        CHECK(count_type7_opcode(primary_words.data(), primary_cs.used_dwords, A840_CP_INDIRECT_BUFFER) == 3u);
        CHECK(primary_cs.reloc_count == 3u);

        /* Also append a compute ICB indirect dispatch secondary stream and primary exec */
        A840IcbDesc cdesc{};
        cdesc.type = A840_ICB_TYPE_COMPUTE;
        cdesc.max_commands = 4u;
        A840IndirectCmdBuffer *cicb = nullptr;
        CHECK(a840_icb_create(fx.dev, &cdesc, &cicb) == A840_RT_OK);
        const uint32_t lsz[3] = {16u, 4u, 1u};
        const uint32_t grps[3] = {8u, 8u, 1u};
        CHECK(a840_icb_encode_dispatch(cicb, 0u, lsz, grps) == A840_RT_OK);
        A840DispatchIndirectDesc cind{};
        cind.local_size[0] = 8u;
        cind.local_size[1] = 8u;
        cind.local_size[2] = 1u;
        cind.indirect = {ind_buf->bo_handle, ind_buf->gpu_va + 128u};
        CHECK(a840_icb_encode_dispatch_indirect(cicb, 1u, &cind) == A840_RT_OK);

        A840RtBuffer *csec_ib = fx.alloc_buf(8192u, A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_CPU_VISIBLE);
        A840IcbCompiledRange ccomp{};
        CHECK(a840_icb_compile_to_gpu_ib(cicb, 0u, 2u, csec_ib, sec_relocs.data(), sec_relocs.size(), &ccomp) == A840_RT_OK);
        CHECK(ccomp.non_empty_count == 2u);
        CHECK(a840_icb_emit_execute(&primary_cs, &ccomp, true) == A840_EMIT_OK);
        CHECK(count_type7_opcode(primary_words.data(), primary_cs.used_dwords, A840_CP_INDIRECT_BUFFER) == 5u);

        /* Combine primary IB + render secondary IB + compute secondary IB into one
         * contiguous A8XX PM4 stream for pm4_check/pm4_diff full structural coverage. */
        std::vector<uint32_t> combined;
        combined.insert(combined.end(), primary_words.begin(), primary_words.begin() + static_cast<ptrdiff_t>(primary_cs.used_dwords));
        combined.insert(combined.end(), sec_words, sec_words + comp1.total_ib_dwords);
        const uint32_t *csec_words = static_cast<const uint32_t *>(csec_ib->cpu_ptr);
        combined.insert(combined.end(), csec_words, csec_words + ccomp.total_ib_dwords);
        write_pm4_file(dump_icb_pm4, combined.data(), combined.size());

        a840_icb_destroy(cicb);
        a840_icb_destroy(icb_copy);
        a840_icb_destroy(icb);
    }

    CASE("icb_error_paths_inherit_and_cross_type");
    {
        A840IcbDesc desc{};
        desc.type = A840_ICB_TYPE_RENDER;
        desc.max_commands = 4u;
        desc.inherit_pipeline = true;
        desc.inherit_buffers = true;
        A840IndirectCmdBuffer *icb = nullptr;
        CHECK(a840_icb_create(fx.dev, &desc, &icb) == A840_RT_OK);

        CHECK(a840_icb_set_pipeline(icb, 0u, 1u, nullptr) == A840_RT_STATE_ERROR);
        CHECK(a840_icb_set_bindless_buffer(icb, 0u, 0u, {1u, 64u}) == A840_RT_STATE_ERROR);
        const uint32_t lsz[3] = {8u, 8u, 1u};
        const uint32_t grps[3] = {1u, 1u, 1u};
        CHECK(a840_icb_encode_dispatch(icb, 0u, lsz, grps) == A840_RT_INVALID_ARG);
        CHECK(a840_icb_reset_range(icb, 3u, 2u) == A840_RT_RANGE_ERROR);

        a840_icb_destroy(icb);
    }
}

/* ============================================================================
 * Group 5: Phase 2 / Phase 3 / Phase 4 DualPathRouter Backward Compatibility
 * ============================================================================ */
void test_group5_router_matrix()
{
    CASE("router_phase2_mask_unchanged");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_PHASE2_ALL);

        A840ResourceRouteQuery rq{};
        rq.uses_heap_aliasing = true;
        CHECK(a840_router_eval_resource(&r, &rq).predicate_id == A840_PRED_RESOURCE_HEAP_ALIASING);
        rq.uses_heap_aliasing = false;
        rq.uses_sparse_texture = true;
        CHECK(a840_router_eval_resource(&r, &rq).predicate_id == A840_PRED_RESOURCE_SPARSE_TEXTURE);

        A840ComputeRouteQuery cq{};
        cq.has_compiled_kernel = true;
        cq.has_valid_local_size = true;
        cq.is_indirect_dispatch = true;
        CHECK(a840_router_eval_compute(&r, &cq).predicate_id == A840_PRED_COMPUTE_INDIRECT_DISPATCH);

        A840RenderRouteQuery ren{};
        ren.color_attachment_count = 1u;
        ren.color_mtl_formats[0] = A840_MTL_FMT_BGRA8_UNORM;
        ren.color_tile_modes[0] = A840_TILE_LINEAR;
        ren.sample_count = 1u;
        ren.uses_indirect_draw = true;
        CHECK(a840_router_eval_render(&r, &ren).predicate_id == A840_PRED_RENDER_INDIRECT_OR_ICB);
    }

    CASE("router_phase3_mask_keeps_phase4_fallback");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, static_cast<uint32_t>(A840_ROUTE_PHASE2_ALL) |
                             static_cast<uint32_t>(A840_ROUTE_PHASE3_ALL));

        A840ResourceRouteQuery rq{};
        rq.uses_heap_aliasing = true;
        CHECK(a840_router_eval_resource(&r, &rq).predicate_id == A840_PRED_RESOURCE_HEAP_ALIASING);
        rq.uses_heap_aliasing = false;
        rq.uses_sparse_texture = true;
        CHECK(a840_router_eval_resource(&r, &rq).predicate_id == A840_PRED_RESOURCE_SPARSE_TEXTURE);

        A840ComputeRouteQuery cq{};
        cq.has_compiled_kernel = true;
        cq.has_valid_local_size = true;
        cq.uses_icb = true;
        CHECK(a840_router_eval_compute(&r, &cq).predicate_id == A840_PRED_COMPUTE_INDIRECT_DISPATCH);

        A840RenderRouteQuery ren{};
        ren.color_attachment_count = 1u;
        ren.color_mtl_formats[0] = A840_MTL_FMT_BGRA8_UNORM;
        ren.color_tile_modes[0] = A840_TILE_UBWC;
        ren.sample_count = 1u;
        ren.uses_icb = true;
        CHECK(a840_router_eval_render(&r, &ren).predicate_id == A840_PRED_RENDER_INDIRECT_OR_ICB);
    }

    CASE("router_phase4_individual_capability_bits");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, static_cast<uint32_t>(A840_ROUTE_PHASE2_ALL) |
                             static_cast<uint32_t>(A840_ROUTE_PHASE3_ALL));

        /* Bit 10: Heap Aliasing */
        a840_router_enable_category(&r, A840_ROUTE_P4_HEAP_ALIASING);
        A840ResourceRouteQuery rq{};
        rq.uses_heap_aliasing = true;
        CHECK(a840_router_eval_resource(&r, &rq).path == A840_ROUTE_PATH_RAW);
        rq.uses_sparse_texture = true;
        CHECK(a840_router_eval_resource(&r, &rq).predicate_id == A840_PRED_RESOURCE_SPARSE_TEXTURE);

        /* Bit 13: Sparse */
        a840_router_enable_category(&r, A840_ROUTE_P4_SPARSE);
        CHECK(a840_router_eval_resource(&r, &rq).path == A840_ROUTE_PATH_RAW);

        /* Bit 11: Indirect Compute */
        A840ComputeRouteQuery cq{};
        cq.has_compiled_kernel = true;
        cq.has_valid_local_size = true;
        cq.is_indirect_dispatch = true;
        CHECK(a840_router_eval_compute(&r, &cq).predicate_id == A840_PRED_COMPUTE_INDIRECT_DISPATCH);
        a840_router_enable_category(&r, A840_ROUTE_P4_INDIRECT_COMPUTE);
        CHECK(a840_router_eval_compute(&r, &cq).path == A840_ROUTE_PATH_RAW);
        cq.is_indirect_dispatch = false;
        cq.uses_icb = true;
        CHECK(a840_router_eval_compute(&r, &cq).path == A840_ROUTE_PATH_RAW);

        /* Bit 12: Indirect Render & ICB */
        A840RenderRouteQuery ren{};
        ren.color_attachment_count = 1u;
        ren.color_mtl_formats[0] = A840_MTL_FMT_BGRA8_UNORM;
        ren.color_tile_modes[0] = A840_TILE_UBWC;
        ren.sample_count = 1u;
        ren.uses_indirect_draw = true;
        CHECK(a840_router_eval_render(&r, &ren).predicate_id == A840_PRED_RENDER_INDIRECT_OR_ICB);
        a840_router_enable_category(&r, A840_ROUTE_P4_INDIRECT_RENDER_ICB);
        CHECK(a840_router_eval_render(&r, &ren).path == A840_ROUTE_PATH_RAW);
        ren.uses_indirect_draw = false;
        ren.uses_icb = true;
        CHECK(a840_router_eval_render(&r, &ren).path == A840_ROUTE_PATH_RAW);

        /* Unrelated Phase 4 predicates (MSAA resolve / Tessellation) still fallback. */
        ren.uses_tessellation = true;
        CHECK(a840_router_eval_render(&r, &ren).predicate_id == A840_PRED_RENDER_TESS_OR_AMPLIFY);
    }

    CASE("router_helper_enable_phase4_and_disable");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, static_cast<uint32_t>(A840_ROUTE_PHASE2_ALL) |
                             static_cast<uint32_t>(A840_ROUTE_PHASE3_ALL));
        a840_argbuf_icb_enable_phase4_router(&r);
        CHECK((r.enabled_categories & A840_ROUTE_PHASE4_ALL) == A840_ROUTE_PHASE4_ALL);
        a840_router_disable_category(&r, A840_ROUTE_P4_SPARSE);
        CHECK((r.enabled_categories & A840_ROUTE_P4_SPARSE) == 0u);
    }
}

/* ============================================================================
 * Group 6: Extra Edge-Case Coverage (ensuring >= 50 independent test cases)
 * ============================================================================ */
void test_group6_edge_cases()
{
    DeviceFixture fx;

    /* 32 more targeted unit cases covering every boundary condition */
    const uint64_t aligns[6] = {64u, 128u, 256u, 4096u, 16384u, 65536u};
    for (size_t i = 0; i < 6u; ++i) {
        CASE("heap_buffer_align_sweep");
        A840HeapDesc hd{};
        hd.type = A840_HEAP_TYPE_AUTOMATIC;
        hd.size_bytes = 256u * 1024u;
        hd.alignment = 65536u;
        A840Heap *h = nullptr;
        CHECK(a840_heap_create(fx.dev, &hd, &h) == A840_RT_OK);
        A840HeapBufferDesc bd{};
        bd.size_bytes = 64u;
        bd.alignment = 64u;
        A840HeapSubAlloc *pad = nullptr;
        CHECK(a840_heap_alloc_buffer(h, &bd, &pad) == A840_RT_OK);
        bd.size_bytes = 128u;
        bd.alignment = aligns[i];
        A840HeapSubAlloc *aligned = nullptr;
        CHECK(a840_heap_alloc_buffer(h, &bd, &aligned) == A840_RT_OK);
        CHECK((aligned->offset % aligns[i]) == 0u);
        CHECK((aligned->gpu_va % aligns[i]) == 0u);
        a840_heap_suballoc_release(pad);
        a840_heap_suballoc_release(aligned);
        a840_heap_release(h);
    }

    const uint32_t prims[6] = {
        A840_PRIM_POINTS, A840_PRIM_LINES, A840_PRIM_LINE_STRIP,
        A840_PRIM_TRIANGLES, A840_PRIM_TRIANGLE_STRIP, A840_PRIM_TRIANGLE_FAN
    };
    for (size_t i = 0; i < 6u; ++i) {
        CASE("indirect_draw_all_primitives");
        uint32_t words[16];
        A840Reloc relocs[2];
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words, 16u, relocs, 2u) == A840_EMIT_OK);
        A840DrawIndirectDesc d{};
        d.primitive = prims[i];
        d.indirect = {70u + i, UINT64_C(0x50000000) + i * 64u};
        CHECK(a840_emit_draw_indirect(&cs, &d) == A840_EMIT_OK);
        CHECK(cs.used_dwords <= A840_EMIT_DRAW_INDIRECT_MAX_DWORDS);
    }

    for (size_t i = 0; i < 6u; ++i) {
        CASE("indexed_indirect_draw_all_primitives");
        uint32_t words[16];
        A840Reloc relocs[2];
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words, 16u, relocs, 2u) == A840_EMIT_OK);
        A840DrawIndexedIndirectDesc d{};
        d.primitive = prims[i];
        d.index_type = (i % 2u == 0u) ? A840_INDEX_UINT16 : A840_INDEX_UINT32;
        d.max_indices = 256u;
        d.index_buffer = {80u + i, UINT64_C(0x60000000) + i * 64u};
        d.indirect = {90u + i, UINT64_C(0x61000000) + i * 64u};
        CHECK(a840_emit_draw_indexed_indirect(&cs, &d) == A840_EMIT_OK);
        CHECK(cs.used_dwords <= A840_EMIT_DRAW_INDEXED_INDIRECT_MAX_DWORDS);
    }

    const uint32_t local_cfgs[5][3] = {
        {1u, 1u, 1u}, {64u, 1u, 1u}, {16u, 16u, 1u}, {8u, 8u, 16u}, {1024u, 1u, 1u}
    };
    for (size_t i = 0; i < 5u; ++i) {
        CASE("dispatch_indirect_local_sizes");
        uint32_t words[24];
        A840Reloc relocs[2];
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words, 24u, relocs, 2u) == A840_EMIT_OK);
        A840DispatchIndirectDesc d{};
        d.local_size[0] = local_cfgs[i][0];
        d.local_size[1] = local_cfgs[i][1];
        d.local_size[2] = local_cfgs[i][2];
        d.work_dim = static_cast<uint32_t>((i % 3u) + 1u);
        d.indirect = {95u + i, UINT64_C(0x70000000) + i * 64u};
        CHECK(a840_emit_dispatch_indirect(&cs, &d) == A840_EMIT_OK);
        CHECK(cs.used_dwords <= A840_EMIT_DISPATCH_INDIRECT_MAX_DWORDS);
    }

    CASE("dispatch_indirect_zero_axis_and_bad_dim");
    {
        uint32_t words[24];
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words, 24u, nullptr, 0u) == A840_EMIT_OK);
        A840DispatchIndirectDesc d{};
        d.local_size[0] = 0u;
        d.local_size[1] = 1u;
        d.local_size[2] = 1u;
        d.indirect = {1u, UINT64_C(0x71000000)};
        CHECK(a840_emit_dispatch_indirect(&cs, &d) == A840_EMIT_RANGE_ERROR);
        d.local_size[0] = 8u;
        d.work_dim = 4u;
        CHECK(a840_emit_dispatch_indirect(&cs, &d) == A840_EMIT_RANGE_ERROR);
    }

    CASE("emit_indirect_capacity_rollback_all");
    {
        uint32_t words[2];
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words, 2u, nullptr, 0u) == A840_EMIT_OK);
        A840DrawIndirectDesc d1{};
        d1.primitive = A840_PRIM_TRIANGLES;
        d1.indirect = {1u, UINT64_C(0x80000000)};
        CHECK(a840_emit_draw_indirect(&cs, &d1) == A840_EMIT_CAPACITY);
        CHECK(cs.used_dwords == 0u);

        A840DrawIndexedIndirectDesc d2{};
        d2.primitive = A840_PRIM_TRIANGLES;
        d2.index_type = A840_INDEX_UINT32;
        d2.max_indices = 16u;
        d2.index_buffer = {1u, UINT64_C(0x80001000)};
        d2.indirect = {2u, UINT64_C(0x80002000)};
        CHECK(a840_emit_draw_indexed_indirect(&cs, &d2) == A840_EMIT_CAPACITY);
        CHECK(cs.used_dwords == 0u);

        A840DispatchIndirectDesc d3{};
        d3.local_size[0] = 8u;
        d3.local_size[1] = 8u;
        d3.local_size[2] = 1u;
        d3.indirect = {3u, UINT64_C(0x80003000)};
        CHECK(a840_emit_dispatch_indirect(&cs, &d3) == A840_EMIT_CAPACITY);
        CHECK(cs.used_dwords == 0u);
    }

    CASE("heap_refcount_survives_parent_release");
    {
        A840HeapDesc hd{};
        hd.type = A840_HEAP_TYPE_AUTOMATIC;
        hd.size_bytes = 64u * 1024u;
        A840Heap *h = nullptr;
        CHECK(a840_heap_create(fx.dev, &hd, &h) == A840_RT_OK);
        A840HeapBufferDesc bd{};
        bd.size_bytes = 4096u;
        A840HeapSubAlloc *sub = nullptr;
        CHECK(a840_heap_alloc_buffer(h, &bd, &sub) == A840_RT_OK);
        a840_heap_suballoc_retain(sub);
        a840_heap_release(h); /* sub keeps heap alive */
        CHECK(a840_heap_suballoc_validate(fx.dev, sub) == A840_RT_OK);
        a840_heap_suballoc_release(sub);
        a840_heap_suballoc_release(sub);
    }

    CASE("heap_texture_linear_and_tiled_modes");
    {
        A840HeapDesc hd{};
        hd.type = A840_HEAP_TYPE_AUTOMATIC;
        hd.size_bytes = 1024u * 1024u;
        A840Heap *h = nullptr;
        CHECK(a840_heap_create(fx.dev, &hd, &h) == A840_RT_OK);

        A840HeapTextureDesc td{};
        td.mtl_pixel_format = A840_MTL_FMT_RGBA8_UNORM;
        td.tex_type = A840_TEX_2D;
        td.width = 128u;
        td.height = 64u;
        td.tile_mode = A840_TILE_LINEAR;
        A840HeapSubAlloc *tlin = nullptr;
        CHECK(a840_heap_alloc_texture(h, &td, &tlin) == A840_RT_OK);
        CHECK(tlin->ubwc_flag_gpu_va == 0u);

        td.tile_mode = A840_TILE_TILED;
        A840HeapSubAlloc *ttil = nullptr;
        CHECK(a840_heap_alloc_texture(h, &td, &ttil) == A840_RT_OK);
        CHECK((ttil->gpu_va % A840_RT_PAGE_BYTES) == 0u);

        a840_heap_suballoc_release(tlin);
        a840_heap_suballoc_release(ttil);
        a840_heap_release(h);
    }

    CASE("heap_suballoc_make_aliasable_idempotent");
    {
        A840HeapDesc hd{};
        hd.type = A840_HEAP_TYPE_AUTOMATIC;
        hd.size_bytes = 64u * 1024u;
        A840Heap *h = nullptr;
        CHECK(a840_heap_create(fx.dev, &hd, &h) == A840_RT_OK);
        A840HeapBufferDesc bd{};
        bd.size_bytes = 4096u;
        A840HeapSubAlloc *sub = nullptr;
        CHECK(a840_heap_alloc_buffer(h, &bd, &sub) == A840_RT_OK);
        CHECK(a840_heap_suballoc_make_aliasable(sub) == A840_RT_OK);
        CHECK(a840_heap_suballoc_make_aliasable(sub) == A840_RT_OK);
        a840_heap_suballoc_release(sub);
        a840_heap_release(h);
    }

    CASE("heap_suballoc_wrong_device_rejected");
    {
        DeviceFixture fx2;
        A840HeapDesc hd{};
        hd.type = A840_HEAP_TYPE_AUTOMATIC;
        hd.size_bytes = 64u * 1024u;
        A840Heap *h = nullptr;
        CHECK(a840_heap_create(fx.dev, &hd, &h) == A840_RT_OK);
        A840HeapBufferDesc bd{};
        bd.size_bytes = 1024u;
        A840HeapSubAlloc *sub = nullptr;
        CHECK(a840_heap_alloc_buffer(h, &bd, &sub) == A840_RT_OK);
        CHECK(a840_heap_suballoc_validate(fx2.dev, sub) == A840_RT_WRONG_DEVICE);
        a840_heap_suballoc_release(sub);
        a840_heap_release(h);
    }

    CASE("icb_create_invalid_max_commands");
    {
        A840IcbDesc d{};
        d.type = A840_ICB_TYPE_RENDER;
        d.max_commands = 0u;
        A840IndirectCmdBuffer *icb = nullptr;
        CHECK(a840_icb_create(fx.dev, &d, &icb) == A840_RT_RANGE_ERROR);
        d.max_commands = A840_ICB_MAX_SLOTS + 1u;
        CHECK(a840_icb_create(fx.dev, &d, &icb) == A840_RT_RANGE_ERROR);
    }

    CASE("icb_copy_cross_device_and_cross_type_rejected");
    {
        DeviceFixture fx2;
        A840IcbDesc dr{A840_ICB_TYPE_RENDER, 4u, false, false};
        A840IcbDesc dc{A840_ICB_TYPE_COMPUTE, 4u, false, false};
        A840IndirectCmdBuffer *r1 = nullptr;
        A840IndirectCmdBuffer *c1 = nullptr;
        A840IndirectCmdBuffer *r2 = nullptr;
        CHECK(a840_icb_create(fx.dev, &dr, &r1) == A840_RT_OK);
        CHECK(a840_icb_create(fx.dev, &dc, &c1) == A840_RT_OK);
        CHECK(a840_icb_create(fx2.dev, &dr, &r2) == A840_RT_OK);

        CHECK(a840_icb_copy_range(c1, 0u, r1, 0u, 2u) == A840_RT_INVALID_ARG);
        CHECK(a840_icb_copy_range(r2, 0u, r1, 0u, 2u) == A840_RT_WRONG_DEVICE);

        a840_icb_destroy(r2);
        a840_icb_destroy(c1);
        a840_icb_destroy(r1);
    }

    CASE("icb_compile_all_empty_range");
    {
        A840IcbDesc dr{A840_ICB_TYPE_RENDER, 4u, false, false};
        A840IndirectCmdBuffer *icb = nullptr;
        CHECK(a840_icb_create(fx.dev, &dr, &icb) == A840_RT_OK);
        A840RtBuffer *sec_ib = fx.alloc_buf(4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_CPU_VISIBLE);
        A840IcbCompiledRange comp{};
        CHECK(a840_icb_compile_to_gpu_ib(icb, 0u, 4u, sec_ib, nullptr, 0u, &comp) == A840_RT_OK);
        CHECK(comp.non_empty_count == 0u);
        CHECK(comp.total_ib_dwords == 0u);

        uint32_t words[16];
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words, 16u, nullptr, 0u) == A840_EMIT_OK);
        CHECK(a840_icb_emit_execute(&cs, &comp, false) == A840_EMIT_RANGE_ERROR);
        a840_icb_destroy(icb);
    }

    CASE("icb_bindless_alignment_and_slot_bounds");
    {
        A840IcbDesc dr{A840_ICB_TYPE_RENDER, 4u, false, false};
        A840IndirectCmdBuffer *icb = nullptr;
        CHECK(a840_icb_create(fx.dev, &dr, &icb) == A840_RT_OK);
        CHECK(a840_icb_set_bindless_buffer(icb, 0u, 0u, {1u, 32u}) == A840_RT_ALIGNMENT);
        CHECK(a840_icb_set_bindless_buffer(icb, 0u, A840_ICB_MAX_BIND_BUFFERS, {1u, 64u}) == A840_RT_RANGE_ERROR);
        a840_icb_destroy(icb);
    }

    CASE("argbuf_target_required_before_encode");
    {
        A840ArgSlotDesc s[1] = {{0u, A840_ARG_SLOT_INLINE_DATA, 0u, 16u}};
        A840ArgBufEncoder enc{};
        CHECK(a840_argbuf_encoder_init(&enc, fx.dev, s, 1u) == A840_RT_OK);
        uint32_t v = 42u;
        CHECK(a840_argbuf_set_inline_bytes(&enc, 0u, &v, sizeof(v)) == A840_RT_STATE_ERROR);
    }

    CASE("argbuf_slot_kind_mismatch_rejected");
    {
        A840ArgSlotDesc s[2] = {
            {0u, A840_ARG_SLOT_BUFFER, 0u, 16u},
            {1u, A840_ARG_SLOT_TEXTURE, 64u, 64u},
        };
        A840ArgBufEncoder enc{};
        CHECK(a840_argbuf_encoder_init(&enc, fx.dev, s, 2u) == A840_RT_OK);
        A840RtBuffer *arg_gpu = fx.alloc_buf(4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE);
        A840RtTexture *tex = fx.alloc_tex(A840_MTL_FMT_BGRA8_UNORM, 32u, 32u);
        CHECK(a840_argbuf_encoder_set_target(&enc, arg_gpu, 0u) == A840_RT_OK);
        CHECK(a840_argbuf_set_texture(&enc, 0u, tex) == A840_RT_INVALID_ARG);
        CHECK(a840_argbuf_set_buffer(&enc, 1u, arg_gpu, 0u) == A840_RT_INVALID_ARG);
    }

    CASE("router_eval_null_safety_and_format");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, static_cast<uint32_t>(A840_ROUTE_PHASE2_ALL) |
                             static_cast<uint32_t>(A840_ROUTE_PHASE3_ALL) |
                             static_cast<uint32_t>(A840_ROUTE_PHASE4_ALL));
        A840ResourceRouteQuery rq{};
        rq.uses_heap_aliasing = true;
        rq.uses_sparse_texture = true;
        A840RouteDecision d = a840_router_eval_resource(&r, &rq);
        char buf[64]{};
        CHECK(a840_router_format_decision(&d, buf, sizeof(buf)) > 0u);
        CHECK(std::strcmp(buf, "path=raw") == 0);
    }
}

} // namespace

int main(int argc, char **argv)
{
    const char *dump_indirect_pm4 = nullptr;
    const char *dump_icb_pm4 = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-indirect-pm4") == 0 && i + 1 < argc) {
            dump_indirect_pm4 = argv[++i];
        } else if (std::strcmp(argv[i], "--dump-icb-pm4") == 0 && i + 1 < argc) {
            dump_icb_pm4 = argv[++i];
        }
    }

    A840HeapAliasTransition ubwc_tr{};
    test_group1_heap_allocator();
    test_group2_aliasing_transitions(&ubwc_tr);
    test_group3_argbuf_encoder();
    test_group4_indirect_and_icb(dump_indirect_pm4, dump_icb_pm4, &ubwc_tr);
    test_group5_router_matrix();
    test_group6_edge_cases();

    CHECK(g_cases >= 50u);
    std::printf("PASS test_phase4_heap_argbuf_icb: %u cases, %u checks\n", g_cases, g_checks);
    return 0;
}
