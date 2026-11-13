/* SPDX-License-Identifier: MIT */
#include "NezhaCameraCpas.h"

#include <stddef.h>

/* Stock CPAS v1080_100's three CAMNOC qchannels and version registers,
 * from OnePlus SM8850 camera-kernel at 5ab2a689ff87. */
static const uint32_t control[] = {0xec, 0xf4, 0x130};
static const uint32_t status[] = {0xf0, 0xf8, 0x12c};
enum { Titan1080 = 0x000a0800, Cpas100 = 0x10000000, Accept = 1, TimeoutMs = 100 };

static bool valid(const struct NezhaCameraRegisterIo *top, const struct NezhaCameraCpas *state)
{
    return top != NULL && top->read != NULL && top->write != NULL &&
        top->wait_ms != NULL && state != NULL;
}

static enum NezhaCameraCpasStatus request(const struct NezhaCameraRegisterIo *top,
    struct NezhaCameraCpas *state, unsigned channel, uint32_t value)
{
    const uint32_t before = top->read(top->context, control[channel]);
    if (before > 1) {
        return NezhaCameraCpasReadback;
    }
    state->touched |= 1u << channel;
    if (before != value) {
        top->write(top->context, control[channel], value);
    }
    if (top->read(top->context, control[channel]) != value) {
        return NezhaCameraCpasReadback;
    }
    for (unsigned ms = 0; ms <= TimeoutMs; ++ms) {
        state->status[channel] = top->read(top->context, status[channel]);
        if (state->status[channel] == UINT32_MAX) {
            return NezhaCameraCpasReadback;
        }
        if ((state->status[channel] & Accept) == value) {
            return NezhaCameraCpasOk;
        }
        if (ms != TimeoutMs) {
            top->wait_ms(top->context, 1);
        }
    }
    return NezhaCameraCpasTimeout;
}

enum NezhaCameraCpasStatus nezha_camera_cpas_start(
    const struct NezhaCameraRegisterIo *top, struct NezhaCameraCpas *state)
{
    if (!valid(top, state)) {
        return NezhaCameraCpasInvalid;
    }
    if (state->acquired) {
        return NezhaCameraCpasBusy;
    }
    if (top->read(top->context, 0) != Titan1080 || top->read(top->context, 4) != Cpas100) {
        return NezhaCameraCpasVersion;
    }
    struct NezhaCameraCpas next = {.acquired = true};
    for (unsigned i = 0; i < 3; ++i) {
        next.before[i] = top->read(top->context, control[i]);
        if (next.before[i] > 1) {
            return NezhaCameraCpasReadback;
        }
    }
    *state = next;
    for (unsigned i = 0; i < 3; ++i) {
        const enum NezhaCameraCpasStatus result = request(top, state, i, 1);
        if (result != NezhaCameraCpasOk) {
            return result;
        }
    }
    state->ready = true;
    return NezhaCameraCpasOk;
}

enum NezhaCameraCpasStatus nezha_camera_cpas_drain(
    const struct NezhaCameraRegisterIo *top, struct NezhaCameraCpas *state,
    bool producers_stopped)
{
    if (!valid(top, state) || !state->acquired) {
        return NezhaCameraCpasInvalid;
    }
    if (!producers_stopped) {
        return NezhaCameraCpasBusy;
    }
    state->ready = false;
    state->drained = false;
    /* The stock stop path requests each channel to quiesce. Keep the first
     * failure visible; an unacknowledged channel does not imply DMA idle. */
    enum NezhaCameraCpasStatus first = NezhaCameraCpasOk;
    for (unsigned i = 0; i < 3; ++i) {
        const enum NezhaCameraCpasStatus result = request(top, state, i, 0);
        if (first == NezhaCameraCpasOk && result != NezhaCameraCpasOk) {
            first = result;
        }
    }
    state->drained = first == NezhaCameraCpasOk;
    return first;
}

enum NezhaCameraCpasStatus nezha_camera_cpas_restore(
    const struct NezhaCameraRegisterIo *top, struct NezhaCameraCpas *state)
{
    if (!valid(top, state) || !state->acquired) {
        return NezhaCameraCpasInvalid;
    }
    if (!state->drained) {
        return NezhaCameraCpasBusy;
    }
    for (unsigned i = 3; i != 0; --i) {
        const unsigned channel = i - 1;
        if ((state->touched & (1u << channel)) != 0) {
            const enum NezhaCameraCpasStatus result = request(top, state, channel, state->before[channel]);
            if (result != NezhaCameraCpasOk) {
                return result;
            }
            state->touched &= ~(1u << channel);
        }
    }
    *state = (struct NezhaCameraCpas){0};
    return NezhaCameraCpasOk;
}
