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
