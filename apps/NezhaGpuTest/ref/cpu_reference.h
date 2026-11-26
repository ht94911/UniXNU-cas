/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_GPU_TEST_CPU_REFERENCE_H
#define NEZHA_GPU_TEST_CPU_REFERENCE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nezha_gpu_test {

inline constexpr std::array<const char *, 8> phase2_cases = {{
    "gpu.blit.buffer_copy", "gpu.blit.tex_clear_bgra8", "gpu.blit.tex_blit_scale",
    "gpu.compute.add_vec", "gpu.compute.nonuniform_grid", "gpu.render.solid_triangle",
    "gpu.render.indexed_quad_textured", "gpu.sync.fence_and_order"
}};
inline constexpr std::uint32_t grid_guard = 0xd3adbeefu;
inline constexpr float float_guard = -12345.25f;

struct Bgra8 {
    std::uint8_t b, g, r, a;
    bool operator==(const Bgra8 &other) const noexcept;
};
static_assert(sizeof(Bgra8) == 4, "BGRA8 packed pixel");

struct Color { float r, g, b, a; };
struct Rect { std::size_t x, y, width, height; };
/* Eight individual floats also have the exact same layout in the Metal shader. */
struct Vertex { float x, y, r, g, b, a, u, v; };
static_assert(sizeof(Vertex) == 32, "Metal TestVertex layout");

struct Image {
    std::size_t width = 0, height = 0;
    std::vector<Bgra8> pixels;
    Bgra8 &at(std::size_t x, std::size_t y);
    const Bgra8 &at(std::size_t x, std::size_t y) const;
};

enum class Filter { Nearest, Bilinear };
struct Comparison {
    double max_err = 0;
    std::size_t mismatched_px = 0;
    std::size_t compared = 0;
    bool shape_equal = true;
    bool passed() const noexcept;
    void merge(const Comparison &other);
};
struct RasterReference {
    Image image;
    /* Per-pixel UNORM tolerance: zero for interior/background, 1 at edges. */
    std::vector<std::uint8_t> tolerance;
};

std::vector<std::uint8_t> byte_pattern(std::size_t count, std::uint32_t seed = 1);
std::vector<std::uint8_t> buffer_copy_reference(const std::vector<std::uint8_t> &source,
    const std::vector<std::uint8_t> &initial, std::size_t source_offset,
    std::size_t destination_offset, std::size_t bytes);
Image make_image(std::size_t width, std::size_t height, Bgra8 fill);
Image gradient_image(std::size_t width, std::size_t height);
Image checker_image(std::size_t width, std::size_t height, std::size_t block = 4);
Image clear_reference(const Image &initial, Rect rectangle, Bgra8 color);
Color sample_reference(const Image &source, double u, double v, Filter filter);
Image scale_reference(const Image &source, std::size_t width, std::size_t height, Filter filter);
Bgra8 quantize(Color color);
Bgra8 alpha_blend_reference(Color source, Bgra8 destination);

std::vector<float> vector_input(std::size_t count, bool second);
std::vector<float> add_vec_reference(const std::vector<float> &a,
                                   const std::vector<float> &b, float multiplier);
std::uint32_t grid_value(std::size_t x, std::size_t y);
std::vector<std::uint32_t> nonuniform_grid_reference(std::size_t width = 127,
    std::size_t height = 83, std::size_t guard_words = 8);

std::array<Vertex, 3> triangle_vertices(std::size_t width = 256, std::size_t height = 256);
RasterReference triangle_reference(std::size_t width, std::size_t height,
    const std::array<Vertex, 3> &vertices, Bgra8 clear);
std::array<Vertex, 4> quad_vertices(std::size_t width, std::size_t height);
Image indexed_quad_reference(std::size_t width, std::size_t height, const Image &texture,
    const std::vector<std::uint32_t> &indices, Bgra8 clear);

Comparison compare_bytes(const std::vector<std::uint8_t> &expected,
                         const std::vector<std::uint8_t> &actual);
Comparison compare_words(const std::vector<std::uint32_t> &expected,
                         const std::vector<std::uint32_t> &actual);
Comparison compare_floats(const std::vector<float> &expected,
                         const std::vector<float> &actual, double tolerance = 0);
Comparison compare_images(const Image &expected, const Image &actual, std::uint8_t tolerance = 0);
Comparison compare_raster(const RasterReference &expected, const Image &actual);
std::vector<std::uint32_t> fence_chain_reference(std::size_t commands = 16);
Comparison compare_fence_chain(const std::vector<std::uint32_t> &state,
    const std::vector<std::uint32_t> &observed_reads, const std::vector<std::uint32_t> &completion_order,
    std::size_t commands = 16);
std::string hwtest_line(const std::string &case_id, const Comparison &result,
    std::uint64_t elapsed_us, const std::string &path, const std::string &note);

} // namespace nezha_gpu_test
#endif
