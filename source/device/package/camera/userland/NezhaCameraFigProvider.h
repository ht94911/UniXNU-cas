/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_FIG_PROVIDER_H
#define NEZHA_CAMERA_FIG_PROVIDER_H

#include "NezhaFigCapture24A446.h"
#include "NezhaCameraGraphOutput.h"

/* Backend controls run on the provider's serial queue. They must finish
 * within the native driver's bounded control operation and must not wait
 * for, or call back into, this provider. A completed frame stays owned by
 * its caller until deliver returns. A stop must retain that CPU frame if
 * invoked reentrantly by a graph handler. */
struct NezhaCameraFigBackend {
    void *context;
    NezhaFigStatus (*begin)(void *);
    NezhaFigStatus (*end)(void *);
    /* Optional frame-reader binding. The independent stream owns it;
     * final stream release stops the reader and unbinds it. A temporary
     * device may release its array while the graph retains that stream.
     * A backend can reject a second simultaneous stream. */
    NezhaFigStatus (*bind_stream)(void *, NezhaFigObject);
    /* Queue one original HakiOS exposure command on the frame descriptor.
     * A successful return acknowledges queueing, not hardware application. */
    NezhaFigStatus (*exposure)(void *, const struct hakios_camera_command *);
};

/* Create one rear-wide stream for the already qualified main-sensor
 * backend. formats is the actual capture-source array, with measured
 * sensor geometry/rates and the graph outputs this backend implements.
 * The caller keeps backend.context alive until its last stream is finalized.
 * The returned object is a FigCaptureDevice with one owned stream. */
NezhaFigStatus nezha_camera_fig_provider_create(NezhaFigAllocator allocator,
    const struct NezhaCameraFigBackend *backend, CFArrayRef formats,
    NezhaFigObject *device);

/* Synchronously process a completed, CPU-owned main-sensor frame on the
 * provider queue. metadata carries that frame's real control/readback
 * values. Returns unavailable while only reserved or after graph Stop.
 * The backend retains and releases its own RAW/DMA storage around this
 * call; the provider never accepts a hardware completion inferred from
 * an unrelated counter or a template frame. */
NezhaFigStatus nezha_camera_fig_provider_deliver(NezhaFigObject device,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw,
    size_t raw_bytes, uint64_t completed_time, CFDictionaryRef metadata);

/* Queue one completed CPU frame without blocking its reader behind the
 * property queue. The caller retains raw until release(context); the
 * provider retains its stream while queued. Stop/restart discards an old
 * reservation's queued frame through the same stream generation gate. */
NezhaFigStatus nezha_camera_fig_stream_enqueue(NezhaFigObject stream,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw,
    size_t raw_bytes, uint64_t completed_time, CFDictionaryRef metadata,
    void (*release)(void *), void *context);

#endif
