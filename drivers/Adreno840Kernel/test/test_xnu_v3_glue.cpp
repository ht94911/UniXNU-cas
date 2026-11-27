/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoV3Glue.h"
#include "Adreno840Regs.h"
#include "a840_context_init.h"
#include "a840_emit_3d.h"
#include "a840_emit_compute.h"
#include "a840_emit_ops.h"
#include "gen/a840_bitfields.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* Optional real emitter links:
 *  - When linked with the exact Task Card minimal C_SRCS (a840_context_init.c +
 *    a840_cs.c), these weak declarations resolve to nullptr and tests validate
 *    real a840_emit_context_init/bin_preamble + a840_cs-built 2D/compute/3D
 *    PM4 streams.
 *  - When linked with the full kits/Adreno840Emit C sources (and --require-real-emitters),
 *    these resolve to the real Wave 1/2 emitter implementations and every
 *    real emitted PM4 stream is validated directly.
 */
extern "C" {
A840EmitStatus a840_emit_2d_clear(A840CmdStream *cs, const A840Clear2DDesc *desc)
    __attribute__((weak));
A840EmitStatus a840_emit_compute_dispatch(A840CmdStream *cs, const A840DispatchDesc *desc)
    __attribute__((weak));
A840EmitStatus a840_emit_3d_draw(A840CmdStream *cs, const A840DrawDesc *desc)
    __attribute__((weak));
A840EmitStatus a840_emit_wfi(A840CmdStream *cs) __attribute__((weak));
}

namespace {

std::atomic<uint64_t> g_checks{0};
unsigned g_cases = 0;
const char *g_active_case = "";
bool g_require_real_emitters = false;
unsigned g_real_emitters_invoked = 0;

#define CHECK(expr) do { \
    g_checks.fetch_add(1u, std::memory_order_relaxed); \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL %s:%d [%s]: %s\n", \
            __FILE__, __LINE__, g_active_case, #expr); \
        std::exit(1); \
    } \
} while (0)

void begin_case(const char *name)
{
    g_active_case = name;
    g_cases++;
}

constexpr uint64_t kDefaultZeroPageVa = UINT64_C(0x100000000);
constexpr uint64_t kAltZeroPageVa = UINT64_C(0x200000000);

constexpr uint32_t kAllSixBlockedRegs[6] = {
    A840_REG_RB_DBG_ECO_CNTL,
    A840_REG_RB_RBP_CNTL,
    A840_REG_RB_GC_GMEM_PROTECT,
    A840_REG_SP_NC_MODE_CNTL_2,
    A840_REG_VPC_LB_MODE_CNTL,
    A840_REG_PC_CONTEXT_SWITCH_GFX_PREEMPTION_MODE
};

A840ContextInitParams make_ctx_init_params()
{
    A840ContextInitParams p{};
    p.border_color_gpu_va = UINT64_C(0x100000);
    p.border_color_bo = 77u;
    p.bin_preamble_br_va = UINT64_C(0x200000);
    p.bin_preamble_bv_va = UINT64_C(0x210000);
    p.preempt_preamble_va = UINT64_C(0x220000);
    p.preempt_postamble_va = UINT64_C(0x230000);
    p.bin_preamble_br_dwords = 124u;
    p.bin_preamble_bv_dwords = 118u;
    p.preempt_preamble_dwords = 8u;
    p.preempt_postamble_dwords = 4u;
    p.vis_stream_count = 3u;
    p.use_tex_coord_nearest_even = 0u;
    p.enable_early_preamble_reset = 1u;
    return p;
}

struct CmdBuf {
    std::vector<uint32_t> words;
    std::vector<A840Reloc> relocs;
    A840CmdStream cs{};

    explicit CmdBuf(size_t cap = 2048u, size_t reloc_cap = 128u)
        : words(cap, 0u), relocs(reloc_cap)
    {
        CHECK(a840_cs_init(&cs, words.data(), words.size(),
            relocs.data(), relocs.size()) == A840_EMIT_OK);
    }
};

void emit_2d_clear_stream(CmdBuf &cb)
{
    auto *volatile fn = &a840_emit_2d_clear;
    if (fn != nullptr) {
        A840Clear2DDesc desc{};
        desc.dst.bo_handle = 0x200u;
        desc.dst.gpu_va = UINT64_C(0x200000);
        desc.dst.format_id = A840_FMT6_8_8_8_8_UNORM;
        desc.dst.width = 64u;
        desc.dst.height = 32u;
        desc.dst.pitch = 256u;
        desc.rect = {3u, 4u, 32u, 16u};
        desc.color[0] = 0x12u;
        desc.color[1] = 0x34u;
        desc.color[2] = 0x56u;
        desc.color[3] = 0xffu;
        CHECK((*fn)(&cb.cs, &desc) == A840_EMIT_OK);
        g_real_emitters_invoked |= 1u;
    } else {
        CHECK(!g_require_real_emitters);
        const uint32_t blit_cntl = 0x00f03080u;
        const uint32_t dst_lo_hi[2] = {0x00200000u, 0x0u};
        const uint32_t blit_op = 3u;
        CHECK(a840_cs_emit_pkt4(&cb.cs, A840_REG_GRAS_A2D_BLT_CNTL, &blit_cntl, 1u) == A840_EMIT_OK);
        CHECK(a840_cs_emit_pkt4(&cb.cs, A840_REG_RB_A2D_DEST_BUFFER_BASE, dst_lo_hi, 2u) == A840_EMIT_OK);
        CHECK(a840_cs_emit_pkt7(&cb.cs, A840_CP_BLIT, &blit_op, 1u) == A840_EMIT_OK);
    }
}

void emit_compute_dispatch_stream(CmdBuf &cb, A840DispatchMode mode = A840_DISPATCH_UNIFORM)
{
    auto *volatile fn = &a840_emit_compute_dispatch;
    if (fn != nullptr) {
        A840CompiledStage stage{};
        stage.abi_version = ADRENO_COMPILE_ABI_VERSION;
        stage.stage = A840_STAGE_COMPUTE;
        stage.instruction_groups = 40u;
        stage.isa_dwords = 160u;
        stage.max_full_reg = 7u;
        stage.max_half_reg = 3u;
        stage.branchstack = 3u;
        stage.constlen_vec4 = 128u;
        stage.local_size[0] = 8u;
        stage.local_size[1] = 4u;
        stage.local_size[2] = 2u;

        A840DispatchDesc desc{};
        desc.stage = &stage;
        desc.mode = mode;
        desc.size[0] = (mode == A840_DISPATCH_NON_UNIFORM) ? 17u : 3u;
        desc.size[1] = (mode == A840_DISPATCH_NON_UNIFORM) ? 7u : 2u;
        desc.size[2] = (mode == A840_DISPATCH_NON_UNIFORM) ? 3u : 1u;
        desc.indirect = {70u, UINT64_C(0x10000800040)};
        CHECK((*fn)(&cb.cs, &desc) == A840_EMIT_OK);
        g_real_emitters_invoked |= 2u;
    } else {
        CHECK(!g_require_real_emitters);
        const uint32_t ndrange[4] = {3u, 24u, 0u, 8u};
        const uint32_t exec_cs[4] = {0u, 3u, 2u, 1u};
        CHECK(a840_cs_emit_pkt4(&cb.cs, A840_REG_SP_CS_NDRANGE_0, ndrange, 4u) == A840_EMIT_OK);
        CHECK(a840_cs_emit_pkt7(&cb.cs, A840_CP_EXEC_CS, exec_cs, 4u) == A840_EMIT_OK);
    }
}

void emit_3d_draw_stream(CmdBuf &cb, A840IndexType idx_type = A840_INDEX_NONE)
{
    auto *volatile fn = &a840_emit_3d_draw;
    if (fn != nullptr) {
        A840DrawDesc desc{};
        desc.primitive = A840_PRIM_TRIANGLES;
        desc.index_type = idx_type;
        desc.count = 6u;
        desc.instance_count = 2u;
        desc.first_instance = 7u;
        desc.first_vertex = 3u;
        desc.base_vertex = -2;
        desc.first_index = 4u;
        desc.index_capacity = 32u;
        desc.index_buffer = {88u, UINT64_C(0x11000008000)};
        CHECK((*fn)(&cb.cs, &desc) == A840_EMIT_OK);
        g_real_emitters_invoked |= 4u;
    } else {
        CHECK(!g_require_real_emitters);
        const uint32_t draw_cntl = 132u;
        const uint32_t draw_args[3] = {0u, 2u, 6u};
        CHECK(a840_cs_emit_pkt4(&cb.cs, A840_REG_PC_DRAWCALL_CNTL, &draw_cntl, 1u) == A840_EMIT_OK);
        CHECK(a840_cs_emit_pkt7(&cb.cs, A840_CP_DRAW_INDX_OFFSET, draw_args, 3u) == A840_EMIT_OK);
        CHECK(a840_cs_emit_sds_group(&cb.cs, 0u, A840_SDS_SYSMEM, 10u,
            UINT64_C(0x10004000), 8u) == A840_EMIT_OK);
    }
}

/* ======================== GROUP 1: IB DIAG (18 cases) ======================== */

void test_ib_null_and_empty_args()
{
    begin_case("ib_null_and_empty_args");
    uint32_t dummy = 0u;
    A840KmdIbDiagResult res{};
    CHECK(a840_kmd_validate_ib_words(&dummy, 1u, nullptr) == A840_KMD_IB_INVALID_ARG);
    CHECK(a840_kmd_validate_ib_words(nullptr, 1u, &res) == A840_KMD_IB_INVALID_ARG);
    CHECK(res.status == A840_KMD_IB_INVALID_ARG);
    CHECK(a840_kmd_validate_ib_words(nullptr, 0u, &res) == A840_KMD_IB_INVALID_ARG);
    CHECK(a840_kmd_validate_ib_words(&dummy, 0u, &res) == A840_KMD_IB_EMPTY);
    CHECK(res.status == A840_KMD_IB_EMPTY);

    /* Aliasing rejection without mutating overlapping output. */
    alignas(A840KmdIbDiagResult) uint8_t alias_buf[sizeof(A840KmdIbDiagResult)]{};
    auto *aliased_out = reinterpret_cast<A840KmdIbDiagResult *>(alias_buf);
    aliased_out->total_packets = 999u;
    CHECK(a840_kmd_validate_ib_words(
        reinterpret_cast<const uint32_t *>(alias_buf), 4u, aliased_out) == A840_KMD_IB_INVALID_ARG);
    CHECK(aliased_out->total_packets == 999u);

    /* Byte wrapper checks alignment and multiple-of-4 size. */
    alignas(uint32_t) uint8_t raw[16]{};
    CHECK(a840_kmd_validate_ib_bytes(nullptr, 4u, &res) == A840_KMD_IB_INVALID_ARG);
    CHECK(a840_kmd_validate_ib_bytes(raw, 4u, nullptr) == A840_KMD_IB_INVALID_ARG);
    CHECK(a840_kmd_validate_ib_bytes(raw, 3u, &res) == A840_KMD_IB_INVALID_ARG);
    CHECK(a840_kmd_validate_ib_bytes(raw + 1u, 4u, &res) == A840_KMD_IB_INVALID_ARG);
    CHECK(a840_kmd_validate_ib_bytes(raw, 0u, &res) == A840_KMD_IB_EMPTY);
}

void test_ib_valid_context_init_full()
{
    begin_case("ib_valid_context_init_full");
    CmdBuf cb;
    const auto p = make_ctx_init_params();
    CHECK(a840_emit_context_init(&cb.cs, &p) == A840_EMIT_OK);
    CHECK(cb.cs.used_dwords > 100u);

    A840KmdIbDiagResult diag{};
    CHECK(a840_kmd_validate_ib_words(cb.words.data(), cb.cs.used_dwords, &diag) == A840_KMD_IB_OK);
    CHECK(diag.status == A840_KMD_IB_OK);
    CHECK(diag.validated_dwords == cb.cs.used_dwords);
    CHECK(diag.total_packets == diag.type4_packets + diag.type7_packets);
    CHECK(diag.type4_packets >= 40u);
    CHECK(diag.type7_packets >= 5u);
    CHECK(diag.offending_reg == 0u);
    CHECK(diag.offending_dword_index == 0u);
}

void test_ib_valid_bin_preambles_br_and_bv()
{
    begin_case("ib_valid_bin_preambles_br_and_bv");
    const auto p = make_ctx_init_params();
    for (int is_bv = 0; is_bv <= 1; ++is_bv) {
        CmdBuf cb;
        CHECK(a840_emit_bin_preamble(&cb.cs, &p, is_bv) == A840_EMIT_OK);
        A840KmdIbDiagResult diag{};
        CHECK(a840_kmd_validate_ib_words(cb.words.data(), cb.cs.used_dwords, &diag) == A840_KMD_IB_OK);
        CHECK(diag.validated_dwords == cb.cs.used_dwords);
        CHECK(diag.total_packets > 0u);
    }
}

void test_ib_valid_2d_clear_stream()
{
    begin_case("ib_valid_2d_clear_stream");
    CmdBuf cb;
    emit_2d_clear_stream(cb);
    CHECK(cb.cs.used_dwords > 0u);
    A840KmdIbDiagResult diag{};
    CHECK(a840_kmd_validate_ib_words(cb.words.data(), cb.cs.used_dwords, &diag) == A840_KMD_IB_OK);
    CHECK(diag.validated_dwords == cb.cs.used_dwords);
    CHECK(diag.type4_packets > 0u && diag.type7_packets > 0u);
}

void test_ib_valid_compute_dispatch_stream()
{
    begin_case("ib_valid_compute_dispatch_stream");
    CmdBuf cb;
    emit_compute_dispatch_stream(cb, A840_DISPATCH_UNIFORM);
    emit_compute_dispatch_stream(cb, A840_DISPATCH_NON_UNIFORM);
    A840KmdIbDiagResult diag{};
    CHECK(a840_kmd_validate_ib_words(cb.words.data(), cb.cs.used_dwords, &diag) == A840_KMD_IB_OK);
    CHECK(diag.validated_dwords == cb.cs.used_dwords);
    CHECK(diag.type4_packets >= 2u && diag.type7_packets >= 2u);
}

void test_ib_valid_3d_draw_stream()
{
    begin_case("ib_valid_3d_draw_stream");
    CmdBuf cb;
    emit_3d_draw_stream(cb, A840_INDEX_NONE);
    emit_3d_draw_stream(cb, A840_INDEX_UINT16);
    emit_3d_draw_stream(cb, A840_INDEX_UINT32);
    A840KmdIbDiagResult diag{};
    CHECK(a840_kmd_validate_ib_words(cb.words.data(), cb.cs.used_dwords, &diag) == A840_KMD_IB_OK);
    CHECK(diag.validated_dwords == cb.cs.used_dwords);
    CHECK(diag.type4_packets >= 3u && diag.type7_packets >= 3u);
}

void test_ib_valid_combined_frame_ib()
{
    begin_case("ib_valid_combined_frame_ib");
    CmdBuf cb;
    const auto p = make_ctx_init_params();
    CHECK(a840_emit_context_init(&cb.cs, &p) == A840_EMIT_OK);
    emit_2d_clear_stream(cb);
    emit_compute_dispatch_stream(cb, A840_DISPATCH_UNIFORM);
    emit_3d_draw_stream(cb, A840_INDEX_UINT32);
    CHECK(a840_cs_emit_sds_group(&cb.cs, 1u, A840_SDS_GMEM, 11u, UINT64_C(0x10008000), 12u) == A840_EMIT_OK);
    CHECK(a840_cs_emit_pkt7(&cb.cs, A840_CP_WAIT_FOR_IDLE, nullptr, 0u) == A840_EMIT_OK);

    A840KmdIbDiagResult diag{};
    CHECK(a840_kmd_validate_ib_words(cb.words.data(), cb.cs.used_dwords, &diag) == A840_KMD_IB_OK);

    std::vector<A840PM4Packet> pkts(512u);
    size_t decoded = 0u;
    CHECK(a840_pm4_decode(cb.words.data(), cb.cs.used_dwords, pkts.data(), pkts.size(), &decoded) == A840_EMIT_OK);
    CHECK(diag.total_packets == decoded);
    CHECK(diag.validated_dwords == cb.cs.used_dwords);
}

void test_ib_reject_unknown_packet_types()
{
    begin_case("ib_reject_unknown_packet_types");
    const uint32_t bad_types[] = {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 10u, 15u};
    for (uint32_t t : bad_types) {
        CmdBuf cb;
        CHECK(a840_cs_emit_pkt7(&cb.cs, A840_CP_WAIT_FOR_IDLE, nullptr, 0u) == A840_EMIT_OK);
        const size_t bad_idx = cb.cs.used_dwords;
        const uint32_t bad_word = (t << 28u) | 0x00123456u;
        cb.words[bad_idx] = bad_word;
        cb.cs.used_dwords++;

        A840KmdIbDiagResult diag{};
        CHECK(a840_kmd_validate_ib_words(cb.words.data(), cb.cs.used_dwords, &diag) == A840_KMD_IB_UNKNOWN_PACKET);
        CHECK(diag.offending_dword_index == bad_idx);
        CHECK(diag.offending_header_index == bad_idx);
        CHECK(diag.offending_packet_index == 1u);
        CHECK(diag.offending_word == bad_word);
    }
}

void test_ib_reject_type4_bad_count_parity()
{
    begin_case("ib_reject_type4_bad_count_parity");
    CmdBuf cb;
    const uint32_t v = 0x11223344u;
    CHECK(a840_cs_emit_pkt7(&cb.cs, A840_CP_NOP, &v, 1u) == A840_EMIT_OK);
    const size_t t4_idx = cb.cs.used_dwords;
    CHECK(a840_cs_emit_pkt4(&cb.cs, A840_REG_GRAS_A2D_BLT_CNTL, &v, 1u) == A840_EMIT_OK);
    cb.words[t4_idx] ^= (UINT32_C(1) << 7u); /* flip count odd-parity */

    A840KmdIbDiagResult diag{};
    CHECK(a840_kmd_validate_ib_words(cb.words.data(), cb.cs.used_dwords, &diag) == A840_KMD_IB_BAD_PARITY);
    CHECK(diag.offending_dword_index == t4_idx);
    CHECK(diag.offending_header_index == t4_idx);
    CHECK(diag.offending_packet_index == 1u);
    CHECK(diag.offending_reg == A840_REG_GRAS_A2D_BLT_CNTL);
}

void test_ib_reject_type4_bad_reg_parity()
{
    begin_case("ib_reject_type4_bad_reg_parity");
    CmdBuf cb;
    const uint32_t v = 0x55667788u;
    CHECK(a840_cs_emit_pkt4(&cb.cs, A840_REG_GRAS_A2D_BLT_CNTL, &v, 1u) == A840_EMIT_OK);
    cb.words[0] ^= (UINT32_C(1) << 27u); /* flip reg odd-parity */

    A840KmdIbDiagResult diag{};
    CHECK(a840_kmd_validate_ib_words(cb.words.data(), cb.cs.used_dwords, &diag) == A840_KMD_IB_BAD_PARITY);
    CHECK(diag.offending_dword_index == 0u);
    CHECK(diag.offending_reg == A840_REG_GRAS_A2D_BLT_CNTL);
}

void test_ib_reject_type7_bad_count_and_opcode_parity()
{
    begin_case("ib_reject_type7_bad_count_and_opcode_parity");
    for (unsigned bit : {15u, 23u}) {
        CmdBuf cb;
        const uint32_t payload[2] = {1u, 2u};
        CHECK(a840_cs_emit_pkt7(&cb.cs, A840_CP_WAIT_FOR_IDLE, nullptr, 0u) == A840_EMIT_OK);
        const size_t idx = cb.cs.used_dwords;
        CHECK(a840_cs_emit_pkt7(&cb.cs, A840_CP_NOP, payload, 2u) == A840_EMIT_OK);
        cb.words[idx] ^= (UINT32_C(1) << bit);

        A840KmdIbDiagResult diag{};
        CHECK(a840_kmd_validate_ib_words(cb.words.data(), cb.cs.used_dwords, &diag) == A840_KMD_IB_BAD_PARITY);
        CHECK(diag.offending_dword_index == idx);
        CHECK(diag.offending_opcode == A840_CP_NOP);
        CHECK(diag.offending_packet_index == 1u);
    }
}

void test_ib_reject_bad_headers_reserved_and_zero_count()
{
    begin_case("ib_reject_bad_headers_reserved_and_zero_count");
    A840KmdIbDiagResult diag{};

    /* Type4 count == 0 */
    uint32_t hdr = 0u;
    CHECK(a840_pm4_type4_header(A840_REG_GRAS_A2D_BLT_CNTL, 1u, &hdr) == A840_EMIT_OK);
    uint32_t zero_cnt[2] = {hdr & ~UINT32_C(0x7f), 0u};
    CHECK(a840_kmd_validate_ib_words(zero_cnt, 2u, &diag) == A840_KMD_IB_BAD_HEADER);
    CHECK(diag.offending_dword_index == 0u);

    /* Type4 reserved bit 26 set */
    uint32_t res_t4[2] = {hdr | (UINT32_C(1) << 26u), 0u};
    CHECK(a840_kmd_validate_ib_words(res_t4, 2u, &diag) == A840_KMD_IB_BAD_HEADER);
    CHECK(diag.offending_dword_index == 0u);

    /* Type4 reg + count > 18-bit limit */
    uint32_t wrap_hdr = 0u;
    CHECK(a840_pm4_type4_header(0x3ffffu, 2u, &wrap_hdr) == A840_EMIT_OK);
    uint32_t wrap_words[3] = {wrap_hdr, 1u, 2u};
    CHECK(a840_kmd_validate_ib_words(wrap_words, 3u, &diag) == A840_KMD_IB_BAD_HEADER);
    CHECK(diag.offending_dword_index == 0u);

    /* Type7 reserved bit 14 and reserved bits 24..27 */
    uint32_t t7 = 0u;
    CHECK(a840_pm4_type7_header(A840_CP_WAIT_FOR_IDLE, 0u, &t7) == A840_EMIT_OK);
    uint32_t res_t7_14[1] = {t7 | (UINT32_C(1) << 14u)};
    CHECK(a840_kmd_validate_ib_words(res_t7_14, 1u, &diag) == A840_KMD_IB_BAD_HEADER);
    uint32_t res_t7_hi[1] = {t7 | (UINT32_C(1) << 24u)};
    CHECK(a840_kmd_validate_ib_words(res_t7_hi, 1u, &diag) == A840_KMD_IB_BAD_HEADER);
}

void test_ib_reject_truncated_type4_and_type7()
{
    begin_case("ib_reject_truncated_type4_and_type7");
    A840KmdIbDiagResult diag{};

    /* Truncated Type4 */
    CmdBuf cb4;
    const uint32_t vals[4] = {1u, 2u, 3u, 4u};
    CHECK(a840_cs_emit_pkt7(&cb4.cs, A840_CP_WAIT_FOR_IDLE, nullptr, 0u) == A840_EMIT_OK);
    const size_t hdr_idx4 = cb4.cs.used_dwords;
    CHECK(a840_cs_emit_pkt4(&cb4.cs, A840_REG_SP_CS_NDRANGE_0, vals, 4u) == A840_EMIT_OK);
    CHECK(a840_kmd_validate_ib_words(cb4.words.data(), cb4.cs.used_dwords - 1u, &diag) == A840_KMD_IB_TRUNCATED);
    CHECK(diag.offending_dword_index == hdr_idx4);
    CHECK(diag.offending_header_index == hdr_idx4);
    CHECK(diag.offending_reg == A840_REG_SP_CS_NDRANGE_0);

    /* Truncated Type7 */
    CmdBuf cb7;
    CHECK(a840_cs_emit_pkt7(&cb7.cs, A840_CP_NOP, vals, 3u) == A840_EMIT_OK);
    CHECK(a840_kmd_validate_ib_words(cb7.words.data(), 2u, &diag) == A840_KMD_IB_TRUNCATED);
    CHECK(diag.offending_dword_index == 0u);
    CHECK(diag.offending_opcode == A840_CP_NOP);
}

void test_ib_guardrail_all_6_blocked_regs_single()
{
    begin_case("ib_guardrail_all_6_blocked_regs_single");
    const auto p = make_ctx_init_params();
    for (size_t i = 0u; i < 6u; ++i) {
        const uint32_t blocked_reg = kAllSixBlockedRegs[i];
        CHECK(a840_reg_is_umd_blocked_on_a8xx(blocked_reg) != 0);

        CmdBuf cb;
        CHECK(a840_emit_context_init(&cb.cs, &p) == A840_EMIT_OK);
        const size_t pkt_hdr_idx = cb.cs.used_dwords;
        const uint32_t sentinel = 0xdead0000u | static_cast<uint32_t>(i);
        CHECK(a840_cs_emit_pkt4(&cb.cs, blocked_reg, &sentinel, 1u) == A840_EMIT_OK);
        CHECK(a840_cs_emit_pkt7(&cb.cs, A840_CP_WAIT_FOR_IDLE, nullptr, 0u) == A840_EMIT_OK);

        A840KmdIbDiagResult diag{};
        CHECK(a840_kmd_validate_ib_words(cb.words.data(), cb.cs.used_dwords, &diag) == A840_KMD_IB_BLOCKED_REG);
        CHECK(diag.status == A840_KMD_IB_BLOCKED_REG);
        CHECK(diag.offending_reg == blocked_reg);
        CHECK(diag.offending_header_index == pkt_hdr_idx);
        CHECK(diag.offending_dword_index == pkt_hdr_idx + 1u);
        CHECK(diag.offending_word == sentinel);
    }
}

void test_ib_guardrail_coalesced_multi_reg_type4()
{
    begin_case("ib_guardrail_coalesced_multi_reg_type4");
    for (size_t i = 0u; i < 6u; ++i) {
        const uint32_t blocked_reg = kAllSixBlockedRegs[i];
        const uint32_t start_reg = blocked_reg - 2u;
        const uint32_t payload[5] = {0x10u, 0x20u, 0xbad00000u | static_cast<uint32_t>(i), 0x40u, 0x50u};

        CmdBuf cb;
        CHECK(a840_cs_emit_pkt7(&cb.cs, A840_CP_WAIT_FOR_IDLE, nullptr, 0u) == A840_EMIT_OK);
        const size_t hdr_idx = cb.cs.used_dwords;
        CHECK(a840_cs_emit_pkt4(&cb.cs, start_reg, payload, 5u) == A840_EMIT_OK);

        A840KmdIbDiagResult diag{};
        CHECK(a840_kmd_validate_ib_words(cb.words.data(), cb.cs.used_dwords, &diag) == A840_KMD_IB_BLOCKED_REG);
        CHECK(diag.offending_reg == blocked_reg);
        CHECK(diag.offending_header_index == hdr_idx);
        CHECK(diag.offending_dword_index == hdr_idx + 1u + 2u);
        CHECK(diag.offending_word == payload[2]);
        CHECK(diag.offending_packet_index == 1u);
    }
}

void test_ib_guardrail_adjacent_non_blocked_regs_pass()
{
    begin_case("ib_guardrail_adjacent_non_blocked_regs_pass");
    CmdBuf cb;
    const uint32_t val = 0x12345678u;
    for (uint32_t reg : kAllSixBlockedRegs) {
        CHECK(a840_reg_is_umd_blocked_on_a8xx(reg - 1u) == 0);
        CHECK(a840_reg_is_umd_blocked_on_a8xx(reg + 1u) == 0);
        CHECK(a840_cs_emit_pkt4(&cb.cs, reg - 1u, &val, 1u) == A840_EMIT_OK);
        CHECK(a840_cs_emit_pkt4(&cb.cs, reg + 1u, &val, 1u) == A840_EMIT_OK);
    }
    A840KmdIbDiagResult diag{};
    CHECK(a840_kmd_validate_ib_words(cb.words.data(), cb.cs.used_dwords, &diag) == A840_KMD_IB_OK);
    CHECK(diag.type4_packets == 12u);
    CHECK(diag.total_reg_writes == 12u);
}

void test_ib_validate_bytes_wrapper_pass_and_fail()
{
    begin_case("ib_validate_bytes_wrapper_pass_and_fail");
    CmdBuf cb;
    emit_2d_clear_stream(cb);
    A840KmdIbDiagResult diag{};
    CHECK(a840_kmd_validate_ib_bytes(cb.words.data(), cb.cs.used_dwords * sizeof(uint32_t), &diag) == A840_KMD_IB_OK);

    const uint32_t v = 1u;
    CHECK(a840_cs_emit_pkt4(&cb.cs, A840_REG_VPC_LB_MODE_CNTL, &v, 1u) == A840_EMIT_OK);
    CHECK(a840_kmd_validate_ib_bytes(cb.words.data(), cb.cs.used_dwords * sizeof(uint32_t), &diag) == A840_KMD_IB_BLOCKED_REG);
    CHECK(diag.offending_reg == A840_REG_VPC_LB_MODE_CNTL);
}

void test_ib_diag_ascii_formatter_and_truncation()
{
    begin_case("ib_diag_ascii_formatter_and_truncation");
    CmdBuf cb;
    const uint32_t v = 0xcafebabeu;
    CHECK(a840_cs_emit_pkt4(&cb.cs, A840_REG_RB_GC_GMEM_PROTECT, &v, 1u) == A840_EMIT_OK);
    A840KmdIbDiagResult diag{};
    CHECK(a840_kmd_validate_ib_words(cb.words.data(), cb.cs.used_dwords, &diag) == A840_KMD_IB_BLOCKED_REG);

    char full[A840_KMD_IB_DIAG_LINE_MAX];
    const size_t full_len = a840_kmd_format_ib_diag(&diag, full, sizeof(full));
    CHECK(full_len > 20u && full_len < sizeof(full));
    CHECK(std::strlen(full) == full_len);
    const std::string line(full);
    CHECK(line.find("a840_ib status=blocked_reg") != std::string::npos);
    CHECK(line.find("dword=1") != std::string::npos);
    CHECK(line.find("reg=0x08e09") != std::string::npos);
    CHECK(line.find("word=0xcafebabe") != std::string::npos);

    CHECK(a840_kmd_format_ib_diag(&diag, nullptr, 16u) == 0u);
    CHECK(a840_kmd_format_ib_diag(&diag, full, 0u) == 0u);
    CHECK(a840_kmd_format_ib_diag(nullptr, full, sizeof(full)) == 0u);
    CHECK(full[0] == '\0');

    for (size_t cap : {size_t(1), size_t(2), size_t(8), size_t(24), size_t(48)}) {
        std::vector<char> small(cap + 8u, 'Z');
        const size_t n = a840_kmd_format_ib_diag(&diag, small.data(), cap);
        CHECK(n == cap - 1u);
        CHECK(small[n] == '\0');
        CHECK(small[cap] == 'Z');
    }
}

/* ======================== GROUP 2: FAULT FORMATTER (7 cases) ======================== */

void test_fault_fmt_all_fields_golden()
{
    begin_case("fault_fmt_all_fields_golden");
    A840KmdFaultRecord rec{};
    rec.sequence = 42u;
    rec.client_id = 7u;
    rec.queue_id = 3u;
    rec.fence = 123456u;
    rec.ib1_pc = UINT64_C(0x0000123456789abc);
    rec.ib2_pc = UINT64_C(0x0000fedcba987654);
    rec.rbbm_status = 0x80000010u;
    rec.cp_status = 0x0000a55au;
    rec.smmu_fault_va = UINT64_C(0x00000001deadbeef);
    rec.smmu_fsr = 0x40000002u;
    rec.fault_kind = A840_KMD_FAULT_SMMU_TRANSLATION;
    rec.timestamp_ticks = UINT64_C(9876543210);

    char buf[A840_KMD_FAULT_SNAPSHOT_LINE_MAX];
    const size_t n = a840_kmd_format_fault_snapshot(&rec, buf, sizeof(buf));
    CHECK(n > 100u && n < sizeof(buf));
    CHECK(std::strlen(buf) == n);
    for (size_t i = 0u; i < n; ++i) {
        CHECK(buf[i] >= 0x20 && buf[i] <= 0x7e); /* printable single-line ASCII */
    }
    const std::string s(buf);
    const std::string expected =
        "a840_fault seq=42 kind=smmu_translation client=7 queue=3 fence=123456 "
        "ib1_pc=0x0000123456789abc ib2_pc=0x0000fedcba987654 "
        "rbbm=0x80000010 cp=0x0000a55a "
        "smmu_va=0x00000001deadbeef smmu_fsr=0x40000002 ticks=9876543210";
    CHECK(s == expected);
}

void test_fault_fmt_all_fault_kinds()
{
    begin_case("fault_fmt_all_fault_kinds");
    struct KindExpect { uint32_t kind; const char *name; };
    const KindExpect list[] = {
        {A840_KMD_FAULT_NONE, "kind=none"},
        {A840_KMD_FAULT_GPU_HANG, "kind=gpu_hang"},
        {A840_KMD_FAULT_SMMU_TRANSLATION, "kind=smmu_translation"},
        {A840_KMD_FAULT_SMMU_PERMISSION, "kind=smmu_permission"},
        {A840_KMD_FAULT_IB_ERROR, "kind=ib_error"},
        {99u, "kind=unknown"}
    };
    for (const auto &item : list) {
        A840KmdFaultRecord rec{};
        rec.fault_kind = item.kind;
        char buf[A840_KMD_FAULT_SNAPSHOT_LINE_MAX];
        const size_t n = a840_kmd_format_fault_snapshot(&rec, buf, sizeof(buf));
        CHECK(n > 0u);
        CHECK(std::string(buf).find(item.name) != std::string::npos);
    }
}

void test_fault_fmt_zero_and_max_u64_boundaries()
{
    begin_case("fault_fmt_zero_and_max_u64_boundaries");
    char buf[A840_KMD_FAULT_SNAPSHOT_LINE_MAX];

    A840KmdFaultRecord z{};
    const size_t nz = a840_kmd_format_fault_snapshot(&z, buf, sizeof(buf));
    CHECK(nz > 0u && nz < sizeof(buf));
    CHECK(std::string(buf).find("client=0 queue=0 fence=0") != std::string::npos);
    CHECK(std::string(buf).find("ib1_pc=0x0000000000000000") != std::string::npos);
    CHECK(std::string(buf).find("ticks=0") != std::string::npos);

    A840KmdFaultRecord m{};
    m.sequence = UINT64_MAX;
    m.client_id = UINT64_MAX;
    m.queue_id = UINT64_MAX;
    m.fence = UINT64_MAX;
    m.ib1_pc = UINT64_MAX;
    m.ib2_pc = UINT64_MAX;
    m.rbbm_status = UINT32_MAX;
    m.cp_status = UINT32_MAX;
    m.smmu_fault_va = UINT64_MAX;
    m.smmu_fsr = UINT32_MAX;
    m.fault_kind = A840_KMD_FAULT_SMMU_TRANSLATION;
    m.timestamp_ticks = UINT64_MAX;

    const size_t nm = a840_kmd_format_fault_snapshot(&m, buf, sizeof(buf));
    CHECK(nm > 200u && nm < A840_KMD_FAULT_SNAPSHOT_LINE_MAX - 1u);
    CHECK(std::string(buf).find("ticks=18446744073709551615") != std::string::npos);
    CHECK(std::string(buf).find("smmu_va=0xffffffffffffffff") != std::string::npos);
    CHECK(std::string(buf).find("smmu_fsr=0xffffffff") != std::string::npos);
}

void test_fault_fmt_truncation_sweep()
{
    begin_case("fault_fmt_truncation_sweep");
    A840KmdFaultRecord rec{};
    rec.sequence = 11u;
    rec.client_id = 22u;
    rec.queue_id = 33u;
    rec.fence = 44u;
    rec.ib1_pc = UINT64_C(0x1000200030004000);
    rec.ib2_pc = UINT64_C(0x5000600070008000);
    rec.rbbm_status = 0x11223344u;
    rec.cp_status = 0x55667788u;
    rec.smmu_fault_va = UINT64_C(0x99aabbccddeeff00);
    rec.smmu_fsr = 0x01020304u;
    rec.fault_kind = A840_KMD_FAULT_GPU_HANG;
    rec.timestamp_ticks = 999999u;

    char full[A840_KMD_FAULT_SNAPSHOT_LINE_MAX];
    const size_t full_len = a840_kmd_format_fault_snapshot(&rec, full, sizeof(full));
    CHECK(full_len > 100u);

    for (size_t cap = 1u; cap <= full_len + 4u; ++cap) {
        std::vector<char> guarded(cap + 16u, '#');
        const size_t n = a840_kmd_format_fault_snapshot(&rec, guarded.data(), cap);
        const size_t expect = (cap - 1u < full_len) ? (cap - 1u) : full_len;
        CHECK(n == expect);
        CHECK(guarded[n] == '\0');
        CHECK(std::memcmp(guarded.data(), full, n) == 0);
        for (size_t k = cap; k < guarded.size(); ++k)
            CHECK(guarded[k] == '#');
    }
}

void test_fault_fmt_null_and_zero_cap()
{
    begin_case("fault_fmt_null_and_zero_cap");
    A840KmdFaultRecord rec{};
    char buf[32];
    std::memset(buf, 'X', sizeof(buf));
    CHECK(a840_kmd_format_fault_snapshot(&rec, nullptr, 32u) == 0u);
    CHECK(a840_kmd_format_fault_snapshot(&rec, buf, 0u) == 0u);
    CHECK(buf[0] == 'X');
    CHECK(a840_kmd_format_fault_snapshot(nullptr, buf, sizeof(buf)) == 0u);
    CHECK(buf[0] == '\0');
}

void test_fault_fmt_aliased_buffer()
{
    begin_case("fault_fmt_aliased_buffer");
    alignas(A840KmdFaultRecord) char storage[A840_KMD_FAULT_SNAPSHOT_LINE_MAX]{};
    auto *rec = reinterpret_cast<A840KmdFaultRecord *>(storage);
    rec->sequence = 5u;
    rec->client_id = 9u;
    rec->queue_id = 1u;
    rec->fence = 777u;
    rec->ib1_pc = UINT64_C(0x12340000);
    rec->ib2_pc = UINT64_C(0x56780000);
    rec->rbbm_status = 0xaau;
    rec->cp_status = 0xbbu;
    rec->smmu_fault_va = UINT64_C(0x90000000);
    rec->smmu_fsr = 0xccu;
    rec->fault_kind = A840_KMD_FAULT_SMMU_PERMISSION;
    rec->timestamp_ticks = 54321u;

    char ref[A840_KMD_FAULT_SNAPSHOT_LINE_MAX];
    const size_t n_ref = a840_kmd_format_fault_snapshot(rec, ref, sizeof(ref));
    const size_t n_alias = a840_kmd_format_fault_snapshot(rec, storage, sizeof(storage));
    CHECK(n_ref == n_alias);
    CHECK(std::strcmp(ref, storage) == 0);
}

void test_fault_fmt_determinism()
{
    begin_case("fault_fmt_determinism");
    A840KmdFaultRecord rec{};
    rec.sequence = 100u;
    rec.client_id = 3u;
    rec.queue_id = 2u;
    rec.fence = 55u;
    rec.fault_kind = A840_KMD_FAULT_IB_ERROR;
    rec.timestamp_ticks = 8888u;

    char a[A840_KMD_FAULT_SNAPSHOT_LINE_MAX];
    char b[A840_KMD_FAULT_SNAPSHOT_LINE_MAX];
    const size_t na = a840_kmd_format_fault_snapshot(&rec, a, sizeof(a));
    const size_t nb = a840_kmd_format_fault_snapshot(&rec, b, sizeof(b));
    CHECK(na == nb);
    CHECK(std::memcmp(a, b, na + 1u) == 0);
}

/* ======================== MOCK XNU HOST ENVIRONMENT ======================== */

struct MockV2Client {
    MockV2Client() = default;
    explicit MockV2Client(uint32_t t) : tag(t) {}
    uint32_t tag = 0u;
    bool memory_pressure = false;
    uint64_t next_fence = 1u;
    std::mutex mu;
    struct BoEntry { uint64_t handle; uint64_t bytes; std::vector<uint32_t> words; };
    std::vector<BoEntry> bos;
    uint64_t submit_calls = 0u;
    uint64_t batch_calls = 0u;
    uint64_t other_v2_calls = 0u;

    void add_bo(uint64_t handle, uint64_t bytes, std::vector<uint32_t> words = {})
    {
        std::lock_guard<std::mutex> lk(mu);
        for (auto &b : bos) {
            if (b.handle == handle) {
                b.bytes = bytes;
                b.words = std::move(words);
                return;
            }
        }
        bos.push_back({handle, bytes, std::move(words)});
    }
};

struct MockVm {
    uint32_t asid = 0u;
};

struct MockSubmit {
    uint32_t ring_id = 0u;
};

struct MockHostEnv {
    std::mutex mu;
    std::atomic<uint64_t> hw_ticks{1000u};
    bool fail_next_map = false;
    uint32_t smmu_allocs = 0u;
    uint32_t smmu_frees = 0u;
    uint32_t smmu_maps = 0u;
    uint32_t smmu_unmaps = 0u;
    uint32_t last_map_perms = 0u;
    bool last_backing_all_zero = false;
    std::vector<std::string> fault_lines;
    std::vector<std::string> ib_reject_lines;
    std::vector<uint32_t> promise_events;

    static IOReturn v2_dispatch_cb(
        void *cookie,
        void *client_ptr,
        uint32_t selector,
        const void *in_buf,
        size_t in_bytes,
        void *out_buf,
        size_t out_bytes,
        size_t *out_written)
    {
        (void)cookie;
        auto *c = static_cast<MockV2Client *>(client_ptr);
        if (c == nullptr)
            return kIOReturnBadArgument;
        std::lock_guard<std::mutex> lk(c->mu);
        if (selector == NativeAdrenoV2MethodQuery) {
            if (out_buf == nullptr || out_bytes != sizeof(NativeAdrenoV2ParamsWire))
                return kIOReturnBadArgument;
            NativeAdrenoV2ParamsWire p{};
            p.abi_version = NativeAdrenoAbiVersionV2;
            p.input_bits = 48u;
            p.output_bits = 36u;
            p.flags = NativeAdrenoParamsV2HostImport | NativeAdrenoParamsV2SubmitBatch |
                (c->memory_pressure ? static_cast<uint32_t>(NativeAdrenoParamsV2MemoryPressure) : 0u);
            p.max_bo_bytes = UINT64_C(1) << 30u;
            std::memcpy(out_buf, &p, sizeof(p));
            if (out_written != nullptr)
                *out_written = sizeof(p);
            return kIOReturnSuccess;
        }
        if (selector == NativeAdrenoV2MethodSubmit) {
            if (in_buf == nullptr || in_bytes != sizeof(NativeAdrenoV2SubmitWire) ||
                out_buf == nullptr || out_bytes != sizeof(NativeAdrenoV2SubmitReplyWire))
                return kIOReturnBadArgument;
            c->submit_calls++;
            NativeAdrenoV2SubmitReplyWire rep{};
            rep.fence = c->next_fence++;
            std::memcpy(out_buf, &rep, sizeof(rep));
            if (out_written != nullptr)
                *out_written = sizeof(rep);
            return kIOReturnSuccess;
        }
        if (selector == NativeAdrenoV2MethodSubmitBatch) {
            if (in_buf == nullptr || in_bytes != sizeof(NativeAdrenoV2SubmitBatchWire) ||
                out_buf == nullptr || out_bytes != sizeof(NativeAdrenoV2SubmitBatchReplyWire))
                return kIOReturnBadArgument;
            c->batch_calls++;
            NativeAdrenoV2SubmitBatchWire req{};
            std::memcpy(&req, in_buf, sizeof(req));
            NativeAdrenoV2SubmitBatchReplyWire rep{};
            rep.batch = (req.action == NativeAdrenoV2BatchBegin) ? 42u : req.batch;
            rep.fence = (req.action == NativeAdrenoV2BatchCommit) ? c->next_fence++ : 0u;
            std::memcpy(out_buf, &rep, sizeof(rep));
            if (out_written != nullptr)
                *out_written = sizeof(rep);
            return kIOReturnSuccess;
        }
        /* Generic v2 method 2..18 echo */
        c->other_v2_calls++;
        if (out_buf != nullptr && out_bytes >= sizeof(uint32_t)) {
            const uint32_t echo = selector ^ 0x5a5a0000u;
            std::memcpy(out_buf, &echo, sizeof(echo));
            if (out_written != nullptr)
                *out_written = sizeof(echo);
        }
        return kIOReturnSuccess;
    }

    static A840KmdStatus lookup_bo_size_cb(
        void *cookie,
        void *client_ptr,
        uint64_t bo_handle,
        uint64_t *out_bo_bytes)
    {
        (void)cookie;
        auto *c = static_cast<MockV2Client *>(client_ptr);
        if (c == nullptr || out_bo_bytes == nullptr)
            return A840_KMD_INVALID_ARG;
        std::lock_guard<std::mutex> lk(c->mu);
        for (const auto &b : c->bos) {
            if (b.handle == bo_handle) {
                *out_bo_bytes = b.bytes;
                return A840_KMD_OK;
            }
        }
        return A840_KMD_NOT_FOUND;
    }

    static IOReturn resolve_ib_words_cb(
        void *cookie,
        void *client_ptr,
        void *vm,
        uint64_t bo_handle,
        uint64_t iova,
        uint64_t bytes,
        const uint32_t **out_words,
        size_t *out_dwords)
    {
        (void)cookie;
        (void)vm;
        (void)iova;
        auto *c = static_cast<MockV2Client *>(client_ptr);
        if (c == nullptr || out_words == nullptr || out_dwords == nullptr)
            return kIOReturnBadArgument;
        std::lock_guard<std::mutex> lk(c->mu);
        for (const auto &b : c->bos) {
            if (b.handle == bo_handle) {
                if (bytes > b.words.size() * sizeof(uint32_t) || (bytes % sizeof(uint32_t)) != 0u)
                    return kIOReturnBadArgument;
                *out_words = b.words.data();
                *out_dwords = static_cast<size_t>(bytes / sizeof(uint32_t));
                return kIOReturnSuccess;
            }
        }
        return kIOReturnNotFound;
    }

    static uint64_t read_ticks_cb(void *cookie)
    {
        auto *env = static_cast<MockHostEnv *>(cookie);
        return env->hw_ticks.load(std::memory_order_relaxed);
    }

    static A840KmdStatus custom_alloc_cb(
        void *cookie, uint64_t bytes, uint64_t *out_phys, void **out_cpu, void **out_handle)
    {
        auto *env = static_cast<MockHostEnv *>(cookie);
        auto *mem = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(bytes)));
        if (mem == nullptr)
            return A840_KMD_NO_MEMORY;
        /* Poison with 0xCC to prove ZeroPage manager zeroes every single byte. */
        std::memset(mem, 0xcc, static_cast<size_t>(bytes));
        std::lock_guard<std::mutex> lk(env->mu);
        env->smmu_allocs++;
        *out_phys = UINT64_C(0x400000000);
        *out_cpu = mem;
        *out_handle = mem;
        return A840_KMD_OK;
    }

    static void custom_free_cb(void *cookie, uint64_t phys, void *handle, uint64_t bytes)
    {
        (void)phys;
        (void)bytes;
        auto *env = static_cast<MockHostEnv *>(cookie);
        {
            std::lock_guard<std::mutex> lk(env->mu);
            env->smmu_frees++;
        }
        std::free(handle);
    }

    static A840KmdStatus custom_map_ro_cb(
        void *cookie, uint64_t client_id, void *smmu_ctx,
        uint64_t gpu_va, uint64_t phys_addr, uint64_t bytes, uint32_t perms)
    {
        (void)client_id; (void)smmu_ctx; (void)gpu_va; (void)phys_addr; (void)bytes;
        auto *env = static_cast<MockHostEnv *>(cookie);
        std::lock_guard<std::mutex> lk(env->mu);
        if (env->fail_next_map) {
            env->fail_next_map = false;
            return A840_KMD_IO_ERROR;
        }
        env->smmu_maps++;
        env->last_map_perms = perms;
        return A840_KMD_OK;
    }

    static A840KmdStatus custom_unmap_cb(
        void *cookie, uint64_t client_id, void *smmu_ctx, uint64_t gpu_va, uint64_t bytes)
    {
        (void)client_id; (void)smmu_ctx; (void)gpu_va; (void)bytes;
        auto *env = static_cast<MockHostEnv *>(cookie);
        std::lock_guard<std::mutex> lk(env->mu);
        env->smmu_unmaps++;
        return A840_KMD_OK;
    }

    static void fault_log_cb(
        void *cookie, const char *ascii_line, size_t len, const A840KmdFaultRecord *rec)
    {
        (void)rec;
        auto *env = static_cast<MockHostEnv *>(cookie);
        std::lock_guard<std::mutex> lk(env->mu);
        env->fault_lines.emplace_back(ascii_line, len);
    }

    static void ib_reject_cb(
        void *cookie, uint64_t client_id, uint64_t queue_id,
        const A840KmdIbDiagResult *diag, const char *ascii_line)
    {
        (void)client_id; (void)queue_id; (void)diag;
        auto *env = static_cast<MockHostEnv *>(cookie);
        std::lock_guard<std::mutex> lk(env->mu);
        env->ib_reject_lines.emplace_back(ascii_line);
    }

    static void promise_observer_cb(
        void *cookie,
        uint64_t client_id,
        const struct NativeAdrenoAbiWriterPromiseV3 *promise,
        uint32_t state)
    {
        (void)client_id;
        (void)promise;
        auto *env = static_cast<MockHostEnv *>(cookie);
        std::lock_guard<std::mutex> lk(env->mu);
        env->promise_events.push_back(state);
    }

    NativeAdrenoV3Glue::Config make_config(bool custom_smmu = false, bool audit_ib = true)
    {
        NativeAdrenoV3Glue::Config cfg{};
        cfg.cap_flags = 0u;
        cfg.zero_page_default_va = kDefaultZeroPageVa;
        cfg.audit_ib_on_submit = audit_ib;
        cfg.promise_observer = &MockHostEnv::promise_observer_cb;
        cfg.promise_observer_cookie = this;
        cfg.fault_log_cb = &MockHostEnv::fault_log_cb;
        cfg.fault_log_cookie = this;
        cfg.ib_reject_cb = &MockHostEnv::ib_reject_cb;
        cfg.ib_reject_cookie = this;
        cfg.host_ops.cookie = this;
        cfg.host_ops.v2_dispatch = &MockHostEnv::v2_dispatch_cb;
        cfg.host_ops.lookup_bo_size = &MockHostEnv::lookup_bo_size_cb;
        cfg.host_ops.resolve_ib_words = &MockHostEnv::resolve_ib_words_cb;
        cfg.host_ops.read_ticks = &MockHostEnv::read_ticks_cb;
        if (custom_smmu) {
            cfg.host_ops.smmu_ops.cookie = this;
            cfg.host_ops.smmu_ops.alloc_backing_page = &MockHostEnv::custom_alloc_cb;
            cfg.host_ops.smmu_ops.free_backing_page = &MockHostEnv::custom_free_cb;
            cfg.host_ops.smmu_ops.map_client_ro = &MockHostEnv::custom_map_ro_cb;
            cfg.host_ops.smmu_ops.unmap_client = &MockHostEnv::custom_unmap_cb;
        }
        return cfg;
    }
};

/* ======================== GROUP 3: GLUE FUNCTIONAL (20 cases) ======================== */

void test_glue_uninit_guards_and_double_init()
{
    begin_case("glue_uninit_guards_and_double_init");
    NativeAdrenoV3Glue glue;
    MockV2Client c1{1u};
    CHECK(!glue.initialized());
    CHECK(glue.advertisedCaps() == 0u);
    CHECK(glue.openClient(&c1, nullptr, nullptr) == kIOReturnNotReady);
    CHECK(glue.closeClient(&c1) == kIOReturnNotReady);
    CHECK(glue.externalMethod(&c1, 1u, nullptr, 0u, nullptr, 0u) == kIOReturnNotReady);
    CHECK(glue.signalFenceIrq(&c1, 0u, 1u, 10u) == kIOReturnNotReady);
    A840KmdFaultInput fi{};
    CHECK(glue.reportHardwareFault(&c1, fi) == kIOReturnNotReady);

    /* Partial custom SMMU ops must fail cleanly without leaking locks. */
    MockHostEnv env;
    auto bad_cfg = env.make_config(false);
    bad_cfg.host_ops.smmu_ops.alloc_backing_page = &MockHostEnv::custom_alloc_cb;
    CHECK(glue.init(bad_cfg) == kIOReturnBadArgument);
    CHECK(!glue.initialized());

    /* Misaligned zero_page_default_va rejected. */
    auto mis_cfg = env.make_config(false);
    mis_cfg.zero_page_default_va = UINT64_C(0x100001000);
    CHECK(glue.init(mis_cfg) == kIOReturnBadArgument);

    /* Valid init and double-init rejection. */
    auto ok_cfg = env.make_config(false);
    CHECK(glue.init(ok_cfg) == kIOReturnSuccess);
    CHECK(glue.initialized());
    CHECK(glue.init(ok_cfg) == kIOReturnBusy);
    glue.destroy();
    CHECK(!glue.initialized());
    glue.destroy(); /* idempotent */
}

void test_glue_open_close_multi_client_zero_page_sharing()
{
    begin_case("glue_open_close_multi_client_zero_page_sharing");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);

    MockV2Client c1{1u}, c2{2u}, c3{3u};
    MockVm vm1{1u}, vm2{2u}, vm3{3u};
    uint64_t id1 = 0u, id2 = 0u, id3 = 0u;
    CHECK(glue.openClient(&c1, &vm1, nullptr, 0u, &id1) == kIOReturnSuccess);
    CHECK(glue.openClient(&c2, &vm2, nullptr, kAltZeroPageVa, &id2) == kIOReturnSuccess);
    CHECK(glue.openClient(&c3, &vm3, nullptr, 0u, &id3) == kIOReturnSuccess);
    CHECK(id1 != 0u && id2 != id1 && id3 != id2);

    NativeAdrenoAbiZeroPageReplyV3 r1{}, r2{}, r3{};
    size_t w = 0u;
    CHECK(glue.externalMethod(&c1, NativeAdrenoMethodGetZeroPageV3, nullptr, 0u, &r1, sizeof(r1), &w) == kIOReturnSuccess);
    CHECK(w == sizeof(r1) && r1.gpu_va == kDefaultZeroPageVa && r1.bytes == A840_KMD_ZERO_PAGE_BYTES);
    CHECK(glue.externalMethod(&c2, NativeAdrenoMethodGetZeroPageV3, nullptr, 0u, &r2, sizeof(r2)) == kIOReturnSuccess);
    CHECK(r2.gpu_va == kAltZeroPageVa && r2.bytes == A840_KMD_ZERO_PAGE_BYTES);
    CHECK(glue.externalMethod(&c3, NativeAdrenoMethodGetZeroPageV3, nullptr, 0u, &r3, sizeof(r3)) == kIOReturnSuccess);
    CHECK(r3.gpu_va == kDefaultZeroPageVa && r3.bytes == A840_KMD_ZERO_PAGE_BYTES);

    auto st = glue.getStats();
    CHECK(st.open_clients == 3u);
    CHECK(st.zero_page.active_clients == 3u);
    CHECK(st.zero_page.backing_allocated == 1u);
    CHECK(st.zero_page.total_allocs == 1u);
    CHECK(st.zero_page.total_frees == 0u);

    A840KmdClientCloseSummary sum{};
    CHECK(glue.closeClient(&c1, &sum) == kIOReturnSuccess);
    CHECK(sum.zero_page_unbound == 1u);
    CHECK(glue.getStats().zero_page.backing_allocated == 1u);

    CHECK(glue.closeClient(&c2, &sum) == kIOReturnSuccess);
    CHECK(glue.getStats().zero_page.backing_allocated == 1u);

    CHECK(glue.closeClient(&c3, &sum) == kIOReturnSuccess);
    st = glue.getStats();
    CHECK(st.open_clients == 0u);
    CHECK(st.zero_page.active_clients == 0u);
    CHECK(st.zero_page.backing_allocated == 0u);
    CHECK(st.zero_page.total_frees == 1u);
}

void test_glue_custom_smmu_write_perm_defense_and_failure_rollback()
{
    begin_case("glue_custom_smmu_write_perm_defense_and_failure_rollback");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(true)) == kIOReturnSuccess);

    MockV2Client c1{1u}, c2{2u};
    MockVm vm1{1u}, vm2{2u};
    env.fail_next_map = true;
    CHECK(glue.openClient(&c1, &vm1, nullptr) == kIOReturnIOError);
    CHECK(env.smmu_allocs == 1u && env.smmu_frees == 1u);
    CHECK(glue.getStats().open_clients == 0u);

    /* Now succeed and verify RO permission passed to custom SMMU. */
    CHECK(glue.openClient(&c1, &vm1, nullptr) == kIOReturnSuccess);
    CHECK(env.last_map_perms == A840_KMD_SMMU_PERM_READ_ONLY);
    CHECK(glue.openClient(&c2, &vm2, nullptr) == kIOReturnSuccess);
    CHECK(env.smmu_allocs == 2u && env.smmu_maps == 2u);
    CHECK(glue.closeClient(&c1) == kIOReturnSuccess);
    CHECK(glue.closeClient(&c2) == kIOReturnSuccess);
    CHECK(env.smmu_unmaps == 2u && env.smmu_frees == 2u);
}

void test_glue_v2_passthrough_query_cap_augmentation()
{
    begin_case("glue_v2_passthrough_query_cap_augmentation");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);

    MockV2Client c1{1u};
    c1.memory_pressure = true;
    CHECK(glue.openClient(&c1, nullptr, nullptr) == kIOReturnSuccess);

    NativeAdrenoV2ParamsWire params{};
    size_t written = 0u;
    CHECK(glue.externalMethod(&c1, NativeAdrenoV2MethodQuery, nullptr, 0u,
        &params, sizeof(params), &written) == kIOReturnSuccess);
    CHECK(written == sizeof(params));
    CHECK(params.abi_version == NativeAdrenoAbiVersionV3);
    CHECK((params.flags & NativeAdrenoParamsV2HostImport) != 0u);
    CHECK((params.flags & NativeAdrenoParamsV2MemoryPressure) != 0u);
    CHECK((params.flags & NativeAdrenoParamsV3TimelineWaitAny) != 0u);
    CHECK((params.flags & NativeAdrenoParamsV3SharedZeroPage) != 0u);
    CHECK((params.flags & NativeAdrenoParamsV3SharedTier0Pack) != 0u);
    CHECK((params.flags & NativeAdrenoParamsV3WriterPromise) != 0u);
    CHECK((params.flags & NativeAdrenoParamsV3UbwcSurfaceMeta) != 0u);
    /* Phase 4/5 methods 22/26 MUST NOT be advertised. */
    CHECK((params.flags & NativeAdrenoParamsV3SparseVmBind) == 0u);
    CHECK((params.flags & NativeAdrenoParamsV3PreemptRings) == 0u);
}

void test_glue_v2_passthrough_methods_2_to_18()
{
    begin_case("glue_v2_passthrough_methods_2_to_18");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);

    MockV2Client c1{1u};
    CHECK(glue.openClient(&c1, nullptr, nullptr) == kIOReturnSuccess);

    const uint32_t methods[] = {2u, 3u, 4u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u};
    for (uint32_t m : methods) {
        uint32_t in_w = m;
        uint32_t out_w = 0u;
        size_t written = 0u;
        CHECK(glue.externalMethod(&c1, m, &in_w, sizeof(in_w), &out_w, sizeof(out_w), &written) == kIOReturnSuccess);
        CHECK(written == sizeof(out_w));
        CHECK(out_w == (m ^ 0x5a5a0000u));
    }
    CHECK(c1.other_v2_calls == 16u);
    CHECK(glue.getStats().v2_passthrough_calls == 16u);
}

void test_glue_unsupported_and_envelope_ceiling()
{
    begin_case("glue_unsupported_and_envelope_ceiling");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);
    MockV2Client c1{1u};
    CHECK(glue.openClient(&c1, nullptr, nullptr) == kIOReturnSuccess);

    uint8_t out[64];
    std::memset(out, 0xaa, sizeof(out));
    const uint32_t bad_selectors[] = {0u, NativeAdrenoMethodMapSparseV3, NativeAdrenoMethodQueueConfigV3, 27u, 999u};
    for (uint32_t sel : bad_selectors) {
        CHECK(glue.externalMethod(&c1, sel, nullptr, 0u, out, sizeof(out)) == kIOReturnUnsupported);
        for (uint8_t b : out)
            CHECK(b == 0xaa);
    }

    /* 3104-byte envelope ceiling enforcement. */
    std::vector<uint8_t> big(NativeAdrenoAbiEnvelopeBytes + 1u, 0u);
    CHECK(glue.externalMethod(&c1, NativeAdrenoMethodWaitAnyV3, big.data(), big.size(), out, 16u) == kIOReturnBadArgument);
    CHECK(glue.externalMethod(&c1, NativeAdrenoMethodGetZeroPageV3, nullptr, 0u, big.data(), big.size()) == kIOReturnBadArgument);
}

void test_glue_submit_method5_with_ib_audit_pass()
{
    begin_case("glue_submit_method5_with_ib_audit_pass");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false, true)) == kIOReturnSuccess);

    MockV2Client c1{1u};
    MockVm vm1{1u};
    CHECK(glue.openClient(&c1, &vm1, nullptr) == kIOReturnSuccess);

    CmdBuf cb;
    const auto p = make_ctx_init_params();
    CHECK(a840_emit_context_init(&cb.cs, &p) == A840_EMIT_OK);
    emit_2d_clear_stream(cb);
    emit_compute_dispatch_stream(cb);
    emit_3d_draw_stream(cb);

    std::vector<uint32_t> ib_words(cb.words.begin(), cb.words.begin() + cb.cs.used_dwords);
    c1.add_bo(101u, ib_words.size() * sizeof(uint32_t), ib_words);

    NativeAdrenoV2SubmitWire req{};
    req.buffer_count = 1u;
    req.queue = 0u;
    req.buffers[0].handle = 101u;
    req.buffers[0].iova = UINT64_C(0x100000);
    req.buffers[0].bytes = ib_words.size() * sizeof(uint32_t);
    NativeAdrenoV2SubmitReplyWire rep{};
    size_t w = 0u;
    CHECK(glue.externalMethod(&c1, NativeAdrenoV2MethodSubmit, &req, sizeof(req),
        &rep, sizeof(rep), &w) == kIOReturnSuccess);
    CHECK(w == sizeof(rep) && rep.fence == 1u);
    CHECK(c1.submit_calls == 1u);

    auto st = glue.getStats();
    CHECK(st.ib_validated_ok == 1u && st.ib_rejected_total == 0u);
    A840KmdIbDiagResult last{};
    CHECK(glue.getLastIbDiag(&last) == kIOReturnSuccess);
    CHECK(last.status == A840_KMD_IB_OK && last.validated_dwords == ib_words.size());
}

void test_glue_submit_method5_intercepts_blocked_reg_and_bad_parity()
{
    begin_case("glue_submit_method5_intercepts_blocked_reg_and_bad_parity");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false, true)) == kIOReturnSuccess);

    MockV2Client c1{1u};
    MockVm vm1{1u};
    CHECK(glue.openClient(&c1, &vm1, nullptr) == kIOReturnSuccess);

    /* Blocked Gen8 register inside IB -> kIOReturnNotPermitted, v2 NOT called. */
    CmdBuf bad_reg;
    const uint32_t v = 0x12345678u;
    CHECK(a840_cs_emit_pkt7(&bad_reg.cs, A840_CP_WAIT_FOR_IDLE, nullptr, 0u) == A840_EMIT_OK);
    CHECK(a840_cs_emit_pkt4(&bad_reg.cs, A840_REG_SP_NC_MODE_CNTL_2, &v, 1u) == A840_EMIT_OK);
    std::vector<uint32_t> w1(bad_reg.words.begin(), bad_reg.words.begin() + bad_reg.cs.used_dwords);
    c1.add_bo(201u, w1.size() * sizeof(uint32_t), w1);

    NativeAdrenoV2SubmitWire req{};
    req.buffer_count = 1u;
    req.queue = 2u;
    req.buffers[0] = {201u, UINT64_C(0x200000), w1.size() * sizeof(uint32_t)};
    NativeAdrenoV2SubmitReplyWire rep{};
    CHECK(glue.externalMethod(&c1, NativeAdrenoV2MethodSubmit, &req, sizeof(req),
        &rep, sizeof(rep)) == kIOReturnNotPermitted);
    CHECK(c1.submit_calls == 0u);
    CHECK(rep.fence == 0u);

    A840KmdIbDiagResult diag{};
    CHECK(glue.getLastIbDiag(&diag) == kIOReturnSuccess);
    CHECK(diag.status == A840_KMD_IB_BLOCKED_REG);
    CHECK(diag.offending_reg == A840_REG_SP_NC_MODE_CNTL_2);
    CHECK(diag.offending_header_index == 1u);
    CHECK(diag.offending_dword_index == 2u);
    CHECK(env.ib_reject_lines.size() == 1u);
    CHECK(env.ib_reject_lines[0].find("status=blocked_reg") != std::string::npos);

    /* Channel stays healthy because bad IB was blocked BEFORE hardware! */
    CHECK(glue.checkSubmitChannel(&c1, 2u) == kIOReturnSuccess);

    /* Bad parity IB -> kIOReturnBadArgument, v2 NOT called. */
    CmdBuf bad_par;
    CHECK(a840_cs_emit_pkt4(&bad_par.cs, A840_REG_GRAS_A2D_BLT_CNTL, &v, 1u) == A840_EMIT_OK);
    bad_par.words[0] ^= (UINT32_C(1) << 27u);
    std::vector<uint32_t> w2(bad_par.words.begin(), bad_par.words.begin() + bad_par.cs.used_dwords);
    c1.add_bo(202u, w2.size() * sizeof(uint32_t), w2);
    req.buffers[0] = {202u, UINT64_C(0x300000), w2.size() * sizeof(uint32_t)};
    CHECK(glue.externalMethod(&c1, NativeAdrenoV2MethodSubmit, &req, sizeof(req),
        &rep, sizeof(rep)) == kIOReturnBadArgument);
    CHECK(c1.submit_calls == 0u);
    CHECK(glue.getStats().ib_rejected_total == 2u);
    CHECK(glue.getStats().ib_rejected_blocked_reg == 1u);
}

void test_glue_submit_batch_method19_append_audit_and_abort()
{
    begin_case("glue_submit_batch_method19_append_audit_and_abort");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false, true)) == kIOReturnSuccess);
    MockV2Client c1{1u};
    MockVm vm1{1u};
    CHECK(glue.openClient(&c1, &vm1, nullptr) == kIOReturnSuccess);

    CmdBuf good, bad;
    emit_2d_clear_stream(good);
    const uint32_t v = 1u;
    CHECK(a840_cs_emit_pkt4(&bad.cs, A840_REG_RB_RBP_CNTL, &v, 1u) == A840_EMIT_OK);
    c1.add_bo(10u, good.cs.used_dwords * 4u, std::vector<uint32_t>(good.words.begin(), good.words.begin() + good.cs.used_dwords));
    c1.add_bo(11u, bad.cs.used_dwords * 4u, std::vector<uint32_t>(bad.words.begin(), bad.words.begin() + bad.cs.used_dwords));

    NativeAdrenoV2SubmitBatchWire req{};
    NativeAdrenoV2SubmitBatchReplyWire rep{};

    /* BEGIN */
    req.queue = 1u;
    req.total_buffers = 1u;
    req.action = NativeAdrenoV2BatchBegin;
    CHECK(glue.externalMethod(&c1, NativeAdrenoV2MethodSubmitBatch, &req, sizeof(req), &rep, sizeof(rep)) == kIOReturnSuccess);
    CHECK(rep.batch == 42u);

    /* APPEND valid */
    req.batch = rep.batch;
    req.action = NativeAdrenoV2BatchAppend;
    req.buffer_count = 1u;
    req.buffers[0] = {10u, UINT64_C(0x100000), good.cs.used_dwords * 4u};
    CHECK(glue.externalMethod(&c1, NativeAdrenoV2MethodSubmitBatch, &req, sizeof(req), &rep, sizeof(rep)) == kIOReturnSuccess);

    /* APPEND blocked reg -> intercepted! */
    const uint64_t calls_before = c1.batch_calls;
    req.buffers[0] = {11u, UINT64_C(0x200000), bad.cs.used_dwords * 4u};
    CHECK(glue.externalMethod(&c1, NativeAdrenoV2MethodSubmitBatch, &req, sizeof(req), &rep, sizeof(rep)) == kIOReturnNotPermitted);
    CHECK(c1.batch_calls == calls_before);

    /* Poison queue 1: COMMIT must fail with Aborted, but ABORT is still allowed! */
    A840KmdFaultInput fi{};
    fi.queue_id = 1u;
    fi.fence = 1u;
    fi.fault_kind = A840_KMD_FAULT_GPU_HANG;
    CHECK(glue.reportHardwareFault(&c1, fi) == kIOReturnSuccess);

    req.action = NativeAdrenoV2BatchCommit;
    req.buffer_count = 0u;
    CHECK(glue.externalMethod(&c1, NativeAdrenoV2MethodSubmitBatch, &req, sizeof(req), &rep, sizeof(rep)) == kIOReturnAborted);

    req.action = NativeAdrenoV2BatchAbort;
    CHECK(glue.externalMethod(&c1, NativeAdrenoV2MethodSubmitBatch, &req, sizeof(req), &rep, sizeof(rep)) == kIOReturnSuccess);
}

void test_glue_submit_with_audit_direct_api_and_disable_flag()
{
    begin_case("glue_submit_with_audit_direct_api_and_disable_flag");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false, true)) == kIOReturnSuccess);
    MockV2Client c1{1u};
    CHECK(glue.openClient(&c1, nullptr, nullptr) == kIOReturnSuccess);

    CmdBuf good, bad;
    emit_3d_draw_stream(good);
    const uint32_t v = 7u;
    CHECK(a840_cs_emit_pkt4(&bad.cs, A840_REG_PC_CONTEXT_SWITCH_GFX_PREEMPTION_MODE, &v, 1u) == A840_EMIT_OK);

    A840KmdIbDiagResult diag{};
    CHECK(glue.validateSubmitIb(&c1, 0u, good.words.data(), good.cs.used_dwords, &diag) == kIOReturnSuccess);
    CHECK(diag.status == A840_KMD_IB_OK);
    CHECK(glue.validateSubmitIb(&c1, 0u, bad.words.data(), bad.cs.used_dwords, &diag) == kIOReturnNotPermitted);
    CHECK(diag.offending_reg == A840_REG_PC_CONTEXT_SWITCH_GFX_PREEMPTION_MODE);

    NativeAdrenoV2SubmitWire req{};
    req.buffer_count = 1u;
    req.queue = 0u;
    req.buffers[0] = {1u, UINT64_C(0x10000), good.cs.used_dwords * 4u};
    NativeAdrenoV2SubmitReplyWire rep{};
    CHECK(glue.submitWithAudit(&c1, 0u, bad.words.data(), bad.cs.used_dwords,
        &req, sizeof(req), &rep, sizeof(rep), &diag) == kIOReturnNotPermitted);
    CHECK(c1.submit_calls == 0u);
    CHECK(glue.submitWithAudit(&c1, 0u, good.words.data(), good.cs.used_dwords,
        &req, sizeof(req), &rep, sizeof(rep), &diag) == kIOReturnSuccess);
    CHECK(c1.submit_calls == 1u && rep.fence == 1u);

    /* With audit_ib_on_submit=false, poison guard STILL blocks submission! */
    NativeAdrenoV3Glue glue_no_audit;
    CHECK(glue_no_audit.init(env.make_config(false, false)) == kIOReturnSuccess);
    MockV2Client c2{2u};
    CHECK(glue_no_audit.openClient(&c2, nullptr, nullptr) == kIOReturnSuccess);
    CHECK(glue_no_audit.submitWithAudit(&c2, 0u, bad.words.data(), bad.cs.used_dwords,
        &req, sizeof(req), &rep, sizeof(rep)) == kIOReturnSuccess);
    A840KmdFaultInput fi{};
    fi.queue_id = 0u;
    fi.fault_kind = A840_KMD_FAULT_GPU_HANG;
    CHECK(glue_no_audit.reportHardwareFault(&c2, fi) == kIOReturnSuccess);
    CHECK(glue_no_audit.submitWithAudit(&c2, 0u, good.words.data(), good.cs.used_dwords,
        &req, sizeof(req), &rep, sizeof(rep)) == kIOReturnAborted);
}

void test_glue_method23_shared_pack_configure_and_stack_key()
{
    begin_case("glue_method23_shared_pack_configure_and_stack_key");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);
    MockV2Client c1{1u};
    CHECK(glue.openClient(&c1, nullptr, nullptr) == kIOReturnSuccess);

    NativeAdrenoAbiSharedPackReplyV3 pack{};
    pack.gpu_va = UINT64_C(0x300000000);
    pack.cpu_va = UINT64_C(0x700000000);
    pack.bytes = 65536u * 4u;
    pack.actual_stack_key = UINT64_C(0x1122334455667788);

    /* Non-RO pack must be rejected. */
    CHECK(glue.configureSharedPack(pack, 0u, 1u) == kIOReturnBadArgument);
    CHECK(glue.configureSharedPack(pack, 1u, 1u) == kIOReturnSuccess);

    NativeAdrenoAbiSharedPackReqV3 req{};
    NativeAdrenoAbiSharedPackReplyV3 rep{};
    req.expected_stack_key = pack.actual_stack_key;
    req.map_cpu = 1u;
    CHECK(glue.externalMethod(&c1, NativeAdrenoMethodMapSharedPackV3, &req, sizeof(req),
        &rep, sizeof(rep)) == kIOReturnSuccess);
    CHECK(rep.gpu_va == pack.gpu_va && rep.cpu_va == pack.cpu_va && rep.bytes == pack.bytes);

    req.map_cpu = 0u;
    CHECK(glue.externalMethod(&c1, NativeAdrenoMethodMapSharedPackV3, &req, sizeof(req),
        &rep, sizeof(rep)) == kIOReturnSuccess);
    CHECK(rep.gpu_va == pack.gpu_va && rep.cpu_va == 0u);

    req.expected_stack_key = UINT64_C(0x9999);
    CHECK(glue.externalMethod(&c1, NativeAdrenoMethodMapSharedPackV3, &req, sizeof(req),
        &rep, sizeof(rep)) == kIOReturnBadArgument);
}

void test_glue_method24_writer_promise_lifecycle_and_close_cancel()
{
    begin_case("glue_method24_writer_promise_lifecycle_and_close_cancel");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);
    MockV2Client c1{1u};
    CHECK(glue.openClient(&c1, nullptr, nullptr) == kIOReturnSuccess);

    NativeAdrenoAbiWriterPromiseV3 req{};
    NativeAdrenoAbiWriterPromiseReplyV3 rep{};
    req.op = NativeAdrenoPromiseMake;
    req.surface_count = 2u;
    req.queue = 0u;
    req.surface_ids[0] = 1001u;
    req.surface_ids[1] = 1002u;
    CHECK(glue.externalMethod(&c1, NativeAdrenoMethodWriterPromiseV3, &req, sizeof(req),
        &rep, sizeof(rep)) == kIOReturnSuccess);
    CHECK(rep.accepted == 1u && rep.promise_token != 0u);
    const uint64_t tok1 = rep.promise_token;

    /* Fulfill tok1 */
    req.op = NativeAdrenoPromiseFulfill;
    req.promise_token = tok1;
    req.fence = 55u;
    CHECK(glue.externalMethod(&c1, NativeAdrenoMethodWriterPromiseV3, &req, sizeof(req),
        &rep, sizeof(rep)) == kIOReturnSuccess);

    /* Make tok2 and leave pending until closeClient */
    req = {};
    req.op = NativeAdrenoPromiseMake;
    req.surface_count = 1u;
    req.queue = 0u;
    req.surface_ids[0] = 2001u;
    CHECK(glue.externalMethod(&c1, NativeAdrenoMethodWriterPromiseV3, &req, sizeof(req),
        &rep, sizeof(rep)) == kIOReturnSuccess);

    A840KmdClientCloseSummary sum{};
    CHECK(glue.closeClient(&c1, &sum) == kIOReturnSuccess);
    CHECK(sum.canceled_promises == 1u);
    CHECK(env.promise_events.size() == 4u);
    CHECK(env.promise_events[0] == A840_ABI_V3_PROMISE_PENDING);
    CHECK(env.promise_events[1] == A840_ABI_V3_PROMISE_FULFILLED);
    CHECK(env.promise_events[2] == A840_ABI_V3_PROMISE_PENDING);
    CHECK(env.promise_events[3] == A840_ABI_V3_PROMISE_CANCELED);
}

void test_glue_method25_surface_meta_bo_lookup()
{
    begin_case("glue_method25_surface_meta_bo_lookup");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);
    MockV2Client c1{1u}, c2{2u};
    c1.add_bo(500u, 1024u * 1024u);
    CHECK(glue.openClient(&c1, nullptr, nullptr) == kIOReturnSuccess);
    CHECK(glue.openClient(&c2, nullptr, nullptr) == kIOReturnSuccess);

    NativeAdrenoAbiSurfaceMetaV3 req{}, rep{};
    req.bo_handle = 500u;
    req.op = 1u; /* set */
    req.ubwc_enabled = 1u;
    req.ubwc_version = 6u;
    req.highest_bank_bit = 16u;
    req.swizzle_levels = 6u;
    req.macrotile_mode = 1u;
    req.meta_offset = 4096u;
    req.meta_pitch = 256u;
    req.flags = 1u;

    CHECK(glue.externalMethod(&c1, NativeAdrenoMethodSurfaceMetaV3, &req, sizeof(req),
        &rep, sizeof(rep)) == kIOReturnSuccess);

    /* Read back from c1 */
    NativeAdrenoAbiSurfaceMetaV3 get_req{};
    get_req.bo_handle = 500u;
    get_req.op = 0u;
    rep = {};
    CHECK(glue.externalMethod(&c1, NativeAdrenoMethodSurfaceMetaV3, &get_req, sizeof(get_req),
        &rep, sizeof(rep)) == kIOReturnSuccess);
    CHECK(rep.ubwc_version == 6u && rep.highest_bank_bit == 16u && rep.meta_offset == 4096u);

    /* c2 does NOT own BO 500 -> kIOReturnNotFound */
    CHECK(glue.externalMethod(&c2, NativeAdrenoMethodSurfaceMetaV3, &get_req, sizeof(get_req),
        &rep, sizeof(rep)) == kIOReturnNotFound);

    /* Out-of-bounds meta_offset rejected */
    req.meta_offset = 2u * 1024u * 1024u;
    CHECK(glue.externalMethod(&c1, NativeAdrenoMethodSurfaceMetaV3, &req, sizeof(req),
        &rep, sizeof(rep)) == kIOReturnBadArgument);
}

void test_glue_wait_any_v3_fast_path_and_timeout_on_wait_backend()
{
    begin_case("glue_wait_any_v3_fast_path_and_timeout_on_wait_backend");
    MockHostEnv env;
    env.hw_ticks.store(5000u);
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);
    MockV2Client c1{1u};
    CHECK(glue.openClient(&c1, nullptr, nullptr) == kIOReturnSuccess);

    /* Signal queue 0 to fence 10 */
    CHECK(glue.signalFenceIrq(&c1, 0u, 10u, 4500u) == kIOReturnSuccess);

    /* Fast path hit: already satisfied */
    NativeAdrenoAbiWaitAnyV3 req{};
    NativeAdrenoAbiWaitAnyReplyV3 rep{};
    req.count = 1u;
    req.flags = 0u;
    req.timeout_ns = UINT64_C(50000000);
    req.queues[0] = 0u;
    req.fences[0] = 10u;
    CHECK(glue.externalMethod(&c1, NativeAdrenoMethodWaitAnyV3, &req, sizeof(req),
        &rep, sizeof(rep)) == kIOReturnSuccess);
    CHECK(rep.satisfied_mask == 1u && rep.timed_out == 0u);
    CHECK(rep.observed_ticks == 5000u); /* read_ticks callback active */

    /* Zero-timeout poll miss */
    req.fences[0] = 11u;
    req.timeout_ns = 0u;
    CHECK(glue.externalMethod(&c1, NativeAdrenoMethodWaitAnyV3, &req, sizeof(req),
        &rep, sizeof(rep)) == kIOReturnSuccess);
    CHECK(rep.satisfied_mask == 0u && rep.timed_out == 1u);

    /* Short real timeout exercising WaitBackendState sleep + timeout */
    req.timeout_ns = UINT64_C(5000000); /* 5 ms */
    CHECK(glue.externalMethod(&c1, NativeAdrenoMethodWaitAnyV3, &req, sizeof(req),
        &rep, sizeof(rep)) == kIOReturnSuccess);
    CHECK(rep.satisfied_mask == 0u && rep.timed_out == 1u);

    auto st = glue.getStats();
    CHECK(st.wait_backend_locks > 0u);
    CHECK(st.wait_backend_waits >= 1u);
    CHECK(st.wait_backend_timeouts >= 1u);
}

void test_glue_wait_any_v3_irq_wakeup_wait_any_and_wait_all()
{
    begin_case("glue_wait_any_v3_irq_wakeup_wait_any_and_wait_all");
    MockHostEnv env;
    env.hw_ticks.store(100u);
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);
    MockV2Client c1{1u};
    CHECK(glue.openClient(&c1, nullptr, nullptr) == kIOReturnSuccess);

    /* WAIT_ANY on queues {0, 1} waiting for fences {5, 7}; IRQ signals queue 1 -> 7 */
    std::atomic<bool> waiter_done{false};
    NativeAdrenoAbiWaitAnyReplyV3 any_rep{};
    IOReturn any_rc = kIOReturnError;
    std::thread t_any([&]() {
        NativeAdrenoAbiWaitAnyV3 req{};
        req.count = 2u;
        req.flags = 0u;
        req.timeout_ns = UINT64_C(5000000000);
        req.queues[0] = 0u; req.fences[0] = 5u;
        req.queues[1] = 1u; req.fences[1] = 7u;
        any_rc = glue.externalMethod(&c1, NativeAdrenoMethodWaitAnyV3, &req, sizeof(req),
            &any_rep, sizeof(any_rep));
        waiter_done.store(true, std::memory_order_release);
    });

    while (glue.getStats().wait_any.active_waiters == 0u)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    env.hw_ticks.store(8888u);
    CHECK(glue.signalFenceIrq(&c1, 1u, 7u, 8888u) == kIOReturnSuccess);
    t_any.join();
    CHECK(waiter_done.load());
    CHECK(any_rc == kIOReturnSuccess);
    CHECK(any_rep.satisfied_mask == (1u << 1u));
    CHECK(any_rep.timed_out == 0u);
    CHECK(any_rep.observed_ticks == 8888u);

    /* WAIT_ALL on queues {0, 1} waiting for fences {5, 9} */
    NativeAdrenoAbiWaitAnyReplyV3 all_rep{};
    IOReturn all_rc = kIOReturnError;
    std::thread t_all([&]() {
        NativeAdrenoAbiWaitAnyV3 req{};
        req.count = 2u;
        req.flags = 1u; /* wait_all */
        req.timeout_ns = UINT64_C(5000000000);
        req.queues[0] = 0u; req.fences[0] = 5u;
        req.queues[1] = 1u; req.fences[1] = 9u;
        all_rc = glue.externalMethod(&c1, NativeAdrenoMethodWaitAnyV3, &req, sizeof(req),
            &all_rep, sizeof(all_rep));
    });

    while (glue.getStats().wait_any.active_waiters == 0u)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(glue.signalFenceIrq(&c1, 0u, 5u, 9000u) == kIOReturnSuccess);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(glue.getStats().wait_any.active_waiters == 1u); /* still waiting for queue 1 */
    CHECK(glue.signalFenceIrq(&c1, 1u, 9u, 9500u) == kIOReturnSuccess);
    t_all.join();
    CHECK(all_rc == kIOReturnSuccess);
    CHECK(all_rep.satisfied_mask == 3u && all_rep.timed_out == 0u && all_rep.observed_ticks == 9500u);
    CHECK(glue.getStats().wait_backend_wakes >= 3u);
}

void test_glue_fault_single_channel_isolation_and_ascii_log()
{
    begin_case("glue_fault_single_channel_isolation_and_ascii_log");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false, true)) == kIOReturnSuccess);

    MockV2Client ca{1u}, cb{2u};
    uint64_t id_a = 0u, id_b = 0u;
    CHECK(glue.openClient(&ca, nullptr, nullptr, 0u, &id_a) == kIOReturnSuccess);
    CHECK(glue.openClient(&cb, nullptr, nullptr, 0u, &id_b) == kIOReturnSuccess);

    CmdBuf good;
    emit_2d_clear_stream(good);
    std::vector<uint32_t> good_w(good.words.begin(), good.words.begin() + good.cs.used_dwords);
    ca.add_bo(1u, good_w.size() * 4u, good_w);
    cb.add_bo(1u, good_w.size() * 4u, good_w);

    /* Waiter blocked on (Client A, queue 0) must wake immediately with kIOReturnAborted on fault */
    IOReturn wait_rc = kIOReturnSuccess;
    std::thread t_wait([&]() {
        NativeAdrenoAbiWaitAnyV3 req{};
        NativeAdrenoAbiWaitAnyReplyV3 rep{};
        req.count = 1u;
        req.timeout_ns = A840_KMD_WAIT_INFINITE;
        req.queues[0] = 0u;
        req.fences[0] = 100u;
        wait_rc = glue.externalMethod(&ca, NativeAdrenoMethodWaitAnyV3, &req, sizeof(req),
            &rep, sizeof(rep));
    });
    while (glue.getStats().wait_any.active_waiters == 0u)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    A840KmdFaultInput fi{};
    fi.queue_id = 0u;
    fi.fence = 99u;
    fi.ib1_pc = UINT64_C(0x10004000);
    fi.ib2_pc = UINT64_C(0x20008000);
    fi.rbbm_status = 0x80000100u;
    fi.cp_status = 0x00000042u;
    fi.smmu_fault_va = UINT64_C(0x00000004dead0000);
    fi.smmu_fsr = 0x40000004u;
    fi.fault_kind = A840_KMD_FAULT_SMMU_TRANSLATION;
    fi.timestamp_ticks = 77777u;

    A840KmdFaultRecord rec{};
    char ascii[A840_KMD_FAULT_SNAPSHOT_LINE_MAX];
    size_t ascii_len = 0u;
    CHECK(glue.reportHardwareFault(&ca, fi, &rec, ascii, sizeof(ascii), &ascii_len) == kIOReturnSuccess);
    t_wait.join();
    CHECK(wait_rc == kIOReturnAborted);
    CHECK(rec.client_id == id_a && rec.queue_id == 0u && rec.fence == 99u);
    CHECK(ascii_len > 100u && std::strlen(ascii) == ascii_len);
    CHECK(env.fault_lines.size() == 1u && env.fault_lines[0] == std::string(ascii));
    CHECK(env.fault_lines[0].find("ib1_pc=0x0000000010004000") != std::string::npos);
    CHECK(env.fault_lines[0].find("smmu_va=0x00000004dead0000") != std::string::npos);
    CHECK(env.fault_lines[0].find("ticks=77777") != std::string::npos);

    /* (Client A, queue 0) is poisoned; (Client A, queue 1) and (Client B, queue 0) stay healthy! */
    CHECK(glue.checkSubmitChannel(&ca, 0u) == kIOReturnAborted);
    CHECK(glue.checkSubmitChannel(&ca, 1u) == kIOReturnSuccess);
    CHECK(glue.checkSubmitChannel(&cb, 0u) == kIOReturnSuccess);

    NativeAdrenoV2SubmitWire sub{};
    NativeAdrenoV2SubmitReplyWire srep{};
    sub.buffer_count = 1u;
    sub.buffers[0] = {1u, UINT64_C(0x10000), good_w.size() * 4u};

    sub.queue = 0u;
    CHECK(glue.externalMethod(&ca, NativeAdrenoV2MethodSubmit, &sub, sizeof(sub), &srep, sizeof(srep)) == kIOReturnAborted);
    sub.queue = 1u;
    CHECK(glue.externalMethod(&ca, NativeAdrenoV2MethodSubmit, &sub, sizeof(sub), &srep, sizeof(srep)) == kIOReturnSuccess);
    sub.queue = 0u;
    CHECK(glue.externalMethod(&cb, NativeAdrenoV2MethodSubmit, &sub, sizeof(sub), &srep, sizeof(srep)) == kIOReturnSuccess);
}

void test_glue_fault_ring_retained_after_client_close()
{
    begin_case("glue_fault_ring_retained_after_client_close");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);
    MockV2Client c1{1u};
    CHECK(glue.openClient(&c1, nullptr, nullptr) == kIOReturnSuccess);

    A840KmdFaultInput fi{};
    fi.queue_id = 3u;
    fi.fence = 123u;
    fi.fault_kind = A840_KMD_FAULT_GPU_HANG;
    fi.timestamp_ticks = 4444u;
    CHECK(glue.reportHardwareFault(&c1, fi) == kIOReturnSuccess);

    A840KmdClientCloseSummary sum{};
    CHECK(glue.closeClient(&c1, &sum) == kIOReturnSuccess);
    CHECK(sum.cleared_poisoned_channels == 1u);

    /* Post-mortem fault ring snapshot stays accessible after close. */
    A840KmdFaultRecord latest{};
    char ascii[A840_KMD_FAULT_SNAPSHOT_LINE_MAX];
    size_t len = 0u;
    CHECK(glue.getLatestFaultSnapshot(&latest, ascii, sizeof(ascii), &len) == kIOReturnSuccess);
    CHECK(latest.queue_id == 3u && latest.fence == 123u && len > 80u);
    CHECK(std::string(ascii).find("kind=gpu_hang") != std::string::npos);

    /* Re-opening client starts healthy. */
    CHECK(glue.openClient(&c1, nullptr, nullptr) == kIOReturnSuccess);
    CHECK(glue.checkSubmitChannel(&c1, 3u) == kIOReturnSuccess);
}

void test_glue_close_drains_blocking_waiter_cleanly()
{
    begin_case("glue_close_drains_blocking_waiter_cleanly");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);
    MockV2Client c1{1u};
    CHECK(glue.openClient(&c1, nullptr, nullptr) == kIOReturnSuccess);

    IOReturn wait_rc = kIOReturnSuccess;
    std::thread waiter([&]() {
        NativeAdrenoAbiWaitAnyV3 req{};
        NativeAdrenoAbiWaitAnyReplyV3 rep{};
        req.count = 1u;
        req.timeout_ns = A840_KMD_WAIT_INFINITE;
        req.queues[0] = 0u;
        req.fences[0] = 999u;
        wait_rc = glue.externalMethod(&c1, NativeAdrenoMethodWaitAnyV3, &req, sizeof(req),
            &rep, sizeof(rep));
    });

    while (glue.getStats().wait_any.active_waiters == 0u)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    CHECK(glue.closeClient(&c1) == kIOReturnSuccess);
    waiter.join();
    CHECK(wait_rc == kIOReturnNotReady);
    CHECK(glue.getStats().open_clients == 0u);
    CHECK(glue.getStats().zero_page.backing_allocated == 0u);
}

void test_glue_duplicate_open_and_unknown_client_errors()
{
    begin_case("glue_duplicate_open_and_unknown_client_errors");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);

    MockV2Client c1{1u}, unknown{99u};
    CHECK(glue.openClient(nullptr, nullptr, nullptr) == kIOReturnBadArgument);
    CHECK(glue.openClient(&c1, nullptr, nullptr) == kIOReturnSuccess);
    CHECK(glue.openClient(&c1, nullptr, nullptr) == kIOReturnBusy);

    CHECK(glue.closeClient(&unknown) == kIOReturnNotFound);
    CHECK(glue.externalMethod(&unknown, NativeAdrenoV2MethodQuery, nullptr, 0u, nullptr, 0u) == kIOReturnNotFound);
    CHECK(glue.signalFenceIrq(&unknown, 0u, 1u, 1u) == kIOReturnNotFound);
    CHECK(glue.checkSubmitChannel(&unknown, 0u) == kIOReturnNotFound);

    uint64_t cid = 0u;
    CHECK(glue.getClientId(&unknown, &cid) == kIOReturnNotFound);
    CHECK(glue.getClientId(&c1, nullptr) == kIOReturnBadArgument);
    CHECK(glue.getClientId(&c1, &cid) == kIOReturnSuccess && cid != 0u);
}

void test_glue_destroy_auto_closes_remaining_clients()
{
    begin_case("glue_destroy_auto_closes_remaining_clients");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(true)) == kIOReturnSuccess);
    MockV2Client c[4]{};
    for (int i = 0; i < 4; ++i)
        CHECK(glue.openClient(&c[i], nullptr, nullptr) == kIOReturnSuccess);
    CHECK(env.smmu_allocs == 1u && env.smmu_maps == 4u);
    glue.destroy();
    CHECK(!glue.initialized());
    CHECK(env.smmu_unmaps == 4u && env.smmu_frees == 1u);
}

/* ======================== GROUP 4: CONCURRENCY & TSAN STRESS (7 cases) ======================== */

void test_tsan_multi_client_wait_any_irq_storm()
{
    begin_case("tsan_multi_client_wait_any_irq_storm");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);

    constexpr int kClients = 4;
    constexpr int kSteps = 16;
    MockV2Client clients[kClients]{};
    for (int i = 0; i < kClients; ++i)
        CHECK(glue.openClient(&clients[i], nullptr, nullptr) == kIOReturnSuccess);

    std::vector<std::thread> waiters;
    for (int i = 0; i < kClients; ++i) {
        waiters.emplace_back([&, i]() {
            for (uint64_t f = 1u; f <= kSteps; ++f) {
                NativeAdrenoAbiWaitAnyV3 req{};
                NativeAdrenoAbiWaitAnyReplyV3 rep{};
                req.count = 2u;
                req.flags = (f % 2u == 0u) ? 1u : 0u; /* alternate WAIT_ANY and WAIT_ALL */
                req.timeout_ns = UINT64_C(5000000000);
                req.queues[0] = 0u; req.fences[0] = f;
                req.queues[1] = 1u; req.fences[1] = f;
                const IOReturn rc = glue.externalMethod(&clients[i],
                    NativeAdrenoMethodWaitAnyV3, &req, sizeof(req), &rep, sizeof(rep));
                CHECK(rc == kIOReturnSuccess);
                CHECK(rep.timed_out == 0u);
                if (req.flags == 1u)
                    CHECK(rep.satisfied_mask == 3u);
                else
                    CHECK(rep.satisfied_mask != 0u);
            }
        });
    }

    std::thread irq0([&]() {
        for (uint64_t f = 1u; f <= kSteps; ++f) {
            for (int i = 0; i < kClients; ++i) {
                const uint64_t t = env.hw_ticks.fetch_add(10u, std::memory_order_relaxed) + 10u;
                CHECK(glue.signalFenceIrq(&clients[i], 0u, f, t) == kIOReturnSuccess);
            }
            std::this_thread::yield();
        }
    });
    std::thread irq1([&]() {
        for (uint64_t f = 1u; f <= kSteps; ++f) {
            for (int i = 0; i < kClients; ++i) {
                const uint64_t t = env.hw_ticks.fetch_add(10u, std::memory_order_relaxed) + 10u;
                CHECK(glue.signalFenceIrq(&clients[i], 1u, f, t) == kIOReturnSuccess);
            }
            std::this_thread::yield();
        }
    });

    irq0.join();
    irq1.join();
    for (auto &t : waiters)
        t.join();
    for (int i = 0; i < kClients; ++i)
        CHECK(glue.closeClient(&clients[i]) == kIOReturnSuccess);
    CHECK(glue.getStats().irq_signals == static_cast<uint64_t>(kClients * kSteps * 2));
}

void test_tsan_concurrent_submit_ib_audit_good_and_blocked()
{
    begin_case("tsan_concurrent_submit_ib_audit_good_and_blocked");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false, true)) == kIOReturnSuccess);

    CmdBuf good, bad_reg, bad_par;
    const auto p = make_ctx_init_params();
    CHECK(a840_emit_context_init(&good.cs, &p) == A840_EMIT_OK);
    emit_2d_clear_stream(good);
    const uint32_t v = 1u;
    CHECK(a840_cs_emit_pkt4(&bad_reg.cs, A840_REG_RB_DBG_ECO_CNTL, &v, 1u) == A840_EMIT_OK);
    CHECK(a840_cs_emit_pkt4(&bad_par.cs, A840_REG_GRAS_A2D_BLT_CNTL, &v, 1u) == A840_EMIT_OK);
    bad_par.words[0] ^= (UINT32_C(1) << 7u);

    std::vector<uint32_t> gw(good.words.begin(), good.words.begin() + good.cs.used_dwords);
    std::vector<uint32_t> bw(bad_reg.words.begin(), bad_reg.words.begin() + bad_reg.cs.used_dwords);
    std::vector<uint32_t> pw(bad_par.words.begin(), bad_par.words.begin() + bad_par.cs.used_dwords);

    constexpr int kThreads = 4;
    constexpr int kIters = 15;
    MockV2Client clients[kThreads]{};
    for (int i = 0; i < kThreads; ++i) {
        clients[i].add_bo(1u, gw.size() * 4u, gw);
        clients[i].add_bo(2u, bw.size() * 4u, bw);
        clients[i].add_bo(3u, pw.size() * 4u, pw);
        CHECK(glue.openClient(&clients[i], nullptr, nullptr) == kIOReturnSuccess);
    }

    std::vector<std::thread> workers;
    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&, i]() {
            for (int k = 0; k < kIters; ++k) {
                NativeAdrenoV2SubmitWire req{};
                NativeAdrenoV2SubmitReplyWire rep{};
                req.buffer_count = 1u;
                req.queue = 0u;
                req.buffers[0] = {1u, UINT64_C(0x10000), gw.size() * 4u};
                CHECK(glue.externalMethod(&clients[i], NativeAdrenoV2MethodSubmit, &req, sizeof(req),
                    &rep, sizeof(rep)) == kIOReturnSuccess);

                req.buffers[0] = {2u, UINT64_C(0x20000), bw.size() * 4u};
                CHECK(glue.externalMethod(&clients[i], NativeAdrenoV2MethodSubmit, &req, sizeof(req),
                    &rep, sizeof(rep)) == kIOReturnNotPermitted);

                req.buffers[0] = {3u, UINT64_C(0x30000), pw.size() * 4u};
                CHECK(glue.externalMethod(&clients[i], NativeAdrenoV2MethodSubmit, &req, sizeof(req),
                    &rep, sizeof(rep)) == kIOReturnBadArgument);
            }
        });
    }
    for (auto &t : workers)
        t.join();

    auto st = glue.getStats();
    CHECK(st.ib_validated_ok == static_cast<uint64_t>(kThreads * kIters));
    CHECK(st.ib_rejected_total == static_cast<uint64_t>(kThreads * kIters * 2));
    CHECK(st.ib_rejected_blocked_reg == static_cast<uint64_t>(kThreads * kIters));
}

void test_tsan_concurrent_fault_isolation_vs_healthy_clients()
{
    begin_case("tsan_concurrent_fault_isolation_vs_healthy_clients");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false, true)) == kIOReturnSuccess);

    MockV2Client c_fault{1u}, c_healthy{2u};
    CmdBuf good;
    emit_3d_draw_stream(good);
    std::vector<uint32_t> gw(good.words.begin(), good.words.begin() + good.cs.used_dwords);
    c_fault.add_bo(1u, gw.size() * 4u, gw);
    c_healthy.add_bo(1u, gw.size() * 4u, gw);

    CHECK(glue.openClient(&c_fault, nullptr, nullptr) == kIOReturnSuccess);
    CHECK(glue.openClient(&c_healthy, nullptr, nullptr) == kIOReturnSuccess);

    std::atomic<bool> stop{false};
    std::thread fault_thread([&]() {
        for (int k = 0; k < 20; ++k) {
            A840KmdFaultInput fi{};
            fi.queue_id = 0u;
            fi.fence = static_cast<uint64_t>(k + 1);
            fi.fault_kind = A840_KMD_FAULT_GPU_HANG;
            fi.timestamp_ticks = static_cast<uint64_t>(1000 + k);
            char line[A840_KMD_FAULT_SNAPSHOT_LINE_MAX];
            CHECK(glue.reportHardwareFault(&c_fault, fi, nullptr, line, sizeof(line)) == kIOReturnSuccess);
            CHECK(std::strstr(line, "kind=gpu_hang") != nullptr);
            std::this_thread::yield();
        }
        stop.store(true, std::memory_order_release);
    });

    std::thread healthy_submit([&]() {
        for (uint64_t k = 1u; k <= 30u; ++k) {
            NativeAdrenoV2SubmitWire req{};
            NativeAdrenoV2SubmitReplyWire rep{};
            req.buffer_count = 1u;
            req.queue = 0u;
            req.buffers[0] = {1u, UINT64_C(0x10000), gw.size() * 4u};
            CHECK(glue.externalMethod(&c_healthy, NativeAdrenoV2MethodSubmit, &req, sizeof(req),
                &rep, sizeof(rep)) == kIOReturnSuccess);
            CHECK(glue.signalFenceIrq(&c_healthy, 0u, rep.fence, 5000u + k) == kIOReturnSuccess);

            /* Client A's OTHER queue (queue 1) also stays completely healthy! */
            req.queue = 1u;
            CHECK(glue.externalMethod(&c_fault, NativeAdrenoV2MethodSubmit, &req, sizeof(req),
                &rep, sizeof(rep)) == kIOReturnSuccess);
        }
    });

    fault_thread.join();
    healthy_submit.join();
    CHECK(glue.checkSubmitChannel(&c_fault, 0u) == kIOReturnAborted);
    CHECK(glue.checkSubmitChannel(&c_fault, 1u) == kIOReturnSuccess);
    CHECK(glue.checkSubmitChannel(&c_healthy, 0u) == kIOReturnSuccess);
    CHECK(glue.getStats().faults_reported == 20u);
}

void test_tsan_concurrent_open_close_churn_with_zero_page()
{
    begin_case("tsan_concurrent_open_close_churn_with_zero_page");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);

    constexpr int kWorkers = 4;
    std::vector<std::thread> threads;
    for (int w = 0; w < kWorkers; ++w) {
        threads.emplace_back([&, w]() {
            MockV2Client local_c{static_cast<uint32_t>(w + 1)};
            local_c.add_bo(77u, 65536u);
            for (int iter = 0; iter < 15; ++iter) {
                CHECK(glue.openClient(&local_c, nullptr, nullptr) == kIOReturnSuccess);
                NativeAdrenoAbiZeroPageReplyV3 zp{};
                CHECK(glue.externalMethod(&local_c, NativeAdrenoMethodGetZeroPageV3,
                    nullptr, 0u, &zp, sizeof(zp)) == kIOReturnSuccess);
                CHECK(zp.gpu_va == kDefaultZeroPageVa && zp.bytes == A840_KMD_ZERO_PAGE_BYTES);

                NativeAdrenoV2ParamsWire q{};
                CHECK(glue.externalMethod(&local_c, NativeAdrenoV2MethodQuery,
                    nullptr, 0u, &q, sizeof(q)) == kIOReturnSuccess);
                CHECK(q.abi_version == NativeAdrenoAbiVersionV3);

                NativeAdrenoAbiSurfaceMetaV3 sm{}, sm_out{};
                sm.bo_handle = 77u;
                sm.op = 1u;
                sm.ubwc_enabled = 1u;
                sm.ubwc_version = 6u;
                sm.highest_bank_bit = 16u;
                sm.swizzle_levels = 6u;
                sm.macrotile_mode = 1u;
                sm.meta_offset = 4096u;
                sm.meta_pitch = 64u;
                CHECK(glue.externalMethod(&local_c, NativeAdrenoMethodSurfaceMetaV3,
                    &sm, sizeof(sm), &sm_out, sizeof(sm_out)) == kIOReturnSuccess);

                CHECK(glue.closeClient(&local_c) == kIOReturnSuccess);
            }
        });
    }
    for (auto &t : threads)
        t.join();
    auto st = glue.getStats();
    CHECK(st.open_clients == 0u);
    CHECK(st.zero_page.active_clients == 0u);
    CHECK(st.zero_page.backing_allocated == 0u);
    CHECK(st.zero_page.total_allocs == st.zero_page.total_frees);
}

void test_tsan_concurrent_close_vs_blocking_wait_any_race()
{
    begin_case("tsan_concurrent_close_vs_blocking_wait_any_race");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);

    for (int round = 0; round < 12; ++round) {
        MockV2Client c{static_cast<uint32_t>(round + 1)};
        CHECK(glue.openClient(&c, nullptr, nullptr) == kIOReturnSuccess);

        std::atomic<bool> started{false};
        IOReturn wait_rc = kIOReturnError;
        std::thread waiter([&]() {
            started.store(true, std::memory_order_release);
            NativeAdrenoAbiWaitAnyV3 req{};
            NativeAdrenoAbiWaitAnyReplyV3 rep{};
            req.count = 1u;
            req.timeout_ns = UINT64_C(2000000000);
            req.queues[0] = 0u;
            req.fences[0] = 500u;
            wait_rc = glue.externalMethod(&c, NativeAdrenoMethodWaitAnyV3, &req, sizeof(req),
                &rep, sizeof(rep));
        });

        while (!started.load(std::memory_order_acquire))
            std::this_thread::yield();
        if (round % 2 == 0) {
            while (glue.getStats().wait_any.active_waiters == 0u)
                std::this_thread::yield();
        }
        CHECK(glue.closeClient(&c) == kIOReturnSuccess);
        waiter.join();
        CHECK(wait_rc == kIOReturnNotReady || wait_rc == kIOReturnNotFound);
    }
    CHECK(glue.getStats().open_clients == 0u);
}

void test_tsan_concurrent_writer_promise_and_surface_meta_churn()
{
    begin_case("tsan_concurrent_writer_promise_and_surface_meta_churn");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);

    NativeAdrenoAbiSharedPackReplyV3 pack{};
    pack.gpu_va = UINT64_C(0x500000000);
    pack.cpu_va = UINT64_C(0x600000000);
    pack.bytes = 65536u;
    pack.actual_stack_key = UINT64_C(0xabcdef1234567890);
    CHECK(glue.configureSharedPack(pack, 1u, 1u) == kIOReturnSuccess);

    constexpr int kClients = 4;
    MockV2Client clients[kClients]{};
    for (int i = 0; i < kClients; ++i) {
        clients[i].add_bo(10u + static_cast<uint64_t>(i), 262144u);
        CHECK(glue.openClient(&clients[i], nullptr, nullptr) == kIOReturnSuccess);
    }

    std::vector<std::thread> workers;
    for (int i = 0; i < kClients; ++i) {
        workers.emplace_back([&, i]() {
            for (int k = 0; k < 20; ++k) {
                NativeAdrenoAbiSharedPackReqV3 preq{pack.actual_stack_key, 1u, 0u};
                NativeAdrenoAbiSharedPackReplyV3 prep{};
                CHECK(glue.externalMethod(&clients[i], NativeAdrenoMethodMapSharedPackV3,
                    &preq, sizeof(preq), &prep, sizeof(prep)) == kIOReturnSuccess);

                NativeAdrenoAbiWriterPromiseV3 wreq{};
                NativeAdrenoAbiWriterPromiseReplyV3 wrep{};
                wreq.op = NativeAdrenoPromiseMake;
                wreq.surface_count = 1u;
                wreq.queue = 0u;
                wreq.surface_ids[0] = static_cast<uint64_t>(i * 100 + k + 1);
                CHECK(glue.externalMethod(&clients[i], NativeAdrenoMethodWriterPromiseV3,
                    &wreq, sizeof(wreq), &wrep, sizeof(wrep)) == kIOReturnSuccess);

                wreq.op = (k % 2 == 0) ? NativeAdrenoPromiseFulfill : NativeAdrenoPromiseCancel;
                wreq.promise_token = wrep.promise_token;
                wreq.fence = (k % 2 == 0) ? static_cast<uint64_t>(k + 1) : 0u;
                CHECK(glue.externalMethod(&clients[i], NativeAdrenoMethodWriterPromiseV3,
                    &wreq, sizeof(wreq), &wrep, sizeof(wrep)) == kIOReturnSuccess);

                NativeAdrenoAbiSurfaceMetaV3 sreq{}, srep{};
                sreq.bo_handle = 10u + static_cast<uint64_t>(i);
                sreq.op = 1u;
                sreq.ubwc_enabled = 1u;
                sreq.ubwc_version = 6u;
                sreq.highest_bank_bit = 16u;
                sreq.swizzle_levels = 6u;
                sreq.macrotile_mode = 1u;
                sreq.meta_offset = 4096u;
                sreq.meta_pitch = 128u;
                CHECK(glue.externalMethod(&clients[i], NativeAdrenoMethodSurfaceMetaV3,
                    &sreq, sizeof(sreq), &srep, sizeof(srep)) == kIOReturnSuccess);

                (void)glue.getStats();
            }
        });
    }
    for (auto &t : workers)
        t.join();
    for (int i = 0; i < kClients; ++i)
        CHECK(glue.closeClient(&clients[i]) == kIOReturnSuccess);
}

void test_tsan_full_lifecycle_stress_rounds(unsigned stress_rounds)
{
    begin_case("tsan_full_lifecycle_stress_rounds");
    MockHostEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false, true)) == kIOReturnSuccess);

    CmdBuf good, bad;
    const auto p = make_ctx_init_params();
    CHECK(a840_emit_context_init(&good.cs, &p) == A840_EMIT_OK);
    emit_2d_clear_stream(good);
    emit_compute_dispatch_stream(good);
    emit_3d_draw_stream(good);
    const uint32_t v = 0x55u;
    CHECK(a840_cs_emit_pkt4(&bad.cs, A840_REG_VPC_LB_MODE_CNTL, &v, 1u) == A840_EMIT_OK);

    std::vector<uint32_t> gw(good.words.begin(), good.words.begin() + good.cs.used_dwords);
    std::vector<uint32_t> bw(bad.words.begin(), bad.words.begin() + bad.cs.used_dwords);

    for (unsigned r = 0u; r < stress_rounds; ++r) {
        constexpr int kClients = 4;
        MockV2Client clients[kClients]{};
        for (int i = 0; i < kClients; ++i) {
            clients[i].add_bo(1u, gw.size() * 4u, gw);
            clients[i].add_bo(2u, bw.size() * 4u, bw);
            CHECK(glue.openClient(&clients[i], nullptr, nullptr) == kIOReturnSuccess);
        }

        std::vector<std::thread> threads;
        for (int i = 0; i < kClients; ++i) {
            threads.emplace_back([&, i, r]() {
                /* 1. v2 Query passthrough with v3 caps */
                NativeAdrenoV2ParamsWire qp{};
                CHECK(glue.externalMethod(&clients[i], NativeAdrenoV2MethodQuery,
                    nullptr, 0u, &qp, sizeof(qp)) == kIOReturnSuccess);
                CHECK(qp.abi_version == NativeAdrenoAbiVersionV3);

                /* 2. GetZeroPageV3 */
                NativeAdrenoAbiZeroPageReplyV3 zp{};
                CHECK(glue.externalMethod(&clients[i], NativeAdrenoMethodGetZeroPageV3,
                    nullptr, 0u, &zp, sizeof(zp)) == kIOReturnSuccess);
                CHECK(zp.gpu_va == kDefaultZeroPageVa);

                /* 3. Valid Submit + blocked-reg IB rejection */
                NativeAdrenoV2SubmitWire sub{};
                NativeAdrenoV2SubmitReplyWire srep{};
                sub.buffer_count = 1u;
                sub.queue = 0u;
                sub.buffers[0] = {1u, UINT64_C(0x10000), gw.size() * 4u};
                CHECK(glue.externalMethod(&clients[i], NativeAdrenoV2MethodSubmit,
                    &sub, sizeof(sub), &srep, sizeof(srep)) == kIOReturnSuccess);

                sub.buffers[0] = {2u, UINT64_C(0x20000), bw.size() * 4u};
                CHECK(glue.externalMethod(&clients[i], NativeAdrenoV2MethodSubmit,
                    &sub, sizeof(sub), &srep, sizeof(srep)) == kIOReturnNotPermitted);

                /* 4. Blocking WaitAnyV3 woken by IRQ thread below */
                NativeAdrenoAbiWaitAnyV3 wreq{};
                NativeAdrenoAbiWaitAnyReplyV3 wrep{};
                wreq.count = 1u;
                wreq.timeout_ns = UINT64_C(5000000000);
                wreq.queues[0] = 0u;
                wreq.fences[0] = 1u;
                const IOReturn wrc = glue.externalMethod(&clients[i],
                    NativeAdrenoMethodWaitAnyV3, &wreq, sizeof(wreq), &wrep, sizeof(wrep));
                if (i == 0) {
                    /* Client 0 may be either IRQ-signaled or fault-poisoned on queue 0 */
                    CHECK(wrc == kIOReturnSuccess || wrc == kIOReturnAborted);
                } else {
                    CHECK(wrc == kIOReturnSuccess && wrep.satisfied_mask == 1u && wrep.timed_out == 0u);
                }

                /* 5. Client 0 queue 0 fault isolation + ASCII formatting */
                if (i == 0) {
                    A840KmdFaultInput fi{};
                    fi.queue_id = 0u;
                    fi.fence = static_cast<uint64_t>(r + 1u);
                    fi.ib1_pc = UINT64_C(0x10000000) + r * 4u;
                    fi.fault_kind = A840_KMD_FAULT_GPU_HANG;
                    fi.timestamp_ticks = 10000u + r;
                    char ascii[A840_KMD_FAULT_SNAPSHOT_LINE_MAX];
                    size_t alen = 0u;
                    CHECK(glue.reportHardwareFault(&clients[0], fi, nullptr,
                        ascii, sizeof(ascii), &alen) == kIOReturnSuccess);
                    CHECK(alen > 80u && std::strstr(ascii, "a840_fault ") == ascii);
                }
            });
        }

        std::thread irq([&]() {
            for (int i = 0; i < kClients; ++i) {
                const uint64_t t = env.hw_ticks.fetch_add(5u, std::memory_order_relaxed) + 5u;
                (void)glue.signalFenceIrq(&clients[i], 0u, 1u, t);
            }
        });

        irq.join();
        for (auto &t : threads)
            t.join();

        for (int i = 0; i < kClients; ++i) {
            A840KmdClientCloseSummary sum{};
            CHECK(glue.closeClient(&clients[i], &sum) == kIOReturnSuccess);
            CHECK(sum.zero_page_unbound == 1u);
            if (i == 0)
                CHECK(sum.cleared_poisoned_channels == 1u);
        }
    }

    auto st = glue.getStats();
    CHECK(st.open_clients == 0u);
    CHECK(st.zero_page.backing_allocated == 0u);
    CHECK(st.faults_reported == stress_rounds);
    CHECK(st.ib_rejected_blocked_reg == static_cast<uint64_t>(stress_rounds * 4u));
}

} /* namespace */

int main(int argc, char **argv)
{
    unsigned stress_rounds = 2u;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tsan-stress") == 0 && i + 1 < argc) {
            const long v = std::strtol(argv[++i], nullptr, 10);
            if (v > 0 && v <= 200)
                stress_rounds = static_cast<unsigned>(v);
        } else if (std::strcmp(argv[i], "--require-real-emitters") == 0) {
            g_require_real_emitters = true;
        }
    }

    /* Group 1: PM4 IB Pre-Submission Validator & Gen8 Guardrail (18 cases) */
    test_ib_null_and_empty_args();
    test_ib_valid_context_init_full();
    test_ib_valid_bin_preambles_br_and_bv();
    test_ib_valid_2d_clear_stream();
    test_ib_valid_compute_dispatch_stream();
    test_ib_valid_3d_draw_stream();
    test_ib_valid_combined_frame_ib();
    test_ib_reject_unknown_packet_types();
    test_ib_reject_type4_bad_count_parity();
    test_ib_reject_type4_bad_reg_parity();
    test_ib_reject_type7_bad_count_and_opcode_parity();
    test_ib_reject_bad_headers_reserved_and_zero_count();
    test_ib_reject_truncated_type4_and_type7();
    test_ib_guardrail_all_6_blocked_regs_single();
    test_ib_guardrail_coalesced_multi_reg_type4();
    test_ib_guardrail_adjacent_non_blocked_regs_pass();
    test_ib_validate_bytes_wrapper_pass_and_fail();
    test_ib_diag_ascii_formatter_and_truncation();

    /* Group 2: Allocation-Free Fault Snapshot Formatter (7 cases) */
    test_fault_fmt_all_fields_golden();
    test_fault_fmt_all_fault_kinds();
    test_fault_fmt_zero_and_max_u64_boundaries();
    test_fault_fmt_truncation_sweep();
    test_fault_fmt_null_and_zero_cap();
    test_fault_fmt_aliased_buffer();
    test_fault_fmt_determinism();

    /* Group 3: NativeAdrenoV3Glue Functional & Negative (20 cases) */
    test_glue_uninit_guards_and_double_init();
    test_glue_open_close_multi_client_zero_page_sharing();
    test_glue_custom_smmu_write_perm_defense_and_failure_rollback();
    test_glue_v2_passthrough_query_cap_augmentation();
    test_glue_v2_passthrough_methods_2_to_18();
    test_glue_unsupported_and_envelope_ceiling();
    test_glue_submit_method5_with_ib_audit_pass();
    test_glue_submit_method5_intercepts_blocked_reg_and_bad_parity();
    test_glue_submit_batch_method19_append_audit_and_abort();
    test_glue_submit_with_audit_direct_api_and_disable_flag();
    test_glue_method23_shared_pack_configure_and_stack_key();
    test_glue_method24_writer_promise_lifecycle_and_close_cancel();
    test_glue_method25_surface_meta_bo_lookup();
    test_glue_wait_any_v3_fast_path_and_timeout_on_wait_backend();
    test_glue_wait_any_v3_irq_wakeup_wait_any_and_wait_all();
    test_glue_fault_single_channel_isolation_and_ascii_log();
    test_glue_fault_ring_retained_after_client_close();
    test_glue_close_drains_blocking_waiter_cleanly();
    test_glue_duplicate_open_and_unknown_client_errors();
    test_glue_destroy_auto_closes_remaining_clients();

    /* Group 4: Multi-Threaded Concurrency & TSan Stress (7 cases) */
    test_tsan_multi_client_wait_any_irq_storm();
    test_tsan_concurrent_submit_ib_audit_good_and_blocked();
    test_tsan_concurrent_fault_isolation_vs_healthy_clients();
    test_tsan_concurrent_open_close_churn_with_zero_page();
    test_tsan_concurrent_close_vs_blocking_wait_any_race();
    test_tsan_concurrent_writer_promise_and_surface_meta_churn();
    test_tsan_full_lifecycle_stress_rounds(stress_rounds);

    if (g_require_real_emitters)
        CHECK(g_real_emitters_invoked == 7u);

    std::printf("PASS test_xnu_v3_glue: %u cases, %llu checks, %u stress rounds, real_emitters=0x%x, 0 failures\n",
        g_cases,
        static_cast<unsigned long long>(g_checks.load()),
        stress_rounds,
        g_real_emitters_invoked);
    return 0;
}
