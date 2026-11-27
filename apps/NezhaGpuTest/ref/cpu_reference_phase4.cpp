/* SPDX-License-Identifier: MIT */
#include "cpu_reference_phase4.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace nezha_gpu_test::phase4 {
namespace {
constexpr std::size_t maximum_elements = 16 * 1024 * 1024;
constexpr std::size_t heap_guard_bytes = 32;

std::size_t area(std::size_t width, std::size_t height) {
    if (!width || !height || width > maximum_elements / height)
        throw std::invalid_argument("invalid image extent");
    return width * height;
}
void valid_image(const Image &image) {
    if (image.pixels.size() != area(image.width, image.height))
        throw std::invalid_argument("image storage extent");
}
void valid_half(const HalfImage &image) {
    if (image.pixels.size() != area(image.width, image.height))
        throw std::invalid_argument("half image storage extent");
}
void range(std::size_t size, std::size_t start, std::size_t count) {
    if (start > size || count > size - start)
        throw std::out_of_range("reference range");
}
std::size_t volume(std::array<std::uint32_t, 3> extent) {
    const auto plane = area(extent[0], extent[1]);
    if (!extent[2] || plane > maximum_elements / extent[2])
        throw std::invalid_argument("dispatch extent");
    return plane * extent[2];
}
std::uint16_t sixteenths(unsigned value) {
    if (!value) return 0;
    unsigned leading = 0;
    while ((1u << (leading + 1)) <= value) ++leading;
    const auto exponent = 15u + leading - 4u;
    const auto fraction = (value - (1u << leading)) << (10u - leading);
    return static_cast<std::uint16_t>((exponent << 10) | fraction);
}
void store16(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint16_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value);
    bytes.at(offset + 1) = static_cast<std::uint8_t>(value >> 8);
}
std::uint16_t load16(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(unsigned(bytes.at(offset)) | (unsigned(bytes.at(offset + 1)) << 8));
}
std::uint32_t pack(Bgra8 pixel) {
    return std::uint32_t(pixel.b) | (std::uint32_t(pixel.g) << 8) |
           (std::uint32_t(pixel.r) << 16) | (std::uint32_t(pixel.a) << 24);
}
std::array<double, 4> codes(std::uint32_t word) {
    return {{double(word & 255), double((word >> 8) & 255),
             double((word >> 16) & 255), double(word >> 24)}};
}
std::array<double, 4> sampled_codes(const Image &texture, double u, double v, Filter filter) {
    const auto color = sample_reference(texture, u, v, filter);
    if (filter == Filter::Nearest) return codes(pack(quantize(color)));
    return {{double(color.b) * 255, double(color.g) * 255,
             double(color.r) * 255, double(color.a) * 255}};
}
std::uint8_t channel(double value) {
    return static_cast<std::uint8_t>(std::floor(std::clamp(value, 0.0, 255.0) + 0.5));
}
void valid_instances(std::uint32_t base, std::uint32_t count) {
    if (count > 64 || std::uint64_t(base) + count > 4096)
        throw std::invalid_argument("instance range");
}
void valid_vertex(IntVertex vertex) {
    if (vertex.x < -4096 || vertex.x > 4096 || vertex.y < -4096 || vertex.y > 4096)
        throw std::invalid_argument("vertex coordinate");
}
std::int64_t edge(IntVertex a, IntVertex b, std::int64_t x2, std::int64_t y2) {
    return (std::int64_t(b.x) - a.x) * (y2 - std::int64_t(a.y) * 2) -
           (std::int64_t(b.y) - a.y) * (x2 - std::int64_t(a.x) * 2);
}
void raster_triangle(Image &image, std::array<IntVertex, 3> triangle,
                     std::uint32_t instance, bool indexed) {
    for (auto &vertex : triangle) {
        valid_vertex(vertex);
        vertex.x += static_cast<std::int32_t>(instance * 5 + (indexed ? 32 : 0));
        vertex.y += indexed ? 16 : 0;
    }
    const auto sign = edge(triangle[0], triangle[1], std::int64_t(triangle[2].x) * 2,
                           std::int64_t(triangle[2].y) * 2);
    if (!sign) return;
    const Bgra8 color{static_cast<std::uint8_t>(13 + instance * 3),
                     static_cast<std::uint8_t>(23 + instance * 5),
                     static_cast<std::uint8_t>(indexed ? 77 : 201), 255};
    for (std::size_t y = 0; y < image.height; ++y)
        for (std::size_t x = 0; x < image.width; ++x) {
            const auto x2 = static_cast<std::int64_t>(x) * 2 + 1;
            const auto y2 = static_cast<std::int64_t>(y) * 2 + 1;
            const auto a = edge(triangle[0], triangle[1], x2, y2);
            const auto b = edge(triangle[1], triangle[2], x2, y2);
            const auto c = edge(triangle[2], triangle[0], x2, y2);
            if ((sign > 0 && a >= 0 && b >= 0 && c >= 0) ||
                (sign < 0 && a <= 0 && b <= 0 && c <= 0)) image.at(x, y) = color;
        }
}
void append32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
void valid_icb(IcbCommand command) {
    if (!command.enabled || !command.instance_count) return;
    valid_instances(command.base_instance, command.instance_count);
    const auto &r = command.rectangle;
    if (!r.width || !r.height) throw std::invalid_argument("empty recorded ICB rectangle");
    range(128, r.x, r.width);
    range(64, r.y, r.height);
    const auto last = std::uint64_t(command.base_instance) + command.instance_count - 1;
    if (last * 5 > 128 - r.x - r.width)
        throw std::out_of_range("ICB instance rectangle");
}
} // namespace

Comparison HeapComparison::summary() const {
    auto result = buffer;
    result.merge(bgra); result.merge(rgba16f); result.merge(guards);
    return result;
}
bool HeapComparison::passed() const noexcept {
    return buffer.passed() && bgra.passed() && rgba16f.passed() && guards.passed();
}
std::uint8_t heap_buffer_byte(std::size_t offset) {
    return static_cast<std::uint8_t>((offset & 255) * 37 + 19);
}
Bgra8 heap_bgra_pixel(std::size_t x, std::size_t y) {
    return {static_cast<std::uint8_t>((x * 17 + y * 3 + 41) & 255),
            static_cast<std::uint8_t>((x * 5 + y * 29 + 67) & 255),
            static_cast<std::uint8_t>((x * 11 + y * 7 + 113) & 255), 255};
}
HalfPixel heap_half_pixel(std::size_t x, std::size_t y) {
    return {sixteenths(static_cast<unsigned>(x & 15)),
            sixteenths(static_cast<unsigned>(y & 15)),
            sixteenths(static_cast<unsigned>((x + 3 * y) & 15)), 0x3c00};
}
HeapReference heap_alias_reference(std::size_t width, std::size_t height) {
    const auto count = area(width, height);
    if (count > maximum_elements / 8) throw std::length_error("heap fixture size");
    const auto bytes = count * 8;
    std::vector<std::uint8_t> physical(bytes + heap_guard_bytes * 2, 0xa7);
    HeapReference result;
    for (std::size_t i = 0; i < bytes; ++i)
        physical[heap_guard_bytes + i] = heap_buffer_byte(i);
    result.linear_buffer.assign(physical.begin() + heap_guard_bytes,
                                physical.end() - heap_guard_bytes);

    // Destroy/rebind the alias between phases. No read of a dead alias occurs.
    for (std::size_t y = 0; y < height; ++y)
        for (std::size_t x = 0; x < width; ++x) {
            const auto pixel = heap_bgra_pixel(x, y);
            const auto offset = heap_guard_bytes + (y * width + x) * 4;
            physical[offset] = pixel.b; physical[offset + 1] = pixel.g;
            physical[offset + 2] = pixel.r; physical[offset + 3] = pixel.a;
        }
    result.bgra = make_image(width, height, clear_pixel);
    for (std::size_t i = 0; i < count; ++i) {
        const auto offset = heap_guard_bytes + i * 4;
        result.bgra.pixels[i] = {physical[offset], physical[offset + 1],
                                physical[offset + 2], physical[offset + 3]};
    }
    for (std::size_t y = 0; y < height; ++y)
        for (std::size_t x = 0; x < width; ++x) {
            const auto pixel = heap_half_pixel(x, y);
            const auto offset = heap_guard_bytes + (y * width + x) * 8;
            store16(physical, offset, pixel.r); store16(physical, offset + 2, pixel.g);
            store16(physical, offset + 4, pixel.b); store16(physical, offset + 6, pixel.a);
        }
    result.rgba16f = {width, height, std::vector<HalfPixel>(count)};
    for (std::size_t i = 0; i < count; ++i) {
        const auto offset = heap_guard_bytes + i * 8;
        result.rgba16f.pixels[i] = {load16(physical, offset), load16(physical, offset + 2),
                                   load16(physical, offset + 4), load16(physical, offset + 6)};
    }
    result.guards.assign(physical.begin(), physical.begin() + heap_guard_bytes);
    result.guards.insert(result.guards.end(), physical.end() - heap_guard_bytes, physical.end());
    return result;
}
Comparison compare_half_images(const HalfImage &expected, const HalfImage &actual) {
    valid_half(expected); valid_half(actual);
    Comparison result;
    result.shape_equal = expected.width == actual.width && expected.height == actual.height;
    if (!result.shape_equal) { result.mismatched_px = 1; return result; }
    result.compared = expected.pixels.size();
    for (std::size_t i = 0; i < result.compared; ++i) {
        const auto &a = expected.pixels[i], &b = actual.pixels[i];
        const auto error = std::max({std::abs(int(a.r) - int(b.r)), std::abs(int(a.g) - int(b.g)),
                                     std::abs(int(a.b) - int(b.b)), std::abs(int(a.a) - int(b.a))});
        result.max_err = std::max(result.max_err, double(error));
        result.mismatched_px += error != 0;
    }
    return result;
}
HeapComparison compare_heap_alias(const HeapReference &expected, const HeapReference &actual) {
    return {compare_bytes(expected.linear_buffer, actual.linear_buffer),
            compare_images(expected.bgra, actual.bgra, 0),
            compare_half_images(expected.rgba16f, actual.rgba16f),
            compare_bytes(expected.guards, actual.guards)};
}

BindlessInputs bindless_inputs(Filter filter) {
    BindlessInputs result;
    result.filter = filter;
    result.weights = {{.125f, .125f, .125f, .125f, .25f, .25f}};
    for (std::size_t slot = 0; slot < 4; ++slot) {
        result.buffers[slot].resize(256);
        for (std::size_t i = 0; i < 256; ++i)
            result.buffers[slot][i] = pack({static_cast<std::uint8_t>(slot * 31 + i * 7 + 5),
                static_cast<std::uint8_t>(slot * 47 + i * 11 + 13),
                static_cast<std::uint8_t>(slot * 37 + i * 3 + 7), 255});
    }
    for (std::size_t slot = 0; slot < 2; ++slot) {
        auto &texture = result.textures[slot];
        texture = make_image(slot ? 23 : 17, slot ? 19 : 13, clear_pixel);
        for (std::size_t y = 0; y < texture.height; ++y)
            for (std::size_t x = 0; x < texture.width; ++x)
                texture.at(x, y) = {static_cast<std::uint8_t>(x * 13 + y * 3 + slot * 71),
                    static_cast<std::uint8_t>(x * 5 + y * 17 + slot * 41),
                    static_cast<std::uint8_t>(x * 19 + y * 7 + slot * 23), 255};
    }
    result.indices.resize(bindless_width * bindless_height);
    for (std::size_t y = 0; y < bindless_height; ++y)
        for (std::size_t x = 0; x < bindless_width; ++x) {
            auto &index = result.indices[y * bindless_width + x];
            for (std::size_t k = 0; k < 4; ++k) {
                index.buffer_slots[k] = static_cast<std::uint32_t>((k + x / 16 + y / 16) % 4);
                index.element_indices[k] = static_cast<std::uint32_t>(((y * bindless_width + x) * (k * 2 + 1) + k * 13) % 256);
            }
            index.texture_slots = {{static_cast<std::uint32_t>((x / 32 + y / 32) % 2),
                                    static_cast<std::uint32_t>(1 - (x / 32 + y / 32) % 2)}};
        }
    return result;
}
Image bindless_reference(const BindlessInputs &input) {
    if (input.filter != Filter::Nearest && input.filter != Filter::Bilinear)
        throw std::invalid_argument("bindless filter");
    if (input.indices.size() != bindless_width * bindless_height)
        throw std::invalid_argument("bindless selector extent");
    for (const auto weight : input.weights)
        if (!std::isfinite(weight) || weight < 0 || weight > 1)
            throw std::invalid_argument("bindless scalar weight");
    for (const auto &buffer : input.buffers)
        if (buffer.empty() || buffer.size() > maximum_elements)
            throw std::invalid_argument("bindless buffer extent");
    for (const auto &texture : input.textures) valid_image(texture);
    auto result = make_image(bindless_width, bindless_height, clear_pixel);
    for (std::size_t y = 0; y < bindless_height; ++y)
        for (std::size_t x = 0; x < bindless_width; ++x) {
            const auto &index = input.indices[y * bindless_width + x];
            std::array<double, 4> sum{};
            for (std::size_t k = 0; k < 4; ++k) {
                if (index.buffer_slots[k] >= 4) throw std::out_of_range("bindless buffer index");
                const auto value = codes(input.buffers[index.buffer_slots[k]].at(index.element_indices[k]));
                for (std::size_t c = 0; c < 4; ++c) sum[c] += value[c] * input.weights[k];
            }
            const auto u = (double(x) + .5) / bindless_width;
            const auto v = (double(y) + .5) / bindless_height;
            for (std::size_t k = 0; k < 2; ++k) {
                if (index.texture_slots[k] >= 2) throw std::out_of_range("bindless texture index");
                const auto value = sampled_codes(input.textures[index.texture_slots[k]], k ? 1 - u : u, v, input.filter);
                for (std::size_t c = 0; c < 4; ++c) sum[c] += value[c] * input.weights[k + 4];
            }
            result.at(x, y) = {channel(sum[0]), channel(sum[1]), channel(sum[2]), channel(sum[3])};
        }
    return result;
}
Comparison compare_bindless(const Image &expected, const Image &actual, Filter filter) {
    if (filter != Filter::Nearest && filter != Filter::Bilinear)
        throw std::invalid_argument("bindless comparison filter");
    return compare_images(expected, actual, filter == Filter::Nearest ? 0 : 1);
}

IndirectArguments fill_indirect_args(std::uint32_t instances) {
    valid_instances(2, instances);
    return {{6, instances, 6, 2}, {6, instances, 3, -4, 1}, {3, 2, 1}};
}
std::vector<std::uint8_t> indirect_argument_bytes(const IndirectArguments &args) {
    std::vector<std::uint8_t> bytes;
    for (const auto value : {args.draw.vertex_count, args.draw.instance_count, args.draw.vertex_start,
         args.draw.base_instance, args.indexed.index_count, args.indexed.instance_count,
         args.indexed.index_start, static_cast<std::uint32_t>(args.indexed.base_vertex),
         args.indexed.base_instance, args.dispatch.x, args.dispatch.y, args.dispatch.z})
        append32(bytes, value);
    return bytes;
}
std::vector<IntVertex> indirect_vertices(bool indexed) {
    if (indexed) return {{-99, -99}, {-99, -99}, {-99, -99}, {-99, -99},
                         {1, 1}, {5, 1}, {5, 4}, {1, 4}};
    return {{-99, -99}, {-99, -99}, {-99, -99}, {-99, -99}, {-99, -99}, {-99, -99},
            {1, 1}, {5, 1}, {5, 4}, {1, 1}, {5, 4}, {1, 4}};
}
std::vector<std::uint32_t> indirect_indices() { return {99, 99, 99, 8, 9, 10, 8, 10, 11}; }
Image draw_indirect_reference(const Image &initial, const std::vector<IntVertex> &vertices, DrawArguments args) {
    valid_image(initial);
    auto result = initial;
    if (!args.instance_count || !args.vertex_count) return result;
    valid_instances(args.base_instance, args.instance_count);
    range(vertices.size(), args.vertex_start, args.vertex_count);
    if (args.vertex_count % 3) throw std::invalid_argument("triangle vertex count");
    for (std::uint32_t instance = 0; instance < args.instance_count; ++instance)
        for (std::size_t i = 0; i < args.vertex_count; i += 3) {
            const auto first = std::size_t(args.vertex_start) + i;
            raster_triangle(result, {{vertices[first], vertices[first + 1], vertices[first + 2]}},
                            args.base_instance + instance, false);
        }
    return result;
}
Image draw_indexed_indirect_reference(const Image &initial, const std::vector<IntVertex> &vertices,
    const std::vector<std::uint32_t> &indices, DrawIndexedArguments args) {
    valid_image(initial);
    auto result = initial;
    if (!args.instance_count || !args.index_count) return result;
    valid_instances(args.base_instance, args.instance_count);
    range(indices.size(), args.index_start, args.index_count);
    if (args.index_count % 3) throw std::invalid_argument("triangle index count");
    for (std::uint32_t instance = 0; instance < args.instance_count; ++instance)
        for (std::size_t i = 0; i < args.index_count; i += 3) {
            std::array<IntVertex, 3> triangle{};
            for (std::size_t k = 0; k < 3; ++k) {
                const auto index = std::int64_t(indices[std::size_t(args.index_start) + i + k]) + args.base_vertex;
                if (index < 0 || std::uint64_t(index) >= vertices.size())
                    throw std::out_of_range("base vertex adjusted index");
                triangle[k] = vertices[static_cast<std::size_t>(index)];
            }
            raster_triangle(result, triangle, args.base_instance + instance, true);
        }
    return result;
}
std::uint32_t indirect_dispatch_value(std::size_t x, std::size_t y, std::size_t z) {
    return 0x10000000u + (static_cast<std::uint32_t>(x) + 1) * 17u +
           (static_cast<std::uint32_t>(y) + 1) * 257u + (static_cast<std::uint32_t>(z) + 1) * 65537u;
}
std::vector<std::uint32_t> dispatch_storage(std::array<std::uint32_t, 3> extent) {
    return std::vector<std::uint32_t>(volume(extent) + guard_words * 2, grid_guard);
}
std::vector<std::uint32_t> dispatch_indirect_reference(const std::vector<std::uint32_t> &initial,
    std::array<std::uint32_t, 3> extent, DispatchArguments args,
    std::array<std::uint32_t, 3> threads_per_group) {
    if (initial.size() != volume(extent) + guard_words * 2)
        throw std::invalid_argument("dispatch storage extent");
    const std::array<std::uint32_t, 3> groups{{args.x, args.y, args.z}};
    std::array<std::size_t, 3> grid{};
    for (std::size_t k = 0; k < 3; ++k) {
        if (!threads_per_group[k]) throw std::invalid_argument("zero threadgroup dimension");
        const auto dimension = std::uint64_t(groups[k]) * threads_per_group[k];
        if (dimension > extent[k]) throw std::out_of_range("indirect dispatch outside target");
        grid[k] = static_cast<std::size_t>(dimension);
    }
    auto result = initial;
    for (std::size_t z = 0; z < grid[2]; ++z)
        for (std::size_t y = 0; y < grid[1]; ++y)
            for (std::size_t x = 0; x < grid[0]; ++x)
                result[guard_words + (z * extent[1] + y) * extent[0] + x] = indirect_dispatch_value(x, y, z);
    return result;
}
IndirectReference indirect_reference(std::uint32_t instances) {
    const auto args = fill_indirect_args(instances);
    const auto initial = make_image(64, 32, clear_pixel);
    const std::array<std::uint32_t, 3> extent{{12, 6, 1}};
    return {indirect_argument_bytes(args), draw_indirect_reference(initial, indirect_vertices(false), args.draw),
            draw_indexed_indirect_reference(initial, indirect_vertices(true), indirect_indices(), args.indexed),
            dispatch_indirect_reference(dispatch_storage(extent), extent, args.dispatch)};
}
Comparison compare_indirect(const IndirectReference &expected, const IndirectReference &actual) {
    auto result = compare_bytes(expected.argument_bytes, actual.argument_bytes);
    result.merge(compare_images(expected.draw, actual.draw, 0));
    result.merge(compare_images(expected.indexed, actual.indexed, 0));
    result.merge(compare_words(expected.dispatch, actual.dispatch));
    return result;
}

IcbProgram icb_program() {
    IcbProgram program{};
    for (std::size_t i = 0; i < icb_slots; ++i)
        program[i] = {i % 3 != 1, static_cast<std::uint32_t>(i % 3 + 1), 0, 1,
                      {i % 4 * 32 + 2, i / 4 * 16 + 2, 3, 4},
                      {static_cast<std::uint8_t>(31 + i * 3), static_cast<std::uint8_t>(71 + i * 5),
                       static_cast<std::uint8_t>(41 + i * 7), 255}};
    return program;
}
void reset_range(IcbProgram &program, std::size_t first, std::size_t count) {
    range(icb_slots, first, count);
    for (std::size_t i = first; i < first + count; ++i) program[i] = {};
}
void record_icb_slot(IcbProgram &program, std::size_t slot, IcbCommand command) {
    range(icb_slots, slot, 1);
    valid_icb(command);
    program[slot] = command;
}
IcbReplay replay_icb_sequence(const IcbProgram &program, const std::vector<std::uint32_t> &slots) {
    if (slots.size() > 512) throw std::invalid_argument("ICB fixture sequence length");
    IcbReplay result;
    result.image = make_image(128, 64, clear_pixel);
    result.pixel_execution_count.resize(result.image.pixels.size());
    for (const auto slot : slots) {
        if (slot >= icb_slots) throw std::out_of_range("ICB slot");
        const auto &command = program[slot];
        valid_icb(command);
        if (!command.enabled || !command.instance_count) continue;
        ++result.slot_execution_count[slot];
        result.executed_slots.push_back(slot);
        for (std::uint32_t local = 0; local < command.instance_count; ++local) {
            const auto instance = command.base_instance + local;
            const auto &r = command.rectangle;
            const auto left = r.x + instance * 5;
            auto color = command.color;
            color.r = static_cast<std::uint8_t>(std::uint64_t(color.r) + command.generation * 37ull + instance * 7ull);
            for (std::size_t y = r.y; y < r.y + r.height; ++y)
                for (std::size_t x = left; x < left + r.width; ++x) {
                    result.image.at(x, y) = color;
                    ++result.pixel_execution_count[y * result.image.width + x];
                }
        }
    }
    return result;
}
IcbReplay replay_icb(const IcbProgram &program, std::size_t first, std::size_t end) {
    if (end < first) throw std::out_of_range("reversed ICB range");
    range(icb_slots, first, end - first);
    std::vector<std::uint32_t> sequence;
    for (std::size_t i = first; i < end; ++i) sequence.push_back(static_cast<std::uint32_t>(i));
    return replay_icb_sequence(program, sequence);
}
Comparison compare_icb(const IcbReplay &expected, const IcbReplay &actual) {
    auto result = compare_images(expected.image, actual.image, 0);
    result.merge(compare_words(expected.pixel_execution_count, actual.pixel_execution_count));
    result.merge(compare_words({expected.slot_execution_count.begin(), expected.slot_execution_count.end()},
                               {actual.slot_execution_count.begin(), actual.slot_execution_count.end()}));
    result.merge(compare_words(expected.executed_slots, actual.executed_slots));
    return result;
}

LifecycleError end_encoding(EncoderModel &encoder) noexcept {
    if (encoder.ended) return LifecycleError::EncoderEnded;
    encoder.ended = true;
    return LifecycleError::None;
}
LifecycleError lifecycle_error(EncoderModel encoder, const LifecycleResource *resource,
                               std::size_t offset) noexcept {
    if (encoder.ended) return LifecycleError::EncoderEnded;
    if (!resource) return LifecycleError::NullIndirectBuffer;
    if (resource->device != encoder.device) return LifecycleError::CrossDevice;
    if (offset % 4) return LifecycleError::UnalignedOffset;
    if (offset > resource->bytes || sizeof(DispatchArguments) > resource->bytes - offset)
        return LifecycleError::IndirectOutOfBounds;
    if (resource->purgeable == PurgeableState::Empty) return LifecycleError::EmptyHeap;
    return LifecycleError::None;
}
LifecycleOutcome lifecycle_reference(EncoderModel encoder, const LifecycleResource *resource,
    std::size_t offset, const Image &initial) {
    valid_image(initial);
    LifecycleOutcome result{lifecycle_error(encoder, resource, offset), initial, 0};
    if (result.error == LifecycleError::None) {
        result.target.pixels[0] = {61, 83, 107, 255};
        result.writes = 1;
    }
    return result;
}
Comparison compare_lifecycle(LifecycleError expected_error, const Image &initial,
                             const LifecycleOutcome &actual) {
    valid_image(initial);
    auto target = initial;
    if (expected_error == LifecycleError::None) target.pixels[0] = {61, 83, 107, 255};
    auto result = compare_images(target, actual.target, 0);
    result.merge(compare_words({static_cast<std::uint32_t>(expected_error)},
                               {static_cast<std::uint32_t>(actual.error)}));
    result.merge(compare_words({expected_error == LifecycleError::None ? 1u : 0u}, {actual.writes}));
    return result;
}
const char *lifecycle_error_name(LifecycleError error) noexcept {
    switch (error) {
        case LifecycleError::None: return "NONE";
        case LifecycleError::EncoderEnded: return "ENCODER_ENDED";
        case LifecycleError::CrossDevice: return "CROSS_DEVICE";
        case LifecycleError::NullIndirectBuffer: return "NULL_INDIRECT_BUFFER";
        case LifecycleError::UnalignedOffset: return "UNALIGNED_OFFSET";
        case LifecycleError::IndirectOutOfBounds: return "INDIRECT_OUT_OF_BOUNDS";
        case LifecycleError::EmptyHeap: return "EMPTY_HEAP";
    }
    return "UNKNOWN";
}

} // namespace nezha_gpu_test::phase4
