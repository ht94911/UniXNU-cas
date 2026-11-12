/* SPDX-License-Identifier: MIT */
#include "NezhaCameraRpmh.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { IrqEnable = 0xd00, IrqStatus = 0xd04, IrqClear = 0xd08, Tcs = 0x1240,
       Control = 0x24, Status = 0x28, Command = 0x34, Address = 0x38, Data = 0x3c,
       Ack = 0x40, Reply = 0x44, Slot = 4 };

static const uint32_t address[] = {0x40010, 0x40000, 0x44040};

struct Rsc {
    uint32_t reg[0x1300 / 4];
    uint32_t mv[3], on[3];
    unsigned reads, votes, waits;
    bool timeout;
};

static uint32_t read_rsc(void *context, uint32_t offset)
{
    struct Rsc *rsc = context;
    assert(offset % 4 == 0 && offset < sizeof(rsc->reg));
    return rsc->reg[offset / 4];
}

static void write_rsc(void *context, uint32_t offset, uint32_t value)
{
    struct Rsc *rsc = context;
    assert(offset % 4 == 0 && offset < sizeof(rsc->reg));
    rsc->reg[offset / 4] = value;
    if (offset == IrqEnable) {
        assert((value & ~Slot) == 2);
    } else if (offset == IrqClear) {
        assert(value == Slot);
        rsc->reg[IrqStatus / 4] &= ~value;
    } else if (offset == Tcs + Control && value == 0x1010000) {
        const uint32_t target = rsc->reg[(Tcs + Address) / 4];
        const uint32_t message = rsc->reg[(Tcs + Command) / 4];
        unsigned rail = 0;
        while (rail < 3 && target != address[rail] && target != address[rail] + 4) {
            ++rail;
        }
        assert(rail < 3 && (message == 0x108 || message == 0x10108));
        uint32_t *field = target == address[rail] ? &rsc->mv[rail] : &rsc->on[rail];
        if (rsc->timeout) {
            rsc->reg[(Tcs + Status) / 4] = 0;
            return;
        }
        if (message == 0x108) {
            ++rsc->reads;
            rsc->reg[(Tcs + Reply) / 4] = *field;
        } else {
            ++rsc->votes;
            *field = rsc->reg[(Tcs + Data) / 4];
        }
        rsc->reg[(Tcs + Ack) / 4] = 0x10100;
        rsc->reg[IrqStatus / 4] |= Slot;
    }
}

static void wait_ms(void *context, unsigned ms)
{
    struct Rsc *rsc = context;
    rsc->waits += ms;
}

int main(void)
{
    uint8_t db[144 + 24 * 3] = {[4] = 0xdb, [5] = 0x30, [6] = 0x03, [7] = 0x0c, [8] = 4, [14] = 3};
    const char *names[] = {"B2B_E0", "B1B_E0", "L5M_E1"};
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t *entry = db + 144 + i * 24;
        memcpy(entry, names[i], 6);
        for (unsigned b = 0; b < 4; ++b) {
            entry[16 + b] = (uint8_t)(address[i] >> (b * 8));
        }
    }
    struct Rsc rsc = {.reg = {[0] = 0x40300, [0xc / 4] = 0x800c8104,
                              [IrqEnable / 4] = 2 | Slot, [(Tcs + Status) / 4] = 1},
                      .mv = {2720, 3400, 1200}, .on = {1, 1, 0}};
    const struct NezhaCameraRegisterIo io = {&rsc, read_rsc, write_rsc, wait_ms};
    struct NezhaCameraRpmh owner;
    assert(nezha_camera_rpmh_init(&owner, &io, db, sizeof(db)));
    assert(nezha_camera_rpmh_acquire(&owner, NezhaCameraMainBob2, 3296));
    assert(rsc.on[0] == 1 && rsc.mv[0] == 3296 && rsc.votes == 1);
    assert(nezha_camera_rpmh_release(&owner, NezhaCameraMainBob2));
    assert(rsc.on[0] == 1 && rsc.mv[0] == 2720 && rsc.votes == 2);

    assert(nezha_camera_rpmh_acquire(&owner, NezhaCameraMainBob1, 3400));
    assert(nezha_camera_rpmh_release(&owner, NezhaCameraMainBob1));
    assert(rsc.votes == 2);

    assert(nezha_camera_rpmh_acquire(&owner, NezhaCameraMainVio, 1800));
    assert(rsc.on[2] == 1 && rsc.mv[2] == 1800 && rsc.votes == 4);
    assert(nezha_camera_rpmh_release(&owner, NezhaCameraMainVio));
    assert(rsc.on[2] == 0 && rsc.mv[2] == 1200 && rsc.votes == 6);
    assert(rsc.reg[IrqEnable / 4] == (2 | Slot) && !owner.unsettled);

    rsc.mv[0] = 3400;
    assert(!nezha_camera_rpmh_acquire(&owner, NezhaCameraMainBob2, 3296));
    assert(rsc.votes == 6 && owner.status == NezhaCameraRpmhReadback);
    rsc.timeout = true;
    assert(!nezha_camera_rpmh_acquire(&owner, NezhaCameraMainVio, 1800));
    assert(owner.unsettled && owner.status == NezhaCameraRpmhTimeout && rsc.waits == 100);
    assert(!nezha_camera_rpmh_acquire(&owner, NezhaCameraMainVio, 1800));
    assert(rsc.votes == 6 && rsc.waits == 100);

    puts("Camera RPMh reads and restores live rails and leaves a timed-out request owned");
}
