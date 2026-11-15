/* SPDX-License-Identifier: MIT */
#include "NezhaCameraCsiphy24.h"

/* Semantics from Qualcomm cam_csiphy_core.c. Register values are supplied
 * by the local stock module extractor, preserving the board's revision. */
enum {
    PhyDefault = 1, PhySettleLow = 4, PhySettleHigh = 8,
    PhyDphy = 16, PhyCphy = 32, PhyDphyCombo = 128, PhyCdr = 2048,
    PhyLaneEnable = 0x1014, PhyRefgenStatus = 0x1184,
    PhyRefgenReady = 1 << 7,
    MainCphyMask = (1 << 1) | (1 << 3) | (1 << 5),
};

static bool table_valid(struct NezhaCameraPhyTable table, bool lane)
{
    if (table.registers == NULL || table.count == 0 || table.count > 128) {
        return false;
    }
    for (size_t i = 0; i < table.count; ++i) {
        const struct NezhaCameraPhyRegister *entry = &table.registers[i];
        if (entry->offset % 4 != 0 || entry->offset >= (lane ? 0x200u : 0x1200u) ||
            entry->offset == PhyLaneEnable || entry->value > 0xff || entry->delay_us > 1000 ||
            (entry->kind != PhyDefault && entry->kind != PhyDphy && entry->kind != PhyCphy && entry->kind != PhyDphyCombo &&
             entry->kind != PhySettleLow && entry->kind != PhySettleHigh && entry->kind != PhyCdr)) {
            return false;
        }
    }
    return true;
}

static bool profile_valid(const struct NezhaCameraCsiphy24Io *io,
                          const struct NezhaCameraCsiphy24Profile *profile)
{
    return io != NULL && io->registers.read != NULL && io->registers.write != NULL &&
        io->registers.wait_ms != NULL && io->wait_us != NULL && profile != NULL &&
        profile->lane_offsets[0] == 0x200 && profile->lane_offsets[1] == 0x600 &&
        profile->lane_offsets[2] == 0xa00 && profile->settle_count != 0 &&
        table_valid(profile->common, false) && table_valid(profile->reset_enter, false) &&
        table_valid(profile->reset_exit, false) && table_valid(profile->lane, true) &&
        table_valid(profile->rate, true);
}

static void apply(const struct NezhaCameraCsiphy24Io *io, struct NezhaCameraPhyTable table,
                  unsigned lane_offset, unsigned settle, bool clear)
{
    const struct NezhaCameraRegisterIo *regs = &io->registers;
    for (size_t i = 0; i < table.count; ++i) {
        const struct NezhaCameraPhyRegister *entry = &table.registers[i];
        if (entry->kind == PhyDphy || entry->kind == PhyDphyCombo) {
            continue;
        }
        uint32_t value = entry->value;
        if (entry->kind == PhySettleLow) {
            value = settle & 0xffu;
        } else if (entry->kind == PhySettleHigh) {
            value = settle >> 8;
        }
        regs->write(regs->context, lane_offset + entry->offset, clear ? 0 : value);
        if (entry->delay_us != 0) {
            io->wait_us(regs->context, entry->delay_us);
        }
    }
}

bool nezha_camera_csiphy24_start(const struct NezhaCameraCsiphy24Io *io,
    const struct NezhaCameraCsiphy24Profile *profile, struct NezhaCameraCsiphy24State *state)
{
    if (!profile_valid(io, profile) || state == NULL || state->reset_owned ||
        io->registers.read(io->registers.context, PhyLaneEnable) != 0) {
        return false;
    }
    state->reset_owned = true;
    apply(io, profile->reset_enter, 0, profile->settle_count, false);
    bool reference_ready = false;
    for (unsigned ms = 0; ms <= 10; ++ms) {
        state->refgen_status = io->registers.read(io->registers.context, PhyRefgenStatus);
        if (state->refgen_status != UINT32_MAX && (state->refgen_status & PhyRefgenReady) != 0) {
            reference_ready = true;
            break;
        }
        if (ms != 10) {
            io->registers.wait_ms(io->registers.context, 1);
        }
    }
    if (!reference_ready) {
        return false;
    }
    apply(io, profile->common, 0, profile->settle_count, false);
    state->lanes_enabled = true;
    io->registers.write(io->registers.context, PhyLaneEnable, MainCphyMask);
    for (unsigned lane = 0; lane < 3; ++lane) {
        const unsigned offset = profile->lane_offsets[lane];
        apply(io, profile->lane, offset, profile->settle_count, false);
        apply(io, profile->rate, offset, profile->settle_count, false);
    }
    apply(io, profile->reset_exit, 0, profile->settle_count, false);
    return io->registers.read(io->registers.context, PhyLaneEnable) == MainCphyMask;
}

bool nezha_camera_csiphy24_stop(const struct NezhaCameraCsiphy24Io *io,
    const struct NezhaCameraCsiphy24Profile *profile, struct NezhaCameraCsiphy24State *state)
{
    if (!profile_valid(io, profile) || state == NULL) {
        return false;
    }
    if (!state->reset_owned) {
        return true;
    }
    const uint32_t lanes = io->registers.read(io->registers.context, PhyLaneEnable);
    if (lanes != 0 && lanes != MainCphyMask) {
        return false;
    }
    if (state->lanes_enabled) {
        io->registers.write(io->registers.context, PhyLaneEnable, 0);
        if (io->registers.read(io->registers.context, PhyLaneEnable) != 0) {
            return false;
        }
    }
    apply(io, profile->reset_enter, 0, profile->settle_count, false);
    apply(io, profile->common, 0, profile->settle_count, true);
    *state = (struct NezhaCameraCsiphy24State){0};
    return true;
}
