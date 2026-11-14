/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_FRAME_PROBE_H
#define NEZHA_CAMERA_FRAME_PROBE_H

#include "NezhaCameraMainCapture.h"

#ifdef __cplusplus
extern "C" {
#endif

enum NezhaCameraFrameStatus {
    NezhaFramesOk, NezhaFramesInvalid, NezhaFramesTimeout, NezhaFramesIo,
    NezhaFramesSmmu, NezhaFramesSequence, NezhaFramesUnwritten, NezhaFramesCache,
};

struct NezhaCameraFrameMemoryIo {
    void *context;
    uint8_t *(*acquire)(void *context, unsigned buffer);
    bool (*return_frame)(void *context, unsigned buffer, bool requeue);
    uint32_t (*fault)(void *context);
    /* Optional single IRQ consumer. Fill a zero event when none is
     * pending. A queue overflow or hardware read failure returns false. */
    bool (*next_event)(void *context, struct NezhaCamera1080Events *event);
};

struct NezhaCameraFrameResult {
    unsigned frames, updated, sof, eof;
    unsigned final_buffer;
    uint32_t last_rx, last_rdi, last_buffer, last_address, fault;
    uint32_t sample_digest[2];
    bool sampled[2];
};

/* Continuous worker's CPU reader. Deliver finishes before this exact DMA
 * buffer is republished. It returns false on consumer failure. Continue
 * also gates callback-triggered Stop before any requeue. */
struct NezhaCameraFrameConsumer {
    void *context;
    bool (*continuing)(void *);
    bool (*deliver)(void *, const struct NezhaCameraRaw10Layout *, const uint8_t *,
        size_t bytes, uint64_t completed_time, uint32_t sequence);
};

/* Drain one completed frame with a bounded wait from an already running
 * capture. The caller repeats only while its real client remains active,
 * then stops producers/IRQ and drains DMA through the existing teardown. */
enum NezhaCameraFrameStatus nezha_camera_frame_consume(
    const struct NezhaCameraMainCaptureIo *io, struct NezhaCameraMainCapture *capture,
    const struct NezhaCameraRegisterIo *csid, const struct NezhaCameraFrameMemoryIo *memory,
    const struct NezhaCameraFrameConsumer *consumer, unsigned budget_ms);

/* Collect up to 20 completed frames from the already streaming RDI0.
 * A completion must name the expected queue head. Sample several rows
 * after cache acquisition to detect an untouched sentinel or an empty
 * buffer. Refresh those sentinel bytes before reuse, so a stale image
 * cannot pass as a new frame. Digests are diagnostic only. */
enum NezhaCameraFrameStatus nezha_camera_frame_probe(
    const struct NezhaCameraMainCaptureIo *io, struct NezhaCameraMainCapture *capture,
    const struct NezhaCameraRegisterIo *csid, const struct NezhaCameraRegisterIo *vfe,
    const struct NezhaCameraFrameMemoryIo *memory, unsigned target, unsigned budget_ms,
    struct NezhaCameraFrameResult *result);

#ifdef __cplusplus
}
#endif
#endif
