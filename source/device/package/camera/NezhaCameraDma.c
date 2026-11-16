/* SPDX-License-Identifier: MIT */
#include "NezhaCameraDma.h"

enum {
    TablePageBytes = 4096,
    /* msm_cam_smmu_ife in the stock DT reserves the first MiB. */
    CameraFirstIova = 0x100000,
};

ArmSmmu4KStatus nezha_camera_build_frame_span_tables(const ArmSmmu4KConfig *config,
    const struct NezhaCameraFrameMapping frames[2], uint32_t width, uint32_t height,
    const ArmSmmu4KRange *ranges, size_t range_count,
    uint64_t *tables, size_t capacity_pages, struct NezhaCameraDmaPlan *out)
{
    if (out == NULL) {
        return ArmSmmu4KArgument;
    }
    *out = (struct NezhaCameraDmaPlan){0};
    if (config == NULL || frames == NULL || ranges == NULL || range_count == 0 ||
        tables == NULL || config->input_bits != 32) {
        return ArmSmmu4KArgument;
    }

    struct NezhaCameraRdi1080 layout;
    if (!nezha_camera_rdi1080_layout(width, height, frames, &layout)) {
        return ArmSmmu4KBadRange;
    }
    const uint64_t page_mask = TablePageBytes - 1u;
    const uint64_t bytes = ((uint64_t)layout.layout.frame_bytes + page_mask) & ~page_mask;
    size_t cursor = 0;
    for (unsigned frame = 0; frame < 2; ++frame) {
        const uint64_t start = frames[frame].iova;
        if (start < CameraFirstIova || (start & page_mask) != 0 ||
            frames[frame].mapped_bytes < bytes || start > UINT64_MAX - bytes) {
            return ArmSmmu4KBadRange;
        }
        uint64_t next = start;
        const uint64_t end = start + bytes;
        while (next < end) {
            if (cursor == range_count) {
                return ArmSmmu4KBadRange;
            }
            const ArmSmmu4KRange *range = &ranges[cursor++];
            if (range->iova != next || range->bytes == 0 || range->bytes > end - next ||
                ((range->iova | range->physical | range->bytes) & page_mask) != 0 ||
                range->physical == 0 || range->physical > UINT64_MAX - range->bytes ||
                range->permissions != (ArmSmmu4KRead | ArmSmmu4KWrite) ||
                range->writeback != 0 || range->shareability != 2) {
                return ArmSmmu4KBadRange;
            }
            next += range->bytes;
        }
    }
    if (cursor != range_count) {
        return ArmSmmu4KBadRange;
    }
    /* The same physical page cannot represent two independent in-flight
     * camera buffers. Allocator spans are normally few and coalesced. */
    for (size_t i = 0; i < range_count; ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (ranges[i].physical < ranges[j].physical + ranges[j].bytes &&
                ranges[j].physical < ranges[i].physical + ranges[i].bytes) {
                return ArmSmmu4KOverlap;
            }
        }
    }

    ArmSmmu4KResult result;
    ArmSmmu4KStatus status = ArmSmmu4KBuild(config, ranges, range_count, tables, capacity_pages, &result);
    if (status != ArmSmmu4KSuccess) {
        return status;
    }
    out->frames[0] = (struct NezhaCameraFrameMapping){frames[0].iova, bytes};
    out->frames[1] = (struct NezhaCameraFrameMapping){frames[1].iova, bytes};
    out->tables = result;
    return ArmSmmu4KSuccess;
}

ArmSmmu4KStatus nezha_camera_build_frame_tables(const ArmSmmu4KConfig *config,
    const struct NezhaCameraDmaBacking backing[2], uint32_t width, uint32_t height,
    uint64_t *tables, size_t capacity_pages, struct NezhaCameraDmaPlan *out)
{
    if (backing == NULL || config == NULL || tables == NULL || out == NULL || config->input_bits != 32) {
        if (out != NULL) {
            *out = (struct NezhaCameraDmaPlan){0};
        }
        return ArmSmmu4KArgument;
    }
    const struct NezhaCameraFrameMapping frames[] = {
        {backing[0].iova, backing[0].bytes}, {backing[1].iova, backing[1].bytes}};
    struct NezhaCameraRdi1080 layout;
    if (!nezha_camera_rdi1080_layout(width, height, frames, &layout)) {
        if (out != NULL) {
            *out = (struct NezhaCameraDmaPlan){0};
        }
        return ArmSmmu4KBadRange;
    }
    const uint64_t bytes = ((uint64_t)layout.layout.frame_bytes + TablePageBytes - 1u) &
        ~(uint64_t)(TablePageBytes - 1u);
    ArmSmmu4KRange ranges[2];
    for (unsigned i = 0; i < 2; ++i) {
        ranges[i] = (ArmSmmu4KRange){backing[i].iova, backing[i].physical, bytes,
            ArmSmmu4KRead | ArmSmmu4KWrite, 0, 2};
    }
    return nezha_camera_build_frame_span_tables(config, frames, width, height,
        ranges, 2, tables, capacity_pages, out);
}

ArmSmmu4KStatus nezha_camera_attach_ife_dma(const ArmSmmu4KIo *io,
    unsigned slot, unsigned bank, const ArmSmmu4KContextConfig *context,
    unsigned poll_budget, struct NezhaCameraDmaAttachment *attachment,
    struct NezhaCameraDmaReadback *readback)
{
    if (io == NULL || context == NULL || attachment == NULL || readback == NULL ||
        context->lower == NULL || context->lower->input_bits != 32 || context->upper != NULL) {
        return ArmSmmu4KArgument;
    }
    *readback = (struct NezhaCameraDmaReadback){0};
    if (attachment->context_active || attachment->stream.attempted ||
        attachment->needs_hardware_recovery) {
        return ArmSmmu4KBusy;
    }

    const ArmSmmu4KStream route = {slot, NezhaCameraIfeSid};
    ArmSmmu4KStatus status = ArmSmmu4KCheckFaultedStreams(io, &route, 1, bank, &readback->stream);
    if (status != ArmSmmu4KSuccess) {
        return status;
    }
    attachment->bank = bank;
    status = ArmSmmu4KProgramDisabledContext(io, bank, context, poll_budget, &readback->context);
    if (status != ArmSmmu4KSuccess) {
        attachment->needs_hardware_recovery = readback->context.writes != 0 &&
            readback->context.must_retain_tables != 0;
        return status;
    }
    attachment->context_active = true;
    status = ArmSmmu4KAttachStreams(io, &attachment->stream, &route, 1, bank,
                                   context, &readback->stream);
    attachment->needs_hardware_recovery = attachment->stream.poisoned != 0;
    return status;
}

ArmSmmu4KStatus nezha_camera_detach_ife_dma(const ArmSmmu4KIo *io,
    const ArmSmmu4KContextConfig *context, unsigned poll_budget,
    struct NezhaCameraDmaAttachment *attachment,
    struct NezhaCameraDmaReadback *readback)
{
    if (io == NULL || context == NULL || attachment == NULL || readback == NULL) {
        return ArmSmmu4KArgument;
    }
    *readback = (struct NezhaCameraDmaReadback){0};
    if (attachment->needs_hardware_recovery) {
        return ArmSmmu4KStateMismatch;
    }
    if (attachment->stream.attached) {
        ArmSmmu4KStatus status = ArmSmmu4KFaultStreams(io, &attachment->stream, &readback->stream);
        if (status != ArmSmmu4KSuccess) {
            attachment->needs_hardware_recovery = true;
            return status;
        }
    }
    if (attachment->context_active) {
        ArmSmmu4KStatus status = ArmSmmu4KDisableOwnedContext(io, attachment->bank,
            context, poll_budget, &readback->context);
        if (status != ArmSmmu4KSuccess) {
            attachment->needs_hardware_recovery = true;
            return status;
        }
    }
    *attachment = (struct NezhaCameraDmaAttachment){0};
    return ArmSmmu4KSuccess;
}
