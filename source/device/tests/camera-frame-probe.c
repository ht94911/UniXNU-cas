/* SPDX-License-Identifier: MIT */
#include "NezhaCameraFrameProbe.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct FrameModel {
    uint8_t buffer[2][256];
    unsigned step, returned, acquired, published, delivered;
    uint32_t addresses[2];
    bool no_second_write, wrong_address, stopped, no_timestamp, no_event, stop_in_callback, deliver_failed, publish_failed;
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
    if (model->publish_failed) return false;
    ++model->returned;
    return true;
}

static uint32_t fault(void *cookie)
{
    (void)cookie;
    return 0;
}

static bool next_event(void *cookie,struct NezhaCamera1080Events *event)
{
    struct FrameModel *m=cookie;
    *event=(struct NezhaCamera1080Events){0};
    if (!m->no_event) *event=(struct NezhaCamera1080Events){
        .buffer_done=true,.consumed_address=m->addresses[(m->step+(m->wrong_address ? 1u:0u))%2],
        .completed_time=m->no_timestamp ? 0u:100u+m->step};
    return true;
}

static bool continuing(void *cookie) { return !((struct FrameModel *)cookie)->stopped; }
static bool deliver(void *cookie,const struct NezhaCameraRaw10Layout *layout,
    const uint8_t *frame,size_t bytes,uint64_t ticks,uint32_t sequence)
{
    struct FrameModel *m=cookie;
    assert(bytes==256 && layout->stride==64 && frame==m->buffer[m->step%2] &&
        ticks==100u+m->step && sequence==m->delivered+1);
    for (unsigned i=0;i<bytes;++i) assert(frame[i]==0x50+m->step);
    ++m->delivered;
    if (m->stop_in_callback) m->stopped=true;
    return !m->deliver_failed;
}

static struct NezhaCameraMainCapture streaming(struct FrameModel *model)
{
    memset(model->buffer[0],0x50,sizeof(model->buffer[0]));
    return (struct NezhaCameraMainCapture){.rdi_started=true,.stream_may_be_on=true,.dma.context_active=true,
        .layout={{48,4,64,64,256},1,{model->addresses[0],model->addresses[1]}},.queued={0,1},.queued_count=2};
}

static enum NezhaCameraFrameStatus simulate(struct FrameModel *model, struct NezhaCameraFrameResult *result)
{
    memset(model->buffer[0], 0x50, sizeof(model->buffer[0]));
    const struct NezhaCamera1080Io camss = {model, write, write};
    const struct NezhaCameraMainCaptureIo io = {.camss = &camss};
    const struct NezhaCameraRegisterIo csid = {model, csid_read, write, wait}, vfe = {model, vfe_read, write, wait};
    const struct NezhaCameraFrameMemoryIo memory = {model, acquire, release, fault, NULL};
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

    const struct NezhaCamera1080Io camss={&model,write,write};
    const struct NezhaCameraMainCaptureIo io={.camss=&camss};
    const struct NezhaCameraRegisterIo csid={&model,csid_read,write,wait};
    const struct NezhaCameraFrameMemoryIo memory={&model,acquire,release,fault,next_event};
    const struct NezhaCameraFrameConsumer consumer={&model,continuing,deliver};
    model=original;
    struct NezhaCameraMainCapture capture=streaming(&model);
    for (unsigned i=0;i<7;++i) {
        assert(nezha_camera_frame_consume(&io,&capture,&csid,&memory,&consumer,10)==NezhaFramesOk);
        wait(&model,1);
    }
    assert(model.delivered==7 && model.published==7 && !capture.cpu_frames && capture.queued_count==2);

    model=original;capture=streaming(&model);
    assert(nezha_camera_frame_consume(&io,&capture,&csid,&memory,&consumer,0)==NezhaFramesOk);
    assert(model.delivered==1 && model.published==1 && capture.completed_frames==1);
    model.no_event=true;
    const unsigned before_poll=capture.completed_frames;
    const unsigned step_before_poll=model.step;
    assert(nezha_camera_frame_consume(&io,&capture,&csid,&memory,&consumer,0)==NezhaFramesOk);
    assert(capture.completed_frames==before_poll && model.step==step_before_poll);

    model=original;capture=streaming(&model);model.stop_in_callback=true;
    assert(nezha_camera_frame_consume(&io,&capture,&csid,&memory,&consumer,10)==NezhaFramesOk);
    assert(model.delivered==1 && !model.published && !capture.cpu_frames && capture.queued_count==1);
    unsigned reads=model.acquired;
    assert(nezha_camera_frame_consume(&io,&capture,&csid,&memory,&consumer,10)==NezhaFramesOk && model.acquired==reads);

    model=original;capture=streaming(&model);model.deliver_failed=true;
    assert(nezha_camera_frame_consume(&io,&capture,&csid,&memory,&consumer,10)==NezhaFramesIo);
    assert(model.delivered==1 && !model.published && !capture.cpu_frames);
    model=original;capture=streaming(&model);model.publish_failed=true;
    assert(nezha_camera_frame_consume(&io,&capture,&csid,&memory,&consumer,10)==NezhaFramesCache);
    assert(capture.cpu_frames==1 && model.delivered==1); /* cleanup must drain before releasing this frame */

    model=original;capture=streaming(&model);model.wrong_address=true;
    assert(nezha_camera_frame_consume(&io,&capture,&csid,&memory,&consumer,10)==NezhaFramesSequence && !model.acquired);
    model=original;capture=streaming(&model);model.no_timestamp=true;
    assert(nezha_camera_frame_consume(&io,&capture,&csid,&memory,&consumer,10)==NezhaFramesSequence && !model.acquired);
    model=original;capture=streaming(&model);model.no_event=true;
    assert(nezha_camera_frame_consume(&io,&capture,&csid,&memory,&consumer,10)==NezhaFramesTimeout && !model.acquired);
    puts("Camera frame sampling rejects a stale reused buffer and never acquires a mismatched completion");
    puts("Continuous camera frames preserve IRQ time, render before requeue, stop during callback and retain a failed CPU publication");
}
