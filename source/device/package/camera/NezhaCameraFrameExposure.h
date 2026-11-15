/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_FRAME_EXPOSURE_H
#define NEZHA_CAMERA_FRAME_EXPOSURE_H

#include "NezhaCameraOvx10500u.h"
#include "userland/upstream/hakios/hakios.camera.h"

/* All four original mode1 modules and their HAL agree: 2246 lines at
 * 30fps, LineCountToExposure rounds 14841.199ns to 14841ns, and exposure,
 * gain and frame length take two completed frames to apply. The original
 * exposure parser gives margin10, minimum1 and maximum12111182 lines.
 * Only this fixed mode's sole worker writes controls between snapshots. */
enum {
    NezhaCameraMode1LineNs = 14841,
    NezhaCameraMode1FrameLines = 2246,
    NezhaCameraMode1MaxLines = 12111182,
    NezhaCameraMode1Margin = 10,
    NezhaCameraControlDelayFrames = 2,
};

static inline bool nezha_camera_frame_exposure(struct hakios_camera_frame *frame,
    const struct NezhaCameraOvxControlReadback *control, uint32_t sampled_after,
    uint32_t completed_sequence)
{
    if (!frame || !control || control->bytes_read != 11 || !sampled_after ||
        completed_sequence < sampled_after ||
        completed_sequence - sampled_after < NezhaCameraControlDelayFrames ||
        !control->exposure_lines || control->exposure_lines > NezhaCameraMode1MaxLines ||
        control->frame_lines < NezhaCameraMode1FrameLines ||
        control->frame_lines > NezhaCameraMode1MaxLines + NezhaCameraMode1Margin ||
        control->exposure_lines > control->frame_lines - NezhaCameraMode1Margin ||
        control->analog_gain_q8 < 256 || control->analog_gain_q8 > 255u * 256u ||
        control->digital_gain_q10 != 1024) return false;
    frame->exposure_lines = control->exposure_lines;
    frame->line_ns = NezhaCameraMode1LineNs;
    frame->gain = control->analog_gain_q8;
    frame->gain_unity = 256;
    frame->gain_max = 255u * 256u;
    frame->frame_lines = control->frame_lines;
    frame->frame_lines_min = NezhaCameraMode1FrameLines;
    frame->frame_lines_max = NezhaCameraMode1MaxLines + NezhaCameraMode1Margin;
    frame->margin = NezhaCameraMode1Margin;
    return true;
}

#endif
