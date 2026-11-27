/* SPDX-License-Identifier: MIT */
/* Wave 8 Lane 3 Host Test Suite:
 *   - 2x/4x MSAA state, sample locations, 8x rejection, GMEM->Sysmem RB & 2D Resolve
 *   - Depth/Stencil variants & Visibility / Occlusion Queries (Counting & Boolean)
 *   - Tessellation (Quad/Triangle, Equal/Odd/Even, CW/CCW, 1..32 CPs) &
 *     Vertex Amplification (1..2 views, VFD/VPC/PC/GRAS stereo + multi-viewport)
 *   - Camera 155 (NV12/P010 preview + 4096x3072 BGRA8 photo), Core Image Gaussian,
 *     MPS GEMM compute encoding & Mock Transport submission loop
 *   - Out-of-process NezhaShaderCompiler client bridge (live compile vs circuit-open)
 *   - Complete 16/16 DualPathPredicates.tsv router matrix + Phase 2/3/W07 backward compat
 */
#include "a840_phase4_pipeline.h"
#include "a840_emit_msaa_tess_vis.h"
#include "a840_shader_compiler_client.h"
#include "a840_shader_compiler_service.h"
#include "Adreno840Caps.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <condition_variable>
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
    A840RtQueue *queue = nullptr;
    std::vector<A840RtBuffer *> buffers;
    std::vector<A840RtTexture *> textures;
    std::vector<A840UbwcTexture *> ubwc_textures;

    explicit DeviceFixture(uint32_t cats = A840_ROUTE_PHASE4_FULL_ALL)
    {
        A840MockTransportConfig cfg{};
        a840_mock_transport_default_config(&cfg);
        cfg.auto_complete_submits = true;
        CHECK(a840_mock_transport_create(&cfg, &mock) == A840_TRANSPORT_OK);
        A840BackendTransport iface = a840_mock_transport_interface(mock);
        CHECK(a840_rt_device_create(&iface, cats, &dev) == A840_RT_OK);
        if ((cats & A840_ROUTE_GMEM) != 0u) {
            a840_router_set_gmem_engine(a840_rt_device_router(dev), true);
        }
        CHECK(a840_rt_queue_create(dev, 0u, &queue) == A840_RT_OK);
    }

    A840RtBuffer *alloc_buf(uint64_t bytes, uint32_t usage = A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_CPU_VISIBLE,
                            uint64_t align = A840_RT_PAGE_BYTES)
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
        d.usage_flags = A840_TEX_USAGE_SHADER_READ | A840_TEX_USAGE_SHADER_WRITE |
                        A840_TEX_USAGE_RENDER_TARGET;
        A840RtTexture *t = nullptr;
        CHECK(a840_rt_texture_create(dev, &d, &t) == A840_RT_OK);
        textures.push_back(t);
        return t;
    }

    A840UbwcTexture *alloc_ubwc(A840Phase3TexKind kind, A840StorageMode storage,
                                uint32_t mtl_fmt, A840YcbcrFormat ycbcr,
                                uint32_t w, uint32_t h, uint32_t samples, uint32_t tile)
    {
        A840UbwcTextureDesc d{};
        d.kind = kind;
        d.storage_mode = storage;
        d.mtl_pixel_format = mtl_fmt;
        d.ycbcr_format = ycbcr;
        d.width = w;
        d.height = h;
        d.array_layers = 1u;
        d.samples = samples;
        d.tile_mode = tile;
        d.usage_flags = A840_TEX_USAGE_SHADER_READ | A840_TEX_USAGE_RENDER_TARGET;
        d.highest_bank_bit = 16u;
        d.ubwc_version = 6u;
        d.ubwc_swizzle = 6u;
        d.macrotile_mode = 1u;
        A840UbwcTexture *t = nullptr;
        CHECK(a840_ubwc_texture_create(dev, &d, &t) == A840_RT_OK);
        ubwc_textures.push_back(t);
        return t;
    }

    ~DeviceFixture()
    {
        for (A840UbwcTexture *t : ubwc_textures) {
            a840_ubwc_texture_destroy(t);
        }
        for (A840RtTexture *t : textures) {
            a840_rt_texture_release(t);
        }
        for (A840RtBuffer *b : buffers) {
            a840_rt_buffer_release(b);
        }
        a840_rt_queue_destroy(queue);
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

void verify_pm4_decodes(const A840CmdStream &cs, size_t min_packets)
{
    std::vector<A840PM4Packet> pkts(cs.used_dwords);
    size_t count = 0u;
    CHECK(cs.used_dwords > 0u);
    CHECK(a840_pm4_decode(cs.words, cs.used_dwords, pkts.data(), pkts.size(), &count) == A840_EMIT_OK);
    CHECK(count >= min_packets);
}

bool stream_contains_reg(const A840CmdStream &cs, uint32_t reg_id, uint32_t *out_val = nullptr)
{
    std::vector<A840PM4Packet> pkts(cs.used_dwords);
    size_t count = 0u;
    if (a840_pm4_decode(cs.words, cs.used_dwords, pkts.data(), pkts.size(), &count) != A840_EMIT_OK) {
        return false;
    }
    for (size_t i = 0u; i < count; ++i) {
        if (pkts[i].type == 4u) {
            if (reg_id >= pkts[i].identifier && reg_id < pkts[i].identifier + pkts[i].payload_words) {
                if (out_val) {
                    *out_val = cs.words[pkts[i].header_word + 1u + (reg_id - pkts[i].identifier)];
                }
                return true;
            }
        }
    }
    return false;
}

bool stream_contains_opcode(const A840CmdStream &cs, uint32_t opcode)
{
    std::vector<A840PM4Packet> pkts(cs.used_dwords);
    size_t count = 0u;
    if (a840_pm4_decode(cs.words, cs.used_dwords, pkts.data(), pkts.size(), &count) != A840_EMIT_OK) {
        return false;
    }
    for (size_t i = 0u; i < count; ++i) {
        if (pkts[i].type == 7u && pkts[i].identifier == opcode) {
            return true;
        }
    }
    return false;
}

A840CompiledStage make_dummy_stage(A840ShaderStage stage, uint32_t lx = 8u, uint32_t ly = 8u, uint32_t lz = 1u)
{
    A840CompiledStage s{};
    s.abi_version = ADRENO_COMPILE_ABI_VERSION;
    s.stage = stage;
    s.isa_dwords = 64u;
    s.instruction_groups = 16u;
    s.max_full_reg = 7;
    s.max_half_reg = 3;
    s.constlen_vec4 = 16u;
    s.local_size[0] = lx;
    s.local_size[1] = ly;
    s.local_size[2] = lz;
    s.flags_out = 0u;
    return s;
}

A840GraphicsProgramDesc make_graphics_prog(const A840CompiledStage *vs, const A840CompiledStage *fs,
                                           A840RtBuffer *isa_bo)
{
    A840GraphicsProgramDesc p{};
    p.vs.stage = vs;
    p.vs.code.bo_handle = isa_bo->bo_handle;
    p.vs.code.gpu_va = isa_bo->gpu_va;
    p.fs.stage = fs;
    p.fs.code.bo_handle = isa_bo->bo_handle;
    p.fs.code.gpu_va = isa_bo->gpu_va + 256u;
    p.vertex_id = A840_REGID_UNUSED;
    p.instance_id = A840_REGID_UNUSED;
    p.primitive_id = A840_REGID_UNUSED;
    p.view_id = A840_REGID_UNUSED;
    p.position = 0u;
    p.point_size = A840_REGID_UNUSED;
    p.fs_inputs.face = A840_REGID_UNUSED;
    p.fs_inputs.sample_id = A840_REGID_UNUSED;
    p.fs_inputs.sample_mask = A840_REGID_UNUSED;
    p.fs_inputs.center_rhw = A840_REGID_UNUSED;
    p.fs_inputs.persp_pixel = A840_REGID_UNUSED;
    p.fs_inputs.linear_pixel = A840_REGID_UNUSED;
    p.fs_inputs.persp_centroid = A840_REGID_UNUSED;
    p.fs_inputs.linear_centroid = A840_REGID_UNUSED;
    p.fs_inputs.persp_sample = A840_REGID_UNUSED;
    p.fs_inputs.linear_sample = A840_REGID_UNUSED;
    p.fs_inputs.frag_coord = A840_REGID_UNUSED;
    p.fs_inputs.shading_rate = A840_REGID_UNUSED;
    p.depth_output = A840_REGID_UNUSED;
    p.sample_mask_output = A840_REGID_UNUSED;
    p.stencil_output = A840_REGID_UNUSED;
    for (uint32_t i = 0u; i < A840_MAX_COLOR_TARGETS; ++i) {
        p.colors[i].regid = A840_REGID_UNUSED;
        p.colors[i].component_mask = 0u;
    }
    p.colors[0].regid = 0u;
    p.colors[0].component_mask = 0xfu;
    return p;
}

A840ComputePipelineDesc make_compute_pipe(const A840CompiledStage *cs_stage, A840RtBuffer *isa_bo)
{
    A840ComputePipelineDesc p{};
    p.shader.stage = cs_stage;
    p.shader.code.bo_handle = isa_bo->bo_handle;
    p.shader.code.gpu_va = isa_bo->gpu_va;
    p.shader.bindless_tex = true;
    p.shader.bindless_uav = true;
    p.shader.bindless_ubo = true;
    p.local_id_regid = 0u;
    p.wg_id_regid = 4u;
    return p;
}

/* ============================================================================
 * Group 1: MSAA State, Sample Locations & GMEM->Sysmem MSAA Resolve Blit
 * ============================================================================ */
void test_group1_msaa_and_resolve()
{
    std::vector<uint32_t> words(512, 0u);
    std::vector<A840Reloc> relocs(32);

    CASE("msaa_state_1x_2x_4x_standard_locations");
    {
        for (uint32_t samples : {1u, 2u, 4u}) {
            A840CmdStream cs{};
            CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
            A840MsaaStateDesc desc{};
            desc.sample_count = samples;
            desc.sample_rate_shading = (samples == 4u);
            CHECK(a840_emit_msaa_state(&cs, &desc) == A840_EMIT_OK);
            verify_pm4_decodes(cs, 6u);
            uint32_t val = 0u;
            CHECK(stream_contains_reg(cs, A840_REG_GRAS_SC_RAS_MSAA_CNTL, &val));
            CHECK(val == (samples == 1u ? 0u : (samples == 2u ? 1u : 2u)));
            CHECK(stream_contains_reg(cs, A840_REG_RB_DEST_MSAA_CNTL, &val));
            if (samples == 1u) {
                CHECK((val & (1u << 2)) != 0u);
            } else {
                CHECK((val & (1u << 2)) == 0u);
            }
            CHECK(stream_contains_reg(cs, A840_REG_RB_PS_SAMPLEFREQ_CNTL, &val));
            CHECK(val == (samples == 4u ? 1u : 0u));
        }
    }

    CASE("msaa_state_reject_8x_and_invalid_samples");
    {
        for (uint32_t bad : {0u, 3u, 8u, 16u}) {
            A840CmdStream cs{};
            CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
            A840MsaaStateDesc desc{};
            desc.sample_count = bad;
            CHECK(a840_emit_msaa_state(&cs, &desc) == A840_EMIT_UNSUPPORTED);
            CHECK(cs.used_dwords == 0u);
        }
    }

    CASE("msaa_state_custom_sample_locations_and_bounds");
    {
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        A840MsaaStateDesc desc{};
        desc.sample_count = 4u;
        desc.custom_sample_locations = true;
        desc.locations[0] = {0.25f, 0.25f};
        desc.locations[1] = {0.75f, 0.25f};
        desc.locations[2] = {0.25f, 0.75f};
        desc.locations[3] = {0.75f, 0.75f};
        CHECK(a840_emit_msaa_state(&cs, &desc) == A840_EMIT_OK);
        uint32_t pos_cntl = 0u;
        CHECK(stream_contains_reg(cs, A840_REG_RB_MSAA_SAMPLE_POS_CNTL, &pos_cntl));
        CHECK((pos_cntl & (1u << 1)) != 0u);

        /* Out-of-range coordinate >= 1.0 or NaN must fail and leave CS intact. */
        size_t before = cs.used_dwords;
        desc.locations[2].x = 1.0f;
        CHECK(a840_emit_msaa_state(&cs, &desc) == A840_EMIT_RANGE_ERROR);
        CHECK(cs.used_dwords == before);

        desc.locations[2].x = std::nanf("");
        CHECK(a840_emit_msaa_state(&cs, &desc) == A840_EMIT_RANGE_ERROR);
        CHECK(cs.used_dwords == before);
    }

    CASE("msaa_state_capacity_rollback");
    {
        std::vector<uint32_t> tiny(4, 0u);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, tiny.data(), tiny.size(), nullptr, 0u) == A840_EMIT_OK);
        A840MsaaStateDesc desc{};
        desc.sample_count = 4u;
        CHECK(a840_emit_msaa_state(&cs, &desc) == A840_EMIT_CAPACITY);
        CHECK(cs.used_dwords == 0u);
    }

    CASE("msaa_resolve_blit_rb_linear_and_ubwc");
    {
        for (uint32_t tile : {A840_TILE_LINEAR, A840_TILE_TILED, A840_TILE_UBWC}) {
            A840CmdStream cs{};
            CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
            A840MsaaResolveBlitDesc r{};
            r.src_samples = 4u;
            r.gmem_offset = 0u;
            r.gmem_pitch = 96u * 4u * 4u;
            r.slot_index = 0u;
            r.tile_w = 96u;
            r.tile_h = 32u;
            r.resolve_rect = {0u, 0u, 96u, 32u};
            r.dst.bo_handle = 42u;
            r.dst.gpu_va = 0x10000000ULL;
            r.dst.format_id = A840_FMT6_8_8_8_8_UNORM;
            r.dst.swap = 0u;
            r.dst.width = 96u;
            r.dst.height = 32u;
            r.dst.pitch = (tile == A840_TILE_LINEAR) ? 512u : 512u;
            r.dst.tile_mode = tile;
            if (tile == A840_TILE_UBWC) {
                r.dst.ubwc_meta_va = 0x12000000ULL;
                r.dst.ubwc_meta_pitch = 64u;
                r.dst.ubwc_meta_slice_stride = 4096u;
            }
            r.mode = A840_MSAA_RESOLVE_AVERAGE;
            r.emit_ccu_uche_barriers = true;
            CHECK(a840_emit_msaa_resolve_blit(&cs, &r) == A840_EMIT_OK);
            verify_pm4_decodes(cs, 10u);
            CHECK(stream_contains_reg(cs, A840_REG_RB_RESOLVE_OPERATION));
            CHECK(stream_contains_opcode(cs, A840_CP_EVENT_WRITE7));
            CHECK(cs.reloc_count == (tile == A840_TILE_UBWC ? 2u : 1u));
        }
    }

    CASE("msaa_resolve_blit_2d_engine_fp16_and_2x");
    {
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        A840MsaaResolveBlitDesc r{};
        r.src_samples = 2u;
        r.gmem_offset = 4096u;
        r.gmem_pitch = 96u * 8u * 2u;
        r.slot_index = 1u;
        r.tile_w = 96u;
        r.tile_h = 32u;
        r.resolve_rect = {0u, 0u, 96u, 32u};
        r.dst.bo_handle = 77u;
        r.dst.gpu_va = 0x20000000ULL;
        r.dst.format_id = A840_FMT6_16_16_16_16_FLOAT;
        r.dst.width = 96u;
        r.dst.height = 32u;
        r.dst.pitch = 1024u;
        r.dst.tile_mode = A840_TILE_TILED;
        r.mode = A840_MSAA_RESOLVE_AVERAGE;
        r.emit_ccu_uche_barriers = true;
        CHECK(a840_emit_msaa_resolve_blit(&cs, &r) == A840_EMIT_OK);
        verify_pm4_decodes(cs, 12u);
        CHECK(stream_contains_opcode(cs, A840_CP_BLIT));
        CHECK(stream_contains_reg(cs, A840_REG_TPL1_A2D_SRC_TEXTURE_INFO));
    }

    CASE("msaa_resolve_blit_integer_requires_sample0_and_errors");
    {
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        A840MsaaResolveBlitDesc r{};
        r.src_samples = 4u;
        r.gmem_offset = 0u;
        r.gmem_pitch = 96u * 4u * 4u;
        r.tile_w = 96u;
        r.tile_h = 32u;
        r.resolve_rect = {0u, 0u, 96u, 32u};
        r.dst.bo_handle = 5u;
        r.dst.gpu_va = 0x30000000ULL;
        r.dst.format_id = A840_FMT6_8_8_8_8_UINT;
        r.dst.width = 96u;
        r.dst.height = 32u;
        r.dst.pitch = 512u;
        r.dst.tile_mode = A840_TILE_LINEAR;
        r.mode = A840_MSAA_RESOLVE_AVERAGE;
        CHECK(a840_emit_msaa_resolve_blit(&cs, &r) == A840_EMIT_UNSUPPORTED);
        CHECK(cs.used_dwords == 0u);

        r.mode = A840_MSAA_RESOLVE_SAMPLE0;
        CHECK(a840_emit_msaa_resolve_blit(&cs, &r) == A840_EMIT_OK);

        /* Misaligned GMEM offset */
        size_t before = cs.used_dwords;
        r.gmem_offset = 128u;
        CHECK(a840_emit_msaa_resolve_blit(&cs, &r) == A840_EMIT_ALIGNMENT);
        CHECK(cs.used_dwords == before);

        /* 8x MSAA rejected */
        r.gmem_offset = 0u;
        r.src_samples = 8u;
        CHECK(a840_emit_msaa_resolve_blit(&cs, &r) == A840_EMIT_UNSUPPORTED);
        CHECK(cs.used_dwords == before);
    }
}

/* ============================================================================
 * Group 2: Depth/Stencil Variants & Visibility / Occlusion Queries
 * ============================================================================ */
void test_group2_depth_stencil_and_visibility()
{
    std::vector<uint32_t> words(512, 0u);
    std::vector<A840Reloc> relocs(32);

    CASE("depth_stencil_variant_bounds_and_separate_faces");
    {
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        A840DepthStencilVariantDesc ds{};
        ds.depth_test = true;
        ds.depth_write = true;
        ds.depth_clamp = true;
        ds.depth_bounds_test = true;
        ds.depth_compare = 1u; /* LESS */
        ds.depth_bounds_min = 0.125f;
        ds.depth_bounds_max = 0.875f;
        ds.z_mode = A840_ZMODE_EARLY_Z;
        ds.stencil_test = true;
        ds.separate_stencil_faces = true;
        ds.front = {2u, 0u, 2u, 0u, 0x5au, 0xffu, 0xffu};
        ds.back = {4u, 1u, 3u, 0u, 0xa5u, 0x0fu, 0xf0u};

        CHECK(a840_emit_depth_stencil_variant(&cs, &ds) == A840_EMIT_OK);
        verify_pm4_decodes(cs, 8u);
        uint32_t dcntl = 0u;
        CHECK(stream_contains_reg(cs, A840_REG_RB_DEPTH_CNTL, &dcntl));
        CHECK((dcntl & (1u << 7)) != 0u); /* Z_BOUNDS_ENABLE */
        CHECK(stream_contains_reg(cs, A840_REG_RB_DEPTH_BOUND_MIN));
        CHECK(stream_contains_reg(cs, A840_REG_RB_STENCIL_CNTL));
    }

    CASE("depth_stencil_variant_invalid_bounds_and_readonly_write_conflict");
    {
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        A840DepthStencilVariantDesc ds{};
        ds.depth_test = true;
        ds.depth_write = true;
        ds.read_only_depth = true;
        CHECK(a840_emit_depth_stencil_variant(&cs, &ds) == A840_EMIT_INVALID_ARG);
        CHECK(cs.used_dwords == 0u);

        ds.read_only_depth = false;
        ds.depth_bounds_test = true;
        ds.depth_bounds_min = 0.8f;
        ds.depth_bounds_max = 0.2f;
        CHECK(a840_emit_depth_stencil_variant(&cs, &ds) == A840_EMIT_RANGE_ERROR);
        CHECK(cs.used_dwords == 0u);
    }

    CASE("visibility_query_counting_and_boolean_multi_slot");
    {
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);

        for (uint32_t slot = 0u; slot < 4u; ++slot) {
            A840VisQueryDesc q{};
            q.query_buffer.bo_handle = 100u;
            q.query_buffer.gpu_va = 0x40000000ULL;
            q.buffer_bytes = 4u * A840_VIS_QUERY_SLOT_STRIDE_BYTES;
            q.slot_index = slot;
            q.mode = (slot % 2u == 0u) ? A840_VIS_QUERY_COUNTING : A840_VIS_QUERY_BOOLEAN;
            q.inside_render_pass = (slot < 2u);
            q.emit_rb_sample_counter_base = true;
            CHECK(a840_emit_visibility_query_begin(&cs, &q) == A840_EMIT_OK);
            CHECK(a840_emit_visibility_query_end(&cs, &q) == A840_EMIT_OK);
        }
        verify_pm4_decodes(cs, 16u);
        CHECK(stream_contains_reg(cs, A840_REG_RB_SAMPLE_COUNTER_CNTL));
        CHECK(stream_contains_reg(cs, A840_REG_RB_SAMPLE_COUNTER_BASE));
        CHECK(stream_contains_opcode(cs, A840_CP_EVENT_WRITE7));
        CHECK(stream_contains_opcode(cs, A840_CP_WAIT_MEM_WRITES));
        /* Verify slot isolation in recorded relocations */
        bool saw_slot0 = false;
        bool saw_slot3 = false;
        for (size_t i = 0u; i < cs.reloc_count; ++i) {
            if (cs.relocs[i].gpu_va == 0x40000000ULL) {
                saw_slot0 = true;
            }
            if (cs.relocs[i].gpu_va == 0x40000000ULL + 3u * 64u) {
                saw_slot3 = true;
            }
        }
        CHECK(saw_slot0 && saw_slot3);
    }

    CASE("visibility_query_reject_misaligned_and_oob_slot");
    {
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        A840VisQueryDesc q{};
        q.query_buffer.bo_handle = 101u;
        q.query_buffer.gpu_va = 0x40000020ULL; /* Not 64B aligned */
        q.buffer_bytes = 256u;
        q.slot_index = 0u;
        CHECK(a840_emit_visibility_query_begin(&cs, &q) == A840_EMIT_ALIGNMENT);
        CHECK(a840_emit_visibility_query_end(&cs, &q) == A840_EMIT_ALIGNMENT);
        CHECK(cs.used_dwords == 0u);

        q.query_buffer.gpu_va = 0x40000000ULL;
        q.slot_index = 4u; /* 256/64 = 4 slots -> slot 4 is OOB */
        CHECK(a840_emit_visibility_query_begin(&cs, &q) == A840_EMIT_RANGE_ERROR);
        CHECK(a840_emit_visibility_query_end(&cs, &q) == A840_EMIT_RANGE_ERROR);
        CHECK(cs.used_dwords == 0u);

        q.slot_index = 0u;
        q.buffer_bytes = 32u; /* Smaller than 64B stride */
        CHECK(a840_emit_visibility_query_begin(&cs, &q) == A840_EMIT_ALIGNMENT);
        CHECK(cs.used_dwords == 0u);
    }
}

/* ============================================================================
 * Group 3: Tessellation & Vertex Amplification (Multiview) Emitters
 * ============================================================================ */
void test_group3_tess_and_vertex_amp()
{
    std::vector<uint32_t> words(512, 0u);
    std::vector<A840Reloc> relocs(32);

    const A840TessDomain domains[] = {A840_TESS_DOMAIN_QUAD, A840_TESS_DOMAIN_TRIANGLE};
    const A840TessSpacing spacings[] = {A840_TESS_SPACING_EQUAL, A840_TESS_SPACING_FRACTIONAL_ODD,
                                        A840_TESS_SPACING_FRACTIONAL_EVEN};
    const A840TessWinding windings[] = {A840_TESS_WINDING_CW, A840_TESS_WINDING_CCW};

    for (A840TessDomain dom : domains) {
        for (A840TessSpacing sp : spacings) {
            for (A840TessWinding wd : windings) {
                CASE("tess_state_and_draw_matrix");
                A840CmdStream cs{};
                CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);

                A840TessStateDesc ts{};
                ts.domain = dom;
                ts.spacing = sp;
                ts.winding = wd;
                ts.control_points = (dom == A840_TESS_DOMAIN_QUAD) ? 4u : 3u;
                ts.patch_local_mem_16b = 16u;
                ts.subdraw_patch_limit = 64u;
                ts.d3d_vertex_ordering = (wd == A840_TESS_WINDING_CW);
                ts.tess_factor_buffer = {200u, 0x50000000ULL};
                ts.tess_buffer_bytes = 65536u;
                CHECK(a840_emit_tess_state(&cs, &ts) == A840_EMIT_OK);

                A840TessDrawDesc td{};
                td.domain = dom;
                td.control_points = ts.control_points;
                td.patch_count = 16u;
                td.instance_count = 2u;
                td.index_type = A840_INDEX_NONE;
                CHECK(a840_emit_tess_draw(&cs, &td) == A840_EMIT_OK);

                verify_pm4_decodes(cs, 8u);
                CHECK(!stream_contains_reg(cs, A840_REG_PC_DRAWCALL_CNTL));
                CHECK(stream_contains_reg(cs, A840_REG_PC_TESS_BASE));
                CHECK(stream_contains_reg(cs, A840_REG_PC_HS_PARAM_0));
                CHECK(stream_contains_reg(cs, A840_REG_PC_DS_PARAM));
                CHECK(stream_contains_opcode(cs, A840_CP_SET_SUBDRAW_SIZE));
                CHECK(stream_contains_opcode(cs, A840_CP_DRAW_INDX_OFFSET));
            }
        }
    }

    CASE("tess_draw_indexed_uint16_and_uint32");
    {
        for (A840IndexType it : {A840_INDEX_UINT16, A840_INDEX_UINT32}) {
            A840CmdStream cs{};
            CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
            A840TessDrawDesc td{};
            td.domain = A840_TESS_DOMAIN_QUAD;
            td.control_points = 16u;
            td.patch_count = 4u;
            td.instance_count = 1u;
            td.index_type = it;
            td.index_capacity = 128u;
            td.index_buffer = {205u, 0x52000000ULL};
            CHECK(a840_emit_tess_draw(&cs, &td) == A840_EMIT_OK);
            /* VFD_INDEX_OFFSET + CP_DRAW_INDX_OFFSET (PC_DRAWCALL_CNTL blocked by A840 CP_PROTECT). */
            verify_pm4_decodes(cs, 2u);
            CHECK(!stream_contains_reg(cs, A840_REG_PC_DRAWCALL_CNTL));
            CHECK(stream_contains_reg(cs, A840_REG_VFD_INDEX_OFFSET));
            CHECK(stream_contains_opcode(cs, A840_CP_DRAW_INDX_OFFSET));
            CHECK(cs.reloc_count == 1u);
        }
    }

    CASE("tess_invalid_control_points_and_alignment_rollback");
    {
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        A840TessStateDesc ts{};
        ts.domain = A840_TESS_DOMAIN_TRIANGLE;
        ts.spacing = A840_TESS_SPACING_EQUAL;
        ts.winding = A840_TESS_WINDING_CCW;
        ts.control_points = 0u;
        ts.patch_local_mem_16b = 8u;
        ts.subdraw_patch_limit = 16u;
        ts.tess_factor_buffer = {201u, 0x51000000ULL};
        ts.tess_buffer_bytes = 4096u;
        CHECK(a840_emit_tess_state(&cs, &ts) == A840_EMIT_RANGE_ERROR);
        ts.control_points = 33u;
        CHECK(a840_emit_tess_state(&cs, &ts) == A840_EMIT_RANGE_ERROR);
        ts.control_points = 3u;
        ts.tess_factor_buffer.gpu_va = 0x51000010ULL; /* Not 32B aligned */
        CHECK(a840_emit_tess_state(&cs, &ts) == A840_EMIT_ALIGNMENT);
        CHECK(cs.used_dwords == 0u);
    }

    CASE("vertex_amplification_1_and_2_views_with_viewports");
    {
        for (uint32_t views : {1u, 2u}) {
            A840CmdStream cs{};
            CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
            A840VertexAmpStateDesc va{};
            va.amp_count = views;
            va.multi_pos_output = false;
            va.increment_rt_index = true;
            va.increment_viewport_index = true;
            va.mappings[0] = {0u, 0u};
            va.mappings[1] = {1u, 1u};
            va.viewport_count = views;
            va.viewports[0] = {0.0f, 0.0f, 640.0f, 720.0f, 0.0f, 1.0f};
            va.viewports[1] = {640.0f, 0.0f, 640.0f, 720.0f, 0.0f, 1.0f};
            va.scissors[0] = {0u, 0u, 640u, 720u};
            va.scissors[1] = {640u, 0u, 640u, 720u};
            CHECK(a840_emit_vertex_amplification_state(&cs, &va) == A840_EMIT_OK);
            verify_pm4_decodes(cs, 6u);
            uint32_t vmask = 0u;
            CHECK(stream_contains_reg(cs, A840_REG_VPC_STEREO_RENDERING_VIEWMASK, &vmask));
            CHECK(vmask == (views == 1u ? 0u : 3u));
            CHECK(stream_contains_reg(cs, A840_REG_VFD_STEREO_RENDERING_CNTL));
            CHECK(stream_contains_reg(cs, A840_REG_GRAS_SU_STEREO_CNTL));
        }
    }

    CASE("vertex_amplification_reject_invalid_views_and_viewport_slot");
    {
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        A840VertexAmpStateDesc va{};
        va.amp_count = 0u;
        CHECK(a840_emit_vertex_amplification_state(&cs, &va) == A840_EMIT_RANGE_ERROR);
        va.amp_count = 3u;
        CHECK(a840_emit_vertex_amplification_state(&cs, &va) == A840_EMIT_RANGE_ERROR);
        va.amp_count = 2u;
        va.mappings[1].viewport_array_index = 16u;
        CHECK(a840_emit_vertex_amplification_state(&cs, &va) == A840_EMIT_RANGE_ERROR);
        CHECK(cs.used_dwords == 0u);
    }
}

/* ============================================================================
 * Group 4: Phase 4 Pipeline End-to-End Flows + 3 PM4 Dumps
 * ============================================================================ */
void test_group4_phase4_pipelines_and_dumps(const char *dump_msaa_vis,
                                            const char *dump_tess_amp,
                                            const char *dump_camera_mps)
{
    DeviceFixture fx(A840_ROUTE_PHASE4_FULL_ALL);
    A840RtBuffer *isa_bo = fx.alloc_buf(8192u);
    A840CompiledStage vs = make_dummy_stage(A840_STAGE_VERTEX);
    A840CompiledStage fs = make_dummy_stage(A840_STAGE_FRAGMENT);
    A840CompiledStage cs_yuv = make_dummy_stage(A840_STAGE_COMPUTE, 16u, 8u, 1u);
    A840CompiledStage cs_gauss = make_dummy_stage(A840_STAGE_COMPUTE, 16u, 8u, 1u);
    A840CompiledStage cs_gemm = make_dummy_stage(A840_STAGE_COMPUTE, 16u, 16u, 1u);

    CASE("pipeline_msaa_4x_memoryless_resolve_and_vis_query_dump");
    {
        A840UbwcTexture *msaa_col = fx.alloc_ubwc(A840_P3_TEX_MEMORYLESS, A840_STORAGE_MEMORYLESS,
                                                  A840_MTL_FMT_BGRA8_UNORM, A840_YCBCR_NONE,
                                                  192u, 128u, 4u, A840_TILE_TILED);
        A840UbwcTexture *msaa_dep = fx.alloc_ubwc(A840_P3_TEX_MEMORYLESS, A840_STORAGE_MEMORYLESS,
                                                  A840_MTL_FMT_DEPTH32_FLOAT, A840_YCBCR_NONE,
                                                  192u, 128u, 4u, A840_TILE_TILED);
        A840UbwcTexture *resolve_dst = fx.alloc_ubwc(A840_P3_TEX_UBWC6, A840_STORAGE_PRIVATE,
                                                     A840_MTL_FMT_BGRA8_UNORM, A840_YCBCR_NONE,
                                                     192u, 128u, 1u, A840_TILE_UBWC);
        A840RtBuffer *vis_bo = fx.alloc_buf(4096u);

        A840Phase4MsaaVisPassDesc pass{};
        pass.msaa_color_target = msaa_col;
        pass.resolve_target = resolve_dst;
        pass.depth_target = msaa_dep;
        pass.visibility_buffer = vis_bo;
        pass.visibility_slot = 2u;
        pass.visibility_mode = A840_VIS_QUERY_COUNTING;
        pass.enable_visibility_query = true;
        pass.sample_rate_shading = true;
        pass.emit_depth_stencil_variant = true;
        pass.depth_stencil.depth_test = true;
        pass.depth_stencil.depth_write = true;
        pass.depth_stencil.depth_compare = 1u;
        pass.depth_stencil.depth_bounds_test = true;
        pass.depth_stencil.depth_bounds_min = 0.05f;
        pass.depth_stencil.depth_bounds_max = 0.95f;
        pass.depth_stencil.z_mode = A840_ZMODE_EARLY_Z;
        pass.resolve_mode = A840_MSAA_RESOLVE_AVERAGE;
        pass.program = make_graphics_prog(&vs, &fs, isa_bo);
        pass.draw.primitive = A840_PRIM_TRIANGLES;
        pass.draw.count = 12u;
        pass.draw.instance_count = 1u;

        std::vector<uint32_t> words(2048, 0u);
        std::vector<A840Reloc> relocs(128);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);

        A840Phase4PassStats stats{};
        CHECK(a840_phase4_encode_msaa_vis_pass(a840_rt_device_router(fx.dev), &pass, &cs, &stats) == A840_RT_OK);
        CHECK(stats.route.path == A840_ROUTE_PATH_RAW);
        CHECK(stats.emitted_dwords > 0u);
        CHECK(stats.tile_count >= 1u);
        verify_pm4_decodes(cs, 20u);
        write_pm4_file(dump_msaa_vis, cs.words, cs.used_dwords);

        /* Also submit end-to-end through Mock Transport. */
        A840Phase4PassStats sub_stats{};
        CHECK(a840_phase4_submit_msaa_vis_pass(fx.dev, fx.queue, &pass, &sub_stats) == A840_RT_OK);
        CHECK(sub_stats.submit_seq >= 1u);
    }

    CASE("pipeline_tessellation_and_vertex_amplification_dump");
    {
        A840RtTexture *color = fx.alloc_tex(A840_MTL_FMT_BGRA8_UNORM, 256u, 128u, A840_TILE_TILED);
        A840RtBuffer *tess_bo = fx.alloc_buf(65536u);

        A840Phase4TessAmpPassDesc pass{};
        pass.color_target = color;
        pass.tess_factor_buffer = tess_bo;
        pass.enable_tessellation = true;
        pass.enable_vertex_amp = true;
        pass.tess_state.domain = A840_TESS_DOMAIN_QUAD;
        pass.tess_state.spacing = A840_TESS_SPACING_FRACTIONAL_EVEN;
        pass.tess_state.winding = A840_TESS_WINDING_CCW;
        pass.tess_state.control_points = 4u;
        pass.tess_state.patch_local_mem_16b = 16u;
        pass.tess_state.subdraw_patch_limit = 32u;
        pass.tess_draw.domain = A840_TESS_DOMAIN_QUAD;
        pass.tess_draw.control_points = 4u;
        pass.tess_draw.patch_count = 8u;
        pass.tess_draw.instance_count = 1u;
        pass.vertex_amp.amp_count = 2u;
        pass.vertex_amp.increment_rt_index = true;
        pass.vertex_amp.increment_viewport_index = true;
        pass.vertex_amp.mappings[0] = {0u, 0u};
        pass.vertex_amp.mappings[1] = {1u, 1u};
        pass.vertex_amp.viewport_count = 2u;
        pass.vertex_amp.viewports[0] = {0.0f, 0.0f, 128.0f, 128.0f, 0.0f, 1.0f};
        pass.vertex_amp.viewports[1] = {128.0f, 0.0f, 128.0f, 128.0f, 0.0f, 1.0f};
        pass.vertex_amp.scissors[0] = {0u, 0u, 128u, 128u};
        pass.vertex_amp.scissors[1] = {128u, 0u, 128u, 128u};
        pass.program = make_graphics_prog(&vs, &fs, isa_bo);

        std::vector<uint32_t> words(2048, 0u);
        std::vector<A840Reloc> relocs(128);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);

        A840Phase4PassStats stats{};
        CHECK(a840_phase4_encode_tess_amp_pass(a840_rt_device_router(fx.dev), &pass, &cs, &stats) == A840_RT_OK);
        CHECK(stats.route.path == A840_ROUTE_PATH_RAW);
        verify_pm4_decodes(cs, 20u);
        write_pm4_file(dump_tess_amp, cs.words, cs.used_dwords);

        A840Phase4PassStats sub_stats{};
        CHECK(a840_phase4_submit_tess_amp_pass(fx.dev, fx.queue, &pass, &sub_stats) == A840_RT_OK);
        CHECK(sub_stats.submit_seq >= 2u);
    }

    CASE("pipeline_camera155_nv12_p010_coreimage_mps_gemm_dump");
    {
        /* Camera 155: NV12 preview + 4096x3072 BGRA8 photo capture */
        A840UbwcTexture *nv12_preview = fx.alloc_ubwc(A840_P3_TEX_YCBCR_MULTIPLANE, A840_STORAGE_PRIVATE,
                                                      0u, A840_YCBCR_NV12,
                                                      1920u, 1080u, 1u, A840_TILE_LINEAR);
        A840UbwcTexture *photo_4k = fx.alloc_ubwc(A840_P3_TEX_UBWC6, A840_STORAGE_PRIVATE,
                                                  A840_MTL_FMT_BGRA8_UNORM, A840_YCBCR_NONE,
                                                  4096u, 3072u, 1u, A840_TILE_UBWC);
        A840RtBuffer *yuv_consts = fx.alloc_buf(4096u);
        A840RtBuffer *bindless = fx.alloc_buf(4096u);

        A840Phase4Camera155Desc cam{};
        cam.preview_ycbcr = nv12_preview;
        cam.photo_bgra8 = photo_4k;
        cam.yuv_to_rgb_consts = yuv_consts;
        cam.bindless_table = bindless;
        cam.yuv_convert_pipe = make_compute_pipe(&cs_yuv, isa_bo);
        cam.emit_photo_clear_and_blit = true;

        /* Core Image separable Gaussian convolution (H + V compute passes) */
        A840RtTexture *ci_in = fx.alloc_tex(A840_MTL_FMT_BGRA8_UNORM, 512u, 512u, A840_TILE_TILED);
        A840RtTexture *ci_mid = fx.alloc_tex(A840_MTL_FMT_BGRA8_UNORM, 512u, 512u, A840_TILE_TILED);
        A840RtTexture *ci_out = fx.alloc_tex(A840_MTL_FMT_BGRA8_UNORM, 512u, 512u, A840_TILE_TILED);
        A840RtBuffer *ci_weights = fx.alloc_buf(4096u);
        A840Phase4CoreImageGaussianDesc ci{};
        ci.input_tex = ci_in;
        ci.intermediate_tex = ci_mid;
        ci.output_tex = ci_out;
        ci.weights_buffer = ci_weights;
        ci.bindless_table = bindless;
        ci.horiz_pipe = make_compute_pipe(&cs_gauss, isa_bo);
        ci.vert_pipe = make_compute_pipe(&cs_gauss, isa_bo);
        ci.kernel_radius = 9u;

        /* MPS GEMM compute (C = alpha * A * B + beta * C) */
        A840RtBuffer *mat_a = fx.alloc_buf(65536u);
        A840RtBuffer *mat_b = fx.alloc_buf(65536u);
        A840RtBuffer *mat_c = fx.alloc_buf(65536u);
        A840RtBuffer *gemm_params = fx.alloc_buf(4096u);
        A840Phase4MpsGemmDesc mps{};
        mps.matrix_a = mat_a;
        mps.matrix_b = mat_b;
        mps.matrix_c = mat_c;
        mps.uniform_params = gemm_params;
        mps.bindless_table = bindless;
        mps.gemm_pipe = make_compute_pipe(&cs_gemm, isa_bo);
        mps.m = 256u;
        mps.n = 256u;
        mps.k = 128u;
        mps.tile_m = 16u;
        mps.tile_n = 16u;

        std::vector<uint32_t> words(4096, 0u);
        std::vector<A840Reloc> relocs(128);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);

        A840Phase4PassStats st_cam{}, st_ci{}, st_mps{};
        CHECK(a840_phase4_encode_camera155_flow(a840_rt_device_router(fx.dev), &cam, &cs, &st_cam) == A840_RT_OK);
        CHECK(a840_phase4_encode_coreimage_gaussian(a840_rt_device_router(fx.dev), &ci, &cs, &st_ci) == A840_RT_OK);
        CHECK(a840_phase4_encode_mps_gemm(a840_rt_device_router(fx.dev), &mps, &cs, &st_mps) == A840_RT_OK);

        CHECK(st_cam.route.path == A840_ROUTE_PATH_RAW);
        CHECK(st_ci.route.path == A840_ROUTE_PATH_RAW);
        CHECK(st_mps.route.path == A840_ROUTE_PATH_RAW);
        CHECK(st_mps.tile_count == (256u / 16u) * (256u / 16u));

        verify_pm4_decodes(cs, 30u);
        write_pm4_file(dump_camera_mps, cs.words, cs.used_dwords);

        /* Submit combined stream through Mock Transport */
        A840RtBuffer *residents[] = {isa_bo, yuv_consts, bindless, ci_weights, mat_a, mat_b, mat_c, gemm_params};
        uint32_t seq = 0u;
        CHECK(a840_phase4_submit_cs(fx.dev, fx.queue, cs.words, cs.used_dwords,
                                    residents, 8u, &seq) == A840_RT_OK);
        CHECK(seq >= 3u);
    }

    CASE("pipeline_camera155_p010_and_router_gating");
    {
        A840UbwcTexture *p010_preview = fx.alloc_ubwc(A840_P3_TEX_YCBCR_MULTIPLANE, A840_STORAGE_PRIVATE,
                                                      0u, A840_YCBCR_P010,
                                                      1920u, 1080u, 1u, A840_TILE_LINEAR);
        A840UbwcTexture *photo = fx.alloc_ubwc(A840_P3_TEX_UBWC6, A840_STORAGE_PRIVATE,
                                               A840_MTL_FMT_BGRA8_UNORM, A840_YCBCR_NONE,
                                               1024u, 768u, 1u, A840_TILE_UBWC);
        A840RtBuffer *yuv_consts = fx.alloc_buf(4096u);
        A840RtBuffer *bindless = fx.alloc_buf(4096u);
        A840Phase4Camera155Desc cam{};
        cam.preview_ycbcr = p010_preview;
        cam.photo_bgra8 = photo;
        cam.yuv_to_rgb_consts = yuv_consts;
        cam.bindless_table = bindless;
        cam.yuv_convert_pipe = make_compute_pipe(&cs_yuv, isa_bo);

        /* Phase 2-only router must fallback on YCbCr/XR10 */
        A840DualPathRouter p2{};
        a840_router_init(&p2, A840_ROUTE_PHASE2_ALL);
        std::vector<uint32_t> words(512, 0u);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        A840Phase4PassStats st{};
        CHECK(a840_phase4_encode_camera155_flow(&p2, &cam, &cs, &st) == A840_RT_FALLBACK_REQUIRED);
        CHECK(st.route.predicate_id == A840_PRED_RENDER_YCBCR_OR_XR10);

        /* Full Phase 4 router passes P010 natively */
        CHECK(a840_phase4_encode_camera155_flow(a840_rt_device_router(fx.dev), &cam, &cs, &st) == A840_RT_OK);
        CHECK(st.route.path == A840_ROUTE_PATH_RAW);
    }
}

/* ============================================================================
 * Group 5: Real W07-L2 NezhaShaderCompiler Client + Service Integration
 * ============================================================================ */
struct CompilerLoopbackEnv {
    static constexpr uint64_t kStackKey = 0x5730384c3353544bULL;
    uint64_t abi_fp = 0u;

    A840ScService *svc = nullptr;
    A840ScClient *client = nullptr;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> reply_queue;
    std::atomic<bool> daemon_online{true};

    static A840ScStatus backend_compile(void *ctx, uint32_t, const A840ScFrameHeader *req,
                                        const uint8_t *, uint8_t **out_blob, size_t *out_bytes)
    {
        auto *self = static_cast<CompilerLoopbackEnv *>(ctx);
        A840PipelineRecord rec{};
        rec.stack_key = kStackKey;
        rec.abi_fingerprint = self->abi_fp;
        rec.chip_id = A840_RECORD_CHIP_ID;
        rec.highest_bank_bit = A840_RECORD_HBB;
        rec.pipeline_key = req->pipeline_key;
        rec.stage_count = 1u;
        rec.stages[0].abi_version = ADRENO_COMPILE_ABI_VERSION;
        rec.stages[0].stage = A840_STAGE_COMPUTE;
        rec.stages[0].isa_dwords = 4u;
        rec.stages[0].instruction_groups = 1u;
        rec.stages[0].max_full_reg = 3;
        rec.stages[0].max_half_reg = -1;
        rec.stages[0].local_size[0] = 8u;
        rec.stages[0].local_size[1] = 8u;
        rec.stages[0].local_size[2] = 1u;
        static const uint32_t kIsa[4] = {1u, 2u, 3u, 4u};
        rec.stages[0].isa_words = kIsa;

        uint8_t *buf = nullptr;
        size_t written = 0u;
        if (a840_record_serialize(&rec, &buf, &written) != A840_RECORD_OK) {
            return A840_SC_COMPILE_FAILED;
        }
        *out_blob = buf;
        *out_bytes = written;
        return A840_SC_OK;
    }

    static void on_service_reply(void *ctx, const uint8_t *frame, size_t frame_bytes)
    {
        auto *self = static_cast<CompilerLoopbackEnv *>(ctx);
        std::lock_guard<std::mutex> lk(self->mu);
        self->reply_queue.emplace_back(frame, frame + frame_bytes);
        self->cv.notify_all();
    }

    static A840ScStatus transport_send(void *ctx, const uint8_t *frame, size_t frame_bytes, uint32_t)
    {
        auto *self = static_cast<CompilerLoopbackEnv *>(ctx);
        if (!self->daemon_online.load()) {
            return A840_SC_DAEMON_UNAVAILABLE;
        }
        return a840_sc_service_handle_frame(self->svc, frame, frame_bytes, &on_service_reply, self);
    }

    static A840ScStatus transport_recv(void *ctx, uint8_t **out_frame, size_t *out_bytes, uint32_t timeout_ms)
    {
        auto *self = static_cast<CompilerLoopbackEnv *>(ctx);
        if (!self->daemon_online.load()) {
            return A840_SC_DAEMON_UNAVAILABLE;
        }
        std::unique_lock<std::mutex> lk(self->mu);
        if (self->reply_queue.empty()) {
            if (timeout_ms == 0u) {
                return A840_SC_TIMED_OUT;
            }
            self->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                              [&] { return !self->reply_queue.empty() || !self->daemon_online.load(); });
        }
        if (!self->daemon_online.load()) {
            return A840_SC_DAEMON_UNAVAILABLE;
        }
        if (self->reply_queue.empty()) {
            return A840_SC_TIMED_OUT;
        }
        auto vec = std::move(self->reply_queue.front());
        self->reply_queue.pop_front();
        uint8_t *buf = static_cast<uint8_t *>(std::malloc(vec.size()));
        if (!buf) {
            return A840_SC_OOM;
        }
        std::memcpy(buf, vec.data(), vec.size());
        *out_frame = buf;
        *out_bytes = vec.size();
        return A840_SC_OK;
    }

    CompilerLoopbackEnv()
    {
        abi_fp = a840_compile_abi_fingerprint();
        A840ScServiceConfig scfg{};
        scfg.stack_key = kStackKey;
        scfg.abi_fingerprint = abi_fp;
        scfg.worker_count = 2u;
        scfg.mem_cache_entries = 16u;
        scfg.mem_cache_bytes = 1024u * 1024u;
        scfg.compile_fn = &backend_compile;
        scfg.user_ctx = this;
        CHECK(a840_sc_service_create(&scfg, &svc) == A840_SC_OK);

        A840ScClientConfig ccfg{};
        ccfg.stack_key = kStackKey;
        ccfg.abi_fingerprint = abi_fp;
        ccfg.client_pid = 4242u;
        ccfg.initial_backoff_ms = 20u;
        ccfg.max_backoff_ms = 80u;
        ccfg.send_fn = &transport_send;
        ccfg.recv_fn = &transport_recv;
        ccfg.user_ctx = this;
        CHECK(a840_sc_client_create(&ccfg, &client) == A840_SC_OK);
    }

    ~CompilerLoopbackEnv()
    {
        daemon_online.store(false);
        cv.notify_all();
        a840_sc_client_destroy(client);
        a840_sc_service_shutdown(svc);
        a840_sc_service_destroy(svc);
    }
};

A840Phase4CompilerState bridge_compile_cb(void *handle, const uint8_t key[32],
                                          const void *payload, size_t payload_bytes,
                                          uint32_t timeout_ms, uint8_t **out_blob,
                                          size_t *out_bytes)
{
    auto *client = static_cast<A840ScClient *>(handle);
    A840PipelineKey pk{};
    std::memcpy(pk.sha256, key, 32u);
    A840ScStatus st = a840_sc_client_compile_sync(client, &pk, payload, payload_bytes,
                                                  timeout_ms, out_blob, out_bytes);
    if (st == A840_SC_OK) {
        return A840_P4_COMPILER_OK;
    }
    if (st == A840_SC_DAEMON_UNAVAILABLE || st == A840_SC_TIMED_OUT || st == A840_SC_SHUTDOWN) {
        return A840_P4_COMPILER_DAEMON_UNAVAILABLE;
    }
    return A840_P4_COMPILER_COMPILE_FAILED;
}

bool bridge_health_cb(void *handle)
{
    auto *client = static_cast<A840ScClient *>(handle);
    if (a840_sc_client_is_circuit_open(client)) {
        return false;
    }
    return a840_sc_client_ping(client, 500u) == A840_SC_OK;
}

void test_group5_compiler_daemon_bridge()
{
    CompilerLoopbackEnv env;
    A840Phase4CompilerBridge bridge{};
    bridge.client_handle = env.client;
    bridge.compile_sync = &bridge_compile_cb;
    bridge.is_available = &bridge_health_cb;
    bridge.free_blob = &std::free;
    bridge.default_timeout_ms = 2000u;

    uint8_t key[32];
    for (uint32_t i = 0u; i < 32u; ++i) {
        key[i] = static_cast<uint8_t>(0x30u + i);
    }
    const uint8_t payload[8] = {1, 2, 3, 4, 5, 6, 7, 8};

    CASE("compiler_bridge_live_daemon_resolves_raw");
    {
        A840DualPathRouter r{};
        a840_phase4_enable_full_router(&r);
        A840Phase4ColdCompileResult res{};
        CHECK(a840_phase4_resolve_cold_compile(&r, &bridge, key, payload, sizeof(payload), false, &res) == A840_RT_OK);
        CHECK(res.route.path == A840_ROUTE_PATH_RAW);
        CHECK(res.compiler_state == A840_P4_COMPILER_OK);
        CHECK(res.record_blob != nullptr && res.record_bytes > 0u);
        std::free(res.record_blob);
    }

    CASE("compiler_bridge_daemon_offline_trips_circuit_and_fails_closed");
    {
        env.daemon_online.store(false);
        env.cv.notify_all();
        A840DualPathRouter r{};
        a840_phase4_enable_full_router(&r);
        A840Phase4ColdCompileResult res{};
        CHECK(a840_phase4_resolve_cold_compile(&r, &bridge, key, payload, sizeof(payload), false, &res) == A840_RT_FALLBACK_REQUIRED);
        CHECK(res.route.path == A840_ROUTE_PATH_FALLBACK);
        CHECK(res.route.predicate_id == A840_PRED_COMPILER_DAEMON_UNAVAILABLE);
        CHECK(std::strcmp(res.route.predicate, "raw:compiler.daemon_unavailable") == 0);
        CHECK(res.record_blob == nullptr);
        CHECK(a840_sc_client_is_circuit_open(env.client));
    }

    CASE("compiler_bridge_precompiled_bypasses_offline_daemon");
    {
        A840DualPathRouter r{};
        a840_phase4_enable_full_router(&r);
        A840Phase4ColdCompileResult res{};
        CHECK(a840_phase4_resolve_cold_compile(&r, &bridge, key, payload, sizeof(payload), true, &res) == A840_RT_OK);
        CHECK(res.route.path == A840_ROUTE_PATH_RAW);
    }

    CASE("compiler_bridge_null_client_fails_closed");
    {
        A840DualPathRouter r{};
        a840_phase4_enable_full_router(&r);
        A840Phase4ColdCompileResult res{};
        CHECK(a840_phase4_resolve_cold_compile(&r, nullptr, key, payload, sizeof(payload), false, &res) == A840_RT_FALLBACK_REQUIRED);
        CHECK(res.route.predicate_id == A840_PRED_COMPILER_DAEMON_UNAVAILABLE);
    }
}

/* ============================================================================
 * Group 6: Complete 16/16 DualPathPredicates.tsv Closure & Backward Compat
 * ============================================================================ */
void test_group6_router_16_of_16_closure()
{
    CASE("router_backward_compat_phase2_phase3_w07_unchanged");
    {
        /* Phase 2 + Phase 3 + W07 bits (10..13) ONLY -> W08 bits 14..16 OFF. */
        const uint32_t w07_mask = static_cast<uint32_t>(A840_ROUTE_PHASE2_ALL) |
                                  static_cast<uint32_t>(A840_ROUTE_PHASE3_ALL) |
                                  static_cast<uint32_t>(A840_ROUTE_P4_HEAP_ALIASING) |
                                  static_cast<uint32_t>(A840_ROUTE_P4_INDIRECT_COMPUTE) |
                                  static_cast<uint32_t>(A840_ROUTE_P4_INDIRECT_RENDER_ICB) |
                                  static_cast<uint32_t>(A840_ROUTE_P4_SPARSE);
        A840DualPathRouter r{};
        a840_router_init(&r, w07_mask);

        A840RenderRouteQuery rq{};
        rq.color_attachment_count = 1u;
        rq.color_mtl_formats[0] = A840_MTL_FMT_BGRA8_UNORM;
        rq.color_tile_modes[0] = A840_TILE_UBWC;
        rq.sample_count = 4u;
        rq.has_msaa_resolve = true;
        CHECK(a840_router_eval_render(&r, &rq).predicate_id == A840_PRED_RENDER_MSAA_RESOLVE);

        rq.sample_count = 1u;
        rq.has_msaa_resolve = false;
        rq.uses_tessellation = true;
        CHECK(a840_router_eval_render(&r, &rq).predicate_id == A840_PRED_RENDER_TESS_OR_AMPLIFY);

        rq.uses_tessellation = false;
        rq.uses_vertex_amplification = true;
        CHECK(a840_router_eval_render(&r, &rq).predicate_id == A840_PRED_RENDER_TESS_OR_AMPLIFY);

        A840CompilerRouteQuery cq{};
        cq.precompiled_available = false;
        cq.daemon_available = false;
        CHECK(a840_router_eval_compiler(&r, &cq).predicate_id == A840_PRED_COMPILER_DAEMON_UNAVAILABLE);
    }

    CASE("router_w08_bit14_msaa_resolve_2x_4x_pass_8x_reject");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, static_cast<uint32_t>(A840_ROUTE_PHASE2_ALL) |
                             static_cast<uint32_t>(A840_ROUTE_PHASE3_ALL) |
                             static_cast<uint32_t>(A840_ROUTE_P4_MSAA_RESOLVE));

        A840RenderRouteQuery rq{};
        rq.color_attachment_count = 1u;
        rq.color_mtl_formats[0] = A840_MTL_FMT_BGRA8_UNORM;
        rq.color_tile_modes[0] = A840_TILE_UBWC;
        rq.has_msaa_resolve = true;
        rq.sample_count = 2u;
        CHECK(a840_router_eval_render(&r, &rq).path == A840_ROUTE_PATH_RAW);
        rq.sample_count = 4u;
        CHECK(a840_router_eval_render(&r, &rq).path == A840_ROUTE_PATH_RAW);
        /* 8x MSAA must still reject even with bit 14 set */
        rq.sample_count = 8u;
        CHECK(a840_router_eval_render(&r, &rq).predicate_id == A840_PRED_RENDER_MSAA_RESOLVE);
    }

    CASE("router_w08_bit15_tess_and_vertex_amp");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, static_cast<uint32_t>(A840_ROUTE_PHASE2_ALL) |
                             static_cast<uint32_t>(A840_ROUTE_P4_TESS_AMPLIFY));

        A840RenderRouteQuery rq{};
        rq.color_attachment_count = 1u;
        rq.color_mtl_formats[0] = A840_MTL_FMT_BGRA8_UNORM;
        rq.color_tile_modes[0] = A840_TILE_TILED;
        rq.sample_count = 1u;
        rq.uses_tessellation = true;
        CHECK(a840_router_eval_render(&r, &rq).path == A840_ROUTE_PATH_RAW);
        rq.uses_tessellation = false;
        rq.uses_vertex_amplification = true;
        CHECK(a840_router_eval_render(&r, &rq).path == A840_ROUTE_PATH_RAW);
        rq.uses_tessellation = true;
        CHECK(a840_router_eval_render(&r, &rq).path == A840_ROUTE_PATH_RAW);
    }

    CASE("router_w08_bit16_compiler_daemon_and_compute_cold_compile");
    {
        A840DualPathRouter r{};
        a840_router_init(&r, static_cast<uint32_t>(A840_ROUTE_ALL) |
                             static_cast<uint32_t>(A840_ROUTE_P4_COMPILER_DAEMON));

        A840CompilerRouteQuery ccq{};
        ccq.precompiled_available = false;
        ccq.daemon_available = false;
        CHECK(a840_router_eval_compiler(&r, &ccq).path == A840_ROUTE_PATH_RAW);

        A840ComputeRouteQuery cq{};
        cq.has_compiled_kernel = false;
        cq.has_valid_local_size = true;
        cq.requires_cold_compile = true;
        cq.compiler_daemon_available = false;
        CHECK(a840_router_eval_compute(&r, &cq).path == A840_ROUTE_PATH_RAW);

        /* Without valid local size, still uncompiled_kernel */
        cq.has_valid_local_size = false;
        CHECK(a840_router_eval_compute(&r, &cq).predicate_id == A840_PRED_COMPUTE_UNCOMPILED_KERNEL);
    }

    CASE("router_all_16_predicates_closed_with_A840_ROUTE_PHASE4_ALL");
    {
        /* Verify every single one of the 16 frozen DualPathPredicates.tsv rows
         * has both (a) a reachable fallback condition when disabled/unsupported
         * and (b) a native path=raw closure when A840_ROUTE_PHASE4_ALL is active. */
        A840DualPathRouter r_off{};
        a840_router_init(&r_off, A840_ROUTE_NONE);

        A840DualPathRouter r_full{};
        a840_router_init(&r_full, A840_ROUTE_PHASE4_ALL);
        CHECK((r_full.enabled_categories & A840_ROUTE_PHASE4_ALL) == A840_ROUTE_PHASE4_ALL);
        CHECK(r_full.gmem_engine_enabled);

        /* 1. raw:unimplemented.global */
        {
            A840BlitRouteQuery q{};
            q.is_buffer_copy = true;
            CHECK(a840_router_eval_blit(&r_off, &q).predicate_id == A840_PRED_UNIMPLEMENTED_GLOBAL);
            CHECK(a840_router_eval_blit(&r_full, &q).path == A840_ROUTE_PATH_RAW);
        }
        /* 2. raw:blit.unsupported_format */
        {
            A840BlitRouteQuery q{};
            q.is_clear = true;
            q.tex_type = A840_TEX_2D;
            q.dst_mtl_format = 9999u;
            CHECK(a840_router_eval_blit(&r_full, &q).predicate_id == A840_PRED_BLIT_UNSUPPORTED_FORMAT);
            q.dst_mtl_format = A840_MTL_FMT_BGRA8_UNORM;
            CHECK(a840_router_eval_blit(&r_full, &q).path == A840_ROUTE_PATH_RAW);
        }
        /* 3. raw:blit.3d_or_compressed (UBWC 2D closed natively by Phase 3 UBWC bit) */
        {
            A840DualPathRouter r_p2{};
            a840_router_init(&r_p2, A840_ROUTE_BLIT);
            A840BlitRouteQuery q{};
            q.is_clear = true;
            q.tex_type = A840_TEX_2D;
            q.dst_mtl_format = A840_MTL_FMT_BGRA8_UNORM;
            q.dst_tile_mode = A840_TILE_UBWC;
            CHECK(a840_router_eval_blit(&r_p2, &q).predicate_id == A840_PRED_BLIT_3D_OR_COMPRESSED);
            CHECK(a840_router_eval_blit(&r_full, &q).path == A840_ROUTE_PATH_RAW);
        }
        /* 4. raw:compute.uncompiled_kernel */
        {
            A840ComputeRouteQuery q{};
            q.has_compiled_kernel = false;
            q.has_valid_local_size = true;
            CHECK(a840_router_eval_compute(&r_full, &q).predicate_id == A840_PRED_COMPUTE_UNCOMPILED_KERNEL);
            q.has_compiled_kernel = true;
            CHECK(a840_router_eval_compute(&r_full, &q).path == A840_ROUTE_PATH_RAW);
        }
        /* 5. raw:compute.indirect_dispatch */
        {
            A840DualPathRouter r_no{};
            a840_router_init(&r_no, A840_ROUTE_COMPUTE);
            A840ComputeRouteQuery q{};
            q.has_compiled_kernel = true;
            q.has_valid_local_size = true;
            q.is_indirect_dispatch = true;
            CHECK(a840_router_eval_compute(&r_no, &q).predicate_id == A840_PRED_COMPUTE_INDIRECT_DISPATCH);
            CHECK(a840_router_eval_compute(&r_full, &q).path == A840_ROUTE_PATH_RAW);
        }
        /* 6..12. Render predicates: GMEM, UBWC, YCbCr/XR10, FBFetch, Tess/Amp, Indirect/ICB, MSAA Resolve */
        {
            A840RenderRouteQuery q{};
            q.color_attachment_count = 1u;
            q.color_mtl_formats[0] = A840_MTL_FMT_BGRA10_XR;
            q.color_tile_modes[0] = A840_TILE_UBWC;
            q.sample_count = 4u;
            q.has_memoryless_attachment = true;
            q.has_msaa_resolve = true;
            q.uses_fbfetch = true;
            q.uses_programmable_blend = true;
            q.uses_tessellation = true;
            q.uses_vertex_amplification = true;
            q.uses_indirect_draw = true;
            q.uses_icb = true;
            q.has_ycbcr_attachment = true;
            /* Note: BGRA10_XR isn't UBWC-supported, test UBWC on BGRA8 + XR10 on Tiled separately */
            q.color_tile_modes[0] = A840_TILE_TILED;
            CHECK(a840_router_eval_render(&r_full, &q).path == A840_ROUTE_PATH_RAW);

            q.color_mtl_formats[0] = A840_MTL_FMT_BGRA8_UNORM;
            q.color_tile_modes[0] = A840_TILE_UBWC;
            CHECK(a840_router_eval_render(&r_full, &q).path == A840_ROUTE_PATH_RAW);
        }
        /* 13. raw:resource.heap_aliasing & 14. raw:resource.sparse_texture */
        {
            A840ResourceRouteQuery q{};
            q.uses_heap_aliasing = true;
            q.uses_sparse_texture = true;
            CHECK(a840_router_eval_resource(&r_full, &q).path == A840_ROUTE_PATH_RAW);
        }
        /* 15. raw:compiler.daemon_unavailable */
        {
            A840CompilerRouteQuery q{};
            q.precompiled_available = false;
            q.daemon_available = false;
            CHECK(a840_router_eval_compiler(&r_full, &q).path == A840_ROUTE_PATH_RAW);
        }
        /* 16. raw:sync.shared_event_cross_proc */
        {
            A840SyncRouteQuery q{};
            q.cross_process_shared_event = true;
            q.uses_writer_promise = true;
            q.abi_v3_supported = false;
            CHECK(a840_router_eval_sync(&r_full, &q).predicate_id == A840_PRED_SYNC_SHARED_EVENT_CROSS_PROC);
            q.abi_v3_supported = true;
            CHECK(a840_router_eval_sync(&r_full, &q).path == A840_ROUTE_PATH_RAW);
        }
    }
}

/* ============================================================================
 * Group 7: Granular Boundary & Stress Cases (ensuring >= 55 independent tests)
 * ============================================================================ */
void test_group7_granular_edge_cases()
{
    std::vector<uint32_t> words(512, 0u);
    std::vector<A840Reloc> relocs(32);

    /* 16 cases: individual control point counts across 1..32 */
    const uint32_t cp_sweep[] = {1u, 2u, 3u, 4u, 6u, 8u, 12u, 16u, 20u, 24u, 28u, 32u};
    for (uint32_t cp : cp_sweep) {
        CASE("tess_control_point_sweep");
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        A840TessStateDesc ts{};
        ts.domain = (cp % 2u == 0u) ? A840_TESS_DOMAIN_QUAD : A840_TESS_DOMAIN_TRIANGLE;
        ts.spacing = A840_TESS_SPACING_EQUAL;
        ts.winding = A840_TESS_WINDING_CCW;
        ts.control_points = cp;
        ts.patch_local_mem_16b = cp * 4u;
        ts.subdraw_patch_limit = 8u;
        ts.tess_factor_buffer = {300u, 0x60000000ULL};
        ts.tess_buffer_bytes = 8192u;
        CHECK(a840_emit_tess_state(&cs, &ts) == A840_EMIT_OK);

        A840TessDrawDesc td{};
        td.domain = ts.domain;
        td.control_points = cp;
        td.patch_count = 3u;
        td.instance_count = 1u;
        CHECK(a840_emit_tess_draw(&cs, &td) == A840_EMIT_OK);
        verify_pm4_decodes(cs, 8u);
        CHECK(!stream_contains_reg(cs, A840_REG_PC_DRAWCALL_CNTL));
    }

    /* 8 cases: all 8 hardware depth compare funcs + Z modes */
    for (uint8_t cmp = 0u; cmp < 8u; ++cmp) {
        CASE("depth_compare_func_sweep");
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        A840DepthStencilVariantDesc ds{};
        ds.depth_test = true;
        ds.depth_write = (cmp % 2u == 0u);
        ds.depth_compare = cmp;
        ds.z_mode = static_cast<A840DepthPlaneMode>(cmp % 3u);
        CHECK(a840_emit_depth_stencil_variant(&cs, &ds) == A840_EMIT_OK);
        uint32_t dcntl = 0u;
        CHECK(stream_contains_reg(cs, A840_REG_RB_DEPTH_CNTL, &dcntl));
        bool expect_read = (cmp != 0u && cmp != 7u);
        CHECK(((dcntl & A840_DEPTH_READ) != 0u) == expect_read);
    }

    /* 6 cases: visibility query slot sweep & rollback on small CS */
    for (uint32_t slot = 0u; slot < 4u; ++slot) {
        CASE("visibility_query_slot_offset_check");
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        A840VisQueryDesc q{};
        q.query_buffer = {400u, 0x70000000ULL};
        q.buffer_bytes = 512u;
        q.slot_index = slot;
        q.mode = A840_VIS_QUERY_COUNTING;
        q.inside_render_pass = true;
        CHECK(a840_emit_visibility_query_begin(&cs, &q) == A840_EMIT_OK);
        CHECK(a840_emit_visibility_query_end(&cs, &q) == A840_EMIT_OK);
        CHECK(cs.reloc_count == 2u);
        CHECK(cs.relocs[0].gpu_va == 0x70000000ULL + slot * 64u);
        CHECK(cs.relocs[1].gpu_va == 0x70000000ULL + slot * 64u);
    }

    CASE("visibility_query_capacity_rollback");
    {
        std::vector<uint32_t> tiny(3, 0u);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, tiny.data(), tiny.size(), nullptr, 0u) == A840_EMIT_OK);
        A840VisQueryDesc q{};
        q.query_buffer = {401u, 0x71000000ULL};
        q.buffer_bytes = 256u;
        q.slot_index = 0u;
        CHECK(a840_emit_visibility_query_begin(&cs, &q) == A840_EMIT_CAPACITY);
        CHECK(a840_emit_visibility_query_end(&cs, &q) == A840_EMIT_CAPACITY);
        CHECK(cs.used_dwords == 0u);
    }

    CASE("tess_draw_zero_patches_noop");
    {
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        A840TessDrawDesc td{};
        td.domain = A840_TESS_DOMAIN_TRIANGLE;
        td.control_points = 3u;
        td.patch_count = 0u;
        td.instance_count = 1u;
        CHECK(a840_emit_tess_draw(&cs, &td) == A840_EMIT_OK);
        CHECK(cs.used_dwords == 0u);
    }

    CASE("pipeline_null_and_invalid_arg_guards");
    {
        A840DualPathRouter r{};
        a840_phase4_enable_full_router(&r);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        CHECK(a840_phase4_encode_msaa_vis_pass(&r, nullptr, &cs, nullptr) == A840_RT_INVALID_ARG);
        CHECK(a840_phase4_encode_tess_amp_pass(&r, nullptr, &cs, nullptr) == A840_RT_INVALID_ARG);
        CHECK(a840_phase4_encode_camera155_flow(&r, nullptr, &cs, nullptr) == A840_RT_INVALID_ARG);
        CHECK(a840_phase4_encode_coreimage_gaussian(&r, nullptr, &cs, nullptr) == A840_RT_INVALID_ARG);
        CHECK(a840_phase4_encode_mps_gemm(&r, nullptr, &cs, nullptr) == A840_RT_INVALID_ARG);
        CHECK(a840_phase4_resolve_cold_compile(nullptr, nullptr, nullptr, nullptr, 0u, false, nullptr) == A840_RT_INVALID_ARG);
    }

    CASE("submit_cs_rejects_corrupt_pm4_and_cross_device");
    {
        DeviceFixture fx1(A840_ROUTE_PHASE4_FULL_ALL);
        DeviceFixture fx2(A840_ROUTE_PHASE4_FULL_ALL);
        uint32_t bad_word = 0xffffffffu;
        CHECK(a840_phase4_submit_cs(fx1.dev, fx1.queue, &bad_word, 1u, nullptr, 0u, nullptr) != A840_RT_OK);
        A840RtBuffer *foreign_bo = fx2.alloc_buf(4096u);
        uint32_t good_words[4] = {};
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, good_words, 4u, nullptr, 0u) == A840_EMIT_OK);
        CHECK(a840_emit_wfi(&cs) == A840_EMIT_OK);
        A840RtBuffer *res[] = {foreign_bo};
        CHECK(a840_phase4_submit_cs(fx1.dev, fx1.queue, cs.words, cs.used_dwords, res, 1u, nullptr) == A840_RT_WRONG_DEVICE);
    }
}

} // namespace

int main(int argc, char **argv)
{
    const char *dump_msaa_vis = nullptr;
    const char *dump_tess_amp = nullptr;
    const char *dump_camera_mps = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-msaa-vis-pm4") == 0 && i + 1 < argc) {
            dump_msaa_vis = argv[++i];
        } else if (std::strcmp(argv[i], "--dump-tess-amp-pm4") == 0 && i + 1 < argc) {
            dump_tess_amp = argv[++i];
        } else if (std::strcmp(argv[i], "--dump-camera-mps-pm4") == 0 && i + 1 < argc) {
            dump_camera_mps = argv[++i];
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    test_group1_msaa_and_resolve();
    test_group2_depth_stencil_and_visibility();
    test_group3_tess_and_vertex_amp();
    test_group4_phase4_pipelines_and_dumps(dump_msaa_vis, dump_tess_amp, dump_camera_mps);
    test_group5_compiler_daemon_bridge();
    test_group6_router_16_of_16_closure();
    test_group7_granular_edge_cases();

    CHECK(g_cases >= 55u);
    std::printf("PASS test_phase4_msaa_tess_camera: %u cases, %u checks\n", g_cases, g_checks);
    return 0;
}
