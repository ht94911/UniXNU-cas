/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_PD_H
#define NEZHA_PD_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NP_PD_ENTRIES 7u
#define NP_PD_BYTES (NP_PD_ENTRIES * 16u)
enum { NP_PD_FIXED = 0, NP_PD_APDO = 3 };
typedef struct { uint32_t min_mv,max_mv,max_ma,type; } NpPdEntry;
typedef struct {
    NpPdEntry entry[NP_PD_ENTRIES];
    uint32_t present_mask,invalid_mask;
    uint64_t epoch,at_ms;
    uint8_t valid;
} NpPdCaps;
typedef struct { uint32_t mv,ma,index,pps; } NpPdChoice;
/* OS4 property0x21014: seven16-byte LE records, not piano's12-byte ABI.
 * This decodes advertised capability; it does not certify adapter/cable auth. */
int np_pd_decode(NpPdCaps *,const uint8_t *,size_t,uint64_t epoch,uint64_t at_ms);
/* Select a capability only. Board limits/auth/role/temperature still gate IO. */
int np_pd_choose(const NpPdCaps *,uint64_t epoch,uint32_t mv,uint32_t limit_ma,
                 int allow_pps,NpPdChoice *);
/* Property0x2101c is the firmware's reported PPS status, not a synchronized
 * input-power sample or an acknowledgement of an outstanding request. */
int np_pd_status(uint32_t wire,uint32_t *mv,uint32_t *ma);
#ifdef __cplusplus
}
#endif
#endif
