/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_IRQ_QUEUE_H
#define NEZHA_CAMERA_IRQ_QUEUE_H

#include "NezhaCameraCamss1080.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { NezhaCameraIrqSlots = 32 };

struct NezhaCameraIrqQueue {
    struct NezhaCamera1080Events event[NezhaCameraIrqSlots];
    uint32_t produced, consumed, overflow;
};

/* Single CSID IRQ producer and one normal capture worker. The producer
 * always records the hardware's consumed address with its event; frame
 * reads, cache operations and requeue happen only on the consumer. */
bool nezha_camera_irq_push(struct NezhaCameraIrqQueue *queue, const struct NezhaCamera1080Events *event);
bool nezha_camera_irq_pop(struct NezhaCameraIrqQueue *queue, struct NezhaCamera1080Events *event);
bool nezha_camera_irq_overflow(const struct NezhaCameraIrqQueue *queue);

#ifdef __cplusplus
}
#endif
#endif
