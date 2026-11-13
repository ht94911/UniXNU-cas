/* SPDX-License-Identifier: MIT */
#include "NezhaCameraCrm.h"

#include <stddef.h>

/* Register protocol: OnePlus SM8850 kernel fc30e54174d2,
 * drivers/soc/qcom/crm-v2.c cam_crm_desc_v3 and drivers/clk/qcom/
 * {camcc-canoe.c,clk-rcg2.c}. Board bindings match the stock nezha FDT.
 * Firmware LUTs and completion IRQs own the clock transition. */
enum {
    LutVcdStride = 0x210, LutLevelStride = 0x18,
    LutConfig = 0x150, LutLAlpha = 0x154,
    SwDrv0 = 0x11c, SwVcdStride = 0x20,
    Active = 0, Sleep = 4, Wake = 8,
    IrqStatus = 0x10, IrqClear = 0x14, IrqEnable = 0x18,
    Completion = 1, TimeoutMs = 100,
};

static bool clock_valid(enum NezhaCameraCrmClock clock)
{
    return clock == NezhaCameraCrmTfe0 || clock == NezhaCameraCrmCsid ||
        clock == NezhaCameraCrmCphyRx;
}

static bool io_valid(const struct NezhaCameraRegisterIo *io)
{
    return io != NULL && io->read != NULL && io->write != NULL && io->wait_ms != NULL;
}

uint32_t nezha_camera_crm_canoe_parent_rate(void *context,
    enum NezhaCameraCrmClock clock, unsigned source, unsigned l, unsigned alpha)
{
    const struct NezhaCameraRegisterIo *camcc = context;
    if (camcc == NULL || camcc->read == NULL || !clock_valid(clock) || alpha > 0xffffu) {
        return 0;
    }
    if (source == 0) {
        return l == 0 ? 19200000 : 0;
    }
    uint32_t pll;
    unsigned shift = 0, encoding = 0, divisor = 1;
    if (clock == NezhaCameraCrmTfe0) {
        if (source != 6) {
            return 0;
        }
        pll = 0x3000;
        shift = 10;
        encoding = 1;
        divisor = 2;
    } else if (source >= 1 && source <= 5) {
        pll = source >= 4 ? 0x6000 : 0;
        if (source != 1) {
            const bool odd = source == 3 || source == 4;
            shift = odd ? 14u : 10u;
            encoding = odd ? 2u : 1u;
            divisor = odd ? 3u : 2u;
        }
    } else {
        return 0;
    }
    if (shift != 0) {
        const uint32_t user = camcc->read(camcc->context, pll + 0x18);
        if (user == UINT32_MAX) {
            return 0;
        }
        const unsigned value = (user >> shift) & 15u;
        if (value == 0) {
            divisor = 1;
        } else if (value != encoding) {
            return 0;
        }
    }
    if (l == 0) {
        l = camcc->read(camcc->context, pll + 0x10);
        alpha = camcc->read(camcc->context, pll + 0x14);
        if (l == UINT32_MAX || alpha > 0xffffu) {
            return 0;
        }
        l &= 0xffffu;
    }
    if (l == 0 || l > 0xffffu) {
        return 0;
    }
    /* alpha_pll_calc_rate rounds narrow-alpha PLLs up to whole kHz before
     * the post-divider. In particular 0x25/0x8777 is 720.560 MHz. */
    const uint64_t raw = (uint64_t)19200000 * l + ((uint64_t)19200000 * alpha >> 16);
    const uint64_t parent_hz = ((raw + 999u) / 1000u * 1000u) / divisor;
    return parent_hz <= UINT32_MAX ? (uint32_t)parent_hz : 0;
}

enum NezhaCameraCrmStatus nezha_camera_crm_load_plan(
    const struct NezhaCameraRegisterIo *crmc, const struct NezhaCameraCrmParent *parent,
    enum NezhaCameraCrmClock clock, struct NezhaCameraCrmPlan *plan)
{
    if (crmc == NULL || crmc->read == NULL || parent == NULL || parent->rate == NULL ||
        plan == NULL || !clock_valid(clock)) {
        return NezhaCameraCrmInvalid;
    }
    struct NezhaCameraCrmPlan next = {.clock = clock};
    for (unsigned level = 1; level <= 8; ++level) {
        const uint32_t offset = (unsigned)clock * LutVcdStride + level * LutLevelStride;
        const uint32_t cfg = crmc->read(crmc->context, offset + LutConfig);
        const uint32_t lalpha = crmc->read(crmc->context, offset + LutLAlpha);
        if (cfg == UINT32_MAX || lalpha == UINT32_MAX) {
            return NezhaCameraCrmReadback;
        }
        const unsigned source = (cfg >> 8) & 7u;
        const unsigned l = lalpha & 0xffu;
        const unsigned alpha = lalpha >> 16;
        const uint32_t parent_hz = parent->rate(parent->context, clock, source, l, alpha);
        if (parent_hz == 0) {
            return NezhaCameraCrmNoRate;
        }
        /* clk_rcg2_crmc treats HID=0 as 1. Round up after the half-divider. */
        unsigned hid = cfg & 31u;
        if (hid == 0) {
            hid = 1;
        }
        const uint64_t divided = ((uint64_t)parent_hz * 2u + hid) / (hid + 1u);
        if (divided == 0 || divided > UINT32_MAX) {
            return NezhaCameraCrmNoRate;
        }
        const uint32_t rate = (uint32_t)divided;
        if (next.count != 0 && rate <= next.rate[next.count - 1]) {
            break;
        }
        /* The vote wire format has three level bits. An eighth distinct
         * frequency is not representable and must not wrap to OFF. */
        if (level == 8) {
            return NezhaCameraCrmNoRate;
        }
        next.rate[next.count++] = rate;
    }
    if (next.count == 0) {
        return NezhaCameraCrmNoRate;
    }
    *plan = next;
    return NezhaCameraCrmOk;
}

static uint32_t base(const struct NezhaCameraCrmLease *lease)
{
    return SwDrv0 + (unsigned)lease->clock * SwVcdStride;
}

static enum NezhaCameraCrmStatus finish_active(
    const struct NezhaCameraRegisterIo *drv, struct NezhaCameraCrmLease *lease)
{
    const uint32_t offset = base(lease);
    for (unsigned ms = 0; ms <= TimeoutMs; ++ms) {
        const uint32_t irq = drv->read(drv->context, offset + IrqStatus);
        if (irq == UINT32_MAX) {
            return NezhaCameraCrmReadback;
        }
        if ((irq & Completion) != 0) {
            /* Only this VCD's event is consumed. An acknowledgement alone
             * is insufficient if the software vote register did not latch. */
            drv->write(drv->context, offset + IrqClear, Completion);
            if ((drv->read(drv->context, offset + IrqStatus) & Completion) != 0 ||
                drv->read(drv->context, offset + Active) != lease->installed) {
                return NezhaCameraCrmReadback;
            }
            lease->pending = false;
            return NezhaCameraCrmOk;
        }
        if (ms != TimeoutMs) {
            drv->wait_ms(drv->context, 1);
        }
    }
    return NezhaCameraCrmTimeout;
}

static enum NezhaCameraCrmStatus install_active(
    const struct NezhaCameraRegisterIo *drv, struct NezhaCameraCrmLease *lease,
    unsigned level)
{
    lease->installed = level;
    lease->pending = true;
    drv->write(drv->context, base(lease) + Active, level);
    return finish_active(drv, lease);
}

enum NezhaCameraCrmStatus nezha_camera_crm_acquire(
    const struct NezhaCameraRegisterIo *drv, const struct NezhaCameraCrmPlan *plan,
    uint32_t minimum_hz, struct NezhaCameraCrmLease *lease)
{
    if (!io_valid(drv) || plan == NULL || lease == NULL || !clock_valid(plan->clock) ||
        plan->count == 0 || plan->count > 7 || minimum_hz == 0) {
        return NezhaCameraCrmInvalid;
    }
    if (lease->requested || lease->pending || lease->ready) {
        return NezhaCameraCrmBusy;
    }
    unsigned level = 1;
    while (level <= plan->count && plan->rate[level - 1] < minimum_hz) {
        ++level;
    }
    if (level > plan->count) {
        return NezhaCameraCrmNoRate;
    }

    struct NezhaCameraCrmLease next = {.clock = plan->clock};
    const uint32_t offset = base(&next);
    for (unsigned state = 0; state < 3; ++state) {
        next.before[state] = drv->read(drv->context, offset + state * 4u);
        /* This camera test does not take over another SW DRV0 client. */
        if (next.before[state] != 0) {
            return NezhaCameraCrmBusy;
        }
    }
    if (drv->read(drv->context, offset + IrqStatus) != 0) {
        return NezhaCameraCrmBusy;
    }
    next.irq_enable_before = drv->read(drv->context, offset + IrqEnable);
    if (next.irq_enable_before == UINT32_MAX) {
        return NezhaCameraCrmReadback;
    }
    next.requested = true;
    *lease = next;
    /* Poll completion in the camera worker. Do not introduce a second IRQ
     * dispatcher or clear pending events belonging to another voter. */
    drv->write(drv->context, offset + IrqEnable, next.irq_enable_before & ~1u);
    if ((drv->read(drv->context, offset + IrqEnable) & Completion) != 0) {
        return NezhaCameraCrmReadback;
    }
    enum NezhaCameraCrmStatus result = install_active(drv, lease, level);
    if (result != NezhaCameraCrmOk) {
        return result;
    }
    drv->write(drv->context, offset + Sleep, level);
    drv->write(drv->context, offset + Wake, level);
    if (drv->read(drv->context, offset + Sleep) != level ||
        drv->read(drv->context, offset + Wake) != level) {
        return NezhaCameraCrmReadback;
    }
    lease->ready = true;
    return NezhaCameraCrmOk;
}

enum NezhaCameraCrmStatus nezha_camera_crm_release(
    const struct NezhaCameraRegisterIo *drv, struct NezhaCameraCrmLease *lease,
    bool consumers_quiescent)
{
    if (!io_valid(drv) || lease == NULL || !clock_valid(lease->clock)) {
        return NezhaCameraCrmInvalid;
    }
    if (!consumers_quiescent) {
        return NezhaCameraCrmBusy;
    }
    if (!lease->requested) {
        return NezhaCameraCrmOk;
    }
    lease->ready = false;
    if (lease->pending) {
        const enum NezhaCameraCrmStatus completed = finish_active(drv, lease);
        if (completed != NezhaCameraCrmOk) {
            return completed;
        }
    }
    const uint32_t offset = base(lease);
    for (unsigned state = 0; state < 3; ++state) {
        const uint32_t current = drv->read(drv->context, offset + state * 4u);
        if (current != lease->installed && current != lease->before[state]) {
            return NezhaCameraCrmBusy;
        }
    }
    drv->write(drv->context, offset + Wake, lease->before[2]);
    drv->write(drv->context, offset + Sleep, lease->before[1]);
    if (drv->read(drv->context, offset + Sleep) != lease->before[1] ||
        drv->read(drv->context, offset + Wake) != lease->before[2]) {
        return NezhaCameraCrmReadback;
    }
    if (drv->read(drv->context, offset + Active) != lease->before[0]) {
        const enum NezhaCameraCrmStatus result = install_active(drv, lease, lease->before[0]);
        if (result != NezhaCameraCrmOk) {
            return result;
        }
    }
    const uint32_t irq_enable = drv->read(drv->context, offset + IrqEnable);
    drv->write(drv->context, offset + IrqEnable,
        (irq_enable & ~1u) | (lease->irq_enable_before & 1u));
    if (drv->read(drv->context, offset + IrqEnable) !=
        ((irq_enable & ~1u) | (lease->irq_enable_before & 1u))) {
        return NezhaCameraCrmReadback;
    }
    *lease = (struct NezhaCameraCrmLease){0};
    return NezhaCameraCrmOk;
}
