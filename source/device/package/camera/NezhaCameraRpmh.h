/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_RPMH_H
#define NEZHA_CAMERA_RPMH_H

#include "NezhaCameraMainPins.h"
#include "upstream/QcomRpmh.h"

#ifdef __cplusplus
extern "C" {
#endif

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
    bool readback_unavailable;
};

enum NezhaCameraPhyRail { NezhaCameraPhy1p2, NezhaCameraPhy0p9 };
enum NezhaCameraArcRail { NezhaCameraArcMmCx, NezhaCameraArcMxC, NezhaCameraArcMx };

/* A PHY consumer's software request, matching stock VRM get_voltage and
 * is_enabled. Zero initially means this consumer has no local vote; it
 * does not describe the voltage held by remote RPMh masters. */
struct NezhaCameraPhyRequest {
    unsigned millivolts, mode, original_mv, original_mode;
    bool enabled, original_on, voltage_changed, mode_changed, enable_changed;
};

struct NezhaCameraArcState {
    QcomRpmhResource resource;
    /* This APPS consumer's acknowledged request, as in the stock
     * rpmh_regulator_arc_get_voltage_sel. It is not a measured voltage
     * or the aggregate level held by other remote RPMh requesters. */
    uint32_t original_index, current_index, requested_index;
    bool changed;
};

/* Optional common RSC owner. It serializes only register programming and
 * IRQ bit updates; the caller holds the TCS lease across the wait. */
struct NezhaCameraRpmhGuard {
    bool (*enter)(void *context);
    void (*leave)(void *context);
    bool (*write)(void *context, uint32_t offset, uint32_t value);
    bool (*irq_enable)(void *context, bool enabled);
};

struct NezhaCameraRpmh {
    struct NezhaCameraRegisterIo rsc;
    struct NezhaCameraRpmhGuard guard;
    bool (*before_transaction)(void *context, uint32_t address, uint32_t value, bool read);
    void (*vote_acknowledged)(void *context, uint32_t address, uint32_t value, uint32_t ack);
    void *observer_context;
    QcomRscLayout layout;
    struct NezhaCameraVrmState rail[5];
    struct NezhaCameraPhyRequest phy[2];
    struct NezhaCameraArcState arc[3];
    enum NezhaCameraRpmhStatus status;
    uint32_t last_address, last_value, last_irq, last_tcs_status, last_command_status, last_reply;
    bool unsettled, capture_resources, restoring;
    /* Only the isolated CCI identity trial, with no other local camera
     * rail consumers, can test an acknowledged but unreadable vote. */
    bool isolated_sensor_votes;
};

/* The camera-only test calls this after its existing APPS RPMh consumer is
 * finished. A shared integration must use the common APPS TCS owner. */
bool nezha_camera_rpmh_init(struct NezhaCameraRpmh *owner,
    const struct NezhaCameraRegisterIo *rsc, const uint8_t *db, size_t db_bytes);
bool nezha_camera_rpmh_init_guarded(struct NezhaCameraRpmh *owner,
    const struct NezhaCameraRegisterIo *rsc, const struct NezhaCameraRpmhGuard *guard,
    const uint8_t *db, size_t db_bytes);
bool nezha_camera_rpmh_acquire(void *context, enum NezhaCameraMainRail rail, unsigned millivolts);
bool nezha_camera_rpmh_release(void *context, enum NezhaCameraMainRail rail);

/* C2 additions. Resolve all addresses from the live command database.
 * Calls share the same acknowledged APPS TCS transaction as C1. The
 * platform retains its shared resource lease across all calls and owns
 * the APPS rail requests, not the combined state of other RPMh masters. */
bool nezha_camera_rpmh_init_capture(struct NezhaCameraRpmh *owner,
    const uint8_t *db, size_t db_bytes);
bool nezha_camera_rpmh_acquire_phy(struct NezhaCameraRpmh *owner, enum NezhaCameraPhyRail rail);
bool nezha_camera_rpmh_release_phy(struct NezhaCameraRpmh *owner, enum NezhaCameraPhyRail rail);
bool nezha_camera_rpmh_acquire_arc(struct NezhaCameraRpmh *owner,
    enum NezhaCameraArcRail rail, unsigned level);
bool nezha_camera_rpmh_release_arc(struct NezhaCameraRpmh *owner, enum NezhaCameraArcRail rail);

#ifdef __cplusplus
}
#endif
#endif
