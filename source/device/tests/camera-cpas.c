/* SPDX-License-Identifier: MIT */
#include "NezhaCameraCpas.h"

#include <assert.h>
#include <stdio.h>

struct Model {
    uint32_t version, control[3], status[3];
    unsigned writes, waits;
    unsigned hold;
};

static uint32_t read_word(void *context, uint32_t offset)
{
    const struct Model *m = context;
    switch (offset) {
    case 0: return m->version;
    case 4: return 0x10000000;
    case 0xec: return m->control[0];
    case 0xf4: return m->control[1];
    case 0x130: return m->control[2];
    case 0xf0: return m->status[0];
    case 0xf8: return m->status[1];
    case 0x12c: return m->status[2];
    default: assert(0); return UINT32_MAX;
    }
}

static void write_word(void *context, uint32_t offset, uint32_t value)
{
    struct Model *m = context;
    const unsigned channel = offset == 0xec ? 0 : offset == 0xf4 ? 1 : 2;
    assert(offset == 0xec || offset == 0xf4 || offset == 0x130);
    assert(value <= 1);
    ++m->writes;
    m->control[channel] = value;
    if ((m->hold & (1u << channel)) == 0) {
        m->status[channel] = value;
    }
}

static void wait_ms(void *context, unsigned ms)
{
    struct Model *m = context;
    m->waits += ms;
}

int main(void)
{
    struct Model m = {.version = 0x000a0800, .control = {1, 0, 1}, .status = {1, 0, 1}};
    const struct NezhaCameraRegisterIo io = {&m, read_word, write_word, wait_ms};
    struct NezhaCameraCpas state = {0};
    assert(nezha_camera_cpas_start(&io, &state) == NezhaCameraCpasOk);
    assert(m.writes == 1 && state.ready && !state.drained);
    assert(nezha_camera_cpas_drain(&io, &state, false) == NezhaCameraCpasBusy);
    assert(m.writes == 1 && state.ready);

    m.hold = 2;
    assert(nezha_camera_cpas_drain(&io, &state, true) == NezhaCameraCpasTimeout);
    assert(state.status[1] == 1 && m.waits == 100 && !state.drained && state.acquired);
    assert(nezha_camera_cpas_restore(&io, &state) == NezhaCameraCpasBusy);
    /* The hardware accepted the previously issued stop late. Reobserve
     * it without resubmitting that channel's request. */
    m.hold = 0;
    m.status[1] = 0;
    const unsigned writes = m.writes;
    assert(nezha_camera_cpas_drain(&io, &state, true) == NezhaCameraCpasOk);
    assert(state.drained && m.writes == writes);
    assert(nezha_camera_cpas_restore(&io, &state) == NezhaCameraCpasOk);
    assert(m.control[0] == 1 && m.control[1] == 0 && m.control[2] == 1 && !state.acquired);

    m = (struct Model){.version = UINT32_MAX};
    assert(nezha_camera_cpas_start(&io, &state) == NezhaCameraCpasVersion);
    assert(m.writes == 0);
    puts("Camera CPAS keeps DMA ownership until all three CAMNOC channels accept stop");
}
