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
/* Prepare a caller-owned temporary copy of SHA-verified PERI NVM. The caller
 * must retain the original bytes and digest, own allocation/release, and keep
 * this copy stable during the call. Complete NVM validation precedes writes;
 * errors leave the copy unchanged. For the established 3200000-bps profile,
 * only tag17 payload byte1 in type2 NVM sets becomes the HAL baud code 0x11.
 * Type4 containers reuse the same validated walker; other sets/tags remain
 * intact. Other baud profiles return EOPNOTSUPP. This does not verify a SHA,
 * change addresses/IBS/calibration, transfer bytes, or prove controller ready. */
int qbt_firmware_prepare_peri_nvm(uint8_t *copy, size_t, uint32_t operating_baud);
#ifdef __cplusplus
}
#endif
#endif
