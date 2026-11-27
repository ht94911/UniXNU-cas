/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_GPU_TEST_CPU_REFERENCE_CA_GLASS_H
#define NEZHA_GPU_TEST_CPU_REFERENCE_CA_GLASS_H

#include "cpu_reference.h"

namespace nezha_gpu_test::ca_glass {

inline constexpr std::array<const char *, 2> cases = {{"gpu.ca.sweep", "ui.glass_live"}};
inline constexpr unsigned max_lsb = 1;
inline constexpr std::size_t glass_frame_count = 60;

/* Working pixels use premultiplied, linear Display P3. Pane tints are straight
 * alpha so that changing tint opacity does not change its RGB definition. */
struct LinearRgba { double r, g, b, a; };
struct LinearImage {
    std::size_t width, height;
    std::vector<LinearRgba> pixels;
};
struct RoundedRect { double x, y, width, height, radius; };
struct Pane { RoundedRect clip; LinearRgba tint; };
struct BackdropPass {
    unsigned radius;
    double sigma, saturation, brightness;
    std::vector<Pane> panes;
};
enum class Axis { Horizontal, Vertical };
enum class Target { Srgb8, Xr10Linear, Xr10Srgb };

/* Unpacked channel codes, one integer per channel. Extract the upper ten
 * bits of each 16-bit component before comparing a BGRA10_XR readback. */
struct Pixel {
    std::uint16_t b, g, r, a;
    bool operator==(const Pixel &other) const noexcept {
        return b == other.b && g == other.g && r == other.r && a == other.a;
    }
};
struct Surface {
    std::size_t width, height;
    Target target;
    std::vector<Pixel> pixels;
};
struct Composite {
    Surface image;
    std::vector<std::uint8_t> clip_mask;
};
struct PixelComparison {
    Comparison pixels;
    std::size_t leaked_px = 0, black_px = 0;
};

double rounded_coverage(RoundedRect rect, double sample_x, double sample_y);
std::vector<double> gaussian_kernel(unsigned radius, double sigma);
LinearImage gaussian_pass(const LinearImage &source, unsigned radius, double sigma, Axis axis);
LinearRgba source_over(LinearRgba source, LinearRgba destination);
LinearRgba adjust_backdrop(LinearRgba pixel, double saturation, double brightness);
Pixel encode_target(LinearRgba pixel, Target target);

/* A pass blurs the incoming backdrop once, then applies its panes in order.
 * The next pass reads the completed previous pass. */
Composite compose(const LinearImage &background, const std::vector<BackdropPass> &passes, Target target);
std::vector<Composite> ca_sweep_reference();
PixelComparison compare_composite(const Composite &expected, const Surface &actual);

struct GlassFrame {
    int scroll_x, scroll_y;
    Surface image;
};
enum class Fault { FrameCount, Shape, BlackFrame, Tear, StaleTile, FrozenFrame, PixelMismatch };
struct FrameFault {
    Fault kind;
    std::size_t frame, x = 0, y = 0;
};
struct SequenceComparison {
    Comparison pixels;
    std::vector<FrameFault> faults;
    bool passed() const noexcept { return pixels.passed() && faults.empty(); }
};

std::vector<GlassFrame> glass_live_reference();
/* Captures are in presentation order. Motion comes from the reference input
 * sequence. Tear y is the first distinguishable row on the bottom side.
 * Stale Tile x/y give the tile origin in pixels, including partial tiles. */
SequenceComparison compare_glass_live(const std::vector<GlassFrame> &expected,
                                     const std::vector<Surface> &actual);

} // namespace nezha_gpu_test::ca_glass
#endif
