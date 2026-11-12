/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_MAIN_PINS_H
#define NEZHA_CAMERA_MAIN_PINS_H

#include "NezhaCameraRegisterIo.h"
#include <stdbool.h>

enum { NezhaCameraMainPinCount = 6 };

enum NezhaCameraMainRail {
    NezhaCameraMainBob2,
    NezhaCameraMainBob1,
    NezhaCameraMainVio,
};

struct NezhaCameraMainRailIo {
    void *context;
    bool (*acquire)(void *context, enum NezhaCameraMainRail rail, unsigned millivolts);
    /* Also resolves an attempted acquisition with an uncertain result. */
    bool (*release)(void *context, enum NezhaCameraMainRail rail);
};

struct NezhaCameraMainPins {
    uint32_t pin_control[NezhaCameraMainPinCount];
    uint32_t pin_output[NezhaCameraMainPinCount];
    uint32_t mclk_config;
    unsigned pins_changed;
    unsigned rails_requested;
    bool reset_held;
    bool mclk_rate_owned;
    bool mclk_gate_owned;
    bool running;
};

/* The owner supplies the stock nezha TLMM and MCLK0 apertures, with MCLK0
 * registers relative to CAMBISTMCLKCC + 0x4000. Acquire the six board pins
 * and the clock exclusively. Start with a zeroed state. */
bool nezha_camera_main_hold_reset(const struct NezhaCameraRegisterIo *tlmm,
    struct NezhaCameraMainPins *state);

/* Apply the stock module's rail, MCLK and reset order after holding reset.
 * The rail owner acknowledges each vote and owns restoration of its vote. */
bool nezha_camera_main_start_pins(const struct NezhaCameraRegisterIo *tlmm,
    const struct NezhaCameraRegisterIo *mclk, const struct NezhaCameraMainRailIo *rails,
    struct NezhaCameraMainPins *state);

/* The caller has drained capture and CCI before shutdown. A timeout leaves
 * the sensor in reset and retains the clock and pin ownership for recovery. */
bool nezha_camera_main_restore_pins(const struct NezhaCameraRegisterIo *tlmm,
    const struct NezhaCameraRegisterIo *mclk, const struct NezhaCameraMainRailIo *rails,
    struct NezhaCameraMainPins *state);

#endif
