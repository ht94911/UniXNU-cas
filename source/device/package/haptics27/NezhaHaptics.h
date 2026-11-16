/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_HAPTICS_H
#define NEZHA_HAPTICS_H
#include <stdint.h>
#include <stddef.h>
#include "NezhaHapticsProtocol.h"
#include <PaganiHaptics/HapticsCore.h>
#include <Sm8850SpmiV8/Sm8850SpmiV8.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { NZ_HAPTICS_QUEUE = 128, NZ_HAPTICS_SEGMENT_FRAMES = 96 };
enum { NZ_HAPTICS_PROBE = 1, NZ_HAPTICS_PREPARE = 2, NZ_HAPTICS_ON = 3 };
enum { NZ_HAPTICS_EFFECT_COUNT = 15, NZ_HAPTICS_MAX_MS = 500, NZ_HAPTICS_MAX_PERMILLE = 1000 };
/* Versioned local transport. PCM follows this 40-byte header, signed LE16.
 * START returns a server-generated session and epoch via the ioctl state.
 * Every later packet identifies it and advances sequence exactly once. */
typedef struct {
    Sm8850SpmiV8 bus;
    Sm8850SpmiChannel channels[PGH_PERIPHS];
    PghSpmiTransport transport;
    PghSpmiIo io;
} NzHapticsBus;
int nz_haptics_bus_init(NzHapticsBus *, const Sm8850SpmiOps *);
/* Caller holds the motor lease; selector writes only expose existing results. */
int nz_haptics_factory(NzHapticsBus *, PghFactoryState *);
void nz_haptics_config(PghConfig *, const PghFactoryState *, unsigned mode);
typedef struct { uint64_t begin_us,end_us;uint32_t epoch,sequence,amplitude,operation,effect; } NzHapticsSegment;
typedef struct {
    uint32_t session,epoch,next_sequence,head,count;
    uint32_t ready,running,stopping,error,active_sequence;
    uint32_t completed,cancelled,requests,underruns,active_operation;
    uint64_t last_end_us,active_until_us,on_begin_us,rest_until_us;
    NzHapticsSegment segments[NZ_HAPTICS_QUEUE];
} NzHapticsQueue;
void nz_haptics_queue_open(NzHapticsQueue *,uint32_t session);
int nz_haptics_queue_start(NzHapticsQueue *,uint32_t session,uint32_t *epoch);
int nz_haptics_queue_pcm(NzHapticsQueue *,const NzHapticsPacket *,const int16_t *,uint64_t now_us,uint64_t at_us);
int nz_haptics_queue_stop(NzHapticsQueue *,uint32_t session,uint32_t epoch);
void nz_haptics_queue_close(NzHapticsQueue *);
/* next returns one due segment; fully expired data is discarded, never played
 * late as a new tap. The worker holds the same gate through hardware writes. */
int nz_haptics_queue_next(NzHapticsQueue *,uint64_t now_us,NzHapticsSegment *);
void nz_haptics_status(const NzHapticsQueue *,NzHapticsStatus *);
void nezha_haptics_start(void);
int nezha_haptics_wait_ready(uint32_t timeout_ms);
int nezha_haptics_quiesce(void);
int nezha_haptics_suspended(int suspend);
int nezha_haptics_ios_commit(int plugin_visible,int preset_visible);
#ifdef __cplusplus
}
#endif
#endif
