/* SPDX-License-Identifier: MIT */
#include "cpu_reference_phase4_ext.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

using namespace nezha_gpu_test;
namespace p4 = nezha_gpu_test::phase4_ext;
namespace {
std::size_t cases_run = 0, checks = 0;
std::string current;
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) throw std::runtime_error(#__VA_ARGS__); } while (false)
void run(const char *name, const std::function<void()> &f) { current = name; f(); ++cases_run; }
template<class F> void rejects(F f) {
    bool rejected = false;
    try { f(); } catch (const std::exception &) { rejected = true; }
    CHECK(rejected);
}
p4::Triangle white_triangle(float extent = 2) {
    return {{{0, 0, 1, 1, 1, 1, 0, 0}, {extent, 0, 1, 1, 1, 1, 0, 0}, {0, extent, 1, 1, 1, 1, 0, 0}}};
}
p4::MsaaReference msaa() { return p4::msaa4x_reference(2, 2, white_triangle()); }
p4::TessReference tess(unsigned factor = 4) {
    return p4::amplify_two_views(p4::tessellate_triangle(white_triangle(16), factor), 16, 16, {{0, 16}});
}
p4::Nv12Frame gray_frame(std::uint8_t y = 16) {
    auto f = p4::nv12_fixture(4, 4);
    std::fill(f.y.begin(), f.y.end(), y); std::fill(f.uv.begin(), f.uv.end(), 128); return f;
}
p4::CameraReference camera() { return p4::camera155_reference(p4::nv12_fixture(8, 6, 3), 4, 3, {1, 1, 7, 5}); }
void exact(const Image &a, const Image &b) { CHECK(compare_images(a, b).passed()); }
void msaa_tests() {
    run("msaa_standard_rotated_positions", [] {
        CHECK(p4::sample_locations[0] == std::array<double, 2>({.375, .125}));
        CHECK(p4::sample_locations[3] == std::array<double, 2>({.625, .875}));
        std::set<double> xs, ys;
        for (auto s : p4::sample_locations) { xs.insert(s[0]); ys.insert(s[1]); }
        CHECK(xs.size() == 4 && ys.size() == 4);
    });
    run("msaa_diagonal_known_masks_and_average", [] {
        const auto r = msaa(); CHECK(r.coverage_mask == std::vector<std::uint8_t>({15, 5, 5, 0}));
        CHECK(r.resolved.at(0, 0) == Bgra8({255, 255, 255, 255}));
        CHECK(r.resolved.at(1, 0) == Bgra8({128, 128, 128, 255}));
        CHECK(r.resolved.at(1, 1) == p4::clear_pixel);
    });
    run("msaa_single_sample_has_sharp_diagonal", [] {
        const auto r = msaa(); CHECK(r.single_sample.at(1, 0) == p4::clear_pixel);
        CHECK(!compare_images(r.single_sample, r.resolved, 1).passed());
    });
    run("msaa_per_sample_color_known", [] {
        const auto r = msaa(); CHECK(r.samples[1][0] == Bgra8({255, 255, 255, 255}));
        CHECK(r.samples[1][1] == p4::clear_pixel); CHECK(r.samples[1][2] == r.samples[1][0]);
    });
    run("msaa_reversed_winding_preserved", [] {
        auto t = white_triangle(); std::swap(t[1], t[2]); CHECK(p4::compare_msaa(msaa(), p4::msaa4x_reference(2, 2, t)).passed());
    });
    run("msaa_resolve_rounds_half_up_all_channels", [] {
        std::vector<std::array<Bgra8, 4>> samples(1, {{{0, 2, 4, 6}, {1, 3, 5, 7}, {0, 2, 4, 6}, {1, 3, 5, 7}}});
        CHECK(p4::resolve_msaa4x(1, 1, samples).at(0, 0) == Bgra8({1, 3, 5, 7}));
    });
    run("msaa_one_lsb_tolerated", [] { const auto a = msaa(); auto b = a; b.resolved.at(1, 0).b++; CHECK(p4::compare_msaa(a, b).passed()); });
    run("msaa_two_lsb_refused", [] { const auto a = msaa(); auto b = a; b.resolved.at(1, 0).b += 2; CHECK(!p4::compare_msaa(a, b).passed()); });
    run("msaa_missing_sample_refused", [] { const auto a = msaa(); auto b = a; b.samples[1][2] = p4::clear_pixel; CHECK(!p4::compare_msaa(a, b).passed()); });
    run("msaa_sample_slot_shift_refused", [] { const auto a = msaa(); auto b = a; std::swap(b.samples[1][0], b.samples[1][1]); CHECK(!p4::compare_msaa(a, b).passed()); });
    run("msaa_coverage_overwrite_refused", [] { const auto a = msaa(); auto b = a; b.coverage_mask[3] = 1; CHECK(!p4::compare_msaa(a, b).passed()); });
    run("msaa_missing_storage_refused", [] { auto r = msaa(); r.samples.pop_back(); rejects([&] { (void)p4::compare_msaa(r, r); }); });
    run("msaa_invalid_mask_refused", [] { auto r = msaa(); r.coverage_mask[0] = 16; rejects([&] { (void)p4::compare_msaa(r, r); }); });
    run("msaa_resolve_extent_refused", [] { rejects([] { (void)p4::resolve_msaa4x(2, 2, {}); }); });
    run("msaa_zero_extent_refused", [] { rejects([] { (void)p4::msaa4x_reference(0, 2, white_triangle()); }); });
    run("msaa_overflow_extent_refused", [] { rejects([] { (void)p4::msaa4x_reference(std::numeric_limits<std::size_t>::max(), 2, white_triangle()); }); });
    run("msaa_degenerate_refused", [] { auto t = white_triangle(); t[2] = t[1]; rejects([&] { (void)p4::msaa4x_reference(2, 2, t); }); });
    run("msaa_nonfinite_vertex_refused", [] { auto t = white_triangle(); t[0].x = std::numeric_limits<float>::quiet_NaN(); rejects([&] { (void)p4::msaa4x_reference(2, 2, t); }); });
    run("msaa_invalid_color_refused", [] { auto t = white_triangle(); t[0].r = 2; rejects([&] { (void)p4::msaa4x_reference(2, 2, t); }); });
    run("msaa_offscreen_clipped_to_clear", [] { auto t = white_triangle(); for (auto &v : t) { v.x -= 8; v.y -= 8; } const auto r = p4::msaa4x_reference(2, 2, t); exact(r.resolved, make_image(2, 2, p4::clear_pixel)); });
}
void visibility_tests() {
    run("visibility_exact_front_partial_hidden_counts", [] { const auto r = p4::visibility_reference(8, 8, p4::visibility_scene()); CHECK(r.passing_fragments == std::vector<std::uint64_t>({6, 22, 0})); CHECK(r.query_results == r.passing_fragments); });
    run("visibility_boolean_known", [] { const auto r = p4::visibility_reference(8, 8, p4::visibility_scene(), p4::QueryMode::Boolean); CHECK(r.query_results == std::vector<std::uint64_t>({1, 1, 0})); });
    run("visibility_front_depth_survives", [] { const auto r = p4::visibility_reference(8, 8, p4::visibility_scene()); CHECK(r.depth[0] == .25); CHECK(r.color.at(0, 0) == p4::visibility_scene()[0].color); CHECK(r.depth.back() == 1); });
    run("visibility_depth_write_disabled_changes_counts", [] { const auto r = p4::visibility_reference(8, 8, p4::visibility_scene(), p4::QueryMode::Counting, p4::DepthCompare::Less, false); CHECK(r.query_results == std::vector<std::uint64_t>({6, 28, 6})); });
    run("visibility_less_equal_includes_ties", [] { const auto t = p4::visibility_scene()[0]; const auto r = p4::visibility_reference(8, 8, {t, t}, p4::QueryMode::Counting, p4::DepthCompare::LessEqual); CHECK(r.query_results == std::vector<std::uint64_t>({6, 6})); });
    run("visibility_less_excludes_ties", [] { const auto t = p4::visibility_scene()[0]; CHECK(p4::visibility_reference(8, 8, {t, t}).query_results == std::vector<std::uint64_t>({6, 0})); });
    run("visibility_query_slot_overwrite_detected", [] { const auto a = p4::visibility_reference(8, 8, p4::visibility_scene()); auto b = a; std::swap(b.query_results[0], b.query_results[1]); CHECK(!p4::compare_visibility(a, b).passed()); });
    run("visibility_wrong_boolean_detected", [] { const auto a = p4::visibility_reference(8, 8, p4::visibility_scene(), p4::QueryMode::Boolean); auto b = a; b.query_results[2] = 1; CHECK(!p4::compare_visibility(a, b).passed()); });
    run("visibility_fragment_omission_detected", [] { const auto a = p4::visibility_reference(8, 8, p4::visibility_scene()); auto b = a; --b.passing_fragments[1]; CHECK(!p4::compare_visibility(a, b).passed()); });
    run("visibility_depth_corruption_detected", [] { const auto a = p4::visibility_reference(8, 8, p4::visibility_scene()); auto b = a; b.depth[0] = .5; CHECK(!p4::compare_visibility(a, b).passed()); });
    run("visibility_nan_depth_detected", [] { const auto a = p4::visibility_reference(8, 8, p4::visibility_scene()); auto b = a; b.depth[0] = std::numeric_limits<double>::quiet_NaN(); CHECK(!p4::compare_visibility(a, b).passed()); });
    run("visibility_truncated_depth_refused", [] { auto r = p4::visibility_reference(8, 8, p4::visibility_scene()); r.depth.pop_back(); rejects([&] { (void)p4::compare_visibility(r, r); }); });
    run("visibility_invalid_query_mode_refused", [] { rejects([] { (void)p4::visibility_reference(8, 8, p4::visibility_scene(), static_cast<p4::QueryMode>(42)); }); });
    run("visibility_invalid_depth_mode_refused", [] { rejects([] { (void)p4::visibility_reference(8, 8, p4::visibility_scene(), p4::QueryMode::Counting, static_cast<p4::DepthCompare>(42)); }); });
    run("visibility_empty_scene_refused", [] { rejects([] { (void)p4::visibility_reference(8, 8, {}); }); });
    run("visibility_depth_outside_range_refused", [] { auto s = p4::visibility_scene(); s[0].vertices[0].z = -1; rejects([&] { (void)p4::visibility_reference(8, 8, s); }); });
    run("visibility_degenerate_refused", [] { auto s = p4::visibility_scene(); s[0].vertices[2] = s[0].vertices[1]; rejects([&] { (void)p4::visibility_reference(8, 8, s); }); });
}
void tess_tests() {
    run("tess_factor1_exact_control_and_triangle", [] { const auto m = p4::tessellate_triangle(white_triangle(), 1); CHECK(m.vertices.size() == 3); CHECK(m.indices.size() == 3); CHECK(m.vertices[m.indices[1]].x == 2); });
    run("tess_factor4_lattice_and_triangle_counts", [] { const auto m = p4::tessellate_triangle(white_triangle(16), 4); CHECK(m.vertices.size() == 15 && m.indices.size() == 48); CHECK(m.vertices[5].x == 4 && m.vertices[5].y == 0); });
    run("tess_barycentric_color_known", [] { const auto m = p4::tessellate_triangle(triangle_vertices(16, 16), 2); CHECK(m.vertices[1].r == .5f && m.vertices[1].b == .5f); });
    run("tess_maximum_factor32_bounded", [] { const auto m = p4::tessellate_triangle(white_triangle(16), 32); CHECK(m.vertices.size() == 561 && m.indices.size() == 3072); });
    run("tess_two_views_match_and_no_overdraw", [] { const auto r = tess(); for (std::size_t y = 0; y < 16; ++y) for (std::size_t x = 0; x < 16; ++x) { CHECK(r.image.at(x, y) == r.image.at(x + 16, y)); CHECK(r.coverage[y * 32 + x] <= 1); } });
    run("tess_factor1_vs_factor4_same_pixels", [] { CHECK(p4::compare_tess(tess(1), tess(4)).passed()); });
    run("tess_factor3_matches_unsubdivided", [] { CHECK(p4::compare_tess(tess(1), tess(3)).passed()); });
    run("tess_all_integer_factors_preserve_shared_edges", [] {
        const auto expected = tess(1);
        for (unsigned factor = 2; factor <= 32; ++factor)
            CHECK(p4::compare_tess(expected, tess(factor)).passed());
    });
    run("tess_reverse_control_winding_preserved", [] { auto t = white_triangle(16); std::swap(t[1], t[2]); CHECK(p4::compare_tess(tess(), p4::amplify_two_views(p4::tessellate_triangle(t, 4), 16, 16, {{0, 16}})).passed()); });
    run("tess_missing_second_view_detected", [] { const auto a = tess(); auto b = a; for (std::size_t y = 0; y < 16; ++y) for (std::size_t x = 16; x < 32; ++x) b.image.at(x, y) = p4::clear_pixel; CHECK(!p4::compare_tess(a, b).passed()); });
    run("tess_coverage_overdraw_detected", [] { const auto a = tess(); auto b = a; ++b.coverage[0]; CHECK(!p4::compare_tess(a, b).passed()); });
    run("tess_viewport_shift_detected", [] { const auto a = tess(); auto b = a; b.viewport_offsets[1] = 15; CHECK(!p4::compare_tess(a, b).passed()); });
    run("tess_missing_index_refused", [] { auto m = p4::tessellate_triangle(white_triangle(), 4); m.indices.pop_back(); rejects([&] { (void)p4::amplify_two_views(m, 16, 16, {{0, 16}}); }); });
    run("tess_index_out_of_bounds_refused", [] { auto m = p4::tessellate_triangle(white_triangle(), 4); m.indices[0] = 1000; rejects([&] { (void)p4::amplify_two_views(m, 16, 16, {{0, 16}}); }); });
    run("tess_duplicate_triangle_detected", [] { auto m = p4::tessellate_triangle(white_triangle(16), 4); std::copy_n(m.indices.begin(), 3, m.indices.begin() + 3); CHECK(!p4::compare_tess(tess(), p4::amplify_two_views(m, 16, 16, {{0, 16}})).passed()); });
    run("tess_zero_factor_refused", [] { rejects([] { (void)p4::tessellate_triangle(white_triangle(), 0); }); });
    run("tess_excessive_factor_refused", [] { rejects([] { (void)p4::tessellate_triangle(white_triangle(), 33); }); });
    run("tess_overlapping_viewports_refused", [] { const auto m = p4::tessellate_triangle(white_triangle(), 4); rejects([&] { (void)p4::amplify_two_views(m, 16, 16, {{0, 8}}); }); });
    run("tess_negative_offset_refused", [] { const auto m = p4::tessellate_triangle(white_triangle(), 4); rejects([&] { (void)p4::amplify_two_views(m, 16, 16, {{-1, 16}}); }); });
    run("tess_clips_geometry_to_each_view", [] { auto t = white_triangle(32); const auto r = p4::amplify_two_views(p4::tessellate_triangle(t, 4), 16, 16, {{0, 16}}); CHECK(*std::max_element(r.coverage.begin(), r.coverage.end()) == 1); });
}
void camera_tests() {
    run("camera_video_black_white_known", [] { CHECK(p4::bt709_nv12_pixel(gray_frame(16), 0, 0) == Bgra8({0, 0, 0, 255})); CHECK(p4::bt709_nv12_pixel(gray_frame(235), 0, 0) == Bgra8({255, 255, 255, 255})); });
    run("camera_video_middle_gray_known", [] { CHECK(p4::bt709_nv12_pixel(gray_frame(126), 0, 0) == Bgra8({128, 128, 128, 255})); });
    run("camera_bt709_red_vector_known", [] { auto f = gray_frame(63); for (std::size_t i = 0; i < f.uv.size(); i += 2) { f.uv[i] = 102; f.uv[i + 1] = 240; } CHECK(p4::bt709_nv12_pixel(f, 0, 0) == Bgra8({0, 1, 255, 255})); });
    run("camera_bt709_blue_vector_known", [] { auto f = gray_frame(32); for (std::size_t i = 0; i < f.uv.size(); i += 2) { f.uv[i] = 240; f.uv[i + 1] = 118; } CHECK(p4::bt709_nv12_pixel(f, 0, 0) == Bgra8({255, 0, 1, 255})); });
    run("camera_full_range_known_endpoints", [] { auto f = gray_frame(0); f.range = p4::YuvRange::Full; CHECK(p4::bt709_nv12_pixel(f, 0, 0) == Bgra8({0, 0, 0, 255})); std::fill(f.y.begin(), f.y.end(), 255); CHECK(p4::bt709_nv12_pixel(f, 0, 0) == Bgra8({255, 255, 255, 255})); });
    run("camera_preview_center_sampling_known", [] { const auto f = p4::nv12_fixture(8, 6, 3); const auto r = camera(); CHECK(r.preview.at(0, 0) == p4::bt709_nv12_pixel(f, 1, 1)); CHECK(r.preview.at(3, 2) == p4::bt709_nv12_pixel(f, 7, 5)); });
    run("camera_photo_odd_crop_preserves_chroma_phase", [] { const auto f = p4::nv12_fixture(8, 6, 3); const auto r = camera(); for (std::size_t y = 0; y < 5; ++y) for (std::size_t x = 0; x < 7; ++x) CHECK(r.photo.at(x, y) == p4::bt709_nv12_pixel(f, x + 1, y + 1)); });
    run("camera_pitch_padding_does_not_affect_outputs", [] { const auto a = p4::camera155_reference(p4::nv12_fixture(8, 6), 4, 3, {1, 1, 7, 5}); CHECK(p4::compare_camera(a, camera()).passed()); });
    run("camera_full_resolution_4096x3072_photo", [] { auto f = p4::nv12_fixture(4096, 3072); std::fill(f.y.begin(), f.y.end(), 235); std::fill(f.uv.begin(), f.uv.end(), 128); const auto r = p4::camera155_reference(f, 32, 24, {0, 0, 4096, 3072}); CHECK(r.photo.pixels.size() == 12582912); CHECK(r.photo.pixels.front() == r.photo.pixels.back()); CHECK(r.photo.pixels.back() == Bgra8({255, 255, 255, 255})); });
    run("camera_edge_one_pixel_crop", [] { const auto f = p4::nv12_fixture(8, 6); const auto r = p4::camera155_reference(f, 1, 1, {7, 5, 1, 1}); CHECK(r.photo.at(0, 0) == p4::bt709_nv12_pixel(f, 7, 5)); });
    run("camera_uv_swap_changes_color_detected", [] { const auto a = camera(); auto f = p4::nv12_fixture(8, 6, 3); for (std::size_t y = 0; y < 3; ++y) for (std::size_t x = 0; x < 8; x += 2) std::swap(f.uv[y * f.uv_stride + x], f.uv[y * f.uv_stride + x + 1]); CHECK(!p4::compare_camera(a, p4::camera155_reference(f, 4, 3, {1, 1, 7, 5})).passed()); });
    run("camera_preview_one_lsb_tolerated", [] { const auto a = camera(); auto b = a; b.preview.pixels[0].g ^= 1; CHECK(p4::compare_camera(a, b).passed()); });
    run("camera_photo_two_lsb_detected", [] { const auto a = camera(); auto b = a; b.photo.pixels[0].g += 2; CHECK(!p4::compare_camera(a, b).passed()); });
    run("camera_crop_metadata_shift_detected", [] { const auto a = camera(); auto b = a; b.photo_crop.x = 0; CHECK(!p4::compare_camera(a, b).passed()); });
    run("camera_missing_photo_pixel_refused", [] { const auto a = camera(); auto b = a; b.photo.pixels.pop_back(); rejects([&] { (void)p4::compare_camera(a, b); }); });
    run("camera_odd_sensor_extent_refused", [] { rejects([] { (void)p4::nv12_fixture(7, 6); }); });
    run("camera_y_plane_truncated_refused", [] { auto f = gray_frame(); f.y.pop_back(); rejects([&] { (void)p4::camera155_reference(f, 1, 1, {0, 0, 1, 1}); }); });
    run("camera_uv_plane_truncated_refused", [] { auto f = gray_frame(); f.uv.pop_back(); rejects([&] { (void)p4::bt709_nv12_pixel(f, 0, 0); }); });
    run("camera_invalid_pitch_refused", [] { auto f = gray_frame(); f.uv_stride = 3; rejects([&] { (void)p4::bt709_nv12_pixel(f, 0, 0); }); });
    run("camera_crop_outside_sensor_refused", [] { rejects([] { (void)p4::camera155_reference(gray_frame(), 2, 2, {3, 0, 2, 2}); }); });
    run("camera_crop_offset_overflow_refused", [] { rejects([] { (void)p4::camera155_reference(gray_frame(), 2, 2, {std::numeric_limits<std::size_t>::max(), 0, 1, 1}); }); });
    run("camera_zero_preview_refused", [] { rejects([] { (void)p4::camera155_reference(gray_frame(), 0, 2, {0, 0, 1, 1}); }); });
    run("camera_zero_crop_refused", [] { rejects([] { (void)p4::camera155_reference(gray_frame(), 1, 1, {0, 0, 0, 1}); }); });
    run("camera_pixel_bounds_refused", [] { rejects([] { (void)p4::bt709_nv12_pixel(gray_frame(), 4, 0); }); });
    run("camera_invalid_color_range_refused", [] { auto f = gray_frame(); f.range = static_cast<p4::YuvRange>(9); rejects([&] { (void)p4::bt709_nv12_pixel(f, 0, 0); }); });
}
void numeric_tests() {
    run("half_known_codes_and_signed_zero", [] { CHECK(p4::float_to_half(1) == 0x3c00); CHECK(p4::float_to_half(-2) == 0xc000); CHECK(p4::float_to_half(-0.0f) == 0x8000); CHECK(std::signbit(p4::half_to_float(0x8000))); });
    run("half_ties_to_even", [] { CHECK(p4::float_to_half(1.00048828125f) == 0x3c00); CHECK(p4::float_to_half(1.00146484375f) == 0x3c02); });
    run("half_subnormals_and_tie_to_zero", [] { CHECK(p4::float_to_half(std::ldexp(1.0f, -24)) == 1); CHECK(p4::float_to_half(std::ldexp(1.0f, -25)) == 0); CHECK(p4::half_to_float(1) == std::ldexp(1.0f, -24)); });
    run("half_all_finite_codes_exact_roundtrip", [] { for (unsigned i = 0; i < 65536; ++i) if ((i & 0x7c00) != 0x7c00) CHECK(p4::float_to_half(p4::half_to_float(static_cast<std::uint16_t>(i))) == i); });
    run("half_overflow_refused", [] { rejects([] { (void)p4::float_to_half(65520); }); });
    run("half_nan_input_refused", [] { rejects([] { (void)p4::float_to_half(std::numeric_limits<float>::quiet_NaN()); }); });
    run("half_infinite_code_refused", [] { rejects([] { (void)p4::half_to_float(0x7c00); }); });
    run("gemm_known_rectangular_fp32", [] { const p4::Matrix a{2, 3, {1, 2, 3, 4, 5, 6}}, b{3, 2, {7, 8, 9, 10, 11, 12}}; CHECK(p4::gemm_reference(a, b, p4::Accumulation::Float32).values == std::vector<float>({58, 64, 139, 154})); });
    run("gemm_known_rectangular_fp16", [] { const p4::Matrix a{2, 3, {1, 2, 3, 4, 5, 6}}, b{3, 2, {7, 8, 9, 10, 11, 12}}; CHECK(p4::gemm_reference(a, b, p4::Accumulation::Float16).values == std::vector<float>({58, 64, 139, 154})); });
    run("gemm_identity_and_signs", [] { const p4::Matrix a{2, 2, {-3, 7, .5f, -2}}, identity{2, 2, {1, 0, 0, 1}}; CHECK(p4::gemm_reference(a, identity, p4::Accumulation::Float16).values == a.values); });
    run("gemm_fp16_accumulation_rounds_each_step", [] { const p4::Matrix a{1, 4, {1, .0003f, .0003f, .0003f}}, b{4, 1, {1, 1, 1, 1}}; CHECK(p4::gemm_reference(a, b, p4::Accumulation::Float16).values[0] == 1); CHECK(p4::gemm_reference(a, b, p4::Accumulation::Float32).values[0] > 1.0008f); });
    run("gemm_relative_error_non_dyadic", [] { const p4::Matrix a{2, 3, {.1f, .2f, .3f, .4f, .5f, .6f}}, b{3, 2, {.7f, .8f, .9f, 1.1f, 1.2f, 1.3f}}; const auto exact64 = p4::gemm_precise(a, b); CHECK(p4::compare_gemm(exact64, p4::gemm_reference(a, b, p4::Accumulation::Float32), 2, 2, 1e-5).passed()); CHECK(p4::compare_gemm(exact64, p4::gemm_reference(a, b, p4::Accumulation::Float16), 2, 2, .02).passed()); });
    run("gemm_relative_error_excess_detected", [] { CHECK(!p4::compare_gemm({100}, {1, 1, {103}}, 1, 1, .02, 0).passed()); });
    run("gemm_zero_expected_uses_absolute_tolerance", [] { CHECK(p4::compare_gemm({0}, {1, 1, {.0000005f}}, 1, 1, .01).passed()); CHECK(!p4::compare_gemm({0}, {1, 1, {.0001f}}, 1, 1, .01).passed()); });
    run("gemm_nan_actual_detected", [] { CHECK(!p4::compare_gemm({1}, {1, 1, {std::numeric_limits<float>::quiet_NaN()}}, 1, 1, .01).passed()); });
    run("gemm_transposed_shape_detected", [] { CHECK(!p4::compare_gemm({1, 2}, {2, 1, {1, 2}}, 1, 2, .01).passed()); });
    run("gemm_missing_storage_detected", [] { CHECK(!p4::compare_gemm({1, 2}, {1, 2, {1}}, 1, 2, .01).passed()); });
    run("gemm_inner_dimension_refused", [] { rejects([] { (void)p4::gemm_reference({1, 2, {1, 2}}, {1, 1, {1}}, p4::Accumulation::Float32); }); });
    run("gemm_nonfinite_input_refused", [] { rejects([] { (void)p4::gemm_reference({1, 1, {std::numeric_limits<float>::infinity()}}, {1, 1, {1}}, p4::Accumulation::Float32); }); });
    run("gemm_fp16_product_overflow_refused", [] { rejects([] { (void)p4::gemm_reference({1, 1, {512}}, {1, 1, {512}}, p4::Accumulation::Float16); }); });
    run("gemm_invalid_tolerance_refused", [] { rejects([] { (void)p4::compare_gemm({1}, {1, 1, {1}}, 1, 1, -1); }); });
    run("gemm_invalid_accumulation_refused", [] { rejects([] { (void)p4::gemm_reference({1, 1, {1}}, {1, 1, {1}}, static_cast<p4::Accumulation>(42)); }); });
    run("blur_constant_rgba_preserved", [] { const auto a = make_image(7, 3, {42, 67, 123, 19}); exact(a, p4::gaussian5_reference(a)); });
    run("blur_single_pixel_clamps_every_tap", [] { const auto a = make_image(1, 1, {1, 2, 3, 4}); exact(a, p4::gaussian5_reference(a)); });
    run("blur_center_impulse_2d_golden", [] { auto a = make_image(5, 5, {0, 0, 0, 255}); a.at(2, 2) = {255, 255, 255, 255}; const auto r = p4::gaussian5_reference(a); CHECK(r.at(2, 2) == Bgra8({36, 36, 36, 255})); CHECK(r.at(0, 0) == Bgra8({1, 1, 1, 255})); CHECK(r.at(2, 1).r == 24); });
    run("blur_edge_clamp_known", [] { auto a = make_image(5, 1, {0, 0, 0, 255}); a.at(0, 0).r = 255; const auto r = p4::gaussian5_reference(a); CHECK(r.at(0, 0).r == 175 && r.at(1, 0).r == 80 && r.at(2, 0).r == 16); });
    run("blur_no_intermediate_unorm_rounding", [] { auto a = make_image(5, 5, {0, 0, 0, 0}); a.at(2, 2).r = 5; CHECK(p4::gaussian5_reference(a).at(2, 2).r == 1); });
    run("blur_missing_vertical_pass_detected", [] { auto a = make_image(5, 5, {0, 0, 0, 255}); a.at(2, 2).b = 255; CHECK(!compare_images(a, p4::gaussian5_reference(a), 1).passed()); });
    run("blur_storage_corruption_refused", [] { auto a = make_image(5, 5, {0, 0, 0, 255}); a.pixels.pop_back(); rejects([&] { (void)p4::gaussian5_reference(a); }); });
    run("blur_zero_extent_refused", [] { rejects([] { (void)p4::gaussian5_reference({0, 1, {}}); }); });
    run("sample_five_case_ids_and_independent_goldens", [] { for (std::size_t i = 0; i < 5; ++i) { const auto r = p4::sample_comparison(i); CHECK(r.passed()); CHECK(r.compared > 0); CHECK(hwtest_line(p4::cases[i], r, 0, "raw", "cpu_reference_only").find("status=PASS") != std::string::npos); } });
    run("sample_out_of_range_refused", [] { rejects([] { (void)p4::sample_comparison(5); }); });
}
std::filesystem::path checked_output(const std::filesystem::path &path) {
    namespace fs = std::filesystem;
    const fs::path root("/home/hua/nezha-work/ios27/adreno840-native/.cache/w08-l4");
    auto raw = path.generic_string(); std::replace(raw.begin(), raw.end(), '\\', '/');
    std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (raw.find("c:") != std::string::npos || raw.find("/mnt/c/") != std::string::npos)
        throw std::invalid_argument("C_DRIVE_OUTPUT_FORBIDDEN");
    const auto target = fs::weakly_canonical(path), relative = target.lexically_relative(root);
    if (fs::weakly_canonical(root) != root || relative.empty() || *relative.begin() == ".." ||
        fs::is_symlink(path) || !fs::is_directory(target.parent_path()))
        throw std::invalid_argument("OUTPUT_OUTSIDE_W08_L4_CACHE");
    return target;
}
} // namespace
int main(int argc, char **argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "usage: test_cpu_reference_phase4_ext [--emit-phase4-ext-hwtest CACHE_PATH]\n"; return 0;
        }
        if (argc != 1 && (argc != 3 || std::string(argv[1]) != "--emit-phase4-ext-hwtest"))
            throw std::invalid_argument("invalid Phase 4 extended test arguments");
        std::filesystem::path output;
        if (argc == 3) output = checked_output(argv[2]);
        msaa_tests(); visibility_tests(); tess_tests(); camera_tests(); numeric_tests(); CHECK(cases_run >= 60);
        std::vector<std::string> lines;
        for (std::size_t i = 0; i < p4::cases.size(); ++i) {
            const auto start = std::chrono::steady_clock::now(); const auto r = p4::sample_comparison(i); CHECK(r.passed());
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
            lines.push_back(hwtest_line(p4::cases[i], r, static_cast<std::uint64_t>(us), "raw", "cpu_reference_only"));
        }
        if (!output.empty()) {
            std::ofstream file(output); if (!file) throw std::runtime_error("sample output open failed");
            for (const auto &line : lines) file << line << '\n';
            file.close(); if (!file) throw std::runtime_error("sample output write failed");
        }
        std::cout << "PASS: " << cases_run << " independent cases, " << checks << " checks; five Phase 4 extended CPU references; CPU reference only\n";
        for (const auto &line : lines) std::cout << line << '\n';
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << current << ": " << e.what() << '\n'; return 1; }
}
