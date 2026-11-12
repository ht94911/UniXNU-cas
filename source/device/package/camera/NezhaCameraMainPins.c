/* SPDX-License-Identifier: MIT */
#include "NezhaCameraMainPins.h"

#include <stddef.h>

/* Stock cam-sensor0 and CCI0 master 1 pinctrl. The reset/MCLK/LDO timing is
 * based on the same-device u0er ed8978ac0087 camera power sequence. */
enum {
    MainReset, MainLdo, MainMclk, MainSda, MainScl,
    GpioStride = 0x1000,
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
static const unsigned pins[NezhaCameraMainPinCount] = {106, 107, 89, 111, 112};
static const uint32_t clock_off = UINT32_C(1) << 31;

static bool registers_valid(const struct NezhaCameraRegisterIo *io)
{
    return io != NULL && io->read != NULL && io->write != NULL && io->wait_ms != NULL;
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
                    bool high)
{
    const uint32_t offset = pins[index] * GpioStride;
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

bool nezha_camera_main_hold_reset(const struct NezhaCameraRegisterIo *tlmm,
    struct NezhaCameraMainPins *state)
{
    if (!registers_valid(tlmm) || state == NULL || state->reset_held) {
        return false;
    }
    for (unsigned i = 0; i < NezhaCameraMainPinCount; ++i) {
        const uint32_t offset = pins[i] * GpioStride;
        state->pin_control[i] = tlmm->read(tlmm->context, offset);
        state->pin_output[i] = tlmm->read(tlmm->context, offset + 4) & GpioOutput;
    }
    state->reset_held = true;
    return set_pin(tlmm, MainReset, GpioOutputEnable, false);
}

bool nezha_camera_main_start_pins(const struct NezhaCameraRegisterIo *tlmm,
    const struct NezhaCameraRegisterIo *mclk, struct NezhaCameraMainPins *state)
{
    if (!registers_valid(tlmm) || !registers_valid(mclk) || state == NULL ||
        !state->reset_held || state->running || state->mclk_rate_owned || state->mclk_gate_owned ||
        (mclk->read(mclk->context, MclkCommand) & MclkUpdate) != 0) {
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
        update(mclk, MclkConfig, MclkRateMask, MclkXo);
        update(mclk, MclkCommand, MclkUpdate, MclkUpdate);
        if (!wait_for(mclk, MclkCommand, MclkUpdate, 0) ||
            (mclk->read(mclk->context, MclkConfig) & MclkRateMask) != MclkXo) {
            return false;
        }
    }
    if ((branch & MclkEnable) == 0) {
        state->mclk_gate_owned = true;
        update(mclk, MclkBranch, MclkEnable, MclkEnable);
    }
    if (!wait_for(mclk, MclkBranch, clock_off | MclkEnable, MclkEnable) ||
        !set_pin(tlmm, MainMclk, GpioFunction1, false) ||
        !set_pin(tlmm, MainLdo, GpioOutputEnable, true)) {
        return false;
    }
    tlmm->wait_ms(tlmm->context, 1);
    if (!set_pin(tlmm, MainReset, GpioOutputEnable, true)) {
        return false;
    }
    tlmm->wait_ms(tlmm->context, 18);
    state->running = set_pin(tlmm, MainSda, GpioFunction1, false) &&
        set_pin(tlmm, MainScl, GpioFunction1, false);
    return state->running;
}

bool nezha_camera_main_restore_pins(const struct NezhaCameraRegisterIo *tlmm,
    const struct NezhaCameraRegisterIo *mclk, struct NezhaCameraMainPins *state)
{
    if (!registers_valid(tlmm) || !registers_valid(mclk) || state == NULL || !state->reset_held ||
        !set_pin(tlmm, MainReset, GpioOutputEnable, false)) {
        return false;
    }
    state->running = false;
    if (state->mclk_gate_owned) {
        update(mclk, MclkBranch, MclkEnable, 0);
        if (!wait_for(mclk, MclkBranch, clock_off | MclkEnable, clock_off)) {
            return false;
        }
        state->mclk_gate_owned = false;
    }
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
    /* Restore reset last, once the stock enable and pinmux state is back. */
    for (unsigned i = NezhaCameraMainPinCount; i-- > 0;) {
        const uint32_t offset = pins[i] * GpioStride;
        const uint32_t mask = GpioConfigMask | GpioExtendedEnable;
        if ((state->pin_control[i] & GpioOutputEnable) != 0) {
            update(tlmm, offset + 4, GpioOutput, state->pin_output[i]);
        }
        update(tlmm, offset, mask, state->pin_control[i] & mask);
        update(tlmm, offset + 4, GpioOutput, state->pin_output[i]);
        if ((tlmm->read(tlmm->context, offset) & mask) != (state->pin_control[i] & mask) ||
            (tlmm->read(tlmm->context, offset + 4) & GpioOutput) != state->pin_output[i]) {
            return false;
        }
    }
    *state = (struct NezhaCameraMainPins){0};
    return true;
}
