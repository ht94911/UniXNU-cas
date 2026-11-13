/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_NATIVE_CAPTURE_H
#define NEZHA_CAMERA_NATIVE_CAPTURE_H

#include "NezhaCameraCaptureResources.h"
#include "NezhaCameraFrameProbe.h"
#include "NezhaCameraProfile.h"
#include "NezhaCameraNativeSmmu.h"

#ifdef __cplusplus
extern "C" {
#endif

struct NezhaCameraNativeCaptureRequest {
    const uint8_t *profile;
    size_t profile_bytes;
    /* Packaging records the actually matched stock module. A chip ID
     * alone cannot choose ordinary vs AS1 initialization. */
    bool module_match_confirmed;
    /* The board's internal TPG2 emits color bars through CSID0/RDI0.
     * This path does not power a physical camera or consume a profile. */
    bool test_pattern;
    unsigned slot, bank, asid, cci_queue_words;
    const uint8_t *command_database;
    size_t database_bytes;
    /* Optional caller-owned kernel buffer for one packed RAW10 frame. */
    uint8_t *raw_sample;
    size_t raw_sample_capacity;
    /* Optional checkpoint from the existing worker, before MMIO stages. */
    bool (*checkpoint)(unsigned step, unsigned a, unsigned b, unsigned c);
    bool (*rpmh_checkpoint)(void *context, uint32_t address, uint32_t value, bool read);
};

enum NezhaCameraNativeCaptureStep {
    NezhaNativeIdle, NezhaNativeProfile, NezhaNativeSharedOwners, NezhaNativeResources,
    NezhaNativePins, NezhaNativeCci, NezhaNativeCsid, NezhaNativeIrq,
    NezhaNativeMemory, NezhaNativeSmmu, NezhaNativeStream, NezhaNativeFrames,
};

enum NezhaCameraNativeCleanupStep {
    NezhaNativeCleanupIdle, NezhaNativeStopSensor, NezhaNativeDrain,
    NezhaNativeStopIrq, NezhaNativeDetach, NezhaNativeReleaseMemory,
    NezhaNativeRestorePins, NezhaNativeReleaseResources, NezhaNativeCleanupDone,
};

struct NezhaCameraNativeCaptureResult {
    unsigned step, status;
    enum NezhaCameraNativeCleanupStep cleanup_step;
    unsigned capture_stage, sensor_status, dma_status;
    size_t applied_writes;
    size_t raw_sample_bytes;
    uint32_t irq_fault;
    struct NezhaCamera1080Events irq_failure;
    struct NezhaCameraNativeSmmuFault smmu_fault;
    bool passed, cleaned;
    struct NezhaCameraCaptureResources resources;
    struct NezhaCameraFrameResult frames;
    struct NezhaCameraOvxIdentity identity;
    struct NezhaCameraCciObservation cci;
    uint32_t rpmh_address, rpmh_ack, rpmh_reply;
};

/* Source-only until integrated with the selected shared USB/SMMU owner.
 * One normal-thread capture per boot. All backing and IRQ contexts have
 * boot lifetime if hardware cannot confirm teardown. */
bool nezha_camera_native_capture(const struct NezhaCameraNativeCaptureRequest *request,
    struct NezhaCameraNativeCaptureResult *result);

#ifdef __cplusplus
}
#endif
#endif
