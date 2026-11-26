/* SPDX-License-Identifier: MIT */
#include "a840_context_init.h"
#include "a840_abi_v3_host.h"
#include "gen/Adreno840Regs.h"
#include "chip_facts_a840.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

/* The required standalone GCC/Clang link includes read-only a840_cs.c.
 * The Wave runner deliberately compiles the Adreno840 suite on its own.
 * Weak test-only references permit that independent suite; the standalone
 * gates require and execute the actual reference cross-comparison.
 */
extern "C" {
A840EmitStatus a840_cs_init(A840CmdStream *, uint32_t *, size_t, A840Reloc *, size_t) __attribute__((weak));
A840EmitStatus a840_cs_emit_pkt4(A840CmdStream *, uint32_t, const uint32_t *, uint32_t) __attribute__((weak));
A840EmitStatus a840_cs_emit_pkt7(A840CmdStream *, uint32_t, const uint32_t *, uint32_t) __attribute__((weak));
A840EmitStatus a840_pm4_type4_header(uint32_t, uint32_t, uint32_t *) __attribute__((weak));
A840EmitStatus a840_pm4_type7_header(uint32_t, uint32_t, uint32_t *) __attribute__((weak));
A840EmitStatus a840_pm4_decode(const uint32_t *, size_t, A840PM4Packet *, size_t, size_t *) __attribute__((weak));
}

namespace {
std::size_t checks = 0, cases = 0, cross_packets = 0;
const char *active_case = "start";
void begin(const char *name) { active_case = name; ++cases; }
void check(bool condition, const char *expression, int line)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL [%s] line %d: %s\n", active_case, line, expression);
        std::exit(EXIT_FAILURE);
    }
}
#define CASE(name) begin(name)
#define CHECK(expression) check((expression), #expression, __LINE__)

template<class T> bool same(const T &a, const T &b)
{ return std::memcmp(&a, &b, sizeof(T)) == 0; }

constexpr std::size_t WORDS = 640;
struct Stream {
    std::array<std::uint32_t, WORDS> words{};
    std::array<A840Reloc, 32> relocs{};
    A840CmdStream cs{};
    explicit Stream(std::size_t word_capacity = WORDS, std::size_t reloc_capacity = 32)
    {
        words.fill(0xdeadbeefu);
        std::memset(relocs.data(), 0xa5, sizeof(relocs));
        cs = {words.data(), word_capacity, 0, relocs.data(), reloc_capacity, 0};
    }
};
A840ContextInitParams parameters()
{
    A840ContextInitParams p{};
    p.border_color_gpu_va = 0x100000;
    p.border_color_bo = 77;
    p.bin_preamble_br_va = 0x200000;
    p.bin_preamble_bv_va = 0x210000;
    p.preempt_preamble_va = 0x220000;
    p.preempt_postamble_va = 0x230000;
    p.bin_preamble_br_dwords = 124;
    p.bin_preamble_bv_dwords = 118;
    p.preempt_preamble_dwords = 8;
    p.preempt_postamble_dwords = 4;
    p.vis_stream_count = 3;
    p.enable_early_preamble_reset = 1;
    return p;
}
struct Packet {
    std::size_t start;
    std::uint32_t type, id, count;
};
std::uint32_t parity(std::uint32_t value)
{
    std::uint32_t p = 1;
    while (value) { p ^= 1; value &= value - 1; }
    return p;
}
std::vector<Packet> decode(const std::uint32_t *words, std::size_t count)
{
    std::vector<Packet> result;
    const std::array<std::uint32_t, 6> forbidden = {{
        A840_REG_RB_DBG_ECO_CNTL, A840_REG_RB_RBP_CNTL, A840_REG_RB_GC_GMEM_PROTECT,
        A840_REG_SP_NC_MODE_CNTL_2, A840_REG_VPC_LB_MODE_CNTL,
        A840_REG_PC_CONTEXT_SWITCH_GFX_PREEMPTION_MODE
    }};
    for (std::size_t at = 0; at < count;) {
        const auto header = words[at];
        const auto type = header >> 28;
        CHECK(type == 4 || type == 7);
        const auto n = header & (type == 4 ? 127u : 16383u);
        const auto id = (header >> (type == 4 ? 8 : 16)) & (type == 4 ? 262143u : 127u);
        const auto rebuilt = type == 4 ?
            (4u << 28) | n | (parity(n) << 7) | (id << 8) | (parity(id) << 27) :
            (7u << 28) | n | (parity(n) << 15) | (id << 16) | (parity(id) << 23);
        CHECK(header == rebuilt);
        CHECK(n <= count - at - 1);
        if (type == 4) {
            CHECK(n != 0);
            for (std::uint32_t i = 0; i < n; ++i) {
                CHECK(a840_reg_is_umd_blocked_on_a8xx(id + i) == 0);
                CHECK(std::find(forbidden.begin(), forbidden.end(), id + i) == forbidden.end());
            }
        }
        result.push_back({at, type, id, n});
        at += n + 1;
    }
    return result;
}
std::uint32_t last_reg(const Stream &stream, std::uint32_t reg)
{
    std::uint32_t value = UINT32_MAX;
    for (const auto &p : decode(stream.words.data(), stream.cs.used_dwords))
        if (p.type == 4 && reg >= p.id && reg - p.id < p.count)
            value = stream.words[p.start + 1 + reg - p.id];
    return value;
}
bool has_reg(const Stream &stream, std::uint32_t reg)
{
    for (const auto &p : decode(stream.words.data(), stream.cs.used_dwords))
        if (p.type == 4 && reg >= p.id && reg - p.id < p.count)
            return true;
    return false;
}
void tail_unchanged(const Stream &s, std::size_t start)
{
    for (std::size_t i = start; i < s.words.size(); ++i)
        CHECK(s.words[i] == 0xdeadbeefu);
}
void reject_context(Stream &s, const A840ContextInitParams &p)
{
    const auto words = s.words;
    const auto relocs = s.relocs;
    const auto cs = s.cs;
    CHECK(a840_emit_context_init(&s.cs, &p) != A840_EMIT_OK);
    CHECK(s.words == words && same(s.relocs, relocs) && same(s.cs, cs));
}
void reject_bin(Stream &s, const A840ContextInitParams &p, int bv)
{
    const auto words = s.words;
    const auto relocs = s.relocs;
    const auto cs = s.cs;
    CHECK(a840_emit_bin_preamble(&s.cs, &p, bv) != A840_EMIT_OK);
    CHECK(s.words == words && same(s.relocs, relocs) && same(s.cs, cs));
}

void check_table()
{
    CASE("TSV compiled table source order and six blocked rows");
    std::size_t count = 0, blocked = 0, statics = 0, rp = 0;
    const auto *table = a840_context_init_table(&count);
    CHECK(table != nullptr && count == 69);
    auto path = std::filesystem::absolute(std::filesystem::path(__FILE__)).parent_path().parent_path() /
                "caps/context_init_a840.tsv";
    std::ifstream file(path);
    CHECK(file.good());
    std::string line;
    std::size_t row = 0;
    bool header = false;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!header) {
            CHECK(line == "reg_macro\treg_offset_hex\tvalue_hex\tscope\tsource_ref\tnote");
            header = true;
            continue;
        }
        std::vector<std::string> cells;
        std::stringstream split(line);
        for (std::string cell; std::getline(split, cell, '\t');) cells.push_back(cell);
        CHECK(cells.size() == 6 && row < count);
        const auto &r = table[row++];
        CHECK(cells[0] == r.reg_macro);
        CHECK(std::stoul(cells[1], nullptr, 16) == r.reg);
        CHECK(std::stoul(cells[2], nullptr, 16) == r.value);
        CHECK(cells[4] == r.source_ref && cells[5] == r.note);
        if (cells[3] == "static_ctx") { CHECK(r.scope == A840_CTX_STATIC); ++statics; }
        else if (cells[3] == "rp_init") { CHECK(r.scope == A840_CTX_RP); ++rp; }
        else { CHECK(cells[3] == "umd_blocked_a8xx" && r.scope == A840_CTX_UMD_BLOCKED); ++blocked; }
        CHECK(a840_reg_is_umd_blocked_on_a8xx(r.reg) == (r.scope == A840_CTX_UMD_BLOCKED));
    }
    CHECK(row == 69 && statics == 49 && rp == 14 && blocked == 6);
    CHECK(std::strlen(a840_context_init_tsv_sha256()) == 64);
    CHECK(a840_reg_is_umd_blocked_on_a8xx(A840_REG_RB_GC_GMEM_PROTECT) == 1);
    CHECK(a840_reg_is_umd_blocked_on_a8xx(A840_REG_SP_UPDATE_CNTL) == 0);
}

void cross_reference(const Stream &stream)
{
    if (a840_cs_init == nullptr) return;
    CHECK(a840_cs_emit_pkt4 != nullptr && a840_cs_emit_pkt7 != nullptr &&
          a840_pm4_decode != nullptr && a840_pm4_type4_header != nullptr &&
          a840_pm4_type7_header != nullptr);
    Stream other;
    CHECK(a840_cs_init(&other.cs, other.words.data(), other.words.size(), nullptr, 0) == A840_EMIT_OK);
    std::array<A840PM4Packet, 256> decoded{};
    std::size_t packets = 0;
    CHECK(a840_pm4_decode(stream.words.data(), stream.cs.used_dwords,
                         decoded.data(), decoded.size(), &packets) == A840_EMIT_OK);
    for (std::size_t i = 0; i < packets; ++i) {
        const auto &p = decoded[i];
        const auto *payload = stream.words.data() + p.header_word + 1;
        std::uint32_t header = 0;
        if (p.type == 4) {
            CHECK(a840_pm4_type4_header(p.identifier, p.payload_words, &header) == A840_EMIT_OK);
            CHECK(a840_cs_emit_pkt4(&other.cs, p.identifier, payload, p.payload_words) == A840_EMIT_OK);
        } else {
            CHECK(a840_pm4_type7_header(p.identifier, p.payload_words, &header) == A840_EMIT_OK);
            CHECK(a840_cs_emit_pkt7(&other.cs, p.identifier, payload, p.payload_words) == A840_EMIT_OK);
        }
        CHECK(header == stream.words[p.header_word]);
        ++cross_packets;
    }
    CHECK(other.cs.used_dwords == stream.cs.used_dwords);
    CHECK(std::memcmp(other.words.data(), stream.words.data(),
                      stream.cs.used_dwords * sizeof(std::uint32_t)) == 0);
}

void test_context()
{
    check_table();
    const auto p = parameters();
    CASE("full per-command prefix and exact maximum");
    Stream full;
    CHECK(a840_emit_context_init(&full.cs, &p) == A840_EMIT_OK);
    CHECK(full.cs.used_dwords == A840_CTX_INIT_MAX_DWORDS && full.cs.reloc_count == 6);
    tail_unchanged(full, full.cs.used_dwords);
    const auto packets = decode(full.words.data(), full.cs.used_dwords);
    CHECK(packets[0].type == 7 && packets[0].id == A840_CP_THREAD_CONTROL);
    CHECK(full.words[1] == 3);
    const std::array<std::uint32_t, 4> events = {{25, 24, 40, 51}};
    for (std::size_t i = 0; i < events.size(); ++i) {
        CHECK(packets[i + 1].id == A840_CP_EVENT_WRITE7 && packets[i + 1].count == 1);
        CHECK(full.words[packets[i + 1].start + 1] == events[i]);
    }
    CHECK(packets[5].id == A840_CP_WAIT_FOR_IDLE && packets[5].count == 0);
    CHECK(packets[6].type == 4 && packets[6].id == A840_REG_SP_UPDATE_CNTL);
    CHECK(full.words[packets[6].start + 1] == 63);
    CHECK(packets[7].id == A840_CP_WAIT_FOR_IDLE);
    CHECK(last_reg(full, A840_REG_SP_VS_CONST_CONFIG) == 0x102);
    CHECK(last_reg(full, A840_REG_SP_PS_CNTL_0) == 0);
    CHECK(last_reg(full, A840_REG_TPL1_MODE_CNTL) == 0x10a2);
    CHECK(last_reg(full, A840_REG_TPL1_PS_ROTATION_CNTL) == 0);
    CHECK(last_reg(full, A840_REG_RB_CCU_CACHE_CNTL) == 0xced00ff0);
    std::size_t ambles = 0;
    for (const auto &packet : packets) {
        if (packet.type == 7 && packet.id == A840_CP_SET_DRAW_STATE)
            CHECK(full.words[packet.start + 1] == (1u << 18));
        if (packet.type == 7 && packet.id == A840_CP_BV_BR_COUNT_OPS)
            CHECK(full.words[packet.start + 1] == 2 && full.words[packet.start + 2] == p.vis_stream_count);
        if (packet.type == 7 && packet.id == A840_CP_SET_AMBLE) {
            CHECK(packet.count == 3);
            CHECK((full.words[packet.start + 3] >> 20) != 3); /* never KMD_AMBLE_TYPE */
            ++ambles;
        }
    }
    CHECK(ambles == 4);
    for (std::size_t i = 0; i < full.cs.reloc_count; ++i) {
        const auto &r = full.relocs[i];
        CHECK(r.flags == A840_RELOC_READ && r.dword_offset + 1 < full.cs.used_dwords);
        CHECK((static_cast<std::uint64_t>(full.words[r.dword_offset + 1]) << 32 |
               full.words[r.dword_offset]) == r.gpu_va);
        CHECK(r.bo_handle == (i < 2 ? p.border_color_bo : 0));
    }

    CASE("BR bin preamble exact maximum and no thread override");
    Stream br;
    CHECK(a840_emit_bin_preamble(&br.cs, &p, 0) == A840_EMIT_OK);
    CHECK(br.cs.used_dwords == A840_BIN_PREAMBLE_MAX_DWORDS && br.cs.reloc_count == 2);
    CHECK(last_reg(br, A840_REG_RB_CCU_CACHE_CNTL) == 0x0fccd14c);
    CHECK(last_reg(br, A840_REG_RB_BUFFER_CNTL) == 0 && last_reg(br, A840_REG_RB_CLEAR_TARGET) == 1);
    for (const auto &packet : decode(br.words.data(), br.cs.used_dwords)) CHECK(packet.type == 4);
    CASE("BV bin preamble skips BR-only CCU and tile setup");
    Stream bv;
    CHECK(a840_emit_bin_preamble(&bv.cs, &p, 1) == A840_EMIT_OK);
    CHECK(bv.cs.used_dwords == 118 && bv.cs.reloc_count == 2);
    CHECK(!has_reg(bv, A840_REG_RB_CCU_CACHE_CNTL) && !has_reg(bv, A840_REG_RB_BUFFER_CNTL));
    CHECK(has_reg(bv, A840_REG_VPC_ATTR_BUF_GMEM_BASE));
    decode(bv.words.data(), bv.cs.used_dwords);
    CASE("real read-only a840_cs.c packet cross-reference");
    cross_reference(full); cross_reference(br); cross_reference(bv);

    CASE("every context capacity below maximum preserves all tail bytes and cursors");
    for (std::size_t capacity = 0; capacity < A840_CTX_INIT_MAX_DWORDS; ++capacity) {
        Stream small(capacity); reject_context(small, p);
    }
    CASE("every BR bin capacity below maximum preserves tails");
    for (std::size_t capacity = 0; capacity < A840_BIN_PREAMBLE_MAX_DWORDS; ++capacity) {
        Stream small(capacity); reject_bin(small, p, 0);
    }
    CASE("relocation capacity rollback including nonzero prefixes");
    Stream relocs(WORDS, 5);
    relocs.cs.used_dwords = 3;
    relocs.words[0] = relocs.words[1] = relocs.words[2] = 99;
    relocs.cs.reloc_count = 1;
    reject_context(relocs, p);
    Stream relocs_bin(WORDS, 1); reject_bin(relocs_bin, p, 1);
    CASE("success appends and offsets relocations without overwriting prefix");
    Stream appended;
    appended.cs.used_dwords = 3;
    appended.words[0] = 1; appended.words[1] = 2; appended.words[2] = 3;
    appended.cs.reloc_count = 1;
    CHECK(a840_emit_context_init(&appended.cs, &p) == A840_EMIT_OK);
    CHECK(appended.words[0] == 1 && appended.words[1] == 2 && appended.words[2] == 3);
    CHECK(appended.relocs[1].dword_offset == full.relocs[0].dword_offset + 3);
    CASE("optional relocation storage omitted");
    Stream no_reloc;
    no_reloc.cs.relocs = nullptr; no_reloc.cs.reloc_capacity = 0;
    CHECK(a840_emit_context_init(&no_reloc.cs, &p) == A840_EMIT_OK);
    CHECK(no_reloc.cs.reloc_count == 0);
    CASE("optional pre/postamble disabled with explicit zero-address packets");
    auto optional = p;
    optional.preempt_preamble_va = optional.preempt_postamble_va = 0;
    optional.preempt_preamble_dwords = optional.preempt_postamble_dwords = 0;
    Stream disabled;
    CHECK(a840_emit_context_init(&disabled.cs, &optional) == A840_EMIT_OK);
    CHECK(disabled.cs.used_dwords == A840_CTX_INIT_MAX_DWORDS && disabled.cs.reloc_count == 4);
    CASE("nearest-even and caller-controlled early-reset branch");
    auto rounded = p;
    rounded.use_tex_coord_nearest_even = 1;
    rounded.enable_early_preamble_reset = 0;
    Stream altered;
    CHECK(a840_emit_context_init(&altered.cs, &rounded) == A840_EMIT_OK);
    CHECK(last_reg(altered, A840_REG_TPL1_MODE_CNTL) == 0x10a6);
    CHECK(!has_reg(altered, A840_REG_SP_PS_CNTL_0));
    CHECK(altered.cs.used_dwords == A840_CTX_INIT_MAX_DWORDS - 2);
    for (int mutation = 0; mutation < 15; ++mutation) {
        CASE("invalid context parameter rejects without any CS write");
        auto bad = p;
        switch (mutation) {
        case 0: bad.border_color_gpu_va = 0; break;
        case 1: bad.border_color_gpu_va += 4; break;
        case 2: bad.border_color_gpu_va = UINT64_C(1) << 48; break;
        case 3: bad.bin_preamble_br_va = 0; break;
        case 4: bad.bin_preamble_bv_va += 1; break;
        case 5: bad.bin_preamble_br_dwords = 0; break;
        case 6: bad.bin_preamble_bv_dwords = UINT32_C(1) << 20; break;
        case 7: bad.preempt_preamble_va = 0; break;
        case 8: bad.preempt_postamble_dwords = 0; break;
        case 9: bad.preempt_preamble_va = (UINT64_C(1) << 48) - 4; break;
        case 10: bad.use_tex_coord_nearest_even = 2; break;
        case 11: bad.enable_early_preamble_reset = 2; break;
        case 12: bad.vis_stream_count = 0; break;
        case 13: bad.vis_stream_count = 65536; break;
        case 14: bad.bin_preamble_br_va = (UINT64_C(1) << 48) - 4; break;
        }
        Stream rejected; reject_context(rejected, bad);
    }
    CASE("invalid BV selector and malformed CS cursor");
    Stream invalid_bv; reject_bin(invalid_bv, p, 2);
    Stream invalid_cursor; invalid_cursor.cs.used_dwords = WORDS + 1; reject_context(invalid_cursor, p);
    CASE("input alias and overlapping relocation storage rejected");
    Stream aliased;
    const auto before = aliased.words;
    CHECK(a840_emit_context_init(&aliased.cs,
          reinterpret_cast<const A840ContextInitParams *>(aliased.words.data())) == A840_EMIT_ALIAS);
    CHECK(aliased.words == before && aliased.cs.used_dwords == 0);
    Stream overlapping;
    overlapping.cs.relocs = reinterpret_cast<A840Reloc *>(overlapping.words.data());
    reject_context(overlapping, p);
    CASE("null pointers rejected without writes");
    Stream nulls;
    CHECK(a840_emit_context_init(nullptr, &p) == A840_EMIT_INVALID_ARG);
    CHECK(a840_emit_context_init(&nulls.cs, nullptr) == A840_EMIT_INVALID_ARG);
    CHECK(nulls.cs.used_dwords == 0);
}


constexpr std::uint32_t CORE_FLAGS = NativeAdrenoParamsV3TimelineWaitAny |
    NativeAdrenoParamsV3SharedZeroPage | NativeAdrenoParamsV3SharedTier0Pack |
    NativeAdrenoParamsV3WriterPromise | NativeAdrenoParamsV3UbwcSurfaceMeta;

A840AbiV3State abi_state(std::uint32_t flags = CORE_FLAGS)
{
    A840AbiV3State st{};
    CHECK(a840_abi_v3_init(&st, flags) == A840_ABI_V3_OK);
    return st;
}
struct FenceTable {
    std::array<std::uint64_t, 32> reached{};
    std::size_t calls = 0;
    std::uint64_t fail_queue = UINT64_MAX;
};
A840AbiV3Status query_fence(void *cookie, std::uint64_t queue, std::uint64_t *value)
{
    auto &table = *static_cast<FenceTable *>(cookie);
    ++table.calls;
    if (queue >= table.reached.size() || queue == table.fail_queue)
        return A840_ABI_V3_NOT_FOUND;
    *value = table.reached[static_cast<std::size_t>(queue)];
    return A840_ABI_V3_OK;
}
A840AbiV3Status wait(const A840AbiV3State &st, const NativeAdrenoAbiWaitAnyV3 &r,
    FenceTable &table, std::uint64_t elapsed, NativeAdrenoAbiWaitAnyReplyV3 &reply)
{
    return a840_abi_v3_eval_wait_any(&st, &r, query_fence, &table, elapsed, 987654, &reply);
}

void test_wait_and_boundaries()
{
    CASE("ABI init capabilities and reserved methods");
    auto st = abi_state(CORE_FLAGS | NativeAdrenoParamsV2HostImport |
                        NativeAdrenoParamsV3SparseVmBind | NativeAdrenoParamsV3PreemptRings);
    CHECK(st.cap_flags == (CORE_FLAGS | NativeAdrenoParamsV2HostImport));
    const std::array<std::uint32_t, 5> methods = {{20, 21, 23, 24, 25}};
    for (auto method : methods) CHECK(a840_abi_v3_check_method(&st, method) == A840_ABI_V3_OK);
    CHECK(a840_abi_v3_check_method(&st, 22) == A840_ABI_V3_UNSUPPORTED);
    CHECK(a840_abi_v3_check_method(&st, 26) == A840_ABI_V3_UNSUPPORTED);
    CHECK(a840_abi_v3_check_method(&st, 999) == A840_ABI_V3_UNSUPPORTED);
    CASE("unknown init flags reject without mutation");
    const auto old = st;
    CHECK(a840_abi_v3_init(&st, 1u << 31) == A840_ABI_V3_INVALID_ARG);
    CHECK(same(st, old));
    CHECK(a840_abi_v3_init(nullptr, CORE_FLAGS) == A840_ABI_V3_INVALID_ARG);
    CASE("exact fixed-size method envelopes and oversized rejection");
    const std::array<std::array<std::size_t, 3>, 5> sizes = {{
        {{20, sizeof(NativeAdrenoAbiWaitAnyV3), sizeof(NativeAdrenoAbiWaitAnyReplyV3)}},
        {{21, 0, sizeof(NativeAdrenoAbiZeroPageReplyV3)}},
        {{23, sizeof(NativeAdrenoAbiSharedPackReqV3), sizeof(NativeAdrenoAbiSharedPackReplyV3)}},
        {{24, sizeof(NativeAdrenoAbiWriterPromiseV3), sizeof(NativeAdrenoAbiWriterPromiseReplyV3)}},
        {{25, sizeof(NativeAdrenoAbiSurfaceMetaV3), sizeof(NativeAdrenoAbiSurfaceMetaV3)}}
    }};
    for (const auto &s : sizes) {
        CHECK(a840_abi_v3_validate_envelope(static_cast<std::uint32_t>(s[0]), s[1], s[2]) == A840_ABI_V3_OK);
        CHECK(a840_abi_v3_validate_envelope(static_cast<std::uint32_t>(s[0]), s[1] + 1, s[2]) == A840_ABI_V3_INVALID_ARG);
        CHECK(a840_abi_v3_validate_envelope(static_cast<std::uint32_t>(s[0]), s[1], s[2] - 1) == A840_ABI_V3_INVALID_ARG);
    }
    CHECK(a840_abi_v3_validate_envelope(20, 3105, 16) == A840_ABI_V3_RANGE);
    CHECK(a840_abi_v3_validate_envelope(22, 0, 0) == A840_ABI_V3_UNSUPPORTED);
    CASE("WaitAny any semantics and full satisfied mask reporting");
    NativeAdrenoAbiWaitAnyV3 request{};
    request.count = 3; request.timeout_ns = 10;
    for (std::uint32_t i = 0; i < 3; ++i) { request.queues[i] = i; request.fences[i] = 5; }
    FenceTable table;
    table.reached[1] = 5;
    NativeAdrenoAbiWaitAnyReplyV3 reply{};
    CHECK(wait(st, request, table, 10, reply) == A840_ABI_V3_OK);
    CHECK(reply.satisfied_mask == 2 && reply.timed_out == 0 && reply.observed_ticks == 987654);
    CHECK(table.calls == 3);
    CASE("WaitAll timeout and completion precedence");
    request.flags = 1;
    CHECK(wait(st, request, table, 9, reply) == A840_ABI_V3_OK);
    CHECK(reply.satisfied_mask == 2 && reply.timed_out == 0);
    CHECK(wait(st, request, table, 10, reply) == A840_ABI_V3_OK && reply.timed_out == 1);
    table.reached.fill(5);
    CHECK(wait(st, request, table, 100, reply) == A840_ABI_V3_OK);
    CHECK(reply.satisfied_mask == 7 && reply.timed_out == 0);
    CASE("32-fence full mask including bit31 without shift-by-32");
    request = {};
    request.count = 32; request.flags = 1; request.timeout_ns = UINT64_MAX;
    for (std::uint32_t i = 0; i < 32; ++i) {
        request.queues[i] = i;
        request.fences[i] = UINT64_C(1) << 40;
        table.reached[i] = request.fences[i];
    }
    table.calls = 0;
    CHECK(wait(st, request, table, UINT64_MAX, reply) == A840_ABI_V3_OK);
    CHECK(reply.satisfied_mask == UINT32_MAX && reply.timed_out == 0 && table.calls == 32);
    CASE("32-fence any mask and highest-bit-only completion");
    table.reached.fill(0); table.reached[31] = UINT64_C(1) << 40;
    request.flags = 0;
    CHECK(wait(st, request, table, 0, reply) == A840_ABI_V3_OK);
    CHECK(reply.satisfied_mask == (1u << 31) && reply.timed_out == 0);
    CASE("zero timeout and default queue timeline zero");
    request = {};
    request.count = 1; request.queues[0] = 0; request.fences[0] = 1;
    table.reached[0] = 0;
    CHECK(wait(st, request, table, 0, reply) == A840_ABI_V3_OK && reply.timed_out == 1);
    request.fences[0] = 0;
    CHECK(wait(st, request, table, 0, reply) == A840_ABI_V3_OK &&
          reply.satisfied_mask == 1 && reply.timed_out == 0);
    CASE("query failure cannot partially overwrite output");
    request.count = 2; request.queues[1] = 1;
    table.fail_queue = 1;
    std::memset(&reply, 0xa5, sizeof(reply));
    const auto unchanged = reply;
    CHECK(wait(st, request, table, 0, reply) == A840_ABI_V3_NOT_FOUND);
    CHECK(same(reply, unchanged));
    table.fail_queue = UINT64_MAX;
    for (int bad = 0; bad < 3; ++bad) {
        CASE("WaitAny count/flag bounds reject before querying");
        auto invalid = request;
        if (bad == 0) invalid.count = 0;
        if (bad == 1) invalid.count = 33;
        if (bad == 2) invalid.flags = 2;
        table.calls = 0;
        CHECK(wait(st, invalid, table, 0, reply) == A840_ABI_V3_INVALID_ARG);
        CHECK(table.calls == 0 && same(reply, unchanged));
    }
    CASE("WaitAny null query and missing capability");
    CHECK(a840_abi_v3_eval_wait_any(&st, &request, nullptr, &table, 0, 0, &reply) == A840_ABI_V3_INVALID_ARG);
    auto unsupported = abi_state(0);
    CHECK(wait(unsupported, request, table, 0, reply) == A840_ABI_V3_UNSUPPORTED);
}

void test_zero_and_pack()
{
    auto st = abi_state();
    NativeAdrenoAbiZeroPageReplyV3 page{0x100000, 65536}, reply{};
    CASE("zero page unconfigured");
    std::memset(&reply, 0xa5, sizeof(reply));
    const auto untouched = reply;
    CHECK(a840_abi_v3_get_zero_page(&st, &reply) == A840_ABI_V3_NOT_FOUND && same(reply, untouched));
    CASE("zero page aligned RO range");
    CHECK(a840_abi_v3_set_zero_page(&st, &page, 1) == A840_ABI_V3_OK);
    CHECK(a840_abi_v3_get_zero_page(&st, &reply) == A840_ABI_V3_OK && same(page, reply));
    for (int mutation = 0; mutation < 7; ++mutation) {
        CASE("zero page alignment/size/VA/RO rejection");
        auto bad = page;
        const auto before = st;
        std::uint32_t readonly = 1;
        switch (mutation) {
        case 0: bad.gpu_va = 0; break;
        case 1: bad.gpu_va += 4096; break;
        case 2: bad.bytes = 65535; break;
        case 3: bad.bytes = 65536 + 4096; break;
        case 4: bad.gpu_va = UINT64_C(1) << 48; break;
        case 5: bad.gpu_va = (UINT64_C(1) << 48) - 65536; bad.bytes = 131072; break;
        case 6: readonly = 0; break;
        }
        CHECK(a840_abi_v3_set_zero_page(&st, &bad, readonly) != A840_ABI_V3_OK);
        CHECK(same(st, before));
    }
    NativeAdrenoAbiSharedPackReplyV3 pack{0x500000, 0x700000, 8192, 0xabcdef}, mapped{};
    NativeAdrenoAbiSharedPackReqV3 query{pack.actual_stack_key, 1, 0};
    CASE("shared pack unconfigured");
    CHECK(a840_abi_v3_map_shared_pack(&st, &query, &mapped) == A840_ABI_V3_NOT_FOUND);
    CASE("shared pack exact stack_key and CPU/GPU RO mapping");
    CHECK(a840_abi_v3_set_shared_pack(&st, &pack, 1, 1) == A840_ABI_V3_OK);
    CHECK(a840_abi_v3_map_shared_pack(&st, &query, &mapped) == A840_ABI_V3_OK && same(mapped, pack));
    CASE("GPU-only pack query does not expose CPU VA");
    query.map_cpu = 0;
    CHECK(a840_abi_v3_map_shared_pack(&st, &query, &mapped) == A840_ABI_V3_OK && mapped.cpu_va == 0);
    CASE("stack_key mismatch returns only actual key");
    query.expected_stack_key ^= 1;
    const auto before_key = st;
    CHECK(a840_abi_v3_map_shared_pack(&st, &query, &mapped) == A840_ABI_V3_STACK_KEY_MISMATCH);
    CHECK(mapped.actual_stack_key == pack.actual_stack_key && mapped.gpu_va == 0 &&
          mapped.cpu_va == 0 && mapped.bytes == 0 && same(st, before_key));
    query.expected_stack_key = pack.actual_stack_key;
    CASE("shared pack missing CPU mapping rejected when requested");
    auto gpu_only = pack; gpu_only.cpu_va = 0;
    CHECK(a840_abi_v3_set_shared_pack(&st, &gpu_only, 1, 0) == A840_ABI_V3_OK);
    query.map_cpu = 1;
    CHECK(a840_abi_v3_map_shared_pack(&st, &query, &mapped) == A840_ABI_V3_NOT_FOUND);
    CHECK(a840_abi_v3_set_shared_pack(&st, &pack, 1, 1) == A840_ABI_V3_OK);
    for (int mutation = 0; mutation < 7; ++mutation) {
        CASE("pack alignment/range/permissions/key rejection is transactional");
        auto bad = pack;
        std::uint32_t gpu_ro = 1, cpu_ro = 1;
        const auto before = st;
        switch (mutation) {
        case 0: bad.gpu_va += 4; break;
        case 1: bad.cpu_va += 4; break;
        case 2: bad.bytes = 8191; break;
        case 3: bad.actual_stack_key = 0; break;
        case 4: gpu_ro = 0; break;
        case 5: cpu_ro = 0; break;
        case 6: bad.cpu_va = UINT64_MAX - 4095; break;
        }
        CHECK(a840_abi_v3_set_shared_pack(&st, &bad, gpu_ro, cpu_ro) != A840_ABI_V3_OK);
        CHECK(same(st, before));
    }
    CASE("map_cpu and reserved fields reject with unchanged output");
    const auto old = mapped;
    query.map_cpu = 2;
    CHECK(a840_abi_v3_map_shared_pack(&st, &query, &mapped) == A840_ABI_V3_INVALID_ARG && same(mapped, old));
    query.map_cpu = 1; query.reserved = 1;
    CHECK(a840_abi_v3_map_shared_pack(&st, &query, &mapped) == A840_ABI_V3_INVALID_ARG && same(mapped, old));
}

struct PromiseSink {
    std::array<std::uint64_t, 256> tokens{};
    std::size_t live = 0, made = 0, fulfilled = 0, canceled = 0;
};
void promise_observer(void *cookie, std::uint64_t client,
    const NativeAdrenoAbiWriterPromiseV3 *r, std::uint32_t state)
{
    auto &sink = *static_cast<PromiseSink *>(cookie);
    CHECK(client != 0 && r != nullptr && r->promise_token != 0);
    if (state == A840_ABI_V3_PROMISE_PENDING) {
        CHECK(sink.live < sink.tokens.size());
        sink.tokens[sink.live++] = r->promise_token;
        ++sink.made;
    } else {
        auto it = std::find(sink.tokens.begin(), sink.tokens.begin() + static_cast<std::ptrdiff_t>(sink.live),
                            r->promise_token);
        CHECK(it != sink.tokens.begin() + static_cast<std::ptrdiff_t>(sink.live));
        *it = sink.tokens[--sink.live];
        if (state == A840_ABI_V3_PROMISE_FULFILLED) ++sink.fulfilled;
        else { CHECK(state == A840_ABI_V3_PROMISE_CANCELED); ++sink.canceled; }
    }
}
NativeAdrenoAbiWriterPromiseV3 make_request(std::uint64_t surface = 1, std::uint32_t count = 1)
{
    NativeAdrenoAbiWriterPromiseV3 r{};
    r.op = NativeAdrenoPromiseMake;
    r.surface_count = count;
    for (std::uint32_t i = 0; i < std::min(count, 16u); ++i) r.surface_ids[i] = surface + i;
    return r;
}
std::uint64_t make(A840AbiV3State &st, std::uint64_t client, const NativeAdrenoAbiWriterPromiseV3 &r)
{
    NativeAdrenoAbiWriterPromiseReplyV3 reply{};
    CHECK(a840_abi_v3_writer_promise(&st, client, &r, &reply) == A840_ABI_V3_OK);
    CHECK(reply.accepted == 1 && reply.reserved == 0 && reply.promise_token != 0);
    return reply.promise_token;
}
NativeAdrenoAbiSurfaceMetaV3 metadata(std::uint64_t bo = 1)
{
    NativeAdrenoAbiSurfaceMetaV3 m{};
    m.bo_handle = bo; m.op = 1; m.ubwc_enabled = 1; m.ubwc_version = 6;
    m.highest_bank_bit = 16; m.swizzle_levels = 6; m.macrotile_mode = 1;
    m.meta_offset = 4096; m.meta_pitch = 64; m.flags = 1;
    return m;
}

void test_promises()
{
    auto st = abi_state();
    PromiseSink sink;
    CHECK(a840_abi_v3_set_promise_observer(&st, promise_observer, &sink) == A840_ABI_V3_OK);
    auto r = make_request(5, 16);
    CASE("promise Make full 16-surface list");
    const auto token = make(st, 10, r);
    CHECK(sink.live == 1 && a840_abi_v3_pending_promises(&st, 10) == 1);
    r.promise_token = token; r.op = NativeAdrenoPromiseFulfill; r.fence = 9; r.queue = 2;
    NativeAdrenoAbiWriterPromiseReplyV3 reply{};
    CASE("promise Make to Fulfill resolves display dependency");
    CHECK(a840_abi_v3_writer_promise(&st, 10, &r, &reply) == A840_ABI_V3_OK);
    CHECK(sink.live == 0 && sink.fulfilled == 1 && a840_abi_v3_pending_promises(&st, 10) == 0);
    CASE("double Fulfill is rejected with no state/output change");
    const auto old = st; const auto old_reply = reply;
    CHECK(a840_abi_v3_writer_promise(&st, 10, &r, &reply) == A840_ABI_V3_STATE_CONFLICT);
    CHECK(same(st, old) && same(reply, old_reply));
    CASE("Make to Cancel and double Cancel rejection");
    auto cancel = make_request(20);
    cancel.promise_token = make(st, 10, cancel);
    cancel.op = NativeAdrenoPromiseCancel;
    CHECK(a840_abi_v3_writer_promise(&st, 10, &cancel, &reply) == A840_ABI_V3_OK);
    CHECK(sink.canceled == 1 && sink.live == 0);
    const auto canceled = st;
    CHECK(a840_abi_v3_writer_promise(&st, 10, &cancel, &reply) == A840_ABI_V3_STATE_CONFLICT);
    CHECK(same(st, canceled));
    CASE("token ownership and unknown-token rejection");
    auto owned = make_request(30); owned.promise_token = make(st, 10, owned);
    owned.op = NativeAdrenoPromiseFulfill; owned.fence = 1;
    const auto before_owner = st;
    CHECK(a840_abi_v3_writer_promise(&st, 11, &owned, &reply) == A840_ABI_V3_NOT_FOUND);
    CHECK(same(st, before_owner));
    owned.promise_token = UINT64_MAX;
    CHECK(a840_abi_v3_writer_promise(&st, 10, &owned, &reply) == A840_ABI_V3_NOT_FOUND);
    for (int mutation = 0; mutation < 6; ++mutation) {
        CASE("promise argument/surface bounds reject transactionally");
        auto bad = make_request();
        const auto before = st;
        std::uint64_t client = 10;
        switch (mutation) {
        case 0: bad.surface_count = 0; break;
        case 1: bad.surface_count = 17; break;
        case 2: bad.surface_count = 2; bad.surface_ids[1] = bad.surface_ids[0]; break;
        case 3: bad.surface_ids[0] = 0; break;
        case 4: bad.promise_token = 8; break;
        case 5: client = 0; break;
        }
        CHECK(a840_abi_v3_writer_promise(&st, client, &bad, &reply) != A840_ABI_V3_OK);
        CHECK(same(st, before));
    }
    CASE("Fulfill requires fence and unchanged original surface list");
    auto pending = make_request(100);
    pending.promise_token = make(st, 12, pending);
    pending.op = NativeAdrenoPromiseFulfill;
    auto before = st;
    CHECK(a840_abi_v3_writer_promise(&st, 12, &pending, &reply) == A840_ABI_V3_INVALID_ARG);
    CHECK(same(st, before));
    pending.fence = 1; pending.surface_ids[0] = 101;
    CHECK(a840_abi_v3_writer_promise(&st, 12, &pending, &reply) == A840_ABI_V3_STATE_CONFLICT);
    CHECK(same(st, before));

    CASE("64 active promise slots reach finite capacity without leakage");
    auto full = abi_state();
    PromiseSink full_sink;
    CHECK(a840_abi_v3_set_promise_observer(&full, promise_observer, &full_sink) == A840_ABI_V3_OK);
    std::array<std::uint64_t, 64> tokens{};
    for (std::size_t i = 0; i < tokens.size(); ++i)
        tokens[i] = make(full, 99, make_request(1000 + i));
    CHECK(a840_abi_v3_pending_promises(&full, 0) == 64 && full_sink.live == 64);
    auto extra = make_request(5000);
    const auto capacity_state = full;
    CHECK(a840_abi_v3_writer_promise(&full, 99, &extra, &reply) == A840_ABI_V3_CAPACITY);
    CHECK(same(full, capacity_state));
    CASE("retired slots recycle while old tokens never repeat");
    auto retire = make_request(1000);
    retire.promise_token = tokens[0]; retire.op = NativeAdrenoPromiseFulfill; retire.fence = 1;
    CHECK(a840_abi_v3_writer_promise(&full, 99, &retire, &reply) == A840_ABI_V3_OK);
    auto new_token = make(full, 99, extra);
    CHECK(new_token > tokens.back() && full_sink.live == 64);
    CHECK(a840_abi_v3_writer_promise(&full, 99, &retire, &reply) == A840_ABI_V3_NOT_FOUND);
    CASE("client disconnect cancels all 64 outstanding display promises once");
    CHECK(a840_abi_v3_cancel_all_for_client(&full, 99) == 64);
    CHECK(full_sink.live == 0 && full_sink.canceled == 64 && a840_abi_v3_pending_promises(&full, 0) == 0);
    CHECK(a840_abi_v3_cancel_client_promises(&full, 99) == 0 && full_sink.canceled == 64);
    CASE("token exhaustion is rejected without wrap or state modification");
    auto exhaustion = abi_state();
    exhaustion.last_promise_token = UINT64_MAX;
    const auto exhausted_before = exhaustion;
    CHECK(a840_abi_v3_writer_promise(&exhaustion, 1, &extra, &reply) == A840_ABI_V3_CAPACITY);
    CHECK(same(exhaustion, exhausted_before));
    CASE("disconnect isolates other clients and also reclaims owned surface metadata");
    auto clients = abi_state();
    PromiseSink clients_sink;
    CHECK(a840_abi_v3_set_promise_observer(&clients, promise_observer, &clients_sink) == A840_ABI_V3_OK);
    for (unsigned i = 0; i < 20; ++i) make(clients, 1, make_request(100 + i));
    for (unsigned i = 0; i < 4; ++i) make(clients, 2, make_request(200 + i));
    auto m = metadata(9); NativeAdrenoAbiSurfaceMetaV3 mreply{};
    CHECK(a840_abi_v3_surface_meta(&clients, 1, &m, 65536, &mreply) == A840_ABI_V3_OK);
    CHECK(a840_abi_v3_cancel_client_promises(&clients, 1) == 20);
    CHECK(clients_sink.live == 4 && clients_sink.canceled == 20 &&
          a840_abi_v3_pending_promises(&clients, 2) == 4);
    m.op = 0;
    CHECK(a840_abi_v3_surface_meta(&clients, 1, &m, 65536, &mreply) == A840_ABI_V3_NOT_FOUND);
    CHECK(a840_abi_v3_cancel_client_promises(&clients, 2) == 4 && clients_sink.live == 0);
}


void test_surface_meta()
{
    auto st = abi_state();
    auto request = metadata();
    NativeAdrenoAbiSurfaceMetaV3 reply{};
    CASE("SurfaceMeta Kaanapali HBB16 set/get");
    CHECK(a840_abi_v3_surface_meta(&st, 7, &request, 65536, &reply) == A840_ABI_V3_OK);
    CHECK(reply.highest_bank_bit == 16 && same(reply, request));
    auto get = NativeAdrenoAbiSurfaceMetaV3{};
    get.bo_handle = request.bo_handle;
    CHECK(a840_abi_v3_surface_meta(&st, 7, &get, 65536, &reply) == A840_ABI_V3_OK);
    CHECK(reply.op == 0 && reply.highest_bank_bit == 16 && reply.meta_offset == request.meta_offset);
    for (int mutation = 0; mutation < 10; ++mutation) {
        CASE("SurfaceMeta rejects bad UBWC/HBB/swizzle/macro/flags/alignment/bounds");
        auto bad = request;
        std::uint64_t bytes = 65536;
        const auto before = st;
        const auto old_reply = reply;
        switch (mutation) {
        case 0: bad.highest_bank_bit = 15; break;
        case 1: bad.ubwc_version = 5; break;
        case 2: bad.swizzle_levels = 7; break;
        case 3: bad.macrotile_mode = 0; break;
        case 4: bad.flags = 2; break;
        case 5: bad.meta_offset += 1; break;
        case 6: bad.meta_pitch = 63; break;
        case 7: bad.meta_offset = 65536; break;
        case 8: bytes = 0; break;
        case 9: bad.ubwc_enabled = 2; break;
        }
        CHECK(a840_abi_v3_surface_meta(&st, 7, &bad, bytes, &reply) != A840_ABI_V3_OK);
        CHECK(same(st, before) && same(reply, old_reply));
    }
    CASE("HBB15 cannot bypass validation by disabling UBWC");
    auto disabled = request;
    disabled.ubwc_enabled = 0; disabled.highest_bank_bit = 15;
    disabled.meta_pitch = 0; disabled.meta_offset = 0;
    CHECK(a840_abi_v3_surface_meta(&st, 7, &disabled, 65536, &reply) == A840_ABI_V3_INVALID_ARG);
    CASE("valid uncompressed metadata still retains Kaanapali configuration");
    disabled.highest_bank_bit = 16;
    CHECK(a840_abi_v3_surface_meta(&st, 7, &disabled, 65536, &reply) == A840_ABI_V3_OK);
    CASE("zero metadata pitch not valid for enabled UBWC");
    auto zero_pitch = request; zero_pitch.meta_pitch = 0;
    CHECK(a840_abi_v3_surface_meta(&st, 7, &zero_pitch, 65536, &reply) == A840_ABI_V3_INVALID_ARG);
    CASE("metadata handles are isolated per client");
    CHECK(a840_abi_v3_surface_meta(&st, 8, &get, 65536, &reply) == A840_ABI_V3_NOT_FOUND);
    auto other = request;
    CHECK(a840_abi_v3_surface_meta(&st, 8, &other, 65536, &reply) == A840_ABI_V3_OK);
    CASE("same handle across clients has independent lifecycle");
    CHECK(a840_abi_v3_cancel_client_promises(&st, 7) == 0);
    CHECK(a840_abi_v3_surface_meta(&st, 7, &get, 65536, &reply) == A840_ABI_V3_NOT_FOUND);
    CHECK(a840_abi_v3_surface_meta(&st, 8, &get, 65536, &reply) == A840_ABI_V3_OK);
    CASE("64 metadata slots enforce capacity and allow existing-slot updates");
    auto full = abi_state();
    for (std::uint64_t i = 1; i <= 64; ++i) {
        auto m = metadata(i);
        CHECK(a840_abi_v3_surface_meta(&full, 10, &m, 65536, &reply) == A840_ABI_V3_OK);
    }
    auto overflow = metadata(65);
    const auto before_full = full;
    CHECK(a840_abi_v3_surface_meta(&full, 10, &overflow, 65536, &reply) == A840_ABI_V3_CAPACITY);
    CHECK(same(full, before_full));
    auto update = metadata(1); update.meta_offset = 8192;
    CHECK(a840_abi_v3_surface_meta(&full, 10, &update, 65536, &reply) == A840_ABI_V3_OK);
    CASE("disconnect reclaims all metadata slots without clearing other clients");
    CHECK(a840_abi_v3_cancel_all_for_client(&full, 10) == 0);
    for (std::uint64_t i = 65; i < 129; ++i) {
        auto m = metadata(i);
        CHECK(a840_abi_v3_surface_meta(&full, 11, &m, 65536, &reply) == A840_ABI_V3_OK);
    }
    CASE("bo range changes cannot expose stale out-of-range metadata");
    get.bo_handle = 65;
    CHECK(a840_abi_v3_surface_meta(&full, 11, &get, 4096, &reply) == A840_ABI_V3_RANGE);
    CASE("SurfaceMeta null/unsupported/invalid op rejection");
    auto unsupported = abi_state(0);
    CHECK(a840_abi_v3_surface_meta(&unsupported, 7, &request, 65536, &reply) == A840_ABI_V3_UNSUPPORTED);
    CHECK(a840_abi_v3_surface_meta(&st, 7, nullptr, 65536, &reply) == A840_ABI_V3_INVALID_ARG);
    request.op = 2;
    CHECK(a840_abi_v3_surface_meta(&st, 7, &request, 65536, &reply) == A840_ABI_V3_INVALID_ARG);
}

void dump(const char *filename)
{
    CASE("binary PM4 dump combines full context and both restore preambles");
    const auto target = std::filesystem::weakly_canonical(std::filesystem::path(filename));
    const auto cache = std::filesystem::path("/home/hua/nezha-work/ios27/adreno840-native/.cache");
    const auto relative = target.lexically_relative(cache);
    CHECK(!relative.empty() && !relative.is_absolute() && *relative.begin() != ".." && relative.has_parent_path());
    Stream stream;
    const auto p = parameters();
    CHECK(a840_emit_context_init(&stream.cs, &p) == A840_EMIT_OK);
    CHECK(a840_emit_bin_preamble(&stream.cs, &p, 0) == A840_EMIT_OK);
    CHECK(a840_emit_bin_preamble(&stream.cs, &p, 1) == A840_EMIT_OK);
    CHECK(stream.cs.used_dwords == 415 && stream.cs.reloc_count == 10);
    decode(stream.words.data(), stream.cs.used_dwords);
    if (a840_cs_init != nullptr) CHECK(cross_packets > 0); /* Standalone dump checks real CS reference. */
    std::ofstream file(target, std::ios::binary);
    CHECK(file.good());
    for (std::size_t i = 0; i < stream.cs.used_dwords; ++i)
        for (unsigned shift = 0; shift < 32; shift += 8)
            file.put(static_cast<char>((stream.words[i] >> shift) & 255u));
    file.close();
    CHECK(file.good());
    std::printf("DUMP: %zu dwords, %zu relocations -> %s\n",
                stream.cs.used_dwords, stream.cs.reloc_count, filename);
}
} /* namespace */

int main(int argc, char **argv)
{
    if (argc != 1 && (argc != 3 || std::strcmp(argv[1], "--dump-pm4") != 0)) {
        std::fprintf(stderr, "usage: test_context_init_and_abi3 [--dump-pm4 cache-path]\n");
        return EXIT_FAILURE;
    }
    test_context();
    test_wait_and_boundaries();
    test_zero_and_pack();
    test_promises();
    test_surface_meta();
    if (argc == 3) dump(argv[2]);
    CHECK(cases >= 40);
    std::printf("PASS: Gen8 context173 / bin BR124 BV118; TSV, parity, blocked registers and byte-preserving transactions\n");
    std::printf("PASS: ABI20/21/23/24/25; WaitAny32, RO mapping/key mismatch, promise disconnect cleanup, HBB16/HBB15\n");
    std::printf("PASS: %zu independent cases, %zu checks; real CS cross-reference packets=%zu\n",
                cases, checks, cross_packets);
    if (a840_cs_init == nullptr)
        std::printf("NOTE: standalone Adreno840 suite has no external CS object; required standalone gates cover cross-reference\n");
    return EXIT_SUCCESS;
}
