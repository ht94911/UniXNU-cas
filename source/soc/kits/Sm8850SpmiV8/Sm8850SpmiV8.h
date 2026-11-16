/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* SM8850 arbiter v8. Bus geometry: nezha N0; protocol: Qualcomm downstream
 * spmi-pmic-arb.c fc30e54174d254ff7f33622a9278e4435f6718d2 and the existing
 * nezha keys/ADC transaction lifetime. No peripheral policy lives here. */
#ifndef SM8850_SPMI_V8_H
#define SM8850_SPMI_V8_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { SM8850_SPMI_BUSES = 2, SM8850_SPMI_MAX_APIDS = 8192 };
typedef struct {
    void *context;
    int (*read32)(void *, uint64_t, uint32_t *);
    int (*write32)(void *, uint64_t, uint32_t);
    void (*pause_us)(void *, unsigned);
    /* Same sleepable gate as keys and ADC: held from mapping/owner validation
     * through CMD, STATUS and RDATA. Never call from interrupt context. */
    int (*acquire)(void *);
    void (*release)(void *);
} Sm8850SpmiOps;
typedef struct Sm8850SpmiV8 {
    Sm8850SpmiOps io;
    uint32_t version;
    uint16_t first[SM8850_SPMI_BUSES], count[SM8850_SPMI_BUSES];
    uint8_t ee, ready;
    uint8_t pending;
    uint64_t pending_base;
    uint32_t status;
    int last_error;
} Sm8850SpmiV8;
typedef struct {
    uint16_t ppid, apid;
    uint8_t bus, owner, irq_owner, mappings;
} Sm8850SpmiChannel;
int sm8850_spmi_v8_init(Sm8850SpmiV8 *, const Sm8850SpmiOps *, unsigned ee);
int sm8850_spmi_v8_find(Sm8850SpmiV8 *, unsigned bus, uint16_t ppid, Sm8850SpmiChannel *);
/* Eight bytes maximum; no retry after uncertain completion. Writes require a
 * live EE-owned map. Reads also recheck the bus and PPID, never a stale APID. */
int sm8850_spmi_v8_read(Sm8850SpmiV8 *, const Sm8850SpmiChannel *, uint8_t reg, uint8_t *, unsigned);
int sm8850_spmi_v8_write(Sm8850SpmiV8 *, const Sm8850SpmiChannel *, uint8_t reg, const uint8_t *, unsigned);
int sm8850_spmi_v8_owns(Sm8850SpmiV8 *, const Sm8850SpmiChannel *);
#ifdef __cplusplus
}
#endif
#endif
