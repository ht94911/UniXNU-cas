/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_BLUETOOTH_PLATFORM_H
#define NEZHA_BLUETOOTH_PLATFORM_H
#include "NezhaBluetoothBoard.h"
#include "NezhaBluetoothFirmware.h"
#ifdef __cplusplus
extern "C" {
#endif
int nezha_bt_platform_init(struct nezha_bt_board_ops *, const struct nezha_bt_inputs *,
                           void *executor, void (*wake)(void *), void (*wait)(void *, uint64_t),
                           size_t (*receive)(void *, const uint8_t *, size_t));
void nezha_bluetooth_probe(void);
void nezha_bluetooth_register(void);
void nezha_bluetooth_stop(void); /* normal XNU exit hook; asynchronous cancellation */
int nezha_bluetooth_stop_and_wait(uint32_t timeout_ms);
extern volatile uint64_t native_bluetooth_stats[32];
#ifdef __cplusplus
}
#endif
#endif
