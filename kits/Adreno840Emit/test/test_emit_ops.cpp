/* SPDX-License-Identifier: MIT */
#include "a840_emit_ops.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"
#include "../Adreno840/chip_facts_a840.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

std::size_t assertions = 0;
const char *dump_path = nullptr;

[[noreturn]] void fail(const char *expr, int line)
{
    std::fprintf(stderr, "test_emit_ops.cpp:%d: %s\n", line, expr);
    std::abort();
}

#define CHECK(expr) do { ++assertions; if (!(expr)) fail(#expr, __LINE__); } while (false)

constexpr std::uint64_t va_limit = UINT64_C(1) << 48;
constexpr std::uint32_t all_caches = A840_CACHE_UCHE_INV | A840_CACHE_CCU_FLUSH_COLOR |
    A840_CACHE_CCU_FLUSH_DEPTH | A840_CACHE_CCU_INV_COLOR;

struct Fixture {
    std::array<std::uint32_t, 512> words{};
    std::array<A840Reloc, 64> relocs{};
    A840CmdStream cs{};
    explicit Fixture(std::size_t capacity = 512, std::size_t reloc_capacity = 64)
    {
        for (std::size_t i = 0; i < words.size(); ++i)
            words[i] = UINT32_C(0xde000000) + static_cast<std::uint32_t>(i);
        std::memset(relocs.data(), 0xa5, sizeof(relocs));
        CHECK(a840_cs_init(&cs, words.data(), capacity, relocs.data(), reloc_capacity)
            == A840_EMIT_OK);
    }
};

struct Snapshot {
    std::array<unsigned char, sizeof(Fixture::words)> words{};
    std::array<unsigned char, sizeof(Fixture::relocs)> relocs{};
    std::array<unsigned char, sizeof(A840CmdStream)> cs{};
    explicit Snapshot(const Fixture &f)
    {
        std::memcpy(words.data(), f.words.data(), words.size());
        std::memcpy(relocs.data(), f.relocs.data(), relocs.size());
        std::memcpy(cs.data(), &f.cs, cs.size());
    }
    void unchanged(const Fixture &f) const
    {
        CHECK(std::memcmp(words.data(), f.words.data(), words.size()) == 0);
        CHECK(std::memcmp(relocs.data(), f.relocs.data(), relocs.size()) == 0);
        CHECK(std::memcmp(cs.data(), &f.cs, cs.size()) == 0);
    }
    void tail(const Fixture &f) const
    {
        const auto at = f.cs.used_dwords;
        CHECK(std::memcmp(words.data() + at * 4, f.words.data() + at,
            words.size() - at * 4) == 0);
        const auto reloc_at = f.cs.reloc_count * sizeof(A840Reloc);
        CHECK(std::memcmp(relocs.data() + reloc_at, f.relocs.data() + f.cs.reloc_count,
            relocs.size() - reloc_at) == 0);
    }
};

A840TexDescInput texture()
{
    A840TexDescInput in{};
    in.format_id = A840_FMT6_8_8_8_8_UNORM;
    in.type = A840_TEX_2D;
    in.width = 64;
    in.height = 32;
    in.depth = 1;
    in.mip_levels = 1;
    in.samples = 1;
    in.pitch = 256;
    in.slice_stride = 8192;
    in.gpu_va = UINT64_C(0x123456780000);
    in.highest_bank_bit = A840_KAANAPALI_HIGHEST_BANK_BIT;
    for (std::uint32_t i = 0; i < 4; ++i) in.swizzle[i] = i;
    return in;
}

void ubwc_texture(A840TexDescInput &in)
{
    in.tile_mode = A840_TILE_UBWC;
    in.ubwc_meta_va = UINT64_C(0x223456780000);
    in.ubwc_meta_pitch = 64;
    in.ubwc_meta_slice_stride = 4096;
    in.ubwc_block_width = 16;
    in.ubwc_block_height = 4;
}

A840SampDescInput sampler()
{
    A840SampDescInput in{};
    in.min_filter = A840_FILTER_LINEAR;
    in.mag_filter = A840_FILTER_LINEAR;
    in.mip_filter = A840_MIP_LINEAR;
    in.max_aniso = 1;
    in.max_lod = 7.5f;
    in.compare_func = A840_COMPARE_DISABLED;
    return in;
}

A840Surface2D surface(std::uint64_t va)
{
    A840Surface2D value{};
    value.bo_handle = va / 4096;
    value.gpu_va = va;
    value.format_id = A840_FMT6_8_8_8_8_UNORM;
    value.width = 64;
    value.height = 32;
    value.pitch = 256;
    return value;
}

void ubwc_surface(A840Surface2D &value)
{
    value.tile_mode = A840_TILE_UBWC;
    value.ubwc_meta_va = value.gpu_va + UINT64_C(0x80000);
    value.ubwc_meta_pitch = 64;
    value.ubwc_meta_slice_stride = 4096;
}

A840Clear2DDesc clear_desc()
{
    A840Clear2DDesc desc{};
    desc.dst = surface(UINT64_C(0x200000));
    desc.rect = {3, 4, 32, 16};
    desc.color[0] = 0x12;
    desc.color[1] = 0x34;
    desc.color[2] = 0x56;
    desc.color[3] = 0xff;
    return desc;
}

A840Blit2DDesc blit_desc()
{
    A840Blit2DDesc desc{};
    desc.src = surface(UINT64_C(0x400000));
    desc.dst = surface(UINT64_C(0x600000));
    desc.src_rect = {2, 3, 40, 20};
    desc.dst_rect = {4, 5, 20, 10};
    desc.filter = A840_FILTER_LINEAR;
    return desc;
}

template <typename Input, std::size_t N, typename Encoder>
void descriptor_error(const Input &in, A840EmitStatus expected, Encoder encode)
{
    std::array<std::uint32_t, N + 2> output{};
    output.fill(UINT32_C(0xa5b6c7d8));
    const auto before = output;
    CHECK(encode(&in, output.data() + 1) == expected);
    CHECK(output == before);
}

void tex_error(const A840TexDescInput &in, A840EmitStatus expected)
{
    descriptor_error<A840TexDescInput, 16>(in, expected, a840_encode_tex_const);
}

void samp_error(const A840SampDescInput &in, A840EmitStatus expected)
{
    descriptor_error<A840SampDescInput, 4>(in, expected, a840_encode_samp_const);
}

std::array<A840PM4Packet, 256> decode(const A840CmdStream &cs, std::size_t &count)
{
    std::array<A840PM4Packet, 256> packets{};
    count = 999;
    CHECK(a840_pm4_decode(cs.words, cs.used_dwords, packets.data(), packets.size(), &count)
        == A840_EMIT_OK);
    return packets;
}

std::uint32_t read_reg(const A840CmdStream &cs, std::uint32_t reg)
{
    std::size_t count = 0;
    const auto packets = decode(cs, count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto &p = packets[i];
        if (p.type == 4 && reg >= p.identifier && reg - p.identifier < p.payload_words)
            return cs.words[p.header_word + 1 + reg - p.identifier];
    }
    fail("register absent from emitted stream", __LINE__);
}

void no_wfi(const A840CmdStream &cs)
{
    std::size_t count = 0;
    const auto packets = decode(cs, count);
    for (std::size_t i = 0; i < count; ++i)
        CHECK(packets[i].type != 7 || packets[i].identifier != A840_CP_WAIT_FOR_IDLE);
}

void check_relocs(const A840CmdStream &cs)
{
    for (std::size_t i = 0; i < cs.reloc_count; ++i) {
        const auto &reloc = cs.relocs[i];
        CHECK(static_cast<std::size_t>(reloc.dword_offset) + 1 < cs.used_dwords);
        CHECK(cs.words[reloc.dword_offset] == static_cast<std::uint32_t>(reloc.gpu_va));
        CHECK(cs.words[reloc.dword_offset + 1] == static_cast<std::uint32_t>(reloc.gpu_va >> 32));
    }
}

template <typename Emit>
void word_capacity(Emit emit, std::size_t budget)
{
    Fixture reference;
    const Snapshot before(reference);
    CHECK(emit(&reference.cs) == A840_EMIT_OK);
    const auto size = reference.cs.used_dwords;
    CHECK(size > 0 && size <= budget);
    before.tail(reference);
    for (std::size_t available = 0; available < size; ++available) {
        Fixture f(5 + available);
        CHECK(a840_emit_mem_write64(&f.cs, 3, 0x1800000, 19) == A840_EMIT_OK);
        const Snapshot snapshot(f);
        CHECK(emit(&f.cs) == A840_EMIT_CAPACITY);
        snapshot.unchanged(f);
    }
    Fixture exact(size + 1);
    CHECK(a840_emit_wfi(&exact.cs) == A840_EMIT_OK);
    const Snapshot exact_before(exact);
    CHECK(emit(&exact.cs) == A840_EMIT_OK);
    CHECK(exact.cs.used_dwords == size + 1);
    CHECK(std::memcmp(exact.words.data() + 1, reference.words.data(), size * 4) == 0);
    CHECK(exact.cs.reloc_count == reference.cs.reloc_count);
    for (std::size_t i = 0; i < exact.cs.reloc_count; ++i)
        CHECK(exact.relocs[i].dword_offset == reference.relocs[i].dword_offset + 1);
    exact_before.tail(exact);
}

template <typename Emit>
void relocation_capacity(Emit emit, std::size_t required)
{
    for (std::size_t capacity = 0; capacity < required; ++capacity) {
        Fixture f(512, capacity);
        const Snapshot before(f);
        CHECK(emit(&f.cs) == A840_EMIT_CAPACITY);
        before.unchanged(f);
    }
    Fixture exact(512, required);
    CHECK(emit(&exact.cs) == A840_EMIT_OK);
    CHECK(exact.cs.reloc_count == required);
    check_relocs(exact.cs);
    Fixture absent;
    CHECK(a840_cs_init(&absent.cs, absent.words.data(), absent.words.size(), nullptr, 0)
        == A840_EMIT_OK);
    CHECK(emit(&absent.cs) == A840_EMIT_OK);
    CHECK(absent.cs.reloc_count == 0);
}

void test_texture_linear_golden()
{
    const auto in = texture();
    std::array<std::uint32_t, 18> words{};
    words.fill(0xdeadbeef);
    CHECK(a840_encode_tex_const(&in, words.data() + 1) == A840_EMIT_OK);
    /* Independent golden positions from a8xx_descriptors.xml:45-103. */
    const std::uint32_t expected[16] = {0x56780000, 0x00121234, 0x00100040,
        0x00358c30, 0, 0, 0x00000800, 2, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(std::memcmp(words.data() + 1, expected, sizeof(expected)) == 0);
    CHECK(words.front() == 0xdeadbeef && words.back() == 0xdeadbeef);
}

void test_texture_tiled_srgb()
{
    auto in = texture();
    in.tile_mode = A840_TILE_TILED;
    in.srgb = 1;
    in.tile_all = 1;
    in.swap = 3;
    in.pitch_align_log2 = 8;
    std::uint32_t words[16]{};
    CHECK(a840_encode_tex_const(&in, words) == A840_EMIT_OK);
    CHECK(words[4] == 0x33 && words[5] == 0);
    CHECK(((words[3] >> 8) & 3) == 3);
    CHECK(words[6] == 0x02000800);
}

void test_texture_ubwc_hbb16()
{
    auto in = texture();
    ubwc_texture(in);
    in.srgb = 1;
    std::uint32_t words[16]{};
    CHECK(a840_encode_tex_const(&in, words) == A840_EMIT_OK);
    CHECK(words[4] == 0x56780027);
    CHECK(words[5] == 0x00022234);
    CHECK(words[8] == 0x32000001);
    CHECK(words[10] == 0 && words[15] == 0); /* No invented per-texture HBB/version. */
    in.highest_bank_bit = A840_MESA_HIGHEST_BANK_BIT;
    tex_error(in, A840_EMIT_RANGE_ERROR);
}

void test_texture_1d()
{
    auto in = texture();
    in.type = A840_TEX_1D;
    in.height = 1;
    in.slice_stride = 4096;
    std::uint32_t words[16]{};
    CHECK(a840_encode_tex_const(&in, words) == A840_EMIT_OK);
    CHECK(((words[1] >> 17) & 7) == 0 && words[2] == 0x8040);
    in.height = 2;
    tex_error(in, A840_EMIT_INVALID_ARG);
}

void test_texture_3d_mips()
{
    auto in = texture();
    in.type = A840_TEX_3D;
    in.depth = 8;
    in.mip_levels = 4;
    in.min_slice_stride = 4096;
    std::uint32_t words[16]{};
    CHECK(a840_encode_tex_const(&in, words) == A840_EMIT_OK);
    CHECK(words[1] == 0x00861234 && words[6] == 0x30000800);
    CHECK(words[7] == 0x01000002);
    in.min_slice_stride = 0;
    tex_error(in, A840_EMIT_INVALID_ARG);
    in.mip_levels = 1;
    CHECK(a840_encode_tex_const(&in, words) == A840_EMIT_OK);
    CHECK(words[7] == 0x02000002);
}

void test_texture_cube_array()
{
    auto in = texture();
    in.type = A840_TEX_CUBE;
    in.height = 64;
    in.depth = 12;
    in.slice_stride = 16384;
    std::uint32_t words[16]{};
    CHECK(a840_encode_tex_const(&in, words) == A840_EMIT_OK);
    CHECK(((words[1] >> 17) & 7) == 2 && (words[1] >> 20) == 2);
    in.depth = 7;
    tex_error(in, A840_EMIT_INVALID_ARG);
    in.depth = 6;
    in.height = 32;
    tex_error(in, A840_EMIT_INVALID_ARG);
}

void test_texture_msaa()
{
    auto in = texture();
    for (std::uint32_t exponent = 0; exponent < 4; ++exponent) {
        in.samples = 1u << exponent;
        in.pitch = 256 * in.samples;
        in.slice_stride = static_cast<std::uint64_t>(in.pitch) * in.height;
        std::uint32_t words[16]{};
        CHECK(a840_encode_tex_const(&in, words) == A840_EMIT_OK);
        CHECK((words[2] >> 30) == exponent);
    }
    in.samples = 3;
    tex_error(in, A840_EMIT_RANGE_ERROR);
    in.samples = 16;
    tex_error(in, A840_EMIT_RANGE_ERROR);
    in.samples = 2;
    in.mip_levels = 2;
    tex_error(in, A840_EMIT_UNSUPPORTED);
}

void test_texture_all_swizzles()
{
    auto in = texture();
    for (std::uint32_t channel = 0; channel < 4; ++channel) {
        for (std::uint32_t swiz = 0; swiz < 6; ++swiz) {
            in.swizzle[channel] = swiz;
            std::uint32_t words[16]{};
            CHECK(a840_encode_tex_const(&in, words) == A840_EMIT_OK);
            const auto expected = swiz < 4 ? swiz + 3 : swiz - 3;
            CHECK(((words[3] >> (10 + channel * 3)) & 7) == expected);
        }
        in.swizzle[channel] = channel;
    }
    in.swizzle[0] = 6;
    tex_error(in, A840_EMIT_RANGE_ERROR);
}

void test_texture_dimensions_and_mips()
{
    auto in = texture();
    in.width = 0;
    tex_error(in, A840_EMIT_INVALID_ARG);
    in = texture();
    in.width = 32768;
    tex_error(in, A840_EMIT_RANGE_ERROR);
    in = texture();
    in.depth = 4096;
    tex_error(in, A840_EMIT_RANGE_ERROR);
    in = texture();
    in.mip_levels = 0;
    tex_error(in, A840_EMIT_RANGE_ERROR);
    in.mip_levels = 8;
    tex_error(in, A840_EMIT_RANGE_ERROR);
    in.mip_levels = 7;
    std::uint32_t words[16]{};
    CHECK(a840_encode_tex_const(&in, words) == A840_EMIT_OK);
    CHECK((words[6] >> 28) == 6);
}

void test_texture_format_and_boolean_ranges()
{
    auto in = texture();
    in.format_id = 0;
    tex_error(in, A840_EMIT_RANGE_ERROR);
    in.format_id = 256;
    tex_error(in, A840_EMIT_RANGE_ERROR);
    in.format_id = A840_FMT6_R8_G8B8_2PLANE_420_UNORM;
    tex_error(in, A840_EMIT_UNSUPPORTED);
    in = texture(); in.swap = 4; tex_error(in, A840_EMIT_RANGE_ERROR);
    in = texture(); in.tile_mode = 3; tex_error(in, A840_EMIT_RANGE_ERROR);
    in = texture(); in.srgb = 2; tex_error(in, A840_EMIT_RANGE_ERROR);
    in = texture(); in.tile_all = 2; tex_error(in, A840_EMIT_RANGE_ERROR);
    in = texture(); in.type = 4; tex_error(in, A840_EMIT_RANGE_ERROR);
}

void test_texture_pitch_and_slice_bounds()
{
    auto in = texture();
    in.pitch = 255; tex_error(in, A840_EMIT_ALIGNMENT);
    in = texture(); in.pitch_align_log2 = 5; tex_error(in, A840_EMIT_RANGE_ERROR);
    in = texture(); in.pitch_align_log2 = 22; tex_error(in, A840_EMIT_RANGE_ERROR);
    in = texture(); in.pitch = 1u << 21; tex_error(in, A840_EMIT_RANGE_ERROR);
    in = texture(); in.slice_stride = 8193; tex_error(in, A840_EMIT_ALIGNMENT);
    in = texture(); in.slice_stride = 4096; tex_error(in, A840_EMIT_RANGE_ERROR);
    in = texture(); in.slice_stride = UINT64_C(1) << 35; tex_error(in, A840_EMIT_RANGE_ERROR);
    in = texture(); in.min_slice_stride = 4096; tex_error(in, A840_EMIT_INVALID_ARG);
}

void test_texture_va_bounds()
{
    auto in = texture();
    in.gpu_va = 0; tex_error(in, A840_EMIT_INVALID_ARG);
    in.gpu_va = va_limit; tex_error(in, A840_EMIT_RANGE_ERROR);
    in.gpu_va = va_limit - 4096; tex_error(in, A840_EMIT_RANGE_ERROR);
    in.gpu_va = texture().gpu_va + 1; tex_error(in, A840_EMIT_ALIGNMENT);
    in.gpu_va = va_limit - in.slice_stride;
    std::uint32_t words[16]{};
    CHECK(a840_encode_tex_const(&in, words) == A840_EMIT_OK);
    CHECK((words[1] & 0x1ffff) == 0xffff);
}

void test_texture_ubwc_bounds()
{
    auto good = texture(); ubwc_texture(good);
    auto in = good; in.ubwc_meta_va = 0; tex_error(in, A840_EMIT_INVALID_ARG);
    in = good; in.ubwc_meta_va = va_limit; tex_error(in, A840_EMIT_RANGE_ERROR);
    in = good; ++in.ubwc_meta_va; tex_error(in, A840_EMIT_ALIGNMENT);
    in = good; in.ubwc_meta_pitch = 65; tex_error(in, A840_EMIT_ALIGNMENT);
    in = good; in.ubwc_meta_pitch = 16384; tex_error(in, A840_EMIT_RANGE_ERROR);
    in = good; in.ubwc_meta_slice_stride = 0; tex_error(in, A840_EMIT_INVALID_ARG);
    in = good; in.ubwc_meta_slice_stride = 4097; tex_error(in, A840_EMIT_ALIGNMENT);
    in = good; in.ubwc_meta_slice_stride = UINT64_C(1) << 27; tex_error(in, A840_EMIT_RANGE_ERROR);
    in = good; in.ubwc_block_width = 0; tex_error(in, A840_EMIT_RANGE_ERROR);
    in = good; in.ubwc_block_width = 3; tex_error(in, A840_EMIT_RANGE_ERROR);
    in = good; in.ubwc_meta_va = in.gpu_va; tex_error(in, A840_EMIT_ALIAS);
    in = good; in.tile_mode = A840_TILE_TILED; tex_error(in, A840_EMIT_INVALID_ARG);
    in = good; in.type = A840_TEX_3D; in.mip_levels = 2; in.min_slice_stride = 4096;
    tex_error(in, A840_EMIT_UNSUPPORTED);
}

void test_texture_tiled_and_msaa_layout_bounds()
{
    auto in = texture();
    in.tile_mode = A840_TILE_TILED;
    in.gpu_va += 64;
    tex_error(in, A840_EMIT_ALIGNMENT);
    in = texture(); in.samples = 8;
    tex_error(in, A840_EMIT_RANGE_ERROR); /* 64 RGBA texels * 8 samples need 2048B. */
    in = texture(); in.srgb = 1; in.format_id = A840_FMT6_32_UINT;
    tex_error(in, A840_EMIT_UNSUPPORTED);
    in = texture(); in.tile_mode = A840_TILE_TILED; in.format_id = A840_FMT6_16_16_16_16_FLOAT;
    in.pitch = 1024; in.slice_stride = 32768;
    std::uint32_t words[16]{};
    CHECK(a840_encode_tex_const(&in, words) == A840_EMIT_OK);
    CHECK(words[6] == 0x03002000); /* FDL cpp=8 -> pitch alignment 2^9. */
    in.pitch = 768;
    tex_error(in, A840_EMIT_ALIGNMENT);
    in = texture(); in.tile_mode = A840_TILE_TILED;
    in.height = 17; in.slice_stride = 4096;
    tex_error(in, A840_EMIT_RANGE_ERROR);
    in = texture(); in.tile_mode = A840_TILE_TILED; in.pitch_align_log2 = 6;
    in.pitch = 320; in.slice_stride = 12288;
    tex_error(in, A840_EMIT_ALIGNMENT); /* Declared minimum cannot waive tile alignment. */
    in = texture(); in.tile_mode = A840_TILE_TILED; in.samples = 2;
    in.format_id = A840_FMT6_8_8_UNORM; in.pitch = 256; in.slice_stride = 8192;
    CHECK(a840_encode_tex_const(&in, words) == A840_EMIT_OK);
    CHECK(((words[6] >> 24) & 15) == 2); /* R8G8 MSAA has cpp=4, not the cpp=2 special case. */
}

void test_texture_storage_alias_and_null()
{
    auto in = texture();
    std::uint32_t out[16]{};
    CHECK(a840_encode_tex_const(nullptr, out) == A840_EMIT_INVALID_ARG);
    CHECK(a840_encode_tex_const(&in, nullptr) == A840_EMIT_INVALID_ARG);
    const auto before = in;
    CHECK(a840_encode_tex_const(&in, reinterpret_cast<std::uint32_t *>(&in)) == A840_EMIT_ALIAS);
    CHECK(std::memcmp(&in, &before, sizeof(in)) == 0);
    CHECK(a840_encode_tex_const(&in, reinterpret_cast<std::uint32_t *>(
        reinterpret_cast<unsigned char *>(out) + 1)) == A840_EMIT_ALIGNMENT);
}

void test_sampler_golden()
{
    auto in = sampler();
    in.address_s = A840_ADDRESS_CLAMP_TO_EDGE;
    in.address_t = A840_ADDRESS_MIRROR_REPEAT;
    in.address_r = A840_ADDRESS_CLAMP_TO_BORDER;
    in.min_lod = 1.25f;
    in.lod_bias = -1.5f;
    in.compare_func = 3;
    in.border_color_idx = 7;
    std::uint32_t words[4]{};
    CHECK(a840_encode_samp_const(&in, words) == A840_EMIT_OK);
    const std::uint32_t expected[4] = {0x1e803455, 0x0c140780, 0x380, 0};
    CHECK(std::memcmp(words, expected, sizeof(words)) == 0);
}

void test_sampler_anisotropy()
{
    auto in = sampler();
    for (std::uint32_t exponent = 0; exponent <= 4; ++exponent) {
        in.max_aniso = 1u << exponent;
        std::uint32_t words[4]{};
        CHECK(a840_encode_samp_const(&in, words) == A840_EMIT_OK);
        CHECK((words[0] >> 29) == exponent);
        CHECK(((words[0] >> 2) & 3) == (exponent ? 2u : 1u));
        CHECK(((words[0] >> 4) & 3) == (exponent ? 2u : 1u));
    }
    in.max_aniso = 3; samp_error(in, A840_EMIT_RANGE_ERROR);
    in.max_aniso = 32; samp_error(in, A840_EMIT_RANGE_ERROR);
}

void test_sampler_mip_modes()
{
    auto in = sampler();
    for (std::uint32_t mode = 0; mode <= 2; ++mode) {
        in.mip_filter = mode;
        std::uint32_t words[4]{};
        CHECK(a840_encode_samp_const(&in, words) == A840_EMIT_OK);
        CHECK((words[0] & 3) == (mode == 0 ? 2u : mode == 1 ? 0u : 1u));
    }
}

void test_sampler_lod_edges()
{
    auto in = sampler();
    in.max_lod = 4095.0f / 256.0f;
    in.min_lod = in.max_lod;
    in.lod_bias = -16;
    std::uint32_t words[4]{};
    CHECK(a840_encode_samp_const(&in, words) == A840_EMIT_OK);
    CHECK((words[0] & 0x1fff0000) == 0x10000000);
    CHECK((words[1] & 0xffffff) == 0xffffff);
    in.lod_bias = 4095.0f / 256.0f;
    CHECK(a840_encode_samp_const(&in, words) == A840_EMIT_OK);
    CHECK((words[0] & 0x1fff0000) == 0x0fff0000);
    in.lod_bias = 16; samp_error(in, A840_EMIT_RANGE_ERROR);
    in.lod_bias = -16.01f; samp_error(in, A840_EMIT_RANGE_ERROR);
    in = sampler(); in.min_lod = -1; samp_error(in, A840_EMIT_RANGE_ERROR);
    in = sampler(); in.min_lod = 8; samp_error(in, A840_EMIT_RANGE_ERROR);
    in = sampler(); in.max_lod = 16; samp_error(in, A840_EMIT_RANGE_ERROR);
}

void test_sampler_nonfinite()
{
    auto in = sampler();
    in.min_lod = std::numeric_limits<float>::quiet_NaN(); samp_error(in, A840_EMIT_RANGE_ERROR);
    in = sampler(); in.max_lod = std::numeric_limits<float>::infinity(); samp_error(in, A840_EMIT_RANGE_ERROR);
    in = sampler(); in.lod_bias = std::numeric_limits<float>::quiet_NaN(); samp_error(in, A840_EMIT_RANGE_ERROR);
    in = sampler(); in.lod_bias = -std::numeric_limits<float>::infinity(); samp_error(in, A840_EMIT_RANGE_ERROR);
}

void test_sampler_unnormalized()
{
    auto in = sampler();
    in.unnormalized_coords = 1;
    in.mip_filter = A840_MIP_NONE;
    in.max_lod = 0;
    in.address_s = in.address_t = in.address_r = A840_ADDRESS_CLAMP_TO_EDGE;
    std::uint32_t words[4]{};
    CHECK(a840_encode_samp_const(&in, words) == A840_EMIT_OK);
    CHECK((words[1] >> 31) == 1 && (words[0] & 3) == 2);
    in.max_aniso = 2; samp_error(in, A840_EMIT_INVALID_ARG);
    in.max_aniso = 1; in.address_s = A840_ADDRESS_REPEAT; samp_error(in, A840_EMIT_INVALID_ARG);
}

void test_sampler_enum_ranges()
{
    auto in = sampler();
    in.min_filter = 2; samp_error(in, A840_EMIT_RANGE_ERROR);
    in = sampler(); in.mag_filter = 3; samp_error(in, A840_EMIT_RANGE_ERROR);
    in = sampler(); in.mip_filter = 3; samp_error(in, A840_EMIT_RANGE_ERROR);
    in = sampler(); in.address_s = 5; samp_error(in, A840_EMIT_RANGE_ERROR);
    in = sampler(); in.address_t = 5; samp_error(in, A840_EMIT_RANGE_ERROR);
    in = sampler(); in.address_r = 5; samp_error(in, A840_EMIT_RANGE_ERROR);
    in = sampler(); in.compare_func = 9; samp_error(in, A840_EMIT_RANGE_ERROR);
    in = sampler(); in.border_color_idx = 1u << 25; samp_error(in, A840_EMIT_RANGE_ERROR);
    in = sampler(); in.unnormalized_coords = 2; samp_error(in, A840_EMIT_RANGE_ERROR);
}

void test_sampler_storage()
{
    auto in = sampler();
    std::uint32_t out[4]{};
    CHECK(a840_encode_samp_const(nullptr, out) == A840_EMIT_INVALID_ARG);
    CHECK(a840_encode_samp_const(&in, nullptr) == A840_EMIT_INVALID_ARG);
    const auto before = in;
    CHECK(a840_encode_samp_const(&in, reinterpret_cast<std::uint32_t *>(&in)) == A840_EMIT_ALIAS);
    CHECK(std::memcmp(&in, &before, sizeof(in)) == 0);
    CHECK(a840_encode_samp_const(&in, reinterpret_cast<std::uint32_t *>(
        reinterpret_cast<unsigned char *>(out) + 1)) == A840_EMIT_ALIGNMENT);
}

void test_copy_packet_and_relocs()
{
    Fixture f;
    CHECK(a840_emit_wfi(&f.cs) == A840_EMIT_OK);
    CHECK(a840_emit_buffer_copy(&f.cs, 7, UINT64_C(0x123450000), 8, UINT64_C(0x223460000), 1024)
        == A840_EMIT_OK);
    std::size_t count = 0;
    const auto packets = decode(f.cs, count);
    CHECK(count == 2 && packets[1].identifier == 0x75 && packets[1].payload_words == 5);
    CHECK(f.words[2] == 256 && f.words[3] == 0x23450000 && f.words[4] == 1);
    CHECK(f.words[5] == 0x23460000 && f.words[6] == 2);
    CHECK(f.relocs[0].dword_offset == 3 && f.relocs[1].dword_offset == 5);
    CHECK(f.relocs[0].flags == A840_RELOC_READ && f.relocs[1].flags == A840_RELOC_WRITE);
    CHECK(f.relocs[0].bo_handle == 7 && f.relocs[1].bo_handle == 8);
    check_relocs(f.cs);
}

void test_copy_arguments()
{
    Fixture f;
    const Snapshot before(f);
    CHECK(a840_emit_buffer_copy(&f.cs, 0, 0x1000, 0, 0x2000, 0) == A840_EMIT_INVALID_ARG);
    CHECK(a840_emit_buffer_copy(&f.cs, 0, 0x1000, 0, 0x2000, 3) == A840_EMIT_ALIGNMENT);
    CHECK(a840_emit_buffer_copy(&f.cs, 0, 0x1001, 0, 0x2000, 4) == A840_EMIT_ALIGNMENT);
    CHECK(a840_emit_buffer_copy(&f.cs, 0, 0, 0, 0x2000, 4) == A840_EMIT_INVALID_ARG);
    CHECK(a840_emit_buffer_copy(&f.cs, 0, va_limit, 0, 0x2000, 4) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_emit_buffer_copy(&f.cs, 0, va_limit - 4, 0, 0x2000, 8) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_emit_buffer_copy(&f.cs, 0, 0x1000, 0, 0x1000, 4) == A840_EMIT_ALIAS);
    CHECK(a840_emit_buffer_copy(&f.cs, 0, 0x1000, 0, 0x1004, 8) == A840_EMIT_ALIAS);
    before.unchanged(f);
}

void test_copy_boundary_success()
{
    Fixture f;
    CHECK(a840_emit_buffer_copy(&f.cs, 0, va_limit - 4, 0, 0x1000, 4) == A840_EMIT_OK);
    CHECK(a840_emit_buffer_copy(&f.cs, 0, 0x1000, 0, 0x1004, 4) == A840_EMIT_OK);
    check_relocs(f.cs);
}

void test_copy_word_capacity()
{
    word_capacity([](A840CmdStream *cs) { return a840_emit_buffer_copy(cs, 7, 0x1000, 8, 0x2000, 16); },
        A840_EMIT_BUFFER_COPY_MAX_DWORDS);
}

void test_copy_reloc_capacity()
{
    relocation_capacity([](A840CmdStream *cs) { return a840_emit_buffer_copy(cs, 7, 0x1000, 8, 0x2000, 16); }, 2);
}

void test_clear_packet_golden()
{
    Fixture f;
    const auto desc = clear_desc();
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_OK);
    CHECK(f.cs.used_dwords == 29 && f.cs.used_dwords <= A840_EMIT_2D_CLEAR_MAX_DWORDS);
    CHECK(read_reg(f.cs, 0x8500) == 0x00f03080);
    CHECK(read_reg(f.cs, 0x8c00) == 0x00f03080);
    CHECK(read_reg(f.cs, 0x8c01) == 0);
    CHECK(read_reg(f.cs, 0x8505) == 0x00040003 && read_reg(f.cs, 0x8506) == 0x00130022);
    CHECK(read_reg(f.cs, 0x8c17) == 0x30 && read_reg(f.cs, 0x8c18) == 0x200000);
    CHECK(read_reg(f.cs, 0x8c1a) == 4 && read_reg(f.cs, 0x8c20) == 0);
    CHECK(read_reg(f.cs, 0x8c2c) == 0x12 && read_reg(f.cs, 0x8c2f) == 0xff);
    CHECK(read_reg(f.cs, 0xa9bf) == 0xf180 && read_reg(f.cs, 0xb2d2) == 0x20000000);
    std::size_t count = 0; const auto packets = decode(f.cs, count);
    CHECK(packets[count - 1].identifier == 0x2c && packets[count - 1].payload_words == 1);
    CHECK(f.words[f.cs.used_dwords - 1] == 3);
    CHECK(f.cs.reloc_count == 1 && f.relocs[0].flags == A840_RELOC_WRITE);
    no_wfi(f.cs); check_relocs(f.cs);
}

void test_clear_ubwc()
{
    auto desc = clear_desc(); ubwc_surface(desc.dst);
    Fixture f;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_OK);
    CHECK(read_reg(f.cs, 0x8c17) == 0x1330);
    CHECK(read_reg(f.cs, 0x8c20) == 0x280000 && read_reg(f.cs, 0x8c22) == 0x4001);
    CHECK(f.cs.reloc_count == 2 && f.relocs[1].gpu_va == desc.dst.ubwc_meta_va);
    check_relocs(f.cs);
}

void test_clear_srgb_and_integer()
{
    auto desc = clear_desc(); desc.dst.srgb = 1;
    Fixture f;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_OK);
    CHECK(read_reg(f.cs, 0x8500) == 0x01f03080 && read_reg(f.cs, 0xa9bf) == 0xf980);
    CHECK(read_reg(f.cs, 0x8c17) == 0x2030);
    desc.dst.srgb = 0; desc.dst.format_id = A840_FMT6_32_UINT;
    Fixture integer;
    CHECK(a840_emit_2d_clear(&integer.cs, &desc) == A840_EMIT_OK);
    CHECK(read_reg(integer.cs, 0x8500) == 0x07f04b80);
    CHECK((read_reg(integer.cs, 0xa9bf) & 6) == 4);
}

void test_clear_rect_errors()
{
    Fixture f;
    const Snapshot before(f);
    auto desc = clear_desc(); desc.rect.width = 0;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_INVALID_ARG);
    desc = clear_desc(); desc.rect.x = 63;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_RANGE_ERROR);
    desc = clear_desc(); desc.rect.x = UINT32_MAX;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_RANGE_ERROR);
    desc = clear_desc(); desc.dst.width = 20000; desc.dst.pitch = 80000;
    desc.rect = {16384, 0, 1, 1};
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_RANGE_ERROR);
    before.unchanged(f);
}

void test_clear_surface_errors()
{
    Fixture f; const Snapshot before(f);
    auto desc = clear_desc(); desc.dst.gpu_va += 1;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_ALIGNMENT);
    desc = clear_desc(); desc.dst.pitch = 255;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_ALIGNMENT);
    desc = clear_desc(); desc.dst.pitch = 64;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_RANGE_ERROR);
    desc = clear_desc(); desc.dst.gpu_va = va_limit - 4096;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_RANGE_ERROR);
    desc = clear_desc(); desc.dst.format_id = A840_FMT6_DXT1;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_UNSUPPORTED);
    desc = clear_desc(); desc.dst.format_id = A840_FMT6_32_UINT; desc.dst.srgb = 1;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_UNSUPPORTED);
    before.unchanged(f);
}

void test_clear_ubwc_errors()
{
    Fixture f; const Snapshot before(f);
    auto desc = clear_desc(); ubwc_surface(desc.dst); desc.dst.ubwc_meta_va = desc.dst.gpu_va;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_ALIAS);
    desc = clear_desc(); ubwc_surface(desc.dst); desc.dst.ubwc_meta_pitch = 65;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_ALIGNMENT);
    desc = clear_desc(); ubwc_surface(desc.dst); desc.dst.ubwc_meta_pitch = 16384;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_RANGE_ERROR);
    desc = clear_desc(); desc.dst.ubwc_meta_va = 0x1000;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_INVALID_ARG);
    before.unchanged(f);
}

void test_tiled_surface_alignment_and_flag_units()
{
    auto desc = clear_desc(); ubwc_surface(desc.dst);
    desc.dst.gpu_va += 64;
    Fixture f; const Snapshot before(f);
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_ALIGNMENT);
    desc = clear_desc(); ubwc_surface(desc.dst); desc.dst.pitch = 320;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_ALIGNMENT);
    desc = clear_desc(); ubwc_surface(desc.dst); desc.dst.ubwc_meta_pitch = 8192;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_RANGE_ERROR);
    desc = clear_desc(); desc.dst.tile_mode = A840_TILE_TILED;
    desc.dst.width = 1; desc.dst.height = 1; desc.dst.pitch = 1024;
    desc.dst.gpu_va = va_limit - 4096; desc.rect = {0, 0, 1, 1};
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_RANGE_ERROR);
    before.unchanged(f);
    desc = clear_desc(); ubwc_surface(desc.dst);
    desc.dst.ubwc_meta_slice_stride = 8192;
    CHECK(a840_emit_2d_clear(&f.cs, &desc) == A840_EMIT_OK);
    /* FDL passes bytes/4; XML shr=7 yields bytes/512 at bit11. */
    CHECK(read_reg(f.cs, 0x8c22) == 0x8001);
    auto blit = blit_desc(); ubwc_surface(blit.src); ubwc_surface(blit.dst);
    blit.src.ubwc_meta_slice_stride = 8192;
    Fixture g;
    CHECK(a840_emit_2d_blit(&g.cs, &blit) == A840_EMIT_OK);
    CHECK(read_reg(g.cs, 0xb2cc) == 0x8001);
}

void test_clear_word_capacity()
{
    const auto desc = clear_desc();
    word_capacity([&](A840CmdStream *cs) { return a840_emit_2d_clear(cs, &desc); }, A840_EMIT_2D_CLEAR_MAX_DWORDS);
}

void test_clear_reloc_capacity()
{
    auto desc = clear_desc(); ubwc_surface(desc.dst);
    relocation_capacity([&](A840CmdStream *cs) { return a840_emit_2d_clear(cs, &desc); }, 2);
}

void test_blit_packet_golden()
{
    Fixture f; const auto desc = blit_desc();
    CHECK(a840_emit_2d_blit(&f.cs, &desc) == A840_EMIT_OK);
    CHECK(f.cs.used_dwords == 39 && f.cs.used_dwords <= A840_EMIT_2D_BLIT_MAX_DWORDS);
    CHECK(read_reg(f.cs, 0x8500) == 0x00f03000);
    CHECK(read_reg(f.cs, 0x8505) == 0x00050004 && read_reg(f.cs, 0x8506) == 0x000e0017);
    CHECK(read_reg(f.cs, 0x8501) == 0x200 && read_reg(f.cs, 0x8502) == 0x2900);
    CHECK(read_reg(f.cs, 0x8503) == 0x300 && read_reg(f.cs, 0x8504) == 0x1600);
    CHECK(read_reg(f.cs, 0xb2c0) == 0x00510030 && read_reg(f.cs, 0xb2c1) == 0x100040);
    CHECK(read_reg(f.cs, 0xb2c2) == 0x400000 && read_reg(f.cs, 0xb2c4) == 0x800);
    CHECK(read_reg(f.cs, 0xb2ca) == 0 && read_reg(f.cs, 0x8c18) == 0x600000);
    CHECK(f.cs.reloc_count == 2 && f.relocs[0].flags == A840_RELOC_WRITE && f.relocs[1].flags == A840_RELOC_READ);
    no_wfi(f.cs); check_relocs(f.cs);
}

void test_blit_ubwc_four_relocs()
{
    Fixture f; auto desc = blit_desc(); ubwc_surface(desc.src); ubwc_surface(desc.dst);
    CHECK(a840_emit_2d_blit(&f.cs, &desc) == A840_EMIT_OK);
    CHECK(read_reg(f.cs, 0xb2c0) == 0x00511330);
    CHECK(read_reg(f.cs, 0xb2ca) == 0x480000 && read_reg(f.cs, 0xb2cc) == 0x4001);
    CHECK(read_reg(f.cs, 0x8c20) == 0x680000 && read_reg(f.cs, 0x8c22) == 0x4001);
    CHECK(f.cs.reloc_count == 4);
    check_relocs(f.cs);
}

void test_blit_tiled_nearest()
{
    Fixture f; auto desc = blit_desc();
    desc.src.tile_mode = A840_TILE_TILED; desc.dst.tile_mode = A840_TILE_TILED;
    desc.filter = A840_FILTER_NEAREST; desc.src.swap = 1; desc.dst.swap = 3;
    CHECK(a840_emit_2d_blit(&f.cs, &desc) == A840_EMIT_OK);
    CHECK(read_reg(f.cs, 0xb2c0) == 0x00500730 && read_reg(f.cs, 0x8c17) == 0xf30);
}

void test_blit_format_conversion_and_half()
{
    Fixture f; auto desc = blit_desc();
    desc.src.format_id = A840_FMT6_16_16_FLOAT;
    desc.dst.format_id = A840_FMT6_32_FLOAT;
    CHECK(a840_emit_2d_blit(&f.cs, &desc) == A840_EMIT_OK);
    CHECK((read_reg(f.cs, 0xa9bf) & 1) == 1);
    Fixture g; desc.src.format_id = A840_FMT6_32_UINT;
    CHECK(a840_emit_2d_blit(&g.cs, &desc) == A840_EMIT_UNSUPPORTED);
    desc.dst.format_id = A840_FMT6_32_UINT;
    CHECK(a840_emit_2d_blit(&g.cs, &desc) == A840_EMIT_UNSUPPORTED); /* Integer linear filtering. */
    desc.filter = A840_FILTER_NEAREST;
    CHECK(a840_emit_2d_blit(&g.cs, &desc) == A840_EMIT_OK);
}

void test_blit_rect_and_filter_errors()
{
    Fixture f; const Snapshot before(f);
    auto desc = blit_desc(); desc.src_rect.width = 0;
    CHECK(a840_emit_2d_blit(&f.cs, &desc) == A840_EMIT_INVALID_ARG);
    desc = blit_desc(); desc.dst_rect.height = UINT32_MAX;
    CHECK(a840_emit_2d_blit(&f.cs, &desc) == A840_EMIT_RANGE_ERROR);
    desc = blit_desc(); desc.src_rect.x = UINT32_MAX;
    CHECK(a840_emit_2d_blit(&f.cs, &desc) == A840_EMIT_RANGE_ERROR);
    desc = blit_desc(); desc.filter = 2;
    CHECK(a840_emit_2d_blit(&f.cs, &desc) == A840_EMIT_RANGE_ERROR);
    before.unchanged(f);
}

void test_blit_surface_alias_errors()
{
    Fixture f; const Snapshot before(f);
    auto desc = blit_desc(); desc.dst.gpu_va = desc.src.gpu_va;
    CHECK(a840_emit_2d_blit(&f.cs, &desc) == A840_EMIT_ALIAS);
    desc = blit_desc(); desc.dst.gpu_va = desc.src.gpu_va + 64;
    CHECK(a840_emit_2d_blit(&f.cs, &desc) == A840_EMIT_ALIAS);
    desc = blit_desc(); ubwc_surface(desc.src); desc.src.ubwc_meta_va = desc.dst.gpu_va;
    CHECK(a840_emit_2d_blit(&f.cs, &desc) == A840_EMIT_ALIAS);
    desc = blit_desc(); ubwc_surface(desc.dst); desc.dst.ubwc_meta_va = desc.src.gpu_va;
    CHECK(a840_emit_2d_blit(&f.cs, &desc) == A840_EMIT_ALIAS);
    before.unchanged(f);
}

void test_blit_word_capacity()
{
    const auto desc = blit_desc();
    word_capacity([&](A840CmdStream *cs) { return a840_emit_2d_blit(cs, &desc); }, A840_EMIT_2D_BLIT_MAX_DWORDS);
}

void test_blit_reloc_capacity()
{
    auto desc = blit_desc(); ubwc_surface(desc.src); ubwc_surface(desc.dst);
    relocation_capacity([&](A840CmdStream *cs) { return a840_emit_2d_blit(cs, &desc); }, 4);
}

void test_emit_descriptor_storage_alias()
{
    Fixture f; auto clear = clear_desc(); auto blit = blit_desc();
    const Snapshot before(f);
    CHECK(a840_emit_2d_clear(&f.cs, nullptr) == A840_EMIT_INVALID_ARG);
    CHECK(a840_emit_2d_blit(&f.cs, nullptr) == A840_EMIT_INVALID_ARG);
    CHECK(a840_emit_2d_clear(&f.cs, reinterpret_cast<const A840Clear2DDesc *>(f.words.data()))
        == A840_EMIT_ALIAS);
    CHECK(a840_emit_2d_blit(&f.cs, reinterpret_cast<const A840Blit2DDesc *>(f.relocs.data()))
        == A840_EMIT_ALIAS);
    CHECK(a840_emit_2d_clear(&f.cs, reinterpret_cast<const A840Clear2DDesc *>(&f.cs))
        == A840_EMIT_ALIAS);
    CHECK(a840_emit_2d_blit(&f.cs, reinterpret_cast<const A840Blit2DDesc *>(
        reinterpret_cast<unsigned char *>(&blit) + 1)) == A840_EMIT_ALIGNMENT);
    CHECK(a840_emit_2d_clear(nullptr, &clear) == A840_EMIT_INVALID_ARG);
    before.unchanged(f);
}

void test_wfi_and_wait_mem()
{
    Fixture f;
    CHECK(a840_emit_wfi(&f.cs) == A840_EMIT_OK);
    CHECK(a840_emit_wait_mem_writes(&f.cs) == A840_EMIT_OK);
    std::size_t count = 0; const auto packets = decode(f.cs, count);
    CHECK(count == 2 && packets[0].identifier == 0x26 && packets[1].identifier == 0x12);
    CHECK(packets[0].payload_words == 0 && packets[1].payload_words == 0);
    CHECK(f.cs.reloc_count == 0 && f.cs.used_dwords == 2);
}

void test_wait_word_capacity()
{
    word_capacity(a840_emit_wfi, A840_EMIT_WFI_MAX_DWORDS);
    word_capacity(a840_emit_wait_mem_writes, A840_EMIT_WAIT_MEM_WRITES_MAX_DWORDS);
}

void test_cache_events_golden()
{
    Fixture f;
    CHECK(a840_emit_cache_flush_inv(&f.cs, all_caches) == A840_EMIT_OK);
    std::size_t count = 0; const auto packets = decode(f.cs, count);
    CHECK(count == 4 && f.cs.used_dwords == 8 && f.cs.reloc_count == 0);
    const std::uint32_t expected[] = {0x1d, 0x1c, 0x19, 0x33};
    for (std::size_t i = 0; i < count; ++i) {
        CHECK(packets[i].identifier == 0x46 && packets[i].payload_words == 1);
        CHECK(f.words[packets[i].header_word + 1] == expected[i]);
    }
    no_wfi(f.cs);
}

void test_cache_all_masks()
{
    for (std::uint32_t mask = 0; mask < 16; ++mask) {
        Fixture f;
        CHECK(a840_emit_cache_flush_inv(&f.cs, mask) == A840_EMIT_OK);
        unsigned bits = 0; for (auto x = mask; x; x &= x - 1) ++bits;
        CHECK(f.cs.used_dwords == 2 * bits && f.cs.used_dwords <= A840_EMIT_CACHE_FLUSH_INV_MAX_DWORDS);
    }
    Fixture f; const Snapshot before(f);
    CHECK(a840_emit_cache_flush_inv(&f.cs, 16) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_emit_cache_flush_inv(&f.cs, UINT32_MAX) == A840_EMIT_RANGE_ERROR);
    before.unchanged(f);
}

void test_cache_capacity_transaction()
{
    word_capacity([](A840CmdStream *cs) { return a840_emit_cache_flush_inv(cs, all_caches); },
        A840_EMIT_CACHE_FLUSH_INV_MAX_DWORDS);
    Fixture empty(0, 0); const Snapshot before(empty);
    CHECK(a840_emit_cache_flush_inv(&empty.cs, 0) == A840_EMIT_OK);
    before.unchanged(empty);
}

void test_mem_write64_golden()
{
    Fixture f;
    CHECK(a840_emit_mem_write64(&f.cs, 9, UINT64_C(0x123450000), UINT64_C(0xfedcba9876543210))
        == A840_EMIT_OK);
    std::size_t count = 0; const auto packets = decode(f.cs, count);
    CHECK(count == 1 && packets[0].identifier == 0x3d && packets[0].payload_words == 4);
    CHECK(f.words[1] == 0x23450000 && f.words[2] == 1);
    CHECK(f.words[3] == 0x76543210 && f.words[4] == 0xfedcba98);
    CHECK(f.relocs[0].dword_offset == 1 && f.relocs[0].bo_handle == 9);
    CHECK(f.relocs[0].flags == A840_RELOC_WRITE);
    check_relocs(f.cs); no_wfi(f.cs);
}

void test_mem_write64_errors_and_edges()
{
    Fixture f; const Snapshot before(f);
    CHECK(a840_emit_mem_write64(&f.cs, 0, 0, 0) == A840_EMIT_INVALID_ARG);
    CHECK(a840_emit_mem_write64(&f.cs, 0, 0x1004, 0) == A840_EMIT_ALIGNMENT);
    CHECK(a840_emit_mem_write64(&f.cs, 0, va_limit, 0) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_emit_mem_write64(&f.cs, 0, va_limit - 4, 0) == A840_EMIT_RANGE_ERROR);
    before.unchanged(f);
    CHECK(a840_emit_mem_write64(&f.cs, 0, va_limit - 8, UINT64_MAX) == A840_EMIT_OK);
    CHECK(f.words[3] == UINT32_MAX && f.words[4] == UINT32_MAX);
}

void test_mem_write64_capacity()
{
    const auto emit = [](A840CmdStream *cs) { return a840_emit_mem_write64(cs, 0, 0x1000, UINT64_MAX); };
    word_capacity(emit, A840_EMIT_MEM_WRITE64_MAX_DWORDS);
    relocation_capacity(emit, 1);
}

void test_timestamp_marker_golden()
{
    Fixture f;
    CHECK(a840_emit_timestamp_event(&f.cs, A840_EVENT_RB_DONE_TS, 19, UINT64_C(0x112345000),
        UINT64_C(0xfedcba9876543210)) == A840_EMIT_OK);
    std::size_t count = 0; const auto packets = decode(f.cs, count);
    CHECK(count == 1 && packets[0].identifier == 0x46 && packets[0].payload_words == 5);
    CHECK(f.words[1] == 0x08100016);
    CHECK(f.words[2] == 0x12345000 && f.words[3] == 1);
    CHECK(f.words[4] == 0x76543210 && f.words[5] == 0xfedcba98);
    CHECK(f.relocs[0].dword_offset == 2 && f.relocs[0].bo_handle == 19);
    no_wfi(f.cs); check_relocs(f.cs);
}

void test_timestamp_gpu_clock()
{
    Fixture f;
    CHECK(a840_emit_timestamp_event(&f.cs, A840_EVENT_RB_DONE_TS | A840_TIMESTAMP_GPU_CLOCK,
        1, 0x1000, 0) == A840_EMIT_OK);
    CHECK(f.cs.used_dwords == 4 && f.words[1] == 0x08300016 && f.words[2] == 0x1000);
    CHECK(a840_emit_timestamp_event(&f.cs, A840_EVENT_CACHE_FLUSH_TS | A840_TIMESTAMP_GPU_CLOCK,
        1, 0x2000, 0) == A840_EMIT_OK);
    CHECK(f.words[5] == 0x08300004);
    no_wfi(f.cs); check_relocs(f.cs);
}

void test_timestamp_errors()
{
    Fixture f; const Snapshot before(f);
    CHECK(a840_emit_timestamp_event(&f.cs, 256, 0, 0x1000, 0) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_emit_timestamp_event(&f.cs, A840_EVENT_CCU_FLUSH_COLOR, 0, 0x1000, 0)
        == A840_EMIT_UNSUPPORTED);
    CHECK(a840_emit_timestamp_event(&f.cs, A840_EVENT_RB_DONE_TS | A840_TIMESTAMP_GPU_CLOCK,
        0, 0x1000, 1) == A840_EMIT_INVALID_ARG);
    CHECK(a840_emit_timestamp_event(&f.cs, A840_EVENT_RB_DONE_TS, 0, 0x1004, 0)
        == A840_EMIT_ALIGNMENT);
    CHECK(a840_emit_timestamp_event(&f.cs, A840_EVENT_RB_DONE_TS, 0, va_limit, 0)
        == A840_EMIT_RANGE_ERROR);
    before.unchanged(f);
}

void test_timestamp_capacity()
{
    const auto marker = [](A840CmdStream *cs) {
        return a840_emit_timestamp_event(cs, A840_EVENT_RB_DONE_TS, 1, 0x1000, UINT64_MAX);
    };
    word_capacity(marker, A840_EMIT_TIMESTAMP_EVENT_MAX_DWORDS);
    relocation_capacity(marker, 1);
    const auto clock = [](A840CmdStream *cs) {
        return a840_emit_timestamp_event(cs, A840_EVENT_RB_DONE_TS | A840_TIMESTAMP_GPU_CLOCK, 1, 0x1000, 0);
    };
    word_capacity(clock, A840_EMIT_TIMESTAMP_EVENT_MAX_DWORDS);
}

void test_all_emit_null_and_corrupt_cs()
{
    auto clear = clear_desc(); auto blit = blit_desc();
    CHECK(a840_emit_wfi(nullptr) == A840_EMIT_INVALID_ARG);
    CHECK(a840_emit_wait_mem_writes(nullptr) == A840_EMIT_INVALID_ARG);
    CHECK(a840_emit_cache_flush_inv(nullptr, all_caches) == A840_EMIT_INVALID_ARG);
    CHECK(a840_emit_buffer_copy(nullptr, 0, 0x1000, 0, 0x2000, 4) == A840_EMIT_INVALID_ARG);
    CHECK(a840_emit_mem_write64(nullptr, 0, 0x1000, 0) == A840_EMIT_INVALID_ARG);
    CHECK(a840_emit_timestamp_event(nullptr, A840_EVENT_RB_DONE_TS, 0, 0x1000, 0)
        == A840_EMIT_INVALID_ARG);
    Fixture f; f.cs.used_dwords = f.cs.capacity_dwords + 1; const Snapshot before(f);
    CHECK(a840_emit_cache_flush_inv(&f.cs, all_caches) == A840_EMIT_INVALID_ARG);
    CHECK(a840_emit_buffer_copy(&f.cs, 0, 0x1000, 0, 0x2000, 4) == A840_EMIT_INVALID_ARG);
    CHECK(a840_emit_2d_clear(&f.cs, &clear) == A840_EMIT_INVALID_ARG);
    CHECK(a840_emit_2d_blit(&f.cs, &blit) == A840_EMIT_INVALID_ARG);
    CHECK(a840_emit_mem_write64(&f.cs, 0, 0x1000, 0) == A840_EMIT_INVALID_ARG);
    before.unchanged(f);
}

void test_relocation_offset_overflow()
{
    A840Reloc relocs[4]{};
    A840CmdStream cs{reinterpret_cast<std::uint32_t *>(std::uintptr_t{0x1000}),
        static_cast<std::size_t>(UINT32_MAX) + 128, UINT32_MAX, relocs, 4, 0};
    const auto before = cs;
    CHECK(a840_emit_buffer_copy(&cs, 0, 0x1000, 0, 0x2000, 4) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_emit_mem_write64(&cs, 0, 0x1000, 0) == A840_EMIT_RANGE_ERROR);
    CHECK(a840_emit_timestamp_event(&cs, A840_EVENT_RB_DONE_TS, 0, 0x1000, 0)
        == A840_EMIT_RANGE_ERROR);
    CHECK(std::memcmp(&cs, &before, sizeof(cs)) == 0);
    for (const auto &reloc : relocs) CHECK(reloc.dword_offset == 0 && reloc.gpu_va == 0);
}

void test_batch_failure_then_success()
{
    Fixture f;
    CHECK(a840_emit_mem_write64(&f.cs, 1, 0x1000, 19) == A840_EMIT_OK);
    const Snapshot before(f);
    auto bad = blit_desc(); bad.dst_rect.width = UINT32_MAX;
    for (unsigned i = 0; i < 16; ++i) {
        CHECK(a840_emit_2d_blit(&f.cs, &bad) == A840_EMIT_RANGE_ERROR);
        before.unchanged(f);
    }
    const auto good = blit_desc();
    CHECK(a840_emit_2d_blit(&f.cs, &good) == A840_EMIT_OK);
    CHECK(f.cs.reloc_count == 3);
    before.tail(f); check_relocs(f.cs);
}

void dump_stream(const A840CmdStream &cs)
{
    if (!dump_path) return;
    std::FILE *file = std::fopen(dump_path, "wb");
    CHECK(file != nullptr);
    for (std::size_t i = 0; i < cs.used_dwords; ++i) {
        const auto value = cs.words[i];
        const unsigned char bytes[4] = {static_cast<unsigned char>(value),
            static_cast<unsigned char>(value >> 8), static_cast<unsigned char>(value >> 16),
            static_cast<unsigned char>(value >> 24)};
        CHECK(std::fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes));
    }
    CHECK(std::fclose(file) == 0);
    std::printf("DUMP: %s (%zu dwords, little-endian, complete emitted stream)\n", dump_path, cs.used_dwords);
}

void test_composite_dump_and_roundtrip()
{
    Fixture f;
    const Snapshot before(f);
    auto clear = clear_desc(); ubwc_surface(clear.dst);
    auto blit = blit_desc(); ubwc_surface(blit.src); ubwc_surface(blit.dst);
    CHECK(a840_emit_buffer_copy(&f.cs, 1, 0x1000000, 2, 0x1200000, 4096) == A840_EMIT_OK);
    CHECK(a840_emit_2d_clear(&f.cs, &clear) == A840_EMIT_OK);
    CHECK(a840_emit_2d_blit(&f.cs, &blit) == A840_EMIT_OK);
    CHECK(a840_emit_cache_flush_inv(&f.cs, all_caches) == A840_EMIT_OK);
    CHECK(a840_emit_wait_mem_writes(&f.cs) == A840_EMIT_OK);
    CHECK(a840_emit_wfi(&f.cs) == A840_EMIT_OK);
    CHECK(a840_emit_mem_write64(&f.cs, 3, 0x1400000, UINT64_C(0x76543210fedcba98)) == A840_EMIT_OK);
    CHECK(a840_emit_timestamp_event(&f.cs, A840_EVENT_RB_DONE_TS, 3, 0x1400008,
        UINT64_C(0x123456789abcdef0)) == A840_EMIT_OK);
    CHECK(a840_emit_timestamp_event(&f.cs, A840_EVENT_RB_DONE_TS | A840_TIMESTAMP_GPU_CLOCK,
        3, 0x1400010, 0) == A840_EMIT_OK);
    CHECK(f.cs.used_dwords == 99);
    CHECK(f.cs.reloc_count == 11);
    const auto budget = A840_EMIT_BUFFER_COPY_MAX_DWORDS + A840_EMIT_2D_CLEAR_MAX_DWORDS +
        A840_EMIT_2D_BLIT_MAX_DWORDS + A840_EMIT_CACHE_FLUSH_INV_MAX_DWORDS + A840_EMIT_WFI_MAX_DWORDS +
        A840_EMIT_WAIT_MEM_WRITES_MAX_DWORDS + A840_EMIT_MEM_WRITE64_MAX_DWORDS +
        2 * A840_EMIT_TIMESTAMP_EVENT_MAX_DWORDS;
    CHECK(f.cs.used_dwords <= budget);
    std::size_t count = 0; const auto packets = decode(f.cs, count);
    CHECK(count == 32);
    CHECK(packets[0].identifier == 0x75);
    CHECK(packets[count - 1].identifier == 0x46 && packets[count - 1].payload_words == 3);
    check_relocs(f.cs); before.tail(f);
    dump_stream(f.cs);
}

struct Test { const char *name; void (*run)(); };
#define TEST(name) {#name, name}
const Test tests[] = {
    TEST(test_texture_linear_golden), TEST(test_texture_tiled_srgb), TEST(test_texture_ubwc_hbb16),
    TEST(test_texture_1d), TEST(test_texture_3d_mips), TEST(test_texture_cube_array), TEST(test_texture_msaa),
    TEST(test_texture_all_swizzles), TEST(test_texture_dimensions_and_mips), TEST(test_texture_format_and_boolean_ranges),
    TEST(test_texture_pitch_and_slice_bounds), TEST(test_texture_va_bounds), TEST(test_texture_ubwc_bounds),
    TEST(test_texture_tiled_and_msaa_layout_bounds), TEST(test_texture_storage_alias_and_null),
    TEST(test_sampler_golden), TEST(test_sampler_anisotropy),
    TEST(test_sampler_mip_modes), TEST(test_sampler_lod_edges), TEST(test_sampler_nonfinite), TEST(test_sampler_unnormalized),
    TEST(test_sampler_enum_ranges), TEST(test_sampler_storage), TEST(test_copy_packet_and_relocs), TEST(test_copy_arguments),
    TEST(test_copy_boundary_success), TEST(test_copy_word_capacity), TEST(test_copy_reloc_capacity),
    TEST(test_clear_packet_golden), TEST(test_clear_ubwc), TEST(test_clear_srgb_and_integer), TEST(test_clear_rect_errors),
    TEST(test_clear_surface_errors), TEST(test_clear_ubwc_errors), TEST(test_tiled_surface_alignment_and_flag_units),
    TEST(test_clear_word_capacity), TEST(test_clear_reloc_capacity),
    TEST(test_blit_packet_golden), TEST(test_blit_ubwc_four_relocs), TEST(test_blit_tiled_nearest),
    TEST(test_blit_format_conversion_and_half), TEST(test_blit_rect_and_filter_errors), TEST(test_blit_surface_alias_errors),
    TEST(test_blit_word_capacity), TEST(test_blit_reloc_capacity), TEST(test_emit_descriptor_storage_alias),
    TEST(test_wfi_and_wait_mem), TEST(test_wait_word_capacity), TEST(test_cache_events_golden), TEST(test_cache_all_masks),
    TEST(test_cache_capacity_transaction), TEST(test_mem_write64_golden), TEST(test_mem_write64_errors_and_edges),
    TEST(test_mem_write64_capacity), TEST(test_timestamp_marker_golden), TEST(test_timestamp_gpu_clock),
    TEST(test_timestamp_errors), TEST(test_timestamp_capacity), TEST(test_all_emit_null_and_corrupt_cs),
    TEST(test_relocation_offset_overflow), TEST(test_batch_failure_then_success), TEST(test_composite_dump_and_roundtrip)
};
static_assert(sizeof(tests) / sizeof(tests[0]) >= 30, "at least 30 independent tests");
} // namespace

int main(int argc, char **argv)
{
    if (argc == 3 && std::strcmp(argv[1], "--dump-pm4") == 0) dump_path = argv[2];
    else if (argc != 1) {
        std::fprintf(stderr, "usage: test_emit_ops [--dump-pm4 <path>]\n");
        return 2;
    }
    for (const auto &test : tests) { test.run(); std::printf("PASS %s\n", test.name); }
    std::printf("PASS: %zu independent W01 test cases, %zu assertions\n", sizeof(tests) / sizeof(tests[0]), assertions);
    return 0;
}
