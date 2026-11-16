/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef QCOM_RPC_LISTENER_H
#define QCOM_RPC_LISTENER_H
#include "QcomFastRpc.h"
#ifdef __cplusplus
extern "C" {
#endif
#define QCOM_LISTENER_BYTES 32768u
enum {QcomListenerAttach=1,QcomListenerInit,QcomListenerOpen,QcomListenerRegister,QcomListenerRunning,QcomListenerFailed};
typedef struct {
    void *context;
    /* Actual reverse module/QAIC scalar. Input pointers last this callback;
     * output buffers are caller-owned and must not change after it returns. */
    int32_t (*invoke)(void *,uint32_t handle,uint32_t scalar,QcomRpcArg *,uint32_t count);
} QcomRpcProvider;
typedef struct {
    QcomFastRpc *rpc;QcomRpcProvider provider;
    uint32_t phase,control_token,listener_token,remote_handle,reverse_calls;
    uint32_t previous_context,previous_result,output_bytes;
    uint32_t primitive_in[4],primitive_out[4],open_in[2],open_out[2];
    char open_error[256];uint8_t input[QCOM_LISTENER_BYTES],output[QCOM_LISTENER_BYTES];
    int error;
} QcomRpcListener;
int qcom_listener_prepare(QcomRpcListener *,QcomFastRpc *,const QcomRpcProvider *);
int qcom_listener_poll(QcomRpcListener *,uint64_t now_us,int sensor_pd_up);
#ifdef __cplusplus
}
#endif
#endif
