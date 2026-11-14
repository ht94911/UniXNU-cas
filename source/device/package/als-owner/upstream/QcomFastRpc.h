/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Qualcomm FastRPC inline-buffer ABI, as used by SM8850 dsp-kernel.
 * GLINK is polled by the existing ADSP owner. No direct DMA or reset here. */
#ifndef QCOM_FAST_RPC_H
#define QCOM_FAST_RPC_H
#include "QcomGlink.h"
#ifdef __cplusplus
extern "C" {
#endif
#define QCOM_RPC_SLOTS 4u
#define QCOM_RPC_ARGS 8u
#define QCOM_RPC_SLOT_BYTES 131072u
#define QCOM_RPC_RX_BYTES 385u /* current ADSP DT qcom,intents = <0x181 0x40> */
enum {QcomRpcOkay=0,QcomRpcAgain=1,QcomRpcArgument=-1,QcomRpcWire=-2,
      QcomRpcLost=-3,QcomRpcTimeout=-4,QcomRpcNoSpace=-5,QcomRpcSync=-6};
enum {QcomRpcFree,QcomRpcQueued,QcomRpcAwaiting,QcomRpcComplete,QcomRpcRetained};
typedef struct {void *data;uint32_t bytes;} QcomRpcArg;
typedef struct {
    void *context;
    /* Caller has already assigned this memory in the sensor session's SMMU.
     * for_device publishes CPU writes; the other direction invalidates DSP
     * output only after a genuine final response. No mapping happens here. */
    int (*sync)(void *,uint32_t offset,uint32_t bytes,int for_device);
} QcomRpcOps;
typedef struct {
    uint32_t state,handle,scalar,inbufs,outbufs,metadata_bytes,total_bytes;
    uint32_t offset[QCOM_RPC_ARGS];QcomRpcArg args[QCOM_RPC_ARGS];
    uint64_t context,deadline_us;
    int32_t result;uint32_t early_response;uint8_t message[40];
} QcomRpcCall;
typedef struct {
    QcomGlink *glink;QcomGlinkChannel *channel;QcomRpcOps ops;
    uint8_t *memory;uint64_t dsp_address,job;
    uint32_t bytes,pid,tid,online,sent,received,stale,early,selected,signals;
    uint8_t intents[QCOM_RPC_RX_BYTES*8];
    QcomRpcCall call[QCOM_RPC_SLOTS];int error;
} QcomFastRpc;
int qcom_rpc_prepare(QcomFastRpc *,QcomGlink *,uint8_t *memory,uint32_t bytes,
                      uint64_t sensor_dsp_address,uint32_t pid,uint32_t tid,const QcomRpcOps *);
/* scalar contains exact method/buffer counts. Only inline, non-overlapping
 * input/output buffers are supported; fd/handle passing is never fabricated.
 * token remains reserved on timeout/loss while the DSP may own its memory.
 * timeout0 is permitted only for listener next2, whose remote wait is idle. */
int qcom_rpc_begin(QcomFastRpc *,uint32_t handle,uint32_t scalar,const QcomRpcArg *,
                   uint32_t count,int kernel_pid_zero,uint64_t now_us,uint64_t timeout_us,uint32_t *token);
int qcom_rpc_poll(QcomFastRpc *,uint64_t now_us);
int qcom_rpc_take(QcomFastRpc *,uint32_t token,int32_t *result);
void qcom_rpc_lost(QcomFastRpc *,int error);
#ifdef __cplusplus
}
#endif
#endif
