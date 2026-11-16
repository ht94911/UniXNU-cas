/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_CONTROL_H
#define NEZHA_CAMERA_CONTROL_H

#include "userland/upstream/hakios/hakios.camera.h"

/* Darwin _IOW('N', 0x31, struct hakios_camera_command), fixed 24-byte
 * input. Keep the original read-only frame descriptor; this one command
 * queues exposure for its owner and never grants general vnode writes. */
#define NEZHA_CAMERA_EXPOSURE_IOCTL 0x80184e31UL

/* Original mode1 Q8 analog quantization, shared by the command producer
 * and CCI plan so a rounded request can match its later real readback. */
static inline uint16_t nezha_camera_control_gain_q8(uint32_t gain)
{
    uint32_t step=2,threshold=2u*256u;
    while(gain>threshold && step<256) {step<<=1;threshold<<=1;}
    return (uint16_t)(gain&~(step-1u));
}

#endif
