/* SPDX-License-Identifier: MIT */
#include "cpu_reference_ca_glass.h"
#include "cpu_reference_phase3.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>

namespace nezha_gpu_test::ca_glass {
namespace {

bool valid_size(std::size_t width, std::size_t height, std::size_t count)
{
    return width && height && width <= std::numeric_limits<std::size_t>::max() / height &&
           width * height == count;
}

bool finite(LinearRgba p)
{
    return std::isfinite(p.r) && std::isfinite(p.g) && std::isfinite(p.b) && std::isfinite(p.a);
}

bool same_shape(const Surface &expected, const Surface &actual)
{
    return expected.target == actual.target && expected.width == actual.width &&
           expected.height == actual.height && valid_size(actual.width, actual.height, actual.pixels.size());
}

double unit(double x) { return std::clamp(x, 0.0, 1.0); }

LinearRgba weighted(LinearRgba p, double weight)
{
    return {p.r * weight, p.g * weight, p.b * weight, p.a * weight};
}

LinearRgba mix(LinearRgba a, LinearRgba b, double t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

std::size_t edge_sample(std::size_t center, std::size_t tap, std::size_t radius, std::size_t extent)
{
    if (tap < radius) return center - std::min(center, radius - tap);
    return center + std::min(extent - 1 - center, tap - radius);
}

LinearImage convolve(const LinearImage &source, const std::vector<double> &weights, Axis axis)
{
    LinearImage out{source.width, source.height, std::vector<LinearRgba>(source.pixels.size())};
    const auto radius = weights.size() / 2;
    for (std::size_t y = 0; y < source.height; ++y) {
        for (std::size_t x = 0; x < source.width; ++x) {
            LinearRgba sum{};
            for (std::size_t tap = 0; tap < weights.size(); ++tap) {
                const auto sx = axis == Axis::Horizontal ? edge_sample(x, tap, radius, source.width) : x;
                const auto sy = axis == Axis::Vertical ? edge_sample(y, tap, radius, source.height) : y;
                const auto p = weighted(source.pixels[sy * source.width + sx], weights[tap]);
                sum.r += p.r;
                sum.g += p.g;
                sum.b += p.b;
                sum.a += p.a;
            }
            out.pixels[y * out.width + x] = sum;
        }
    }
    return out;
}

/* Extended sRGB uses the reflected transfer curve for negative inputs.
 * Display P3 and sRGB share D65. Matrix = XYZ_to_lin_sRGB * lin_P3_to_XYZ
 * from CSS Color 4, Section 19. The XR storage quantizer is from Metal. */
double srgb_transfer(double linear)
{
    const double magnitude = std::abs(linear);
    const double encoded = magnitude <= 0.0031308 ? magnitude * 12.92 :
                           1.055 * std::pow(magnitude, 1.0 / 2.4) - 0.055;
    return std::copysign(encoded, linear);
}

std::uint16_t unorm8(double value)
{
    return static_cast<std::uint16_t>(std::lround(unit(value) * 255));
}

std::uint16_t xr10(double value)
{
    return static_cast<std::uint16_t>(std::lround(std::clamp(value * 510 + 384, 0.0, 1023.0)));
}

unsigned pixel_error(Pixel a, Pixel b)
{
    return std::max({std::abs(a.b - b.b), std::abs(a.g - b.g),
                     std::abs(a.r - b.r), std::abs(a.a - b.a)});
}

bool black(Pixel p, Target target)
{
    const unsigned zero = target == Target::Srgb8 ? 0 : 384;
    return p.b == zero && p.g == zero && p.r == zero;
}

PixelComparison compare_pixels(const Surface &expected, const Surface &actual,
                              const std::vector<std::uint8_t> *clip_mask = nullptr)
{
    if (!valid_size(expected.width, expected.height, expected.pixels.size()) ||
        (clip_mask && clip_mask->size() != expected.pixels.size()))
        throw std::invalid_argument("invalid reference surface or clip mask");

    PixelComparison out;
    if (!same_shape(expected, actual)) {
        out.pixels.shape_equal = false;
        out.pixels.mismatched_px = 1;
        return out;
    }

    out.pixels.compared = expected.pixels.size();
    for (std::size_t i = 0; i < out.pixels.compared; ++i) {
        const auto error = pixel_error(expected.pixels[i], actual.pixels[i]);
        const bool leak = error && clip_mask && !(*clip_mask)[i];
        const bool missing = black(actual.pixels[i], actual.target) && !black(expected.pixels[i], expected.target);
        out.pixels.max_err = std::max(out.pixels.max_err, static_cast<double>(error));
        const auto p = actual.pixels[i];
        const unsigned code_limit = actual.target == Target::Srgb8 ? 255 : 1023;
        const bool invalid_code = std::max({p.r, p.g, p.b, p.a}) > code_limit;
        out.pixels.mismatched_px += leak || missing || invalid_code || error > max_lsb;
        out.leaked_px += leak;
        out.black_px += missing;
    }
    return out;
}

LinearImage wallpaper(std::size_t width, std::size_t height, unsigned dx, unsigned dy)
{
    LinearImage image{width, height, std::vector<LinearRgba>(width * height)};
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const auto u = x + dx, v = y + dy;
            const double check = ((u / 13 + v / 9) % 2) * 0.14;
            image.pixels[y * width + x] = {
                0.10 + check + ((5 * u + 7 * v) % 101) / 170.0,
                0.09 + ((3 * u + 11 * v) % 89) / 175.0,
                0.12 + check + ((17 * u + 3 * v) % 97) / 185.0, 1
            };
        }
    }
    return image;
}

void apply_pane(LinearImage &image, const LinearImage &blurred, std::vector<std::uint8_t> &mask,
                const BackdropPass &pass, const Pane &pane)
{
    const auto &clip = pane.clip;
    if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.width) ||
        !std::isfinite(clip.height) || !std::isfinite(clip.radius) || !finite(pane.tint) ||
        clip.width < 0 || clip.height < 0 || clip.radius < 0 || pane.tint.a < 0 || pane.tint.a > 1)
        throw std::invalid_argument("invalid glass pane");
    const auto limit = [](double value, std::size_t extent) {
        return static_cast<std::size_t>(std::clamp(value, 0.0, static_cast<double>(extent)));
    };
    const auto x0 = limit(std::floor(clip.x), image.width), x1 = limit(std::ceil(clip.x + clip.width), image.width);
    const auto y0 = limit(std::floor(clip.y), image.height), y1 = limit(std::ceil(clip.y + clip.height), image.height);
    const LinearRgba tint{pane.tint.r * pane.tint.a, pane.tint.g * pane.tint.a,
                         pane.tint.b * pane.tint.a, pane.tint.a};
    for (auto y = y0; y < y1; ++y) {
        for (auto x = x0; x < x1; ++x) {
            const double coverage = rounded_coverage(clip, x + 0.5, y + 0.5);
            if (coverage == 0) continue;
            const auto i = y * image.width + x;
            const auto backdrop = adjust_backdrop(blurred.pixels[i], pass.saturation, pass.brightness);
            image.pixels[i] = mix(image.pixels[i], source_over(tint, backdrop), coverage);
            mask[i] = 1;
        }
    }
}

std::optional<std::size_t> tear_row(const Surface &actual, const Surface &current, const Surface &adjacent)
{
    if (!same_shape(actual, current) || !same_shape(actual, adjacent)) return std::nullopt;
    constexpr unsigned now = 1, adjacent_frame = 2;
    std::vector<unsigned> matches(actual.height);
    for (std::size_t y = 0; y < actual.height; ++y) {
        bool current_row = true, adjacent_row = true;
        for (std::size_t x = 0; x < actual.width; ++x) {
            const auto i = y * actual.width + x;
            current_row &= pixel_error(actual.pixels[i], current.pixels[i]) <= max_lsb;
            adjacent_row &= pixel_error(actual.pixels[i], adjacent.pixels[i]) <= max_lsb;
        }
        matches[y] = (current_row ? now : 0) | (adjacent_row ? adjacent_frame : 0);
    }
    const auto split = [&matches](unsigned top, unsigned bottom) -> std::optional<std::size_t> {
        bool top_seen = false;
        std::size_t y = 0;
        for (; y < matches.size() && (matches[y] & top); ++y)
            top_seen |= matches[y] == top;
        if (!top_seen || y == matches.size()) return std::nullopt;
        const auto first_bottom = y;
        for (; y < matches.size(); ++y)
            if (!(matches[y] & bottom)) return std::nullopt;
        return first_bottom;
    };
    if (auto row = split(now, adjacent_frame)) return row;
    return split(adjacent_frame, now);
}

bool matches_tile(const Surface &actual, const Surface &reference, std::size_t left, std::size_t top,
                  std::size_t right, std::size_t bottom)
{
    if (!same_shape(actual, reference)) return false;
    for (auto y = top; y < bottom; ++y)
        for (auto x = left; x < right; ++x)
            if (pixel_error(actual.pixels[y * actual.width + x], reference.pixels[y * actual.width + x]) > max_lsb)
                return false;
    return true;
}

void find_stale_tiles(SequenceComparison &out, std::size_t frame, const Surface &actual,
                      const std::vector<GlassFrame> &reference)
{
    const auto &current = reference[frame].image;
    for (std::size_t top = 0; top < actual.height; top += phase3::tile_height) {
        const auto bottom = top + std::min(phase3::tile_height, actual.height - top);
        for (std::size_t left = 0; left < actual.width; left += phase3::tile_width) {
            const auto right = left + std::min(phase3::tile_width, actual.width - left);
            if (matches_tile(actual, current, left, top, right, bottom)) continue;
            for (auto prior = frame; prior > 0; --prior) {
                if (!matches_tile(actual, reference[prior - 1].image, left, top, right, bottom)) continue;
                out.faults.push_back({Fault::StaleTile, frame, left, top});
                break;
            }
        }
    }
}
} // namespace

double rounded_coverage(RoundedRect rect, double sample_x, double sample_y)
{
    if (rect.width <= 0 || rect.height <= 0) return 0;
    const double half_x = rect.width * 0.5, half_y = rect.height * 0.5;
    const double radius = std::clamp(rect.radius, 0.0, std::min(half_x, half_y));
    const double x = std::abs(sample_x - rect.x - half_x) - (half_x - radius);
    const double y = std::abs(sample_y - rect.y - half_y) - (half_y - radius);
    const double distance = std::hypot(std::max(x, 0.0), std::max(y, 0.0)) +
                            std::min(std::max(x, y), 0.0) - radius;
    return unit(0.5 - distance);
}

std::vector<double> gaussian_kernel(unsigned radius, double sigma)
{
    if (!std::isfinite(sigma) || sigma <= 0) throw std::invalid_argument("invalid Gaussian sigma");
    std::vector<double> weights(2 * static_cast<std::size_t>(radius) + 1);
    for (std::size_t i = 0; i < weights.size(); ++i) {
        const double ratio = (static_cast<double>(i) - radius) / sigma;
        weights[i] = std::exp(-0.5 * ratio * ratio);
    }
    const double sum = std::accumulate(weights.begin(), weights.end(), 0.0);
    for (auto &weight : weights) weight /= sum;
    return weights;
}

LinearImage gaussian_pass(const LinearImage &source, unsigned radius, double sigma, Axis axis)
{
    if (!valid_size(source.width, source.height, source.pixels.size()))
        throw std::invalid_argument("invalid linear image");
    return convolve(source, gaussian_kernel(radius, sigma), axis);
}

LinearRgba source_over(LinearRgba source, LinearRgba destination)
{
    const auto remaining = weighted(destination, 1 - source.a);
    return {source.r + remaining.r, source.g + remaining.g, source.b + remaining.b, source.a + remaining.a};
}

LinearRgba adjust_backdrop(LinearRgba p, double saturation, double brightness)
{
    const double luminance = 0.2289745641 * p.r + 0.6917385218 * p.g + 0.0792869141 * p.b;
    return {(luminance + (p.r - luminance) * saturation) * brightness,
            (luminance + (p.g - luminance) * saturation) * brightness,
            (luminance + (p.b - luminance) * saturation) * brightness, p.a};
}

Pixel encode_target(LinearRgba p, Target target)
{
    if (!finite(p)) throw std::invalid_argument("nonfinite linear color");
    const double r = 1.2249401763 * p.r - 0.2249401763 * p.g;
    const double g = -0.0420569547 * p.r + 1.0420569547 * p.g;
    const double b = -0.0196375546 * p.r - 0.0786360456 * p.g + 1.0982736002 * p.b;
    switch (target) {
    case Target::Srgb8:
        return {unorm8(srgb_transfer(b)), unorm8(srgb_transfer(g)), unorm8(srgb_transfer(r)), unorm8(p.a)};
    case Target::Xr10Linear:
        return {xr10(b), xr10(g), xr10(r), xr10(unit(p.a))};
    case Target::Xr10Srgb:
        return {xr10(srgb_transfer(b)), xr10(srgb_transfer(g)), xr10(srgb_transfer(r)), xr10(unit(p.a))};
    }
    throw std::invalid_argument("invalid output target");
}

Composite compose(const LinearImage &background, const std::vector<BackdropPass> &passes, Target target)
{
    if (!valid_size(background.width, background.height, background.pixels.size()))
        throw std::invalid_argument("invalid backdrop image");
    LinearImage image = background;
    Composite out{{image.width, image.height, target, {}}, std::vector<std::uint8_t>(image.pixels.size())};
    for (const auto &pass : passes) {
        if (!std::isfinite(pass.saturation) || !std::isfinite(pass.brightness) ||
            pass.saturation < 0 || pass.brightness < 0)
            throw std::invalid_argument("invalid backdrop adjustment");
        const auto kernel = gaussian_kernel(pass.radius, pass.sigma);
        const auto blurred = pass.radius ? convolve(convolve(image, kernel, Axis::Horizontal), kernel, Axis::Vertical) : image;
        for (const auto &pane : pass.panes) apply_pane(image, blurred, out.clip_mask, pass, pane);
    }
    out.image.pixels.reserve(image.pixels.size());
    for (auto p : image.pixels) out.image.pixels.push_back(encode_target(p, target));
    return out;
}

std::vector<Composite> ca_sweep_reference()
{
    const auto background = wallpaper(53, 37, 0, 0);
    struct Setting {
        Target target;
        RoundedRect clip;
        LinearRgba tint;
        unsigned radius;
        double sigma, saturation, brightness;
        bool stacked = false;
    };
    constexpr Setting settings[] = {
        {Target::Srgb8,      { 4, 3, 37, 26, 0},       {0.8, 0.4, 0.2, 0.45}, 0, 1,   1,   1},
        {Target::Srgb8,      { 4, 3, 37, 26, 8},       {0.2, 0.4, 0.8, 0.30}, 2, 1.1, 1,   1},
        {Target::Srgb8,      { 4.3, 3.7, 37, 26, 13},  {0.7, 0.4, 0.2, 0},    3, 1.5, 0,   0.8},
        {Target::Srgb8,      {-4.2, 19.3, 61, 19, 8},  {0.2, 0.8, 0.3, 0.45}, 4, 2,   1.8, 1.2},
        {Target::Xr10Linear,{ 4, 3, 37, 26, 0},       {1.1, 0,   0,   0.9},  0, 1,   1,   1},
        {Target::Xr10Linear,{21.5, 8, 34.3, 27, 11},  {0.2, 1.1, 0.2, 0.55}, 1, 0.7, 1.5, 1.2},
        {Target::Xr10Linear,{ 2.1, 1, 49, 33.4, 16},  {0.1, 0.6, 1.1, 0.45}, 3, 1.3, 0.4, 0.7, true},
        {Target::Xr10Linear,{ 7, 5, 33, 26, 20},      {0.8, 0.8, 0.8, 1},    4, 2.2, 1.6, 1.1},
        {Target::Xr10Srgb,  { 1, 3, 50, 29, 11},      {0.9, 0,   0.4, 0.9},  2, 1,   1.6, 1.1},
        {Target::Xr10Srgb,  {18.5,-5, 29, 37, 10},    {0.2, 0.7, 1.3, 0.6},  4, 1.8, 0.6, 1.3, true},
        {Target::Xr10Srgb,  {13.4, 2.1, 28, 30, 13}, {0.6, 0.8, 0.2, 0.25}, 2, 0.8, 1.3, 0.9, true},
        {Target::Xr10Srgb,  { 9, 5, 0, 21, 5},        {0.4, 0.4, 0.4, 0.25}, 0, 1,   1,   1},
    };
    std::vector<Composite> out;
    for (const auto &s : settings) {
        std::vector<BackdropPass> passes{{s.radius, s.sigma, s.saturation, s.brightness, {{s.clip, s.tint}}}};
        if (s.stacked)
            passes.push_back({2, 0.9, 0.8, 1.1, {{{12.25, 6.75, 39, 26, 9}, {0.95, 0.6, 0.3, 0.25}}}});
        out.push_back(compose(background, passes, s.target));
    }
    return out;
}

PixelComparison compare_composite(const Composite &expected, const Surface &actual)
{
    return compare_pixels(expected.image, actual, &expected.clip_mask);
}

std::vector<GlassFrame> glass_live_reference()
{
    constexpr int width = 212, height = 88;
    constexpr int stride_x = 38, stride_y = 23;
    constexpr std::array<LinearRgba, 3> tints{{{0.65, 0.82, 1.05, 0.26},
                                               {0.92, 0.64, 0.94, 0.32},
                                               {0.61, 0.89, 0.68, 0.30}}};
    std::vector<GlassFrame> frames;
    frames.reserve(glass_frame_count);
    for (int frame = 0; frame < static_cast<int>(glass_frame_count); ++frame) {
        const int dx = std::max(0, frame - 29) * 5, dy = std::min(frame, 29) * 3;
        BackdropPass pass{2, 1.15, 1.22, 1.04, {}};
        for (int row = 0; row <= height / stride_y + 1; ++row) {
            for (int col = 0; col <= width / stride_x + 1; ++col) {
                const double x = col * stride_x - dx % stride_x + 3.25;
                const double y = row * stride_y - dy % stride_y + 2.75;
                const auto tint = tints[(row + col + dx / stride_x + dy / stride_y) % tints.size()];
                pass.panes.push_back({{x, y, 33, 19, 6.5}, tint});
            }
        }
        frames.push_back({dx, dy, compose(wallpaper(width, height, dx, dy), {pass}, Target::Srgb8).image});
    }
    return frames;
}

SequenceComparison compare_glass_live(const std::vector<GlassFrame> &expected, const std::vector<Surface> &actual)
{
    if (expected.size() != glass_frame_count) throw std::invalid_argument("invalid glass reference frame count");
    SequenceComparison out;
    const auto count = std::min(actual.size(), expected.size());
    if (actual.size() != expected.size()) {
        out.pixels.shape_equal = false;
        out.pixels.mismatched_px = 1;
        out.faults.push_back({Fault::FrameCount, count});
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto &current = expected[i].image;
        const auto &capture = actual[i];
        const auto comparison = compare_pixels(current, capture).pixels;
        out.pixels.merge(comparison);
        if (!comparison.shape_equal) {
            out.faults.push_back({Fault::Shape, i});
            continue;
        }
        if (std::all_of(capture.pixels.begin(), capture.pixels.end(), [&](Pixel p) { return black(p, capture.target); })) {
            out.faults.push_back({Fault::BlackFrame, i});
            continue;
        }
        if (i && (expected[i].scroll_x != expected[i - 1].scroll_x || expected[i].scroll_y != expected[i - 1].scroll_y) &&
            same_shape(current, actual[i - 1]) && capture.pixels == actual[i - 1].pixels) {
            out.faults.push_back({Fault::FrozenFrame, i});
            continue;
        }
        if (comparison.passed()) continue;

        std::optional<std::size_t> split;
        if (i) split = tear_row(capture, current, expected[i - 1].image);
        if (!split && i + 1 < expected.size())
            split = tear_row(capture, current, expected[i + 1].image);
        if (!split && i && i + 1 < expected.size())
            split = tear_row(capture, expected[i - 1].image, expected[i + 1].image);
        if (split) {
            out.faults.push_back({Fault::Tear, i, 0, *split});
            continue;
        }

        const auto previous_fault_count = out.faults.size();
        if (i) find_stale_tiles(out, i, capture, expected);
        if (out.faults.size() == previous_fault_count) out.faults.push_back({Fault::PixelMismatch, i});
    }
    return out;
}

} // namespace nezha_gpu_test::ca_glass
