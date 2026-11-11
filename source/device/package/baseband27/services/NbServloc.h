/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The service locator (QMI service 64, version 1, instance 1 as announced 0x101): GET_DOMAIN_LIST (0x21) answers which
 * protection domains provide a named service, the job of Linux pd-mapper / Android pd_mapper on the application processor.
 *
 * Algorithm: sheng package/QcomQrtr.c qcom_servloc_answer (BSD-2-Clause-Patent), moved onto the shared NbQmi writer.
 * Request 0x01 service name, 0x10 optional u32 first index; answer 0x02 result, 0x10 u16 total, 0x11 u16 database revision,
 * 0x12 list (u8 count, then {u8 length, name, u32 instance, u8 data valid, u32 data}).
 *
 * nezha domains come from the device's own JSON files (read-only copies of 2026-11-07): modemr.jsn msm/modem/root_pd
 * instance 180 {tms/servreg, gps/gps_service}, modemuo.jsn msm/modem/oem_pd 180 {tms/servreg, oempd/srv1}, adspr/adsps/adspua
 * msm/adsp/{root_pd, sensor_pd, audio_pd} 74 (audio_pd also avs/audio), cdspr msm/cdsp/root_pd 76 {tms/servreg, avs/audio}. */
#ifndef NB_SERVLOC_H
#define NB_SERVLOC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define NB_SERVLOC_SERVICE 0x40u
#define NB_SERVLOC_INSTANCE 0x101u
#define NB_SERVLOC_GET_DOMAIN_LIST 0x21u

typedef struct { const char *domain; uint32_t instance; const char *const *services; } NbPdDomain;

extern const NbPdDomain nb_nezha_domains[];
extern const unsigned nb_nezha_domain_count;

/* Answer for one request into out; 0 when it is not a request to answer. Every listed service string is matched exactly
 * ("provider/service"). */
uint32_t nb_servloc_answer(const NbPdDomain *domains, unsigned count, const uint8_t *request, uint32_t length, uint8_t *out,
                           uint32_t capacity);

#ifdef __cplusplus
}
#endif
#endif
