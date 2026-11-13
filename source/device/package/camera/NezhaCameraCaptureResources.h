/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_CAPTURE_RESOURCES_H
#define NEZHA_CAMERA_CAPTURE_RESOURCES_H

#include "NezhaCameraCamcc.h"
#include "NezhaCameraCpas.h"
#include "NezhaCameraCrm.h"
#include "NezhaCameraCsiphy24.h"
#include "NezhaCameraIfeRsc.h"
#include "NezhaCameraRpmh.h"

struct NezhaCameraCaptureResourceIo {
    const struct NezhaCameraGccIo *gcc;
    const struct NezhaCameraRegisterIo *camcc, *crm, *crmc, *crm_common, *ife_rsc, *cpas;
    const struct NezhaCameraCsiphy24Io *phy;
    struct NezhaCameraRpmh *rpmh;
};

enum NezhaCameraResourceStep {
    NezhaResourcesIdle, NezhaResourcesArc, NezhaResourcesPhyRails, NezhaResourcesInterface,
    NezhaResourcesTop, NezhaResourcesPll0, NezhaResourcesCrmIdentity, NezhaResourcesCrmClock,
    NezhaResourcesCciClock, NezhaResourcesCaptureClock, NezhaResourcesIfeRsc,
    NezhaResourcesCpas, NezhaResourcesPhy, NezhaResourcesReady,
};

struct NezhaCameraCaptureResources {
    enum NezhaCameraResourceStep step, cleanup_step;
    unsigned status, detail;
    struct NezhaCameraInterfaces interfaces;
    struct NezhaCameraCamccLease top;
    struct NezhaCameraPll0Lease pll0;
    struct NezhaCameraCrmLease clocks[4];
    struct NezhaCameraCaptureClocks capture;
    struct NezhaCameraIfeRsc ife;
    struct NezhaCameraCpas cpas;
    struct NezhaCameraCsiphy24State phy;
};

/* Normal worker, with exclusive camera and RPMh ownership. No capture or
 * sensor MMIO runs before this sequence succeeds. Use the actual selected
 * stock C-PHY tables and separately calculated IFE0 bandwidth. */
bool nezha_camera_capture_resources_start(const struct NezhaCameraCaptureResourceIo *io,
    const struct NezhaCameraIfeBandwidth *bandwidth, const struct NezhaCameraCsiphy24Profile *profile,
    struct NezhaCameraCaptureResources *state);

/* Stop producers, then establish a positive DMA drain. The caller keeps
 * translation and buffers alive until this succeeds. */
bool nezha_camera_capture_resources_drain(const struct NezhaCameraCaptureResourceIo *io,
    struct NezhaCameraCaptureResources *state, bool producers_stopped);

/* After CCI/sensor cleanup and SMMU detach, release the dependencies from
 * child to parent. The first missing acknowledgement retains its parents. */
bool nezha_camera_capture_resources_stop(const struct NezhaCameraCaptureResourceIo *io,
    const struct NezhaCameraIfeBandwidth *clear, const struct NezhaCameraCsiphy24Profile *profile,
    struct NezhaCameraCaptureResources *state, bool sensor_quiet, bool dma_detached);

#endif
