/* SPDX-License-Identifier: MIT */
#include "a840_ca_render_bridge.h"
#include "a840_pack_cache_rt.h"
#include "a840_ubwc_texture.h"
#include "gen/a840_bitfields.h"
#include "gen/a840_gmem_bitfields.h"

#include <array>
#include <cstdint>
#include <memory>
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

constexpr uint64_t kStackKey = UINT64_C(0x24a4460000840001);
constexpr uint64_t kWrongStackKey = UINT64_C(0x24a4460000840999);
constexpr uint32_t kScreenW = 1200u;
constexpr uint32_t kScreenH = 2608u;

struct MockFixture {
    A840MockTransport *mock = nullptr;
    A840RtGmemQueue gmem_q{};
    std::vector<A840RtBuffer *> buffers;

    explicit MockFixture(bool auto_complete = false)
    {
        A840MockTransportConfig cfg{};
        a840_mock_transport_default_config(&cfg);
        cfg.auto_complete_submits = auto_complete;
        CHECK(a840_mock_transport_create(&cfg, &mock) == A840_TRANSPORT_OK);
        CHECK(a840_rt_gmem_open(&gmem_q, a840_mock_transport_interface(mock)) == A840_RT_OK);
    }

    A840RtDevice *dev() { return gmem_q.device; }

    A840RtBuffer *alloc_buf(uint64_t bytes, uint32_t usage, uint64_t align = A840_RT_PAGE_BYTES)
    {
        A840RtBufferDesc d{};
        d.size_bytes = bytes;
        d.alignment = align;
        d.usage_flags = usage;
        A840RtBuffer *b = nullptr;
        CHECK(a840_rt_buffer_create(dev(), &d, &b) == A840_RT_OK);
        buffers.push_back(b);
        return b;
    }

    ~MockFixture()
    {
        for (A840RtBuffer *b : buffers) {
            a840_rt_buffer_release(b);
        }
        a840_rt_gmem_close(&gmem_q);
        CHECK(a840_mock_transport_live_queue_count(mock) == 0u);
        a840_mock_transport_destroy(mock);
    }
};

struct AlignedPackBuf {
    uint8_t *data = nullptr;
    size_t bytes = 0;

    explicit AlignedPackBuf(size_t n) : bytes(n)
    {
        void *p = nullptr;
        CHECK(posix_memalign(&p, 128u, n) == 0);
        data = static_cast<uint8_t *>(p);
        std::memset(data, 0, n);
    }
    ~AlignedPackBuf() { std::free(data); }
    AlignedPackBuf(const AlignedPackBuf &) = delete;
    AlignedPackBuf &operator=(const AlignedPackBuf &) = delete;
};

struct PipelineFixture {
    A840PipelineRecord rec{};
    A840BindingPlan plan{};
    std::array<std::array<uint32_t, 8>, 2> isa{};
    std::array<std::array<uint32_t, 4>, 2> consts{};
    std::array<std::array<uint32_t, 4>, 2> sds{};

    void setup(uint8_t id, uint64_t stack_key = kStackKey)
    {
        std::memset(&rec, 0, sizeof(rec));
        std::memset(&plan, 0, sizeof(plan));
        rec.abi_fingerprint = a840_compile_abi_fingerprint();
        rec.stack_key = stack_key;
        rec.chip_id = A840_PACK_V1_CHIP_ID;
        rec.highest_bank_bit = A840_PACK_V1_HBB;
        rec.stage_count = 2u;
        rec.pipeline_key.sha256[0] = id;
        rec.pipeline_key.sha256[1] = static_cast<uint8_t>(id ^ 0x5au);
        rec.pipeline_key.sha256[31] = static_cast<uint8_t>(id + 17u);
        plan.stage_count = 2u;
        for (uint32_t s = 0; s < 2u; ++s) {
            A840CompiledStage &st = rec.stages[s];
            st.abi_version = 1u;
            st.stage = s;
            st.isa_words = isa[s].data();
            st.isa_dwords = 8u;
            st.instruction_groups = 2u;
            st.max_full_reg = 7;
            st.max_half_reg = -1;
            st.constlen_vec4 = 4u;
            st.const_data_words = consts[s].data();
            st.const_data_dwords = 4u;
            st.sds_state_words = sds[s].data();
            st.sds_state_dwords = 4u;
            st.max_waves = 16u;
            st.local_size[0] = 1u;
            st.local_size[1] = 1u;
            st.local_size[2] = 1u;
            isa[s].fill(0x03000000u + static_cast<uint32_t>(id) * 16u + s);
            consts[s].fill(static_cast<uint32_t>(id) + s + 1u);
            /* Valid CP_NOP header + payload inside pre-encoded SDS buffer. */
            uint32_t nop_hdr = 0u;
            CHECK(a840_pm4_type7_header(A840_CP_NOP, 3u, &nop_hdr) == A840_EMIT_OK);
            sds[s][0] = nop_hdr;
            sds[s][1] = 0x11110000u + id;
            sds[s][2] = 0x22220000u + s;
            sds[s][3] = 0x33330000u + id;
            plan.stages[s].stage = s;
            plan.stages[s].const_bytes = 64u;
        }
    }
};

std::unique_ptr<AlignedPackBuf> build_pack(uint32_t tier,
                                           std::vector<PipelineFixture *> fixtures,
                                           uint64_t stack_key = kStackKey)
{
    std::array<A840PackV1BuilderItem, 32> items{};
    std::array<uint32_t, 128> buckets{};
    A840PackV1Builder b{};
    CHECK(a840_pack_v1_builder_init(&b, tier, stack_key, a840_compile_abi_fingerprint(),
                                    items.data(), items.size(),
                                    buckets.data(), buckets.size()) == A840_PACK_OK);
    for (PipelineFixture *f : fixtures) {
        CHECK(a840_pack_v1_builder_add(&b, &f->rec, &f->plan) == A840_PACK_OK);
    }
    size_t need = 0u;
    CHECK(a840_pack_v1_builder_measure(&b, &need) == A840_PACK_OK);
    auto out = std::make_unique<AlignedPackBuf>(need);
    size_t built = 0u;
    CHECK(a840_pack_v1_builder_build(&b, out->data, out->bytes, &built) == A840_PACK_OK);
    CHECK(built == need);
    return out;
}

void write_pm4_file(const char *path, const uint32_t *words, size_t dwords)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    CHECK(f.good());
    f.write(reinterpret_cast<const char *>(words),
            static_cast<std::streamsize>(dwords * sizeof(uint32_t)));
    CHECK(f.good());
}

/* =========================================================================
 * Group 1: UBWC 6.0 HBB=16 / Layout / Descriptor / MDSS Scanout (14 cases)
 * ========================================================================= */
void test_group1_ubwc6()
{
    MockFixture mf;

    CASE("ubwc6_layout_1200x2608_bgra8_exact_fdl");
    {
        A840UbwcLayout lay{};
        CHECK(a840_ubwc_compute_layout(A840_MTL_FMT_BGRA8_UNORM, kScreenW, kScreenH, 1u, 1u, &lay) == A840_RT_OK);
        CHECK(lay.cpp == 4u);
        CHECK(lay.ubwc_block_width == 16u && lay.ubwc_block_height == 4u);
        CHECK(lay.flag_pitch == 128u && lay.flag_rows == 656u);
        CHECK(lay.flag_slice_bytes == 86016u);
        CHECK(lay.data_pitch == 4864u && lay.data_padded_height == 2608u);
        CHECK(lay.data_slice_bytes == 12685312u);
        CHECK(lay.flag_offset == 0u && lay.data_offset == 86016u);
        CHECK(lay.total_bytes == 86016u + 12685312u);
    }

    CASE("ubwc6_create_bgra8_1200x2608_hbb16_descriptor");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_UBWC6;
        d.storage_mode = A840_STORAGE_PRIVATE;
        d.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
        d.width = kScreenW;
        d.height = kScreenH;
        d.usage_flags = A840_TEX_USAGE_RENDER_TARGET | A840_TEX_USAGE_SHADER_READ;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
        CHECK(tex->highest_bank_bit == 16u && tex->ubwc_version == 6u);
        CHECK(tex->ubwc_swizzle == 6u && tex->macrotile_mode == 1u);
        CHECK(tex->backing_bo != nullptr);
        CHECK(tex->flag_range.bo == tex->backing_bo->bo_handle);
        CHECK(tex->data_range.bo == tex->backing_bo->bo_handle);
        CHECK(tex->flag_range.va + tex->flag_range.bytes <= tex->data_range.va);
        CHECK((tex->tex_const[0][4] & A840_TEX_4_FLAG) != 0u);
        CHECK((tex->tex_const[0][4] & A840_TEX_4_TILE_MODE_MAX) == A840_TILE6_3);
        CHECK(tex->surface_meta_negotiated);
        CHECK(tex->surface_meta.highest_bank_bit == 16u);
        CHECK(a840_ubwc_texture_validate_sysmem_target(tex) == A840_RT_OK);
        a840_ubwc_texture_destroy(tex);
    }

    CASE("ubwc6_create_bgra8_srgb_descriptor");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_UBWC6;
        d.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM_SRGB;
        d.width = 512u;
        d.height = 512u;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
        CHECK(tex->is_srgb);
        CHECK((tex->tex_const[0][4] & A840_TEX_4_SRGB) != 0u);
        a840_ubwc_texture_destroy(tex);
    }

    CASE("ubwc6_create_rgba16float_8bpp_block8x4");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_UBWC6;
        d.mtl_pixel_format = 115u; /* RGBA16Float */
        d.width = 256u;
        d.height = 256u;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
        CHECK(tex->ubwc.cpp == 8u);
        CHECK(tex->ubwc.ubwc_block_width == 8u && tex->ubwc.ubwc_block_height == 4u);
        a840_ubwc_texture_destroy(tex);
    }

    CASE("ubwc6_create_rg8_quirk_block16x8");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_UBWC6;
        d.mtl_pixel_format = A840_MTL_FMT_RG8_UNORM;
        d.width = 256u;
        d.height = 128u;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
        CHECK(tex->ubwc.ubwc_block_width == 16u && tex->ubwc.ubwc_block_height == 8u);
        CHECK((tex->ubwc.data_pitch % 128u) == 0u);
        a840_ubwc_texture_destroy(tex);
    }

    CASE("ubwc6_create_rgba32float_16bpp_block4x4");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_UBWC6;
        d.mtl_pixel_format = 125u; /* RGBA32Float */
        d.width = 256u;
        d.height = 128u;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
        CHECK(tex->ubwc.cpp == 16u && tex->ubwc.ubwc_block_width == 4u && tex->ubwc.ubwc_block_height == 4u);
        a840_ubwc_texture_destroy(tex);
    }

    CASE("ubwc6_reject_hbb15");
    {
        size_t before = a840_mock_transport_live_bo_count(mf.mock);
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_UBWC6;
        d.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
        d.width = 256u;
        d.height = 256u;
        d.highest_bank_bit = 15u;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_UNSUPPORTED);
        CHECK(tex == nullptr);
        CHECK(a840_mock_transport_live_bo_count(mf.mock) == before);
    }

    CASE("ubwc6_reject_hbb14_and_hbb17");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_UBWC6;
        d.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
        d.width = 128u;
        d.height = 128u;
        A840UbwcTexture *tex = nullptr;
        d.highest_bank_bit = 14u;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_UNSUPPORTED);
        d.highest_bank_bit = 17u;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_UNSUPPORTED);
    }

    CASE("ubwc6_reject_wrong_ubwc_version_swizzle_macrotile");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_UBWC6;
        d.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
        d.width = 128u;
        d.height = 128u;
        A840UbwcTexture *tex = nullptr;
        d.ubwc_version = 5u;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_UNSUPPORTED);
        d.ubwc_version = 6u;
        d.ubwc_swizzle = 5u;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_UNSUPPORTED);
        d.ubwc_swizzle = 6u;
        d.macrotile_mode = 2u;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_UNSUPPORTED);
    }

    CASE("ubwc6_reject_non_ubwc_format");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_UBWC6;
        d.width = 128u;
        d.height = 128u;
        A840UbwcTexture *tex = nullptr;
        d.mtl_pixel_format = 1u; /* A8Unorm: ubwc_supported=0 */
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_UNSUPPORTED);
        d.mtl_pixel_format = A840_MTL_FMT_BGR10_XR;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_UNSUPPORTED);
    }

    const uint32_t mdss_fmts[] = {A840_MTL_FMT_BGRA8_UNORM, A840_MTL_FMT_BGRA8_UNORM_SRGB};
    A840MdssScanoutCaps mdss{};
    mdss.flags = A840_SCANOUT_CAP_UBWC6;
    mdss.max_width = 4096u;
    mdss.max_height = 4096u;
    mdss.formats = mdss_fmts;
    mdss.format_count = 2u;

    CASE("ubwc6_scanout_mdss_direct_eligible");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_UBWC6;
        d.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
        d.width = kScreenW;
        d.height = kScreenH;
        d.for_scanout = true;
        d.mdss_caps = &mdss;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
        CHECK(tex->scanout_eligible);
        CHECK(!tex->fallback_linear_required);
        CHECK(tex->linear_resolve_bo == nullptr);
        CHECK((tex->surface_meta.flags & A840_SCANOUT_FLAG_ELIGIBLE) != 0u);
        a840_ubwc_texture_destroy(tex);
    }

    CASE("ubwc6_scanout_mdss_incompatible_format_fallback_linear");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_UBWC6;
        d.mtl_pixel_format = 115u; /* RGBA16Float not in mdss_fmts */
        d.width = 512u;
        d.height = 512u;
        d.for_scanout = true;
        d.mdss_caps = &mdss;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
        CHECK(!tex->scanout_eligible);
        CHECK(tex->fallback_linear_required);
        CHECK(tex->linear_resolve_bo != nullptr);
        CHECK((tex->surface_meta.flags & A840_SCANOUT_FLAG_ELIGIBLE) == 0u);
        a840_ubwc_texture_destroy(tex);
    }

    CASE("ubwc6_scanout_mdss_oversized_fallback_linear");
    {
        A840MdssScanoutCaps small_mdss = mdss;
        small_mdss.max_width = 1024u;
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_UBWC6;
        d.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
        d.width = kScreenW;
        d.height = kScreenH;
        d.for_scanout = true;
        d.mdss_caps = &small_mdss;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
        CHECK(!tex->scanout_eligible);
        CHECK(tex->fallback_linear_required);
        CHECK(tex->linear_resolve_bo != nullptr);
        a840_ubwc_texture_destroy(tex);
    }

    CASE("ubwc6_scanout_mdss_null_caps_fallback_linear");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_UBWC6;
        d.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
        d.width = 256u;
        d.height = 256u;
        d.for_scanout = true;
        d.mdss_caps = nullptr;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
        CHECK(!tex->scanout_eligible);
        CHECK(tex->fallback_linear_required);
        CHECK(tex->linear_resolve_bo != nullptr);
        a840_ubwc_texture_destroy(tex);
    }
}

void test_group2_memoryless()
{
    MockFixture mf;

    CASE("memoryless_depth32float_zero_bo_zero_va");
    {
        size_t bo_before = a840_mock_transport_live_bo_count(mf.mock);
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_MEMORYLESS;
        d.storage_mode = A840_STORAGE_MEMORYLESS;
        d.mtl_pixel_format = A840_MTL_FMT_DEPTH32_FLOAT;
        d.width = kScreenW;
        d.height = kScreenH;
        d.usage_flags = A840_TEX_USAGE_RENDER_TARGET;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
        CHECK(a840_mock_transport_live_bo_count(mf.mock) == bo_before);
        CHECK(tex->backing_bo == nullptr);
        CHECK(tex->data_range.bo == 0u && tex->data_range.va == 0u && tex->data_range.bytes == 0u);
        CHECK(tex->flag_range.bo == 0u && tex->flag_range.va == 0u && tex->flag_range.bytes == 0u);
        CHECK(tex->plane_count == 0u && tex->gmem_bytes_per_pixel == 4u);
        a840_ubwc_texture_destroy(tex);
    }

    CASE("memoryless_bgra8_color_zero_bo");
    {
        size_t bo_before = a840_mock_transport_live_bo_count(mf.mock);
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_MEMORYLESS;
        d.storage_mode = A840_STORAGE_MEMORYLESS;
        d.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
        d.width = 640u;
        d.height = 480u;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
        CHECK(a840_mock_transport_live_bo_count(mf.mock) == bo_before);
        CHECK(tex->backing_bo == nullptr);
        a840_ubwc_texture_destroy(tex);
    }

    A840UbwcTextureDesc md{};
    md.kind = A840_P3_TEX_MEMORYLESS;
    md.storage_mode = A840_STORAGE_MEMORYLESS;
    md.mtl_pixel_format = A840_MTL_FMT_DEPTH32_FLOAT;
    md.width = kScreenW;
    md.height = kScreenH;
    A840UbwcTexture *mtex = nullptr;
    CHECK(a840_ubwc_texture_create(mf.dev(), &md, &mtex) == A840_RT_OK);

    CASE("memoryless_reject_sysmem_target");
    CHECK(a840_ubwc_texture_validate_sysmem_target(mtex) == A840_RT_UNSUPPORTED);

    CASE("memoryless_reject_cpu_readback");
    {
        uint8_t buf[64]{};
        CHECK(a840_ubwc_texture_cpu_readback(mtex, buf, sizeof(buf)) == A840_RT_UNSUPPORTED);
    }

    CASE("memoryless_reject_cpu_visible_and_scanout_flags");
    {
        A840UbwcTexture *bad = nullptr;
        A840UbwcTextureDesc d1 = md;
        d1.usage_flags = A840_TEX_USAGE_CPU_VISIBLE;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d1, &bad) == A840_RT_UNSUPPORTED);
        A840UbwcTextureDesc d2 = md;
        d2.for_scanout = true;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d2, &bad) == A840_RT_UNSUPPORTED);
    }

    CASE("memoryless_reject_gmem_load_and_store");
    {
        A840GmemAttachment att{};
        A840GmemBinding bind{};
        CHECK(a840_ubwc_texture_fill_gmem_attachment(mtex, A840_LOAD_LOAD, A840_STORE_DONT_CARE, &att, &bind) == A840_RT_UNSUPPORTED);
        CHECK(a840_ubwc_texture_fill_gmem_attachment(mtex, A840_LOAD_CLEAR, A840_STORE_STORE, &att, &bind) == A840_RT_UNSUPPORTED);
        CHECK(a840_ubwc_texture_fill_gmem_attachment(mtex, A840_LOAD_CLEAR, A840_STORE_RESOLVE, &att, &bind) == A840_RT_UNSUPPORTED);
    }

    CASE("memoryless_accept_gmem_clear_dont_care");
    {
        A840GmemAttachment att{};
        A840GmemBinding bind{};
        CHECK(a840_ubwc_texture_fill_gmem_attachment(mtex, A840_LOAD_CLEAR, A840_STORE_DONT_CARE, &att, &bind) == A840_RT_OK);
        CHECK(att.memoryless && !att.ubwc && att.cpp == 4u && att.store == A840_STORE_DONT_CARE);
        CHECK(bind.image.data.bo == 0u && bind.image.data.va == 0u && bind.image.flags.bo == 0u);
    }

    CASE("memoryless_reject_wrong_storage_mode");
    {
        A840UbwcTextureDesc bad_d = md;
        bad_d.storage_mode = A840_STORAGE_PRIVATE;
        A840UbwcTexture *bad = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &bad_d, &bad) == A840_RT_INVALID_ARG);
    }

    a840_ubwc_texture_destroy(mtex);
}

void test_group3_xr10_and_ycbcr()
{
    MockFixture mf;

    CASE("xr10_bgr10_linear_and_tiled");
    {
        for (uint32_t tm : {A840_TILE_LINEAR, A840_TILE_TILED}) {
            A840UbwcTextureDesc d{};
            d.kind = A840_P3_TEX_XR10;
            d.mtl_pixel_format = A840_MTL_FMT_BGR10_XR;
            d.width = 640u;
            d.height = 480u;
            d.tile_mode = tm;
            A840UbwcTexture *tex = nullptr;
            CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
            CHECK(tex->is_xr10 && !tex->is_srgb && tex->requires_xr_codec && tex->cpp == 4u);
            CHECK(tex->plane_count == 1u && tex->tex_const[0][0] != 0u);
            a840_ubwc_texture_destroy(tex);
        }
    }

    CASE("xr10_bgr10_srgb_descriptor");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_XR10;
        d.mtl_pixel_format = A840_MTL_FMT_BGR10_XR_SRGB;
        d.width = 320u;
        d.height = 240u;
        d.tile_mode = A840_TILE_TILED;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
        CHECK(tex->is_xr10 && tex->is_srgb && tex->requires_xr_codec);
        a840_ubwc_texture_destroy(tex);
    }

    CASE("xr10_bgra10_linear_and_srgb");
    {
        for (uint32_t fmt : {static_cast<uint32_t>(A840_MTL_FMT_BGRA10_XR),
                             static_cast<uint32_t>(A840_MTL_FMT_BGRA10_XR_SRGB)}) {
            A840UbwcTextureDesc d{};
            d.kind = A840_P3_TEX_XR10;
            d.mtl_pixel_format = fmt;
            d.width = 256u;
            d.height = 256u;
            d.tile_mode = A840_TILE_TILED;
            A840UbwcTexture *tex = nullptr;
            CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
            CHECK(tex->is_xr10 && tex->cpp == 8u && tex->plane_count == 1u);
            CHECK(tex->is_srgb == (fmt == A840_MTL_FMT_BGRA10_XR_SRGB));
            a840_ubwc_texture_destroy(tex);
        }
    }

    CASE("xr10_reject_ubwc_tile_mode");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_XR10;
        d.mtl_pixel_format = A840_MTL_FMT_BGR10_XR;
        d.width = 128u;
        d.height = 128u;
        d.tile_mode = A840_TILE_UBWC;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_UNSUPPORTED);
    }

    CASE("xr10_scanout_never_direct_ubwc");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_XR10;
        d.mtl_pixel_format = A840_MTL_FMT_BGRA10_XR;
        d.width = 256u;
        d.height = 256u;
        d.tile_mode = A840_TILE_TILED;
        d.for_scanout = true;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
        CHECK(!tex->scanout_eligible && tex->fallback_linear_required && tex->linear_resolve_bo != nullptr);
        a840_ubwc_texture_destroy(tex);
    }

    CASE("ycbcr_nv12_linear_dual_plane_descriptors");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_YCBCR_MULTIPLANE;
        d.ycbcr_format = A840_YCBCR_NV12;
        d.width = kScreenW;
        d.height = kScreenH;
        d.tile_mode = A840_TILE_LINEAR;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
        CHECK(tex->plane_count == 2u);
        CHECK(tex->plane_widths[0] == kScreenW && tex->plane_heights[0] == kScreenH);
        CHECK(tex->plane_widths[1] == kScreenW / 2u && tex->plane_heights[1] == kScreenH / 2u);
        CHECK(tex->plane_mtl_formats[0] == A840_MTL_FMT_R8_UNORM);
        CHECK(tex->plane_mtl_formats[1] == A840_MTL_FMT_RG8_UNORM);
        CHECK(tex->plane_offsets[0] == 0u && tex->plane_offsets[1] >= tex->plane_bytes[0]);
        CHECK((tex->plane_offsets[1] % A840_RT_PAGE_BYTES) == 0u);
        CHECK((tex->tex_const[0][3] & A840_TEX_3_FMT_MAX) == A840_FMT6_8_UNORM);
        CHECK((tex->tex_const[1][3] & A840_TEX_3_FMT_MAX) == A840_FMT6_8_8_UNORM);
        a840_ubwc_texture_destroy(tex);
    }

    CASE("ycbcr_nv12_tiled_dual_plane_descriptors");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_YCBCR_MULTIPLANE;
        d.ycbcr_format = A840_YCBCR_NV12;
        d.width = 640u;
        d.height = 480u;
        d.tile_mode = A840_TILE_TILED;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
        CHECK((tex->tex_const[0][4] & A840_TEX_4_TILE_MODE_MAX) == A840_TILE6_3);
        CHECK((tex->tex_const[1][4] & A840_TEX_4_TILE_MODE_MAX) == A840_TILE6_3);
        a840_ubwc_texture_destroy(tex);
    }

    CASE("ycbcr_p010_linear_and_tiled_dual_plane_descriptors");
    {
        for (uint32_t tm : {A840_TILE_LINEAR, A840_TILE_TILED}) {
            A840UbwcTextureDesc d{};
            d.kind = A840_P3_TEX_YCBCR_MULTIPLANE;
            d.ycbcr_format = A840_YCBCR_P010;
            d.width = 640u;
            d.height = 360u;
            d.tile_mode = tm;
            A840UbwcTexture *tex = nullptr;
            CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
            CHECK(tex->plane_count == 2u);
            CHECK((tex->tex_const[0][3] & A840_TEX_3_FMT_MAX) == A840_FMT6_16_UNORM);
            CHECK((tex->tex_const[1][3] & A840_TEX_3_FMT_MAX) == A840_FMT6_16_16_UNORM);
            a840_ubwc_texture_destroy(tex);
        }
    }

    CASE("ycbcr_reject_odd_dimensions_and_ubwc");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_YCBCR_MULTIPLANE;
        d.ycbcr_format = A840_YCBCR_NV12;
        d.width = 121u;
        d.height = 120u;
        d.tile_mode = A840_TILE_LINEAR;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_INVALID_ARG);
        d.width = 120u;
        d.tile_mode = A840_TILE_UBWC;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_UNSUPPORTED);
    }

    CASE("ycbcr_scanout_never_certified_ubwc");
    {
        A840UbwcTextureDesc d{};
        d.kind = A840_P3_TEX_YCBCR_MULTIPLANE;
        d.ycbcr_format = A840_YCBCR_NV12;
        d.width = 320u;
        d.height = 240u;
        d.tile_mode = A840_TILE_LINEAR;
        d.for_scanout = true;
        A840UbwcTexture *tex = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &d, &tex) == A840_RT_OK);
        CHECK(!tex->scanout_eligible && tex->fallback_linear_required);
        a840_ubwc_texture_destroy(tex);
    }
}

void test_group4_pack_cache_rt()
{
    PipelineFixture f0, f1, f2, f_shared, f_wrong;
    f0.setup(1u);
    f1.setup(2u);
    f2.setup(3u);
    f_shared.setup(4u);
    f_wrong.setup(5u, kWrongStackKey);

    auto pack_t0 = build_pack(0u, {&f0, &f_shared});
    auto pack_t1 = build_pack(1u, {&f1, &f2, &f_shared});
    auto pack_bad_stack = build_pack(0u, {&f_wrong}, kWrongStackKey);

    constexpr uint64_t kT0GpuVa = UINT64_C(0x0000100000000000);
    constexpr uint64_t kT1GpuVa = UINT64_C(0x0000200000000000);

    A840PackCacheRt cache{};
    CHECK(a840_pack_cache_rt_init(&cache, kStackKey, a840_compile_abi_fingerprint()) == A840_RT_OK);

    CASE("pack_rt_mount_tier0_and_tier1_via_reply");
    {
        NativeAdrenoAbiSharedPackReplyV3 reply{};
        reply.gpu_va = kT0GpuVa;
        reply.cpu_va = reinterpret_cast<uint64_t>(pack_t0->data);
        reply.bytes = pack_t0->bytes;
        reply.actual_stack_key = kStackKey;
        CHECK(a840_pack_cache_rt_mount_tier0_reply(&cache, &reply, 1001u) == A840_RT_OK);
        CHECK(a840_pack_cache_rt_mount_tier1(&cache, pack_t1->data, pack_t1->bytes, kT1GpuVa, 1002u) == A840_RT_OK);
        CHECK(cache.tier0.mounted && cache.tier1.mounted);
    }

    CASE("pack_rt_lookup_tier0_hit_zero_copy_ptrs");
    {
        A840PackLookupResult res{};
        CHECK(a840_pack_cache_rt_lookup(&cache, &f0.rec.pipeline_key, &res) == A840_RT_OK);
        CHECK(res.hit_tier == A840_PACK_HIT_TIER0);
        auto base = reinterpret_cast<uintptr_t>(pack_t0->data);
        auto isa_p = reinterpret_cast<uintptr_t>(res.entry.stages[0].isa_words);
        CHECK(isa_p >= base && isa_p < base + pack_t0->bytes);
        CHECK((isa_p % A840_PACK_V1_ISA_ALIGNMENT) == 0u);
        CHECK(res.entry.stages[0].isa_words[0] == f0.isa[0][0]);
    }

    CASE("pack_rt_lookup_tier0_precedence_over_tier1");
    {
        auto before = a840_pack_cache_rt_stats(&cache);
        A840PackLookupResult res{};
        CHECK(a840_pack_cache_rt_lookup(&cache, &f_shared.rec.pipeline_key, &res) == A840_RT_OK);
        CHECK(res.hit_tier == A840_PACK_HIT_TIER0);
        auto after = a840_pack_cache_rt_stats(&cache);
        CHECK(after.tier0_hits == before.tier0_hits + 1u && after.tier1_hits == before.tier1_hits);
    }

    CASE("pack_rt_lookup_tier1_fallback_hit");
    {
        auto before = a840_pack_cache_rt_stats(&cache);
        A840PackLookupResult res{};
        CHECK(a840_pack_cache_rt_lookup(&cache, &f1.rec.pipeline_key, &res) == A840_RT_OK);
        CHECK(res.hit_tier == A840_PACK_HIT_TIER1);
        auto after = a840_pack_cache_rt_stats(&cache);
        CHECK(after.tier1_hits == before.tier1_hits + 1u && after.tier0_hits == before.tier0_hits);
    }

    CASE("pack_rt_lookup_miss_increments_misses");
    {
        A840PipelineKey absent{};
        absent.sha256[0] = 0xfeu;
        auto before = a840_pack_cache_rt_stats(&cache);
        A840PackLookupResult res{};
        CHECK(a840_pack_cache_rt_lookup(&cache, &absent, &res) == A840_RT_FALLBACK_REQUIRED);
        CHECK(res.hit_tier == A840_PACK_HIT_NONE);
        CHECK(a840_pack_cache_rt_stats(&cache).misses == before.misses + 1u);
    }

    CASE("pack_rt_gpu_va_conversion_isa_and_sds");
    {
        A840PackLookupResult res{};
        CHECK(a840_pack_cache_rt_lookup(&cache, &f0.rec.pipeline_key, &res) == A840_RT_OK);
        CHECK(res.stage_gpu[0].isa_gpu_va == kT0GpuVa + res.stage_gpu[0].isa_offset);
        CHECK((res.stage_gpu[0].isa_gpu_va % A840_PACK_V1_ISA_ALIGNMENT) == 0u);
        CHECK(res.stage_gpu[1].sds_gpu_va == kT0GpuVa + res.stage_gpu[1].sds_offset);
        A840ShaderCode sc{};
        CHECK(a840_pack_cache_rt_make_shader_code(&res, 0u, &f0.rec.stages[0], &sc) == A840_RT_OK);
        CHECK(sc.code.bo_handle == 1001u && sc.code.gpu_va == res.stage_gpu[0].isa_gpu_va);
        A840SdsGroupDesc sds{};
        CHECK(a840_pack_cache_rt_make_sds_group(&res, 1u, 3u, A840_SDS_GMEM, &sds) == A840_RT_OK);
        CHECK(sds.group_id == 3u && sds.state.gpu_va == res.stage_gpu[1].sds_gpu_va);
    }

    CASE("pack_rt_reject_stack_key_mismatch");
    {
        CHECK(a840_pack_cache_rt_mount_tier0(&cache, pack_bad_stack->data, pack_bad_stack->bytes, kT0GpuVa, 99u) == A840_RT_UNSUPPORTED);
        CHECK(cache.tier0.mounted && cache.tier0.bo_handle == 1001u);
    }

    CASE("pack_rt_reject_abi_fingerprint_mismatch");
    {
        A840PackCacheRt bad_abi{};
        CHECK(a840_pack_cache_rt_init(&bad_abi, kStackKey, a840_compile_abi_fingerprint() ^ 1u) == A840_RT_OK);
        CHECK(a840_pack_cache_rt_mount_tier0(&bad_abi, pack_t0->data, pack_t0->bytes, kT0GpuVa, 1u) == A840_RT_UNSUPPORTED);
        CHECK(!bad_abi.tier0.mounted);
    }

    CASE("pack_rt_reject_corrupted_sha256_and_wrong_tier");
    {
        CHECK(a840_pack_cache_rt_mount_tier0(&cache, pack_t1->data, pack_t1->bytes, kT1GpuVa, 1u) == A840_RT_INVALID_ARG);
        CHECK(a840_pack_cache_rt_mount_tier1(&cache, pack_t0->data, pack_t0->bytes, kT0GpuVa, 1u) == A840_RT_INVALID_ARG);
        AlignedPackBuf corrupt(pack_t0->bytes);
        std::memcpy(corrupt.data, pack_t0->data, pack_t0->bytes);
        corrupt.data[corrupt.bytes - 1u] ^= 0xffu;
        CHECK(a840_pack_cache_rt_mount_tier0(&cache, corrupt.data, corrupt.bytes, kT0GpuVa, 1u) == A840_RT_UNSUPPORTED);
    }

    CASE("pack_rt_unmount_and_remount_clean");
    {
        a840_pack_cache_rt_unmount_tier0(&cache);
        CHECK(!cache.tier0.mounted);
        A840PackLookupResult res{};
        CHECK(a840_pack_cache_rt_lookup(&cache, &f_shared.rec.pipeline_key, &res) == A840_RT_OK);
        CHECK(res.hit_tier == A840_PACK_HIT_TIER1);
        a840_pack_cache_rt_reset_stats(&cache);
        auto st = a840_pack_cache_rt_stats(&cache);
        CHECK(st.tier0_hits == 0u && st.tier1_hits == 0u && st.misses == 0u);
    }
}

void test_group5_router_compat()
{
    CASE("router_phase2_mask_unchanged_fallbacks");
    {
        for (uint32_t mask : {static_cast<uint32_t>(A840_ROUTE_PHASE2_ALL),
                              static_cast<uint32_t>(A840_ROUTE_ALL)}) {
            A840DualPathRouter r{};
            a840_router_init(&r, mask);
            A840RenderRouteQuery q{};
            q.color_attachment_count = 1u;
            q.color_mtl_formats[0] = A840_MTL_FMT_BGRA8_UNORM;
            q.has_memoryless_attachment = true;
            CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_GMEM_REQUIRED);
            q.has_memoryless_attachment = false;
            q.color_tile_modes[0] = A840_TILE_UBWC;
            CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_UBWC_ATTACHMENT);
            q.color_tile_modes[0] = A840_TILE_TILED;
            q.uses_programmable_blend = true;
            CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_FBFETCH_BLEND);
        }
    }

    CASE("router_phase3_gmem_memoryless_raw");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_RENDER_SYSMEM | A840_ROUTE_GMEM);
        A840RenderRouteQuery q{};
        q.color_attachment_count = 1u;
        q.color_mtl_formats[0] = A840_MTL_FMT_BGRA8_UNORM;
        q.color_tile_modes[0] = A840_TILE_TILED;
        q.has_memoryless_attachment = true;
        CHECK(a840_router_eval_render(&r, &q).path == A840_ROUTE_PATH_RAW);
        q.color_tile_modes[0] = A840_TILE_UBWC;
        CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_UBWC_ATTACHMENT);
    }

    CASE("router_phase3_ubwc_attachment_raw");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_RENDER_SYSMEM | A840_ROUTE_UBWC);
        A840RenderRouteQuery q{};
        q.color_attachment_count = 1u;
        q.color_mtl_formats[0] = A840_MTL_FMT_BGRA8_UNORM;
        q.color_tile_modes[0] = A840_TILE_UBWC;
        CHECK(a840_router_eval_render(&r, &q).path == A840_ROUTE_PATH_RAW);
        q.color_mtl_formats[0] = 1u;
        CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_UBWC_ATTACHMENT);
    }

    CASE("router_phase3_fbfetch_requires_gmem_and_fbfetch");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_RENDER_SYSMEM);
        a840_router_set_gmem_engine(&r, true);
        A840RenderRouteQuery q{};
        q.color_attachment_count = 1u;
        q.color_mtl_formats[0] = A840_MTL_FMT_BGRA8_UNORM;
        q.uses_fbfetch = true;
        q.uses_programmable_blend = true;
        CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_FBFETCH_BLEND);
        a840_router_enable_category(&r, A840_ROUTE_FBFETCH);
        CHECK(a840_router_eval_render(&r, &q).path == A840_ROUTE_PATH_RAW);
    }

    CASE("router_phase3_xr10_and_ycbcr_raw");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_RENDER_SYSMEM | A840_ROUTE_XR10_YCBCR);
        A840RenderRouteQuery q{};
        q.color_attachment_count = 1u;
        q.color_mtl_formats[0] = A840_MTL_FMT_BGR10_XR;
        CHECK(a840_router_eval_render(&r, &q).path == A840_ROUTE_PATH_RAW);
        q.color_mtl_formats[0] = A840_MTL_FMT_BGRA10_XR_SRGB;
        CHECK(a840_router_eval_render(&r, &q).path == A840_ROUTE_PATH_RAW);
        q.color_attachment_count = 0u;
        q.has_ycbcr_attachment = true;
        CHECK(a840_router_eval_render(&r, &q).path == A840_ROUTE_PATH_RAW);
    }

    CASE("router_phase4_predicates_remain_fallback");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, A840_ROUTE_PHASE2_ALL | A840_ROUTE_PHASE3_ALL);
        A840RenderRouteQuery q{};
        q.color_attachment_count = 1u;
        q.color_mtl_formats[0] = A840_MTL_FMT_BGRA8_UNORM;
        q.sample_count = 4u;
        q.has_msaa_resolve = true;
        CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_MSAA_RESOLVE);
        q.sample_count = 1u;
        q.has_msaa_resolve = false;
        q.uses_tessellation = true;
        CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_TESS_OR_AMPLIFY);
        q.uses_tessellation = false;
        q.uses_indirect_draw = true;
        CHECK(a840_router_eval_render(&r, &q).predicate_id == A840_PRED_RENDER_INDIRECT_OR_ICB);
    }
}

void test_group6_ca_liquid_glass(const char *dump_glass_pm4, const char *dump_draw_ib_pm4)
{
    PipelineFixture p0, p1, p2, p3;
    p0.setup(10u);
    p1.setup(11u);
    p2.setup(12u);
    p3.setup(13u);

    auto pack_t0 = build_pack(0u, {&p0, &p1, &p2});
    auto pack_t1 = build_pack(1u, {&p3});

    MockFixture mf(false);
    a840_ca_bridge_enable_phase3_router(a840_rt_device_router(mf.dev()));

    A840RtBuffer *t0_bo = mf.alloc_buf(pack_t0->bytes, A840_BUF_USAGE_READ | A840_BUF_USAGE_SHADER | A840_BUF_USAGE_CPU_VISIBLE);
    A840RtBuffer *t1_bo = mf.alloc_buf(pack_t1->bytes, A840_BUF_USAGE_READ | A840_BUF_USAGE_SHADER | A840_BUF_USAGE_CPU_VISIBLE);
    std::memcpy(t0_bo->cpu_ptr, pack_t0->data, pack_t0->bytes);
    std::memcpy(t1_bo->cpu_ptr, pack_t1->data, pack_t1->bytes);

    A840PackCacheRt pack_cache{};
    CHECK(a840_pack_cache_rt_init(&pack_cache, kStackKey, a840_compile_abi_fingerprint()) == A840_RT_OK);
    CHECK(a840_pack_cache_rt_mount_tier0(&pack_cache, t0_bo->cpu_ptr, pack_t0->bytes, t0_bo->gpu_va, t0_bo->bo_handle) == A840_RT_OK);
    CHECK(a840_pack_cache_rt_mount_tier1(&pack_cache, t1_bo->cpu_ptr, pack_t1->bytes, t1_bo->gpu_va, t1_bo->bo_handle) == A840_RT_OK);

    const uint32_t mdss_fmts[] = {A840_MTL_FMT_BGRA8_UNORM};
    A840MdssScanoutCaps mdss{A840_SCANOUT_CAP_UBWC6, 4096u, 4096u, 0u, 0u, mdss_fmts, 1u};

    A840UbwcTextureDesc col_d{};
    col_d.kind = A840_P3_TEX_UBWC6;
    col_d.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
    col_d.width = kScreenW;
    col_d.height = kScreenH;
    col_d.usage_flags = A840_TEX_USAGE_RENDER_TARGET | A840_TEX_USAGE_SHADER_READ;
    col_d.for_scanout = true;
    col_d.mdss_caps = &mdss;
    A840UbwcTexture *color_tex = nullptr;
    CHECK(a840_ubwc_texture_create(mf.dev(), &col_d, &color_tex) == A840_RT_OK);
    CHECK(color_tex->scanout_eligible);

    size_t bo_before_depth = a840_mock_transport_live_bo_count(mf.mock);
    A840UbwcTextureDesc dep_d{};
    dep_d.kind = A840_P3_TEX_MEMORYLESS;
    dep_d.storage_mode = A840_STORAGE_MEMORYLESS;
    dep_d.mtl_pixel_format = A840_MTL_FMT_DEPTH32_FLOAT;
    dep_d.width = kScreenW;
    dep_d.height = kScreenH;
    dep_d.usage_flags = A840_TEX_USAGE_RENDER_TARGET;
    A840UbwcTexture *depth_tex = nullptr;
    CHECK(a840_ubwc_texture_create(mf.dev(), &dep_d, &depth_tex) == A840_RT_OK);
    CHECK(a840_mock_transport_live_bo_count(mf.mock) == bo_before_depth);

    A840RtBuffer *main_ib = mf.alloc_buf(65536u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE);
    A840RtBuffer *draw_ib = mf.alloc_buf(65536u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE);

    A840CaFrameDesc frame{};
    frame.color_target = color_tex;
    frame.depth_target = depth_tex;
    frame.pack_cache = &pack_cache;
    frame.color_load = A840_LOAD_CLEAR;
    frame.color_store = A840_STORE_STORE;
    frame.clear_color[0] = 4278190335u;
    frame.clear_depth = 1065353216u;
    frame.pass_count = 4u;
    const A840PipelineKey keys[4] = {p0.rec.pipeline_key, p1.rec.pipeline_key, p2.rec.pipeline_key, p3.rec.pipeline_key};
    for (uint32_t i = 0; i < 4u; ++i) {
        frame.passes[i].pipeline_key = keys[i];
        frame.passes[i].uses_fbfetch = (i > 0u);
        frame.passes[i].depth_test = true;
        frame.passes[i].depth_write = (i == 0u);
        frame.passes[i].depth_compare = 3u;
        frame.passes[i].blend_enabled = (i > 0u);
        frame.passes[i].vertex_count = 6u;
        frame.passes[i].instance_count = 1u;
        frame.passes[i].emit_sds = true;
        frame.passes[i].sds_group_id = i;
    }

    CASE("ca_glass_1200x2608_4pass_fbfetch_submit_and_pm4_audit");
    {
        std::array<A840Reloc, 256> scratch{};
        A840RtBuffer *extra[] = {t0_bo, t1_bo};
        A840CaFrameFlight flight{};
        CHECK(a840_ca_bridge_submit_frame(&mf.gmem_q, &frame, main_ib, draw_ib,
                                          scratch.data(), scratch.size(),
                                          extra, 2u, &flight) == A840_RT_OK);
        CHECK(flight.stats.route_decision.path == A840_ROUTE_PATH_RAW);
        CHECK(flight.stats.gmem_plan.path == A840_PATH_GMEM);
        CHECK(flight.stats.gmem_plan.tiles_x == 2u && flight.stats.gmem_plan.tiles_y == 2u);
        CHECK(flight.stats.passes_encoded == 4u);
        CHECK(flight.stats.fbfetch_barriers_emitted == 3u);
        CHECK(flight.stats.sds_groups_emitted == 4u);
        CHECK(flight.stats.tier0_hits == 3u && flight.stats.tier1_hits == 1u);
        CHECK(flight.stats.draw_ib_wfi_count == 0u);
        CHECK(flight.stats.memoryless_reloc_count == 0u);

        A840MockSubmissionView view{};
        CHECK(a840_mock_transport_get_submission(mf.mock, 0u, &view) == A840_TRANSPORT_OK);
        CHECK(view.ib_count == 2u);
        uint32_t main_wfis = 0u, main_ibs = 0u, zero_bos = 0u;
        CHECK(a840_ca_bridge_audit_pm4(view.words, view.dword_count,
                                       scratch.data(), view.reloc_count,
                                       &main_wfis, &main_ibs, &zero_bos) == A840_RT_OK);
        CHECK(main_wfis == 4u && main_ibs == 4u && zero_bos == 0u);
        for (uint32_t r = 0; r < view.reloc_count; ++r) {
            uint64_t bo = scratch[r].bo_handle;
            CHECK(bo != 0u);
            if (r + 1u < view.reloc_count) {
                CHECK(bo == color_tex->backing_bo->bo_handle || bo == draw_ib->bo_handle);
            }
        }

        if (dump_glass_pm4) {
            CHECK(a840_mock_transport_dump_pm4(mf.mock, dump_glass_pm4) == A840_TRANSPORT_OK);
        }
        if (dump_draw_ib_pm4) {
            write_pm4_file(dump_draw_ib_pm4,
                           static_cast<const uint32_t *>(draw_ib->cpu_ptr),
                           flight.stats.draw_ib_dwords);
        }
        std::printf("CA LiquidGlass 1200x2608: tiles=%ux%u used=%u passes=%u fbfetch_barriers=%u sds=%u main_dwords=%zu draw_dwords=%u main_wfis=%u draw_wfis=%u\n",
                    flight.stats.gmem_plan.tiles_x, flight.stats.gmem_plan.tiles_y,
                    flight.stats.gmem_plan.used_bytes, flight.stats.passes_encoded,
                    flight.stats.fbfetch_barriers_emitted, flight.stats.sds_groups_emitted,
                    view.dword_count, flight.stats.draw_ib_dwords,
                    main_wfis, flight.stats.draw_ib_wfi_count);

        a840_mock_transport_complete_all(mf.mock);
        CHECK(a840_ca_bridge_retire_frame(&flight, 1000000u) == A840_RT_OK);
    }

    CASE("ca_glass_rejects_phase2_only_router_zero_silent_fallback");
    {
        A840DualPathRouter p2_router{};
        a840_router_init(&p2_router, A840_ROUTE_PHASE2_ALL);
        A840CaFrameDesc f_p2 = frame;
        f_p2.router = &p2_router;
        std::array<A840Reloc, 256> scratch{};
        A840CaFrameFlight flight{};
        size_t subs_before = a840_mock_transport_submission_count(mf.mock);
        CHECK(a840_ca_bridge_submit_frame(&mf.gmem_q, &f_p2, main_ib, draw_ib,
                                          scratch.data(), scratch.size(),
                                          nullptr, 0u, &flight) == A840_RT_FALLBACK_REQUIRED);
        CHECK(flight.stats.route_decision.predicate_id == A840_PRED_RENDER_UBWC_ATTACHMENT);
        CHECK(a840_mock_transport_submission_count(mf.mock) == subs_before);
    }

    CASE("ca_glass_scanout_fallback_linear_resolve_submit");
    {
        A840UbwcTextureDesc fb_d = col_d;
        fb_d.mdss_caps = nullptr;
        A840UbwcTexture *fb_col = nullptr;
        CHECK(a840_ubwc_texture_create(mf.dev(), &fb_d, &fb_col) == A840_RT_OK);
        CHECK(!fb_col->scanout_eligible && fb_col->fallback_linear_required && fb_col->linear_resolve_bo != nullptr);
        A840CaFrameDesc f_lin = frame;
        f_lin.color_target = fb_col;
        std::array<A840Reloc, 256> scratch{};
        A840CaFrameFlight flight{};
        CHECK(a840_ca_bridge_submit_frame(&mf.gmem_q, &f_lin, main_ib, draw_ib,
                                          scratch.data(), scratch.size(),
                                          nullptr, 0u, &flight) == A840_RT_OK);
        a840_mock_transport_complete_all(mf.mock);
        CHECK(a840_ca_bridge_retire_frame(&flight, 1000000u) == A840_RT_OK);
        a840_ubwc_texture_destroy(fb_col);
    }

    CASE("ca_glass_pack_miss_rejects_atomically_no_partial_submit");
    {
        A840CaFrameDesc f_miss = frame;
        f_miss.passes[2].pipeline_key.sha256[0] = 0xeeu;
        std::array<A840Reloc, 256> scratch{};
        A840CaFrameFlight flight{};
        size_t subs_before = a840_mock_transport_submission_count(mf.mock);
        CHECK(a840_ca_bridge_submit_frame(&mf.gmem_q, &f_miss, main_ib, draw_ib,
                                          scratch.data(), scratch.size(),
                                          nullptr, 0u, &flight) == A840_RT_FALLBACK_REQUIRED);
        CHECK(a840_mock_transport_submission_count(mf.mock) == subs_before);
    }

    CASE("ca_glass_multi_frame_retire_zero_bo_leak");
    {
        size_t bo_steady = a840_mock_transport_live_bo_count(mf.mock);
        for (int iter = 0; iter < 3; ++iter) {
            std::array<A840Reloc, 256> scratch{};
            A840CaFrameFlight flight{};
            CHECK(a840_ca_bridge_submit_frame(&mf.gmem_q, &frame, main_ib, draw_ib,
                                              scratch.data(), scratch.size(),
                                              nullptr, 0u, &flight) == A840_RT_OK);
            a840_mock_transport_complete_all(mf.mock);
            CHECK(a840_ca_bridge_retire_frame(&flight, 1000000u) == A840_RT_OK);
        }
        CHECK(a840_mock_transport_live_bo_count(mf.mock) == bo_steady);
    }

    a840_ubwc_texture_destroy(depth_tex);
    a840_ubwc_texture_destroy(color_tex);
}

} // namespace

int main(int argc, char **argv)
{
    const char *dump_glass_pm4 = nullptr;
    const char *dump_draw_ib_pm4 = nullptr;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-ca-glass-pm4") == 0) {
            dump_glass_pm4 = argv[++i];
        } else if (std::strcmp(argv[i], "--dump-draw-ib-pm4") == 0) {
            dump_draw_ib_pm4 = argv[++i];
        }
    }

    test_group1_ubwc6();
    test_group2_memoryless();
    test_group3_xr10_and_ycbcr();
    test_group4_pack_cache_rt();
    test_group5_router_compat();
    test_group6_ca_liquid_glass(dump_glass_pm4, dump_draw_ib_pm4);

    CHECK(g_cases >= 45u);
    std::printf("PASS test_phase3_runtime_integration: %u cases, %u checks\n", g_cases, g_checks);
    return 0;
}
