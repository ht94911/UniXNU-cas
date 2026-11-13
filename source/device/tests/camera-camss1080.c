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
    uint32_t address;
    unsigned commits, resets, waits;
    bool fail_reset;
};

static uint32_t read_consumed(void *context, uint32_t offset)
{
    struct IrqSession *session = context;
    assert(offset == 0x2c90 && (session->buffers & 0x10000) != 0);
    return session->address;
}

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
    case 0x108: assert(value == 0x11); break;
    case 0x10c:
        assert(value == 5 && (session->top & 1u) == 0);
        ++session->resets;
        if (!session->fail_reset) {
            session->top |= 1;
        }
        break;
    case 0x188: session->top &= ~value; break;
    case 0x1a8: session->buffers &= ~value; break;
    case 0x22c: session->rdi0 &= ~value; break;
    case 0x110: assert(value == 1); ++session->commits; break;
    default: assert(0);
    }
}

static void reset_wait(void *context, unsigned milliseconds)
{
    struct IrqSession *session = context;
    session->waits += milliseconds;
}

static void record_requeue(void *context, uint32_t offset, uint32_t value)
{
    static const uint32_t registers[] = {0x2c04, 0x118, 0x11c, 0x114, 0x11c};
    static const uint32_t values[] = {0x210000, 0x100, 1, 0x100, 1};
    unsigned *step = context;
    assert(*step < 5 && registers[*step] == offset && values[*step] == value);
    ++*step;
}

struct TestPattern {
    struct Session *receiver;
    uint32_t registers[0x200 / 4];
    bool generator_started;
    bool ignore_type;
};

static uint32_t tpg_read(void *context, uint32_t offset)
{
    struct TestPattern *tpg = context;
    assert(offset < 0x200 && offset % 4 == 0);
    return tpg->registers[offset / 4];
}

static void tpg_write(void *context, uint32_t offset, uint32_t value)
{
    struct TestPattern *tpg = context;
    (void)tpg_read(context, offset);
    if (offset == 0x1f4 && value == 0x10) {
        assert(tpg->receiver->rdi_started && tpg->receiver->buffer_count == 2);
        tpg->generator_started = true;
    }
    if (offset != 0x80 || !tpg->ignore_type) {
        tpg->registers[offset / 4] = value;
    }
}

static void tpg_wait(void *context, unsigned ms)
{
    assert(context != NULL && ms == 1);
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

    input = (struct NezhaCamera1080Input){NezhaCamera1080Cphy, 0, 3, {0, 1, 2}};
    assert(nezha_camera1080_start_rdi0(&io, &input, 4096, 3072, buffers));
    assert(session.rdi_started && session.rx_config == 0x01102102);

    session = (struct Session){0};
    input.lanes[2] = 3;
    assert(!nezha_camera1080_start_rdi0(&io, &input, 4096, 3072, buffers));
    assert(session.writes == 0);

    input = (struct NezhaCamera1080Input){.source = NezhaCamera1080Tpg, .index = 2};
    assert(nezha_camera1080_start_rdi0(&io, &input, 640, 480, buffers));
    assert(session.rdi_started && session.rx_config == 0x38032103);

    struct IrqSession irq = {.top = 0x3000c, .rx = 0x40,
        .buffers = 0x30000, .rdi0 = 0x801201, .address = UINT32_MAX};
    struct NezhaCamera1080Events events;
    const struct NezhaCameraRegisterIo csid = {&irq, read_status, clear_status, reset_wait};
    const struct NezhaCameraRegisterIo vfe = {.context = &irq, .read = read_consumed};
    assert(!nezha_camera1080_take_rdi0_events(&csid, &vfe, &events));
    assert(irq.commits == 0 && irq.buffers == 0x30000 && !events.buffer_done);
    irq.address = 0x200000;
    assert(nezha_camera1080_take_rdi0_events(&csid, &vfe, &events));
    assert(events.buffer_done && events.updated && events.start_of_frame && events.end_of_frame);
    assert(events.consumed_address == 0x200000);
    assert(events.rx_irq == 0x40 && events.rdi0_irq == 0x801201);
    assert(irq.buffers == 0x20000 && irq.rdi0 == 1 && irq.top == 0x3000c && irq.commits == 1);
    assert(nezha_camera1080_take_rdi0_events(&csid, &vfe, &events));
    assert(!events.buffer_done && !events.start_of_frame && irq.commits == 1);

    irq = (struct IrqSession){.top = 0x10001, .buffers = 0x30000};
    uint32_t reset_status = 0;
    assert(nezha_camera1080_prepare_csid(&csid, &reset_status));
    assert(reset_status == 0x10001 && irq.resets == 1 && irq.top == 0x10000 && irq.buffers == 0x20000);
    irq.fail_reset = true;
    assert(!nezha_camera1080_prepare_csid(&csid, &reset_status));
    assert(irq.resets == 2 && irq.waits == 100);

    unsigned step = 0;
    struct NezhaCameraRdi1080 layout;
    assert(nezha_camera_rdi1080_layout(640, 480, buffers, &layout));
    const struct NezhaCamera1080Io requeue = {&step, record_requeue, record_requeue};
    assert(nezha_camera1080_requeue_rdi0(&requeue, &layout, 1) && step == 5);

    session = (struct Session){0};
    struct TestPattern tpg = {.receiver = &session, .registers = {0x20040000}, .ignore_type = true};
    const struct NezhaCameraRegisterIo tpg_io = {&tpg, tpg_read, tpg_write, tpg_wait};
    assert(!nezha_camera1080_start_tpg_bars(&io, &tpg_io, 2, buffers));
    assert(session.writes == 0 && !tpg.generator_started);
    tpg.ignore_type = false;
    assert(nezha_camera1080_start_tpg_bars(&io, &tpg_io, 2, buffers));
    assert(tpg.generator_started && tpg.registers[0x7c / 4] == 0x028001e0);

    puts("CAMSS1080 ordered both frame buffers and their updates before starting RDI0");
}
