/* SPDX-License-Identifier: MIT */
#include "NezhaCameraFrameProbe.h"
#include "NezhaCameraFrameMarker.h"

static bool sample(const uint8_t *frame, const struct NezhaCameraRdi1080 *rdi, uint32_t *digest)
{
    uint32_t hash = 2166136261u;
    const uint32_t stride = rdi->layout.stride;
    const uint32_t bytes = rdi->layout.frame_bytes;
    const uint32_t height = rdi->layout.height;
    const uint32_t width = rdi->layout.packed_line_bytes;
    if (!frame || width < 64 || stride < width || height < 4 || bytes / stride < height) {
        return false;
    }
    for (unsigned row = 0; row < 4; ++row) {
        const uint8_t *line = frame + nezha_camera_frame_marker_offset(&rdi->layout, row);
        bool changed = false, nonzero = false;
        for (unsigned i = 0; i < 64; ++i) {
            changed |= line[i] != 0xa5;
            nonzero |= line[i] != 0;
            hash = (hash ^ line[i]) * 16777619u;
        }
        if (!changed || !nonzero) {
            return false;
        }
    }
    *digest = hash;
    return true;
}

enum NezhaCameraFrameStatus nezha_camera_frame_consume(
    const struct NezhaCameraMainCaptureIo *io, struct NezhaCameraMainCapture *capture,
    const struct NezhaCameraRegisterIo *csid, const struct NezhaCameraFrameMemoryIo *memory,
    const struct NezhaCameraFrameConsumer *consumer, unsigned budget_ms)
{
    if (!io || !capture || !capture->rdi_started || !capture->stream_may_be_on ||
        !csid || !csid->wait_ms || !memory || !memory->next_event || !memory->acquire ||
        !memory->return_frame || !memory->fault || !consumer || !consumer->continuing ||
        !consumer->deliver || !budget_ms || budget_ms > 3000) return NezhaFramesInvalid;
    for (unsigned ms=0;ms<=budget_ms;++ms) {
        if (!consumer->continuing(consumer->context)) return NezhaFramesOk;
        if (memory->fault(memory->context)) return NezhaFramesSmmu;
        struct NezhaCamera1080Events event;
        if (!memory->next_event(memory->context,&event)) return NezhaFramesIo;
        if (event.buffer_done) {
            if (!event.completed_time || capture->completed_frames==UINT32_MAX) return NezhaFramesSequence;
            unsigned index;
            if (!nezha_camera_main_capture_complete_frame(capture,&event,&index)) return NezhaFramesSequence;
            uint8_t *frame=memory->acquire(memory->context,index);
            if (!frame) {
                (void)nezha_camera_main_capture_release_frame(io,capture,index,false);
                return NezhaFramesCache;
            }
            uint32_t digest;
            const bool changed=sample(frame,&capture->layout,&digest);
            const bool delivered=changed && consumer->deliver(consumer->context,&capture->layout.layout,
                frame,capture->layout.layout.frame_bytes,event.completed_time,capture->completed_frames);
            const bool requeue=delivered && consumer->continuing(consumer->context);
            if (requeue) {
                for (unsigned row=0;row<4;++row) {
                    uint8_t *line=frame+nezha_camera_frame_marker_offset(&capture->layout.layout,row);
                    for (unsigned i=0;i<64;++i) line[i]=0xa5;
                }
            }
            if (!memory->return_frame(memory->context,index,requeue)) return NezhaFramesCache;
            if (!nezha_camera_main_capture_release_frame(io,capture,index,requeue)) return NezhaFramesSequence;
            return !changed ? NezhaFramesUnwritten : !delivered ? NezhaFramesIo : NezhaFramesOk;
        }
        if (ms<budget_ms) csid->wait_ms(csid->context,1);
    }
    return NezhaFramesTimeout;
}

enum NezhaCameraFrameStatus nezha_camera_frame_probe(
    const struct NezhaCameraMainCaptureIo *io, struct NezhaCameraMainCapture *capture,
    const struct NezhaCameraRegisterIo *csid, const struct NezhaCameraRegisterIo *vfe,
    const struct NezhaCameraFrameMemoryIo *memory, unsigned target, unsigned budget_ms,
    struct NezhaCameraFrameResult *result)
{
    if (!io || !capture || !capture->rdi_started || !capture->stream_may_be_on ||
        !csid || !csid->wait_ms || !vfe || !memory || !memory->acquire ||
        !memory->return_frame || !memory->fault || target < 1 || target > 20 ||
        !budget_ms || budget_ms > 3000 || !result) {
        return NezhaFramesInvalid;
    }
    *result = (struct NezhaCameraFrameResult){0};
    for (unsigned ms = 0; ms <= budget_ms; ++ms) {
        result->fault = memory->fault(memory->context);
        if (result->fault) {
            return NezhaFramesSmmu;
        }
        struct NezhaCamera1080Events events;
        const bool received = memory->next_event ? memory->next_event(memory->context, &events) :
            nezha_camera1080_take_rdi0_events(csid, vfe, &events);
        if (!received) {
            return NezhaFramesIo;
        }
        result->updated += events.updated;
        result->sof += events.start_of_frame;
        result->eof += events.end_of_frame;
        result->last_rx |= events.rx_irq;
        result->last_rdi |= events.rdi0_irq;
        result->last_buffer |= events.buffer_irq;
        if (events.buffer_done) {
            result->last_address = events.consumed_address;
            unsigned buffer;
            if (!nezha_camera_main_capture_complete_frame(capture, &events, &buffer)) {
                return NezhaFramesSequence;
            }
            uint8_t *frame = memory->acquire(memory->context, buffer);
            if (!frame) {
                (void)nezha_camera_main_capture_release_frame(io, capture, buffer, false);
                return NezhaFramesCache;
            }
            const bool changed = sample(frame, &capture->layout, &result->sample_digest[buffer]);
            result->sampled[buffer] |= changed;
            result->frames += changed;
            const bool requeue = changed && result->frames < target;
            if (requeue) {
                for (unsigned row = 0; row < 4; ++row) {
                    uint8_t *line = frame + nezha_camera_frame_marker_offset(&capture->layout.layout, row);
                    for (unsigned i = 0; i < 64; ++i) line[i] = 0xa5;
                }
            }
            if (!memory->return_frame(memory->context, buffer, requeue)) {
                /* A failed publication keeps the native CPU claim. The
                 * caller must stop and resolve it before freeing memory. */
                return NezhaFramesCache;
            }
            if (!nezha_camera_main_capture_release_frame(io, capture, buffer, requeue)) {
                (void)nezha_camera_main_capture_release_frame(io, capture, buffer, false);
                return NezhaFramesSequence;
            }
            if (!changed) {
                return NezhaFramesUnwritten;
            }
            if (result->frames == target) {
                result->final_buffer = buffer;
                return result->updated && result->sof && result->eof ? NezhaFramesOk : NezhaFramesSequence;
            }
        }
        if (ms < budget_ms) {
            csid->wait_ms(csid->context, 1);
        }
    }
    return NezhaFramesTimeout;
}
