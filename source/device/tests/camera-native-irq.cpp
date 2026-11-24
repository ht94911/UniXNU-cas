/* SPDX-License-Identifier: MIT */
#include "NezhaCameraNativeIrq.h"
#include <kern/sched_prim.h>
#include <kern/clock.h>

#include <cassert>
#include <cstdio>
#include <cstring>

static void (*handler)(void *);
static void *irq_context;
static unsigned attaches;
static unsigned wait_arms, wait_blocks, wait_clears, wake_calls, wait_timeouts;
static unsigned inject_stage;
static event_t wait_channel;
static bool wait_registered, wait_notified, wait_refused;
static void inject_completion(void);

extern "C" thread_t current_thread(void) { return reinterpret_cast<thread_t>(1); }
extern "C" wait_result_t assert_wait_timeout(event_t event, int mode, uint32_t interval, uint32_t scale)
{
    assert(event && mode == THREAD_UNINT && interval == 1 && scale == NSEC_PER_MSEC);
    assert(!wait_registered);
    ++wait_arms;
    if (wait_refused) return THREAD_AWAKENED;
    // The producer can run after the empty check but before wait registration.
    if (inject_stage == 1) inject_completion();
    wait_channel = event;
    wait_registered = true;
    wait_notified = false;
    // It can also run after registration, before the worker's final recheck.
    if (inject_stage == 2) inject_completion();
    if (inject_stage == 4) nezha_camera_native_irq_wake();
    return THREAD_WAITING;
}
extern "C" int clear_wait(thread_t thread, wait_result_t result)
{
    assert(thread == current_thread() && result == THREAD_AWAKENED && wait_registered);
    ++wait_clears;
    wait_notified = true;
    return 0;
}
extern "C" int thread_wakeup_prim(event_t event, bool one, wait_result_t result)
{
    assert(event && !one && result == THREAD_AWAKENED);
    ++wake_calls;
    if (wait_registered && wait_channel == event) wait_notified = true;
    return 0;
}
extern "C" wait_result_t thread_block(thread_continue_t continuation)
{
    assert(continuation == THREAD_CONTINUE_NULL && wait_registered);
    ++wait_blocks;
    // Producer after final recheck, immediately before the actual scheduler block.
    if (inject_stage == 3) inject_completion();
    if (!wait_notified) ++wait_timeouts;
    wait_registered = false;
    return wait_notified ? THREAD_AWAKENED : 2;
}

extern "C" int armcpu_spi_attach(uint32_t id, bool edge, void (*callback)(void *), void *cookie,
    uint32_t *readback, uint64_t *route)
{
    assert(id == 633 && edge && !handler);
    handler = callback;
    irq_context = cookie;
    *readback = 7;
    *route = 0;
    ++attaches;
    return 0;
}

extern "C" void IOSleep(unsigned ms) { assert(ms == 1); }
static uint64_t completed_ticks;
extern "C" uint64_t mach_absolute_time(void) { return ++completed_ticks; }

struct Model {
    uint32_t top{}, rx{}, rdi{}, buffer{}, address{};
    uint32_t masks[4]{};
    unsigned reads{}, writes{};
    uint32_t refused_mask{};
};
static Model *active_model;
static void inject_completion(void)
{
    assert(active_model);
    active_model->top = 1u << 16 | 1u << 3;
    active_model->rdi = 1u << 23;
    active_model->buffer = 1u << 16;
    active_model->address = 0x123400;
    handler(irq_context);
}

static uint32_t read(void *cookie, uint32_t offset)
{
    auto &m = *static_cast<Model *>(cookie);
    ++m.reads;
    switch (offset) {
    case 0x180: return m.top;
    case 0x1a0: return m.buffer;
    case 0x1b0: return m.rx;
    case 0x224: return m.rdi;
    case 0x184: return m.masks[0];
    case 0x1a4: return m.masks[1];
    case 0x1b4: return m.masks[2];
    case 0x228: return m.masks[3];
    case 0x2c90: return m.address;
    default: assert(false); return UINT32_MAX;
    }
}

static void write(void *cookie, uint32_t offset, uint32_t value)
{
    auto &m = *static_cast<Model *>(cookie);
    ++m.writes;
    if (offset == m.refused_mask) return;
    switch (offset) {
    case 0x188: m.top &= ~value; break;
    case 0x1a8: m.buffer &= ~value; break;
    case 0x1b8: m.rx &= ~value; break;
    case 0x22c: m.rdi &= ~value; break;
    case 0x110: assert(value == 1); break;
    case 0x184: m.masks[0] = value; break;
    case 0x1a4: m.masks[1] = value; break;
    case 0x1b4: m.masks[2] = value; break;
    case 0x228: m.masks[3] = value; break;
    default: assert(false);
    }
}

int main(int argc, char **argv)
{
    Model model;
    active_model = &model;
    const NezhaCameraRegisterIo io = {&model, read, write, nullptr};
    NezhaCameraNativeIrq *owner = nullptr;
    NezhaCameraNativeIrqSetup setup{};
    model.masks[2] = 1;
    assert(!nezha_camera_native_irq_open(&owner, &io, &io, &setup) && !owner && !attaches);
    assert(setup.status == NezhaIrqSetupBusyMask && setup.offset == 0x1b4 && setup.observed == 1);
    model.masks[2] = 0;
    model.masks[0] = 1; // Stock stopped TOP retains the reset-done subscription.
    assert(nezha_camera_native_irq_open(&owner, &io, &io, &setup));
    NezhaCameraNativeIrq *active=owner;
    assert(!nezha_camera_native_irq_retire(&active) && active==owner);
    assert(attaches == 1 && model.masks[0] == ((1u << 16) | (1u << 3) | (1u << 2) | 1) &&
           model.masks[1] == (1u << 16));
    NezhaCamera1080Events event{};
    assert(nezha_camera_native_irq_next(owner, &event) && !event.buffer_done);
    // Exact Camera131 child word: RUP miss and informational SOF/VC-DT
    // state, without BUF_DONE. This must preserve the warning, clear the
    // interrupt and allow later completions without inventing one here.
    model.top = 1u << 16;
    model.rdi = 0x11110;
    handler(irq_context);
    assert(!nezha_camera_native_irq_fault(owner) && !model.top && !model.rdi);
    assert(nezha_camera_native_irq_next(owner, &event) && event.rdi0_irq == 0x11110);
    assert(event.start_of_frame && !event.updated && !event.buffer_done && !event.completed_time);
    model.top = 1u << 16;
    model.rdi = 1u << 8; // Standalone notice is preserved, with no frame.
    handler(irq_context);
    assert(!nezha_camera_native_irq_fault(owner) && !model.top && !model.rdi);
    assert(nezha_camera_native_irq_next(owner, &event) && event.rdi0_irq == (1u << 8));
    assert(!event.start_of_frame && !event.end_of_frame && !event.buffer_done && !event.updated);
    for (unsigned i = 0; i < 2; ++i) {
        model.top = 1u << 16 | 1u << 3 | 1u << 2;
        model.rx = 1u << 4; // Informational lane start.
        model.rdi = 1u << 23 | 1u << 16 | 1u << 12 | 1u << 9;
        model.buffer = 1u << 16;
        model.address = 0x10000u + i * 0x10000u;
        handler(irq_context);
        assert(!model.top && !model.rdi && !model.buffer && !model.rx);
    }
    for (unsigned i = 0; i < 2; ++i) {
        assert(nezha_camera_native_irq_next(owner, &event));
        assert(event.buffer_done && event.start_of_frame && event.end_of_frame && event.updated);
        assert(event.consumed_address == 0x10000u + i * 0x10000u);
        assert(event.completed_time == i+1);
    }
    assert(nezha_camera_native_irq_next(owner, &event) && !event.buffer_done);
    for (unsigned boundary = 1; boundary <= 3; ++boundary) {
        const unsigned timeouts_before = wait_timeouts;
        inject_stage = boundary;
        nezha_camera_native_irq_wait(owner, 1);
        assert(wait_timeouts == timeouts_before && !wait_registered);
        assert(nezha_camera_native_irq_next(owner, &event) && event.buffer_done &&
            event.consumed_address == 0x123400);
        assert(nezha_camera_native_irq_next(owner, &event) && !event.buffer_done);
    }
    inject_stage = 0;
    inject_completion();
    const unsigned pending_arms = wait_arms, ready_reads = model.reads, ready_writes = model.writes;
    nezha_camera_native_irq_wait(owner, 1);
    assert(wait_arms == pending_arms && model.reads == ready_reads && model.writes == ready_writes);
    assert(nezha_camera_native_irq_next(owner, &event) && event.buffer_done);
    inject_stage = 4;
    const unsigned cancel_timeouts = wait_timeouts;
    nezha_camera_native_irq_wait(owner, 1);
    assert(wait_timeouts == cancel_timeouts && !wait_registered);
    assert(nezha_camera_native_irq_next(owner, &event) && !event.buffer_done);
    inject_stage = 0;
    const unsigned empty_reads = model.reads, empty_writes = model.writes;
    nezha_camera_native_irq_wait(owner, 1);
    assert(wait_timeouts == 1 && model.reads == empty_reads && model.writes == empty_writes);
    wait_refused = true;
    const unsigned blocks = wait_blocks;
    nezha_camera_native_irq_wait(owner, 1);
    assert(wait_blocks == blocks && !wait_registered);
    wait_refused = false;
    const unsigned valid_arms = wait_arms;
    nezha_camera_native_irq_wait(nullptr, 1);
    nezha_camera_native_irq_wait(owner, 0);
    nezha_camera_native_irq_wait(owner, 1001);
    assert(wait_arms == valid_arms);
    assert(wait_clears >= 3 && wake_calls >= 8);
    puts("Native camera IRQ wait: queued completion, three sleep boundaries, Close wake, timeout and refused wait passed");
    assert(nezha_camera_native_irq_stop(owner));
    assert(model.masks[0] == 1 && model.masks[1] == 0 && model.masks[2] == 0 && model.masks[3] == 0);
    const unsigned stopped_reads=model.reads, stopped_writes=model.writes;
    const unsigned stopped_arms=wait_arms;
    nezha_camera_native_irq_wait(owner,1);
    assert(wait_arms==stopped_arms);
    handler(irq_context);
    assert(model.reads==stopped_reads && model.writes==stopped_writes);
    NezhaCameraNativeIrq *reopened=nullptr;
    model.refused_mask = 0x228;
    assert(!nezha_camera_native_irq_open(&reopened,&io,&io,&setup) && reopened==owner);
    assert(setup.status == NezhaIrqSetupMaskReadback && setup.offset == 0x228 &&
        setup.observed == 0 && setup.expected != 0);
    model.refused_mask = 0;
    assert(nezha_camera_native_irq_stop(owner) && model.masks[0] == 1);
    reopened=nullptr;
    assert(nezha_camera_native_irq_open(&reopened,&io,&io,&setup) && reopened==owner && attaches==1);
    assert(nezha_camera_native_irq_next(owner,&event) && !event.buffer_done);
    uint32_t fatal_rx = 1u << 12, fatal_rdi = 1u << 24;
    if (argc == 2 && !std::strcmp(argv[1], "overflow")) { fatal_rx = 0; fatal_rdi = 1u << 19; }
    if (argc == 2 && !std::strcmp(argv[1], "illegal-dt")) { fatal_rx = 0; fatal_rdi = 1u << 7; }
    model.rx = fatal_rx;
    model.rdi = fatal_rdi;
    model.top = 1u << 16 | 1u << 2;
    handler(irq_context);
    assert(nezha_camera_native_irq_fault(owner) == 2 && !nezha_camera_native_irq_next(owner, &event));
    const unsigned fault_arms=wait_arms;
    nezha_camera_native_irq_wait(owner,1);
    assert(wait_arms==fault_arms);
    assert(nezha_camera_native_irq_stop(owner));
    assert(nezha_camera_native_irq_failure(owner, &event) && event.rx_irq == fatal_rx &&
        event.rdi0_irq == fatal_rdi && !model.top && !model.rx && !model.rdi);
    reopened=nullptr;
    assert(!nezha_camera_native_irq_open(&reopened,&io,&io,&setup) && !reopened && attaches==1);
    const unsigned reads = model.reads, writes = model.writes;
    handler(irq_context);
    assert(model.reads == reads && model.writes == writes);
    assert(nezha_camera_native_irq_retire(&owner) && !owner);
    assert(model.reads == reads && model.writes == writes);
    assert(nezha_camera_native_irq_failure(static_cast<NezhaCameraNativeIrq *>(irq_context), &event));
    assert(event.rx_irq == fatal_rx && event.rdi0_irq == fatal_rdi);
    reopened=nullptr;
    model.masks[2]=1;
    assert(!nezha_camera_native_irq_open(&reopened,&io,&io,&setup) && !reopened);
    assert(setup.status==NezhaIrqSetupBusyMask);
    model.masks[2]=0;
    assert(nezha_camera_native_irq_open(&reopened,&io,&io,&setup) && attaches==1);
    assert(!nezha_camera_native_irq_fault(reopened));
    assert(nezha_camera_native_irq_next(reopened,&event) && !event.buffer_done);
    model.top=(1u<<16)|(1u<<3);
    model.rdi=1u<<12;model.buffer=1u<<16;model.address=0x30000;
    handler(irq_context);
    assert(nezha_camera_native_irq_next(reopened,&event) && event.buffer_done && event.consumed_address==0x30000);
    assert(nezha_camera_native_irq_stop(reopened));
    puts("Native CSID IRQ preserves RUP miss without stopping; ordered completions, real fatal errors and stopped MMIO remain protected");
    puts("Native retired IRQ permits a fresh reservation after caller's full teardown; unretired fault and busy masks still refuse");
}
