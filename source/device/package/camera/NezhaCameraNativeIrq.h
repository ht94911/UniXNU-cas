/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_NATIVE_IRQ_H
#define NEZHA_CAMERA_NATIVE_IRQ_H

#include "NezhaCameraIrqQueue.h"

#ifdef __cplusplus
extern "C" {
#endif

struct NezhaCameraNativeIrq;
enum NezhaCameraNativeIrqSetupStatus {
    NezhaIrqSetupOk, NezhaIrqSetupInvalid, NezhaIrqSetupBusyMask,
    NezhaIrqSetupAttach, NezhaIrqSetupSpiReadback, NezhaIrqSetupMaskReadback,
};
struct NezhaCameraNativeIrqSetup {
    enum NezhaCameraNativeIrqSetupStatus status;
    uint32_t offset, observed, expected;
};

enum { NezhaCameraNativeIrqFaultWords = 13 };
struct NezhaCameraNativeIrqFailureSnapshot {
    /* WM cfg/image/frame increment, consumed/last/fifo/current address,
     * limiter, bus CCIF/overflow, and CSID RUP/AUP/SET respectively.
     * Values are raw, not a decoded queue depth or hardware ACK. */
    uint32_t words[NezhaCameraNativeIrqFaultWords], valid_words;
    uint32_t produced, consumed;
    uint64_t fault_ticks;
    struct NezhaCamera1080Events last_completed;
};

/* Call after the owned CSID reset has completed, before RDI0 starts.
 * The supplied MMIO contexts remain valid for the callback's boot-long
 * lifetime, matching the selected ArmCpu SPI interface. */
bool nezha_camera_native_irq_open(struct NezhaCameraNativeIrq **owner,
    const struct NezhaCameraRegisterIo *csid, const struct NezhaCameraRegisterIo *vfe,
    struct NezhaCameraNativeIrqSetup *setup);
bool nezha_camera_native_irq_next(struct NezhaCameraNativeIrq *owner,
    struct NezhaCamera1080Events *event);
/* Sleep only while this owner's acknowledged event queue is empty.
 * The IRQ wakes the existing worker; arm/recheck closes the sleep race.
 * Timeout is still bounded, and this never reads or writes hardware. */
void nezha_camera_native_irq_wait(struct NezhaCameraNativeIrq *owner, unsigned milliseconds);
/* Close can wake the boot-long channel without dereferencing a changing
 * capture-owner pointer. The normal worker still performs all teardown. */
void nezha_camera_native_irq_wake(void);
uint32_t nezha_camera_native_irq_fault(const struct NezhaCameraNativeIrq *owner);
/* Read the preserved terminal event after the handler has been stopped. */
bool nezha_camera_native_irq_failure(const struct NezhaCameraNativeIrq *owner,
    struct NezhaCamera1080Events *event);
/* One fault-only readback, after IRQ status acknowledgement and before
 * source masking, while both maps are still powered. No normal-frame
 * polling, context selection, recovery or extra worker is introduced. */
bool nezha_camera_native_irq_failure_snapshot(const struct NezhaCameraNativeIrq *owner,
    struct NezhaCameraNativeIrqFailureSnapshot *snapshot);

/* Mask this stream's sources, then wait for the callback to leave MMIO.
 * The retained callback becomes inert; this does not detach the SPI. */
bool nezha_camera_native_irq_stop(struct NezhaCameraNativeIrq *owner);

/* The capture worker calls this only after source reset, CPAS drain,
 * DMA detach and all resource releases have succeeded. Preserve the old
 * terminal event until the next open; the boot-long callback stays inert. */
bool nezha_camera_native_irq_retire(struct NezhaCameraNativeIrq **owner);

#ifdef __cplusplus
}
#endif
#endif
