/* SPDX-License-Identifier: GPL-2.0-only */
/* UART18 clock table/RCG/branch contracts: Linux gcc-kaanapali.c and
 * clk-rcg2.c at fa4ad798. This is not the sheng UART14 register map. */
#include "Sm8850Bt.h"
#ifdef KERNEL
#include <sys/errno.h>
#else
#include <errno.h>
#endif
#define VOTES 0x52018u
#define RCG 0xa8640u
#define UART (1u << 17)
#define SHARED (15u << 8) /* wrap3 m/s AHB, core, core2x */
#define PEERS (31u << 12) /* SE0..SE4; UART18 is SE5 */
#define DFS_ENABLE 0x14u
#define DFS_CFG(level) (0x1cu + 4u * (level))
#define DFS_M(level) (0x5cu + 4u * (level))
#define DFS_N(level) (0x9cu + 4u * (level))
#define CMD_UPDATE 1u
#define CMD_ROOT_EN 2u
#define CMD_ROOT_OFF (1u << 31)
#define CFG_MASK (0x371fu | (1u << 20))
static uint32_t rd(const struct sm8850_bt_io *io, uint32_t r) { return io->read(io->context, SM8850_BT_GCC, r); }
static void wr(const struct sm8850_bt_io *io, uint32_t r, uint32_t v) { io->write(io->context, SM8850_BT_GCC, r, v); }
/* clk_rcg2_set_force_enable waits for the root before programming M/N/CFG.
 * UART18 does not use hw_clk_ctrl. Only remove a force vote added by this
 * operation; a force vote inherited from the platform remains in place. */
static int force_root(const struct sm8850_bt_io *io, uint32_t command) {
    wr(io, RCG, command | CMD_ROOT_EN);
    for (unsigned n = 0; n < 500; n++) {
        if (!(rd(io, RCG) & CMD_ROOT_OFF)) return 0;
        io->delay_us(io->context, 1);
    }
    return -ETIMEDOUT;
}
static int release_force(const struct sm8850_bt_io *io, uint32_t command) {
    if (command & CMD_ROOT_EN) return 0;
    wr(io, RCG, rd(io, RCG) & ~CMD_ROOT_EN);
    return (rd(io, RCG) & CMD_ROOT_EN) ? -EIO : 0;
}
static int update(const struct sm8850_bt_io *io, uint32_t cmd) {
    wr(io, RCG, cmd | CMD_UPDATE);
    for (unsigned n = 0; n < 1000; n++) {
        if (!(rd(io, RCG) & CMD_UPDATE)) return 0;
        io->delay_us(io->context, 1);
    }
    return -ETIMEDOUT;
}
int sm8850_bt_clock_get(const struct sm8850_bt_io *io, struct sm8850_bt_clock *c) {
    if (!io || !io->read || !io->write || !io->delay_us || !c) return -EINVAL;
    if (c->failed) return -EIO;
    if (c->acquired) return 0;
    uint32_t before = rd(io, VOTES);
    c->added_votes = (UART | SHARED) & ~before;
    c->acquired = true; /* retain ownership even after a partial write */
    wr(io, VOTES, before | UART | SHARED);
    if ((rd(io, VOTES) & (UART | SHARED)) != (UART | SHARED)) goto failed;
    static const uint32_t halt[] = {0x233f0, 0x233f4, 0x233f8, 0x2340c, 0xa8630};
    for (unsigned i = 0; i < sizeof(halt) / sizeof(halt[0]); i++) {
        unsigned n;
        for (n = 0; n < 1000 && (rd(io, halt[i]) & (1u << 31)); n++) io->delay_us(io->context, 1);
        if (n == 1000) goto failed;
    }
    return 0;
failed:
    c->failed = true; return -EIO;
}
/* clk_rcg2_dfs_populate_freq/recalc_rate at the same source lock. Firmware
 * owns these eight banks. With DFS enabled, changing CFG/M/N of the ordinary
 * RCG does not select the rate: GENI_CLK_SEL selects one of these banks.
 * The inherited GPLL0 rate is the same 600 MHz parent used by the table below;
 * no PLL, DFS-enable or performance-bank register is written here. */
static uint32_t dfs_frequency(const struct sm8850_bt_io *io, unsigned level) {
    uint32_t cfg = rd(io, RCG + DFS_CFG(level)), parent, mode = (cfg >> 12) & 3u;
    switch ((cfg >> 8) & 7u) {
    case 0: parent = 19200000; break;
    case 1: parent = 600000000; break;
    case 6: parent = 300000000; break;
    default: return 0;
    }
    uint64_t frequency = parent;
    unsigned divider = cfg & 31u;
    if (divider) frequency = frequency * 2u / (divider + 1u);
    if (mode) {
        if (mode != 2) return 0;
        uint32_t m = rd(io, RCG + DFS_M(level)) & 0xffffu;
        uint32_t n = (~rd(io, RCG + DFS_N(level)) & 0xffffu) + m;
        if (!m || n < m) return 0;
        frequency = frequency * m / n;
    }
    return (uint32_t)frequency;
}
static int dfs_source(const struct sm8850_bt_io *io, uint32_t baud,
                      uint32_t *hz, uint32_t *selector) {
    uint64_t target = (uint64_t)baud * 16u, best = UINT64_MAX;
    for (unsigned level = 0; level < 8; level++) {
        uint32_t frequency = dfs_frequency(io, level);
        uint64_t divider = ((uint64_t)frequency + target / 2u) / target;
        if (!frequency || !divider || divider > 4095) continue;
        uint64_t actual = frequency / divider;
        uint64_t diff = actual > target ? actual - target : target - actual;
        if (diff * 100u > target || diff >= best) continue;
        *hz = frequency; *selector = level; best = diff;
    }
    return best == UINT64_MAX ? -ERANGE : 0;
}
int sm8850_bt_clock_source(const struct sm8850_bt_io *io, struct sm8850_bt_clock *c,
                          uint32_t baud, uint32_t *hz, uint32_t *selector) {
    if (!io || !c || !hz || !selector || !c->acquired || c->failed) return -EINVAL;
    *hz = *selector = 0;
    uint32_t m, n, frequency;
    switch (baud) {
    case 115200: m = 384; n = 15625; frequency = 7372800; break;
    case 3000000: m = 4; n = 25; frequency = 48000000; break;
    case 3200000: m = 64; n = 375; frequency = 51200000; break;
    case 4000000: m = 16; n = 75; frequency = 64000000; break;
    default: return -EINVAL;
    }
    /* GPLL0 is a shared parent. Its HLOS vote is inherited from the selected
     * platform, which must retain it; Bluetooth never changes PLL rate/votes. */
    if (!(rd(io, 0x52020) & 1u)) return -ENXIO;
    c->dfsr = rd(io, RCG + DFS_ENABLE);
    if (c->dfsr & 1u) {
        int e = dfs_source(io, baud, hz, selector);
        if (!e) c->selector = *selector;
        return e;
    }
    if (!c->source_saved) {
        for (unsigned i = 0; i < 5; i++) c->rcg[i] = rd(io, RCG + i * 4);
        if (c->rcg[0] & CMD_UPDATE) return -EBUSY;
        c->source_saved = true;
    }
    uint32_t command = rd(io, RCG);
    if (command & CMD_UPDATE) return -EBUSY;
    int e = force_root(io, command);
    if (e) { c->failed = true; return e; }
    wr(io, RCG + 8, m);
    wr(io, RCG + 12, ~(n - m) & 0xffffu);
    wr(io, RCG + 16, ~n & 0xffffu);
    uint32_t cfg = rd(io, RCG + 4);
    wr(io, RCG + 4, (cfg & ~CFG_MASK) | (6u << 8) | (2u << 12) | 1u);
    e = update(io, command | CMD_ROOT_EN);
    if (!e && ((rd(io, RCG + 4) & CFG_MASK) != ((6u << 8) | (2u << 12) | 1u) ||
        (rd(io, RCG + 8) & 0xffffu) != m || (rd(io, RCG + 12) & 0xffffu) != (~(n - m) & 0xffffu) ||
        (rd(io, RCG + 16) & 0xffffu) != (~n & 0xffffu))) e = -EIO;
    if (!e) e = release_force(io, command);
    if (e) { c->failed = true; return e; }
    *hz = frequency; c->selector = 0; return 0;
}
int sm8850_bt_clock_put(const struct sm8850_bt_io *io, struct sm8850_bt_clock *c) {
    if (!io || !c) return -EINVAL;
    if (!c->acquired) return 0;
    if (c->failed) return -EIO; /* unacknowledged RCG/clock operation remains owned */
    if (c->source_saved) {
        uint32_t command = rd(io, RCG);
        if (command & CMD_UPDATE) return -EBUSY;
        int e = force_root(io, command);
        if (e) { c->failed = true; return e; }
        for (unsigned i = 1; i < 5; i++) wr(io, RCG + i * 4, c->rcg[i]);
        e = update(io, command | CMD_ROOT_EN);
        if (!e) e = release_force(io, command);
        if (e) { c->failed = true; return e; }
    }
    uint32_t votes = rd(io, VOTES), remove = c->added_votes;
    if (votes & PEERS) remove &= ~SHARED;
    wr(io, VOTES, votes & ~remove);
    if ((rd(io, VOTES) & remove) != 0) { c->failed = true; return -EIO; }
    c->added_votes = 0; c->source_saved = c->acquired = false; return 0;
}
