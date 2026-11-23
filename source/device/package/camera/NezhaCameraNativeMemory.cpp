/* SPDX-License-Identifier: MIT */
#include "NezhaCameraNativeMemory.h"
#include "NezhaCameraFrameMarker.h"
#include "NativeAdrenoMemory.h"

#include <IOKit/IOLib.h>

namespace {
constexpr size_t Page = 4096;
constexpr size_t TablePages = 32;
constexpr size_t MaxFrame = 15 * 1024 * 1024; // 4096x3072 packed RAW10.
constexpr uint64_t FirstIova = 0x01000000;
constexpr uint64_t SecondIova = 0x02000000;
constexpr uint64_t ThirdIova = 0x03000000;
constexpr unsigned Frames = NezhaCameraRdi1080MaxFrames;
constexpr unsigned AllHeld = (1u << (Frames + 1u)) - 1u;
constexpr uint8_t Sentinel = 0xa5;

void device_barrier()
{
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
}

struct NezhaCameraNativeMemory {
    NativeAdrenoMemory frame[Frames], tables;
    NezhaCameraDmaPlan plan{};
    ArmSmmu4KConfig config{};
    size_t frame_bytes{};
    NezhaCameraRaw10Layout layout{};
    unsigned held{}, cpu{};
    unsigned physical_bits{};
    bool ready{}, pool_ready{};
};

// One capture worker. Keep the successfully prepared, bounded camera pool
// wired across close/open; only a fully detached session may reuse it.
static NezhaCameraNativeMemory memory_storage;
static bool memory_in_use;
static NezhaCameraNativeMemoryFailure memory_failure;

static bool failed(unsigned step, unsigned object, uint32_t status,
    size_t bytes = 0, size_t spans = 0)
{
    if (!memory_failure.step) {
        memory_failure.step = step;
        memory_failure.object = object;
        memory_failure.status = status;
        memory_failure.requested_bytes = bytes;
        memory_failure.spans = spans;
        memory_failure.held = memory_storage.held;
    }
    return false;
}

void nezha_camera_native_memory_failure(NezhaCameraNativeMemoryFailure *out)
{
    if (out) *out = memory_failure;
}

bool nezha_camera_native_memory_prepare(NezhaCameraNativeMemory **out,
    unsigned physical_bits, uint32_t width, uint32_t height)
{
    memory_failure = {};
    if (!out || *out || memory_in_use || (physical_bits != 36 && physical_bits != 40)) {
        return failed(NezhaMemoryArguments, 0, kIOReturnBadArgument);
    }
    const NezhaCameraFrameMapping allowed[] = {
        {FirstIova, MaxFrame}, {SecondIova, MaxFrame}, {ThirdIova, MaxFrame}};
    NezhaCameraRdi1080 layout{};
    if (!nezha_camera_rdi1080_pool_layout(width, height, allowed, Frames, &layout) ||
        layout.layout.frame_bytes > MaxFrame) {
        return failed(NezhaMemoryLayout, 0, kIOReturnBadArgument);
    }
    auto *memory = &memory_storage;
    const size_t bytes = (size_t(layout.layout.frame_bytes) + Page - 1) & ~(Page - 1);
    const bool reused = memory->pool_ready;
    memory_failure.reused = reused;
    if (memory->held || memory->cpu || (reused &&
        (memory->physical_bits != physical_bits || memory->frame_bytes != bytes ||
         memory->layout.width != width || memory->layout.height != height))) {
        return failed(NezhaMemoryPool, 0, kIOReturnBusy, bytes);
    }
    memory_in_use = true;
    *out = memory;
    memory->ready = false;
    memory->frame_bytes = bytes;
    memory->layout = layout.layout;
    memory->physical_bits = physical_bits;
    IOReturn operation;
    if (!reused) {
        operation = memory->tables.allocate(TablePages * Page, physical_bits, true, TablePages * Page);
        if (operation != kIOReturnSuccess) {
            return failed(NezhaMemoryTableAllocate, 0, operation, TablePages * Page,
                memory->tables.spanCount());
        }
    }
    if (memory->tables.spanCount() != 1 || !memory->tables.mapping() ||
        memory->tables.bytes() != TablePages * Page || memory->tables.quarantined() ||
        memory->tables.deviceReferences()) {
        return failed(NezhaMemoryTableSpans, 0, kIOReturnUnsupported, TablePages * Page,
            memory->tables.spanCount());
    }
    for (unsigned i = 0; i < Frames; ++i) {
        auto &frame = memory->frame[i];
        if (!reused) {
            operation = frame.allocate(bytes, physical_bits, false, MaxFrame);
            if (operation != kIOReturnSuccess) {
                return failed(NezhaMemoryFrameAllocate, i + 1, operation, bytes, frame.spanCount());
            }
        }
        if (!frame.mapping() || frame.bytes() != bytes || !frame.spanCount() ||
            frame.quarantined() || frame.deviceReferences()) {
            return failed(NezhaMemoryPool, i + 1, kIOReturnNotReady, bytes, frame.spanCount());
        }
        // RAW has one kernel mapping and no user/physical-aperture reader.
        // The existing owner cleans its old lines and changes all tracked
        // CPU mappings while no device reference exists. Its per-frame
        // acquire/publication checks then need no 15 MiB cache-line walk.
        if (!reused) {
            operation = frame.makeWriteCombined();
            if (operation != kIOReturnSuccess) {
                return failed(NezhaMemoryFrameCacheMode, i + 1, operation, bytes, frame.spanCount());
            }
        }
        if (!frame.writeCombined()) {
            return failed(NezhaMemoryFrameCacheMode, i + 1, kIOReturnNotReady, bytes, frame.spanCount());
        }
        memset(frame.mapping(), Sentinel, bytes);
        operation = frame.publishForDevice(0, bytes, true);
        if (operation != kIOReturnSuccess) {
            return failed(NezhaMemoryFramePublish, i + 1, operation, bytes, frame.spanCount());
        }
    }
    if (!reused) {
        size_t count = 0;
        for (const auto &frame : memory->frame) count += frame.spanCount();
        if (!count || count > (Frames * MaxFrame / Page)) {
            return failed(NezhaMemoryRanges, 0, kIOReturnNoResources, count * sizeof(ArmSmmu4KRange), count);
        }
        auto *ranges = IONewData(ArmSmmu4KRange, count);
        if (!ranges) {
            return failed(NezhaMemoryRanges, 0, kIOReturnNoMemory, count * sizeof(ArmSmmu4KRange), count);
        }
        const NezhaCameraFrameMapping frames[] = {
            {FirstIova, bytes}, {SecondIova, bytes}, {ThirdIova, bytes}};
        size_t cursor = 0;
        for (unsigned i = 0; i < Frames; ++i) {
            const auto &frame = memory->frame[i];
            for (size_t j = 0; j < frame.spanCount(); ++j) {
                const auto &span = frame.spans()[j];
                ranges[cursor++] = {frames[i].iova + span.offset, span.physical, span.bytes,
                    ArmSmmu4KRead | ArmSmmu4KWrite, 0, 2};
            }
        }
        memory->config = {32, physical_bits, memory->tables.spans()[0].physical, Frames * bytes / Page};
        const auto status = nezha_camera_build_frame_pool_span_tables(&memory->config, frames, Frames, width, height,
            ranges, count, static_cast<uint64_t *>(memory->tables.mapping()), TablePages, &memory->plan);
        IODeleteData(ranges, ArmSmmu4KRange, count);
        if (status != ArmSmmu4KSuccess) {
            return failed(NezhaMemoryBuildTables, 0, status, TablePages * Page, count);
        }
    }
    // The pool and its exact physical spans have not changed. Keep the
    // immutable mapping plan as well, avoiding fresh range allocations.
    operation = memory->tables.publishForDevice(0, memory->plan.tables.table_pages * Page, false,
        false, NativeAdrenoPhaseTable);
    if (operation != kIOReturnSuccess) {
        return failed(NezhaMemoryTablePublish, 0, operation, memory->plan.tables.table_pages * Page,
            memory->tables.spanCount());
    }
    device_barrier();
    memory->ready = true;
    memory->pool_ready = true;
    memory_failure.raw_write_combined = true;
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
        return failed(NezhaMemoryHold, 0, kIOReturnNotReady);
    }
    NativeAdrenoMemory *owned[] = {&memory->tables, &memory->frame[0], &memory->frame[1], &memory->frame[2]};
    for (unsigned i = 0; i < Frames + 1u; ++i) {
        const IOReturn operation = owned[i]->holdForDevice();
        if (operation != kIOReturnSuccess) {
            return failed(NezhaMemoryHold, i, operation, owned[i]->bytes(), owned[i]->spanCount());
        }
        memory->held |= 1u << i;
    }
    return true;
}

const void *nezha_camera_native_memory_acquire(NezhaCameraNativeMemory *memory, unsigned index)
{
    if (!memory || !memory->ready || memory->held != AllHeld || index >= Frames || (memory->cpu & (1u << index))) {
        return nullptr;
    }
    auto &frame = memory->frame[index];
    if (!frame.writeCombined()) return nullptr;
    if (frame.acquireDeviceWrites(0, memory->frame_bytes, NativeAdrenoPhaseComplete) != kIOReturnSuccess) {
        return nullptr;
    }
    device_barrier();
    memory->cpu |= 1u << index;
    return frame.mapping();
}

bool nezha_camera_native_memory_publish(NezhaCameraNativeMemory *memory, unsigned index)
{
    if (!memory || !memory->ready || index >= Frames || (memory->cpu & (1u << index)) == 0) {
        return false;
    }
    if (!memory->frame[index].writeCombined()) return false;
    /* Keep the same four marker ranges and owner checks. The exclusive
     * RAW mapping is uncached; the barrier below completes marker stores
     * before the caller publishes an address to the VFE write master. */
    for (unsigned row = 0; row < NezhaCameraFrameMarkerRows; ++row) {
        const size_t marker = nezha_camera_frame_marker_offset(&memory->layout, row);
        const size_t begin = marker & ~(Page - 1);
        const size_t end = (marker + NezhaCameraFrameMarkerBytes + Page - 1) & ~(Page - 1);
        if (end > memory->frame_bytes || end <= begin ||
            memory->frame[index].publishForDevice(begin, end - begin, true) != kIOReturnSuccess) {
            return false;
        }
    }
    device_barrier();
    memory->cpu &= ~(1u << index);
    return true;
}

bool nezha_camera_native_memory_drop(NezhaCameraNativeMemory *memory, unsigned index)
{
    if (!memory || !memory->ready || index >= Frames || (memory->cpu & (1u << index)) == 0) {
        return false;
    }
    memory->cpu &= ~(1u << index);
    return true;
}

bool nezha_camera_native_memory_release(NezhaCameraNativeMemory **out, bool dma_detached)
{
    if (!out || *out != &memory_storage || !memory_in_use) {
        return failed(NezhaMemoryRelease, 0, kIOReturnBadArgument);
    }
    auto *memory = *out;
    if ((memory->held && !dma_detached) || memory->cpu) {
        return failed(NezhaMemoryRelease, 0, kIOReturnBusy);
    }
    memory->ready = false;
    NativeAdrenoMemory *owned[] = {&memory->tables, &memory->frame[0], &memory->frame[1], &memory->frame[2]};
    for (unsigned i = Frames + 1u; i != 0; --i) {
        const unsigned index = i - 1, bit = 1u << index;
        if (memory->held & bit) {
            const IOReturn operation = owned[index]->retireAfterQuiescence();
            if (operation != kIOReturnSuccess) {
                return failed(NezhaMemoryRetire, index, operation, owned[index]->bytes(), owned[index]->spanCount());
            }
            memory->held &= ~bit;
        }
        if (owned[index]->quarantined() || owned[index]->deviceReferences()) {
            return failed(NezhaMemoryRelease, index, kIOReturnBusy, owned[index]->bytes(), owned[index]->spanCount());
        }
        if (!memory->pool_ready && owned[index]->backed()) {
            const IOReturn operation = owned[index]->release();
            if (operation != kIOReturnSuccess) {
                return failed(NezhaMemoryRelease, index, operation, owned[index]->bytes(), owned[index]->spanCount());
            }
        }
    }
    memory->cpu = 0;
    if (!memory->pool_ready) {
        memory->plan = {};
        memory->config = {};
        memory->frame_bytes = 0;
        memory->layout = {};
        memory->physical_bits = 0;
    }
    memory_in_use = false;
    *out = nullptr;
    return true;
}
