/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_NATIVE_SMMU_H
#define NEZHA_AUDIO_NATIVE_SMMU_H
#include "NezhaAudioNative.h"
#include "../platform/AudioResourceOwner.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    NaResourceOwner *owner;NaNativeWindow window;
    uint32_t attempted,context_offset,irq_attached,irq_readback;
    uint32_t fault_pending,fault_status,fault_syndrome;
    uint64_t irq_router,fault_address;int error;
} NaNativeSmmu;
/* Map the leased apps SMMU and register the existing ArmCpu SPI callback for
 * bank7 (stock interrupts entry8: SPI104/INTID136). No translation or route
 * is programmed here; AudioAssembly does that only after both PAS stops. */
int na_native_smmu_bind(NaNativeSmmu *,NaResourceOwner *,NaSmmuOwner *);
int na_native_smmu_faulted(const NaNativeSmmu *);
#ifdef __cplusplus
}
#endif
#endif
