/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_ASSEMBLY_H
#define NEZHA_AUDIO_ASSEMBLY_H
#include "../core/AudioDriver.h"
#include "../core/AudioTopology.h"
#include "AudioSmmu.h"
#include "AdspPower.h"
#include "NezhaAudioBoard.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {void *cpu;NaRange physical;} NaOwnedBuffer;
typedef struct {
    NaOwnedBuffer dma;
    uint32_t iova,port;
    const void *graph;uint32_t graph_bytes;
    NaMediaEndpoints media;
} NaAssemblyStream;
typedef struct {
    void *context;
    int (*smem_lock)(void *);void (*smem_unlock)(void *);
    void (*doorbell)(void *,uint32_t signal);
    int (*sync)(void *,const NaOwnedBuffer *,uint32_t offset,uint32_t bytes,int for_device);
    uint64_t (*host_time)(void *);
    int (*poll_shared)(void *);
} NaAssemblyOps;
typedef struct {
    /* Immutable, kernel-private plan from the selected package's existing
     * owner. All mappings/allocations and leases live through failure. This
     * object contains no inferred TCS, context bank, or physical address. */
    uint64_t adsp_lease;
    volatile void *smem;
    const NaFirmwarePiece *firmware[2];uint32_t firmware_count[2]; /*main,dtb*/
    void *firmware_map[2];NaOwnedBuffer metadata[2];
    NaPasOps pas;
    volatile uint32_t *qmp;const uint8_t *cmd_db;uint32_t tcs;
    NaProxyOps proxy;
    NaSmmuOwner smmu;NaOwnedBuffer tables;
    const NaRange *reserved;uint32_t reserved_count;
    NaAssemblyStream stream[3]; /*output, optional mic, mandatory VI*/
    const NaTasImage *tuning;NaTasSelection tas;
    NaTiCalibration calibration;uint32_t ti_instance;
    NaBoardIo board;
    NaPrmResource resources[NA_PRM_RESOURCES];uint32_t resource_count,output_votes,input_votes;
    NaAudioStatus *status;uint64_t *output_ring,*input_ring;
    uint32_t timebase_numer,timebase_denom,capture_proven,capture_probe_allowed;
    NaAssemblyOps ops;
} NaAssemblyPlan;
typedef struct {
    NaAssemblyPlan plan;
    NaSplitFirmware firmware[2];NaPasLoad loader[2];
    NaProxyPower proxy;NaSmmu smmu;NaRxQueue rx;NaAdsp adsp;
    NaGeniBoard speaker_board;NaMicBoard mic_board;NaTasEndpoint speaker;
    NaDriver driver;
    uint32_t initialized,dma_attempted,dma_count;
    NaRange dma_range[3],reserved[128];uint32_t iova[3],reserved_count;
    int error;
} NaAssembly;
/* Executes only validation, CPU-buffer initialization and readonly resource
 * discovery. No PAS, firmware writes, SMMU programming or board power here.
 * Then pass &assembly.driver to the existing native worker start function.
 * Caller must zero a fresh assembly; reinitialization is forbidden. */
int na_assembly_init(NaAssembly *,const NaAssemblyPlan *,uint64_t now_ms);
/* Forwarded only by the selected shared IPCC dispatcher for LPASS signal2. */
void na_assembly_smp2p(NaAssembly *);
#ifdef __cplusplus
}
#endif
#endif
