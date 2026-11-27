/* SPDX-License-Identifier: MIT */
#include "cpu_reference_ca_glass.h"
#include "cpu_reference_phase3.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string_view>

namespace {
using namespace nezha_gpu_test;
using namespace nezha_gpu_test::ca_glass;
namespace fs = std::filesystem;

std::size_t cases_run = 0, checks = 0;
const char *active_case = "";

void check(bool valid, const char *expression, int line)
{
    ++checks;
    if (valid) return;
    std::cerr << "FAIL [" << active_case << "] line " << line << ": " << expression << '\n';
    std::exit(EXIT_FAILURE);
}
#define CHECK(expression) check((expression), #expression, __LINE__)

template<class Test> void test(const char *name, Test body)
{
    active_case = name;
    ++cases_run;
    body();
}

template<class Call> bool rejects(Call call)
{
    try { call(); } catch (const std::invalid_argument &) { return true; }
    return false;
}

bool near(double a, double b) { return std::abs(a - b) < 1e-12; }

LinearImage solid(std::size_t width, std::size_t height, LinearRgba color)
{
    return {width, height, std::vector<LinearRgba>(width * height, color)};
}

void test_geometry()
{
    const RoundedRect rounded{2, 2, 12, 8, 3};
    test("rounded rectangle covers its center", [&] { CHECK(rounded_coverage(rounded, 8, 6) == 1); });
    test("straight SDF boundary covers exactly half a pixel", [&] { CHECK(rounded_coverage(rounded, 2, 6) == 0.5); });
    test("subpixel sample outside the straight edge", [&] { CHECK(rounded_coverage(rounded, 1.75, 6) == 0.25); });
    test("one half pixel outside has zero coverage", [&] { CHECK(rounded_coverage(rounded, 1.5, 6) == 0); });
    test("rounded corner excludes the bounding-box corner", [&] { CHECK(rounded_coverage(rounded, 2.5, 2.5) == 0); });
    test("radius zero includes the bounding-box corner", [] { CHECK(rounded_coverage({2, 2, 12, 8, 0}, 2.5, 2.5) == 1); });
    test("top-left and bottom-right have mirror coverage", [&] {
        CHECK(near(rounded_coverage(rounded, 3.1, 2.7), rounded_coverage(rounded, 12.9, 9.3)));
    });
    test("the corner radius is limited by the shorter extent", [] {
        CHECK(near(rounded_coverage({0, 0, 12, 8, 99}, 1.5, 1.5),
                   rounded_coverage({0, 0, 12, 8, 4}, 1.5, 1.5)));
    });
    test("subpixel edge crossing the viewport is still sampled", [] {
        CHECK(near(rounded_coverage({-0.2, 0.25, 5, 4, 0}, 0.5, 0.5), 0.75));
    });
    test("empty rectangle cannot write a pixel", [] { CHECK(rounded_coverage({0, 0, 0, 4, 2}, 0.5, 0.5) == 0); });
}

void test_gaussian()
{
    const auto kernel = gaussian_kernel(2, 1);
    auto impulse = solid(7, 5, {0, 0, 0, 1});
    impulse.pixels[2 * 7 + 3].r = 1;
    const auto horizontal = gaussian_pass(impulse, 2, 1, Axis::Horizontal);
    const auto both = gaussian_pass(horizontal, 2, 1, Axis::Vertical);
    test("normalized kernel sums to one", [&] { CHECK(near(std::accumulate(kernel.begin(), kernel.end(), 0.0), 1)); });
    test("Gaussian taps have symmetric decreasing weights", [&] {
        CHECK(kernel.size() == 5 && kernel[0] == kernel[4] && kernel[1] == kernel[3]);
        CHECK(kernel[0] < kernel[1] && kernel[1] < kernel[2]);
    });
    test("known sigma-one radius-two center tap", [&] { CHECK(near(kernel[2], 0.4026199468942474)); });
    test("zero radius keeps exactly one tap", [] { CHECK(gaussian_kernel(0, 1) == std::vector<double>{1}); });
    test("sigma zero rejects", [] { CHECK(rejects([] { gaussian_kernel(2, 0); })); });
    test("nonfinite sigma rejects", [] {
        CHECK(rejects([] { gaussian_kernel(2, std::numeric_limits<double>::quiet_NaN()); }));
    });
    test("horizontal pass touches only the impulse row", [&] {
        CHECK(horizontal.pixels[1 * 7 + 3].r == 0);
        CHECK(near(horizontal.pixels[2 * 7 + 2].r, kernel[1]));
    });
    test("vertical pass spreads into non-impulse rows", [&] {
        CHECK(near(both.pixels[1 * 7 + 3].r, kernel[1] * kernel[2]));
        CHECK(near(both.pixels[0 * 7 + 5].r, kernel[0] * kernel[0]));
    });
    test("direct two-dimensional Gaussian agrees at all pixels", [&] {
        /* Independent radial Gaussian on the impulse, with the 2D normalizer. */
        double scale = 0;
        for (int y = -2; y <= 2; ++y)
            for (int x = -2; x <= 2; ++x)
                scale += std::exp(-(x * x + y * y) / 2.0);
        for (int y = 0; y < 5; ++y) {
            for (int x = 0; x < 7; ++x) {
                const auto dx = x - 3, dy = y - 2;
                const double want = std::abs(dx) > 2 ? 0 : std::exp(-(dx * dx + dy * dy) / 2.0) / scale;
                CHECK(near(both.pixels[y * 7 + x].r, want));
                CHECK(near(both.pixels[y * 7 + x].a, 1));
            }
        }
    });
    test("image borders use clamped-edge taps", [&] {
        auto edge = solid(3, 1, {0, 0, 0, 1});
        edge.pixels[0].r = 1;
        CHECK(near(gaussian_pass(edge, 2, 1, Axis::Horizontal).pixels[0].r, kernel[0] + kernel[1] + kernel[2]));
    });
    test("single-pixel image keeps all channels", [] {
        const auto p = gaussian_pass(solid(1, 1, {0.2, 0.3, 0.4, 0.5}), 3, 1, Axis::Vertical).pixels[0];
        CHECK(near(p.r, 0.2) && near(p.g, 0.3) && near(p.b, 0.4) && near(p.a, 0.5));
    });
    test("transparent neighbors contribute premultiplied color", [] {
        LinearImage row{2, 1, {{0, 0, 0, 0}, {1, 0, 0, 1}}};
        const auto p = gaussian_pass(row, 1, 1, Axis::Horizontal).pixels[0];
        CHECK(p.r > 0 && near(p.r, p.a));
    });
    test("bad Gaussian input shape rejects", [] {
        CHECK(rejects([] { gaussian_pass({2, 3, {}}, 1, 1, Axis::Horizontal); }));
    });
}

void test_color()
{
    test("premultiplied source over checks RGB and alpha", [] {
        const auto result = source_over({0.1, 0.2, 0.3, 0.25}, {0.4, 0.3, 0.2, 0.6});
        CHECK(near(result.r, 0.4) && near(result.g, 0.425) && near(result.b, 0.45) && near(result.a, 0.7));
    });
    test("transparent source keeps the backdrop", [] {
        const auto p = source_over({}, {0.2, 0.3, 0.4, 0.5});
        CHECK(p.r == 0.2 && p.g == 0.3 && p.b == 0.4 && p.a == 0.5);
    });
    test("opaque source covers the backdrop", [] {
        const auto p = source_over({0.5, 0.4, 0.3, 1}, {0.1, 0.2, 0.3, 0.4});
        CHECK(p.r == 0.5 && p.g == 0.4 && p.b == 0.3 && p.a == 1);
    });
    test("desaturation uses Display P3 luminance", [] {
        const auto p = adjust_backdrop({0.8, 0, 0, 1}, 0, 1);
        CHECK(near(p.r, 0.18317965128) && p.r == p.g && p.g == p.b && p.a == 1);
    });
    test("brightness scales RGB and leaves alpha intact", [] {
        const auto p = adjust_backdrop({0.8, 0.4, 0.2, 0.5}, 1, 0.5);
        CHECK(near(p.r, 0.4) && near(p.g, 0.2) && near(p.b, 0.1) && p.a == 0.5);
    });
    test("saturation above one preserves extended-range values", [] {
        const auto p = adjust_backdrop({0.95, 0, 0, 1}, 2, 1);
        CHECK(p.r > 1 && p.b < 0);
    });
    test("sRGB eight-bit black and white channel codes", [] {
        CHECK((encode_target({0, 0, 0, 1}, Target::Srgb8) == Pixel{0, 0, 0, 255}));
        CHECK((encode_target({1, 1, 1, 1}, Target::Srgb8) == Pixel{255, 255, 255, 255}));
    });
    test("sRGB eight-bit gray uses the nonlinear transfer", [] {
        CHECK((encode_target({0.18, 0.18, 0.18, 0.5}, Target::Srgb8) == Pixel{118, 118, 118, 128}));
    });
    test("XR10 linear has black at 384 and white at 894", [] {
        CHECK((encode_target({0, 0, 0, 0}, Target::Xr10Linear) == Pixel{384, 384, 384, 384}));
        CHECK((encode_target({1, 1, 1, 1}, Target::Xr10Linear) == Pixel{894, 894, 894, 894}));
    });
    test("XR10 sRGB uses gamma for color and linear alpha", [] {
        CHECK((encode_target({0.18, 0.18, 0.18, 0.5}, Target::Xr10Srgb) == Pixel{619, 619, 619, 639}));
        CHECK(encode_target({0.18, 0.18, 0.18, 0.5}, Target::Xr10Linear).r == 476);
    });
    test("Display P3 red maps past sRGB gamut in linear XR10", [] {
        CHECK((encode_target({1, 0, 0, 1}, Target::Xr10Linear) == Pixel{374, 363, 1009, 894}));
        CHECK((encode_target({1, 0, 0, 1}, Target::Srgb8) == Pixel{0, 0, 255, 255}));
    });
    test("XR10 sRGB reflects the transfer curve below black", [] {
        CHECK((encode_target({-0.18, -0.18, -0.18, 1}, Target::Xr10Srgb) == Pixel{149, 149, 149, 894}));
    });
    test("XR10 clamps to ten-bit storage and clamps alpha to unity", [] {
        CHECK((encode_target({4, 4, 4, 2}, Target::Xr10Linear) == Pixel{1023, 1023, 1023, 894}));
        CHECK((encode_target({-4, -4, -4, -1}, Target::Xr10Linear) == Pixel{0, 0, 0, 384}));
    });
    test("nonfinite colors reject", [] {
        CHECK(rejects([] { encode_target({0, std::numeric_limits<double>::infinity(), 0, 1}, Target::Xr10Srgb); }));
    });
}

Comparison test_sweep(const std::vector<Composite> &reference)
{
    Comparison summary;
    test("sweep covers twelve configurations across three target encodings", [&] {
        CHECK(reference.size() == 12);
        for (const auto target : {Target::Srgb8, Target::Xr10Linear, Target::Xr10Srgb})
            CHECK(std::count_if(reference.begin(), reference.end(), [&](const Composite &sample) {
                return sample.image.target == target;
            }) == 4);
    });
    test("all sweep reference pixels pass their comparator", [&] {
        for (const auto &sample : reference)
            summary.merge(compare_composite(sample, sample.image).pixels);
        CHECK(summary.passed() && summary.max_err == 0 && summary.compared == 53 * 37 * reference.size());
    });
    const auto &rounded = reference[1];
    const auto interior = 10 * rounded.image.width + 17;
    const auto corner = 3 * rounded.image.width + 4;
    test("one code at a covered pixel is within the tolerance", [&] {
        auto candidate = rounded.image;
        ++candidate.pixels[interior].r;
        const auto result = compare_composite(rounded, candidate);
        CHECK(rounded.clip_mask[interior] && result.pixels.passed() && result.pixels.max_err == 1);
    });
    test("two codes at a covered pixel fail the sweep", [&] {
        auto candidate = rounded.image;
        candidate.pixels[interior].r += 2;
        const auto result = compare_composite(rounded, candidate);
        CHECK(!result.pixels.passed() && result.pixels.mismatched_px == 1 && result.pixels.max_err == 2);
    });
    test("rounded-corner overdraw from a square clip fails", [&] {
        auto candidate = rounded.image;
        candidate.pixels[corner] = reference[0].image.pixels[corner];
        const auto result = compare_composite(rounded, candidate);
        CHECK(!rounded.clip_mask[corner] && result.leaked_px == 1 && !result.pixels.passed());
    });
    test("even one code outside the clip counts as leakage", [&] {
        auto candidate = rounded.image;
        ++candidate.pixels.front().b;
        const auto result = compare_composite(rounded, candidate);
        CHECK(result.leaked_px == 1 && result.pixels.max_err == 1 && !result.pixels.passed());
    });
    test("an unexpected black block reports every black pixel", [&] {
        auto candidate = rounded.image;
        for (std::size_t y = 10; y < 13; ++y)
            for (std::size_t x = 16; x < 20; ++x)
                candidate.pixels[y * candidate.width + x] = {0, 0, 0, 255};
        const auto result = compare_composite(rounded, candidate);
        CHECK(result.black_px == 12 && result.pixels.mismatched_px == 12 && !result.pixels.passed());
    });
    test("XR10 black uses the XR code rather than integer zero", [&] {
        const auto &xr = reference[4];
        auto candidate = xr.image;
        candidate.pixels[interior] = {384, 384, 384, 894};
        CHECK(compare_composite(xr, candidate).black_px == 1);
    });
    test("intentional black is accepted", [] {
        const auto black = compose(solid(1, 1, {0, 0, 0, 1}), {}, Target::Srgb8);
        CHECK(compare_composite(black, black.image).black_px == 0);
    });
    test("shape, encoding and truncated pixel buffers fail", [&] {
        auto candidate = rounded.image;
        candidate.target = Target::Xr10Linear;
        CHECK(!compare_composite(rounded, candidate).pixels.shape_equal);
        candidate = rounded.image;
        candidate.pixels.pop_back();
        CHECK(!compare_composite(rounded, candidate).pixels.shape_equal);
    });
    test("a code beyond the target precision fails even at one LSB", [] {
        const BackdropPass pass{0, 1, 1, 1, {{{0, 0, 3, 3, 0}, {1, 1, 1, 1}}}};
        const auto golden = compose(solid(3, 3, {}), {pass}, Target::Srgb8);
        auto candidate = golden.image;
        candidate.pixels.front().r = 256;
        const auto result = compare_composite(golden, candidate);
        CHECK(result.pixels.max_err == 1 && !result.pixels.passed());
    });
    test("empty clip does not shade any destination pixel", [&] {
        CHECK(std::none_of(reference.back().clip_mask.begin(), reference.back().clip_mask.end(), [](auto b) { return b; }));
    });
    test("blur kernel offset by one sample fails the comparison", [] {
        auto input = solid(9, 9, {0.04, 0.04, 0.04, 1});
        input.pixels[4 * 9 + 4] = {1, 1, 1, 1};
        const BackdropPass pass{2, 1, 1, 1, {{{0, 0, 9, 9, 0}, {0, 0, 0, 0}}}};
        const auto golden = compose(input, {pass}, Target::Srgb8);
        auto candidate = golden.image;
        candidate.pixels[4 * 9 + 4] = golden.image.pixels[4 * 9 + 5];
        CHECK(compare_composite(golden, candidate).pixels.max_err > 1);
        CHECK(!compare_composite(golden, candidate).pixels.passed());
    });
    test("omitting the vertical blur fails the comparison", [] {
        auto input = solid(9, 9, {0.04, 0.04, 0.04, 1});
        input.pixels[4 * 9 + 4] = {1, 1, 1, 1};
        const BackdropPass pass{2, 1, 1, 1, {{{0, 0, 9, 9, 0}, {0, 0, 0, 0}}}};
        const auto golden = compose(input, {pass}, Target::Srgb8);
        const auto candidate = compose(gaussian_pass(input, 2, 1, Axis::Horizontal), {}, Target::Srgb8);
        CHECK(compare_composite(golden, candidate.image).pixels.mismatched_px > 1);
    });
    test("overlapping passes retain order and read the last backdrop", [] {
        const auto background = solid(7, 5, {0.2, 0.3, 0.4, 1});
        const BackdropPass red{0, 1, 1, 1, {{{0, 0, 7, 5, 0}, {1, 0, 0, 0.5}}}};
        const BackdropPass blue{0, 1, 1, 1, {{{0, 0, 7, 5, 0}, {0, 0, 1, 0.5}}}};
        const auto result = compose(background, {red, blue}, Target::Xr10Linear);
        CHECK(result.image.pixels[0] == encode_target({0.30, 0.075, 0.60, 1}, Target::Xr10Linear));
        CHECK(!compare_composite(result, compose(background, {blue, red}, Target::Xr10Linear).image).pixels.passed());
    });
    test("offscreen panes leave the full output unchanged", [] {
        const auto background = solid(7, 5, {0.2, 0.3, 0.4, 1});
        const BackdropPass pass{2, 1, 1, 1, {{{-10, -9, 7, 5, 1}, {1, 0, 0, 1}}}};
        const auto result = compose(background, {pass}, Target::Srgb8);
        CHECK(result.image.pixels == compose(background, {}, Target::Srgb8).image.pixels);
    });
    test("negative clip extent rejects", [] {
        const BackdropPass pass{0, 1, 1, 1, {{{1, 1, -4, 5, 1}, {1, 1, 1, 1}}}};
        CHECK(rejects([&] { compose(solid(7, 5, {}), {pass}, Target::Srgb8); }));
    });
    return summary;
}

void copy_region(Surface &destination, const Surface &source,
                 std::size_t x, std::size_t y, std::size_t width, std::size_t height)
{
    for (auto row = y; row < y + height; ++row) {
        const auto offset = row * destination.width + x;
        std::copy_n(source.pixels.begin() + offset, width, destination.pixels.begin() + offset);
    }
}

SequenceComparison test_sequence(const std::vector<GlassFrame> &reference)
{
    std::vector<Surface> actual;
    for (const auto &frame : reference) actual.push_back(frame.image);
    const auto valid = compare_glass_live(reference, actual);
    test("all sixty distinct moving frames pass at full pixel coverage", [&] {
        CHECK(reference.size() == 60 && valid.passed() && valid.pixels.compared == 212 * 88 * 60);
        for (std::size_t i = 1; i < reference.size(); ++i) {
            CHECK(reference[i].scroll_x > reference[i - 1].scroll_x || reference[i].scroll_y > reference[i - 1].scroll_y);
            CHECK(reference[i].image.pixels != reference[i - 1].image.pixels);
        }
    });
    test("scene covers vertical scrolling and horizontal paging", [&] {
        CHECK(reference[29].scroll_y > 0 && reference[29].scroll_x == 0);
        CHECK(reference.back().scroll_x > 0 && reference.back().scroll_y == reference[29].scroll_y);
    });

    constexpr std::size_t frame = 18;
    const auto check_fault = [&](std::vector<Surface> &candidate, Fault fault, std::size_t index,
                                 std::size_t x = 0, std::size_t y = 0) {
        const auto result = compare_glass_live(reference, candidate);
        CHECK(!result.passed());
        CHECK(result.faults.size() == 1);
        const auto &found = result.faults.front();
        CHECK(found.kind == fault && found.frame == index && found.x == x && found.y == y);
    };
    const auto check_modified_frame = [&](Fault fault, auto inject, std::size_t x = 0, std::size_t y = 0) {
        auto candidate = actual;
        inject(candidate[frame]);
        check_fault(candidate, fault, frame, x, y);
    };

    test("opaque black frame fails regardless of alpha", [&] {
        check_modified_frame(Fault::BlackFrame, [](Surface &capture) {
            std::fill(capture.pixels.begin(), capture.pixels.end(), Pixel{0, 0, 0, 255});
        });
    });
    test("clear black frame fails", [&] {
        auto candidate = actual;
        std::fill(candidate.front().pixels.begin(), candidate.front().pixels.end(), Pixel{});
        check_fault(candidate, Fault::BlackFrame, 0);
    });
    test("bottom half from the preceding frame tears between scanlines", [&] {
        check_modified_frame(Fault::Tear, [&](Surface &capture) {
            copy_region(capture, actual[frame - 1], 0, 41, capture.width, capture.height - 41);
        }, 0, 41);
    });
    test("top half from the preceding frame tears between scanlines", [&] {
        check_modified_frame(Fault::Tear, [&](Surface &capture) {
            copy_region(capture, actual[frame - 1], 0, 0, capture.width, 43);
        }, 0, 43);
    });
    test("bottom half from the following frame also tears", [&] {
        check_modified_frame(Fault::Tear, [&](Surface &capture) {
            copy_region(capture, actual[frame + 1], 0, 32, capture.width, capture.height - 32);
        }, 0, 32);
    });
    test("previous and following frames can tear without pixels from the target frame", [&] {
        check_modified_frame(Fault::Tear, [&](Surface &capture) {
            copy_region(capture, actual[frame - 1], 0, 0, capture.width, 45);
            copy_region(capture, actual[frame + 1], 0, 45, capture.width, capture.height - 45);
        }, 0, 45);
    });
    test("stale 96-by-32 interior tile reports its exact origin", [&] {
        check_modified_frame(Fault::StaleTile, [&](Surface &capture) {
            copy_region(capture, actual[frame - 1], 96, 32, phase3::tile_width, phase3::tile_height);
        }, 96, 32);
    });
    test("retained tile from more than one frame back is recognized", [&] {
        check_modified_frame(Fault::StaleTile, [&](Surface &capture) {
            copy_region(capture, actual[frame - 4], 96, 0, 96, 32);
        }, 96, 0);
    });
    test("partial tile at the right and bottom also reports its origin", [&] {
        check_modified_frame(Fault::StaleTile, [&](Surface &capture) {
            copy_region(capture, actual[frame - 1], 192, 64, capture.width - 192, capture.height - 64);
        }, 192, 64);
    });
    test("stale first tile remains distinct from tearing", [&] {
        check_modified_frame(Fault::StaleTile, [&](Surface &capture) {
            copy_region(capture, actual[frame - 1], 0, 0, 96, 32);
        });
    });
    test("ordinary single seam corruption fails without fabricating a stale tile", [&] {
        check_modified_frame(Fault::PixelMismatch, [](Surface &capture) {
            capture.pixels[32 * capture.width + 96].b = 0;
        });
    });
    test("identical adjacent captures with nonzero motion fail as frozen", [&] {
        check_modified_frame(Fault::FrozenFrame, [&](Surface &capture) { capture = actual[frame - 1]; });
    });
    test("intentional duplicate during zero displacement is accepted", [&] {
        auto still_reference = reference;
        auto candidate = actual;
        still_reference[frame] = still_reference[frame - 1];
        candidate[frame] = candidate[frame - 1];
        CHECK(compare_glass_live(still_reference, candidate).passed());
    });
    test("one LSB of capture noise is allowed during motion", [&] {
        auto candidate = actual;
        ++candidate[frame].pixels[0].r;
        const auto result = compare_glass_live(reference, candidate);
        CHECK(result.passed() && result.pixels.max_err == 1);
    });
    test("missing frame is reported even if every supplied capture passes", [&] {
        auto candidate = actual;
        candidate.pop_back();
        check_fault(candidate, Fault::FrameCount, 59);
    });
    test("extra frame is reported at the tail", [&] {
        auto candidate = actual;
        candidate.push_back(candidate.back());
        check_fault(candidate, Fault::FrameCount, 60);
    });
    test("truncated capture buffer fails as a shape error", [&] {
        check_modified_frame(Fault::Shape, [](Surface &capture) { capture.pixels.pop_back(); });
    });
    return valid;
}

std::uint64_t elapsed(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
}

void emit(const fs::path &path, const Comparison &sweep, std::uint64_t sweep_us,
          const Comparison &glass, std::uint64_t glass_us)
{
    const fs::path cache("/home/hua/nezha-work/ios27/adreno840-native/.cache");
    const auto output = fs::weakly_canonical(path);
    const auto relative = output.lexically_relative(cache);
    if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." || !relative.has_parent_path())
        throw std::invalid_argument("HWTEST output must be inside project .cache/<subdir>");
    std::ofstream file(output);
    file << hwtest_line(ca_glass::cases[0], sweep, sweep_us, "raw", "cpu_reference_only") << '\n'
         << hwtest_line(ca_glass::cases[1], glass, glass_us, "raw", "cpu_reference_only") << '\n';
    file.close();
    if (!file) throw std::runtime_error("cannot write HWTEST sample");
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 1 && (argc != 3 || std::string_view(argv[1]) != "--emit-ca-glass-hwtest")) {
        std::cerr << "usage: test_cpu_reference_ca_glass [--emit-ca-glass-hwtest FILE]\n";
        return 2;
    }
    try {
        test_geometry();
        test_gaussian();
        test_color();
        const auto sweep_start = std::chrono::steady_clock::now();
        const auto sweep = test_sweep(ca_sweep_reference());
        const auto sweep_us = elapsed(sweep_start);
        const auto glass_start = std::chrono::steady_clock::now();
        const auto glass = test_sequence(glass_live_reference());
        const auto glass_us = elapsed(glass_start);
        CHECK(sweep.passed() && glass.passed() && cases_run >= 40);
        if (argc == 3) emit(argv[2], sweep, sweep_us, glass.pixels, glass_us);
        std::cout << "PASS: " << cases_run << " independent cases, " << checks << " checks; "
                  << sweep.compared << " sweep pixels, " << glass.pixels.compared << " pixels across "
                  << glass_frame_count << " moving frames; CPU reference only\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL [" << active_case << "]: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
