/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_NATIVE_IRQ_H
#define NEZHA_CAMERA_NATIVE_IRQ_H

#include "NezhaCameraIrqQueue.h"

#ifdef __cplusplus
extern "C" {
#endif

struct NezhaCameraNativeIrq;

/* Call after the owned CSID reset has completed, before RDI0 starts.
 * The supplied MMIO contexts remain valid for the callback's boot-long
 * lifetime, matching the selected ArmCpu SPI interface. */
bool nezha_camera_native_irq_open(struct NezhaCameraNativeIrq **owner,
    const struct NezhaCameraRegisterIo *csid, const struct NezhaCameraRegisterIo *vfe);
bool nezha_camera_native_irq_next(struct NezhaCameraNativeIrq *owner,
    struct NezhaCamera1080Events *event);
uint32_t nezha_camera_native_irq_fault(const struct NezhaCameraNativeIrq *owner);
/* Read the preserved terminal event after the handler has been stopped. */
bool nezha_camera_native_irq_failure(const struct NezhaCameraNativeIrq *owner,
    struct NezhaCamera1080Events *event);

/* Mask this stream's sources, then wait for the callback to leave MMIO.
 * The retained callback becomes inert; this does not detach the SPI. */
bool nezha_camera_native_irq_stop(struct NezhaCameraNativeIrq *owner);

#ifdef __cplusplus
}
#endif
#endif
