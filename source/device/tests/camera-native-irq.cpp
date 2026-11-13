/* SPDX-License-Identifier: MIT */
#include "NezhaCameraNativeIrq.h"

#include <cassert>
#include <cstdio>

static void (*handler)(void *);
static void *irq_context;
static unsigned attaches;

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

struct Model {
    uint32_t top{}, rx{}, rdi{}, buffer{}, address{};
    uint32_t masks[4]{};
    unsigned reads{}, writes{};
};

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
    switch (offset) {
    case 0x188: m.top &= ~value; break;
    case 0x1a8: m.buffer &= ~value; break;
    case 0x22c: m.rdi &= ~value; break;
    case 0x110: assert(value == 1); break;
    case 0x184: m.masks[0] = value; break;
    case 0x1a4: m.masks[1] = value; break;
    case 0x1b4: m.masks[2] = value; break;
    case 0x228: m.masks[3] = value; break;
    default: assert(false);
    }
}

int main()
{
    Model model;
    const NezhaCameraRegisterIo io = {&model, read, write, nullptr};
    NezhaCameraNativeIrq *owner = nullptr;
    model.masks[2] = 1;
    assert(!nezha_camera_native_irq_open(&owner, &io, &io) && !owner && !attaches);
    model.masks[2] = 0;
    assert(nezha_camera_native_irq_open(&owner, &io, &io));
    assert(attaches == 1 && model.masks[0] == ((1u << 16) | (1u << 3) | (1u << 2)) &&
           model.masks[1] == (1u << 16));
    NezhaCamera1080Events event{};
    assert(nezha_camera_native_irq_next(owner, &event) && !event.buffer_done);
    for (unsigned i = 0; i < 2; ++i) {
        model.top = 1u << 16 | 1u << 3;
        model.rx = 1u << 4; // Informational lane start.
        model.rdi = 1u << 23 | 1u << 12 | 1u << 9;
        model.buffer = 1u << 16;
        model.address = 0x10000u + i * 0x10000u;
        handler(irq_context);
        assert(!model.top && !model.rdi && !model.buffer);
    }
    for (unsigned i = 0; i < 2; ++i) {
        assert(nezha_camera_native_irq_next(owner, &event));
        assert(event.buffer_done && event.start_of_frame && event.end_of_frame && event.updated);
        assert(event.consumed_address == 0x10000u + i * 0x10000u);
    }
    assert(nezha_camera_native_irq_next(owner, &event) && !event.buffer_done);
    model.rx = 1u << 12; // Lost EOT on lane 0.
    handler(irq_context);
    assert(nezha_camera_native_irq_fault(owner) == 2 && !nezha_camera_native_irq_next(owner, &event));
    assert(nezha_camera_native_irq_stop(owner));
    assert(nezha_camera_native_irq_failure(owner, &event) && event.rx_irq == (1u << 12));
    const unsigned reads = model.reads, writes = model.writes;
    handler(irq_context);
    assert(model.reads == reads && model.writes == writes);
    puts("Native CSID IRQ delivers ordered completion addresses and does not touch MMIO after stop");
}
