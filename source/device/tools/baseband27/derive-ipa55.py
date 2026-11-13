#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Derive the nezha IPA v5.5 / GSI v5.0 protocol headers from the vendored QcomBaseband v4.2 ones (ipa/upstream, dd0199c).

Each change is an exact, counted text replacement, so the result stays a reviewable diff of the original and fails loudly
if the upstream text moves. Every change cites the mainline drivers/net/ipa version branch it follows.
Run from package/baseband27/ipa: python3 ../../../tools/baseband27/derive-ipa55.py
"""
import sys


class Edit:
    def __init__(self, path):
        self.s = open(path).read()

    def rep(self, old, new, count=1):
        n = self.s.count(old)
        if n != count:
            sys.exit(f"expected {count} of {old[:70]!r}, found {n}")
        self.s = self.s.replace(old, new)

    def all(self, old, new):
        if old not in self.s:
            sys.exit(f"none of {old[:70]!r}")
        self.s = self.s.replace(old, new)


def gsi():
    e = Edit("upstream/coachz_gsi_proto.h")
    e.rep(""" * Portable decisions of mainline drivers/net/ipa/gsi.c for the IPA v4.2 GSI (gsi_init's channel table, gsi_setup,
 * gsi_channel_setup, the channel and event ring programming, the command words and the generic command result).
 * Register numbers come from the generated coachz_ipa_v42.h. Linux 7.4-rc1 (6628e7d4c6b2); the code is our own.
 */""", """ * nezha (SM8850, IPA v5.5 / GSI v5.0) version of QcomBaseband ipa/coachz_gsi_proto.h (dd0199c, BSD-2-Clause,
 * HoshinoYumeka), derived by tools/baseband27/derive-ipa55.py: same functions and names so coachz_gsi.c reads either.
 * Changes follow the mainline gsi.c version branches between v4.2 and v5.5:
 *   - the AP no longer allocates modem channels (gsi_channel_init modem_alloc is v4.2 only);
 *   - the event ring index moved from CH_C_CNTXT_0.ERINDEX to CH_C_CNTXT_1.CH_ERINDEX (v5.0+);
 *   - CH_C_QOS: PREFETCH_MODE = escape buffer only replaces USE_ESCAPE_BUF_ONLY (v4.5+), DB_IN_BYTES on every
 *     channel (v4.9+);
 *   - the event ring count comes from HW_PARAM_4.EV_PER_EE (v5.0+);
 *   - GENERIC_CMD has a params field (v4.11+, 0 here); disabling modem flow control may answer RETRY (v4.11+).
 * CHTYPE_PROTOCOL has no MSB part on v5.0+ (ch_c_cntxt_0_type_encode), as on v4.2. Registers from nb_ipa_v55.h.
 */""")
    e.rep('#include "coachz_ipa_proto.h"', '#include "nb_ipa55_proto.h"')
    e.rep("#define CGS_CMD_TIMEOUT_MS     50u", """#define CGS_CMD_TIMEOUT_MS     50u
#define CGS_PREFETCH_ESCAPE_BUF_ONLY 1u   /* enum gsi_prefetch_mode */
#define CGS_FLOW_RETRIES       5u    /* GSI_CHANNEL_MODEM_FLOW_RETRIES */""")
    e.rep("    uint32_t modem_channels;           /* v4.2: channels the AP allocates for the modem */",
          "    uint32_t modem_channels;           /* always 0 on v5.5: the modem allocates its own channels */")
    e.rep("        if (e->ee == CGS_EE_MODEM) { t.modem_channels |= 1u << e->channel; continue; }   /* no ring data; not validated */",
          "        if (e->ee == CGS_EE_MODEM) continue;   /* no ring data, not validated, not allocated by the AP */")
    e.rep("""/* ------------------------------------------------------------------ gsi_ring_setup (HW_PARAM_2 before v5.0) */
struct cgs_counts { uint32_t channels, evt_rings; };
static inline struct cgs_counts cgs_ring_counts(uint32_t hw_param_2)
{
    struct cgs_counts c;
    c.channels = cip_decode(CGSI_HW_PARAM_2_NUM_CH_PER_EE, hw_param_2);
    c.evt_rings = cip_decode(CGSI_HW_PARAM_2_NUM_EV_PER_EE, hw_param_2);""",
          """/* ------------------------------------------------------------------ gsi_ring_setup (v5.0+: HW_PARAM_2, HW_PARAM_4) */
struct cgs_counts { uint32_t channels, evt_rings; };
static inline struct cgs_counts cgs_ring_counts(uint32_t hw_param_2, uint32_t hw_param_4)
{
    struct cgs_counts c;
    c.channels = cip_decode(CGSI_HW_PARAM_2_NUM_CH_PER_EE, hw_param_2);
    c.evt_rings = cip_decode(CGSI_HW_PARAM_4_EV_PER_EE, hw_param_4);""")
    e.rep("""/* ------------------------------------------------------------------ gsi_channel_program (v4.2) */
static inline uint32_t cgs_ch_cntxt_0(const struct cgs_channel *c)
{
    uint32_t v = cip_encode(CGSI_CH_C_CNTXT_0_CHTYPE_PROTOCOL, CGS_TYPE_GPI);
    if (c->toward_ipa) v |= CGSI_CH_C_CNTXT_0_CHTYPE_DIR;
    return v | cip_encode(CGSI_CH_C_CNTXT_0_ERINDEX, c->evt_ring) | cip_encode(CGSI_CH_C_CNTXT_0_ELEMENT_SIZE, CGS_ELEMENT);
}
static inline uint32_t cgs_ch_cntxt_1(const struct cgs_channel *c) { return cip_encode(CGSI_CH_C_CNTXT_1_CH_R_LENGTH, c->tre_count * CGS_ELEMENT); }
/* QOS: the command channel gets the largest WRR weight; every other channel uses the escape buffer only (v4.0..4.4) */
static inline uint32_t cgs_ch_qos(const struct cgs_channel *c)
{
    return c->command ? CGSI_CH_C_QOS_WRR_WEIGHT : CGSI_CH_C_QOS_USE_ESCAPE_BUF_ONLY;
}""", """/* ------------------------------------------------------------------ gsi_channel_program (v5.0+) */
static inline uint32_t cgs_ch_cntxt_0(const struct cgs_channel *c)
{
    uint32_t v = cip_encode(CGSI_CH_C_CNTXT_0_CHTYPE_PROTOCOL, CGS_TYPE_GPI);
    if (c->toward_ipa) v |= CGSI_CH_C_CNTXT_0_CHTYPE_DIR;
    return v | cip_encode(CGSI_CH_C_CNTXT_0_ELEMENT_SIZE, CGS_ELEMENT);
}
static inline uint32_t cgs_ch_cntxt_1(const struct cgs_channel *c)
{
    return cip_encode(CGSI_CH_C_CNTXT_1_CH_R_LENGTH, c->tre_count * CGS_ELEMENT) |
           cip_encode(CGSI_CH_C_CNTXT_1_CH_ERINDEX, c->evt_ring);
}
/* QOS: the command channel gets the largest WRR weight and the default prefetch buffers, every other channel the escape
   buffer only; all count doorbells in bytes */
static inline uint32_t cgs_ch_qos(const struct cgs_channel *c)
{
    const uint32_t v = c->command ? CGSI_CH_C_QOS_WRR_WEIGHT
                                  : cip_encode(CGSI_CH_C_QOS_PREFETCH_MODE, CGS_PREFETCH_ESCAPE_BUF_ONLY);
    return v | CGSI_CH_C_QOS_DB_IN_BYTES;
}""")
    e.rep("""/* GENERIC_CMD on behalf of the modem EE (no params before v4.11) */
static inline uint32_t cgs_generic_cmd(uint32_t channel, uint32_t opcode)
{
    return cip_encode(CGSI_GENERIC_CMD_GENERIC_OPCODE, opcode) | cip_encode(CGSI_GENERIC_CMD_GENERIC_CHID, channel) |
           cip_encode(CGSI_GENERIC_CMD_GENERIC_EE, CGS_EE_MODEM);
}""", """/* GENERIC_CMD on behalf of the modem EE (params from v4.11; 0 for every command used here) */
static inline uint32_t cgs_generic_cmd(uint32_t channel, uint32_t opcode)
{
    return cip_encode(CGSI_GENERIC_CMD_GENERIC_OPCODE, opcode) | cip_encode(CGSI_GENERIC_CMD_GENERIC_CHID, channel) |
           cip_encode(CGSI_GENERIC_CMD_GENERIC_EE, CGS_EE_MODEM) | cip_encode(CGSI_GENERIC_CMD_GENERIC_PARAMS, 0);
}
/* gsi_modem_channel_flow_control: how often a command that answered RETRY is sent again */
static inline uint32_t cgs_flow_retries(int enable) { return enable ? 0u : CGS_FLOW_RETRIES; }""")
    open("nb_gsi55_proto.h", "w").write(e.s)


def ipa():
    e = Edit("upstream/coachz_ipa_proto.h")
    e.rep("""/*
 * Portable decisions of mainline drivers/net/ipa's probe-time configuration for IPA v4.2 (ipa_main.c ipa_config ->
 * ipa_hardware_config, ipa_mem.c ipa_mem_config). Register numbers come from the generated coachz_ipa_v42.h.
 * Linux 7.4-rc1 (6628e7d4c6b2); the code is our own.
 */""", """/*
 * nezha (SM8850, IPA v5.5) version of QcomBaseband ipa/coachz_ipa_proto.h (dd0199c, BSD-2-Clause, HoshinoYumeka),
 * derived by tools/baseband27/derive-ipa55.py. Mainline ipa_main.c ipa_hardware_config for v4.5+: no BCR, no TX_CFG or
 * CLKON workaround, COMP_CFG only the in-order disables, QSB for DDR and PCIe, Qtime timers instead of COUNTER_CFG,
 * nothing for hashing (v5.0 caches are on by default), DCD as before. ipa_mem_config with the device's own SRAM layout
 * (cipa_mem from the phone's ipa_bootup log). Endpoints: up to 36 on v5.0+. Resources: eight groups, four register pairs.
 */""")
    e.rep('#include "coachz_ipa_v42.h"\n#include "coachz_ipa_qmi.h"', '#include "nb_ipa_v55.h"\n#include "nb_ipa55_qmi.h"')
    e.rep("""/* sc7180.dtsi &ipa: "ipa-reg", "ipa-shared", "gsi" */
#define CIP_REG_PA     0x01e40000u
#define CIP_REG_BYTES  0x7000u
#define CIP_SHARED_PA  0x01e47000u
#define CIP_SHARED_BYTES 0x2000u   /* ipa->mem_size before SHARED_MEM_SIZE is read */
#define CIP_GSI_PA     0x01e04000u
#define CIP_GSI_BYTES  0x2c000u""", """/* nezha IPA: wrapper 0x03e00000, IPA_CFG at +0x140000 ("ipa-reg"), SRAM after it ("ipa-shared", 0x5000 bytes per
   ipa3_post_init), GSI 0x03e04000/0xfc000 (the phone's ipa_bootup log) */
#define CIP_REG_PA     CIPA_REG_BASE
#define CIP_REG_BYTES  0x10000u
#define CIP_SHARED_PA  CIPA_SRAM_BASE
#define CIP_SHARED_BYTES CIPA_MEM_SIZE   /* ipa->mem_size before SHARED_MEM_SIZE is read */
#define CIP_GSI_PA     CGSI_BASE
#define CIP_GSI_BYTES  CGSI_SIZE""")
    e.rep("""/* ------------------------------------------------------------------ ipa_hardware_config for v4.2, in order */
/* bcr: backward_compat; tx: PA_MASK_EN off (4.0 < v < 4.5); clkon: CLKON_GLOBAL | GLOBAL_2X_CLK (4.0 <= v < 4.5) */
static inline uint32_t cip_bcr(void) { return CIPA_BACKWARD_COMPAT; }
static inline uint32_t cip_tx_cfg(uint32_t reg) { return reg & ~CIPA_IPA_TX_CFG_PA_MASK_EN; }
static inline uint32_t cip_clkon(void) { return CIPA_CLKON_CFG_CLKON_GLOBAL | CIPA_CLKON_CFG_GLOBAL_2X_CLK; }
/* comp: 4.0 < v < 4.5 sets GSI_MULTI_AXI_MASTERS_DIS; every v4.0+ sets the two in-order disables */
static inline uint32_t cip_comp_cfg(uint32_t reg)
{
    return reg | CIPA_COMP_CFG_GSI_MULTI_AXI_MASTERS_DIS | CIPA_COMP_CFG_GSI_MULTI_INORDER_RD_DIS | CIPA_COMP_CFG_GSI_MULTI_INORDER_WR_DIS;
}
/* qsb: one master (DDR) on v4.2 */
static inline uint32_t cip_qsb_writes(void) { return cip_encode(CIPA_QSB_MAX_WRITES_GEN_QMB_0_MAX_WRITES, cipa_qsb[0].max_writes); }
static inline uint32_t cip_qsb_reads(void)
{
    return cip_encode(CIPA_QSB_MAX_READS_GEN_QMB_0_MAX_READS, cipa_qsb[0].max_reads) |
           cip_encode(CIPA_QSB_MAX_READS_GEN_QMB_0_MAX_READS_BEATS, cipa_qsb[0].max_reads_beats);
}
/* timing before v4.5: COUNTER_CFG.AGGR_GRANULARITY = DIV_ROUND_CLOSEST(500 us * 32 kHz, 1 s) - 1 */
#define CIP_AGGR_GRANULARITY_US 500u
#define CIP_TIMER_FREQUENCY 32000u
static inline uint32_t cip_aggr_granularity_val(uint32_t usec)
{
    return (uint32_t)(((uint64_t)usec * CIP_TIMER_FREQUENCY + 500000u) / 1000000u) - 1u;
}
static inline uint32_t cip_counter_cfg(void) { return cip_encode(CIPA_COUNTER_CFG_AGGR_GRANULARITY, cip_aggr_granularity_val(CIP_AGGR_GRANULARITY_US)); }
/* hashing: v4.2 only, FILT_ROUT_HASH_EN = 0; dcd: idle indication, debounce 256, no constant non-idle */""",
          """/* ------------------------------------------------------------------ ipa_hardware_config for v5.5, in order */
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
/* hashing: nothing on v5.0+; dcd: idle indication, debounce 256, no constant non-idle */""")
    e.rep("""/* ipa_mem_size_valid (every region ends within mem_size) and ipa_mem_valid_one for the v4.2 table */
static inline int cip_mem_valid(uint32_t mem_size)
{
    for (unsigned i = 0; i < CI_MEM_COUNT; i++) {
        const struct ci_region *r = &ci_v42_mem[i];
        const uint32_t multiple = i == CI_MODEM ? 4u : 8u;""", """/* ipa_mem_size_valid (every region ends within mem_size) and ipa_mem_valid_one for the nezha table */
static inline int cip_mem_valid(uint32_t mem_size)
{
    for (unsigned i = 0; i < CI_MEM_COUNT; i++) {
        const struct ci_region *r = &ci_nezha_mem[i];
        const uint32_t multiple = i == CI_MODEM ? 4u : 8u;""")
    e.rep("    return mem_offset + ci_v42_mem[region].offset - 4u * (k + 1u);", "    return mem_offset + ci_nezha_mem[region].offset - 4u * (k + 1u);")
    e.rep("""/* ------------------------------------------------------------------ ipa_endpoint_config (v3.5+) */
#define CIP_ENDPOINT_MAX 36u   /* IPA_ENDPOINT_MAX */""", """/* ------------------------------------------------------------------ ipa_endpoint_config (v3.5+) */
#define CIP_ENDPOINT_MAX 36u   /* IPA_ENDPOINT_MAX; the phone reports "IPA Pipes num 36" */""")
    e.rep("""   CIP_ENDPOINT_MAX + 1 when the hardware reports more endpoints than the driver (36) or IPA before v5.0 (32) supports. */
static inline unsigned cip_endpoint_check(uint32_t flavor_reg)
{
    const struct cip_flavor f = cip_flavor(flavor_reg);
    if (f.limit > CIP_ENDPOINT_MAX || f.limit > 32u) return CIP_ENDPOINT_MAX + 1u;""", """   CIP_ENDPOINT_MAX + 1 when the hardware reports more endpoints than the driver supports (36; the 32 limit is pre-v5.0). */
static inline unsigned cip_endpoint_check(uint32_t flavor_reg)
{
    const struct cip_flavor f = cip_flavor(flavor_reg);
    if (f.limit > CIP_ENDPOINT_MAX) return CIP_ENDPOINT_MAX + 1u;""")
    e.rep("""/* ipa_resource_config_common for group pair (2k, 2k+1): Y only when the group count reaches it. The SRC and DST GRP_01/23
   registers share one field layout on v4.2. */""", """/* ipa_resource_config_common for group pair (2k, 2k+1): Y only when the group count reaches it. Every SRC and DST
   GRP_01/23/45/67 register shares one field layout on v5.5 too. */""")
    e.rep("#define CIP_RSRC_DST_TYPES (sizeof cipa_resource_dst / sizeof cipa_resource_dst[0])", """#define CIP_RSRC_DST_TYPES (sizeof cipa_resource_dst / sizeof cipa_resource_dst[0])
/* Register pairs a group count needs: (groups + 1) / 2, up to four on v5.5 (six source, seven destination groups). */
static inline unsigned cip_resource_pairs(unsigned groups) { return (groups + 1u) / 2u; }""")
    open("nb_ipa55_proto.h", "w").write(e.s)


def qmi():
    e = Edit("upstream/coachz_ipa_qmi.h")
    e.rep("""/*
 * The AP/modem IPA QMI handshake of mainline drivers/net/ipa (ipa_qmi.c, ipa_qmi_msg.[ch], data/ipa_data-v4.2.c,
 * Linux 7.4-rc1 6628e7d4c6b2), portable: message encodings, the INIT_DRIVER request built from the IPA v4.2 memory
 * layout, and the "ready" rules. No transport: the caller moves the bytes over the QRTR it already runs. Values and
 * order from Linux; the code is our own.
""", """/*
 * nezha version of QcomBaseband ipa/coachz_ipa_qmi.h (dd0199c, BSD-2-Clause, HoshinoYumeka), derived by
 * tools/baseband27/derive-ipa55.py. The handshake is unchanged. INIT_DRIVER follows the request the phone's own IPA driver
 * sends to this modem firmware (downstream dataipa ipa_qmi_service.c ipa3_qmi_init_modem_send_sync_msg and the TLV
 * numbers of ipa_qmi_service_v01.c): the nezha SRAM layout, the hashed tables 0x1b..0x1e (the phone's log line "IPA
 * filter/route rule hashable = False" prints ipa_fltrt_not_hashable, so hashing is ON: its table init uses 0x608 etc. and the
 * device tree has no qcom,ipa-fltrt-not-hashable), 0x18 only after a modem restart (is_ssr_bootup), drop statistics
 * (0x21/0x22), filter and route statistics (0x23), the IPA SMEM size (0x24) and the peripheral statistics block (0x25). Sizes are plain sizes
 * here (downstream), where mainline adds mem_offset to them; mem_offset is 0 on this device so both agree.
""")
    e.rep("""/* ipa_data-v4.2.c ipa_mem_local_data (offsets within the IPA-local memory) and the constants INIT_DRIVER uses */
enum ci_mem { CI_UC_SHARED, CI_UC_INFO, CI_V4_FILTER, CI_V6_FILTER, CI_V4_ROUTE, CI_V6_ROUTE, CI_MODEM_HEADER,
              CI_MODEM_PROC_CTX, CI_AP_PROC_CTX, CI_PDN_CONFIG, CI_STATS_QUOTA_MODEM, CI_STATS_TETHERING, CI_MODEM, CI_MEM_COUNT };
struct ci_region { uint16_t offset, size; uint8_t canaries; };
static const struct ci_region ci_v42_mem[CI_MEM_COUNT] = {
    { 0x0000, 0x0080, 0 }, { 0x0080, 0x0200, 0 }, { 0x0290, 0x0078, 2 }, { 0x0318, 0x0078, 2 }, { 0x03a0, 0x0078, 2 },
    { 0x0428, 0x0078, 2 }, { 0x04a8, 0x0140, 2 }, { 0x05f0, 0x0200, 2 }, { 0x07f0, 0x0200, 0 }, { 0x09f8, 0x0050, 2 },
    { 0x0a50, 0x0060, 2 }, { 0x0ab0, 0x0140, 0 }, { 0x0bf0, 0x140c, 0 },
};
#define CI_V42_MODEM_ROUTE_COUNT 8u
/* endpoint ids (ipa_data-v4.2.c ipa_gsi_endpoint_data) */
#define CI_EP_AP_MODEM_TX 1u
#define CI_EP_AP_COMMAND_TX 6u
#define CI_EP_AP_LAN_RX 8u
#define CI_EP_AP_MODEM_RX 9u""", """/* The nezha SRAM regions (nb_ipa_v55.h cipa_mem, the phone's ipa_bootup log) under the names this code uses */
#include "nb_ipa_v55.h"
enum ci_mem { CI_UC_SHARED, CI_UC_INFO, CI_V4_FILTER, CI_V6_FILTER, CI_V4_ROUTE, CI_V6_ROUTE, CI_V4_FILTER_HASHED, CI_V6_FILTER_HASHED,
              CI_V4_ROUTE_HASHED, CI_V6_ROUTE_HASHED, CI_MODEM_HEADER, CI_AP_HEADER,
              CI_MODEM_PROC_CTX, CI_AP_PROC_CTX, CI_PDN_CONFIG, CI_STATS_QUOTA_MODEM, CI_STATS_TETHERING, CI_STATS_FILTER_ROUTE,
              CI_STATS_DROP, CI_MODEM, CI_MEM_COUNT };
#define ci_region cipa_region
#define CI_R(name) cipa_mem[CIPA_MEM_##name]
static const struct ci_region ci_nezha_mem[CI_MEM_COUNT] = {
    CI_R(UC_SHARED), CI_R(UC_INFO), CI_R(V4_FILTER), CI_R(V6_FILTER), CI_R(V4_ROUTE), CI_R(V6_ROUTE), CI_R(V4_FILTER_HASHED),
    CI_R(V6_FILTER_HASHED), CI_R(V4_ROUTE_HASHED), CI_R(V6_ROUTE_HASHED), CI_R(MODEM_HEADER),
    CI_R(AP_HEADER), CI_R(MODEM_PROC_CTX), CI_R(AP_PROC_CTX), CI_R(PDN_CONFIG), CI_R(STATS_QUOTA_MODEM), CI_R(STATS_TETHERING),
    CI_R(STATS_FILTER_ROUTE), CI_R(STATS_DROP), CI_R(MODEM),
};
#undef CI_R
#define CI_NEZHA_MODEM_ROUTE_COUNT CIPA_MODEM_ROUTE_COUNT
/* endpoint ids (nb_ipa_v55.h cipa_endpoints) */
#define CI_EP_AP_MODEM_TX 2u
#define CI_EP_AP_COMMAND_TX 14u
#define CI_EP_AP_LAN_RX 17u
#define CI_EP_AP_MODEM_RX 24u
/* downstream ipa_i.h / ipa_qmi_service.c */
#define CI_FNR_START_INDEX 64u
#define CI_FNR_INDEX_COUNT 191u
#define CI_FNR_STATS_SIZE (CI_FNR_INDEX_COUNT * 16u)
#define CI_PER_STATS_SMEM_SIZE 0x800u""")
    e.rep("""/* init_modem_driver_req: every field the v4.2 layout defines, in ipa_init_modem_driver_req_ei order. */
static inline size_t ci_init_driver_req(uint8_t *out, size_t room, uint16_t txn, uint32_t mem_offset, int uc_loaded)
{
    struct ci_buf b = { out, room, 0, 0 };
    const struct ci_region *m = ci_v42_mem;""", """/* init_modem_driver_req in TLV order. smem_size: the SMEM_IPA_FILTER_TABLE item; per_stats_addr: its address as the IPA
   sees it (the item is mapped 1:1 in the IPA's context bank), 0 to leave the block out. */
static inline size_t ci_init_driver_req(uint8_t *out, size_t room, uint16_t txn, uint32_t mem_offset, int uc_loaded,
                                        uint32_t smem_size, uint32_t per_stats_addr)
{
    struct ci_buf b = { out, room, 0, 0 };
    const struct ci_region *m = ci_nezha_mem;""")
    e.rep("""    ci_tlv_pair(&b, 0x12, mem_offset + m[CI_V4_ROUTE].offset, CI_V42_MODEM_ROUTE_COUNT - 1u);
    ci_tlv_pair(&b, 0x13, mem_offset + m[CI_V6_ROUTE].offset, CI_V42_MODEM_ROUTE_COUNT - 1u);""", """    ci_tlv_pair(&b, 0x12, mem_offset + m[CI_V4_ROUTE].offset, CI_NEZHA_MODEM_ROUTE_COUNT - 1u);
    ci_tlv_pair(&b, 0x13, mem_offset + m[CI_V6_ROUTE].offset, CI_NEZHA_MODEM_ROUTE_COUNT - 1u);""")
    e.rep("""    ci_tlv_u8(&b, 0x18, uc_loaded ? 1u : 0u);""", """    if (uc_loaded) ci_tlv_u8(&b, 0x18, 1u);   /* is_ssr_bootup: only sent (as 1) after a modem restart */""")
    e.rep("""    /* no compression table, no hashed route/filter tables on v4.2 */
    ci_tlv_u32(&b, 0x1f, mem_offset + m[CI_STATS_QUOTA_MODEM].offset);
    ci_tlv_u32(&b, 0x20, mem_offset + m[CI_STATS_QUOTA_MODEM].size);   /* sic: mainline adds mem_offset to the size */
    /* no STATS_DROP region on v4.2 */
    return ci_finish(&b);""", """    /* no compression table; the hashed tables, as ipa3_qmi_init_modem_send_sync_msg sends them when hashing is on */
    ci_tlv_pair(&b, 0x1b, mem_offset + m[CI_V4_ROUTE_HASHED].offset, CI_NEZHA_MODEM_ROUTE_COUNT - 1u);
    ci_tlv_pair(&b, 0x1c, mem_offset + m[CI_V6_ROUTE_HASHED].offset, CI_NEZHA_MODEM_ROUTE_COUNT - 1u);
    ci_tlv_u32(&b, 0x1d, mem_offset + m[CI_V4_FILTER_HASHED].offset);
    ci_tlv_u32(&b, 0x1e, mem_offset + m[CI_V6_FILTER_HASHED].offset);
    ci_tlv_u32(&b, 0x1f, mem_offset + m[CI_STATS_QUOTA_MODEM].offset);
    ci_tlv_u32(&b, 0x20, m[CI_STATS_QUOTA_MODEM].size);
    ci_tlv_u32(&b, 0x21, mem_offset + m[CI_STATS_DROP].offset);
    ci_tlv_u32(&b, 0x22, m[CI_STATS_DROP].size);
    ci_u8(&b, 0x23); ci_u16(&b, 10);   /* ipa_filter_stats_info_type_v01 */
    ci_u32(&b, mem_offset + m[CI_STATS_FILTER_ROUTE].offset); ci_u32(&b, CI_FNR_STATS_SIZE);
    ci_u8(&b, (uint8_t)CI_FNR_START_INDEX); ci_u8(&b, (uint8_t)(CI_FNR_START_INDEX + CI_FNR_INDEX_COUNT - 1u));
    ci_tlv_pair(&b, 0x24, 0, smem_size);   /* ipa_modem_mem_info_type_v01: start unset, size */
    if (per_stats_addr) ci_tlv_pair(&b, 0x25, per_stats_addr, CI_PER_STATS_SMEM_SIZE);
    return ci_finish(&b);""")
    e.rep("#define CI_INIT_DRIVER_REQ_MAX 162u", "#define CI_INIT_DRIVER_REQ_MAX 240u")
    open("nb_ipa55_qmi.h", "w").write(e.s)


def cmd():
    e = Edit("upstream/coachz_ipa_cmd_proto.h")
    e.rep(""" * Portable decisions of mainline drivers/net/ipa for the middle of ipa_setup on IPA v4.2: ipa_endpoint_program's register
 * values for the AP endpoints, the GSI TRE format (gsi_trans.c), the immediate command payloads (ipa_cmd.c), ipa_cmd_init's
 * checks, and the command lists of ipa_mem_setup and ipa_table_setup with the table area ipa_table_init builds.
 * Register numbers come from the generated coachz_ipa_v42.h. Linux 7.4-rc1 (6628e7d4c6b2); the code is our own.
 */""", """ * nezha (SM8850, IPA v5.5) version of QcomBaseband ipa/coachz_ipa_cmd_proto.h (dd0199c, BSD-2-Clause, HoshinoYumeka), derived
 * by tools/baseband27/derive-ipa55.py. Mainline version branches followed (ipa_endpoint.c, ipa_cmd.c, ipa_table.c, ipa_mem.c):
 *   - aggregation time limit in Qtime ticks with AGGR_GRAN_SEL (v4.5+); no SEQ_REP_TYPE (v4.5+);
 *   - checksum offload mode "inline" with one 8-byte header in both directions when enabled (v4.5+; the first phase runs
 *     without offload);
 *   - IP_PACKET_INIT takes a full 8-bit endpoint (v5.0+); the filter bitmap is not shifted (v5.0+);
 *   - header memory covers the modem and AP header regions; every table init also carries the hashed half.
 * The filter bitmap is the device's own (0x94e6 in the phone's log), which also names endpoints this driver does not use.
 */""")
    e.rep('#include "coachz_gsi_proto.h"', '#include "nb_gsi55_proto.h"')
    e.rep("/* ------------------------------------------------------------------ ipa_endpoint_program (v4.2, AP endpoints) */",
          "/* ------------------------------------------------------------------ ipa_endpoint_program (v5.5, AP endpoints) */")
    e.rep("#define CIC_AGGR_GRANULARITY_US 500u\n", "")
    e.rep("#define CIC_QMAP_UL_CSUM 4u       /* struct rmnet_map_ul_csum_header */",
          "#define CIC_QMAP_V5_CSUM 8u       /* struct rmnet_map_v5_csum_header, both directions */\n"
          "#define CIC_CS_OFFLOAD_INLINE 1u  /* IPA_CS_OFFLOAD_INLINE */")
    e.rep("HOL_BLOCK: no v4.2 AP RX\n   endpoint has holb_drop", "HOL_BLOCK: no nezha AP RX\n   endpoint has holb_drop")
    e.rep("""        if (e->toward_ipa)
            v = cip_encode(CIPA_ENDP_INIT_CFG_CS_METADATA_HDR_OFFSET, CIC_QMAP_HEADER / 4u) | cip_encode(CIPA_ENDP_INIT_CFG_CS_OFFLOAD_EN, 1);
        else
            v = cip_encode(CIPA_ENDP_INIT_CFG_CS_OFFLOAD_EN, 2);""",
          """        if (e->toward_ipa) v = cip_encode(CIPA_ENDP_INIT_CFG_CS_METADATA_HDR_OFFSET, CIC_QMAP_HEADER / 4u);
        v |= cip_encode(CIPA_ENDP_INIT_CFG_CS_OFFLOAD_EN, CIC_CS_OFFLOAD_INLINE);""")
    e.rep("const uint32_t size = CIC_QMAP_HEADER + (e->checksum && e->toward_ipa ? CIC_QMAP_UL_CSUM : 0u);",
          "const uint32_t size = CIC_QMAP_HEADER + (e->checksum ? CIC_QMAP_V5_CSUM : 0u);   /* below the HDR_LEN_MSB range */")
    e.rep("""            if (e->rx_aggr_time_limit)
                v |= cip_encode(CIPA_ENDP_INIT_AGGR_TIME_LIMIT, (e->rx_aggr_time_limit + CIC_AGGR_GRANULARITY_US / 2u) / CIC_AGGR_GRANULARITY_US);""",
          """            if (e->rx_aggr_time_limit) {   /* aggr_time_limit_encode */
                uint32_t select;
                const uint32_t ticks = cip_qtime_val(e->rx_aggr_time_limit,
                    CIPA_ENDP_INIT_AGGR_TIME_LIMIT >> __builtin_ctz(CIPA_ENDP_INIT_AGGR_TIME_LIMIT), &select);
                v |= cip_encode(CIPA_ENDP_INIT_AGGR_AGGR_GRAN_SEL, select) | cip_encode(CIPA_ENDP_INIT_AGGR_TIME_LIMIT, ticks);
            }""")
    e.rep("CIC_W(ENDP_INIT_SEQ, cip_encode(CIPA_ENDP_INIT_SEQ_SEQ_TYPE, e->tx_seq_type) | cip_encode(CIPA_ENDP_INIT_SEQ_SEQ_REP_TYPE, e->tx_seq_rep_type));",
          "CIC_W(ENDP_INIT_SEQ, cip_encode(CIPA_ENDP_INIT_SEQ_SEQ_TYPE, e->tx_seq_type));")
    e.rep("""/* ipa_cmd_table_init_add: hash rules address, flags {hash size 11:0, hash addr 27:12, nhash size 39:28, nhash addr 55:40},
   non-hash rules address; v4.2 has no hashed tables, so the hash half is zero */
static inline struct cic_cmd cic_table_init(uint32_t opcode, uint32_t mem_offset, uint32_t offset, uint32_t size, uint64_t addr)
{
    struct cic_cmd c = { opcode, 24, { 0 } };
    cic_le64(c.payload + 8, (uint64_t)(size & 0xfffu) << 28 | (uint64_t)((mem_offset + offset) & 0xffffu) << 40);
    cic_le64(c.payload + 16, addr);
    return c;
}""", """/* ipa_cmd_table_init_add: hash rules address, flags {hash size 11:0, hash addr 27:12, nhash size 39:28, nhash addr 55:40},
   non-hash rules address */
static inline struct cic_cmd cic_table_init(uint32_t opcode, uint32_t mem_offset, uint32_t offset, uint32_t size, uint64_t addr,
                                            uint32_t hash_offset, uint32_t hash_size, uint64_t hash_addr)
{
    struct cic_cmd c = { opcode, 24, { 0 } };
    cic_le64(c.payload, hash_addr);
    cic_le64(c.payload + 8, (uint64_t)(hash_size & 0xfffu) | (uint64_t)((mem_offset + hash_offset) & 0xffffu) << 12 |
                            (uint64_t)(size & 0xfffu) << 28 | (uint64_t)((mem_offset + offset) & 0xffffu) << 40);
    cic_le64(c.payload + 16, addr);
    return c;
}""")
    e.rep("c.payload[0] = (uint8_t)(endpoint & 0x1fu);", "c.payload[0] = (uint8_t)endpoint;   /* all 8 bits on v5.0+ */")
    e.all("ci_v42_mem", "ci_nezha_mem")
    e.rep("""    if (h->offset > 0xffffu || mem_offset > 0xffffu - h->offset || h->size > 0xfffu) return 0;
    const uint32_t status = CIPA_ENDP_STATUS + (CIPA_EP_COUNT - 1u) * CIPA_ENDP_STATUS_STRIDE, max = 0xfffffu;""",
          """    if (h->offset > 0xffffu || mem_offset > 0xffffu - h->offset || h->size + ci_nezha_mem[CI_AP_HEADER].size > 0xfffu) return 0;
    const uint32_t status = CIPA_ENDP_STATUS + (CIP_ENDPOINT_MAX - 1u) * CIPA_ENDP_STATUS_STRIDE, max = 0xfffffu;""")
    e.rep("""/* Header memory (the modem's; v4.2 has no AP header region) from the zero buffer, then zero the two processing context
   regions""", """/* Header memory (the modem region and the AP region after it, initialized together) from the zero buffer, then zero the two
   processing context regions""")
    e.rep("out[n++] = cic_hdr_init_local(mem_offset, ci_nezha_mem[CI_MODEM_HEADER].offset, ci_nezha_mem[CI_MODEM_HEADER].size, zero_addr);",
          "out[n++] = cic_hdr_init_local(mem_offset, ci_nezha_mem[CI_MODEM_HEADER].offset,\n"
          "                                  ci_nezha_mem[CI_MODEM_HEADER].size + ci_nezha_mem[CI_AP_HEADER].size, zero_addr);")
    e.rep("""/* filtered: endpoints with filter support (AP and modem), from ipa_endpoint_init */
static inline uint64_t cic_filtered(void)
{
    uint64_t f = 0;
    for (unsigned i = 0; i < sizeof cipa_endpoints / sizeof cipa_endpoints[0]; i++)
        if (cip_endpoint_defined(&cipa_endpoints[i]) && cipa_endpoints[i].filter) f |= 1ull << cipa_endpoints[i].endpoint;
    return f;
}""", """/* filtered: the endpoints the device filters on, as its own driver reports them (ipa_flt_bitmap in the phone's log) */
static inline uint64_t cic_filtered(void) { return CIPA_FILTER_MAP; }""")
    e.rep("""/* ipa_table_mem_valid for v4.2 (no hashing) */""", """/* ipa_table_mem_valid: the hashed tables have the sizes of the others */""")
    e.rep("""    if (f4->size != f6->size || r4->size != r6->size || f4->size / 8u < 2u || r4->size / 8u < 2u) return 0;""",
          """    if (f4->size != f6->size || r4->size != r6->size || f4->size / 8u < 2u || r4->size / 8u < 2u) return 0;
    if (ci_nezha_mem[CI_V4_FILTER_HASHED].size != f4->size || ci_nezha_mem[CI_V6_FILTER_HASHED].size != f6->size ||
        ci_nezha_mem[CI_V4_ROUTE_HASHED].size != r4->size || ci_nezha_mem[CI_V6_ROUTE_HASHED].size != r6->size) return 0;""")
    e.rep("/* The table area: the zero rule, the filter bitmap (shifted left one before v5.0), then every entry the zero rule's address.",
          "/* The table area: the zero rule, the filter bitmap (not shifted on v5.0+), then every entry the zero rule's address.")
    e.rep("cic_le64(table + 8, cic_filtered() << 1);", "cic_le64(table + 8, cic_filtered());")
    e.rep("""/* ipa_table_setup: per table (v4 filter, v6 filter, v4 route, v6 route) an init command; filter tables also zero the rest
   of their region. Filter tables start at the bitmap (skip 1), routes at the first zero-rule pointer (skip 2). */""",
          """/* ipa_table_setup: per table (v4 filter, v6 filter, v4 route, v6 route) an init command for both halves; filter tables also
   zero the rest of both regions (at most eight commands). Filter tables start at the bitmap (skip 1), routes at the first
   zero-rule pointer (skip 2). */""")
    e.rep("""    static const struct { uint32_t opcode; unsigned region; int filter; } t[4] = {
        { CIC_CMD_IP_V4_FILTER_INIT, CI_V4_FILTER, 1 }, { CIC_CMD_IP_V6_FILTER_INIT, CI_V6_FILTER, 1 },
        { CIC_CMD_IP_V4_ROUTING_INIT, CI_V4_ROUTE, 0 }, { CIC_CMD_IP_V6_ROUTING_INIT, CI_V6_ROUTE, 0 },
    };""", """    static const struct { uint32_t opcode; unsigned region, hashed; int filter; } t[4] = {
        { CIC_CMD_IP_V4_FILTER_INIT, CI_V4_FILTER, CI_V4_FILTER_HASHED, 1 }, { CIC_CMD_IP_V6_FILTER_INIT, CI_V6_FILTER, CI_V6_FILTER_HASHED, 1 },
        { CIC_CMD_IP_V4_ROUTING_INIT, CI_V4_ROUTE, CI_V4_ROUTE_HASHED, 0 }, { CIC_CMD_IP_V6_ROUTING_INIT, CI_V6_ROUTE, CI_V6_ROUTE_HASHED, 0 },
    };""")
    e.rep("""        out[n++] = cic_table_init(t[i].opcode, mem_offset, m->offset, size, table_addr + (t[i].filter ? 8u : 16u));
        if (t[i].filter) out[n++] = cic_dma_shared_mem(mem_offset, m->offset + size, m->size - size, zero_addr, 1);""",
          """        const struct ci_region *h = &ci_nezha_mem[t[i].hashed];
        const uint64_t addr = table_addr + (t[i].filter ? 8u : 16u);
        out[n++] = cic_table_init(t[i].opcode, mem_offset, m->offset, size, addr, h->offset, size, addr);
        if (!t[i].filter) continue;
        out[n++] = cic_dma_shared_mem(mem_offset, m->offset + size, m->size - size, zero_addr, 1);
        out[n++] = cic_dma_shared_mem(mem_offset, h->offset + size, h->size - size, zero_addr, 1);""")
    e.rep("#ifndef COACHZ_IPA_CMD_PROTO_H\n#define COACHZ_IPA_CMD_PROTO_H", "#ifndef NB_IPA55_CMD_PROTO_H\n#define NB_IPA55_CMD_PROTO_H")
    e.rep("#endif /* COACHZ_IPA_CMD_PROTO_H */", "#endif /* NB_IPA55_CMD_PROTO_H */")
    open("nb_ipa55_cmd_proto.h", "w").write(e.s)


def rx():
    e = Edit("upstream/coachz_ipa_rx_proto.h")
    e.rep(""" * Portable decisions of mainline drivers/net/ipa for an AP RX endpoint on IPA v4.2 (AP_LAN_RX first): buffer and TRE layout""",
          """ * nezha (SM8850, IPA v5.5) version of QcomBaseband ipa/coachz_ipa_rx_proto.h (dd0199c, BSD-2-Clause, HoshinoYumeka), derived
 * by tools/baseband27/derive-ipa55.py: the v5.0+ packet status layout, and the pad after a packet is 1 << pad_align bytes
 * (HDR_PAD_TO_ALIGNMENT is a power of two; downstream ipa_dp.c rounds to 4 for the value 2 this endpoint uses, where
 * mainline's ALIGN(length, 2) would misplace every later status in an aggregated buffer).
 *
 * Portable decisions of mainline drivers/net/ipa for an AP RX endpoint (AP_LAN_RX first): buffer and TRE layout""")
    e.rep('#include "coachz_ipa_cmd_proto.h"', '#include "nb_ipa55_cmd_proto.h"')
    e.rep("#ifndef COACHZ_IPA_RX_PROTO_H\n#define COACHZ_IPA_RX_PROTO_H", "#ifndef NB_IPA55_RX_PROTO_H\n#define NB_IPA55_RX_PROTO_H")
    e.rep("#endif /* COACHZ_IPA_RX_PROTO_H */", "#endif /* NB_IPA55_RX_PROTO_H */")
    e.rep("ipa_status_extract, before v5.0 */", "ipa_status_extract, v5.0+ */")
    e.rep("""    const uint32_t w0 = cic_rd32(p), w1 = cic_rd32(p + 4), w3 = cic_rd32(p + 12);""",
          """    const uint32_t w0 = cic_rd32(p), w1 = cic_rd32(p + 4), w3 = cic_rd32(p + 12), w7 = cic_rd32(p + 28);""")
    e.rep("""    s.src = (w1 >> 16) & 0x1fu;
    s.dst = (w1 >> 24) & 0x1fu;""", """    s.src = (w1 >> 16) & 0xffu;
    s.dst = (w7 >> 16) & 0xffu;""")
    e.rep("const uint32_t align = e->rx_pad_align ? e->rx_pad_align : 1u;", "const uint32_t align = 1u << e->rx_pad_align;")
    open("nb_ipa55_rx_proto.h", "w").write(e.s)


def modem():
    e = Edit("upstream/coachz_ipa_modem_proto.h")
    e.rep(""" * Portable decisions of the cellular data path on IPA v4.2, as mainline Linux runs it:""",
          """ * nezha (SM8850, IPA v5.5) version of QcomBaseband ipa/coachz_ipa_modem_proto.h (dd0199c, BSD-2-Clause, HoshinoYumeka),
 * derived by tools/baseband27/derive-ipa55.py.
 *
 * Portable decisions of the cellular data path, as mainline Linux runs it:""")
    e.rep(""" * Data format: the IPA endpoints fix it. AP_MODEM_TX has checksum offload with an 8-byte header (QMAP header plus the UL
 * checksum header) and AP_MODEM_RX has checksum offload (each packet followed by the 8-byte DL checksum trailer), which is
 * rmnet's MAPv4: INGRESS_DEAGGREGATION | INGRESS_MAP_CKSUMV4 | EGRESS_MAP_CKSUMV4. The UL checksum header is written as
 * zeros (rmnet's software-checksum path, when the stack has already computed the checksums), and the DL trailer is ignored
 * (rmnet without NETIF_F_RXCSUM), so the network stack verifies checksums itself.
""", """ * Data format: the IPA endpoints fix it. The first phase runs AP_MODEM_TX and AP_MODEM_RX without checksum offload, so a
 * frame is the 4-byte QMAP header and the IP packet in both directions (rmnet with only INGRESS_DEAGGREGATION) and the
 * network stack computes and verifies checksums. WDA must negotiate the same format with the modem. The v5 inline checksum
 * header (8 bytes, both directions) is a later step.
""")
    e.rep('#include "coachz_ipa_rx_proto.h"', '#include "nb_ipa55_rx_proto.h"')
    e.rep("#ifndef COACHZ_IPA_MODEM_PROTO_H\n#define COACHZ_IPA_MODEM_PROTO_H", "#ifndef NB_IPA55_MODEM_PROTO_H\n#define NB_IPA55_MODEM_PROTO_H")
    e.rep("#endif /* COACHZ_IPA_MODEM_PROTO_H */", "#endif /* NB_IPA55_MODEM_PROTO_H */")
    e.rep("#define CIM_DATA_FORMAT (CIM_INGRESS_DEAGGREGATION | CIM_INGRESS_MAP_CKSUMV4 | CIM_EGRESS_MAP_CKSUMV4)",
          "#define CIM_DATA_FORMAT CIM_INGRESS_DEAGGREGATION   /* no checksum offload in the first phase */")
    e.rep("/* The modem endpoints must match the format: QMAP on both, checksum offload on both (MAPv4), status on neither RX side */",
          "/* The modem endpoints must match the format: QMAP on both, checksum offload as the format says, no status on the RX side */")
    open("nb_ipa55_modem_proto.h", "w").write(e.s)


def kernel():
    e = Edit("upstream/coachz_ipa_kernel.h")
    e.rep("/* Kernel-side state shared by the pending IPA files (coachz_ipa_config.c, coachz_gsi.c, coachz_ipa_cmd.c). */",
          "/* Kernel-side state shared by the nezha IPA files, derived from QcomBaseband ipa/coachz_ipa_kernel.h (dd0199c) by\n"
          "   tools/baseband27/derive-ipa55.py. Addresses named phys are the ones the IPA uses: IOVAs of its SMMU context bank here. */")
    e.rep("#ifndef COACHZ_IPA_KERNEL_H\n#define COACHZ_IPA_KERNEL_H", "#ifndef NB_IPA55_KERNEL_H\n#define NB_IPA55_KERNEL_H")
    e.rep('#include "coachz_gsi_proto.h"', '#include "nb_gsi55_proto.h"')
    e.rep("    uint32_t suspend_enabled;  /* ipa_interrupt.suspend_enabled: endpoints with TX_SUSPEND on (one 32-endpoint unit on v4.2) */",
          "    uint64_t suspend_enabled;  /* ipa_interrupt.suspend_enabled: endpoints with TX_SUSPEND on (two 32-endpoint units on v5.5) */")
    e.rep("    int (*qmp_send)(const char *text);   /* the AOSS QMP sender (the Wi-Fi patch's wifi_qmp_send) */",
          "    int (*qmp_send)(const char *text);   /* the AOSS QMP sender (the shared QMP owner) */")
    e.rep("    vm_offset_t virt;                       /* the \"gsi\" region, 0x01e04000 */", "    vm_offset_t virt;                       /* the \"gsi\" region, 0x03e04000 */")
    open("nb_ipa55_kernel.h", "w").write(e.s)


def config_c():
    e = Edit("upstream/coachz_ipa_config.c")
    e.rep(""" * IPA v4.2 probe-time configuration: mainline ipa_config (ipa_main.c) in its order. Not in the kernel build.
 *   ipa_hardware_config, ipa_mem_config, ipa_interrupt_config, ipa_uc_config, ipa_endpoint_config, ipa_table_config,
 *   ipa_resource_config, ipa_modem_config.
 * ipa_table_config does nothing on v4.2 (ipa_table_hash_support is false for 4.2 only). ipa_modem_config registers an
 * MPSS SSR notifier; here the integrator calls coachz_ipa_modem_starting() where it powers the modem up, which is what the
 * notifier's QCOM_SSR_BEFORE_POWERUP does (ipa_uc_power, then ipa_smp2p_notify_reset on the SMP2P side).
 * The IPA core clock and interconnects must already be on (coachz-prelink-wifi.patch wifi_ipa_power). The apps SMMU is in
 * bypass (sCR0 CLIENTPD, as the Wi-Fi patch reads), so the IPA sees physical addresses.
 * Interrupts: "ipa" is GIC SPI 311 edge (sc7180.dtsi). coachz_ipa_interrupt() is ipa_isr_thread's body; call it from that
 * interrupt or from a poll.
 */""", """ * nezha (SM8850, IPA v5.5) probe-time configuration, derived from QcomBaseband ipa/coachz_ipa_config.c (dd0199c, BSD-2-Clause,
 * HoshinoYumeka) by tools/baseband27/derive-ipa55.py: mainline ipa_config (ipa_main.c) in its order.
 *   ipa_hardware_config, ipa_mem_config, ipa_interrupt_config, ipa_uc_config, ipa_endpoint_config, ipa_table_config,
 *   ipa_resource_config, ipa_modem_config.
 * ipa_table_config zeroes the hash tuples of the AP's filter endpoints and route tables; the modem's stay untouched, as the
 * phone's own driver leaves them (ipa3_setup_flt_hash_tuple, ipa3_setup_rt_hash_tuple skip modem pipes and indexes).
 * The board calls coachz_ipa_modem_starting() where it powers the modem up (ipa_uc_power on QCOM_SSR_BEFORE_POWERUP).
 * The IPA core clock and interconnects must already be on, and the board has mapped the DMA memory in the IPA's SMMU context bank.
 * Interrupts: "ipa" is GIC SPI 655, level. coachz_ipa_interrupt() is ipa_isr_thread's body; call it from that interrupt or a poll.
 */""")
    e.rep('#include "coachz_ipa_kernel.h"', '#include "nb_ipa55_kernel.h"')
    e.rep("""/* ipa_hardware_config: bcr, tx, clkon, comp, qsb, timing (counter), hashing, dcd */
static void hardware_config(struct coachz_ipa *ipa)
{
    wr(ipa->reg, CIPA_IPA_BCR, cip_bcr());
    wr(ipa->reg, CIPA_IPA_TX_CFG, cip_tx_cfg(rd(ipa->reg, CIPA_IPA_TX_CFG)));
    wr(ipa->reg, CIPA_CLKON_CFG, cip_clkon());
    wr(ipa->reg, CIPA_COMP_CFG, cip_comp_cfg(rd(ipa->reg, CIPA_COMP_CFG)));""",
          """/* ipa_hardware_config for v5.5: comp, qsb, timing (Qtime), dcd; bcr, tx, clkon and hashing have nothing to do */
static void hardware_config(struct coachz_ipa *ipa)
{
    wr(ipa->reg, CIPA_COMP_CFG, cip_comp_cfg(rd(ipa->reg, CIPA_COMP_CFG)));""")
    e.rep("""    wr(ipa->reg, CIPA_COUNTER_CFG, cip_counter_cfg());
    wr(ipa->reg, CIPA_FILT_ROUT_HASH_EN, 0);
""", """    wr(ipa->reg, CIPA_TIMERS_XO_CLK_DIV_CFG, 0);   /* the divider is off while the rates change */
    wr(ipa->reg, CIPA_QTIME_TIMESTAMP_CFG, cip_qtime_timestamp());
    wr(ipa->reg, CIPA_TIMERS_PULSE_GRAN_CFG, cip_pulse_gran());
    wr(ipa->reg, CIPA_TIMERS_XO_CLK_DIV_CFG, cip_xo_div(0));
    wr(ipa->reg, CIPA_TIMERS_XO_CLK_DIV_CFG, cip_xo_div(1));
""")
    e.rep('printf("COACHZ ipa: a v4.2 memory region ends beyond the memory limit\\n");', 'printf("COACHZ ipa: a memory region ends beyond the memory limit\\n");')
    e.all("ci_v42_mem", "ci_nezha_mem")
    e.rep("""    __builtin_memset(ipa->zero_virt, 0, CIP_MEM_MAX);
    return 0;""", """    /* IPA v5.5 _ipa_init_sram_v3: publish the uC descriptor marker
     * after the canaries. Its region is separate from uC command/shared RAM. */
    if (!cipa_mem[CIPA_MEM_UC_DESC_RAM].size ||
        cipa_mem[CIPA_MEM_UC_DESC_RAM].offset + 4u > ipa->mem_size)
        return -EINVAL;
    const uint32_t descriptor = ipa->mem_offset + cipa_mem[CIPA_MEM_UC_DESC_RAM].offset;
    wr(ipa->shared, descriptor, 0xabcdef01u);
    printf("COACHZ ipa: uC descriptor marker 0x%x at 0x%x\\n", rd(ipa->shared, descriptor), descriptor);
    __builtin_memset(ipa->zero_virt, 0, CIP_MEM_MAX);
    return 0;""")
    e.rep("""/* ipa_resource_config: v4.2 has one source and one destination group, so only GRP_01's X limits are written */
static int resource_config(struct coachz_ipa *ipa)
{""", """/* ipa_table_config: no hashing keys on the AP's filter endpoints and route tables; the modem's are the modem's */
static void table_config(struct coachz_ipa *ipa)
{
    for (uint32_t id = 0; id < CIP_ENDPOINT_MAX; id++) {
        if (!(CIPA_FILTER_MAP >> id & 1u)) continue;
        int modem = 0;
        for (unsigned i = 0; i < sizeof cipa_endpoints / sizeof cipa_endpoints[0]; i++)
            if (cipa_endpoints[i].ee == CGS_EE_MODEM && cipa_endpoints[i].endpoint == id) modem = 1;
        if (!modem) wr(ipa->reg, CIPA_ENDP_FILTER_CACHE_CFG + id * CIPA_ENDP_FILTER_CACHE_CFG_STRIDE, 0);
    }
    for (uint32_t route = CIPA_MODEM_ROUTE_COUNT; route < CIPA_ROUTE_COUNT; route++)
        wr(ipa->reg, CIPA_ENDP_ROUTER_CACHE_CFG + route * CIPA_ENDP_ROUTER_CACHE_CFG_STRIDE, 0);
}

/* ipa_resource_config: six source and seven destination groups, so all four register pairs */
static int resource_config(struct coachz_ipa *ipa)
{
    static const uint32_t src[4] = { CIPA_SRC_RSRC_GRP_01_RSRC_TYPE, CIPA_SRC_RSRC_GRP_23_RSRC_TYPE, CIPA_SRC_RSRC_GRP_45_RSRC_TYPE, CIPA_SRC_RSRC_GRP_67_RSRC_TYPE };
    static const uint32_t dst[4] = { CIPA_DST_RSRC_GRP_01_RSRC_TYPE, CIPA_DST_RSRC_GRP_23_RSRC_TYPE, CIPA_DST_RSRC_GRP_45_RSRC_TYPE, CIPA_DST_RSRC_GRP_67_RSRC_TYPE };""")
    e.rep("""    for (unsigned t = 0; t < CIP_RSRC_SRC_TYPES; t++) {
        wr(ipa->reg, CIPA_SRC_RSRC_GRP_01_RSRC_TYPE + t * CIPA_SRC_RSRC_GRP_01_RSRC_TYPE_STRIDE,
            cip_resource_value(cipa_resource_src[t], 0, CIPA_RSRC_GROUP_SRC_COUNT));
        if (CIPA_RSRC_GROUP_SRC_COUNT >= 3)
            wr(ipa->reg, CIPA_SRC_RSRC_GRP_23_RSRC_TYPE + t * CIPA_SRC_RSRC_GRP_23_RSRC_TYPE_STRIDE,
                cip_resource_value(cipa_resource_src[t], 1, CIPA_RSRC_GROUP_SRC_COUNT));
    }
    for (unsigned t = 0; t < CIP_RSRC_DST_TYPES; t++) {
        wr(ipa->reg, CIPA_DST_RSRC_GRP_01_RSRC_TYPE + t * CIPA_DST_RSRC_GRP_01_RSRC_TYPE_STRIDE,
            cip_resource_value(cipa_resource_dst[t], 0, CIPA_RSRC_GROUP_DST_COUNT));
        if (CIPA_RSRC_GROUP_DST_COUNT >= 3)
            wr(ipa->reg, CIPA_DST_RSRC_GRP_23_RSRC_TYPE + t * CIPA_DST_RSRC_GRP_23_RSRC_TYPE_STRIDE,
                cip_resource_value(cipa_resource_dst[t], 1, CIPA_RSRC_GROUP_DST_COUNT));
    }
    return 0;""", """    for (unsigned t = 0; t < CIP_RSRC_SRC_TYPES; t++)
        for (unsigned p = 0; p < cip_resource_pairs(CIPA_RSRC_GROUP_SRC_COUNT); p++)
            wr(ipa->reg, src[p] + t * CIPA_SRC_RSRC_GRP_01_RSRC_TYPE_STRIDE, cip_resource_value(cipa_resource_src[t], p, CIPA_RSRC_GROUP_SRC_COUNT));
    for (unsigned t = 0; t < CIP_RSRC_DST_TYPES; t++)
        for (unsigned p = 0; p < cip_resource_pairs(CIPA_RSRC_GROUP_DST_COUNT); p++)
            wr(ipa->reg, dst[p] + t * CIPA_DST_RSRC_GRP_01_RSRC_TYPE_STRIDE, cip_resource_value(cipa_resource_dst[t], p, CIPA_RSRC_GROUP_DST_COUNT));
    return 0;""")
    e.rep("""    if (!err) err = resource_config(ipa);   /* ipa_table_config: nothing on v4.2 */""",
          """    if (!err) table_config(ipa);
    if (!err) err = resource_config(ipa);""")
    e.rep("""    printf("COACHZ ipa: configured (COMP_CFG 0x%x CLKON 0x%x QSB 0x%x/0x%x COUNTER 0x%x IRQ_EN 0x%x)\\n",
        rd(ipa->reg, CIPA_COMP_CFG), rd(ipa->reg, CIPA_CLKON_CFG), rd(ipa->reg, CIPA_QSB_MAX_WRITES),
        rd(ipa->reg, CIPA_QSB_MAX_READS), rd(ipa->reg, CIPA_COUNTER_CFG), rd(ipa->reg, CIPA_IPA_IRQ_EN));""",
          """    printf("COACHZ ipa: configured (COMP_CFG 0x%x QSB 0x%x/0x%x QTIME 0x%x GRAN 0x%x XO 0x%x IRQ_EN 0x%x)\\n",
        rd(ipa->reg, CIPA_COMP_CFG), rd(ipa->reg, CIPA_QSB_MAX_WRITES), rd(ipa->reg, CIPA_QSB_MAX_READS),
        rd(ipa->reg, CIPA_QTIME_TIMESTAMP_CFG), rd(ipa->reg, CIPA_TIMERS_PULSE_GRAN_CFG),
        rd(ipa->reg, CIPA_TIMERS_XO_CLK_DIV_CFG), rd(ipa->reg, CIPA_IPA_IRQ_EN));""")
    e.rep("if (id == CIP_IRQ_TX_SUSPEND)   /* ipa_interrupt_suspend_clear_all: one 32-endpoint unit on v4.2 */",
          "if (id == CIP_IRQ_TX_SUSPEND)   /* ipa_interrupt_suspend_clear_all */")
    e.rep("""    const uint32_t weight = (uint32_t)__builtin_popcount(ipa->suspend_enabled);""",
          """    const uint32_t weight = (uint32_t)__builtin_popcountll(ipa->suspend_enabled);""")
    e.rep("""    ipa->suspend_enabled = enable ? ipa->suspend_enabled | mask : ipa->suspend_enabled & ~mask;""",
          """    ipa->suspend_enabled = enable ? ipa->suspend_enabled | 1ull << endpoint : ipa->suspend_enabled & ~(1ull << endpoint);""")
    open("nb_ipa55_config.c", "w").write(e.s)


def gsi_c():
    e = Edit("upstream/coachz_gsi.c")
    e.rep(""" * The IPA v4.2 GSI as mainline gsi.c sets it up after the modem raises ipa-setup-ready (ipa_setup -> gsi_setup). Not in
 * the kernel build. The modem loads the GSI firmware ("qcom,gsi-loader = modem"), so GSI_STATUS must already say ENABLED.
""", """ * nezha (SM8850, GSI v5.0) version of QcomBaseband ipa/coachz_gsi.c (dd0199c, BSD-2-Clause, HoshinoYumeka), derived by
 * tools/baseband27/derive-ipa55.py: the GSI as mainline gsi.c sets it up (ipa_setup -> gsi_setup). The application processor
 * loads and authenticates the GSI firmware (ipa_fws, PAS 15) before this runs, so GSI_STATUS must already say ENABLED. The
 * modem allocates its own channels on v5.5.
""")
    e.rep(""" * it too once setup is done. The apps SMMU is in bypass, so ring addresses are physical.
 * Ring memory: one block, write-combined or uncached, zeroed, aligned to 64 KiB (largest ring first keeps each 2^n ring
 * aligned to its size, which the hardware requires). v4.2 needs 40 KiB: four TRE rings (8 + 4 + 4 + 4 KiB) and their
 * four event rings.
""", """ * it too once setup is done. Ring addresses are IOVAs of the IPA's SMMU context bank.
 * Ring memory: one block, write-combined or uncached, zeroed, aligned to 64 KiB (largest ring first keeps each 2^n ring
 * aligned to its size, which the hardware requires). nezha needs 40 KiB: four TRE rings (8 + 4 + 4 + 4 KiB) and their
 * four event rings.
""")
    e.rep('#include "coachz_ipa_kernel.h"', '#include "nb_ipa55_kernel.h"')
    e.rep("""static void modem_channel_halt(struct coachz_gsi *g, uint32_t channel)
{
    int err, retries = 10;   /* GSI_CHANNEL_MODEM_HALT_RETRIES */
    do err = generic_command(g, channel, CGS_GENERIC_HALT_CHANNEL); while (err == -EAGAIN && retries--);
    if (err) printf("COACHZ gsi: error %d halting modem channel %u\\n", err, channel);
}
""", """/* gsi_modem_channel_flow_control: enable or disable flow control on a modem TX channel (from the endpoint teardown) */
void coachz_gsi_modem_flow_control(struct coachz_gsi *g, uint32_t channel, int enable)
{
    int err;
    uint32_t retries = cgs_flow_retries(enable);
    do err = generic_command(g, channel, enable ? CGS_GENERIC_ENABLE_FLOW_CONTROL : CGS_GENERIC_DISABLE_FLOW_CONTROL);
    while (err == -EAGAIN && retries--);
    if (err) printf("COACHZ gsi: error %d %sabling modem channel %u flow control\\n", err, enable ? "en" : "dis", channel);
}
""")
    e.rep("""    uint32_t mask = g->t.modem_channels, allocated = 0;   /* v4.2: the AP allocates the modem's channels */
    while (mask) {
        const uint32_t ch = (uint32_t)__builtin_ctz(mask);
        if ((err = generic_command(g, ch, CGS_GENERIC_ALLOCATE_CHANNEL))) {
            printf("COACHZ gsi: modem channel %u allocation failed, %d\\n", ch, err);
            while (allocated) { const uint32_t h = 31u - (uint32_t)__builtin_clz(allocated); allocated ^= 1u << h; modem_channel_halt(g, h); }
            goto unwind;
        }
        mask ^= 1u << ch;
        allocated |= 1u << ch;
    }
    return 0;""", """    return 0;""")
    e.rep("""    const uint32_t hw = rd(g, CGSI_HW_PARAM_2);
    g->n = cgs_ring_counts(hw);
    printf("COACHZ gsi: HW_PARAM_2 0x%08x: %u channels, %u event rings\\n", hw, g->n.channels, g->n.evt_rings);""",
          """    const uint32_t hw = rd(g, CGSI_HW_PARAM_2), hw4 = rd(g, CGSI_HW_PARAM_4);
    g->n = cgs_ring_counts(hw, hw4);
    printf("COACHZ gsi: HW_PARAM_2 0x%08x HW_PARAM_4 0x%08x: %u channels, %u event rings\\n", hw, hw4, g->n.channels, g->n.evt_rings);""")
    e.rep("/* gsi_setup: GSI firmware loaded by the modem; gsi_irq_setup, gsi_ring_setup, the error log, the channels */",
          "/* gsi_setup: GSI firmware loaded by the AP; gsi_irq_setup, gsi_ring_setup, the error log, the channels */")
    e.rep("""        printf(", modem channels 0x%x\\n", g->t.modem_channels);""", """        printf("\\n");""")
    open("nb_gsi55.c", "w").write(e.s)


def cmd_c():
    e = Edit("upstream/coachz_ipa_cmd.c")
    e.rep(""" * The middle of mainline ipa_setup on IPA v4.2, after gsi_setup:""", """ * nezha (SM8850, IPA v5.5) version of QcomBaseband ipa/coachz_ipa_cmd.c (dd0199c, BSD-2-Clause, HoshinoYumeka), derived by
 * tools/baseband27/derive-ipa55.py.
 *
 * The middle of mainline ipa_setup, after gsi_setup:""")
    e.rep(" * Transactions (gsi_trans.c): each command takes one TRE on channel 1;", " * Transactions (gsi_trans.c): each command takes one TRE on the command channel (12);")
    e.rep(" * DMA memory (apps SMMU in bypass, so physical addresses): one write-combined", " * DMA memory (IOVAs of the IPA's SMMU context bank): one write-combined")
    e.rep('#include "coachz_ipa_kernel.h"', '#include "nb_ipa55_kernel.h"')
    e.rep('#include "coachz_ipa_cmd_proto.h"', '#include "nb_ipa55_cmd_proto.h"')
    e.rep("#define CIC_COMMAND_CHANNEL 1u\n", "")
    open("nb_ipa55_cmd.c", "w").write(e.s)


def rx_c():
    e = Edit("upstream/coachz_ipa_rx.c")
    e.rep(" * An AP RX endpoint of IPA v4.2 as mainline runs it,", """ * nezha (SM8850, IPA v5.5) version of QcomBaseband ipa/coachz_ipa_rx.c (dd0199c, BSD-2-Clause, HoshinoYumeka), derived by
 * tools/baseband27/derive-ipa55.py.
 *
 * An AP RX endpoint as mainline runs it,""")
    e.rep(" * After this mainline's ipa_setup calls ipa_qmi_setup, which is the handshake in coachz_ipa_qmi.h.", " * After this mainline's ipa_setup calls ipa_qmi_setup, which is the handshake in nb_ipa55_qmi.h.")
    e.rep('#include "coachz_ipa_kernel.h"', '#include "nb_ipa55_kernel.h"')
    e.rep('#include "coachz_ipa_rx_proto.h"', '#include "nb_ipa55_rx_proto.h"')
    open("nb_ipa55_rx.c", "w").write(e.s)


def modem_c():
    e = Edit("upstream/coachz_ipa_modem.c")
    e.rep(""" * ipa_modem_start and the cellular data path on IPA v4.2, as mainline Linux runs it (ipa_modem.c for rmnet_ipa0, the rmnet
 * QMAP layer above it). Not in the kernel build; drivers/RamBlock/coachz_modem.c includes it together with
 * coachz_ipa_cmd.c and coachz_ipa_rx.c in one translation unit, so its static names are prefixed cim_.
""", """ * nezha (SM8850, IPA v5.5) version of QcomBaseband ipa/coachz_ipa_modem.c (dd0199c, BSD-2-Clause, HoshinoYumeka), derived by
 * tools/baseband27/derive-ipa55.py.
 *
 * ipa_modem_start and the cellular data path, as mainline Linux runs it (ipa_modem.c for rmnet_ipa0, the rmnet QMAP layer
 * above it). The board file includes it together with nb_ipa55_cmd.c and nb_ipa55_rx.c in one translation unit, so its
 * static names are prefixed cim_.
""")
    e.rep('#include "coachz_ipa_kernel.h"', '#include "nb_ipa55_kernel.h"')
    e.rep('#include "coachz_ipa_modem_proto.h"', '#include "nb_ipa55_modem_proto.h"')
    e.rep(' * Mainline: after the QMI handshake ipa_modem_start registers rmnet_ipa0 (raw IP, MTU 1500). Opening it enables AP_MODEM_TX\n * (start GSI channel 0) and AP_MODEM_RX (channel 3:',
          ' * Mainline: after the QMI handshake ipa_modem_start registers rmnet_ipa0 (raw IP, MTU 1500). Opening it enables AP_MODEM_TX\n * (start GSI channel 11) and AP_MODEM_RX (channel 1:')
    e.rep(" * gsi_channel_tre_max (505 for channel 0) and the slots the DMA block holds.", " * gsi_channel_tre_max and the slots the DMA block holds.")
    e.rep('printf("COACHZ modem: endpoints do not match the MAPv4 data format\\n");', 'printf("COACHZ modem: endpoints do not match the data format\\n");')
    open("nb_ipa55_modem.c", "w").write(e.s)


if __name__ == "__main__":
    gsi()
    ipa()
    qmi()
    cmd()
    rx()
    modem()
    kernel()
    config_c()
    gsi_c()
    cmd_c()
    rx_c()
    modem_c()
