/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_FACTORY_H
#define NEZHA_CAMERA_FACTORY_H

#include "NezhaCameraFigProvider.h"

/* The selected 24A446 vendor's three-argument default create function.
 * The platform binds one qualified backend before installing this factory
 * in BWFigCaptureDeviceVendor. Bind once; the backend context has process
 * lifetime. No device is exposed while ready returns false. */
NezhaFigStatus nezha_camera_factory_bind(const struct NezhaCameraFigBackend *backend,
    bool (*ready)(void *));
NezhaFigStatus nezha_camera_device_create(NezhaFigAllocator allocator,
    NezhaFigString device_id, NezhaFigObject *device);

#endif
