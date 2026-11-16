/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_CAMERA_NATIVE_GCC_H
#define NEZHA_CAMERA_NATIVE_GCC_H

#include "NezhaCameraCamcc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bind the five camera-specific GCC gate offsets to the same short
 * configuration lock used by the selected SM8850 native consumers. */
bool nezha_camera_native_gcc(struct NezhaCameraGccIo *io);

#ifdef __cplusplus
}
#endif
#endif
