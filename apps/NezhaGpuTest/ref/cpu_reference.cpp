/* SPDX-License-Identifier: MIT */
#include "cpu_reference.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace nezha_gpu_test {
namespace {
constexpr std::size_t maximum_pixels = 16 * 1024 * 1024;
constexpr std::size_t maximum_elements = 64 * 1024 * 1024;

std::size_t area(std::size_t width, std::size_t height) {
    if (!width || !height || width > maximum_pixels / height)
        throw std::invalid_argument("invalid or excessive image dimensions");
    return width * height;
}
void image_valid(const Image &image) {
    if (image.pixels.size() != area(image.width, image.height))
        throw std::invalid_argument("image storage does not match dimensions");
}
void range(std::size_t size, std::size_t offset, std::size_t count) {
    if (offset > size || count > size - offset)
        throw std::out_of_range("buffer or rectangle extent");
}
Color normalized(Bgra8 pixel) {
    constexpr float scale = 1.0f / 255.0f;
    return {pixel.r * scale, pixel.g * scale, pixel.b * scale, pixel.a * scale};
}
std::uint8_t channel(float value) {
    if (!std::isfinite(value)) throw std::invalid_argument("nonfinite color");
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}
double edge(const Vertex &a, const Vertex &b, double x, double y) {
    return (static_cast<double>(b.x) - a.x) * (y - a.y) -
           (static_cast<double>(b.y) - a.y) * (x - a.x);
}
bool top_left(const Vertex &a, const Vertex &b) {
    return b.y < a.y || (b.y == a.y && b.x > a.x);
}
bool covered(double value, const Vertex &a, const Vertex &b) {
    return value > 0 || (value == 0 && top_left(a, b));
}
double pixel_error(Bgra8 a, Bgra8 b) {
    return std::max({std::abs(int(a.b) - int(b.b)), std::abs(int(a.g) - int(b.g)),
                     std::abs(int(a.r) - int(b.r)), std::abs(int(a.a) - int(b.a))});
}
template<typename T>
Comparison integral_compare(const std::vector<T> &expected, const std::vector<T> &actual) {
    Comparison result;
    result.shape_equal = expected.size() == actual.size();
    result.mismatched_px = expected.size() > actual.size() ? expected.size() - actual.size() : actual.size() - expected.size();
    result.compared = std::min(expected.size(), actual.size());
    for (std::size_t i = 0; i < result.compared; ++i) {
        const double error = std::abs(double(expected[i]) - double(actual[i]));
        result.max_err = std::max(result.max_err, error);
        result.mismatched_px += expected[i] != actual[i];
    }
    return result;
}
bool token(const std::string &text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '.' || c == ':' || c == '-';
    });
}
} // namespace

bool Bgra8::operator==(const Bgra8 &other) const noexcept {
    return b == other.b && g == other.g && r == other.r && a == other.a;
}
Bgra8 &Image::at(std::size_t x, std::size_t y) {
    if (x >= width || y >= height) throw std::out_of_range("pixel coordinate");
    return pixels.at(y * width + x);
}
const Bgra8 &Image::at(std::size_t x, std::size_t y) const {
    if (x >= width || y >= height) throw std::out_of_range("pixel coordinate");
    return pixels.at(y * width + x);
}
bool Comparison::passed() const noexcept { return shape_equal && mismatched_px == 0; }
void Comparison::merge(const Comparison &other) {
    max_err = std::max(max_err, other.max_err);
    mismatched_px += other.mismatched_px;
    compared += other.compared;
    shape_equal = shape_equal && other.shape_equal;
}

std::vector<std::uint8_t> byte_pattern(std::size_t count, std::uint32_t seed) {
    if (count > maximum_elements) throw std::length_error("byte pattern too large");
    std::vector<std::uint8_t> data(count);
    for (std::size_t i = 0; i < count; ++i) {
        seed = seed * 1664525u + 1013904223u;
        data[i] = static_cast<std::uint8_t>((seed >> 24) ^ (i & 255u));
    }
    return data;
}
std::vector<std::uint8_t> buffer_copy_reference(const std::vector<std::uint8_t> &source,
    const std::vector<std::uint8_t> &initial, std::size_t source_offset,
    std::size_t destination_offset, std::size_t bytes) {
    range(source.size(), source_offset, bytes);
    range(initial.size(), destination_offset, bytes);
    auto result = initial;
    if (bytes) std::memcpy(result.data() + destination_offset, source.data() + source_offset, bytes);
    return result;
}
Image make_image(std::size_t width, std::size_t height, Bgra8 fill) {
    return {width, height, std::vector<Bgra8>(area(width, height), fill)};
}
Image gradient_image(std::size_t width, std::size_t height) {
    auto image = make_image(width, height, {0, 0, 0, 255});
    for (std::size_t y = 0; y < height; ++y)
        for (std::size_t x = 0; x < width; ++x)
            image.at(x, y) = {static_cast<std::uint8_t>((x * 5 + y * 7) & 255),
                              static_cast<std::uint8_t>((x * 3 + y * 11) & 255),
                              static_cast<std::uint8_t>((x * 13 + y * 2) & 255), 255};
    return image;
}
Image checker_image(std::size_t width, std::size_t height, std::size_t block) {
    if (!block) throw std::invalid_argument("zero checker block");
    auto image = make_image(width, height, {0, 0, 0, 128});
    for (std::size_t y = 0; y < height; ++y)
        for (std::size_t x = 0; x < width; ++x)
            image.at(x, y) = ((x / block + y / block) & 1) ? Bgra8{224, 160, 32, 128} : Bgra8{8, 48, 96, 128};
    return image;
}
Image clear_reference(const Image &initial, Rect rectangle, Bgra8 color) {
    image_valid(initial);
    range(initial.width, rectangle.x, rectangle.width);
    range(initial.height, rectangle.y, rectangle.height);
    auto result = initial;
    for (std::size_t y = rectangle.y; y < rectangle.y + rectangle.height; ++y)
        std::fill_n(result.pixels.begin() + static_cast<std::ptrdiff_t>(y * initial.width + rectangle.x), rectangle.width, color);
    return result;
}
Bgra8 quantize(Color color) { return {channel(color.b), channel(color.g), channel(color.r), channel(color.a)}; }
Color sample_reference(const Image &source, double u, double v, Filter filter) {
    image_valid(source);
    if (!std::isfinite(u) || !std::isfinite(v)) throw std::invalid_argument("invalid sample coordinate");
    const double x = std::clamp(u, 0.0, 1.0) * double(source.width) - 0.5;
    const double y = std::clamp(v, 0.0, 1.0) * double(source.height) - 0.5;
    auto fetch = [&](long long sx, long long sy) {
        return normalized(source.at(static_cast<std::size_t>(std::clamp(sx, 0LL, static_cast<long long>(source.width) - 1)),
                                    static_cast<std::size_t>(std::clamp(sy, 0LL, static_cast<long long>(source.height) - 1))));
    };
    if (filter == Filter::Nearest) return fetch(static_cast<long long>(std::floor(x + 0.5)), static_cast<long long>(std::floor(y + 0.5)));
    const auto x0 = static_cast<long long>(std::floor(x)), y0 = static_cast<long long>(std::floor(y));
    const auto p00 = fetch(x0, y0), p10 = fetch(x0 + 1, y0), p01 = fetch(x0, y0 + 1), p11 = fetch(x0 + 1, y0 + 1);
    const double fx = x - std::floor(x), fy = y - std::floor(y);
    auto lerp = [&](float a, float b, float c, float d) {
        return static_cast<float>(((1 - fx) * a + fx * b) * (1 - fy) + ((1 - fx) * c + fx * d) * fy);
    };
    return {lerp(p00.r, p10.r, p01.r, p11.r), lerp(p00.g, p10.g, p01.g, p11.g),
            lerp(p00.b, p10.b, p01.b, p11.b), lerp(p00.a, p10.a, p01.a, p11.a)};
}
Image scale_reference(const Image &source, std::size_t width, std::size_t height, Filter filter) {
    image_valid(source);
    auto result = make_image(width, height, {0, 0, 0, 0});
    for (std::size_t y = 0; y < height; ++y)
        for (std::size_t x = 0; x < width; ++x)
            result.at(x, y) = quantize(sample_reference(source, (double(x) + 0.5) / double(width),
                                                       (double(y) + 0.5) / double(height), filter));
    return result;
}
Bgra8 alpha_blend_reference(Color source, Bgra8 destination) {
    const auto dest = normalized(destination);
    const float a = std::clamp(source.a, 0.0f, 1.0f);
    return quantize({source.r * a + dest.r * (1 - a), source.g * a + dest.g * (1 - a),
                     source.b * a + dest.b * (1 - a), a + dest.a * (1 - a)});
}
std::vector<float> vector_input(std::size_t count, bool second) {
    if (count > maximum_elements) throw std::length_error("vector input too large");
    std::vector<float> data(count);
    for (std::size_t i = 0; i < count; ++i)
        data[i] = static_cast<float>(static_cast<int>(i % (second ? 127 : 251)) - (second ? 63 : 125)) * (second ? 0.0625f : 0.125f);
    return data;
}
std::vector<float> add_vec_reference(const std::vector<float> &a, const std::vector<float> &b, float multiplier) {
    if (a.size() != b.size() || !std::isfinite(multiplier)) throw std::invalid_argument("invalid add_vec inputs");
    std::vector<float> result(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) result[i] = a[i] + b[i] * multiplier;
    return result;
}
std::uint32_t grid_value(std::size_t x, std::size_t y) {
    if (x > 65535 || y > 65535) throw std::invalid_argument("grid coordinate too large");
    return static_cast<std::uint32_t>((x + 1) * 17 + (y + 1) * 257);
}
std::vector<std::uint32_t> nonuniform_grid_reference(std::size_t width, std::size_t height, std::size_t guard_words) {
    const auto count = area(width, height);
    if (width > 65536 || height > 65536 || guard_words > (maximum_elements - count) / 2)
        throw std::invalid_argument("grid extent or guard overflow");
    std::vector<std::uint32_t> result(count + 2 * guard_words, grid_guard);
    for (std::size_t y = 0; y < height; ++y)
        for (std::size_t x = 0; x < width; ++x) result[guard_words + y * width + x] = grid_value(x, y);
    return result;
}
std::array<Vertex, 3> triangle_vertices(std::size_t width, std::size_t height) {
    (void)area(width, height);
    const auto w = static_cast<float>(width), h = static_cast<float>(height);
    return {{{w * .25f, h * .25f, 1, 0, 0, 1, 0, 0},
             {w * .75f, h * .25f, 0, 1, 0, 1, 0, 0},
             {w * .25f, h * .75f, 0, 0, 1, 1, 0, 0}}};
}
RasterReference triangle_reference(std::size_t width, std::size_t height,
    const std::array<Vertex, 3> &vertices, Bgra8 clear) {
    RasterReference result{make_image(width, height, clear), std::vector<std::uint8_t>(area(width, height), 0)};
    auto triangle = vertices;
    for (const auto &v : triangle)
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.r) || !std::isfinite(v.g) ||
            !std::isfinite(v.b) || !std::isfinite(v.a)) throw std::invalid_argument("invalid triangle vertex");
    double total = edge(triangle[0], triangle[1], triangle[2].x, triangle[2].y);
    if (!std::isfinite(total) || total == 0) throw std::invalid_argument("degenerate triangle");
    if (total < 0) { std::swap(triangle[1], triangle[2]); total = -total; }
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const double px = double(x) + .5, py = double(y) + .5;
            const double e0 = edge(triangle[1], triangle[2], px, py), e1 = edge(triangle[2], triangle[0], px, py),
                         e2 = edge(triangle[0], triangle[1], px, py);
            if (!covered(e0, triangle[1], triangle[2]) || !covered(e1, triangle[2], triangle[0]) ||
                !covered(e2, triangle[0], triangle[1])) continue;
            auto interpolate = [&](float a, float b, float c) { return static_cast<float>((a * e0 + b * e1 + c * e2) / total); };
            result.image.at(x, y) = quantize({interpolate(triangle[0].r, triangle[1].r, triangle[2].r),
                                             interpolate(triangle[0].g, triangle[1].g, triangle[2].g),
                                             interpolate(triangle[0].b, triangle[1].b, triangle[2].b),
                                             interpolate(triangle[0].a, triangle[1].a, triangle[2].a)});
            auto distance = [&](const Vertex &a, const Vertex &b, double value) {
                return value / std::hypot(double(b.x) - a.x, double(b.y) - a.y);
            };
            if (std::min({distance(triangle[1], triangle[2], e0), distance(triangle[2], triangle[0], e1),
                          distance(triangle[0], triangle[1], e2)}) < 1.0) result.tolerance[y * width + x] = 1;
        }
    }
    return result;
}
std::array<Vertex, 4> quad_vertices(std::size_t width, std::size_t height) {
    (void)area(width, height);
    const auto w = static_cast<float>(width), h = static_cast<float>(height);
    return {{{0, 0, 1, 1, 1, 1, 0, 0}, {w, 0, 1, 1, 1, 1, 1, 0},
             {w, h, 1, 1, 1, 1, 1, 1}, {0, h, 1, 1, 1, 1, 0, 1}}};
}
Image indexed_quad_reference(std::size_t width, std::size_t height, const Image &texture,
    const std::vector<std::uint32_t> &indices, Bgra8 clear) {
    image_valid(texture);
    if (indices.size() != 6) throw std::invalid_argument("quad needs two indexed triangles");
    for (auto index : indices) if (index >= 4) throw std::out_of_range("quad index");
    const auto vertices = quad_vertices(width, height);
    auto result = make_image(width, height, clear);
    for (std::size_t base = 0; base < indices.size(); base += 3) {
        auto a = vertices[indices[base]], b = vertices[indices[base + 1]], c = vertices[indices[base + 2]];
        double total = edge(a, b, c.x, c.y);
        if (total == 0) throw std::invalid_argument("degenerate indexed quad triangle");
        if (total < 0) { std::swap(b, c); total = -total; }
        for (std::size_t y = 0; y < height; ++y) for (std::size_t x = 0; x < width; ++x) {
            const double e0 = edge(b, c, double(x) + .5, double(y) + .5),
                         e1 = edge(c, a, double(x) + .5, double(y) + .5), e2 = edge(a, b, double(x) + .5, double(y) + .5);
            if (!covered(e0, b, c) || !covered(e1, c, a) || !covered(e2, a, b)) continue;
            const double u = (a.u * e0 + b.u * e1 + c.u * e2) / total, v = (a.v * e0 + b.v * e1 + c.v * e2) / total;
            result.at(x, y) = alpha_blend_reference(sample_reference(texture, u, v, Filter::Nearest), result.at(x, y));
        }
    }
    return result;
}
Comparison compare_bytes(const std::vector<std::uint8_t> &a, const std::vector<std::uint8_t> &b) { return integral_compare(a, b); }
Comparison compare_words(const std::vector<std::uint32_t> &a, const std::vector<std::uint32_t> &b) { return integral_compare(a, b); }
Comparison compare_floats(const std::vector<float> &expected, const std::vector<float> &actual, double tolerance) {
    if (!std::isfinite(tolerance) || tolerance < 0) throw std::invalid_argument("invalid FP32 tolerance");
    Comparison result;
    result.shape_equal = expected.size() == actual.size();
    result.mismatched_px = expected.size() > actual.size() ? expected.size() - actual.size() : actual.size() - expected.size();
    result.compared = std::min(expected.size(), actual.size());
    for (std::size_t i = 0; i < result.compared; ++i) {
        const bool finite = std::isfinite(expected[i]) && std::isfinite(actual[i]);
        const double error = finite ? std::abs(double(expected[i]) - actual[i]) : 1e30;
        result.max_err = std::max(result.max_err, error);
        result.mismatched_px += !finite || error > tolerance;
    }
    return result;
}
Comparison compare_images(const Image &expected, const Image &actual, std::uint8_t tolerance) {
    image_valid(expected); image_valid(actual);
    Comparison result;
    result.shape_equal = expected.width == actual.width && expected.height == actual.height;
    if (!result.shape_equal) { result.mismatched_px = 1; return result; }
    result.compared = expected.pixels.size();
    for (std::size_t i = 0; i < result.compared; ++i) {
        const double error = pixel_error(expected.pixels[i], actual.pixels[i]);
        result.max_err = std::max(result.max_err, error);
        result.mismatched_px += error > tolerance;
    }
    return result;
}
Comparison compare_raster(const RasterReference &expected, const Image &actual) {
    if (expected.tolerance.size() != expected.image.pixels.size()) throw std::invalid_argument("raster tolerance shape");
    auto result = compare_images(expected.image, actual, 255);
    if (!result.shape_equal) return result;
    for (std::size_t i = 0; i < result.compared; ++i)
        result.mismatched_px += pixel_error(expected.image.pixels[i], actual.pixels[i]) > expected.tolerance[i];
    return result;
}
std::vector<std::uint32_t> fence_chain_reference(std::size_t commands) {
    if (!commands || commands > 65535) throw std::invalid_argument("invalid fence chain length");
    std::vector<std::uint32_t> result(commands + 1);
    for (std::size_t i = 0; i <= commands; ++i) result[i] = 0xa0000000u + static_cast<std::uint32_t>(i);
    return result;
}
Comparison compare_fence_chain(const std::vector<std::uint32_t> &state,
    const std::vector<std::uint32_t> &reads, const std::vector<std::uint32_t> &order, std::size_t commands) {
    const auto expected = fence_chain_reference(commands);
    auto result = compare_words(expected, state);
    result.merge(compare_words(std::vector<std::uint32_t>(expected.begin(), expected.end() - 1), reads));
    std::vector<std::uint32_t> expected_order(commands);
    for (std::size_t i = 0; i < commands; ++i) expected_order[i] = static_cast<std::uint32_t>(i);
    result.merge(compare_words(expected_order, order));
    return result;
}
std::string hwtest_line(const std::string &case_id, const Comparison &result,
    std::uint64_t elapsed_us, const std::string &path, const std::string &note) {
    if (!token(case_id) || !token(path) || !token(note) ||
        (path != "raw" && path.rfind("fallback:raw:", 0) != 0) || !std::isfinite(result.max_err) || result.max_err < 0)
        throw std::invalid_argument("invalid HWTEST record");
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "HWTEST case=" << case_id << " status=" << (result.passed() ? "PASS" : "FAIL") << " path=" << path
           << " max_err=" << std::setprecision(9) << result.max_err << " mismatched_px=" << result.mismatched_px
           << " time_us=" << elapsed_us << " note=" << note;
    return stream.str();
}
} // namespace nezha_gpu_test
