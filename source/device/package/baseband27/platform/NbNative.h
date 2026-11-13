/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef NB_NATIVE_H
#define NB_NATIVE_H
#include "NbBaseband.h"
#include "NbPower.h"
#include <ArmSmmu4KStreams.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {void *descriptor,*mapping;uint8_t *cpu;uint64_t physical;uint32_t bytes;} NbNativeMemory;
typedef struct {
    Sm8850Resources *common;Sm8850SmmuAssignment assignment;uint64_t lease;
    volatile uint8_t *registers;uint32_t context_offset,fault,irq_readback;uint64_t irq_route;
    NbNativeMemory tables;
    ArmSmmu4KConfig config;ArmSmmu4KResult result;ArmSmmu4KContextConfig context;
    ArmSmmu4KStreamState streams;ArmSmmu4KProgramResult program;ArmSmmu4KStreamResult stream_result;
    ArmSmmu4KIo io;unsigned attempted,ready;
} NbNativeSmmu;
int nb_native_allocate(NbNativeMemory *,uint32_t bytes,uint32_t alignment,int low,int uncached);
void nb_native_release(NbNativeMemory *);
void *nb_native_map(uint64_t physical,uint32_t bytes,int memory);
int nb_native_sync(void *,void *,uint64_t,uint64_t);
int nb_native_low_sync(void *cpu,uint64_t physical,uint64_t bytes);
void nb_native_publish_writes(void);
uint64_t nb_native_now_ms(void);
int nb_native_smmu_prepare(NbNativeSmmu *,Sm8850Resources *,const NbNativeMemory *,const NbIpaMemory *,NbRange smem);
int nb_native_smmu_enable(NbNativeSmmu *);
int nb_native_smmu_status(NbNativeSmmu *);
int nezha_bb_bind(NbBasebandConfig *);
int nezha_bb_events(void (*)(void),void (*)(void));
int nezha_bb_dispatch(void);
int nezha_bb_owners_ready(void);
#ifdef __cplusplus
}
#endif
#endif
