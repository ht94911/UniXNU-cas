/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Independently written scalar decoder. Register addresses, bit encodings and
 * conversion ratios were checked against Xiaomi's published SC8585 register
 * definitions. No Linux driver code, regmap operations or policy is included.
 * The GPL-2.0 vendor implementation remains a separately pinned reference. */
#include "NezhaCharge.h"
#include <string.h>

int np_sc8585_decode(NpPump *p, uint8_t chip, const uint8_t c[13], uint16_t valid, const uint8_t a[18])
{
    if (!p || !c || !a) return -1;
    memset(p, 0, sizeof(*p)); p->chip = chip;
    if (chip != 0x85) return -1; /* 0x41/other variants have different scales */
    const uint16_t required = (1u << 1) | (1u << 4) | (1u << 11) | (1u << 12);
    if ((valid & required) != required) return -1;
    p->switching_valid = !!(valid & 1); p->switching = !!(c[0] & 2);
    p->enable = !!(c[1] & 0x80);
    p->mode = c[4] & 7;
    p->adc_enabled = !!(c[11] & 0x80); /* register15 ADC_EN */
    if (!p->adc_enabled || ((c[11] & 0x40) && !(c[11] & 0x20))) return 0;
    const uint8_t disabled = c[12];
    uint32_t raw[9];
    for (unsigned i = 0; i < 9; ++i) raw[i] = (uint32_t)a[2*i] << 8 | a[2*i+1];
    /* Never reuse SC8581's 1.5625mA step: SC8585 IBUS is 1.875mA. Keep the
     * wide intermediate; the source driver's u16 assignment can truncate. */
    if (!(c[11] & 1) && raw[0] <= 0xfff) { p->bus_ma = (raw[0] * 1875u) / 1000u; p->valid |= 1; }
    if (!(disabled & 0x80) && raw[1] <= 0xfff) { p->bus_mv = raw[1] * 625u / 100u; p->valid |= 2; }
    if (!(disabled & 8) && raw[5] <= 0xfff) { p->battery_mv = raw[5] * 125u / 100u; p->valid |= 4; }
    /* The selected stock SC8585 branch does not enable IBAT/TSBAT channels.
     * Their generic SC8581 scale is not a qualified SC8585 measurement.
     * Battery current comes from the actual BQ gauge. */
    if (!(disabled & 1) && raw[8] <= 0x1ff) { p->die_centi_c = (int32_t)raw[8] * 50; p->valid |= 16; }
    return 0;
}

int np_sc8585_protection_decode(NpPumpProtection *p,uint8_t chip,
                               const uint8_t c[22],uint8_t first_state)
{
    if(!p)return -1;
    memset(p,0,sizeof(*p));
    if(!c||chip!=0x85)return -1;
    const unsigned mode=c[0x0e - 1]&7;
    if(mode>2)return -1;
    /* These are SC8585's coordinates, not SC8581's finer BUS_OVP/OCP steps.
     * OS4's actual set_operation_mode selects these fields by forward mode. */
    static const uint32_t bus_base[3]={15000,7500,3750};
    static const uint32_t bus_step[3]={800,400,200};
    static const uint32_t watchdog[8]={0,200,500,1000,5000,30000,100000,255000};
    static const uint32_t soft_start[8]={0,40,80,320,1280,5120,20480,81920};
    p->battery_ovp_mv=4450u+(c[0]&31u)*25u;
    p->usb_ovp_mv=(c[2]&15u)==15u?7500u:11000u+(c[2]&15u)*1000u;
    p->wpc_ovp_mv=(c[3]&15u)==15u?7500u:11000u+(c[3]&15u)*1000u;
    p->bus_ovp_mv=bus_base[mode]+((c[4]&0x3cu)>>2)*bus_step[mode];
    p->output_ovp_mv=4800u+(c[4]&3u)*200u;
    p->bus_ocp_ma=2100u+(c[5]&31u)*150u;
    p->pmid_ovp_mv=200u+(c[7]&7u)*100u;
    p->pmid_uvp_mv=50u+(c[8]&7u)*50u;
    p->battery_ovp_enabled=!(c[0]&0x80);
    p->bus_ocp_enabled=!(c[5]&0x80);
    p->bus_ucp_enabled=!(c[6]&0x80);
    p->pmid_ovp_enabled=!(c[7]&0x80);
    p->pmid_uvp_enabled=!(c[8]&0x80);
    p->watchdog_ms=watchdog[c[0x0d-1]&7u];
    p->soft_start_ms=soft_start[(c[0x0d-1]>>3)&7u];
    const uint8_t state=c[0x0a-1]|first_state;
    /* Actual OS4 ops_cp_get_errorhl_stat gives LOW priority over HIGH. The
     * voltage-window status is distinct from a hardware fault latch. */
    p->bus_error=(c[0x0a-1]&0x10)?1:(c[0x0a-1]&8)?2:0;
    if(c[0]&0x20)p->faults|=NP_PUMP_BAT_OVP;
    /* OS4 skips the SC8581-only IBAT protection for chip vendor4/ID85. */
    if(c[2]&0x20)p->faults|=NP_PUMP_USB_OVP;
    if(c[3]&0x20)p->faults|=NP_PUMP_WPC_OVP;
    if(c[5]&0x20)p->faults|=NP_PUMP_BUS_OCP;
    if(c[6]&1)p->faults|=NP_PUMP_BUS_UCP;
    if(c[7]&8)p->faults|=NP_PUMP_PMID_OVP;
    if(c[8]&8)p->faults|=NP_PUMP_PMID_UVP;
    if(state&1)p->faults|=NP_PUMP_CBOOT;
    if(state&0x80)p->faults|=NP_PUMP_POR;
    const uint8_t fault=c[0x13-1];
    if(fault&0x40)p->faults|=NP_PUMP_TSHUT;
    if(fault&0x20)p->faults|=NP_PUMP_SS_TIMEOUT;
    if(fault&0x10)p->faults|=NP_PUMP_WD_TIMEOUT;
    if(fault&8)p->faults|=NP_PUMP_CONVERTER_OCP;
    if(fault&4)p->faults|=NP_PUMP_SS_FAIL;
    if(fault&2)p->faults|=NP_PUMP_VBUS_OVP;
    if(fault&1)p->faults|=NP_PUMP_VOUT_OVP;
    p->valid=1;
    return 0;
}
