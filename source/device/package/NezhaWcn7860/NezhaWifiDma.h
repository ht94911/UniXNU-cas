/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NEZHA_WIFI_DMA_H
#define NEZHA_WIFI_DMA_H
#include <stddef.h>
#include <stdint.h>

/* These registers belong only to the WLAN endpoint and its PCIe0 root port. */
enum NezhaWifiStopRegister {
    NezhaWifiStopMhiControl, NezhaWifiStopMhiStatus, NezhaWifiStopExecEnv,
    NezhaWifiStopPciCommand, NezhaWifiStopPmStatus, NezhaWifiStopElbiControl
};
typedef struct {
    void *context;
    int (*read)(void *, unsigned, uint32_t *);
    int (*write)(void *, unsigned, uint32_t);
    int (*disable_bus_master_and_msi)(void *);
    int (*assert_radio_reset)(void *);
    uint64_t (*now_us)(void *);
    void (*sleep_us)(void *, unsigned);
} NezhaWifiStopOps;
typedef struct {
    unsigned phase, completed, consumed, l2_requested;
    int error;
    uint32_t control, status, environment, command, pm_status;
} NezhaWifiStopProof;

/* Requires a healthy mission-mode endpoint and a single, stopped host worker.
 * Mirrors MHI reset completion and msm_pcie_pm_suspend's PM_Enter_L23 proof,
 * then verifies board PERST/WLAN_EN containment. No success from delay alone,
 * GPIO alone, faulting an SMMU stream, a stale L2 state or an MMIO failure. */
int NezhaWifiStopDma(const NezhaWifiStopOps *, NezhaWifiStopProof *, unsigned timeout_us);

typedef struct {
    void *address;
    uint64_t iova;
    size_t bytes;
    unsigned published, in_use, quarantined;
} NezhaWifiDmaRecord;
typedef struct {
    NezhaWifiDmaRecord *records;
    unsigned capacity, count, generations, reuses;
} NezhaWifiDmaPool;

/* Pages/mappings never leave this driver's ownership. Failed/partial mappings
 * stay unpublished and cannot be recycled. CAL storage is carried unchanged. */
int NezhaWifiDmaTrack(NezhaWifiDmaPool *, void *, size_t, unsigned *index);
int NezhaWifiDmaPublish(NezhaWifiDmaPool *, unsigned index, uint64_t iova);
int NezhaWifiDmaTake(NezhaWifiDmaPool *, size_t, uint64_t alignment, NezhaWifiDmaRecord *);
int NezhaWifiDmaRecycle(NezhaWifiDmaPool *, NezhaWifiStopProof *, const void *calibration, size_t);
void NezhaWifiDmaQuarantine(NezhaWifiDmaPool *);
#endif
