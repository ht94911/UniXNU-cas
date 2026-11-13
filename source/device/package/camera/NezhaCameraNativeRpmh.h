/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_CAMERA_NATIVE_RPMH_H
#define NEZHA_CAMERA_NATIVE_RPMH_H

#include "NezhaCameraRpmh.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A single camera worker borrows the same APPS TCS2 slot used by the
 * microphone. Use the live, read-only command database for this board. */
bool nezha_camera_native_rpmh_open(struct NezhaCameraRpmh **owner,
    const uint8_t *db, size_t db_bytes);

/* Call after restoring owned camera votes. Never hand a half-completed
 * transaction to the next TCS2 owner. */
bool nezha_camera_native_rpmh_close(struct NezhaCameraRpmh **owner);

#ifdef __cplusplus
}
#endif
#endif
