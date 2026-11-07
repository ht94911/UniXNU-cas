/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef NEZHA_AUDIO_ADSP_INPUTS_H
#define NEZHA_AUDIO_ADSP_INPUTS_H
#include "NezhaAudioNative.h"
#include "NezhaAudioInputs.h"
#include "../platform/AudioAssembly.h"
#include "../platform/AudioResourceOwner.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    NaResourceOwner resources;
    NaInputPack pack;
    NaFirmwarePiece pieces[2][QCOM_MDT_MAX_SEGMENTS],mdt[2];
    NaSplitFirmware firmware[2];
    NaNativeWindow region[2];NaNativeMemory metadata[2];
    uint32_t count[2],attempted;uint64_t lease;int error;
} NaNativeAdspInputs;
/* Fill the firmware/PAS portion of an otherwise caller-owned assembly plan.
 * Read-only blob parsing, real CPU allocations and mappings only: no PAS,
 * firmware-region write, SMMU enable, graph, codec or service publication.
 * Remaining shared-memory/board/DMA/graph fields must still be bound before
 * na_assembly_init; an incomplete plan continues to be rejected there. */
int na_native_prepare_adsp_inputs(NaNativeAdspInputs *,NaAssemblyPlan *);
#ifdef __cplusplus
}
#endif
#endif
