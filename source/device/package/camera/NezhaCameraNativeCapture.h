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
    /* A standby snapshot is qualified at the first completed frame;
     * subsequent snapshots use the actual completed-frame read boundary.
     * The frame adapter still waits two more boundaries before publishing. */
    void (*readback)(void *, const struct NezhaCameraOvxControlReadback *,
        uint32_t qualified_after);
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
    /* Keep the existing cleanup values used by retained run decoders. */
    NezhaNativeStopCsid,
};

/* Bounded wall-clock maxima from the existing worker/callback path.
 * Completion ticks come from the IRQ owner; no additional IRQ or MMIO
 * reads are performed. Overlapping waits are not summed as CPU work. */
struct NezhaCameraNativeFrameWork {
    uint32_t deliveries, returns;
    uint64_t queue_max, acquire_max, delivery_max, publish_max;
    uint64_t completion_to_publish_max, control_write_max, control_read_max;
    uint32_t replacements;
    uint64_t completion_to_address_max;
    uint32_t copy_service_calls, copy_recycled_frames;
    uint64_t copy_service_max;
};

struct NezhaCameraNativeCaptureResult {
    unsigned step, status;
    enum NezhaCameraNativeCleanupStep cleanup_step;
    unsigned capture_stage, sensor_status, dma_status;
    unsigned control_serviced_frames;
    struct NezhaCameraIfeBcmFailure bandwidth_failure;
    size_t applied_writes;
    size_t raw_sample_bytes;
    uint32_t irq_fault;
    bool irq_stopped;
    bool csid_stop_attempted, csid_stop_acknowledged;
    uint32_t csid_stop_reset_status;
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
    bool control_readback_before_stream;
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
    struct NezhaCameraNativeMemoryFailure memory_failure;
    struct NezhaCameraNativeFrameWork work;
};

/* One normal capture worker. Diagnostic mode stops at 20 frames; a bound
 * consumer runs until its Stop gate closes and can restart only after
 * complete successful teardown. IRQ contexts remain mapped for the boot
 * lifetime. An error remains in the result and current read; only complete
 * teardown permits a new reservation. Indeterminate cleanup retains state. */
bool nezha_camera_native_capture(const struct NezhaCameraNativeCaptureRequest *request,
    struct NezhaCameraNativeCaptureResult *result);

/* Continuous session: Stop only closes the worker's gate; it never waits
 * from the delivering callback. The platform joins the worker before
 * destroying the client's context or starting another session. */
void nezha_camera_native_capture_request_stop(void);
bool nezha_camera_native_capture_running(void);
/* A completed cleanup releases every owner even when the stream failed.
 * This does not clear the caller's error or restart the current session. */
bool nezha_camera_native_capture_can_reopen(void);
/* Only the synchronous consumer of the current completed frame may call
 * this, between its copy chunks and after releasing its own client mutex.
 * The original capture worker alone drains its already ACKed IRQ queue. */
bool nezha_camera_native_capture_service_copy(const uint8_t *completed_frame);

#ifdef __cplusplus
}
#endif
#endif
