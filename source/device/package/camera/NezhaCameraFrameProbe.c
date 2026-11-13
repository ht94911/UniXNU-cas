/* SPDX-License-Identifier: MIT */
#include "NezhaCameraFrameProbe.h"

static size_t sample_offset(const struct NezhaCameraRaw10Layout *layout, unsigned row)
{
    const size_t y = (size_t)((uint64_t)row * (layout->height - 1) / 3);
    return y * layout->stride + layout->packed_line_bytes / 2 - 32;
}

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
        const uint8_t *line = frame + sample_offset(&rdi->layout, row);
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
        if (!nezha_camera1080_take_rdi0_events(csid, vfe, &events)) {
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
                    uint8_t *line = frame + sample_offset(&capture->layout.layout, row);
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
                return result->updated && result->sof && result->eof ? NezhaFramesOk : NezhaFramesSequence;
            }
        }
        if (ms < budget_ms) {
            csid->wait_ms(csid->context, 1);
        }
    }
    return NezhaFramesTimeout;
}
