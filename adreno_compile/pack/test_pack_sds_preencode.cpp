/* SPDX-License-Identifier: MIT */
#include "a840_pack_sds_preencode.h"
#include "Adreno840Regs.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::size_t g_cases = 0;
std::size_t g_checks = 0;
const char *g_active_case = "init";

void begin_case(const char *name)
{
    g_active_case = name;
    ++g_cases;
}

void check_cond(bool ok, const char *expr, int line)
{
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL [%s] line %d: %s\n", g_active_case, line, expr);
        std::exit(EXIT_FAILURE);
    }
}

#define CASE(name) begin_case(name)
#define CHECK(expr) check_cond((expr), #expr, __LINE__)

constexpr uint64_t TEST_STACK_KEY = UINT64_C(0x2026112600000602);
constexpr uint32_t FLAG_USES_FBFETCH = 1u << 2;
constexpr uint32_t FLAG_MERGED_REGS = 1u << 3;
constexpr uint32_t FLAG_EARLY_PREAMBLE = 1u << 4;

struct AlignedBuf {
    uint8_t *data = nullptr;
    std::size_t bytes = 0;

    explicit AlignedBuf(std::size_t sz) : bytes(sz)
    {
        void *p = nullptr;
        CHECK(posix_memalign(&p, 128, sz) == 0);
        data = static_cast<uint8_t *>(p);
        std::memset(data, 0xa5, sz);
    }

    ~AlignedBuf() { std::free(data); }
    AlignedBuf(const AlignedBuf &) = delete;
    AlignedBuf &operator=(const AlignedBuf &) = delete;
};

struct StageFixture {
    A840CompiledStage stage{};
    std::array<uint32_t, 32> isa{};
    std::array<uint32_t, 8> consts{};

    void init(A840ShaderStage kind, uint32_t flags_out, uint32_t pvt_bytes = 0u)
    {
        std::memset(&stage, 0, sizeof(stage));
        for (std::size_t i = 0; i < isa.size(); ++i)
            isa[i] = 0x10000000u + static_cast<uint32_t>(kind) * 256u + static_cast<uint32_t>(i);
        for (std::size_t i = 0; i < consts.size(); ++i)
            consts[i] = 0x20000000u + static_cast<uint32_t>(i);

        stage.abi_version = ADRENO_COMPILE_ABI_VERSION;
        stage.stage = static_cast<uint32_t>(kind);
        stage.isa_words = isa.data();
        stage.isa_dwords = static_cast<uint32_t>(isa.size());
        stage.instruction_groups = static_cast<uint32_t>(isa.size() / 4u);
        stage.max_full_reg = 7;
        stage.max_half_reg = 3;
        stage.branchstack = 4u;
        stage.constlen_vec4 = 8u;
        stage.const_data_dwords = static_cast<uint32_t>(consts.size());
        stage.const_data_words = consts.data();
        stage.private_mem_bytes = pvt_bytes;
        stage.shared_mem_bytes = (kind == A840_STAGE_COMPUTE) ? 4096u : 0u;
        stage.local_size[0] = (kind == A840_STAGE_COMPUTE) ? 8u : 1u;
        stage.local_size[1] = (kind == A840_STAGE_COMPUTE) ? 8u : 1u;
        stage.local_size[2] = 1u;
        stage.flags_out = flags_out;
        stage.max_waves = 16u;
    }
};

A840ComputePipelineDesc make_compute_desc(const StageFixture &cs_fix)
{
    A840ComputePipelineDesc d{};
    d.shader.stage = &cs_fix.stage;
    d.shader.textures = 2u;
    d.shader.samplers = 1u;
    d.shader.uavs = 1u;
    d.shader.bindless_tex = true;
    d.shader.bindless_samp = true;
    d.shader.bindless_uav = true;
    d.shader.bindless_ubo = true;
    d.local_id_regid = 0u;
    d.wg_id_regid = 4u;
    d.round_robin = false;
    d.linear_dispatch = true;
    return d;
}

A840GraphicsProgramDesc make_graphics_desc(const StageFixture &vs_fix,
                                           const StageFixture &fs_fix,
                                           bool with_depth = false)
{
    A840GraphicsProgramDesc d{};
    d.vs.stage = &vs_fix.stage;
    d.vs.bindless_ubo = true;
    d.fs.stage = &fs_fix.stage;
    d.fs.textures = 2u;
    d.fs.samplers = 1u;
    d.fs.bindless_tex = true;
    d.fs.bindless_samp = true;
    d.fs.bindless_ubo = true;

    d.vertex_input_count = 1u;
    d.vertex_inputs[0] = {0u, 0xfu};
    d.vertex_id = 4u;
    d.instance_id = A840_REGID_UNUSED;
    d.primitive_id = A840_REGID_UNUSED;
    d.view_id = A840_REGID_UNUSED;
    d.position = 8u;
    d.point_size = A840_REGID_UNUSED;

    d.varying_count = 2u;
    d.varyings[0] = {12u, 0x3u, false, 0u, A840_VARYING_NORMAL, false};
    d.varyings[1] = {16u, 0xfu, true, 0u, A840_VARYING_NORMAL, false};

    d.fs_inputs.face = A840_REGID_UNUSED;
    d.fs_inputs.sample_id = A840_REGID_UNUSED;
    d.fs_inputs.sample_mask = A840_REGID_UNUSED;
    d.fs_inputs.center_rhw = A840_REGID_UNUSED;
    d.fs_inputs.persp_pixel = 0u;
    d.fs_inputs.linear_pixel = A840_REGID_UNUSED;
    d.fs_inputs.persp_centroid = A840_REGID_UNUSED;
    d.fs_inputs.linear_centroid = A840_REGID_UNUSED;
    d.fs_inputs.persp_sample = A840_REGID_UNUSED;
    d.fs_inputs.linear_sample = A840_REGID_UNUSED;
    d.fs_inputs.frag_coord = 4u;
    d.fs_inputs.frag_coord_mask = 0x3u;
    d.fs_inputs.shading_rate = A840_REGID_UNUSED;

    d.depth_output = with_depth ? 20u : A840_REGID_UNUSED;
    d.sample_mask_output = A840_REGID_UNUSED;
    d.stencil_output = A840_REGID_UNUSED;
    for (uint32_t i = 0; i < A840_MAX_COLOR_TARGETS; ++i)
        d.colors[i] = {A840_REGID_UNUSED, 0u, false};
    d.colors[0] = {0u, 0xfu, false};
    d.colors[1] = {4u, 0xfu, true};
    d.pixel_lod = true;
    d.full_quad = false;
    d.fs_thread128 = true;
    return d;
}

bool stream_has_reg(const uint32_t *words, uint32_t word_count, uint32_t reg_id)
{
    std::array<A840PM4Packet, A840_PACK_SDS_MAX_DWORDS> pkts{};
    std::size_t count = 0u;
    CHECK(a840_pm4_decode(words, word_count, pkts.data(), pkts.size(), &count) == A840_EMIT_OK);
    for (std::size_t i = 0; i < count; ++i) {
        if (pkts[i].type == 4u && reg_id >= pkts[i].identifier &&
            reg_id < pkts[i].identifier + pkts[i].payload_words) {
            return true;
        }
    }
    return false;
}

A840PipelineRecord make_record(uint8_t id, const A840CompiledStage *s0,
                               const A840CompiledStage *s1, A840BindingPlan *out_plan)
{
    A840PipelineRecord rec{};
    std::memset(out_plan, 0, sizeof(*out_plan));
    rec.abi_fingerprint = a840_compile_abi_fingerprint();
    rec.stack_key = TEST_STACK_KEY;
    rec.chip_id = A840_PACK_V1_CHIP_ID;
    rec.highest_bank_bit = A840_PACK_V1_HBB;
    rec.pipeline_key.sha256[0] = id;
    rec.pipeline_key.sha256[31] = static_cast<uint8_t>(id ^ 0x5au);
    rec.stage_count = (s1 != nullptr) ? 2u : 1u;
    rec.stages[0] = *s0;
    if (s1 != nullptr)
        rec.stages[1] = *s1;
    out_plan->stage_count = rec.stage_count;
    for (uint32_t i = 0; i < rec.stage_count; ++i) {
        out_plan->stages[i].stage = rec.stages[i].stage;
        out_plan->stages[i].const_bytes = 64u;
    }
    return rec;
}

} // namespace

int main(int argc, char **argv)
{
    std::filesystem::path export_dir;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--export-dir") == 0 && i + 1 < argc) {
            export_dir = argv[++i];
        } else {
            export_dir = argv[i];
        }
    }
    if (!export_dir.empty())
        std::filesystem::create_directories(export_dir);

    StageFixture cs_fix, cs_pvt_fix, vs_fix, fs_fix, vs_pvt_fix, fs_fb_fix;
    cs_fix.init(A840_STAGE_COMPUTE, FLAG_MERGED_REGS | FLAG_EARLY_PREAMBLE, 0u);
    cs_pvt_fix.init(A840_STAGE_COMPUTE, FLAG_MERGED_REGS, 256u);
    vs_fix.init(A840_STAGE_VERTEX, FLAG_MERGED_REGS | FLAG_EARLY_PREAMBLE, 0u);
    fs_fix.init(A840_STAGE_FRAGMENT, FLAG_MERGED_REGS | FLAG_EARLY_PREAMBLE, 0u);
    vs_pvt_fix.init(A840_STAGE_VERTEX, FLAG_MERGED_REGS, 128u);
    fs_fb_fix.init(A840_STAGE_FRAGMENT,
                   FLAG_USES_FBFETCH | FLAG_MERGED_REGS | FLAG_EARLY_PREAMBLE, 256u);

    A840SdsBlock cs_static{}, cs_reloc{}, gfx_static{}, gfx_fbfetch{}, gfx_reloc{};

    CASE("compute_static_only_preencode");
    {
        auto desc = make_compute_desc(cs_fix);
        CHECK(a840_pack_sds_preencode_compute(&desc, A840_SDS_MODE_STATIC_ONLY, &cs_static) == A840_SDS_OK);
        CHECK(cs_static.word_count > 0u && cs_static.packet_count > 0u && cs_static.reloc_count == 0u);
        CHECK(cs_static.pipeline_kind == A840_SDS_PIPELINE_COMPUTE);
        CHECK(!stream_has_reg(cs_static.words, cs_static.word_count, A840_REG_SP_CS_BASE));
        CHECK(!stream_has_reg(cs_static.words, cs_static.word_count, A840_REG_SP_CS_PVT_MEM_BASE));
        CHECK(stream_has_reg(cs_static.words, cs_static.word_count, A840_REG_SP_MODE_CNTL));
        CHECK(stream_has_reg(cs_static.words, cs_static.word_count, A840_REG_SP_CS_CNTL_0));
        CHECK(stream_has_reg(cs_static.words, cs_static.word_count, A840_REG_SP_CS_INSTR_SIZE));
        CHECK(stream_has_reg(cs_static.words, cs_static.word_count, A840_REG_SP_CS_PROGRAM_COUNTER_OFFSET));
        CHECK(stream_has_reg(cs_static.words, cs_static.word_count, A840_REG_SP_CS_CNTL_1));
        CHECK(stream_has_reg(cs_static.words, cs_static.word_count, A840_REG_SP_CS_WGE_CNTL));
        CHECK(stream_has_reg(cs_static.words, cs_static.word_count, A840_REG_SP_CS_WIE_CNTL_0));
        uint32_t pkts = 0u;
        CHECK(a840_pack_sds_verify(cs_static.words, cs_static.word_count,
                                   A840_SDS_MODE_STATIC_ONLY, &pkts) == A840_SDS_OK);
        CHECK(pkts == cs_static.packet_count);
    }

    CASE("compute_static_only_with_private_mem_strips_base_keeps_params");
    {
        auto desc = make_compute_desc(cs_pvt_fix);
        A840SdsBlock blk{};
        CHECK(a840_pack_sds_preencode_compute(&desc, A840_SDS_MODE_STATIC_ONLY, &blk) == A840_SDS_OK);
        CHECK(blk.reloc_count == 0u);
        CHECK(!stream_has_reg(blk.words, blk.word_count, A840_REG_SP_CS_BASE));
        CHECK(!stream_has_reg(blk.words, blk.word_count, A840_REG_SP_CS_PVT_MEM_BASE));
        CHECK(stream_has_reg(blk.words, blk.word_count, A840_REG_SP_CS_PVT_MEM_PARAM));
        CHECK(stream_has_reg(blk.words, blk.word_count, A840_REG_SP_CS_PVT_MEM_SIZE));
        CHECK(stream_has_reg(blk.words, blk.word_count, A840_REG_SP_CS_PVT_MEM_STACK_OFFSET));
    }

    CASE("compute_relocatable_template_code_only");
    {
        auto desc = make_compute_desc(cs_fix);
        CHECK(a840_pack_sds_preencode_compute(&desc, A840_SDS_MODE_RELOCATABLE_TEMPLATE, &cs_reloc) == A840_SDS_OK);
        CHECK(cs_reloc.reloc_count == 1u);
        CHECK(cs_reloc.relocs[0].kind == A840_SDS_RELOC_CS_CODE);
        CHECK(cs_reloc.relocs[0].reg_id == A840_REG_SP_CS_BASE);
        CHECK(cs_reloc.words[cs_reloc.relocs[0].dword_offset] == 0u);
        CHECK(cs_reloc.words[cs_reloc.relocs[0].dword_offset + 1u] == 0u);
    }

    CASE("compute_relocatable_template_with_pvt_mem_and_patch");
    {
        auto desc = make_compute_desc(cs_pvt_fix);
        A840SdsBlock tmpl{};
        CHECK(a840_pack_sds_preencode_compute(&desc, A840_SDS_MODE_RELOCATABLE_TEMPLATE, &tmpl) == A840_SDS_OK);
        CHECK(tmpl.reloc_count == 2u);
        CHECK(tmpl.relocs[0].kind == A840_SDS_RELOC_CS_CODE);
        CHECK(tmpl.relocs[1].kind == A840_SDS_RELOC_CS_PVT_MEM);
        std::array<uint32_t, A840_PACK_SDS_MAX_DWORDS> patched{};
        A840SdsPatchAddrs addrs{};
        addrs.cs_code_va = UINT64_C(0x0000100020000080);
        addrs.cs_pvt_va = UINT64_C(0x0000100030000020);
        CHECK(a840_pack_sds_patch_relocs(patched.data(), tmpl.word_count, &tmpl, &addrs) == A840_SDS_OK);
        CHECK(patched[tmpl.relocs[0].dword_offset] == static_cast<uint32_t>(addrs.cs_code_va));
        CHECK(patched[tmpl.relocs[0].dword_offset + 1u] == static_cast<uint32_t>(addrs.cs_code_va >> 32));
        CHECK(patched[tmpl.relocs[1].dword_offset] == static_cast<uint32_t>(addrs.cs_pvt_va));
        CHECK(patched[tmpl.relocs[1].dword_offset + 1u] == static_cast<uint32_t>(addrs.cs_pvt_va >> 32));
    }

    CASE("graphics_static_only_preencode");
    {
        auto desc = make_graphics_desc(vs_fix, fs_fix, false);
        const uint32_t mask = A840_SDS_BINNING | A840_SDS_GMEM | A840_SDS_SYSMEM;
        CHECK(a840_pack_sds_preencode_graphics(&desc, A840_SDS_MODE_STATIC_ONLY, mask, &gfx_static) == A840_SDS_OK);
        CHECK(gfx_static.word_count > 0u && gfx_static.packet_count > 0u && gfx_static.reloc_count == 0u);
        CHECK(gfx_static.pipeline_kind == A840_SDS_PIPELINE_GRAPHICS);
        CHECK(gfx_static.enable_mask == mask);
        CHECK(!stream_has_reg(gfx_static.words, gfx_static.word_count, A840_REG_SP_VS_BASE));
        CHECK(!stream_has_reg(gfx_static.words, gfx_static.word_count, A840_REG_SP_PS_BASE));
        CHECK(!stream_has_reg(gfx_static.words, gfx_static.word_count, A840_REG_SP_VS_PVT_MEM_BASE));
        CHECK(!stream_has_reg(gfx_static.words, gfx_static.word_count, A840_REG_SP_PS_PVT_MEM_BASE));
        CHECK(stream_has_reg(gfx_static.words, gfx_static.word_count, A840_REG_SP_VS_CNTL_0));
        CHECK(stream_has_reg(gfx_static.words, gfx_static.word_count, A840_REG_SP_PS_CNTL_0));
        CHECK(stream_has_reg(gfx_static.words, gfx_static.word_count, A840_REG_SP_VS_INSTR_SIZE));
        CHECK(stream_has_reg(gfx_static.words, gfx_static.word_count, A840_REG_SP_PS_INSTR_SIZE));
        CHECK(stream_has_reg(gfx_static.words, gfx_static.word_count, A840_REG_VFD_CNTL_0));
        CHECK(stream_has_reg(gfx_static.words, gfx_static.word_count, A840_REG_SP_VS_OUTPUT_0_REG));
        CHECK(stream_has_reg(gfx_static.words, gfx_static.word_count, A840_REG_SP_PS_OUTPUT_0_REG));
        CHECK(stream_has_reg(gfx_static.words, gfx_static.word_count, A840_REG_RB_PS_OUTPUT_MASK));
    }

    CASE("graphics_fbfetch_gmem_static_only_with_depth_and_pvt");
    {
        auto desc = make_graphics_desc(vs_pvt_fix, fs_fb_fix, true);
        CHECK(a840_pack_sds_preencode_graphics(&desc, A840_SDS_MODE_STATIC_ONLY,
                                               A840_SDS_GMEM, &gfx_fbfetch) == A840_SDS_OK);
        CHECK(gfx_fbfetch.reloc_count == 0u && gfx_fbfetch.enable_mask == A840_SDS_GMEM);
        CHECK(!stream_has_reg(gfx_fbfetch.words, gfx_fbfetch.word_count, A840_REG_SP_PS_BASE));
        CHECK(!stream_has_reg(gfx_fbfetch.words, gfx_fbfetch.word_count, A840_REG_SP_PS_PVT_MEM_BASE));
        CHECK(stream_has_reg(gfx_fbfetch.words, gfx_fbfetch.word_count, A840_REG_SP_PS_PVT_MEM_SIZE));
    }

    CASE("graphics_relocatable_template_code_only");
    {
        auto desc = make_graphics_desc(vs_fix, fs_fix, false);
        CHECK(a840_pack_sds_preencode_graphics(&desc, A840_SDS_MODE_RELOCATABLE_TEMPLATE,
                                               A840_SDS_GMEM | A840_SDS_SYSMEM, &gfx_reloc) == A840_SDS_OK);
        CHECK(gfx_reloc.reloc_count == 2u);
        CHECK(gfx_reloc.relocs[0].kind == A840_SDS_RELOC_VS_CODE);
        CHECK(gfx_reloc.relocs[1].kind == A840_SDS_RELOC_FS_CODE);
    }

    CASE("graphics_relocatable_template_4_relocs_patch");
    {
        auto desc = make_graphics_desc(vs_pvt_fix, fs_fb_fix, true);
        A840SdsBlock tmpl{};
        CHECK(a840_pack_sds_preencode_graphics(&desc, A840_SDS_MODE_RELOCATABLE_TEMPLATE,
                                               A840_SDS_GMEM, &tmpl) == A840_SDS_OK);
        CHECK(tmpl.reloc_count == 4u);
        std::array<uint32_t, A840_PACK_SDS_MAX_DWORDS> patched{};
        A840SdsPatchAddrs addrs{};
        addrs.vs_code_va = UINT64_C(0x0000010000100000);
        addrs.vs_pvt_va  = UINT64_C(0x0000010000200000);
        addrs.fs_code_va = UINT64_C(0x0000010000300000);
        addrs.fs_pvt_va  = UINT64_C(0x0000010000400000);
        CHECK(a840_pack_sds_patch_relocs(patched.data(), tmpl.word_count, &tmpl, &addrs) == A840_SDS_OK);
        uint32_t pkts = 0u;
        CHECK(a840_pack_sds_verify(patched.data(), tmpl.word_count,
                                   A840_SDS_MODE_RELOCATABLE_TEMPLATE, &pkts) == A840_SDS_OK);
        CHECK(pkts == tmpl.packet_count);
    }

    CASE("preencode_is_deterministic");
    {
        auto cd = make_compute_desc(cs_fix);
        auto gd = make_graphics_desc(vs_fix, fs_fix, false);
        A840SdsBlock c2{}, g2{};
        CHECK(a840_pack_sds_preencode_compute(&cd, A840_SDS_MODE_STATIC_ONLY, &c2) == A840_SDS_OK);
        CHECK(a840_pack_sds_preencode_graphics(&gd, A840_SDS_MODE_STATIC_ONLY,
                                               A840_SDS_BINNING | A840_SDS_GMEM | A840_SDS_SYSMEM,
                                               &g2) == A840_SDS_OK);
        CHECK(c2.word_count == cs_static.word_count);
        CHECK(std::memcmp(c2.words, cs_static.words, cs_static.word_count * sizeof(uint32_t)) == 0);
        CHECK(std::memcmp(c2.sha256, cs_static.sha256, 32u) == 0);
        CHECK(g2.word_count == gfx_static.word_count);
        CHECK(std::memcmp(g2.words, gfx_static.words, gfx_static.word_count * sizeof(uint32_t)) == 0);
        CHECK(std::memcmp(g2.sha256, gfx_static.sha256, 32u) == 0);
    }

    CASE("pack_v1_compute_sds_roundtrip_zero_copy_readonly_mmap");
    {
        StageFixture cs_copy = cs_fix;
        CHECK(a840_pack_sds_attach_stage(&cs_static, &cs_copy.stage) == A840_SDS_OK);
        CHECK(cs_copy.stage.sds_state_words == cs_static.words);
        CHECK(cs_copy.stage.sds_state_dwords == cs_static.word_count);

        A840BindingPlan plan{};
        A840PipelineRecord rec = make_record(1u, &cs_copy.stage, nullptr, &plan);
        std::array<A840PackV1BuilderItem, 8> items{};
        std::array<uint32_t, 32> buckets{};
        A840PackV1Builder b{};
        CHECK(a840_pack_v1_builder_init(&b, 0u, TEST_STACK_KEY, a840_compile_abi_fingerprint(),
                                        items.data(), items.size(), buckets.data(), buckets.size()) == A840_PACK_OK);
        CHECK(a840_pack_v1_builder_add(&b, &rec, &plan) == A840_PACK_OK);
        std::size_t sz = 0u;
        CHECK(a840_pack_v1_builder_measure(&b, &sz) == A840_PACK_OK);
        AlignedBuf buf(sz);
        std::size_t built = 0u;
        CHECK(a840_pack_v1_builder_build(&b, buf.data, buf.bytes, &built) == A840_PACK_OK);
        CHECK(built == sz);

        A840PackV1View view{};
        CHECK(a840_pack_v1_open_memory(buf.data, buf.bytes, TEST_STACK_KEY,
                                       a840_compile_abi_fingerprint(), &view) == A840_PACK_OK);
        A840PackV1Entry entry{};
        CHECK(a840_pack_v1_lookup(&view, &rec.pipeline_key, &entry) == A840_PACK_OK);
        CHECK(entry.stages[0].sds_state_dwords == cs_static.word_count);
        CHECK(reinterpret_cast<const uint8_t *>(entry.stages[0].sds_state_words) >= buf.data &&
              reinterpret_cast<const uint8_t *>(entry.stages[0].sds_state_words) +
              cs_static.word_count * sizeof(uint32_t) <= buf.data + buf.bytes);
        CHECK(std::memcmp(entry.stages[0].sds_state_words, cs_static.words,
                          cs_static.word_count * sizeof(uint32_t)) == 0);
        CHECK(a840_pack_sds_verify(entry.stages[0].sds_state_words,
                                   entry.stages[0].sds_state_dwords,
                                   A840_SDS_MODE_STATIC_ONLY, nullptr) == A840_SDS_OK);
    }

    CASE("pack_v1_graphics_sds_roundtrip_in_memory");
    {
        StageFixture vs_copy = vs_fix, fs_copy = fs_fix;
        CHECK(a840_pack_sds_attach_stage(&gfx_static, &vs_copy.stage) == A840_SDS_OK);
        A840BindingPlan plan{};
        A840PipelineRecord rec = make_record(2u, &vs_copy.stage, &fs_copy.stage, &plan);
        std::array<A840PackV1BuilderItem, 8> items{};
        std::array<uint32_t, 32> buckets{};
        A840PackV1Builder b{};
        CHECK(a840_pack_v1_builder_init(&b, 0u, TEST_STACK_KEY, a840_compile_abi_fingerprint(),
                                        items.data(), items.size(), buckets.data(), buckets.size()) == A840_PACK_OK);
        CHECK(a840_pack_v1_builder_add(&b, &rec, &plan) == A840_PACK_OK);
        std::size_t sz = 0u;
        CHECK(a840_pack_v1_builder_measure(&b, &sz) == A840_PACK_OK);
        AlignedBuf buf(sz);
        std::size_t built = 0u;
        CHECK(a840_pack_v1_builder_build(&b, buf.data, buf.bytes, &built) == A840_PACK_OK);
        A840PackV1View view{};
        CHECK(a840_pack_v1_open_memory(buf.data, buf.bytes, TEST_STACK_KEY,
                                       a840_compile_abi_fingerprint(), &view) == A840_PACK_OK);
        A840PackV1Entry entry{};
        CHECK(a840_pack_v1_lookup(&view, &rec.pipeline_key, &entry) == A840_PACK_OK);
        CHECK(entry.stage_count == 2u);
        CHECK(entry.stages[0].sds_state_dwords == gfx_static.word_count);
        CHECK(std::memcmp(entry.stages[0].sds_state_words, gfx_static.words,
                          gfx_static.word_count * sizeof(uint32_t)) == 0);
        CHECK(entry.stages[1].sds_state_dwords == 0u);
    }

    CASE("pack_v1_multi_pipeline_disk_save_and_readonly_mmap_zero_copy_bind");
    {
        StageFixture s_cs = cs_fix, s_vs1 = vs_fix, s_fs1 = fs_fix, s_vs2 = vs_pvt_fix, s_fs2 = fs_fb_fix;
        StageFixture s_vs3 = vs_fix, s_fs3 = fs_fix;
        CHECK(a840_pack_sds_attach_stage(&cs_static, &s_cs.stage) == A840_SDS_OK);
        CHECK(a840_pack_sds_attach_stage(&gfx_static, &s_vs1.stage) == A840_SDS_OK);
        CHECK(a840_pack_sds_attach_stage(&gfx_fbfetch, &s_vs2.stage) == A840_SDS_OK);
        CHECK(a840_pack_sds_attach_stage(&gfx_reloc, &s_vs3.stage) == A840_SDS_OK);

        A840BindingPlan p1{}, p2{}, p3{}, p4{};
        A840PipelineRecord r1 = make_record(10u, &s_cs.stage, nullptr, &p1);
        A840PipelineRecord r2 = make_record(20u, &s_vs1.stage, &s_fs1.stage, &p2);
        A840PipelineRecord r3 = make_record(30u, &s_vs2.stage, &s_fs2.stage, &p3);
        A840PipelineRecord r4 = make_record(40u, &s_vs3.stage, &s_fs3.stage, &p4);

        std::array<A840PackV1BuilderItem, 16> items{};
        std::array<uint32_t, 64> buckets{};
        A840PackV1Builder b{};
        CHECK(a840_pack_v1_builder_init(&b, 0u, TEST_STACK_KEY, a840_compile_abi_fingerprint(),
                                        items.data(), items.size(), buckets.data(), buckets.size()) == A840_PACK_OK);
        CHECK(a840_pack_v1_builder_add(&b, &r1, &p1) == A840_PACK_OK);
        CHECK(a840_pack_v1_builder_add(&b, &r2, &p2) == A840_PACK_OK);
        CHECK(a840_pack_v1_builder_add(&b, &r3, &p3) == A840_PACK_OK);
        CHECK(a840_pack_v1_builder_add(&b, &r4, &p4) == A840_PACK_OK);
        std::size_t sz = 0u;
        CHECK(a840_pack_v1_builder_measure(&b, &sz) == A840_PACK_OK);
        AlignedBuf buf(sz);
        std::size_t built = 0u;
        CHECK(a840_pack_v1_builder_build(&b, buf.data, buf.bytes, &built) == A840_PACK_OK);

        std::filesystem::path pack_path = export_dir.empty()
            ? std::filesystem::path("/home/hua/nezha-work/ios27/adreno840-native/.cache/w06-l2/sds-test-multi.pack.bin")
            : (export_dir / "sds-test-multi.pack.bin");
        std::filesystem::create_directories(pack_path.parent_path());
        CHECK(a840_pack_v1_save_file(pack_path.c_str(), buf.data, buf.bytes,
                                     TEST_STACK_KEY, a840_compile_abi_fingerprint()) == A840_PACK_OK);

        int fd = ::open(pack_path.c_str(), O_RDONLY);
        CHECK(fd >= 0);
        struct stat st{};
        CHECK(::fstat(fd, &st) == 0 && static_cast<std::size_t>(st.st_size) == sz);
        void *mapped = ::mmap(nullptr, sz, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        CHECK(mapped != MAP_FAILED);

        A840PackV1View view{};
        CHECK(a840_pack_v1_open_memory(mapped, sz, TEST_STACK_KEY,
                                       a840_compile_abi_fingerprint(), &view) == A840_PACK_OK);
        CHECK(view.entry_count == 4u);

        A840PackV1Entry e1{}, e2{}, e3{}, e4{};
        CHECK(a840_pack_v1_lookup(&view, &r1.pipeline_key, &e1) == A840_PACK_OK);
        CHECK(a840_pack_v1_lookup(&view, &r2.pipeline_key, &e2) == A840_PACK_OK);
        CHECK(a840_pack_v1_lookup(&view, &r3.pipeline_key, &e3) == A840_PACK_OK);
        CHECK(a840_pack_v1_lookup(&view, &r4.pipeline_key, &e4) == A840_PACK_OK);

        CHECK(std::memcmp(e1.stages[0].sds_state_words, cs_static.words, cs_static.word_count * 4u) == 0);
        CHECK(std::memcmp(e2.stages[0].sds_state_words, gfx_static.words, gfx_static.word_count * 4u) == 0);
        CHECK(std::memcmp(e3.stages[0].sds_state_words, gfx_fbfetch.words, gfx_fbfetch.word_count * 4u) == 0);
        CHECK(std::memcmp(e4.stages[0].sds_state_words, gfx_reloc.words, gfx_reloc.word_count * 4u) == 0);

        CASE("zero_copy_cp_set_draw_state_bind_from_mmap");
        const uint64_t pack_gpu_base = UINT64_C(0x0000004000000000);
        const auto *base_u8 = static_cast<const uint8_t *>(mapped);
        const uint64_t gfx_off = static_cast<uint64_t>(
            reinterpret_cast<const uint8_t *>(e2.stages[0].sds_state_words) - base_u8);
        const uint64_t fb_off = static_cast<uint64_t>(
            reinterpret_cast<const uint8_t *>(e3.stages[0].sds_state_words) - base_u8);
        CHECK((gfx_off % 4u) == 0u && (fb_off % 4u) == 0u);

        std::array<uint32_t, 64> bind_words{};
        std::array<A840Reloc, 8> bind_relocs{};
        A840CmdStream bind_cs{};
        CHECK(a840_cs_init(&bind_cs, bind_words.data(), bind_words.size(),
                           bind_relocs.data(), bind_relocs.size()) == A840_EMIT_OK);
        CHECK(a840_pack_sds_emit_bind(&bind_cs, 0u, A840_SDS_BINNING | A840_SDS_GMEM | A840_SDS_SYSMEM,
                                      A840GpuAddress{77u, pack_gpu_base + gfx_off},
                                      e2.stages[0].sds_state_words, e2.stages[0].sds_state_dwords) == A840_SDS_OK);
        CHECK(a840_pack_sds_emit_bind(&bind_cs, 1u, A840_SDS_GMEM,
                                      A840GpuAddress{77u, pack_gpu_base + fb_off},
                                      e3.stages[0].sds_state_words, e3.stages[0].sds_state_dwords) == A840_SDS_OK);
        CHECK(bind_cs.used_dwords == 8u && bind_cs.reloc_count == 2u);
        CHECK(bind_relocs[0].flags == (A840_RELOC_SDS | A840_RELOC_READ));
        CHECK(bind_relocs[0].gpu_va == pack_gpu_base + gfx_off);
        CHECK(bind_relocs[1].gpu_va == pack_gpu_base + fb_off);

        std::array<A840PM4Packet, 8> pkts{};
        std::size_t pkt_cnt = 0u;
        CHECK(a840_pm4_decode(bind_words.data(), bind_cs.used_dwords, pkts.data(), pkts.size(), &pkt_cnt) == A840_EMIT_OK);
        CHECK(pkt_cnt == 2u && pkts[0].type == 7u && pkts[0].identifier == A840_CP_SET_DRAW_STATE);
        CHECK(pkts[1].type == 7u && pkts[1].identifier == A840_CP_SET_DRAW_STATE);

        if (!export_dir.empty()) {
            CHECK(a840_pack_sds_export_pm4_file((export_dir / "bind_sds_groups.pm4").c_str(),
                                                bind_words.data(),
                                                static_cast<uint32_t>(bind_cs.used_dwords)) == A840_SDS_OK);
        }

        CASE("cold_entry_record_interop_preserves_sds");
        A840PipelineRecord cold{};
        CHECK(a840_pack_v1_entry_record(&view, &e2, &cold) == A840_PACK_OK);
        CHECK(cold.owned_storage == nullptr);
        CHECK(cold.stages[0].sds_state_words == e2.stages[0].sds_state_words);
        CHECK(cold.stages[0].sds_state_dwords == gfx_static.word_count);

        CHECK(::munmap(mapped, sz) == 0);
    }

    CASE("verify_rejects_corrupted_parity");
    {
        A840SdsBlock bad = cs_static;
        bad.words[0] ^= (UINT32_C(1) << 7);
        CHECK(a840_pack_sds_verify(bad.words, bad.word_count,
                                   A840_SDS_MODE_STATIC_ONLY, nullptr) == A840_SDS_VERIFY_ERROR);
    }

    CASE("verify_rejects_truncated_stream");
    {
        CHECK(a840_pack_sds_verify(cs_static.words, cs_static.word_count - 1u,
                                   A840_SDS_MODE_STATIC_ONLY, nullptr) == A840_SDS_VERIFY_ERROR);
    }

    CASE("verify_rejects_type7_packet_in_sds");
    {
        std::array<uint32_t, 8> t7{};
        A840CmdStream s{};
        CHECK(a840_cs_init(&s, t7.data(), t7.size(), nullptr, 0u) == A840_EMIT_OK);
        CHECK(a840_cs_emit_pkt7(&s, A840_CP_WAIT_FOR_IDLE, nullptr, 0u) == A840_EMIT_OK);
        CHECK(a840_pack_sds_verify(t7.data(), static_cast<uint32_t>(s.used_dwords),
                                   A840_SDS_MODE_STATIC_ONLY, nullptr) == A840_SDS_VERIFY_ERROR);
        CHECK(a840_pack_sds_verify(t7.data(), static_cast<uint32_t>(s.used_dwords),
                                   A840_SDS_MODE_RELOCATABLE_TEMPLATE, nullptr) == A840_SDS_VERIFY_ERROR);
    }

    CASE("verify_rejects_bo_va_register_in_static_only_mode");
    {
        CHECK(a840_pack_sds_verify(cs_reloc.words, cs_reloc.word_count,
                                   A840_SDS_MODE_STATIC_ONLY, nullptr) == A840_SDS_VERIFY_ERROR);
        CHECK(a840_pack_sds_verify(gfx_reloc.words, gfx_reloc.word_count,
                                   A840_SDS_MODE_STATIC_ONLY, nullptr) == A840_SDS_VERIFY_ERROR);
        CHECK(a840_pack_sds_verify(cs_reloc.words, cs_reloc.word_count,
                                   A840_SDS_MODE_RELOCATABLE_TEMPLATE, nullptr) == A840_SDS_OK);
    }

    CASE("attach_stage_rejects_tampered_sha_and_preserves_stage");
    {
        A840SdsBlock tampered = cs_static;
        tampered.sha256[0] ^= 0xffu;
        A840CompiledStage st = cs_fix.stage;
        const A840CompiledStage before = st;
        CHECK(a840_pack_sds_attach_stage(&tampered, &st) == A840_SDS_VERIFY_ERROR);
        CHECK(std::memcmp(&st, &before, sizeof(st)) == 0);
        A840SdsBlock bad_cnt = cs_static;
        bad_cnt.packet_count += 1u;
        CHECK(a840_pack_sds_attach_stage(&bad_cnt, &st) == A840_SDS_VERIFY_ERROR);
        CHECK(std::memcmp(&st, &before, sizeof(st)) == 0);
    }

    CASE("preencode_compute_invalid_args_transactional");
    {
        A840SdsBlock sentinel{};
        std::memset(&sentinel, 0x5a, sizeof(sentinel));
        const A840SdsBlock before = sentinel;
        CHECK(a840_pack_sds_preencode_compute(nullptr, A840_SDS_MODE_STATIC_ONLY, &sentinel) == A840_SDS_INVALID_ARG);
        CHECK(std::memcmp(&sentinel, &before, sizeof(sentinel)) == 0);

        auto bad_stage = make_compute_desc(cs_fix);
        bad_stage.shader.stage = &fs_fix.stage;
        CHECK(a840_pack_sds_preencode_compute(&bad_stage, A840_SDS_MODE_STATIC_ONLY, &sentinel) == A840_SDS_INVALID_ARG);
        CHECK(std::memcmp(&sentinel, &before, sizeof(sentinel)) == 0);

        auto good = make_compute_desc(cs_fix);
        CHECK(a840_pack_sds_preencode_compute(&good, static_cast<A840SdsEncodeMode>(99), &sentinel) == A840_SDS_INVALID_ARG);
        CHECK(std::memcmp(&sentinel, &before, sizeof(sentinel)) == 0);
    }

    CASE("preencode_graphics_invalid_args_transactional");
    {
        A840SdsBlock sentinel{};
        std::memset(&sentinel, 0x5a, sizeof(sentinel));
        const A840SdsBlock before = sentinel;
        auto desc = make_graphics_desc(vs_fix, fs_fix, false);
        CHECK(a840_pack_sds_preencode_graphics(&desc, A840_SDS_MODE_STATIC_ONLY, 0u, &sentinel) == A840_SDS_INVALID_ARG);
        CHECK(a840_pack_sds_preencode_graphics(&desc, A840_SDS_MODE_STATIC_ONLY, UINT32_C(1) << 30, &sentinel) == A840_SDS_INVALID_ARG);
        auto swapped = desc;
        swapped.vs.stage = &fs_fix.stage;
        CHECK(a840_pack_sds_preencode_graphics(&swapped, A840_SDS_MODE_STATIC_ONLY, A840_SDS_GMEM, &sentinel) == A840_SDS_INVALID_ARG);
        CHECK(std::memcmp(&sentinel, &before, sizeof(sentinel)) == 0);
    }

    CASE("patch_relocs_rejects_bad_va_and_static_template_transactional");
    {
        std::array<uint32_t, A840_PACK_SDS_MAX_DWORDS> dst{};
        dst.fill(0xdeadbeefu);
        const auto before = dst;
        A840SdsPatchAddrs addrs{};
        addrs.cs_code_va = UINT64_C(0x10000);
        CHECK(a840_pack_sds_patch_relocs(dst.data(), cs_static.word_count, &cs_static, &addrs) == A840_SDS_INVALID_ARG);
        addrs.cs_code_va = UINT64_C(0x10004);
        CHECK(a840_pack_sds_patch_relocs(dst.data(), cs_reloc.word_count, &cs_reloc, &addrs) == A840_SDS_RANGE_ERROR);
        addrs.cs_code_va = UINT64_C(1) << 48;
        CHECK(a840_pack_sds_patch_relocs(dst.data(), cs_reloc.word_count, &cs_reloc, &addrs) == A840_SDS_RANGE_ERROR);
        addrs.cs_code_va = 0u;
        CHECK(a840_pack_sds_patch_relocs(dst.data(), cs_reloc.word_count, &cs_reloc, &addrs) == A840_SDS_RANGE_ERROR);
        CHECK(dst == before);
    }

    CASE("emit_bind_rejects_bad_slot_mask_alignment_and_capacity");
    {
        std::array<uint32_t, 4> words{};
        words.fill(0xa5a5a5a5u);
        const auto before = words;
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        CHECK(a840_pack_sds_emit_bind(&cs, ADRENO840_MAX_SDS_SLOTS, A840_SDS_GMEM,
                                      A840GpuAddress{1u, 0x10000u},
                                      cs_static.words, cs_static.word_count) == A840_SDS_INVALID_ARG);
        CHECK(a840_pack_sds_emit_bind(&cs, 0u, 0u,
                                      A840GpuAddress{1u, 0x10000u},
                                      cs_static.words, cs_static.word_count) == A840_SDS_INVALID_ARG);
        CHECK(a840_pack_sds_emit_bind(&cs, 0u, A840_SDS_GMEM,
                                      A840GpuAddress{1u, 0x10002u},
                                      cs_static.words, cs_static.word_count) == A840_SDS_EMIT_ERROR);
        A840CmdStream tiny{};
        CHECK(a840_cs_init(&tiny, words.data(), 3u, nullptr, 0u) == A840_EMIT_OK);
        CHECK(a840_pack_sds_emit_bind(&tiny, 0u, A840_SDS_GMEM,
                                      A840GpuAddress{1u, 0x10000u},
                                      cs_static.words, cs_static.word_count) == A840_SDS_CAPACITY);
        CHECK(words == before && cs.used_dwords == 0u && tiny.used_dwords == 0u);
    }

    CASE("export_pm4_rejects_c_drive_and_malformed_stream");
    {
        CHECK(a840_pack_sds_export_pm4_file("C:/temp/bad.pm4", cs_static.words, cs_static.word_count) == A840_SDS_INVALID_ARG);
        CHECK(a840_pack_sds_export_pm4_file("/mnt/c/bad.pm4", cs_static.words, cs_static.word_count) == A840_SDS_INVALID_ARG);
        CHECK(a840_pack_sds_export_pm4_file("", cs_static.words, cs_static.word_count) == A840_SDS_INVALID_ARG);
        uint32_t bad_word = 0xffffffffu;
        CHECK(a840_pack_sds_export_pm4_file("/home/hua/nezha-work/ios27/adreno840-native/.cache/w06-l2/never.pm4",
                                            &bad_word, 1u) == A840_SDS_VERIFY_ERROR);
    }

    CASE("export_pm4_files_for_pm4_check_a8xx");
    {
        std::filesystem::path dir = export_dir.empty()
            ? std::filesystem::path("/home/hua/nezha-work/ios27/adreno840-native/.cache/w06-l2/sds-pm4")
            : export_dir;
        std::filesystem::create_directories(dir);

        std::array<uint32_t, A840_PACK_SDS_MAX_DWORDS> cs_patched{};
        A840SdsPatchAddrs cs_a{};
        cs_a.cs_code_va = UINT64_C(0x0000002000100000);
        CHECK(a840_pack_sds_patch_relocs(cs_patched.data(), cs_reloc.word_count, &cs_reloc, &cs_a) == A840_SDS_OK);

        std::array<uint32_t, A840_PACK_SDS_MAX_DWORDS> gfx_patched{};
        A840SdsPatchAddrs gfx_a{};
        gfx_a.vs_code_va = UINT64_C(0x0000002000200000);
        gfx_a.fs_code_va = UINT64_C(0x0000002000300000);
        CHECK(a840_pack_sds_patch_relocs(gfx_patched.data(), gfx_reloc.word_count, &gfx_reloc, &gfx_a) == A840_SDS_OK);

        auto verify_export = [&](const char *fname, const uint32_t *w, uint32_t wc) {
            auto p = dir / fname;
            CHECK(a840_pack_sds_export_pm4_file(p.c_str(), w, wc) == A840_SDS_OK);
            std::ifstream in(p, std::ios::binary | std::ios::ate);
            CHECK(in.good());
            auto fsz = static_cast<std::size_t>(in.tellg());
            CHECK(fsz == static_cast<std::size_t>(wc) * sizeof(uint32_t));
            in.seekg(0);
            std::vector<uint8_t> raw(fsz);
            in.read(reinterpret_cast<char *>(raw.data()), static_cast<std::streamsize>(fsz));
            CHECK(in.good());
            for (uint32_t i = 0; i < wc; ++i) {
                uint32_t got = static_cast<uint32_t>(raw[i * 4u]) |
                               (static_cast<uint32_t>(raw[i * 4u + 1u]) << 8) |
                               (static_cast<uint32_t>(raw[i * 4u + 2u]) << 16) |
                               (static_cast<uint32_t>(raw[i * 4u + 3u]) << 24);
                CHECK(got == w[i]);
            }
        };

        verify_export("compute_static_sds.pm4", cs_static.words, cs_static.word_count);
        verify_export("compute_reloc_patched.pm4", cs_patched.data(), cs_reloc.word_count);
        verify_export("graphics_static_sds.pm4", gfx_static.words, gfx_static.word_count);
        verify_export("graphics_fbfetch_sds.pm4", gfx_fbfetch.words, gfx_fbfetch.word_count);
        verify_export("graphics_reloc_patched.pm4", gfx_patched.data(), gfx_reloc.word_count);
    }

    CASE("status_strings_non_null");
    {
        for (int s = 0; s <= static_cast<int>(A840_SDS_IO_ERROR) + 1; ++s) {
            const char *txt = a840_pack_sds_status_string(static_cast<A840SdsPreencodeStatus>(s));
            CHECK(txt != nullptr && txt[0] != '\0');
        }
    }

    CHECK(g_cases >= 20u);
    std::printf("PASS: test_pack_sds_preencode (%zu cases / %zu checks)\n", g_cases, g_checks);
    return 0;
}
