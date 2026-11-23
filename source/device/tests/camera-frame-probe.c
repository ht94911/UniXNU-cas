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
    unsigned pending_notices;
    bool endless_notices;
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
    if (m->pending_notices || m->endless_notices) {
        if (m->pending_notices) --m->pending_notices;
        event->rdi0_irq=1u<<8;
        return true;
    }
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
        .layout={.layout={48,4,64,64,256},.frame_increment=1,
            .image_address={model->addresses[0],model->addresses[1]}},.queued={0,1},.queued_count=2};
}

static enum NezhaCameraFrameStatus simulate(struct FrameModel *model, struct NezhaCameraFrameResult *result)
{
    memset(model->buffer[0], 0x50, sizeof(model->buffer[0]));
    const struct NezhaCamera1080Io camss = {model, write, write};
    const struct NezhaCameraMainCaptureIo io = {.camss = &camss};
    const struct NezhaCameraRegisterIo csid = {model, csid_read, write, wait}, vfe = {model, vfe_read, write, wait};
    const struct NezhaCameraFrameMemoryIo memory = {model, acquire, release, fault, NULL};
    struct NezhaCameraMainCapture capture = {.rdi_started = true, .stream_may_be_on = true, .dma.context_active = true,
        .layout = {.layout={48, 4, 64, 64, 256}, .frame_increment=1,
            .image_address={model->addresses[0], model->addresses[1]}},
        .queued = {0, 1}, .queued_count = 2};
    const enum NezhaCameraFrameStatus status = nezha_camera_frame_probe(&io, &capture, &csid, &vfe, &memory, 3, 10, result);
    assert(capture.cpu_frames == 0);
    return status;
}

struct SpareModel {
    uint8_t buffer[3][256];
    struct NezhaCameraMainCapture capture;
    unsigned completed, replacements, acquired, delivered, published;
    unsigned old, replacement;
    bool stopped, stop_in_callback, delivery_failure, publication_failure, wrong_address;
};

static bool spare_continuing(void *cookie) { return !((struct SpareModel *)cookie)->stopped; }
static bool spare_event(void *cookie,struct NezhaCamera1080Events *event)
{
    struct SpareModel *m=cookie;
    assert(m->capture.queued_count==2 && m->capture.ready_frames && !m->capture.cpu_frames);
    m->old=m->capture.queued[0];
    memset(m->buffer[m->old],0x40+(int)m->completed,sizeof(m->buffer[0]));
    *event=(struct NezhaCamera1080Events){.buffer_done=true,
        .consumed_address=m->capture.layout.image_address[m->wrong_address ?
            m->capture.queued[1]:m->old],.completed_time=100+m->completed};
    return true;
}
static void spare_write_csid(void *cookie,uint32_t offset,uint32_t value)
{
    (void)cookie;
    assert((offset==0x118 && value==256) || (offset==0x114 && value==256) ||
        (offset==0x11c && value==1));
}
static void spare_write_vfe(void *cookie,uint32_t offset,uint32_t value)
{
    struct SpareModel *m=cookie;
    assert(offset==0x2c04 && m->capture.queued_count==1 &&
        m->capture.cpu_frames==(1u<<m->old));
    assert(m->acquired==m->completed && m->delivered==m->completed);
    unsigned next=0;
    while (next<3 && m->capture.layout.image_address[next]!=value) ++next;
    assert(next<3 && next!=m->old && next!=m->capture.queued[0] &&
        m->capture.ready_frames==(1u<<next));
    /* A returned buffer was fully republished before it became a spare. */
    if (m->replacements)
        for (unsigned i=0;i<256;++i) assert(m->buffer[next][i]==0xa5);
    m->replacement=next;++m->replacements;
}
static uint8_t *spare_acquire(void *cookie,unsigned index)
{
    struct SpareModel *m=cookie;
    assert(index==m->old && m->replacements==m->completed+1 &&
        m->capture.queued_count==2 && m->capture.queued[1]==m->replacement &&
        m->capture.cpu_frames==(1u<<index) && !m->capture.ready_frames);
    ++m->acquired;return m->buffer[index];
}
static bool spare_deliver(void *cookie,const struct NezhaCameraRaw10Layout *layout,
    const uint8_t *frame,size_t bytes,uint64_t ticks,uint32_t sequence)
{
    struct SpareModel *m=cookie;
    assert(layout->frame_bytes==256 && bytes==256 && frame==m->buffer[m->old] &&
        ticks==100+m->completed && sequence==m->completed+1);
    /* The replacement is already submitted during an arbitrarily slow
     * CPU copy. The completed pointer cannot be in either hardware slot. */
    for (unsigned pass=0;pass<4;++pass) {
        assert(m->capture.queued_count==2 && m->capture.queued[0]!=m->old &&
            m->capture.queued[1]!=m->old);
        for (unsigned i=0;i<256;++i) assert(frame[i]==0x40+m->completed);
    }
    ++m->delivered;
    if (m->stop_in_callback) m->stopped=true;
    return !m->delivery_failure;
}
static bool spare_return(void *cookie,unsigned index,bool requeue)
{
    struct SpareModel *m=cookie;
    assert(index==m->old && m->capture.cpu_frames==(1u<<index) &&
        m->capture.queued_count==2 && !m->capture.ready_frames);
    if (requeue) {
        for (unsigned i=0;i<256;++i) assert(m->buffer[index][i]==0xa5);
        ++m->published;
    }
    return !m->publication_failure;
}
struct CopyModel {
    uint8_t buffer[3][256];
    struct NezhaCameraMainCapture capture;
    unsigned pending,notices,recycled,writes,acquired,returned;
    unsigned current,held;
    bool endless,wrong,unwritten,acquire_failure,publication_failure,queue_failure,smmu_failure;
    bool stopped,stop_after_acquire,stop_after_publish;
};
static bool copy_event(void *context,struct NezhaCamera1080Events *event)
{
    struct CopyModel *m=context;
    *event=(struct NezhaCamera1080Events){0};
    if(m->queue_failure)return false;
    if(m->notices||m->endless) {
        if(m->notices)--m->notices;
        event->rdi0_irq=1u<<8;return true;
    }
    if(!m->pending)return true;
    --m->pending;
    assert(m->capture.cpu_frames==(1u<<m->held) && m->capture.queued_count==2 && !m->capture.ready_frames);
    m->current=m->capture.queued[0];
    if(!m->unwritten)memset(m->buffer[m->current],0x60+(int)m->recycled,256);
    *event=(struct NezhaCamera1080Events){.buffer_done=true,
        .consumed_address=m->capture.layout.image_address[m->wrong?m->held:m->current],
        .completed_time=200+m->recycled};
    return true;
}
static uint8_t *copy_acquire(void *context,unsigned index)
{
    struct CopyModel *m=context;
    assert(index==m->current && index!=m->held && m->capture.cpu_frames==((1u<<index)|(1u<<m->held)));
    if(m->stop_after_acquire)m->stopped=true;
    ++m->acquired;return m->acquire_failure?NULL:m->buffer[index];
}
static bool copy_return(void *context,unsigned index,bool requeue)
{
    struct CopyModel *m=context;
    assert(index==m->current && index!=m->held && m->capture.cpu_frames==((1u<<index)|(1u<<m->held)));
    if(requeue)for(unsigned i=0;i<256;++i)assert(m->buffer[index][i]==0xa5);
    ++m->returned;
    if(m->stop_after_publish)m->stopped=true;
    return !m->publication_failure;
}
static bool copy_continuing(void *context) {return !((struct CopyModel *)context)->stopped;}
static uint32_t copy_fault(void *context) {return ((struct CopyModel *)context)->smmu_failure?1:0;}
static void copy_write_csid(void *context,uint32_t offset,uint32_t value)
{
    (void)context;
    assert((offset==0x118&&value==256)||(offset==0x114&&value==256)||(offset==0x11c&&value==1));
}
static void copy_write_vfe(void *context,uint32_t offset,uint32_t value)
{
    struct CopyModel *m=context;
    assert(offset==0x2c04 && m->capture.cpu_frames==((1u<<m->current)|(1u<<m->held)) &&
        m->capture.queued_count==1 && !m->capture.ready_frames);
    assert(value==m->capture.layout.image_address[m->current] &&
        m->returned==m->acquired && m->current!=m->held);
    for(unsigned i=0;i<256;++i)assert(m->buffer[m->held][i]==0x55 && m->buffer[m->current][i]==0xa5);
    ++m->writes;++m->recycled;
}
static struct CopyModel copy_model(void)
{
    struct CopyModel m={.capture={.rdi_started=true,.stream_may_be_on=true,
        .dma.context_active=true,.layout={.layout={48,4,64,64,256},.frame_increment=1,
        .image_address={0x10000,0x20000,0x30000},.frame_count=3},.queued={1,2},
        .queued_count=2,.cpu_frames=1}};
    memset(m.buffer,0xa5,sizeof(m.buffer));memset(m.buffer[0],0x55,256);return m;
}
static void check_copy_recycle(void)
{
    struct CopyModel m=copy_model();
    const struct NezhaCamera1080Io camss={&m,copy_write_csid,copy_write_vfe};
    const struct NezhaCameraMainCaptureIo io={.camss=&camss};
    const struct NezhaCameraFrameMemoryIo memory={&m,copy_acquire,copy_return,copy_fault,copy_event};
    const struct NezhaCameraFrameConsumer consumer={&m,copy_continuing,NULL};
    m.pending=20;m.notices=20;
    unsigned recycled=99;
    assert(nezha_camera_frame_recycle_during_copy(&io,&m.capture,&memory,&consumer,0,&recycled)==NezhaFramesOk);
    assert(recycled==12 && m.recycled==12 && m.pending==8);
    assert(nezha_camera_frame_recycle_during_copy(&io,&m.capture,&memory,&consumer,0,&recycled)==NezhaFramesOk);
    assert(recycled==8 && m.recycled==20 && m.capture.cpu_frames==1 && m.capture.queued_count==2);
    const unsigned writes=m.writes;
    assert(nezha_camera_frame_recycle_during_copy(&io,&m.capture,&memory,&consumer,0,&recycled)==NezhaFramesOk && !recycled && writes==m.writes);
    m.endless=true;
    assert(nezha_camera_frame_recycle_during_copy(&io,&m.capture,&memory,&consumer,0,&recycled)==NezhaFramesOk && !recycled && writes==m.writes);
    for(unsigned mode=0;mode<6;++mode) {
        m=copy_model();m.pending=1;
        m.wrong=mode==0;m.unwritten=mode==1;m.acquire_failure=mode==2;
        m.publication_failure=mode==3;m.queue_failure=mode==4;m.smmu_failure=mode==5;
        const enum NezhaCameraFrameStatus expected=mode==0?NezhaFramesSequence:mode==1?NezhaFramesUnwritten:
            mode==2||mode==3?NezhaFramesCache:mode==4?NezhaFramesIo:NezhaFramesSmmu;
        assert(nezha_camera_frame_recycle_during_copy(&io,&m.capture,&memory,&consumer,0,&recycled)==expected && !m.writes);
        assert(m.capture.cpu_frames&1u);
        for(unsigned i=0;i<256;++i)assert(m.buffer[0][i]==0x55);
    }
    for(unsigned held=1;held<4;++held) {
        m=copy_model();
        assert(nezha_camera_frame_recycle_during_copy(&io,&m.capture,&memory,&consumer,held,&recycled)==NezhaFramesInvalid && !m.writes);
    }
    for(unsigned held=1;held<3;++held) {
        m=copy_model();m.held=held;m.capture.cpu_frames=1u<<held;
        m.capture.queued[0]=(held+1)%3;m.capture.queued[1]=(held+2)%3;m.pending=20;
        memset(m.buffer,0xa5,sizeof(m.buffer));memset(m.buffer[held],0x55,256);
        assert(nezha_camera_frame_recycle_during_copy(&io,&m.capture,&memory,&consumer,held,&recycled)==NezhaFramesOk && recycled==20);
        assert(m.capture.cpu_frames==(1u<<held) && m.capture.queued_count==2);
    }
    for(unsigned mode=0;mode<3;++mode) {
        m=copy_model();m.pending=1;
        m.stopped=mode==0;m.stop_after_acquire=mode==1;m.stop_after_publish=mode==2;
        assert(nezha_camera_frame_recycle_during_copy(&io,&m.capture,&memory,&consumer,0,&recycled)==NezhaFramesOk && !recycled && !m.writes);
        assert(m.capture.cpu_frames==1 && m.acquired==(mode!=0) && m.returned==m.acquired);
    }
    for(unsigned mode=0;mode<2;++mode) {
        m=copy_model();m.capture.queued[1]=mode?3:m.capture.queued[0];
        assert(nezha_camera_frame_recycle_during_copy(&io,&m.capture,&memory,&consumer,0,&recycled)==NezhaFramesInvalid && !m.writes);
    }
    puts("RAW copy recycling:32-event bound,20 newer frames,held-source unchanged,no recursive delivery,faults retain ownership");
}

static void check_spare_pool(void)
{
    struct SpareModel m={.capture={.rdi_started=true,.stream_may_be_on=true,
        .dma.context_active=true,.layout={.layout={48,4,64,64,256},.frame_increment=1,
            .image_address={0x10000,0x20000,0x30000},.frame_count=3},
        .queued={0,1},.queued_count=2,.ready_frames=4}};
    const struct SpareModel initial=m;
    memset(m.buffer,0xa5,sizeof(m.buffer));
    const struct NezhaCamera1080Io camss={&m,spare_write_csid,spare_write_vfe};
    const struct NezhaCameraMainCaptureIo io={.camss=&camss};
    const struct NezhaCameraRegisterIo csid={&m,csid_read,write,wait};
    const struct NezhaCameraFrameMemoryIo memory={&m,spare_acquire,spare_return,fault,spare_event};
    const struct NezhaCameraFrameConsumer consumer={&m,spare_continuing,spare_deliver};
    for (unsigned i=0;i<18;++i) {
        assert(nezha_camera_frame_consume(&io,&m.capture,&csid,&memory,&consumer,0)==NezhaFramesOk);
        assert(m.capture.queued_count==2 && !m.capture.cpu_frames &&
            m.capture.ready_frames==(1u<<m.old) && m.replacements==i+1);
        ++m.completed;
    }
    assert(m.delivered==18 && m.published==18);
    check_copy_recycle();
    for (unsigned mode=0;mode<9;++mode) {
        m=initial;memset(m.buffer,0xa5,sizeof(m.buffer));
        m.stop_in_callback=mode==0;m.delivery_failure=mode==1;
        m.publication_failure=mode==2;m.wrong_address=mode==3;
        if (mode==4) m.capture.ready_frames=0;
        if (mode==5) m.capture.ready_frames=1;
        if (mode==6) m.capture.ready_frames=6;
        if (mode==7) m.capture.queued_count=0;
        if (mode==8) m.capture.queued[1]=2;
        /* Invalid ownership is checked directly because the event fixture
         * intentionally requires an actual idle spare before completion. */
        if (mode>=4) {
            const struct NezhaCamera1080Events event={.buffer_done=true,
                .consumed_address=0x10000,.completed_time=100};
            unsigned index;
            if (mode==4 || mode==6 || mode==8) assert(nezha_camera_main_capture_complete_frame(&m.capture,&event,&index));
            else {
                assert(!nezha_camera_main_capture_complete_frame(&m.capture,&event,&index));
                m.capture.queued_count=1;m.capture.cpu_frames=1;
            }
            assert(!nezha_camera_main_capture_queue_spare(&io,&m.capture) && !m.replacements);
            continue;
        }
        const enum NezhaCameraFrameStatus expected=mode==0 ? NezhaFramesOk:
            mode==1 ? NezhaFramesIo:mode==2 ? NezhaFramesCache:NezhaFramesSequence;
        assert(nezha_camera_frame_consume(&io,&m.capture,&csid,&memory,&consumer,0)==expected);
        if (mode==3) assert(!m.replacements && !m.acquired);
        else {
            assert(m.replacements==1 && m.capture.queued_count==2 && !m.capture.ready_frames);
            assert(m.capture.cpu_frames==(mode==2 ? 1u:0u));
            if (mode==0) {
                const unsigned writes=m.replacements;
                assert(nezha_camera_frame_consume(&io,&m.capture,&csid,&memory,&consumer,0)==NezhaFramesOk);
                assert(m.replacements==writes);
            }
        }
    }
    puts("Three-buffer RAW pool: spare address before CPU access,18 rotations, no completed-buffer reuse, Stop/fault ownership retained");
}

int main(void)
{
    check_spare_pool();
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

    /* An already queued RUP notice does not consume a millisecond of
     * DMA replacement time or hide the completion behind it. */
    for (unsigned budget=0;budget<2;++budget) {
        model=original;capture=streaming(&model);model.pending_notices=20;
        assert(nezha_camera_frame_consume(&io,&capture,&csid,&memory,&consumer,budget?10:0)==NezhaFramesOk);
        assert(!model.pending_notices && model.step==0 && model.delivered==1 &&
               model.published==1 && capture.completed_frames==1);
    }
    /* An unending notice producer still reaches the caller's finite
     * wait budget; none of its notices can fabricate a completed frame. */
    model=original;capture=streaming(&model);model.endless_notices=true;
    assert(nezha_camera_frame_consume(&io,&capture,&csid,&memory,&consumer,0)==NezhaFramesOk);
    assert(model.step==0 && !model.acquired && !capture.completed_frames);
    assert(nezha_camera_frame_consume(&io,&capture,&csid,&memory,&consumer,3)==NezhaFramesTimeout);
    assert(model.step==3 && !model.acquired && !capture.completed_frames);

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
