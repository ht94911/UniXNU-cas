/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_DEVICE_BACKEND_H
#define NEZHA_CAMERA_DEVICE_BACKEND_H

/* Bind the single native /dev/nezha-camera reader into the tested factory.
 * The system vendor must still install nezha_camera_device_create. */
int nezha_camera_device_backend_bind(void);

#endif
