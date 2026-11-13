/* SPDX-License-Identifier: MIT */
#include "NezhaCameraMainCapture.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The hardware helpers have their own tests. Record their observable
 * call order and make one sensor or DMA operation fail at a time. */
static char operations[16];
static unsigned operation_count;
static bool fail_attach, fail_stop;

static void event(char marker)
{
    assert(operation_count + 1 < sizeof(operations));
    operations[operation_count++] = marker;
    operations[operation_count] = 0;
}

enum NezhaCameraCciStatus nezha_camera_ovx10500u_read_identity(
    const struct NezhaCameraRegisterIo *cci, struct NezhaCameraOvxIdentity *identity)
{
    assert(cci != NULL);
    event('i');
    *identity = (struct NezhaCameraOvxIdentity){.bytes = {0x12, 0x34}, .bytes_read = 2};
    return NezhaCciOk;
}

enum NezhaCameraCciStatus nezha_camera_ovx10500u_apply_mode(
    const struct NezhaCameraRegisterIo *cci, const struct NezhaCameraOvxModeWrite *writes,
    size_t count, unsigned burst_queue_words, size_t *applied, struct NezhaCameraCciObservation *transfer)
{
    assert(cci != NULL && writes != NULL && count == 1 && burst_queue_words == 64);
    event('m');
    *applied = count;
    *transfer = (struct NezhaCameraCciObservation){0};
    return NezhaCciOk;
}

enum NezhaCameraCciStatus nezha_camera_ovx10500u_set_stream(
    const struct NezhaCameraRegisterIo *cci, bool running,
    struct NezhaCameraCciObservation *transfer)
{
    assert(cci != NULL);
    event(running ? 's' : 'x');
    *transfer = (struct NezhaCameraCciObservation){.needs_recovery = !running && fail_stop};
    return transfer->needs_recovery ? NezhaCciTimeout : NezhaCciOk;
}

ArmSmmu4KStatus nezha_camera_attach_ife_dma(const ArmSmmu4KIo *io,
    unsigned slot, unsigned bank, const ArmSmmu4KContextConfig *context,
    unsigned poll_budget, struct NezhaCameraDmaAttachment *attachment,
    struct NezhaCameraDmaReadback *readback)
{
    assert(io != NULL && slot == 2 && bank == 1 && context != NULL && poll_budget != 0);
    assert(readback != NULL);
    event('a');
    attachment->context_active = !fail_attach;
    return fail_attach ? ArmSmmu4KBusy : ArmSmmu4KSuccess;
}

ArmSmmu4KStatus nezha_camera_detach_ife_dma(const ArmSmmu4KIo *io,
    const ArmSmmu4KContextConfig *context, unsigned poll_budget,
    struct NezhaCameraDmaAttachment *attachment,
    struct NezhaCameraDmaReadback *readback)
{
    assert(io != NULL && context != NULL && poll_budget != 0 && readback != NULL);
    assert(attachment->context_active);
    event('d');
    attachment->context_active = false;
    return ArmSmmu4KSuccess;
}

bool nezha_camera1080_start_rdi0(const struct NezhaCamera1080Io *io,
    const struct NezhaCamera1080Input *input, uint32_t width, uint32_t height,
    const struct NezhaCameraFrameMapping buffers[2])
{
    assert(io != NULL && input != NULL && width == 640 && height == 480 && buffers != NULL);
    event('r');
    return true;
}

void nezha_camera1080_request_rdi0_stop(const struct NezhaCamera1080Io *io)
{
    assert(io != NULL);
    event('t');
}

bool nezha_camera1080_requeue_rdi0(const struct NezhaCamera1080Io *io,
    const struct NezhaCameraRdi1080 *layout, unsigned buffer)
{
    assert(io != NULL && layout->image_address[0] == 0x2000 && buffer == 0);
    event('q');
    return true;
}

static void unused_register_write(void *context, uint32_t offset, uint32_t value)
{
    (void)context;
    (void)offset;
    (void)value;
    assert(0);
}

int main(void)
{
    const struct NezhaCameraRegisterIo cci = {0};
    const struct NezhaCamera1080Io camss = {NULL, unused_register_write, unused_register_write};
    const ArmSmmu4KIo smmu = {0};
    const struct NezhaCameraMainCaptureIo io = {&cci, &camss, &smmu, 64};
    const struct NezhaCameraDmaPlan plan = {.frames = {{0x200000, 0x60000}, {0x300000, 0x60000}}};
    const ArmSmmu4KContextConfig context = {.lower_tables = &plan.tables};
    const struct NezhaCameraOvxModeWrite standby = {.reg = 0x3514, .value = 0x7e};
    struct NezhaCameraMainMode mode = {0x4321, 640, 480, &standby, 1,
                                      {NezhaCamera1080Cphy, 0, 3, {0, 1, 2}}};
    struct NezhaCameraMainCapture capture = {0};

    assert(!nezha_camera_main_capture_start(&io, &mode, &plan, &context, 2, 1, 100, &capture));
    assert(strcmp(operations, "i") == 0);
    assert(nezha_camera_main_capture_finish_stop(&io, &context, 100, &capture, false));

    operation_count = 0;
    mode.expected_identity = 0x1234;
    fail_attach = true;
    assert(!nezha_camera_main_capture_start(&io, &mode, &plan, &context, 2, 1, 100, &capture));
    assert(strcmp(operations, "ima") == 0 && capture.dma_status == ArmSmmu4KBusy);
    assert(nezha_camera_main_capture_finish_stop(&io, &context, 100, &capture, false));

    operation_count = 0;
    fail_attach = false;
    assert(nezha_camera_main_capture_start(&io, &mode, &plan, &context, 2, 1, 100, &capture));
    assert(strcmp(operations, "imars") == 0);
    assert(!nezha_camera_main_capture_finish_stop(&io, &context, 100, &capture, true));

    unsigned buffer = 9;
    struct NezhaCamera1080Events events = {.start_of_frame = true};
    assert(!nezha_camera_main_capture_complete_frame(&capture, &events, &buffer) && buffer == 9);
    events.buffer_done = true;
    events.consumed_address = 0x3000;
    assert(!nezha_camera_main_capture_complete_frame(&capture, &events, &buffer) && buffer == 9);
    assert(capture.queued_count == 2 && capture.cpu_frames == 0);
    events.consumed_address = 0x2000;
    assert(nezha_camera_main_capture_complete_frame(&capture, &events, &buffer) && buffer == 0);
    assert(nezha_camera_main_capture_release_frame(&io, &capture, 0, true));
    assert(!nezha_camera_main_capture_release_frame(&io, &capture, 0, true));
    events.consumed_address = 0x3000;
    assert(nezha_camera_main_capture_complete_frame(&capture, &events, &buffer) && buffer == 1);
    events.consumed_address = 0x2000;
    assert(nezha_camera_main_capture_complete_frame(&capture, &events, &buffer) && buffer == 0);
    assert(!nezha_camera_main_capture_complete_frame(&capture, &events, &buffer));
    assert(capture.completed_frames == 3 && capture.cpu_frames == 3);

    fail_stop = true;
    assert(!nezha_camera_main_capture_request_stop(&io, &capture));
    assert(strcmp(operations, "imarsqx") == 0);
    assert(capture.dma.context_active && !nezha_camera_main_capture_finish_stop(&io, &context, 100, &capture, true));
    fail_stop = false;
    capture.sensor_transfer.needs_recovery = false; /* model of owner recovery */
    assert(nezha_camera_main_capture_request_stop(&io, &capture));
    assert(strcmp(operations, "imarsqxxt") == 0);
    assert(!nezha_camera_main_capture_finish_stop(&io, &context, 100, &capture, true));
    assert(!nezha_camera_main_capture_release_frame(&io, &capture, 0, true));
    assert(nezha_camera_main_capture_release_frame(&io, &capture, 0, false));
    assert(nezha_camera_main_capture_release_frame(&io, &capture, 1, false));
    assert(!nezha_camera_main_capture_finish_stop(&io, &context, 100, &capture, false));
    assert(capture.dma.context_active);
    assert(nezha_camera_main_capture_finish_stop(&io, &context, 100, &capture, true));
    assert(strcmp(operations, "imarsqxxtd") == 0 && capture.stage == NezhaCaptureIdle);

    puts("Main camera verifies identity, starts DMA before stream-on, and hands off frames before teardown");
}
