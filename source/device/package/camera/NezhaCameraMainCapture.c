/* SPDX-License-Identifier: MIT */
#include "NezhaCameraMainCapture.h"

static bool valid_dma_owner(const struct NezhaCameraMainCaptureIo *io)
{
    return (io->dma_enter == NULL) == (io->dma_leave == NULL);
}

static unsigned frame_count(const struct NezhaCameraMainCapture *capture)
{
    return capture->layout.frame_count ? capture->layout.frame_count : NezhaCameraRdi1080QueuedFrames;
}

bool nezha_camera_main_capture_start(const struct NezhaCameraMainCaptureIo *io,
    const struct NezhaCameraMainMode *mode, const struct NezhaCameraDmaPlan *buffers,
    const ArmSmmu4KContextConfig *context, unsigned slot, unsigned bank,
    unsigned poll_budget, struct NezhaCameraMainCapture *capture)
{
    struct NezhaCameraRdi1080 layout;
    if (io == NULL || io->camss == NULL || io->smmu == NULL || !valid_dma_owner(io) ||
        mode == NULL || buffers == NULL ||
        context == NULL || context->lower_tables != &buffers->tables || capture == NULL ||
        capture->stage != NezhaCaptureIdle ||
        !nezha_camera_rdi1080_pool_layout(mode->width, mode->height, buffers->frames,
            buffers->frame_count ? buffers->frame_count : NezhaCameraRdi1080QueuedFrames, &layout)) {
        return false;
    }

    capture->uses_tpg = mode->input.source == NezhaCamera1080Tpg;
    if (capture->uses_tpg) {
        if (io->tpg == NULL || mode->input.index >= 3 || mode->width != 640 || mode->height != 480) {
            return false;
        }
    } else {
        if (io->cci == NULL || mode->expected_identity == 0 ||
            (mode->input.source != NezhaCamera1080Dphy && mode->input.source != NezhaCamera1080Cphy)) {
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
        if (io->standby_controls != NULL) {
            struct NezhaCameraStandbyControls *initial = io->standby_controls;
            initial->attempted = true;
            initial->status = nezha_camera_ovx10500u_read_controls(io->cci, &initial->sample);
            capture->sensor_status = initial->status;
            capture->sensor_transfer = initial->sample.transfer;
            if (initial->status != NezhaCciOk) {
                return false;
            }
        }
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
    capture->rdi_started = capture->uses_tpg ?
        nezha_camera1080_start_tpg_bars(io->camss, io->tpg, mode->input.index, buffers->frames) :
        nezha_camera1080_start_rdi0(io->camss, &mode->input, mode->width, mode->height, buffers->frames);
    if (!capture->rdi_started) {
        return false;
    }
    capture->layout = layout;
    capture->queued[0] = 0;
    capture->queued[1] = 1;
    capture->queued_count = 2;
    capture->completed_frames = 0;
    capture->cpu_frames = 0;
    capture->ready_frames = layout.frame_count == NezhaCameraRdi1080MaxFrames ? 1u << 2 : 0;

    capture->stage = NezhaCaptureStreamRequest;
    capture->stream_may_be_on = true;
    if (!capture->uses_tpg) {
        capture->sensor_status = nezha_camera_ovx10500u_set_stream(io->cci, true, &capture->sensor_transfer);
    }
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
    if (frame_count(capture) > NezhaCameraRdi1080MaxFrames || index >= frame_count(capture) ||
        ((capture->cpu_frames | capture->ready_frames) & (1u << index)) != 0 ||
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

bool nezha_camera_main_capture_queue_spare(const struct NezhaCameraMainCaptureIo *io,
    struct NezhaCameraMainCapture *capture)
{
    if (io == NULL || capture == NULL || frame_count(capture) != NezhaCameraRdi1080MaxFrames ||
        !capture->rdi_started || !capture->stream_may_be_on || !capture->dma.context_active ||
        capture->queued_count != NezhaCameraRdi1080QueuedFrames - 1u || !capture->ready_frames ||
        (capture->ready_frames & (capture->ready_frames - 1u)) != 0 || !capture->cpu_frames ||
        (capture->cpu_frames & (capture->cpu_frames - 1u)) != 0 ||
        (capture->ready_frames & capture->cpu_frames) != 0 ||
        (capture->ready_frames >> NezhaCameraRdi1080MaxFrames) != 0 ||
        (capture->cpu_frames >> NezhaCameraRdi1080MaxFrames) != 0 ||
        capture->queued[0] >= NezhaCameraRdi1080MaxFrames ||
        ((capture->cpu_frames | capture->ready_frames) & (1u << capture->queued[0])) != 0) return false;
    for (unsigned i = 0; i < NezhaCameraRdi1080MaxFrames; ++i) {
        if (!(capture->ready_frames & (1u << i))) continue;
        for (unsigned j = 0; j < capture->queued_count; ++j)
            if (capture->queued[j] == i) return false;
        if (!nezha_camera1080_requeue_rdi0(io->camss, &capture->layout, i)) return false;
        capture->queued[capture->queued_count++] = (uint8_t)i;
        capture->ready_frames &= ~(1u << i);
        return true;
    }
    return false;
}

bool nezha_camera_main_capture_release_frame(const struct NezhaCameraMainCaptureIo *io,
    struct NezhaCameraMainCapture *capture, unsigned buffer, bool requeue)
{
    if (capture == NULL || frame_count(capture) > NezhaCameraRdi1080MaxFrames ||
        buffer >= frame_count(capture) || (capture->cpu_frames & (1u << buffer)) == 0) {
        return false;
    }
    if (requeue) {
        if (io == NULL || !capture->rdi_started || !capture->stream_may_be_on ||
            !capture->dma.context_active || capture->queued_count > NezhaCameraRdi1080QueuedFrames) {
            return false;
        }
        if (frame_count(capture) == NezhaCameraRdi1080MaxFrames) {
            /* The spare filled the address queue before this CPU read.
             * Mark this published buffer idle; never submit a third address. */
            if (capture->queued_count != NezhaCameraRdi1080QueuedFrames || capture->ready_frames ||
                capture->queued[0] >= frame_count(capture) || capture->queued[1] >= frame_count(capture) ||
                capture->queued[0] == capture->queued[1] || capture->queued[0] == buffer ||
                capture->queued[1] == buffer) return false;
            capture->ready_frames |= 1u << buffer;
        } else {
            if (capture->queued_count >= NezhaCameraRdi1080QueuedFrames ||
                !nezha_camera1080_requeue_rdi0(io->camss, &capture->layout, buffer)) return false;
            capture->queued[capture->queued_count++] = (uint8_t)buffer;
        }
    }
    capture->cpu_frames &= ~(1u << buffer);
    return true;
}

bool nezha_camera_main_capture_recycle_while_copying(const struct NezhaCameraMainCaptureIo *io,
    struct NezhaCameraMainCapture *capture, unsigned buffer, unsigned held_buffer)
{
    if (!io || !capture || frame_count(capture) != NezhaCameraRdi1080MaxFrames ||
        buffer >= NezhaCameraRdi1080MaxFrames || held_buffer >= NezhaCameraRdi1080MaxFrames ||
        buffer == held_buffer || !capture->rdi_started || !capture->stream_may_be_on ||
        !capture->dma.context_active || capture->ready_frames || capture->queued_count != 1 ||
        capture->cpu_frames != ((1u << buffer) | (1u << held_buffer)) ||
        capture->queued[0] >= NezhaCameraRdi1080MaxFrames ||
        capture->queued[0] == buffer || capture->queued[0] == held_buffer) return false;
    /* The older source remains CPU-owned. Only this newly completed,
     * sampled and republished mapping fills the address queue again. */
    if (!nezha_camera1080_requeue_rdi0(io->camss, &capture->layout, buffer)) return false;
    capture->queued[capture->queued_count++] = (uint8_t)buffer;
    capture->cpu_frames &= ~(1u << buffer);
    return true;
}

bool nezha_camera_main_capture_request_stop(const struct NezhaCameraMainCaptureIo *io,
    struct NezhaCameraMainCapture *capture)
{
    if (io == NULL || capture == NULL || capture->sensor_transfer.needs_recovery) {
        return false;
    }
    if (capture->stream_may_be_on && !capture->uses_tpg) {
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
        if (capture->uses_tpg) {
            if (io->tpg == NULL || io->tpg->write == NULL) {
                return false;
            }
            nezha_camera1080_request_tpg_stop(io->camss, io->tpg);
            capture->stream_may_be_on = false;
        } else {
            nezha_camera1080_request_rdi0_stop(io->camss);
        }
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
    capture->ready_frames = 0;
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
