/* SPDX-License-Identifier: MIT */
#include "cpu_reference_phase4.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>

using namespace nezha_gpu_test;
namespace p4 = nezha_gpu_test::phase4;
namespace {
std::size_t cases_run = 0, checks = 0;
std::string current_case;
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) throw std::runtime_error(#__VA_ARGS__); } while (false)

void run(const char *name, const std::function<void()> &action) {
    current_case = name;
    action();
    ++cases_run;
}
template<typename F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::exception &) { rejected = true; }
    CHECK(rejected);
}
void exact(const Image &expected, const Image &actual) {
    const auto result = compare_images(expected, actual, 0);
    CHECK(result.passed()); CHECK(result.max_err == 0);
    CHECK(result.compared == expected.pixels.size());
}
Image target() { return make_image(64, 32, p4::clear_pixel); }
std::vector<std::uint32_t> all_slots() {
    std::vector<std::uint32_t> slots(16);
    std::iota(slots.begin(), slots.end(), 0u);
    return slots;
}
void lifecycle_fault(p4::LifecycleError error, p4::EncoderModel encoder,
                     const p4::LifecycleResource *resource, std::size_t offset) {
    const auto initial = target();
    const auto result = p4::lifecycle_reference(encoder, resource, offset, initial);
    CHECK(result.error == error); CHECK(result.writes == 0);
    exact(initial, result.target);
    CHECK(p4::compare_lifecycle(error, initial, result).passed());
}

void heap_tests() {
    run("heap_default_extent_and_exact_readbacks", [] {
        const auto ref = p4::heap_alias_reference();
        CHECK(ref.linear_buffer.size() == 64 * 32 * 8);
        CHECK(ref.bgra.width == 64 && ref.bgra.height == 32);
        CHECK(ref.rgba16f.pixels.size() == 64 * 32);
        CHECK(p4::compare_heap_alias(ref, ref).passed());
    });
    run("heap_single_pixel_known_bgra_and_fp16_codes", [] {
        const auto ref = p4::heap_alias_reference(1, 1);
        CHECK(ref.bgra.at(0, 0) == Bgra8({41, 67, 113, 255}));
        const auto p = ref.rgba16f.pixels[0];
        CHECK(p.r == 0 && p.g == 0 && p.b == 0 && p.a == 0x3c00);
    });
    run("heap_linear_bytes_independent_formula", [] {
        const auto ref = p4::heap_alias_reference(3, 2);
        CHECK(ref.linear_buffer[0] == 19 && ref.linear_buffer[1] == 56);
        CHECK(ref.linear_buffer[7] == 22 && ref.linear_buffer[47] == 222);
    });
    run("heap_two_allocation_guards_remain_intact", [] {
        const auto ref = p4::heap_alias_reference(17, 9);
        CHECK(ref.guards.size() == 64);
        CHECK(std::all_of(ref.guards.begin(), ref.guards.end(), [](auto b) { return b == 0xa7; }));
    });
    run("heap_bgra_all_pixels_independent_integer_pattern", [] {
        const auto ref = p4::heap_alias_reference(17, 9);
        for (std::size_t y = 0; y < 9; ++y)
            for (std::size_t x = 0; x < 17; ++x)
                CHECK(ref.bgra.at(x, y) == Bgra8({static_cast<std::uint8_t>(x * 17 + y * 3 + 41),
                    static_cast<std::uint8_t>(x * 5 + y * 29 + 67),
                    static_cast<std::uint8_t>(x * 11 + y * 7 + 113), 255}));
    });
    run("heap_fp16_dyadic_values_known_codes", [] {
        const auto p = p4::heap_half_pixel(1, 15);
        CHECK(p.r == 0x2c00 && p.g == 0x3b80 && p.b == 0x3b00 && p.a == 0x3c00);
    });
    run("heap_stale_ubwc_one_pixel_is_failure", [] {
        const auto ref = p4::heap_alias_reference(17, 9); auto stale = ref;
        stale.bgra.at(16, 4) = ref.bgra.at(15, 4);
        const auto result = p4::compare_heap_alias(ref, stale);
        CHECK(!result.passed()); CHECK(result.bgra.mismatched_px == 1);
        CHECK(result.buffer.passed() && result.rgba16f.passed());
    });
    run("heap_old_linear_alias_residue_is_failure", [] {
        const auto ref = p4::heap_alias_reference(8, 4); auto stale = ref;
        stale.bgra.at(0, 0) = {ref.linear_buffer[0], ref.linear_buffer[1],
                               ref.linear_buffer[2], ref.linear_buffer[3]};
        CHECK(p4::compare_heap_alias(ref, stale).bgra.mismatched_px == 1);
    });
    run("heap_old_bgra_alias_in_half_output_is_failure", [] {
        const auto ref = p4::heap_alias_reference(8, 4); auto stale = ref;
        const auto b = ref.bgra.at(2, 1);
        stale.rgba16f.pixels[10].r = static_cast<std::uint16_t>(unsigned(b.b) | (unsigned(b.g) << 8));
        CHECK(p4::compare_heap_alias(ref, stale).rgba16f.mismatched_px == 1);
    });
    run("heap_bgra_one_lsb_never_tolerated", [] {
        const auto ref = p4::heap_alias_reference(8, 4); auto bad = ref;
        bad.bgra.at(7, 3).a ^= 1;
        const auto result = p4::compare_heap_alias(ref, bad);
        CHECK(!result.passed() && result.bgra.max_err == 1);
    });
    run("heap_half_one_bit_never_tolerated", [] {
        const auto ref = p4::heap_alias_reference(8, 4); auto bad = ref;
        bad.rgba16f.pixels[13].b ^= 1;
        const auto result = p4::compare_heap_alias(ref, bad);
        CHECK(!result.passed() && result.rgba16f.max_err == 1);
        CHECK(result.rgba16f.mismatched_px == 1);
    });
    run("heap_guard_write_is_failure", [] {
        const auto ref = p4::heap_alias_reference(8, 4); auto bad = ref;
        bad.guards.back() ^= 1;
        CHECK(!p4::compare_heap_alias(ref, bad).passed());
        CHECK(p4::compare_heap_alias(ref, bad).guards.mismatched_px == 1);
    });
    run("heap_wrong_shape_is_failure", [] {
        CHECK(!p4::compare_heap_alias(p4::heap_alias_reference(8, 4),
                                      p4::heap_alias_reference(4, 8)).passed());
    });
    run("heap_zero_extent_is_controlled_error", [] { rejects([] { (void)p4::heap_alias_reference(0, 8); }); });
    run("heap_overflow_extent_is_controlled_error", [] {
        rejects([] { (void)p4::heap_alias_reference(std::numeric_limits<std::size_t>::max(), 2); });
    });
}

void bindless_tests() {
    run("bindless_128_square_all_six_resources_indexed", [] {
        const auto input = p4::bindless_inputs();
        std::array<bool, 4> buffers{}; std::array<bool, 2> textures{};
        for (const auto &i : input.indices) {
            for (auto b : i.buffer_slots) buffers.at(b) = true;
            for (auto t : i.texture_slots) textures.at(t) = true;
        }
        CHECK(std::all_of(buffers.begin(), buffers.end(), [](auto b) { return b; }));
        CHECK(std::all_of(textures.begin(), textures.end(), [](auto b) { return b; }));
        const auto image = p4::bindless_reference(input);
        CHECK(image.width == 128 && image.height == 128 && image.pixels.size() == 16384);
    });
    run("bindless_first_pixel_independent_golden", [] {
        CHECK(p4::bindless_reference(p4::bindless_inputs()).at(0, 0) == Bgra8({87, 91, 107, 255}));
    });
    run("bindless_four_buffers_and_two_textures_dyadic_tie_rounding", [] {
        auto input = p4::bindless_inputs();
        for (std::size_t i = 0; i < 4; ++i)
            std::fill(input.buffers[i].begin(), input.buffers[i].end(), 0xff000000u + std::uint32_t(i + 1) * 10 * 0x010101u);
        input.textures[0] = make_image(1, 1, {50, 50, 50, 255});
        input.textures[1] = make_image(1, 1, {70, 70, 70, 255});
        const auto actual = p4::bindless_reference(input);
        exact(make_image(128, 128, {43, 43, 43, 255}), actual);
    });
    run("bindless_zero_scalar_weights_clear_every_channel", [] {
        auto input = p4::bindless_inputs(); input.weights.fill(0);
        exact(make_image(128, 128, {0, 0, 0, 0}), p4::bindless_reference(input));
    });
    run("bindless_buffer_selector_and_element_are_indirect", [] {
        auto input = p4::bindless_inputs(); input.weights = {{1, 0, 0, 0, 0, 0}};
        input.indices[0].buffer_slots[0] = 3; input.indices[0].element_indices[0] = 9;
        CHECK(p4::bindless_reference(input).at(0, 0) == Bgra8({161, 253, 145, 255}));
    });
    run("bindless_texture_selector_is_indirect", [] {
        auto input = p4::bindless_inputs(); input.weights = {{0, 0, 0, 0, 1, 0}};
        input.indices[0].texture_slots[0] = 1;
        CHECK(p4::bindless_reference(input).at(0, 0) == Bgra8({71, 41, 23, 255}));
    });
    run("bindless_second_texture_uses_mirrored_u", [] {
        auto input = p4::bindless_inputs(); input.weights = {{0, 0, 0, 0, 0, 1}};
        CHECK(p4::bindless_reference(input).at(0, 0) == Bgra8({101, 151, 185, 255}));
    });
    run("bindless_nearest_requires_zero_lsb", [] {
        const auto image = p4::bindless_reference(p4::bindless_inputs()); auto bad = image;
        bad.at(12, 19).r ^= 1;
        const auto result = p4::compare_bindless(image, bad, Filter::Nearest);
        CHECK(!result.passed() && result.max_err == 1 && result.mismatched_px == 1);
    });
    run("bindless_linear_allows_one_lsb", [] {
        const auto image = p4::bindless_reference(p4::bindless_inputs(Filter::Bilinear)); auto actual = image;
        actual.at(12, 19).b ^= 1;
        const auto result = p4::compare_bindless(image, actual, Filter::Bilinear);
        CHECK(result.passed() && result.max_err == 1);
    });
    run("bindless_linear_two_lsb_is_failure", [] {
        const auto image = p4::bindless_reference(p4::bindless_inputs(Filter::Bilinear)); auto bad = image;
        bad.at(12, 19).b ^= 2;
        CHECK(!p4::compare_bindless(image, bad, Filter::Bilinear).passed());
    });
    run("bindless_buffer_index_out_of_range_is_error", [] {
        auto input = p4::bindless_inputs(); input.indices[0].buffer_slots[2] = 4;
        rejects([&] { (void)p4::bindless_reference(input); });
    });
    run("bindless_buffer_element_out_of_range_is_error", [] {
        auto input = p4::bindless_inputs(); input.indices[0].element_indices[1] = 256;
        rejects([&] { (void)p4::bindless_reference(input); });
    });
    run("bindless_texture_index_out_of_range_is_error", [] {
        auto input = p4::bindless_inputs(); input.indices[0].texture_slots[0] = 2;
        rejects([&] { (void)p4::bindless_reference(input); });
    });
    run("bindless_empty_resource_is_error", [] {
        auto input = p4::bindless_inputs(); input.buffers[2].clear();
        rejects([&] { (void)p4::bindless_reference(input); });
    });
    run("bindless_nonfinite_weight_is_error", [] {
        auto input = p4::bindless_inputs(); input.weights[4] = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { (void)p4::bindless_reference(input); });
    });
    run("bindless_negative_weight_is_error", [] {
        auto input = p4::bindless_inputs(); input.weights[4] = -.125f;
        rejects([&] { (void)p4::bindless_reference(input); });
    });
    run("bindless_selector_shape_is_checked", [] {
        auto input = p4::bindless_inputs(); input.indices.pop_back();
        rejects([&] { (void)p4::bindless_reference(input); });
    });
    run("bindless_texture_storage_shape_is_checked", [] {
        auto input = p4::bindless_inputs(); input.textures[1].pixels.pop_back();
        rejects([&] { (void)p4::bindless_reference(input); });
    });
}

void indirect_tests() {
    run("indirect_apple_argument_wire_sizes", [] {
        CHECK(sizeof(p4::DrawArguments) == 16 && sizeof(p4::DrawIndexedArguments) == 20);
        CHECK(sizeof(p4::DispatchArguments) == 12 && sizeof(p4::IndirectArguments) == 48);
    });
    run("indirect_compute_producer_exact_little_endian_wire", [] {
        const auto wire = p4::indirect_argument_bytes(p4::fill_indirect_args());
        CHECK(wire.size() == 48 && wire[0] == 6 && wire[4] == 3 && wire[8] == 6 && wire[12] == 2);
        CHECK(wire[24] == 3 && wire[28] == 252 && wire[29] == 255 && wire[31] == 255);
        CHECK(wire[32] == 1 && wire[36] == 3 && wire[40] == 2 && wire[44] == 1);
    });
    run("indirect_draw_vertex_start_skips_poison_prefix", [] {
        const auto ref = p4::indirect_reference();
        CHECK(ref.draw.at(1, 1) == p4::clear_pixel);
        CHECK(ref.draw.at(11, 1) == Bgra8({19, 33, 201, 255}));
        CHECK(ref.draw.at(14, 3) == Bgra8({19, 33, 201, 255}));
        CHECK(ref.draw.at(15, 3) == p4::clear_pixel);
    });
    run("indirect_draw_base_instance_and_three_instances", [] {
        const auto image = p4::indirect_reference().draw;
        CHECK(image.at(16, 1) == Bgra8({22, 38, 201, 255}));
        CHECK(image.at(21, 1) == Bgra8({25, 43, 201, 255}));
        CHECK(std::count_if(image.pixels.begin(), image.pixels.end(), [](auto p) { return !(p == p4::clear_pixel); }) == 36);
    });
    run("indirect_index_start_skips_poison_indices", [] {
        const auto ref = p4::indirect_reference();
        CHECK(ref.indexed.at(38, 17) == Bgra8({16, 28, 77, 255}));
        CHECK(ref.indexed.at(41, 19) == Bgra8({16, 28, 77, 255}));
        CHECK(ref.indexed.at(42, 19) == p4::clear_pixel);
    });
    run("indirect_negative_base_vertex_and_instance_start", [] {
        const auto ref = p4::indirect_reference();
        CHECK(ref.indexed.at(43, 17) == Bgra8({19, 33, 77, 255}));
        CHECK(ref.indexed.at(48, 17) == Bgra8({22, 38, 77, 255}));
        CHECK(ref.indexed.at(37, 17) == p4::clear_pixel);
    });
    run("indirect_zero_instances_leave_both_targets_pristine", [] {
        const auto ref = p4::indirect_reference(0);
        exact(target(), ref.draw); exact(target(), ref.indexed);
    });
    run("indirect_zero_vertices_never_dereference_empty_geometry", [] {
        exact(target(), p4::draw_indirect_reference(target(), {}, {0, 3, 0xffffffffu, 0}));
    });
    run("indirect_zero_instances_never_dereference_empty_indices", [] {
        exact(target(), p4::draw_indexed_indirect_reference(target(), {}, {}, {6, 0, 0xffffffffu, -99, 0}));
    });
    run("indirect_dispatch_known_values_and_both_guards", [] {
        const auto ref = p4::indirect_reference();
        CHECK(ref.dispatch.size() == 88);
        CHECK(ref.dispatch[8] == 0x10010113u && ref.dispatch[8 + 71] == 0x100106d3u);
        CHECK(std::all_of(ref.dispatch.begin(), ref.dispatch.begin() + 8, [](auto w) { return w == grid_guard; }));
        CHECK(std::all_of(ref.dispatch.end() - 8, ref.dispatch.end(), [](auto w) { return w == grid_guard; }));
    });
    run("indirect_zero_dispatch_leaves_target_and_guards_pristine", [] {
        const std::array<std::uint32_t, 3> extent{{12, 6, 1}};
        const auto initial = p4::dispatch_storage(extent);
        CHECK(p4::dispatch_indirect_reference(initial, extent, {0, 2, 1}) == initial);
    });
    run("indirect_three_dimensional_dispatch_linearization", [] {
        const std::array<std::uint32_t, 3> extent{{2, 2, 2}};
        const auto output = p4::dispatch_indirect_reference(p4::dispatch_storage(extent), extent, {2, 2, 2}, {{1, 1, 1}});
        CHECK(output[8] == 0x10010113u && output[15] == 0x10020226u);
    });
    run("indirect_one_pixel_misalignment_is_failure", [] {
        const auto ref = p4::indirect_reference(); auto bad = ref;
        bad.draw.at(15, 1) = ref.draw.at(14, 1);
        CHECK(!p4::compare_indirect(ref, bad).passed());
    });
    run("indirect_one_compute_word_misalignment_is_failure", [] {
        const auto ref = p4::indirect_reference(); auto bad = ref;
        std::swap(bad.dispatch[8], bad.dispatch[9]);
        CHECK(p4::compare_indirect(ref, bad).mismatched_px == 2);
    });
    run("indirect_argument_residue_is_failure", [] {
        const auto ref = p4::indirect_reference(); auto bad = ref;
        bad.argument_bytes[4] = 2;
        CHECK(!p4::compare_indirect(ref, bad).passed());
    });
    run("indirect_vertex_range_error_is_controlled", [] {
        rejects([] { (void)p4::draw_indirect_reference(target(), p4::indirect_vertices(false), {6, 1, 10, 0}); });
    });
    run("indirect_base_vertex_underflow_is_controlled", [] {
        auto args = p4::fill_indirect_args().indexed; args.base_vertex = -100;
        rejects([&] { (void)p4::draw_indexed_indirect_reference(target(), p4::indirect_vertices(true), p4::indirect_indices(), args); });
    });
    run("indirect_dispatch_cannot_write_outside_capacity", [] {
        const std::array<std::uint32_t, 3> extent{{12, 6, 1}};
        rejects([&] { (void)p4::dispatch_indirect_reference(p4::dispatch_storage(extent), extent, {4, 2, 1}); });
    });
    run("indirect_zero_threadgroup_dimension_is_controlled", [] {
        const std::array<std::uint32_t, 3> extent{{12, 6, 1}};
        rejects([&] { (void)p4::dispatch_indirect_reference(p4::dispatch_storage(extent), extent, {3, 2, 1}, {{0, 3, 1}}); });
    });
}

void icb_tests() {
    run("icb_sixteen_slots_mixed_enabled_and_empty", [] {
        const auto ref = p4::replay_icb(p4::icb_program());
        CHECK(ref.executed_slots == std::vector<std::uint32_t>({0, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15}));
        CHECK(std::accumulate(ref.pixel_execution_count.begin(), ref.pixel_execution_count.end(), 0u) == 252);
        CHECK(ref.image.at(2, 2) == Bgra8({31, 71, 78, 255}));
    });
    run("icb_enabled_zero_instance_slot_is_empty", [] {
        auto program = p4::icb_program(); program[2].instance_count = 0;
        const auto ref = p4::replay_icb(program);
        CHECK(ref.slot_execution_count[2] == 0 && ref.image.at(66, 2) == p4::clear_pixel);
    });
    run("icb_all_empty_slots_preserve_target", [] {
        const auto ref = p4::replay_icb(p4::IcbProgram{});
        exact(make_image(128, 64, p4::clear_pixel), ref.image);
        CHECK(ref.executed_slots.empty());
    });
    run("icb_multinstance_subrange_four_through_eleven", [] {
        const auto ref = p4::replay_icb(p4::icb_program(), 4, 12);
        CHECK(ref.executed_slots == std::vector<std::uint32_t>({5, 6, 8, 9, 11}));
        CHECK(ref.slot_execution_count[3] == 0 && ref.slot_execution_count[12] == 0);
        CHECK(ref.image.at(12, 50) == p4::clear_pixel);
    });
    run("icb_instance_subdraw_distinct_color_and_offset", [] {
        const auto ref = p4::replay_icb(p4::icb_program(), 2, 3);
        CHECK(ref.image.at(66, 2) == Bgra8({37, 81, 92, 255}));
        CHECK(ref.image.at(71, 2) == Bgra8({37, 81, 99, 255}));
        CHECK(ref.image.at(76, 2) == Bgra8({37, 81, 106, 255}));
    });
    run("icb_reset_range_clears_exactly_four_slots", [] {
        auto program = p4::icb_program(); p4::reset_range(program, 4, 4);
        const auto ref = p4::replay_icb(program);
        CHECK(ref.slot_execution_count[3] == 1 && ref.slot_execution_count[8] == 1);
        for (std::size_t i = 4; i < 8; ++i) CHECK(ref.slot_execution_count[i] == 0);
    });
    run("icb_reset_and_rerecord_new_generation", [] {
        auto program = p4::icb_program(); auto command = program[5];
        p4::reset_range(program, 4, 4); command.generation = 2; command.instance_count = 2;
        p4::record_icb_slot(program, 5, command);
        const auto ref = p4::replay_icb(program, 4, 12);
        CHECK(ref.slot_execution_count[5] == 1 && ref.slot_execution_count[6] == 0);
        CHECK(ref.image.at(34, 18) == Bgra8({46, 96, 150, 255}));
    });
    run("icb_old_rerecorded_generation_is_failure", [] {
        auto program = p4::icb_program(); const auto old = p4::replay_icb(program, 4, 12);
        program[5].generation = 2;
        CHECK(!p4::compare_icb(p4::replay_icb(program, 4, 12), old).passed());
    });
    run("icb_missed_execution_is_failure", [] {
        auto program = p4::icb_program(); const auto ref = p4::replay_icb(program);
        program[5].enabled = false;
        CHECK(!p4::compare_icb(ref, p4::replay_icb(program)).passed());
    });
    run("icb_duplicate_execution_detected_even_when_image_identical", [] {
        const auto program = p4::icb_program(); const auto ref = p4::replay_icb(program);
        auto slots = all_slots(); slots.push_back(5);
        const auto duplicate = p4::replay_icb_sequence(program, slots);
        exact(ref.image, duplicate.image);
        CHECK(duplicate.slot_execution_count[5] == 2);
        CHECK(!p4::compare_icb(ref, duplicate).passed());
    });
    run("icb_execution_outside_selected_subrange_is_failure", [] {
        const auto program = p4::icb_program();
        const auto outside = p4::replay_icb_sequence(program, {4, 5, 6, 7, 8, 9, 10, 11, 12});
        CHECK(!p4::compare_icb(p4::replay_icb(program, 4, 12), outside).passed());
    });
    run("icb_slot_sixteen_is_controlled_error", [] {
        rejects([] { (void)p4::replay_icb_sequence(p4::icb_program(), {16}); });
    });
    run("icb_command_trace_detects_reordered_replay", [] {
        const auto program = p4::icb_program(); auto slots = all_slots(); std::reverse(slots.begin(), slots.end());
        const auto reversed = p4::replay_icb_sequence(program, slots);
        exact(p4::replay_icb(program).image, reversed.image);
        CHECK(!p4::compare_icb(p4::replay_icb(program), reversed).passed());
    });
    run("icb_zero_length_subrange_has_no_effect", [] {
        CHECK(p4::replay_icb(p4::icb_program(), 16, 16).executed_slots.empty());
    });
    run("icb_reset_range_overflow_is_controlled_error", [] {
        auto program = p4::icb_program();
        rejects([&] { p4::reset_range(program, 15, std::numeric_limits<std::size_t>::max()); });
    });
    run("icb_base_instance_applies_to_pixel_position", [] {
        auto program = p4::icb_program(); program[0].base_instance = 2;
        const auto ref = p4::replay_icb(program, 0, 1);
        CHECK(ref.image.at(2, 2) == p4::clear_pixel && ref.image.at(12, 2) == Bgra8({31, 71, 92, 255}));
    });
}

void lifecycle_tests() {
    run("lifecycle_valid_encoding_has_one_controlled_write", [] {
        const p4::LifecycleResource resource{};
        const auto outcome = p4::lifecycle_reference({}, &resource, 0, target());
        CHECK(outcome.error == p4::LifecycleError::None && outcome.writes == 1);
        CHECK(outcome.target.at(0, 0) == Bgra8({61, 83, 107, 255}));
        CHECK(p4::compare_lifecycle(p4::LifecycleError::None, target(), outcome).passed());
    });
    run("lifecycle_repeated_end_encoding_is_controlled_error", [] {
        p4::EncoderModel encoder{};
        CHECK(p4::end_encoding(encoder) == p4::LifecycleError::None && encoder.ended);
        CHECK(p4::end_encoding(encoder) == p4::LifecycleError::EncoderEnded && encoder.ended);
    });
    run("lifecycle_encode_after_end_leaves_target_pristine", [] {
        const p4::LifecycleResource resource{};
        lifecycle_fault(p4::LifecycleError::EncoderEnded, {1, true}, &resource, 0);
    });
    run("lifecycle_cross_device_resource_leaves_target_pristine", [] {
        const p4::LifecycleResource resource{2, 64, p4::PurgeableState::NonVolatile};
        lifecycle_fault(p4::LifecycleError::CrossDevice, {}, &resource, 0);
    });
    run("lifecycle_null_indirect_buffer_cannot_crash", [] {
        lifecycle_fault(p4::LifecycleError::NullIndirectBuffer, {}, nullptr, 0);
    });
    run("lifecycle_unaligned_indirect_offset_cannot_write", [] {
        const p4::LifecycleResource resource{};
        lifecycle_fault(p4::LifecycleError::UnalignedOffset, {}, &resource, 2);
    });
    run("lifecycle_out_of_bounds_indirect_offset_cannot_write", [] {
        const p4::LifecycleResource resource{};
        lifecycle_fault(p4::LifecycleError::IndirectOutOfBounds, {}, &resource, 56);
    });
    run("lifecycle_empty_purgeable_heap_cannot_write", [] {
        const p4::LifecycleResource resource{1, 64, p4::PurgeableState::Empty};
        lifecycle_fault(p4::LifecycleError::EmptyHeap, {}, &resource, 0);
    });
    run("lifecycle_volatile_but_not_empty_heap_is_available", [] {
        const p4::LifecycleResource resource{1, 64, p4::PurgeableState::Volatile};
        CHECK(p4::lifecycle_reference({}, &resource, 0, target()).error == p4::LifecycleError::None);
    });
    run("lifecycle_ended_encoder_error_precedes_null_buffer", [] {
        lifecycle_fault(p4::LifecycleError::EncoderEnded, {1, true}, nullptr, 1);
    });
    run("lifecycle_last_aligned_twelve_byte_argument_is_valid", [] {
        const p4::LifecycleResource resource{};
        CHECK(p4::lifecycle_reference({}, &resource, 52, target()).error == p4::LifecycleError::None);
    });
    run("lifecycle_offset_size_overflow_is_controlled_error", [] {
        const p4::LifecycleResource resource{};
        lifecycle_fault(p4::LifecycleError::IndirectOutOfBounds, {}, &resource,
                        std::numeric_limits<std::size_t>::max() - 3);
    });
    run("lifecycle_one_dirty_pixel_after_error_is_failure", [] {
        auto bad = p4::lifecycle_reference({}, nullptr, 0, target()); bad.target.at(8, 3).r ^= 1;
        CHECK(!p4::compare_lifecycle(p4::LifecycleError::NullIndirectBuffer, target(), bad).passed());
    });
    run("lifecycle_wrong_controlled_error_is_failure", [] {
        auto bad = p4::lifecycle_reference({}, nullptr, 0, target()); bad.error = p4::LifecycleError::None;
        CHECK(!p4::compare_lifecycle(p4::LifecycleError::NullIndirectBuffer, target(), bad).passed());
    });
    run("lifecycle_false_write_count_is_failure", [] {
        auto bad = p4::lifecycle_reference({}, nullptr, 0, target()); bad.writes = 1;
        CHECK(!p4::compare_lifecycle(p4::LifecycleError::NullIndirectBuffer, target(), bad).passed());
    });
    run("lifecycle_invalid_target_storage_is_controlled_error", [] {
        auto bad = target(); bad.pixels.pop_back();
        rejects([&] { (void)p4::lifecycle_reference({}, nullptr, 0, bad); });
    });
}

std::vector<std::string> samples() {
    std::vector<std::string> lines;
    auto measure = [&](std::size_t index, const std::function<Comparison()> &action) {
        const auto begin = std::chrono::steady_clock::now();
        const auto result = action();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin).count();
        CHECK(result.passed());
        lines.push_back(hwtest_line(p4::cases[index], result, static_cast<std::uint64_t>(elapsed),
                                     "raw", "cpu_reference_only"));
    };
    measure(0, [] { const auto ref = p4::heap_alias_reference(); return p4::compare_heap_alias(ref, ref).summary(); });
    measure(1, [] {
        const auto ref = p4::bindless_reference(p4::bindless_inputs());
        return p4::compare_bindless(ref, ref, Filter::Nearest);
    });
    measure(2, [] { const auto ref = p4::indirect_reference(); return p4::compare_indirect(ref, ref); });
    measure(3, [] {
        auto program = p4::icb_program(); auto command = program[5]; command.generation = 2;
        p4::reset_range(program, 4, 4); p4::record_icb_slot(program, 5, command);
        const auto ref = p4::replay_icb(program, 4, 12);
        return p4::compare_icb(ref, ref);
    });
    measure(4, [] {
        const auto initial = target();
        const p4::LifecycleResource valid{}, foreign{2, 64, p4::PurgeableState::NonVolatile},
                                    empty{1, 64, p4::PurgeableState::Empty};
        Comparison result;
        result.merge(p4::compare_lifecycle(p4::LifecycleError::EncoderEnded, initial,
            p4::lifecycle_reference({1, true}, &valid, 0, initial)));
        result.merge(p4::compare_lifecycle(p4::LifecycleError::CrossDevice, initial,
            p4::lifecycle_reference({}, &foreign, 0, initial)));
        result.merge(p4::compare_lifecycle(p4::LifecycleError::NullIndirectBuffer, initial,
            p4::lifecycle_reference({}, nullptr, 0, initial)));
        result.merge(p4::compare_lifecycle(p4::LifecycleError::UnalignedOffset, initial,
            p4::lifecycle_reference({}, &valid, 2, initial)));
        result.merge(p4::compare_lifecycle(p4::LifecycleError::EmptyHeap, initial,
            p4::lifecycle_reference({}, &empty, 0, initial)));
        return result;
    });
    return lines;
}
void export_samples(const std::filesystem::path &path, const std::vector<std::string> &lines) {
    const auto resolved = std::filesystem::weakly_canonical(path).generic_string();
    const std::string root = "/home/hua/nezha-work/ios27/adreno840-native/.cache/w07-l4/";
    if (resolved.rfind(root, 0) != 0) throw std::invalid_argument("sample output outside Wave 7 Lane 4 cache");
    std::ofstream output(path);
    if (!output) throw std::runtime_error("cannot open sample output");
    for (const auto &line : lines) output << line << '\n';
    output.close();
    if (!output) throw std::runtime_error("cannot write complete sample output");
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 1 && (argc != 3 || std::string(argv[1]) != "--emit-phase4-hwtest"))
            throw std::invalid_argument("usage: test_cpu_reference_phase4 [--emit-phase4-hwtest CACHE_PATH]");
        heap_tests(); bindless_tests(); indirect_tests(); icb_tests(); lifecycle_tests();
        CHECK(cases_run >= 55);
        const auto lines = samples();
        if (argc == 3) export_samples(argv[2], lines);
        std::cout << "PASS: " << cases_run << " independent cases, " << checks
                  << " checks; five Phase 4 CPU references; CPU reference only\n";
        for (const auto &line : lines) std::cout << line << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << current_case << ": " << error.what() << '\n';
        return 1;
    }
}
