/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef QBT_FIRMWARE_H
#define QBT_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum qbt_firmware_kind { QBT_FW_TLV_PATCH, QBT_FW_ELF_PATCH, QBT_FW_NVM };
/* Structural validation before the unmodified Linux code makes its private
 * NVM copy. Does not replace the board SHA-256 or the controller's signature
 * verification. No input byte, address or calibration tag is changed here. */
int qbt_firmware_validate(enum qbt_firmware_kind, const uint8_t *, size_t);
#ifdef __cplusplus
}
#endif
#endif
