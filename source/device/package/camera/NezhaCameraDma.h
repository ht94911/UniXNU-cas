/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_DMA_H
#define NEZHA_CAMERA_DMA_H

#include "ArmSmmu4K.h"
#include "ArmSmmu4KStreams.h"
#include "NezhaCameraRdi1080.h"

enum { NezhaCameraIfeSid = 0x1c00 };

struct NezhaCameraDmaBacking {
    uint64_t physical;
    uint64_t iova;
    uint64_t bytes;
};

struct NezhaCameraDmaPlan {
    struct NezhaCameraFrameMapping frames[2];
    ArmSmmu4KResult tables;
};

/* Build inactive stage-1 tables for two independently owned frame buffers.
 * Supply the 32-bit low IOVA config and an owned native-4K table pool.
 * Backings are ordered by increasing IOVA. The platform owns cache sync,
 * apps-SMMU locking, bank/ASID allocation and exact IFE stream attachment. */
ArmSmmu4KStatus nezha_camera_build_frame_tables(const ArmSmmu4KConfig *config,
    const struct NezhaCameraDmaBacking backing[2], uint32_t width, uint32_t height,
    uint64_t *tables, size_t capacity_pages, struct NezhaCameraDmaPlan *out);

struct NezhaCameraDmaAttachment {
    unsigned bank;
    bool context_active;
    bool needs_hardware_recovery;
    ArmSmmu4KStreamState stream;
};

struct NezhaCameraDmaReadback {
    ArmSmmu4KProgramResult context;
    ArmSmmu4KStreamResult stream;
};

/* Start with a zeroed attachment. The platform owns the selected exact
 * faulting slot, bank, ASID, fault handling and cache-published tables under
 * the shared apps-SMMU lock. Keep the camera quiescent during attach/detach. */
ArmSmmu4KStatus nezha_camera_attach_ife_dma(const ArmSmmu4KIo *io,
    unsigned slot, unsigned bank, const ArmSmmu4KContextConfig *context,
    unsigned poll_budget, struct NezhaCameraDmaAttachment *attachment,
    struct NezhaCameraDmaReadback *readback);

/* Once the camera DMA is drained, fault IFE's stream and synchronize
 * the disabled context before freeing the underlying allocation. */
ArmSmmu4KStatus nezha_camera_detach_ife_dma(const ArmSmmu4KIo *io,
    const ArmSmmu4KContextConfig *context, unsigned poll_budget,
    struct NezhaCameraDmaAttachment *attachment,
    struct NezhaCameraDmaReadback *readback);

#endif
