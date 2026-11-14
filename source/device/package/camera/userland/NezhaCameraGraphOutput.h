/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_GRAPH_OUTPUT_H
#define NEZHA_CAMERA_GRAPH_OUTPUT_H

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include "NezhaCameraPixelBuffer.h"

typedef void (^NezhaCameraPixelHandler)(CVPixelBufferRef, CMTime);

/* One output of the provider, operated on its serial frame/property queue.
 * Use the graph's BufferPool and PixelBufferHandler. The three preview
 * references follow HakiOS's measured deferred-display ownership; still
 * outputs release after delivery. Start gates graph callbacks separately
 * from a sensor's reservation/warm-up. Initialize this object to zero. */
struct NezhaCameraGraphOutput {
    CVPixelBufferPoolRef pool;
    NezhaCameraPixelHandler handler;
    CVPixelBufferRef held[3];
    unsigned oldest, count, generation;
    bool running, preview;
};

CVReturn nezha_camera_graph_output_configure(struct NezhaCameraGraphOutput *output,
    CVPixelBufferPoolRef pool, NezhaCameraPixelHandler handler, bool preview);
CVReturn nezha_camera_graph_output_start(struct NezhaCameraGraphOutput *output);
void nezha_camera_graph_output_stop(struct NezhaCameraGraphOutput *output);
void nezha_camera_graph_output_reset(struct NezhaCameraGraphOutput *output);

/* Metadata is an actual per-frame dictionary supplied by the provider.
 * Timestamp is that completed frame's mach_absolute_time clock, not a
 * receipt time. A successful call has rendered/unlocked and invoked the
 * graph handler; it does not prove the application displayed the frame. */
CVReturn nezha_camera_graph_output_deliver(struct NezhaCameraGraphOutput *output,
    const struct NezhaCameraPreview *preview, const struct NezhaCameraPreviewFrame *frame,
    const uint8_t *raw, size_t raw_bytes, uint64_t timestamp, CFDictionaryRef metadata);

#endif
