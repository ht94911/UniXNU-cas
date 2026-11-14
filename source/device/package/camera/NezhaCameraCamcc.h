/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_CAMCC_H
#define NEZHA_CAMERA_CAMCC_H

#include "NezhaCameraRegisterIo.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum NezhaCameraCamccStatus {
    NezhaCameraCamccOk,
    NezhaCameraCamccInvalid,
    NezhaCameraCamccBusy,
    NezhaCameraCamccTimeout,
    NezhaCameraCamccSource,
};

struct NezhaCameraCamccLease {
    unsigned enabled_by_us;
    bool top_ready;
    bool cci_rate_owned;
    uint32_t cci_config_before;
    uint32_t cci_config_installed;
};

struct NezhaCameraPll0Lease {
    uint32_t mode_before, opmode_before, saved[7];
    bool owned, configured, ready;
};

struct NezhaCameraCaptureClocks {
    struct NezhaCameraCamccLease gates;
    uint32_t root_before[3];
    unsigned roots, forced_roots, failed_step;
    bool tfe_owned, retain_owned, ready;
};

/* Board interface clocks that stock Canoe enables before touching the
 * camera controllers. The GCC callback updates bit 0 with the shared GCC
 * owner; the CAMCC callback only accesses its owned camera instance. */
struct NezhaCameraGccIo {
    void *context;
    uint32_t (*read)(void *context, uint32_t offset);
    bool (*update)(void *context, uint32_t offset, bool enabled);
};

struct NezhaCameraInterfaces {
    unsigned gcc_owned, camcc_owned;
    uint32_t failed_offset;
    bool ready;
};

enum NezhaCameraCamccStatus nezha_camera_interfaces_start(
    const struct NezhaCameraGccIo *gcc, const struct NezhaCameraRegisterIo *camcc,
    struct NezhaCameraInterfaces *interfaces);
enum NezhaCameraCamccStatus nezha_camera_interfaces_stop(
    const struct NezhaCameraGccIo *gcc, const struct NezhaCameraRegisterIo *camcc,
    struct NezhaCameraInterfaces *interfaces, bool consumers_quiescent);

/* C2 branch group after the top domain, PLL0, CRM votes and interconnect
 * are live. Configure only the software-owned roots: timer and AHB.
 * TFE, CSID, C-PHY RX and CAMNOC rate transitions belong to CRM. */
enum NezhaCameraCamccStatus nezha_camera_camcc_acquire_capture(
    const struct NezhaCameraRegisterIo *io, const struct NezhaCameraCamccLease *top,
    const struct NezhaCameraPll0Lease *pll, struct NezhaCameraCaptureClocks *capture);
enum NezhaCameraCamccStatus nezha_camera_camcc_release_capture(
    const struct NezhaCameraRegisterIo *io, struct NezhaCameraCaptureClocks *capture,
    bool consumers_quiescent);

/* CAM PLL0 serves the PHY timer and the first C-PHY/CSID clock plans.
 * Caller owns this camera domain and has taken the stock calibration
 * floor on VDD_MXC (LOW_SVS_D1 for the 1.306 GHz calibration). A live,
 * correctly configured PLL is borrowed without writes. PLL3 remains
 * under the firmware's CRM clock plan. */
enum NezhaCameraCamccStatus nezha_camera_camcc_acquire_pll0(
    const struct NezhaCameraRegisterIo *io, struct NezhaCameraPll0Lease *lease);
enum NezhaCameraCamccStatus nezha_camera_camcc_release_pll0(
    const struct NezhaCameraRegisterIo *io, struct NezhaCameraPll0Lease *lease,
    bool consumers_quiescent);

/* Requires the platform's exclusive, correctly mapped CAMCC instance.
 * Initialize the lease to zero and retain it for cleanup after failure. */
enum NezhaCameraCamccStatus nezha_camera_camcc_acquire_top(
    const struct NezhaCameraRegisterIo *io, struct NezhaCameraCamccLease *lease);

/* Select XO or the stock 37.5 MHz CCI0 profile from an already running
 * matching PLL0. An enabled branch can only be borrowed at the same rate. */
enum NezhaCameraCamccStatus nezha_camera_camcc_select_cci0_rate(
    const struct NezhaCameraRegisterIo *io, struct NezhaCameraCamccLease *lease,
    uint32_t rate_hz);

/* Call after the platform has selected and verified the CCI0 clock rate. */
enum NezhaCameraCamccStatus nezha_camera_camcc_enable_cci0(
    const struct NezhaCameraRegisterIo *io, struct NezhaCameraCamccLease *lease);

/* Restore the CCI source and gate while PLL0 and the top domain remain
 * live. Capture teardown may then release PLL0 before powering down top. */
enum NezhaCameraCamccStatus nezha_camera_camcc_release_cci0(
    const struct NezhaCameraRegisterIo *io, struct NezhaCameraCamccLease *lease);

/* First quiesce all consumers of the owned clock/domain. Keep ownership if
 * a clock or domain does not confirm restoration. */
enum NezhaCameraCamccStatus nezha_camera_camcc_release(
    const struct NezhaCameraRegisterIo *io, struct NezhaCameraCamccLease *lease);

#ifdef __cplusplus
}
#endif
#endif
