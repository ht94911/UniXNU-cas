/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_POWER_LOG_H
#define NEZHA_POWER_LOG_H
#define NP_POWER_LOG_LIMIT_BYTES 1048576u
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { uint32_t phase,error,written,dropped,bytes;char path[96];uint32_t capped; } NpPowerLogStatus;
/* Optional bounded diagnostic file in the existing iOS Data /var/log.
 * No raw block writes, new pinned files, volumes or filesystem reset. */
int np_power_log_start(void);
int np_power_log_push(const char *,size_t);
void np_power_log_stop(void);
int np_power_log_status(NpPowerLogStatus *);
#ifdef __cplusplus
}
#endif
#endif
