/* SPDX-License-Identifier: MIT */
#include "NezhaCameraCamss1080.h"

#include <assert.h>
#include <stdio.h>

struct Session {
    unsigned writes;
    unsigned buffer_count;
    unsigned address_updates;
    uint32_t rx_config;
    bool vfe_enabled;
    bool rdi_started;
};

static void write_csid(void *context, uint32_t offset, uint32_t value)
{
    struct Session *session = context;
    assert(!session->rdi_started);
    ++session->writes;
    if (offset == 0x880) {
        session->rx_config = value;
    } else if (offset == 0x118) {
        assert(value == 0x100 && session->buffer_count == 2);
        ++session->address_updates;
    } else if (offset == 0x5488 && value == 1) {
        assert(session->vfe_enabled && session->address_updates == 2);
        session->rdi_started = true;
    }
    /* This sequence must not program CSID interrupt masks. */
    assert(offset != 0x184 && offset != 0x1a4 && offset != 0x1b4 && offset != 0x228);
}

static void write_vfe(void *context, uint32_t offset, uint32_t value)
{
    struct Session *session = context;
    assert(!session->rdi_started);
    ++session->writes;
    if (offset == 0x2c00) {
        assert(value == 0x10001);
        session->vfe_enabled = true;
    } else if (offset == 0x2c04) {
        assert(session->vfe_enabled && session->buffer_count < 2);
        assert(value == (session->buffer_count == 0 ? 0x200000u : 0x210000u));
        ++session->buffer_count;
    }
}

struct IrqSession {
    uint32_t top, rx, buffers, rdi0;
    unsigned commits;
};

static uint32_t read_status(void *context, uint32_t offset)
{
    struct IrqSession *session = context;
    switch (offset) {
    case 0x180: return session->top;
    case 0x1b0: return session->rx;
    case 0x1a0: return session->buffers;
    case 0x224: return session->rdi0;
    default: assert(0); return 0;
    }
}

static void clear_status(void *context, uint32_t offset, uint32_t value)
{
    struct IrqSession *session = context;
    switch (offset) {
    case 0x188: session->top &= ~value; break;
    case 0x1a8: session->buffers &= ~value; break;
    case 0x22c: session->rdi0 &= ~value; break;
    case 0x110: assert(value == 1); ++session->commits; break;
    default: assert(0);
    }
}

int main(void)
{
    struct Session session = {0};
    const struct NezhaCamera1080Io io = {&session, write_csid, write_vfe};
    const struct NezhaCameraFrameMapping buffers[2] = {
        {0x20000000, 0x01000000}, {0x21000000, 0x01000000},
    };
    struct NezhaCamera1080Input input = {NezhaCamera1080Dphy, 2, 4, {0, 1, 2, 3}};
    assert(nezha_camera1080_start_rdi0(&io, &input, 4096, 3072, buffers));
    assert(session.rdi_started && session.rx_config == 0x00332103);

    session = (struct Session){0};
    input.lanes[3] = 2;
    assert(!nezha_camera1080_start_rdi0(&io, &input, 4096, 3072, buffers));
    assert(session.writes == 0);

    input = (struct NezhaCamera1080Input){.source = NezhaCamera1080Tpg, .index = 2};
    assert(nezha_camera1080_start_rdi0(&io, &input, 640, 480, buffers));
    assert(session.rdi_started && session.rx_config == 0x38032103);

    struct IrqSession irq = {.top = 0x3000c, .rx = 0x40,
        .buffers = 0x30000, .rdi0 = 0x801201};
    struct NezhaCamera1080Events events;
    const struct NezhaCameraRegisterIo csid = {&irq, read_status, clear_status, NULL};
    assert(nezha_camera1080_take_rdi0_events(&csid, &events));
    assert(events.buffer_done && events.updated && events.start_of_frame && events.end_of_frame);
    assert(events.rx_irq == 0x40 && events.rdi0_irq == 0x801201);
    assert(irq.buffers == 0x20000 && irq.rdi0 == 1 && irq.top == 0x3000c && irq.commits == 1);
    assert(nezha_camera1080_take_rdi0_events(&csid, &events));
    assert(!events.buffer_done && !events.start_of_frame && irq.commits == 1);

    puts("CAMSS1080 ordered both frame buffers and their updates before starting RDI0");
}
