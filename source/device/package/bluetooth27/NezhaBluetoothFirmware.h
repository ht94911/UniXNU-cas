/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_BLUETOOTH_FIRMWARE_H
#define NEZHA_BLUETOOTH_FIRMWARE_H
#include "qbt.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NEZHA_BT_FIRMWARE_PREFIX "firmware/qca"
#define NEZHA_BT_FIRMWARE_ROOT "/private/var/ngpu/" NEZHA_BT_FIRMWARE_PREFIX
#define NEZHA_BT_NVM_RECORDS 64u
struct nezha_bt_nvm_entry { char name[60]; uint8_t digest[32]; };
struct nezha_bt_inputs {
    struct qbt_boot_options boot;
    char patch[60], nvm[60];
    uint8_t patch_digest[32], address[6];
    uint32_t nvm_count;
    struct nezha_bt_nvm_entry nvm_entries[NEZHA_BT_NVM_RECORDS];
};
/* Immutable chosen data, generated from the approved package's private
 * firmware manifest. No generic filename or synthesized address fallback. */
int nezha_bt_inputs_read(struct nezha_bt_inputs *);
int nezha_bt_firmware_read(void *, const char *, const uint8_t **, size_t *);
void nezha_bt_firmware_release(void *, const uint8_t *, size_t);
#ifdef __cplusplus
}
#endif
#endif
