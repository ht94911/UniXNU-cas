/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_MAIN_CAPTURE_H
#define NEZHA_CAMERA_MAIN_CAPTURE_H

#include "NezhaCameraCamss1080.h"
#include "NezhaCameraDma.h"
#include "NezhaCameraOvx10500u.h"

struct NezhaCameraMainCaptureIo {
    const struct NezhaCameraRegisterIo *cci;
    const struct NezhaCamera1080Io *camss;
    const ArmSmmu4KIo *smmu;
    unsigned cci_queue_words;
    /* Optional paired hooks. The native shared SMMU owner supplies both
     * when configured; CCI and receiver work run outside this section. */
    void *dma_owner;
    bool (*dma_enter)(void *owner);
    void (*dma_leave)(void *owner);
};

struct NezhaCameraMainMode {
    uint16_t expected_identity;
    uint32_t width;
    uint32_t height;
    const struct NezhaCameraOvxModeWrite *standby;
    size_t standby_count;
    struct NezhaCamera1080Input input;
};

enum NezhaCameraCaptureStage {
    NezhaCaptureIdle,
    NezhaCaptureIdentity,
    NezhaCaptureStandby,
    NezhaCaptureDma,
    NezhaCaptureReceiver,
    NezhaCaptureStreamRequest,
};

struct NezhaCameraMainCapture {
    enum NezhaCameraCaptureStage stage;
    enum NezhaCameraCciStatus sensor_status;
    ArmSmmu4KStatus dma_status;
    struct NezhaCameraOvxIdentity identity;
    struct NezhaCameraCciObservation sensor_transfer;
    struct NezhaCameraDmaReadback dma_readback;
    struct NezhaCameraDmaAttachment dma;
    struct NezhaCameraRdi1080 layout;
    uint32_t completed_frames;
    uint8_t queued[2];
    unsigned queued_count;
    unsigned cpu_frames;
    size_t applied_writes;
    bool rdi_started;
    bool stream_may_be_on;
};

/* Start with zeroed state. The platform owns live sensor power/CCI timing,
 * CSIPHY, camera clocks and DMA tables. The paired hooks cover only the
 * selected stream/slot/bank configuration; otherwise the caller owns it.
 * This only submits the stream. Count RUP and real BUF_DONE separately. */
bool nezha_camera_main_capture_start(const struct NezhaCameraMainCaptureIo *io,
    const struct NezhaCameraMainMode *mode, const struct NezhaCameraDmaPlan *buffers,
    const ArmSmmu4KContextConfig *context, unsigned slot, unsigned bank,
    unsigned poll_budget, struct NezhaCameraMainCapture *capture);

/* Consume exactly one BUF_DONE whose VFE consumed address agrees with
 * the queued head. A mismatch retains ownership and calls for a drain.
 * Keep all calls on the capture owner and synchronize before CPU access. */
bool nezha_camera_main_capture_complete_frame(struct NezhaCameraMainCapture *capture,
    const struct NezhaCamera1080Events *events, unsigned *buffer);

/* Drop the CPU's claim after copying/processing. Requeue only while the
 * receiver and DMA are active, after synchronizing for device access. */
bool nezha_camera_main_capture_release_frame(const struct NezhaCameraMainCaptureIo *io,
    struct NezhaCameraMainCapture *capture, unsigned buffer, bool requeue);

/* On a CCI failure retain the current mapping. Once the platform has
 * recovered that master, clear its recovery flag and retry stopping. */
bool nezha_camera_main_capture_request_stop(const struct NezhaCameraMainCaptureIo *io,
    struct NezhaCameraMainCapture *capture);

/* Pass dma_drained only after CPAS has acknowledged stop (or before any
 * capture start reached the receiver). A failed drain keeps the tables. */
bool nezha_camera_main_capture_finish_stop(const struct NezhaCameraMainCaptureIo *io,
    const ArmSmmu4KContextConfig *context, unsigned poll_budget,
    struct NezhaCameraMainCapture *capture, bool dma_drained);

#endif
