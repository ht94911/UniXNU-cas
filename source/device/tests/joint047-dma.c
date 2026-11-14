/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../package/bluetooth27/Sm8850Bt.h"
#include "../package/NezhaJointResources.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static uint32_t smmu[0x100000/4], gcc[SM8850_BT_GCC_BYTES/4];
static unsigned writes;
static uint32_t read_reg(void *p, unsigned window, uint32_t offset)
{
    (void)p;
    assert(window == SM8850_BT_SMMU || window == SM8850_BT_GCC);
    return window == SM8850_BT_SMMU ? smmu[offset/4] : gcc[offset/4];
}
static void write_reg(void *p, unsigned window, uint32_t offset, uint32_t value)
{
    (void)p;
    assert(window == SM8850_BT_SMMU);
    assert(offset >= 0x800 && offset < 0xe00);
    unsigned slot = ((offset < 0xc00 ? offset-0x800 : offset-0xc00)/4);
    assert(!nezha_sensor_smmu_slot(slot));
    smmu[offset/4] = value;
    ++writes;
}
static void sleep_us(void *p, unsigned us) { (void)p; (void)us; }
static const struct sm8850_bt_io io = {NULL, read_reg, write_reg, sleep_us};
static void reset(void)
{
    memset(smmu, 0, sizeof smmu);
    memset(gcc, 0, sizeof gcc);
    writes = 0;
    smmu[0x20/4] = (1u<<27) | 16u;
    smmu[0x24/4] = (3u<<28) | 16u;
    smmu[0x28/4] = 0x222;
    for (unsigned i=0; i<9; ++i) {
        smmu[(0x800+4*i)/4] = 0x80000200u+i;
        smmu[(0xc00+4*i)/4] = 0;
    }
    smmu[0x800/4] = 0x80000060u;
}
int main(void)
{
    struct sm8850_bt_dma_route route = {0};
    reset();
    assert(!sm8850_bt_dma_get(&io, &route, 0x8c0000000ULL, 32768));
    assert(route.slot == 13 && route.acquired && writes == 2);
    for (unsigned i=9; i<=12; ++i) assert(!smmu[(0x800+4*i)/4]);
    assert(!sm8850_bt_dma_put(&io, &route) && !route.acquired);

    reset(); route = (struct sm8850_bt_dma_route){0};
    for (unsigned i=13; i<16; ++i) smmu[(0x800+4*i)/4] = 0x80000300u+i;
    assert(sm8850_bt_dma_get(&io, &route, 0x8c0000000ULL, 32768) == -ENOSPC);
    assert(!writes && !route.acquired);

    reset(); route = (struct sm8850_bt_dma_route){0};
    smmu[(0x800+9*4)/4] = 0x800004c3u;
    assert(sm8850_bt_dma_get(&io, &route, 0x8c0000000ULL, 32768) == -EBUSY);
    assert(!writes && !route.acquired);

    reset(); route = (struct sm8850_bt_dma_route){0};
    smmu[0xc00/4] = NezhaSensorSmmuBank;
    assert(sm8850_bt_dma_get(&io, &route, 0x8c0000000ULL, 32768) == -EBUSY);
    assert(!writes);

    reset(); route = (struct sm8850_bt_dma_route){0};
    smmu[(0x800+13*4)/4] = 0x800004c3u;
    assert(!sm8850_bt_dma_get(&io, &route, 0x8c0000000ULL, 32768));
    assert(!writes && !route.installed);
    assert(!sm8850_bt_dma_put(&io, &route) && !writes);
    puts("PASS: Bluetooth uses slot13, preserves sensor9-12, refuses exhaustion/conflict and retains an existing valid route");
}
