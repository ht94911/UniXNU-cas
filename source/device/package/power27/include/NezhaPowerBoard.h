/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_POWER_BOARD_H
#define NEZHA_POWER_BOARD_H
#ifdef KERNEL
#include "NezhaGaugeBus.h"
#include "NezhaSoccp.h"
#include "NezhaPowerRuntime.h"

/* Adapter to the selected package's EXISTING shared resource owner. These are
 * required dependencies, not weak success stubs. The lease is nonrevocable
 * until verified quiesce, including intervals with zero clock votes. One lifetime reservation owns
 * GPII and the optional SOCCP edge. This module never programs an SMMU bank or
 * rewrites a whole GCC vote word behind another driver's back. */
typedef struct {
    void *context;
    uint64_t lease;
    uint32_t gpii;
    PgpiI2cClock stock_clock;
    volatile uint8_t *smem;
    int enable_soccp;
    int (*lease_valid)(void *, uint64_t);
    int (*read32)(void *, uint64_t lease, uint64_t address, uint32_t *);
    /* Shared owner refcounts bits18..21,27 of GCC152008 and the three ICC
     * paths. Zero succeeds. Off drops ONLY this client's vote. */
    int (*gauge_vote)(void *, uint64_t lease, int on);
    int (*dma_identity)(void *, uint64_t lease, uint32_t sid, uint64_t pa, uint32_t len);
    /* Same ALL-AP lock used by audio/WLAN SMEM allocators, then TCSR lock3. */
    int (*smem_lock)(void *); /* nonzero acquired */
    void (*smem_unlock)(void *);
    void (*doorbell)(void *, uint32_t client, uint32_t signal);
    uint64_t (*now_ms)(void *);
    void (*pause_us)(void *, uint32_t);
} NpBoardResources;
typedef struct {
    NpBoardResources resources;
    NpGaugeBus bus;
    NpSoccp soccp;
    uint64_t attach_retry_at, attach_deadline;
    uint8_t vote, quarantine, attach_attempted, suspend;
} NpPowerBoard;
/* No hardware access. schema carries only the verified property definitions;
 * callbacks are filled here. schema_verified=0 leaves charger data unknown. */
int np_board_prepare(NpPowerBoard *, const NpBoardResources *, NpPowerOps *schema);
#endif
#endif
