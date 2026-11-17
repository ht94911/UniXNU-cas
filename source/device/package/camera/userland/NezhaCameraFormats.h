/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_FORMATS_H
#define NEZHA_CAMERA_FORMATS_H

#include <CoreFoundation/CoreFoundation.h>

/* One actual main-sensor mode: binned 4096x3072 RAW10 at 30fps. The
 * system receives rendered BGRA/420f/420v through its own output pools.
 * Caller releases the returned array. This does not publish a device. */
CFArrayRef nezha_camera_main_formats(void);

#endif
