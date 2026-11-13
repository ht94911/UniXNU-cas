/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_CPAS_H
#define NEZHA_CAMERA_CPAS_H

#include "NezhaCameraRegisterIo.h"
#include <stdbool.h>

enum NezhaCameraCpasStatus {
    NezhaCameraCpasOk,
    NezhaCameraCpasInvalid,
    NezhaCameraCpasVersion,
    NezhaCameraCpasBusy,
    NezhaCameraCpasTimeout,
    NezhaCameraCpasReadback,
};

struct NezhaCameraCpas {
    uint32_t before[3];
    uint32_t status[3];
    unsigned touched;
    bool acquired, ready, drained;
};

/* CPAS top at 0x900b000, with the camera domains, interconnect and all
 * three Q-channels exclusively owned and clocked by the caller. Only the
 * identified Titan 1080 / CPAS 1.0.0 handshake is implemented here. */
enum NezhaCameraCpasStatus nezha_camera_cpas_start(
    const struct NezhaCameraRegisterIo *top, struct NezhaCameraCpas *state);

/* After source, CSID and every writer are stopped, require QACCEPTN=0 on
 * RT/NRT/PDX while their clocks and DMA mappings are still present. A
 * missing acknowledgement retains the lease; it never frees memory. */
enum NezhaCameraCpasStatus nezha_camera_cpas_drain(
    const struct NezhaCameraRegisterIo *top, struct NezhaCameraCpas *state,
    bool producers_stopped);

/* After the drain and DMA detach, restore the inherited requests. This
 * does not release the surrounding CAMCC, GCC or interconnect leases. */
enum NezhaCameraCpasStatus nezha_camera_cpas_restore(
    const struct NezhaCameraRegisterIo *top, struct NezhaCameraCpas *state);

#endif
