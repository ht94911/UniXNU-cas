/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_BLUETOOTH_DIAGNOSTIC_H
#define NEZHA_BLUETOOTH_DIAGNOSTIC_H
#include "qbt.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Owner publishes software state only. Provider10 never walks the core or
 * takes a UART/transport/platform lock. HCI payload and BD_ADDR are omitted. */
int nezha_bt_diagnostic_init(void);
void nezha_bt_diagnostic_update(const uint64_t state[32], const struct qbt_stats *,
                                uint64_t ns, uint32_t pending, uint32_t stopping, uint32_t stopped);
#ifdef __cplusplus
}
#endif
#endif
