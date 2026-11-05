/* SPDX-License-Identifier: GPL-2.0-only */
/* Nezha/SM8850 Peach: board power, CESTA v3, PCIe0 and isolated firmware DMA.
 * Register sequences follow Qualcomm's pci-msm.c, crm-v2.c, clk-rpmh.c and
 * rpmh-regulator.c, plus this phone's captured DT. No Linux module ABI.
 * Opt-in nezha_wifi=1; en0 is discovered after firmware initialization.
 * Its data link stays down until actual association/security completion.
 * All buffers and mappings live until reboot, including on failure. */
#if defined(__arm64__) || defined(__aarch64__)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/random.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include "adreno/ArmSmmu4KStreams.h"
#include "NezhaWifiPlatform.h"
#include "NezhaWifiAuto.h"
#include "NezhaWifiDma.h"
#include "Sm8850Rpmh.h"
#include "qcom/PeachMhiFirmware.h"
#include "qcom/PeachMhiHost.h"
#include "qcom/PeachWlfw.h"
#include "qcom/PeachCe.h"
#include "qcom/PeachHtc.h"
#include "qcom/PeachHtt.h"
#include "qcom/PeachWmi.h"
#include "qcom/PeachSrng.h"
#include "qcom/PeachRx.h"
#include "qcom/PeachTx.h"
#include "qcom/PeachStation.h"
#include "qcom/PeachRsn.h"
#include "qcom/PeachNet.h"
#include "qcom/NezhaPcieData.h"

extern kern_return_t thread_terminate(thread_t);
void nezha_wifi_init(void);

enum { W_GCC, W_PARF, W_PHY, W_SM, W_CRM, W_COMMON, W_CRMC, W_DBI,
       W_ATU, W_CFG, W_BAR, W_SMMU, W_EN, W_SW, W_PERST, W_SOL, W_RSC, W_DB, W_NWIN };
struct wifi_window { uint64_t pa; size_t bytes; vm_offset_t va; };
static struct wifi_window wifi_windows[W_NWIN] = {
    {0x00100000, 0xb0000, 0}, {0x01c00000, 0x3000, 0},
    {0x01c06000, 0x2000, 0}, {0x01d07000, 0x7000, 0},
    {0x01d01000, 0x2000, 0}, {NEZHA_PCIE_CRM_COMMON_PA, NEZHA_PCIE_CRM_COMMON_BYTES, 0},
    {0x01d03800, 0x2000, 0}, {0x400000000, 0x10000, 0},
    {0x410000000, 0x1000, 0}, {0x400100000, 0x4000, 0},
    {0x40400000, 0x200000, 0}, {0x15000000, 0x100000, 0},
    {0x0f110000, 0x1000, 0}, {0x0f112000, 0x2000, 0},
    {0x0f166000, 0x3000, 0}, {0x0f1cc000, 0x2000, 0},
    {0x18920000, 0x10000, 0}, {0x81c60000, 0x20000, 0},
};
static int wifi_fault, wifi_wait;
static unsigned wifi_phase, wifi_bank, wifi_slot, wifi_msi_cap;
static const char *wifi_step = "init";
static uint64_t wifi_arena_pa, wifi_tables_pa;
static vm_offset_t wifi_arena, wifi_tables;
static ArmSmmu4KIo wifi_smmu_io;
static ArmSmmu4KConfig wifi_table_config;
static ArmSmmu4KResult wifi_table_result;
static ArmSmmu4KContextConfig wifi_context;
static ArmSmmu4KStreamState wifi_stream;
static ArmSmmu4KStreamResult wifi_stream_result;
static PeachMhiFirmware wifi_firmware;
static PeachMhiDma wifi_rddm;
static uint32_t wifi_rddm_session;
static PeachMhiHost wifi_host;
static PeachWlfw wifi_wlfw;
static PeachCe wifi_ce;
static PeachHtc wifi_htc;
static PeachHtt wifi_htt;
static PeachWmi wifi_wmi;
static PeachSrng wifi_srng[PeachSrngKinds];
static PeachHttRing wifi_rxmac[2];
static unsigned wifi_rxmac_count;
static PeachRx wifi_rx;
static PeachTx wifi_tx;
static PeachStation wifi_station;
static PeachRsn wifi_rsn;
static PeachNetRequest wifi_request;
static unsigned wifi_net_attached, wifi_net_state = PeachNetStarting;
static uint32_t wifi_target_window = 0xffffffffu;
static uint64_t wifi_context_offset, wifi_dma_next, wifi_check_at;
static uint32_t wifi_context_control;
static unsigned wifi_table_next;
#define WIFI_RECOVERIES 3u
#define WIFI_DMA_ALLOCATIONS (((PEACH_WLFW_SEGMENTS + PEACH_WMI_MEMORY_REQUESTS + 12) * (WIFI_RECOVERIES + 1)) + 1)
static NezhaWifiDmaRecord wifi_dma_records[WIFI_DMA_ALLOCATIONS];
static NezhaWifiDmaPool wifi_dma_pool = {.records=wifi_dma_records, .capacity=WIFI_DMA_ALLOCATIONS};
static NezhaWifiStopProof wifi_stop_proof;
#define WIFI_IOVA 0x20000000ull
#define WIFI_DMA_BYTES 0x920000u
#define WIFI_DMA_LIMIT 0x30000000ull
/* Peach firmware validates the DDR geometry through the IPA window's end,
 * as CNSS host_cap does on this phone. This publishes an address range;
 * allocations stay capped above and SMMU entries exist only for owned pages. */
#define WIFI_IOVA_LIMIT 0xb0010000ull
#define WIFI_TABLE_BYTES 0x90000u
#define WIFI_MSI_OFFSET 0x808000u
static uint64_t wifi_arena_iova = WIFI_IOVA;
static unsigned wifi_generations, wifi_quarantined_recoveries, wifi_radio_off, wifi_l2_requested;
#define WIFI_TRY(expr) do { int err_ = (expr); if (err_) return err_; } while (0)
static int wifi_dma_health(void);

static void
wifi_sleep(void *unused, unsigned us)
{
    (void)unused;
    uint64_t deadline;
    clock_interval_to_deadline(us, NSEC_PER_USEC, &deadline);
    (void)assert_wait_deadline(&wifi_wait, THREAD_UNINT, deadline);
    thread_block(THREAD_CONTINUE_NULL);
}

static uint64_t
wifi_now(void *unused)
{
    (void)unused;
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

/* Use kernel_map rather than the small shared io_map submap. Merely mapping
 * an unpowered window performs no device access. Partial mappings are retained. */
static int
wifi_map(unsigned index)
{
    struct wifi_window *w = &wifi_windows[index];
    if (w->va) return 0;
    w->va = nezha_wifi_map_device(w->pa, w->bytes);
    if (!w->va) return ENOMEM;
    return 0;
}

static int
wifi_access(unsigned window, size_t off, uint64_t *value, unsigned write, unsigned wide)
{
    if (window >= W_NWIN || !value || write > 1 || wide > 1) return EINVAL;
    struct wifi_window *w = &wifi_windows[window];
    size_t bytes = wide ? 8 : 4;
    if (wifi_fault) return EIO;
    if (!w->va || (off & (bytes - 1)) || off > w->bytes - bytes) return EINVAL;
    if (nezha_wifi_mmio_access(w->va + off, value, write, wide)) {
        wifi_fault = 1;
        printf("NEZHA WIFI: MMIO fault in %s at %#llx (%s); stopping\n",
            wifi_step, w->pa + off, write ? "write" : "read");
        return EIO;
    }
    return 0;
}

static int wifi_rd(unsigned w, size_t o, uint32_t *v)
{
    uint64_t value = 0;
    int err = wifi_access(w, o, &value, 0, 0);
    *v = (uint32_t)value;
    return err;
}
static int wifi_wr(unsigned w, size_t o, uint32_t v)
{
    uint64_t value = v;
    return wifi_access(w, o, &value, 1, 0);
}
static int wifi_modify(unsigned w, size_t o, uint32_t clear, uint32_t set)
{
    uint32_t value;
    WIFI_TRY(wifi_rd(w, o, &value));
    return wifi_wr(w, o, (value & ~clear) | set);
}
static int wifi_poll(unsigned w, size_t o, uint32_t mask, uint32_t want, unsigned ms)
{
    uint64_t end = wifi_now(NULL) + ms * 1000ull;
    do {
        uint32_t value;
        if (w == W_BAR) WIFI_TRY(wifi_dma_health());
        WIFI_TRY(wifi_rd(w, o, &value));
        if (value != 0xffffffffu && (value & mask) == want) return 0;
        wifi_sleep(NULL, 1000);
    } while (wifi_now(NULL) < end);
    return ETIMEDOUT;
}

/* RSC transactions run only on this worker after USB/touch have initialized.
 * The SoC parser validates the command database rather than guessing addresses. */
static int wifi_rsc_read(void *c, unsigned w, uint32_t o, uint32_t *v)
{ (void)c; return wifi_rd(w == Sm8850RpmhRsc ? W_RSC : W_DB, o, v); }
static int wifi_rsc_write(void *c, unsigned w, uint32_t o, uint32_t v)
{ (void)c; return w == Sm8850RpmhRsc ? wifi_wr(W_RSC, o, v) : EINVAL; }
static Sm8850Rpmh wifi_rpmh = {.context = NULL, .read = wifi_rsc_read, .write = wifi_rsc_write,
    .now_us = wifi_now, .sleep_us = wifi_sleep, .db_bytes = 0x20000, .rsc_bytes = 0x10000};
static int wifi_rpmh_resource(const char *name, uint32_t offset, uint32_t value)
{ return Sm8850RpmhResource(&wifi_rpmh, name, offset, value); }
static int wifi_rpmh_arc(const char *name, uint32_t level)
{ return Sm8850RpmhArc(&wifi_rpmh, name, level); }

/* All pin settings below are from the captured pinctrl states. The pinctrl-
 * canoe source gives mux selector 1 for gpio18/19/103's requested functions. */
static int wifi_pin(unsigned w, size_t off, unsigned mux, unsigned pull,
                    unsigned drive, int output)
{
    if (output >= 0) WIFI_TRY(wifi_modify(w, off + 4, 2, output ? 2 : 0));
    return wifi_modify(w, off, 0x3ff, pull | mux << 2 | drive << 6 |
        (output >= 0 ? 1u << 9 : 0));
}

static int
wifi_power(void)
{
    /* CNSS vreg list order with this board's DT voltage/load overrides.
     * Mode is sent only when the provider defines it; 5 is PMIC7 LDO OPM.
     * wcal-pbs is a calibration trigger, not a rail in cnss_vreg_list. */
    static const struct { const char *name; uint16_t mv; uint8_t mode; } rails[] = {
        {"L3G_E0", 1200, 5}, {"L3K_E1", 1200, 7}, {"L2G_E0", 1800, 5},
        {"S2J_E1", 892, 0}, {"S8F_E0", 1876, 0}, {"S7F_E0", 1328, 7},
        {"S1J_E1", 892, 0},
    };
    wifi_step = "board power";
    WIFI_TRY(wifi_pin(W_PERST, 0, 0, 1, 0, 0));
    WIFI_TRY(wifi_pin(W_EN, 0, 0, 1, 0, 0));
    wifi_sleep(NULL, 5000);
    for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
        WIFI_TRY(wifi_rpmh_resource(rails[i].name, 0, rails[i].mv));
        if (rails[i].mode) WIFI_TRY(wifi_rpmh_resource(rails[i].name, 8, rails[i].mode));
        WIFI_TRY(wifi_rpmh_resource(rails[i].name, 4, 1));
    }
    WIFI_TRY(wifi_rpmh_resource("C1A_E0", 4, 1));
    WIFI_TRY(wifi_modify(W_SW, 0, 0x3c, 1u << 2));
    WIFI_TRY(wifi_modify(W_SW, 0x1000, 0x3c, 1u << 2));
    WIFI_TRY(wifi_pin(W_SOL, 0, 0, 1, 1, 0));
    WIFI_TRY(wifi_pin(W_SOL, 0x1000, 0, 0, 0, -1));
    WIFI_TRY(wifi_pin(W_PERST, 0x1000, 1, 3, 0, -1));
    WIFI_TRY(wifi_pin(W_PERST, 0x2000, 0, 3, 0, -1));
    WIFI_TRY(wifi_pin(W_EN, 0, 0, 3, 7, 1));
    wifi_sleep(NULL, 1000);
    wifi_phase = 1;
    return 0;
}

static int
wifi_cesta(unsigned perf)
{
    uint32_t cfg, enabled, update, busy;
    wifi_step = "CESTA v3";
    WIFI_TRY(wifi_rd(W_COMMON, 0x0c, &enabled));
    WIFI_TRY(wifi_rd(W_COMMON, 4, &cfg));
    printf("NEZHA WIFI: CESTA enabled %#x config %#x\n", enabled, cfg);
    unsigned resources = (cfg >> 6) & 15, nodes = (cfg >> 26) & 31;
    unsigned states = cfg & 15, channels = (cfg >> 4) & 3;
    if (!enabled || enabled == 0xffffffffu || channels != 2 || states < 2 || states > 5 ||
        resources < 1 || resources > 2 || nodes > 16 || perf > 5 ||
        (nodes && 0xb0 + (nodes - 1) * 0x2c + 0x14 + states * 4 > 0x370)) return ENOTSUP;
    WIFI_TRY(wifi_rd(W_CRM, 0x374, &update));
    WIFI_TRY(wifi_rd(W_CRM, 0x370, &busy));
    if (update & ~3u || update == 3 || (busy & ~3u)) return EBUSY;
    unsigned old = update == 2 ? 1 : 0, target = update ? old ^ 1 : 0;
    /* Preserve every other resource and passthrough vote in HW DRV0. Only
     * RC0 PERF_OL state 0/1 belongs to this driver. Do not touch HW DRV1. */
    for (unsigned type = 0; type < 2; type++) {
        unsigned n = type ? nodes : resources, base = type ? 0xb0 : 0;
        for (unsigned r = 0; r < n; r++) for (unsigned s = 0; s < states; s++) {
            uint32_t value;
            size_t off = base + r * 0x2c + s * 4;
            WIFI_TRY(wifi_rd(W_CRM, off + old * 0x14, &value));
            if (!type && !r && s < 2) value = perf;
            WIFI_TRY(wifi_wr(W_CRM, off + target * 0x14, value));
        }
    }
    WIFI_TRY(wifi_wr(W_CRM, 0x374, 1u << target));
    WIFI_TRY(wifi_poll(W_CRM, 0x370, 1u << target, 1u << target, 10));
    return 0;
}

static int
wifi_pcie_resources(void)
{
    wifi_step = "PCIe state manager";
    WIFI_TRY(wifi_wr(W_SM, 0x1040, 0));
    WIFI_TRY(wifi_wr(W_SM, 0x1048, 0));
    for (unsigned i = 0; i < sizeof(wifi_sm_seq) / 4; i++)
        WIFI_TRY(wifi_wr(W_SM, 4 * i, wifi_sm_seq[i]));
    for (unsigned i = 0; i < sizeof(wifi_sm_branch) / 4; i++)
        WIFI_TRY(wifi_wr(W_SM, 0x1000 + 4 * i, wifi_sm_branch[i]));
    WIFI_TRY(wifi_modify(W_SM, 0x1090, 0, 1));
    /* GCC's captured mx provider has init-voltage-level=384. */
    wifi_step = "PCIe MX ARC vote";
    int arc_error = wifi_rpmh_arc("lmx.lvl", 384);
    printf("NEZHA WIFI: PCIe lmx.lvl request 384 result %d\n", arc_error);
    if (arc_error) return arc_error;
    wifi_step = "PCIe PHY GDSC";
    WIFI_TRY(wifi_modify(W_GCC, 0x6c000, 0xfff002, 0x222000));
    WIFI_TRY(wifi_modify(W_GCC, 0x5214c, 4, 0));
    WIFI_TRY(wifi_poll(W_GCC, 0x6c004, 1u << 16, 1u << 16, 10));
    WIFI_TRY(wifi_modify(W_GCC, 0x6c000, 0, 1u << 11));
    WIFI_TRY(wifi_cesta(4)); /* DT target Gen3 -> performance level 4. */
    wifi_phase = 2;
    return 0;
}

static int
wifi_atu(unsigned region, uint32_t type, uint64_t base, uint64_t end, uint64_t target)
{
    unsigned off = region * 0x200;
    WIFI_TRY(wifi_wr(W_ATU, off, type));
    WIFI_TRY(wifi_wr(W_ATU, off + 8, (uint32_t)base));
    WIFI_TRY(wifi_wr(W_ATU, off + 12, (uint32_t)(base >> 32)));
    WIFI_TRY(wifi_wr(W_ATU, off + 16, (uint32_t)end));
    WIFI_TRY(wifi_wr(W_ATU, off + 20, (uint32_t)target));
    WIFI_TRY(wifi_wr(W_ATU, off + 24, (uint32_t)(target >> 32)));
    WIFI_TRY(wifi_wr(W_ATU, off + 4, 1u << 31));
    return wifi_poll(W_ATU, off + 4, 1u << 31, 1u << 31, 10);
}

/* The controller's hardware BDF-to-SID table uses the PCIe CRC8 hash. This
 * computes that required register index, not a file-integrity check. */
static unsigned wifi_sid_hash(unsigned bdf)
{
    unsigned crc = 0;
    for (int shift = 8; shift >= 0; shift -= 8) {
        crc ^= (bdf >> shift) & 255;
        for (unsigned i = 0; i < 8; i++) crc = (crc << 1) ^ ((crc & 128) ? 7 : 0);
        crc &= 255;
    }
    return crc;
}

static int
wifi_core_preset(uint32_t capability)
{
    /* Extended capability offsets vary with Synopsys IP revision. */
    unsigned offset = 0x100, gen = capability & 15, lanes = (capability >> 4) & 63;
    WIFI_TRY(wifi_modify(W_DBI, 0x8bc, 0, 1));
    for (unsigned visited = 0; offset && visited < 64; visited++) {
        uint32_t header;
        if ((offset & 3) || offset < 0x100 || offset > 0xfc0) return EINVAL;
        WIFI_TRY(wifi_rd(W_DBI, offset, &header));
        if (!header || header == 0xffffffffu) break;
        unsigned id = header & 65535;
        if ((id == 0x19 && gen >= 3) || (id == 0x26 && gen >= 4)) {
            unsigned first = id == 0x19 ? 12 : 32, step = id == 0x19 ? 2 : 4;
            for (unsigned i = 0; i < lanes; i += step)
                WIFI_TRY(wifi_wr(W_DBI, offset + first + i / step * 4, 0x55555555));
        }
        unsigned next = header >> 20;
        if (next && next <= offset) return EINVAL;
        offset = next;
    }
    return wifi_modify(W_DBI, 0x8bc, 1, 0);
}

/* Polling must suppress interrupts at the endpoint, not just at PARF.
 * Never enable bus mastering before every discovered MSI/MSI-X is disabled
 * and legacy INTx is masked. Capability offsets come from this endpoint. */
static int
wifi_mask_interrupts(void)
{
    uint32_t value, next;
    WIFI_TRY(wifi_rd(W_CFG, 4, &value));
    WIFI_TRY(wifi_wr(W_CFG, 4, (value & 0xfffbu) | 0x400));
    /* A checked endpoint reset invalidates the previous enumeration. Keep
     * duplicate MSI detection local to this walk, including calibration restart. */
    wifi_msi_cap = 0;
    if (value & 0x100000) {
        WIFI_TRY(wifi_rd(W_CFG, 0x34, &next));
        next &= 255;
        uint64_t visited = 0;
        for (unsigned n = 0; next && n < 48; n++) {
            if ((next & 3) || next < 0x40 || next > 0xfc || visited & 1ull << (next / 4))
                return EINVAL;
            visited |= 1ull << (next / 4);
            WIFI_TRY(wifi_rd(W_CFG, next, &value));
            uint32_t masked = value;
            if ((value & 255) == 5) {
                if (wifi_msi_cap) return EINVAL;
                wifi_msi_cap = next;
                masked &= ~(1u << 16);
            }
            if ((value & 255) == 0x11) masked = (masked & ~(1u << 31)) | 1u << 30;
            if (masked != value) {
                WIFI_TRY(wifi_wr(W_CFG, next, masked));
                uint32_t check;
                WIFI_TRY(wifi_rd(W_CFG, next, &check));
                if (check != masked) return EIO;
            }
            next = (value >> 8) & 255;
        }
        if (next) return EINVAL;
    }
    WIFI_TRY(wifi_rd(W_CFG, 4, &value));
    return (value & 0x404) == 0x400 ? 0 : EIO;
}

static int
wifi_pcie_link(void)
{
    uint32_t value;
    wifi_step = "PCIe core reset";
    WIFI_TRY(wifi_modify(W_GCC, 0x6b000, 0, 1));
    WIFI_TRY(wifi_modify(W_GCC, 0x6c01c, 0, 1));
    wifi_sleep(NULL, 1000);
    WIFI_TRY(wifi_modify(W_GCC, 0x6c01c, 1, 0));
    WIFI_TRY(wifi_modify(W_GCC, 0x6b000, 1, 0));
    wifi_sleep(NULL, 1000);
    wifi_step = "PCIe PARF";
    WIFI_TRY(wifi_wr(W_PARF, 0x1000, 4));
    WIFI_TRY(wifi_modify(W_PARF, 0x40, 1, 0));
    WIFI_TRY(wifi_wr(W_PARF, 0, 0x365e));
    WIFI_TRY(wifi_modify(W_PARF, 0x174, 0, 1u << 4));
    WIFI_TRY(wifi_wr(W_PARF, 0x22c, 0)); /* Ring polling; no unhandled IRQs. */
    WIFI_TRY(wifi_wr(W_PARF, 0x508, 0));
    WIFI_TRY(wifi_wr(W_PARF, 0x350, 0));
    WIFI_TRY(wifi_wr(W_PARF, 0x354, 4));
    WIFI_TRY(wifi_wr(W_PARF, 0x634, 0x10000000));
    WIFI_TRY(wifi_wr(W_PARF, 0x638, 4));
    WIFI_TRY(wifi_wr(W_PARF, 0x358, 0));
    WIFI_TRY(wifi_wr(W_PARF, 0x35c, 0x80000000));
    WIFI_TRY(wifi_modify(W_PARF, 0x1a8, 1u << 31, 0));
    WIFI_TRY(wifi_modify(W_PARF, 0x1a4, 1, 0));
    WIFI_TRY(wifi_modify(W_PARF, 0x4a0, 1, 0));
    wifi_step = "PCIe PHY sequence";
    WIFI_TRY(wifi_wr(W_PHY, 0x440, 3));
    for (unsigned i = 0; i < sizeof(wifi_phy_seq) / sizeof(wifi_phy_seq[0]); i++) {
        WIFI_TRY(wifi_wr(W_PHY, wifi_phy_seq[i][0], wifi_phy_seq[i][1]));
        if (wifi_phy_seq[i][2]) wifi_sleep(NULL, wifi_phy_seq[i][2]);
    }
    wifi_sleep(NULL, 1000);
    WIFI_TRY(wifi_poll(W_PHY, 0x414, 1u << 7, 0, 100));
    wifi_sleep(NULL, 10000); /* DT qcom,ep-latency=10 ms. */
    WIFI_TRY(wifi_modify(W_DBI, 0x714, 0, 1u << 5));
    WIFI_TRY(wifi_modify(W_DBI, 0x80c, 0x1f00, 1u << 8));
    WIFI_TRY(wifi_rd(W_DBI, 0x7c, &value));
    for (unsigned gen = 3; gen <= (value & 15) && gen <= 4; gen++) {
        WIFI_TRY(wifi_modify(W_DBI, 0x890, 3u << 24, (gen - 3) << 24));
        WIFI_TRY(wifi_modify(W_DBI, 0x8a8, 0xffff00, 0));
        WIFI_TRY(wifi_modify(W_DBI, 0x890, 1, 0));
        WIFI_TRY(wifi_modify(W_DBI, 0x8ac, 31, 0));
    }
    WIFI_TRY(wifi_core_preset(value));
    WIFI_TRY(wifi_modify(W_DBI, 0x9c, 15, 3));
    WIFI_TRY(wifi_rd(W_DBI, 0x78, &value));
    WIFI_TRY(wifi_wr(W_DBI, 0x78, (value & 0x8fff) | 0x5000));
    WIFI_TRY(wifi_modify(W_PARF, 0x2c4, 1, 0));
    WIFI_TRY(wifi_pin(W_PERST, 0, 0, 1, 0, 1));
    wifi_sleep(NULL, 1000);
    wifi_step = "PCIe link training";
    WIFI_TRY(wifi_modify(W_PARF, 0x1b0, 0, 1u << 8));
    WIFI_TRY(wifi_poll(W_DBI, 0xf28, 0x400, 0x400, 100));
    WIFI_TRY(wifi_poll(W_DBI, 0x80, 1u << 29, 1u << 29, 100));
    WIFI_TRY(wifi_rd(W_DBI, 0x80, &value));
    printf("NEZHA WIFI: PCIe Gen%u x%u\n", (value >> 16) & 15, (value >> 20) & 63);
    WIFI_TRY(wifi_modify(W_DBI, 0x80, 0xffff0003u, 0)); /* No ASPM until MHI PM exists. */
    WIFI_TRY(wifi_wr(W_DBI, 0xb40, 20));
    WIFI_TRY(wifi_modify(W_PARF, 0x2c48, 0, 9));
    for (unsigned i = 0; i < 256; i++) WIFI_TRY(wifi_wr(W_PARF, 0x2000 + 4 * i, 0));
    WIFI_TRY(wifi_wr(W_PARF, 0x2000 + 4 * wifi_sid_hash(0x100), 0x01000100));
    WIFI_TRY(wifi_modify(W_PARF, 0x2c00, 1, 0));
    /* CFG0 only bus 1. Region 2 maps the captured 32-bit MEM range. */
    WIFI_TRY(wifi_atu(0, 4, 0x400100000, 0x40010ffff, 0x01000000));
    WIFI_TRY(wifi_atu(2, 0, 0x40300000, 0x43ffffff, 0x40300000));
    wifi_step = "Peach endpoint";
    WIFI_TRY(wifi_rd(W_CFG, 0, &value));
    if (value != 0x110e17cb) return ENODEV;
    WIFI_TRY(wifi_mask_interrupts());
    WIFI_TRY(wifi_rd(W_CFG, 4, &value));
    WIFI_TRY(wifi_wr(W_CFG, 4, value & 0xfff8));
    uint32_t bar, size;
    WIFI_TRY(wifi_rd(W_CFG, 0x10, &bar));
    if ((bar & 7) != 4) return ENOTSUP;
    WIFI_TRY(wifi_wr(W_CFG, 0x10, 0xffffffffu));
    WIFI_TRY(wifi_rd(W_CFG, 0x10, &size));
    WIFI_TRY(wifi_wr(W_CFG, 0x10, bar));
    if ((size & ~15u) != 0xffe00000u) return ENOTSUP;
    WIFI_TRY(wifi_wr(W_CFG, 0x10, 0x40400000 | (bar & 15)));
    WIFI_TRY(wifi_wr(W_CFG, 0x14, 0));
    WIFI_TRY(wifi_rd(W_CFG, 0x18, &bar));
    if (bar & 1) return ENOTSUP;
    WIFI_TRY(wifi_wr(W_CFG, 0x18, 0x40300000 | (bar & 15)));
    if ((bar & 6) == 4) WIFI_TRY(wifi_wr(W_CFG, 0x1c, 0));
    WIFI_TRY(wifi_wr(W_DBI, 0x18, 0x00010100));
    WIFI_TRY(wifi_wr(W_DBI, 0x20, 0x43f04030));
    WIFI_TRY(wifi_wr(W_DBI, 0x24, 0x0000fff0));
    WIFI_TRY(wifi_rd(W_DBI, 4, &value));
    WIFI_TRY(wifi_wr(W_DBI, 4, (value & 0xffff) | 6));
    WIFI_TRY(wifi_rd(W_CFG, 4, &value));
    WIFI_TRY(wifi_wr(W_CFG, 4, (value & 0xfffb) | 2));
    wifi_phase = 3;
    return 0;
}

static int wifi_smmu_read32(void *c, size_t o, uint32_t *v)
{ (void)c; return wifi_rd(W_SMMU, o, v); }
static int wifi_smmu_write32(void *c, size_t o, uint32_t v)
{ (void)c; return wifi_wr(W_SMMU, o, v); }
static int wifi_smmu_read64(void *c, size_t o, uint64_t *v)
{ (void)c; return wifi_access(W_SMMU, o, v, 0, 1); }
static int wifi_smmu_write64(void *c, size_t o, uint64_t v)
{ (void)c; return wifi_access(W_SMMU, o, &v, 1, 1); }
static int wifi_smmu_barrier(void *c)
{ (void)c; __asm__ volatile("dsb sy" ::: "memory"); return wifi_fault ? EIO : 0; }

static vm_offset_t wifi_contig(size_t bytes, uint64_t *pa)
{
    vm_offset_t va = nezha_wifi_allocate_pages(bytes, 1);
    if (!va) return 0;
    *pa = nezha_wifi_physical(va);
    if (!*pa) return 0;
    return va;
}

/* pmap changes every CPU alias and performs the required cache/TLB transition.
 * Use normal NC for shared rings/data, avoiding an assumption that a newly
 * powered endpoint already has snoop attributes configured like Android. */

static int
wifi_dma_locked(void)
{
    uint32_t id0, id1, id2;
    WIFI_TRY(wifi_rd(W_SMMU, 0x20, &id0));
    WIFI_TRY(wifi_rd(W_SMMU, 0x24, &id1));
    WIFI_TRY(wifi_rd(W_SMMU, 0x28, &id2));
    unsigned slots = id0 & 255, banks = id1 & 255, s2banks = (id1 >> 16) & 255;
    uint64_t page = id1 & (1u << 31) ? 0x10000 : 0x1000;
    uint64_t cbbase = page << (((id1 >> 28) & 7) + 1);
    if (!(id0 & 1u << 27) || !slots || !banks || banks > 128 || s2banks >= banks ||
        cbbase + banks * page > wifi_windows[W_SMMU].bytes || id0 & 1u << 8) return ENOTSUP;
    bool used[128] = {false}, asid_used[256] = {false};
    wifi_slot = ~0u;
    for (unsigned s = 0; s < slots; s++) {
        uint32_t smr, route;
        WIFI_TRY(wifi_rd(W_SMMU, 0x800 + s * 4, &smr));
        WIFI_TRY(wifi_rd(W_SMMU, 0xc00 + s * 4, &route));
        if (!(smr & 1u << 31)) { if (wifi_slot == ~0u) wifi_slot = s; continue; }
        if (((0x1401u ^ smr) & ~(smr >> 16) & 0x7fff) == 0) return EBUSY;
        if (((route >> 16) & 3) == 0 && (route & 255) < banks) used[route & 255] = true;
    }
    wifi_bank = ~0u;
    for (unsigned b = s2banks; b < banks; b++) {
        uint32_t ctl;
        WIFI_TRY(wifi_rd(W_SMMU, cbbase + b * page, &ctl));
        if (!(ctl & 1) && !used[b]) { if (wifi_bank == ~0u) wifi_bank = b; continue; }
        if (ctl & 1) {
            uint64_t ttbr;
            WIFI_TRY(wifi_access(W_SMMU, cbbase + b * page + 0x20, &ttbr, 0, 1));
            if ((ttbr >> 48) < 256) asid_used[ttbr >> 48] = true;
        }
    }
    unsigned asid = 1;
    while (asid < 256 && asid_used[asid]) asid++;
    if (wifi_slot == ~0u || wifi_bank == ~0u || asid == 256) return EBUSY;
    wifi_smmu_io = (ArmSmmu4KIo){NULL, wifi_windows[W_SMMU].bytes,
        wifi_smmu_read32, wifi_smmu_read64, wifi_smmu_write32, wifi_smmu_write64, wifi_smmu_barrier};
    /* The endpoint is still in PBL with PCI_COMMAND_MASTER clear. Only a free
     * exact stream slot and a disabled unreferenced stage-1 bank are claimed. */
    WIFI_TRY(wifi_wr(W_SMMU, 0xc00 + wifi_slot * 4, 2u << 16));
    ArmSmmu4KStream route = {wifi_slot, 0x1401};
    if (ArmSmmu4KCheckFaultedStreams(&wifi_smmu_io, &route, 1, wifi_bank,
        &wifi_stream_result) != ArmSmmu4KSuccess) return EBUSY;
    wifi_arena = wifi_contig(WIFI_DMA_BYTES, &wifi_arena_pa);
    wifi_tables = wifi_contig(WIFI_TABLE_BYTES, &wifi_tables_pa);
    if (!wifi_arena || !wifi_tables) return ENOMEM;
    if (nezha_wifi_make_uncached(wifi_arena, WIFI_DMA_BYTES)) return EIO;
    unsigned arena_record;
    WIFI_TRY(NezhaWifiDmaTrack(&wifi_dma_pool, (void *)wifi_arena, WIFI_DMA_BYTES, &arena_record));
    static const uint8_t widths[] = {32, 36, 40, 42, 44, 48};
    unsigned encoding = (id2 >> 4) & 15;
    if (encoding >= sizeof(widths) || (wifi_arena_pa + WIFI_DMA_BYTES) >> widths[encoding] ||
        (wifi_tables_pa + WIFI_TABLE_BYTES) >> widths[encoding]) return ENOTSUP;
    wifi_table_config = (ArmSmmu4KConfig){32, widths[encoding], wifi_tables_pa,
        (WIFI_DMA_LIMIT - WIFI_IOVA) / 0x1000};
    ArmSmmu4KRange range = {WIFI_IOVA, wifi_arena_pa, WIFI_DMA_BYTES,
        ArmSmmu4KRead | ArmSmmu4KWrite, 0, 2};
    if (ArmSmmu4KBuild(&wifi_table_config, &range, 1, (uint64_t *)wifi_tables,
        WIFI_TABLE_BYTES / 0x1000, &wifi_table_result) != ArmSmmu4KSuccess) return ENOTSUP;
    nezha_wifi_clean_tables(wifi_tables_pa, WIFI_TABLE_BYTES);
    wifi_context = (ArmSmmu4KContextConfig){&wifi_table_config, NULL, &wifi_table_result,
        NULL, id0, id2, asid, 0, 0, 0, 0}; /* exact TTBR, no GPU switching policy */
    /* No qcom,actlr entry in the captured apps SMMU DT matches SID 0x1401. */
    WIFI_TRY(wifi_wr(W_SMMU, cbbase + wifi_bank * page + 4, 0));
    uint32_t actlr;
    WIFI_TRY(wifi_rd(W_SMMU, cbbase + wifi_bank * page + 4, &actlr));
    if (actlr) return EIO;
    ArmSmmu4KProgramResult result;
    if (ArmSmmu4KProgramDisabledContext(&wifi_smmu_io, wifi_bank, &wifi_context,
        1000, &result) != ArmSmmu4KSuccess) return EIO;
    if (ArmSmmu4KAttachStreams(&wifi_smmu_io, &wifi_stream, &route, 1, wifi_bank,
        &wifi_context, &wifi_stream_result) != ArmSmmu4KSuccess) return EIO;
    wifi_context_offset = cbbase + wifi_bank * page;
    /* The common programming/attachment helpers run while bus mastering is
     * off. From here the owned bank uses bounded FSR polling, with CFIE clear;
     * do not send faults to an IRQ for which this port has no dispatcher. */
    WIFI_TRY(wifi_rd(W_SMMU, wifi_context_offset, &wifi_context_control));
    wifi_context_control &= ~(1u << 6);
    WIFI_TRY(wifi_wr(W_SMMU, wifi_context_offset, wifi_context_control));
    uint32_t control;
    WIFI_TRY(wifi_rd(W_SMMU, wifi_context_offset, &control));
    if (control != wifi_context_control) return EIO;
    wifi_table_next = (unsigned)wifi_table_result.table_pages;
    wifi_dma_next = WIFI_IOVA + WIFI_DMA_BYTES;
    WIFI_TRY(NezhaWifiDmaPublish(&wifi_dma_pool, arena_record, WIFI_IOVA));
    printf("NEZHA WIFI: SID 0x1401 -> bank %u slot %u ASID %u, IOVA %#llx/%#x\n",
        wifi_bank, wifi_slot, asid, WIFI_IOVA, WIFI_DMA_BYTES);
    wifi_phase = 4;
    return 0;
}

static int
wifi_dma_health(void)
{
    if (!wifi_context_offset) return 0;
    uint64_t now = wifi_now(NULL);
    if (now < wifi_check_at) return wifi_fault ? EIO : 0;
    wifi_check_at = now + 1000;
    uint32_t fsr, control, smr, s2cr;
    WIFI_TRY(wifi_rd(W_SMMU, wifi_context_offset + 0x58, &fsr));
    WIFI_TRY(wifi_rd(W_SMMU, wifi_context_offset, &control));
    WIFI_TRY(wifi_rd(W_SMMU, 0x800 + wifi_slot * 4, &smr));
    WIFI_TRY(wifi_rd(W_SMMU, 0xc00 + wifi_slot * 4, &s2cr));
    if ((fsr & 0xc00001feu) || control != wifi_context_control ||
        smr != 0x80001401u || s2cr != wifi_bank) {
        uint64_t far = 0;
        if (fsr & 0xc00001feu) (void)wifi_access(W_SMMU, wifi_context_offset + 0x60, &far, 0, 1);
        printf("NEZHA WIFI: own DMA fault/change FSR=%#x FAR=%#llx SCTLR=%#x SMR=%#x S2CR=%#x\n",
            fsr, far, control, smr, s2cr);
        wifi_fault = 1;
        return EIO;
    }
    return 0;
}

/* Fresh mappings append only invalid LPAE entries in our owned domain. After
 * completed MHI/PCIe/board shutdown, whole buffers can be reused at exactly
 * their existing CPU/IOVA mapping. No mapping is replaced or unmapped. Initial
 * tables, CAL, unconfirmed generations and partial allocations stay owned.
 * As arm-smmu/io-pgtable-arm, new valid mappings need table clean + publication,
 * not invalidation of other contexts or the shared global SMMU. */
static int
wifi_allocate_dma(void *unused, size_t bytes, PeachMhiDma *out)
{
    (void)unused;
    size_t rounded = round_page(bytes);
    if (!out || !bytes || rounded < bytes || rounded > WIFI_DMA_LIMIT - WIFI_IOVA) return ENOMEM;
    /* Match coherent IOVA allocation alignment. Q6 maps the firmware DDR
     * with large TLB pages; a merely 4K-aligned base exhausts that mapping. */
    uint64_t alignment = PAGE_SIZE;
    while (alignment < rounded) alignment <<= 1;
    NezhaWifiDmaRecord reused;
    int reuse = NezhaWifiDmaTake(&wifi_dma_pool, rounded, alignment, &reused);
    if (!reuse) {
        memset(reused.address, 0, rounded);
        __asm__ volatile("dsb sy" ::: "memory");
        *out = (PeachMhiDma){reused.address, reused.iova, bytes};
        return 0;
    }
    if (reuse != ENOENT) return reuse;
    if (wifi_dma_pool.count >= WIFI_DMA_ALLOCATIONS || wifi_dma_next >= WIFI_DMA_LIMIT ||
        rounded > WIFI_DMA_LIMIT - wifi_dma_next) return ENOMEM;
    uint64_t iova = (wifi_dma_next + alignment - 1) & ~(alignment - 1);
    if (iova >= WIFI_DMA_LIMIT || rounded > WIFI_DMA_LIMIT - iova) return ENOMEM;
    vm_offset_t va = nezha_wifi_allocate_pages(rounded, 0);
    if (!va) return ENOMEM;
    unsigned record;
    WIFI_TRY(NezhaWifiDmaTrack(&wifi_dma_pool, (void *)va, rounded, &record));
    if (nezha_wifi_make_uncached(va, rounded)) return EIO;
    nezha_wifi_domain_lock();
    int err = wifi_dma_health();
    for (size_t off = 0; !err && off < rounded; off += 0x1000) {
        uint64_t address = iova + off;
        uint64_t pa = nezha_wifi_physical(va + off);
        if (!pa || (pa & 0xfff) || pa >> wifi_table_config.output_bits) { err = EIO; break; }
        uint64_t *table = (uint64_t *)wifi_tables;
        for (unsigned level = wifi_table_result.start_level; level < 3; level++) {
            unsigned shift = 39 - 9 * level;
            unsigned width = level == wifi_table_result.start_level ? 32 - shift : 9;
            uint64_t *entry = &table[(address >> shift) & ((1u << width) - 1)];
            if (!*entry) {
                if (wifi_table_next >= WIFI_TABLE_BYTES / 0x1000) { err = ENOMEM; break; }
                *entry = (wifi_tables_pa + 0x1000ull * wifi_table_next++) | 3;
            }
            uint64_t page = *entry & 0xfffffffff000ull;
            if ((*entry & 3) != 3 || page < wifi_tables_pa || page - wifi_tables_pa >= WIFI_TABLE_BYTES) {
                err = EIO; break;
            }
            table = (uint64_t *)(wifi_tables + page - wifi_tables_pa);
        }
        if (err) break;
        uint64_t *leaf = &table[(address >> 12) & 511];
        if (*leaf) { err = EBUSY; break; }
        *leaf = pa | (3ull << 53) | 1u << 11 | 1u << 10 | 2u << 8 | 1u << 6 | 3;
    }
    nezha_wifi_clean_tables(wifi_tables_pa, WIFI_TABLE_BYTES);
    __asm__ volatile("dsb sy" ::: "memory");
    if (!err) err = NezhaWifiDmaPublish(&wifi_dma_pool, record, iova);
    if (!err) {
        *out = (PeachMhiDma){(void *)va, iova, bytes};
        wifi_dma_next = iova + rounded;
    }
    nezha_wifi_domain_unlock();
    return err;
}

static int wifi_dma(void)
{
    wifi_step = "isolated WLAN DMA";
    nezha_wifi_domain_lock();
    int err = wifi_dma_locked();
    nezha_wifi_domain_unlock();
    return err;
}

static PeachMhiDma wifi_dma_part(size_t offset, size_t bytes)
{
    return (PeachMhiDma){(void *)(wifi_arena + offset), wifi_arena_iova + offset, bytes};
}
static int wifi_firmware_read(void *c, uint64_t o, void *p, size_t n)
{ (void)c; return nezha_wifi_firmware_pread("peach/amss20.bin", o, p, n, NULL); }
static int wifi_bar_read(void *c, uint32_t o, uint32_t *v)
{ (void)c; WIFI_TRY(wifi_dma_health()); return wifi_rd(W_BAR, o, v); }
static int wifi_bar_write(void *c, uint32_t o, uint32_t v)
{ (void)c; WIFI_TRY(wifi_dma_health()); return wifi_wr(W_BAR, o, v); }
static void wifi_dma_sync(void *c, const PeachMhiDma *dma, size_t bytes)
{
    (void)c;
    (void)dma; (void)bytes; /* Shared DMA has only normal-NC CPU mappings. */
    __asm__ volatile("dsb sy" ::: "memory");
}
static PeachMhiIo wifi_mhi_io = {NULL, wifi_bar_read, wifi_bar_write, wifi_dma_sync,
    wifi_now, wifi_sleep, 0x200000, WIFI_IOVA, WIFI_DMA_LIMIT - WIFI_IOVA};
static void wifi_barrier(void *c)
{ (void)c; __asm__ volatile("dsb sy" ::: "memory"); }

/* MHI firmware still sends MSI when the host consumes rings by polling.
 * Keep INTx/MSI-X disabled; route MSI writes into an owned normal-NC word,
 * as an IOMMU-mapped MSI message, without targeting an unregistered GIC IRQ. */
static int wifi_enable_poll_msi(void)
{
    uint32_t cap, data, check;
    if (!wifi_msi_cap || !wifi_arena) return ENOTSUP;
    WIFI_TRY(wifi_rd(W_CFG, wifi_msi_cap, &cap));
    if ((cap & 255) != 5 || (cap & 1u << 16) || ((cap >> 17) & 7) != 5) return ENOTSUP;
    unsigned wide = !!(cap & 1u << 23), mask = !!(cap & 1u << 24);
    unsigned data_off = wifi_msi_cap + (wide ? 12 : 8);
    unsigned mask_off = wifi_msi_cap + (wide ? 16 : 12);
    uint32_t address = (uint32_t)(wifi_arena_iova + WIFI_MSI_OFFSET);
    *(volatile uint32_t *)(wifi_arena + WIFI_MSI_OFFSET) = 0;
    wifi_barrier(NULL);
    if (mask) WIFI_TRY(wifi_wr(W_CFG, mask_off, 0xffffffffu));
    WIFI_TRY(wifi_wr(W_CFG, wifi_msi_cap + 4, address));
    if (wide) WIFI_TRY(wifi_wr(W_CFG, wifi_msi_cap + 8, 0));
    WIFI_TRY(wifi_rd(W_CFG, data_off, &data));
    WIFI_TRY(wifi_wr(W_CFG, data_off, data & 0xffff0000u));
    cap = (cap & ~(7u << 20 | 1u << 26)) | 5u << 20 | 1u << 16;
    WIFI_TRY(wifi_wr(W_CFG, wifi_msi_cap, cap));
    WIFI_TRY(wifi_rd(W_CFG, wifi_msi_cap, &check));
    if (check != cap) return EIO;
    WIFI_TRY(wifi_rd(W_CFG, wifi_msi_cap + 4, &check));
    if (check != address) return EIO;
    if (mask) {
        WIFI_TRY(wifi_wr(W_CFG, mask_off, 0));
        WIFI_TRY(wifi_rd(W_CFG, mask_off, &check));
        if (check) return EIO;
    }
    printf("NEZHA WIFI: MSI 32 vectors -> owned IOVA %#x (polled)\n", address);
    return 0;
}

static int wifi_wlfw_file(void *c, const char *name, uint64_t o, void *p, size_t n, uint64_t *size)
{ (void)c; return nezha_wifi_firmware_pread(name, o, p, n, size); }
static void wifi_wlfw_log(void *c, const char *s, uint32_t a, uint32_t b)
{ (void)c; printf("NEZHA WIFI: %s %#x %#x\n", s, a, b); }
static const PeachWlfwOps wifi_wlfw_ops = {NULL, wifi_allocate_dma, wifi_wlfw_file,
    wifi_wlfw_log, WIFI_IOVA, WIFI_IOVA_LIMIT - WIFI_IOVA};

/* Peach cnss2/pci.c: target register window through BAR0+0x80000, selected
 * by BAR0+0x3278. One worker owns this selector; MHI uses only low BAR offsets. */
static int wifi_target_offset(uint32_t target, uint32_t *bar)
{
    if ((target & 3) || target >= 0x2000000) return EINVAL;
    WIFI_TRY(wifi_dma_health());
    if (target < 0x80000) { *bar = target; return 0; }
    uint32_t window = 0x40000000u | ((target >> 19) & 0x3f);
    if (window != wifi_target_window) {
        uint32_t check;
        WIFI_TRY(wifi_wr(W_BAR, 0x3278, window));
        WIFI_TRY(wifi_rd(W_BAR, 0x3278, &check));
        if (check != window) {
            printf("NEZHA WIFI: target window wanted %#x read %#x\n", window, check);
            return EIO;
        }
        wifi_target_window = window;
    }
    *bar = 0x80000 + (target & 0x7ffff);
    return 0;
}
static int wifi_target_read(void *c, uint32_t target, uint32_t *value)
{
    (void)c;
    uint32_t bar;
    WIFI_TRY(wifi_target_offset(target, &bar));
    return wifi_rd(W_BAR, bar, value);
}
static int wifi_target_write(void *c, uint32_t target, uint32_t value)
{
    (void)c;
    uint32_t bar;
    WIFI_TRY(wifi_target_offset(target, &bar));
    WIFI_TRY(wifi_wr(W_BAR, bar, value));
    wifi_barrier(NULL);
    return 0;
}
static int wifi_peer_metadata_diagnostic(void)
{
    int requested = 0;
    (void)PE_parse_boot_argn("nezha_wifi_fw_diag", &requested, sizeof(requested));
    /* The source's SRAM/QMI address was specific to another firmware SHA.
     * No unqualified read or forced firmware crash in this27 port. */
    return requested ? ENOTSUP : 0;
}
static int wifi_packet_receive(void *c, const void *packet, size_t bytes, const PeachRxInfo *info)
{
    (void)c;
    if (wifi_station.security == PeachStationWpa2Psk) {
        if (peach_rsn_receive(&wifi_rsn, packet, bytes, info)) peach_net_receive(packet, bytes);
    } else if (wifi_station.phase == PeachStationOpenAssociated && wifi_tx.authorized &&
               !info->encrypted && !memcmp(info->transmitter, wifi_station.bssid, 6)) {
        const PeachHttPeer *p = peach_htt_peer(&wifi_htt, 0, wifi_station.bssid);
        if (p && p->generation == wifi_station.peer_generation && p->peer == info->peer)
            peach_net_receive(packet, bytes);
    }
    return 0;
}
static int wifi_protocol_progress(void *c)
{
    (void)c;
    int err = peach_wlfw_poll(&wifi_wlfw);
    /* RX only queues EAPOL/copies data. WMI waits may reenter this progress
     * callback; the RSN state machine runs solely in the outer worker. */
    if (!err) err = peach_wmi_poll_management(&wifi_wmi);
    if (!err) err = peach_rx_poll(&wifi_rx, wifi_packet_receive, NULL);
    if (!err) err = peach_tx_poll(&wifi_tx);
    /* AP rejection/deauthentication is connection state, not an HTC/CE
     * failure. The outer worker owns recovery and reports it to iOS. */
    if (!err) (void)peach_station_poll(&wifi_station);
    /* Passive scans wait on this callback. Continue draining authorized
     * BSD traffic while scanning instead of stalling DHCP/TCP until it ends. */
    if (!err && !wifi_station.error && wifi_tx.authorized)
        err = peach_net_transmit(&wifi_tx);
    return err;
}
static int wifi_service_receive(void *c, unsigned service, const void *p, size_t n)
{
    (void)c;
    int err;
    uint32_t message;
    if (service == 0x100) {
        peach_wmi_receive(&wifi_wmi, p, n);
        err = wifi_wmi.error; message = wifi_wmi.last_event;
    } else if (service == 0x300) {
        peach_htt_receive(&wifi_htt, p, n);
        err = wifi_htt.error; message = wifi_htt.last_message;
    } else return PeachCeProtocol;
    (void)message; /* protocol parser retains bounded status; no raw packet dump */
    return err;
}
static const PeachCeOps wifi_ce_ops = {NULL, wifi_target_read, wifi_target_write,
    wifi_barrier, peach_htc_receive, &wifi_htc};
static const PeachHtcOps wifi_htc_ops = {NULL, wifi_protocol_progress, wifi_now,
    wifi_sleep, wifi_wlfw_log, wifi_service_receive};
static const PeachWmiOps wifi_wmi_ops = {NULL, wifi_allocate_dma, wifi_barrier,
    wifi_wlfw_log, WIFI_IOVA, WIFI_DMA_LIMIT - WIFI_IOVA};
static int wifi_srng_health(void *c) { (void)c; return wifi_dma_health(); }
static const PeachSrngOps wifi_srng_ops = {NULL, wifi_target_read, wifi_target_write,
    wifi_srng_health, wifi_barrier};
static int wifi_prepare_srng(void)
{
    /* Retained ring pool plus link/queue/packet memory. Prepare REO before
     * enabling rings; RX buffers are posted after the HTT layout command. */
    const unsigned entries[PeachSrngKinds] = {512, 128, 128, 512, 512, 128, 1024, 512, 128};
    size_t total = 0;
    for (unsigned i = 0; i < PeachSrngKinds; i++) total += peach_srng_bytes(i, entries[i]);
    PeachMhiDma memory;
    WIFI_TRY(wifi_allocate_dma(NULL, total, &memory));
    size_t offset = 0;
    for (unsigned i = 0; i < PeachSrngKinds; i++) {
        size_t bytes = peach_srng_bytes(i, entries[i]);
        PeachMhiDma part = {(unsigned char *)memory.bytes + offset, memory.iova + offset, bytes};
        WIFI_TRY(peach_srng_prepare(&wifi_srng[i], i, entries[i], part, &wifi_srng_ops));
        offset += bytes;
    }
    PeachMhiDma rx_memory;
    WIFI_TRY(wifi_allocate_dma(NULL, PEACH_RX_DMA_BYTES, &rx_memory));
    int rx_err = peach_rx_prepare(&wifi_rx, wifi_srng, rx_memory);
    if (rx_err) {
        printf("NEZHA WIFI: REO prepare failed %d at %#x value %#x\n", rx_err,
            wifi_rx.last_register, wifi_rx.last_value);
        return EIO;
    }
    PeachMhiDma tx_memory;
    WIFI_TRY(wifi_allocate_dma(NULL, PEACH_TX_DMA_BYTES, &tx_memory));
    WIFI_TRY(peach_tx_prepare(&wifi_tx, wifi_srng, &wifi_htt, tx_memory,
        wifi_wmi.radio.lmac, wifi_wmi.radio.pdev));
    /* dp_cmn_device_init precedes core_start/WMI INIT in the working
     * driver. That includes TCL, WBM completions and the seeded idle-link
     * pool, not only REO destinations. HTT configures LMAC rings later. */
    for (unsigned i = 0; i < PeachSrngKinds; i++) {
        if (wifi_srng[i].lmac) continue;
        WIFI_TRY(peach_srng_start_host(&wifi_srng[i]));
    }
    WIFI_TRY(peach_srng_seed_idle(&wifi_srng[PeachSrngIdleLink], wifi_rx.links, PEACH_RX_LINKS));
    printf("NEZHA WIFI: host REO/TCL/WBM rings and idle pool ready before WMI INIT\n");
    return 0;
}
/* Android's actual HTT stream configures HOST1/SW-to-SW and, separately,
 * RXDMA_HOST_BUF/SW-to-HW for each active PHY. The firmware moves
 * buffers between them. These intermediate rings remain firmware-owned;
 * the host neither seeds them nor consumes their entries. */
static int wifi_setup_rxmac(void)
{
    enum { entries = 4096, bytes = entries * 8 + 128 };
    /* Regulatory capabilities also describe inactive PHYs. The selected
     * SINGLE mode has exactly one PHY; do not configure the DBS sibling. */
    wifi_rxmac_count = 1;
    PeachMhiDma memory;
    WIFI_TRY(wifi_allocate_dma(NULL, bytes * wifi_rxmac_count, &memory));
    memset(memory.bytes, 0, bytes * wifi_rxmac_count);
    for (unsigned i = 0; i < wifi_rxmac_count; i++) {
        unsigned char *p = (unsigned char *)memory.bytes + i * bytes;
        uint64_t base = memory.iova + i * bytes;
        wifi_rxmac[i] = (PeachHttRing){
            {p, base, entries * 8}, {p + entries * 8, base + entries * 8, 4},
            {p + entries * 8 + 64, base + entries * 8 + 64, 4},
            i + 1, 0, PeachHttSwToHw, entries, 2
        };
        WIFI_TRY(peach_htt_setup_ring(&wifi_htt, &wifi_rxmac[i]));
    }
    printf("NEZHA WIFI: %u firmware-owned RXMAC ring setups submitted\n", wifi_rxmac_count);
    return 0;
}
static int wifi_setup_srng(void)
{
    int tx_err = peach_tx_start(&wifi_tx);
    if (tx_err) {
        printf("NEZHA WIFI: TX prepare failed %d\n", tx_err);
        return EIO;
    }
    /* Vendor QCA_HOST2FW_RXBUF_RING path: HOST1_TO_FW is software ring 5
     * in the single host-MAC slot 0. RXDMA destination uses target PDEV. */
    int err = peach_srng_start_lmac(&wifi_srng[PeachSrngRxBuffer], &wifi_htt,
        0, 5, PeachHttSwToSw);
    if (!err) err = wifi_setup_rxmac();
    if (!err) err = peach_srng_start_lmac(&wifi_srng[PeachSrngRxException], &wifi_htt,
        wifi_wmi.radio.pdev, 7, PeachHttHwToSw);
    if (err) {
        printf("NEZHA WIFI: HTT SRING_SETUP failed %d, pdev %u ring %u status %#x message %#x\n",
            err, wifi_htt.setup_pdev, wifi_htt.setup_id, wifi_htt.setup_status, wifi_htt.last_message);
        return EIO;
    }
    err = peach_rx_start(&wifi_rx, &wifi_htt, wifi_wmi.radio.pdev, wifi_wmi.peer_metadata_version);
    if (err) {
        printf("NEZHA WIFI: RX start failed %d, retained in-flight %u\n", err, wifi_rx.in_flight);
        return EIO;
    }
    return 0;
}

static int
wifi_stage_firmware(void)
{
    uint64_t bytes = 0;
    wifi_step = "stage Peach firmware";
    WIFI_TRY(nezha_wifi_firmware_pread("peach/amss20.bin", 0, NULL, 0, &bytes));
    if (bytes <= PEACH_MHI_SEGMENT_BYTES) return EINVAL;
    uint64_t amss = bytes - PEACH_MHI_SEGMENT_BYTES;
    unsigned count = (unsigned)((amss + PEACH_MHI_SEGMENT_BYTES - 1) / PEACH_MHI_SEGMENT_BYTES);
    if (count > PEACH_MHI_MAX_SEGMENTS || (count + 1ull) * PEACH_MHI_SEGMENT_BYTES > 0x800000) return EFBIG;
    wifi_firmware.sbl = wifi_dma_part(0, PEACH_MHI_SEGMENT_BYTES);
    for (unsigned i = 0; i < count; i++)
        wifi_firmware.amss[i] = wifi_dma_part((i + 1) * PEACH_MHI_SEGMENT_BYTES, PEACH_MHI_SEGMENT_BYTES);
    wifi_firmware.vectors = wifi_dma_part(0x800000, 0x1000);
    int err = peach_mhi_firmware_stage(&wifi_firmware, wifi_firmware_read, NULL, bytes);
    printf("NEZHA WIFI: firmware stage %d, %llu bytes\n", err, bytes);
    if (err) return EINVAL;
    wifi_phase = 5;
    return 0;
}

static int
wifi_rddm_reason(void)
{
    /* Retain the 7.5 MiB capacity from the existing local 067/068 path.
     * Capacity is not the firmware-declared payload length; the diagnostic
     * publisher validates those separately. Never reuse after a crash. */
    const size_t bytes = NEZHA_WIFI_RDDM_BYTES;
    if (!wifi_rddm.bytes || !wifi_rddm_session) return ENOENT;
    wifi_check_at = 0;
    WIFI_TRY(wifi_dma_health());
    int err = peach_mhi_read_rddm(&wifi_firmware, &wifi_mhi_io, wifi_rddm_session);
    printf("NEZHA WIFI: RDDM result %d status %#x\n", err, wifi_firmware.last_status);
    if (err) return EIO;
    wifi_barrier(NULL);
    wifi_check_at = 0;
    WIFI_TRY(wifi_dma_health());
    int saved = nezha_wifi_diagnostic_dump(wifi_rddm.bytes, bytes);
    printf("NEZHA WIFI: RDDM DMA buffer retention result %d\n", saved);
    /* cnss2 rddm_header: two u32s followed by at most twelve 64-byte entries.
     * File contents follow the header in table order, not at target PAs. */
    uint32_t header[2];
    memcpy(header, wifi_rddm.bytes, sizeof(header));
    printf("NEZHA WIFI: RDDM version %u header %#x\n", header[0], header[1]);
    if (header[1] < 8 || header[1] > 8 + 12 * 64 || (header[1] - 8) % 64) return EINVAL;
    size_t offset = header[1];
    for (unsigned i = 0; i < (header[1] - 8) / 64; i++) {
        const unsigned char *entry = (unsigned char *)wifi_rddm.bytes + 8 + i * 64;
        uint64_t size;
        memcpy(&size, entry + 16, sizeof(size));
        if (size > bytes - offset) return EINVAL;
        printf("NEZHA WIFI: RDDM file %.20s size %#llx offset %#lx (%.20s)\n",
            entry + 44, size, offset, entry + 24);
        if (!memcmp(entry + 44, "Q6-SFR.bin", 11)) {
            unsigned length = size > 1024 ? 1024 : (unsigned)size;
            printf("NEZHA WIFI: firmware reason %.*s\n", length, (char *)wifi_rddm.bytes + offset);
        }
        offset += (size_t)size;
    }
    return ENOENT;
}

static void wifi_mhi_failure(void)
{
    if (wifi_phase < 6 || wifi_fault) return;
    printf("NEZHA WIFI: layer errors CE/HTC/WMI/HTT/RX/TX %d/%d/%d/%d/%d/%d, events WMI %#x HTT %#x\n",
        wifi_ce.error, wifi_htc.error, wifi_wmi.error, wifi_htt.error,
        wifi_rx.error, wifi_tx.error, wifi_wmi.last_event, wifi_htt.last_message);
    if (wifi_tx.prepared && wifi_tx.ring->started && wifi_tx.completion->started) {
        wifi_barrier(NULL);
        printf("NEZHA WIFI: TX TCL published/consumed DWORDs %u/%u WBM head/tail %u/%u in-flight %u\n",
            wifi_tx.ring->wp * wifi_tx.ring->dwords,
            *(volatile uint32_t *)wifi_tx.ring->tail.bytes,
            *(volatile uint32_t *)wifi_tx.completion->head.bytes,
            wifi_tx.completion->rp * wifi_tx.completion->dwords, wifi_tx.in_flight);
    }
    if (wifi_tx.error) {
        const uint32_t *d = wifi_tx.last_descriptor;
        printf("NEZHA WIFI: TX last completion DWORDs0..1 %#x %#x\n", d[0], d[1]);
        printf("NEZHA WIFI: TX last completion DWORDs2..7 %#x %#x %#x %#x %#x %#x\n",
            d[2], d[3], d[4], d[5], d[6], d[7]);
    }
    if (wifi_rx.error) {
        const uint32_t *d = wifi_rx.last_descriptor;
        printf("NEZHA WIFI: RX last register/value %#x %#x descriptor0..3 %#x %#x %#x %#x\n",
            wifi_rx.last_register, wifi_rx.last_value, d[0], d[1], d[2], d[3]);
        printf("NEZHA WIFI: RX last descriptor4..7 %#x %#x %#x %#x\n",
            d[4], d[5], d[6], d[7]);
    }
    printf("NEZHA WIFI: last MSI data %#x\n",
        *(volatile uint32_t *)(wifi_arena + WIFI_MSI_OFFSET));
    uint32_t status = 0, debug[6] = {0};
    (void)wifi_rd(W_BAR, 0x48, &status);
    printf("NEZHA WIFI: MHI state %u EE %u status %#x ring %#llx event %#llx %#x %#x\n",
        wifi_host.state, wifi_host.environment, status,
        wifi_host.last_ring_pointer, wifi_host.last_event_pointer,
        wifi_host.last_event_data[0], wifi_host.last_event_data[1]);
    for (unsigned i = 0; i < 6; i++)
        if (wifi_rd(W_BAR, wifi_firmware.bhi + 0x28 + i * 4, &debug[i])) return;
    printf("NEZHA WIFI: BHI EE/status/error/debug %#x %#x %#x %#x %#x %#x, TX %u/%u RX %u/%u\n",
        debug[0], debug[1], debug[2], debug[3], debug[4], debug[5],
        wifi_host.tx_rp, wifi_host.tx_wp, wifi_host.rx_rp, wifi_host.rx_wp);
    if (wifi_host.memory.bytes) for (unsigned ch = 20; ch <= 21; ch++) {
        uint32_t ctx[11], tre[4] = {0};
        memcpy(ctx, (unsigned char *)wifi_host.memory.bytes + ch * 44, sizeof(ctx));
        if (ctx[3] >= wifi_host.memory.iova &&
            ctx[3] - wifi_host.memory.iova <= wifi_host.memory.capacity - sizeof(tre))
            memcpy(tre, (unsigned char *)wifi_host.memory.bytes + ctx[3] - wifi_host.memory.iova, sizeof(tre));
        printf("NEZHA WIFI: MHI ch%u cfg/type/er %#x/%#x/%#x base/len/rp/wp %#x/%#x/%#x/%#x first TRE %#x:%#x %#x %#x\n",
            ch, ctx[0], ctx[1], ctx[2], ctx[3], ctx[5], ctx[7], ctx[9],
            tre[1], tre[0], tre[2], tre[3]);
    }
    if (debug[0] == 3) (void)wifi_rddm_reason();
}

static int
wifi_boot_firmware(PeachMhiDma calibration)
{
    /* Only entered after initial hardware reset, or after CAL_DONE + OFF
     * and a second checked PERST/WLAN_EN/PCIe reset. Keep every DMA mapping. */
    wifi_firmware.phase = 0;
    WIFI_TRY(wifi_stage_firmware());
    wifi_step = "configure polled MSI delivery";
    WIFI_TRY(wifi_enable_poll_msi());
    wifi_step = "BHI SBL download";
    uint32_t command, session;
    read_random(&session, sizeof(session));
    session = (session & 0x3fffffffu) | 1;
    WIFI_TRY(wifi_rd(W_CFG, 4, &command));
    WIFI_TRY(wifi_wr(W_CFG, 4, (command & 0xffff) | 6));
    int err = peach_mhi_download_sbl(&wifi_firmware, &wifi_mhi_io, session);
    if (err) {
        printf("NEZHA WIFI: BHI result %d, status %#x\n", err, wifi_firmware.last_status);
        return EIO;
    }
    wifi_phase = 6;
    wifi_step = "MHI READY/M0";
    err = peach_mhi_host_start(&wifi_host, &wifi_mhi_io,
        wifi_dma_part(0x810000, PEACH_MHI_HOST_BYTES), wifi_barrier);
    if (err) return EIO;
    WIFI_TRY(wifi_poll(W_BAR, wifi_firmware.bhi + 0x28, 0xffffffffu, 1, 10000));
    wifi_step = "prearm firmware crash reason";
    if (!wifi_rddm.bytes) WIFI_TRY(wifi_allocate_dma(NULL, NEZHA_WIFI_RDDM_BYTES + 0x1000, &wifi_rddm));
    PeachMhiDma dump = {wifi_rddm.bytes, wifi_rddm.iova, NEZHA_WIFI_RDDM_BYTES};
    PeachMhiDma vectors = {(unsigned char *)wifi_rddm.bytes + NEZHA_WIFI_RDDM_BYTES,
        wifi_rddm.iova + NEZHA_WIFI_RDDM_BYTES, 0x1000};
    read_random(&wifi_rddm_session, sizeof(wifi_rddm_session));
    wifi_rddm_session = (wifi_rddm_session & 0x3fffffffu) | 1;
    err = peach_mhi_prepare_rddm(&wifi_firmware, &wifi_mhi_io, dump, vectors, wifi_rddm_session);
    if (err) {
        printf("NEZHA WIFI: RDDM prepare failed: %d\n", err);
        return EIO;
    }
    wifi_phase = 7;
    wifi_step = "BHIe AMSS download";
    read_random(&session, sizeof(session));
    session = (session & 0x3fffffffu) | 1;
    err = peach_mhi_download_amss(&wifi_firmware, &wifi_mhi_io, session);
    if (err) {
        printf("NEZHA WIFI: BHIe result %d, status %#x\n", err, wifi_firmware.last_status);
        return EIO;
    }
    WIFI_TRY(wifi_poll(W_BAR, wifi_firmware.bhi + 0x28, 0xffffffffu, 2, 10000));
    wifi_step = "MHI IPCR channels";
    if (peach_wlfw_init(&wifi_wlfw, &wifi_host, &wifi_wlfw_ops)) return EIO;
    wifi_wlfw.calibration = calibration;
    wifi_wlfw.calibration_done = calibration.bytes != NULL;
    err = peach_mhi_host_open_ipcr(&wifi_host);
    if (err) {
        printf("NEZHA WIFI: MHI IPCR open failed: %d\n", err);
        return EIO;
    }
    wifi_phase = 8;
    wifi_step = "QRTR/WLFW bootstrap";
    err = peach_wlfw_bootstrap(&wifi_wlfw);
    if (err) {
        printf("NEZHA WIFI: WLFW phase %u failed: %d\n", wifi_wlfw.phase, err);
        return EIO;
    }
    wifi_phase = 9;
    return 0;
}

static int wifi_assert_radio_reset(void *unused)
{
    (void)unused;
    WIFI_TRY(wifi_pin(W_PERST, 0, 0, 1, 0, 0));
    WIFI_TRY(wifi_pin(W_EN, 0, 0, 1, 0, 0));
    uint32_t reset, enable;
    WIFI_TRY(wifi_rd(W_PERST, 4, &reset)); WIFI_TRY(wifi_rd(W_EN, 4, &enable));
    if ((reset | enable) & 2) return EBUSY;
    wifi_radio_off = 1; return 0;
}
static int wifi_stop_read(void *unused, unsigned reg, uint32_t *value)
{
    (void)unused;
    switch (reg) {
    case NezhaWifiStopMhiControl: return wifi_rd(W_BAR, 0x38, value);
    case NezhaWifiStopMhiStatus: return wifi_rd(W_BAR, 0x48, value);
    case NezhaWifiStopExecEnv: return wifi_rd(W_BAR, wifi_firmware.bhi + 0x28, value);
    case NezhaWifiStopPciCommand: return wifi_rd(W_CFG, 4, value);
    case NezhaWifiStopPmStatus: return wifi_rd(W_PARF, 0x28, value);
    default: return EINVAL;
    }
}
static int wifi_stop_write(void *unused, unsigned reg, uint32_t value)
{
    (void)unused;
    switch (reg) {
    case NezhaWifiStopMhiControl: return wifi_wr(W_BAR, 0x38, value);
    /* Captured live DT: ELBI is DBI + 0xf20; SYS_CTRL is +4. */
    case NezhaWifiStopElbiControl: return wifi_wr(W_DBI, 0xf24, value);
    default: return EINVAL;
    }
}
static int wifi_stop_mask(void *unused)
{ (void)unused; return wifi_mask_interrupts(); }
static int wifi_disable_radio(void)
{
    memset(&wifi_stop_proof, 0, sizeof(wifi_stop_proof));
    if (wifi_fault || wifi_host.poisoned) return EIO;
    int err = EBUSY;
    if (wifi_wmi.ready && wifi_host.ipcr_up && !wifi_host.poisoned) {
        const NezhaWifiStopOps ops = {NULL, wifi_stop_read, wifi_stop_write,
            wifi_stop_mask, wifi_assert_radio_reset, wifi_now, wifi_sleep};
        err = NezhaWifiStopDma(&ops, &wifi_stop_proof, 1000000);
        if (wifi_stop_proof.l2_requested) wifi_l2_requested = 1;
    }
    printf("NEZHA WIFI: DMA stop phase %u result %d reset %#x EE %u command %#x PM %#x complete %u\n",
        wifi_stop_proof.phase, err, wifi_stop_proof.control, wifi_stop_proof.environment,
        wifi_stop_proof.command, wifi_stop_proof.pm_status, wifi_stop_proof.completed);
    if (!err) return 0;
    /* A failed device access is handled only by the existing PERST containment
     * path. Incomplete reset/L2 never authorizes buffer reuse. */
    if (wifi_fault || wifi_stop_proof.environment == 3) return err;
    if (wifi_stop_proof.phase < 5) WIFI_TRY(wifi_mask_interrupts());
    WIFI_TRY(wifi_assert_radio_reset(NULL));
    return 0;
}
static int wifi_reset_endpoint(void)
{
    /* A prior successful radio-off leaves config space inaccessible. It has
     * already disabled DMA; use only the always-on GPIOs until power is back. */
    if (!wifi_radio_off) {
        uint32_t command, check;
        WIFI_TRY(wifi_rd(W_CFG, 4, &command));
        WIFI_TRY(wifi_wr(W_CFG, 4, command & 0xfffbu));
        WIFI_TRY(wifi_rd(W_CFG, 4, &check));
        if (check & 4) return EBUSY;
        WIFI_TRY(wifi_pin(W_PERST, 0, 0, 1, 0, 0));
        WIFI_TRY(wifi_rd(W_PERST, 4, &check));
        if (check & 2) return EBUSY;
    }
    wifi_sleep(NULL, 100000);
    WIFI_TRY(wifi_power());
    WIFI_TRY(wifi_pcie_resources());
    if (wifi_l2_requested) {
        WIFI_TRY(wifi_wr(W_DBI, 0xf24, 0));
        uint32_t control;
        WIFI_TRY(wifi_rd(W_DBI, 0xf24, &control));
        if (control & (1u << 4)) return EBUSY;
        wifi_l2_requested = 0;
    }
    WIFI_TRY(wifi_pcie_link());
    WIFI_TRY(wifi_dma_health());
    wifi_target_window = 0xffffffffu;
    wifi_radio_off = 0;
    return 0;
}

static int wifi_mission_start(PeachMhiDma calibration)
{
    int err;
    WIFI_TRY(wifi_boot_firmware(calibration));
    wifi_step = "prepare CE DMA";
    PeachMhiDma ce_dma;
    WIFI_TRY(wifi_allocate_dma(NULL, PEACH_CE_DMA_BYTES, &ce_dma));
    if (peach_ce_prepare(&wifi_ce, ce_dma, &wifi_ce_ops) ||
        peach_htc_init(&wifi_htc, &wifi_ce, &wifi_htc_ops) ||
        peach_htt_init(&wifi_htt, &wifi_htc) ||
        peach_wmi_init(&wifi_wmi, &wifi_htc, &wifi_wmi_ops)) return EIO;
    wifi_step = "WLFW WLAN_CFG/MISSION";
    DTEntry chosen;
    const void *mac = NULL;
    unsigned mac_bytes = 0;
    if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
        SecureDTGetProperty(chosen, "nezha-wifi-mac", &mac, &mac_bytes) != kSuccess ||
        mac_bytes != 6) return EINVAL;
    if (peach_wlfw_wlan_enable(&wifi_wlfw, mac)) return EIO;
    wifi_phase = 10;
    wifi_step = "Peach CE rings";
    err = peach_ce_start(&wifi_ce);
    if (err) {
        printf("NEZHA WIFI: CE setup failed %d at %#x value %#x\n", err,
            wifi_ce.last_register, wifi_ce.last_value);
        return EIO;
    }
    wifi_phase = 11;
    wifi_step = "HTC READY and WMI/HTT connect";
    err = peach_htc_bootstrap(&wifi_htc);
    if (err) {
        printf("NEZHA WIFI: HTC failed: %d, CE %d at %#x value %#x\n", err, wifi_ce.error,
            wifi_ce.last_register, wifi_ce.last_value);
        return EIO;
    }
    wifi_phase = 12;
    wifi_step = "WMI services and PHY capabilities";
    err = peach_wmi_wait_services(&wifi_wmi);
    if (err) {
        printf("NEZHA WIFI: WMI capabilities failed: %d event %#x tag %u\n", err,
            wifi_wmi.last_event, wifi_wmi.last_tag);
        return EIO;
    }
    wifi_step = "host REO setup before WMI INIT";
    WIFI_TRY(wifi_prepare_srng());
    wifi_step = "WMI SERVICE_READY/INIT/READY";
    err = peach_wmi_bootstrap(&wifi_wmi);
    if (err) {
        printf("NEZHA WIFI: WMI phase %u failed: %d event %#x tag %u\n", wifi_wmi.phase, err,
            wifi_wmi.last_event, wifi_wmi.last_tag);
        return EIO;
    }
    WIFI_TRY(wifi_peer_metadata_diagnostic());
    wifi_phase = 13;
    wifi_step = "HTT version negotiation";
    err = peach_htt_negotiate(&wifi_htt);
    if (err) {
        printf("NEZHA WIFI: HTT failed: %d\n", err);
        return EIO;
    }
    wifi_phase = 14;
    /* Actual Android order: management refill precedes the data/RXDMA rings. */
    wifi_step = "management RX refill";
    WIFI_TRY(peach_wmi_start_management(&wifi_wmi, &wifi_htt));
    wifi_step = "Peach UMAC/LMAC SRNG setup";
    WIFI_TRY(wifi_setup_srng());
    if (peach_station_init(&wifi_station, &wifi_wmi, &wifi_htt, &wifi_rx, &wifi_tx)) return EIO;
    wifi_phase = 15;
    wifi_step = "regulatory rules and passive scan";
    err = peach_wmi_passive_scan(&wifi_wmi);
    if (err && err != PeachWmiScanFailed) {
        printf("NEZHA WIFI: passive scan failed %d, event %#x reason %u started %u\n",
            err, wifi_wmi.scan_event, wifi_wmi.scan_reason, wifi_wmi.scan_started);
        return EIO;
    }
    wifi_phase = 16;
    int capture = 0;
    (void)PE_parse_boot_argn("nezha_wifi_dump", &capture, sizeof(capture));
    if (capture == 1) {
        wifi_barrier(NULL);
        printf("NEZHA WIFI: RX refill head/tail %#x/%#x, owned %u received %llu discarded %llu released %llu\n",
            *(volatile uint32_t *)wifi_srng[PeachSrngRxBuffer].head.bytes,
            *(volatile uint32_t *)wifi_srng[PeachSrngRxBuffer].tail.bytes,
            wifi_rx.in_flight, wifi_rx.received, wifi_rx.discarded, wifi_rx.released);
        for (unsigned i = 0; i < wifi_rxmac_count; i++)
            printf("NEZHA WIFI: RXMAC pdev %u head/tail %#x/%#x\n", i + 1,
                *(volatile uint32_t *)wifi_rxmac[i].head.bytes,
                *(volatile uint32_t *)wifi_rxmac[i].tail.bytes);
        printf("NEZHA WIFI: management RX received %llu discarded %llu tail %#x\n",
            wifi_wmi.mgmt_rx_received, wifi_wmi.mgmt_rx_discarded,
            wifi_wmi.mgmt_rx_dma.bytes ? *(volatile uint32_t *)
                ((unsigned char *)wifi_wmi.mgmt_rx_dma.bytes + 1088) : 0);
        WIFI_TRY(peach_wmi_request_radio_stats(&wifi_wmi));
    }
    if (err == PeachWmiScanFailed) {
        printf("NEZHA WIFI: firmware ended initial scan, event %#x reason %u; explicit retry available\n",
            wifi_wmi.scan_event, wifi_wmi.scan_reason);
        return 0;
    }
    printf("NEZHA WIFI: passive scan completed with %u BSS; awaiting explicit network selection\n",
        wifi_wmi.bss_count);
    return 0;
}

static int wifi_start(void)
{
    for (unsigned i = 0; i < W_NWIN; i++) WIFI_TRY(wifi_map(i));
    WIFI_TRY(wifi_power());
    WIFI_TRY(wifi_pcie_resources());
    WIFI_TRY(wifi_pcie_link());
    WIFI_TRY(wifi_dma());
    WIFI_TRY(wifi_boot_firmware((PeachMhiDma){0}));
    wifi_step = "cold boot calibration";
    int err = peach_wlfw_calibrate(&wifi_wlfw);
    if (err) {
        printf("NEZHA WIFI: calibration failed %d, QMI %#x remote %u\n", err,
            wifi_wlfw.message, wifi_wlfw.remote_error);
        return EIO;
    }
    PeachMhiDma calibration = wifi_wlfw.calibration;
    WIFI_TRY(wifi_reset_endpoint());
    printf("NEZHA WIFI: endpoint reset after calibration; CAL DMA retained\n");
    return wifi_mission_start(calibration);
}

static int wifi_restart_mission(PeachMhiDma calibration);
static unsigned wifi_select_bss(const PeachControlJoinData *j)
{
    unsigned selected = PEACH_WMI_BSS_ENTRIES;
    for (unsigned i = 0; i < wifi_wmi.bss_count; i++) {
        const PeachWmiBss *b = &wifi_wmi.bss[i];
        if (b->ssid_bytes != j->ssid_bytes || memcmp(b->ssid, j->ssid, j->ssid_bytes) ||
            (j->bssid_valid && memcmp(b->bssid, j->bssid, 6))) continue;
        int rejected = peach_station_candidate(b, j->security);
        if (rejected) {
            printf("NEZHA WIFI: candidate rejected %d MHz %u capability %#x DTIM %u IE bytes/truncated %u/%u\n",
                rejected, b->frequency, b->capability, b->dtim_period, b->ie_bytes, b->ies_truncated);
            continue;
        }
        if (selected == PEACH_WMI_BSS_ENTRIES || b->rssi > wifi_wmi.bss[selected].rssi) selected = i;
    }
    return selected;
}
static int wifi_connection_request(void)
{
    if (!peach_net_take_request(&wifi_request)) return 0;
    int err = 0;
    if (wifi_request.action == PeachControlPower) {
        if (!wifi_request.radio_power) {
            /* Open networks have no RSN owner, so gate TX explicitly too. */
            (void)peach_tx_authorize(&wifi_tx, 0);
            peach_net_leave(); peach_rsn_clear(&wifi_rsn);
            if (!wifi_radio_off) err = wifi_disable_radio();
            wifi_net_state = err ? PeachNetFailed : PeachNetOff;
            wifi_step = "WLAN_EN/PERST asserted; radio off, DMA retained";
        } else if (wifi_radio_off) {
            err = wifi_restart_mission(wifi_wlfw.calibration);
            wifi_net_state = err ? PeachNetFailed : PeachNetReady;
            if (!err) peach_net_status(&wifi_wmi, wifi_net_state, 0);
        }
        peach_net_power_complete(wifi_request.radio_power, err);
        nezha_wifi_auto_radio_complete(wifi_request.radio_power, err);
        goto clear_request;
    }
    if (wifi_request.action == PeachControlScan) {
        wifi_step = "requested passive scan";
        int err = peach_wmi_passive_scan(&wifi_wmi);
        if (err == PeachWmiScanFailed) {
            peach_net_status(&wifi_wmi, wifi_net_state, EAGAIN);
            return 0;
        }
        if (!err) peach_net_status(&wifi_wmi, wifi_net_state, 0);
        return err;
    }
    if (wifi_request.action == PeachControlJoin)
        printf("NEZHA WIFI: manual join SSID bytes %u security %u\n",
            wifi_request.join.ssid_bytes, wifi_request.join.security);
    if (wifi_tx.bound) {
        /* Reset the actual endpoint to retire keyed peers. A completed bus
         * stop can recycle owned mappings; unconfirmed stops retain old DMA. */
        printf("NEZHA WIFI: manual leave/switch generation %u RX %llu/%llu TX %llu/%llu/%llu\n",
            wifi_generations + 1, wifi_rx.received, wifi_rx.discarded,
            wifi_tx.submitted, wifi_tx.acked, wifi_tx.dropped);
        wifi_net_state = PeachNetReady;
        peach_net_leave(); peach_rsn_clear(&wifi_rsn);
        unsigned char mac[6]; memcpy(mac, wifi_wmi.mac, sizeof(mac));
        err = wifi_restart_mission(wifi_wlfw.calibration);
        if (!err && memcmp(mac, wifi_wmi.mac, sizeof(mac))) err = EIO;
        if (err) goto clear_request;
    }
    if (wifi_request.action == PeachControlLeave) {
        wifi_request.action = 0;
        peach_net_status(&wifi_wmi, PeachNetReady, 0);
        printf("NEZHA WIFI: manual leave complete; scan ready\n");
        goto clear_request;
    }
    peach_net_status(&wifi_wmi, PeachNetAssociating, 0);
    PeachControlJoinData *j = &wifi_request.join;
    unsigned selected = wifi_select_bss(j);
    if (selected == PEACH_WMI_BSS_ENTRIES) {
        /* A manual selection may have fallen out of a later full scan.
         * One host-filtered passive sweep gives that selected AP a slot. */
        wifi_step = "passive rescan for manual selection";
        err = peach_wmi_passive_scan_target(&wifi_wmi, j->ssid, j->ssid_bytes,
            j->bssid_valid ? j->bssid : NULL);
        if (err == PeachWmiScanFailed) err = 0;
        if (err) goto clear_request;
        selected = wifi_select_bss(j);
    }
    if (selected == PEACH_WMI_BSS_ENTRIES) {
        printf("NEZHA WIFI: no usable candidate in %u BSS, scan dropped %u\n",
            wifi_wmi.bss_count, wifi_wmi.bss_dropped);
        peach_net_status(&wifi_wmi, PeachNetReady, ENOENT);
    }
    else {
        peach_net_selection(&wifi_wmi.bss[selected], j->security);
        const PeachWmiBss *b = &wifi_wmi.bss[selected];
        printf("NEZHA WIFI: selected BSSID %02x:%02x:%02x:%02x:%02x:%02x frequency %u RSSI %d\n",
            b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3], b->bssid[4], b->bssid[5],
            b->frequency, b->rssi);
        if (wifi_request.passphrase_bytes)
            err = peach_rsn_pmk(wifi_request.passphrase, wifi_request.passphrase_bytes,
                j->ssid, j->ssid_bytes, j->pmk);
        volatile unsigned char *passphrase = wifi_request.passphrase;
        for (size_t i = 0; i < sizeof(wifi_request.passphrase); i++) passphrase[i] = 0;
        wifi_request.passphrase_bytes = 0;
        if (j->security == PeachStationWpa2Psk)
            if (!err) err = peach_rsn_prepare(&wifi_rsn, &wifi_station, selected, j->pmk);
        wifi_step = "802.11 authentication/association";
        if (!err) err = peach_station_associate(&wifi_station, selected, j->security);
        if (!err && j->security == PeachStationWpa2Psk) {
            wifi_step = "RSN four-way handshake";
            err = peach_rsn_start(&wifi_rsn);
            if (!err) {
                wifi_net_state = PeachNetHandshake;
                peach_net_status(&wifi_wmi, wifi_net_state, 0);
            }
        } else if (!err) {
            err = peach_wmi_authorize(&wifi_wmi, &wifi_htt);
            if (!err) err = wifi_station.error;
            if (!err) err = peach_tx_authorize(&wifi_tx, 1);
        }
    }
clear_request:;
    volatile unsigned char *secret = (void *)&wifi_request;
    for (size_t i = 0; i < sizeof(wifi_request); i++) secret[i] = 0;
    return err;
}

static int wifi_connection_recover(int error)
{
    if (wifi_htc.error || wifi_ce.error || wifi_wmi.error || wifi_htt.error ||
        wifi_rx.error || wifi_tx.error || wifi_wlfw.error || wifi_host.poisoned ||
        wifi_dma_health()) return error;
    printf("NEZHA WIFI: connection failed %d, on-air status %u station/RSN/action %u/%u/%u; checking pre-key retirement\n",
        error, wifi_station.on_air_status, wifi_station.phase, wifi_rsn.phase, wifi_rsn.tx_action);
    peach_rsn_clear(&wifi_rsn);
    int err = peach_station_recover_unkeyed(&wifi_station);
    if (err) {
        printf("NEZHA WIFI: connection retirement unconfirmed: %d\n", err);
        return error;
    }
    wifi_net_state = PeachNetReady;
    peach_net_status(&wifi_wmi, wifi_net_state, error);
    wifi_step = "awaiting explicit network selection";
    printf("NEZHA WIFI: pre-key connection retired; manual retry available, old DMA retained\n");
    return 0;
}

/* PERST assertion is the sole recovery write permitted after another MMIO
 * fault. It uses only the already mapped, always-on TLMM GPIO102 window.
 * Retain DMA and tables even if containment fails; never infer drain. */
static void wifi_contain(void)
{
    if (wifi_windows[W_PERST].va) {
        uint64_t value;
        uintptr_t va = wifi_windows[W_PERST].va + 4;
        if (!nezha_wifi_mmio_access(va, &value, 0, 0)) {
            value &= ~2ull;
            unsigned failed = nezha_wifi_mmio_access(va, &value, 1, 0);
            uint64_t config = 0, check = 2;
            failed |= nezha_wifi_mmio_access(va - 4, &config, 0, 0);
            failed |= nezha_wifi_mmio_access(va, &check, 0, 0);
            failed |= (config & 0x23c) != 0x200 || (check & 2) != 0;
            printf("NEZHA WIFI: PERST %s; drain unproven, DMA retained\n",
                failed ? "containment unconfirmed" : "GPIO assertion read back");
        }
    }
}

static int wifi_restart_mission(PeachMhiDma calibration)
{
    wifi_step = "firmware recovery reset";
    if (!wifi_radio_off) WIFI_TRY(wifi_disable_radio());
    int recycle = NezhaWifiDmaRecycle(&wifi_dma_pool, &wifi_stop_proof,
                                      calibration.bytes, calibration.capacity);
    if (recycle) {
        NezhaWifiDmaQuarantine(&wifi_dma_pool);
        if (wifi_quarantined_recoveries >= WIFI_RECOVERIES) return ENOSPC;
        wifi_quarantined_recoveries++;
    }
    printf("NEZHA WIFI: DMA generation %u recycle %u quarantine %u/%u allocations %u reused %u IOVA end %#llx\n",
        ++wifi_generations, !recycle, wifi_quarantined_recoveries, WIFI_RECOVERIES,
        wifi_dma_pool.count, wifi_dma_pool.reuses, wifi_dma_next);
    WIFI_TRY(wifi_reset_endpoint());
    /* Never change an existing SMMU mapping or release any owned allocation.
     * CAL stays in use; recycled buffers need the consumed hardware proof. */
    PeachMhiDma arena;
    WIFI_TRY(wifi_allocate_dma(NULL, WIFI_DMA_BYTES, &arena));
    wifi_arena = (vm_offset_t)arena.bytes; wifi_arena_iova = arena.iova;
    memset(&wifi_firmware, 0, sizeof(wifi_firmware));
    memset(&wifi_rddm, 0, sizeof(wifi_rddm)); wifi_rddm_session = 0;
    memset(&wifi_host, 0, sizeof(wifi_host)); memset(&wifi_wlfw, 0, sizeof(wifi_wlfw));
    memset(&wifi_ce, 0, sizeof(wifi_ce)); memset(&wifi_htc, 0, sizeof(wifi_htc));
    memset(&wifi_htt, 0, sizeof(wifi_htt)); memset(&wifi_wmi, 0, sizeof(wifi_wmi));
    memset(wifi_srng, 0, sizeof(wifi_srng));
    memset(wifi_rxmac, 0, sizeof(wifi_rxmac)); wifi_rxmac_count = 0;
    memset(&wifi_rx, 0, sizeof(wifi_rx)); memset(&wifi_tx, 0, sizeof(wifi_tx));
    memset(&wifi_station, 0, sizeof(wifi_station));
    return wifi_mission_start(calibration);
}

static int wifi_firmware_recover(int error)
{
    int automatic = 0;
    (void)PE_parse_boot_argn("nezha_wifi_recover", &automatic, sizeof(automatic));
    if (automatic != 1 || wifi_fault || wifi_quarantined_recoveries >= WIFI_RECOVERIES ||
        !wifi_wlfw.calibration_done || !wifi_wlfw.calibration.bytes || !wifi_wmi.ready)
        return error;
    PeachMhiDma calibration = wifi_wlfw.calibration;
    unsigned char mac[6]; memcpy(mac, wifi_wmi.mac, sizeof(mac));
    wifi_net_state = PeachNetRecovering;
    peach_net_recovering(error); peach_rsn_clear(&wifi_rsn);
    for (unsigned attempt = 0; !wifi_fault && attempt < WIFI_RECOVERIES; attempt++) {
        printf("NEZHA WIFI: firmware recovery %u/%u, error %d station/RSN %u/%u\n",
            attempt + 1, WIFI_RECOVERIES, error, wifi_station.phase, wifi_rsn.phase);
        wifi_mhi_failure();
        if (wifi_fault) break;
        int err = wifi_restart_mission(calibration);
        if (!err && memcmp(mac, wifi_wmi.mac, sizeof(mac))) return EIO;
        if (!err) {
            wifi_net_state = PeachNetReady;
            peach_net_status(&wifi_wmi, wifi_net_state, 0);
            printf("NEZHA WIFI: firmware recovered; en0 retained, scan ready, manual join required\n");
            return 0;
        }
        /* A failure during restarted bring-up must not bypass the remaining
         * recovery attempts simply because READY has not been reached yet. */
        printf("NEZHA WIFI: recovery bring-up failed %d in %s; retained DMA\n", err, wifi_step);
        error = err;
        wifi_sleep(NULL, 100000);
    }
    return error;
}

static void
wifi_thread(void *unused, wait_result_t result)
{
    (void)unused; (void)result;
    thread_set_thread_name(current_thread(), "nezha-wcn7860");
    int err = nezha_wifi_diagnostic_init();
    uint64_t deadline = wifi_now(NULL) + 300000000;
    while (!err && !nezha_wifi_platform_ready() && wifi_now(NULL) < deadline) wifi_sleep(NULL, 100000);
    if (!err && !nezha_wifi_platform_ready()) err = ETIMEDOUT;
    while (!err) {
        int ready = nezha_wifi_firmware_prepare();
        if (!ready) ready = nezha_wifi_auto_prepare();
        if (!ready) break;
        if ((ready != ENOENT && ready != EAGAIN) || wifi_now(NULL) >= deadline) { err = ready; break; }
        wifi_sleep(NULL, 100000);
    }
    if (!err) err = peach_net_init();
    if (!err) err = wifi_start();
    if (!err) {
        wifi_net_state = PeachNetReady;
        peach_net_status(&wifi_wmi, wifi_net_state, wifi_wmi.scan_done ? 0 : EAGAIN);
        wifi_step = "BSD/Apple80211 interface discovery";
        err = peach_net_attach(wifi_wmi.mac);
        if (!err) wifi_net_attached = 1;
    }
    while (!err) {
        nezha_wifi_diagnostic_publish(wifi_phase, wifi_net_state, 0,
            wifi_rx.received, wifi_tx.submitted, wifi_tx.dropped, wifi_tx.in_flight,
            wifi_bank, wifi_slot, wifi_step);
        nezha_wifi_auto_tick(wifi_now(NULL));
        if (wifi_radio_off) {
            err = wifi_connection_request();
            if (!err) wifi_sleep(NULL, 100000);
            continue;
        }
        err = peach_htc_poll(&wifi_htc);
        if (!err) err = wifi_station.error;
        unsigned rsn_phase = wifi_rsn.phase;
        if (!err) err = peach_rsn_poll(&wifi_rsn);
        if (wifi_rsn.phase != rsn_phase)
            printf("NEZHA WIFI: RSN phase %u -> %u error %d action %u\n",
                rsn_phase, wifi_rsn.phase, err, wifi_rsn.tx_action);
        if (!err) err = wifi_connection_request();
        if (!err && wifi_radio_off) continue; /* do not reopen link from old TX state */
        if (err) err = wifi_connection_recover(err);
        if (!err && wifi_tx.authorized) {
            if (!err && wifi_net_state != PeachNetOnline) {
                wifi_net_state = PeachNetOnline;
                peach_net_status(&wifi_wmi, wifi_net_state, 0);
                printf("NEZHA WIFI: association/security complete; en0 link up, IP configuration pending\n");
            }
            if (!err) err = peach_net_transmit(&wifi_tx);
        } else if (!err && wifi_net_attached && wifi_net_state == PeachNetOnline) {
            wifi_net_state = PeachNetHandshake;
            peach_net_status(&wifi_wmi, wifi_net_state, 0);
        }
        if (err && wifi_net_attached) err = wifi_firmware_recover(err);
        if (!err) wifi_sleep(NULL, wifi_tx.bound ? 1000 : 5000);
    }
    printf("NEZHA WIFI: failure station/RSN/action %u/%u/%u WMI event %#x\n",
        wifi_station.phase, wifi_rsn.phase, wifi_rsn.tx_action, wifi_wmi.last_event);
    printf("NEZHA WIFI: failure RX received/discarded %llu/%llu TX submitted/acked/dropped %llu/%llu/%llu\n",
        wifi_rx.received, wifi_rx.discarded, wifi_tx.submitted, wifi_tx.acked, wifi_tx.dropped);
    nezha_wifi_diagnostic_publish(wifi_phase, PeachNetFailed, err,
        wifi_rx.received, wifi_tx.submitted, wifi_tx.dropped, wifi_tx.in_flight,
        wifi_bank, wifi_slot, wifi_step);
    peach_net_stop(err); peach_rsn_clear(&wifi_rsn);
    nezha_wifi_auto_stop(err); nezha_wifi_auto_tick(wifi_now(NULL));
    if (!wifi_radio_off) wifi_mhi_failure();
    wifi_contain();
    printf("NEZHA WIFI: phase %u stopped in %s: %d; data link down\n",
        wifi_phase, wifi_step, err);
    thread_terminate(current_thread());
}

/* Offline checks are reachable only on the existing synthetic QEMU board,
 * with a separate explicit option. No radio/firmware/domain function is used.
 * These observations never advertise link-up, keys, IP or real RF reception. */
volatile unsigned nezha_wifi_offline_stage;
volatile int nezha_wifi_offline_error;
__attribute__((noinline)) void nezha_wifi_offline_complete(void)
{ __asm__ volatile("" ::: "memory"); }
static void wifi_offline_thread(void *unused, wait_result_t result)
{
    (void)unused; (void)result;
    thread_set_thread_name(current_thread(), "wifi27-offline-check");
    extern int peach_wlan_offline_check(void);
    uint64_t word = 0x12345678, value = 0;
    nezha_wifi_offline_stage = 1;
    /* Actual guest page allocation/cache transition and a virtual PL011
     * mapping qualify this kernel binding; no WCN7860 DMA is simulated. */
    uintptr_t page = nezha_wifi_allocate_pages(4096, 0);
    uintptr_t contiguous = nezha_wifi_allocate_pages(8192, 1);
    int error = (!page || !contiguous || !nezha_wifi_physical(page) ||
        nezha_wifi_physical(contiguous + 4096) != nezha_wifi_physical(contiguous) + 4096) ? ENOMEM : 0;
    if (!error && nezha_wifi_make_uncached(page, 4096)) error = EIO;
    if (!error) {
        *(volatile uint64_t *)page = 0x1122334455667788ull;
        if (*(volatile uint64_t *)page != 0x1122334455667788ull) error = EIO;
    }
    uintptr_t pl011 = error ? 0 : nezha_wifi_map_device(0x09000000, 4096);
    if (!error && (!pl011 || nezha_wifi_mmio_access(pl011 + 0x18, &value, 0, 0))) error = EIO;
    if (!error) error = (int)nezha_wifi_mmio_access((uintptr_t)&word, &value, 0, 1);
    if (!error && value != word) error = EIO;
    if (!error) {
        value = 0xabcdef;
        error = (int)nezha_wifi_mmio_access((uintptr_t)&word, &value, 1, 1);
        if (!error && word != value) error = EIO;
    }
    if (!error) {
        nezha_wifi_offline_stage = 2;
        /* Unmapped guest kernel address, never a physical or live MMIO address. */
        if (nezha_wifi_mmio_access(0xfffffff000001000ull, &value, 0, 0) != 1) error = EIO;
    }
    if (!error) { nezha_wifi_offline_stage = 3; error = peach_net_init(); }
    if (!error) {
        const unsigned char test_mac[6] = {2, 0, 0, 0, 0, 1};
        error = peach_net_attach(test_mac);
    }
    if (!error) { nezha_wifi_offline_stage = 4; error = peach_wlan_offline_check(); }
    PeachNetInfo info; peach_net_info(&info);
    if (!error && (info.powered || info.state == PeachNetOnline)) error = EIO;
    nezha_wifi_offline_error = error;
    if (!error) nezha_wifi_offline_stage = 5;
    printf("NEZHA WIFI OFFLINE: QEMU-only stage=%u error=%d; real kernel/IOKit lifecycle, no radio or network link\n",
        nezha_wifi_offline_stage, error);
    nezha_wifi_offline_complete();
    thread_terminate(current_thread());
}

void
nezha_wifi_init(void)
{
    int on = 0;
    thread_t thread;
    (void)PE_parse_boot_argn("nezha_wifi", &on, sizeof(on));
    if (on != 1 && on != 2) return;
    DTEntry root;
    const void *data;
    unsigned size = 0;
    if (SecureDTLookupEntry(NULL, "/", &root) != kSuccess ||
        SecureDTGetProperty(root, "compatible", &data, &size) != kSuccess) return;
    bool nezha = false, offline = false;
    for (unsigned off = 0; off < size;) {
        size_t n = strnlen((const char *)data + off, size - off);
        if (n == size - off) return;
        if (n == 12 && !memcmp((const char *)data + off, "xiaomi,nezha", 12)) nezha = true;
        if (n == 16 && !memcmp((const char *)data + off, "linux,dummy-virt", 16)) offline = true;
        off += (unsigned)n + 1;
    }
    if ((on == 1 && !nezha) || (on == 2 && (!offline || nezha))) return;
    static unsigned started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
    if (kernel_thread_start(on == 2 ? wifi_offline_thread : wifi_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
#endif
