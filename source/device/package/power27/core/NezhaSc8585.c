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
    if (!(disabled & 4) && raw[6] <= 0xfff) { p->battery_ma = raw[6] * 3125u / 1000u; p->valid |= 8; }
    if (!(disabled & 1) && raw[8] <= 0x1ff) { p->die_centi_c = (int32_t)raw[8] * 50; p->valid |= 16; }
    return 0;
}
