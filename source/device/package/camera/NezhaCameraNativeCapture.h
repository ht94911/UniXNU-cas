/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_NATIVE_CAPTURE_H
#define NEZHA_CAMERA_NATIVE_CAPTURE_H

#include "NezhaCameraCaptureResources.h"
#include "NezhaCameraFrameProbe.h"
#include "NezhaCameraProfile.h"
#include "NezhaCameraNativeSmmu.h"
#include "NezhaCameraNativeIrq.h"
#include "NezhaCameraEepromAf.h"
#include "NezhaCameraAk7316.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A caller queues settings without touching CCI. The sole capture worker
 * takes one request after a completed-frame boundary and reports the actual
 * group ACK or first error. The boundary is not a claim that an already
 * queued RAW frame contains those settings. Callbacks run without DMA locks. */
struct NezhaCameraNativeControlConsumer {
    void *context;
    bool (*take)(void *, struct NezhaCameraOvxControls *);
    void (*complete)(void *, enum NezhaCameraCciStatus,
        const struct NezhaCameraOvxControlResult *, uint32_t completed_before);
    void (*readback)(void *, const struct NezhaCameraOvxControlReadback *,
        uint32_t sampled_after);
};

struct NezhaCameraNativeCaptureRequest {
    const uint8_t *profile;
    size_t profile_bytes;
    /* The board's internal TPG2 emits color bars through CSID0/RDI0.
     * This path does not power a physical camera or consume a profile. */
    bool test_pattern;
    /* The camera-only trial uses the same three acknowledged sensor
     * votes proven by 010. PHY/ARC requests use their stock software
     * ledger plus ACK; actual PHY and frame results qualify hardware. */
    bool isolated_sensor_votes;
    unsigned slot, bank, asid, cci_queue_words;
    const uint8_t *command_database;
    size_t database_bytes;
    /* Optional caller-owned kernel buffer for one packed RAW10 frame. */
    uint8_t *raw_sample;
    size_t raw_sample_capacity;
    /* Optional checkpoint from the existing worker, before MMIO stages. */
    bool (*checkpoint)(unsigned step, unsigned a, unsigned b, unsigned c);
    bool (*rpmh_checkpoint)(void *context, uint32_t address, uint32_t value, bool read);
    void (*rpmh_vote_acknowledged)(void *context, uint32_t address, uint32_t value, uint32_t ack);
    /* Optional continuous main-camera reader on the normal capture worker.
     * It gates Stop and completes its CPU copy before DMA reuse. Both
     * callbacks must finish promptly; raw_sample and TPG are diagnostic. */
    const struct NezhaCameraFrameConsumer *consumer;
    const struct NezhaCameraNativeControlConsumer *controls;
    /* Optional once-per-session AF preparation read, before receiver or
     * DMA starts. Read only EEPROM prefix68 and programmed actuator DAC.
     * This never writes the actuator, advertises AF or sets framefocus. */
    bool AF_readonly_probe;
};

enum NezhaCameraNativeCaptureStep {
    NezhaNativeIdle, NezhaNativeProfile, NezhaNativeSharedOwners, NezhaNativeResources,
    NezhaNativePins, NezhaNativeCci, NezhaNativeCsid, NezhaNativeIrq,
    NezhaNativeMemory, NezhaNativeSmmu, NezhaNativeStream, NezhaNativeFrames, NezhaNativeRevision,
    NezhaNativeIfeBandwidth, NezhaNativeRegisterMap, NezhaNativeGccOwner, NezhaNativeRpmhOwner,
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
    struct NezhaCameraIfeBcmFailure bandwidth_failure;
    size_t applied_writes;
    size_t raw_sample_bytes;
    uint32_t irq_fault;
    struct NezhaCameraNativeIrqSetup irq_setup;
    struct NezhaCamera1080Events irq_failure;
    struct NezhaCameraNativeSmmuFault smmu_fault;
    bool passed, cleaned;
    struct NezhaCameraCaptureResources resources;
    struct NezhaCameraFrameResult frames;
    struct NezhaCameraOvxIdentity identity;
    struct NezhaCameraOvxRevision revision;
    struct NezhaCameraCciObservation cci;
    struct NezhaCameraOvxControlResult control_result;
    uint32_t control_completed_before;
    enum NezhaCameraCciStatus control_status;
    bool control_attempted;
    struct NezhaCameraOvxControlReadback control_readback;
    uint32_t control_readback_after;
    enum NezhaCameraCciStatus control_readback_status;
    bool control_readback_verified;
    bool AF_readonly_attempted;
    enum NezhaCameraCciStatus AF_eeprom_status, AF_position_status;
    struct NezhaCameraEepromAfRead AF_eeprom;
    struct NezhaCameraAk7316Result AF_position;
    /* One bounded readback before teardown of a failed active stream.
     * Raw register values never count as completed frames. */
    uint32_t stream_readback_status, stream_readback;
    uint32_t stream_start_readback_status, stream_start_readback;
    uint32_t capture_registers[14];
    uint32_t rpmh_address, rpmh_ack, rpmh_reply;
};

/* One normal capture worker. Diagnostic mode stops at 20 frames; a bound
 * consumer runs until its Stop gate closes and can restart only after
 * complete successful teardown. IRQ contexts remain mapped for the boot
 * lifetime. Indeterminate cleanup or a fault retains the original state. */
bool nezha_camera_native_capture(const struct NezhaCameraNativeCaptureRequest *request,
    struct NezhaCameraNativeCaptureResult *result);

/* Continuous session: Stop only closes the worker's gate; it never waits
 * from the delivering callback. The platform joins the worker before
 * destroying the client's context or starting another session. */
void nezha_camera_native_capture_request_stop(void);
bool nezha_camera_native_capture_running(void);

#ifdef __cplusplus
}
#endif
#endif
