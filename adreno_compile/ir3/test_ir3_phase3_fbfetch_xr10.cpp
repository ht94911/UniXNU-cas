/* SPDX-License-Identifier: MIT */
#include "AdrenoCompileABI.h"

#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../../kits/Adreno840Emit/gen/a840_bitfields.h"
#include "compiler/shader_enums.h"
extern "C" {
#include "common/disasm.h"
#include "ir3-isa.h"
}

namespace {

std::size_t g_cases = 0;
std::size_t g_checks = 0;
const char *g_active_case = "init";

void begin_case(const char *name)
{
    g_active_case = name;
    ++g_cases;
}

void check_cond(bool ok, const char *expr, int line, const char *detail = nullptr)
{
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL [%s] line %d: %s%s%s\n",
                     g_active_case, line, expr,
                     (detail && detail[0]) ? " :: " : "",
                     (detail && detail[0]) ? detail : "");
        std::exit(EXIT_FAILURE);
    }
}

#define CASE(name) begin_case(name)
#define CHECK(expr) check_cond((expr), #expr, __LINE__)
#define CHECK_MSG(expr, msg) check_cond((expr), #expr, __LINE__, (msg))

/* Minimal deterministic SPIR-V 1.3 binary builder (zero external assembler needed). */
struct SpvBuilder {
    std::vector<uint32_t> words;

    explicit SpvBuilder(uint32_t bound)
    {
        words.push_back(0x07230203u); /* Magic */
        words.push_back(0x00010300u); /* Version 1.3 */
        words.push_back(0u);          /* Generator */
        words.push_back(bound);       /* Bound */
        words.push_back(0u);          /* Schema */
    }

    void op(uint16_t opcode, const std::vector<uint32_t> &operands)
    {
        uint32_t wc = static_cast<uint32_t>(operands.size() + 1u);
        words.push_back((wc << 16) | static_cast<uint32_t>(opcode));
        for (uint32_t v : operands)
            words.push_back(v);
    }

    static std::vector<uint32_t> pack_str(const char *s)
    {
        std::size_t len = std::strlen(s) + 1u;
        std::size_t nw = (len + 3u) / 4u;
        std::vector<uint32_t> out(nw, 0u);
        for (std::size_t i = 0; i < len; ++i) {
            out[i / 4u] |= static_cast<uint32_t>(static_cast<unsigned char>(s[i])) << ((i % 4u) * 8u);
        }
        return out;
    }

    void op_entry_point(uint32_t exec_model, uint32_t fn_id, const char *name,
                        const std::vector<uint32_t> &ifaces)
    {
        std::vector<uint32_t> ops = {exec_model, fn_id};
        auto s = pack_str(name);
        ops.insert(ops.end(), s.begin(), s.end());
        ops.insert(ops.end(), ifaces.begin(), ifaces.end());
        op(15u /* OpEntryPoint */, ops);
    }
};

constexpr uint16_t SPV_OP_CAPABILITY = 17u;
constexpr uint16_t SPV_OP_MEMORY_MODEL = 14u;
constexpr uint16_t SPV_OP_EXECUTION_MODE = 16u;
constexpr uint16_t SPV_OP_DECORATE = 71u;
constexpr uint16_t SPV_OP_MEMBER_DECORATE = 72u;
constexpr uint16_t SPV_OP_TYPE_VOID = 19u;
constexpr uint16_t SPV_OP_TYPE_FUNCTION = 33u;
constexpr uint16_t SPV_OP_TYPE_INT = 21u;
constexpr uint16_t SPV_OP_TYPE_FLOAT = 22u;
constexpr uint16_t SPV_OP_TYPE_VECTOR = 23u;
constexpr uint16_t SPV_OP_TYPE_IMAGE = 25u;
constexpr uint16_t SPV_OP_TYPE_STRUCT = 30u;
constexpr uint16_t SPV_OP_TYPE_POINTER = 32u;
constexpr uint16_t SPV_OP_CONSTANT = 43u;
constexpr uint16_t SPV_OP_CONSTANT_COMPOSITE = 44u;
constexpr uint16_t SPV_OP_VARIABLE = 59u;
constexpr uint16_t SPV_OP_LOAD = 61u;
constexpr uint16_t SPV_OP_STORE = 62u;
constexpr uint16_t SPV_OP_ACCESS_CHAIN = 65u;
constexpr uint16_t SPV_OP_IMAGE_READ = 98u;
constexpr uint16_t SPV_OP_FADD = 129u;
constexpr uint16_t SPV_OP_FMUL = 133u;
constexpr uint16_t SPV_OP_FUNCTION = 54u;
constexpr uint16_t SPV_OP_LABEL = 248u;
constexpr uint16_t SPV_OP_RETURN = 253u;
constexpr uint16_t SPV_OP_FUNCTION_END = 56u;

constexpr uint32_t SPV_CAP_SHADER = 1u;
constexpr uint32_t SPV_CAP_INPUT_ATTACHMENT = 40u;
constexpr uint32_t SPV_CAP_SAMPLE_RATE_SHADING = 35u;
constexpr uint32_t SPV_EXEC_FRAGMENT = 4u;
constexpr uint32_t SPV_MODE_ORIGIN_UPPER_LEFT = 7u;

constexpr uint32_t SPV_SC_UNIFORM_CONSTANT = 0u;
constexpr uint32_t SPV_SC_INPUT = 1u;
constexpr uint32_t SPV_SC_OUTPUT = 3u;
constexpr uint32_t SPV_SC_PUSH_CONSTANT = 9u;

constexpr uint32_t SPV_DEC_BLOCK = 2u;
constexpr uint32_t SPV_DEC_BUILTIN = 11u;
constexpr uint32_t SPV_DEC_LOCATION = 30u;
constexpr uint32_t SPV_DEC_BINDING = 33u;
constexpr uint32_t SPV_DEC_DESC_SET = 34u;
constexpr uint32_t SPV_DEC_OFFSET = 35u;
constexpr uint32_t SPV_DEC_INPUT_ATT_IDX = 43u;
constexpr uint32_t SPV_BUILTIN_SAMPLE_ID = 18u;

std::vector<uint32_t> build_fbfetch_1x_spv()
{
    SpvBuilder b(40u);
    b.op(SPV_OP_CAPABILITY, {SPV_CAP_SHADER});
    b.op(SPV_OP_CAPABILITY, {SPV_CAP_INPUT_ATTACHMENT});
    b.op(SPV_OP_MEMORY_MODEL, {0u, 1u});
    b.op_entry_point(SPV_EXEC_FRAGMENT, 4u, "main", {11u});
    b.op(SPV_OP_EXECUTION_MODE, {4u, SPV_MODE_ORIGIN_UPPER_LEFT});

    b.op(SPV_OP_DECORATE, {11u, SPV_DEC_LOCATION, 0u});
    b.op(SPV_OP_DECORATE, {14u, SPV_DEC_DESC_SET, 0u});
    b.op(SPV_OP_DECORATE, {14u, SPV_DEC_BINDING, 0u});
    b.op(SPV_OP_DECORATE, {14u, SPV_DEC_INPUT_ATT_IDX, 0u});
    b.op(SPV_OP_DECORATE, {15u, SPV_DEC_BLOCK});
    b.op(SPV_OP_MEMBER_DECORATE, {15u, 0u, SPV_DEC_OFFSET, 0u});

    b.op(SPV_OP_TYPE_VOID, {1u});
    b.op(SPV_OP_TYPE_FUNCTION, {2u, 1u});
    b.op(SPV_OP_TYPE_FLOAT, {6u, 32u});
    b.op(SPV_OP_TYPE_VECTOR, {7u, 6u, 4u});
    b.op(SPV_OP_TYPE_INT, {8u, 32u, 1u});
    b.op(SPV_OP_TYPE_VECTOR, {9u, 8u, 2u});
    b.op(SPV_OP_TYPE_POINTER, {10u, SPV_SC_OUTPUT, 7u});
    b.op(SPV_OP_VARIABLE, {10u, 11u, SPV_SC_OUTPUT});

    b.op(SPV_OP_TYPE_IMAGE, {12u, 6u, 6u, 0u, 0u, 0u, 2u, 0u});
    b.op(SPV_OP_TYPE_POINTER, {13u, SPV_SC_UNIFORM_CONSTANT, 12u});
    b.op(SPV_OP_VARIABLE, {13u, 14u, SPV_SC_UNIFORM_CONSTANT});

    b.op(SPV_OP_TYPE_STRUCT, {15u, 7u});
    b.op(SPV_OP_TYPE_POINTER, {16u, SPV_SC_PUSH_CONSTANT, 15u});
    b.op(SPV_OP_VARIABLE, {16u, 17u, SPV_SC_PUSH_CONSTANT});
    b.op(SPV_OP_TYPE_POINTER, {18u, SPV_SC_PUSH_CONSTANT, 7u});

    b.op(SPV_OP_CONSTANT, {8u, 19u, 0u});
    b.op(SPV_OP_CONSTANT_COMPOSITE, {9u, 20u, 19u, 19u});
    b.op(SPV_OP_CONSTANT, {6u, 21u, 0x3f000000u});
    b.op(SPV_OP_CONSTANT_COMPOSITE, {7u, 22u, 21u, 21u, 21u, 21u});

    b.op(SPV_OP_FUNCTION, {1u, 4u, 0u, 2u});
    b.op(SPV_OP_LABEL, {5u});
    b.op(SPV_OP_LOAD, {12u, 23u, 14u});
    b.op(SPV_OP_IMAGE_READ, {7u, 24u, 23u, 20u});
    b.op(SPV_OP_ACCESS_CHAIN, {18u, 25u, 17u, 19u});
    b.op(SPV_OP_LOAD, {7u, 26u, 25u});
    b.op(SPV_OP_FMUL, {7u, 27u, 24u, 22u});
    b.op(SPV_OP_FADD, {7u, 28u, 27u, 26u});
    b.op(SPV_OP_STORE, {11u, 28u});
    b.op(SPV_OP_RETURN, {});
    b.op(SPV_OP_FUNCTION_END, {});
    return b.words;
}

std::vector<uint32_t> build_fbfetch_msaa_spv()
{
    SpvBuilder b(40u);
    b.op(SPV_OP_CAPABILITY, {SPV_CAP_SHADER});
    b.op(SPV_OP_CAPABILITY, {SPV_CAP_SAMPLE_RATE_SHADING});
    b.op(SPV_OP_CAPABILITY, {SPV_CAP_INPUT_ATTACHMENT});
    b.op(SPV_OP_MEMORY_MODEL, {0u, 1u});
    b.op_entry_point(SPV_EXEC_FRAGMENT, 4u, "main", {11u, 21u});
    b.op(SPV_OP_EXECUTION_MODE, {4u, SPV_MODE_ORIGIN_UPPER_LEFT});

    b.op(SPV_OP_DECORATE, {11u, SPV_DEC_LOCATION, 0u});
    b.op(SPV_OP_DECORATE, {14u, SPV_DEC_DESC_SET, 0u});
    b.op(SPV_OP_DECORATE, {14u, SPV_DEC_BINDING, 0u});
    b.op(SPV_OP_DECORATE, {14u, SPV_DEC_INPUT_ATT_IDX, 0u});
    b.op(SPV_OP_DECORATE, {21u, SPV_DEC_BUILTIN, SPV_BUILTIN_SAMPLE_ID});

    b.op(SPV_OP_TYPE_VOID, {1u});
    b.op(SPV_OP_TYPE_FUNCTION, {2u, 1u});
    b.op(SPV_OP_TYPE_FLOAT, {6u, 32u});
    b.op(SPV_OP_TYPE_VECTOR, {7u, 6u, 4u});
    b.op(SPV_OP_TYPE_INT, {8u, 32u, 1u});
    b.op(SPV_OP_TYPE_VECTOR, {9u, 8u, 2u});
    b.op(SPV_OP_TYPE_POINTER, {10u, SPV_SC_OUTPUT, 7u});
    b.op(SPV_OP_VARIABLE, {10u, 11u, SPV_SC_OUTPUT});

    b.op(SPV_OP_TYPE_IMAGE, {12u, 6u, 6u, 0u, 0u, 1u, 2u, 0u});
    b.op(SPV_OP_TYPE_POINTER, {13u, SPV_SC_UNIFORM_CONSTANT, 12u});
    b.op(SPV_OP_VARIABLE, {13u, 14u, SPV_SC_UNIFORM_CONSTANT});

    b.op(SPV_OP_TYPE_POINTER, {20u, SPV_SC_INPUT, 8u});
    b.op(SPV_OP_VARIABLE, {20u, 21u, SPV_SC_INPUT});

    b.op(SPV_OP_CONSTANT, {8u, 19u, 0u});
    b.op(SPV_OP_CONSTANT_COMPOSITE, {9u, 22u, 19u, 19u});

    b.op(SPV_OP_FUNCTION, {1u, 4u, 0u, 2u});
    b.op(SPV_OP_LABEL, {5u});
    b.op(SPV_OP_LOAD, {12u, 23u, 14u});
    b.op(SPV_OP_LOAD, {8u, 24u, 21u});
    b.op(SPV_OP_IMAGE_READ, {7u, 25u, 23u, 22u, 0x40u, 24u});
    b.op(SPV_OP_STORE, {11u, 25u});
    b.op(SPV_OP_RETURN, {});
    b.op(SPV_OP_FUNCTION_END, {});
    return b.words;
}

std::vector<uint32_t> build_fbfetch_dual_spv()
{
    SpvBuilder b(40u);
    b.op(SPV_OP_CAPABILITY, {SPV_CAP_SHADER});
    b.op(SPV_OP_CAPABILITY, {SPV_CAP_INPUT_ATTACHMENT});
    b.op(SPV_OP_MEMORY_MODEL, {0u, 1u});
    b.op_entry_point(SPV_EXEC_FRAGMENT, 4u, "main", {11u});
    b.op(SPV_OP_EXECUTION_MODE, {4u, SPV_MODE_ORIGIN_UPPER_LEFT});

    b.op(SPV_OP_DECORATE, {11u, SPV_DEC_LOCATION, 0u});
    b.op(SPV_OP_DECORATE, {14u, SPV_DEC_DESC_SET, 0u});
    b.op(SPV_OP_DECORATE, {14u, SPV_DEC_BINDING, 0u});
    b.op(SPV_OP_DECORATE, {14u, SPV_DEC_INPUT_ATT_IDX, 0u});
    b.op(SPV_OP_DECORATE, {15u, SPV_DEC_DESC_SET, 0u});
    b.op(SPV_OP_DECORATE, {15u, SPV_DEC_BINDING, 1u});
    b.op(SPV_OP_DECORATE, {15u, SPV_DEC_INPUT_ATT_IDX, 1u});

    b.op(SPV_OP_TYPE_VOID, {1u});
    b.op(SPV_OP_TYPE_FUNCTION, {2u, 1u});
    b.op(SPV_OP_TYPE_FLOAT, {6u, 32u});
    b.op(SPV_OP_TYPE_VECTOR, {7u, 6u, 4u});
    b.op(SPV_OP_TYPE_INT, {8u, 32u, 1u});
    b.op(SPV_OP_TYPE_VECTOR, {9u, 8u, 2u});
    b.op(SPV_OP_TYPE_POINTER, {10u, SPV_SC_OUTPUT, 7u});
    b.op(SPV_OP_VARIABLE, {10u, 11u, SPV_SC_OUTPUT});

    b.op(SPV_OP_TYPE_IMAGE, {12u, 6u, 6u, 0u, 0u, 0u, 2u, 0u});
    b.op(SPV_OP_TYPE_POINTER, {13u, SPV_SC_UNIFORM_CONSTANT, 12u});
    b.op(SPV_OP_VARIABLE, {13u, 14u, SPV_SC_UNIFORM_CONSTANT});
    b.op(SPV_OP_VARIABLE, {13u, 15u, SPV_SC_UNIFORM_CONSTANT});

    b.op(SPV_OP_CONSTANT, {8u, 19u, 0u});
    b.op(SPV_OP_CONSTANT_COMPOSITE, {9u, 20u, 19u, 19u});

    b.op(SPV_OP_FUNCTION, {1u, 4u, 0u, 2u});
    b.op(SPV_OP_LABEL, {5u});
    b.op(SPV_OP_LOAD, {12u, 21u, 14u});
    b.op(SPV_OP_LOAD, {12u, 22u, 15u});
    b.op(SPV_OP_IMAGE_READ, {7u, 23u, 21u, 20u});
    b.op(SPV_OP_IMAGE_READ, {7u, 24u, 22u, 20u});
    b.op(SPV_OP_FADD, {7u, 25u, 23u, 24u});
    b.op(SPV_OP_STORE, {11u, 25u});
    b.op(SPV_OP_RETURN, {});
    b.op(SPV_OP_FUNCTION_END, {});
    return b.words;
}

std::vector<uint32_t> build_plain_fs_spv()
{
    SpvBuilder b(32u);
    b.op(SPV_OP_CAPABILITY, {SPV_CAP_SHADER});
    b.op(SPV_OP_MEMORY_MODEL, {0u, 1u});
    b.op_entry_point(SPV_EXEC_FRAGMENT, 4u, "main", {11u});
    b.op(SPV_OP_EXECUTION_MODE, {4u, SPV_MODE_ORIGIN_UPPER_LEFT});

    b.op(SPV_OP_DECORATE, {11u, SPV_DEC_LOCATION, 0u});
    b.op(SPV_OP_DECORATE, {15u, SPV_DEC_BLOCK});
    b.op(SPV_OP_MEMBER_DECORATE, {15u, 0u, SPV_DEC_OFFSET, 0u});

    b.op(SPV_OP_TYPE_VOID, {1u});
    b.op(SPV_OP_TYPE_FUNCTION, {2u, 1u});
    b.op(SPV_OP_TYPE_FLOAT, {6u, 32u});
    b.op(SPV_OP_TYPE_VECTOR, {7u, 6u, 4u});
    b.op(SPV_OP_TYPE_INT, {8u, 32u, 1u});
    b.op(SPV_OP_TYPE_POINTER, {10u, SPV_SC_OUTPUT, 7u});
    b.op(SPV_OP_VARIABLE, {10u, 11u, SPV_SC_OUTPUT});
    b.op(SPV_OP_TYPE_STRUCT, {15u, 7u});
    b.op(SPV_OP_TYPE_POINTER, {16u, SPV_SC_PUSH_CONSTANT, 15u});
    b.op(SPV_OP_VARIABLE, {16u, 17u, SPV_SC_PUSH_CONSTANT});
    b.op(SPV_OP_TYPE_POINTER, {18u, SPV_SC_PUSH_CONSTANT, 7u});
    b.op(SPV_OP_CONSTANT, {8u, 19u, 0u});

    b.op(SPV_OP_FUNCTION, {1u, 4u, 0u, 2u});
    b.op(SPV_OP_LABEL, {5u});
    b.op(SPV_OP_ACCESS_CHAIN, {18u, 20u, 17u, 19u});
    b.op(SPV_OP_LOAD, {7u, 21u, 20u});
    b.op(SPV_OP_STORE, {11u, 21u});
    b.op(SPV_OP_RETURN, {});
    b.op(SPV_OP_FUNCTION_END, {});
    return b.words;
}

std::string disasm_a840_and_verify(const A840CompiledStage &compiled, struct shader_stats *out_stats)
{
    char *stat_text = nullptr;
    std::size_t stat_len = 0;
    FILE *stat_fp = open_memstream(&stat_text, &stat_len);
    CHECK(stat_fp != nullptr);
    int rc = disasm_a3xx_stat(compiled.isa_words, static_cast<int>(compiled.isa_dwords),
                              0, stat_fp, 800, out_stats);
    std::fclose(stat_fp);
    std::free(stat_text);
    CHECK(rc == 0);

    char *asm_text = nullptr;
    std::size_t asm_len = 0;
    FILE *asm_fp = open_memstream(&asm_text, &asm_len);
    CHECK(asm_fp != nullptr);
    struct isa_decode_options opts{};
    opts.gpu_id = 800;
    opts.show_errors = true;
    opts.max_errors = 5;
    opts.branch_labels = true;
    ir3_isa_disasm(const_cast<uint32_t *>(compiled.isa_words),
                   static_cast<int>(compiled.isa_dwords * sizeof(uint32_t)),
                   asm_fp, &opts);
    std::fclose(asm_fp);
    CHECK(asm_text != nullptr && asm_len > 0);
    std::string text(asm_text, asm_len);
    std::free(asm_text);

    uint32_t total = 0;
    for (unsigned c = 0; c < 8u; ++c)
        total += out_stats->instrs_per_cat[c];
    CHECK(total > 0u);
    CHECK(text.find("end") != std::string::npos);
    CHECK(text.find("no match") == std::string::npos);
    CHECK(text.find("ERROR") == std::string::npos);
    return text;
}

A840CompileTarget kaanapali_target(uint32_t flags = 0u)
{
    A840CompileTarget t{};
    t.abi_version = ADRENO_COMPILE_ABI_VERSION;
    t.chip_id = UINT64_C(0x44050a31);
    t.num_slices = 3u;
    t.num_ccu = 6u;
    t.highest_bank_bit = 16u;
    t.ubwc_swizzle = 6u;
    t.macrotile_mode = 1u;
    t.flags = flags;
    return t;
}

A840TextureLayoutQuery base_query(uint32_t fmt, uint32_t w, uint32_t h,
                                  uint32_t tile_mode, uint32_t flags = 0u)
{
    A840TextureLayoutQuery q{};
    q.abi_version = ADRENO_COMPILE_ABI_VERSION;
    q.mtl_pixel_format = fmt;
    q.width = w;
    q.height = h;
    q.depth = 1u;
    q.array_layers = 1u;
    q.mip_levels = 1u;
    q.sample_count = 1u;
    q.tile_mode = tile_mode;
    q.flags = flags;
    return q;
}

void expect_layout_reject(const A840CompileTarget &target,
                          const A840TextureLayoutQuery &q,
                          A840CompileStatus expected)
{
    A840TextureLayoutResult sentinel{};
    std::memset(&sentinel, 0xa5, sizeof(sentinel));
    const A840TextureLayoutResult before = sentinel;
    A840CompileStatus st = a840_compute_texture_layout(&target, &q, &sentinel);
    CHECK(st == expected);
    CHECK(std::memcmp(&sentinel, &before, sizeof(sentinel)) == 0);
}

constexpr uint32_t OUT_FLAG_USES_FBFETCH = 1u << 2;
constexpr uint32_t PLANE_FLAG(uint32_t plane) { return plane << 8; }
constexpr uint32_t tex_wh(uint32_t w, uint32_t h)
{
    return (w << A840_TEX_2_WIDTH_SHIFT) | (h << A840_TEX_2_HEIGHT_SHIFT);
}

void run_fbfetch_tests()
{
    const auto spv_1x = build_fbfetch_1x_spv();
    const auto spv_msaa = build_fbfetch_msaa_spv();
    const auto spv_dual = build_fbfetch_dual_spv();
    const auto spv_plain = build_plain_fs_spv();

    auto compile_fs = [](const A840CompileTarget &t,
                         const std::vector<uint32_t> &spv,
                         uint32_t sample_count,
                         uint32_t stage_flags,
                         A840CompiledStage &out) {
        A840StageInput in{};
        in.abi_version = ADRENO_COMPILE_ABI_VERSION;
        in.stage = A840_STAGE_FRAGMENT;
        in.spirv_words = spv.data();
        in.spirv_bytes = spv.size() * sizeof(uint32_t);
        in.entry_point = "main";
        in.color_rt_mask = 1u;
        in.sample_count = sample_count;
        in.flags = stage_flags;
        char err[512] = {};
        A840CompileStatus st = a840_compile_stage(&t, &in, &out, err, sizeof(err));
        CHECK_MSG(st == A840_COMPILE_OK, err);
        CHECK(out.abi_version == ADRENO_COMPILE_ABI_VERSION);
        CHECK(out.stage == A840_STAGE_FRAGMENT);
        CHECK(out.isa_words != nullptr && out.isa_dwords > 0u && (out.isa_dwords % 2u) == 0u);
        CHECK(out.instruction_groups > 0u);
        CHECK(out.owner != nullptr);
    };

    CASE("fbfetch_1x_gmem_stage_flag");
    {
        A840CompileTarget t = kaanapali_target(0u);
        A840CompiledStage out{};
        compile_fs(t, spv_1x, 1u, A840_COMPILE_FLAG_FBFETCH_GMEM, out);
        CHECK((out.flags_out & OUT_FLAG_USES_FBFETCH) != 0u);
        struct shader_stats stats{};
        std::string asm_txt = disasm_a840_and_verify(out, &stats);
        CHECK(stats.instrs_per_cat[5] > 0u);
        CHECK(asm_txt.find("isam") != std::string::npos);
        a840_compile_stage_free(&out);
    }

    CASE("fbfetch_1x_gmem_target_flag");
    {
        A840CompileTarget t = kaanapali_target(A840_COMPILE_FLAG_FBFETCH_GMEM);
        A840CompiledStage out{};
        compile_fs(t, spv_1x, 1u, 0u, out);
        CHECK((out.flags_out & OUT_FLAG_USES_FBFETCH) != 0u);
        struct shader_stats stats{};
        (void)disasm_a840_and_verify(out, &stats);
        CHECK(stats.instrs_per_cat[5] > 0u);
        a840_compile_stage_free(&out);
    }

    CASE("fbfetch_1x_gmem_with_early_preamble");
    {
        A840CompileTarget t = kaanapali_target(0u);
        A840CompiledStage out{};
        compile_fs(t, spv_1x, 1u,
                   A840_COMPILE_FLAG_FBFETCH_GMEM | A840_COMPILE_FLAG_EARLY_PREAMBLE, out);
        CHECK((out.flags_out & OUT_FLAG_USES_FBFETCH) != 0u);
        struct shader_stats stats{};
        (void)disasm_a840_and_verify(out, &stats);
        CHECK(stats.instrs_per_cat[5] > 0u);
        a840_compile_stage_free(&out);
    }

    CASE("fbfetch_1x_gmem_with_half_round_rtz");
    {
        A840CompileTarget t = kaanapali_target(0u);
        A840CompiledStage out{};
        compile_fs(t, spv_1x, 1u,
                   A840_COMPILE_FLAG_FBFETCH_GMEM | A840_COMPILE_FLAG_HALF_ROUND_RTZ, out);
        CHECK((out.flags_out & OUT_FLAG_USES_FBFETCH) != 0u);
        struct shader_stats stats{};
        (void)disasm_a840_and_verify(out, &stats);
        a840_compile_stage_free(&out);
    }

    CASE("fbfetch_1x_sysmem_backward_compat_without_gmem_flag");
    {
        A840CompileTarget t = kaanapali_target(0u);
        A840CompiledStage out{};
        compile_fs(t, spv_1x, 1u, 0u, out);
        struct shader_stats stats{};
        (void)disasm_a840_and_verify(out, &stats);
        CHECK(stats.instrs_per_cat[5] > 0u);
        a840_compile_stage_free(&out);
    }

    CASE("fbfetch_msaa4x_gmem_isamm");
    {
        A840CompileTarget t = kaanapali_target(0u);
        A840CompiledStage out{};
        compile_fs(t, spv_msaa, 4u, A840_COMPILE_FLAG_FBFETCH_GMEM, out);
        CHECK((out.flags_out & OUT_FLAG_USES_FBFETCH) != 0u);
        struct shader_stats stats{};
        std::string asm_txt = disasm_a840_and_verify(out, &stats);
        CHECK(stats.instrs_per_cat[5] > 0u);
        CHECK(asm_txt.find("isamm") != std::string::npos);
        a840_compile_stage_free(&out);
    }

    CASE("fbfetch_dual_input_attachments_gmem");
    {
        A840CompileTarget t = kaanapali_target(0u);
        A840CompiledStage out{};
        compile_fs(t, spv_dual, 1u, A840_COMPILE_FLAG_FBFETCH_GMEM, out);
        CHECK((out.flags_out & OUT_FLAG_USES_FBFETCH) != 0u);
        struct shader_stats stats{};
        (void)disasm_a840_and_verify(out, &stats);
        CHECK(stats.instrs_per_cat[5] >= 2u);
        a840_compile_stage_free(&out);
    }

    CASE("plain_fs_without_input_attachment_has_zero_fbfetch_flag");
    {
        A840CompileTarget t = kaanapali_target(0u);
        A840CompiledStage out{};
        compile_fs(t, spv_plain, 1u, 0u, out);
        CHECK((out.flags_out & OUT_FLAG_USES_FBFETCH) == 0u);
        struct shader_stats stats{};
        (void)disasm_a840_and_verify(out, &stats);
        a840_compile_stage_free(&out);
    }

    CASE("plain_fs_with_gmem_flag_still_has_zero_fbfetch_flag");
    {
        A840CompileTarget t = kaanapali_target(A840_COMPILE_FLAG_FBFETCH_GMEM);
        A840CompiledStage out{};
        compile_fs(t, spv_plain, 1u, A840_COMPILE_FLAG_FBFETCH_GMEM, out);
        CHECK((out.flags_out & OUT_FLAG_USES_FBFETCH) == 0u);
        struct shader_stats stats{};
        (void)disasm_a840_and_verify(out, &stats);
        a840_compile_stage_free(&out);
    }

    CASE("fbfetch_deterministic_isa_output");
    {
        A840CompileTarget t = kaanapali_target(A840_COMPILE_FLAG_FBFETCH_GMEM);
        A840CompiledStage a{}, b{};
        compile_fs(t, spv_1x, 1u, A840_COMPILE_FLAG_EARLY_PREAMBLE, a);
        compile_fs(t, spv_1x, 1u, A840_COMPILE_FLAG_EARLY_PREAMBLE, b);
        CHECK(a.isa_dwords == b.isa_dwords);
        CHECK(a.flags_out == b.flags_out);
        CHECK(a.max_full_reg == b.max_full_reg);
        CHECK(a.max_half_reg == b.max_half_reg);
        CHECK(std::memcmp(a.isa_words, b.isa_words,
                          static_cast<std::size_t>(a.isa_dwords) * sizeof(uint32_t)) == 0);
        a840_compile_stage_free(&a);
        a840_compile_stage_free(&b);
    }
}

void run_xr10_and_ycbcr_fdl_tests()
{
    const A840CompileTarget t16 = kaanapali_target(0u);
    constexpr uint32_t FMT_BGRA10_XR = 552u;
    constexpr uint32_t FMT_BGRA10_XR_SRGB = 553u;
    constexpr uint32_t FMT_BGR10_XR = 554u;
    constexpr uint32_t FMT_BGR10_XR_SRGB = 555u;
    constexpr uint32_t FMT_NV12_VID = 500u;
    constexpr uint32_t FMT_NV12_FULL = 505u;
    constexpr uint32_t FMT_P010_VID = 508u;
    constexpr uint32_t FMT_P010_FULL = 509u;
    constexpr uint32_t CV_420V = 0x34323076u;
    constexpr uint32_t CV_420F = 0x34323066u;
    constexpr uint32_t CV_X420 = 0x78343230u;
    constexpr uint32_t CV_XF20 = 0x78663230u;

    A840TextureLayoutResult bgr10_lin{}, bgr10_ubwc{};

    CASE("xr10_bgr10_linear_1200x2608");
    {
        auto q = base_query(FMT_BGR10_XR, 1200u, 2608u, 0u, 4u);
        CHECK(a840_compute_texture_layout(&t16, &q, &bgr10_lin) == A840_COMPILE_OK);
        CHECK(bgr10_lin.ubwc_enabled == 0u);
        CHECK(bgr10_lin.pitch0_bytes >= 1200u * 4u);
        CHECK(bgr10_lin.total_bytes >= static_cast<uint64_t>(bgr10_lin.pitch0_bytes) * 2608u);
        CHECK((bgr10_lin.tex_const_template[4] & A840_TEX_4_FLAG) == 0u);
        CHECK(bgr10_lin.tex_const_template[2] == tex_wh(1200u, 2608u));
    }

    CASE("xr10_bgr10_ubwc6_hbb16_1200x2608");
    {
        auto q = base_query(FMT_BGR10_XR, 1200u, 2608u, 2u, 4u);
        CHECK(a840_compute_texture_layout(&t16, &q, &bgr10_ubwc) == A840_COMPILE_OK);
        CHECK(bgr10_ubwc.ubwc_enabled == 1u);
        CHECK(bgr10_ubwc.base_align == 4096u);
        CHECK(bgr10_ubwc.ubwc_meta_offset == 0u);
        CHECK(bgr10_ubwc.ubwc_meta_pitch > 0u);
        CHECK(bgr10_ubwc.total_bytes > bgr10_lin.total_bytes);
        CHECK((bgr10_ubwc.tex_const_template[4] & A840_TEX_4_FLAG) != 0u);
        CHECK(bgr10_ubwc.tex_const_template[8] != 0u);
        A840TextureLayoutResult ref94{};
        auto q94 = base_query(94u, 1200u, 2608u, 2u, 4u);
        CHECK(a840_compute_texture_layout(&t16, &q94, &ref94) == A840_COMPILE_OK);
        CHECK(std::memcmp(&bgr10_ubwc, &ref94, sizeof(ref94)) == 0);
    }

    CASE("xr10_bgr10_srgb_linear_and_ubwc6_1200x2608");
    {
        A840TextureLayoutResult lin{}, ubwc{};
        auto ql = base_query(FMT_BGR10_XR_SRGB, 1200u, 2608u, 0u, 4u);
        auto qu = base_query(FMT_BGR10_XR_SRGB, 1200u, 2608u, 2u, 4u);
        CHECK(a840_compute_texture_layout(&t16, &ql, &lin) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &qu, &ubwc) == A840_COMPILE_OK);
        CHECK(std::memcmp(&lin, &bgr10_lin, sizeof(lin)) == 0);
        CHECK(std::memcmp(&ubwc, &bgr10_ubwc, sizeof(ubwc)) == 0);
    }

    A840TextureLayoutResult bgra10_lin{}, bgra10_ubwc{};

    CASE("xr10_bgra10_linear_1200x2608");
    {
        auto q = base_query(FMT_BGRA10_XR, 1200u, 2608u, 0u, 4u);
        CHECK(a840_compute_texture_layout(&t16, &q, &bgra10_lin) == A840_COMPILE_OK);
        CHECK(bgra10_lin.ubwc_enabled == 0u);
        CHECK(bgra10_lin.pitch0_bytes >= 1200u * 8u);
        CHECK(bgra10_lin.total_bytes >= static_cast<uint64_t>(bgra10_lin.pitch0_bytes) * 2608u);
    }

    CASE("xr10_bgra10_ubwc6_hbb16_1200x2608");
    {
        auto q = base_query(FMT_BGRA10_XR, 1200u, 2608u, 2u, 4u);
        CHECK(a840_compute_texture_layout(&t16, &q, &bgra10_ubwc) == A840_COMPILE_OK);
        CHECK(bgra10_ubwc.ubwc_enabled == 1u);
        CHECK(bgra10_ubwc.base_align == 4096u);
        CHECK(bgra10_ubwc.ubwc_meta_offset == 0u);
        CHECK(bgra10_ubwc.ubwc_meta_pitch > 0u);
        CHECK(bgra10_ubwc.total_bytes > bgr10_ubwc.total_bytes);
        CHECK((bgra10_ubwc.tex_const_template[4] & A840_TEX_4_FLAG) != 0u);
        A840TextureLayoutResult ref110{};
        auto q110 = base_query(110u, 1200u, 2608u, 2u, 4u);
        CHECK(a840_compute_texture_layout(&t16, &q110, &ref110) == A840_COMPILE_OK);
        CHECK(std::memcmp(&bgra10_ubwc, &ref110, sizeof(ref110)) == 0);
    }

    CASE("xr10_bgra10_srgb_linear_and_ubwc6_1200x2608");
    {
        A840TextureLayoutResult lin{}, ubwc{};
        auto ql = base_query(FMT_BGRA10_XR_SRGB, 1200u, 2608u, 0u, 4u);
        auto qu = base_query(FMT_BGRA10_XR_SRGB, 1200u, 2608u, 2u, 4u);
        CHECK(a840_compute_texture_layout(&t16, &ql, &lin) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &qu, &ubwc) == A840_COMPILE_OK);
        CHECK(std::memcmp(&lin, &bgra10_lin, sizeof(lin)) == 0);
        CHECK(std::memcmp(&ubwc, &bgra10_ubwc, sizeof(ubwc)) == 0);
    }

    CASE("xr10_mips_and_msaa4x_ubwc6");
    {
        A840TextureLayoutResult mip_res{}, msaa_res{};
        auto qm = base_query(FMT_BGR10_XR, 512u, 512u, 2u);
        qm.mip_levels = 4u;
        CHECK(a840_compute_texture_layout(&t16, &qm, &mip_res) == A840_COMPILE_OK);
        CHECK(mip_res.ubwc_enabled == 1u && mip_res.total_bytes > 512u * 512u * 4u);

        auto q4 = base_query(FMT_BGRA10_XR, 512u, 512u, 2u);
        q4.sample_count = 4u;
        CHECK(a840_compute_texture_layout(&t16, &q4, &msaa_res) == A840_COMPILE_OK);
        CHECK(msaa_res.ubwc_enabled == 1u && msaa_res.total_bytes >= 512u * 512u * 8u * 4u);
    }

    CASE("nv12_plane0_y_linear_1920x1080");
    {
        A840TextureLayoutResult y500{}, y420v{}, r8{};
        auto q0 = base_query(FMT_NV12_VID, 1920u, 1080u, 0u, PLANE_FLAG(0u));
        auto qv = base_query(CV_420V, 1920u, 1080u, 0u, PLANE_FLAG(0u));
        auto qr = base_query(10u, 1920u, 1080u, 0u);
        CHECK(a840_compute_texture_layout(&t16, &q0, &y500) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &qv, &y420v) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &qr, &r8) == A840_COMPILE_OK);
        CHECK(y500.ubwc_enabled == 0u && y500.pitch0_bytes >= 1920u);
        CHECK(std::memcmp(&y500, &y420v, sizeof(y500)) == 0);
        CHECK(std::memcmp(&y500, &r8, sizeof(y500)) == 0);
    }

    CASE("nv12_plane1_uv_linear_1920x1080");
    {
        A840TextureLayoutResult uv500{}, uv420v{}, rg8{};
        auto q1 = base_query(FMT_NV12_VID, 1920u, 1080u, 0u, PLANE_FLAG(1u));
        auto qv = base_query(CV_420V, 1920u, 1080u, 0u, PLANE_FLAG(1u));
        auto qr = base_query(30u, 960u, 540u, 0u);
        CHECK(a840_compute_texture_layout(&t16, &q1, &uv500) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &qv, &uv420v) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &qr, &rg8) == A840_COMPILE_OK);
        CHECK(uv500.ubwc_enabled == 0u && uv500.pitch0_bytes >= 960u * 2u);
        CHECK(uv500.tex_const_template[2] == tex_wh(960u, 540u));
        CHECK(std::memcmp(&uv500, &uv420v, sizeof(uv500)) == 0);
        CHECK(std::memcmp(&uv500, &rg8, sizeof(uv500)) == 0);
    }

    CASE("nv12_dual_plane_ubwc6_hbb16_1920x1080");
    {
        A840TextureLayoutResult y{}, uv{}, yf{}, uvf{};
        auto qy = base_query(FMT_NV12_VID, 1920u, 1080u, 2u, PLANE_FLAG(0u));
        auto quv = base_query(FMT_NV12_VID, 1920u, 1080u, 2u, PLANE_FLAG(1u));
        auto qyf = base_query(CV_420F, 1920u, 1080u, 2u, PLANE_FLAG(0u));
        auto quvf = base_query(CV_420F, 1920u, 1080u, 2u, PLANE_FLAG(1u));
        CHECK(a840_compute_texture_layout(&t16, &qy, &y) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &quv, &uv) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &qyf, &yf) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &quvf, &uvf) == A840_COMPILE_OK);
        CHECK(y.ubwc_enabled == 1u && y.base_align == 4096u && y.ubwc_meta_pitch > 0u);
        CHECK(uv.ubwc_enabled == 1u && uv.base_align == 4096u && uv.ubwc_meta_pitch > 0u);
        CHECK((y.tex_const_template[4] & A840_TEX_4_FLAG) != 0u);
        CHECK((uv.tex_const_template[4] & A840_TEX_4_FLAG) != 0u);
        CHECK(y.total_bytes > uv.total_bytes);
        CHECK(std::memcmp(&y, &yf, sizeof(y)) == 0);
        CHECK(std::memcmp(&uv, &uvf, sizeof(uv)) == 0);
    }

    CASE("nv12_full_matches_direct_r8_rg8_ubwc6_1280x720");
    {
        A840TextureLayoutResult y{}, uv{}, r8{}, rg8{};
        auto qy = base_query(FMT_NV12_FULL, 1280u, 720u, 2u, PLANE_FLAG(0u));
        auto quv = base_query(FMT_NV12_FULL, 1280u, 720u, 2u, PLANE_FLAG(1u));
        auto qr8 = base_query(10u, 1280u, 720u, 2u);
        auto qrg8 = base_query(30u, 640u, 360u, 2u);
        CHECK(a840_compute_texture_layout(&t16, &qy, &y) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &quv, &uv) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &qr8, &r8) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &qrg8, &rg8) == A840_COMPILE_OK);
        CHECK(std::memcmp(&y, &r8, sizeof(y)) == 0);
        CHECK(std::memcmp(&uv, &rg8, sizeof(uv)) == 0);
    }

    CASE("p010_plane0_y_linear_3840x2160");
    {
        A840TextureLayoutResult y{}, yx{}, r16{};
        auto q0 = base_query(FMT_P010_VID, 3840u, 2160u, 0u, PLANE_FLAG(0u));
        auto qx = base_query(CV_X420, 3840u, 2160u, 0u, PLANE_FLAG(0u));
        auto qr = base_query(20u, 3840u, 2160u, 0u);
        CHECK(a840_compute_texture_layout(&t16, &q0, &y) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &qx, &yx) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &qr, &r16) == A840_COMPILE_OK);
        CHECK(y.ubwc_enabled == 0u && y.pitch0_bytes >= 3840u * 2u);
        CHECK(std::memcmp(&y, &yx, sizeof(y)) == 0);
        CHECK(std::memcmp(&y, &r16, sizeof(y)) == 0);
    }

    CASE("p010_plane1_uv_linear_3840x2160");
    {
        A840TextureLayoutResult uv{}, uvx{}, rg16{};
        auto q1 = base_query(FMT_P010_VID, 3840u, 2160u, 0u, PLANE_FLAG(1u));
        auto qx = base_query(CV_X420, 3840u, 2160u, 0u, PLANE_FLAG(1u));
        auto qr = base_query(60u, 1920u, 1080u, 0u);
        CHECK(a840_compute_texture_layout(&t16, &q1, &uv) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &qx, &uvx) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &qr, &rg16) == A840_COMPILE_OK);
        CHECK(uv.ubwc_enabled == 0u && uv.pitch0_bytes >= 1920u * 4u);
        CHECK(uv.tex_const_template[2] == tex_wh(1920u, 1080u));
        CHECK(std::memcmp(&uv, &uvx, sizeof(uv)) == 0);
        CHECK(std::memcmp(&uv, &rg16, sizeof(uv)) == 0);
    }

    CASE("p010_dual_plane_ubwc6_hbb16_3840x2160");
    {
        A840TextureLayoutResult y{}, uv{}, yf{}, uvf{};
        auto qy = base_query(FMT_P010_VID, 3840u, 2160u, 2u, PLANE_FLAG(0u));
        auto quv = base_query(FMT_P010_VID, 3840u, 2160u, 2u, PLANE_FLAG(1u));
        auto qyf = base_query(CV_XF20, 3840u, 2160u, 2u, PLANE_FLAG(0u));
        auto quvf = base_query(CV_XF20, 3840u, 2160u, 2u, PLANE_FLAG(1u));
        CHECK(a840_compute_texture_layout(&t16, &qy, &y) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &quv, &uv) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &qyf, &yf) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &quvf, &uvf) == A840_COMPILE_OK);
        CHECK(y.ubwc_enabled == 1u && y.base_align == 4096u && y.ubwc_meta_pitch > 0u);
        CHECK(uv.ubwc_enabled == 1u && uv.base_align == 4096u && uv.ubwc_meta_pitch > 0u);
        CHECK((y.tex_const_template[4] & A840_TEX_4_FLAG) != 0u);
        CHECK((uv.tex_const_template[4] & A840_TEX_4_FLAG) != 0u);
        CHECK(std::memcmp(&y, &yf, sizeof(y)) == 0);
        CHECK(std::memcmp(&uv, &uvf, sizeof(uv)) == 0);
    }

    CASE("p010_full_matches_direct_r16_rg16_ubwc6_1920x1080");
    {
        A840TextureLayoutResult y{}, uv{}, r16{}, rg16{};
        auto qy = base_query(FMT_P010_FULL, 1920u, 1080u, 2u, PLANE_FLAG(0u));
        auto quv = base_query(FMT_P010_FULL, 1920u, 1080u, 2u, PLANE_FLAG(1u));
        auto qr16 = base_query(20u, 1920u, 1080u, 2u);
        auto qrg16 = base_query(60u, 960u, 540u, 2u);
        CHECK(a840_compute_texture_layout(&t16, &qy, &y) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &quv, &uv) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &qr16, &r16) == A840_COMPILE_OK);
        CHECK(a840_compute_texture_layout(&t16, &qrg16, &rg16) == A840_COMPILE_OK);
        CHECK(std::memcmp(&y, &r16, sizeof(y)) == 0);
        CHECK(std::memcmp(&uv, &rg16, sizeof(uv)) == 0);
    }

    CASE("existing_bgra8_rgba8_rgba16f_depth32f_regression");
    {
        const uint32_t legacy[] = {80u, 70u, 115u, 252u};
        for (uint32_t fmt : legacy) {
            A840TextureLayoutResult lin{}, ubwc{};
            auto ql = base_query(fmt, 1200u, 2608u, 0u);
            auto qu = base_query(fmt, 1200u, 2608u, 2u);
            CHECK(a840_compute_texture_layout(&t16, &ql, &lin) == A840_COMPILE_OK);
            CHECK(a840_compute_texture_layout(&t16, &qu, &ubwc) == A840_COMPILE_OK);
            CHECK(ubwc.ubwc_enabled == 1u && ubwc.base_align == 4096u);
            CHECK(ubwc.total_bytes > 0u && lin.total_bytes > 0u);
        }
    }

    CASE("fail_closed_hbb15_rejected_on_xr10_and_ycbcr");
    {
        A840CompileTarget t15 = kaanapali_target(0u);
        t15.highest_bank_bit = 15u;
        for (uint32_t fmt : {FMT_BGRA10_XR, FMT_BGRA10_XR_SRGB, FMT_BGR10_XR, FMT_BGR10_XR_SRGB,
                             FMT_NV12_VID, FMT_P010_VID, CV_420V, CV_X420}) {
            expect_layout_reject(t15, base_query(fmt, 256u, 256u, 2u), A840_COMPILE_INVALID_ARG);
            expect_layout_reject(t15, base_query(fmt, 256u, 256u, 0u), A840_COMPILE_INVALID_ARG);
        }
    }

    CASE("fail_closed_hbb14_and_hbb17_rejected");
    {
        A840CompileTarget t14 = kaanapali_target(0u);
        t14.highest_bank_bit = 14u;
        A840CompileTarget t17 = kaanapali_target(0u);
        t17.highest_bank_bit = 17u;
        expect_layout_reject(t14, base_query(FMT_BGR10_XR, 256u, 256u, 2u), A840_COMPILE_INVALID_ARG);
        expect_layout_reject(t17, base_query(FMT_NV12_VID, 256u, 256u, 2u), A840_COMPILE_INVALID_ARG);
    }

    CASE("fail_closed_wrong_ubwc_swizzle_or_macrotile_on_phase3");
    {
        A840CompileTarget bad_swiz = kaanapali_target(0u);
        bad_swiz.ubwc_swizzle = 3u;
        A840CompileTarget bad_macro = kaanapali_target(0u);
        bad_macro.macrotile_mode = 0u;
        expect_layout_reject(bad_swiz, base_query(FMT_BGR10_XR, 256u, 256u, 2u), A840_COMPILE_INVALID_ARG);
        expect_layout_reject(bad_macro, base_query(FMT_BGRA10_XR, 256u, 256u, 2u), A840_COMPILE_INVALID_ARG);
        expect_layout_reject(bad_swiz, base_query(FMT_NV12_VID, 256u, 256u, 2u), A840_COMPILE_INVALID_ARG);
        expect_layout_reject(bad_macro, base_query(FMT_P010_VID, 256u, 256u, 2u), A840_COMPILE_INVALID_ARG);
    }

    CASE("fail_closed_odd_or_tiny_dimensions_on_ycbcr");
    {
        expect_layout_reject(t16, base_query(FMT_NV12_VID, 1921u, 1080u, 2u), A840_COMPILE_INVALID_ARG);
        expect_layout_reject(t16, base_query(FMT_NV12_VID, 1920u, 1081u, 2u), A840_COMPILE_INVALID_ARG);
        expect_layout_reject(t16, base_query(FMT_P010_VID, 1u, 2u, 0u), A840_COMPILE_INVALID_ARG);
        expect_layout_reject(t16, base_query(FMT_P010_VID, 2u, 1u, 0u), A840_COMPILE_INVALID_ARG);
    }

    CASE("fail_closed_illegal_plane_index_on_ycbcr");
    {
        expect_layout_reject(t16, base_query(FMT_NV12_VID, 256u, 256u, 2u, PLANE_FLAG(2u)),
                             A840_COMPILE_INVALID_ARG);
        expect_layout_reject(t16, base_query(FMT_P010_VID, 256u, 256u, 2u, PLANE_FLAG(3u)),
                             A840_COMPILE_INVALID_ARG);
        expect_layout_reject(t16, base_query(CV_420V, 256u, 256u, 0u, PLANE_FLAG(255u)),
                             A840_COMPILE_INVALID_ARG);
    }

    CASE("fail_closed_nonzero_plane_on_nonplanar_formats");
    {
        expect_layout_reject(t16, base_query(80u, 256u, 256u, 2u, PLANE_FLAG(1u)),
                             A840_COMPILE_INVALID_ARG);
        expect_layout_reject(t16, base_query(FMT_BGR10_XR, 256u, 256u, 2u, PLANE_FLAG(1u)),
                             A840_COMPILE_INVALID_ARG);
        expect_layout_reject(t16, base_query(FMT_BGRA10_XR, 256u, 256u, 2u, PLANE_FLAG(1u)),
                             A840_COMPILE_INVALID_ARG);
    }

    CASE("fail_closed_ycbcr_disallows_3d_cube_msaa_mips_depth");
    {
        expect_layout_reject(t16, base_query(FMT_NV12_VID, 256u, 256u, 2u, 1u),
                             A840_COMPILE_INVALID_ARG);
        expect_layout_reject(t16, base_query(FMT_NV12_VID, 256u, 256u, 2u, 2u),
                             A840_COMPILE_INVALID_ARG);
        auto q_msaa = base_query(FMT_P010_VID, 256u, 256u, 2u);
        q_msaa.sample_count = 4u;
        expect_layout_reject(t16, q_msaa, A840_COMPILE_INVALID_ARG);
        auto q_mip = base_query(FMT_P010_VID, 256u, 256u, 2u);
        q_mip.mip_levels = 2u;
        expect_layout_reject(t16, q_mip, A840_COMPILE_INVALID_ARG);
        auto q_depth = base_query(FMT_NV12_VID, 256u, 256u, 2u);
        q_depth.depth = 2u;
        expect_layout_reject(t16, q_depth, A840_COMPILE_INVALID_ARG);
    }

    CASE("fail_closed_oversized_dimensions_and_unknown_flags");
    {
        expect_layout_reject(t16, base_query(FMT_BGR10_XR, 16385u, 256u, 2u),
                             A840_COMPILE_INVALID_ARG);
        expect_layout_reject(t16, base_query(FMT_BGRA10_XR, 256u, 16385u, 2u),
                             A840_COMPILE_INVALID_ARG);
        expect_layout_reject(t16, base_query(FMT_BGR10_XR, 256u, 256u, 2u, 1u << 3),
                             A840_COMPILE_INVALID_ARG);
        expect_layout_reject(t16, base_query(80u, 256u, 256u, 2u, 1u << 16),
                             A840_COMPILE_INVALID_ARG);
        expect_layout_reject(t16, base_query(80u, 256u, 256u, 2u, 3u),
                             A840_COMPILE_INVALID_ARG);
    }
}

} // namespace

int main()
{
    run_fbfetch_tests();
    run_xr10_and_ycbcr_fdl_tests();
    CHECK(g_cases >= 25u);
    std::printf("PASS: test_ir3_phase3_fbfetch_xr10 (%zu cases / %zu checks)\n",
                g_cases, g_checks);
    return 0;
}
