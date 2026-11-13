/* SPDX-License-Identifier: MIT */
#include "NezhaCameraCci.h"
#include "NezhaCameraOvx10500u.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* This fixture uses the stock main camera route, CCI0 master 1. The hardware
 * can signal completion and a fault at the same time. */
enum {
    M0Report = 1u << 4,
    M1ReadDone = 1u << 12,
    M1AddressReport = 1u << 16,
    M1DataReport = 1u << 20,
    M1AddressNack = 1u << 29,
    M1DataNack = 1u << 30,
};

struct Model {
    uint32_t commands[2][4];
    unsigned queued[2];
    unsigned submitted;
    unsigned writes;
    unsigned waits;
    unsigned fifo_reads;
    uint32_t irq;
    uint32_t address_result;
    uint32_t data_result;
    uint32_t fifo_level;
    uint32_t data_words;
    uint32_t fifo_word;
    uint32_t expected_write;
    uint32_t expected_write_tail;
    uint32_t expected_read;
    uint32_t timing[5];
    unsigned revision_step;
    uint8_t revision_vendor;
    bool revision_script, revision_read_nack;
    bool discard_load;
    bool halt_timeout;
    unsigned resets;
};

static uint32_t read32(void *context, uint32_t offset)
{
    struct Model *model = context;
    switch (offset) {
    case 0x504: return model->queued[0];
    case 0x604: return model->queued[1];
    case 0xc0c: return model->irq;
    case 0x21c: return model->fifo_level;
    case 0x218:
        assert(model->fifo_level != 0);
        --model->fifo_level;
        ++model->fifo_reads;
        return model->fifo_word;
    default:
        fprintf(stderr, "unexpected MMIO read at %#x\n", offset);
        abort();
    }
}

static void write32(void *context, uint32_t offset, uint32_t value)
{
    struct Model *model = context;
    ++model->writes;
    switch (offset) {
    case 0x034:
        assert(value == 2);
        if (!model->halt_timeout) {
            model->irq |= 1u << 26;
        }
        break;
    case 0x004:
        assert(value == 0x0003f001);
        ++model->resets;
        model->irq |= 1u << 24;
        model->fifo_level = 0;
        model->queued[0] = model->queued[1] = 0;
        break;
    case 0x200: case 0x204: case 0x208: case 0x20c: case 0x210:
        model->timing[(offset - 0x200u) / 4u] = value;
        break;
    case 0x510:
    case 0x610: {
        unsigned queue = offset == 0x510 ? 0 : 1;
        if (!model->discard_load) {
            assert(model->queued[queue] < 4);
            model->commands[queue][model->queued[queue]++] = value;
        }
        break;
    }
    case 0x500:
    case 0x600:
        assert(value == model->queued[offset == 0x500 ? 0 : 1]);
        break;
    case 0x008:
        ++model->submitted;
        if (model->revision_script) {
            static const uint32_t slave[] = {0x511, 0x511, 0x101, 0x101, 0x101, 0x101};
            static const uint32_t transfer[] = {0x00010029, 0x1a, 0x01000139, 0x000e9029, 0x1a, 0x00000139};
            const unsigned index = model->revision_step++;
            const unsigned queue = index == 1 || index == 4;
            assert(index < 6 && value == (queue ? 8u : 4u));
            assert(model->commands[queue][0] == slave[index] && model->commands[queue][1] == transfer[index]);
            assert(model->commands[queue][2] == 0x108);
            model->queued[queue] = 0;
            if (queue) {
                model->irq |= index == 4 && model->revision_read_nack ? M1DataNack : M1DataReport | M1ReadDone;
                model->fifo_level = !(index == 4 && model->revision_read_nack);
                model->fifo_word = index == 1 ? (uint32_t)model->revision_vendor << 8 : 0x4100;
            } else {
                model->irq |= M1AddressReport;
            }
            break;
        }
        if (value == 4) {
            assert(model->commands[0][0] == 0x101);
            assert(model->commands[0][1] == model->expected_write);
            const unsigned words = model->queued[0];
            if (words == 4) {
                assert(model->commands[0][2] == model->expected_write_tail);
            }
            assert(words >= 3 && model->commands[0][words - 1] == 0x108);
            model->irq |= model->address_result;
            model->queued[0] = 0;
        } else {
            assert(value == 8);
            assert(model->commands[1][0] == 0x101);
            assert(model->commands[1][1] == model->expected_read);
            assert(model->commands[1][2] == 0x108);
            model->irq |= model->data_result;
            model->fifo_level = model->data_words;
            model->queued[1] = 0;
        }
        break;
    case 0xc08:
        assert((value & M0Report) == 0);
        model->irq &= ~value;
        break;
    case 0xc00:
        assert(value == 1);
        break;
    default:
        fprintf(stderr, "unexpected MMIO write at %#x\n", offset);
        abort();
    }
}

static void wait_ms(void *context, unsigned milliseconds)
{
    struct Model *model = context;
    assert(milliseconds == 1 || (model->revision_script && milliseconds == 3));
    model->waits += milliseconds;
}

static struct Model initial(void)
{
    return (struct Model){
        .irq = M0Report,
        .address_result = M1AddressReport,
        .data_result = M1DataReport | M1ReadDone,
        .data_words = 1,
        .fifo_word = 0x12345621,
        .expected_write = 0x000a3029,
        .expected_read = 0x1a,
    };
}

static enum NezhaCameraCciStatus probe(struct Model *model, uint8_t *value,
                                      struct NezhaCameraCciObservation *observed)
{
    const struct NezhaCameraRegisterIo io = {model, read32, write32, wait_ms};
    return nezha_camera_cci_read_byte(&io, 1, 0x10, 0x300a, value, observed);
}

int main(void)
{
    struct Model m = initial();
    struct NezhaCameraCciObservation observed;
    uint8_t value = 0;
    assert(probe(&m, &value, &observed) == NezhaCciOk);
    assert(value == 0x56 && m.submitted == 2 && m.fifo_reads == 1);
    assert(m.irq == M0Report && !observed.needs_recovery);

    m = initial();
    value = 0xcc;
    m.address_result |= M1AddressNack;
    assert(probe(&m, &value, &observed) == NezhaCciNack);
    assert(value == 0xcc && m.submitted == 1 && m.fifo_reads == 0);
    assert(observed.phase == NezhaCciAddress && observed.needs_recovery);

    m = initial();
    m.data_result |= M1DataNack;
    assert(probe(&m, &value, &observed) == NezhaCciNack);
    assert(value == 0xcc && m.submitted == 2 && m.fifo_reads == 0);
    assert(observed.phase == NezhaCciData && observed.needs_recovery);

    m = initial();
    m.data_result = 0;
    assert(probe(&m, &value, &observed) == NezhaCciTimeout);
    assert(m.waits == 100 && m.fifo_reads == 0 && value == 0xcc);
    assert(observed.needs_recovery);

    m = initial();
    m.data_words = 2;
    assert(probe(&m, &value, &observed) == NezhaCciFifo);
    assert(m.fifo_reads == 0 && observed.needs_recovery);

    m = initial();
    m.fifo_level = 1;
    assert(probe(&m, &value, &observed) == NezhaCciBusy && m.writes == 0);
    m = initial();
    m.queued[1] = 1;
    assert(probe(&m, &value, &observed) == NezhaCciBusy && m.writes == 0);

    m = initial();
    m.discard_load = true;
    assert(probe(&m, &value, &observed) == NezhaCciQueue);
    assert(m.submitted == 0 && observed.needs_recovery);

    m = initial();
    m.expected_write = 0x7e143539;
    const struct NezhaCameraRegisterIo io = {&m, read32, write32, wait_ms};
    assert(nezha_camera_cci_write_byte(&io, 1, 0x10, 0x3514, 0x7e, &observed) == NezhaCciOk);
    assert(m.submitted == 1 && m.fifo_reads == 0 && !observed.needs_recovery);
    assert(observed.phase == NezhaCciWrite);

    m = initial();
    m.expected_write = 0x125a6f49;
    m.expected_write_tail = 0x34;
    assert(nezha_camera_cci_write_word(&io, 1, 0x10, 0x6f5a, 0x1234, &observed) == NezhaCciOk);
    assert(m.submitted == 1 && m.fifo_reads == 0 && !observed.needs_recovery);

    m = initial();
    m.expected_write = 0x7e143539;
    m.address_result = M1AddressNack;
    assert(nezha_camera_cci_write_byte(&io, 1, 0x10, 0x3514, 0x7e, &observed) == NezhaCciNack);
    assert(m.submitted == 1 && observed.needs_recovery);

    m = initial();
    assert(!nezha_camera_cci_standard_timing(&io, 1, 24000000) && m.writes == 0);
    assert(nezha_camera_cci_standard_timing(&io, 1, 37500000) && m.writes == 5);
    assert(m.timing[0] == 0xc900ae && m.timing[4] == 0x163);
    assert(nezha_camera_cci_standard_timing(&io, 1, 19200000));
    assert(m.timing[0] == 0x4e0072 && m.timing[4] == 0x161);

    m = initial();
    m.expected_write = 0x000b3029;
    m.expected_read = 0x2a;
    m.fifo_word = 0x00585021;
    struct NezhaCameraOvxIdentity identity;
    assert(nezha_camera_ovx10500u_read_identity(&io, &identity) == NezhaCciOk);
    assert(identity.bytes_read == 2 && m.submitted == 2 && m.fifo_reads == 1);
    assert(((unsigned)identity.bytes[0] << 8 | identity.bytes[1]) == NezhaCameraOvx10500uId);

    m = initial();
    m.expected_write = 0x000b3029;
    m.expected_read = 0x2a;
    m.data_result = M1DataNack;
    assert(nezha_camera_ovx10500u_read_identity(&io, &identity) == NezhaCciNack);
    assert(identity.bytes_read == 0 && m.submitted == 2);

    m = initial();
    size_t applied = 99;
    const struct NezhaCameraOvxModeWrite invalid[] = {{.reg = 0x3514, .value = 0x7e}, {.reg = 0x0100, .value = 1}};
    assert(nezha_camera_ovx10500u_apply_mode(&io, invalid, 2, 0, &applied, &observed) == NezhaCciInvalid);
    assert(applied == 0 && m.writes == 0);

    m.expected_write = 0x7e143539;
    const struct NezhaCameraOvxModeWrite standby[] = {{.reg = 0x3514, .value = 0x7e, .delay_ms = 1}};
    assert(nezha_camera_ovx10500u_apply_mode(&io, standby, 1, 0, &applied, &observed) == NezhaCciOk);
    assert(applied == 1 && m.submitted == 1 && m.waits == 1);
    m = initial();
    m.expected_write = 0x01000139;
    assert(nezha_camera_ovx10500u_set_stream(&io, true, &observed) == NezhaCciOk);

    m = initial();
    m.revision_script = true;
    m.revision_vendor = 3;
    struct NezhaCameraOvxRevision revision;
    assert(nezha_camera_ovx10500u_read_revision(&io, &revision) == NezhaCciOk);
    assert(revision.vendor == 3 && revision.version == 0x41 && revision.vendor_read && revision.version_read);
    assert(!revision.stream_may_be_on && m.submitted == 6 && m.waits == 3);

    m = initial();
    m.revision_script = true;
    m.revision_vendor = 9;
    assert(nezha_camera_ovx10500u_read_revision(&io, &revision) == NezhaCciInvalid);
    assert(revision.vendor_read && !revision.version_read && !revision.stream_may_be_on && m.submitted == 2);

    m = initial();
    m.revision_script = m.revision_read_nack = true;
    m.revision_vendor = 7;
    assert(nezha_camera_ovx10500u_read_revision(&io, &revision) == NezhaCciNack);
    assert(revision.stream_may_be_on && revision.transfer.needs_recovery && m.submitted == 5);

    m = initial();
    m.irq |= M1DataNack | (1u << 24) | (1u << 26);
    m.queued[1] = 3;
    m.fifo_level = 1;
    assert(nezha_camera_cci_recover_master(&io, 1, 37500000, &observed) == NezhaCciOk);
    assert(m.resets == 1 && m.irq == M0Report && m.timing[0] == 0xc900ae);
    assert(!observed.needs_recovery && observed.phase == NezhaCciReset);

    m = initial();
    m.halt_timeout = true;
    assert(nezha_camera_cci_recover_master(&io, 1, 37500000, &observed) == NezhaCciTimeout);
    assert(m.resets == 0 && observed.needs_recovery && observed.phase == NezhaCciHalt);

    puts("Camera CCI passed the address/data, IRQ ownership, error, timeout and FIFO checks");
}
