/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "NezhaPowerBoard.h"
#include <string.h>
#include <sys/errno.h>

static int valid(NpPowerBoard *b) {
    return !b->quarantine && b->resources.lease_valid(b->resources.context, b->resources.lease);
}
static uint64_t now(void *v) {
    NpPowerBoard *b = v; return b->resources.now_ms(b->resources.context);
}
static int read32(NpPowerBoard *b, uint64_t pa, uint32_t *value) {
    if (!valid(b)) return EACCES;
    return b->resources.read32(b->resources.context, b->resources.lease, pa, value);
}
static int dma(void *v, uint64_t pa, uint32_t bytes) {
    NpPowerBoard *b = v;
    if (!valid(b)) return EACCES;
    return b->resources.dma_identity(b->resources.context, b->resources.lease, 0xb6, pa, bytes);
}
static void release_bus(void *v, int stopped) {
    NpPowerBoard *b = v;
    if (!stopped || !valid(b)) { b->quarantine = 1; return; }
    if (b->vote && b->resources.gauge_vote(b->resources.context, b->resources.lease, 0)) {
        b->quarantine = 1; return;
    }
    b->vote = 0;
}
static int acquire_bus(void *v) {
    NpPowerBoard *b = v;
    if (!valid(b) || b->suspend || b->vote) return EBUSY;
    if (b->resources.gauge_vote(b->resources.context, b->resources.lease, 1)) return EIO;
    b->vote = 1;
    const uint32_t branches[] = {0x2315c, 0x23148, 0x23140, 0x23144, 0x18508};
    for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); ++i) {
        unsigned tries; uint32_t value = 0;
        for (tries = 0; tries < 100; ++tries) {
            if (read32(b, 0x100000 + branches[i], &value)) goto fail;
            if (!(value & 0x80000000u)) break;
            b->resources.pause_us(b->resources.context, 100);
        }
        if (tries == 100) goto fail;
    }
    uint32_t firmware = 0, fifo_disabled = 0;
    if (read32(b, 0xa94068, &firmware) || read32(b, 0xa94064, &fifo_disabled) ||
        ((firmware >> 8) & 255) != 3 || !(fifo_disabled & 1)) goto fail;
    /* Shared pinmux/root clock already belongs to the platform. Leave it
     * configured. The GPI LOCK arbitrates SE execution with SOCCP. */
    return 0;
fail:
    release_bus(b, 1); return EIO;
}
static int gauge(void *v, uint8_t out[NP_GAUGE_WORDS][2], uint32_t *fields) {
    NpPowerBoard *b = v; return np_gauge_bus_read(&b->bus, out, fields);
}
static int lock(void *v) {
    NpPowerBoard *b = v;
    return valid(b) && b->resources.smem_lock(b->resources.context);
}
static void unlock(void *v) {
    NpPowerBoard *b = v; b->resources.smem_unlock(b->resources.context);
}
static void kick(void *v, uint32_t client, uint32_t signal) {
    NpPowerBoard *b = v;
    if (valid(b)) b->resources.doorbell(b->resources.context, client, signal);
    else b->quarantine = 1;
}
static void link(void *v, int up, uint64_t epoch) {
    (void)v; np_power27_link(up, epoch);
}
static void receive(void *v, uint64_t epoch, const uint8_t *p, uint32_t n) {
    (void)v; np_power27_receive(epoch, p, n);
}
static int poll(void *v) {
    NpPowerBoard *b = v;
    if (!valid(b) || b->suspend) return -1;
    if (!b->resources.enable_soccp) return 0;
    if (!b->attach_attempted) {
        b->attach_attempted = 1;
        NpSoccpIo io = {b, lock, unlock, kick, now, link, receive};
        if (np_soccp_attach(&b->soccp, b->resources.smem, 0x200000, &io)) return -1;
    }
    return np_soccp_poll(&b->soccp);
}
static int send(void *v, const uint8_t *p, uint32_t n) {
    NpPowerBoard *b = v;
    if (!valid(b) || b->suspend || !b->resources.enable_soccp) return -2;
    return np_soccp_send(&b->soccp, p, n);
}
static int quiesce(void *v) {
    NpPowerBoard *b = v;
    b->suspend = 1;
    if (b->resources.enable_soccp) np_soccp_quiesce(&b->soccp);
    if (b->bus.gpi.owned || b->vote || b->bus.gpi.poisoned || b->quarantine) return EIO;
    return 0;
}
static int resume(void *v) {
    NpPowerBoard *b = v;
    if (!valid(b)) return EACCES;
    b->suspend = 0;
    if (b->soccp.attached) return np_soccp_resume(&b->soccp);
    return 0;
}
int np_board_prepare(NpPowerBoard *b, const NpBoardResources *r, NpPowerOps *ops) {
    if (!b || !r || !ops || !r->lease || r->gpii >= 5 || !r->lease_valid || !r->read32 ||
        !r->gauge_vote || !r->dma_identity || !r->now_ms || !r->pause_us ||
        !r->stock_clock.clk_div || !r->stock_clock.t_cycle ||
        (r->enable_soccp && (!r->smem || !r->smem_lock || !r->smem_unlock || !r->doorbell))) return EINVAL;
    memset(b, 0, sizeof(*b)); b->resources = *r;
    b->bus.context = b; b->bus.acquire = acquire_bus; b->bus.release = release_bus;
    b->bus.gpii = r->gpii; b->bus.clock = r->stock_clock;
    b->bus.gpi.dma_context = b; b->bus.gpi.authorize_dma = dma;
    ops->context = b; ops->now_ms = now; ops->poll_link = poll; ops->send_mca = send;
    ops->read_gauge = gauge; ops->quiesce = quiesce; ops->resume = resume;
    if (!r->enable_soccp) ops->schema_verified = 0;
    return 0;
}
