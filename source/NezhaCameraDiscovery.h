/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_DISCOVERY_H
#define NEZHA_CAMERA_DISCOVERY_H

/* Install bounded observers in the existing diagnostic client. The
 * original AVFCapture methods keep their result and ownership contracts. */
void nezha_camera_discovery_observe(void);

#endif
