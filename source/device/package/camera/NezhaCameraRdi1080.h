/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_RDI1080_H
#define NEZHA_CAMERA_RDI1080_H

#include <stdbool.h>
#include <stdint.h>

struct NezhaCameraRaw10Layout {
    uint32_t width;
    uint32_t height;
    uint32_t packed_line_bytes;
    uint32_t stride;
    uint32_t frame_bytes;
};

struct NezhaCameraFrameMapping {
    uint64_t iova;
    uint64_t mapped_bytes;
};

struct NezhaCameraRdi1080 {
    struct NezhaCameraRaw10Layout layout;
    uint32_t frame_increment;
    uint32_t image_address[2];
};

/* First capture uses two disjoint, camera-owned, 32-bit IOVA mappings.
 * These values describe the full VFE1080 RDI0 write master. */
bool nezha_camera_rdi1080_layout(uint32_t width, uint32_t height,
    const struct NezhaCameraFrameMapping buffers[2], struct NezhaCameraRdi1080 *out);

/* Program the geometry of full VFE1080 RDI0. The caller must hold a
 * verified, powered VFE instance and an exclusive, stopped write master.
 * The later capture sequence owns bus clocks, addresses, CSID updates,
 * IRQ registration and enabling or stopping DMA. */
void nezha_camera_rdi1080_program_geometry(const struct NezhaCameraRdi1080 *layout,
    void *context, void (*write)(void *, uint32_t offset, uint32_t value));

#endif
