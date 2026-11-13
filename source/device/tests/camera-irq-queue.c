/* SPDX-License-Identifier: MIT */
#include "NezhaCameraIrqQueue.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct NezhaCameraIrqQueue queue = {0};
    struct NezhaCamera1080Events event;
    assert(!nezha_camera_irq_pop(&queue, &event));
    for (unsigned i = 0; i < NezhaCameraIrqSlots; ++i) {
        const struct NezhaCamera1080Events next = {.consumed_address = 0x10000u + i, .buffer_done = true};
        assert(nezha_camera_irq_push(&queue, &next));
    }
    for (unsigned i = 0; i < NezhaCameraIrqSlots; ++i) {
        assert(nezha_camera_irq_pop(&queue, &event));
        assert(event.consumed_address == 0x10000u + i && event.buffer_done);
    }
    assert(!nezha_camera_irq_pop(&queue, &event));
    queue.produced = queue.consumed = UINT32_MAX - 1;
    for (unsigned i = 0; i < 1000; ++i) {
        event.consumed_address = i;
        assert(nezha_camera_irq_push(&queue, &event));
        assert(nezha_camera_irq_pop(&queue, &event) && event.consumed_address == i);
    }
    for (unsigned i = 0; i < NezhaCameraIrqSlots; ++i) assert(nezha_camera_irq_push(&queue, &event));
    assert(!nezha_camera_irq_push(&queue, &event));
    assert(nezha_camera_irq_overflow(&queue) && !nezha_camera_irq_pop(&queue, &event));
    puts("Camera IRQ queue preserves completion addresses across wrap and stops acceptance on overflow");
}
