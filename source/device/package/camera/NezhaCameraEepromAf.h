/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_EEPROM_AF_H
#define NEZHA_CAMERA_EEPROM_AF_H

#include "NezhaCameraCci.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { NezhaCameraEepromAfPrefixBytes = 0x44 };

struct NezhaCameraEepromAfRead {
    struct NezhaCameraCciObservation transfer;
    unsigned bytes_read;
    uint8_t bytes[NezhaCameraEepromAfPrefixBytes];
    bool AF_checksum_valid;
    uint16_t infinity_dac, macro_dac;
};

/* Read only the original AF block of the already identified main module:
 * EEPROM slave0xa2/7bit0x51, word register address, CCI0/master1. All
 * power/CCI resources must belong to the caller. No EEPROM programming,
 * sensor stream setting, placeholder/golden or unrelated block access.
 * Stop at the first CCI failure and report the actual partial byte count. */
enum NezhaCameraCciStatus nezha_camera_eeprom_read_af(
    const struct NezhaCameraRegisterIo *cci, struct NezhaCameraEepromAfRead *result);

#ifdef __cplusplus
}
#endif
#endif
