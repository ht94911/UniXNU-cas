/* SPDX-License-Identifier: MIT */
#include "NezhaCameraMainCapture.h"

static bool valid_dma_owner(const struct NezhaCameraMainCaptureIo *io)
{
    return (io->dma_enter == NULL) == (io->dma_leave == NULL);
}

bool nezha_camera_main_capture_start(const struct NezhaCameraMainCaptureIo *io,
    const struct NezhaCameraMainMode *mode, const struct NezhaCameraDmaPlan *buffers,
    const ArmSmmu4KContextConfig *context, unsigned slot, unsigned bank,
    unsigned poll_budget, struct NezhaCameraMainCapture *capture)
{
    struct NezhaCameraRdi1080 layout;
    if (io == NULL || io->cci == NULL || io->camss == NULL || io->smmu == NULL || !valid_dma_owner(io) ||
        mode == NULL || mode->expected_identity == 0 || buffers == NULL ||
        context == NULL || context->lower_tables != &buffers->tables || capture == NULL ||
        capture->stage != NezhaCaptureIdle ||
        (mode->input.source != NezhaCamera1080Dphy && mode->input.source != NezhaCamera1080Cphy) ||
        !nezha_camera_rdi1080_layout(mode->width, mode->height, buffers->frames, &layout)) {
        return false;
    }

    capture->stage = NezhaCaptureIdentity;
    capture->sensor_status = nezha_camera_ovx10500u_read_identity(io->cci, &capture->identity);
    capture->sensor_transfer = capture->identity.transfer;
    const uint16_t chip = (uint16_t)((uint16_t)capture->identity.bytes[0] << 8) |
        capture->identity.bytes[1];
    if (capture->sensor_status != NezhaCciOk || chip != mode->expected_identity) {
        return false;
    }

    capture->stage = NezhaCaptureStandby;
    capture->sensor_status = nezha_camera_ovx10500u_apply_mode(io->cci, mode->standby,
        mode->standby_count, io->cci_queue_words, &capture->applied_writes, &capture->sensor_transfer);
    if (capture->sensor_status != NezhaCciOk) {
        return false;
    }

    capture->stage = NezhaCaptureDma;
    if (io->dma_enter != NULL && !io->dma_enter(io->dma_owner)) {
        capture->dma_status = ArmSmmu4KBusy;
        return false;
    }
    capture->dma_status = nezha_camera_attach_ife_dma(io->smmu, slot, bank, context,
        poll_budget, &capture->dma, &capture->dma_readback);
    if (io->dma_leave != NULL) {
        io->dma_leave(io->dma_owner);
    }
    if (capture->dma_status != ArmSmmu4KSuccess) {
        return false;
    }

    capture->stage = NezhaCaptureReceiver;
    capture->rdi_started = nezha_camera1080_start_rdi0(io->camss, &mode->input, mode->width,
        mode->height, buffers->frames);
    if (!capture->rdi_started) {
        return false;
    }
    capture->layout = layout;
    capture->queued[0] = 0;
    capture->queued[1] = 1;
    capture->queued_count = 2;
    capture->completed_frames = 0;
    capture->cpu_frames = 0;

    capture->stage = NezhaCaptureStreamRequest;
    capture->stream_may_be_on = true;
    capture->sensor_status = nezha_camera_ovx10500u_set_stream(io->cci, true, &capture->sensor_transfer);
    return capture->sensor_status == NezhaCciOk;
}

bool nezha_camera_main_capture_complete_frame(struct NezhaCameraMainCapture *capture,
    const struct NezhaCamera1080Events *events, unsigned *buffer)
{
    if (capture == NULL || events == NULL || buffer == NULL || !events->buffer_done ||
        !capture->rdi_started || !capture->dma.context_active ||
        capture->queued_count == 0 || capture->queued_count > 2) {
        return false;
    }
    const unsigned index = capture->queued[0];
    /* An interrupt can coalesce multiple completions. Do not infer that
     * the queue head completed if hardware reports a different address. */
    if (index > 1 || (capture->cpu_frames & (1u << index)) != 0 ||
        events->consumed_address != capture->layout.image_address[index]) {
        return false;
    }
    capture->queued[0] = capture->queued[1];
    --capture->queued_count;
    capture->cpu_frames |= 1u << index;
    ++capture->completed_frames;
    *buffer = index;
    return true;
}

bool nezha_camera_main_capture_release_frame(const struct NezhaCameraMainCaptureIo *io,
    struct NezhaCameraMainCapture *capture, unsigned buffer, bool requeue)
{
    if (capture == NULL || buffer > 1 || (capture->cpu_frames & (1u << buffer)) == 0) {
        return false;
    }
    if (requeue) {
        if (io == NULL || !capture->rdi_started || !capture->stream_may_be_on ||
            !capture->dma.context_active || capture->queued_count >= 2 ||
            !nezha_camera1080_requeue_rdi0(io->camss, &capture->layout, buffer)) {
            return false;
        }
        capture->queued[capture->queued_count++] = (uint8_t)buffer;
    }
    capture->cpu_frames &= ~(1u << buffer);
    return true;
}

bool nezha_camera_main_capture_request_stop(const struct NezhaCameraMainCaptureIo *io,
    struct NezhaCameraMainCapture *capture)
{
    if (io == NULL || capture == NULL || capture->sensor_transfer.needs_recovery) {
        return false;
    }
    if (capture->stream_may_be_on) {
        capture->sensor_status = nezha_camera_ovx10500u_set_stream(
            io->cci, false, &capture->sensor_transfer);
        if (capture->sensor_status != NezhaCciOk) {
            return false;
        }
        capture->stream_may_be_on = false;
    }
    if (capture->rdi_started) {
        if (io->camss == NULL || io->camss->write_csid == NULL || io->camss->write_vfe == NULL) {
            return false;
        }
        nezha_camera1080_request_rdi0_stop(io->camss);
        capture->rdi_started = false;
    }
    return true;
}

bool nezha_camera_main_capture_finish_stop(const struct NezhaCameraMainCaptureIo *io,
    const ArmSmmu4KContextConfig *context, unsigned poll_budget,
    struct NezhaCameraMainCapture *capture, bool dma_drained)
{
    if (io == NULL || !valid_dma_owner(io) || capture == NULL || capture->stream_may_be_on ||
        capture->rdi_started || capture->sensor_transfer.needs_recovery || capture->cpu_frames != 0) {
        return false;
    }
    if (!dma_drained && capture->stage >= NezhaCaptureReceiver) {
        return false;
    }
    capture->queued_count = 0;
    if (capture->dma.context_active || capture->dma.needs_hardware_recovery) {
        if (io->dma_enter != NULL && !io->dma_enter(io->dma_owner)) {
            capture->dma_status = ArmSmmu4KBusy;
            return false;
        }
        capture->dma_status = nezha_camera_detach_ife_dma(io->smmu, context, poll_budget,
            &capture->dma, &capture->dma_readback);
        if (io->dma_leave != NULL) {
            io->dma_leave(io->dma_owner);
        }
        if (capture->dma_status != ArmSmmu4KSuccess) {
            return false;
        }
    }
    capture->stage = NezhaCaptureIdle;
    return true;
}
