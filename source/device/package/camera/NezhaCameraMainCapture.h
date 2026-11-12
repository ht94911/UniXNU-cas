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
    size_t applied_writes;
    bool rdi_started;
    bool stream_may_be_on;
};

/* Start with zeroed state. The platform owns live sensor power/CCI timing,
 * CSIPHY, camera clocks, DMA tables and the selected stream/slot/bank lock.
 * This only submits the stream. Count RUP and real BUF_DONE separately. */
bool nezha_camera_main_capture_start(const struct NezhaCameraMainCaptureIo *io,
    const struct NezhaCameraMainMode *mode, const struct NezhaCameraDmaPlan *buffers,
    const ArmSmmu4KContextConfig *context, unsigned slot, unsigned bank,
    unsigned poll_budget, struct NezhaCameraMainCapture *capture);

/* On a CCI failure retain the current mapping. Once the platform has
 * recovered that master, clear its recovery flag and retry stopping. */
bool nezha_camera_main_capture_request_stop(const struct NezhaCameraMainCaptureIo *io,
    struct NezhaCameraMainCapture *capture);

/* Call after actual confirmation that writes to the frame buffers drained. */
bool nezha_camera_main_capture_finish_stop(const struct NezhaCameraMainCaptureIo *io,
    const ArmSmmu4KContextConfig *context, unsigned poll_budget,
    struct NezhaCameraMainCapture *capture);

#endif
