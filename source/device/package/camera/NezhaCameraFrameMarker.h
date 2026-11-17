/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_FRAME_MARKER_H
#define NEZHA_CAMERA_FRAME_MARKER_H

#include "NezhaCameraRdi1080.h"

enum { NezhaCameraFrameMarkerRows = 4, NezhaCameraFrameMarkerBytes = 64 };

/* The CPU's only writes into a completed DMA frame are these four stale
 * frame markers. The consumer receives a const view of all other bytes. */
static inline size_t nezha_camera_frame_marker_offset(
    const struct NezhaCameraRaw10Layout *layout, unsigned row)
{
    const size_t y = (size_t)((uint64_t)row * (layout->height - 1) /
        (NezhaCameraFrameMarkerRows - 1));
    return y * layout->stride + layout->packed_line_bytes / 2 -
        NezhaCameraFrameMarkerBytes / 2;
}

#endif
