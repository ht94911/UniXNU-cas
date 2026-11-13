/* SPDX-License-Identifier: MIT */
#include "NezhaCameraNativeMemory.h"
#include "NativeAdrenoMemory.h"

#include <IOKit/IOLib.h>

namespace {
constexpr size_t Page = 4096;
constexpr size_t TablePages = 32;
constexpr size_t MaxFrame = 15 * 1024 * 1024; // 4096x3072 packed RAW10.
constexpr uint64_t FirstIova = 0x01000000;
constexpr uint64_t SecondIova = 0x02000000;
constexpr uint8_t Sentinel = 0xa5;
}

struct NezhaCameraNativeMemory {
    NativeAdrenoMemory frame[2], tables;
    NezhaCameraDmaPlan plan{};
    ArmSmmu4KConfig config{};
    size_t frame_bytes{};
    unsigned held{}, cpu{};
    bool ready{};
};

// One diagnostic capture per boot, like the existing late camera worker.
// Keep typed objects initialized without making ownership depend on a
// C++ allocation recipe or destructor during kernel shutdown.
static NezhaCameraNativeMemory memory_storage;
static bool memory_in_use;

bool nezha_camera_native_memory_prepare(NezhaCameraNativeMemory **out,
    unsigned physical_bits, uint32_t width, uint32_t height)
{
    if (!out || *out || memory_in_use || (physical_bits != 36 && physical_bits != 40)) {
        return false;
    }
    const NezhaCameraFrameMapping allowed[] = {{FirstIova, MaxFrame}, {SecondIova, MaxFrame}};
    NezhaCameraRdi1080 layout{};
    if (!nezha_camera_rdi1080_layout(width, height, allowed, &layout) ||
        layout.layout.frame_bytes > MaxFrame) {
        return false;
    }
    auto *memory = &memory_storage;
    memory_in_use = true;
    *out = memory;
    const size_t bytes = (size_t(layout.layout.frame_bytes) + Page - 1) & ~(Page - 1);
    memory->frame_bytes = bytes;
    if (memory->tables.allocate(TablePages * Page, physical_bits, true, TablePages * Page) != kIOReturnSuccess ||
        memory->tables.spanCount() != 1) {
        return false;
    }
    for (unsigned i = 0; i < 2; ++i) {
        auto &frame = memory->frame[i];
        if (frame.allocate(bytes, physical_bits, false, MaxFrame) != kIOReturnSuccess) {
            return false;
        }
        memset(frame.mapping(), Sentinel, bytes);
        if (frame.publishForDevice(0, bytes, true) != kIOReturnSuccess) {
            return false;
        }
    }
    const size_t count = memory->frame[0].spanCount() + memory->frame[1].spanCount();
    if (!count || count > (2 * MaxFrame / Page)) {
        return false;
    }
    auto *ranges = IONewData(ArmSmmu4KRange, count);
    if (!ranges) {
        return false;
    }
    const NezhaCameraFrameMapping frames[] = {{FirstIova, bytes}, {SecondIova, bytes}};
    size_t cursor = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const auto &frame = memory->frame[i];
        for (size_t j = 0; j < frame.spanCount(); ++j) {
            const auto &span = frame.spans()[j];
            ranges[cursor++] = {frames[i].iova + span.offset, span.physical, span.bytes,
                ArmSmmu4KRead | ArmSmmu4KWrite, 0, 2};
        }
    }
    memory->config = {32, physical_bits, memory->tables.spans()[0].physical, 2 * bytes / Page};
    const auto status = nezha_camera_build_frame_span_tables(&memory->config, frames, width, height,
        ranges, count, static_cast<uint64_t *>(memory->tables.mapping()), TablePages, &memory->plan);
    IODeleteData(ranges, ArmSmmu4KRange, count);
    if (status != ArmSmmu4KSuccess ||
        memory->tables.publishForDevice(0, memory->plan.tables.table_pages * Page, false,
                                        false, NativeAdrenoPhaseTable) != kIOReturnSuccess) {
        return false;
    }
    memory->ready = true;
    return true;
}

const NezhaCameraDmaPlan *nezha_camera_native_memory_plan(const NezhaCameraNativeMemory *memory)
{
    return memory && memory->ready ? &memory->plan : nullptr;
}

const ArmSmmu4KConfig *nezha_camera_native_memory_config(const NezhaCameraNativeMemory *memory)
{
    return memory && memory->ready ? &memory->config : nullptr;
}

bool nezha_camera_native_memory_hold(NezhaCameraNativeMemory *memory)
{
    if (!memory || !memory->ready || memory->held != 0) {
        return false;
    }
    NativeAdrenoMemory *owned[] = {&memory->tables, &memory->frame[0], &memory->frame[1]};
    for (unsigned i = 0; i < 3; ++i) {
        if (owned[i]->holdForDevice() != kIOReturnSuccess) {
            return false;
        }
        memory->held |= 1u << i;
    }
    return true;
}

const void *nezha_camera_native_memory_acquire(NezhaCameraNativeMemory *memory, unsigned index)
{
    if (!memory || !memory->ready || memory->held != 7 || index > 1 || (memory->cpu & (1u << index))) {
        return nullptr;
    }
    auto &frame = memory->frame[index];
    if (frame.acquireDeviceWrites(0, memory->frame_bytes, NativeAdrenoPhaseComplete) != kIOReturnSuccess) {
        return nullptr;
    }
    memory->cpu |= 1u << index;
    return frame.mapping();
}

bool nezha_camera_native_memory_publish(NezhaCameraNativeMemory *memory, unsigned index)
{
    if (!memory || !memory->ready || index > 1 || (memory->cpu & (1u << index)) == 0) {
        return false;
    }
    if (memory->frame[index].publishForDevice(0, memory->frame_bytes, true) != kIOReturnSuccess) {
        return false;
    }
    memory->cpu &= ~(1u << index);
    return true;
}

bool nezha_camera_native_memory_drop(NezhaCameraNativeMemory *memory, unsigned index)
{
    if (!memory || !memory->ready || index > 1 || (memory->cpu & (1u << index)) == 0) {
        return false;
    }
    memory->cpu &= ~(1u << index);
    return true;
}

bool nezha_camera_native_memory_release(NezhaCameraNativeMemory **out, bool dma_detached)
{
    if (!out || *out != &memory_storage || !memory_in_use) {
        return false;
    }
    auto *memory = *out;
    if ((memory->held && !dma_detached) || memory->cpu) {
        return false;
    }
    memory->ready = false;
    NativeAdrenoMemory *owned[] = {&memory->tables, &memory->frame[0], &memory->frame[1]};
    for (unsigned i = 3; i != 0; --i) {
        const unsigned index = i - 1, bit = 1u << index;
        if (memory->held & bit) {
            if (owned[index]->retireAfterQuiescence() != kIOReturnSuccess) {
                return false;
            }
            memory->held &= ~bit;
        }
        if (owned[index]->backed() && owned[index]->release() != kIOReturnSuccess) {
            return false;
        }
    }
    memory->cpu = 0;
    memory->plan = {};
    memory->config = {};
    memory->frame_bytes = 0;
    memory_in_use = false;
    *out = nullptr;
    return true;
}
