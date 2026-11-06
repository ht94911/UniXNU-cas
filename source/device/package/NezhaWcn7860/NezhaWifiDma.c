/* SPDX-License-Identifier: GPL-2.0-only */
#include "NezhaWifiDma.h"
#if defined(KERNEL)
#include <libkern/libkern.h>
#include <sys/errno.h>
#else
#include <string.h>
#include <errno.h>
#endif

static int stop_read(const NezhaWifiStopOps *o, unsigned r, uint32_t *v)
{
    int e = o->read(o->context, r, v);
    return e ? e : *v == UINT32_MAX ? EIO : 0;
}
static int stop_poll(const NezhaWifiStopOps *o, unsigned r, uint32_t mask,
                     uint32_t wanted, unsigned timeout, uint32_t *last)
{
    uint64_t start = o->now_us(o->context);
    for (unsigned attempts = 0; attempts <= timeout / 1000 + 1; attempts++) {
        int e = stop_read(o, r, last);
        if (e || (*last & mask) == wanted) return e;
        uint64_t now = o->now_us(o->context);
        if (now < start || now - start >= timeout) return ETIMEDOUT;
        o->sleep_us(o->context, 1000);
    }
    return ETIMEDOUT;
}
#define STOP_CHECK(x) do { int e_ = (x); if (e_) { p->error = e_; return e_; } } while (0)
int NezhaWifiStopDma(const NezhaWifiStopOps *o, NezhaWifiStopProof *p, unsigned timeout)
{
    if (!p) return EINVAL;
    memset(p, 0, sizeof(*p));
    if (!o || !o->read || !o->write || !o->disable_bus_master_and_msi ||
        !o->assert_radio_reset || !o->now_us || !o->sleep_us || timeout < 1000 || timeout > 2000000) {
        p->error = EINVAL; return EINVAL;
    }
    p->phase = 1;
    STOP_CHECK(stop_read(o, NezhaWifiStopMhiControl, &p->control));
    STOP_CHECK(stop_read(o, NezhaWifiStopExecEnv, &p->environment));
    STOP_CHECK(stop_read(o, NezhaWifiStopPmStatus, &p->pm_status));
    STOP_CHECK(stop_read(o, NezhaWifiStopPciPmControl, &p->pm_control_before));
    p->pm_control = p->pm_control_before;
    if ((p->control & 2) || p->environment != 2 || (p->pm_control_before & 3) ||
        (p->pm_status & (7u << 7)) == (3u << 7)) {
        p->error = EBUSY; return EBUSY;
    }
    p->phase = 2;
    /* MHI host pm.c: RESET completion stops host-memory access by MHI. */
    STOP_CHECK(o->write(o->context, NezhaWifiStopMhiControl, p->control | 2u));
    STOP_CHECK(stop_poll(o, NezhaWifiStopMhiControl, 2, 0, timeout, &p->control));
    p->phase = 3;
    STOP_CHECK(stop_read(o, NezhaWifiStopExecEnv, &p->environment));
    if (p->environment != 0) {
        if (p->environment != 1 && p->environment != 2) { p->error = EIO; return EIO; }
        STOP_CHECK(stop_poll(o, NezhaWifiStopMhiStatus, 1, 1, timeout, &p->status));
        if (p->status & 4) { p->error = EIO; return EIO; }
    }
    p->phase = 4;
    STOP_CHECK(o->disable_bus_master_and_msi(o->context));
    STOP_CHECK(stop_read(o, NezhaWifiStopPciCommand, &p->command));
    if (p->command & 4) { p->error = EBUSY; return EBUSY; }
    p->phase = 5;
    /* Actual Nezha cnss2.ko enters PCI_D3hot before suspending the link.
     * Clear the W1C PME_Status bit in the write value, preserving its status.
     * The board adapter has validated a type-0 endpoint PM capability. */
    STOP_CHECK(stop_read(o, NezhaWifiStopPciPmControl, &p->pm_control));
    if (p->pm_control & 3) { p->error = EBUSY; return EBUSY; }
    STOP_CHECK(o->write(o->context, NezhaWifiStopPciPmControl,
                       (p->pm_control & ~(3u | (1u << 15))) | 3u));
    o->sleep_us(o->context, 10000); /* PCI D3hot transition settling time. */
    STOP_CHECK(stop_poll(o, NezhaWifiStopPciPmControl, 3, 3, timeout, &p->pm_control));
    p->phase = 6;
    /* Same-SM8850 msm_pcie_pm_suspend: ELBI_SYS_CTRL.PME_TURNOFF, followed
     * by PARF_PM_STTS_1.L2. Covers the PCIe client, including offloaded CE. */
    STOP_CHECK(o->write(o->context, NezhaWifiStopElbiControl, 1u << 4));
    p->l2_requested = 1;
    STOP_CHECK(stop_poll(o, NezhaWifiStopPmStatus, 7u << 7, 3u << 7, timeout, &p->pm_status));
    p->phase = 7;
    STOP_CHECK(o->assert_radio_reset(o->context));
    p->phase = 8; p->completed = 1;
    return 0;
}
#undef STOP_CHECK

static int pool_valid(const NezhaWifiDmaPool *p)
{ return p && p->records && p->capacity && p->count <= p->capacity; }
int NezhaWifiDmaTrack(NezhaWifiDmaPool *p, void *address, size_t bytes, unsigned *index)
{
    if (!pool_valid(p) || !address || !bytes || !index || (uintptr_t)address > UINTPTR_MAX - bytes)
        return EINVAL;
    if (p->count == p->capacity) return ENOSPC;
    for (unsigned i = 0; i < p->count; i++) {
        const NezhaWifiDmaRecord *r = &p->records[i];
        if ((uintptr_t)address < (uintptr_t)r->address + r->bytes &&
            (uintptr_t)r->address < (uintptr_t)address + bytes) return EINVAL;
    }
    unsigned n = p->count++;
    p->records[n] = (NezhaWifiDmaRecord){.address=address, .bytes=bytes, .in_use=1};
    *index = n; return 0;
}
int NezhaWifiDmaPublish(NezhaWifiDmaPool *p, unsigned n, uint64_t iova)
{
    if (!pool_valid(p) || n >= p->count || !iova || p->records[n].published ||
        p->records[n].bytes > UINT64_MAX - iova) return EINVAL;
    NezhaWifiDmaRecord *r = &p->records[n];
    for (unsigned i = 0; i < p->count; i++) {
        const NezhaWifiDmaRecord *s = &p->records[i];
        if (s->published && iova < s->iova + s->bytes && s->iova < iova + r->bytes) return EINVAL;
    }
    r->iova = iova; r->published = 1; return 0;
}
int NezhaWifiDmaTake(NezhaWifiDmaPool *p, size_t bytes, uint64_t alignment, NezhaWifiDmaRecord *out)
{
    if (!pool_valid(p) || !out || !bytes || !alignment || (alignment & (alignment - 1))) return EINVAL;
    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < p->count; i++) {
        NezhaWifiDmaRecord *r = &p->records[i];
        if (r->published && !r->in_use && !r->quarantined && r->bytes == bytes && !(r->iova & (alignment - 1))) {
            r->in_use = 1; p->reuses++; *out = *r; return 0;
        }
    }
    return ENOENT;
}
void NezhaWifiDmaQuarantine(NezhaWifiDmaPool *p)
{
    if (!pool_valid(p)) return;
    for (unsigned i = 0; i < p->count; i++)
        if (p->records[i].in_use) p->records[i].quarantined = 1;
}
int NezhaWifiDmaRecycle(NezhaWifiDmaPool *p, NezhaWifiStopProof *proof,
                       const void *calibration, size_t bytes)
{
    if (!pool_valid(p) || !proof || proof->consumed || !proof->completed || proof->error ||
        proof->phase != 8 || !proof->l2_requested || (proof->control & 2) || (proof->command & 4) ||
        (proof->pm_control_before & 3) || (proof->pm_control & 3) != 3 ||
        (proof->pm_status & (7u << 7)) != (3u << 7) ||
        (proof->environment != 0 && ((proof->environment != 1 && proof->environment != 2) ||
         !(proof->status & 1) || (proof->status & 4)))) return EINVAL;
    proof->consumed = 1;
    if (!calibration || !bytes || (uintptr_t)calibration > UINTPTR_MAX - bytes) return EINVAL;
    unsigned keep = p->count;
    for (unsigned i = 0; i < p->count; i++) {
        const NezhaWifiDmaRecord *r = &p->records[i];
        if (r->published && (uintptr_t)calibration >= (uintptr_t)r->address &&
            (uintptr_t)calibration - (uintptr_t)r->address <= r->bytes &&
            bytes <= r->bytes - ((uintptr_t)calibration - (uintptr_t)r->address)) keep = i;
    }
    if (keep == p->count) return EINVAL;
    p->records[keep].in_use = 1;
    for (unsigned i = 0; i < p->count; i++) {
        NezhaWifiDmaRecord *r = &p->records[i];
        if (i != keep && r->published && !r->quarantined) r->in_use = 0;
    }
    p->generations++; return 0;
}
