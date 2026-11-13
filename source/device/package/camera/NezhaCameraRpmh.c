/* SPDX-License-Identifier: MIT */
#include "NezhaCameraRpmh.h"

/* The APPS DRV2 configuration is from the stock FDT and the existing touch
 * driver. Request construction is shared with sheng. The read command, reply
 * and VRM field layout follow Linux rpmh-rsc.c and qcom-rpmh-regulator.c. */
enum {
    AppsDrvId = 0x40300, AppsConfig = 0x800c8104u,
    TcsOffset = 0xd00, CameraTcs = 2, TcsBit = 1 << CameraTcs,
    CommandWrite = 1 << 16, CommandAcknowledged = (1 << 16) | (1 << 8),
    VrmVoltage = 0, VrmEnable = 4, VrmVoltageMask = 0x1fff, VrmEnableMask = 1,
    FirstPhy = 3,
};

static const struct {
    const char *name;
    unsigned requested_mv, min_mv, max_mv;
} rail_profile[] = {
    [NezhaCameraMainBob2] = {"B2B_E0", 3296, 2704, 3552},
    [NezhaCameraMainBob1] = {"B1B_E0", 3400, 3400, 3400},
    [NezhaCameraMainVio] = {"L5M_E1", 1800, 1200, 1800},
    [FirstPhy + NezhaCameraPhy1p2] = {"L1D_E0", 1200, 1200, 1256},
    [FirstPhy + NezhaCameraPhy0p9] = {"L3I_E0", 880, 880, 920},
};
static const char *const arc_name[] = {"mmcx.lvl", "mxc.lvl", "mx.lvl"};

static uint32_t read_rsc(const struct NezhaCameraRpmh *owner, uint32_t offset)
{
    return owner->rsc.read(owner->rsc.context, offset);
}

static bool write_rsc(const struct NezhaCameraRpmh *owner, uint32_t offset, uint32_t value)
{
    if (owner->guard.write != NULL) {
        return owner->guard.write(owner->rsc.context, offset, value);
    }
    owner->rsc.write(owner->rsc.context, offset, value);
    return true;
}

static bool enter(const struct NezhaCameraRpmh *owner)
{
    return owner->guard.enter == NULL || owner->guard.enter(owner->rsc.context);
}

static void leave(const struct NezhaCameraRpmh *owner)
{
    if (owner->guard.leave != NULL) {
        owner->guard.leave(owner->rsc.context);
    }
}

static bool set_irq(const struct NezhaCameraRpmh *owner, bool enabled)
{
    if (owner->guard.irq_enable != NULL) {
        return owner->guard.irq_enable(owner->rsc.context, enabled);
    }
    const uint32_t old = read_rsc(owner, TcsOffset);
    if (old == UINT32_MAX) {
        return false;
    }
    const uint32_t expected = enabled ? old | TcsBit : old & ~(uint32_t)TcsBit;
    return write_rsc(owner, TcsOffset, expected) &&
        ((read_rsc(owner, TcsOffset) ^ expected) & TcsBit) == 0;
}

static bool idle(const struct NezhaCameraRpmh *owner, uint32_t tcs)
{
    const QcomRscLayout *l = &owner->layout;
    return read_rsc(owner, tcs + l->status) == 1 &&
        read_rsc(owner, tcs + l->control) == 0 &&
        read_rsc(owner, tcs + l->cmd_enable) == 0 &&
        read_rsc(owner, tcs + l->wait_for_cmpl) == 0 &&
        (read_rsc(owner, TcsOffset + l->irq_status) & TcsBit) == 0;
}

static bool transact(struct NezhaCameraRpmh *owner, uint32_t address,
                     uint32_t value, uint32_t *reply)
{
    const QcomRscLayout *l = &owner->layout;
    const uint32_t tcs_relative = CameraTcs * l->tcs_stride;
    const uint32_t tcs = TcsOffset + tcs_relative;
    owner->last_address = address;
    owner->last_value = value;
    if (!owner->restoring && owner->before_transaction != NULL &&
        !owner->before_transaction(owner->observer_context, address, value, reply != NULL)) {
        owner->status = NezhaCameraRpmhReadback;
        return false;
    }
    if (owner->unsettled || !enter(owner)) {
        owner->status = NezhaCameraRpmhBusy;
        return false;
    }
    owner->last_irq = read_rsc(owner, TcsOffset + l->irq_status);
    owner->last_tcs_status = read_rsc(owner, tcs + l->status);
    owner->last_command_status = UINT32_MAX;
    owner->last_reply = UINT32_MAX;
    if (!idle(owner, tcs)) {
        leave(owner);
        owner->status = NezhaCameraRpmhBusy;
        return false;
    }

    QcomRpmhWrite writes[9];
    const unsigned count = qcom_rpmh_active_request(l, CameraTcs, &address, &value, 1, writes, 9);
    if (count != 9) {
        leave(owner);
        owner->status = NezhaCameraRpmhInvalid;
        return false;
    }
    const uint32_t irq_enable = read_rsc(owner, TcsOffset);
    if (irq_enable == UINT32_MAX) {
        leave(owner);
        owner->status = NezhaCameraRpmhReadback;
        return false;
    }
    owner->unsettled = true;
    /* Unlike touch's ACK-only poll, this path qualifies completion with
     * the TCS IRQ status. The RSC only publishes that bit when enabled,
     * as in Linux and the existing native audio/baseband owner. */
    if ((irq_enable & TcsBit) == 0 && !set_irq(owner, true)) {
        leave(owner);
        owner->status = NezhaCameraRpmhReadback;
        return false;
    }
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t offset = writes[i].offset;
        uint32_t word = writes[i].value;
        if (reply != NULL) {
            if (offset == tcs_relative + l->cmd_msgid) {
                word &= ~(uint32_t)CommandWrite;
            }
            if (offset == tcs_relative + l->cmd_data) {
                continue;
            }
        }
        if (!write_rsc(owner, TcsOffset + offset, word)) {
            leave(owner);
            owner->status = NezhaCameraRpmhReadback;
            return false;
        }
    }
    leave(owner);

    bool done = false;
    for (unsigned ms = 0; ms <= 100; ++ms) {
        owner->last_irq = read_rsc(owner, TcsOffset + l->irq_status);
        owner->last_tcs_status = read_rsc(owner, tcs + l->status);
        owner->last_command_status = read_rsc(owner, tcs + l->cmd_data + 4);
        if (owner->last_irq != UINT32_MAX && (owner->last_irq & TcsBit) != 0 &&
            owner->last_tcs_status == 1 && owner->last_command_status != UINT32_MAX &&
            (owner->last_command_status & CommandAcknowledged) == CommandAcknowledged) {
            done = true;
            break;
        }
        if (ms != 100) {
            owner->rsc.wait_ms(owner->rsc.context, 1);
        }
    }
    if (!done) {
        if (reply != NULL && owner->last_command_status != UINT32_MAX &&
            (owner->last_command_status & CommandAcknowledged) == CommandAcknowledged) {
            owner->last_reply = read_rsc(owner, tcs + l->cmd_data + 8);
        }
        owner->status = NezhaCameraRpmhTimeout;
        return false;
    }

    if (!enter(owner)) {
        owner->status = NezhaCameraRpmhBusy;
        return false;
    }
    const uint32_t command_status = read_rsc(owner, tcs + l->cmd_data + 4);
    owner->last_command_status = command_status;
    const bool acknowledged = command_status != UINT32_MAX &&
        (command_status & CommandAcknowledged) == CommandAcknowledged;
    if (reply != NULL && acknowledged) {
        *reply = read_rsc(owner, tcs + l->cmd_data + 8);
        owner->last_reply = *reply;
    }
    const unsigned finish_count = qcom_rpmh_finish(l, CameraTcs, writes, 9);
    bool restored = finish_count != 0;
    for (unsigned i = 0; restored && i < finish_count; ++i) {
        restored = write_rsc(owner, TcsOffset + writes[i].offset, writes[i].value);
    }
    restored = restored && write_rsc(owner, tcs + l->wait_for_cmpl, 0) &&
        ((irq_enable & TcsBit) != 0 || set_irq(owner, false));
    owner->unsettled = !restored || !idle(owner, tcs) ||
        ((read_rsc(owner, TcsOffset) ^ irq_enable) & TcsBit) != 0;
    leave(owner);
    const bool ok = !owner->unsettled && acknowledged && (reply == NULL || *reply != UINT32_MAX);
    owner->status = ok ? NezhaCameraRpmhOk : NezhaCameraRpmhReadback;
    if (ok && reply == NULL && owner->vote_acknowledged != NULL) {
        owner->vote_acknowledged(owner->observer_context, address, value, command_status);
    }
    return ok;
}

bool nezha_camera_rpmh_init(struct NezhaCameraRpmh *owner,
    const struct NezhaCameraRegisterIo *rsc, const uint8_t *db, size_t db_bytes)
{
    return nezha_camera_rpmh_init_guarded(owner, rsc, NULL, db, db_bytes);
}

bool nezha_camera_rpmh_init_guarded(struct NezhaCameraRpmh *owner,
    const struct NezhaCameraRegisterIo *rsc, const struct NezhaCameraRpmhGuard *guard,
    const uint8_t *db, size_t db_bytes)
{
    if (owner == NULL || rsc == NULL || rsc->read == NULL || rsc->wait_ms == NULL ||
        (guard == NULL && rsc->write == NULL) ||
        (guard != NULL && (guard->enter == NULL || guard->leave == NULL || guard->write == NULL ||
                          guard->irq_enable == NULL)) ||
        rsc->read(rsc->context, 0) != AppsDrvId || rsc->read(rsc->context, 0xc) != AppsConfig) {
        return false;
    }
    struct NezhaCameraRpmh init = {.rsc = *rsc, .layout = qcom_rsc_layout(AppsDrvId)};
    if (guard != NULL) {
        init.guard = *guard;
    }
    for (unsigned i = 0; i < 3; ++i) {
        QcomRpmhResource resource;
        if (qcom_cmd_db_find(db, db_bytes, rail_profile[i].name, &resource) != QcomRpmhOk ||
            (resource.address & 0xffff000fu) != 0x40000u) {
            return false;
        }
        for (unsigned j = 0; j < i; ++j) {
            if (init.rail[j].address == resource.address) {
                return false;
            }
        }
        init.rail[i].address = resource.address;
    }
    *owner = init;
    return true;
}

static bool read_rail(struct NezhaCameraRpmh *owner, struct NezhaCameraVrmState *rail)
{
    uint32_t voltage, enable;
    if (!transact(owner, rail->address + VrmVoltage, 0, &voltage) ||
        !transact(owner, rail->address + VrmEnable, 0, &enable)) {
        return false;
    }
    rail->observed_mv = voltage & VrmVoltageMask;
    rail->observed_on = (enable & VrmEnableMask) != 0;
    return true;
}

static bool acquire_vrm(struct NezhaCameraRpmh *owner, unsigned index, unsigned millivolts)
{
    struct NezhaCameraVrmState *rail = &owner->rail[index];
    if (rail->voltage_changed || rail->enable_changed || !read_rail(owner, rail)) {
        return false;
    }
    rail->original_on = rail->observed_on;
    rail->original_mv = rail->observed_mv;
    rail->readback_unavailable = false;
    const bool original_in_range = rail->original_mv >= rail_profile[index].min_mv &&
        rail->original_mv <= rail_profile[index].max_mv;
    if ((!original_in_range && (rail->original_on || rail->original_mv != 0)) ||
        (rail->original_on && rail->original_mv > millivolts)) {
        owner->status = NezhaCameraRpmhReadback;
        return false;
    }

    if (rail->original_mv != millivolts) {
        rail->voltage_changed = true;
        if (!transact(owner, rail->address + VrmVoltage, millivolts, NULL)) {
            return false;
        }
    }
    if (!rail->original_on) {
        rail->enable_changed = true;
        if (!transact(owner, rail->address + VrmEnable, 1, NULL)) {
            return false;
        }
    }
    if (rail->voltage_changed || rail->enable_changed) {
        if (owner->isolated_sensor_votes && index < FirstPhy) {
            owner->rsc.wait_ms(owner->rsc.context, 2);
        }
        if (!read_rail(owner, rail)) return false;
        if (owner->isolated_sensor_votes && index < FirstPhy &&
            rail->original_mv == 0 && !rail->original_on && rail->observed_mv == 0 && !rail->observed_on) {
            /* The 009 hardware completed both writes and returned zero
             * for both reads. The ACKs grant this temporary APPS vote;
             * only a real CCI response can qualify sensor power. */
            rail->readback_unavailable = true;
            return true;
        }
    }
    if (!rail->observed_on || rail->observed_mv != millivolts) {
        owner->status = NezhaCameraRpmhReadback;
        return false;
    }
    return true;
}

static bool release_vrm(struct NezhaCameraRpmh *owner, unsigned index)
{
    struct NezhaCameraVrmState *rail = &owner->rail[index];
    if (!rail->voltage_changed && !rail->enable_changed) {
        return true;
    }
    if (!read_rail(owner, rail)) {
        return false;
    }
    const unsigned target = rail_profile[index].requested_mv;
    if ((rail->observed_mv != rail->original_mv && rail->observed_mv != target) ||
        (!rail->enable_changed && rail->observed_on != rail->original_on)) {
        owner->status = NezhaCameraRpmhReadback;
        return false;
    }

    /* A zero reply after an acknowledged enable can be an unsupported
     * readback, not proof that our APPS vote has already been removed. */
    if (rail->enable_changed &&
        !transact(owner, rail->address + VrmEnable, 0, NULL)) {
        return false;
    }
    /* An inherited zero is the absent setpoint of a disabled regulator. */
    if (rail->voltage_changed && rail->original_mv != 0 && rail->observed_mv != rail->original_mv &&
        !transact(owner, rail->address + VrmVoltage, rail->original_mv, NULL)) {
        return false;
    }
    if (!read_rail(owner, rail)) {
        return false;
    }
    if (rail->observed_on != rail->original_on ||
        (rail->original_mv != 0 && rail->observed_mv != rail->original_mv)) {
        owner->status = NezhaCameraRpmhReadback;
        return false;
    }
    rail->voltage_changed = false;
    rail->enable_changed = false;
    return true;
}

bool nezha_camera_rpmh_acquire(void *context, enum NezhaCameraMainRail rail, unsigned millivolts)
{
    return context != NULL && (unsigned)rail < FirstPhy &&
        millivolts == rail_profile[rail].requested_mv && acquire_vrm(context, (unsigned)rail, millivolts);
}

bool nezha_camera_rpmh_release(void *context, enum NezhaCameraMainRail rail)
{
    if (context == NULL || (unsigned)rail >= FirstPhy) {
        return false;
    }
    struct NezhaCameraRpmh *owner = context;
    owner->restoring = true;
    const bool result = release_vrm(owner, (unsigned)rail);
    owner->restoring = false;
    return result;
}

bool nezha_camera_rpmh_init_capture(struct NezhaCameraRpmh *owner,
    const uint8_t *db, size_t db_bytes)
{
    if (owner == NULL || owner->rsc.read == NULL || owner->capture_resources || owner->unsettled) {
        return false;
    }
    for (unsigned i = FirstPhy; i < sizeof(rail_profile) / sizeof(rail_profile[0]); ++i) {
        QcomRpmhResource resource;
        if (qcom_cmd_db_find(db, db_bytes, rail_profile[i].name, &resource) != QcomRpmhOk ||
            (resource.address & 0xffff000fu) != 0x40000u) {
            return false;
        }
        for (unsigned j = 0; j < i; ++j) {
            if (owner->rail[j].address == resource.address) {
                return false;
            }
        }
        owner->rail[i].address = resource.address;
    }
    for (unsigned i = 0; i < 3; ++i) {
        QcomRpmhResource resource;
        if (qcom_cmd_db_find(db, db_bytes, arc_name[i], &resource) != QcomRpmhOk ||
            (resource.address & 0xffff0003u) != 0x30000u || resource.level_count < 2 ||
            resource.levels[0] != 0) {
            return false;
        }
        for (unsigned j = 1; j < resource.level_count; ++j) {
            if (resource.levels[j] < resource.levels[j - 1]) {
                return false;
            }
        }
        for (unsigned j = 0; j < i; ++j) {
            if (owner->arc[j].resource.address == resource.address) {
                return false;
            }
        }
        owner->arc[i].resource = resource;
    }
    owner->capture_resources = true;
    return true;
}

bool nezha_camera_rpmh_acquire_phy(struct NezhaCameraRpmh *owner, enum NezhaCameraPhyRail rail)
{
    const unsigned index = FirstPhy + (unsigned)rail;
    return owner != NULL && owner->capture_resources && (unsigned)rail < 2 &&
        acquire_vrm(owner, index, rail_profile[index].requested_mv);
}

bool nezha_camera_rpmh_release_phy(struct NezhaCameraRpmh *owner, enum NezhaCameraPhyRail rail)
{
    return owner != NULL && owner->capture_resources && (unsigned)rail < 2 &&
        release_vrm(owner, FirstPhy + (unsigned)rail);
}

static bool read_arc(struct NezhaCameraRpmh *owner, struct NezhaCameraArcState *arc)
{
    uint32_t reply;
    if (!transact(owner, arc->resource.address, 0, &reply)) {
        return false;
    }
    if (reply >= arc->resource.level_count) {
        owner->status = NezhaCameraRpmhReadback;
        return false;
    }
    arc->observed_index = reply;
    return true;
}

bool nezha_camera_rpmh_acquire_arc(struct NezhaCameraRpmh *owner,
    enum NezhaCameraArcRail rail, unsigned level)
{
    if (owner == NULL || !owner->capture_resources || (unsigned)rail >= 3 || level == 0) {
        return false;
    }
    struct NezhaCameraArcState *arc = &owner->arc[rail];
    uint32_t index;
    if (qcom_rpmh_level_index(&arc->resource, level, &index) != QcomRpmhOk ||
        arc->resource.levels[index] < level) {
        owner->status = NezhaCameraRpmhInvalid;
        return false;
    }
    if (arc->changed || !read_arc(owner, arc)) {
        return false;
    }
    arc->original_index = arc->observed_index;
    arc->requested_index = index;
    if (arc->resource.levels[arc->observed_index] >= level) {
        return true;
    }
    arc->changed = true;
    if (!transact(owner, arc->resource.address, index, NULL) || !read_arc(owner, arc)) {
        return false;
    }
    if (arc->observed_index != index) {
        owner->status = NezhaCameraRpmhReadback;
        return false;
    }
    return true;
}

bool nezha_camera_rpmh_release_arc(struct NezhaCameraRpmh *owner, enum NezhaCameraArcRail rail)
{
    if (owner == NULL || !owner->capture_resources || (unsigned)rail >= 3) {
        return false;
    }
    struct NezhaCameraArcState *arc = &owner->arc[rail];
    if (!arc->changed) {
        return true;
    }
    if (!read_arc(owner, arc)) {
        return false;
    }
    if (arc->observed_index != arc->original_index) {
        if (arc->observed_index != arc->requested_index) {
            owner->status = NezhaCameraRpmhReadback;
            return false;
        }
        if (!transact(owner, arc->resource.address, arc->original_index, NULL) || !read_arc(owner, arc)) {
            return false;
        }
    }
    if (arc->observed_index != arc->original_index) {
        owner->status = NezhaCameraRpmhReadback;
        return false;
    }
    arc->changed = false;
    return true;
}
