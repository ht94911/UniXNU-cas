/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_PREVIEW_H
#define NEZHA_CAMERA_PREVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "upstream/hakios/camera_isp.h"

/* User-space processing of CPU-owned, completed MIPI RAW10. These are
 * processing descriptors, not a new kernel transport or an iOS ABI. */
struct NezhaCameraPreviewFrame {
    uint32_t width, height, stride;
    uint32_t bayer; /* 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR */
    uint32_t black, white;
};

enum NezhaCameraPreviewFormat {
    NezhaCameraPreviewBgra, NezhaCameraPreviewNv12Full, NezhaCameraPreviewNv12Video,
};

struct NezhaCameraPreviewOutput {
    enum NezhaCameraPreviewFormat format;
    uint32_t width, height;
    uint8_t *plane[2];
    size_t stride[2], capacity[2];
};

struct NezhaCameraPreview {
    struct isp processing;
    uint64_t completed_sequence;
    uint32_t graph_generation;
};

void nezha_camera_preview_init(struct NezhaCameraPreview *preview);

/* Measure each completed frame once before its three outputs. HakiOS's
 * grey/neutral-cell estimator smooths the shared red/blue gains; sensor
 * exposure and analog gain remain under the native capture controller. */
bool nezha_camera_preview_update_white_balance(struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw, size_t raw_bytes);

/* Same once-per-frame measurement, optionally used by the real exposure
 * controller. AWB is shared across all graph outputs as before. */
bool nezha_camera_preview_measure(struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame,const uint8_t *raw,size_t raw_bytes,
    struct isp_stats *statistics);

/* Generate a queued exposure request only from actual frame control
 * metadata. Zero/partial fields do not invoke AE or change its state. */
bool nezha_camera_preview_auto_exposure(struct NezhaCameraPreview *preview,
    const struct hakios_camera_frame *actual,const struct isp_stats *statistics,
    struct hakios_camera_command *command);

/* Current stock OVX mode is BGGR, black=64, white=1023. The unchanged
 * upstream processor uses that black/white scale. Other calibrations
 * must be implemented explicitly before they can be accepted here.
 * The caller owns raw and the output for the duration of this call.
 * Acquire CPU ownership of a completed DMA frame and invalidate its cache
 * before passing it to this processor; retain its memory until return. */
bool nezha_camera_preview_render(const struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw, size_t raw_bytes,
    const struct NezhaCameraPreviewOutput *output);

/* StillImage NV12 uses the pinned processor's full Bayer demosaic before
 * scaling. BGRA follows its existing preview path, as in HakiOS. */
bool nezha_camera_still_render(const struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw, size_t raw_bytes,
    const struct NezhaCameraPreviewOutput *output);

/* HakiOS's render_piece splits an output into even row bands. Each call
 * reads the same immutable frame state and owns only these destination
 * rows, including their corresponding NV12 chroma rows. */
bool nezha_camera_preview_render_rows(const struct NezhaCameraPreview *preview,
    const struct NezhaCameraPreviewFrame *frame, const uint8_t *raw, size_t raw_bytes,
    const struct NezhaCameraPreviewOutput *output, uint32_t first_row, uint32_t end_row);

#endif
