/* SPDX-License-Identifier: MIT */
#include "cpu_reference_phase4_ext.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace nezha_gpu_test::phase4_ext {
namespace {
constexpr std::size_t max_pixels = 16 * 1024 * 1024;
std::size_t area(std::size_t w, std::size_t h) {
    if (!w || !h || w > max_pixels / h) throw std::invalid_argument("image extent");
    return w * h;
}
void valid_image(const Image &image) {
    if (image.pixels.size() != area(image.width, image.height))
        throw std::invalid_argument("image storage extent");
}
void valid_vertex(const Vertex &v) {
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || std::abs(v.x) > 65536 || std::abs(v.y) > 65536 ||
        !std::isfinite(v.r) || !std::isfinite(v.g) || !std::isfinite(v.b) || !std::isfinite(v.a) ||
        !std::isfinite(v.u) || !std::isfinite(v.v) ||
        std::min({v.r, v.g, v.b, v.a}) < 0 || std::max({v.r, v.g, v.b, v.a}) > 1)
        throw std::invalid_argument("invalid color vertex");
}
template<class V> double edge(const V &a, const V &b, double x, double y) {
    return (double(b.x) - a.x) * (y - a.y) - (double(b.y) - a.y) * (x - a.x);
}
template<class V> bool covered(double e, const V &a, const V &b) {
    return e > 0 || (e == 0 && (b.y < a.y || (b.y == a.y && b.x > a.x)));
}
template<class V> std::array<double, 3> weights(const std::array<V, 3> &t, double x, double y) {
    return {{edge(t[1], t[2], x, y), edge(t[2], t[0], x, y), edge(t[0], t[1], x, y)}};
}
template<class V> bool inside(const std::array<V, 3> &t, const std::array<double, 3> &w) {
    return covered(w[0], t[1], t[2]) && covered(w[1], t[2], t[0]) && covered(w[2], t[0], t[1]);
}
Triangle checked_triangle(Triangle t) {
    for (const auto &v : t) valid_vertex(v);
    const auto total = edge(t[0], t[1], t[2].x, t[2].y);
    if (total == 0) throw std::invalid_argument("degenerate triangle");
    if (total < 0) std::swap(t[1], t[2]);
    return t;
}
Bgra8 interpolated(const Triangle &t, const std::array<double, 3> &w) {
    const double total = w[0] + w[1] + w[2];
    const auto component = [&](float Vertex::*p) {
        return static_cast<float>((t[0].*p * w[0] + t[1].*p * w[1] + t[2].*p * w[2]) / total);
    };
    return quantize({component(&Vertex::r), component(&Vertex::g), component(&Vertex::b), component(&Vertex::a)});
}
Comparison counts(const std::vector<std::uint64_t> &expected, const std::vector<std::uint64_t> &actual) {
    Comparison out;
    out.shape_equal = expected.size() == actual.size();
    out.compared = std::min(expected.size(), actual.size());
    out.mismatched_px = std::max(expected.size(), actual.size()) - out.compared;
    for (std::size_t i = 0; i < out.compared; ++i) {
        const auto e = expected[i] > actual[i] ? expected[i] - actual[i] : actual[i] - expected[i];
        out.max_err = std::max(out.max_err, static_cast<double>(e));
        out.mismatched_px += e != 0;
    }
    return out;
}
void valid_msaa(const MsaaReference &r) {
    valid_image(r.resolved); valid_image(r.single_sample);
    const auto n = r.resolved.pixels.size();
    if (r.single_sample.width != r.resolved.width || r.single_sample.height != r.resolved.height ||
        r.samples.size() != n || r.coverage_mask.size() != n ||
        std::any_of(r.coverage_mask.begin(), r.coverage_mask.end(), [](auto m) { return m > 15; }))
        throw std::invalid_argument("MSAA storage or sample mask");
}
void valid_tess(const TessMesh &mesh) {
    if (!mesh.factor || mesh.factor > 32 ||
        mesh.vertices.size() != (mesh.factor + 1u) * (mesh.factor + 2u) / 2 ||
        mesh.indices.size() != mesh.factor * mesh.factor * 3u)
        throw std::invalid_argument("tessellation mesh extent");
    for (const auto &v : mesh.vertices) valid_vertex(v);
    for (const auto i : mesh.indices) if (i >= mesh.vertices.size()) throw std::out_of_range("tess index");
}
void nv12_valid(const Nv12Frame &f) {
    (void)area(f.width, f.height);
    if (f.width % 2 || f.height % 2 || f.y_stride < f.width || f.uv_stride < f.width ||
        f.y_stride > max_pixels / f.height || f.uv_stride > max_pixels / (f.height / 2) ||
        f.y.size() != f.y_stride * f.height || f.uv.size() != f.uv_stride * (f.height / 2) ||
        (f.range != YuvRange::Video && f.range != YuvRange::Full))
        throw std::invalid_argument("NV12 shape, pitch or range");
}
std::uint8_t code(double c) {
    return static_cast<std::uint8_t>(std::floor(std::clamp(c, 0.0, 255.0) + .5));
}
Bgra8 convert_pixel(const Nv12Frame &f, std::size_t x, std::size_t y) {
    const auto uv = y / 2 * f.uv_stride + x / 2 * 2;
    const bool video = f.range == YuvRange::Video;
    const double luma = (double(f.y[y * f.y_stride + x]) - (video ? 16 : 0)) / (video ? 219 : 255);
    const double cb = (double(f.uv[uv]) - 128) / (video ? 224 : 255);
    const double cr = (double(f.uv[uv + 1]) - 128) / (video ? 224 : 255);
    return {code((luma + 1.8556 * cb) * 255),
            code((luma - .1873242729306488 * cb - .4681242729306488 * cr) * 255),
            code((luma + 1.5748 * cr) * 255), 255};
}
void matrix_valid(const Matrix &m) {
    if (!m.rows || !m.columns || m.rows > 128 || m.columns > 128 || m.values.size() != m.rows * m.columns ||
        std::any_of(m.values.begin(), m.values.end(), [](float v) { return !std::isfinite(v); }))
        throw std::invalid_argument("matrix extent or nonfinite value");
}
void gemm_valid(const Matrix &a, const Matrix &b) {
    matrix_valid(a); matrix_valid(b);
    if (a.columns != b.rows) throw std::invalid_argument("GEMM inner dimension");
}
float rounded_half(float v) { return half_to_float(float_to_half(v)); }
std::uint32_t rounded_shift(std::uint32_t value, unsigned shift) {
    const auto base = value >> shift, remainder = value & ((1u << shift) - 1), halfway = 1u << (shift - 1);
    return base + (remainder > halfway || (remainder == halfway && (base & 1u)));
}
std::size_t tap_coord(std::size_t center, unsigned tap, std::size_t extent) {
    return tap < 2 ? center - std::min(center, std::size_t(2 - tap)) :
                    center + std::min(extent - 1 - center, std::size_t(tap - 2));
}
std::array<unsigned, 4> components(Bgra8 p) { return {{p.b, p.g, p.r, p.a}}; }
Image direct_blur(const Image &source) {
    constexpr std::array<unsigned, 5> kernel{{1, 4, 6, 4, 1}};
    auto out = make_image(source.width, source.height, clear_pixel);
    for (std::size_t y = 0; y < source.height; ++y) for (std::size_t x = 0; x < source.width; ++x) {
        std::array<unsigned, 4> sum{};
        for (unsigned j = 0; j < 5; ++j) for (unsigned i = 0; i < 5; ++i) {
            const auto p = components(source.at(tap_coord(x, i, source.width), tap_coord(y, j, source.height)));
            for (unsigned c = 0; c < 4; ++c) sum[c] += p[c] * kernel[i] * kernel[j];
        }
        out.at(x, y) = {static_cast<std::uint8_t>((sum[0] + 128) / 256),
            static_cast<std::uint8_t>((sum[1] + 128) / 256), static_cast<std::uint8_t>((sum[2] + 128) / 256),
            static_cast<std::uint8_t>((sum[3] + 128) / 256)};
    }
    return out;
}
} // namespace

Image resolve_msaa4x(std::size_t w, std::size_t h, const std::vector<std::array<Bgra8, 4>> &samples) {
    if (samples.size() != area(w, h)) throw std::invalid_argument("four-sample storage extent");
    auto out = make_image(w, h, clear_pixel);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        std::array<unsigned, 4> sum{};
        for (const auto sample : samples[i]) {
            const auto p = components(sample);
            for (unsigned c = 0; c < 4; ++c) sum[c] += p[c];
        }
        out.pixels[i] = {static_cast<std::uint8_t>((sum[0] + 2) / 4),
            static_cast<std::uint8_t>((sum[1] + 2) / 4), static_cast<std::uint8_t>((sum[2] + 2) / 4),
            static_cast<std::uint8_t>((sum[3] + 2) / 4)};
    }
    return out;
}
MsaaReference msaa4x_reference(std::size_t w, std::size_t h, Triangle triangle, Bgra8 clear) {
    const auto n = area(w, h);
    triangle = checked_triangle(triangle);
    MsaaReference out{make_image(w, h, clear), make_image(w, h, clear),
                      std::vector<std::array<Bgra8, 4>>(n, {{clear, clear, clear, clear}}),
                      std::vector<std::uint8_t>(n)};
    for (std::size_t y = 0; y < h; ++y) for (std::size_t x = 0; x < w; ++x) {
        const auto pos = y * w + x;
        const auto center = weights(triangle, x + .5, y + .5);
        if (inside(triangle, center)) out.single_sample.pixels[pos] = interpolated(triangle, center);
        for (unsigned s = 0; s < 4; ++s) {
            const auto bary = weights(triangle, x + sample_locations[s][0], y + sample_locations[s][1]);
            if (!inside(triangle, bary)) continue;
            out.coverage_mask[pos] |= static_cast<std::uint8_t>(1u << s);
            out.samples[pos][s] = interpolated(triangle, bary);
        }
    }
    out.resolved = resolve_msaa4x(w, h, out.samples);
    return out;
}
Comparison compare_msaa(const MsaaReference &a, const MsaaReference &b) {
    valid_msaa(a); valid_msaa(b);
    auto out = compare_images(a.resolved, b.resolved, 1);
    out.merge(compare_images(a.single_sample, b.single_sample, 1));
    out.merge(compare_bytes(a.coverage_mask, b.coverage_mask));
    if (a.resolved.width == b.resolved.width && a.resolved.height == b.resolved.height) {
        Image left{4, a.samples.size(), {}}, right{4, b.samples.size(), {}};
        for (const auto &p : a.samples) left.pixels.insert(left.pixels.end(), p.begin(), p.end());
        for (const auto &p : b.samples) right.pixels.insert(right.pixels.end(), p.begin(), p.end());
        out.merge(compare_images(left, right, 1));
    }
    return out;
}

std::vector<DepthTriangle> visibility_scene() {
    return {{{{{0, 0, .25}, {4, 0, .25}, {0, 4, .25}}}, {30, 60, 90, 255}},
            {{{{0, 0, .5}, {8, 0, .5}, {0, 8, .5}}}, {90, 60, 30, 255}},
            {{{{0, 0, .75}, {4, 0, .75}, {0, 4, .75}}}, {200, 0, 200, 255}}};
}
VisibilityReference visibility_reference(std::size_t w, std::size_t h,
    const std::vector<DepthTriangle> &scene, QueryMode mode, DepthCompare compare, bool depth_write) {
    const auto n = area(w, h);
    if (scene.empty() || scene.size() > 1024 ||
        (mode != QueryMode::Counting && mode != QueryMode::Boolean) ||
        (compare != DepthCompare::Less && compare != DepthCompare::LessEqual))
        throw std::invalid_argument("query scene or mode");
    VisibilityReference out{make_image(w, h, clear_pixel), std::vector<double>(n, 1),
                            std::vector<std::uint64_t>(scene.size()), std::vector<std::uint64_t>(scene.size())};
    for (std::size_t q = 0; q < scene.size(); ++q) {
        auto t = scene[q].vertices;
        for (const auto &v : t) if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
            std::abs(v.x) > 65536 || std::abs(v.y) > 65536 || v.z < 0 || v.z > 1)
            throw std::invalid_argument("query depth vertex");
        auto total = edge(t[0], t[1], t[2].x, t[2].y);
        if (total == 0) throw std::invalid_argument("degenerate depth triangle");
        if (total < 0) { std::swap(t[1], t[2]); total = -total; }
        for (std::size_t y = 0; y < h; ++y) for (std::size_t x = 0; x < w; ++x) {
            const auto bary = weights(t, x + .5, y + .5);
            if (!inside(t, bary)) continue;
            const auto i = y * w + x;
            const double z = (bary[0] * t[0].z + bary[1] * t[1].z + bary[2] * t[2].z) / total;
            if (!(compare == DepthCompare::Less ? z < out.depth[i] : z <= out.depth[i])) continue;
            ++out.passing_fragments[q];
            out.color.pixels[i] = scene[q].color;
            if (depth_write) out.depth[i] = z;
        }
        out.query_results[q] = mode == QueryMode::Boolean ? (out.passing_fragments[q] != 0) : out.passing_fragments[q];
    }
    return out;
}
Comparison compare_visibility(const VisibilityReference &a, const VisibilityReference &b) {
    valid_image(a.color); valid_image(b.color);
    if (a.depth.size() != a.color.pixels.size() || b.depth.size() != b.color.pixels.size() ||
        a.query_results.size() != a.passing_fragments.size() || b.query_results.size() != b.passing_fragments.size() ||
        a.query_results.empty() || b.query_results.empty()) throw std::invalid_argument("query result extent");
    auto out = compare_images(a.color, b.color, 0);
    out.merge(counts(a.passing_fragments, b.passing_fragments)); out.merge(counts(a.query_results, b.query_results));
    Comparison depth;
    depth.shape_equal = a.depth.size() == b.depth.size();
    depth.compared = std::min(a.depth.size(), b.depth.size());
    depth.mismatched_px = std::max(a.depth.size(), b.depth.size()) - depth.compared;
    for (std::size_t i = 0; i < depth.compared; ++i) {
        const bool finite = std::isfinite(a.depth[i]) && std::isfinite(b.depth[i]);
        const double error = finite ? std::abs(a.depth[i] - b.depth[i]) : 1e30;
        depth.max_err = std::max(depth.max_err, error); depth.mismatched_px += error != 0;
    }
    out.merge(depth);
    return out;
}

TessMesh tessellate_triangle(Triangle control, unsigned factor) {
    (void)checked_triangle(control);
    if (!factor || factor > 32) throw std::invalid_argument("tessellation factor outside 1..32");
    TessMesh out{factor, {}, {}};
    for (unsigned i = 0; i <= factor; ++i) for (unsigned j = 0; j <= factor - i; ++j) {
        const double b = double(i) / factor, c = double(j) / factor, a = 1 - b - c;
        const auto p = [&](float Vertex::*component) {
            return static_cast<float>(a * control[0].*component + b * control[1].*component + c * control[2].*component);
        };
        out.vertices.push_back({p(&Vertex::x), p(&Vertex::y), p(&Vertex::r), p(&Vertex::g),
            p(&Vertex::b), p(&Vertex::a), p(&Vertex::u), p(&Vertex::v)});
    }
    const auto index = [factor](unsigned i, unsigned j) {
        return i * (factor + 1) - i * (i ? i - 1 : 0) / 2 + j;
    };
    for (unsigned i = 0; i < factor; ++i) for (unsigned j = 0; j < factor - i; ++j) {
        out.indices.insert(out.indices.end(), {index(i, j), index(i + 1, j), index(i, j + 1)});
        if (i + j + 1 < factor)
            out.indices.insert(out.indices.end(), {index(i + 1, j), index(i + 1, j + 1), index(i, j + 1)});
    }
    valid_tess(out);
    return out;
}
TessReference amplify_two_views(const TessMesh &mesh, std::size_t vw, std::size_t h,
                                std::array<std::int32_t, 2> offsets) {
    valid_tess(mesh);
    if (!vw || vw > 32768 || !h || h > 65536 || offsets[0] < 0 || offsets[1] < 0 ||
        std::size_t(offsets[0]) > vw || std::size_t(offsets[1]) > vw ||
        std::abs(std::int64_t(offsets[0]) - offsets[1]) < static_cast<std::int64_t>(vw))
        throw std::invalid_argument("two disjoint viewports required");
    const auto n = area(2 * vw, h);
    TessReference out{make_image(2 * vw, h, clear_pixel), std::vector<std::uint32_t>(n), offsets};
    for (const auto offset : offsets) for (std::size_t first = 0; first < mesh.indices.size(); first += 3) {
        Triangle t{{mesh.vertices[mesh.indices[first]], mesh.vertices[mesh.indices[first + 1]],
                    mesh.vertices[mesh.indices[first + 2]]}};
        /* Rasterization uses a fixed subpixel grid. Snap shared vertices to
         * 8 fractional bits before evaluating the top-left rule, so a
         * non-dyadic barycentric coordinate cannot open cracks or move an
         * otherwise exact diagonal across a pixel center. Attributes retain
         * their interpolated values; only raster coordinates are snapped. */
        for (auto &v : t) {
            v.x = static_cast<float>(std::round((double(v.x) + offset) * 256) / 256);
            v.y = static_cast<float>(std::round(double(v.y) * 256) / 256);
        }
        t = checked_triangle(t);
        const auto bound = [](double v, std::size_t lo, std::size_t hi) {
            return static_cast<std::size_t>(std::clamp(v, double(lo), double(hi)));
        };
        const auto x0 = bound(std::floor(std::min({t[0].x, t[1].x, t[2].x})), offset, offset + vw);
        const auto x1 = bound(std::ceil(std::max({t[0].x, t[1].x, t[2].x})), offset, offset + vw);
        const auto y0 = bound(std::floor(std::min({t[0].y, t[1].y, t[2].y})), 0, h);
        const auto y1 = bound(std::ceil(std::max({t[0].y, t[1].y, t[2].y})), 0, h);
        for (auto y = y0; y < y1; ++y) for (auto x = x0; x < x1; ++x) {
            const auto bary = weights(t, x + .5, y + .5);
            if (!inside(t, bary)) continue;
            out.image.at(x, y) = interpolated(t, bary); ++out.coverage[y * 2 * vw + x];
        }
    }
    return out;
}
Comparison compare_tess(const TessReference &a, const TessReference &b) {
    valid_image(a.image); valid_image(b.image);
    if (a.coverage.size() != a.image.pixels.size() || b.coverage.size() != b.image.pixels.size())
        throw std::invalid_argument("tessellation coverage extent");
    auto out = compare_images(a.image, b.image, 1); out.merge(compare_words(a.coverage, b.coverage));
    out.merge(compare_words({static_cast<std::uint32_t>(a.viewport_offsets[0]), static_cast<std::uint32_t>(a.viewport_offsets[1])},
                            {static_cast<std::uint32_t>(b.viewport_offsets[0]), static_cast<std::uint32_t>(b.viewport_offsets[1])}));
    return out;
}

Nv12Frame nv12_fixture(std::size_t w, std::size_t h, std::size_t padding) {
    (void)area(w, h);
    if (w % 2 || h % 2 || padding > 4096 || w + padding > max_pixels / h)
        throw std::invalid_argument("NV12 fixture extent");
    Nv12Frame out{w, h, w + padding, w + padding, {}, {}, YuvRange::Video};
    out.y.assign(out.y_stride * h, 0xa7); out.uv.assign(out.uv_stride * (h / 2), 0xa7);
    for (std::size_t y = 0; y < h; ++y) for (std::size_t x = 0; x < w; ++x)
        out.y[y * out.y_stride + x] = static_cast<std::uint8_t>(16 + (x * 13 + y * 7) % 220);
    for (std::size_t y = 0; y < h / 2; ++y) for (std::size_t x = 0; x < w; x += 2) {
        out.uv[y * out.uv_stride + x] = static_cast<std::uint8_t>(80 + (x * 3 + y * 5) % 97);
        out.uv[y * out.uv_stride + x + 1] = static_cast<std::uint8_t>(80 + (x * 7 + y * 3) % 97);
    }
    nv12_valid(out);
    return out;
}
Bgra8 bt709_nv12_pixel(const Nv12Frame &f, std::size_t x, std::size_t y) {
    nv12_valid(f);
    if (x >= f.width || y >= f.height) throw std::out_of_range("NV12 pixel coordinate");
    return convert_pixel(f, x, y);
}
CameraReference camera155_reference(const Nv12Frame &f, std::size_t pw, std::size_t ph, Rect crop) {
    nv12_valid(f); (void)area(pw, ph); (void)area(crop.width, crop.height);
    if (crop.x > f.width || crop.width > f.width - crop.x || crop.y > f.height || crop.height > f.height - crop.y)
        throw std::out_of_range("photo crop outside sensor");
    CameraReference out{make_image(pw, ph, clear_pixel), make_image(crop.width, crop.height, clear_pixel), crop};
    for (std::size_t y = 0; y < ph; ++y) for (std::size_t x = 0; x < pw; ++x) {
        const auto sx = (2 * x + 1) * f.width / (2 * pw), sy = (2 * y + 1) * f.height / (2 * ph);
        out.preview.at(x, y) = convert_pixel(f, sx, sy);
    }
    for (std::size_t y = 0; y < crop.height; ++y) for (std::size_t x = 0; x < crop.width; ++x)
        out.photo.at(x, y) = convert_pixel(f, crop.x + x, crop.y + y);
    return out;
}
Comparison compare_camera(const CameraReference &a, const CameraReference &b) {
    auto out = compare_images(a.preview, b.preview, 1); out.merge(compare_images(a.photo, b.photo, 1));
    const auto crop = [](Rect r) { return std::vector<std::uint64_t>{r.x, r.y, r.width, r.height}; };
    out.merge(counts(crop(a.photo_crop), crop(b.photo_crop)));
    return out;
}

std::uint16_t float_to_half(float value) {
    if (!std::isfinite(value)) throw std::invalid_argument("nonfinite FP16 input");
    std::uint32_t bits; std::memcpy(&bits, &value, sizeof(bits));
    const auto sign = (bits >> 16) & 0x8000u, fraction = bits & 0x7fffffu;
    const int exponent = int((bits >> 23) & 255u) - 127;
    if (exponent > 15) throw std::overflow_error("FP16 overflow");
    if (exponent < -25) return static_cast<std::uint16_t>(sign);
    if (exponent < -14)
        return static_cast<std::uint16_t>(sign | rounded_shift(0x800000u | fraction, unsigned(-exponent - 1)));
    auto half_exp = unsigned(exponent + 15), mantissa = rounded_shift(fraction, 13);
    if (mantissa == 1024) { mantissa = 0; ++half_exp; }
    if (half_exp >= 31) throw std::overflow_error("FP16 rounding overflow");
    return static_cast<std::uint16_t>(sign | (half_exp << 10) | mantissa);
}
float half_to_float(std::uint16_t bits) {
    const auto exponent = (bits >> 10) & 31u, fraction = bits & 1023u;
    if (exponent == 31) throw std::invalid_argument("nonfinite FP16 code");
    const float magnitude = exponent ? std::ldexp(float(1024u + fraction), int(exponent) - 25) :
                                      std::ldexp(float(fraction), -24);
    return bits & 0x8000u ? -magnitude : magnitude;
}
Matrix gemm_reference(const Matrix &a, const Matrix &b, Accumulation mode) {
    gemm_valid(a, b);
    if (mode != Accumulation::Float32 && mode != Accumulation::Float16) throw std::invalid_argument("GEMM mode");
    Matrix out{a.rows, b.columns, std::vector<float>(a.rows * b.columns)};
    for (std::size_t i = 0; i < a.rows; ++i) for (std::size_t j = 0; j < b.columns; ++j) {
        float sum = 0;
        for (std::size_t k = 0; k < a.columns; ++k) {
            const float x = a.values[i * a.columns + k], y = b.values[k * b.columns + j];
            if (mode == Accumulation::Float16) sum = rounded_half(sum + rounded_half(rounded_half(x) * rounded_half(y)));
            else { volatile float product = x * y; sum = sum + product; }
            if (!std::isfinite(sum)) throw std::overflow_error("GEMM accumulation overflow");
        }
        out.values[i * b.columns + j] = sum;
    }
    return out;
}
std::vector<double> gemm_precise(const Matrix &a, const Matrix &b, bool half_inputs) {
    gemm_valid(a, b);
    std::vector<double> out(a.rows * b.columns);
    for (std::size_t k = 0; k < a.columns; ++k) for (std::size_t j = 0; j < b.columns; ++j)
        for (std::size_t i = 0; i < a.rows; ++i) {
            const float x = a.values[i * a.columns + k], y = b.values[k * b.columns + j];
            out[i * b.columns + j] += double(half_inputs ? rounded_half(x) : x) * (half_inputs ? rounded_half(y) : y);
        }
    return out;
}
NumericComparison compare_gemm(const std::vector<double> &expected, const Matrix &actual,
    std::size_t rows, std::size_t columns, double relative_tolerance, double absolute_tolerance) {
    if (!rows || !columns || rows > 128 || columns > 128 || expected.size() != rows * columns ||
        !std::isfinite(relative_tolerance) || !std::isfinite(absolute_tolerance) ||
        relative_tolerance < 0 || absolute_tolerance < 0 ||
        std::any_of(expected.begin(), expected.end(), [](double v) { return !std::isfinite(v); }))
        throw std::invalid_argument("GEMM comparison reference or tolerance");
    NumericComparison out;
    if (actual.rows != rows || actual.columns != columns || actual.values.size() != expected.size()) {
        out.values.shape_equal = false; out.values.mismatched_px = 1; return out;
    }
    out.values.compared = expected.size();
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const bool finite = std::isfinite(actual.values[i]);
        const double e = finite ? std::abs(expected[i] - actual.values[i]) : 1e30;
        const double scale = std::max(std::abs(expected[i]), absolute_tolerance);
        const double relative = scale ? e / scale : (e ? 1e30 : 0);
        out.values.max_err = std::max(out.values.max_err, e);
        out.max_relative_err = std::max(out.max_relative_err, relative);
        out.values.mismatched_px += !finite || e > absolute_tolerance + relative_tolerance * std::abs(expected[i]);
    }
    return out;
}
Image gaussian5_reference(const Image &source) {
    valid_image(source);
    constexpr std::array<unsigned, 5> kernel{{1, 4, 6, 4, 1}};
    std::vector<std::array<double, 4>> intermediate(source.pixels.size());
    for (std::size_t y = 0; y < source.height; ++y) for (std::size_t x = 0; x < source.width; ++x)
        for (unsigned i = 0; i < 5; ++i) {
            const auto p = components(source.at(tap_coord(x, i, source.width), y));
            for (unsigned c = 0; c < 4; ++c) intermediate[y * source.width + x][c] += double(p[c] * kernel[i]) / 16;
        }
    auto out = make_image(source.width, source.height, clear_pixel);
    for (std::size_t y = 0; y < source.height; ++y) for (std::size_t x = 0; x < source.width; ++x) {
        std::array<double, 4> sum{};
        for (unsigned j = 0; j < 5; ++j) for (unsigned c = 0; c < 4; ++c)
            sum[c] += intermediate[tap_coord(y, j, source.height) * source.width + x][c] * kernel[j] / 16;
        out.at(x, y) = {code(sum[0]), code(sum[1]), code(sum[2]), code(sum[3])};
    }
    return out;
}
Comparison sample_comparison(std::size_t index) {
    if (index == 0) {
        const Triangle t{{{0, 0, 1, 1, 1, 1, 0, 0}, {2, 0, 1, 1, 1, 1, 0, 0}, {0, 2, 1, 1, 1, 1, 0, 0}}};
        const auto r = msaa4x_reference(2, 2, t);
        Image golden{2, 2, {{255, 255, 255, 255}, {128, 128, 128, 255}, {128, 128, 128, 255}, clear_pixel}};
        auto out = compare_images(golden, r.resolved, 1);
        out.merge(compare_bytes({15, 5, 5, 0}, r.coverage_mask)); return out;
    }
    if (index == 1) {
        const auto r = visibility_reference(8, 8, visibility_scene());
        auto out = counts({6, 22, 0}, r.passing_fragments); out.merge(counts({6, 22, 0}, r.query_results));
        out.merge(counts({1, 1, 0}, visibility_reference(8, 8, visibility_scene(), QueryMode::Boolean).query_results));
        return out;
    }
    if (index == 2) {
        const auto control = triangle_vertices(32, 32);
        const auto r = amplify_two_views(tessellate_triangle(control, 4), 32, 32, {{0, 32}});
        const auto single = triangle_reference(32, 32, control, clear_pixel).image;
        auto golden = make_image(64, 32, clear_pixel);
        for (std::size_t y = 0; y < 32; ++y) for (std::size_t x = 0; x < 32; ++x)
            golden.at(x, y) = golden.at(x + 32, y) = single.at(x, y);
        return compare_images(golden, r.image, 1);
    }
    if (index == 3) {
        auto f = nv12_fixture(8, 6, 3); std::fill(f.uv.begin(), f.uv.end(), 128);
        const auto r = camera155_reference(f, 4, 3, {1, 1, 7, 5});
        auto preview = make_image(4, 3, clear_pixel), photo = make_image(7, 5, clear_pixel);
        const auto gray = [&](std::size_t x, std::size_t y) {
            const auto c = static_cast<std::uint8_t>((unsigned(f.y[y * f.y_stride + x] - 16) * 255 + 109) / 219);
            return Bgra8{c, c, c, 255};
        };
        for (std::size_t y = 0; y < 3; ++y) for (std::size_t x = 0; x < 4; ++x) preview.at(x, y) = gray(2 * x + 1, 2 * y + 1);
        for (std::size_t y = 0; y < 5; ++y) for (std::size_t x = 0; x < 7; ++x) photo.at(x, y) = gray(x + 1, y + 1);
        auto out = compare_images(preview, r.preview, 1); out.merge(compare_images(photo, r.photo, 1)); return out;
    }
    if (index == 4) {
        const Matrix a{2, 3, {1, 2, 3, 4, 5, 6}}, b{3, 2, {7, 8, 9, 10, 11, 12}};
        auto out = compare_gemm({58, 64, 139, 154}, gemm_reference(a, b, Accumulation::Float32), 2, 2, 1e-5).values;
        out.merge(compare_gemm({58, 64, 139, 154}, gemm_reference(a, b, Accumulation::Float16), 2, 2, .02).values);
        const auto source = gradient_image(13, 9);
        out.merge(compare_images(direct_blur(source), gaussian5_reference(source), 1)); return out;
    }
    throw std::out_of_range("Phase 4 extended case index");
}
} // namespace nezha_gpu_test::phase4_ext
