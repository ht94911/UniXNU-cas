/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_MAIN_PINS_H
#define NEZHA_CAMERA_MAIN_PINS_H

#include "NezhaCameraRegisterIo.h"
#include <stdbool.h>

enum { NezhaCameraMainPinCount = 5 };

struct NezhaCameraMainPins {
    uint32_t pin_control[NezhaCameraMainPinCount];
    uint32_t pin_output[NezhaCameraMainPinCount];
    uint32_t mclk_config;
    bool reset_held;
    bool mclk_rate_owned;
    bool mclk_gate_owned;
    bool running;
};

/* The owner supplies the stock nezha TLMM and MCLK0 apertures, with MCLK0
 * registers relative to CAMBISTMCLKCC + 0x4000. Acquire the five board pins
 * and the clock exclusively. Start with a zeroed state. */
bool nezha_camera_main_hold_reset(const struct NezhaCameraRegisterIo *tlmm,
    struct NezhaCameraMainPins *state);

/* Called after the stock shared supply votes are acknowledged. Enables the
 * sensor-only LDO GPIO and the 19.2 MHz MCLK. AFVDD is handled by the lens path. */
bool nezha_camera_main_start_pins(const struct NezhaCameraRegisterIo *tlmm,
    const struct NezhaCameraRegisterIo *mclk, struct NezhaCameraMainPins *state);

/* The caller has drained capture and CCI before shutdown. A timeout leaves
 * the sensor in reset and retains the clock and pin ownership for recovery. */
bool nezha_camera_main_restore_pins(const struct NezhaCameraRegisterIo *tlmm,
    const struct NezhaCameraRegisterIo *mclk, struct NezhaCameraMainPins *state);

#endif
