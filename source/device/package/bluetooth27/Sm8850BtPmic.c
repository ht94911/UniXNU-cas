/* SPDX-License-Identifier: GPL-2.0-only */
/* SPMI v8 and LV/MV GPIO contracts from Linux spmi-pmic-arb.c and
 * pinctrl-spmi-gpio.c at fa4ad798. Addresses/PPID come from nezha N0 DT. */
#include "Sm8850Bt.h"
#ifdef KERNEL
#include <sys/errno.h>
#else
#include <errno.h>
#endif
static int access8(const struct sm8850_bt_io *io, struct sm8850_bt_pmic *p,
                   uint8_t reg, uint8_t *value, bool write) {
    if (p->failed) return -EIO;
    unsigned w = write ? SM8850_BT_PMIC_WRITE : SM8850_BT_PMIC_OBSERVER;
    uint32_t base = (write ? 0x200u : 0x20u) * p->apid;
    if (write) io->write(io->context, w, base + 0x10, *value);
    io->write(io->context, w, base, (write ? 0u : 1u << 27) | (uint32_t)reg << 4);
    for (unsigned n = 0; n < 1000; n++) {
        uint32_t status = io->read(io->context, w, base + 8);
        if (status & 1u) {
            if (status & 0xeu) return -EIO;
            if (!write) *value = (uint8_t)io->read(io->context, w, base + 0x18);
            return 0;
        }
        io->delay_us(io->context, 1);
    }
    p->failed = true; return -ETIMEDOUT;
}
int sm8850_bt_pmic_prepare(const struct sm8850_bt_io *io, struct sm8850_bt_pmic *p) {
    if (!io || !io->read || !io->write || !io->delay_us || !p) return -EINVAL;
    if (p->failed) return -EIO;
    if (p->ready) return 0;
    uint32_t version = io->read(io->context, SM8850_BT_PMIC_CORE, 0);
    if (version < 0x80000000u || version >= 0x80050000u) return -ENODEV;
    uint32_t first = io->read(io->context, SM8850_BT_PMIC_CORE, 4) & 0x1fffu;
    uint32_t count = io->read(io->context, SM8850_BT_PMIC_CORE, 8) & 0x1fffu;
    if (!first || !count || first + count > 8192) return -ENODEV;
    unsigned matches = 0;
    for (uint32_t a = first; a < first + count; a++) {
        uint32_t map = io->read(io->context, SM8850_BT_PMIC_MAP, a * 4);
        if (!map || (map & 0x1fffu) != 0x98cu) continue;
        if ((io->read(io->context, SM8850_BT_PMIC_OWNER, (a - first) * 4) & 7u) != 0) continue;
        p->apid = (uint16_t)a; matches++;
    }
    if (matches != 1) return matches ? -EBUSY : -EPERM;
    p->base_apid = (uint16_t)first;
    uint8_t type = 0;
    int e = access8(io, p, 4, &type, false);
    if (!e) e = access8(io, p, 5, &p->subtype, false);
    if (e) return e;
    if (type != 0x10 || p->subtype < 0x10 || p->subtype > 0x15) return -ENODEV;
    p->ready = true; return 0;
}
static int masked(const struct sm8850_bt_io *io, struct sm8850_bt_pmic *p,
                  uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t before, after;
    int e = access8(io, p, reg, &before, false);
    if (e) return e;
    uint8_t want = (before & ~mask) | value;
    e = access8(io, p, reg, &want, true);
    if (!e) e = access8(io, p, reg, &after, false);
    if (!e && (after & mask) != value) e = -EIO;
    return e;
}
int sm8850_bt_pmic_enable(const struct sm8850_bt_io *io, struct sm8850_bt_pmic *p, bool enable) {
    int e = sm8850_bt_pmic_prepare(io, p);
    if (e) return e;
    /* LV/MV normal output, DT power-source 1, no bias, master enabled.
     * Set the output value before enabling direction to avoid a high glitch. */
    e = masked(io, p, 0x44, 0x8f, enable ? 0x80 : 0);
    if (!e) e = masked(io, p, 0x41, 7, 1);
    if (!e) e = masked(io, p, 0x42, 7, 5); /* PMIC_GPIO_PULL_DISABLE */
    if (!e) e = masked(io, p, 0x40, 3, 1); /* digital output */
    if (!e) e = masked(io, p, 0x46, 0x80, 0x80);
    if (e) p->failed = true;
    return e;
}
