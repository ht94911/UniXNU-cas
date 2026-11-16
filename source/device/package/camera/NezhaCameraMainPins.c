/* SPDX-License-Identifier: MIT */
#include "NezhaCameraMainPins.h"
#include "NezhaCameraCanoePins.h"

#include <stddef.h>

/* Stock cam-sensor0 and CCI0 master 1 pinctrl. Power timing is shared by
 * the four stock nezha OVX10500U module variants. */
enum {
    MainReset, MainLdo, MainMclk, MainSda, MainScl, MainAf,
    GpioStride = NezhaCameraGpioStride,
    GpioOutput = 1 << 1,
    GpioOutputEnable = 1 << 9,
    GpioFunction1 = 1 << 2,
    GpioConfigMask = 0x3ff,
    GpioExtendedPresent = 1 << 11,
    GpioExtendedEnable = 1 << 12,
    MclkCommand = 0,
    MclkConfig = 4,
    MclkBranch = 0x18,
    MclkUpdate = 1,
    MclkEnable = 1,
    MclkRateMask = 0x371f,
    MclkXo = 1,
};
static const unsigned pins[NezhaCameraMainPinCount] = {106, 107, 89, 111, 112, 116};
static const uint32_t clock_off = UINT32_C(1) << 31;

static bool registers_valid(const struct NezhaCameraRegisterIo *io)
{
    return io != NULL && io->read != NULL && io->write != NULL && io->wait_ms != NULL;
}

static bool observe(const struct NezhaCameraMainPins *state, enum NezhaCameraPinStep step,
    unsigned item, unsigned value)
{
    return state->observe.before == NULL || state->observe.before(state->observe.context, step, item, value);
}

static void update(const struct NezhaCameraRegisterIo *io, uint32_t offset,
                   uint32_t mask, uint32_t value)
{
    const uint32_t before = io->read(io->context, offset);
    const uint32_t after = (before & ~mask) | value;
    if (before != after) {
        io->write(io->context, offset, after);
    }
}

static bool wait_for(const struct NezhaCameraRegisterIo *io, uint32_t offset,
                     uint32_t mask, uint32_t value)
{
    for (unsigned ms = 0; ms < 100; ++ms) {
        if ((io->read(io->context, offset) & mask) == value) {
            return true;
        }
        io->wait_ms(io->context, 1);
    }
    return (io->read(io->context, offset) & mask) == value;
}

static bool set_pin(const struct NezhaCameraRegisterIo *io, unsigned index, uint32_t control,
                    bool high, struct NezhaCameraMainPins *state)
{
    const uint32_t offset = pins[index] * GpioStride;
    state->pins_changed |= 1u << index;
    if ((control & GpioOutputEnable) != 0) {
        update(io, offset + 4, GpioOutput, high ? GpioOutput : 0);
    }
    const bool extended = (io->read(io->context, offset) & GpioExtendedPresent) != 0;
    const uint32_t mask = GpioConfigMask | (extended ? GpioExtendedEnable : 0);
    const uint32_t expected = control | (extended ? GpioExtendedEnable : 0);
    update(io, offset, mask, expected);
    return (io->read(io->context, offset) & mask) == expected &&
        ((control & GpioOutputEnable) == 0 ||
         (io->read(io->context, offset + 4) & GpioOutput) == (high ? GpioOutput : 0));
}

static bool start_pin(const struct NezhaCameraRegisterIo *io, unsigned index, uint32_t control,
    bool high, struct NezhaCameraMainPins *state)
{
    return observe(state, NezhaCameraPinDrive, pins[index], control | (high ? 1u << 16 : 0)) &&
        set_pin(io, index, control, high, state);
}

static bool restore_pin(const struct NezhaCameraRegisterIo *tlmm, unsigned pin,
                        struct NezhaCameraMainPins *state)
{
    const unsigned flag = 1u << pin;
    if ((state->pins_changed & flag) == 0) {
        return true;
    }
    const uint32_t offset = pins[pin] * GpioStride;
    const uint32_t mask = GpioConfigMask | GpioExtendedEnable;
    if ((state->pin_control[pin] & GpioOutputEnable) != 0) {
        update(tlmm, offset + 4, GpioOutput, state->pin_output[pin]);
    }
    update(tlmm, offset, mask, state->pin_control[pin] & mask);
    update(tlmm, offset + 4, GpioOutput, state->pin_output[pin]);
    if ((tlmm->read(tlmm->context, offset) & mask) != (state->pin_control[pin] & mask) ||
        (tlmm->read(tlmm->context, offset + 4) & GpioOutput) != state->pin_output[pin]) {
        return false;
    }
    state->pins_changed &= ~flag;
    return true;
}

static bool acquire_rail(const struct NezhaCameraMainRailIo *rails, enum NezhaCameraMainRail rail,
                         unsigned millivolts, struct NezhaCameraMainPins *state)
{
    if (!observe(state, NezhaCameraPinRail, (unsigned)rail, millivolts)) {
        return false;
    }
    state->rails_requested |= 1u << rail;
    return rails->acquire(rails->context, rail, millivolts);
}

static bool release_rail(const struct NezhaCameraMainRailIo *rails, enum NezhaCameraMainRail rail,
                         struct NezhaCameraMainPins *state)
{
    const unsigned flag = 1u << rail;
    if ((state->rails_requested & flag) != 0) {
        if (!rails->release(rails->context, rail)) {
            return false;
        }
        state->rails_requested &= ~flag;
    }
    return true;
}

bool nezha_camera_main_hold_reset(const struct NezhaCameraRegisterIo *tlmm,
    struct NezhaCameraMainPins *state)
{
    if (!registers_valid(tlmm) || state == NULL || state->reset_held) {
        return false;
    }
    for (unsigned i = 0; i < NezhaCameraMainPinCount; ++i) {
        const uint32_t offset = pins[i] * GpioStride;
        if (!observe(state, NezhaCameraPinRead, pins[i], offset)) {
            return false;
        }
        state->pin_control[i] = tlmm->read(tlmm->context, offset);
        const uint32_t output = tlmm->read(tlmm->context, offset + 4);
        if (state->pin_control[i] == UINT32_MAX || output == UINT32_MAX) {
            return false;
        }
        state->pin_output[i] = output & GpioOutput;
    }
    if (!observe(state, NezhaCameraPinDrive, pins[MainReset], GpioOutputEnable)) {
        return false;
    }
    state->reset_held = true;
    return set_pin(tlmm, MainReset, GpioOutputEnable, false, state);
}

static bool start_mclk(const struct NezhaCameraRegisterIo *mclk, struct NezhaCameraMainPins *state)
{
    if ((mclk->read(mclk->context, MclkCommand) & MclkUpdate) != 0) {
        return false;
    }
    const uint32_t branch = mclk->read(mclk->context, MclkBranch);
    const uint32_t config = mclk->read(mclk->context, MclkConfig) & MclkRateMask;
    if (config != MclkXo) {
        if ((branch & (clock_off | MclkEnable)) != clock_off) {
            return false;
        }
        state->mclk_config = config;
        state->mclk_rate_owned = true;
    }
    /* Shared RCGs commit their update while the root is running. */
    if ((branch & MclkEnable) == 0) {
        state->mclk_gate_owned = true;
        update(mclk, MclkBranch, MclkEnable, MclkEnable);
    }
    if (!wait_for(mclk, MclkBranch, clock_off, 0)) {
        return false;
    }
    if (state->mclk_rate_owned) {
        update(mclk, MclkConfig, MclkRateMask, MclkXo);
        update(mclk, MclkCommand, MclkUpdate, MclkUpdate);
        if (!wait_for(mclk, MclkCommand, MclkUpdate, 0) ||
            (mclk->read(mclk->context, MclkConfig) & MclkRateMask) != MclkXo) {
            return false;
        }
    }
    return true;
}

bool nezha_camera_main_start_pins(const struct NezhaCameraRegisterIo *tlmm,
    const struct NezhaCameraRegisterIo *mclk, const struct NezhaCameraMainRailIo *rails,
    struct NezhaCameraMainPins *state)
{
    if (!registers_valid(tlmm) || !registers_valid(mclk) || rails == NULL ||
        rails->acquire == NULL || rails->release == NULL || state == NULL || !state->reset_held ||
        state->running || state->mclk_rate_owned || state->mclk_gate_owned || state->rails_requested != 0) {
        return false;
    }
    if (!acquire_rail(rails, NezhaCameraMainBob2, 3296, state)) {
        return false;
    }
    tlmm->wait_ms(tlmm->context, 1);
    if (!acquire_rail(rails, NezhaCameraMainBob1, 3400, state)) {
        return false;
    }
    tlmm->wait_ms(tlmm->context, 2);
    if (!start_pin(tlmm, MainAf, GpioOutputEnable, true, state)) {
        return false;
    }
    tlmm->wait_ms(tlmm->context, 10);
    if (!observe(state, NezhaCameraPinClock, 0, 19200000) || !start_mclk(mclk, state) ||
        !start_pin(tlmm, MainMclk, GpioFunction1, false, state)) {
        return false;
    }
    tlmm->wait_ms(tlmm->context, 2);
    /* VANA, VDIG and CUSTOM1 share this one physical enable. */
    if (!start_pin(tlmm, MainLdo, GpioOutputEnable, true, state)) {
        return false;
    }
    tlmm->wait_ms(tlmm->context, 1);
    if (!acquire_rail(rails, NezhaCameraMainVio, 1800, state)) {
        return false;
    }
    tlmm->wait_ms(tlmm->context, 3);
    if (!start_pin(tlmm, MainReset, GpioOutputEnable, true, state)) {
        return false;
    }
    tlmm->wait_ms(tlmm->context, 5);
    state->running = start_pin(tlmm, MainSda, GpioFunction1, false, state) &&
        start_pin(tlmm, MainScl, GpioFunction1, false, state);
    return state->running;
}

static bool stop_mclk(const struct NezhaCameraRegisterIo *mclk, struct NezhaCameraMainPins *state)
{
    if (state->mclk_rate_owned) {
        if (!wait_for(mclk, MclkCommand, MclkUpdate, 0)) {
            return false;
        }
        const uint32_t current = mclk->read(mclk->context, MclkConfig) & MclkRateMask;
        if (current != MclkXo && current != state->mclk_config) {
            return false;
        }
        update(mclk, MclkConfig, MclkRateMask, state->mclk_config);
        update(mclk, MclkCommand, MclkUpdate, MclkUpdate);
        if (!wait_for(mclk, MclkCommand, MclkUpdate, 0) ||
            (mclk->read(mclk->context, MclkConfig) & MclkRateMask) != state->mclk_config) {
            return false;
        }
        state->mclk_rate_owned = false;
    }
    if (state->mclk_gate_owned) {
        update(mclk, MclkBranch, MclkEnable, 0);
        if (!wait_for(mclk, MclkBranch, clock_off, clock_off)) {
            return false;
        }
        state->mclk_gate_owned = false;
    }
    return true;
}

bool nezha_camera_main_restore_pins(const struct NezhaCameraRegisterIo *tlmm,
    const struct NezhaCameraRegisterIo *mclk, const struct NezhaCameraMainRailIo *rails,
    struct NezhaCameraMainPins *state)
{
    if (!registers_valid(tlmm) || !registers_valid(mclk) || rails == NULL || rails->release == NULL ||
        state == NULL || !state->reset_held || !set_pin(tlmm, MainReset, GpioOutputEnable, false, state)) {
        return false;
    }
    state->running = false;
    tlmm->wait_ms(tlmm->context, 1);
    if (!restore_pin(tlmm, MainScl, state) || !restore_pin(tlmm, MainSda, state) ||
        !release_rail(rails, NezhaCameraMainVio, state) || !restore_pin(tlmm, MainLdo, state) ||
        !stop_mclk(mclk, state) || !restore_pin(tlmm, MainMclk, state) ||
        !restore_pin(tlmm, MainAf, state)) {
        return false;
    }
    tlmm->wait_ms(tlmm->context, 1);
    if (!release_rail(rails, NezhaCameraMainBob1, state) || !release_rail(rails, NezhaCameraMainBob2, state)) {
        return false;
    }
    tlmm->wait_ms(tlmm->context, 2);
    if (!restore_pin(tlmm, MainReset, state)) {
        return false;
    }
    *state = (struct NezhaCameraMainPins){0};
    return true;
}
