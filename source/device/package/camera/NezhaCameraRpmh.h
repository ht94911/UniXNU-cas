/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_RPMH_H
#define NEZHA_CAMERA_RPMH_H

#include "NezhaCameraMainPins.h"
#include "upstream/QcomRpmh.h"

enum NezhaCameraRpmhStatus {
    NezhaCameraRpmhOk,
    NezhaCameraRpmhInvalid,
    NezhaCameraRpmhBusy,
    NezhaCameraRpmhTimeout,
    NezhaCameraRpmhReadback,
};

struct NezhaCameraVrmState {
    uint32_t address;
    unsigned original_mv;
    unsigned observed_mv;
    bool original_on;
    bool observed_on;
    bool voltage_changed;
    bool enable_changed;
};

struct NezhaCameraRpmh {
    struct NezhaCameraRegisterIo rsc;
    QcomRscLayout layout;
    struct NezhaCameraVrmState rail[3];
    enum NezhaCameraRpmhStatus status;
    uint32_t last_address, last_value, last_irq, last_tcs_status, last_command_status, last_reply;
    bool unsettled;
};

/* The camera-only test calls this after its existing APPS RPMh consumer is
 * finished. A shared integration must use the common APPS TCS owner. */
bool nezha_camera_rpmh_init(struct NezhaCameraRpmh *owner,
    const struct NezhaCameraRegisterIo *rsc, const uint8_t *db, size_t db_bytes);
bool nezha_camera_rpmh_acquire(void *context, enum NezhaCameraMainRail rail, unsigned millivolts);
bool nezha_camera_rpmh_release(void *context, enum NezhaCameraMainRail rail);

#endif
