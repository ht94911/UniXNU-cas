/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaCameraNativeRpmh.h"
#include "NezhaCameraBootArc.h"
#include "Sm8850ResourcesNative.h"

#include <IOKit/IOLib.h>
#include <sys/systm.h>

namespace {
// Match NaResourceMicTcs, not a second independent lease for TCS2.
constexpr unsigned TcsResource = 7;
constexpr unsigned Tcs = 2;
constexpr uint32_t Owner = 0x4e43;

struct NativeRsc {
    Sm8850Resources *common;
    uint64_t lease;
    bool locked;
    NezhaCameraRpmh rpmh;
};

NativeRsc native;
bool enter(void *cookie);
void leave(void *cookie);

/* The camera's TCS lease protects its commands; the shared ledger retains
 * acknowledged boot proxies independently of that temporary lease. */
bool preserve_boot_arcs()
{
    for (unsigned i = 0; i < NezhaCameraBootArcCount; ++i) {
        auto &arc = native.rpmh.arc[i];
        uint32_t index = 0;
        if (!enter(&native)) return false;
        const int found = sm8850_arc_boot_floor_get(native.common, arc.resource.address, &index);
        leave(&native);
        if (found == Sm8850Okay) {
            if (index >= arc.resource.level_count || arc.resource.levels[index] != NezhaCameraBootArcLevel)
                return false;
            arc.current_index = arc.original_index = index;
            continue;
        }
        if (found != Sm8850Conflict || !nezha_camera_rpmh_preserve_boot_arc(&native.rpmh,
            static_cast<NezhaCameraArcRail>(i), NezhaCameraBootArcLevel)) return false;
        if (!enter(&native)) return false;
        const int recorded = sm8850_arc_boot_floor_record(native.common, arc.resource.address, arc.current_index);
        leave(&native);
        printf("NEZHA CAMERA ARC: boot-proxy address=%#x index=%u level=%u ack=%#x record=%d\n",
            arc.resource.address, arc.current_index, NezhaCameraBootArcLevel,
            native.rpmh.last_command_status, recorded);
        if (recorded != Sm8850Okay) return false;
    }
    return true;
}

bool enter(void *cookie)
{
    auto &rsc = *static_cast<NativeRsc *>(cookie);
    if (rsc.locked || sm8850_resource_enter(rsc.common, TcsResource, Owner, rsc.lease)) {
        return false;
    }
    rsc.locked = true;
    return true;
}

void leave(void *cookie)
{
    auto &rsc = *static_cast<NativeRsc *>(cookie);
    rsc.locked = false;
    sm8850_resource_leave(rsc.common);
}

uint32_t read(void *cookie, uint32_t offset)
{
    auto &rsc = *static_cast<NativeRsc *>(cookie);
    const bool held = rsc.locked;
    if (!held && !enter(cookie)) {
        return UINT32_MAX;
    }
    auto &io = rsc.common->io;
    uint32_t value;
    const int result = io.read32(io.context, Sm8850Rsc, offset, &value);
    if (!held) {
        leave(cookie);
    }
    return result == 0 ? value : UINT32_MAX;
}

bool write(void *cookie, uint32_t offset, uint32_t value)
{
    auto &rsc = *static_cast<NativeRsc *>(cookie);
    return rsc.locked && sm8850_tcs_write(rsc.common, Tcs, offset, value) == Sm8850Okay;
}

bool irq_enable(void *cookie, bool enabled)
{
    auto &rsc = *static_cast<NativeRsc *>(cookie);
    unsigned previous;
    return rsc.locked && sm8850_tcs_irq_update(rsc.common, Tcs, enabled, &previous) == Sm8850Okay;
}

void pause(void *, unsigned milliseconds)
{
    IOSleep(milliseconds);
}
}

bool nezha_camera_native_rpmh_open(NezhaCameraRpmh **owner, const uint8_t *db, size_t db_bytes)
{
    if (!owner || *owner || !db || native.lease) {
        return false;
    }
    auto *common = sm8850_native_resources();
    if (!common || sm8850_resource_acquire(common, TcsResource, Owner, &native.lease)) {
        return false;
    }
    native.common = common;
    const NezhaCameraRegisterIo io = {&native, read, nullptr, pause};
    const NezhaCameraRpmhGuard guard = {enter, leave, write, irq_enable};
    if (!nezha_camera_rpmh_init_guarded(&native.rpmh, &io, &guard, db, db_bytes) ||
        !nezha_camera_rpmh_init_capture(&native.rpmh, db, db_bytes)) {
        // Initialization only reads. No vote or IRQ state to restore.
        if (sm8850_resource_release(common, TcsResource, Owner, native.lease, 1) == Sm8850Okay) {
            native = {};
        }
        return false;
    }
    *owner = &native.rpmh;
    /* Publish the owner before writes so an incomplete boot-proxy command
     * follows the same retained-lease teardown path as other RPMh errors. */
    return preserve_boot_arcs();
}

bool nezha_camera_native_rpmh_close(NezhaCameraRpmh **owner)
{
    if (!owner || *owner != &native.rpmh || !native.lease || native.rpmh.unsettled) {
        return false;
    }
    for (const auto &rail : native.rpmh.rail) {
        if (rail.voltage_changed || rail.enable_changed) {
            return false;
        }
    }
    for (const auto &arc : native.rpmh.arc) {
        if (arc.changed) {
            return false;
        }
    }
    for (const auto &phy : native.rpmh.phy) {
        if (phy.voltage_changed || phy.mode_changed || phy.enable_changed) return false;
    }
    if (!enter(&native)) {
        return false;
    }
    const bool idle = sm8850_tcs_idle(native.common, Tcs) == Sm8850Okay;
    leave(&native);
    if (!idle || sm8850_resource_release(native.common, TcsResource, Owner, native.lease, 1)) {
        return false;
    }
    native = {};
    *owner = nullptr;
    return true;
}
