/* SPDX-License-Identifier: MIT */
#include "NezhaCameraCsiphy24.h"

#include <assert.h>
#include <stdio.h>

struct Phy {
    uint32_t registers[0x1200 / 4];
    unsigned writes, wait_us, wait_ms;
};

static uint32_t read_reg(void *context, uint32_t offset)
{
    struct Phy *phy = context;
    assert(offset % 4 == 0 && offset < sizeof(phy->registers));
    return phy->registers[offset / 4];
}

static void write_reg(void *context, uint32_t offset, uint32_t value)
{
    struct Phy *phy = context;
    (void)read_reg(context, offset);
    /* The C-PHY adapter leaves interrupt masks to the platform owner. */
    assert(offset < 0x102c || offset > 0x1054);
    if (offset == 0x1000 && value == 14) {
        for (unsigned i = 0; i < 3; ++i) {
            const unsigned base = 0x200 + i * 0x400;
            assert(phy->registers[(base + 0xc) / 4] == 35);
            assert(phy->registers[(base + 0x68) / 4] == 0xf1);
        }
    }
    phy->registers[offset / 4] = value;
    ++phy->writes;
}

static void wait_us(void *context, unsigned delay)
{
    struct Phy *phy = context;
    phy->wait_us += delay;
}

static void wait_ms(void *context, unsigned delay)
{
    struct Phy *phy = context;
    phy->wait_ms += delay;
}

int main(void)
{
    const struct NezhaCameraPhyRegister common[] = {{0x108c, 0, 1, 16}, {0x108c, 14, 1, 32},
                                                    {0x101c, 0x7a, 0, 1}};
    const struct NezhaCameraPhyRegister enter = {0x1000, 1, 20, 1};
    const struct NezhaCameraPhyRegister exit[] = {{0x1000, 2, 10, 16}, {0x1000, 0, 10, 128}, {0x1000, 14, 10, 32}};
    const struct NezhaCameraPhyRegister lane = {0x94, 13, 0, 1};
    const struct NezhaCameraPhyRegister rate[] = {{0x68, 0xf1, 0, 1}, {0xc, 28, 0, 4}, {8, 0, 0, 8}};
    const struct NezhaCameraCsiphy24Profile profile = {
        {common, 3}, {&enter, 1}, {exit, 3}, {&lane, 1}, {rate, 3}, {0x200, 0x600, 0xa00}, 35,
    };
    struct Phy phy = {.registers = {[0x1184 / 4] = 0x80}};
    struct NezhaCameraCsiphy24State state = {0};
    const struct NezhaCameraCsiphy24Io io = {{&phy, read_reg, write_reg, wait_ms}, wait_us};
    assert(nezha_camera_csiphy24_start(&io, &profile, &state));
    assert(state.reset_owned && state.lanes_enabled && phy.registers[0x1014 / 4] == 0x2a);
    assert(phy.registers[0x1000 / 4] == 14 && phy.wait_us == 31);
    assert(nezha_camera_csiphy24_stop(&io, &profile, &state));
    assert(!state.reset_owned && phy.registers[0x1014 / 4] == 0 && phy.registers[0x1000 / 4] == 1);
    assert(phy.registers[0x108c / 4] == 0 && phy.registers[0x101c / 4] == 0);

    phy = (struct Phy){.registers = {[0x1014 / 4] = 1}};
    assert(!nezha_camera_csiphy24_start(&io, &profile, &state) && phy.writes == 0);
    phy = (struct Phy){0};
    assert(!nezha_camera_csiphy24_start(&io, &profile, &state));
    assert(state.reset_owned && !state.lanes_enabled && phy.wait_ms == 10);
    assert(nezha_camera_csiphy24_stop(&io, &profile, &state));
    assert(!state.reset_owned && phy.registers[0x1014 / 4] == 0);
    puts("C-PHY configures all three trios before releasing reset and retains ownership on a missing reference clock");
}
