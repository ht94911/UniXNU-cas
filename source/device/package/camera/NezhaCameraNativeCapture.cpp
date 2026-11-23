/* SPDX-License-Identifier: MIT */
#include "NezhaCameraNativeCapture.h"
#include "NezhaCameraCanoePins.h"
#include "NezhaCameraNativeGcc.h"
#include "NezhaCameraNativeIrq.h"
#include "NezhaCameraNativeMemory.h"
#include "NezhaCameraNativeRpmh.h"
#include "NezhaCameraNativeSmmu.h"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <string.h>
#include <sys/systm.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

namespace {
struct RegisterWindow {
    vm_offset_t address;
    uint32_t bytes;
};

struct Capture {
    RegisterWindow window[11]{};
    RegisterWindow gpio_window{}, mclk_window{};
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
    uint32_t cci_mask{}, timing[5]{}, vfe_bus_cgc_before{};
    const NezhaCameraNativeCaptureRequest *control_request{};
    unsigned control_frames{};
    NezhaCameraFrameStatus control_frame_status{NezhaFramesOk};
    bool used{}, cci_touched{}, dma_touched{}, running{}, cancelled{}, vfe_bus_cgc_owned{};
    unsigned stopped{};
    uint64_t completed_time{};
    uint64_t replacement_time{};
    bool replacement_pending{};
    const uint8_t *delivery_frame{};
    unsigned delivery_index{};
    bool delivery_active{};
    NezhaCameraFrameStatus copy_service_status{NezhaFramesOk};
    NezhaCameraNativeFrameWork work{};
};

Capture state;
RegisterWindow display_observer[2];
enum { Camcc, Crm, Crmc, CrmCommon, IfeRsc, Cpas, Phy, Cci, Csid, Vfe, Tpg };
constexpr uint32_t physical[] = {0x956d000, 0x9548000, 0x954a000, 0x954c000,
    0x9545000, 0x900b000, 0x9523000, 0x941b000, 0x9253000, 0x9151000, 0x93ff000};
constexpr uint32_t sizes[] = {0x80000, 0x2000, 0x3000, 0x1000, 0x1000,
    0x1000, 0x2000, 0x1000, 0x6000, 0x20000, 0x1000};
constexpr uint32_t cci_master_irqs = 0x60ee6000u | (1u << 12) | (1u << 16) |
    (1u << 20) | (1u << 24) | (1u << 26);
// Qualcomm CAMSS v24 vfe_wm_start disables input clock gating before
// programming the full VFE1080 write master. The stock tfe1080 table
// confirms bus_wr_base 0x1000 and cgc_ovd offset 8.
constexpr uint32_t VfeBusCgc = 0x1008, VfeBusCgcNoGating = 0x07ffffff;

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

/* Four samples distinguish loss during the first APPS request from loss
 * during teardown. These are read-only versions/statuses, not frame
 * delivery evidence. Stock nezha DT supplies both physical addresses. */
void display_state(const char *stage)
{
    constexpr uint32_t address[] = {0x09800000, 0x09ac0000};
    for (unsigned i = 0; i < 2; ++i) {
        if (!display_observer[i].address)
            display_observer[i] = {ml_io_map(address[i], 0x1000), 0x1000};
    }
    if (!display_observer[0].address || !display_observer[1].address) return;
    printf("NEZHA CAMERA DISPLAY: %s dpu=%#x dsi=%#x ctrl=%#x status=%#x\n", stage,
        read(&display_observer[0], 0), read(&display_observer[1], 0),
        read(&display_observer[1], 4), read(&display_observer[1], 8));
}

void write_csid(void *cookie, uint32_t offset, uint32_t value)
{
    auto *s = static_cast<Capture *>(cookie);
    write(&s->window[Csid], offset, value);
    if (offset == 0x11c && value == 1 && s->replacement_pending) {
        /* The existing register write includes its device barrier. Record
         * the address-update submission, not a hardware acceptance or FPS.
         * No extra MMIO, IRQ logging or worker is introduced. */
        const uint64_t after = mach_absolute_time();
        if (s->replacement_time && after >= s->replacement_time &&
            after - s->replacement_time > s->work.completion_to_address_max)
            s->work.completion_to_address_max = after - s->replacement_time;
        ++s->work.replacements;
        s->replacement_pending = false;
    }
}

void write_vfe(void *cookie, uint32_t offset, uint32_t value)
{
    auto *s = static_cast<Capture *>(cookie);
    write(&s->window[Vfe], offset, value);
    if (offset == 0x2c04 && s->completed_time) {
        s->replacement_time = s->completed_time;
        s->replacement_pending = true;
    }
}

bool acquire_vfe_bus(Capture &s)
{
    auto &bus = s.reg[Vfe];
    if (s.vfe_bus_cgc_owned) return false;
    s.vfe_bus_cgc_before = bus.read(bus.context, VfeBusCgc);
    if (s.vfe_bus_cgc_before == UINT32_MAX) return false;
    s.vfe_bus_cgc_owned = true;
    const uint32_t expected = s.vfe_bus_cgc_before | VfeBusCgcNoGating;
    bus.write(bus.context, VfeBusCgc, expected);
    return bus.read(bus.context, VfeBusCgc) == expected;
}

bool restore_vfe_bus(Capture &s)
{
    if (!s.vfe_bus_cgc_owned) return true;
    auto &bus = s.reg[Vfe];
    const uint32_t current = bus.read(bus.context, VfeBusCgc);
    if (current == UINT32_MAX) return false;
    const uint32_t expected = (current & ~VfeBusCgcNoGating) |
        (s.vfe_bus_cgc_before & VfeBusCgcNoGating);
    bus.write(bus.context, VfeBusCgc, expected);
    if (bus.read(bus.context, VfeBusCgc) != expected) return false;
    s.vfe_bus_cgc_owned = false;
    return true;
}

uint8_t *frame_acquire(void *cookie, unsigned buffer)
{
    auto *s = static_cast<Capture *>(cookie);
    const uint64_t before = mach_absolute_time();
    if (s->completed_time && before >= s->completed_time && before - s->completed_time > s->work.queue_max)
        s->work.queue_max = before - s->completed_time;
    auto *frame = const_cast<uint8_t *>(static_cast<const uint8_t *>(nezha_camera_native_memory_acquire(s->memory, buffer)));
    const uint64_t after = mach_absolute_time();
    if (after >= before && after - before > s->work.acquire_max) s->work.acquire_max = after - before;
    return frame;
}

bool frame_return(void *cookie, unsigned buffer, bool requeue)
{
    auto *s = static_cast<Capture *>(cookie);
    const uint64_t before = mach_absolute_time();
    const bool returned = requeue ? nezha_camera_native_memory_publish(s->memory, buffer) :
        nezha_camera_native_memory_drop(s->memory, buffer);
    const uint64_t after = mach_absolute_time();
    if (after >= before && after - before > s->work.publish_max) s->work.publish_max = after - before;
    if (returned) {
        ++s->work.returns;
        if (s->completed_time && after >= s->completed_time &&
            after - s->completed_time > s->work.completion_to_publish_max)
            s->work.completion_to_publish_max = after - s->completed_time;
    }
    s->completed_time = 0;
    return returned;
}

uint32_t frame_fault(void *cookie)
{
    auto *s = static_cast<Capture *>(cookie);
    return nezha_camera_native_smmu_fault(s->smmu);
}

bool frame_next(void *cookie, NezhaCamera1080Events *event)
{
    auto *s = static_cast<Capture *>(cookie);
    const bool next = nezha_camera_native_irq_next(s->irq, event);
    if (next && event->buffer_done) s->completed_time = event->completed_time;
    return next;
}

bool client_continuing(void *cookie)
{
    const auto *request = static_cast<const NezhaCameraNativeCaptureRequest *>(cookie);
    return !__atomic_load_n(&state.stopped, __ATOMIC_ACQUIRE) &&
        request->consumer->continuing(request->consumer->context);
}

bool client_deliver(void *cookie, const NezhaCameraRaw10Layout *layout, const uint8_t *frame,
    size_t bytes, uint64_t ticks, uint32_t sequence)
{
    const auto *request = static_cast<const NezhaCameraNativeCaptureRequest *>(cookie);
    const unsigned owned = state.capture.cpu_frames;
    if (state.delivery_active || !owned || (owned & (owned - 1u))) return false;
    unsigned index = 0;
    while (index < NezhaCameraRdi1080MaxFrames && owned != (1u << index)) ++index;
    if (index == NezhaCameraRdi1080MaxFrames) return false;
    state.delivery_frame = frame;
    state.delivery_index = index;
    state.delivery_active = true;
    state.copy_service_status = NezhaFramesOk;
    const uint64_t before = mach_absolute_time();
    const bool delivered = request->consumer->deliver(request->consumer->context, layout, frame, bytes, ticks, sequence);
    const uint64_t after = mach_absolute_time();
    if (after >= before && after - before > state.work.delivery_max) state.work.delivery_max = after - before;
    if (delivered) ++state.work.deliveries;
    state.delivery_active = false;
    state.delivery_frame = nullptr;
    return delivered && state.copy_service_status == NezhaFramesOk;
}

void wait_controls(void *, unsigned milliseconds)
{
    const auto *request=state.control_request;
    if (!request) { IOSleep(milliseconds);return; }
    const NezhaCameraFrameMemoryIo memory{&state,frame_acquire,frame_return,frame_fault,frame_next};
    const NezhaCameraFrameConsumer client{const_cast<NezhaCameraNativeCaptureRequest *>(request),
        client_continuing,client_deliver};
    for (unsigned ms=0;ms<milliseconds;++ms) {
        /* CCI reports complete independently of RAW DMA. Waiting for CCI
         * must not leave both RAW buffers unconsumed. Reuse this same
         * worker and its exact cache/callback/requeue path; never consume
         * IRQs from another thread or poll the hardware a second time. */
        for (unsigned ready=0;ready<2 && state.control_frame_status==NezhaFramesOk &&
            client_continuing(client.context);++ready) {
            const unsigned before=state.capture.completed_frames;
            state.control_frame_status=nezha_camera_frame_consume(&state.capture_io,&state.capture,
                &state.reg[Csid],&memory,&client,0);
            state.control_frames+=state.capture.completed_frames-before;
            if (state.capture.completed_frames==before) break;
        }
        IODelay(1000);
    }
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
    if (bus.read(bus.context, 0) != NezhaCameraCciCanoeVersion) return false;
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
    if (request.consumer && !client_continuing(const_cast<NezhaCameraNativeCaptureRequest *>(&request))) {
        state.cancelled = true;
        return false;
    }
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
    if (!nezha_camera_capture_resources_drain(&s.resources_io, &result.resources, true)) {
        /* Producers have stopped, but the failed CPAS drain retains every
         * DMA mapping and resource lease. Quiesce only our IRQ callback so
         * its preserved first fault is readable even on this early return. */
        if (s.irq && nezha_camera_native_irq_stop(s.irq)) {
            result.irq_stopped = true;
            result.irq_fault = nezha_camera_native_irq_fault(s.irq);
            (void)nezha_camera_native_irq_failure(s.irq, &result.irq_failure);
        }
        return false;
    }
    result.cleanup_step = NezhaNativeStopIrq;
    if (s.irq && !nezha_camera_native_irq_stop(s.irq)) return false;
    if (s.irq) {
        result.irq_stopped = true;
        result.irq_fault = nezha_camera_native_irq_fault(s.irq);
        (void)nezha_camera_native_irq_failure(s.irq, &result.irq_failure);
    }
    // If cache publication failed, the sole CPU worker still owns this
    // frame. Drop its claim after drain without returning it to hardware.
    for (unsigned i = 0; i < NezhaCameraRdi1080MaxFrames; ++i) {
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
    // Restore the bus clock bits only after producers, IRQ and DMA have
    // stopped. Capture resources still keep the VFE register page powered.
    if (!restore_vfe_bus(s)) return false;
    result.cleanup_step = NezhaNativeRestorePins;
    if (!restore_cci(s)) return false;
    if (s.pins.reset_held && !nezha_camera_main_restore_pins(&gpio, &mclk, &rails, &s.pins)) return false;
    result.cleanup_step = NezhaNativeReleaseResources;
    display_state("before-release");
    if (!nezha_camera_capture_resources_stop(&s.resources_io, &s.clear, profile,
        &result.resources, true, true)) return false;
    display_state("after-release");
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
    if (request->consumer && (request->test_pattern || request->raw_sample ||
        !request->consumer->continuing || !request->consumer->deliver)) return false;
    if (request->controls && (!request->consumer || !request->controls->take ||
        !request->controls->complete || !request->controls->readback)) return false;
    *result = {};
    result->step = NezhaNativeProfile;
    const bool pattern = request->test_pattern;
    if (!pattern && (request->cci_queue_words != NezhaCameraCciCanoeQueue0Words ||
        !nezha_camera_profile_open(request->profile, request->profile_bytes, &state.profile) || !state.profile.vendor)) return false;
    const auto *profile = pattern ? nullptr : &state.profile.phy;
    const uint32_t width = pattern ? 640 : state.profile.width;
    const uint32_t height = pattern ? 480 : state.profile.height;
    const size_t sample_bytes = size_t(width) * height * 5 / 4;
    if (request->raw_sample && request->raw_sample_capacity < sample_bytes) return false;
    state.used = true;
    state.stopped = 0;
    state.cancelled = false;
    state.completed_time = 0;
    state.replacement_time = 0;
    state.replacement_pending = false;
    state.delivery_frame = nullptr;
    state.delivery_active = false;
    state.copy_service_status = NezhaFramesOk;
    state.work = {};
    if (!checkpoint(*request, *result, NezhaNativeSharedOwners, physical[Camcc], physical[Csid], physical[Vfe])) {
        if (state.cancelled) {
            state.used = false;
            result->passed = result->cleaned = true;
            result->cleanup_step = NezhaNativeCleanupDone;
        }
        return result->passed;
    }
    result->step = NezhaNativeRegisterMap;
    for (unsigned i = 0; i < sizeof(physical) / sizeof(physical[0]); ++i) {
        if ((pattern && (i == Phy || i == Cci)) || (!pattern && i == Tpg)) continue;
        if (!state.window[i].address) {
            state.window[i] = {ml_io_map(physical[i], sizes[i]), sizes[i]};
            if (!state.window[i].address) {
                result->status = physical[i];
                return false;
            }
            // Unaligned DT apertures retain a view of a permanent map.
            if (i == Crmc) { state.window[i].address += 0x600; state.window[i].bytes -= 0x600; }
            if (i == CrmCommon) { state.window[i].address += 0xc00; state.window[i].bytes -= 0xc00; }
        }
        state.reg[i] = {&state.window[i], read, write, wait};
    }
    state.reg[Cci].wait_ms=wait_controls;
    if (!pattern) {
        if (!state.gpio_window.address) {
            const vm_offset_t address = ml_io_map(NezhaCameraGpioPhysical, NezhaCameraGpioBytes);
            if (address) state.gpio_window = {address - NezhaCameraGpioOffset,
                NezhaCameraGpioBytes + NezhaCameraGpioOffset};
        }
        if (!state.mclk_window.address) state.mclk_window = {ml_io_map(0x1764000, 0x1000), 0x1000};
        if (!state.gpio_window.address || !state.mclk_window.address) {
            result->status = !state.gpio_window.address ? NezhaCameraGpioPhysical : 0x1764000;
            return false;
        }
    }
    const NezhaCameraRegisterIo gpio{&state.gpio_window, read, write, wait}, mclk{&state.mclk_window, read, write, wait};
    result->step = NezhaNativeGccOwner;
    if (!nezha_camera_native_gcc(&state.gcc)) return false;
    result->step = NezhaNativeRpmhOwner;
    display_state("before-boot-proxy");
    if (!nezha_camera_native_rpmh_open(&state.rpmh, request->command_database, request->database_bytes)) {
        if (state.rpmh && nezha_camera_native_rpmh_close(&state.rpmh)) {
            state.used = false;
            result->cleaned = true;
            result->cleanup_step = NezhaNativeCleanupDone;
        }
        return false;
    }
    display_state("after-boot-proxy");
    state.rpmh->before_transaction = request->rpmh_checkpoint;
    state.rpmh->vote_acknowledged = request->rpmh_vote_acknowledged;
    state.rpmh->isolated_sensor_votes = !pattern && request->isolated_sensor_votes;
    const NezhaCameraMainRailIo rails{state.rpmh, nezha_camera_rpmh_acquire, nezha_camera_rpmh_release};
    state.phy = {state.reg[Phy], wait_us};
    state.resources_io = {&state.gcc, &state.reg[Camcc], &state.reg[Crm], &state.reg[Crmc],
        &state.reg[CrmCommon], &state.reg[IfeRsc], &state.reg[Cpas], &state.phy, state.rpmh};
    state.camss = {&state, write_csid, write_vfe};
    state.capture_io = {};
    state.capture_io.cci = pattern ? nullptr : &state.reg[Cci];
    state.capture_io.tpg = pattern ? &state.reg[Tpg] : nullptr;
    state.capture_io.camss = &state.camss;
    state.capture_io.cci_queue_words = request->cci_queue_words;
    bool okay = false;
    size_t writes = 0;
    do {
        // Qualcomm CAMSS v24 Nord HF MNOC reference for 4096x3072
        // at 30 fps. CAMNOC's separate clock floor is applied by CRM.
        constexpr uint32_t average = 471860000, peak = 925857000;
        result->step = NezhaNativeIfeBandwidth;
        if (!nezha_camera_ife_bandwidth_checked(request->command_database, request->database_bytes,
            average, peak, average, peak, &state.bandwidth, &result->bandwidth_failure) ||
            !nezha_camera_ife_bandwidth_checked(request->command_database, request->database_bytes,
                0, 0, 0, 0, &state.clear, &result->bandwidth_failure)) {
            result->status = result->bandwidth_failure.issue;
            break;
        }
        if (!checkpoint(*request, *result, NezhaNativeResources, physical[Crm], physical[IfeRsc], physical[Cpas]) ||
            !nezha_camera_capture_resources_start(&state.resources_io, &state.bandwidth, profile, &result->resources)) break;
        if (!pattern) {
            if (!checkpoint(*request, *result, NezhaNativePins, NezhaCameraGpioPhysical, 0x1764000, 0) ||
                !nezha_camera_main_hold_reset(&gpio, &state.pins) ||
                !nezha_camera_main_start_pins(&gpio, &mclk, &rails, &state.pins)) break;
            if (!checkpoint(*request, *result, NezhaNativeCci, physical[Cci], 37500000, 1) ||
                !prepare_cci(state, *result)) break;
            if (!checkpoint(*request, *result, NezhaNativeRevision, 0x51, 0x10, 0x900e)) break;
            auto &cci = state.reg[Cci];
            auto &identity = state.capture.identity;
            auto &revision = result->revision;
            result->status = nezha_camera_ovx10500u_read_identity(&cci, &identity);
            state.capture.sensor_transfer = identity.transfer;
            const unsigned chip = (unsigned)identity.bytes[0] << 8 | identity.bytes[1];
            if (result->status != NezhaCciOk || chip != NezhaCameraOvx10500uId) break;
            result->status = nezha_camera_ovx10500u_read_revision(&cci, &revision);
            state.capture.sensor_transfer = revision.transfer;
            state.capture.stream_may_be_on = revision.stream_may_be_on;
            if (result->status != NezhaCciOk) break;
            if (!nezha_camera_profile_revision_writes(&state.profile, revision.vendor, revision.version,
                state.writes, sizeof(state.writes) / sizeof(state.writes[0]), &writes)) {
                result->status = NezhaCciInvalid;
                break;
            }
            if (!checkpoint(*request, *result, NezhaNativeRevision, revision.vendor,
                revision.version, (unsigned)writes)) break;
            if (request->AF_readonly_probe) {
                result->AF_readonly_attempted=true;
                result->AF_eeprom_status=nezha_camera_eeprom_read_af(&cci,&result->AF_eeprom);
                printf("NEZHA CAMERA: AF readonly EEPROM status=%u bytes=%u checksum=%u infinity=%u macro=%u\n",
                    (unsigned)result->AF_eeprom_status,result->AF_eeprom.bytes_read,
                    (unsigned)result->AF_eeprom.AF_checksum_valid,result->AF_eeprom.infinity_dac,
                    result->AF_eeprom.macro_dac);
                if (result->AF_eeprom_status!=NezhaCciOk) {
                    state.capture.sensor_transfer=result->AF_eeprom.transfer;
                    state.capture.sensor_status=result->AF_eeprom_status;
                    result->status=result->AF_eeprom_status;
                    break;
                }
                result->AF_position_status=nezha_camera_ak7316_read_position(&cci,&result->AF_position);
                printf("NEZHA CAMERA: AF readonly position status=%u DAC=%u raw=%#x read=%u\n",
                    (unsigned)result->AF_position_status,result->AF_position.position_dac,
                    result->AF_position.register_value,(unsigned)result->AF_position.position_read);
                if (result->AF_position_status!=NezhaCciOk) {
                    state.capture.sensor_transfer=result->AF_position.transfer;
                    state.capture.sensor_status=result->AF_position_status;
                    result->status=result->AF_position_status;
                    break;
                }
            }
        }
        uint32_t reset;
        if (!checkpoint(*request, *result, NezhaNativeCsid, physical[Csid], physical[Vfe], 0) ||
            !nezha_camera1080_prepare_csid(&state.reg[Csid], &reset)) break;
        if (!checkpoint(*request, *result, NezhaNativeIrq, reset, 0, 0) ||
            !nezha_camera_native_irq_open(&state.irq, &state.reg[Csid], &state.reg[Vfe], &result->irq_setup)) {
            result->status = result->irq_setup.status;
            break;
        }
        unsigned physical_bits;
        if (!checkpoint(*request, *result, NezhaNativeSmmu, request->slot, request->bank, request->asid)) break;
        result->status = nezha_camera_native_smmu_prepare(&state.smmu, request->slot, request->bank,
            request->asid, &physical_bits);
        if (result->status != ArmSmmu4KSuccess) break;
        if (!checkpoint(*request, *result, NezhaNativeMemory, physical_bits, width, height)) break;
        const bool prepared = nezha_camera_native_memory_prepare(&state.memory, physical_bits, width, height);
        const bool held = prepared && nezha_camera_native_memory_hold(state.memory);
        nezha_camera_native_memory_failure(&result->memory_failure);
        if (!held) {
            result->status = result->memory_failure.status;
            break;
        }
        printf("NEZHA CAMERA: native memory ready reused=%u raw-write-combined=%u\n",
            (unsigned)result->memory_failure.reused, (unsigned)result->memory_failure.raw_write_combined);
        result->status = nezha_camera_native_smmu_bind(state.smmu, state.memory);
        if (result->status != ArmSmmu4KSuccess) break;
        state.capture_io.smmu = nezha_camera_native_smmu_io(state.smmu);
        state.capture_io.dma_owner = state.smmu;
        state.capture_io.dma_enter = nezha_camera_native_smmu_capture_enter;
        state.capture_io.dma_leave = nezha_camera_native_smmu_capture_leave;
        if (!checkpoint(*request, *result, NezhaNativeStream, pattern ? physical[Tpg] : physical[Phy], width, height)) break;
        if (!acquire_vfe_bus(state)) {
            result->status = VfeBusCgc;
            break;
        }
        state.dma_touched = true;
        const NezhaCamera1080Input input = pattern ? NezhaCamera1080Input{NezhaCamera1080Tpg, 2, 0, {}, false} :
            NezhaCamera1080Input{NezhaCamera1080Cphy, 0, 3, {0, 1, 2, 0}, state.profile.epd_mode};
        const NezhaCameraMainMode mode{NezhaCameraOvx10500uId, width, height,
            state.writes, writes, input};
        NezhaCameraStandbyControls initial{};
        state.capture_io.standby_controls = request->controls ? &initial : nullptr;
        const bool started = nezha_camera_main_capture_start(&state.capture_io, &mode,
            nezha_camera_native_memory_plan(state.memory), nezha_camera_native_smmu_context(state.smmu),
            request->slot, request->bank, 100, &state.capture);
        state.capture_io.standby_controls = nullptr;
        if (initial.attempted) {
            result->control_readback = initial.sample;
            result->control_readback_status = initial.status;
            result->control_readback_before_stream = true;
            printf("NEZHA CAMERA: native initial controls before-stream=1 status=%u bytes=%u actual=%u/%u/%u/%u\n",
                (unsigned)initial.status, initial.sample.bytes_read, initial.sample.exposure_lines,
                initial.sample.frame_lines, initial.sample.analog_gain_q8, initial.sample.digital_gain_q10);
            if (initial.status != NezhaCciOk) result->status = NezhaFramesIo;
        }
        if (!started) break;
        if (!pattern) {
            uint8_t running = 0;
            NezhaCameraCciObservation observed{};
            result->stream_start_readback_status = nezha_camera_cci_read_byte(&state.reg[Cci], 1, 0x10,
                0x0100, &running, &observed);
            result->stream_start_readback = running;
            if (observed.needs_recovery) state.capture.sensor_transfer = observed;
            if (result->stream_start_readback_status != NezhaCciOk) {
                result->status = result->stream_start_readback_status;
                break;
            }
        }
        // Streaming has already begun. Avoid a filesystem sync before
        // consuming the two-frame queue.
        result->step = NezhaNativeFrames;
        const NezhaCameraFrameMemoryIo memory{&state, frame_acquire, frame_return, frame_fault, frame_next};
        if (request->consumer) {
            const NezhaCameraFrameConsumer client{const_cast<NezhaCameraNativeCaptureRequest *>(request),
                client_continuing, client_deliver};
            __atomic_store_n(&state.running, true, __ATOMIC_RELEASE);
            result->status = NezhaFramesOk;
            uint32_t read_controls_after=0;
            bool control_waiting=false;
            state.control_request=request;
            state.control_frames=0;
            state.control_frame_status=NezhaFramesOk;
            while (client_continuing(client.context)) {
                result->status = nezha_camera_frame_consume(&state.capture_io, &state.capture,
                    &state.reg[Csid], &memory, &client, 1000);
                if (result->status != NezhaFramesOk) {
                    if (state.copy_service_status != NezhaFramesOk) result->status = state.copy_service_status;
                    break;
                }
                if (request->controls && client_continuing(client.context)) {
                    /* Initial settings were actually read after loading
                     * the mode, while DMA and the sensor were stopped.
                     * Qualify that unchanged snapshot only after a real
                     * frame; keep the adapter's two additional boundaries.
                     * A new group still gets two completed-frame boundaries
                     * before its real register read, as before. */
                    if (!result->control_readback_verified ||
                        (control_waiting && state.capture.completed_frames>=read_controls_after)) {
                        if (result->control_readback_verified || !result->control_readback_before_stream) {
                            const uint64_t before = mach_absolute_time();
                            result->control_readback_status=nezha_camera_ovx10500u_read_controls(
                                &state.reg[Cci],&result->control_readback);
                            const uint64_t after = mach_absolute_time();
                            if (after >= before && after - before > state.work.control_read_max)
                                state.work.control_read_max = after - before;
                        }
                        result->control_readback_after=state.capture.completed_frames;
                        if (result->control_readback_status!=NezhaCciOk) {
                            state.capture.sensor_transfer=result->control_readback.transfer;
                            state.capture.sensor_status=result->control_readback_status;
                            result->status=NezhaFramesIo;
                            break;
                        }
                        if (state.control_frame_status!=NezhaFramesOk) {
                            result->status=state.control_frame_status;
                            break;
                        }
                        result->control_readback_verified=true;
                        if (control_waiting && (result->control_readback.exposure_lines!=result->control_result.plan.exposure_lines ||
                            result->control_readback.frame_lines!=result->control_result.plan.frame_lines ||
                            result->control_readback.analog_gain_q8!=result->control_result.plan.analog_gain_q8 ||
                            result->control_readback.digital_gain_q10!=result->control_result.plan.digital_gain_q10)) {
                            result->status=NezhaFramesIo;
                            break;
                        }
                        request->controls->readback(request->controls->context,
                            &result->control_readback,result->control_readback_after);
                        control_waiting=false;
                    }
                    if (control_waiting) continue;
                    NezhaCameraOvxControls controls{};
                    if (request->controls->take(request->controls->context, &controls)) {
                        result->control_attempted = true;
                        result->control_completed_before = state.capture.completed_frames;
                        const uint64_t before = mach_absolute_time();
                        result->control_status = nezha_camera_ovx10500u_apply_controls(
                            &state.reg[Cci], &controls, &result->control_result);
                        const uint64_t after = mach_absolute_time();
                        if (after >= before && after - before > state.work.control_write_max)
                            state.work.control_write_max = after - before;
                        request->controls->complete(request->controls->context,
                            result->control_status, &result->control_result, result->control_completed_before);
                        if (result->control_status != NezhaCciOk) {
                            state.capture.sensor_transfer = result->control_result.transfer;
                            state.capture.sensor_status = result->control_status;
                            result->status = NezhaFramesIo;
                            break;
                        }
                        if (state.control_frame_status!=NezhaFramesOk) {
                            result->status=state.control_frame_status;
                            break;
                        }
                        read_controls_after=state.capture.completed_frames+2;
                        control_waiting=true;
                    }
                }
            }
            state.control_request=nullptr;
            result->control_serviced_frames=state.control_frames;
            result->frames.frames = state.capture.completed_frames;
        } else {
            result->status = nezha_camera_frame_probe(&state.capture_io, &state.capture, &state.reg[Csid],
                &state.reg[Vfe], &memory, 20, 3000, &result->frames);
        }
        okay = result->status == NezhaFramesOk;
    } while (false);
    if (!okay && state.capture.stream_may_be_on && state.capture.rdi_started) {
        /* Use the existing console result after cleanup, retaining this
         * pre-stop observation. No IRQ acknowledgement or poll fallback. */
        static constexpr uint32_t offsets[] = {
            0x880, 0x884, 0x180, 0x1b0, 0x224, 0x1a0,
            0x8c0, 0x8c4, 0x8c8, 0x5480, 0x5488, 0x5494,
        };
        for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i)
            result->capture_registers[i] = read(&state.window[Csid], offsets[i]);
        result->capture_registers[12] = read(&state.window[Vfe], 0x2c00);
        result->capture_registers[13] = read(&state.window[Vfe], VfeBusCgc);
        if (!pattern && !state.capture.sensor_transfer.needs_recovery) {
            uint8_t running = 0;
            NezhaCameraCciObservation observed{};
            result->stream_readback_status = nezha_camera_cci_read_byte(&state.reg[Cci], 1, 0x10,
                0x0100, &running, &observed);
            result->stream_readback = running;
            if (observed.needs_recovery) state.capture.sensor_transfer = observed;
        }
    }
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
    result->work = state.work;
    if (!result->cleaned && result->cleanup_step == NezhaNativeReleaseMemory)
        nezha_camera_native_memory_failure(&result->memory_failure);
    result->passed = (okay || state.cancelled) && result->cleaned && !result->irq_fault && !result->frames.fault &&
        (!request->raw_sample || result->raw_sample_bytes == sample_bytes);
    __atomic_store_n(&state.running, false, __ATOMIC_RELEASE);
    if (request->consumer && result->passed) {
        state.capture = {};
        state.pins = {};
        state.irq = nullptr;
        state.dma_touched = false;
        state.cci_touched = false;
        state.used = false;
    }
    return result->passed;
}

void nezha_camera_native_capture_request_stop(void)
{
    /* The normal worker performs all sensor/IRQ/DMA teardown; callers
     * only close its gate. A callback can request this without waiting
     * for the worker that is currently delivering its frame. */
    __atomic_store_n(&state.stopped, 1u, __ATOMIC_RELEASE);
}

bool nezha_camera_native_capture_service_copy(const uint8_t *completed_frame)
{
    if (!state.delivery_active || !completed_frame || state.delivery_frame != completed_frame ||
        state.copy_service_status != NezhaFramesOk || !state.control_request ||
        __atomic_load_n(&state.stopped, __ATOMIC_ACQUIRE)) return false;
    /* Reuse exactly the original worker's completed-frame/cache/return
     * logic. This cannot recursively call the image consumer or CCI. */
    const uint64_t outer_time = state.completed_time;
    const NezhaCameraFrameMemoryIo memory{&state, frame_acquire, frame_return, frame_fault, frame_next};
    const NezhaCameraFrameConsumer consumer{const_cast<NezhaCameraNativeCaptureRequest *>(state.control_request),
        client_continuing, nullptr};
    unsigned recycled = 0;
    const uint64_t before = mach_absolute_time();
    state.copy_service_status = nezha_camera_frame_recycle_during_copy(&state.capture_io, &state.capture,
        &memory, &consumer, state.delivery_index, &recycled);
    const uint64_t after = mach_absolute_time();
    state.completed_time = outer_time;
    ++state.work.copy_service_calls;
    state.work.copy_recycled_frames += recycled;
    if (after >= before && after - before > state.work.copy_service_max)
        state.work.copy_service_max = after - before;
    return state.copy_service_status == NezhaFramesOk && client_continuing(consumer.context);
}

bool nezha_camera_native_capture_running(void)
{
    return __atomic_load_n(&state.running, __ATOMIC_ACQUIRE);
}
