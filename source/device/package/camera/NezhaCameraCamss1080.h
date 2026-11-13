/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_CAMSS1080_H
#define NEZHA_CAMERA_CAMSS1080_H

#include "NezhaCameraRegisterIo.h"
#include "NezhaCameraRdi1080.h"

#ifdef __cplusplus
extern "C" {
#endif

enum NezhaCamera1080Source {
    NezhaCamera1080Dphy,
    NezhaCamera1080Tpg,
    NezhaCamera1080Cphy,
};

struct NezhaCamera1080Input {
    enum NezhaCamera1080Source source;
    unsigned index;
    unsigned lane_count;
    /* Lane positions as consumed by the CSID, after PHY routing. */
    uint8_t lanes[4];
};

struct NezhaCamera1080Io {
    void *context;
    void (*write_csid)(void *, uint32_t offset, uint32_t value);
    void (*write_vfe)(void *, uint32_t offset, uint32_t value);
};

/* Reset only the owned, idle full CSID before enabling its RDI stream.
 * Use the vendor reset acknowledgement; this is separate from DMA drain. */
bool nezha_camera1080_prepare_csid(const struct NezhaCameraRegisterIo *csid,
    uint32_t *reset_status);

/* Queue VC0 RAW10 on full CSID1080 + full VFE1080 RDI0. The caller owns
 * both stopped blocks, their reset and clock domains, the configured input,
 * the DMA mappings, and IRQ setup. Success means the register sequence was
 * submitted. The caller still needs the RUP and actual buffer completion. */
bool nezha_camera1080_start_rdi0(const struct NezhaCamera1080Io *io,
    const struct NezhaCamera1080Input *input, uint32_t width, uint32_t height,
    const struct NezhaCameraFrameMapping buffers[2]);

struct NezhaCamera1080Events {
    uint32_t top_irq;
    uint32_t rx_irq;
    uint32_t rdi0_irq;
    uint32_t buffer_irq;
    uint32_t consumed_address;
    bool updated;
    bool start_of_frame;
    bool end_of_frame;
    bool buffer_done;
};

/* Take only RDI0 events owned by this stream. On BUF_DONE read TFE's
 * last-consumed address before acknowledging CSID, using the same address
 * units as WM_IMAGE_ADDR. The platform must preserve and diagnose other
 * IRQ status, including RX and unexpected RDI0 errors. */
bool nezha_camera1080_take_rdi0_events(const struct NezhaCameraRegisterIo *csid,
    const struct NezhaCameraRegisterIo *vfe, struct NezhaCamera1080Events *events);

/* Requeue one buffer after its CPU reader releases it and the platform
 * completes cache ownership transfer. Layout and mappings remain active. */
bool nezha_camera1080_requeue_rdi0(const struct NezhaCamera1080Io *io,
    const struct NezhaCameraRdi1080 *layout, unsigned buffer);

/* Request stop after the sensor has stopped sending frames. The caller
 * must establish DMA quiescence before freeing mappings or clocks. */
void nezha_camera1080_request_rdi0_stop(const struct NezhaCamera1080Io *io);

/* The optional 640×480 RAW10 color-bar source uses the same owned
 * receiver and DMA path. Start the generator only after RDI is armed. */
bool nezha_camera1080_start_tpg_bars(const struct NezhaCamera1080Io *io,
    const struct NezhaCameraRegisterIo *tpg, unsigned index,
    const struct NezhaCameraFrameMapping buffers[2]);

/* The caller still verifies DMA drain and releases its mappings. */
void nezha_camera1080_request_tpg_stop(const struct NezhaCamera1080Io *io,
    const struct NezhaCameraRegisterIo *tpg);

#ifdef __cplusplus
}
#endif
#endif
