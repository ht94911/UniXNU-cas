/* SPDX-License-Identifier: MIT */
#include "NezhaCameraIfeRsc.h"

/* Original Canoe DT: both channels have WAKE then SLEEP, no active TCS.
 * Each channel occupies two 0x150-byte slots under the 0x520 TCS base.
 * Channel controls use the hardware-channel layout in rpmh-rsc.c. */
enum {
    RscId = 0, RscConfig = 0x0c, ChannelComplete = 0x494,
    ChannelUpdate = 0x498, ChannelBusy = 0x49c, ChannelEnable = 0x4a0,
    TcsBase = 0x520, TcsStride = 0x150, TcsWait = 0x20, TcsControl = 0x24,
    TcsStatus = 0x28, TcsEnable = 0x2c, TcsMessage = 0x34, CommandStride = 0x18,
    Write = 0x10008, Response = 0x100, CommandMask = (1 << NezhaCameraIfeBcmCount) - 1,
};
static const uint32_t clear_completion = UINT32_C(1) << 31;

static bool valid(const struct NezhaCameraRegisterIo *io, const struct NezhaCameraIfeBandwidth *plan,
    const struct NezhaCameraIfeRsc *state)
{
    if (!io || !io->read || !io->write || !io->wait_ms || !plan || !state ||
        plan->count != NezhaCameraIfeBcmCount) {
        return false;
    }
    for (unsigned i = 0; i < plan->count; ++i) {
        if ((plan->wake[i].address & 0xffff0003u) != 0x50000u ||
            plan->wake[i].address != plan->sleep[i].address ||
            ((plan->wake[i].data | plan->sleep[i].data) & 0x90000000u) != 0 ||
            (plan->wake[i].data ^ plan->sleep[i].data) >> 30 != 0 ||
            plan->wake[i].wait != ((plan->wake[i].data & (1u << 30)) != 0) ||
            plan->sleep[i].wait) {
            return false;
        }
        for (unsigned j = 0; j < i; ++j) {
            if (plan->wake[i].address == plan->wake[j].address) {
                return false;
            }
        }
    }
    return plan->wake[plan->count - 1].wait;
}

static bool update(const struct NezhaCameraRegisterIo *io, uint32_t reg, uint32_t data)
{
    io->write(io->context, reg, data);
    return io->read(io->context, reg) == data;
}

static void sample(const struct NezhaCameraRegisterIo *io, struct NezhaCameraIfeRsc *state)
{
    state->enable = io->read(io->context, ChannelEnable);
    state->update = io->read(io->context, ChannelUpdate);
    state->busy = io->read(io->context, ChannelBusy);
    state->completion = io->read(io->context, ChannelComplete);
}

static bool program(const struct NezhaCameraRegisterIo *io,
    const struct NezhaCameraIfeBcmCommand *commands, unsigned slot)
{
    const uint32_t base = TcsBase + slot * TcsStride;
    uint32_t wait = 0;
    if (io->read(io->context, base + TcsControl) != 0 ||
        io->read(io->context, base + TcsStatus) != 1 ||
        io->read(io->context, base + TcsEnable) != 0 || io->read(io->context, base + TcsWait) != 0) {
        return false;
    }
    for (unsigned i = 0; i < NezhaCameraIfeBcmCount; ++i) {
        const uint32_t reg = base + TcsMessage + i * CommandStride;
        if (!update(io, reg, Write | (commands[i].wait ? Response : 0)) ||
            !update(io, reg + 4, commands[i].address) || !update(io, reg + 8, commands[i].data)) {
            return false;
        }
        if (commands[i].wait) {
            wait |= 1u << i;
        }
    }
    return update(io, base + TcsWait, wait) && update(io, base + TcsEnable, CommandMask);
}

static enum NezhaCameraIfeRscStatus switch_channel(const struct NezhaCameraRegisterIo *io,
    struct NezhaCameraIfeRsc *state, unsigned channel)
{
    const uint32_t bit = 1u << channel;
    const uint32_t wake = 1u << (channel * 2);
    io->write(io->context, ChannelUpdate, bit);
    for (unsigned ms = 0; ms <= 10; ++ms) {
        sample(io, state);
        if (state->enable == 1 && state->update == bit && state->busy == bit &&
            state->completion != UINT32_MAX && (state->completion & wake) != 0) {
            io->write(io->context, ChannelComplete, clear_completion);
            state->pending = false;
            return NezhaCameraIfeRscOk;
        }
        if (ms < 10) {
            io->wait_ms(io->context, 1);
        }
    }
    return NezhaCameraIfeRscTimeout;
}

enum NezhaCameraIfeRscStatus nezha_camera_ife_rsc_start(const struct NezhaCameraRegisterIo *io,
    const struct NezhaCameraIfeBandwidth *plan, struct NezhaCameraIfeRsc *state)
{
    if (!valid(io, plan, state)) {
        return NezhaCameraIfeRscInvalid;
    }
    if (state->owned) {
        return NezhaCameraIfeRscBusy;
    }
    const uint32_t id = io->read(io->context, RscId);
    const uint32_t config = io->read(io->context, RscConfig);
    if (id == UINT32_MAX || ((id >> 16) & 0xffu) < 3 || config == UINT32_MAX ||
        (config & 0x3fu) < 4 || (config >> 27) < NezhaCameraIfeBcmCount) {
        return NezhaCameraIfeRscReadback;
    }
    sample(io, state);
    if (state->enable || state->update || state->busy || state->completion) {
        return NezhaCameraIfeRscBusy;
    }
    for (unsigned slot = 0; slot < 4; ++slot) {
        const uint32_t base = TcsBase + slot * TcsStride;
        if (io->read(io->context, base + TcsControl) != 0 ||
            io->read(io->context, base + TcsStatus) != 1 ||
            io->read(io->context, base + TcsEnable) != 0 || io->read(io->context, base + TcsWait) != 0) {
            return NezhaCameraIfeRscBusy;
        }
        for (unsigned i = 0; i < plan->count; ++i) {
            state->address[i] = plan->wake[i].address;
            const uint32_t reg = base + TcsMessage + i * CommandStride;
            for (unsigned j = 0; j < 3; ++j) {
                state->previous[slot][i][j] = io->read(io->context, reg + j * 4);
                if (state->previous[slot][i][j] == UINT32_MAX) {
                    return NezhaCameraIfeRscReadback;
                }
            }
        }
    }
    state->owned = state->pending = true;
    if (!program(io, plan->wake, 0) || !program(io, plan->sleep, 1) || !update(io, ChannelEnable, 1)) {
        return NezhaCameraIfeRscReadback;
    }
    const enum NezhaCameraIfeRscStatus result = switch_channel(io, state, 0);
    state->ready = result == NezhaCameraIfeRscOk;
    return result;
}

enum NezhaCameraIfeRscStatus nezha_camera_ife_rsc_stop(const struct NezhaCameraRegisterIo *io,
    const struct NezhaCameraIfeBandwidth *clear, struct NezhaCameraIfeRsc *state, bool drained)
{
    if (!valid(io, clear, state)) {
        return NezhaCameraIfeRscInvalid;
    }
    if (!drained || !state->owned || !state->ready || state->pending || state->releasing) {
        return NezhaCameraIfeRscBusy;
    }
    for (unsigned i = 0; i < clear->count; ++i) {
        if (clear->wake[i].address != state->address[i] ||
            ((clear->wake[i].data | clear->sleep[i].data) & ~(1u << 30)) != 0) {
            return NezhaCameraIfeRscInvalid;
        }
    }
    sample(io, state);
    if (state->enable != 1 || state->update != 1 || state->busy != 1 || state->completion) {
        return NezhaCameraIfeRscBusy;
    }
    state->releasing = state->pending = true;
    if (!program(io, clear->wake, 2) || !program(io, clear->sleep, 3)) {
        return NezhaCameraIfeRscReadback;
    }
    const enum NezhaCameraIfeRscStatus result = switch_channel(io, state, 1);
    if (result != NezhaCameraIfeRscOk) {
        return result;
    }
    if (!update(io, ChannelUpdate, 0) || !update(io, ChannelEnable, 0)) {
        return NezhaCameraIfeRscReadback;
    }
    sample(io, state);
    if (state->enable || state->update || state->busy || state->completion) {
        return NezhaCameraIfeRscReadback;
    }
    for (unsigned slot = 0; slot < 4; ++slot) {
        const uint32_t base = TcsBase + slot * TcsStride;
        if (!update(io, base + TcsEnable, 0) || !update(io, base + TcsWait, 0)) {
            return NezhaCameraIfeRscReadback;
        }
        for (unsigned i = 0; i < clear->count; ++i) {
            const uint32_t reg = base + TcsMessage + i * CommandStride;
            for (unsigned j = 0; j < 3; ++j) {
                if (!update(io, reg + j * 4, state->previous[slot][i][j])) {
                    return NezhaCameraIfeRscReadback;
                }
            }
        }
    }
    *state = (struct NezhaCameraIfeRsc){0};
    return NezhaCameraIfeRscOk;
}
