/* SPDX-License-Identifier: MIT */
#include "NezhaCameraNativeCapture.h"
#include "NezhaCameraCanoePins.h"
#include "NezhaCameraNativeGcc.h"
#include "NezhaCameraNativeIrq.h"
#include "NezhaCameraNativeMemory.h"
#include "NezhaCameraNativeRpmh.h"
#include "NezhaCameraNativeSmmu.h"

#include <IOKit/IOLib.h>
#include <machine/machine_routines.h>
#include <string.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

namespace {
struct RegisterWindow {
    vm_offset_t address;
    uint32_t bytes;
};

struct Capture {
    RegisterWindow window[11]{};
    NezhaCameraRegisterIo reg[11]{};
    NezhaCameraGccIo gcc{};
    NezhaCameraCsiphy24Io phy{};
    NezhaCameraCaptureResourceIo resources_io{};
    NezhaCameraProfile profile{};
    NezhaCameraOvxModeWrite writes[2048]{};
    NezhaCameraMainPins pins{};
    NezhaCameraIfeBandwidth bandwidth{}, clear{};
    NezhaCameraRpmh *rpmh{};
    NezhaCameraNativeSmmu *smmu{};
    NezhaCameraNativeMemory *memory{};
    NezhaCameraNativeIrq *irq{};
    NezhaCamera1080Io camss{};
    NezhaCameraMainCaptureIo capture_io{};
    NezhaCameraMainCapture capture{};
    uint32_t cci_mask{}, timing[5]{};
    bool used{}, cci_touched{}, dma_touched{};
};

Capture state;
enum { Camcc, Crm, Crmc, CrmCommon, IfeRsc, Cpas, Phy, Cci, Csid, Vfe, Tpg };
constexpr uint32_t physical[] = {0x956d000, 0x9548000, 0x954a000, 0x954c000,
    0x9545000, 0x900b000, 0x9523000, 0x941b000, 0x9253000, 0x9151000, 0x93ff000};
constexpr uint32_t sizes[] = {0x80000, 0x2000, 0x3000, 0x1000, 0x1000,
    0x1000, 0x2000, 0x1000, 0x6000, 0x20000, 0x1000};
constexpr uint32_t cci_master_irqs = 0x60ee6000u | (1u << 12) | (1u << 16) |
    (1u << 20) | (1u << 24) | (1u << 26);

uint32_t read(void *cookie, uint32_t offset)
{
    const auto &w = *static_cast<RegisterWindow *>(cookie);
    if ((offset & 3u) || offset > w.bytes - 4u) return UINT32_MAX;
    const uint32_t value = *reinterpret_cast<volatile uint32_t *>(w.address + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}

void write(void *cookie, uint32_t offset, uint32_t value)
{
    const auto &w = *static_cast<RegisterWindow *>(cookie);
    if ((offset & 3u) || offset > w.bytes - 4u) return;
    *reinterpret_cast<volatile uint32_t *>(w.address + offset) = value;
    __asm__ volatile("dsb oshst" ::: "memory");
}

void wait(void *, unsigned ms) { IOSleep(ms); }
void wait_us(void *, unsigned us) { IODelay(us); }

void write_csid(void *cookie, uint32_t offset, uint32_t value)
{
    auto *s = static_cast<Capture *>(cookie);
    write(&s->window[Csid], offset, value);
}

void write_vfe(void *cookie, uint32_t offset, uint32_t value)
{
    auto *s = static_cast<Capture *>(cookie);
    write(&s->window[Vfe], offset, value);
}

uint8_t *frame_acquire(void *cookie, unsigned buffer)
{
    auto *s = static_cast<Capture *>(cookie);
    return const_cast<uint8_t *>(static_cast<const uint8_t *>(nezha_camera_native_memory_acquire(s->memory, buffer)));
}

bool frame_return(void *cookie, unsigned buffer, bool requeue)
{
    auto *s = static_cast<Capture *>(cookie);
    return requeue ? nezha_camera_native_memory_publish(s->memory, buffer) :
        nezha_camera_native_memory_drop(s->memory, buffer);
}

uint32_t frame_fault(void *cookie)
{
    auto *s = static_cast<Capture *>(cookie);
    return nezha_camera_native_smmu_fault(s->smmu);
}

bool frame_next(void *cookie, NezhaCamera1080Events *event)
{
    return nezha_camera_native_irq_next(static_cast<Capture *>(cookie)->irq, event);
}

bool cci_idle(const NezhaCameraRegisterIo &bus)
{
    return bus.read(bus.context, 0x304) == 0 && bus.read(bus.context, 0x404) == 0 &&
        bus.read(bus.context, 0x504) == 0 && bus.read(bus.context, 0x604) == 0 &&
        bus.read(bus.context, 0x11c) == 0 && bus.read(bus.context, 0x21c) == 0;
}

bool restore_cci(Capture &s)
{
    if (!s.cci_touched) return true;
    auto &bus = s.reg[Cci];
    if (!cci_idle(bus)) {
        NezhaCameraCciObservation recovery{};
        if (nezha_camera_cci_recover_master(&bus, 1, 37500000, &recovery) != NezhaCciOk || !cci_idle(bus)) return false;
    }
    for (unsigned i = 0; i < 5; ++i) bus.write(bus.context, 0x200 + i * 4, s.timing[i]);
    for (unsigned i = 0; i < 5; ++i) if (bus.read(bus.context, 0x200 + i * 4) != s.timing[i]) return false;
    bus.write(bus.context, 0xc04, s.cci_mask);
    if (bus.read(bus.context, 0xc04) != s.cci_mask) return false;
    s.cci_touched = false;
    return true;
}

bool prepare_cci(Capture &s, NezhaCameraNativeCaptureResult &result)
{
    auto &bus = s.reg[Cci];
    s.cci_mask = bus.read(bus.context, 0xc04);
    if (s.cci_mask == UINT32_MAX || !cci_idle(bus)) return false;
    for (unsigned i = 0; i < 5; ++i) {
        s.timing[i] = bus.read(bus.context, 0x200 + i * 4);
        if (s.timing[i] == UINT32_MAX) return false;
    }
    s.cci_touched = true;
    bus.write(bus.context, 0xc04, s.cci_mask | cci_master_irqs);
    if (bus.read(bus.context, 0xc04) != (s.cci_mask | cci_master_irqs)) return false;
    result.status = nezha_camera_cci_recover_master(&bus, 1, 37500000, &result.cci);
    return result.status == NezhaCciOk;
}

bool checkpoint(const NezhaCameraNativeCaptureRequest &request, NezhaCameraNativeCaptureResult &result,
    NezhaCameraNativeCaptureStep step, unsigned a = 0, unsigned b = 0, unsigned c = 0)
{
    result.step = step;
    return !request.checkpoint || request.checkpoint(step, a, b, c);
}

bool cleanup(Capture &s, NezhaCameraNativeCaptureResult &result,
    const NezhaCameraRegisterIo &gpio, const NezhaCameraRegisterIo &mclk,
    const NezhaCameraMainRailIo &rails, const NezhaCameraCsiphy24Profile *profile, uint8_t *raw_sample)
{
    result.cleanup_step = NezhaNativeStopSensor;
    if (s.capture.sensor_transfer.needs_recovery) {
        NezhaCameraCciObservation recovery{};
        if (nezha_camera_cci_recover_master(&s.reg[Cci], 1, 37500000, &recovery) != NezhaCciOk) return false;
        s.capture.sensor_transfer.needs_recovery = false;
    }
    if (!nezha_camera_main_capture_request_stop(&s.capture_io, &s.capture)) return false;
    result.cleanup_step = NezhaNativeDrain;
    if (!nezha_camera_capture_resources_drain(&s.resources_io, &result.resources, true)) return false;
    result.cleanup_step = NezhaNativeStopIrq;
    if (s.irq && !nezha_camera_native_irq_stop(s.irq)) return false;
    if (s.irq) {
        result.irq_fault = nezha_camera_native_irq_fault(s.irq);
        (void)nezha_camera_native_irq_failure(s.irq, &result.irq_failure);
    }
    // If cache publication failed, the sole CPU worker still owns this
    // frame. Drop its claim after drain without returning it to hardware.
    for (unsigned i = 0; i < 2; ++i) {
        if (!(s.capture.cpu_frames & (1u << i))) continue;
        if (!nezha_camera_native_memory_drop(s.memory, i) ||
            !nezha_camera_main_capture_release_frame(&s.capture_io, &s.capture, i, false)) return false;
    }
    result.cleanup_step = NezhaNativeDetach;
    const bool ready = !s.dma_touched || nezha_camera_native_smmu_prepare_detach(s.smmu);
    nezha_camera_native_smmu_fault_info(s.smmu, &result.smmu_fault);
    result.frames.fault |= result.smmu_fault.status;
    auto capture_io = s.capture_io;
    capture_io.dma_enter = nullptr;
    capture_io.dma_leave = nullptr;
    const bool detached = ready && (!s.dma_touched || nezha_camera_main_capture_finish_stop(&capture_io,
        nezha_camera_native_smmu_context(s.smmu), 100, &s.capture, true));
    if (s.smmu && !nezha_camera_native_smmu_release(&s.smmu, detached)) return false;
    result.cleanup_step = NezhaNativeReleaseMemory;
    if (raw_sample) {
        const unsigned final_buffer = result.frames.final_buffer;
        const auto *sample = nezha_camera_native_memory_acquire(s.memory, final_buffer);
        if (sample) {
            const size_t bytes = s.capture.layout.layout.frame_bytes;
            memcpy(raw_sample, sample, bytes);
            if (!nezha_camera_native_memory_drop(s.memory, final_buffer)) return false;
            result.raw_sample_bytes = bytes;
        }
    }
    if (s.memory && !nezha_camera_native_memory_release(&s.memory, true)) return false;
    result.cleanup_step = NezhaNativeRestorePins;
    if (!restore_cci(s)) return false;
    if (s.pins.reset_held && !nezha_camera_main_restore_pins(&gpio, &mclk, &rails, &s.pins)) return false;
    result.cleanup_step = NezhaNativeReleaseResources;
    if (!nezha_camera_capture_resources_stop(&s.resources_io, &s.clear, profile,
        &result.resources, true, true)) return false;
    if (!nezha_camera_native_rpmh_close(&s.rpmh)) return false;
    result.cleanup_step = NezhaNativeCleanupDone;
    return true;
}
}

bool nezha_camera_native_capture(const NezhaCameraNativeCaptureRequest *request,
    NezhaCameraNativeCaptureResult *result)
{
    if (!request || !result || state.used ||
        !request->command_database || request->database_bytes != 0x20000) return false;
    *result = {};
    result->step = NezhaNativeProfile;
    const bool pattern = request->test_pattern;
    if (!pattern && (!request->module_match_confirmed || request->cci_queue_words < 16 ||
        request->cci_queue_words > 128 || !nezha_camera_profile_open(request->profile, request->profile_bytes, &state.profile) ||
        !nezha_camera_profile_writes(&state.profile, state.writes, sizeof(state.writes) / sizeof(state.writes[0])))) return false;
    const auto *profile = pattern ? nullptr : &state.profile.phy;
    const uint32_t width = pattern ? 640 : state.profile.width;
    const uint32_t height = pattern ? 480 : state.profile.height;
    const size_t sample_bytes = size_t(width) * height * 5 / 4;
    if (request->raw_sample && request->raw_sample_capacity < sample_bytes) return false;
    state.used = true;
    if (!checkpoint(*request, *result, NezhaNativeSharedOwners, physical[Camcc], physical[Csid], physical[Vfe])) return false;
    for (unsigned i = 0; i < sizeof(physical) / sizeof(physical[0]); ++i) {
        if ((pattern && (i == Phy || i == Cci)) || (!pattern && i == Tpg)) continue;
        state.window[i] = {ml_io_map(physical[i], sizes[i]), sizes[i]};
        if (!state.window[i].address) return false;
        state.reg[i] = {&state.window[i], read, write, wait};
    }
    // Unaligned DT apertures are views of their correctly page-covered maps.
    state.window[Crmc].address += 0x600;
    state.window[Crmc].bytes -= 0x600;
    state.window[CrmCommon].address += 0xc00;
    state.window[CrmCommon].bytes -= 0xc00;
    RegisterWindow gpio_window{pattern ? 0 : ml_io_map(NezhaCameraGpioPhysical, NezhaCameraGpioBytes),
        NezhaCameraGpioBytes + NezhaCameraGpioOffset};
    RegisterWindow mclk_window{pattern ? 0 : ml_io_map(0x1764000, 0x1000), 0x1000};
    if (!pattern) {
        if (!gpio_window.address || !mclk_window.address) return false;
        gpio_window.address -= NezhaCameraGpioOffset;
    }
    const NezhaCameraRegisterIo gpio{&gpio_window, read, write, wait}, mclk{&mclk_window, read, write, wait};
    if (!nezha_camera_native_gcc(&state.gcc) || !nezha_camera_native_rpmh_open(&state.rpmh,
        request->command_database, request->database_bytes)) return false;
    state.rpmh->before_transaction = request->rpmh_checkpoint;
    const NezhaCameraMainRailIo rails{state.rpmh, nezha_camera_rpmh_acquire, nezha_camera_rpmh_release};
    state.phy = {state.reg[Phy], wait_us};
    state.resources_io = {&state.gcc, &state.reg[Camcc], &state.reg[Crm], &state.reg[Crmc],
        &state.reg[CrmCommon], &state.reg[IfeRsc], &state.reg[Cpas], &state.phy, state.rpmh};
    state.camss = {&state, write_csid, write_vfe};
    state.capture_io.cci = pattern ? nullptr : &state.reg[Cci];
    state.capture_io.tpg = pattern ? &state.reg[Tpg] : nullptr;
    state.capture_io.camss = &state.camss;
    state.capture_io.cci_queue_words = request->cci_queue_words;
    bool okay = false;
    do {
        // Qualcomm CAMSS v24 Nord HF MNOC reference for 4096x3072
        // at 30 fps. CAMNOC's separate clock floor is applied by CRM.
        constexpr uint32_t average = 471860000, peak = 925857000;
        if (!nezha_camera_ife_bandwidth(request->command_database, request->database_bytes,
            average, peak, average, peak, &state.bandwidth) ||
            !nezha_camera_ife_bandwidth(request->command_database, request->database_bytes, 0, 0, 0, 0, &state.clear)) break;
        if (!checkpoint(*request, *result, NezhaNativeResources, physical[Crm], physical[IfeRsc], physical[Cpas]) ||
            !nezha_camera_capture_resources_start(&state.resources_io, &state.bandwidth, profile, &result->resources)) break;
        if (!pattern) {
            if (!checkpoint(*request, *result, NezhaNativePins, NezhaCameraGpioPhysical, 0x1764000, 0) ||
                !nezha_camera_main_hold_reset(&gpio, &state.pins) ||
                !nezha_camera_main_start_pins(&gpio, &mclk, &rails, &state.pins)) break;
            if (!checkpoint(*request, *result, NezhaNativeCci, physical[Cci], 37500000, 1) ||
                !prepare_cci(state, *result)) break;
        }
        uint32_t reset;
        if (!checkpoint(*request, *result, NezhaNativeCsid, physical[Csid], physical[Vfe], 0) ||
            !nezha_camera1080_prepare_csid(&state.reg[Csid], &reset)) break;
        if (!checkpoint(*request, *result, NezhaNativeIrq, reset, 0, 0) ||
            !nezha_camera_native_irq_open(&state.irq, &state.reg[Csid], &state.reg[Vfe])) break;
        unsigned physical_bits;
        if (!checkpoint(*request, *result, NezhaNativeSmmu, request->slot, request->bank, request->asid)) break;
        result->status = nezha_camera_native_smmu_prepare(&state.smmu, request->slot, request->bank,
            request->asid, &physical_bits);
        if (result->status != ArmSmmu4KSuccess) break;
        if (!checkpoint(*request, *result, NezhaNativeMemory, physical_bits, width, height) ||
            !nezha_camera_native_memory_prepare(&state.memory, physical_bits, width, height) ||
            !nezha_camera_native_memory_hold(state.memory)) break;
        result->status = nezha_camera_native_smmu_bind(state.smmu, state.memory);
        if (result->status != ArmSmmu4KSuccess) break;
        state.capture_io.smmu = nezha_camera_native_smmu_io(state.smmu);
        state.capture_io.dma_owner = state.smmu;
        state.capture_io.dma_enter = nezha_camera_native_smmu_capture_enter;
        state.capture_io.dma_leave = nezha_camera_native_smmu_capture_leave;
        if (!checkpoint(*request, *result, NezhaNativeStream, pattern ? physical[Tpg] : physical[Phy], width, height)) break;
        state.dma_touched = true;
        const NezhaCamera1080Input input = pattern ? NezhaCamera1080Input{NezhaCamera1080Tpg, 2, 0, {}} :
            NezhaCamera1080Input{NezhaCamera1080Cphy, 0, 3, {0, 1, 2, 0}};
        const NezhaCameraMainMode mode{NezhaCameraOvx10500uId, width, height,
            state.writes, state.profile.initialization_count + state.profile.mode_count, input};
        if (!nezha_camera_main_capture_start(&state.capture_io, &mode,
            nezha_camera_native_memory_plan(state.memory), nezha_camera_native_smmu_context(state.smmu),
            request->slot, request->bank, 100, &state.capture)) break;
        // Streaming has already begun. Avoid a filesystem sync before
        // consuming the two-frame queue.
        result->step = NezhaNativeFrames;
        const NezhaCameraFrameMemoryIo memory{&state, frame_acquire, frame_return, frame_fault, frame_next};
        result->status = nezha_camera_frame_probe(&state.capture_io, &state.capture, &state.reg[Csid],
            &state.reg[Vfe], &memory, 20, 3000, &result->frames);
        okay = result->status == NezhaFramesOk;
    } while (false);
    result->identity = state.capture.identity;
    result->capture_stage = state.capture.stage;
    result->sensor_status = state.capture.sensor_status;
    result->dma_status = state.capture.dma_status;
    result->applied_writes = state.capture.applied_writes;
    result->rpmh_address = state.rpmh->last_address;
    result->rpmh_ack = state.rpmh->last_command_status;
    result->rpmh_reply = state.rpmh->last_reply;
    if (state.capture.stage >= NezhaCaptureIdentity && !pattern) result->cci = state.capture.sensor_transfer;
    result->cleaned = cleanup(state, *result, gpio, mclk, rails, profile, okay ? request->raw_sample : nullptr);
    result->passed = okay && result->cleaned && !result->irq_fault && !result->frames.fault &&
        (!request->raw_sample || result->raw_sample_bytes == sample_bytes);
    return result->passed;
}
