/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_IFE_RSC_H
#define NEZHA_CAMERA_IFE_RSC_H

#include "NezhaCameraIfeBandwidth.h"
#include "NezhaCameraRegisterIo.h"

enum NezhaCameraIfeRscStatus {
    NezhaCameraIfeRscOk,
    NezhaCameraIfeRscInvalid,
    NezhaCameraIfeRscBusy,
    NezhaCameraIfeRscReadback,
    NezhaCameraIfeRscTimeout,
};

struct NezhaCameraIfeRsc {
    uint32_t address[NezhaCameraIfeBcmCount];
    uint32_t previous[4][NezhaCameraIfeBcmCount][3];
    uint32_t enable, update, busy, completion;
    bool owned, ready, pending, releasing;
};

/* Stock cam_rsc DRV0 (0x9545000), exclusive to this IFE0 session. The
 * caller must keep its CAMCC interface and power live until release. An
 * inherited enabled channel or any configured TCS is left in place. */
enum NezhaCameraIfeRscStatus nezha_camera_ife_rsc_start(const struct NezhaCameraRegisterIo *io,
    const struct NezhaCameraIfeBandwidth *plan, struct NezhaCameraIfeRsc *state);

/* After CPAS drain, apply a zero vote on the unused channel and restore
 * the empty inherited configuration only after its wake acknowledgement. */
enum NezhaCameraIfeRscStatus nezha_camera_ife_rsc_stop(const struct NezhaCameraRegisterIo *io,
    const struct NezhaCameraIfeBandwidth *clear, struct NezhaCameraIfeRsc *state, bool drained);

#endif
