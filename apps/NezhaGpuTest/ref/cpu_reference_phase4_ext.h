/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_GPU_TEST_CPU_REFERENCE_PHASE4_EXT_H
#define NEZHA_GPU_TEST_CPU_REFERENCE_PHASE4_EXT_H

#include "cpu_reference.h"

namespace nezha_gpu_test::phase4_ext {

inline constexpr std::array<const char *, 5> cases = {{
    "gpu.render.msaa4x_resolve", "gpu.render.visibility_query",
    "gpu.render.tess_amplify", "gpu.camera155.nv12_preview_photo", "gpu.mps.gemm_ci_blur"
}};
using Triangle = std::array<Vertex, 3>;
/* Standard 4x rotated grid, relative to the pixel's top-left corner. */
inline constexpr std::array<std::array<double, 2>, 4> sample_locations = {{
    {{3.0 / 8, 1.0 / 8}}, {{7.0 / 8, 3.0 / 8}},
    {{1.0 / 8, 5.0 / 8}}, {{5.0 / 8, 7.0 / 8}}
}};
inline constexpr Bgra8 clear_pixel{0, 0, 0, 255};

struct MsaaReference {
    Image resolved, single_sample;
    std::vector<std::array<Bgra8, 4>> samples;
    std::vector<std::uint8_t> coverage_mask;
};
Image resolve_msaa4x(std::size_t width, std::size_t height,
                    const std::vector<std::array<Bgra8, 4>> &samples);
MsaaReference msaa4x_reference(std::size_t width, std::size_t height,
                             Triangle triangle, Bgra8 clear = clear_pixel);
Comparison compare_msaa(const MsaaReference &expected, const MsaaReference &actual);

struct DepthVertex { double x, y, z; };
struct DepthTriangle { std::array<DepthVertex, 3> vertices; Bgra8 color; };
enum class QueryMode { Counting, Boolean };
enum class DepthCompare { Less, LessEqual };
struct VisibilityReference {
    Image color;
    std::vector<double> depth;
    std::vector<std::uint64_t> passing_fragments, query_results;
};
std::vector<DepthTriangle> visibility_scene();
VisibilityReference visibility_reference(std::size_t width, std::size_t height,
    const std::vector<DepthTriangle> &scene, QueryMode mode = QueryMode::Counting,
    DepthCompare compare = DepthCompare::Less, bool depth_write = true);
Comparison compare_visibility(const VisibilityReference &expected, const VisibilityReference &actual);

/* Equal-spacing triangle domain. Indices have n*n triangles; shared lattice
 * vertices are stored once, so a top-left fill rule catches holes/overdraw. */
struct TessMesh {
    unsigned factor = 0;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};
struct TessReference {
    Image image;
    std::vector<std::uint32_t> coverage;
    std::array<std::int32_t, 2> viewport_offsets;
};
TessMesh tessellate_triangle(Triangle control, unsigned factor);
TessReference amplify_two_views(const TessMesh &mesh, std::size_t viewport_width,
    std::size_t height, std::array<std::int32_t, 2> offsets);
Comparison compare_tess(const TessReference &expected, const TessReference &actual);

enum class YuvRange { Video, Full };
/* BT.709 nonlinear RGB codes; no transfer-curve or Display P3 conversion is
 * implied. UV is interleaved Cb,Cr at 4:2:0 nearest/chroma-block coordinates. */
struct Nv12Frame {
    std::size_t width = 0, height = 0, y_stride = 0, uv_stride = 0;
    std::vector<std::uint8_t> y, uv;
    YuvRange range = YuvRange::Video;
};
struct CameraReference { Image preview, photo; Rect photo_crop; };
Nv12Frame nv12_fixture(std::size_t width = 64, std::size_t height = 48, std::size_t padding = 0);
Bgra8 bt709_nv12_pixel(const Nv12Frame &frame, std::size_t x, std::size_t y);
/* Preview uses center-aligned nearest scaling; photo is an unscaled crop.
 * Odd crop origins are legal and retain the source's original chroma phase. */
CameraReference camera155_reference(const Nv12Frame &frame, std::size_t preview_width,
                                   std::size_t preview_height, Rect photo_crop);
Comparison compare_camera(const CameraReference &expected, const CameraReference &actual);

enum class Accumulation { Float32, Float16 };
struct Matrix { std::size_t rows = 0, columns = 0; std::vector<float> values; };
struct NumericComparison {
    Comparison values;
    double max_relative_err = 0;
    bool passed() const noexcept { return values.passed(); }
};
/* IEEE binary16 round-to-nearest, ties-to-even, including signed zero and
 * subnormals. Nonfinite inputs and overflow are controlled errors. */
std::uint16_t float_to_half(float value);
float half_to_float(std::uint16_t bits);
Matrix gemm_reference(const Matrix &left, const Matrix &right, Accumulation mode);
std::vector<double> gemm_precise(const Matrix &left, const Matrix &right, bool half_inputs = false);
NumericComparison compare_gemm(const std::vector<double> &expected, const Matrix &actual,
    std::size_t rows, std::size_t columns, double relative_tolerance,
    double absolute_tolerance = 1e-6);
/* Linear BGRA code-space convolution, clamp-to-edge. Weights 1,4,6,4,1 / 16
 * on each axis; quantization happens only after BOTH passes. */
Image gaussian5_reference(const Image &source);
Comparison sample_comparison(std::size_t case_index);

} // namespace nezha_gpu_test::phase4_ext
#endif
