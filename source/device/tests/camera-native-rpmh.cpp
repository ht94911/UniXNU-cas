/* SPDX-License-Identifier: MIT */
/* Actual camera RPMh adapter/core and shared resource owner, with MMIO only
 * modelled. The retained boot proxies belong to the APPS master, not TCS2. */
#include "NezhaCameraNativeRpmh.h"
#include "Sm8850ResourcesNative.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
Sm8850Resources common{};
uint32_t registers[0x2000 / 4]{};
struct Vote {uint32_t address, index;};
std::vector<Vote> votes;
bool locked, timeout;
constexpr uint32_t Tcs = 0x1240;
constexpr uint32_t arc_address[] = {0x30080, 0x300a0, 0x30010};
int lock(void *) {assert(!locked); locked = true; return 0;}
void unlock(void *) {assert(locked); locked = false;}
int read32(void *, unsigned domain, uint32_t offset, uint32_t *value)
{assert(locked && domain == Sm8850Rsc && offset < sizeof(registers)); *value = registers[offset / 4]; return 0;}
int read64(void *, unsigned, uint32_t, uint64_t *) {assert(false); return -1;}
int barrier(void *) {assert(locked); return 0;}
int write32(void *, unsigned domain, uint32_t offset, uint32_t value)
{
    assert(locked && domain == Sm8850Rsc && offset < sizeof(registers));
    if (offset == 0xd08) registers[0xd04 / 4] &= ~value;
    else registers[offset / 4] = value;
    if (offset == Tcs + 0x24 && value == 0x1010000) {
        assert(registers[(Tcs + 0x34) / 4] == 0x10108); // ARC writes; no unreadable RSC getter.
        votes.push_back({registers[(Tcs + 0x38) / 4], registers[(Tcs + 0x3c) / 4]});
        if (timeout) {registers[(Tcs + 0x28) / 4] = 0; return 0;}
        registers[(Tcs + 0x40) / 4] = 0x10100;
        registers[0xd04 / 4] |= 4;
    }
    return 0;
}
void reset()
{
    common = {}; std::memset(registers, 0, sizeof(registers)); votes.clear(); timeout = false;
    const Sm8850ResourceIo io{nullptr, lock, unlock, read32, write32, read64, barrier};
    assert(sm8850_resources_init(&common, &io) == Sm8850Okay);
    registers[0] = 0x40300; registers[0xc / 4] = 0x800c8104;
    registers[(Tcs + 0x28) / 4] = 1;
}
void database(uint8_t *db, size_t bytes)
{
    std::memset(db, 0, bytes);
    const uint8_t header[] = {0xdb, 0x30, 0x03, 0x0c}; std::memcpy(db + 4, header, 4);
    db[8] = 4; db[12] = 192; db[14] = 8;
    const char *names[] = {"B2B_E0", "B1B_E0", "L5M_E1", "L1D_E0", "L3I_E0", "mmcx.lvl", "mxc.lvl", "mx.lvl"};
    const uint32_t addresses[] = {0x40000, 0x40010, 0x44020, 0x44030, 0x44040, 0x30080, 0x300a0, 0x30010};
    const uint16_t levels[] = {0, 16, 56, 64, 128, 256, 384, 448};
    for (unsigned i = 0; i < 8; ++i) {
        uint8_t *entry = db + 144 + i * 24;
        std::memcpy(entry, names[i], std::strlen(names[i]));
        std::memcpy(entry + 16, &addresses[i], 4);
        if (i >= 5) {entry[20] = sizeof(levels); std::memcpy(db + 144 + 192, levels, sizeof(levels));}
    }
}
}
extern "C" Sm8850Resources *sm8850_native_resources(void) {return &common;}
extern "C" void IOSleep(unsigned ms) {assert(!locked && ms == 1);}

int main()
{
    uint8_t db[144 + 192 + 16]; database(db, sizeof(db)); reset();
    uint32_t index = 99;
    assert(sm8850_arc_boot_floor_get(&common, arc_address[0], &index) == Sm8850Conflict && index == 99);
    NezhaCameraRpmh *owner = nullptr;
    assert(nezha_camera_native_rpmh_open(&owner, db, sizeof(db)) && owner && votes.size() == 3);
    for (unsigned i = 0; i < 3; ++i) {
        assert(votes[i].address == arc_address[i] && votes[i].index == 6);
        assert(sm8850_arc_boot_floor_get(&common, arc_address[i], &index) == Sm8850Okay && index == 6);
        assert(nezha_camera_rpmh_acquire_arc(owner, static_cast<NezhaCameraArcRail>(i), 64));
        assert(nezha_camera_rpmh_release_arc(owner, static_cast<NezhaCameraArcRail>(i)));
    }
    assert(votes.size() == 3); // Neither camera start nor cleanup sends a lower APPS vote.
    assert(nezha_camera_rpmh_acquire_arc(owner, NezhaCameraArcMmCx, 448));
    assert(votes.back().index == 7 && !nezha_camera_native_rpmh_close(&owner));
    assert(nezha_camera_rpmh_release_arc(owner, NezhaCameraArcMmCx) && votes.back().index == 6);
    assert(nezha_camera_native_rpmh_close(&owner) && !owner && common.owner[7] == 0);
    const size_t count = votes.size();
    assert(nezha_camera_native_rpmh_open(&owner, db, sizeof(db)) && votes.size() == count);
    assert(owner->arc[0].current_index == 6 && nezha_camera_native_rpmh_close(&owner));
    assert(sm8850_arc_boot_floor_record(&common, arc_address[0], 0) == Sm8850Invalid);
    assert(sm8850_arc_boot_floor_record(&common, arc_address[0], 7) == Sm8850Conflict);

    // A busy TCS is detected before any boot-proxy write or floor is recorded.
    reset(); registers[(Tcs + 0x2c) / 4] = 1;
    assert(!nezha_camera_native_rpmh_open(&owner, db, sizeof(db)) && owner && votes.empty());
    assert(!nezha_camera_native_rpmh_close(&owner));
    registers[(Tcs + 0x2c) / 4] = 0;
    assert(nezha_camera_native_rpmh_close(&owner));
    assert(nezha_camera_native_rpmh_open(&owner, db, sizeof(db)));
    assert(nezha_camera_native_rpmh_close(&owner));

    // Failure before an ACK leaves the original lease retained and creates no
    // fictional boot floor. This terminal fixture is deliberately not reused.
    reset(); timeout = true;
    assert(!nezha_camera_native_rpmh_open(&owner, db, sizeof(db)) && owner && votes.size() == 1);
    assert(sm8850_arc_boot_floor_get(&common, arc_address[0], &index) == Sm8850Conflict);
    assert(owner->unsettled && !nezha_camera_native_rpmh_close(&owner) && common.owner[7]);
    puts("Actual shared RPMh: boot proxy survives camera start/close/reopen; higher camera votes restore it; busy/timeout never invent ACKs");
}
