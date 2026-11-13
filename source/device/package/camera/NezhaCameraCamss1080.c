/* SPDX-License-Identifier: MIT */
#include "NezhaCameraCamss1080.h"

#include <stddef.h>

/* Full CSID/VFE register layout and the two initial buffer sequence come
 * from Qualcomm CAMSS v24, 11c60fc17fd5, csid_configure_stream and
 * vfe_enable_output_v2. The C-PHY selector uses Qualcomm's CSID v2
 * phy_type field. The Lite register pages have a different layout. */
enum {
    CsidResetConfig = 0x108,
    CsidResetCommand = 0x10c,
    CsidIrqCommand = 0x110,
    CsidRegUpdate = 0x114,
    CsidAddressUpdate = 0x118,
    CsidApplyUpdate = 0x11c,
    CsidTopIrqStatus = 0x180,
    CsidTopIrqClear = 0x188,
    CsidBufferIrqStatus = 0x1a0,
    CsidBufferIrqClear = 0x1a8,
    CsidRxIrqStatus = 0x1b0,
    CsidRdi0IrqStatus = 0x224,
    CsidRdi0IrqClear = 0x22c,
    CsidRxConfig0 = 0x880,
    CsidRxConfig1 = 0x884,
    CsidRxCphy = 1 << 24,
    CsidRdi0Config0 = 0x5480,
    CsidRdi0Control = 0x5488,
    CsidRdi0Config1 = 0x5494,
    CsidRdi0PixelStore = 0x5498,
    CsidUpdateRdi0 = 1 << 8,
    CsidRdi0MipiPacking = 1 << 15,
    CsidRdi0BufferDone = 1 << 16,
    CsidTopRdi0 = 1 << 16,
    CsidTopBuffer = 1 << 3,
    CsidResetDone = 1,
    CsidRdiUpdated = 1 << 23,
    CsidRdiInputSof = 1 << 12,
    CsidRdiInputEof = 1 << 9,
    CsidRdiCamifSof = 1 << 4,
    CsidRdiCamifEof = 1 << 3,
    VfeRdi0Config = 0x2c00,
    VfeRdi0ImageAddress = 0x2c04,
    VfeRdi0AddressConfig = 0x2c70,
    VfeRdi0LastConsumedAddress = 0x2c90,
    VfeFrameModeEnable = (1 << 16) | 1,
};

bool nezha_camera1080_prepare_csid(const struct NezhaCameraRegisterIo *csid,
    uint32_t *reset_status)
{
    if (csid == NULL || csid->read == NULL || csid->write == NULL ||
        csid->wait_ms == NULL || reset_status == NULL) {
        return false;
    }
    /* The selected full CSID is stopped and exclusively owned, as in the
     * Qualcomm CAMSS v24 csid_reset path. Keep mask programming with the
     * IRQ owner; here the worker polls reset status and clears owned bits. */
    const uint32_t top = csid->read(csid->context, CsidTopIrqStatus);
    const uint32_t buffer = csid->read(csid->context, CsidBufferIrqStatus);
    if (top == UINT32_MAX || buffer == UINT32_MAX) {
        return false;
    }
    if (top & CsidResetDone) {
        csid->write(csid->context, CsidTopIrqClear, CsidResetDone);
    }
    if (buffer & CsidRdi0BufferDone) {
        csid->write(csid->context, CsidBufferIrqClear, CsidRdi0BufferDone);
    }
    csid->write(csid->context, CsidIrqCommand, 1);
    *reset_status = csid->read(csid->context, CsidTopIrqStatus);
    if (*reset_status == UINT32_MAX || (*reset_status & CsidResetDone) != 0) {
        return false;
    }
    csid->write(csid->context, CsidResetConfig, 0x11);
    csid->write(csid->context, CsidResetCommand, 5);
    for (unsigned ms = 0; ms <= 100; ++ms) {
        *reset_status = csid->read(csid->context, CsidTopIrqStatus);
        if (*reset_status == UINT32_MAX) {
            return false;
        }
        if (*reset_status & CsidResetDone) {
            csid->write(csid->context, CsidTopIrqClear, CsidResetDone);
            csid->write(csid->context, CsidIrqCommand, 1);
            const uint32_t after = csid->read(csid->context, CsidTopIrqStatus);
            return after != UINT32_MAX && (after & CsidResetDone) == 0;
        }
        if (ms != 100) {
            csid->wait_ms(csid->context, 1);
        }
    }
    return false;
}

static const uint32_t raw10_vc0 =
    (0x2bu << 16) | (0xfu << 12) | (1u << 8) | (1u << 6) | (1u << 5);

static void apply_rdi0_update(const struct NezhaCamera1080Io *io, uint32_t update_register)
{
    io->write_csid(io->context, update_register, CsidUpdateRdi0);
    io->write_csid(io->context, CsidApplyUpdate, 1);
}

static bool receiver_config(const struct NezhaCamera1080Input *input, uint32_t *config)
{
    const bool cphy = input->source == NezhaCamera1080Cphy;
    const unsigned lane_limit = cphy ? 3 : 4;
    unsigned lanes;
    uint32_t route;
    switch (input->source) {
    case NezhaCamera1080Dphy:
    case NezhaCamera1080Cphy:
        if (input->index >= 6 || input->lane_count == 0 || input->lane_count > lane_limit) {
            return false;
        }
        lanes = input->lane_count;
        route = (input->index + 1u) << 20;
        if (cphy) {
            route |= CsidRxCphy;
        }
        break;
    case NezhaCamera1080Tpg:
        if (input->index >= 3) {
            return false;
        }
        lanes = 4;
        route = (1u << 27) | ((input->index + 1u) << 28);
        break;
    default:
        return false;
    }

    uint32_t lane_select = 0;
    uint32_t seen = 0;
    for (unsigned i = 0; i < lanes; ++i) {
        const unsigned position = input->source == NezhaCamera1080Tpg ? i : input->lanes[i];
        if (position >= lane_limit || (seen & (1u << position)) != 0) {
            return false;
        }
        seen |= 1u << position;
        lane_select |= position << (4u * i);
    }
    *config = route | (lane_select << 4) | (lanes - 1u);
    return true;
}

bool nezha_camera1080_start_rdi0(const struct NezhaCamera1080Io *io,
    const struct NezhaCamera1080Input *input, uint32_t width, uint32_t height,
    const struct NezhaCameraFrameMapping buffers[2])
{
    struct NezhaCameraRdi1080 vfe;
    uint32_t rx;
    if (io == NULL || io->write_csid == NULL || io->write_vfe == NULL || input == NULL ||
        !receiver_config(input, &rx) || !nezha_camera_rdi1080_layout(width, height, buffers, &vfe)) {
        return false;
    }

    io->write_csid(io->context, CsidRxConfig0, rx);
    io->write_csid(io->context, CsidRxConfig1, 1); /* CSI-2 ECC correction. */
    io->write_csid(io->context, CsidRdi0Control, 0);
    io->write_csid(io->context, CsidRdi0Config0, raw10_vc0);
    io->write_csid(io->context, CsidRdi0Config1, CsidRdi0MipiPacking);
    io->write_csid(io->context, CsidRdi0PixelStore, 9);

    nezha_camera_rdi1080_program_geometry(&vfe, io->context, io->write_vfe);
    /* The stock expanded-memory path stores address >> 8 plus a separate
     * low-byte offset. These owned frame IOVAs are 256-byte aligned. */
    io->write_vfe(io->context, VfeRdi0AddressConfig, 0);
    io->write_vfe(io->context, VfeRdi0Config, VfeFrameModeEnable);
    for (unsigned i = 0; i < 2; ++i) {
        io->write_vfe(io->context, VfeRdi0ImageAddress, vfe.image_address[i]);
    }

    io->write_csid(io->context, CsidRdi0Config0, raw10_vc0 | (1u << 31));
    for (unsigned i = 0; i < 2; ++i) {
        apply_rdi0_update(io, CsidAddressUpdate);
    }
    apply_rdi0_update(io, CsidRegUpdate);
    io->write_csid(io->context, CsidRdi0Control, 1);
    return true;
}

bool nezha_camera1080_take_rdi0_events(const struct NezhaCameraRegisterIo *csid,
    const struct NezhaCameraRegisterIo *vfe, struct NezhaCamera1080Events *events)
{
    if (csid == NULL || csid->read == NULL || csid->write == NULL ||
        vfe == NULL || vfe->read == NULL || events == NULL) {
        return false;
    }
    *events = (struct NezhaCamera1080Events){0};
    events->top_irq = csid->read(csid->context, CsidTopIrqStatus);
    events->rx_irq = csid->read(csid->context, CsidRxIrqStatus);
    events->buffer_irq = csid->read(csid->context, CsidBufferIrqStatus);
    events->rdi0_irq = csid->read(csid->context, CsidRdi0IrqStatus);
    if (events->top_irq == UINT32_MAX || events->rx_irq == UINT32_MAX ||
        events->buffer_irq == UINT32_MAX || events->rdi0_irq == UINT32_MAX) {
        return false;
    }

    const uint32_t sof = CsidRdiInputSof | CsidRdiCamifSof;
    const uint32_t eof = CsidRdiInputEof | CsidRdiCamifEof;
    const uint32_t rdi = events->rdi0_irq & (sof | eof | CsidRdiUpdated);
    const uint32_t buffer = events->buffer_irq & CsidRdi0BufferDone;
    if (buffer != 0) {
        /* Stock cam_vfe_bus_ver3 reads addr_status_0 for the RDI client
         * at BUF_DONE. A failed read must not publish a CPU-owned frame. */
        events->consumed_address = vfe->read(vfe->context, VfeRdi0LastConsumedAddress);
        if (events->consumed_address == UINT32_MAX) {
            return false;
        }
    }
    events->updated = (rdi & CsidRdiUpdated) != 0;
    events->start_of_frame = (rdi & sof) != 0;
    events->end_of_frame = (rdi & eof) != 0;
    events->buffer_done = buffer != 0;

    /* Keep aggregate status asserted if an unhandled child bit is pending. */
    uint32_t top = 0;
    if (rdi != 0 && rdi == events->rdi0_irq) {
        top |= events->top_irq & CsidTopRdi0;
    }
    if (buffer != 0 && buffer == events->buffer_irq) {
        top |= events->top_irq & CsidTopBuffer;
    }
    if (top != 0) {
        csid->write(csid->context, CsidTopIrqClear, top);
    }
    if (buffer != 0) {
        csid->write(csid->context, CsidBufferIrqClear, buffer);
    }
    if (rdi != 0) {
        csid->write(csid->context, CsidRdi0IrqClear, rdi);
    }
    if ((top | buffer | rdi) != 0) {
        csid->write(csid->context, CsidIrqCommand, 1);
    }
    return true;
}

bool nezha_camera1080_requeue_rdi0(const struct NezhaCamera1080Io *io,
    const struct NezhaCameraRdi1080 *layout, unsigned buffer)
{
    if (io == NULL || io->write_csid == NULL || io->write_vfe == NULL || layout == NULL ||
        buffer > 1 || layout->image_address[buffer] == 0) {
        return false;
    }
    io->write_vfe(io->context, VfeRdi0ImageAddress, layout->image_address[buffer]);
    apply_rdi0_update(io, CsidAddressUpdate);
    apply_rdi0_update(io, CsidRegUpdate);
    return true;
}

void nezha_camera1080_request_rdi0_stop(const struct NezhaCamera1080Io *io)
{
    io->write_csid(io->context, CsidRdi0Control, 0);
    io->write_csid(io->context, CsidRdi0Config0, raw10_vc0);
    apply_rdi0_update(io, CsidRegUpdate);
    io->write_vfe(io->context, VfeRdi0Config, 0);
}

enum {
    TpgVersion = 0x000,
    TpgVersion24 = 0x20040000,
    TpgControl = 0x064,
    TpgVc0Config = 0x068,
    TpgVc0Seed = 0x06c,
    TpgVc0Hblank = 0x070,
    TpgVc0Vblank = 0x074,
    TpgVc0Bars = 0x078,
    TpgDt0Size = 0x07c,
    TpgDt0Type = 0x080,
    TpgDt0Payload = 0x084,
    TpgCommand = 0x1f4,
    TpgReset = 1,
    TpgEnable = 1 << 4,
};

bool nezha_camera1080_start_tpg_bars(const struct NezhaCamera1080Io *io,
    const struct NezhaCameraRegisterIo *tpg, unsigned index,
    const struct NezhaCameraFrameMapping buffers[2])
{
    if (io == NULL || io->write_csid == NULL || io->write_vfe == NULL || buffers == NULL ||
        tpg == NULL || tpg->read == NULL || tpg->write == NULL || tpg->wait_ms == NULL ||
        index >= 3 || tpg->read(tpg->context, TpgVersion) != TpgVersion24) {
        return false;
    }

    /* Qualcomm v24 camss-tpg-gen1.c and the same-device TPG v2.4.0
     * register readback. VC0, one RAW10 data type, four lanes. */
    tpg->write(tpg->context, TpgCommand, TpgReset);
    tpg->wait_ms(tpg->context, 1);
    tpg->write(tpg->context, TpgVc0Config, 0);
    tpg->write(tpg->context, TpgVc0Seed, 0x12345678);
    tpg->write(tpg->context, TpgVc0Hblank, 1090);
    tpg->write(tpg->context, TpgVc0Vblank, 48);
    tpg->write(tpg->context, TpgVc0Bars, 0xa00);
    tpg->write(tpg->context, TpgDt0Size, (640u << 16) | 480u);
    tpg->write(tpg->context, TpgDt0Type, 0x2b);
    tpg->write(tpg->context, TpgDt0Payload, 0x20000be8);
    tpg->write(tpg->context, TpgControl, 0x30);
    if (tpg->read(tpg->context, TpgDt0Size) != ((640u << 16) | 480u) ||
        tpg->read(tpg->context, TpgDt0Type) != 0x2b ||
        tpg->read(tpg->context, TpgDt0Payload) != 0x20000be8) {
        return false;
    }
    const struct NezhaCamera1080Input input = {.source = NezhaCamera1080Tpg, .index = index};
    if (!nezha_camera1080_start_rdi0(io, &input, 640, 480, buffers)) {
        return false;
    }
    tpg->write(tpg->context, TpgCommand, TpgEnable);
    return true;
}

void nezha_camera1080_request_tpg_stop(const struct NezhaCamera1080Io *io,
    const struct NezhaCameraRegisterIo *tpg)
{
    tpg->write(tpg->context, TpgCommand, TpgReset);
    nezha_camera1080_request_rdi0_stop(io);
}
