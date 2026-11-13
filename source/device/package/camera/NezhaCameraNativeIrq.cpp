/* SPDX-License-Identifier: MIT */
#include "NezhaCameraNativeIrq.h"

#include <IOKit/IOLib.h>

extern "C" IOReturn armcpu_spi_attach(uint32_t, bool, void (*)(void *),
    void *, uint32_t *, uint64_t *);

namespace {
constexpr uint32_t TopMask = 0x184, BufferMask = 0x1a4, RxMask = 0x1b4, RdiMask = 0x228;
constexpr uint32_t TopEvents = (1u << 2) | (1u << 3) | (1u << 16);
constexpr uint32_t RdiEvents = (1u << 23) | (1u << 12) | (1u << 9) | (1u << 4) | (1u << 3);
// The stock 1080 common v1 table also exposes informational lane and
// line events. Those status bits can be set during a valid capture.
constexpr uint32_t RxErrors = 0x25fff000u | 0x08000000u;
constexpr uint32_t RdiErrors = 0x2c1c6001u | 0x12000000u | 0x180u;
constexpr uint32_t masks[] = {TopMask, BufferMask, RxMask, RdiMask};
}

struct NezhaCameraNativeIrq {
    NezhaCameraRegisterIo csid{}, vfe{};
    NezhaCameraIrqQueue queue{};
    NezhaCamera1080Events failure{};
    uint32_t fault{}, handlers{}, spi_readback{};
    uint64_t spi_route{};
    bool used{}, armed{};
};

static NezhaCameraNativeIrq irq;

static void stop_sources(NezhaCameraNativeIrq *owner)
{
    // The caller qualified an otherwise idle, exclusive CSID0 instance.
    for (uint32_t offset : masks) owner->csid.write(owner->csid.context, offset, 0);
}

static void interrupt(void *cookie)
{
    auto *owner = static_cast<NezhaCameraNativeIrq *>(cookie);
    __atomic_add_fetch(&owner->handlers, 1u, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&owner->armed, __ATOMIC_SEQ_CST)) {
        NezhaCamera1080Events event{};
        const bool read = nezha_camera1080_take_rdi0_events(&owner->csid, &owner->vfe, &event);
        const bool failed = (event.rx_irq & RxErrors) || (event.rdi0_irq & RdiErrors);
        const bool frame_event = event.updated || event.start_of_frame || event.end_of_frame || event.buffer_done;
        if (!read || failed || (frame_event && !nezha_camera_irq_push(&owner->queue, &event))) {
            owner->failure = event;
            __atomic_store_n(&owner->fault, !read ? 1u : failed ? 2u : 3u, __ATOMIC_RELEASE);
            stop_sources(owner);
            __atomic_store_n(&owner->armed, false, __ATOMIC_SEQ_CST);
        }
    }
    __atomic_sub_fetch(&owner->handlers, 1u, __ATOMIC_SEQ_CST);
}

bool nezha_camera_native_irq_open(NezhaCameraNativeIrq **out,
    const NezhaCameraRegisterIo *csid, const NezhaCameraRegisterIo *vfe)
{
    if (!out || *out || !csid || !csid->read || !csid->write || !vfe || !vfe->read || irq.used) {
        return false;
    }
    for (uint32_t offset : masks) if (csid->read(csid->context, offset) != 0) return false;
    irq.used = true;
    irq.csid = *csid;
    irq.vfe = *vfe;
    *out = &irq;
    // Stock CSID0 SPI 0x259, rising-edge. ArmCpu accepts the architectural INTID.
    if (armcpu_spi_attach(32u + 0x259u, true, interrupt, &irq, &irq.spi_readback, &irq.spi_route) != kIOReturnSuccess ||
        (irq.spi_readback & 7u) != 7u) return false;
    constexpr uint32_t BufferDone = 1u << 16;
    __atomic_store_n(&irq.armed, true, __ATOMIC_SEQ_CST);
    csid->write(csid->context, RdiMask, RdiEvents | RdiErrors);
    csid->write(csid->context, BufferMask, BufferDone);
    csid->write(csid->context, RxMask, RxErrors);
    csid->write(csid->context, TopMask, TopEvents);
    return csid->read(csid->context, RdiMask) == (RdiEvents | RdiErrors) &&
        csid->read(csid->context, BufferMask) == BufferDone &&
        csid->read(csid->context, RxMask) == RxErrors &&
        csid->read(csid->context, TopMask) == TopEvents;
}

bool nezha_camera_native_irq_next(NezhaCameraNativeIrq *owner, NezhaCamera1080Events *event)
{
    if (owner != &irq || !event || nezha_camera_native_irq_fault(owner)) return false;
    if (!nezha_camera_irq_pop(&owner->queue, event)) *event = {};
    return true;
}

uint32_t nezha_camera_native_irq_fault(const NezhaCameraNativeIrq *owner)
{
    return owner == &irq ? __atomic_load_n(&owner->fault, __ATOMIC_ACQUIRE) : UINT32_MAX;
}

bool nezha_camera_native_irq_failure(const NezhaCameraNativeIrq *owner, NezhaCamera1080Events *event)
{
    if (owner != &irq || !event || !nezha_camera_native_irq_fault(owner)) return false;
    *event = owner->failure;
    return true;
}

bool nezha_camera_native_irq_stop(NezhaCameraNativeIrq *owner)
{
    if (owner != &irq || !owner->used) return false;
    stop_sources(owner);
    __atomic_store_n(&owner->armed, false, __ATOMIC_SEQ_CST);
    for (unsigned ms = 0; ms <= 100; ++ms) {
        if (__atomic_load_n(&owner->handlers, __ATOMIC_SEQ_CST) == 0) {
            for (uint32_t offset : masks) if (owner->csid.read(owner->csid.context, offset) != 0) return false;
            return true;
        }
        if (ms < 100) IOSleep(1);
    }
    return false;
}
