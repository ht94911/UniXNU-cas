/* SPDX-License-Identifier: MIT */
#include "cpu_reference_phase3.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace nezha_gpu_test::phase3 {
namespace {
constexpr std::size_t maximum_pixels = 16 * 1024 * 1024;
std::size_t area(std::size_t w, std::size_t h) {
    if (!w || !h || w > maximum_pixels / h) throw std::invalid_argument("Phase 3 image extent");
    return w * h;
}
void valid(const Image &image) {
    if (image.pixels.size() != area(image.width, image.height)) throw std::invalid_argument("Phase 3 image storage");
}
void tile_valid(std::size_t tw, std::size_t th) {
    if (!tw || !th || tw % tile_width || th % tile_height || tw > maximum_pixels / th)
        throw std::invalid_argument("Tile must be a bounded 96x32 multiple");
}
Triangle triangle(float ax, float ay, float bx, float by, float cx, float cy, float z, Bgra8 c) {
    return Triangle{{{Vertex{ax, ay, z, 0, c.r, c.g, c.b, c.a}, Vertex{bx, by, z, 0, c.r, c.g, c.b, c.a},
                      Vertex{cx, cy, z, 0, c.r, c.g, c.b, c.a}}}};
}
double edge(const Vertex &a, const Vertex &b, double x, double y) {
    return (double(b.x) - a.x) * (y - a.y) - (double(b.y) - a.y) * (x - a.x);
}
bool covered(double e, const Vertex &a, const Vertex &b) {
    return e > 0 || (e == 0 && (b.y < a.y || (b.y == a.y && b.x > a.x)));
}
std::vector<Triangle> checked_scene(const std::vector<Triangle> &scene) {
    if (scene.empty() || scene.size() > 4096) throw std::invalid_argument("invalid Phase 3 scene size");
    auto checked = scene;
    for (auto &t : checked) {
        for (const auto &v : t.vertices) {
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || v.z < 0 || v.z > 1 ||
                std::abs(v.x) > 65536 || std::abs(v.y) > 65536 || v.r > 255 || v.g > 255 || v.b > 255 || v.a > 255)
                throw std::invalid_argument("invalid Phase 3 vertex");
            const auto &first = t.vertices[0];
            if (v.r != first.r || v.g != first.g || v.b != first.b || v.a != first.a)
                throw std::invalid_argument("Phase 3 scenes use flat colors");
        }
        const double total = edge(t.vertices[0], t.vertices[1], t.vertices[2].x, t.vertices[2].y);
        if (total == 0) throw std::invalid_argument("degenerate Phase 3 triangle");
        if (total < 0) std::swap(t.vertices[1], t.vertices[2]);
    }
    return checked;
}
void draw(Raster &target, const std::vector<Triangle> &scene, bool depth_test,
          std::size_t origin_x, std::size_t origin_y) {
    for (std::size_t index = 0; index < scene.size(); ++index) {
        const auto &v = scene[index].vertices;
        const double total = edge(v[0], v[1], v[2].x, v[2].y);
        for (std::size_t y = 0; y < target.color.height; ++y) {
            for (std::size_t x = 0; x < target.color.width; ++x) {
                const double px = double(x + origin_x) + .5, py = double(y + origin_y) + .5;
                const double e0 = edge(v[1], v[2], px, py), e1 = edge(v[2], v[0], px, py), e2 = edge(v[0], v[1], px, py);
                if (!covered(e0, v[1], v[2]) || !covered(e1, v[2], v[0]) || !covered(e2, v[0], v[1])) continue;
                const auto pos = y * target.color.width + x;
                const float z = static_cast<float>((e0 * v[0].z + e1 * v[1].z + e2 * v[2].z) / total);
                if (depth_test && !(z < target.depth[pos])) continue;
                const Bgra8 source{static_cast<std::uint8_t>(v[0].b), static_cast<std::uint8_t>(v[0].g),
                                   static_cast<std::uint8_t>(v[0].r), static_cast<std::uint8_t>(v[0].a)};
                target.color.pixels[pos] = depth_test ? source : alpha_blend(source, target.color.pixels[pos]);
                if (depth_test) target.depth[pos] = z;
                target.owner[pos] = static_cast<std::uint32_t>(index + 1);
            }
        }
    }
}
Raster empty_raster(std::size_t w, std::size_t h, Bgra8 clear) {
    return {make_image(w, h, clear), std::vector<float>(area(w, h), 1.0f), std::vector<std::uint32_t>(area(w, h), 0)};
}
void config_valid(UbwcConfig c) {
    if (c.version != 6 || c.highest_bank_bit != 16 || c.swizzle_levels != 6 || c.macrotile_mode != 1)
        throw std::invalid_argument("UBWC oracle requires Kaanapali 6/16/6/1");
}
std::uint32_t checksum(const std::vector<std::uint8_t> &data, std::size_t offset, std::size_t bytes) {
    std::uint32_t result = 2166136261u;
    for (std::size_t i = 0; i < bytes; ++i) result = (result ^ data[offset + i]) * 16777619u;
    return result;
}
void append(std::vector<std::uint8_t> &data, Bgra8 p) {
    data.insert(data.end(), {p.b, p.g, p.r, p.a});
}
Bgra8 payload_pixel(const UbwcModel &model, const ModelBlock &block, std::size_t x, std::size_t y) {
    const auto offset = block.offset + (block.kind == BlockKind::Constant ? 0 :
        ((y - block.y) * block.width + x - block.x) * 4);
    return {model.payload[offset], model.payload[offset + 1], model.payload[offset + 2], model.payload[offset + 3]};
}
} // namespace

Bgra8 alpha_blend(Bgra8 s, Bgra8 d) {
    const unsigned a = s.a, inverse = 255 - a;
    auto mix = [&](unsigned x, unsigned y) { return static_cast<std::uint8_t>((x * a + y * inverse + 127) / 255); };
    return {mix(s.b, d.b), mix(s.g, d.g), mix(s.r, d.r),
            static_cast<std::uint8_t>(a + (unsigned(d.a) * inverse + 127) / 255)};
}
std::vector<Triangle> overlap_scene(std::size_t w, std::size_t h) {
    (void)area(w, h);
    const float x = static_cast<float>(w), y = static_cast<float>(h);
    return {triangle(0, 0, x, 0, 0, y, .75f, {224, 32, 16, 85}),
            triangle(x, y, 0, y, x, 0, .5f, {16, 160, 240, 170}),
            triangle(x / 8, y / 8, 7 * x / 8, y / 8, x / 2, 7 * y / 8, .25f, {80, 240, 32, 128}),
            triangle(x / 4, 0, 3 * x / 4, y, 0, y / 2, .125f, {192, 48, 224, 85})};
}
std::vector<Triangle> depth_scene(std::size_t w, std::size_t h) {
    auto scene = overlap_scene(w, h);
    const std::array<float, 4> z{{.75f, .5f, .125f, .25f}};
    for (std::size_t i = 0; i < scene.size(); ++i)
        for (auto &v : scene[i].vertices) { v.z = z[i]; v.a = 255; }
    // Far geometry submitted last must never erase already covered foreground.
    scene.push_back(triangle(0, 0, static_cast<float>(w), 0, 0, static_cast<float>(h), .875f, {255, 0, 255, 255}));
    return scene;
}
Raster raster_linear(std::size_t w, std::size_t h, const std::vector<Triangle> &scene, bool depth_test, Bgra8 clear) {
    auto result = empty_raster(w, h, clear);
    draw(result, checked_scene(scene), depth_test, 0, 0);
    return result;
}
Raster raster_tiled(std::size_t w, std::size_t h, const std::vector<Triangle> &scene, bool memoryless,
                    std::size_t tw, std::size_t th, Bgra8 clear) {
    tile_valid(tw, th);
    auto result = empty_raster(w, h, clear);
    const auto checked = checked_scene(scene);
    for (std::size_t oy = 0; oy < h; oy += th) for (std::size_t ox = 0; ox < w; ox += tw) {
        // A fresh depth allocation/clear per Tile models Memoryless lifetime.
        auto local = empty_raster(std::min(tw, w - ox), std::min(th, h - oy), clear);
        draw(local, checked, memoryless, ox, oy);
        for (std::size_t y = 0; y < local.color.height; ++y) for (std::size_t x = 0; x < local.color.width; ++x) {
            const auto src = y * local.color.width + x, dst = (oy + y) * w + ox + x;
            result.color.pixels[dst] = local.color.pixels[src];
            result.depth[dst] = local.depth[src]; result.owner[dst] = local.owner[src];
        }
    }
    return result;
}
Bgra8 boundary_pixel(std::size_t x, std::size_t y, std::size_t w, std::size_t h) {
    (void)area(w, h);
    if (x >= w || y >= h) throw std::out_of_range("boundary pixel coordinate");
    Bgra8 p{static_cast<std::uint8_t>((x + y) * 255 / std::max<std::size_t>(1, w + h - 2)),
            static_cast<std::uint8_t>(y * 255 / std::max<std::size_t>(1, h - 1)),
            static_cast<std::uint8_t>(x * 255 / std::max<std::size_t>(1, w - 1)), 255};
    if ((x + 2 * y) % 193 < 4) p = {240, 240, 240, 255};
    if (x % 96 == 95) p = {255, 0, 255, 255};
    if (x % 96 == 0) p = {0, 255, 0, 255};
    if (y % 32 == 31) p = {0, 96, 255, 255};
    if (y % 32 == 0) p = {255, 224, 0, 255};
    if ((x % 96 == 0 || x % 96 == 95) && (y % 32 == 0 || y % 32 == 31))
        p = {static_cast<std::uint8_t>((x / 96 * 29 + y / 32 * 17) % 256),
             static_cast<std::uint8_t>(x % 96 == 0 ? 255 : 0), static_cast<std::uint8_t>(y % 32 == 0 ? 255 : 0), 255};
    return p;
}
Image boundary_image(std::size_t w, std::size_t h) {
    auto result = make_image(w, h, {0, 0, 0, 0});
    for (std::size_t y = 0; y < h; ++y) for (std::size_t x = 0; x < w; ++x) result.at(x, y) = boundary_pixel(x, y, w, h);
    return result;
}
Image tile_resolve(const Image &source, std::size_t tw, std::size_t th) {
    valid(source); tile_valid(tw, th);
    auto result = make_image(source.width, source.height, {0, 0, 0, 0});
    for (std::size_t oy = 0; oy < source.height; oy += th) for (std::size_t ox = 0; ox < source.width; ox += tw)
        for (std::size_t y = oy; y < std::min(source.height, oy + th); ++y)
            std::copy_n(source.pixels.begin() + static_cast<std::ptrdiff_t>(y * source.width + ox),
                        std::min(tw, source.width - ox), result.pixels.begin() + static_cast<std::ptrdiff_t>(y * source.width + ox));
    return result;
}
TileComparison compare_tiles(const Image &expected, const Image &actual) {
    valid(expected); valid(actual);
    TileComparison result; result.pixels = compare_images(expected, actual, 0);
    if (!result.pixels.shape_equal) return result;
    for (std::size_t y = 0; y < expected.height; ++y) for (std::size_t x = 0; x < expected.width; ++x) {
        if (expected.at(x, y) == actual.at(x, y)) continue;
        if (!result.has_first) { result.has_first = true; result.first_x = x; result.first_y = y; }
        if (x % 96 == 0 || x % 96 == 95 || y % 32 == 0 || y % 32 == 31 || x + 1 == expected.width || y + 1 == expected.height)
            ++result.seam_mismatches;
    }
    return result;
}
Bgra8 glass_source(std::size_t x, std::size_t y, unsigned pass) {
    if (pass >= 4 || x > 65535 || y > 65535) throw std::invalid_argument("glass coordinates/pass");
    const auto shifted = static_cast<unsigned>(x) + pass * 13;
    const auto row = static_cast<unsigned>(y) + pass * 7;
    auto tap = [](unsigned a, unsigned b, unsigned factor) { return (a * factor + b * (factor + 2)) % 256; };
    auto gaussian = [&](unsigned factor) {
        return static_cast<std::uint8_t>((tap(shifted, row, factor) + 2 * tap(shifted + 1, row, factor) +
                                         tap(shifted + 2, row, factor) + 2) / 4);
    };
    return {gaussian(5), gaussian(3), gaussian(7), 255};
}
Bgra8 glass_mix(Bgra8 previous, Bgra8 source, unsigned pass) {
    if (pass >= 4) throw std::invalid_argument("glass pass");
    const unsigned weight = 1 + pass * 2;
    auto channel = [&](unsigned p, unsigned s) { return static_cast<std::uint8_t>((p * (8 - weight) + s * weight + 4) / 8); };
    return {channel(previous.b, source.b), channel(previous.g, source.g), channel(previous.r, source.r), channel(previous.a, source.a)};
}
Image glass_linear(const Image &initial, unsigned passes) {
    valid(initial); if (!passes || passes > 4) throw std::invalid_argument("glass pass count");
    auto result = initial;
    for (unsigned p = 0; p < passes; ++p) for (std::size_t y = 0; y < initial.height; ++y)
        for (std::size_t x = 0; x < initial.width; ++x) result.at(x, y) = glass_mix(result.at(x, y), glass_source(x, y, p), p);
    return result;
}
Image glass_tiled(const Image &initial, unsigned passes, std::size_t tw, std::size_t th) {
    valid(initial); tile_valid(tw, th); if (!passes || passes > 4) throw std::invalid_argument("glass pass count");
    auto result = make_image(initial.width, initial.height, {0, 0, 0, 0});
    for (std::size_t oy = 0; oy < initial.height; oy += th) for (std::size_t ox = 0; ox < initial.width; ox += tw) {
        const auto w = std::min(tw, initial.width - ox), h = std::min(th, initial.height - oy);
        auto local = make_image(w, h, {0, 0, 0, 0});
        for (std::size_t y = 0; y < h; ++y) for (std::size_t x = 0; x < w; ++x) local.at(x, y) = initial.at(ox + x, oy + y);
        for (unsigned p = 0; p < passes; ++p) for (std::size_t y = 0; y < h; ++y) for (std::size_t x = 0; x < w; ++x)
            local.at(x, y) = glass_mix(local.at(x, y), glass_source(ox + x, oy + y, p), p);
        for (std::size_t y = 0; y < h; ++y) for (std::size_t x = 0; x < w; ++x) result.at(ox + x, oy + y) = local.at(x, y);
    }
    return result;
}
Bgra8 ubwc_pattern_pixel(std::size_t x, std::size_t y) {
    if (x > 65535 || y > 65535) throw std::invalid_argument("UBWC pattern coordinates");
    const auto bx = static_cast<unsigned>(x / 16), by = static_cast<unsigned>(y / 4);
    if ((bx + by) % 2 == 0) return {static_cast<std::uint8_t>((bx * 29 + by * 17) % 256), 48, 192, 255};
    std::uint32_t seed = static_cast<std::uint32_t>(x) * 747796405u + static_cast<std::uint32_t>(y) * 2891336453u + 277803737u;
    seed = (seed ^ (seed >> 16)) * 2246822519u; seed ^= seed >> 13;
    return {static_cast<std::uint8_t>(seed), static_cast<std::uint8_t>(seed >> 8), static_cast<std::uint8_t>(seed >> 16), 255};
}
Image ubwc_pattern(std::size_t w, std::size_t h) {
    auto result = make_image(w, h, {0, 0, 0, 0});
    for (std::size_t y = 0; y < h; ++y) for (std::size_t x = 0; x < w; ++x) result.at(x, y) = ubwc_pattern_pixel(x, y);
    return result;
}
UbwcModel encode_ubwc_model(const Image &source, UbwcConfig config) {
    valid(source); config_valid(config);
    UbwcModel model; model.config = config; model.width = source.width; model.height = source.height;
    for (std::size_t y = 0; y < source.height; y += 4) for (std::size_t x = 0; x < source.width; x += 16) {
        const auto w = std::min<std::size_t>(16, source.width - x), h = std::min<std::size_t>(4, source.height - y);
        bool solid = true;
        for (std::size_t by = 0; by < h; ++by) for (std::size_t bx = 0; bx < w; ++bx)
            solid = solid && source.at(x + bx, y + by) == source.at(x, y);
        const auto start = model.payload.size();
        if (solid) { append(model.payload, source.at(x, y)); ++model.compressed_blocks; }
        else {
            for (std::size_t by = 0; by < h; ++by) for (std::size_t bx = 0; bx < w; ++bx) append(model.payload, source.at(x + bx, y + by));
            ++model.escape_blocks;
        }
        const auto bytes = model.payload.size() - start;
        model.blocks.push_back({solid ? BlockKind::Constant : BlockKind::Escape, x, y, w, h, start, bytes, checksum(model.payload, start, bytes)});
    }
    return model;
}
void validate_ubwc_model(const UbwcModel &model) {
    (void)area(model.width, model.height); config_valid(model.config);
    const auto nx = (model.width + 15) / 16, ny = (model.height + 3) / 4;
    if (model.blocks.size() != nx * ny) throw std::invalid_argument("UBWC model block count");
    std::size_t offset = 0, compressed = 0, escape = 0;
    for (std::size_t i = 0; i < model.blocks.size(); ++i) {
        const auto &b = model.blocks[i]; const auto x = i % nx * 16, y = i / nx * 4;
        const auto w = std::min<std::size_t>(16, model.width - x), h = std::min<std::size_t>(4, model.height - y);
        if (b.x != x || b.y != y || b.width != w || b.height != h || b.offset != offset ||
            (b.kind != BlockKind::Constant && b.kind != BlockKind::Escape)) throw std::invalid_argument("UBWC model metadata");
        const auto required = b.kind == BlockKind::Constant ? std::size_t(4) : w * h * 4;
        if (b.bytes != required || offset > model.payload.size() || required > model.payload.size() - offset)
            throw std::invalid_argument("UBWC model payload bounds");
        if (checksum(model.payload, offset, required) != b.checksum) throw std::invalid_argument("UBWC model payload corruption");
        offset += required; if (b.kind == BlockKind::Constant) ++compressed; else ++escape;
    }
    if (offset != model.payload.size() || compressed != model.compressed_blocks || escape != model.escape_blocks)
        throw std::invalid_argument("UBWC model trailing payload or branch counts");
}
Image decode_ubwc_model(const UbwcModel &model) {
    validate_ubwc_model(model);
    auto result = make_image(model.width, model.height, {0, 0, 0, 0});
    for (const auto &b : model.blocks) for (std::size_t y = b.y; y < b.y + b.height; ++y)
        for (std::size_t x = b.x; x < b.x + b.width; ++x) result.at(x, y) = payload_pixel(model, b, x, y);
    return result;
}
Bgra8 sample_ubwc_model(const UbwcModel &model, std::size_t x, std::size_t y) {
    validate_ubwc_model(model);
    if (x >= model.width || y >= model.height) throw std::out_of_range("UBWC sample coordinate");
    return payload_pixel(model, model.blocks[y / 4 * ((model.width + 15) / 16) + x / 16], x, y);
}
Scanout scanout_model(const UbwcModel &model, MdssCaps caps, std::uint64_t base, std::size_t meta_pitch) {
    validate_ubwc_model(model);
    std::string reason;
    if (!caps.ubwc6) reason = "mdss_no_ubwc6";
    else if (!caps.bgra8) reason = "mdss_format";
    else if (!base || base % 4096 || !meta_pitch || meta_pitch % 64) reason = "mdss_alignment";
    else if (model.width > caps.max_width || model.height > caps.max_height) reason = "mdss_extent";
    if (!reason.empty()) return {ScanoutMode::NativeLinearResolve, reason, decode_ubwc_model(model)};
    auto image = make_image(model.width, model.height, {0, 0, 0, 0});
    const auto nx = (model.width + 15) / 16;
    // Direct scanout samples encoded blocks; native downgrade resolves first.
    for (std::size_t y = 0; y < model.height; ++y) for (std::size_t x = 0; x < model.width; ++x)
        image.at(x, y) = payload_pixel(model, model.blocks[y / 4 * nx + x / 16], x, y);
    return {ScanoutMode::DirectUbwc, "mdss_direct_ubwc", std::move(image)};
}
Comparison sample_comparison(std::size_t index) {
    switch (index) {
    case 0: return compare_images(raster_linear(288, 96, overlap_scene()).color, raster_tiled(288, 96, overlap_scene()).color);
    case 1: return compare_images(raster_linear(288, 96, depth_scene(), true).color, raster_tiled(288, 96, depth_scene(), true).color);
    case 2: { const auto image = boundary_image(); return compare_tiles(image, tile_resolve(image)).pixels; }
    case 3: { const auto image = boundary_image(288, 96); return compare_images(glass_linear(image), glass_tiled(image)); }
    case 4: { const auto image = ubwc_pattern(); return compare_images(image, decode_ubwc_model(encode_ubwc_model(image))); }
    case 5: {
        const auto model = encode_ubwc_model(ubwc_pattern(1200, 2608)); MdssCaps caps; caps.ubwc6 = false;
        return compare_images(scanout_model(model).pixels, scanout_model(model, caps).pixels);
    }
    default: throw std::invalid_argument("Phase 3 sample case index");
    }
}
} // namespace nezha_gpu_test::phase3
