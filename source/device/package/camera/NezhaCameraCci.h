/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_CCI_H
#define NEZHA_CAMERA_CCI_H

#include "NezhaCameraRegisterIo.h"
#include <stdbool.h>

enum NezhaCameraCciStatus {
    NezhaCciOk = 0,
    NezhaCciInvalid,
    NezhaCciBusy,
    NezhaCciQueue,
    NezhaCciNack,
    NezhaCciIoError,
    NezhaCciTimeout,
    NezhaCciFifo,
};

enum NezhaCameraCciPhase {
    NezhaCciIdle = 0,
    NezhaCciAddress,
    NezhaCciData,
    NezhaCciWrite,
};

struct NezhaCameraCciObservation {
    enum NezhaCameraCciPhase phase;
    uint32_t irq;
    uint32_t fifo_words;
    uint32_t fifo_word;
    /* Do not submit another transfer until the owner has recovered the
     * queues and drained the FIFO after a partial submission. */
    bool needs_recovery;
};

/* The caller owns the powered controller's selected master, both idle queues,
 * and completion delivery. Handle any halt/reset before transfer. */
enum NezhaCameraCciStatus nezha_camera_cci_read_byte(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint8_t slave,
    uint16_t reg, uint8_t *value, struct NezhaCameraCciObservation *observation);

/* For 16-bit register addresses and one data byte. The owner supplies the
 * validated sensor mode sequence and keeps stream control separate. */
enum NezhaCameraCciStatus nezha_camera_cci_write_byte(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint8_t slave,
    uint16_t reg, uint8_t value, struct NezhaCameraCciObservation *observation);

/* Select the original standard-mode timing for a measured controller rate.
 * This programs only the selected idle master's timing registers. */
bool nezha_camera_cci_standard_timing(const struct NezhaCameraRegisterIo *io,
    unsigned master, uint32_t clock_hz);

#endif
