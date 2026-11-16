/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * nezha (SM8850, IPA v5.5) version of QcomBaseband ipa/coachz_ipa_proto.h (dd0199c, BSD-2-Clause, HoshinoYumeka),
 * derived by tools/baseband27/derive-ipa55.py. Mainline ipa_main.c ipa_hardware_config for v4.5+: no BCR, no TX_CFG or
 * CLKON workaround, COMP_CFG only the in-order disables, QSB for DDR and PCIe, Qtime timers instead of COUNTER_CFG,
 * nothing for hashing (v5.0 caches are on by default), DCD as before. ipa_mem_config with the device's own SRAM layout
 * (cipa_mem from the phone's ipa_bootup log). Endpoints: up to 36 on v5.0+. Resources: eight groups, four register pairs.
 */
#ifndef COACHZ_IPA_PROTO_H
#define COACHZ_IPA_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include "nb_ipa_v55.h"
#include "nb_ipa55_qmi.h"

/* nezha IPA: wrapper 0x03e00000, IPA_CFG at +0x140000 ("ipa-reg"), SRAM after it ("ipa-shared", 0x5000 bytes per
   ipa3_post_init), GSI 0x03e04000/0xfc000 (the phone's ipa_bootup log) */
#define CIP_REG_PA     CIPA_REG_BASE
#define CIP_REG_BYTES  0x10000u
#define CIP_SHARED_PA  CIPA_SRAM_BASE
#define CIP_SHARED_BYTES CIPA_MEM_SIZE   /* ipa->mem_size before SHARED_MEM_SIZE is read */
#define CIP_GSI_PA     CGSI_BASE
#define CIP_GSI_BYTES  CGSI_SIZE

/* reg.h reg_encode/reg_decode: a value placed under a field mask */
static inline uint32_t cip_encode(uint32_t mask, uint32_t value) { return (value << __builtin_ctz(mask)) & mask; }
static inline uint32_t cip_decode(uint32_t mask, uint32_t reg) { return (reg & mask) >> __builtin_ctz(mask); }

/* ------------------------------------------------------------------ ipa_hardware_config for v5.5, in order */
/* bcr: none on v4.5+; tx: untouched on v4.5+; clkon: untouched on v4.5+ */
/* comp: v4.5+ only the two in-order disables (FULL_FLUSH_WAIT_RS_CLOSURE_EN stays 0) */
static inline uint32_t cip_comp_cfg(uint32_t reg)
{
    return reg | CIPA_COMP_CFG_GSI_MULTI_INORDER_RD_DIS | CIPA_COMP_CFG_GSI_MULTI_INORDER_WR_DIS;
}
/* qsb: DDR (QMB 0) and PCIe (QMB 1). Both masters' reads go in the one register (mainline assigns instead of or-ing the
   PCIe half, which would drop the DDR value; the downstream driver programs both). */
static inline uint32_t cip_qsb_writes(void)
{
    return cip_encode(CIPA_QSB_MAX_WRITES_GEN_QMB_0_MAX_WRITES, cipa_qsb[0].max_writes) |
           cip_encode(CIPA_QSB_MAX_WRITES_GEN_QMB_1_MAX_WRITES, cipa_qsb[1].max_writes);
}
static inline uint32_t cip_qsb_reads(void)
{
    return cip_encode(CIPA_QSB_MAX_READS_GEN_QMB_0_MAX_READS, cipa_qsb[0].max_reads) |
           cip_encode(CIPA_QSB_MAX_READS_GEN_QMB_0_MAX_READS_BEATS, cipa_qsb[0].max_reads_beats) |
           cip_encode(CIPA_QSB_MAX_READS_GEN_QMB_1_MAX_READS, cipa_qsb[1].max_reads) |
           cip_encode(CIPA_QSB_MAX_READS_GEN_QMB_1_MAX_READS_BEATS, cipa_qsb[1].max_reads_beats);
}
/* timing, v4.5+: ipa_qtime_config. The XO divider (192, written as 191) is disabled while the rate changes, then enabled.
   Tag and NAT timestamps use Qtime bits from 14 and 24 (no DPL setting on v5.5). Pulse generators: 0 = 100 us,
   1 = 1 ms, 2 and 3 = 10 ms (v5.0+). The phone's log shows the same divider (191) and its enable. */
#define CIP_TAG_TIMESTAMP_SHIFT 14u
#define CIP_NAT_TIMESTAMP_SHIFT 24u
#define CIP_XO_CLOCK_DIVIDER 192u
#define CIP_GRAN_100_US 3u
#define CIP_GRAN_1_MS 4u
#define CIP_GRAN_10_MS 5u
static inline uint32_t cip_qtime_timestamp(void)
{
    return cip_encode(CIPA_QTIME_TIMESTAMP_CFG_TAG_TIMESTAMP_LSB, CIP_TAG_TIMESTAMP_SHIFT) |
           cip_encode(CIPA_QTIME_TIMESTAMP_CFG_NAT_TIMESTAMP_LSB, CIP_NAT_TIMESTAMP_SHIFT);
}
static inline uint32_t cip_pulse_gran(void)
{
    return cip_encode(CIPA_TIMERS_PULSE_GRAN_CFG_PULSE_GRAN_0, CIP_GRAN_100_US) |
           cip_encode(CIPA_TIMERS_PULSE_GRAN_CFG_PULSE_GRAN_1, CIP_GRAN_1_MS) |
           cip_encode(CIPA_TIMERS_PULSE_GRAN_CFG_PULSE_GRAN_2, CIP_GRAN_10_MS) |
           cip_encode(CIPA_TIMERS_PULSE_GRAN_CFG_PULSE_GRAN_3, CIP_GRAN_10_MS);
}
static inline uint32_t cip_xo_div(int enable)
{
    return cip_encode(CIPA_TIMERS_XO_CLK_DIV_CFG_DIV_VALUE, CIP_XO_CLOCK_DIVIDER - 1u) |
           (enable ? CIPA_TIMERS_XO_CLK_DIV_CFG_DIV_ENABLE : 0u);
}
/* ipa_qtime_val: a time in microseconds as ticks of the finest pulse generator that can hold it; *select names it.
   The 10 ms generator divides by 10000 (mainline divides by 100 there, which only matters above 31 ms). */
static inline uint32_t cip_qtime_val(uint32_t usec, uint32_t max, uint32_t *select)
{
    uint32_t ticks = (usec + 50u) / 100u;
    *select = 0;
    if (ticks <= max) return ticks;
    ticks = (usec + 500u) / 1000u;
    *select = 1;
    if (ticks <= max) return ticks;
    *select = 2;
    return (usec + 5000u) / 10000u;
}
/* hashing: nothing on v5.0+; dcd: idle indication, debounce 256, no constant non-idle */
static inline uint32_t cip_idle_indication(void) { return cip_encode(CIPA_IDLE_INDICATION_CFG_ENTER_IDLE_DEBOUNCE_THRESH, 256); }

/* ------------------------------------------------------------------ ipa_mem_config */
struct cip_mem { uint32_t mem_offset, mem_size; };
/* SHARED_MEM_SIZE: BADDR and SIZE in 8-byte units; a smaller reported size limits the region, a larger one is ignored */
static inline struct cip_mem cip_mem_config(uint32_t shared_mem_size, uint32_t resource_size)
{
    struct cip_mem m;
    m.mem_offset = 8u * cip_decode(CIPA_SHARED_MEM_SIZE_MEM_BADDR, shared_mem_size);
    const uint32_t size = 8u * cip_decode(CIPA_SHARED_MEM_SIZE_MEM_SIZE, shared_mem_size);
    m.mem_size = m.mem_offset + size < resource_size ? size : resource_size;
    return m;
}
/* ipa_mem_size_valid (every region ends within mem_size) and ipa_mem_valid_one for the nezha table */
static inline int cip_mem_valid(uint32_t mem_size)
{
    for (unsigned i = 0; i < CI_MEM_COUNT; i++) {
        const struct ci_region *r = &ci_nezha_mem[i];
        const uint32_t multiple = i == CI_MODEM ? 4u : 8u;
        if ((uint32_t)r->offset + r->size > mem_size) return 0;
        if (!r->size && !r->canaries) return 0;
        if (r->size % multiple || r->offset % 8u || r->offset < r->canaries * 4u) return 0;
    }
    return 1;
}
#define CIP_CANARY 0xdeadbeefu
/* The canaries sit in the words just below a region: offsets mem_offset + offset - 4, - 8, ... */
static inline uint32_t cip_canary_offset(uint32_t mem_offset, unsigned region, unsigned k)
{
    return mem_offset + ci_nezha_mem[region].offset - 4u * (k + 1u);
}
#define CIP_MEM_MAX 0x2000u   /* IPA_MEM_MAX = 2 * PAGE_SIZE (4 KiB pages): the zeroed DMA buffer for ipa_mem_zero */

/* ------------------------------------------------------------------ ipa_interrupt.c, ipa_uc.c */
/* enum ipa_irq_id (ipa_reg.h): bits of IPA_IRQ_STTS / _EN / _CLR */
#define CIP_IRQ_UC_0       2u    /* microcontroller event */
#define CIP_IRQ_UC_1       3u    /* microcontroller response */
#define CIP_IRQ_TX_SUSPEND 14u
/* struct ipa_uc_mem_area at mem_offset + IPA_MEM_UC_SHARED: byte fields read as the 32-bit words holding them */
#define CIP_UC_COMMAND        0x00u
#define CIP_UC_COMMAND_PARAM  0x04u
#define CIP_UC_COMMAND_PARAM_HI 0x08u
#define CIP_UC_RESPONSE       0x0cu
#define CIP_UC_RESPONSE_PARAM 0x10u
#define CIP_UC_EVENT          0x14u
#define CIP_UC_RESPONSE_INIT_COMPLETED 1u
#define CIP_UC_EVENT_ERROR    1u
#define CIP_UC_EVENT_LOG_INFO 2u
#define CIP_UC_COMMAND_ERR_FATAL 4u
/* ipa_power_retention through the AOSS QMP (qcom,qmp on the sc7180 IPA node) */
#define CIP_RETENTION_ON  "{ class: bcm, res: ipa_pc, val: 1 }"
#define CIP_RETENTION_OFF "{ class: bcm, res: ipa_pc, val: 0 }"

/* What ipa_uc_response_hdlr does with a response: 1 = the uC is loaded now (drop the proxy vote, enable retention) */
static inline int cip_uc_response_loads(uint32_t response_word, int uc_powered)
{
    return (response_word & 0xffu) == CIP_UC_RESPONSE_INIT_COMPLETED && uc_powered;
}

/* ------------------------------------------------------------------ ipa_endpoint_config (v3.5+) */
#define CIP_ENDPOINT_MAX 36u   /* IPA_ENDPOINT_MAX; the phone reports "IPA Pipes num 36" */
/* ipa_gsi_endpoint_data_empty: an AP entry with no TLVs is a hole in the table */
static inline int cip_endpoint_defined(const struct cipa_endpoint *e) { return e->ee != 0 || e->tlv_count != 0; }
struct cip_flavor { uint32_t tx_count, rx_count, rx_base, limit; };
static inline struct cip_flavor cip_flavor(uint32_t reg)
{
    struct cip_flavor f;
    f.tx_count = cip_decode(CIPA_FLAVOR_0_MAX_CONS_PIPES, reg);   /* our TX is an IPA consumer */
    f.rx_count = cip_decode(CIPA_FLAVOR_0_MAX_PROD_PIPES, reg);
    f.rx_base = cip_decode(CIPA_FLAVOR_0_PROD_LOWEST, reg);
    f.limit = f.rx_base + f.rx_count;
    return f;
}
/* 0 when every defined endpoint exists in hardware and points the right way; otherwise the endpoint id + 1 that fails, or
   CIP_ENDPOINT_MAX + 1 when the hardware reports more endpoints than the driver supports (36; the 32 limit is pre-v5.0). */
static inline unsigned cip_endpoint_check(uint32_t flavor_reg)
{
    const struct cip_flavor f = cip_flavor(flavor_reg);
    if (f.limit > CIP_ENDPOINT_MAX) return CIP_ENDPOINT_MAX + 1u;
    for (unsigned i = 0; i < sizeof cipa_endpoints / sizeof cipa_endpoints[0]; i++) {
        const struct cipa_endpoint *e = &cipa_endpoints[i];
        if (!cip_endpoint_defined(e)) continue;
        const uint32_t id = e->endpoint;
        const int available = id < f.tx_count || (id >= f.rx_base && id < f.limit);
        if (id >= f.limit || !available) return id + 1u;
        if (e->toward_ipa ? id >= f.tx_count : id < f.rx_base) return id + 1u;
    }
    return 0;
}

/* ------------------------------------------------------------------ ipa_resource_config */
/* ipa_resource_limits_valid: 1..8 groups, and no limit in a group the hardware lacks */
static inline int cip_resource_valid(const uint32_t (*limits)[8][2], unsigned types, unsigned groups)
{
    if (!groups || groups > 8u) return 0;
    for (unsigned i = 0; i < types; i++)
        for (unsigned g = groups; g < 8u; g++)
            if (limits[i][g][0] || limits[i][g][1]) return 0;
    return 1;
}
/* ipa_resource_config_common for group pair (2k, 2k+1): Y only when the group count reaches it. Every SRC and DST
   GRP_01/23/45/67 register shares one field layout on v5.5 too. */
static inline uint32_t cip_resource_value(const uint32_t (*limit)[2], unsigned pair, unsigned groups)
{
    uint32_t v = cip_encode(CIPA_SRC_RSRC_GRP_01_RSRC_TYPE_X_MIN_LIM, limit[2 * pair][0]) |
                 cip_encode(CIPA_SRC_RSRC_GRP_01_RSRC_TYPE_X_MAX_LIM, limit[2 * pair][1]);
    if (groups > 2 * pair + 1)
        v |= cip_encode(CIPA_SRC_RSRC_GRP_01_RSRC_TYPE_Y_MIN_LIM, limit[2 * pair + 1][0]) |
             cip_encode(CIPA_SRC_RSRC_GRP_01_RSRC_TYPE_Y_MAX_LIM, limit[2 * pair + 1][1]);
    return v;
}
#define CIP_RSRC_SRC_TYPES (sizeof cipa_resource_src / sizeof cipa_resource_src[0])
#define CIP_RSRC_DST_TYPES (sizeof cipa_resource_dst / sizeof cipa_resource_dst[0])
/* Register pairs a group count needs: (groups + 1) / 2, up to four on v5.5 (six source, seven destination groups). */
static inline unsigned cip_resource_pairs(unsigned groups) { return (groups + 1u) / 2u; }

#endif /* COACHZ_IPA_PROTO_H */
