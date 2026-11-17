/* SPDX-License-Identifier: GPL-2.0-only */
/* Reuses the qualified nezha27 touch firmware-route contract (673cde8),
 * with a reversible exact QUP3 match. No SMMU context is initialized here. */
#include "Sm8850Bt.h"
#include "../NezhaJointResources.h"
#ifdef KERNEL
#include <sys/errno.h>
#else
#include <errno.h>
#endif
static uint32_t rd(const struct sm8850_bt_io *io, uint32_t r) { return io->read(io->context, SM8850_BT_SMMU, r); }
static void wr(const struct sm8850_bt_io *io, uint32_t r, uint32_t v) { io->write(io->context, SM8850_BT_SMMU, r, v); }
static unsigned width(unsigned n) {
    static const unsigned bits[] = {32, 36, 40, 42, 44, 48};
    return n < sizeof(bits) / sizeof(bits[0]) ? bits[n] : 0;
}
static bool matches(uint32_t smr, uint16_t sid) {
    return (smr & 0x80000000u) && !((sid ^ smr) & ~(smr >> 16) & 0x7fffu);
}
int sm8850_bt_dma_get(const struct sm8850_bt_io *io, struct sm8850_bt_dma_route *r,
                     uint64_t pa, uint32_t bytes) {
    if (!io || !io->read || !io->write || !r || !bytes || !pa || pa >= (UINT64_C(1) << 36) ||
        bytes > (UINT64_C(1) << 36) - pa) return -EINVAL;
    if (r->failed) return -EIO;
    if (r->acquired) return -EBUSY;
    uint32_t id0 = rd(io, 0x20), id1 = rd(io, 0x24), id2 = rd(io, 0x28);
    unsigned count = id0 & 255, banks = id1 & 255;
    if (!(id0 & (1u<<27)) || (id0 & (1u<<8)) || !count || count > 128 || !banks || banks > 128) return -ENODEV;
    for (unsigned shift = 0; shift <= 8; shift += 4) {
        unsigned bits = width((id2 >> shift) & 15);
        if (!bits || pa >= (UINT64_C(1) << bits) || bytes > (UINT64_C(1) << bits) - pa) return -ERANGE;
    }
    unsigned source = UINT32_MAX, slot = UINT32_MAX, free_slot = UINT32_MAX;
    uint32_t source_smr = 0, route = 0;
    for (unsigned i = 0; i < count; i++) {
        uint32_t smr = rd(io, 0x800 + i * 4);
        if (!(smr & 0x80000000u) && free_slot == UINT32_MAX &&
            !nezha_sensor_smmu_slot(i)) free_slot = i;
        if (matches(smr, 0x60)) {
            if (source != UINT32_MAX) return -EBUSY;
            source = i; source_smr = smr; route = rd(io, 0xc00 + i * 4);
        }
        if (matches(smr, 0x4c3)) {
            if (slot != UINT32_MAX) return -EBUSY;
            slot = i;
        }
    }
    if (source == UINT32_MAX || ((route >> 16) & 3) || (route & 255) >= banks) return -ENODEV;
    if (slot != UINT32_MAX && nezha_sensor_smmu_slot(slot)) return -EBUSY;
    unsigned pages = 2u << ((id1 >> 28) & 7), shift = (id1 >> 31) ? 16 : 12, bank = route & 255;
    if (bank == NezhaSensorSmmuBank) return -EBUSY;
    uint64_t cb = ((uint64_t)pages + bank) << shift;
    if (cb + 4 > 0x100000) return -ENODEV;
    uint32_t sctlr = rd(io, (uint32_t)cb);
    if (sctlr & 1) return -EBUSY;
    if (slot != UINT32_MAX && rd(io, 0xc00 + slot * 4) != route) return -EBUSY;
    if (slot == UINT32_MAX) slot = free_slot;
    if (slot == UINT32_MAX) return -ENOSPC;
    r->slot = slot; r->route = route; r->bank_offset = (uint32_t)cb; r->bank_sctlr = sctlr;
    r->old_smr = rd(io, 0x800 + slot * 4); r->old_route = rd(io, 0xc00 + slot * 4);
    if (rd(io, 0x800 + source * 4) != source_smr || rd(io, 0xc00 + source * 4) != route ||
        rd(io, (uint32_t)cb) != sctlr) return -EBUSY;
    r->acquired = true;
    if (r->old_smr & 0x80000000u) return 0; /* existing shared route remains untouched */
    r->installed = true; /* journal both writes before either can fail */
    wr(io, 0xc00 + slot * 4, route);
    wr(io, 0x800 + slot * 4, 0x800004c3u);
    if (rd(io, 0xc00 + slot * 4) != route || rd(io, 0x800 + slot * 4) != 0x800004c3u) {
        r->failed = true; return -EIO;
    }
    return 0;
}
int sm8850_bt_dma_put(const struct sm8850_bt_io *io, struct sm8850_bt_dma_route *r) {
    if (!io || !r) return -EINVAL;
    if (!r->acquired) return 0;
    if (r->installed) {
        uint32_t smr = rd(io, 0x800 + r->slot * 4), route = rd(io, 0xc00 + r->slot * 4);
        if ((smr != 0x800004c3u && smr != r->old_smr) || (route != r->route && route != r->old_route)) return -EBUSY;
        /* QUP3 I2C/audio is another consumer of the same SID. An identity
         * route holds no Bluetooth memory; retain it if a peer SE is enabled. */
        if (!(io->read(io->context, SM8850_BT_GCC, 0x52018) & (31u << 12))) {
            wr(io, 0x800 + r->slot * 4, r->old_smr & ~0x80000000u);
            wr(io, 0xc00 + r->slot * 4, r->old_route);
            wr(io, 0x800 + r->slot * 4, r->old_smr);
            if (rd(io, 0x800 + r->slot * 4) != r->old_smr || rd(io, 0xc00 + r->slot * 4) != r->old_route) return -EIO;
        }
    }
    r->acquired = r->installed = r->failed = false; return 0;
}
