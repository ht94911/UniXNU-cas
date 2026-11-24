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

static enum NezhaCameraCpasStatus poll(const struct NezhaCameraRegisterIo *top,
    struct NezhaCameraCpas *state, unsigned channel, uint32_t value)
{
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

static enum NezhaCameraCpasStatus request(const struct NezhaCameraRegisterIo *top,
    struct NezhaCameraCpas *state, unsigned channel, uint32_t value, bool force)
{
    const uint32_t before = top->read(top->context, control[channel]);
    if (before > 1) {
        return NezhaCameraCpasReadback;
    }
    if (force || before != value) {
        state->touched |= 1u << channel;
        top->write(top->context, control[channel], value);
    }
    if (top->read(top->context, control[channel]) != value) {
        return NezhaCameraCpasReadback;
    }
    return poll(top, state, channel, value);
}

static void remember_failure(const struct NezhaCameraRegisterIo *top,
    struct NezhaCameraCpas *state, unsigned channel, uint32_t expected,
    enum NezhaCameraCpasStatus result)
{
    if (state->failed_offset != 0) return;
    state->failed_offset = result == NezhaCameraCpasTimeout ? status[channel] : control[channel];
    state->failed_control = top->read(top->context, control[channel]);
    state->failed_status = state->status[channel];
    state->failed_expected = expected;
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
    /* cam_cpas_start first observes the CRM-driven wakeup without writing
     * QCHANNEL_CTRL. A failed observation selects the stock force-on pass;
     * its writes must not be elided just because CTRL already reads one. */
    bool active = true;
    for (unsigned i = 0; i < 3; ++i) {
        if (poll(top, state, i, 1) != NezhaCameraCpasOk) active = false;
    }
    if (!active) {
        enum NezhaCameraCpasStatus first = NezhaCameraCpasOk;
        for (unsigned i = 0; i < 3; ++i) {
            const enum NezhaCameraCpasStatus result = request(top, state, i, 1, true);
            if (first == NezhaCameraCpasOk && result != NezhaCameraCpasOk) {
                first = result;
                remember_failure(top, state, i, 1, result);
            }
        }
        if (first != NezhaCameraCpasOk) return first;
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
        const unsigned bit = 1u << i;
        const bool issue = (state->stop_requested & bit) == 0;
        state->stop_requested |= bit;
        /* Stock STOP writes zero even if CTRL already reads zero. A late
         * stop acknowledgement is only reobserved on a subsequent drain. */
        const enum NezhaCameraCpasStatus result = request(top, state, i, 0, issue);
        if (first == NezhaCameraCpasOk && result != NezhaCameraCpasOk) {
            first = result;
            remember_failure(top, state, i, 0, result);
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
            const enum NezhaCameraCpasStatus result = request(top, state, channel, state->before[channel], false);
            if (result != NezhaCameraCpasOk) {
                remember_failure(top, state, channel, state->before[channel], result);
                return result;
            }
            state->touched &= ~(1u << channel);
        }
    }
    /* Cleanup must not erase the observation that rejected this capture. */
    *state = (struct NezhaCameraCpas){
        .failed_offset = state->failed_offset, .failed_control = state->failed_control,
        .failed_status = state->failed_status, .failed_expected = state->failed_expected,
    };
    return NezhaCameraCpasOk;
}

/* These enabled READ_WRITE entries are also present in the retained
 * nezha camera.ko's cam_cpas_v1080_100_camnoc_rt_niu. Qualcomm's
 * cam_cpastop_curr_camnoc_poweron programs them at every power-on. */
static const struct {
    uint32_t offset, value;
} rt0_qos[NezhaCameraCpasRt0QosWords] = {
    {0x2c30, 0x66655554}, {0x2c34, 0x66666666}, {0x2c38, 0x00001e40},
    {0x2c40, 0xffffff00}, {0x2c48, 0x0000000f},
};

static enum NezhaCameraCpasStatus qos_failure(struct NezhaCameraCpasRt0Qos *state,
    unsigned index, uint32_t observed, uint32_t expected, enum NezhaCameraCpasStatus status)
{
    if (!state->failed_offset) {
        state->failed_offset = rt0_qos[index].offset;
        state->failed_observed = observed;
        state->failed_expected = expected;
    }
    return status;
}

enum NezhaCameraCpasStatus nezha_camera_cpas_rt0_qos_start(
    const struct NezhaCameraRegisterIo *rt, const struct NezhaCameraCpas *cpas,
    struct NezhaCameraCpasRt0Qos *state)
{
    if (!rt || !rt->read || !rt->write || !cpas || !state)
        return NezhaCameraCpasInvalid;
    if (!cpas->acquired || !cpas->ready || cpas->drained || state->prepared || state->touched)
        return NezhaCameraCpasBusy;
    *state = (struct NezhaCameraCpasRt0Qos){0};
    /* Save every inherited value before changing any word. A bad map
     * cannot create a partial lease from an incomplete snapshot. */
    for (unsigned i = 0; i < NezhaCameraCpasRt0QosWords; ++i) {
        const uint32_t before = rt->read(rt->context, rt0_qos[i].offset);
        if (before == UINT32_MAX)
            return qos_failure(state, i, before, rt0_qos[i].value, NezhaCameraCpasReadback);
        state->before[i] = state->programmed[i] = before;
    }
    state->prepared = true;
    for (unsigned i = 0; i < NezhaCameraCpasRt0QosWords; ++i) {
        const uint32_t expected = rt0_qos[i].value;
        if (state->before[i] != expected) {
            state->touched |= 1u << i;
            state->changed |= 1u << i;
            rt->write(rt->context, rt0_qos[i].offset, expected);
        }
        const uint32_t observed = rt->read(rt->context, rt0_qos[i].offset);
        state->programmed[i] = observed;
        if (observed != expected)
            return qos_failure(state, i, observed, expected, NezhaCameraCpasReadback);
    }
    state->applied = true;
    return NezhaCameraCpasOk;
}

enum NezhaCameraCpasStatus nezha_camera_cpas_rt0_qos_restore(
    const struct NezhaCameraRegisterIo *rt, const struct NezhaCameraCpas *cpas,
    bool dma_detached, struct NezhaCameraCpasRt0Qos *state)
{
    if (!rt || !rt->read || !rt->write || !cpas || !state)
        return NezhaCameraCpasInvalid;
    if (!cpas->acquired || !cpas->drained || !dma_detached)
        return NezhaCameraCpasBusy;
    if (!state->prepared)
        return state->touched ? NezhaCameraCpasBusy : NezhaCameraCpasOk;
    for (unsigned remaining = NezhaCameraCpasRt0QosWords; remaining; --remaining) {
        const unsigned i = remaining - 1, bit = 1u << i;
        if (!(state->touched & bit)) continue;
        const uint32_t observed = rt->read(rt->context, rt0_qos[i].offset);
        if (observed == UINT32_MAX)
            return qos_failure(state, i, observed, state->before[i], NezhaCameraCpasReadback);
        if (observed != state->before[i]) {
            /* Preserve a value changed outside this lease. Do not undo
             * another owner merely because our original write failed. */
            if (observed != state->programmed[i] && observed != rt0_qos[i].value)
                return qos_failure(state, i, observed, state->programmed[i], NezhaCameraCpasBusy);
            rt->write(rt->context, rt0_qos[i].offset, state->before[i]);
            const uint32_t after = rt->read(rt->context, rt0_qos[i].offset);
            if (after != state->before[i])
                return qos_failure(state, i, after, state->before[i], NezhaCameraCpasReadback);
        }
        state->touched &= ~bit;
    }
    state->prepared = false;
    state->restored = true;
    return NezhaCameraCpasOk;
}
