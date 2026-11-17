/* SPDX-License-Identifier: MIT */
#include "NezhaCameraAk7316.h"
#include <stddef.h>

/* Original local CamX631610 parser and13d1220 movement over the four
 * measured nezha OVX modules: AK7316 slave0x18/7bits0x0c, 12bit DAC,
 * register0/address1/data2/shift4; init02=0/1900us, poll02=0/5000us,
 * then00=0x5460. Deinit02=0/1900us. Preserve the first CCI error. */
enum { Master = 1, Slave = 0x0c, Control = 2, Position = 0 };

static bool valid(const struct NezhaCameraRegisterIo *io, struct NezhaCameraAk7316Result *result)
{
    if (!result) return false;
    *result = (struct NezhaCameraAk7316Result){0};
    return io && io->read && io->write && io->wait_ms;
}

static enum NezhaCameraCciStatus read_position(const struct NezhaCameraRegisterIo *io,
    struct NezhaCameraAk7316Result *result)
{
    uint8_t bytes[2];
    const enum NezhaCameraCciStatus status = nezha_camera_cci_read_reg8(io, Master, Slave,
        Position, bytes, 2, &result->transfer);
    if (status != NezhaCciOk) return status;
    ++result->completed_reads;
    result->register_value = (uint16_t)((uint16_t)bytes[0] << 8 | bytes[1]);
    if (result->register_value & 15u) return NezhaCciFifo;
    result->position_dac = result->register_value >> 4;
    result->position_read = true;
    return NezhaCciOk;
}

enum NezhaCameraCciStatus nezha_camera_ak7316_read_position(
    const struct NezhaCameraRegisterIo *io, struct NezhaCameraAk7316Result *result)
{
    if (!valid(io, result)) return NezhaCciInvalid;
    return read_position(io, result);
}

enum NezhaCameraCciStatus nezha_camera_ak7316_initialize(
    const struct NezhaCameraRegisterIo *io, struct NezhaCameraAk7316Result *result)
{
    if (!valid(io, result)) return NezhaCciInvalid;
    enum NezhaCameraCciStatus status = nezha_camera_cci_write_reg8_byte(io, Master, Slave,
        Control, 0, &result->transfer);
    if (status != NezhaCciOk) return status;
    ++result->completed_writes;
    io->wait_ms(io->context, 2); /* Existing millisecond wait covers1900us. */
    bool ready = false;
    /* Stock cam_cci_i2c_poll performs five reads, each unsuccessful read
     * followed by one millisecond. Do not append another attempt. */
    for (unsigned elapsed = 0; elapsed < 5; ++elapsed) {
        uint8_t control;
        status = nezha_camera_cci_read_reg8(io, Master, Slave, Control, &control, 1, &result->transfer);
        if (status != NezhaCciOk) return status;
        ++result->completed_reads;
        result->control = control;
        if (control == 0) { ready = true; break; }
        io->wait_ms(io->context, 1);
    }
    if (!ready) return NezhaCciTimeout;
    status = nezha_camera_cci_write_reg8_word(io, Master, Slave, Position, 0x5460, &result->transfer);
    if (status != NezhaCciOk) return status;
    ++result->completed_writes;
    status = read_position(io, result);
    return status != NezhaCciOk ? status : result->register_value == 0x5460 ? NezhaCciOk : NezhaCciFifo;
}

enum NezhaCameraCciStatus nezha_camera_ak7316_set_position(
    const struct NezhaCameraRegisterIo *io, uint16_t dac, struct NezhaCameraAk7316Result *result)
{
    if (!valid(io, result) || dac > NezhaCameraAk7316MaxDac) return NezhaCciInvalid;
    const uint16_t target = (uint16_t)(dac << 4);
    enum NezhaCameraCciStatus status = nezha_camera_cci_write_reg8_word(io, Master, Slave,
        Position, target, &result->transfer);
    if (status != NezhaCciOk) return status;
    ++result->completed_writes;
    status = read_position(io, result);
    return status != NezhaCciOk ? status : result->register_value == target ? NezhaCciOk : NezhaCciFifo;
}

enum NezhaCameraCciStatus nezha_camera_ak7316_deinitialize(
    const struct NezhaCameraRegisterIo *io, struct NezhaCameraAk7316Result *result)
{
    if (!valid(io, result)) return NezhaCciInvalid;
    const enum NezhaCameraCciStatus status = nezha_camera_cci_write_reg8_byte(io, Master, Slave,
        Control, 0, &result->transfer);
    if (status != NezhaCciOk) return status;
    ++result->completed_writes;
    io->wait_ms(io->context, 2);
    return NezhaCciOk;
}
