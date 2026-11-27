/* SPDX-License-Identifier: MIT */
#include "cpu_reference_phase3.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>

using namespace nezha_gpu_test;
namespace p3 = nezha_gpu_test::phase3;
namespace {
std::size_t assertions = 0;
#define CHECK(...) do { ++assertions; if (!(__VA_ARGS__)) throw std::runtime_error(std::string(__func__) + ": " + #__VA_ARGS__); } while (false)
template<typename F> void throws(F action) {
    bool caught = false;
    try { action(); } catch (const std::exception &) { caught = true; }
    CHECK(caught);
}
void exact(const Image &a, const Image &b) {
    const auto result = compare_images(a, b, 0);
    CHECK(result.passed()); CHECK(result.max_err == 0); CHECK(result.compared == a.pixels.size());
}
p3::UbwcModel mixed() { return p3::encode_ubwc_model(p3::ubwc_pattern(32, 8)); }

void alpha_half_rounding() { CHECK(p3::alpha_blend({0, 0, 255, 128}, {0, 0, 0, 255}) == Bgra8({0, 0, 128, 255})); }
void alpha_zero_preserves() { CHECK(p3::alpha_blend({255, 255, 255, 0}, {17, 83, 99, 128}) == Bgra8({17, 83, 99, 128})); }
void alpha_full_replaces() { CHECK(p3::alpha_blend({11, 22, 33, 255}, {255, 0, 255, 0}) == Bgra8({11, 22, 33, 255})); }
void overlap_exact_tiles() {
    const auto scene = p3::overlap_scene();
    const auto a = p3::raster_linear(288, 96, scene), b = p3::raster_tiled(288, 96, scene);
    exact(a.color, b.color); CHECK(a.owner == b.owner);
    CHECK(a.color.at(144, 30).a == 255); CHECK(!(a.color.at(144, 30) == p3::scene_clear));
}
void overlap_partial_tile() { const auto s = p3::overlap_scene(197, 73); exact(p3::raster_linear(197, 73, s).color, p3::raster_tiled(197, 73, s).color); }
void overlap_large_tile() { const auto s = p3::overlap_scene(); exact(p3::raster_linear(288, 96, s).color, p3::raster_tiled(288, 96, s, false, 192, 64).color); }
void alpha_layer_order_matters() {
    auto s = p3::overlap_scene(); const auto a = p3::raster_linear(288, 96, s);
    std::reverse(s.begin(), s.end()); CHECK(!compare_images(a.color, p3::raster_linear(288, 96, s).color).passed());
}
void raster_winding_invariant() {
    auto s = p3::overlap_scene(); const auto a = p3::raster_linear(288, 96, s);
    for (auto &t : s) std::swap(t.vertices[0], t.vertices[1]);
    exact(a.color, p3::raster_tiled(288, 96, s).color);
}
void raster_edge_no_tolerance() {
    const auto a = p3::raster_linear(288, 96, p3::overlap_scene()).color;
    auto b = a; b.at(95, 31).r ^= 1; const auto c = compare_images(a, b);
    CHECK(c.max_err == 1); CHECK(c.mismatched_px == 1); CHECK(!c.passed());
}
void raster_invalid_tile() {
    const auto s = p3::overlap_scene();
    throws([&] { (void)p3::raster_tiled(288, 96, s, false, 95, 32); });
    throws([&] { (void)p3::raster_tiled(288, 96, s, false, 96, 31); });
    throws([&] { (void)p3::raster_tiled(288, 96, s, false, 0, 32); });
}
void raster_invalid_dimensions() {
    throws([] { (void)p3::raster_linear(0, 1, p3::overlap_scene()); });
    throws([] { (void)p3::overlap_scene(std::numeric_limits<std::size_t>::max(), 2); });
}
void raster_invalid_vertices() {
    auto s = p3::overlap_scene(); s[0].vertices[0].z = std::numeric_limits<float>::quiet_NaN();
    throws([&] { (void)p3::raster_linear(288, 96, s); });
    s = p3::overlap_scene(); s[0].vertices[0].r = 256; throws([&] { (void)p3::raster_linear(288, 96, s); });
}
void raster_degenerate_and_empty() {
    throws([] { (void)p3::raster_linear(8, 8, {}); });
    throws([] { (void)p3::raster_linear(8, 8, {p3::Triangle{}}); });
}
void memoryless_depth_matches_linear() {
    const auto s = p3::depth_scene(); const auto a = p3::raster_linear(288, 96, s, true), b = p3::raster_tiled(288, 96, s, true);
    exact(a.color, b.color); CHECK(a.depth == b.depth); CHECK(a.owner == b.owner);
}
void depth_foreground_occludes_far_last() {
    const auto a = p3::raster_tiled(288, 96, p3::depth_scene(), true);
    CHECK(a.owner[30 * 288 + 144] == 3); CHECK(a.depth[30 * 288 + 144] == .125f);
    CHECK(a.color.at(144, 30) == Bgra8({80, 240, 32, 255}));
}
void depth_disabled_is_detected() {
    const auto s = p3::depth_scene(); CHECK(!compare_images(p3::raster_linear(288, 96, s, true).color, p3::raster_tiled(288, 96, s, false).color).passed());
}
void depth_flip_is_detected() {
    auto s = p3::depth_scene(); const auto a = p3::raster_linear(288, 96, s, true);
    for (auto &t : s) for (auto &v : t.vertices) v.z = 1 - v.z;
    CHECK(!compare_images(a.color, p3::raster_tiled(288, 96, s, true).color).passed());
}
void memoryless_reset_across_tiles() {
    auto s = p3::depth_scene(288, 96);
    s.resize(1); for (auto &v : s[0].vertices) v.z = .75f;
    const auto a = p3::raster_tiled(288, 96, s, true);
    CHECK(a.depth[10 * 288 + 100] == .75f); CHECK(a.depth[10 * 288 + 200] == .75f);
    CHECK(a.owner[10 * 288 + 100] == 1); CHECK(a.owner[10 * 288 + 200] == 1);
    CHECK(a.depth[95 * 288 + 287] == 1.0f);
}
void dirty_depth_pixel_is_detected() {
    const auto a = p3::raster_tiled(288, 96, p3::depth_scene(), true).color; auto b = a;
    b.at(96, 32) = p3::scene_clear; CHECK(compare_images(a, b).mismatched_px == 1);
}
void equal_depth_keeps_first() {
    auto s = p3::depth_scene(); s.resize(1); s.push_back(s[0]);
    for (auto &v : s[1].vertices) { v.r = 255; v.g = 255; v.b = 255; }
    const auto a = p3::raster_tiled(288, 96, s, true); CHECK(a.owner[10 * 288 + 10] == 1);
}
void depth_partial_tile() {
    const auto s = p3::depth_scene(121, 67); exact(p3::raster_linear(121, 67, s, true).color, p3::raster_tiled(121, 67, s, true).color);
}
void phone_extent_and_all_seams() {
    const auto a = p3::boundary_image(); CHECK(a.width == 1200); CHECK(a.height == 2608); CHECK(a.pixels.size() == 3129600);
    for (std::size_t x = 96; x < 1200; x += 96) {
        CHECK(!(a.at(x - 1, 17) == a.at(x, 17))); CHECK(a.at(x, 17).g == 255);
    }
    for (std::size_t y = 32; y < 2608; y += 32) {
        CHECK(!(a.at(20, y - 1) == a.at(20, y))); CHECK(a.at(20, y).b == 255);
    }
    exact(a, p3::tile_resolve(a));
}
void tile_gradient_and_geometry() {
    const auto a = p3::boundary_image(1200, 2608);
    CHECK(a.at(600, 100).r == 600 * 255 / 1199); CHECK(a.at(600, 100).g == 100 * 255 / 2607);
    CHECK(a.at(500, 700).a == 255); CHECK(a.at(179, 7) == Bgra8({240, 240, 240, 255}));
}
void vertical_95_96_single_pixel_shift() {
    const auto a = p3::boundary_image(193, 65); auto b = a; b.at(96, 10) = a.at(95, 10);
    const auto c = p3::compare_tiles(a, b); CHECK(c.pixels.mismatched_px == 1); CHECK(c.seam_mismatches == 1);
    CHECK(c.first_x == 96); CHECK(c.first_y == 10); CHECK(c.has_first);
}
void vertical_191_192_single_pixel_shift() {
    const auto a = p3::boundary_image(289, 65); auto b = a; b.at(192, 10) = a.at(191, 10);
    const auto c = p3::compare_tiles(a, b); CHECK(c.pixels.mismatched_px == 1); CHECK(c.first_x == 192);
}
void horizontal_31_32_single_pixel_shift() {
    const auto a = p3::boundary_image(193, 65); auto b = a; b.at(10, 32) = a.at(10, 31);
    const auto c = p3::compare_tiles(a, b); CHECK(c.pixels.mismatched_px == 1); CHECK(c.first_y == 32); CHECK(c.seam_mismatches == 1);
}
void horizontal_63_64_single_pixel_shift() {
    const auto a = p3::boundary_image(193, 97); auto b = a; b.at(10, 64) = a.at(10, 63);
    CHECK(p3::compare_tiles(a, b).pixels.mismatched_px == 1);
}
void tile_corner_identity() {
    const auto a = p3::boundary_image(289, 97);
    CHECK(!(a.at(95, 31) == a.at(96, 32))); CHECK(!(a.at(96, 32) == a.at(192, 64)));
}
void resolve_missing_right_edge_detected() {
    const auto a = p3::boundary_image(); auto b = p3::tile_resolve(a); b.at(1199, 2607) = {0, 0, 0, 0};
    const auto c = p3::compare_tiles(a, b); CHECK(c.pixels.mismatched_px == 1); CHECK(c.first_x == 1199); CHECK(c.first_y == 2607);
}
void resolve_missing_bottom_row_detected() {
    const auto a = p3::boundary_image(197, 73); auto b = p3::tile_resolve(a);
    for (std::size_t x = 0; x < b.width; ++x) b.at(x, b.height - 1) = {0, 0, 0, 0};
    CHECK(p3::compare_tiles(a, b).pixels.mismatched_px == 197);
}
void tile_partial_and_large_resolve() { const auto a = p3::boundary_image(197, 73); exact(a, p3::tile_resolve(a, 192, 64)); }
void tile_interior_mutation_distinguished() {
    const auto a = p3::boundary_image(197, 73); auto b = a; b.at(20, 10).r ^= 1;
    const auto c = p3::compare_tiles(a, b); CHECK(c.pixels.mismatched_px == 1); CHECK(c.seam_mismatches == 0);
}
void tile_invalid_shape_and_bounds() {
    throws([] { (void)p3::tile_resolve(Image{1, 1, {}}); });
    throws([] { (void)p3::boundary_pixel(2, 0, 2, 2); });
    const auto a = p3::boundary_image(2, 2), b = p3::boundary_image(3, 2); CHECK(!p3::compare_tiles(a, b).pixels.shape_equal);
}
void glass_known_integer_weights() {
    CHECK(p3::glass_mix({0, 64, 128, 255}, {255, 64, 0, 255}, 0) == Bgra8({32, 64, 112, 255}));
    CHECK(p3::glass_mix({0, 64, 128, 255}, {255, 64, 0, 255}, 3) == Bgra8({223, 64, 16, 255}));
}
void glass_four_pass_exact() { const auto a = p3::boundary_image(288, 96); exact(p3::glass_linear(a), p3::glass_tiled(a)); }
void glass_missing_fourth_pass_detected() {
    const auto a = p3::boundary_image(197, 73); CHECK(!compare_images(p3::glass_linear(a, 4), p3::glass_tiled(a, 3)).passed());
}
void glass_previous_pixel_is_required() {
    const auto a = p3::boundary_image(197, 73); auto b = p3::glass_tiled(a);
    b.at(96, 32) = p3::glass_mix(a.at(96, 32), p3::glass_source(96, 32, 3), 3);
    CHECK(compare_images(p3::glass_linear(a), b).mismatched_px == 1);
}
void glass_partial_tile_global_coordinates() { const auto a = p3::boundary_image(197, 73); exact(p3::glass_linear(a), p3::glass_tiled(a, 4, 192, 64)); }
void glass_invalid_pass() {
    const auto a = p3::boundary_image(4, 4); throws([&] { (void)p3::glass_linear(a, 0); });
    throws([&] { (void)p3::glass_tiled(a, 5); }); throws([] { (void)p3::glass_source(0, 0, 4); });
}
void glass_gaussian_and_alpha() {
    CHECK(p3::glass_source(0, 0, 0) == Bgra8({5, 3, 7, 255}));
    const auto a = p3::glass_tiled(p3::boundary_image(97, 33)); for (const auto &p : a.pixels) CHECK(p.a == 255);
}
void ubwc_constant_block_compressed() {
    const auto a = make_image(16, 4, {7, 19, 83, 255}); const auto model = p3::encode_ubwc_model(a);
    CHECK(model.compressed_blocks == 1); CHECK(model.escape_blocks == 0); CHECK(model.payload.size() == 4); exact(a, p3::decode_ubwc_model(model));
}
void ubwc_escape_block_uncompressed() {
    const auto a = gradient_image(16, 4); const auto model = p3::encode_ubwc_model(a);
    CHECK(model.compressed_blocks == 0); CHECK(model.escape_blocks == 1); CHECK(model.payload.size() == 256); exact(a, p3::decode_ubwc_model(model));
}
void ubwc_mixed_branches_roundtrip() {
    const auto a = p3::ubwc_pattern(256, 128); const auto m = p3::encode_ubwc_model(a);
    CHECK(m.compressed_blocks == 256); CHECK(m.escape_blocks == 256); CHECK(m.payload.size() < a.pixels.size() * 4);
    exact(a, p3::decode_ubwc_model(m));
}
void ubwc_texture_samples_both_branches() {
    const auto a = p3::ubwc_pattern(32, 8); const auto m = p3::encode_ubwc_model(a);
    for (std::size_t y = 0; y < 8; ++y) for (std::size_t x = 0; x < 32; ++x) CHECK(p3::sample_ubwc_model(m, x, y) == a.at(x, y));
}
void ubwc_partial_edge_blocks() { const auto a = p3::ubwc_pattern(19, 7); const auto m = p3::encode_ubwc_model(a); CHECK(m.blocks.size() == 4); exact(a, p3::decode_ubwc_model(m)); }
void ubwc_escape_corruption_rejected() {
    auto m = mixed(); const auto it = std::find_if(m.blocks.begin(), m.blocks.end(), [](const auto &b) { return b.kind == p3::BlockKind::Escape; });
    CHECK(it != m.blocks.end()); m.payload[it->offset + 17] ^= 1; throws([&] { (void)p3::decode_ubwc_model(m); });
}
void ubwc_constant_corruption_rejected() { auto m = mixed(); m.payload[0] ^= 1; throws([&] { (void)p3::decode_ubwc_model(m); }); }
void ubwc_escape_decoded_pixel_mutation() {
    const auto a = p3::ubwc_pattern(32, 8); auto b = p3::decode_ubwc_model(p3::encode_ubwc_model(a)); b.at(17, 1).b ^= 1;
    CHECK(compare_images(a, b).mismatched_px == 1); CHECK(compare_images(a, b).max_err == 1);
}
void ubwc_metadata_corruption_rejected() { auto m = mixed(); ++m.blocks[1].x; throws([&] { p3::validate_ubwc_model(m); }); }
void ubwc_payload_truncation_rejected() { auto m = mixed(); m.payload.pop_back(); throws([&] { p3::validate_ubwc_model(m); }); }
void ubwc_trailing_payload_rejected() { auto m = mixed(); m.payload.push_back(0); throws([&] { p3::validate_ubwc_model(m); }); }
void ubwc_hbb15_rejected() { p3::UbwcConfig c; c.highest_bank_bit = 15; throws([&] { (void)p3::encode_ubwc_model(p3::ubwc_pattern(16, 4), c); }); }
void ubwc_version_swizzle_macrotile_rejected() {
    for (unsigned i = 0; i < 3; ++i) {
        auto m = mixed(); if (i == 0) m.config.version = 5; else if (i == 1) m.config.swizzle_levels = 5; else m.config.macrotile_mode = 0;
        throws([&] { p3::validate_ubwc_model(m); });
    }
}
void ubwc_counts_kind_and_offset_rejected() {
    auto m = mixed(); ++m.escape_blocks; throws([&] { p3::validate_ubwc_model(m); });
    m = mixed(); m.blocks[0].kind = static_cast<p3::BlockKind>(99); throws([&] { p3::validate_ubwc_model(m); });
    m = mixed(); m.blocks[0].offset = std::numeric_limits<std::size_t>::max(); throws([&] { p3::validate_ubwc_model(m); });
}
void ubwc_sample_bounds() { auto m = mixed(); throws([&] { (void)p3::sample_ubwc_model(m, 32, 0); }); }
void mdss_direct_vs_native_linear() {
    const auto m = mixed(); p3::MdssCaps caps; caps.ubwc6 = false;
    const auto a = p3::scanout_model(m), b = p3::scanout_model(m, caps);
    CHECK(a.mode == p3::ScanoutMode::DirectUbwc); CHECK(b.mode == p3::ScanoutMode::NativeLinearResolve);
    CHECK(b.reason == "mdss_no_ubwc6"); exact(a.pixels, b.pixels);
}
void mdss_format_downgrade() { const auto m = mixed(); p3::MdssCaps c; c.bgra8 = false; CHECK(p3::scanout_model(m, c).reason == "mdss_format"); }
void mdss_alignment_downgrade() {
    const auto m = mixed(); CHECK(p3::scanout_model(m, {}, 4097).reason == "mdss_alignment");
    CHECK(p3::scanout_model(m, {}, 4096, 63).mode == p3::ScanoutMode::NativeLinearResolve);
}
void mdss_extent_downgrade() { const auto m = mixed(); p3::MdssCaps c; c.max_width = 31; CHECK(p3::scanout_model(m, c).reason == "mdss_extent"); }
void mdss_phone_full_equivalence() {
    const auto a = p3::ubwc_pattern(1200, 2608); const auto m = p3::encode_ubwc_model(a); p3::MdssCaps c; c.ubwc6 = false;
    CHECK(m.compressed_blocks > 0); CHECK(m.escape_blocks > 0); exact(a, p3::scanout_model(m).pixels); exact(a, p3::scanout_model(m, c).pixels);
}
void sample_six_cases_and_format() {
    CHECK(p3::cases.size() == 6);
    CHECK(hwtest_line(p3::cases[0], {}, 1, "raw", "cpu_reference_only").find("max_err=0 mismatched_px=0") != std::string::npos);
    throws([] { (void)p3::sample_comparison(6); });
}

const std::vector<std::pair<const char *, std::function<void()>>> tests = {
#define TEST(name) {#name, name}
    TEST(alpha_half_rounding), TEST(alpha_zero_preserves), TEST(alpha_full_replaces),
    TEST(overlap_exact_tiles), TEST(overlap_partial_tile), TEST(overlap_large_tile), TEST(alpha_layer_order_matters),
    TEST(raster_winding_invariant), TEST(raster_edge_no_tolerance), TEST(raster_invalid_tile),
    TEST(raster_invalid_dimensions), TEST(raster_invalid_vertices), TEST(raster_degenerate_and_empty),
    TEST(memoryless_depth_matches_linear), TEST(depth_foreground_occludes_far_last), TEST(depth_disabled_is_detected),
    TEST(depth_flip_is_detected), TEST(memoryless_reset_across_tiles), TEST(dirty_depth_pixel_is_detected),
    TEST(equal_depth_keeps_first), TEST(depth_partial_tile), TEST(phone_extent_and_all_seams), TEST(tile_gradient_and_geometry),
    TEST(vertical_95_96_single_pixel_shift), TEST(vertical_191_192_single_pixel_shift),
    TEST(horizontal_31_32_single_pixel_shift), TEST(horizontal_63_64_single_pixel_shift), TEST(tile_corner_identity),
    TEST(resolve_missing_right_edge_detected), TEST(resolve_missing_bottom_row_detected), TEST(tile_partial_and_large_resolve),
    TEST(tile_interior_mutation_distinguished), TEST(tile_invalid_shape_and_bounds), TEST(glass_known_integer_weights),
    TEST(glass_four_pass_exact), TEST(glass_missing_fourth_pass_detected), TEST(glass_previous_pixel_is_required),
    TEST(glass_partial_tile_global_coordinates), TEST(glass_invalid_pass), TEST(glass_gaussian_and_alpha),
    TEST(ubwc_constant_block_compressed), TEST(ubwc_escape_block_uncompressed), TEST(ubwc_mixed_branches_roundtrip),
    TEST(ubwc_texture_samples_both_branches), TEST(ubwc_partial_edge_blocks), TEST(ubwc_escape_corruption_rejected),
    TEST(ubwc_constant_corruption_rejected), TEST(ubwc_escape_decoded_pixel_mutation), TEST(ubwc_metadata_corruption_rejected),
    TEST(ubwc_payload_truncation_rejected), TEST(ubwc_trailing_payload_rejected), TEST(ubwc_hbb15_rejected),
    TEST(ubwc_version_swizzle_macrotile_rejected), TEST(ubwc_counts_kind_and_offset_rejected), TEST(ubwc_sample_bounds),
    TEST(mdss_direct_vs_native_linear), TEST(mdss_format_downgrade), TEST(mdss_alignment_downgrade),
    TEST(mdss_extent_downgrade), TEST(mdss_phone_full_equivalence), TEST(sample_six_cases_and_format)
#undef TEST
};
std::filesystem::path output_path(const char *name) {
    namespace fs = std::filesystem;
    const fs::path path(name), root("/home/hua/nezha-work/ios27/adreno840-native/.cache");
    if (fs::is_symlink(path)) throw std::invalid_argument("symlink sample output");
    const auto absolute = fs::weakly_canonical(path), relative = absolute.lexically_relative(root);
    if (relative.empty() || *relative.begin() == ".." || std::distance(relative.begin(), relative.end()) < 2)
        throw std::invalid_argument("sample output must be inside .cache/<subdir>");
    if (!fs::is_directory(absolute.parent_path())) throw std::invalid_argument("sample output parent must exist");
    return absolute;
}
} // namespace

int main(int argc, char **argv) {
    try {
        std::filesystem::path sample;
        if (argc == 3 && std::string(argv[1]) == "--emit-sample-phase3-hwtest") sample = output_path(argv[2]);
        else if (argc == 2 && std::string(argv[1]) == "--help") { std::puts("usage: test_cpu_reference_phase3 [--emit-sample-phase3-hwtest <cache-path>]"); return 0; }
        else if (argc != 1) throw std::invalid_argument("unknown Phase 3 test argument");
        for (const auto &test : tests) { test.second(); std::printf("PASS %s\n", test.first); }
        std::printf("PASS: %zu independent cases, %zu assertions; CPU reference only; UBWC semantic model, not hardware bitstream\n", tests.size(), assertions);
        if (!sample.empty()) {
            std::ofstream output(sample); if (!output) throw std::runtime_error("cannot open Phase 3 sample");
            for (std::size_t i = 0; i < p3::cases.size(); ++i) {
                const auto started = std::chrono::steady_clock::now(); const auto comparison = p3::sample_comparison(i);
                const auto elapsed = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count());
                CHECK(comparison.passed()); CHECK(comparison.max_err == 0);
                output << hwtest_line(p3::cases[i], comparison, elapsed, "raw", "cpu_reference_only") << '\n';
            }
            output.close(); if (!output) throw std::runtime_error("incomplete Phase 3 sample");
        }
    } catch (const std::exception &error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
    return 0;
}
