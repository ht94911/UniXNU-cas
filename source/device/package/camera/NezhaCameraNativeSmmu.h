/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_NATIVE_SMMU_H
#define NEZHA_CAMERA_NATIVE_SMMU_H

#include "NezhaCameraNativeMemory.h"

#ifdef __cplusplus
extern "C" {
#endif

struct NezhaCameraNativeSmmu;

/* The caller supplies the observed idle route and bank/ASID candidate
 * for the selected board. The shared SM8850 owner revalidates it while
 * serializing with USB and other apps SMMU users. A stream already routed
 * (even if masked to cover IFE) is never taken over. */
ArmSmmu4KStatus nezha_camera_native_smmu_prepare(struct NezhaCameraNativeSmmu **out,
    unsigned slot, unsigned bank, unsigned asid, unsigned *physical_bits);

/* The capture worker keeps this lock only across validation/programming
 * or fault/disable/TLBSYNC. Sensor transfers and frame cache work run
 * outside it. No allocation or waits with the common lock held. */
ArmSmmu4KStatus nezha_camera_native_smmu_enter(struct NezhaCameraNativeSmmu *state,
    const struct NezhaCameraNativeMemory *memory);
void nezha_camera_native_smmu_leave(struct NezhaCameraNativeSmmu *state);
/* After the first enter has bound the table descriptor and interrupt,
 * these paired hooks may be used directly by MainCapture. */
bool nezha_camera_native_smmu_capture_enter(void *state);
void nezha_camera_native_smmu_capture_leave(void *state);
const ArmSmmu4KIo *nezha_camera_native_smmu_io(const struct NezhaCameraNativeSmmu *state);
const ArmSmmu4KContextConfig *nezha_camera_native_smmu_context(const struct NezhaCameraNativeSmmu *state);
uint32_t nezha_camera_native_smmu_fault(const struct NezhaCameraNativeSmmu *state);

/* ArmCpu's selected SPI interface is boot-lifetime. After a successful
 * detach, its callback remains inert over a permanently mapped, disabled
 * context bank; do not free a mapping used by the fault handler. */
bool nezha_camera_native_smmu_release(struct NezhaCameraNativeSmmu **state,
    bool dma_detached);

#ifdef __cplusplus
}
#endif
#endif
