// SPDX-License-Identifier: BSD-2-Clause-Patent
// Contact-origin policy only. The caller serializes updates and supplies the
// selected IOHIDEvent's authenticated scalar accessors.
#pragma once
#include <stdint.h>

enum {
    NZ_EDGE_CONTACTS = 256,
    NZ_EDGE_TOUCH_CHANGED = 2,
    NZ_EDGE_FROM_TIP = 0x800,
    NZ_EDGE_TYPES = 0x43800,
};

struct nezha_touch_edge_state {
    uint32_t origin[NZ_EDGE_CONTACTS];
};

static inline uint32_t
nezha_touch_edge_mask(struct nezha_touch_edge_state *state, unsigned index,
                     uint32_t mask, int touching, int32_t x, int32_t y,
                     unsigned width, unsigned height)
{
    if (!state || index >= NZ_EDGE_CONTACTS || width < 96 || height < 96)
        return mask;

    // Only the contact's beginning chooses its origin. Moving into the edge
    // from an application's content must not turn into a system gesture.
    if (touching && (mask & NZ_EDGE_TOUCH_CHANGED)) {
        uint32_t origin = mask & NZ_EDGE_TYPES;
        if (!origin && x >= 0 && x <= 65536 && y >= 0 && y <= 65536) {
            const int32_t edge_x = (int32_t)(48U * 65536U / width);
            const int32_t edge_y = (int32_t)(48U * 65536U / height);
            if (x < edge_x || x > 65536 - edge_x ||
                y < edge_y || y > 65536 - edge_y)
                origin = NZ_EDGE_FROM_TIP;
        }
        state->origin[index] = origin;
    }

    // Preserve a classification already supplied by the original driver.
    // Otherwise keep this contact's origin through motion and its final lift.
    const uint32_t result = mask | ((mask & NZ_EDGE_TYPES) ? 0 : state->origin[index]);
    if (!touching && (mask & NZ_EDGE_TOUCH_CHANGED))
        state->origin[index] = 0;
    return result;
}
