// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef NEZHA_SENSOR_NATIVE_CALLER_H
#define NEZHA_SENSOR_NATIVE_CALLER_H
#include "../als-owner/platform/AdspLink.h"
#ifdef __cplusplus
extern "C" {
#endif
int nezha_sensors_native_install(NaAdsp *,void *worker_event,uint64_t epoch);
int nezha_sensors_native_prepared(NaAdsp *);
int nezha_sensors_native_after_receive(NaAdsp *);
void nezha_sensors_native_lost(NaAdsp *,int error);
int nezha_sensors_native_rebind(NaAdsp *fresh_owner,uint64_t epoch);
int nezha_sensors_native_stop(NaAdsp *);
bool nezha_sensors_native_drained(NaAdsp *);
// Requests are consumed by the existing ADSP worker; callers never poll QRTR.
int nezha_sensors_stop_and_wait(uint32_t timeout_ms);
#ifdef __cplusplus
}
#endif
#endif
