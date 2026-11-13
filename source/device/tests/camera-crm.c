/* SPDX-License-Identifier: MIT */
#include "NezhaCameraCrm.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { Vcd = 7, VcdBase = 0x11c + Vcd * 0x20, Lut = Vcd * 0x210 + 0x150 };

struct Model {
    uint32_t drv[0x400 / 4], lut[0x2000 / 4];
    unsigned writes, active_writes, waits;
    bool late_completion, lose_sleep;
};

static uint32_t read_drv(void *context, uint32_t offset)
{
    const struct Model *m = context;
    assert(offset >= VcdBase && offset < VcdBase + 0x20 && offset % 4 == 0);
    return m->drv[offset / 4];
}

static void write_drv(void *context, uint32_t offset, uint32_t value)
{
    struct Model *m = context;
    assert(offset >= VcdBase && offset < VcdBase + 0x20 && offset % 4 == 0);
    ++m->writes;
    if (offset == VcdBase + 0x14) {
        assert(value == 1);
        m->drv[(VcdBase + 0x10) / 4] &= ~value;
        return;
    }
    if (offset == VcdBase + 4 && m->lose_sleep) {
        return;
    }
    m->drv[offset / 4] = value;
    if (offset == VcdBase) {
        ++m->active_writes;
        assert(value <= 7 && m->drv[(VcdBase + 0x10) / 4] == 0);
        if (!m->late_completion) {
            m->drv[(VcdBase + 0x10) / 4] = 1;
        }
    }
}

static void wait_ms(void *context, unsigned ms)
{
    struct Model *m = context;
    m->waits += ms;
}

static uint32_t read_lut(void *context, uint32_t offset)
{
    const struct Model *m = context;
    assert(offset < sizeof(m->lut) && offset % 4 == 0);
    return m->lut[offset / 4];
}

static uint32_t read_camcc(void *context, uint32_t offset)
{
    (void)context;
    switch (offset) {
    case 0x0010: return 0x4444003e;
    case 0x0014: return 0x8000;
    case 0x0018: return 0x840f;
    case 0x3010: return 0x44440025;
    case 0x3014: return 0x8777;
    case 0x3018: return 0x040f;
    default: assert(0); return UINT32_MAX;
    }
}

int main(void)
{
    struct Model m = {0};
    const struct NezhaCameraRegisterIo drv = {&m, read_drv, write_drv, wait_ms};
    const struct NezhaCameraRegisterIo lut = {.context = &m, .read = read_lut};
    struct NezhaCameraRegisterIo camcc = {.read = read_camcc};
    const struct NezhaCameraCrmParent parent = {&camcc, nezha_camera_crm_canoe_parent_rate};
    assert(parent.rate(parent.context, NezhaCameraCrmTfe0, 6, 0x25, 0x8777) == 360280000);
    assert(parent.rate(parent.context, NezhaCameraCrmTfe0, 6, 0, 0) == 360280000);
    assert(parent.rate(parent.context, NezhaCameraCrmCsid, 3, 0, 0) == 400000000);
    assert(parent.rate(parent.context, NezhaCameraCrmCsid, 2, 0, 0) == 600000000);
    assert(parent.rate(parent.context, NezhaCameraCrmTfe0, 1, 0, 0) == 0);
    /* Canoe's 1.2 GHz PLL0 / {4.5, 3, 2.5}. The repeated third level
     * terminates the firmware table; levels must not be guessed from source. */
    m.lut[(Lut + 1 * 0x18) / 4] = 0x108;
    m.lut[(Lut + 2 * 0x18) / 4] = 0x105;
    m.lut[(Lut + 3 * 0x18) / 4] = 0x104;
    m.lut[(Lut + 4 * 0x18) / 4] = 0x104;
    struct NezhaCameraCrmPlan plan = {0};
    assert(nezha_camera_crm_load_plan(&lut, &parent, NezhaCameraCrmCphyRx, &plan) == NezhaCameraCrmOk);
    assert(plan.count == 3 && plan.rate[0] == 266666667 && plan.rate[1] == 400000000 &&
           plan.rate[2] == 480000000);
    const struct Model cold = m;
    struct NezhaCameraCrmLease lease = {0};
    m.drv[(VcdBase + 0x18) / 4] = 0xa5;
    /* Mode1: 8 Gbit/s per trio / 32 + 1. */
    assert(nezha_camera_crm_acquire(&drv, &plan, 250000001, &lease) == NezhaCameraCrmOk);
    assert(lease.ready && lease.installed == 1 && m.active_writes == 1);
    assert(m.drv[(VcdBase + 4) / 4] == 1 && m.drv[(VcdBase + 8) / 4] == 1);
    const unsigned before_busy = m.writes;
    assert(nezha_camera_crm_release(&drv, &lease, false) == NezhaCameraCrmBusy);
    assert(m.writes == before_busy);
    assert(nezha_camera_crm_release(&drv, &lease, true) == NezhaCameraCrmOk);
    assert(m.drv[VcdBase / 4] == 0 && m.drv[(VcdBase + 4) / 4] == 0 &&
           m.drv[(VcdBase + 8) / 4] == 0 && m.drv[(VcdBase + 0x18) / 4] == 0xa5);

    m = cold;
    assert(nezha_camera_crm_acquire(&drv, &plan, 480000001, &lease) == NezhaCameraCrmNoRate);
    assert(m.writes == 0);
    m.drv[VcdBase / 4] = 2;
    assert(nezha_camera_crm_acquire(&drv, &plan, 250000001, &lease) == NezhaCameraCrmBusy);
    assert(m.writes == 0);

    m = cold;
    m.late_completion = true;
    assert(nezha_camera_crm_acquire(&drv, &plan, 300000000, &lease) == NezhaCameraCrmTimeout);
    assert(lease.pending && lease.installed == 2 && !lease.ready && m.active_writes == 1);
    assert(nezha_camera_crm_acquire(&drv, &plan, 300000000, &lease) == NezhaCameraCrmBusy);
    assert(nezha_camera_crm_release(&drv, &lease, true) == NezhaCameraCrmTimeout);
    assert(m.active_writes == 1 && m.waits == 200);
    /* A delayed completion permits restoration, without replaying the
     * original vote or overwriting an unresolved clock transition. */
    m.drv[(VcdBase + 0x10) / 4] = 1;
    m.late_completion = false;
    assert(nezha_camera_crm_release(&drv, &lease, true) == NezhaCameraCrmOk);
    assert(m.active_writes == 2 && !lease.pending && !lease.requested);

    m = cold;
    m.lose_sleep = true;
    assert(nezha_camera_crm_acquire(&drv, &plan, 400000001, &lease) == NezhaCameraCrmReadback);
    assert(!lease.ready && !lease.pending && lease.requested);
    assert(nezha_camera_crm_release(&drv, &lease, true) == NezhaCameraCrmOk);

    m = cold;
    m.drv[(VcdBase + 0x10) / 4] = 1;
    assert(nezha_camera_crm_acquire(&drv, &plan, 250000001, &lease) == NezhaCameraCrmBusy);
    assert(m.writes == 0);
    m.lut[(Lut + 1 * 0x18) / 4] = UINT32_MAX;
    assert(nezha_camera_crm_load_plan(&lut, &parent, NezhaCameraCrmCphyRx, &plan) == NezhaCameraCrmReadback);
    puts("Camera CRM uses firmware rates and owned VCD replies; late completion is reconciled without replay");
}
