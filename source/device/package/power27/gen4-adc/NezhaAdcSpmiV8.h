/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_ADC_SPMI_V8_H
#define NEZHA_ADC_SPMI_V8_H
#include <stdint.h>
/* N0 v8 EE0 transport; caller holds its platform owner/serialization lease.
 * MMIO callbacks provide the architecture's device ordering/barriers. */
typedef struct {
 void *context;
 int (*read32)(void *,uint64_t,uint32_t *);
 int (*write32)(void *,uint64_t,uint32_t);
 void (*pause_us)(void *,unsigned);
} NpAdcSpmiOps;
typedef struct {
 NpAdcSpmiOps io;uint32_t apids;int ready;
 /* Only PMH0101 GPIO2 EN_CTL. An unresolved command retains this owner. */
 uint8_t qtm_before,qtm_expected,qtm_last,qtm_owned,qtm_pending,qtm_restore_attempted,qtm_read_valid;
 uint32_t qtm_phase,qtm_owner;
} NpAdcSpmiV8;
int np_adc_spmi_v8_init(NpAdcSpmiV8 *,const NpAdcSpmiOps *);
int np_adc_spmi_v8_owns(void *);
int np_adc_spmi_v8_read(void *,uint16_t,uint8_t,uint8_t *,unsigned);
int np_adc_spmi_v8_write(void *,uint16_t,uint8_t,const uint8_t *,unsigned);
int np_adc_spmi_v8_qtm_prepare(NpAdcSpmiV8 *);
int np_adc_spmi_v8_qtm_restore(NpAdcSpmiV8 *);
#endif
