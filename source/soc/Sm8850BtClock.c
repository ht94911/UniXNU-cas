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
static uint32_t rd(const struct sm8850_bt_io *io, uint32_t r) { return io->read(io->context, SM8850_BT_GCC, r); }
static void wr(const struct sm8850_bt_io *io, uint32_t r, uint32_t v) { io->write(io->context, SM8850_BT_GCC, r, v); }
static int update(const struct sm8850_bt_io *io, uint32_t cmd) {
    wr(io, RCG, cmd | 1u);
    for (unsigned n = 0; n < 1000; n++) {
        if (!(rd(io, RCG) & 1u)) return 0;
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
int sm8850_bt_clock_source(const struct sm8850_bt_io *io, struct sm8850_bt_clock *c,
                          uint32_t baud, uint32_t *hz) {
    if (!io || !c || !hz || !c->acquired || c->failed) return -EINVAL;
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
    if (!c->source_saved) {
        for (unsigned i = 0; i < 5; i++) c->rcg[i] = rd(io, RCG + i * 4);
        if (c->rcg[0] & 1u) return -EBUSY;
        c->source_saved = true;
    }
    uint32_t command = rd(io, RCG);
    if (command & 1u) return -EBUSY;
    wr(io, RCG, command | 2u); /* force root while programming this idle SE */
    wr(io, RCG + 8, m);
    wr(io, RCG + 12, ~(n - m) & 0xffffu);
    wr(io, RCG + 16, ~n & 0xffffu);
    uint32_t cfg = rd(io, RCG + 4);
    wr(io, RCG + 4, (cfg & ~0x371fu) | (6u << 8) | (2u << 12) | 1u);
    int e = update(io, command | 2u);
    if (!e && ((rd(io, RCG + 4) & 0x371fu) != ((6u << 8) | (2u << 12) | 1u) ||
        (rd(io, RCG + 8) & 0xffffu) != m || (rd(io, RCG + 12) & 0xffffu) != (~(n - m) & 0xffffu) ||
        (rd(io, RCG + 16) & 0xffffu) != (~n & 0xffffu))) e = -EIO;
    if (e) { c->failed = true; return e; }
    *hz = frequency; return 0;
}
int sm8850_bt_clock_put(const struct sm8850_bt_io *io, struct sm8850_bt_clock *c) {
    if (!io || !c) return -EINVAL;
    if (!c->acquired) return 0;
    if (c->failed) return -EIO; /* unacknowledged RCG/clock operation remains owned */
    if (c->source_saved) {
        uint32_t command = rd(io, RCG);
        if (command & 1u) return -EBUSY;
        wr(io, RCG, command | 2u);
        for (unsigned i = 1; i < 5; i++) wr(io, RCG + i * 4, c->rcg[i]);
        int e = update(io, (c->rcg[0] & ~1u) | 2u);
        if (e) { c->failed = true; return e; }
        wr(io, RCG, c->rcg[0] & ~1u);
    }
    uint32_t votes = rd(io, VOTES), remove = c->added_votes;
    if (votes & PEERS) remove &= ~SHARED;
    wr(io, VOTES, votes & ~remove);
    if ((rd(io, VOTES) & remove) != 0) { c->failed = true; return -EIO; }
    c->added_votes = 0; c->source_saved = c->acquired = false; return 0;
}
