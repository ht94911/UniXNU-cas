/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_GPU_TEST_CPU_REFERENCE_PHASE3_H
#define NEZHA_GPU_TEST_CPU_REFERENCE_PHASE3_H

#include "cpu_reference.h"

namespace nezha_gpu_test::phase3 {

inline constexpr std::array<const char *, 6> cases = {{
    "gpu.gmem.sysmem_equivalence", "gpu.gmem.memoryless_depth",
    "gpu.gmem.tile_boundary_1200x2608", "gpu.gmem.programmable_blend",
    "gpu.ubwc.roundtrip_bgra8", "gpu.ubwc.scanout_compat_fallback"
}};
inline constexpr std::size_t tile_width = 96, tile_height = 32;
inline constexpr Bgra8 scene_clear{16, 32, 64, 255};

/* Exactly the same 32-byte vertex layout is consumed by the Metal source. */
struct Vertex {
    float x, y, z, padding;
    std::uint32_t r, g, b, a;
};
static_assert(sizeof(Vertex) == 32, "Phase 3 Metal vertex layout");
struct Triangle { std::array<Vertex, 3> vertices; };
struct Raster {
    Image color;
    std::vector<float> depth;
    std::vector<std::uint32_t> owner;
};
std::vector<Triangle> overlap_scene(std::size_t width = 288, std::size_t height = 96);
std::vector<Triangle> depth_scene(std::size_t width = 288, std::size_t height = 96);
Bgra8 alpha_blend(Bgra8 source, Bgra8 destination);
Raster raster_linear(std::size_t width, std::size_t height, const std::vector<Triangle> &scene,
                     bool depth_test = false, Bgra8 clear = scene_clear);
Raster raster_tiled(std::size_t width, std::size_t height, const std::vector<Triangle> &scene,
                    bool memoryless_depth = false, std::size_t tw = tile_width,
                    std::size_t th = tile_height, Bgra8 clear = scene_clear);

Bgra8 boundary_pixel(std::size_t x, std::size_t y, std::size_t width, std::size_t height);
Image boundary_image(std::size_t width = 1200, std::size_t height = 2608);
Image tile_resolve(const Image &source, std::size_t tw = tile_width, std::size_t th = tile_height);
struct TileComparison {
    Comparison pixels;
    std::size_t seam_mismatches = 0;
    std::size_t first_x = 0, first_y = 0;
    bool has_first = false;
};
TileComparison compare_tiles(const Image &expected, const Image &actual);

Bgra8 glass_source(std::size_t x, std::size_t y, unsigned pass);
Bgra8 glass_mix(Bgra8 previous, Bgra8 source, unsigned pass);
Image glass_linear(const Image &initial, unsigned passes = 4);
Image glass_tiled(const Image &initial, unsigned passes = 4,
                  std::size_t tw = tile_width, std::size_t th = tile_height);

/* UBWC semantic oracle only. This constant/escape representation is NOT a
 * Qualcomm UBWC bitstream. It tests lossless block, corruption and scanout
 * contracts without fabricating the private hardware codec or metadata.
 * 16x4 is the BGRA8 block extent; HBB=16 is a separate bank-address property.
 */
struct UbwcConfig {
    unsigned version = 6, highest_bank_bit = 16, swizzle_levels = 6, macrotile_mode = 1;
};
enum class BlockKind { Constant, Escape };
struct ModelBlock {
    BlockKind kind;
    std::size_t x, y, width, height, offset, bytes;
    std::uint32_t checksum;
};
struct UbwcModel {
    UbwcConfig config;
    std::size_t width = 0, height = 0;
    std::vector<ModelBlock> blocks;
    std::vector<std::uint8_t> payload;
    std::size_t compressed_blocks = 0, escape_blocks = 0;
};
inline constexpr std::size_t ubwc_block_width = 16, ubwc_block_height = 4;
Bgra8 ubwc_pattern_pixel(std::size_t x, std::size_t y);
Image ubwc_pattern(std::size_t width = 256, std::size_t height = 128);
UbwcModel encode_ubwc_model(const Image &source, UbwcConfig config = {});
void validate_ubwc_model(const UbwcModel &model);
Image decode_ubwc_model(const UbwcModel &model);
Bgra8 sample_ubwc_model(const UbwcModel &model, std::size_t x, std::size_t y);

struct MdssCaps {
    bool ubwc6 = true, bgra8 = true;
    std::size_t max_width = 1200, max_height = 2608;
};
enum class ScanoutMode { DirectUbwc, NativeLinearResolve };
struct Scanout {
    ScanoutMode mode;
    std::string reason;
    Image pixels;
};
Scanout scanout_model(const UbwcModel &model, MdssCaps caps = {},
                     std::uint64_t base = 4096, std::size_t meta_pitch = 64);
Comparison sample_comparison(std::size_t case_index);

} // namespace nezha_gpu_test::phase3
#endif
