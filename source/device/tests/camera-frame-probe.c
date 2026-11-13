/* SPDX-License-Identifier: MIT */
#include "NezhaCameraFrameProbe.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct FrameModel {
    uint8_t buffer[2][256];
    unsigned step, returned, acquired, published;
    uint32_t addresses[2];
    bool no_second_write, wrong_address;
};

static uint32_t csid_read(void *cookie, uint32_t offset)
{
    (void)cookie;
    switch (offset) {
    case 0x180: case 0x1b0: return 0;
    case 0x1a0: return 1u << 16;
    case 0x224: return 1u << 23 | 1u << 12 | 1u << 9;
    default: assert(0); return UINT32_MAX;
    }
}

static uint32_t vfe_read(void *cookie, uint32_t offset)
{
    struct FrameModel *model = cookie;
    assert(offset == 0x2c90);
    const unsigned slot = (model->step + (model->wrong_address ? 1u : 0u)) % 2;
    return model->addresses[slot];
}

static void write(void *cookie, uint32_t offset, uint32_t value)
{
    (void)cookie;
    (void)offset;
    (void)value;
}

static void wait(void *cookie, unsigned ms)
{
    struct FrameModel *model = cookie;
    assert(ms == 1);
    ++model->step;
    if (model->step < 2 || !model->no_second_write) {
        memset(model->buffer[model->step % 2], 0x50 + (int)model->step, sizeof(model->buffer[0]));
    }
}

static uint8_t *acquire(void *cookie, unsigned buffer)
{
    struct FrameModel *model = cookie;
    assert(buffer == model->step % 2);
    ++model->acquired;
    return model->buffer[buffer];
}

static bool release(void *cookie, unsigned buffer, bool requeue)
{
    struct FrameModel *model = cookie;
    assert(buffer == model->step % 2);
    if (requeue) {
        for (unsigned i = 0; i < sizeof(model->buffer[0]); ++i) assert(model->buffer[buffer][i] == 0xa5);
        ++model->published;
    }
    ++model->returned;
    return true;
}

static uint32_t fault(void *cookie)
{
    (void)cookie;
    return 0;
}

static enum NezhaCameraFrameStatus simulate(struct FrameModel *model, struct NezhaCameraFrameResult *result)
{
    memset(model->buffer[0], 0x50, sizeof(model->buffer[0]));
    const struct NezhaCamera1080Io camss = {model, write, write};
    const struct NezhaCameraMainCaptureIo io = {.camss = &camss};
    const struct NezhaCameraRegisterIo csid = {model, csid_read, write, wait}, vfe = {model, vfe_read, write, wait};
    const struct NezhaCameraFrameMemoryIo memory = {model, acquire, release, fault};
    struct NezhaCameraMainCapture capture = {.rdi_started = true, .stream_may_be_on = true, .dma.context_active = true,
        .layout = {{48, 4, 64, 64, 256}, 1, {model->addresses[0], model->addresses[1]}},
        .queued = {0, 1}, .queued_count = 2};
    const enum NezhaCameraFrameStatus status = nezha_camera_frame_probe(&io, &capture, &csid, &vfe, &memory, 3, 10, result);
    assert(capture.cpu_frames == 0);
    return status;
}

int main(void)
{
    const struct FrameModel original = {.addresses = {0x10000, 0x20000}};
    struct FrameModel model = original;
    struct NezhaCameraFrameResult result;
    assert(simulate(&model, &result) == NezhaFramesOk);
    assert(result.frames == 3 && result.sof == 3 && result.eof == 3 && result.updated == 3);
    assert(model.acquired == 3 && model.returned == 3 && model.published == 2);

    model = original;
    model.no_second_write = true;
    assert(simulate(&model, &result) == NezhaFramesUnwritten);
    assert(result.frames == 2 && model.acquired == 3 && model.returned == 3 && model.published == 2);

    model = original;
    model.wrong_address = true;
    assert(simulate(&model, &result) == NezhaFramesSequence);
    assert(result.frames == 0 && model.acquired == 0);
    puts("Camera frame sampling rejects a stale reused buffer and never acquires a mismatched completion");
}
