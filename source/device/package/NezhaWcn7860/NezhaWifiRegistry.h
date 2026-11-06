/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NEZHA_WIFI_REGISTRY_H
#define NEZHA_WIFI_REGISTRY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t version, valid;
    uint32_t root_busy, stack_busy, controller_busy, interface_busy;
} NezhaWifiRegistryState;
enum { NezhaWifiRegistryOpen = 1, NezhaWifiRegistryName = 2 };
/* Individual read-only busy-count samples, not an atomic tree barrier.
 * All three owned services remain retained until reboot. No tree traversal,
 * wait-for-quiet, workloop scheduling, matching or hardware access occurs. */
void peach_wlan_registry_state(NezhaWifiRegistryState *);
void nezha_wifi_diagnostic_registry_event(unsigned event, int pid,
                                        unsigned request, int32_t result);
#ifdef __cplusplus
}
#endif
#endif
