/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_RESOURCE_OWNER_H
#define NEZHA_AUDIO_RESOURCE_OWNER_H
#include <Sm8850Resources.h>
#include "AudioSmmu.h"
#include "AdspPower.h"
#include "AdspLink.h"
#ifdef __cplusplus
extern "C" {
#endif
/* IDs in the common SoC ledger. Touch retains TCS0; firmware/USB retain
 * banks0..6 and slots0..8. GPU has a separate SMMU. No dynamic allocation. */
enum {NaResourceSmmu=4,NaResourceQmp=5,NaResourceAdspTcs=6,NaResourceMicTcs=7};
typedef struct {
    Sm8850Resources *common;
    Sm8850SmmuAssignment assignment;
    uint64_t smmu_lease,qmp_lease,tcs_lease;
    uint32_t smmu_attempted,registered;
    int error;
    NaAdsp *adsp;
} NaResourceOwner;
/* Binds real owner callbacks; no MMIO, PAS, endpoint or discovery performed.
 * Common owner must already be installed in the selected package. */
int na_resource_owner_init(NaResourceOwner *,Sm8850Resources *);
/* Claim only the explicit assignment, under the common configuration lock.
 * Mapping/power/fault-handler/cache inputs remain supplied by native owner. */
int na_resource_owner_smmu(NaResourceOwner *,NaSmmuOwner *);
NaProxyOps na_resource_owner_proxy(NaResourceOwner *);
int na_resource_owner_smem_lock(void *);
void na_resource_owner_smem_unlock(void *);
void na_resource_owner_doorbell(void *,uint32_t signal);
/* Called after PAS stop and before authentication. Sole SoC dispatcher owns
 * RX FIFO; no second reader in the audio driver. */
int na_resource_owner_ipcc(NaResourceOwner *,NaAdsp *);
int na_resource_owner_dispatch(NaResourceOwner *);
#ifdef __cplusplus
}
#endif
#endif
