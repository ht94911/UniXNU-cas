/* SPDX-License-Identifier: MIT */
#include "NezhaCameraCci.h"

#include <stddef.h>

/* Register layout and FIFO packing cross-checked against i2c-qcom-cci.c at
 * Linux 9d66f26fefac and the nezha w33 probe in u0er ed8978ac0087. */
enum {
    CciQueueStart = 0x008,
    CciSclControl = 0x100,
    CciSdaControl0 = 0x104,
    CciSdaControl1 = 0x108,
    CciSdaControl2 = 0x10c,
    CciMiscControl = 0x110,
    CciReadData = 0x118,
    CciReadLevel = 0x11c,
    CciQueueExec = 0x300,
    CciQueueCount = 0x304,
    CciQueueLoad = 0x310,
    CciIrqGlobalClear = 0xc00,
    CciIrqClear = 0xc08,
    CciIrqStatus = 0xc0c,
    CciSetParam = 1,
    CciReport = 8,
    CciWrite = 9,
    CciRead = 10,
    CciReportIrqEnable = 1 << 8,
    CciTimeoutMs = 100,
};

static uint32_t queue_offset(unsigned master, unsigned queue)
{
    return 0x200u * master + 0x100u * queue;
}

static void clear_status(const struct NezhaCameraRegisterIo *io, uint32_t status)
{
    if (status != 0) {
        io->write(io->context, CciIrqClear, status);
        io->write(io->context, CciIrqGlobalClear, 1);
    }
}

static enum NezhaCameraCciStatus run_queue(
    const struct NezhaCameraRegisterIo *io, unsigned master, unsigned queue,
    uint32_t address_word, uint32_t transfer_word,
    struct NezhaCameraCciObservation *observation)
{
    const uint32_t offset = queue_offset(master, queue);
    const unsigned master_shift = 12u * master;
    const uint32_t report = 1u << (4u + master_shift + 4u * queue);
    const uint32_t read_done = queue == 1 ? 1u << master_shift : 0;
    const uint32_t nack = (3u << (27u + 2u * master));
    const uint32_t error = master == 0 ? 0x18000ee6u : 0x60ee6000u;
    const uint32_t handled = report | read_done | error;

    if (io->read(io->context, CciQueueCount + offset) != 0) {
        return NezhaCciBusy;
    }
    clear_status(io, io->read(io->context, CciIrqStatus) & handled);
    if ((io->read(io->context, CciIrqStatus) & handled) != 0) {
        return NezhaCciBusy;
    }

    observation->needs_recovery = true;
    io->write(io->context, CciQueueLoad + offset, address_word);
    io->write(io->context, CciQueueLoad + offset, transfer_word);
    io->write(io->context, CciQueueLoad + offset, CciReport | CciReportIrqEnable);
    if (io->read(io->context, CciQueueCount + offset) != 3) {
        return NezhaCciQueue;
    }
    io->write(io->context, CciQueueExec + offset, 3);
    io->write(io->context, CciQueueStart, 1u << (2u * master + queue));

    for (unsigned ms = 0; ms <= CciTimeoutMs; ++ms) {
        const uint32_t status = io->read(io->context, CciIrqStatus);
        observation->irq = status;
        if ((status & error) != 0) {
            clear_status(io, status & handled);
            return (status & nack) != 0 ? NezhaCciNack : NezhaCciIoError;
        }
        if ((status & report) != 0) {
            clear_status(io, status & handled);
            return NezhaCciOk;
        }
        if (ms != CciTimeoutMs) {
            io->wait_ms(io->context, 1);
        }
    }
    return NezhaCciTimeout;
}

static enum NezhaCameraCciStatus check_request(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint8_t slave,
    struct NezhaCameraCciObservation *observation)
{
    if (observation == NULL) {
        return NezhaCciInvalid;
    }
    *observation = (struct NezhaCameraCciObservation){0};
    if (io == NULL || io->read == NULL || io->write == NULL || io->wait_ms == NULL ||
        master > 1 || slave < 0x08 || slave > 0x77) {
        return NezhaCciInvalid;
    }

    const uint32_t fifo_offset = 0x100u * master;
    observation->fifo_words = io->read(io->context, CciReadLevel + fifo_offset);
    if (observation->fifo_words != 0 ||
        io->read(io->context, CciQueueCount + queue_offset(master, 1)) != 0) {
        return NezhaCciBusy;
    }
    return NezhaCciOk;
}

enum NezhaCameraCciStatus nezha_camera_cci_read_byte(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint8_t slave,
    uint16_t reg, uint8_t *value, struct NezhaCameraCciObservation *observation)
{
    if (value == NULL) {
        return NezhaCciInvalid;
    }
    enum NezhaCameraCciStatus status = check_request(io, master, slave, observation);
    if (status != NezhaCciOk) {
        return status;
    }

    const uint32_t address_word = CciSetParam | ((uint32_t)slave << 4);
    const uint32_t register_word = (CciWrite | (2u << 4)) |
        ((uint32_t)(reg >> 8) << 8) | ((uint32_t)(reg & 0xffu) << 16);
    observation->phase = NezhaCciAddress;
    status = run_queue(io, master, 0, address_word, register_word, observation);
    if (status != NezhaCciOk) {
        return status;
    }

    observation->phase = NezhaCciData;
    status = run_queue(io, master, 1, address_word, CciRead | (1u << 4), observation);
    if (status != NezhaCciOk) {
        return status;
    }

    const uint32_t fifo_offset = 0x100u * master;
    observation->fifo_words = io->read(io->context, CciReadLevel + fifo_offset);
    if (observation->fifo_words != 1) {
        return NezhaCciFifo;
    }
    observation->fifo_word = io->read(io->context, CciReadData + fifo_offset);
    *value = (uint8_t)(observation->fifo_word >> 8);
    observation->needs_recovery = false;
    return NezhaCciOk;
}

enum NezhaCameraCciStatus nezha_camera_cci_write_byte(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint8_t slave,
    uint16_t reg, uint8_t value, struct NezhaCameraCciObservation *observation)
{
    enum NezhaCameraCciStatus status = check_request(io, master, slave, observation);
    if (status != NezhaCciOk) {
        return status;
    }

    const uint32_t address_word = CciSetParam | ((uint32_t)slave << 4);
    /* Put address and data in one I2C message. A separate WRITE opcode
     * for the value would form a second I2C transaction. */
    const uint32_t write_word = (CciWrite | (3u << 4)) |
        ((uint32_t)(reg >> 8) << 8) | ((uint32_t)(reg & 0xffu) << 16) |
        ((uint32_t)value << 24);
    observation->phase = NezhaCciWrite;
    status = run_queue(io, master, 0, address_word, write_word, observation);
    if (status == NezhaCciOk) {
        observation->needs_recovery = false;
    }
    return status;
}

bool nezha_camera_cci_standard_timing(const struct NezhaCameraRegisterIo *io,
    unsigned master, uint32_t clock_hz)
{
    /* The stock CCI standard-mode values are also in Linux i2c-qcom-cci.c. */
    static const uint32_t at_19_2mhz[] = {0x004e0072, 0x001c001c, 0x000a004d, 118, 0x161};
    static const uint32_t at_37_5mhz[] = {0x00c900ae, 0x00cc00e7, 0x001600a2, 227, 0x163};
    const uint32_t *timing = clock_hz == 19200000 ? at_19_2mhz :
        clock_hz == 37500000 ? at_37_5mhz : NULL;
    if (io == NULL || io->write == NULL || master > 1 || timing == NULL) {
        return false;
    }
    const uint32_t offset = 0x100u * master;
    io->write(io->context, CciSclControl + offset, timing[0]);
    io->write(io->context, CciSdaControl0 + offset, timing[1]);
    io->write(io->context, CciSdaControl1 + offset, timing[2]);
    io->write(io->context, CciSdaControl2 + offset, timing[3]);
    io->write(io->context, CciMiscControl + offset, timing[4]);
    return true;
}
