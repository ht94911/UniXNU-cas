/* SPDX-License-Identifier: MIT */
#include "NezhaCameraIfeRsc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { Complete = 0x494, Update = 0x498, Busy = 0x49c, Enable = 0x4a0,
       Wake0 = 0x520, Wake1 = 0x7c0, TcsStride = 0x150, TcsStatus = 0x28 };

struct Model {
    uint32_t regs[4096 / 4];
    unsigned switches, writes;
    bool timeout;
};

static uint32_t read(void *cookie, uint32_t offset)
{
    struct Model *m = cookie;
    assert(offset < sizeof(m->regs) && offset % 4 == 0);
    return m->regs[offset / 4];
}

static void write(void *cookie, uint32_t offset, uint32_t value)
{
    struct Model *m = cookie;
    assert(offset < sizeof(m->regs) && offset % 4 == 0);
    ++m->writes;
    if (offset == Complete) {
        assert(value == 0x80000000u);
        m->regs[Complete / 4] = 0;
    } else if (offset == Update) {
        if (value) {
            assert(value == 1 || value == 2);
            assert(m->regs[Enable / 4] == 1);
            const unsigned base = value == 1 ? Wake0 : Wake1;
            assert(m->regs[(base + 0x2c) / 4] == 0x3f);
            assert(m->regs[(base + TcsStride + 0x2c) / 4] == 0x3f);
            ++m->switches;
            if (!m->timeout) {
                m->regs[Busy / 4] = value;
                m->regs[Complete / 4] = value == 1 ? 1 : 4;
            }
        }
        m->regs[Update / 4] = value;
    } else {
        if (offset == Enable && value == 0) {
            assert(m->regs[Update / 4] == 0);
            m->regs[Busy / 4] = 0;
        }
        assert(offset == Enable || offset >= Wake0);
        m->regs[offset / 4] = value;
    }
}

static void wait(void *cookie, unsigned ms)
{
    (void)cookie;
    assert(ms == 1);
}

int main(void)
{
    struct Model model = {.regs = {[0] = 0x40300, [0xc / 4] = 8u << 27 | 4u}};
    for (unsigned i = 0; i < 4; ++i) {
        model.regs[(Wake0 + i * TcsStride + TcsStatus) / 4] = 1;
        model.regs[(Wake0 + i * TcsStride + 0x3c) / 4] = 0x1234u + i;
    }
    struct Model original = model;
    const struct NezhaCameraRegisterIo io = {&model, read, write, wait};
    struct NezhaCameraIfeBandwidth clear = {.count = NezhaCameraIfeBcmCount};
    for (unsigned i = 0; i < clear.count; ++i) {
        clear.wake[i] = (struct NezhaCameraIfeBcmCommand){0x50000u + i * 4u, 1u << 30, true};
        clear.sleep[i] = (struct NezhaCameraIfeBcmCommand){clear.wake[i].address, 1u << 30, false};
    }
    struct NezhaCameraIfeBandwidth votes = clear;
    votes.wake[0].data |= (1u << 29) | 3u;
    struct NezhaCameraIfeRsc state = {0};
    model.regs[Enable / 4] = 1;
    assert(nezha_camera_ife_rsc_start(&io, &votes, &state) == NezhaCameraIfeRscBusy && model.writes == 0);
    model = original;
    assert(nezha_camera_ife_rsc_start(&io, &votes, &state) == NezhaCameraIfeRscOk && state.ready);
    const unsigned active_writes = model.writes;
    assert(nezha_camera_ife_rsc_stop(&io, &clear, &state, false) == NezhaCameraIfeRscBusy);
    assert(model.writes == active_writes && model.switches == 1);
    assert(nezha_camera_ife_rsc_stop(&io, &clear, &state, true) == NezhaCameraIfeRscOk);
    assert(!state.owned && model.switches == 2 && !memcmp(original.regs, model.regs, sizeof(model.regs)));

    model = original;
    model.timeout = true;
    assert(nezha_camera_ife_rsc_start(&io, &votes, &state) == NezhaCameraIfeRscTimeout);
    assert(state.owned && state.pending && !state.ready);
    const unsigned timed_out_writes = model.writes;
    assert(nezha_camera_ife_rsc_start(&io, &votes, &state) == NezhaCameraIfeRscBusy);
    assert(nezha_camera_ife_rsc_stop(&io, &clear, &state, true) == NezhaCameraIfeRscBusy);
    assert(model.writes == timed_out_writes && model.switches == 1);
    puts("Camera RSC applies only IFE0 votes, requires wake acknowledgement and restores the dormant slots after drain");
}
