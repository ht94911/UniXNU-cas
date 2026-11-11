// SPDX-License-Identifier: BSD-2-Clause-Patent
// One explicitly marked, fixed Settings tap through the normal HID consumer.
#pragma once
#include "NezhaInput.h"

enum nezha_test_result { NZ_TEST_NONE, NZ_TEST_FRAME, NZ_TEST_HOLD };
struct nezha_touch_test {
    uint64_t pressed_at;
    unsigned phase;
};

static inline enum nezha_test_result
nezha_test_next(struct nezha_touch_test *test, uint64_t now, uint64_t hold_ticks,
               bool due, bool physical_active, bool consumer_idle,
               struct nezha_touch_frame *out) {
    if (!test || !out || !hold_ticks || test->phase == 2) return NZ_TEST_NONE;
    if (!test->phase) {
        if (!due || physical_active || !consumer_idle) return NZ_TEST_NONE;
        test->phase = 1;
        test->pressed_at = now;
        *out = (struct nezha_touch_frame){0};
        out->sequence = 0xf0000001;
        out->active = 1;
        out->x[0] = 444;
        out->y[0] = 1872;
        return NZ_TEST_FRAME;
    }
    if (physical_active || now - test->pressed_at >= hold_ticks) {
        test->phase = 2;
        *out = (struct nezha_touch_frame){0};
        out->sequence = 0xf0000002;
        out->x[0] = 444;
        out->y[0] = 1872;
        return NZ_TEST_FRAME;
    }
    return NZ_TEST_HOLD;
}
