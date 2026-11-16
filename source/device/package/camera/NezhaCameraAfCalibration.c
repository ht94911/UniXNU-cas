/* SPDX-License-Identifier: MIT */
#include "NezhaCameraAfCalibration.h"
#include <stddef.h>

/* Four original V6.0.7 EEPROM modules and CamX13cd830: margins truncate
 * toward zero, clamp to the actuator's 12-bit VCM range, then accumulate
 * codePerStep as float. Infinity occupies the largest AF step index. */
bool nezha_camera_af_calibrate(uint16_t infinity_dac, uint16_t macro_dac,
    struct NezhaCameraAfCalibration *out)
{
    if (!out) return false;
    *out = (struct NezhaCameraAfCalibration){0};
    if (infinity_dac > 4095 || macro_dac > 4095 || infinity_dac == macro_dac) return false;
    const int difference = (int)macro_dac - infinity_dac;
    const unsigned distance = (unsigned)(difference < 0 ? -difference : difference);
    const float range = (float)distance;
    /* Original signed VCM logic applies the inverse sign at zero DAC. */
    int first = infinity_dac + (int)((infinity_dac ? -0.1f : 0.1f) * range);
    int last = macro_dac + (int)((macro_dac ? 0.1f : -0.1f) * range);
    if (first < 0) first = 0;
    if (last > 4095) last = 4095;
    if (first > 4095 || last < 0 || first == last) return false;
    const float increment = (float)(last-first) / NezhaCameraAfSteps;
    float current = (float)first;
    uint16_t minimum=4095, maximum=0;
    for (unsigned index=0; index<NezhaCameraAfSteps; ++index) {
        const int16_t dac=(int16_t)current;
        if (dac < 0 || dac > 4095) return false;
        out->dac[NezhaCameraAfSteps-1-index]=dac;
        if ((uint16_t)dac<minimum) minimum=(uint16_t)dac;
        if ((uint16_t)dac>maximum) maximum=(uint16_t)dac;
        current += increment;
    }
    out->initial_dac=(int16_t)first;
    out->minimum_dac=minimum;
    out->maximum_dac=maximum;
    return true;
}

bool nezha_camera_af_calibrate_eeprom(const uint8_t *bytes, size_t length,
    struct NezhaCameraAfCalibration *out)
{
    if (!out) return false;
    *out = (struct NezhaCameraAfCalibration){0};
    if (!bytes || length <= 0x43 || bytes[0x27] != 1) return false;
    unsigned sum=0;
    for (size_t i=0x28;i<=0x42;++i) sum+=bytes[i];
    if (bytes[0x43] != sum%255u+1u) return false;
    const uint16_t macro=(uint16_t)((uint16_t)bytes[0x2d]<<8 | bytes[0x2e]);
    const uint16_t infinity=(uint16_t)((uint16_t)bytes[0x31]<<8 | bytes[0x32]);
    return nezha_camera_af_calibrate(infinity,macro,out);
}
