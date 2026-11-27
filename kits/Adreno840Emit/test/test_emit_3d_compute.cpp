/* SPDX-License-Identifier: MIT */
#include "a840_emit_3d.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

static unsigned checks, cases;
static const char *active_case;

#define CHECK(expr) do { \
    ++checks; \
    if (!(expr)) { \
        std::fprintf(stderr, "%s:%d [%s] %s\n", __FILE__, __LINE__, active_case, #expr); \
        std::exit(1); \
    } \
} while (0)

static void scenario(const char *name)
{
    active_case = name;
    ++cases;
}

struct Stream {
    alignas(A840CompiledStage) std::array<uint32_t, 4096> words;
    std::array<A840Reloc, 80> relocs;
    A840CmdStream cs{};

    explicit Stream(bool prefix = false)
    {
        words.fill(0xa5a5a5a5);
        for (auto &reloc : relocs)
            reloc = {0xa5a5a5a5, 0xa5a5a5a5, UINT64_C(0xa5a5a5a5a5a5a5a5), UINT64_C(0xa5a5a5a5a5a5a5a5)};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        if (prefix) {
            CHECK(a840_cs_emit_reg64_va(&cs, A840_REG_SP_CS_BASE, 9, 0x8000,
                                       A840_RELOC_READ | A840_RELOC_SHADER) == A840_EMIT_OK);
        }
    }
};

struct Packets {
    const Stream &stream;
    std::array<A840PM4Packet, 384> entries{};
    size_t count = 0;

    explicit Packets(const Stream &s) : stream(s)
    {
        CHECK(a840_pm4_decode(stream.words.data(), stream.cs.used_dwords,
                              entries.data(), entries.size(), &count) == A840_EMIT_OK);
    }

    size_t opcode_count(uint32_t opcode) const
    {
        size_t matches = 0;
        for (size_t i = 0; i < count; ++i)
            matches += entries[i].type == 7 && entries[i].identifier == opcode;
        return matches;
    }

    const A840PM4Packet &opcode(uint32_t code, size_t occurrence = 0) const
    {
        for (size_t i = 0; i < count; ++i) {
            if (entries[i].type == 7 && entries[i].identifier == code && occurrence-- == 0)
                return entries[i];
        }
        CHECK(false);
        return entries[0];
    }

    uint32_t payload(const A840PM4Packet &packet, size_t index) const
    {
        CHECK(index < packet.payload_words);
        return stream.words[packet.header_word + 1 + index];
    }

    uint32_t reg(uint32_t number) const
    {
        for (size_t i = count; i > 0; --i) {
            const auto &packet = entries[i - 1];
            if (packet.type == 4 && number >= packet.identifier &&
                number - packet.identifier < packet.payload_words)
                return payload(packet, number - packet.identifier);
        }
        CHECK(false);
        return 0;
    }
};

template <typename Emit>
static void unchanged(Stream &s, Emit emit, A840EmitStatus expected)
{
    const auto before = s.words;
    const auto relocs = s.relocs;
    const auto used = s.cs.used_dwords;
    const auto count = s.cs.reloc_count;
    CHECK(emit(&s.cs) == expected);
    CHECK(s.cs.used_dwords == used);
    CHECK(s.cs.reloc_count == count);
    CHECK(s.words == before);
    CHECK(std::memcmp(s.relocs.data(), relocs.data(), sizeof(relocs)) == 0);
}

template <typename Emit>
static void exact_capacity(const char *name, size_t words, size_t relocs, Emit emit)
{
    scenario(name);
    Stream complete(true);
    const size_t prefix = complete.cs.used_dwords, prefix_relocs = complete.cs.reloc_count;
    complete.cs.capacity_dwords = prefix + words;
    complete.cs.reloc_capacity = prefix_relocs + relocs;
    CHECK(emit(&complete.cs) == A840_EMIT_OK);
    CHECK(complete.cs.used_dwords == prefix + words);
    CHECK(complete.cs.reloc_count == prefix_relocs + relocs);
    for (size_t i = 0; i < words; ++i) {
        Stream short_stream(true);
        short_stream.cs.capacity_dwords = prefix + i;
        unchanged(short_stream, emit, A840_EMIT_CAPACITY);
    }
    for (size_t i = 0; i < relocs; ++i) {
        Stream short_stream(true);
        short_stream.cs.reloc_capacity = prefix_relocs + i;
        unchanged(short_stream, emit, A840_EMIT_CAPACITY);
    }
}

static void reloc_is(const Stream &s, size_t num, A840GpuAddress address, uint32_t flags,
                     uint32_t tag = 0)
{
    CHECK(num < s.cs.reloc_count);
    const auto &r = s.relocs[num];
    CHECK(r.flags == flags);
    CHECK(r.bo_handle == address.bo_handle);
    CHECK(r.gpu_va == address.gpu_va);
    CHECK(r.dword_offset + 1 < s.cs.used_dwords);
    CHECK(s.words[r.dword_offset] == (static_cast<uint32_t>(address.gpu_va) | tag));
    CHECK(s.words[r.dword_offset + 1] == address.gpu_va >> 32);
}

static A840CompiledStage shader_stage(uint32_t kind, uint32_t instrlen = 5)
{
    A840CompiledStage stage{};
    stage.abi_version = ADRENO_COMPILE_ABI_VERSION;
    stage.stage = kind;
    stage.instruction_groups = instrlen * 8;
    stage.isa_dwords = instrlen * 32;
    stage.max_full_reg = 7;
    stage.max_half_reg = 3;
    stage.branchstack = 3;
    stage.constlen_vec4 = 128;
    stage.private_mem_bytes = 14;
    stage.flags_out = (1u << 3) | (1u << 4);
    stage.local_size[0] = 8;
    stage.local_size[1] = 4;
    stage.local_size[2] = 2;
    return stage;
}

static A840ShaderCode gpu_shader(const A840CompiledStage &stage, uint64_t ordinal)
{
    A840ShaderCode upload{};
    upload.stage = &stage;
    upload.code = {ordinal, UINT64_C(0x10210000000) + ordinal * 0x10000};
    upload.private_memory = {ordinal + 10, UINT64_C(0x10500000000) + ordinal * 0x1000000};
    upload.per_wave_private_memory = true;
    upload.bindless_tex = upload.bindless_uav = true;
    return upload;
}

static A840ComputePipelineDesc compute_pipeline(const A840CompiledStage &stage)
{
    A840ComputePipelineDesc desc{};
    desc.shader = gpu_shader(stage, 1);
    desc.local_id_regid = 4;
    desc.wg_id_regid = 8;
    return desc;
}

static A840BindlessTableDesc bindless()
{
    A840BindlessTableDesc desc{};
    desc.count = ADRENO_COMPILE_MAX_BIND_SETS;
    for (unsigned i = 0; i < desc.count; ++i)
        desc.tables[i] = {50u + i, UINT64_C(0x10200100000) + i * 4096u};
    return desc;
}

static A840DispatchDesc dispatch(const A840CompiledStage &stage, A840DispatchMode mode)
{
    A840DispatchDesc desc{};
    desc.stage = &stage;
    desc.mode = mode;
    if (mode == A840_DISPATCH_NON_UNIFORM) {
        desc.size[0] = 17; desc.size[1] = 7; desc.size[2] = 3;
    } else {
        desc.size[0] = 3; desc.size[1] = 2; desc.size[2] = 1;
    }
    desc.indirect = {70, UINT64_C(0x10000800040)};
    return desc;
}

static void test_pipeline()
{
    const uint32_t limits[] = {128, 132, 192, 196, 256, 260, 512};
    const uint32_t modes[] = {0, 1, 1, 2, 2, 3, 3};
    auto stage = shader_stage(A840_STAGE_COMPUTE);
    auto desc = compute_pipeline(stage);
    stage.shared_mem_bytes = 32768;
    for (size_t i = 0; i < std::size(limits); ++i) {
        scenario("const RAM threshold");
        Stream s;
        stage.constlen_vec4 = limits[i];
        CHECK(a840_emit_compute_pipeline(&s.cs, &desc) == A840_EMIT_OK);
        Packets p(s);
        CHECK(p.reg(A840_REG_SP_CS_CNTL_1) == (31u | modes[i] << 5));
        CHECK(p.reg(A840_REG_SP_CS_CONST_CONFIG) == (256u | limits[i] / 4u));
        CHECK(p.reg(A840_REG_SP_CS_INSTR_SIZE) == 5);
        CHECK(p.reg(A840_REG_SP_CS_WIE_CNTL_0) == 0x04fcfc08);
        CHECK(p.reg(A840_REG_SP_CS_WIE_CNTL_1) == 0x81fc);
        CHECK(p.reg(A840_REG_SP_CS_PVT_MEM_PARAM) == 1);
        CHECK(p.reg(A840_REG_SP_CS_PVT_MEM_SIZE) == (512u | 1u << 31));
        reloc_is(s, 0, desc.shader.code, A840_RELOC_READ | A840_RELOC_SHADER);
        reloc_is(s, 1, desc.shader.private_memory, A840_RELOC_READ | A840_RELOC_WRITE);
    }
    exact_capacity("compute pipeline capacity", A840_EMIT_COMPUTE_PIPELINE_MAX_DWORDS, 2,
        [&](auto *s) { return a840_emit_compute_pipeline(s, &desc); });

    scenario("linear item order and zero shared/private");
    Stream linear;
    stage.private_mem_bytes = stage.shared_mem_bytes = 0;
    desc.linear_dispatch = true;
    CHECK(a840_emit_compute_pipeline(&linear.cs, &desc) == A840_EMIT_OK);
    Packets p(linear);
    CHECK(p.reg(A840_REG_SP_CS_WIE_CNTL_1) == 0x1fc);
    CHECK(p.reg(A840_REG_SP_CS_PVT_MEM_BASE) == 0);
    CHECK(linear.cs.reloc_count == 1);

    Stream s(true);
    auto bad = stage;
    auto invalid = compute_pipeline(bad);
    auto emit = [&](auto *out) { return a840_emit_compute_pipeline(out, &invalid); };
    scenario("shared memory exceeds 32 KiB"); bad.shared_mem_bytes = 32769;
    unchanged(s, emit, A840_EMIT_RANGE_ERROR);
    scenario("empty local dimension"); bad = stage; bad.local_size[1] = 0;
    unchanged(s, emit, A840_EMIT_RANGE_ERROR);
    scenario("oversized workgroup"); bad = stage; bad.local_size[0] = 1024;
    unchanged(s, emit, A840_EMIT_RANGE_ERROR);
    scenario("compile ABI mismatch"); bad = stage; ++bad.abi_version;
    unchanged(s, emit, A840_EMIT_INVALID_ARG);
    scenario("wrong stage"); bad = stage; bad.stage = A840_STAGE_VERTEX;
    unchanged(s, emit, A840_EMIT_INVALID_ARG);
    scenario("const RAM exceeds 512 vec4"); bad = stage; bad.constlen_vec4 = 516;
    unchanged(s, emit, A840_EMIT_RANGE_ERROR);
    scenario("unaligned const RAM"); bad = stage; bad.constlen_vec4 = 125;
    unchanged(s, emit, A840_EMIT_ALIGNMENT);
    scenario("negative register footprint"); bad = stage; bad.max_full_reg = -2;
    unchanged(s, emit, A840_EMIT_RANGE_ERROR);
    scenario("zero instruction length"); bad = stage; bad.instruction_groups = 0;
    unchanged(s, emit, A840_EMIT_RANGE_ERROR);
    scenario("code VA alignment"); bad = stage; ++invalid.shader.code.gpu_va;
    unchanged(s, emit, A840_EMIT_ALIGNMENT);
    scenario("code VA span exceeds 48 bits"); invalid.shader.code.gpu_va = (UINT64_C(1) << 48) - 128;
    unchanged(s, emit, A840_EMIT_RANGE_ERROR);
    scenario("missing private memory allocation"); bad = stage; bad.private_mem_bytes = 512;
    invalid = compute_pipeline(bad); invalid.shader.private_memory = {};
    unchanged(s, emit, A840_EMIT_INVALID_ARG);
    scenario("aliased stage metadata"); invalid = compute_pipeline(stage);
    invalid.shader.stage = reinterpret_cast<const A840CompiledStage *>(s.words.data());
    unchanged(s, emit, A840_EMIT_ALIAS);
    scenario("invalid sysval register"); invalid = desc; invalid.local_id_regid = 253;
    unchanged(s, emit, A840_EMIT_RANGE_ERROR);
}

static void test_bindless_consts()
{
    auto table = bindless();
    scenario("eight A8XX bindless slots");
    Stream s;
    CHECK(a840_emit_compute_bindless(&s.cs, &table) == A840_EMIT_OK);
    Packets p(s);
    for (unsigned i = 0; i < table.count; ++i) {
        CHECK(p.reg(A840_REG_SP_CS_BINDLESS_BASE_0_DESCRIPTOR + i * 2u) ==
              (static_cast<uint32_t>(table.tables[i].gpu_va) | 3u));
        reloc_is(s, i, table.tables[i], A840_RELOC_READ, 3u);
    }
    CHECK(p.reg(A840_REG_SP_CS_BINDLESS_BASE_7_DESCRIPTOR) ==
          (static_cast<uint32_t>(table.tables[7].gpu_va) | 3u));
    exact_capacity("bindless capacity", A840_EMIT_COMPUTE_BINDLESS_MAX_DWORDS, 8,
        [&](auto *out) { return a840_emit_compute_bindless(out, &table); });
    scenario("clear sparse bindless entry");
    table.tables[1] = {};
    Stream sparse;
    CHECK(a840_emit_compute_bindless(&sparse.cs, &table) == A840_EMIT_OK);
    CHECK(Packets(sparse).reg(A840_REG_SP_CS_BINDLESS_BASE_1_DESCRIPTOR) == 0);
    CHECK(sparse.cs.reloc_count == 7);

    auto tables = [&](auto *out) { return a840_emit_compute_bindless(out, &table); };
    scenario("too many bindless bases"); table.count = 9;
    unchanged(s, tables, A840_EMIT_RANGE_ERROR);
    scenario("unaligned final bindless base"); table = bindless(); table.tables[7].gpu_va += 4;
    unchanged(s, tables, A840_EMIT_ALIGNMENT);
    scenario("empty bindless update"); table.count = 0;
    unchanged(s, tables, A840_EMIT_OK);

    std::array<uint32_t, 4u * A840_COMPUTE_MAX_CONST_VEC4> data{};
    data[0] = 17; data.back() = 89;
    A840ConstUploadDesc upload{};
    upload.source = A840_CONST_DIRECT;
    upload.count_vec4 = upload.constlen_vec4 = A840_COMPUTE_MAX_CONST_VEC4;
    upload.words = data.data();
    auto upload_cs = [&](auto *out) { return a840_emit_compute_consts(out, &upload); };
    exact_capacity("full direct const RAM", A840_EMIT_COMPUTE_CONSTS_MAX_DWORDS, 0, upload_cs);
    scenario("direct constant payload");
    Stream direct;
    CHECK(upload_cs(&direct.cs) == A840_EMIT_OK);
    Packets dp(direct);
    const auto &direct_packet = dp.opcode(A840_CP_LOAD_STATE6_FRAG);
    CHECK(dp.payload(direct_packet, 0) == (512u << 22 | 13u << 18 | 1u << 14));
    CHECK(dp.payload(direct_packet, 1) == 0 && dp.payload(direct_packet, 2) == 0);
    CHECK(dp.payload(direct_packet, 3) == 17);
    CHECK(dp.payload(direct_packet, direct_packet.payload_words - 1) == 89);

    upload.source = A840_CONST_INDIRECT;
    upload.dst_vec4 = 508; upload.count_vec4 = 4;
    upload.address = {72, UINT64_C(0x10000800200)};
    exact_capacity("indirect const relocation capacity", 4, 1, upload_cs);
    scenario("indirect constant payload");
    Stream indirect;
    CHECK(upload_cs(&indirect.cs) == A840_EMIT_OK);
    Packets ip(indirect);
    CHECK(ip.payload(ip.opcode(A840_CP_LOAD_STATE6_FRAG), 0) ==
          (4u << 22 | 13u << 18 | 2u << 16 | 1u << 14 | 508u));
    reloc_is(indirect, 0, upload.address, A840_RELOC_READ);
    scenario("const upload overruns window"); ++upload.count_vec4;
    unchanged(s, upload_cs, A840_EMIT_RANGE_ERROR);
    scenario("const upload VA alignment"); --upload.count_vec4; upload.address.gpu_va += 4;
    unchanged(s, upload_cs, A840_EMIT_ALIGNMENT);
    scenario("const upload missing data"); upload.source = A840_CONST_DIRECT; upload.words = nullptr;
    unchanged(s, upload_cs, A840_EMIT_INVALID_ARG);
    scenario("const upload alias"); upload.words = s.words.data();
    unchanged(s, upload_cs, A840_EMIT_ALIAS);
}

static void test_dispatch()
{
    auto stage = shader_stage(A840_STAGE_COMPUTE, 127);
    auto desc = dispatch(stage, A840_DISPATCH_UNIFORM);
    auto emit = [&](auto *out) { return a840_emit_compute_dispatch(out, &desc); };
    scenario("uniform at instrlen 127");
    Stream uniform;
    CHECK(emit(&uniform.cs) == A840_EMIT_OK);
    Packets up(uniform);
    CHECK(up.opcode_count(A840_CP_EVENT_WRITE7) == 0);
    CHECK(up.reg(A840_REG_SP_CS_NDRANGE_0) == (3u | 7u << 2 | 3u << 12 | 1u << 22));
    CHECK(up.reg(A840_REG_SP_CS_NDRANGE_7) == (7u << 2 | 3u << 12 | 1u << 22));
    CHECK(up.reg(A840_REG_SP_CS_NDRANGE_1) == 24 && up.reg(A840_REG_SP_CS_NDRANGE_3) == 8);
    CHECK(up.reg(A840_REG_SP_CS_NDRANGE_5) == 2);
    CHECK(up.payload(up.opcode(A840_CP_EXEC_CS), 1) == 3);

    scenario("non-uniform exact totals and last group size");
    desc = dispatch(stage, A840_DISPATCH_NON_UNIFORM);
    Stream non_uniform;
    CHECK(emit(&non_uniform.cs) == A840_EMIT_OK);
    Packets np(non_uniform);
    CHECK(np.reg(A840_REG_SP_CS_NDRANGE_7) == (2u << 12));
    CHECK(np.reg(A840_REG_SP_CS_NDRANGE_1) == 17);
    CHECK(np.reg(A840_REG_SP_CS_NDRANGE_3) == 7);
    CHECK(np.reg(A840_REG_SP_CS_NDRANGE_5) == 3);
    const auto &exec = np.opcode(A840_CP_EXEC_CS);
    CHECK(np.payload(exec, 1) == 3 && np.payload(exec, 2) == 2 && np.payload(exec, 3) == 2);
    exact_capacity("direct dispatch capacity", non_uniform.cs.used_dwords, 0, emit);

    scenario("indirect Gen8 wait and instrlen 128 context roll");
    stage = shader_stage(A840_STAGE_COMPUTE, 128);
    desc = dispatch(stage, A840_DISPATCH_INDIRECT);
    Stream indirect;
    CHECK(emit(&indirect.cs) == A840_EMIT_OK);
    Packets ip(indirect);
    CHECK(ip.reg(A840_REG_SP_PS_INSTR_SIZE) == 128);
    const auto &launch = ip.opcode(A840_CP_EXEC_CS_INDIRECT);
    const auto &wait = ip.opcode(A840_CP_WAIT_FOR_ME);
    CHECK(wait.payload_words == 0 && wait.header_word + 1 == launch.header_word);
    CHECK(ip.opcode_count(A840_CP_EVENT_WRITE7) == 2);
    CHECK(ip.payload(ip.opcode(A840_CP_EVENT_WRITE7), 0) == 63);
    CHECK(ip.payload(ip.opcode(A840_CP_EVENT_WRITE7, 1), 0) == 63);
    CHECK(ip.opcode(A840_CP_EVENT_WRITE7).header_word < wait.header_word);
    CHECK(ip.opcode(A840_CP_EVENT_WRITE7, 1).header_word > launch.header_word);
    CHECK(ip.payload(launch, 3) == (7u << 2 | 3u << 12 | 1u << 22));
    reloc_is(indirect, 0, desc.indirect, A840_RELOC_READ);
    CHECK(indirect.relocs[0].dword_offset == launch.header_word + 2);
    exact_capacity("maximum indirect dispatch", A840_EMIT_COMPUTE_DISPATCH_MAX_DWORDS, 1, emit);

    Stream s(true);
    scenario("indirect argument VA exceeds 48-bit span"); desc.indirect.gpu_va = (UINT64_C(1) << 48) - 4;
    unchanged(s, emit, A840_EMIT_RANGE_ERROR);
    scenario("indirect argument VA alignment"); desc = dispatch(stage, A840_DISPATCH_INDIRECT);
    desc.indirect.gpu_va += 2;
    unchanged(s, emit, A840_EMIT_ALIGNMENT);
    scenario("uniform total overflow"); desc = dispatch(stage, A840_DISPATCH_UNIFORM);
    desc.size[1] = UINT32_MAX;
    unchanged(s, emit, A840_EMIT_RANGE_ERROR);
    scenario("empty grid"); desc.size[0] = 0;
    unchanged(s, emit, A840_EMIT_OK);
    scenario("dispatch stage required"); desc.stage = nullptr;
    unchanged(s, emit, A840_EMIT_INVALID_ARG);
}

static A840GraphicsProgramDesc graphics_program(const A840CompiledStage &vs, const A840CompiledStage &fs)
{
    A840GraphicsProgramDesc desc{};
    desc.vs = gpu_shader(vs, 2);
    desc.fs = gpu_shader(fs, 3);
    desc.vertex_id = 4;
    desc.instance_id = 8;
    desc.primitive_id = desc.view_id = A840_REGID_UNUSED;
    desc.position = 0;
    desc.point_size = A840_REGID_UNUSED;
    desc.vertex_input_count = 3;
    desc.vertex_inputs[0] = {0, 15};
    desc.vertex_inputs[1] = {4, 3};
    desc.vertex_inputs[2] = {A840_REGID_UNUSED, 0};
    desc.varying_count = 2;
    desc.varyings[0] = {4, 11, true};
    desc.varyings[1] = {8, 3, false};
    const uint8_t unused = A840_REGID_UNUSED;
    desc.fs_inputs = {unused, unused, unused, unused,
        0, unused, unused, unused, unused, unused, 8, unused, 15};
    desc.depth_output = desc.stencil_output = desc.sample_mask_output = unused;
    desc.colors[0] = {0, 15, true};
    desc.colors[7] = {12, 3, false};
    desc.pixel_lod = desc.full_quad = true;
    return desc;
}

static void test_program()
{
    const auto vs = shader_stage(A840_STAGE_VERTEX);
    const auto fs = shader_stage(A840_STAGE_FRAGMENT);
    auto desc = graphics_program(vs, fs);
    auto emit = [&](auto *out) { return a840_emit_3d_program(out, &desc); };
    scenario("Gen8 shader bindings and VPC");
    Stream s;
    CHECK(emit(&s.cs) == A840_EMIT_OK);
    Packets p(s);
    CHECK(A840_REG_RB_PS_CNTL == 0x881f);
    CHECK(A840_REG_PC_VS_INPUT_CNTL == 0x9b0c);
    CHECK(A840_REG_RB_LB_PARAM_LIMIT == 0x8866);
    CHECK(p.reg(A840_REG_RB_PS_CNTL) == 3);
    CHECK(p.reg(A840_REG_PC_VS_INPUT_CNTL) == (3u | 2u << 6));
    CHECK(p.reg(A840_REG_RB_LB_PARAM_LIMIT) == 7);
    CHECK(p.reg(A840_REG_SP_LB_PARAM_LIMIT) == 7);
    CHECK(p.reg(A840_REG_VFD_CNTL_0) == (3u | 3u << 8));
    CHECK(p.reg(A840_REG_VFD_CNTL_1) == 0xfcfc0804);
    CHECK(p.reg(A840_REG_VFD_DEST_CNTL_1_INSTR) == (4u << 4 | 3u));
    CHECK(p.reg(A840_REG_VPC_VS_CNTL) == (9u | 5u << 8 | 255u << 16));
    CHECK(p.reg(A840_REG_VPC_PS_CNTL) == (5u | 255u << 8 | 1u << 16 | 255u << 24));
    CHECK(p.reg(A840_REG_VPC_VARYING_INTERP_MODE_0_MODE) == 21u);
    CHECK(p.reg(A840_REG_VPC_VARYING_LM_TRANSFER_CNTL_0_DISABLE) == ~31u);
    CHECK(p.reg(A840_REG_SP_VS_OUTPUT_CNTL) == 3);
    CHECK(p.reg(A840_REG_SP_VS_VPC_DEST_0_REG) == (5u << 16 | 3u << 8));
    CHECK(p.reg(A840_REG_SP_REG_PROG_ID_0) == 0xfcfcfcfc);
    CHECK(p.reg(A840_REG_SP_REG_PROG_ID_1) == 0xfcfcfc00);
    CHECK(p.reg(A840_REG_SP_REG_PROG_ID_2) == 0x0a08fcfc);
    CHECK(p.reg(A840_REG_SP_REG_PROG_ID_3) == 0xfcfc);
    CHECK(p.reg(A840_REG_RB_PS_OUTPUT_CNTL) == 0);
    CHECK(p.reg(A840_REG_RB_PS_MRT_CNTL) == 8);
    CHECK(p.reg(A840_REG_SP_PS_OUTPUT_0_REG) == 256);
    CHECK(p.reg(A840_REG_SP_PS_OUTPUT_MASK) == 0x3000000f);
    reloc_is(s, 0, desc.vs.code, A840_RELOC_SHADER | A840_RELOC_READ);
    reloc_is(s, 2, desc.fs.code, A840_RELOC_SHADER | A840_RELOC_READ);

    scenario("Gen8 pixel LOD without full quad");
    desc.full_quad = false;
    desc.depth_output = 12;
    Stream lod;
    CHECK(emit(&lod.cs) == A840_EMIT_OK);
    CHECK(Packets(lod).reg(A840_REG_RB_PS_CNTL) == 1);
    CHECK(Packets(lod).reg(A840_REG_RB_PS_OUTPUT_CNTL) == 2);

    scenario("Gen8 full quad without pixel LOD");
    desc.full_quad = true;
    desc.pixel_lod = false;
    Stream quad;
    CHECK(emit(&quad.cs) == A840_EMIT_OK);
    CHECK(Packets(quad).reg(A840_REG_RB_PS_CNTL) == 2);
    CHECK(Packets(quad).reg(A840_REG_SP_PS_OUTPUT_CNTL) == (252u << 24 | 252u << 16 | 12u << 8));

    desc.vertex_input_count = A840_MAX_VERTEX_INPUTS;
    for (auto &input : desc.vertex_inputs)
        input = {4, 15};
    desc.varying_count = 31;
    for (auto &varying : desc.varyings)
        varying = {8, 15, false};
    exact_capacity("maximum VS input and VPC link", A840_EMIT_3D_PROGRAM_MAX_DWORDS, 4, emit);

    Stream unchanged_program(true);
    scenario("VPC would exceed 32 links after position"); desc.varying_count = 32;
    unchanged(unchanged_program, emit, A840_EMIT_RANGE_ERROR);
    scenario("too many vertex fetch instructions"); desc = graphics_program(vs, fs); desc.vertex_input_count = 33;
    unchanged(unchanged_program, emit, A840_EMIT_RANGE_ERROR);
    scenario("missing vertex position"); desc = graphics_program(vs, fs); desc.position = A840_REGID_UNUSED;
    unchanged(unchanged_program, emit, A840_EMIT_RANGE_ERROR);
    scenario("bad FS coordinate allocation"); desc = graphics_program(vs, fs); desc.fs_inputs.frag_coord = 250;
    unchanged(unchanged_program, emit, A840_EMIT_RANGE_ERROR);
    scenario("fragment stage required"); desc = graphics_program(vs, fs); desc.fs.stage = nullptr;
    unchanged(unchanged_program, emit, A840_EMIT_INVALID_ARG);
}

static A840AttachmentStorage attachment(uint64_t ordinal, uint32_t tiling)
{
    A840AttachmentStorage storage{};
    storage.data = {ordinal, UINT64_C(0x12000000000) + ordinal * 0x20000};
    storage.pitch_bytes = 512;
    storage.layer_stride_bytes = 65536;
    storage.tile_mode = tiling;
    if (tiling == A840_TILE_UBWC) {
        storage.flag_buffer = {ordinal + 100, storage.data.gpu_va + 0x10000};
        storage.flag_pitch_bytes = 64;
        storage.flag_layer_stride_bytes = 4096;
    }
    return storage;
}

static A840RenderStateDesc render_state()
{
    A840RenderStateDesc desc{};
    desc.viewport_count = desc.scissor_count = A840_MAX_VIEWPORTS;
    for (uint32_t i = 0; i < A840_MAX_VIEWPORTS; ++i) {
        desc.viewports[i] = {static_cast<float>(i), 64.0f, 128.0f, -64.0f, 0.25f, 0.75f};
        desc.scissors[i] = {i, 2, 96, 40};
    }
    desc.line_width = 1;
    desc.cull_back = desc.front_clockwise = true;
    desc.depth_test = desc.depth_write = desc.depth_clamp = desc.stencil_test = true;
    desc.depth_compare = 1;
    desc.front = {3, 1, 2, 3, 31, 0x3f, 0x7f};
    desc.back = {7, 2, 3, 4, 63, 0x7f, 0x3f};
    desc.sample_mask = UINT16_MAX;
    desc.blend_color[0] = 0.5f;
    desc.blend_color[1] = 0.25f;
    desc.blend_color[2] = 0.125f;
    desc.blend_color[3] = 1.0f;
    desc.depth_format = A840_DEPTH_32;
    desc.depth = attachment(21, A840_TILE_UBWC);
    desc.stencil = attachment(22, A840_TILE_TILED);
    for (uint32_t i = 0; i < A840_MAX_COLOR_TARGETS; ++i) {
        auto &target = desc.colors[i];
        target.storage = attachment(30u + i, A840_TILE_UBWC);
        target.format = A840_FMT6_8_8_8_8_UNORM;
        target.swap = A840_WZYX;
        target.srgb = i == 7;
        target.blend = {true, 15, A840_FACTOR_SRC_ALPHA, A840_FACTOR_ONE_MINUS_SRC_ALPHA,
                        A840_BLEND_ADD, A840_FACTOR_ONE, A840_FACTOR_ZERO, A840_BLEND_MAX};
    }
    return desc;
}

static uint32_t bits(float value)
{
    uint32_t raw;
    std::memcpy(&raw, &value, sizeof(raw));
    return raw;
}

static void test_render_state()
{
    auto desc = render_state();
    auto emit = [&](auto *out) { return a840_emit_3d_render_state(out, &desc); };
    scenario("sixteen viewports and scissors, all eight UBWC MRTs");
    Stream s;
    CHECK(emit(&s.cs) == A840_EMIT_OK);
    const Packets p(s);
    CHECK(p.reg(A840_REG_GRAS_CL_VIEWPORT_0_XOFFSET) == bits(64));
    CHECK(p.reg(A840_REG_GRAS_CL_VIEWPORT_15_XOFFSET) == bits(79));
    CHECK(p.reg(A840_REG_GRAS_CL_VIEWPORT_15_YSCALE) == bits(-32));
    CHECK(p.reg(A840_REG_GRAS_CL_VIEWPORT_15_ZOFFSET) == bits(0.25f));
    CHECK(p.reg(A840_REG_RB_VIEWPORT_ZCLAMP_MAX_15_REG) == bits(0.75f));
    CHECK(p.reg(A840_REG_GRAS_SC_SCREEN_SCISSOR_15_TL) == (15u | 2u << 16));
    CHECK(p.reg(A840_REG_GRAS_SC_SCREEN_SCISSOR_15_BR) == (110u | 41u << 16));
    CHECK(p.reg(A840_REG_GRAS_SU_CNTL) == 22);
    CHECK(p.reg(A840_REG_RB_DEPTH_CNTL) == 359);
    CHECK(p.reg(A840_REG_RB_STENCIL_CNTL) ==
          (7u | 3u << 8 | 1u << 11 | 2u << 14 | 3u << 17 |
           7u << 20 | 2u << 23 | 3u << 26 | 4u << 29));
    CHECK(p.reg(A840_REG_RB_STENCIL_REF_CNTL) == (31u | 63u << 8));
    CHECK(p.reg(A840_REG_RB_STENCIL_MASK) == (0x3fu | 0x7fu << 8));
    CHECK(p.reg(A840_REG_RB_DEPTH_BUFFER_INFO) == (4u | 3u << 5 | 1u << 7));
    CHECK(p.reg(A840_REG_RB_STENCIL_BUFFER_INFO) == (1u | 3u << 2));
    CHECK(p.reg(A840_REG_RB_DEPTH_FLAG_BUFFER_PITCH) == (1u | 8u << 11));
    CHECK(p.reg(A840_REG_RB_MRT_7_BUF_INFO) == (0x30u | 3u << 8 | 1u << 11));
    CHECK(p.reg(A840_REG_RB_MRT_7_PITCH) == 8);
    CHECK(p.reg(A840_REG_RB_MRT_7_ARRAY_PITCH) == 1024);
    CHECK(p.reg(A840_REG_RB_COLOR_FLAG_BUFFER_7_PITCH) == (1u | 8u << 11));
    CHECK(p.reg(A840_REG_RB_MRT_0_CONTROL) == (3u | 15u << 7));
    CHECK(p.reg(A840_REG_RB_MRT_7_BLEND_CONTROL) == (6u | 7u << 8 | 1u << 16 | 4u << 21));
    CHECK(p.reg(A840_REG_SP_MRT_BLEND_CNTL_7_REG) == (3u | 15u << 7));
    CHECK(p.reg(A840_REG_RB_SRGB_CNTL) == 128u);
    CHECK(p.reg(A840_REG_RB_BLEND_CONSTANT_RED_FP32) == bits(0.5f));
    CHECK(p.reg(A840_REG_RB_PS_MRT_CNTL) == 8);
    CHECK(s.cs.reloc_count == 19);
    reloc_is(s, 0, desc.depth.data, A840_RELOC_READ | A840_RELOC_WRITE);
    reloc_is(s, 1, desc.depth.flag_buffer, A840_RELOC_READ | A840_RELOC_WRITE);
    reloc_is(s, 2, desc.stencil.data, A840_RELOC_READ | A840_RELOC_WRITE);
    for (unsigned i = 0; i < A840_MAX_COLOR_TARGETS; ++i) {
        reloc_is(s, 3u + 2u * i, desc.colors[i].storage.data, A840_RELOC_READ | A840_RELOC_WRITE);
        reloc_is(s, 4u + 2u * i, desc.colors[i].storage.flag_buffer, A840_RELOC_READ | A840_RELOC_WRITE);
    }
    exact_capacity("maximum render state capacity", A840_EMIT_3D_RENDER_STATE_MAX_DWORDS, 19, emit);

    scenario("empty scissor, unbound MRT and linear color target");
    desc = render_state();
    desc.scissors[0].width = 0;
    desc.colors[6] = {};
    desc.colors[7].storage = attachment(49, A840_TILE_LINEAR);
    Stream mixed;
    CHECK(emit(&mixed.cs) == A840_EMIT_OK);
    const Packets mp(mixed);
    CHECK(mp.reg(A840_REG_GRAS_SC_SCREEN_SCISSOR_0_TL) == 0x10001);
    CHECK(mp.reg(A840_REG_GRAS_SC_SCREEN_SCISSOR_0_BR) == 0);
    CHECK(mp.reg(A840_REG_RB_MRT_6_BASE) == 0);
    CHECK(mp.reg(A840_REG_RB_MRT_7_BUF_INFO) == A840_FMT6_8_8_8_8_UNORM);
    CHECK(mp.reg(A840_REG_RB_COLOR_FLAG_BUFFER_7_ADDR) == 0);
    CHECK(mixed.cs.reloc_count == 16);

    scenario("no bound depth and stencil");
    desc.depth_format = A840_DEPTH_NONE;
    desc.depth = {}; desc.stencil = {};
    desc.depth_test = desc.depth_write = desc.stencil_test = false;
    Stream color_only;
    CHECK(emit(&color_only.cs) == A840_EMIT_OK);
    CHECK(Packets(color_only).reg(A840_REG_RB_DEPTH_BUFFER_INFO) == 0);
    CHECK(Packets(color_only).reg(A840_REG_RB_STENCIL_BUFFER_INFO) == 0);

    Stream reject(true);
    scenario("viewport count beyond slot 15"); desc = render_state(); ++desc.viewport_count;
    unchanged(reject, emit, A840_EMIT_RANGE_ERROR);
    scenario("scissor count beyond slot 15"); desc = render_state(); ++desc.scissor_count;
    unchanged(reject, emit, A840_EMIT_RANGE_ERROR);
    scenario("non-finite viewport"); desc = render_state();
    desc.viewports[15].width = std::numeric_limits<float>::infinity();
    unchanged(reject, emit, A840_EMIT_RANGE_ERROR);
    scenario("scissor coordinate overflows"); desc = render_state(); desc.scissors[15].x = UINT32_MAX;
    unchanged(reject, emit, A840_EMIT_RANGE_ERROR);
    scenario("bad stencil compare"); desc = render_state(); desc.back.compare = 8;
    unchanged(reject, emit, A840_EMIT_RANGE_ERROR);
    scenario("depth target missing"); desc = render_state(); desc.depth = {};
    unchanged(reject, emit, A840_EMIT_INVALID_ARG);
    scenario("UBWC color flag buffer alignment"); desc = render_state(); ++desc.colors[7].storage.flag_buffer.gpu_va;
    unchanged(reject, emit, A840_EMIT_ALIGNMENT);
    scenario("UBWC stencil plane unsupported"); desc = render_state(); desc.stencil.tile_mode = A840_TILE_UBWC;
    unchanged(reject, emit, A840_EMIT_UNSUPPORTED);
    scenario("color pitch alignment"); desc = render_state(); ++desc.colors[7].storage.pitch_bytes;
    unchanged(reject, emit, A840_EMIT_ALIGNMENT);
    scenario("bad blend factor"); desc = render_state(); desc.colors[7].blend.dst_rgb = 31;
    unchanged(reject, emit, A840_EMIT_RANGE_ERROR);
    scenario("compressed color target is unsupported"); desc = render_state(); desc.colors[7].format = A840_FMT6_DXT1;
    unchanged(reject, emit, A840_EMIT_UNSUPPORTED);
}

static A840DrawDesc draw_desc(A840IndexType type)
{
    A840DrawDesc desc{};
    desc.primitive = A840_PRIM_TRIANGLES;
    desc.index_type = type;
    desc.count = 6;
    desc.instance_count = 2;
    desc.first_instance = 7;
    desc.first_vertex = 3;
    desc.base_vertex = -2;
    desc.first_index = 4;
    desc.index_capacity = 32;
    desc.index_buffer = {88, UINT64_C(0x11000008000)};
    return desc;
}

static void test_draw_sds()
{
    auto draw = draw_desc(A840_INDEX_NONE);
    auto emit = [&](auto *out) { return a840_emit_3d_draw(out, &draw); };
    scenario("non-indexed draw");
    Stream nonindexed;
    CHECK(emit(&nonindexed.cs) == A840_EMIT_OK);
    Packets p(nonindexed);
    for (size_t i = 0; i < p.count; ++i)
        CHECK(!(p.entries[i].type == 4 && p.entries[i].identifier <= A840_REG_PC_DRAWCALL_CNTL &&
                A840_REG_PC_DRAWCALL_CNTL < p.entries[i].identifier + p.entries[i].payload_words));
    CHECK(p.payload(p.opcode(A840_CP_DRAW_INDX_OFFSET), 0) == 132);
    CHECK(p.reg(A840_REG_VFD_INDEX_OFFSET) == 3);
    CHECK(p.reg(A840_REG_VFD_INSTANCE_START_OFFSET) == 7);
    CHECK(p.payload(p.opcode(A840_CP_DRAW_INDX_OFFSET), 2) == 6);
    CHECK(p.opcode(A840_CP_DRAW_INDX_OFFSET).payload_words == 3);
    CHECK(nonindexed.cs.reloc_count == 0);
    exact_capacity("non-indexed draw capacity", 7, 0, emit);

    for (auto kind : {A840_INDEX_UINT16, A840_INDEX_UINT32}) {
        scenario(kind == A840_INDEX_UINT16 ? "16-bit index draw" : "32-bit index draw");
        draw = draw_desc(kind);
        Stream s;
        CHECK(emit(&s.cs) == A840_EMIT_OK);
        Packets q(s);
        const auto &packet = q.opcode(A840_CP_DRAW_INDX_OFFSET);
        CHECK(packet.payload_words == 7);
        CHECK(q.payload(packet, 0) == (4u | (kind == A840_INDEX_UINT16 ? 1u : 2u) << 10));
        CHECK(q.reg(A840_REG_VFD_INDEX_OFFSET) == static_cast<uint32_t>(-2));
        CHECK(q.payload(packet, 1) == 2);
        CHECK(q.payload(packet, 3) == 4 && q.payload(packet, 6) == 32);
        reloc_is(s, 0, draw.index_buffer, A840_RELOC_READ);
        CHECK(s.relocs[0].dword_offset == packet.header_word + 5);
    }
    exact_capacity("maximum indexed draw", A840_EMIT_3D_DRAW_MAX_DWORDS, 1, emit);
    Stream reject(true);
    scenario("zero-instance draw emits nothing"); draw.instance_count = 0;
    unchanged(reject, emit, A840_EMIT_OK);
    scenario("zero-index draw emits nothing"); draw = draw_desc(A840_INDEX_UINT16); draw.count = 0;
    unchanged(reject, emit, A840_EMIT_OK);
    scenario("index count exceeds capacity"); draw = draw_desc(A840_INDEX_UINT32); draw.first_index = 29;
    unchanged(reject, emit, A840_EMIT_RANGE_ERROR);
    scenario("unaligned index buffer"); draw = draw_desc(A840_INDEX_UINT32); draw.index_buffer.gpu_va += 2;
    unchanged(reject, emit, A840_EMIT_ALIGNMENT);
    scenario("instance offset overflow"); draw = draw_desc(A840_INDEX_NONE); draw.first_instance = UINT32_MAX;
    unchanged(reject, emit, A840_EMIT_RANGE_ERROR);

    std::array<A840SdsGroupDesc, ADRENO840_MAX_SDS_SLOTS> groups{};
    for (uint32_t i = 0; i < groups.size(); ++i)
        groups[i] = {i, A840_SDS_SYSMEM | A840_SDS_GMEM, 8, {i + 1, UINT64_C(0x11000100000) + i * 256}};
    auto sds = [&](auto *out) { return a840_emit_sds_group(out, groups.data(), groups.size()); };
    exact_capacity("64-slot SDS capacity", A840_EMIT_SDS_GROUP_MAX_DWORDS, groups.size(), sds);
    scenario("64-slot SDS packet and read relocation");
    Stream sds_stream;
    CHECK(sds(&sds_stream.cs) == A840_EMIT_OK);
    Packets sp(sds_stream);
    CHECK(sp.opcode_count(A840_CP_SET_DRAW_STATE) == 64);
    const auto &last = sp.opcode(A840_CP_SET_DRAW_STATE, 63);
    CHECK(sp.payload(last, 0) == (63u << 24 | 1u << 22 | 1u << 21 | 8u));
    reloc_is(sds_stream, 63, groups[63].state, A840_RELOC_SDS | A840_RELOC_READ);

    scenario("disable SDS group");
    A840SdsGroupDesc disabled{7, 0, 0, {}};
    Stream disable;
    CHECK(a840_emit_sds_group(&disable.cs, &disabled, 1) == A840_EMIT_OK);
    Packets dp(disable);
    CHECK(dp.payload(dp.opcode(A840_CP_SET_DRAW_STATE), 0) == (7u << 24 | 1u << 17));
    CHECK(disable.cs.reloc_count == 0);

    scenario("SDS count beyond 64");
    unchanged(reject, [&](auto *out) { return a840_emit_sds_group(out, groups.data(), 65); },
              A840_EMIT_RANGE_ERROR);
    scenario("SDS group id beyond 63"); groups[63].group_id = 64;
    unchanged(reject, sds, A840_EMIT_RANGE_ERROR);
    scenario("SDS enable mask reserved bit"); groups[63].group_id = 63; groups[63].enable_mask = 1;
    unchanged(reject, sds, A840_EMIT_RANGE_ERROR);
    scenario("SDS address alignment"); groups[63].enable_mask = A840_SDS_SYSMEM;
    ++groups[63].state.gpu_va;
    unchanged(reject, sds, A840_EMIT_ALIGNMENT);
    scenario("SDS area crosses 48-bit VA"); groups[63].state.gpu_va = (UINT64_C(1) << 48) - 4;
    unchanged(reject, sds, A840_EMIT_RANGE_ERROR);
}

static void dump(const Stream &s, const char *path, const char *stage)
{
    auto *file = std::fopen(path, "wb");
    CHECK(file != nullptr);
    for (size_t i = 0; i < s.cs.used_dwords; ++i) {
        const uint32_t word = s.words[i];
        const uint8_t bytes[] = {static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8),
            static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24)};
        CHECK(std::fwrite(bytes, sizeof(bytes), 1, file) == 1);
    }
    CHECK(std::fclose(file) == 0);
    std::printf("%s stream: %zu dwords, %zu packets, %zu relocations\n",
                stage, s.cs.used_dwords, Packets(s).count, s.cs.reloc_count);
}

static void compute_flow(const char *path)
{
    scenario("complete Compute PM4 flow");
    auto stage = shader_stage(A840_STAGE_COMPUTE, 128);
    stage.shared_mem_bytes = 32768;
    auto pipeline = compute_pipeline(stage);
    const auto table = bindless();
    const uint32_t immediate[] = {5, 7, 11, 13};
    A840ConstUploadDesc constants{A840_CONST_DIRECT, 4, 1, 128, immediate, {}};
    Stream s;
    CHECK(a840_emit_compute_pipeline(&s.cs, &pipeline) == A840_EMIT_OK);
    CHECK(a840_emit_compute_bindless(&s.cs, &table) == A840_EMIT_OK);
    CHECK(a840_emit_compute_consts(&s.cs, &constants) == A840_EMIT_OK);
    constants.source = A840_CONST_INDIRECT;
    constants.dst_vec4 = 6;
    constants.address = {19, 0x400000};
    CHECK(a840_emit_compute_consts(&s.cs, &constants) == A840_EMIT_OK);
    for (auto mode : {A840_DISPATCH_UNIFORM, A840_DISPATCH_NON_UNIFORM, A840_DISPATCH_INDIRECT}) {
        const auto launch = dispatch(stage, mode);
        CHECK(a840_emit_compute_dispatch(&s.cs, &launch) == A840_EMIT_OK);
    }
    if (path) dump(s, path, "Compute");
}

static void graphics_flow(const char *path)
{
    scenario("complete VS/FS Render + Draw PM4 flow");
    const auto vs = shader_stage(A840_STAGE_VERTEX);
    const auto fs = shader_stage(A840_STAGE_FRAGMENT);
    const auto program = graphics_program(vs, fs);
    const auto state = render_state();
    const A840SdsGroupDesc groups[] = {{7, A840_SDS_SYSMEM, 18, {49, 0x200000}},
        {63, A840_SDS_GMEM, 12, {50, 0x204000}}};
    Stream s;
    CHECK(a840_emit_3d_program(&s.cs, &program) == A840_EMIT_OK);
    CHECK(a840_emit_3d_render_state(&s.cs, &state) == A840_EMIT_OK);
    CHECK(a840_emit_sds_group(&s.cs, groups, std::size(groups)) == A840_EMIT_OK);
    for (auto type : {A840_INDEX_NONE, A840_INDEX_UINT16, A840_INDEX_UINT32}) {
        const auto draw = draw_desc(type);
        CHECK(a840_emit_3d_draw(&s.cs, &draw) == A840_EMIT_OK);
    }
    if (path) dump(s, path, "3D");
}

int main(int argc, char **argv)
{
    const char *compute_path = nullptr, *graphics_path = nullptr;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 < argc && std::strcmp(argv[i], "--dump-compute-pm4") == 0 && !compute_path)
            compute_path = argv[i + 1];
        else if (i + 1 < argc && std::strcmp(argv[i], "--dump-3d-pm4") == 0 && !graphics_path)
            graphics_path = argv[i + 1];
        else {
            std::fprintf(stderr, "usage: %s [--dump-compute-pm4 path] [--dump-3d-pm4 path]\n", argv[0]);
            return 2;
        }
    }
    test_pipeline();
    test_bindless_consts();
    test_dispatch();
    test_program();
    test_render_state();
    test_draw_sds();
    compute_flow(compute_path);
    graphics_flow(graphics_path);
    std::printf("W02-L3 Compute and 3D: %u independent cases, %u checks passed\n", cases, checks);
}
