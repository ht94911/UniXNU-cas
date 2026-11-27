/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_GPU_TEST_CPU_REFERENCE_PHASE4_H
#define NEZHA_GPU_TEST_CPU_REFERENCE_PHASE4_H

#include "cpu_reference.h"

namespace nezha_gpu_test::phase4 {

inline constexpr std::array<const char *, 5> cases = {{
    "gpu.heap.aliasing", "gpu.argbuf.bindless", "gpu.indirect.draw_dispatch",
    "gpu.icb.replay", "gpu.encoder.lifecycle_faults"
}};
inline constexpr std::size_t bindless_width = 128, bindless_height = 128;
inline constexpr std::size_t icb_slots = 16, guard_words = 8;
inline constexpr Bgra8 clear_pixel{9, 17, 29, 255};

/* Binary16 channel codes, not host float approximations. The fixture uses
 * dyadic values that are exactly representable in both FP16 and FP32. */
struct HalfPixel { std::uint16_t r, g, b, a; };
static_assert(sizeof(HalfPixel) == 8, "RGBA16F packed pixel");
struct HalfImage {
    std::size_t width = 0, height = 0;
    std::vector<HalfPixel> pixels;
};
struct HeapReference {
    std::vector<std::uint8_t> linear_buffer;
    Image bgra;
    HalfImage rgba16f;
    std::vector<std::uint8_t> guards;
};
struct HeapComparison {
    Comparison buffer, bgra, rgba16f, guards;
    Comparison summary() const;
    bool passed() const noexcept;
};
std::uint8_t heap_buffer_byte(std::size_t offset);
Bgra8 heap_bgra_pixel(std::size_t x, std::size_t y);
HalfPixel heap_half_pixel(std::size_t x, std::size_t y);
/* Reuses one byte allocation in sequence. This is an alias lifetime/pixel
 * oracle; it does not encode Qualcomm UBWC metadata or exercise a GPU. */
HeapReference heap_alias_reference(std::size_t width = 64, std::size_t height = 32);
Comparison compare_half_images(const HalfImage &expected, const HalfImage &actual);
HeapComparison compare_heap_alias(const HeapReference &expected, const HeapReference &actual);

/* The 40-byte selector record is shared verbatim with the Metal source. */
struct BindlessIndex {
    std::array<std::uint32_t, 4> buffer_slots, element_indices;
    std::array<std::uint32_t, 2> texture_slots;
};
static_assert(sizeof(BindlessIndex) == 40, "Metal bindless selector layout");
struct BindlessInputs {
    std::array<std::vector<std::uint32_t>, 4> buffers;
    std::array<Image, 2> textures;
    std::vector<BindlessIndex> indices;
    std::array<float, 6> weights;
    Filter filter = Filter::Nearest;
};
BindlessInputs bindless_inputs(Filter filter = Filter::Nearest);
Image bindless_reference(const BindlessInputs &input);
Comparison compare_bindless(const Image &expected, const Image &actual, Filter filter);

struct DrawArguments {
    std::uint32_t vertex_count, instance_count, vertex_start, base_instance;
};
struct DrawIndexedArguments {
    std::uint32_t index_count, instance_count, index_start;
    std::int32_t base_vertex;
    std::uint32_t base_instance;
};
struct DispatchArguments { std::uint32_t x, y, z; };
struct IndirectArguments {
    DrawArguments draw;
    DrawIndexedArguments indexed;
    DispatchArguments dispatch;
};
static_assert(sizeof(DrawArguments) == 16, "MTLDrawPrimitivesIndirectArguments");
static_assert(sizeof(DrawIndexedArguments) == 20, "MTLDrawIndexedPrimitivesIndirectArguments");
static_assert(sizeof(DispatchArguments) == 12, "MTLDispatchThreadgroupsIndirectArguments");
static_assert(offsetof(IndirectArguments, indexed) == 16, "Metal indexed arguments offset");
static_assert(offsetof(IndirectArguments, dispatch) == 36, "Metal dispatch arguments offset");
static_assert(sizeof(IndirectArguments) == 48, "Metal combined indirect arguments layout");
struct IntVertex { std::int32_t x, y; };
static_assert(sizeof(IntVertex) == 8, "Metal indirect vertex layout");
IndirectArguments fill_indirect_args(std::uint32_t instances = 3);
std::vector<std::uint8_t> indirect_argument_bytes(const IndirectArguments &args);
std::vector<IntVertex> indirect_vertices(bool indexed);
std::vector<std::uint32_t> indirect_indices();
Image draw_indirect_reference(const Image &initial, const std::vector<IntVertex> &vertices,
                              DrawArguments args);
Image draw_indexed_indirect_reference(const Image &initial, const std::vector<IntVertex> &vertices,
    const std::vector<std::uint32_t> &indices, DrawIndexedArguments args);
std::uint32_t indirect_dispatch_value(std::size_t x, std::size_t y, std::size_t z);
std::vector<std::uint32_t> dispatch_storage(std::array<std::uint32_t, 3> extent);
std::vector<std::uint32_t> dispatch_indirect_reference(const std::vector<std::uint32_t> &initial,
    std::array<std::uint32_t, 3> extent, DispatchArguments args,
    std::array<std::uint32_t, 3> threads_per_group = {{4, 3, 1}});
struct IndirectReference {
    std::vector<std::uint8_t> argument_bytes;
    Image draw, indexed;
    std::vector<std::uint32_t> dispatch;
};
IndirectReference indirect_reference(std::uint32_t instances = 3);
Comparison compare_indirect(const IndirectReference &expected, const IndirectReference &actual);

struct IcbCommand {
    bool enabled = false;
    std::uint32_t instance_count = 0, base_instance = 0, generation = 0;
    Rect rectangle{0, 0, 0, 0};
    Bgra8 color{0, 0, 0, 0};
};
using IcbProgram = std::array<IcbCommand, icb_slots>;
/* Device runner packs each slot into this explicit Metal vertex record. */
struct IcbDrawRecord {
    std::uint32_t slot, generation, base_instance;
    float x, y, width, height;
    std::uint32_t bgra;
};
static_assert(sizeof(IcbDrawRecord) == 32, "Metal ICB draw record layout");
struct IcbReplay {
    Image image;
    std::vector<std::uint32_t> pixel_execution_count;
    std::array<std::uint32_t, icb_slots> slot_execution_count{};
    std::vector<std::uint32_t> executed_slots;
};
IcbProgram icb_program();
void reset_range(IcbProgram &program, std::size_t first, std::size_t count);
void record_icb_slot(IcbProgram &program, std::size_t slot, IcbCommand command);
/* end is exclusive: replay [4,12) executes only slots 4 through 11.
 * The explicit sequence variant also supplies duplicate/out-of-range-of-
 * selection fault fixtures. Slot IDs outside [0,16) are controlled errors. */
IcbReplay replay_icb(const IcbProgram &program, std::size_t first = 0, std::size_t end = icb_slots);
IcbReplay replay_icb_sequence(const IcbProgram &program, const std::vector<std::uint32_t> &slots);
Comparison compare_icb(const IcbReplay &expected, const IcbReplay &actual);

enum class PurgeableState { NonVolatile, Volatile, Empty };
enum class LifecycleError {
    None, EncoderEnded, CrossDevice, NullIndirectBuffer, UnalignedOffset,
    IndirectOutOfBounds, EmptyHeap
};
struct EncoderModel { std::uint64_t device = 1; bool ended = false; };
struct LifecycleResource {
    std::uint64_t device = 1;
    std::size_t bytes = 64;
    PurgeableState purgeable = PurgeableState::NonVolatile;
};
struct LifecycleOutcome { LifecycleError error; Image target; std::uint32_t writes; };
/* Deterministic policy model. Actual Metal driver errors still require a
 * downstream device runner; these fixtures cannot certify that behavior. */
LifecycleError end_encoding(EncoderModel &encoder) noexcept;
LifecycleError lifecycle_error(EncoderModel encoder, const LifecycleResource *resource,
                               std::size_t indirect_offset) noexcept;
LifecycleOutcome lifecycle_reference(EncoderModel encoder, const LifecycleResource *resource,
    std::size_t indirect_offset, const Image &initial);
Comparison compare_lifecycle(LifecycleError expected_error, const Image &initial,
                             const LifecycleOutcome &actual);
const char *lifecycle_error_name(LifecycleError error) noexcept;

} // namespace nezha_gpu_test::phase4
#endif
