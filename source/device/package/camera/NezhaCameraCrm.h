/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_CRM_H
#define NEZHA_CAMERA_CRM_H

#include "NezhaCameraRegisterIo.h"
#include <stdbool.h>

enum NezhaCameraCrmClock {
    NezhaCameraCrmTfe0 = 0,
    NezhaCameraCrmCsid = 6,
    NezhaCameraCrmCphyRx = 7,
    NezhaCameraCrmCamnoc = 8,
};

enum NezhaCameraCrmStatus {
    NezhaCameraCrmOk,
    NezhaCameraCrmInvalid,
    NezhaCameraCrmBusy,
    NezhaCameraCrmNoRate,
    NezhaCameraCrmTimeout,
    NezhaCameraCrmReadback,
};

/* Resolve the selected parent output, including its PLL post-divider. The
 * firmware LUT supplies L/alpha for a CRM PLL; a zero L uses the live parent.
 * This callback is read-only and does not enable or reprogram the parent. */
struct NezhaCameraCrmParent {
    void *context;
    uint32_t (*rate)(void *context, enum NezhaCameraCrmClock clock,
                     unsigned source, unsigned l, unsigned alpha);
};

/* Stock Canoe parent-map decoder for the three clocks below. context points
 * to a NezhaCameraRegisterIo relative to the complete CAMCC aperture. It
 * reads PLL0/3/6, so the C1-only +0x21000 page mapping is insufficient. */
uint32_t nezha_camera_crm_canoe_parent_rate(void *context,
    enum NezhaCameraCrmClock clock, unsigned source, unsigned l, unsigned alpha);

struct NezhaCameraCrmPlan {
    enum NezhaCameraCrmClock clock;
    uint32_t rate[7];
    unsigned count;
};

struct NezhaCameraCrmLease {
    enum NezhaCameraCrmClock clock;
    uint32_t before[3];
    uint32_t irq_enable_before;
    uint32_t installed, pending_vote;
    bool requested, pending, ready;
};

/* CRMC is the stock qcom,cam_crm-crmc aperture at 0x954a600. Read the
 * increasing firmware LUT, preserving OFF at level 0. Zero/non-increasing
 * entries terminate it, as in clk_rcg2_crmc_populate_freq_table. */
enum NezhaCameraCrmStatus nezha_camera_crm_load_plan(
    const struct NezhaCameraRegisterIo *crmc, const struct NezhaCameraCrmParent *parent,
    enum NezhaCameraCrmClock clock, struct NezhaCameraCrmPlan *plan);

/* Exclusive SW DRV0 ownership and a mapped, clocked qcom,cam-crm-v3 instance
 * are prerequisites. drv is relative to stock 0x9548000. Its other VCDs,
 * hardware voters and global enable are not modified. The platform
 * holds the SW DRV0 interrupt masked at the GIC or consumes it here.
 * Retain a failed lease: a timed-out active request must not be replayed. */
enum NezhaCameraCrmStatus nezha_camera_crm_acquire(
    const struct NezhaCameraRegisterIo *drv, const struct NezhaCameraCrmPlan *plan,
    uint32_t minimum_hz, struct NezhaCameraCrmLease *lease);

/* Camera RT/NRT CAMNOC root: SW DRV0 bandwidth node 0 receives the
 * selected CAMNOC rate in MHz. This is separate from DDR/LLCC votes. */
enum NezhaCameraCrmStatus nezha_camera_crm_acquire_camnoc(
    const struct NezhaCameraRegisterIo *drv, uint32_t minimum_hz, struct NezhaCameraCrmLease *lease);

/* Capture/DMA/IRQs must be stopped before restoration. A timed-out request
 * can only be reconciled by its own completion, never by another request. */
enum NezhaCameraCrmStatus nezha_camera_crm_release(
    const struct NezhaCameraRegisterIo *drv, struct NezhaCameraCrmLease *lease,
    bool consumers_quiescent);

#endif
