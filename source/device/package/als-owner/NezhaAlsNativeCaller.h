// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef NEZHA_ALS_NATIVE_CALLER_H
#define NEZHA_ALS_NATIVE_CALLER_H
#include "platform/AdspLink.h"
#include "NezhaAlsBackend.h"
#ifdef __cplusplus
extern "C" {
#endif
// One fresh control owner and its existing wait event, before GLINK setup.
// Epoch and initial interval are explicit board inputs, not sensor readings.
int nezha_als_native_install(NaAdsp *, void *wait_event, uint64_t epoch,
                             uint32_t initial_interval_us);
int nezha_als_native_prepared(NaAdsp *);
void nezha_als_native_after_receive(NaAdsp *);
void nezha_als_native_lost(NaAdsp *, int error);
#ifdef __cplusplus
}
#endif
#endif
