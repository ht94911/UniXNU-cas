/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_CPAS_H
#define NEZHA_CAMERA_CPAS_H

#include "NezhaCameraRegisterIo.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    uint32_t failed_offset, failed_control, failed_status, failed_expected;
    unsigned touched, stop_requested;
    bool acquired, ready, drained;
};

enum { NezhaCameraCpasRt0QosWords = 5 };
struct NezhaCameraCpasRt0Qos {
    uint32_t before[NezhaCameraCpasRt0QosWords];
    uint32_t programmed[NezhaCameraCpasRt0QosWords];
    uint32_t failed_offset, failed_observed, failed_expected;
    unsigned touched, changed;
    bool prepared, applied, restored;
};

/* CPAS top at 0x900b000, with the camera domains, interconnect and all
 * three Q-channels exclusively owned and clocked by the caller. Only the
 * identified Titan 1080 / CPAS 1.0.0 handshake is implemented here. Observe
 * all three channels first, then use the stock forced-on pass if needed. */
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

/* The full TFE0 RDI uses CAMNOC RT0. Apply only the five static priority,
 * urgency and safe/danger words from this phone's v1080_100 table, after
 * all Q-channels are awake and before any producer or DMA is enabled.
 * Other NIUs, smart-QoS, secure registers and error latches are untouched. */
enum NezhaCameraCpasStatus nezha_camera_cpas_rt0_qos_start(
    const struct NezhaCameraRegisterIo *rt, const struct NezhaCameraCpas *cpas,
    struct NezhaCameraCpasRt0Qos *state);

/* Restore this lease only after CPAS has drained and DMA has detached,
 * while the surrounding camera clocks and the RT register map remain on.
 * Any failed read/write retains the changed words for cleanup. */
enum NezhaCameraCpasStatus nezha_camera_cpas_rt0_qos_restore(
    const struct NezhaCameraRegisterIo *rt, const struct NezhaCameraCpas *cpas,
    bool dma_detached, struct NezhaCameraCpasRt0Qos *state);

#ifdef __cplusplus
}
#endif
#endif
