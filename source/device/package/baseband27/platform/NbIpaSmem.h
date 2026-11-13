/* SPDX-License-Identifier: BSD-2-Clause */
/* ipa_smem_init/ipa_smmu_ap_cb_probe map the actual item through complete
 * identity-mapped pages. Missing storage is allocated after both MSS stops. */
#ifndef NB_IPA_SMEM_H
#define NB_IPA_SMEM_H
#include "NbCommon.h"
#include "NbMss.h"
#include "../upstream/QcomSmem.h"

#define NB_SMEM_IPA_FILTER_TABLE 497u
#define NB_IPA_SMEM_BYTES 0x19000u

static inline int nb_ipa_smem_region(const QcomSmem *smem,
                                     const QcomSmemPartition *partition,
                                     uint64_t physical, NbRange *region,
                                     uint32_t *item_address)
{
    if (!smem || !smem->base || !partition || !region || !item_address ||
        !physical || (physical & 4095) || !smem->size ||
        (smem->size & 4095) || physical >= 0x100000000ULL ||
        smem->size > 0x100000000ULL - physical) return NB_ARGUMENT;
    uint32_t offset = 0, bytes = 0;
    int r = qcom_smem_find(smem, partition, NB_SMEM_IPA_FILTER_TABLE,
                           &offset, &bytes);
    if (r) return r == QcomSmemMissing ? NB_MISSING : NB_WIRE;
    if (bytes != NB_IPA_SMEM_BYTES || offset > smem->size ||
        bytes > smem->size - offset) return NB_OWNERSHIP;
    const uint64_t address = physical + offset;
    const uint64_t start = address & ~UINT64_C(4095);
    const uint64_t end = (address + bytes + 4095) & ~UINT64_C(4095);
    if (end > physical + smem->size || end <= start) return NB_OWNERSHIP;
    *region = (NbRange){start, end - start};
    *item_address = (uint32_t)address;
    return NB_OK;
}

/* Caller holds the existing SMEM hardware mutex. Initial PAS success is
 * required even when the item exists; missing storage is allotted only here. */
static inline int nb_ipa_smem_prepare(const NbMss *m, uint64_t physical,
                                      NbRange *region, uint32_t *item_address)
{
    if (!m || m->stage != NbMssCheck || !m->masters_stopped ||
        m->initial_stop_mask != 3 || m->initial_stop[0] || m->initial_stop[1])
        return NB_OWNERSHIP;
    int r = nb_ipa_smem_region(&m->smem, &m->partition, physical, region, item_address);
    if (r != NB_MISSING) return r;
    r = qcom_smem_alloc(&m->smem, &m->partition, NB_SMEM_IPA_FILTER_TABLE,
                        NB_IPA_SMEM_BYTES);
    if (r && r != QcomSmemExists) return NB_WIRE;
    return nb_ipa_smem_region(&m->smem, &m->partition, physical, region, item_address);
}
#endif
