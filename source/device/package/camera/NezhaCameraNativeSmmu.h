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

ArmSmmu4KStatus nezha_camera_native_smmu_bind(struct NezhaCameraNativeSmmu *state,
    const struct NezhaCameraNativeMemory *memory);
/* The worker uses this pair around attachment. It installs the fault
 * interrupt and stock ACTLR under the same lock as context programming. */
bool nezha_camera_native_smmu_capture_enter(void *state);
void nezha_camera_native_smmu_capture_leave(void *state);
const ArmSmmu4KIo *nezha_camera_native_smmu_io(const struct NezhaCameraNativeSmmu *state);
const ArmSmmu4KContextConfig *nezha_camera_native_smmu_context(const struct NezhaCameraNativeSmmu *state);
uint32_t nezha_camera_native_smmu_fault(const struct NezhaCameraNativeSmmu *state);
struct NezhaCameraNativeSmmuFault {
    uint64_t address;
    uint32_t status, syndrome;
};
void nezha_camera_native_smmu_fault_info(const struct NezhaCameraNativeSmmu *state,
    struct NezhaCameraNativeSmmuFault *info);
/* After producer and CPAS drain, retire the IRQ and reconcile the fault.
 * Holds the configuration lock through detach and ACTLR restoration.
 * Caller must finish or quarantine with release before leaving. */
bool nezha_camera_native_smmu_prepare_detach(struct NezhaCameraNativeSmmu *state);

/* ArmCpu's selected SPI interface is boot-lifetime. After a successful
 * detach, its callback remains inert over a permanently mapped, disabled
 * context bank; do not free a mapping used by the fault handler. */
bool nezha_camera_native_smmu_release(struct NezhaCameraNativeSmmu **state,
    bool dma_detached);

#ifdef __cplusplus
}
#endif
#endif
