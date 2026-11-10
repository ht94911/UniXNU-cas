/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_GEN4_ADC_H
#define NEZHA_GEN4_ADC_H
#include <stdint.h>
/* The platform serializes this owner and verifies live v8/EE0 ownership of
 * SDAM0. Callbacks access only the supplied PMIC peripheral/register bytes. */
typedef struct {
 void *context;
 int (*owns_adc)(void *);
 int (*read)(void *,uint16_t,uint8_t,uint8_t *,unsigned);
 int (*write)(void *,uint16_t,uint8_t,const uint8_t *,unsigned);
 void (*pause_us)(void *,unsigned);
} NpGen4AdcOps;
typedef struct {
 NpGen4AdcOps io;
 uint8_t touched,uncertain,cleanup_started;
 uint8_t before_config[7],requested_config[7];
 uint16_t last_raw;
 unsigned conversions;
} NpGen4Adc;
/* Init has no IO. Read0=conn2747, read1=qtm14b/conn2 with GPIO2 high-Z.
 * A code is published only after the one conversion and cleanup completed. */
int np_gen4_adc_init(NpGen4Adc *,const NpGen4AdcOps *);
int np_gen4_adc_read(NpGen4Adc *,unsigned,uint16_t *);
/* Resume only observation/one not-yet-attempted cleanup of this owner.
 * Never resubmit a possibly accepted conversion or cleanup request. */
int np_gen4_adc_quiesce(NpGen4Adc *);
#endif
