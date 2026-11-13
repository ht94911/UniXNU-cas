/* SPDX-License-Identifier: MIT */
#include "NezhaCameraIrqQueue.h"

bool nezha_camera_irq_push(struct NezhaCameraIrqQueue *queue, const struct NezhaCamera1080Events *event)
{
    if (!queue || !event) return false;
    const uint32_t produced = __atomic_load_n(&queue->produced, __ATOMIC_RELAXED);
    const uint32_t consumed = __atomic_load_n(&queue->consumed, __ATOMIC_ACQUIRE);
    if ((uint32_t)(produced - consumed) >= NezhaCameraIrqSlots) {
        __atomic_store_n(&queue->overflow, 1, __ATOMIC_RELEASE);
        return false;
    }
    queue->event[produced % NezhaCameraIrqSlots] = *event;
    __atomic_store_n(&queue->produced, produced + 1u, __ATOMIC_RELEASE);
    return true;
}

bool nezha_camera_irq_pop(struct NezhaCameraIrqQueue *queue, struct NezhaCamera1080Events *event)
{
    if (!queue || !event || nezha_camera_irq_overflow(queue)) return false;
    const uint32_t consumed = __atomic_load_n(&queue->consumed, __ATOMIC_RELAXED);
    const uint32_t produced = __atomic_load_n(&queue->produced, __ATOMIC_ACQUIRE);
    if (produced == consumed) return false;
    *event = queue->event[consumed % NezhaCameraIrqSlots];
    __atomic_store_n(&queue->consumed, consumed + 1u, __ATOMIC_RELEASE);
    return true;
}

bool nezha_camera_irq_overflow(const struct NezhaCameraIrqQueue *queue)
{
    return !queue || __atomic_load_n(&queue->overflow, __ATOMIC_ACQUIRE) != 0;
}
