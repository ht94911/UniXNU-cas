/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_TAS25XX_PROGRAM_H
#define NEZHA_TAS25XX_PROGRAM_H
#include "NezhaAudioPlatform.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Reader for the v7 wire format found in the acquired tuning input. This is
 * original bounded parsing code, not a relicensed copy of TI's GPL driver.
 * No tuning bytes are compiled into this module. Keep input immutable and
 * retained for the complete image/program lifetime, outside user mappings. */
typedef struct {const uint8_t *data;uint32_t bytes;} NaTasBytes;
enum {NaTasInit,NaTasHw,NaTasProfiles,NaTasInterrupts,NaTasBlockOps,NaTasSections};
typedef struct {
    NaTasBytes section[2][NaTasSections],controls;
    uint8_t declared_address[2],device_type[2],default_hw[8];
    uint8_t iv_width,vbat_monitor;
} NaTasImage;
typedef struct {
    NaTasBytes phase[4]; /* pre-up, post-up, pre-down, post-down */
    uint32_t misc;
} NaTasProfile;
typedef struct {uint32_t commands,written_bytes,delay_ms;} NaTasProgramInfo;
int na_tas_image(NaTasImage *,const void *bytes,uint32_t length);
int na_tas_profile(const NaTasImage *,uint32_t channel,const char *name,NaTasProfile *);
int na_tas_validate(NaTasBytes,NaTasProgramInfo *);

typedef struct {
    void *context;
    /* Existing exclusively leased GENI owner. Registers use book*32768 +
     * page*128 + reg. It must implement bank selection/8-bit I2C correctly.
     * Calls are synchronous, bounded and worker-only; no implicit retries. */
    int (*read)(void *,uint64_t lease,uint8_t address,uint32_t reg,uint8_t *value);
    int (*write)(void *,uint64_t lease,uint8_t address,uint32_t reg,const uint8_t *,uint32_t bytes);
} NaTasIo;
typedef struct {
    NaTasBytes program;NaTasIo io;
    uint64_t lease,wait_until;
    uint32_t offset,commands,finished;
    uint8_t address;
    int error;
} NaTasRun;
/* Start with a zero-initialized run. Validates the entire program before the first hardware call. Caller must
 * have matched the exact device and approved profile under its endpoint lease.
 * Tuning header addresses NEVER replace the verified nezha DT addresses. */
int na_tas_begin(NaTasRun *,NaTasBytes,const NaTasIo *,uint64_t lease,uint32_t channel);
/* One opcode per call; DELAY yields until its deadline instead of sleeping. */
int na_tas_poll(NaTasRun *,uint64_t now_ms);
void na_tas_abort(NaTasRun *);
#ifdef __cplusplus
}
#endif
#endif
