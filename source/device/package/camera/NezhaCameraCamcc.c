/* SPDX-License-Identifier: MIT */
#include "NezhaCameraCamcc.h"

#include <stddef.h>

/* The same-device CAMCC GDSC/AHB probe at u0er ed8978ac0087. This
 * sequence owns only the top and CCI0 gates, and does not program a PLL. */
enum {
    CamccTopGdsc = 0x21334,
    CamccTopStatus = 0x21338,
    CamccCoreAhb = 0x21348,
    CamccCpasAhb = 0x2137c,
    CamccCci0Command = 0x21250,
    CamccCci0Config = 0x21254,
    CamccCci0 = 0x21268,
    CamccPll0Mode = 0x0000,
    CamccPll0Opmode = 0x0004,
    CamccPll0L = 0x0010,
    CamccPll0Alpha = 0x0014,
    CamccPll0User = 0x0018,
    CamccCollapse = 1,
    CamccRetain = 1 << 11,
    CamccPowerDown = 1 << 15,
    CamccPowerUp = 1 << 16,
    CamccEnable = 1,
    CamccUpdate = 1,
    CamccRateMask = 0x371f, /* source, HID and fractional mode */
    CamccTimeoutMs = 100,
    OwnTop = 1,
    OwnRetain = 2,
    OwnCoreAhb = 4,
    OwnCpasAhb = 8,
    OwnCci0 = 16,
};
static const uint32_t clock_off = UINT32_C(1) << 31;

static bool io_valid(const struct NezhaCameraRegisterIo *io)
{
    return io != NULL && io->read != NULL && io->write != NULL && io->wait_ms != NULL;
}

static void change_bits(const struct NezhaCameraRegisterIo *io,
                        uint32_t offset, uint32_t mask, uint32_t value)
{
    const uint32_t current = io->read(io->context, offset);
    const uint32_t next = (current & ~mask) | value;
    if (current != next) {
        io->write(io->context, offset, next);
    }
}

static bool wait_bits(const struct NezhaCameraRegisterIo *io,
                      uint32_t offset, uint32_t mask, uint32_t value)
{
    for (unsigned ms = 0; ms <= CamccTimeoutMs; ++ms) {
        if ((io->read(io->context, offset) & mask) == value) {
            return true;
        }
        if (ms < CamccTimeoutMs) {
            io->wait_ms(io->context, 1);
        }
    }
    return false;
}

static bool enable_branch(const struct NezhaCameraRegisterIo *io,
                          struct NezhaCameraCamccLease *lease,
                          uint32_t offset, unsigned owned_bit, bool has_halt)
{
    if ((io->read(io->context, offset) & CamccEnable) == 0) {
        lease->enabled_by_us |= owned_bit;
        change_bits(io, offset, CamccEnable, CamccEnable);
        if (!has_halt) {
            io->wait_ms(io->context, 1);
        }
    }
    const uint32_t mask = CamccEnable | (has_halt ? clock_off : 0);
    return wait_bits(io, offset, mask, CamccEnable);
}

enum NezhaCameraCamccStatus nezha_camera_camcc_acquire_top(
    const struct NezhaCameraRegisterIo *io, struct NezhaCameraCamccLease *lease)
{
    if (!io_valid(io) || lease == NULL) {
        return NezhaCameraCamccInvalid;
    }
    if (lease->top_ready || lease->enabled_by_us != 0 || lease->cci_rate_owned) {
        return NezhaCameraCamccBusy;
    }

    const uint32_t power = io->read(io->context, CamccTopStatus);
    if (power == UINT32_MAX) {
        return NezhaCameraCamccBusy;
    }
    /* CFG_GDSCR completion flags are polled independently, as in gdsc.c.
     * A completed up transition may coexist with the down-complete flag. */
    if ((power & CamccPowerUp) == 0) {
        if ((power & CamccPowerDown) == 0) {
            return NezhaCameraCamccBusy;
        }
        if ((io->read(io->context, CamccTopGdsc) & CamccCollapse) == 0) {
            return NezhaCameraCamccBusy;
        }
        lease->enabled_by_us |= OwnTop;
        change_bits(io, CamccTopGdsc, CamccCollapse, 0);
        if (!wait_bits(io, CamccTopStatus, CamccPowerUp, CamccPowerUp)) {
            return NezhaCameraCamccTimeout;
        }
        io->wait_ms(io->context, 1);
        if ((io->read(io->context, CamccTopGdsc) & CamccRetain) == 0) {
            lease->enabled_by_us |= OwnRetain;
            change_bits(io, CamccTopGdsc, CamccRetain, CamccRetain);
        }
    }

    if (!enable_branch(io, lease, CamccCoreAhb, OwnCoreAhb, false) ||
        !enable_branch(io, lease, CamccCpasAhb, OwnCpasAhb, true)) {
        return NezhaCameraCamccTimeout;
    }
    lease->top_ready = true;
    return NezhaCameraCamccOk;
}

enum NezhaCameraCamccStatus nezha_camera_camcc_select_cci0_rate(
    const struct NezhaCameraRegisterIo *io, struct NezhaCameraCamccLease *lease,
    uint32_t rate_hz)
{
    if (!io_valid(io) || lease == NULL || !lease->top_ready) {
        return NezhaCameraCamccInvalid;
    }
    if (lease->cci_rate_owned || (io->read(io->context, CamccCci0Command) & CamccUpdate) != 0) {
        return NezhaCameraCamccBusy;
    }

    uint32_t fields;
    if (rate_hz == 19200000) {
        fields = 0x001;
    } else if (rate_hz == 37500000) {
        /* Same-device w29 operating values. A matching PLL0 supplies
         * 600 MHz on OUT_EVEN and CCI0 divides that by 16. */
        if ((io->read(io->context, CamccPll0Mode) & 5u) != 5u ||
            (io->read(io->context, CamccPll0Opmode) & 1u) == 0 ||
            (io->read(io->context, CamccPll0L) & 0xffffu) != 0x3eu ||
            io->read(io->context, CamccPll0Alpha) != 0x8000u ||
            io->read(io->context, CamccPll0User) != 0x840fu) {
            return NezhaCameraCamccSource;
        }
        fields = 0x21f;
    } else {
        return NezhaCameraCamccInvalid;
    }

    const uint32_t before = io->read(io->context, CamccCci0Config) & CamccRateMask;
    if (before == fields) {
        return NezhaCameraCamccOk;
    }
    if ((io->read(io->context, CamccCci0) & (CamccEnable | clock_off)) != clock_off) {
        return NezhaCameraCamccBusy;
    }

    /* clk_rcg2_shared commits the new configuration with a running root.
     * This branch was off and belongs to this lease until restoration. */
    if (!enable_branch(io, lease, CamccCci0, OwnCci0, true)) {
        return NezhaCameraCamccTimeout;
    }

    lease->cci_rate_owned = true;
    lease->cci_config_before = before;
    lease->cci_config_installed = fields;
    change_bits(io, CamccCci0Config, CamccRateMask, fields);
    change_bits(io, CamccCci0Command, CamccUpdate, CamccUpdate);
    if (!wait_bits(io, CamccCci0Command, CamccUpdate, 0) ||
        (io->read(io->context, CamccCci0Config) & CamccRateMask) != fields) {
        return NezhaCameraCamccTimeout;
    }
    return NezhaCameraCamccOk;
}

enum NezhaCameraCamccStatus nezha_camera_camcc_enable_cci0(
    const struct NezhaCameraRegisterIo *io, struct NezhaCameraCamccLease *lease)
{
    if (!io_valid(io) || lease == NULL || !lease->top_ready) {
        return NezhaCameraCamccInvalid;
    }
    return enable_branch(io, lease, CamccCci0, OwnCci0, true) ?
        NezhaCameraCamccOk : NezhaCameraCamccTimeout;
}

static bool disable_owned_branch(const struct NezhaCameraRegisterIo *io,
                                 struct NezhaCameraCamccLease *lease,
                                 uint32_t offset, unsigned owned_bit, bool has_halt)
{
    if ((lease->enabled_by_us & owned_bit) == 0) {
        return true;
    }
    change_bits(io, offset, CamccEnable, 0);
    if (!has_halt) {
        io->wait_ms(io->context, 1);
    }
    const uint32_t off = has_halt ? clock_off : 0;
    if (!wait_bits(io, offset, CamccEnable | off, off)) {
        return false;
    }
    lease->enabled_by_us &= ~owned_bit;
    return true;
}

enum NezhaCameraCamccStatus nezha_camera_camcc_release(
    const struct NezhaCameraRegisterIo *io, struct NezhaCameraCamccLease *lease)
{
    if (!io_valid(io) || lease == NULL) {
        return NezhaCameraCamccInvalid;
    }
    lease->top_ready = false;
    if (lease->cci_rate_owned) {
        if ((lease->enabled_by_us & OwnCci0) == 0) {
            return NezhaCameraCamccBusy;
        }
        if (!wait_bits(io, CamccCci0Command, CamccUpdate, 0)) {
            return NezhaCameraCamccTimeout;
        }
        const uint32_t current = io->read(io->context, CamccCci0Config) & CamccRateMask;
        if (current != lease->cci_config_installed && current != lease->cci_config_before) {
            return NezhaCameraCamccBusy;
        }
        if (current != lease->cci_config_before) {
            change_bits(io, CamccCci0Config, CamccRateMask, lease->cci_config_before);
            change_bits(io, CamccCci0Command, CamccUpdate, CamccUpdate);
            if (!wait_bits(io, CamccCci0Command, CamccUpdate, 0) ||
                (io->read(io->context, CamccCci0Config) & CamccRateMask) != lease->cci_config_before) {
                return NezhaCameraCamccTimeout;
            }
        }
        lease->cci_rate_owned = false;
    }
    if (!disable_owned_branch(io, lease, CamccCci0, OwnCci0, true)) {
        return NezhaCameraCamccTimeout;
    }
    if (!disable_owned_branch(io, lease, CamccCpasAhb, OwnCpasAhb, true) ||
        !disable_owned_branch(io, lease, CamccCoreAhb, OwnCoreAhb, false)) {
        return NezhaCameraCamccTimeout;
    }
    if ((lease->enabled_by_us & OwnRetain) != 0) {
        change_bits(io, CamccTopGdsc, CamccRetain, 0);
        if (!wait_bits(io, CamccTopGdsc, CamccRetain, 0)) {
            return NezhaCameraCamccTimeout;
        }
        lease->enabled_by_us &= ~(unsigned)OwnRetain;
    }
    if ((lease->enabled_by_us & OwnTop) != 0) {
        change_bits(io, CamccTopGdsc, CamccCollapse, CamccCollapse);
        if (!wait_bits(io, CamccTopStatus, CamccPowerDown, CamccPowerDown)) {
            return NezhaCameraCamccTimeout;
        }
        lease->enabled_by_us &= ~(unsigned)OwnTop;
    }
    return NezhaCameraCamccOk;
}
