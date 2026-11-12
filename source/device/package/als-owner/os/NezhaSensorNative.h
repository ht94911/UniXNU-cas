/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_SENSOR_NATIVE_H
#define NEZHA_SENSOR_NATIVE_H
#include "NezhaAudioAdspInputs.h"
#include "NezhaAudioSmmu.h"
#include "../platform/NezhaSensorBundle.h"
#include "../upstream/QcomPdr.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    NaResourceOwner *owner;NaNativeSmmu native;NaSmmuOwner config;
    NaNativeMemory memory,tables;
    ArmSmmu4KConfig table_config;ArmSmmu4KResult table_result;
    ArmSmmu4KContextConfig context;ArmSmmu4KStreamState streams;
    ArmSmmu4KStreamResult stream_result;ArmSmmu4KProgramResult program_result;
    NezhaSensorBundle bundle;NezhaSensorFiles files;
    QcomPdr pdr;QcomFastRpc rpc;QcomRpcListener listener;
    uint32_t attempted,mapped,linked,last_pdr_revision,last_listener_phase,last_reverse_calls;
    int error;
} NezhaSensorNative;
int nezha_sensor_native_buffers(NezhaSensorNative *,NaResourceOwner *,NaAdsp *);
int nezha_sensor_native_link(NezhaSensorNative *,NaAdsp *);
void nezha_sensor_native_poll(NezhaSensorNative *,NaAdsp *,uint64_t now_us);
int nezha_sensor_native_state(const NezhaSensorNative *,uint32_t out[12]);
#ifdef __cplusplus
}
#endif
#endif
