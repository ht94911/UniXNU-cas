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
    void *progress_context;NaPasProgressFn progress;
} NaNativeAdspInputs;
/* Fill the firmware/PAS portion of an otherwise caller-owned assembly plan.
 * Read-only blob parsing, real CPU allocations and mappings only: no PAS,
 * firmware-region write, SMMU enable, graph, codec or service publication.
 * Remaining shared-memory/board/DMA/graph fields must still be bound before
 * na_assembly_init; an incomplete plan continues to be rejected there. */
int na_native_prepare_adsp_inputs(NaNativeAdspInputs *,NaAssemblyPlan *);
/* Distinguish owned WB metadata from exact leased WC firmware aliases. Never
 * pass an arbitrary peripheral PA to XNU's RAM physmap cache operation. */
int na_native_pas_sync(void *,void *cpu,uint64_t physical,uint64_t bytes);
/* The verified board watchdog route, shared by control and playback modes.
 * Configure only after both PAS masters stopped; retain mappings on failure.
 * PDC remains read-only. Only owned SPI751/bit15 may be changed. */
typedef struct {
    NaNativeWindow pdc,spi;
    uint32_t irq,pdcVersion,pdcParameters,pdcBefore,spiBefore,spiAfter,attempted;
    uint64_t router;
    int error;
} NaNativeAdspWatchdog;
int na_native_adsp_watchdog_prepare(NaNativeAdspWatchdog *,NaResourceOwner *,NaAdsp *,uint64_t lease);
#ifdef __cplusplus
}
#endif
#endif
