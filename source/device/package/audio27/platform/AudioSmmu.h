/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_SMMU_H
#define NEZHA_AUDIO_SMMU_H
#include "NezhaAudioPlatform.h"
/* Shared mature component already present in the reference device package.
 * Link its existing objects once. Do not compile a second private copy. */
#include <ArmSmmu4KStreams.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    ArmSmmu4KIo io;uint64_t lease;unsigned bank,asid,stream_slot[4];
    uint32_t powered,fault_handler_installed,global_lease_held,output_bits;
    void *sync_context;int (*clean)(void *,const void *cpu,uint64_t pa,uint64_t bytes);
} NaSmmuOwner;
typedef struct {
    NaSmmuOwner owner;ArmSmmu4KConfig tables_config;ArmSmmu4KResult tables_result;
    ArmSmmu4KContextConfig context;ArmSmmu4KStreamState streams;
    ArmSmmu4KProgramResult program_result;ArmSmmu4KStreamResult stream_result;
    uint32_t started,quarantine;int error;
} NaSmmu;
int na_smmu_start(NaSmmu *,const NaSmmuOwner *,const NaRange *physical,const uint32_t *iova,uint32_t count,
                  void *tables,uint64_t table_pa,uint32_t table_pages,const NaRange *reserved,uint32_t reserved_count);
/* Owner holds its common configuration lease during every observation/change. */
int na_smmu_observe(NaSmmu *,int full_scan);
/* Requires actual PAS stop (or all masters otherwise proved quiescent), then
 * faults routes and disables only this context. A link loss is insufficient. */
int na_smmu_stop(NaSmmu *,int masters_quiescent);
#ifdef __cplusplus
}
#endif
#endif
