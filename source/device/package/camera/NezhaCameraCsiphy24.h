/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_CSIPHY24_H
#define NEZHA_CAMERA_CSIPHY24_H

#include "NezhaCameraRegisterIo.h"
#include <stdbool.h>
#include <stddef.h>

struct NezhaCameraPhyRegister {
    uint32_t offset, value, delay_us, kind;
};

struct NezhaCameraPhyTable {
    const struct NezhaCameraPhyRegister *registers;
    size_t count;
};

struct NezhaCameraCsiphy24Profile {
    struct NezhaCameraPhyTable common, reset_enter, reset_exit, lane, rate;
    unsigned lane_offsets[3];
    uint16_t settle_count;
};

struct NezhaCameraCsiphy24Io {
    struct NezhaCameraRegisterIo registers;
    void (*wait_us)(void *context, unsigned microseconds);
};

struct NezhaCameraCsiphy24State {
    uint32_t refgen_status;
    bool reset_owned;
    bool lanes_enabled;
};

/* CSIPHY0 is stopped and owned exclusively. The caller has already taken
 * its two stock rails, CAMCC domain, RX source and PHY timer clocks. Tables
 * come from this phone's actual ctrl_reg_2_4_0_1 and selected mode's rate.
 * This first main-camera path uses three C-PHY trios, in order 0,1,2. */
bool nezha_camera_csiphy24_start(const struct NezhaCameraCsiphy24Io *io,
    const struct NezhaCameraCsiphy24Profile *profile, struct NezhaCameraCsiphy24State *state);

/* After the sensor and receiver are quiet, disable these trios and reset
 * the exclusively held PHY. Keep its clocks until this returns true. */
bool nezha_camera_csiphy24_stop(const struct NezhaCameraCsiphy24Io *io,
    const struct NezhaCameraCsiphy24Profile *profile, struct NezhaCameraCsiphy24State *state);

#endif
