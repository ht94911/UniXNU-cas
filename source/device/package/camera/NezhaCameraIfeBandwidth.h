/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_IFE_BANDWIDTH_H
#define NEZHA_CAMERA_IFE_BANDWIDTH_H

#include "upstream/QcomRpmh.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NezhaCameraIfeBcmCount = 6 };

struct NezhaCameraIfeBcmCommand {
    uint32_t address, data;
    bool wait;
};

struct NezhaCameraIfeBandwidth {
    struct NezhaCameraIfeBcmCommand wake[NezhaCameraIfeBcmCount];
    struct NezhaCameraIfeBcmCommand sleep[NezhaCameraIfeBcmCount];
    unsigned count;
};

/* Pure compiler for the stock IFE0 high-frequency route. Inputs are
 * the path's average and peak in bytes/s for the two power states. The
 * caller supplies the actual AOP database and programs the dedicated
 * Camera RSC, whose board voter has no active-only TCS. */
bool nezha_camera_ife_bandwidth(const uint8_t *db, size_t db_bytes,
    uint32_t wake_average, uint32_t wake_peak, uint32_t sleep_average,
    uint32_t sleep_peak, struct NezhaCameraIfeBandwidth *out);

#ifdef __cplusplus
}
#endif
#endif
