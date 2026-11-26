/* SPDX-License-Identifier: MIT */
#include "cpu_reference.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

using namespace nezha_gpu_test;

namespace {
std::size_t assertions = 0;
void require(bool value, const char *expression, int line) {
    ++assertions;
    if (!value) throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
}
#define CHECK(value) require((value), #value, __LINE__)
template<typename F> void throws(F function) {
    bool caught = false;
    try { function(); } catch (const std::exception &) { caught = true; }
    CHECK(caught);
}

void pattern_determinism() { CHECK(byte_pattern(256, 7) == byte_pattern(256, 7)); CHECK(byte_pattern(256, 7) != byte_pattern(256, 8)); }
void buffer_64_bytes() {
    const auto source = byte_pattern(128), destination = byte_pattern(128, 9);
    const auto expected = buffer_copy_reference(source, destination, 0, 0, 64);
    CHECK(std::equal(source.begin(), source.begin() + 64, expected.begin()));
    CHECK(std::equal(destination.begin() + 64, destination.end(), expected.begin() + 64));
}
void buffer_one_megabyte() {
    const auto source = byte_pattern(1024 * 1024 + 64), destination = byte_pattern(source.size(), 12);
    const auto result = buffer_copy_reference(source, destination, 16, 32, 1024 * 1024);
    CHECK(result[31] == destination[31]); CHECK(result[32] == source[16]);
    CHECK(result[32 + 1024 * 1024] == destination[32 + 1024 * 1024]);
}
void buffer_unaligned_guards() {
    const auto source = byte_pattern(256), destination = byte_pattern(256, 4);
    const auto result = buffer_copy_reference(source, destination, 3, 7, 129);
    for (std::size_t i = 0; i < result.size(); ++i)
        CHECK(result[i] == (i >= 7 && i < 136 ? source[i - 7 + 3] : destination[i]));
}
void buffer_zero_copy() { const auto data = byte_pattern(16); CHECK(buffer_copy_reference(data, data, 16, 16, 0) == data); }
void buffer_bounds() {
    const auto data = byte_pattern(16);
    throws([&] { (void)buffer_copy_reference(data, data, 15, 0, 2); });
    throws([&] { (void)buffer_copy_reference(data, data, 0, 16, 1); });
}
void buffer_mutation() {
    const auto data = byte_pattern(64); auto bad = data; bad[7] ^= 1;
    const auto result = compare_bytes(data, bad); CHECK(!result.passed()); CHECK(result.mismatched_px == 1); CHECK(result.max_err == 1);
}
void buffer_size_mismatch() { CHECK(!compare_bytes({1, 2}, {1}).passed()); }
void clear_256_full() {
    const auto image = gradient_image(256, 256), result = clear_reference(image, {0, 0, 256, 256}, {7, 23, 91, 255});
    CHECK(std::all_of(result.pixels.begin(), result.pixels.end(), [](Bgra8 p) { return p == Bgra8{7, 23, 91, 255}; }));
}
void clear_256_rect() {
    const auto image = gradient_image(256, 256), result = clear_reference(image, {17, 29, 71, 93}, {11, 22, 33, 255});
    for (std::size_t y = 0; y < 256; ++y) for (std::size_t x = 0; x < 256; ++x)
        CHECK(result.at(x, y) == (x >= 17 && x < 88 && y >= 29 && y < 122 ? Bgra8{11, 22, 33, 255} : image.at(x, y)));
}
void clear_phone_rect() {
    const auto image = gradient_image(1200, 2608), result = clear_reference(image, {13, 19, 1171, 2569}, {17, 65, 193, 255});
    CHECK(result.at(13, 19) == Bgra8({17, 65, 193, 255}));
    CHECK(result.at(1183, 2587) == Bgra8({17, 65, 193, 255}));
    CHECK(result.at(1184, 2587) == image.at(1184, 2587)); CHECK(result.at(13, 2588) == image.at(13, 2588));
    for (std::size_t y = 0; y < 2608; ++y) for (std::size_t x = 0; x < 1200; ++x)
        CHECK(result.at(x, y) == (x >= 13 && x < 1184 && y >= 19 && y < 2588 ? Bgra8{17, 65, 193, 255} : image.at(x, y)));
    CHECK(compare_images(result, result).passed());
}
void clear_outside_mutation() {
    const auto expected = clear_reference(gradient_image(32, 32), {4, 4, 8, 8}, {1, 2, 3, 255});
    auto actual = expected; actual.at(0, 0).r ^= 1;
    CHECK(compare_images(expected, actual).mismatched_px == 1);
}
void clear_empty_and_bounds() {
    const auto image = gradient_image(8, 8);
    CHECK(compare_images(image, clear_reference(image, {8, 8, 0, 0}, {0, 0, 0, 0})).passed());
    throws([&] { (void)clear_reference(image, {7, 7, 2, 2}, {0, 0, 0, 0}); });
}
void scale_nearest_down() {
    const auto source = gradient_image(256, 256), result = scale_reference(source, 128, 128, Filter::Nearest);
    for (std::size_t y = 0; y < 128; ++y) for (std::size_t x = 0; x < 128; ++x) CHECK(result.at(x, y) == source.at(2 * x + 1, 2 * y + 1));
}
void scale_nearest_wide() {
    const auto source = gradient_image(256, 256), result = scale_reference(source, 512, 256, Filter::Nearest);
    CHECK(result.at(1, 40) == source.at(0, 40)); CHECK(result.at(511, 255) == source.at(255, 255));
}
void scale_bilinear_midpoint() {
    auto source = make_image(2, 2, {0, 0, 0, 255});
    source.at(1, 0) = {100, 0, 0, 255}; source.at(0, 1) = {200, 0, 0, 255}; source.at(1, 1) = {100, 0, 0, 255};
    CHECK(scale_reference(source, 1, 1, Filter::Bilinear).at(0, 0).b == 100);
}
void scale_bilinear_down_and_wide() {
    const auto source = gradient_image(256, 256);
    const auto down = scale_reference(source, 128, 128, Filter::Bilinear), wide = scale_reference(source, 512, 256, Filter::Bilinear);
    CHECK(compare_images(down, down).passed()); CHECK(compare_images(wide, wide).passed());
    CHECK(wide.at(0, 0) == source.at(0, 0));
}
void scale_tolerance() {
    const auto expected = make_image(2, 2, {50, 50, 50, 255}); auto actual = expected;
    actual.at(1, 1).g = 51; CHECK(compare_images(expected, actual, 1).passed());
    CHECK(!compare_images(expected, actual, 0).passed());
    actual.at(1, 1).g = 52; CHECK(compare_images(expected, actual, 1).mismatched_px == 1);
}
void scale_clamp_edges() {
    const auto source = gradient_image(4, 4);
    CHECK(quantize(sample_reference(source, -2, -2, Filter::Bilinear)) == source.at(0, 0));
    CHECK(quantize(sample_reference(source, 2, 2, Filter::Bilinear)) == source.at(3, 3));
}
void image_validation() {
    throws([] { (void)make_image(0, 1, {0, 0, 0, 0}); });
    throws([] { (void)make_image(std::numeric_limits<std::size_t>::max(), 2, {0, 0, 0, 0}); });
    throws([] { (void)scale_reference({1, 1, {}}, 1, 1, Filter::Nearest); });
}
void add_vec_exact() {
    const auto a = vector_input(4096, false), b = vector_input(4096, true), expected = add_vec_reference(a, b, 2);
    for (std::size_t i = 0; i < a.size(); ++i) CHECK(expected[i] == a[i] + b[i] * 2);
    CHECK(compare_floats(expected, expected).passed());
}
void add_vec_tail_and_guards() {
    const auto expected = add_vec_reference(vector_input(1025, false), vector_input(1025, true), 2);
    std::vector<float> guarded(expected.size() + 16, float_guard);
    std::copy(expected.begin(), expected.end(), guarded.begin() + 8);
    CHECK(guarded[7] == float_guard); CHECK(guarded[1033] == float_guard); CHECK(guarded[1032] == expected.back());
}
void add_vec_tolerance_and_nan() {
    CHECK(compare_floats({1}, {1.0001f}, .001).passed());
    CHECK(!compare_floats({1}, {1.1f}, .001).passed());
    CHECK(!compare_floats({1}, {std::numeric_limits<float>::quiet_NaN()}, .001).passed());
}
void add_vec_invalid() {
    throws([] { (void)add_vec_reference({1}, {1, 2}, 1); });
    throws([] { (void)compare_floats({1}, {1}, -1); });
}
void nonuniform_grid_127_83() {
    const auto grid = nonuniform_grid_reference(); CHECK(grid.size() == 127 * 83 + 16);
    for (std::size_t y = 0; y < 83; ++y) for (std::size_t x = 0; x < 127; ++x) CHECK(grid[8 + y * 127 + x] == grid_value(x, y));
}
void nonuniform_tail_group() {
    const auto grid = nonuniform_grid_reference(); CHECK(grid[8 + 82 * 127 + 126] == grid_value(126, 82));
    CHECK(grid[8 + 82 * 127 + 120] == grid_value(120, 82));
}
void nonuniform_guard_write_detected() {
    const auto expected = nonuniform_grid_reference(); auto actual = expected;
    CHECK(std::all_of(actual.begin(), actual.begin() + 8, [](auto value) { return value == grid_guard; }));
    actual.back() = 0; CHECK(compare_words(expected, actual).mismatched_px == 1);
}
void nonuniform_missing_tail_detected() {
    const auto expected = nonuniform_grid_reference(); auto actual = expected;
    actual[8 + 127 * 83 - 1] = grid_guard; CHECK(!compare_words(expected, actual).passed());
}
void triangle_core_and_background() {
    const auto reference = triangle_reference(256, 256, triangle_vertices(), {5, 9, 13, 255});
    CHECK(reference.image.at(0, 0) == Bgra8({5, 9, 13, 255}));
    CHECK(reference.image.at(96, 96).a == 255); CHECK(reference.tolerance[96 * 256 + 96] == 0);
    CHECK(reference.image.at(96, 96).r > reference.image.at(96, 96).g);
}
void triangle_self_and_core_mutation() {
    const auto expected = triangle_reference(256, 256, triangle_vertices(), {0, 0, 0, 255}); auto actual = expected.image;
    CHECK(compare_raster(expected, actual).passed()); actual.at(96, 96).r ^= 1;
    CHECK(compare_raster(expected, actual).mismatched_px == 1);
}
void triangle_edge_tolerance() {
    const auto expected = triangle_reference(256, 256, triangle_vertices(), {0, 0, 0, 255}); auto actual = expected.image;
    const auto edge_index = static_cast<std::size_t>(std::find(expected.tolerance.begin(), expected.tolerance.end(), 1) - expected.tolerance.begin());
    CHECK(edge_index < actual.pixels.size());
    const auto original = actual.pixels[edge_index].b;
    actual.pixels[edge_index].b = static_cast<std::uint8_t>(original + 1);
    CHECK(compare_raster(expected, actual).passed()); CHECK(compare_raster(expected, actual).max_err == 1);
    actual.pixels[edge_index].b = static_cast<std::uint8_t>(original + 3); CHECK(!compare_raster(expected, actual).passed());
}
void triangle_reversed_winding() {
    auto vertices = triangle_vertices(); const auto a = triangle_reference(256, 256, vertices, {0, 0, 0, 255});
    std::swap(vertices[1], vertices[2]); const auto b = triangle_reference(256, 256, vertices, {0, 0, 0, 255});
    CHECK(compare_images(a.image, b.image).passed());
}
void triangle_degenerate_rejected() { std::array<Vertex, 3> vertices{}; throws([&] { (void)triangle_reference(8, 8, vertices, {0, 0, 0, 0}); }); }
void quad_checker_and_indices() {
    const auto texture = checker_image(32, 32), reference = indexed_quad_reference(64, 64, texture, {0, 1, 2, 0, 2, 3}, {16, 32, 64, 255});
    CHECK(reference.at(1, 1) == alpha_blend_reference(sample_reference(texture, 1.5 / 64, 1.5 / 64, Filter::Nearest), {16, 32, 64, 255}));
    CHECK(reference.at(1, 1) == reference.at(2, 2)); CHECK(!(reference.at(1, 1) == reference.at(12, 1)));
}
void quad_diagonal_single_coverage() {
    const auto texture = make_image(2, 2, {255, 0, 0, 128}); const Bgra8 clear{0, 0, 0, 255};
    const auto reference = indexed_quad_reference(32, 32, texture, {0, 1, 2, 0, 2, 3}, clear);
    for (const auto &pixel : reference.pixels) CHECK(pixel.b == 128 && pixel.a == 255);
}
void quad_index_width_equivalence() {
    const std::vector<std::uint16_t> narrow{0, 1, 2, 0, 2, 3};
    const std::vector<std::uint32_t> expanded(narrow.begin(), narrow.end()); const auto texture = checker_image(8, 8);
    CHECK(compare_images(indexed_quad_reference(32, 32, texture, expanded, {0, 0, 0, 255}),
                         indexed_quad_reference(32, 32, texture, {0, 1, 2, 0, 2, 3}, {0, 0, 0, 255})).passed());
}
void quad_bad_index_rejected() { throws([] { (void)indexed_quad_reference(8, 8, checker_image(2, 2), {0, 1, 9, 0, 2, 3}, {0, 0, 0, 0}); }); }
void blend_known_values() {
    CHECK(alpha_blend_reference({1, 0, 0, .5f}, {0, 0, 0, 255}) == Bgra8({0, 0, 128, 255}));
    CHECK(alpha_blend_reference({1, 0, 0, 0}, {4, 8, 16, 255}) == Bgra8({4, 8, 16, 255}));
}
void fence_fifo_and_reads() {
    const auto state = fence_chain_reference(); std::vector<std::uint32_t> reads(state.begin(), state.end() - 1), order(16);
    for (std::size_t i = 0; i < 16; ++i) order[i] = static_cast<std::uint32_t>(i);
    CHECK(compare_fence_chain(state, reads, order).passed());
}
void fence_bad_completion_order() {
    const auto state = fence_chain_reference(); std::vector<std::uint32_t> reads(state.begin(), state.end() - 1), order(16);
    for (std::size_t i = 0; i < 16; ++i) order[i] = static_cast<std::uint32_t>(i);
    std::swap(order[5], order[6]); CHECK(compare_fence_chain(state, reads, order).mismatched_px == 2);
}
void fence_dirty_read() {
    const auto state = fence_chain_reference(); std::vector<std::uint32_t> reads(state.begin(), state.end() - 1), order(16);
    for (std::size_t i = 0; i < 16; ++i) order[i] = static_cast<std::uint32_t>(i);
    reads[15] = 0; CHECK(!compare_fence_chain(state, reads, order).passed());
}
void hwtest_format_and_token_guard() {
    CHECK(hwtest_line(phase2_cases[0], {}, 7, "raw", "cpu_reference_only") ==
        "HWTEST case=gpu.blit.buffer_copy status=PASS path=raw max_err=0 mismatched_px=0 time_us=7 note=cpu_reference_only");
    throws([] { (void)hwtest_line(phase2_cases[0], {}, 0, "raw", "invalid note"); });
}

Comparison sample_comparison(std::size_t index) {
    // Actual CPU comparisons for the parser fixture; no GPU execution or
    // driver-path evidence is claimed by these host-only measurements.
    switch (index) {
    case 0: {
        const auto source = byte_pattern(1024 * 1024 + 64), initial = byte_pattern(source.size(), 9);
        auto copied = initial; std::copy_n(source.begin() + 4, 1024 * 1024, copied.begin() + 12);
        return compare_bytes(buffer_copy_reference(source, initial, 4, 12, 1024 * 1024), copied);
    }
    case 1: {
        const auto expected = clear_reference(gradient_image(1200, 2608), {13, 19, 1171, 2569}, {17, 65, 193, 255});
        return compare_images(expected, expected);
    }
    case 2: {
        Comparison result; const auto source = gradient_image(256, 256);
        for (auto filter : {Filter::Nearest, Filter::Bilinear}) {
            const auto down = scale_reference(source, 128, 128, filter), wide = scale_reference(source, 512, 256, filter);
            result.merge(compare_images(down, down)); result.merge(compare_images(wide, wide));
        }
        return result;
    }
    case 3: {
        const auto expected = add_vec_reference(vector_input(4096, false), vector_input(4096, true), 2);
        return compare_floats(expected, expected);
    }
    case 4: { const auto expected = nonuniform_grid_reference(); return compare_words(expected, expected); }
    case 5: {
        const auto expected = triangle_reference(256, 256, triangle_vertices(), {16, 32, 64, 255});
        return compare_raster(expected, expected.image);
    }
    case 6: {
        const auto expected = indexed_quad_reference(256, 256, checker_image(32, 32), {0, 1, 2, 0, 2, 3}, {16, 32, 64, 255});
        return compare_images(expected, expected);
    }
    case 7: {
        const auto state = fence_chain_reference(); std::vector<std::uint32_t> reads(state.begin(), state.end() - 1), order(16);
        for (std::size_t i = 0; i < 16; ++i) order[i] = static_cast<std::uint32_t>(i);
        return compare_fence_chain(state, reads, order);
    }
    default: throw std::invalid_argument("sample case index");
    }
}

const std::vector<std::pair<const char *, std::function<void()>>> tests = {
    {"pattern_determinism", pattern_determinism}, {"buffer_64_bytes", buffer_64_bytes},
    {"buffer_one_megabyte", buffer_one_megabyte}, {"buffer_unaligned_guards", buffer_unaligned_guards},
    {"buffer_zero_copy", buffer_zero_copy}, {"buffer_bounds", buffer_bounds}, {"buffer_mutation", buffer_mutation},
    {"buffer_size_mismatch", buffer_size_mismatch}, {"clear_256_full", clear_256_full},
    {"clear_256_rect", clear_256_rect}, {"clear_phone_rect", clear_phone_rect},
    {"clear_outside_mutation", clear_outside_mutation}, {"clear_empty_and_bounds", clear_empty_and_bounds},
    {"scale_nearest_down", scale_nearest_down}, {"scale_nearest_wide", scale_nearest_wide},
    {"scale_bilinear_midpoint", scale_bilinear_midpoint}, {"scale_bilinear_down_and_wide", scale_bilinear_down_and_wide},
    {"scale_tolerance", scale_tolerance}, {"scale_clamp_edges", scale_clamp_edges}, {"image_validation", image_validation},
    {"add_vec_exact", add_vec_exact}, {"add_vec_tail_and_guards", add_vec_tail_and_guards},
    {"add_vec_tolerance_and_nan", add_vec_tolerance_and_nan}, {"add_vec_invalid", add_vec_invalid},
    {"nonuniform_grid_127_83", nonuniform_grid_127_83}, {"nonuniform_tail_group", nonuniform_tail_group},
    {"nonuniform_guard_write_detected", nonuniform_guard_write_detected}, {"nonuniform_missing_tail_detected", nonuniform_missing_tail_detected},
    {"triangle_core_and_background", triangle_core_and_background}, {"triangle_self_and_core_mutation", triangle_self_and_core_mutation},
    {"triangle_edge_tolerance", triangle_edge_tolerance}, {"triangle_reversed_winding", triangle_reversed_winding},
    {"triangle_degenerate_rejected", triangle_degenerate_rejected}, {"quad_checker_and_indices", quad_checker_and_indices},
    {"quad_diagonal_single_coverage", quad_diagonal_single_coverage}, {"quad_index_width_equivalence", quad_index_width_equivalence},
    {"quad_bad_index_rejected", quad_bad_index_rejected}, {"blend_known_values", blend_known_values},
    {"fence_fifo_and_reads", fence_fifo_and_reads}, {"fence_bad_completion_order", fence_bad_completion_order},
    {"fence_dirty_read", fence_dirty_read}, {"hwtest_format_and_token_guard", hwtest_format_and_token_guard}
};
} // namespace

int main(int argc, char **argv) {
    try {
        std::string sample;
        if (argc == 3 && std::string(argv[1]) == "--emit-sample-hwtest") sample = argv[2];
        else if (argc == 2 && std::string(argv[1]) == "--help") { std::puts("usage: test_cpu_reference [--emit-sample-hwtest <path>]"); return 0; }
        else if (argc != 1) throw std::invalid_argument("usage: test_cpu_reference [--emit-sample-hwtest <path>]");
        for (const auto &test : tests) { test.second(); std::printf("PASS %s\n", test.first); }
        std::printf("PASS: %zu independent cases, %zu assertions; CPU reference only\n", tests.size(), assertions);
        if (!sample.empty()) {
            std::ofstream file(sample);
            if (!file) throw std::runtime_error("cannot open HWTEST output");
            // path=raw exercises the acceptance parser format; the explicit
            // CPU note prevents this fixture from becoming a device receipt.
            for (std::size_t i = 0; i < phase2_cases.size(); ++i) {
                const auto started = std::chrono::steady_clock::now();
                const auto result = sample_comparison(i);
                const auto time = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started).count());
                file << hwtest_line(phase2_cases[i], result, time, "raw", "cpu_reference_only") << '\n';
                CHECK(result.passed());
            }
            file.close(); if (!file) throw std::runtime_error("cannot write complete HWTEST output");
        }
    } catch (const std::exception &error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
    return 0;
}
