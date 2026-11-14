/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_TAS25XX_TABLES_H
#define NEZHA_TAS25XX_TABLES_H
#include "Tas25xxProgram.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t type,channel,reg,reg_type,mask,count,default_index,misc;
    NaTasBytes values;
} NaTasControl;
typedef struct {
    NaTasBytes enable,disable,clear,entries;
    uint32_t count,settle_ms;
} NaTasFaultTable;
int na_tas_control(const NaTasImage *,const char *name,NaTasControl *);
/* Enum value names (e.g. OFF) are checked, never assumed to be index zero. */
int na_tas_enum(const NaTasControl *,const char *value,NaTasBytes *program);
int na_tas_enum_index(const NaTasControl *,uint32_t index,NaTasBytes *program);
int na_tas_format(const NaTasImage *,uint32_t channel,int transmit,const char *name,NaTasBytes *program);
int na_tas_fault_severity(const NaTasFaultTable *,uint32_t channel,uint32_t reported,uint32_t *fatal);
/* Serialized integer-control program in caller-owned 17-byte storage. */
int na_tas_integer(const NaTasControl *,uint32_t index,uint8_t out[17],NaTasBytes *);
int na_tas_block(const NaTasImage *,uint32_t channel,const char tag[5],NaTasBytes *);
int na_tas_hardware(const NaTasImage *,uint32_t channel,uint32_t index,NaTasBytes *);
int na_tas_fault_table(const NaTasImage *,uint32_t channel,NaTasFaultTable *);
/* Read-only checks: PWRCK opcodes describe comparisons, NEVER writes. */
int na_tas_power_check(NaTasBytes,const NaTasIo *,uint64_t lease,uint32_t channel);
/* Returns bitset of reported entries; caller mutes/shuts down on fault instead
 * of applying Android's automatic reset/retry policy. No clear-on-error write. */
int na_tas_faults(const NaTasFaultTable *,const NaTasIo *,uint64_t lease,uint32_t channel,
                  int clock_running,uint32_t *faults);
#ifdef __cplusplus
}
#endif
#endif
