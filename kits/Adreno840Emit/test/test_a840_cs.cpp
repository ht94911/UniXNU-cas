/* SPDX-License-Identifier: MIT */
#include "Adreno840EmitABI.h"
#include "AdrenoPM4.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

std::size_t assertions = 0;

[[noreturn]] void fail(const char *expression, const char *file, int line)
{
    std::fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, expression);
    std::abort();
}

#define CHECK(expression) do { \
    ++assertions; \
    if (!(expression)) fail(#expression, __FILE__, __LINE__); \
} while (false)

constexpr std::uint32_t reg_limit = UINT32_C(1) << 18;
constexpr std::uint32_t sds_binning = UINT32_C(1) << 20;
constexpr std::uint32_t sds_gmem = UINT32_C(1) << 21;
constexpr std::uint32_t sds_sysmem = UINT32_C(1) << 22;
constexpr std::uint32_t sds_disable = UINT32_C(1) << 17;
constexpr std::uint64_t max_va = (UINT64_C(1) << 48) - 1;

std::uint32_t ref4(std::uint32_t reg, std::uint32_t count)
{
    std::uint32_t header = 0;
    CHECK(AdrenoPM4Type4(reg, count, &header) == AdrenoPM4Success);
    return header;
}

std::uint32_t ref7(std::uint32_t opcode, std::uint32_t count)
{
    std::uint32_t header = 0;
    CHECK(AdrenoPM4Type7(opcode, count, &header) == AdrenoPM4Success);
    return header;
}

struct Fixture {
    std::array<std::uint32_t, 40> words{};
    std::array<A840Reloc, 4> relocs{};
    A840CmdStream cs{};

    explicit Fixture(std::size_t capacity = 32, std::size_t reloc_capacity = 4)
    {
        for (std::size_t i = 0; i < words.size(); ++i)
            words[i] = UINT32_C(0x9abc0000) + static_cast<std::uint32_t>(i);
        std::memset(relocs.data(), 0xa5, sizeof(relocs));
        CHECK(a840_cs_init(&cs, words.data(), capacity, relocs.data(),
            reloc_capacity) == A840_EMIT_OK);
    }
};

struct Snapshot {
    std::array<unsigned char, sizeof(Fixture::words)> words{};
    std::array<unsigned char, sizeof(Fixture::relocs)> relocs{};
    std::array<unsigned char, sizeof(A840CmdStream)> cs{};

    explicit Snapshot(const Fixture &fixture)
    {
        std::memcpy(words.data(), fixture.words.data(), words.size());
        std::memcpy(relocs.data(), fixture.relocs.data(), relocs.size());
        std::memcpy(cs.data(), &fixture.cs, cs.size());
    }

    void unchanged(const Fixture &fixture) const
    {
        CHECK(std::memcmp(words.data(), fixture.words.data(), words.size()) == 0);
        CHECK(std::memcmp(relocs.data(), fixture.relocs.data(), relocs.size()) == 0);
        CHECK(std::memcmp(cs.data(), &fixture.cs, cs.size()) == 0);
    }

    void untouched_tail(const Fixture &fixture, std::size_t start) const
    {
        const std::size_t offset = start * sizeof(std::uint32_t);
        CHECK(std::memcmp(words.data() + offset, fixture.words.data() + start,
            words.size() - offset) == 0);
    }
};

void decode_error(const std::uint32_t *words, std::size_t word_count,
    A840EmitStatus expected, std::size_t capacity = 4)
{
    std::array<A840PM4Packet, 4> packets{};
    std::memset(packets.data(), 0xa5, sizeof(packets));
    const auto before = packets;
    std::size_t count = 987;
    CHECK(a840_pm4_decode(words, word_count, packets.data(), capacity, &count)
        == expected);
    CHECK(count == 0);
    CHECK(std::memcmp(packets.data(), before.data(), sizeof(packets)) == 0);
}

void compare_packets(const std::uint32_t *words, std::size_t word_count,
    A840PM4Packet *packets, std::size_t capacity, std::size_t expected_count)
{
    std::array<AdrenoPM4Packet, 256> reference{};
    std::size_t count = 999, reference_count = 999;
    CHECK(capacity <= reference.size());
    CHECK(a840_pm4_decode(words, word_count, packets, capacity, &count)
        == A840_EMIT_OK);
    CHECK(AdrenoPM4Plan(words, word_count, reference.data(), capacity,
        &reference_count) == AdrenoPM4Success);
    CHECK(count == expected_count);
    CHECK(count == reference_count);
    for (std::size_t i = 0; i < count; ++i) {
        CHECK(packets[i].header_word == reference[i].header_word);
        CHECK(packets[i].type == reference[i].type);
        CHECK(packets[i].identifier == reference[i].identifier);
        CHECK(packets[i].payload_words == reference[i].payload_words);
    }
}

void test_init_valid()
{
    Fixture f;
    f.cs.used_dwords = 12;
    f.cs.reloc_count = 3;
    const Snapshot before(f);
    CHECK(a840_cs_init(&f.cs, f.words.data(), 17, f.relocs.data(), 2)
        == A840_EMIT_OK);
    CHECK(f.cs.words == f.words.data());
    CHECK(f.cs.capacity_dwords == 17 && f.cs.used_dwords == 0);
    CHECK(f.cs.relocs == f.relocs.data());
    CHECK(f.cs.reloc_capacity == 2 && f.cs.reloc_count == 0);
    before.untouched_tail(f, 0);
    CHECK(std::memcmp(f.relocs.data(), before.relocs.data(), sizeof(f.relocs)) == 0);
}

void test_init_empty()
{
    A840CmdStream cs{};
    CHECK(a840_cs_init(&cs, nullptr, 0, nullptr, 0) == A840_EMIT_OK);
    CHECK(cs.words == nullptr && cs.relocs == nullptr);
    CHECK(cs.capacity_dwords == 0 && cs.used_dwords == 0);
    CHECK(cs.reloc_capacity == 0 && cs.reloc_count == 0);
    CHECK(a840_cs_emit_pkt7(&cs, 0, nullptr, 0) == A840_EMIT_CAPACITY);
    Fixture f;
    CHECK(a840_cs_init(&cs, f.words.data(), 0, f.relocs.data(), 0)
        == A840_EMIT_OK);
    CHECK(cs.words == f.words.data() && cs.relocs == f.relocs.data());
}

void test_init_missing_arguments()
{
    Fixture f;
    const Snapshot before(f);
    CHECK(a840_cs_init(nullptr, f.words.data(), 32, f.relocs.data(), 4)
        == A840_EMIT_INVALID_ARG);
    CHECK(a840_cs_init(&f.cs, nullptr, 1, nullptr, 0) == A840_EMIT_INVALID_ARG);
    CHECK(a840_cs_init(&f.cs, f.words.data(), 32, nullptr, 1)
        == A840_EMIT_INVALID_ARG);
    before.unchanged(f);
}

void test_init_storage_alias()
{
    Fixture f;
    const Snapshot before(f);
    CHECK(a840_cs_init(&f.cs, reinterpret_cast<std::uint32_t *>(f.relocs.data()),
        6, f.relocs.data(), 1) == A840_EMIT_ALIAS);
    CHECK(a840_cs_init(&f.cs, reinterpret_cast<std::uint32_t *>(f.relocs.data()) + 2,
        4, f.relocs.data(), 1) == A840_EMIT_ALIAS);
    CHECK(a840_cs_init(&f.cs, f.words.data(), 32,
        reinterpret_cast<A840Reloc *>(f.words.data() + 2), 1) == A840_EMIT_ALIAS);
    before.unchanged(f);
}

void test_init_metadata_alias()
{
    Fixture f;
    const Snapshot before(f);
    CHECK(a840_cs_init(&f.cs, reinterpret_cast<std::uint32_t *>(&f.cs), 1,
        nullptr, 0) == A840_EMIT_ALIAS);
    CHECK(a840_cs_init(&f.cs, f.words.data(), 32,
        reinterpret_cast<A840Reloc *>(&f.cs), 1) == A840_EMIT_ALIAS);
    before.unchanged(f);
}

void test_init_adjacent_storage()
{
    struct Storage {
        std::uint32_t words[4];
        A840Reloc relocs[1];
    } storage{};
    static_assert(offsetof(Storage, relocs) == sizeof(storage.words), "adjacent");
    A840CmdStream cs{};
    CHECK(a840_cs_init(&cs, storage.words, 4, storage.relocs, 1) == A840_EMIT_OK);
    CHECK(a840_cs_emit_reg64_va(&cs, 7, 12, UINT64_C(0x123456789abc),
        A840_RELOC_WRITE) == A840_EMIT_OK);
    CHECK(storage.relocs[0].dword_offset == 1);
    CHECK(storage.relocs[0].flags == A840_RELOC_WRITE);
}

void test_init_range_overflow()
{
    Fixture f;
    const Snapshot before(f);
    constexpr auto size_max = std::numeric_limits<std::size_t>::max();
    CHECK(a840_cs_init(&f.cs, f.words.data(), size_max / sizeof(std::uint32_t) + 1,
        nullptr, 0) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_cs_init(&f.cs, f.words.data(), 32, f.relocs.data(),
        size_max / sizeof(A840Reloc) + 1) == A840_EMIT_RANGE_ERROR);
    auto *wrapping = reinterpret_cast<std::uint32_t *>(UINTPTR_MAX - 3);
    CHECK(a840_cs_init(&f.cs, wrapping, 1, nullptr, 0) == A840_EMIT_RANGE_ERROR);
    before.unchanged(f);
}

void test_init_alignment()
{
    Fixture f;
    const Snapshot before(f);
    auto *word_bytes = reinterpret_cast<unsigned char *>(f.words.data());
    auto *reloc_bytes = reinterpret_cast<unsigned char *>(f.relocs.data());
    CHECK(a840_cs_init(&f.cs, reinterpret_cast<std::uint32_t *>(word_bytes + 1),
        1, nullptr, 0) == A840_EMIT_ALIGNMENT);
    CHECK(a840_cs_init(&f.cs, f.words.data(), 32,
        reinterpret_cast<A840Reloc *>(reloc_bytes + 1), 1) == A840_EMIT_ALIGNMENT);
    auto *cs_bytes = reinterpret_cast<unsigned char *>(&f.cs);
    CHECK(a840_cs_init(reinterpret_cast<A840CmdStream *>(cs_bytes + 1),
        f.words.data(), 32, nullptr, 0) == A840_EMIT_ALIGNMENT);
    before.unchanged(f);
}

void test_type4_reference()
{
    for (std::uint32_t reg = 0; reg < reg_limit; ++reg) {
        std::uint32_t actual = 0;
        CHECK(a840_pm4_type4_header(reg, 1, &actual) == A840_EMIT_OK);
        CHECK(actual == ref4(reg, 1));
    }
    constexpr std::uint32_t registers[] = {0, 1, 2, 0x1234, 0x15555, 0x3ff80};
    for (auto reg : registers) {
        for (std::uint32_t count = 1; count <= 127; ++count) {
            std::uint32_t actual = 0;
            CHECK(a840_pm4_type4_header(reg, count, &actual) == A840_EMIT_OK);
            CHECK(actual == ref4(reg, count));
        }
    }
    CHECK(ref4(0, 1) == UINT32_C(0x48000001));
    CHECK(ref4(1, 1) == UINT32_C(0x40000101));
}

void test_type4_boundaries()
{
    std::uint32_t header = UINT32_C(0xa5a5a5a5);
    CHECK(a840_pm4_type4_header(0, 0, &header) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_pm4_type4_header(0, 128, &header) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_pm4_type4_header(reg_limit, 1, &header) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_pm4_type4_header(UINT32_MAX, UINT32_MAX, &header)
        == A840_EMIT_RANGE_ERROR);
    CHECK(header == UINT32_C(0xa5a5a5a5));
    CHECK(a840_pm4_type4_header(0, 1, nullptr) == A840_EMIT_INVALID_ARG);
    CHECK(a840_pm4_type4_header(reg_limit - 1, 1, &header) == A840_EMIT_OK);
    CHECK(header == ref4(reg_limit - 1, 1));
    /* Standalone fields are valid; the emitter/decoder checks the reg span. */
    CHECK(a840_pm4_type4_header(reg_limit - 1, 127, &header) == A840_EMIT_OK);
}

void test_type7_reference()
{
    for (std::uint32_t opcode = 0; opcode <= 127; ++opcode) {
        for (std::uint32_t count = 0; count <= 16383; ++count) {
            std::uint32_t actual = 0;
            CHECK(a840_pm4_type7_header(opcode, count, &actual) == A840_EMIT_OK);
            CHECK(actual == ref7(opcode, count));
        }
    }
    CHECK(ref7(0, 0) == UINT32_C(0x70808000));
    CHECK(ref7(0x43, 3) == UINT32_C(0x70438003));
}

void test_type7_boundaries()
{
    std::uint32_t header = UINT32_C(0xaaaaaaaa);
    CHECK(a840_pm4_type7_header(128, 0, &header) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_pm4_type7_header(0, 16384, &header) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_pm4_type7_header(UINT32_MAX, UINT32_MAX, &header)
        == A840_EMIT_RANGE_ERROR);
    CHECK(header == UINT32_C(0xaaaaaaaa));
    CHECK(a840_pm4_type7_header(0, 0, nullptr) == A840_EMIT_INVALID_ARG);
    CHECK(a840_pm4_type7_header(127, 16383, &header) == A840_EMIT_OK);
    CHECK(header == ref7(127, 16383));
}

void test_header_alignment()
{
    alignas(std::uint32_t) std::array<unsigned char, 8> bytes{};
    bytes.fill(0xa5);
    const auto before = bytes;
    auto *output = reinterpret_cast<std::uint32_t *>(bytes.data() + 1);
    CHECK(a840_pm4_type4_header(0, 1, output) == A840_EMIT_ALIGNMENT);
    CHECK(a840_pm4_type7_header(0, 1, output) == A840_EMIT_ALIGNMENT);
    CHECK(bytes == before);
}

void test_pkt4_basic()
{
    Fixture f;
    const Snapshot before(f);
    const std::uint32_t values[] = {0x01234567, 0x89abcdef, 0xfedcba98};
    CHECK(a840_cs_emit_pkt4(&f.cs, 0x1240, values, 3) == A840_EMIT_OK);
    CHECK(f.cs.used_dwords == 4 && f.cs.reloc_count == 0);
    CHECK(f.words[0] == ref4(0x1240, 3));
    CHECK(std::memcmp(f.words.data() + 1, values, sizeof(values)) == 0);
    before.untouched_tail(f, 4);
    CHECK(std::memcmp(f.relocs.data(), before.relocs.data(), sizeof(f.relocs)) == 0);
}

void test_pkt4_exact_capacity()
{
    Fixture f(4);
    const Snapshot before(f);
    const std::uint32_t values[] = {1, 2, 3};
    CHECK(a840_cs_emit_pkt4(&f.cs, 0, values, 3) == A840_EMIT_OK);
    CHECK(f.cs.used_dwords == 4);
    before.untouched_tail(f, 4);
    const Snapshot full(f);
    CHECK(a840_cs_emit_pkt4(&f.cs, 0, values, 1) == A840_EMIT_CAPACITY);
    full.unchanged(f);
}

void test_pkt4_max_payload()
{
    std::array<std::uint32_t, 127> values{};
    std::array<std::uint32_t, 129> words{};
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = static_cast<std::uint32_t>(i) ^ UINT32_C(0xabcde000);
    words.back() = UINT32_C(0xdeadbeef);
    A840CmdStream cs{};
    CHECK(a840_cs_init(&cs, words.data(), 128, nullptr, 0) == A840_EMIT_OK);
    CHECK(a840_cs_emit_pkt4(&cs, reg_limit - 127, values.data(), 127)
        == A840_EMIT_OK);
    CHECK(cs.used_dwords == 128 && cs.reloc_count == 0);
    CHECK(words[0] == ref4(reg_limit - 127, 127));
    CHECK(std::memcmp(words.data() + 1, values.data(), sizeof(values)) == 0);
    CHECK(words.back() == UINT32_C(0xdeadbeef));
    A840PM4Packet packet{};
    compare_packets(words.data(), cs.used_dwords, &packet, 1, 1);
    CHECK(packet.identifier == reg_limit - 127 && packet.payload_words == 127);
    const auto before = words;
    CHECK(a840_cs_emit_pkt4(&cs, reg_limit - 126, values.data(), 127)
        == A840_EMIT_RANGE_ERROR);
    CHECK(words == before && cs.used_dwords == 128 && cs.reloc_count == 0);
}

void test_pkt4_ranges()
{
    Fixture f;
    const std::uint32_t value = 0x11223344;
    const Snapshot before(f);
    CHECK(a840_cs_emit_pkt4(&f.cs, reg_limit - 1, &value, 2)
        == A840_EMIT_RANGE_ERROR);
    CHECK(a840_cs_emit_pkt4(&f.cs, reg_limit, &value, 1)
        == A840_EMIT_RANGE_ERROR);
    CHECK(a840_cs_emit_pkt4(&f.cs, 0, &value, 0) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_cs_emit_pkt4(&f.cs, 0, &value, 128) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_cs_emit_pkt4(&f.cs, UINT32_MAX, &value, UINT32_MAX)
        == A840_EMIT_RANGE_ERROR);
    before.unchanged(f);
    CHECK(a840_cs_emit_pkt4(&f.cs, reg_limit - 1, &value, 1) == A840_EMIT_OK);
    CHECK(f.words[0] == ref4(reg_limit - 1, 1));
}

void test_pkt4_invalid_arguments()
{
    Fixture f;
    const std::uint32_t value = 7;
    const Snapshot before(f);
    CHECK(a840_cs_emit_pkt4(nullptr, 0, &value, 1) == A840_EMIT_INVALID_ARG);
    CHECK(a840_cs_emit_pkt4(&f.cs, 0, nullptr, 1) == A840_EMIT_INVALID_ARG);
    before.unchanged(f);
}

void test_pkt4_alias()
{
    Fixture f;
    const std::uint32_t value = 7;
    CHECK(a840_cs_emit_pkt4(&f.cs, 0, &value, 1) == A840_EMIT_OK);
    const Snapshot before(f);
    CHECK(a840_cs_emit_pkt4(&f.cs, 0, f.words.data(), 1) == A840_EMIT_ALIAS);
    CHECK(a840_cs_emit_pkt4(&f.cs, 0, f.words.data() + 20, 1) == A840_EMIT_ALIAS);
    CHECK(a840_cs_emit_pkt4(&f.cs, 0,
        reinterpret_cast<const std::uint32_t *>(f.relocs.data()), 1)
        == A840_EMIT_ALIAS);
    CHECK(a840_cs_emit_pkt4(&f.cs, 0,
        reinterpret_cast<const std::uint32_t *>(&f.cs), 1) == A840_EMIT_ALIAS);
    before.unchanged(f);
}

void test_pkt4_rollback()
{
    Fixture f(8);
    CHECK(a840_cs_emit_reg64_va(&f.cs, 0, 5, 0x1000, A840_RELOC_READ)
        == A840_EMIT_OK);
    const std::uint32_t values[] = {1, 2, 3, 4, 5};
    const Snapshot before(f);
    CHECK(a840_cs_emit_pkt4(&f.cs, 4, values, 5) == A840_EMIT_CAPACITY);
    before.unchanged(f);
    CHECK(f.cs.used_dwords == 3 && f.cs.reloc_count == 1);
}

void test_pkt7_basic()
{
    Fixture f;
    const Snapshot before(f);
    const std::uint32_t payload[] = {0x12345678, 0x87654321};
    CHECK(a840_cs_emit_pkt7(&f.cs, 0x46, payload, 2) == A840_EMIT_OK);
    CHECK(f.words[0] == ref7(0x46, 2));
    CHECK(f.words[1] == payload[0] && f.words[2] == payload[1]);
    CHECK(f.cs.used_dwords == 3 && f.cs.reloc_count == 0);
    before.untouched_tail(f, 3);
}

void test_pkt7_zero_payload()
{
    Fixture f(2);
    const Snapshot before(f);
    CHECK(a840_cs_emit_pkt7(&f.cs, 0, nullptr, 0) == A840_EMIT_OK);
    /* An unused zero-length payload may point anywhere, including the CS. */
    CHECK(a840_cs_emit_pkt7(&f.cs, 127, f.words.data(), 0) == A840_EMIT_OK);
    CHECK(f.words[0] == ref7(0, 0) && f.words[1] == ref7(127, 0));
    CHECK(f.cs.used_dwords == 2);
    before.untouched_tail(f, 2);
    const Snapshot full(f);
    CHECK(a840_cs_emit_pkt7(&f.cs, 0, nullptr, 0) == A840_EMIT_CAPACITY);
    full.unchanged(f);
}

void test_pkt7_max_payload()
{
    std::array<std::uint32_t, 16383> payload{};
    std::array<std::uint32_t, 16385> words{};
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload[i] = static_cast<std::uint32_t>(i) ^ UINT32_C(0x55aa1234);
    words.back() = UINT32_C(0xdeadbeef);
    A840CmdStream cs{};
    CHECK(a840_cs_init(&cs, words.data(), 16384, nullptr, 0) == A840_EMIT_OK);
    CHECK(a840_cs_emit_pkt7(&cs, 127, payload.data(), 16383) == A840_EMIT_OK);
    CHECK(cs.used_dwords == 16384 && cs.reloc_count == 0);
    CHECK(words[0] == ref7(127, 16383));
    CHECK(std::memcmp(words.data() + 1, payload.data(), sizeof(payload)) == 0);
    CHECK(words.back() == UINT32_C(0xdeadbeef));
    A840PM4Packet packet{};
    compare_packets(words.data(), cs.used_dwords, &packet, 1, 1);
    CHECK(packet.payload_words == 16383 && packet.identifier == 127);
}

void test_pkt7_ranges()
{
    Fixture f;
    const std::uint32_t payload = 9;
    const Snapshot before(f);
    CHECK(a840_cs_emit_pkt7(&f.cs, 128, &payload, 1) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_cs_emit_pkt7(&f.cs, 0, &payload, 16384) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_cs_emit_pkt7(&f.cs, 0, nullptr, 1) == A840_EMIT_INVALID_ARG);
    CHECK(a840_cs_emit_pkt7(nullptr, 0, nullptr, 0) == A840_EMIT_INVALID_ARG);
    before.unchanged(f);
}

void test_pkt7_alias()
{
    Fixture f;
    const Snapshot before(f);
    CHECK(a840_cs_emit_pkt7(&f.cs, 0, f.words.data() + 1, 2) == A840_EMIT_ALIAS);
    CHECK(a840_cs_emit_pkt7(&f.cs, 0,
        reinterpret_cast<const std::uint32_t *>(f.relocs.data()) + 1, 1)
        == A840_EMIT_ALIAS);
    CHECK(a840_cs_emit_pkt7(&f.cs, 0,
        reinterpret_cast<const std::uint32_t *>(&f.cs) + 1, 1) == A840_EMIT_ALIAS);
    before.unchanged(f);
}

void test_pkt7_rollback()
{
    Fixture f(8);
    CHECK(a840_cs_emit_reg64_va(&f.cs, 0, 5, 0x1000, A840_RELOC_READ)
        == A840_EMIT_OK);
    const std::uint32_t payload[] = {1, 2, 3, 4, 5};
    const Snapshot before(f);
    CHECK(a840_cs_emit_pkt7(&f.cs, 7, payload, 5) == A840_EMIT_CAPACITY);
    before.unchanged(f);
    CHECK(f.cs.used_dwords == 3 && f.cs.reloc_count == 1);
}

void test_reg64_relocation()
{
    Fixture f;
    const Snapshot before(f);
    CHECK(a840_cs_emit_pkt7(&f.cs, 0, nullptr, 0) == A840_EMIT_OK);
    constexpr std::uint64_t va = UINT64_C(0x123456789abc);
    constexpr std::uint64_t bo = UINT64_C(0xfedcba9876543210);
    constexpr std::uint32_t flags = A840_RELOC_READ | A840_RELOC_SHADER;
    CHECK(a840_cs_emit_reg64_va(&f.cs, 0x2040, bo, va, flags) == A840_EMIT_OK);
    CHECK(f.words[1] == ref4(0x2040, 2));
    CHECK(f.words[2] == UINT32_C(0x56789abc) && f.words[3] == UINT32_C(0x1234));
    CHECK(f.cs.used_dwords == 4 && f.cs.reloc_count == 1);
    CHECK(f.relocs[0].dword_offset == 2 && f.relocs[0].flags == flags);
    CHECK(f.relocs[0].bo_handle == bo && f.relocs[0].gpu_va == va);
    before.untouched_tail(f, 4);
    CHECK(std::memcmp(f.relocs.data() + 1, before.relocs.data() + sizeof(A840Reloc),
        3 * sizeof(A840Reloc)) == 0);
}

void test_reg64_optional_relocation()
{
    Fixture f;
    CHECK(a840_cs_init(&f.cs, f.words.data(), 32, nullptr, 0) == A840_EMIT_OK);
    const Snapshot before(f);
    CHECK(a840_cs_emit_reg64_va(&f.cs, 0, 0, 0, 0) == A840_EMIT_OK);
    CHECK(f.words[1] == 0 && f.words[2] == 0 && f.cs.reloc_count == 0);
    CHECK(std::memcmp(f.relocs.data(), before.relocs.data(), sizeof(f.relocs)) == 0);
    CHECK(a840_cs_emit_reg64_va(&f.cs, 2, 7, max_va, UINT32_MAX) == A840_EMIT_OK);
    CHECK(f.words[4] == UINT32_MAX && f.words[5] == 0xffff);
}

void test_reg64_ranges()
{
    Fixture f;
    const Snapshot before(f);
    CHECK(a840_cs_emit_reg64_va(&f.cs, 0, 0, max_va + 1, 0)
        == A840_EMIT_RANGE_ERROR);
    CHECK(a840_cs_emit_reg64_va(&f.cs, reg_limit - 1, 0, 0x1000, 0)
        == A840_EMIT_RANGE_ERROR);
    CHECK(a840_cs_emit_reg64_va(&f.cs, UINT32_MAX, 0, 0, 0)
        == A840_EMIT_RANGE_ERROR);
    CHECK(a840_cs_emit_reg64_va(nullptr, 0, 0, 0, 0) == A840_EMIT_INVALID_ARG);
    before.unchanged(f);
    /* reg64 has no alignment requirement; SDS has a separate 4-byte rule. */
    CHECK(a840_cs_emit_reg64_va(&f.cs, reg_limit - 2, 0, max_va, 0)
        == A840_EMIT_OK);
    CHECK(f.words[0] == ref4(reg_limit - 2, 2));
    CHECK(f.words[1] == UINT32_MAX && f.words[2] == 0xffff);
}

void test_reg64_capacity()
{
    Fixture f(2);
    const Snapshot before(f);
    CHECK(a840_cs_emit_reg64_va(&f.cs, 0, 0, 0x1000, A840_RELOC_READ)
        == A840_EMIT_CAPACITY);
    before.unchanged(f);
    Fixture g(3, 0); /* Non-NULL relocation buffer with zero record capacity. */
    const Snapshot zero(g);
    CHECK(a840_cs_emit_reg64_va(&g.cs, 0, 0, 0x1000, 0) == A840_EMIT_CAPACITY);
    zero.unchanged(g);
}

void test_reg64_full_relocations()
{
    Fixture f(32, 1);
    CHECK(a840_cs_emit_reg64_va(&f.cs, 0, 1, 0x1000, A840_RELOC_WRITE)
        == A840_EMIT_OK);
    const Snapshot before(f);
    CHECK(a840_cs_emit_reg64_va(&f.cs, 2, 2, 0x2000, A840_RELOC_READ)
        == A840_EMIT_CAPACITY);
    before.unchanged(f);
}

void test_reg64_offset_overflow()
{
    A840Reloc relocation{};
    std::memset(&relocation, 0xa5, sizeof(relocation));
    const auto before = relocation;
    /* Synthetic extent: the offset guard must fail before any word access. */
    A840CmdStream cs{reinterpret_cast<std::uint32_t *>(std::uintptr_t{0x1000}),
        static_cast<std::size_t>(UINT32_MAX) + 4, UINT32_MAX, &relocation, 1, 0};
    const auto cs_before = cs;
    CHECK(a840_cs_emit_reg64_va(&cs, 0, 0, 0x1000, 0) == A840_EMIT_RANGE_ERROR);
    CHECK(std::memcmp(&cs, &cs_before, sizeof(cs)) == 0);
    CHECK(std::memcmp(&relocation, &before, sizeof(relocation)) == 0);
}

void test_sds_slot_zero()
{
    Fixture f(4);
    const Snapshot before(f);
    CHECK(a840_cs_emit_sds_group(&f.cs, 0, sds_binning, 7, 0x1000, 1)
        == A840_EMIT_OK);
    CHECK(f.words[0] == UINT32_C(0x70438003));
    CHECK(f.words[1] == UINT32_C(0x00100001));
    CHECK(f.words[2] == 0x1000 && f.words[3] == 0);
    CHECK(f.cs.used_dwords == 4 && f.cs.reloc_count == 1);
    CHECK(f.relocs[0].dword_offset == 2 && f.relocs[0].bo_handle == 7);
    CHECK(f.relocs[0].gpu_va == 0x1000);
    CHECK(f.relocs[0].flags == (A840_RELOC_SDS | A840_RELOC_READ));
    before.untouched_tail(f, 4);
    A840PM4Packet packet{};
    compare_packets(f.words.data(), 4, &packet, 1, 1);
    CHECK(packet.type == 7 && packet.identifier == 0x43 && packet.payload_words == 3);
}

void test_sds_slot_63()
{
    Fixture f;
    CHECK(a840_cs_emit_pkt7(&f.cs, 0, nullptr, 0) == A840_EMIT_OK);
    CHECK(a840_cs_emit_sds_group(&f.cs, 63, sds_binning | sds_gmem | sds_sysmem,
        UINT64_C(0x123456789abcdef0), max_va - 3, 0xffff) == A840_EMIT_OK);
    CHECK(f.words[1] == ref7(0x43, 3));
    CHECK(f.words[2] == UINT32_C(0x3f70ffff));
    CHECK(f.words[3] == UINT32_C(0xfffffffc) && f.words[4] == 0xffff);
    CHECK(f.relocs[0].dword_offset == 3 && f.relocs[0].gpu_va == max_va - 3);
    CHECK(f.relocs[0].bo_handle == UINT64_C(0x123456789abcdef0));
    A840PM4Packet packets[2]{};
    compare_packets(f.words.data(), f.cs.used_dwords, packets, 2, 2);
}

void test_sds_disable()
{
    Fixture f(32, 1);
    CHECK(a840_cs_emit_sds_group(&f.cs, 0, sds_gmem, 1, 0x1000, 10)
        == A840_EMIT_OK);
    const Snapshot before(f);
    /* Disabled groups need no relocation, even when the table is full. */
    CHECK(a840_cs_emit_sds_group(&f.cs, 63, sds_gmem | sds_sysmem, 99, 0, 0)
        == A840_EMIT_OK);
    CHECK(f.words[4] == ref7(0x43, 3));
    CHECK(f.words[5] == UINT32_C(0x3f620000));
    CHECK((f.words[5] & sds_disable) != 0 && (f.words[5] & 0xffff) == 0);
    CHECK(f.words[6] == 0 && f.words[7] == 0);
    CHECK(f.cs.used_dwords == 8 && f.cs.reloc_count == 1);
    CHECK(std::memcmp(f.relocs.data(), before.relocs.data(), sizeof(f.relocs)) == 0);
    CHECK(a840_cs_emit_sds_group(&f.cs, 1, 0, 0, 0x2000, 0) == A840_EMIT_OK);
    CHECK(f.words[9] == UINT32_C(0x01020000) && f.words[10] == 0x2000);
    CHECK(f.cs.reloc_count == 1);
    A840PM4Packet packets[3]{};
    compare_packets(f.words.data(), f.cs.used_dwords, packets, 3, 3);
}

void test_sds_slot_ranges()
{
    Fixture f;
    const Snapshot before(f);
    CHECK(a840_cs_emit_sds_group(&f.cs, 64, sds_gmem, 0, 0x1000, 1)
        == A840_EMIT_RANGE_ERROR);
    CHECK(a840_cs_emit_sds_group(&f.cs, UINT32_MAX, sds_gmem, 0, 0, 0)
        == A840_EMIT_RANGE_ERROR);
    CHECK(a840_cs_emit_sds_group(nullptr, 0, 0, 0, 0, 0) == A840_EMIT_INVALID_ARG);
    before.unchanged(f);
}

void test_sds_address_ranges()
{
    Fixture f;
    const Snapshot before(f);
    CHECK(a840_cs_emit_sds_group(&f.cs, 0, sds_gmem, 0, max_va + 1, 1)
        == A840_EMIT_RANGE_ERROR);
    CHECK(a840_cs_emit_sds_group(&f.cs, 0, sds_gmem, 0, 0x1001, 1)
        == A840_EMIT_ALIGNMENT);
    CHECK(a840_cs_emit_sds_group(&f.cs, 0, sds_gmem, 0, 0x1002, 0)
        == A840_EMIT_ALIGNMENT);
    CHECK(a840_cs_emit_sds_group(&f.cs, 0, sds_gmem, 0, 0, 1)
        == A840_EMIT_INVALID_ARG);
    before.unchanged(f);
}

void test_sds_count_ranges()
{
    Fixture f;
    const Snapshot before(f);
    CHECK(a840_cs_emit_sds_group(&f.cs, 0, 0, 0, 0x1000, 0x10000)
        == A840_EMIT_RANGE_ERROR);
    CHECK(a840_cs_emit_sds_group(&f.cs, 0, 0, 0, 0x1000, UINT32_MAX)
        == A840_EMIT_RANGE_ERROR);
    before.unchanged(f);
}

void test_sds_enable_masks()
{
    Fixture f;
    CHECK(a840_cs_init(&f.cs, f.words.data(), 32, nullptr, 0) == A840_EMIT_OK);
    for (std::uint32_t mask = 0; mask < 8; ++mask) {
        CHECK(a840_cs_emit_sds_group(&f.cs, mask, mask << 20, 0, 0x1000, 3)
            == A840_EMIT_OK);
        CHECK(f.words[mask * 4 + 1] == ((mask << 24) | (mask << 20) | 3));
    }
    std::array<A840PM4Packet, 8> packets{};
    compare_packets(f.words.data(), 32, packets.data(), packets.size(), 8);
    Fixture g;
    const Snapshot before(g);
    constexpr std::uint32_t invalid[] = {1, 1u << 16, 1u << 17, 1u << 19,
        1u << 23, 1u << 24, UINT32_MAX};
    for (auto mask : invalid) {
        CHECK(a840_cs_emit_sds_group(&g.cs, 0, mask, 0, 0x1000, 1)
            == A840_EMIT_RANGE_ERROR);
        before.unchanged(g);
    }
}

void test_sds_capacity()
{
    Fixture f(3);
    const Snapshot before(f);
    CHECK(a840_cs_emit_sds_group(&f.cs, 0, sds_gmem, 0, 0x1000, 1)
        == A840_EMIT_CAPACITY);
    before.unchanged(f);
    Fixture g(32, 1);
    CHECK(a840_cs_emit_reg64_va(&g.cs, 0, 0, 0x1000, 0) == A840_EMIT_OK);
    const Snapshot full(g);
    CHECK(a840_cs_emit_sds_group(&g.cs, 0, sds_gmem, 0, 0x1000, 1)
        == A840_EMIT_CAPACITY);
    full.unchanged(g);
}

void test_sds_optional_relocation()
{
    Fixture f;
    CHECK(a840_cs_init(&f.cs, f.words.data(), 32, nullptr, 0) == A840_EMIT_OK);
    const Snapshot before(f);
    CHECK(a840_cs_emit_sds_group(&f.cs, 12, sds_sysmem, 0, 0x1234000, 55)
        == A840_EMIT_OK);
    CHECK(f.cs.reloc_count == 0 && f.cs.used_dwords == 4);
    CHECK(f.words[1] == UINT32_C(0x0c400037));
    CHECK(std::memcmp(f.relocs.data(), before.relocs.data(), sizeof(f.relocs)) == 0);
    Fixture g(4, 0);
    CHECK(a840_cs_emit_sds_group(&g.cs, 0, 0, 0, 0, 0) == A840_EMIT_OK);
    CHECK(g.cs.reloc_count == 0 && g.cs.used_dwords == 4);
}

void test_sds_offset_overflow()
{
    A840Reloc relocation{};
    const auto before = relocation;
    A840CmdStream cs{reinterpret_cast<std::uint32_t *>(std::uintptr_t{0x1000}),
        static_cast<std::size_t>(UINT32_MAX) + 4,
        static_cast<std::size_t>(UINT32_MAX) - 1, &relocation, 1, 0};
    const auto cs_before = cs;
    CHECK(a840_cs_emit_sds_group(&cs, 0, 0, 0, 0x1000, 1)
        == A840_EMIT_RANGE_ERROR);
    CHECK(std::memcmp(&cs, &cs_before, sizeof(cs)) == 0);
    CHECK(std::memcmp(&relocation, &before, sizeof(relocation)) == 0);
}

void test_decode_roundtrip()
{
    Fixture f;
    const std::uint32_t values[] = {0x11223344, 0x55667788};
    CHECK(a840_cs_emit_pkt7(&f.cs, 0x13, nullptr, 0) == A840_EMIT_OK);
    CHECK(a840_cs_emit_pkt4(&f.cs, 0x400, values, 2) == A840_EMIT_OK);
    CHECK(a840_cs_emit_reg64_va(&f.cs, 0x800, 19, UINT64_C(0xabcd12340000),
        A840_RELOC_READ) == A840_EMIT_OK);
    CHECK(a840_cs_emit_sds_group(&f.cs, 63, sds_gmem, 21, 0x1000, 4)
        == A840_EMIT_OK);
    CHECK(a840_cs_emit_sds_group(&f.cs, 0, sds_gmem, 0, 0, 0) == A840_EMIT_OK);
    std::array<A840PM4Packet, 6> packets{};
    std::memset(packets.data(), 0xa5, sizeof(packets));
    const auto before = packets;
    compare_packets(f.words.data(), f.cs.used_dwords, packets.data(), 6, 5);
    const std::size_t offsets[] = {0, 1, 4, 7, 11};
    for (std::size_t i = 0; i < 5; ++i)
        CHECK(packets[i].header_word == offsets[i]);
    CHECK(packets[1].type == 4 && packets[1].identifier == 0x400);
    CHECK(packets[3].identifier == 0x43 && packets[4].payload_words == 3);
    CHECK(f.relocs[0].dword_offset == 5 && f.relocs[1].dword_offset == 9);
    CHECK(std::memcmp(&packets[5], &before[5], sizeof(packets[5])) == 0);
}

void test_decode_empty()
{
    std::size_t count = 999;
    CHECK(a840_pm4_decode(nullptr, 0, nullptr, 0, &count) == A840_EMIT_OK);
    CHECK(count == 0);
    const std::uint32_t unused = UINT32_C(0xdeadbeef);
    A840PM4Packet packet{};
    std::memset(&packet, 0xa5, sizeof(packet));
    const auto before = packet;
    CHECK(a840_pm4_decode(&unused, 0, &packet, 1, &count) == A840_EMIT_OK);
    CHECK(count == 0 && std::memcmp(&packet, &before, sizeof(packet)) == 0);
    packet.header_word = 999;
    /* Zero-capacity output occupies no storage; its pointer is unused. */
    CHECK(a840_pm4_decode(nullptr, 0, &packet, 0, &packet.header_word)
        == A840_EMIT_OK);
    CHECK(packet.header_word == 0);
}

void test_decode_missing_arguments()
{
    const std::uint32_t word = ref7(0, 0);
    decode_error(nullptr, 1, A840_EMIT_INVALID_ARG);
    A840PM4Packet packet{};
    std::memset(&packet, 0xa5, sizeof(packet));
    const auto before = packet;
    CHECK(a840_pm4_decode(&word, 1, &packet, 1, nullptr) == A840_EMIT_INVALID_ARG);
    CHECK(std::memcmp(&packet, &before, sizeof(packet)) == 0);
    std::size_t count = 999;
    CHECK(a840_pm4_decode(&word, 1, nullptr, 1, &count) == A840_EMIT_INVALID_ARG);
    CHECK(count == 0);
    count = 999;
    CHECK(a840_pm4_decode(nullptr, 0, nullptr, 1, &count) == A840_EMIT_INVALID_ARG);
    CHECK(count == 0);
}

void test_decode_capacity()
{
    const std::uint32_t words[] = {ref7(0, 0), ref4(1, 1), 9};
    decode_error(words, 3, A840_EMIT_CAPACITY, 1);
    std::size_t count = 999;
    CHECK(a840_pm4_decode(words, 3, nullptr, 0, &count) == A840_EMIT_CAPACITY);
    CHECK(count == 0);
}

void test_decode_truncation()
{
    const std::uint32_t pkt4[] = {ref4(2, 2), 0x1234};
    const std::uint32_t pkt7[] = {ref7(0x43, 3), 0, 0};
    decode_error(pkt4, 2, A840_EMIT_RANGE_ERROR);
    decode_error(pkt7, 3, A840_EMIT_RANGE_ERROR);
    const std::uint32_t max_count = ref7(127, 16383);
    decode_error(&max_count, 1, A840_EMIT_RANGE_ERROR);
}

void test_decode_type4_parity()
{
    std::array<std::uint32_t, 2> words{{ref4(0x1234, 1), 9}};
    words[0] ^= UINT32_C(1) << 7;
    decode_error(words.data(), words.size(), A840_EMIT_INVALID_ARG);
    words[0] = ref4(0x1234, 1) ^ (UINT32_C(1) << 27);
    decode_error(words.data(), words.size(), A840_EMIT_INVALID_ARG);
}

void test_decode_type7_parity()
{
    std::array<std::uint32_t, 2> words{{ref7(0x43, 1), 9}};
    words[0] ^= UINT32_C(1) << 15;
    decode_error(words.data(), words.size(), A840_EMIT_INVALID_ARG);
    words[0] = ref7(0x43, 1) ^ (UINT32_C(1) << 23);
    decode_error(words.data(), words.size(), A840_EMIT_INVALID_ARG);
}

void test_decode_single_bit_damage()
{
    const std::uint32_t headers[] = {ref4(0x1234, 3), ref7(0x43, 3)};
    for (auto header : headers) {
        for (std::uint32_t bit = 0; bit < 32; ++bit) {
            const std::uint32_t words[] = {header ^ (UINT32_C(1) << bit), 1, 2, 3};
            const auto type = words[0] >> 28;
            const auto status = type == 4 || type == 7 ? A840_EMIT_INVALID_ARG
                                                     : A840_EMIT_UNSUPPORTED;
            decode_error(words, 4, status);
        }
    }
}

void test_decode_reserved_bits()
{
    const std::uint32_t pkt4[] = {ref4(1, 1) | (UINT32_C(1) << 26), 9};
    const std::uint32_t pkt7[] = {ref7(1, 1) | (UINT32_C(1) << 14), 9};
    decode_error(pkt4, 2, A840_EMIT_INVALID_ARG);
    decode_error(pkt7, 2, A840_EMIT_INVALID_ARG);
}

void test_decode_invalid_type4()
{
    const std::uint32_t zero_count = UINT32_C(0x48000080);
    decode_error(&zero_count, 1, A840_EMIT_INVALID_ARG);
    std::uint32_t header = 0;
    CHECK(a840_pm4_type4_header(reg_limit - 1, 2, &header) == A840_EMIT_OK);
    const std::uint32_t wrapped[] = {header, 1, 2};
    decode_error(wrapped, 3, A840_EMIT_INVALID_ARG);
}

void test_decode_unsupported_type()
{
    for (std::uint32_t type = 0; type < 16; ++type) {
        if (type == 4 || type == 7)
            continue;
        const std::uint32_t word = type << 28;
        decode_error(&word, 1, A840_EMIT_UNSUPPORTED);
    }
}

void test_decode_input_output_alias()
{
    alignas(A840PM4Packet) std::array<std::uint32_t, 32> words{};
    words[0] = ref7(0x43, 3);
    const auto before = words;
    std::size_t count = 999;
    CHECK(a840_pm4_decode(words.data(), 4,
        reinterpret_cast<A840PM4Packet *>(words.data()), 1, &count)
        == A840_EMIT_ALIAS);
    CHECK(count == 0 && words == before);
    count = 999;
    CHECK(a840_pm4_decode(words.data(), 4,
        reinterpret_cast<A840PM4Packet *>(words.data() + 2), 1, &count)
        == A840_EMIT_ALIAS);
    CHECK(count == 0 && words == before);
    /* Reject overlap with the full declared output capacity, not just 1 record. */
    words[12] = ref7(0, 0);
    const auto tail_before = words;
    count = 999;
    CHECK(a840_pm4_decode(words.data() + 12, 1,
        reinterpret_cast<A840PM4Packet *>(words.data()), 3, &count)
        == A840_EMIT_ALIAS);
    CHECK(count == 0 && words == tail_before);
}

void test_decode_count_alias()
{
    alignas(std::size_t) std::array<std::uint32_t, 4> words{{ref7(0, 0), 7, 8, 9}};
    const auto before_words = words;
    A840PM4Packet packet{};
    std::memset(&packet, 0xa5, sizeof(packet));
    const auto before_packet = packet;
    /* Invalid count aliases are rejected before the count write. Otherwise
     * zeroing count would destroy immutable input or the packet sentinel. */
    CHECK(a840_pm4_decode(words.data(), 1, &packet, 1,
        reinterpret_cast<std::size_t *>(words.data())) == A840_EMIT_ALIAS);
    CHECK(words == before_words);
    CHECK(std::memcmp(&packet, &before_packet, sizeof(packet)) == 0);
    CHECK(a840_pm4_decode(words.data(), 1, &packet, 1, &packet.header_word)
        == A840_EMIT_ALIAS);
    CHECK(words == before_words);
    CHECK(std::memcmp(&packet, &before_packet, sizeof(packet)) == 0);
}

void test_decode_alignment()
{
    alignas(A840PM4Packet) std::array<unsigned char, 64> bytes{};
    bytes.fill(0xa5);
    const auto before = bytes;
    const std::uint32_t word = ref7(0, 0);
    decode_error(reinterpret_cast<const std::uint32_t *>(bytes.data() + 1), 1,
        A840_EMIT_ALIGNMENT);
    std::size_t count = 999;
    CHECK(a840_pm4_decode(&word, 1,
        reinterpret_cast<A840PM4Packet *>(bytes.data() + 1), 1, &count)
        == A840_EMIT_ALIGNMENT);
    CHECK(count == 0 && bytes == before);
    CHECK(a840_pm4_decode(&word, 1, nullptr, 0,
        reinterpret_cast<std::size_t *>(bytes.data() + 1)) == A840_EMIT_ALIGNMENT);
    CHECK(bytes == before);
}

void test_decode_range_overflow()
{
    constexpr auto size_max = std::numeric_limits<std::size_t>::max();
    auto *invalid_words = reinterpret_cast<const std::uint32_t *>(UINTPTR_MAX - 3);
    decode_error(invalid_words, size_max / sizeof(std::uint32_t) + 1,
        A840_EMIT_RANGE_ERROR);
    decode_error(invalid_words, 1, A840_EMIT_RANGE_ERROR);
    auto *invalid_packets = reinterpret_cast<A840PM4Packet *>(UINTPTR_MAX - 7);
    std::size_t count = 999;
    CHECK(a840_pm4_decode(nullptr, 0, invalid_packets,
        size_max / sizeof(A840PM4Packet) + 1, &count) == A840_EMIT_RANGE_ERROR);
    CHECK(count == 0);
    count = 999;
    CHECK(a840_pm4_decode(nullptr, 0, invalid_packets, 1, &count)
        == A840_EMIT_RANGE_ERROR);
    CHECK(count == 0);
}

void test_decode_bad_tail_transaction()
{
    std::array<std::uint32_t, 4> words{{ref7(0, 0), ref4(1, 1), 99,
        ref7(0x43, 3)}};
    decode_error(words.data(), words.size(), A840_EMIT_RANGE_ERROR);
    /* Scan errors take priority over insufficient output capacity. */
    decode_error(words.data(), words.size(), A840_EMIT_RANGE_ERROR, 0);
    words.back() = ref7(0, 0) ^ (UINT32_C(1) << 15);
    decode_error(words.data(), words.size(), A840_EMIT_INVALID_ARG, 0);
    words.back() = UINT32_C(0x20000000);
    decode_error(words.data(), words.size(), A840_EMIT_UNSUPPORTED);
}

void test_emit_corrupt_cs()
{
    Fixture f;
    const std::uint32_t value = 1;
    f.cs.used_dwords = f.cs.capacity_dwords + 1;
    const Snapshot bad_used(f);
    CHECK(a840_cs_emit_pkt4(&f.cs, 0, &value, 1) == A840_EMIT_INVALID_ARG);
    CHECK(a840_cs_emit_pkt7(&f.cs, 0, nullptr, 0) == A840_EMIT_INVALID_ARG);
    CHECK(a840_cs_emit_reg64_va(&f.cs, 0, 0, 0, 0) == A840_EMIT_INVALID_ARG);
    CHECK(a840_cs_emit_sds_group(&f.cs, 0, 0, 0, 0, 0) == A840_EMIT_INVALID_ARG);
    bad_used.unchanged(f);
    f.cs.used_dwords = 0;
    f.cs.reloc_count = f.cs.reloc_capacity + 1;
    const Snapshot bad_reloc(f);
    CHECK(a840_cs_emit_pkt7(&f.cs, 0, nullptr, 0) == A840_EMIT_INVALID_ARG);
    bad_reloc.unchanged(f);
    f.cs.reloc_count = 0;
    f.cs.words = nullptr;
    const Snapshot missing_words(f);
    CHECK(a840_cs_emit_pkt7(&f.cs, 0, nullptr, 0) == A840_EMIT_INVALID_ARG);
    missing_words.unchanged(f);
    f.cs.words = f.words.data();
    f.cs.relocs = nullptr;
    const Snapshot missing_relocs(f);
    CHECK(a840_cs_emit_pkt7(&f.cs, 0, nullptr, 0) == A840_EMIT_INVALID_ARG);
    missing_relocs.unchanged(f);
    f.cs.relocs = reinterpret_cast<A840Reloc *>(f.words.data());
    const Snapshot alias(f);
    CHECK(a840_cs_emit_pkt7(&f.cs, 0, nullptr, 0) == A840_EMIT_ALIAS);
    alias.unchanged(f);
}

void test_emit_alignment()
{
    Fixture f;
    alignas(std::uint32_t) std::array<unsigned char, 8> payload{};
    const Snapshot before(f);
    auto *unaligned = reinterpret_cast<const std::uint32_t *>(payload.data() + 1);
    CHECK(a840_cs_emit_pkt4(&f.cs, 0, unaligned, 1) == A840_EMIT_ALIGNMENT);
    CHECK(a840_cs_emit_pkt7(&f.cs, 0, unaligned, 1) == A840_EMIT_ALIGNMENT);
    before.unchanged(f);
    auto *cs_bytes = reinterpret_cast<unsigned char *>(&f.cs);
    CHECK(a840_cs_emit_pkt7(reinterpret_cast<A840CmdStream *>(cs_bytes + 1),
        0, nullptr, 0) == A840_EMIT_ALIGNMENT);
    before.unchanged(f);
    f.cs.words = reinterpret_cast<std::uint32_t *>(
        reinterpret_cast<unsigned char *>(f.words.data()) + 1);
    const Snapshot malformed(f);
    CHECK(a840_cs_emit_reg64_va(&f.cs, 0, 0, 0x1000, 0) == A840_EMIT_ALIGNMENT);
    malformed.unchanged(f);
}

void test_emit_failure_then_success()
{
    Fixture f;
    CHECK(a840_cs_emit_reg64_va(&f.cs, 0, 1, 0x1000, A840_RELOC_READ)
        == A840_EMIT_OK);
    const Snapshot before(f);
    for (std::uint32_t i = 0; i < 16; ++i) {
        CHECK(a840_cs_emit_sds_group(&f.cs, 64, sds_gmem, 0, 0x2000, 1)
            == A840_EMIT_RANGE_ERROR);
        CHECK(a840_cs_emit_pkt4(&f.cs, 0, nullptr, 1) == A840_EMIT_INVALID_ARG);
        CHECK(a840_cs_emit_pkt7(&f.cs, 0, f.words.data(), 1) == A840_EMIT_ALIAS);
        before.unchanged(f);
    }
    CHECK(a840_cs_emit_sds_group(&f.cs, 1, sds_sysmem, 2, 0x2000, 7)
        == A840_EMIT_OK);
    CHECK(f.cs.used_dwords == 7 && f.cs.reloc_count == 2);
    CHECK(f.relocs[1].dword_offset == 5 && f.relocs[1].gpu_va == 0x2000);
    before.untouched_tail(f, 7);
}

void test_adjacent_payload_and_decode()
{
    struct Storage {
        std::uint32_t words[4];
        std::uint32_t payload[3];
    } storage{{0, 0, 0, 0}, {1, 2, 3}};
    static_assert(offsetof(Storage, payload) == sizeof(storage.words), "adjacent");
    A840CmdStream cs{};
    CHECK(a840_cs_init(&cs, storage.words, 4, nullptr, 0) == A840_EMIT_OK);
    CHECK(a840_cs_emit_pkt4(&cs, 0, storage.payload, 3) == A840_EMIT_OK);
    CHECK(storage.words[3] == 3 && storage.payload[0] == 1);
    struct DecodeStorage {
        std::uint32_t words[2];
        A840PM4Packet packet;
        std::size_t count;
    } decoded{};
    static_assert(offsetof(DecodeStorage, packet) == sizeof(decoded.words),
        "adjacent decoder storage");
    decoded.words[0] = ref4(1, 1);
    decoded.words[1] = 99;
    decoded.count = 999;
    CHECK(a840_pm4_decode(decoded.words, 2, &decoded.packet, 1, &decoded.count)
        == A840_EMIT_OK);
    CHECK(decoded.count == 1 && decoded.packet.identifier == 1);
    CHECK(decoded.words[0] == ref4(1, 1) && decoded.words[1] == 99);
}

std::uint32_t random_word(std::uint32_t &state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void test_deterministic_streams()
{
    std::uint32_t random = UINT32_C(0x12345678);
    for (std::size_t trial = 0; trial < 16; ++trial) {
        std::array<std::uint32_t, 4096> words{};
        std::array<A840Reloc, 256> relocs{};
        std::array<A840PM4Packet, 200> packets{};
        std::array<std::size_t, 200> offsets{};
        const std::uint32_t payload[16] = {0, 1, 2, 3, 4, 5, 6, 7,
            8, 9, 10, 11, 12, 13, 14, 15};
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(),
            relocs.size()) == A840_EMIT_OK);
        for (std::size_t i = 0; i < offsets.size(); ++i) {
            offsets[i] = cs.used_dwords;
            const auto reg = random_word(random) % (reg_limit - 127);
            const auto kind = random_word(random) % 4;
            if (kind == 0) {
                const auto count = random_word(random) % 16 + 1;
                CHECK(a840_cs_emit_pkt4(&cs, reg, payload, count) == A840_EMIT_OK);
                CHECK(words[offsets[i]] == ref4(reg, count));
                CHECK(std::memcmp(words.data() + offsets[i] + 1, payload,
                    count * sizeof(payload[0])) == 0);
            } else if (kind == 1) {
                const auto count = random_word(random) % 17;
                const auto opcode = random_word(random) % 128;
                CHECK(a840_cs_emit_pkt7(&cs, opcode, payload, count) == A840_EMIT_OK);
                CHECK(words[offsets[i]] == ref7(opcode, count));
            } else if (kind == 2) {
                const auto va = static_cast<std::uint64_t>(random_word(random)) << 4;
                CHECK(a840_cs_emit_reg64_va(&cs, reg, i, va, A840_RELOC_READ)
                    == A840_EMIT_OK);
                CHECK(relocs[cs.reloc_count - 1].dword_offset == offsets[i] + 1);
            } else {
                const auto group = random_word(random) % 64;
                const auto va = static_cast<std::uint64_t>(random_word(random) | 1) << 4;
                CHECK(a840_cs_emit_sds_group(&cs, group, sds_gmem, i, va, 15)
                    == A840_EMIT_OK);
                CHECK(relocs[cs.reloc_count - 1].dword_offset == offsets[i] + 2);
            }
        }
        compare_packets(words.data(), cs.used_dwords, packets.data(), packets.size(),
            offsets.size());
        for (std::size_t i = 0; i < offsets.size(); ++i)
            CHECK(packets[i].header_word == offsets[i]);
    }
}

struct TestCase {
    const char *name;
    void (*run)();
};

#define TEST(name) {#name, name}

const TestCase tests[] = {
    TEST(test_init_valid),
    TEST(test_init_empty),
    TEST(test_init_missing_arguments),
    TEST(test_init_storage_alias),
    TEST(test_init_metadata_alias),
    TEST(test_init_adjacent_storage),
    TEST(test_init_range_overflow),
    TEST(test_init_alignment),
    TEST(test_type4_reference),
    TEST(test_type4_boundaries),
    TEST(test_type7_reference),
    TEST(test_type7_boundaries),
    TEST(test_header_alignment),
    TEST(test_pkt4_basic),
    TEST(test_pkt4_exact_capacity),
    TEST(test_pkt4_max_payload),
    TEST(test_pkt4_ranges),
    TEST(test_pkt4_invalid_arguments),
    TEST(test_pkt4_alias),
    TEST(test_pkt4_rollback),
    TEST(test_pkt7_basic),
    TEST(test_pkt7_zero_payload),
    TEST(test_pkt7_max_payload),
    TEST(test_pkt7_ranges),
    TEST(test_pkt7_alias),
    TEST(test_pkt7_rollback),
    TEST(test_reg64_relocation),
    TEST(test_reg64_optional_relocation),
    TEST(test_reg64_ranges),
    TEST(test_reg64_capacity),
    TEST(test_reg64_full_relocations),
    TEST(test_reg64_offset_overflow),
    TEST(test_sds_slot_zero),
    TEST(test_sds_slot_63),
    TEST(test_sds_disable),
    TEST(test_sds_slot_ranges),
    TEST(test_sds_address_ranges),
    TEST(test_sds_count_ranges),
    TEST(test_sds_enable_masks),
    TEST(test_sds_capacity),
    TEST(test_sds_optional_relocation),
    TEST(test_sds_offset_overflow),
    TEST(test_decode_roundtrip),
    TEST(test_decode_empty),
    TEST(test_decode_missing_arguments),
    TEST(test_decode_capacity),
    TEST(test_decode_truncation),
    TEST(test_decode_type4_parity),
    TEST(test_decode_type7_parity),
    TEST(test_decode_single_bit_damage),
    TEST(test_decode_reserved_bits),
    TEST(test_decode_invalid_type4),
    TEST(test_decode_unsupported_type),
    TEST(test_decode_input_output_alias),
    TEST(test_decode_count_alias),
    TEST(test_decode_alignment),
    TEST(test_decode_range_overflow),
    TEST(test_decode_bad_tail_transaction),
    TEST(test_emit_corrupt_cs),
    TEST(test_emit_alignment),
    TEST(test_emit_failure_then_success),
    TEST(test_adjacent_payload_and_decode),
    TEST(test_deterministic_streams),
};

static_assert(sizeof(tests) / sizeof(tests[0]) >= 25, "at least 25 independent tests");

} // namespace

int main()
{
    for (const auto &test : tests) {
        test.run();
        std::printf("PASS %s\n", test.name);
    }
    std::printf("PASS: %zu independent test cases, %zu assertions\n",
        sizeof(tests) / sizeof(tests[0]), assertions);
    return 0;
}
