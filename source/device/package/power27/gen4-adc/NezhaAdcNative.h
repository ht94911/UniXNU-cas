/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_ADC_NATIVE_H
#define NEZHA_ADC_NATIVE_H
#include <stdint.h>
typedef struct {
 uint16_t raw;
 int32_t error,c1024,centi_c;
 uint32_t converted,quiescent;
} NpAdcNativeReading;
typedef struct {
 int32_t init_error,stop_error;
 uint32_t attempted;
 NpAdcNativeReading reading[2];
} NpAdcNativeBootProbe;
#ifdef __cplusplus
extern "C" {
#endif
/* Private to the power owner. Live map/owner qualification precedes IO;
 * this prepared adapter is not yet built into a phone candidate. */
int np_adc_native_init(void);
int np_adc_native_read(unsigned,NpAdcNativeReading *);
int np_adc_native_stop(void);
/* Continuous worker continuation. Start is issued once per conversion;
 * poll performs no EOC sleep. Both return1 pending,0 complete or-1 failed.
 * Only a complete conversion, SDAM cleanup and exact QTM restore is valid.
 * Failed outputs retain quiescence so the caller cannot replay uncertain IO. */
int np_adc_native_begin(unsigned,NpAdcNativeReading *);
int np_adc_native_poll(NpAdcNativeReading *);
/* One boot-time probe, before authentication/MCA work starts. A failed first
 * conversion prevents a second; cleanup failures retain the existing owner. */
int np_adc_native_boot_probe(NpAdcNativeBootProbe *);
int np_adc_native_boot_probe_qtm(NpAdcNativeBootProbe *);
#ifdef __cplusplus
}
#endif
#endif
