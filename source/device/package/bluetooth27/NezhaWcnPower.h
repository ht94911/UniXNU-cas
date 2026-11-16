/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_WCN_POWER_H
#define NEZHA_WCN_POWER_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { NEZHA_WCN_WLAN = 1, NEZHA_WCN_BLUETOOTH = 2, NEZHA_WCN_UWB = 3 };
/* All calls below use the same normal-thread resource lock. Other RSC and
 * QUP3/GCC/SMMU consumers must bind to these owner hooks during integration. */
int nezha_wcn_resource_lock(void);
void nezha_wcn_resource_unlock(void);
int nezha_wcn_power_get(unsigned owner, uint64_t *lease);
int nezha_wcn_power_put(unsigned owner, uint64_t lease);
int nezha_wcn_bt_enable(uint64_t lease, bool enabled);
unsigned nezha_wcn_power_owners(void);
/* The validated platform supplies the same serialized RPMh engine, not a
 * second private TCS scheduler. Return negative errno. */
int nezha_wcn_rpmh_resource(const char *, uint32_t offset, uint32_t value);
int nezha_wcn_rpmh_arc(const char *, uint32_t level);
#ifdef __cplusplus
}
#endif
#endif
