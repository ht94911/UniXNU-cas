/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_CCI_H
#define NEZHA_CAMERA_CCI_H

#include "NezhaCameraRegisterIo.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    NezhaCciHalt,
    NezhaCciReset,
    NezhaCciBurst,
};

enum { NezhaCameraCciCanoeVersion = 0x100a0000, NezhaCameraCciCanoeQueue0Words = 1024 };

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
 * and completion delivery. Up to three sequential bytes fit a FIFO word. */
enum NezhaCameraCciStatus nezha_camera_cci_read(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint8_t slave,
    uint16_t reg, uint8_t *bytes, unsigned count, struct NezhaCameraCciObservation *observation);

enum NezhaCameraCciStatus nezha_camera_cci_read_byte(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint8_t slave,
    uint16_t reg, uint8_t *value, struct NezhaCameraCciObservation *observation);

/* For 16-bit register addresses and one data byte. The owner supplies the
 * validated sensor mode sequence and keeps stream control separate. */
enum NezhaCameraCciStatus nezha_camera_cci_write_byte(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint8_t slave,
    uint16_t reg, uint8_t value, struct NezhaCameraCciObservation *observation);

/* Samsung sensor tables use 16-bit register addresses and big-endian
 * 16-bit values. The address and both bytes are one I2C message. */
enum NezhaCameraCciStatus nezha_camera_cci_write_word(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint8_t slave,
    uint16_t reg, uint16_t value, struct NezhaCameraCciObservation *observation);

/* Queue 0 must belong to this controller owner and support Qualcomm's
 * eleven-byte continuations and the Q0 threshold event. queue_words is
 * the hardware's usable queue depth for this controller. */
enum NezhaCameraCciStatus nezha_camera_cci_write_burst(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint8_t slave,
    uint16_t reg, const uint8_t *bytes, size_t count, unsigned queue_words,
    struct NezhaCameraCciObservation *observation);

/* Select the original standard-mode timing for a measured controller rate.
 * This programs only the selected idle master's timing registers. */
bool nezha_camera_cci_standard_timing(const struct NezhaCameraRegisterIo *io,
    unsigned master, uint32_t clock_hz);

/* Recover a failed transaction. The owner must serialize the CCI controller
 * while the common reset-complete bit is in use. Only the selected master's
 * halt, reset and timing are programmed. */
enum NezhaCameraCciStatus nezha_camera_cci_recover_master(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint32_t clock_hz,
    struct NezhaCameraCciObservation *observation);

#ifdef __cplusplus
}
#endif
#endif
