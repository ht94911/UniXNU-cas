/* SPDX-License-Identifier: MIT */
#include "NezhaCameraCci.h"

#include <stddef.h>

/* Register layout and FIFO packing cross-checked against i2c-qcom-cci.c at
 * Linux 9d66f26fefac and the nezha w33 probe in u0er ed8978ac0087. */
enum {
    CciResetCommand = 0x004,
    CciQueueStart = 0x008,
    CciHaltRequest = 0x034,
    CciSclControl = 0x100,
    CciSdaControl0 = 0x104,
    CciSdaControl1 = 0x108,
    CciSdaControl2 = 0x10c,
    CciMiscControl = 0x110,
    CciReadData = 0x118,
    CciReadLevel = 0x11c,
    CciQueueExec = 0x300,
    CciQueueCount = 0x304,
    CciQueueLoad = 0x310,
    CciIrqGlobalClear = 0xc00,
    CciIrqClear = 0xc08,
    CciIrqStatus = 0xc0c,
    CciIrqMask1 = 0xc10,
    CciIrqClear1 = 0xc14,
    CciIrqStatus1 = 0xc18,
    CciSetParam = 1,
    CciLock = 6,
    CciUnlock = 7,
    CciReport = 8,
    CciWrite = 9,
    CciRead = 10,
    CciReportIrqEnable = 1 << 8,
    CciResetAck = 1 << 24,
    CciTimeoutMs = 100,
};

static uint32_t queue_offset(unsigned master, unsigned queue)
{
    return 0x200u * master + 0x100u * queue;
}

static void clear_status(const struct NezhaCameraRegisterIo *io, uint32_t status)
{
    if (status != 0) {
        io->write(io->context, CciIrqClear, status);
        io->write(io->context, CciIrqGlobalClear, 1);
    }
}

static enum NezhaCameraCciStatus run_queue(
    const struct NezhaCameraRegisterIo *io, unsigned master, unsigned queue,
    uint32_t address_word, const uint32_t *transfer, unsigned words,
    struct NezhaCameraCciObservation *observation)
{
    const uint32_t offset = queue_offset(master, queue);
    const unsigned master_shift = 12u * master;
    const uint32_t report = 1u << (4u + master_shift + 4u * queue);
    const uint32_t read_done = queue == 1 ? 1u << master_shift : 0;
    const uint32_t nack = (3u << (27u + 2u * master));
    const uint32_t error = master == 0 ? 0x18000ee6u : 0x60ee6000u;
    const uint32_t handled = report | read_done | error;

    if (io->read(io->context, CciQueueCount + offset) != 0) {
        return NezhaCciBusy;
    }
    clear_status(io, io->read(io->context, CciIrqStatus) & handled);
    if ((io->read(io->context, CciIrqStatus) & handled) != 0) {
        return NezhaCciBusy;
    }

    observation->needs_recovery = true;
    io->write(io->context, CciQueueLoad + offset, address_word);
    for (unsigned i = 0; i < words; ++i) {
        io->write(io->context, CciQueueLoad + offset, transfer[i]);
    }
    io->write(io->context, CciQueueLoad + offset, CciReport | CciReportIrqEnable);
    if (io->read(io->context, CciQueueCount + offset) != words + 2) {
        return NezhaCciQueue;
    }
    io->write(io->context, CciQueueExec + offset, words + 2);
    io->write(io->context, CciQueueStart, 1u << (2u * master + queue));

    for (unsigned ms = 0; ms <= CciTimeoutMs; ++ms) {
        const uint32_t status = io->read(io->context, CciIrqStatus);
        observation->irq = status;
        if ((status & error) != 0) {
            clear_status(io, status & handled);
            return (status & nack) != 0 ? NezhaCciNack : NezhaCciIoError;
        }
        if ((status & report) != 0) {
            clear_status(io, status & handled);
            return NezhaCciOk;
        }
        if (ms != CciTimeoutMs) {
            io->wait_ms(io->context, 1);
        }
    }
    return NezhaCciTimeout;
}

static enum NezhaCameraCciStatus check_request(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint8_t slave,
    struct NezhaCameraCciObservation *observation)
{
    if (observation == NULL) {
        return NezhaCciInvalid;
    }
    *observation = (struct NezhaCameraCciObservation){0};
    if (io == NULL || io->read == NULL || io->write == NULL || io->wait_ms == NULL ||
        master > 1 || slave < 0x08 || slave > 0x77) {
        return NezhaCciInvalid;
    }

    const uint32_t fifo_offset = 0x100u * master;
    observation->fifo_words = io->read(io->context, CciReadLevel + fifo_offset);
    if (observation->fifo_words != 0 ||
        io->read(io->context, CciQueueCount + queue_offset(master, 1)) != 0) {
        return NezhaCciBusy;
    }
    return NezhaCciOk;
}

enum NezhaCameraCciStatus nezha_camera_cci_read(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint8_t slave,
    uint16_t reg, uint8_t *bytes, unsigned count, struct NezhaCameraCciObservation *observation)
{
    if (bytes == NULL || count == 0 || count > 3) {
        return NezhaCciInvalid;
    }
    enum NezhaCameraCciStatus status = check_request(io, master, slave, observation);
    if (status != NezhaCciOk) {
        return status;
    }

    const uint32_t address_word = CciSetParam | ((uint32_t)slave << 4);
    const uint32_t register_word = (CciWrite | (2u << 4)) |
        ((uint32_t)(reg >> 8) << 8) | ((uint32_t)(reg & 0xffu) << 16);
    observation->phase = NezhaCciAddress;
    status = run_queue(io, master, 0, address_word, &register_word, 1, observation);
    if (status != NezhaCciOk) {
        return status;
    }

    observation->phase = NezhaCciData;
    const uint32_t read_word = CciRead | ((uint32_t)count << 4);
    status = run_queue(io, master, 1, address_word, &read_word, 1, observation);
    if (status != NezhaCciOk) {
        return status;
    }

    const uint32_t fifo_offset = 0x100u * master;
    observation->fifo_words = io->read(io->context, CciReadLevel + fifo_offset);
    if (observation->fifo_words != 1) {
        return NezhaCciFifo;
    }
    observation->fifo_word = io->read(io->context, CciReadData + fifo_offset);
    /* The controller places a header byte before the requested bytes. */
    for (unsigned i = 0; i < count; ++i) {
        bytes[i] = (uint8_t)(observation->fifo_word >> (8u * (i + 1u)));
    }
    observation->needs_recovery = false;
    return NezhaCciOk;
}

enum NezhaCameraCciStatus nezha_camera_cci_read_byte(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint8_t slave,
    uint16_t reg, uint8_t *value, struct NezhaCameraCciObservation *observation)
{
    return nezha_camera_cci_read(io, master, slave, reg, value, 1, observation);
}

enum NezhaCameraCciStatus nezha_camera_cci_write_byte(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint8_t slave,
    uint16_t reg, uint8_t value, struct NezhaCameraCciObservation *observation)
{
    enum NezhaCameraCciStatus status = check_request(io, master, slave, observation);
    if (status != NezhaCciOk) {
        return status;
    }

    const uint32_t address_word = CciSetParam | ((uint32_t)slave << 4);
    /* Put address and data in one I2C message. A separate WRITE opcode
     * for the value would form a second I2C transaction. */
    const uint32_t write_word = (CciWrite | (3u << 4)) |
        ((uint32_t)(reg >> 8) << 8) | ((uint32_t)(reg & 0xffu) << 16) |
        ((uint32_t)value << 24);
    observation->phase = NezhaCciWrite;
    status = run_queue(io, master, 0, address_word, &write_word, 1, observation);
    if (status == NezhaCciOk) {
        observation->needs_recovery = false;
    }
    return status;
}

enum NezhaCameraCciStatus nezha_camera_cci_write_word(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint8_t slave,
    uint16_t reg, uint16_t value, struct NezhaCameraCciObservation *observation)
{
    enum NezhaCameraCciStatus status = check_request(io, master, slave, observation);
    if (status != NezhaCciOk) {
        return status;
    }
    const uint32_t address_word = CciSetParam | ((uint32_t)slave << 4);
    const uint32_t transfer[] = {(CciWrite | (4u << 4)) |
        ((uint32_t)(reg >> 8) << 8) | ((uint32_t)(reg & 0xffu) << 16) |
        ((uint32_t)(value >> 8) << 24), value & 0xffu};
    observation->phase = NezhaCciWrite;
    status = run_queue(io, master, 0, address_word, transfer, 2, observation);
    if (status == NezhaCciOk) {
        observation->needs_recovery = false;
    }
    return status;
}

struct Burst {
    const uint8_t *bytes;
    size_t count, sent;
    uint16_t reg;
    unsigned packets, half_packets;
};

static unsigned burst_packet(struct Burst *burst, uint32_t words[3])
{
    uint8_t packet[12] = {0};
    unsigned used = 1;
    if (burst->sent == 0) {
        packet[used++] = (uint8_t)(burst->reg >> 8);
        packet[used++] = (uint8_t)burst->reg;
    }
    while (used < sizeof(packet) && burst->sent < burst->count) {
        packet[used++] = burst->bytes[burst->sent++];
    }
    ++burst->packets;
    const unsigned wire_count = burst->sent == burst->count ? used - 1 :
        (burst->packets % burst->half_packets == 0 ? 14u : 15u);
    packet[0] = (uint8_t)(CciWrite | (wire_count << 4));
    const unsigned word_count = (used + 3u) / 4u;
    for (unsigned i = 0; i < word_count; ++i) {
        words[i] = 0;
        for (unsigned j = 0; j < 4; ++j) {
            words[i] |= (uint32_t)packet[i * 4u + j] << (j * 8u);
        }
    }
    return word_count;
}

static enum NezhaCameraCciStatus burst_wait(const struct NezhaCameraRegisterIo *io,
    unsigned master, uint32_t threshold, bool final, struct NezhaCameraCciObservation *observation)
{
    const uint32_t report = 1u << (4u + 12u * master);
    const uint32_t nack = 3u << (27u + 2u * master);
    const uint32_t errors = master == 0 ? 0x18000ee6u : 0x60ee6000u;
    for (unsigned ms = 0; ms <= CciTimeoutMs; ++ms) {
        const uint32_t irq = io->read(io->context, CciIrqStatus);
        const uint32_t irq1 = io->read(io->context, CciIrqStatus1);
        observation->irq = irq;
        if ((irq & errors) != 0 || (irq & report) != 0) {
            clear_status(io, irq & (errors | report));
            if ((irq & errors) != 0) {
                return (irq & nack) != 0 ? NezhaCciNack : NezhaCciIoError;
            }
            return final ? NezhaCciOk : NezhaCciQueue;
        }
        if ((irq1 & threshold) != 0) {
            io->write(io->context, CciIrqClear1, threshold);
            io->write(io->context, CciIrqGlobalClear, 1);
            if (!final) {
                return NezhaCciOk;
            }
        }
        if (ms != CciTimeoutMs) {
            io->wait_ms(io->context, 1);
        }
    }
    return NezhaCciTimeout;
}

enum NezhaCameraCciStatus nezha_camera_cci_write_burst(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint8_t slave,
    uint16_t reg, const uint8_t *bytes, size_t count, unsigned queue_words,
    struct NezhaCameraCciObservation *observation)
{
    if (bytes == NULL || count == 0 || count > UINT16_MAX ||
        queue_words < 16 || queue_words > NezhaCameraCciCanoeQueue0Words) {
        return NezhaCciInvalid;
    }
    enum NezhaCameraCciStatus status = check_request(io, master, slave, observation);
    if (status != NezhaCciOk) {
        return status;
    }
    const uint32_t offset = queue_offset(master, 0);
    const uint32_t threshold = 1u << (18u + master * 4u);
    const uint32_t owned_irq0 = (1u << (4u + 12u * master)) |
        (master == 0 ? 0x18000ee6u : 0x60ee6000u);
    const uint32_t mask = io->read(io->context, CciIrqMask1);
    if (mask == UINT32_MAX || io->read(io->context, CciQueueCount + offset) != 0 ||
        (io->read(io->context, CciIrqStatus) & owned_irq0) != 0 ||
        (io->read(io->context, CciIrqStatus1) & threshold) != 0) {
        return NezhaCciBusy;
    }

    observation->phase = NezhaCciBurst;
    io->write(io->context, CciIrqMask1, mask | threshold);
    if (io->read(io->context, CciIrqMask1) != (mask | threshold)) {
        io->write(io->context, CciIrqMask1, mask);
        return NezhaCciIoError;
    }
    observation->needs_recovery = true;
    const uint32_t load = CciQueueLoad + offset;
    io->write(io->context, load, CciSetParam | ((uint32_t)slave << 4));
    io->write(io->context, load, CciLock);
    struct Burst burst = {.bytes = bytes, .count = count, .reg = reg,
                          .half_packets = (queue_words - 4u) / 6u};
    unsigned prefix = 2;
    unsigned packets = 2u * burst.half_packets;
    while (burst.sent < burst.count) {
        const uint32_t queued = io->read(io->context, CciQueueCount + offset);
        const size_t bytes_left = burst.count - burst.sent + (burst.sent == 0 ? 2u : 0u);
        const size_t full_packets = bytes_left / 11;
        const size_t tail_bytes = bytes_left % 11;
        const size_t tail_words = tail_bytes ? (tail_bytes + 4) / 4 : 0;
        const size_t load_words = full_packets >= packets ? 3u * packets : 3u * full_packets + tail_words;
        if (queued > queue_words || load_words + 2u > queue_words - queued) {
            status = NezhaCciQueue;
            break;
        }
        unsigned submitted = prefix;
        for (unsigned i = 0; i < packets && burst.sent < burst.count; ++i) {
            uint32_t words[3];
            const unsigned n = burst_packet(&burst, words);
            for (unsigned j = 0; j < n; ++j) {
                io->write(io->context, load, words[j]);
            }
            submitted += n;
        }
        const bool final = burst.sent == burst.count;
        if (final) {
            io->write(io->context, load, CciUnlock);
            io->write(io->context, load, CciReport | CciReportIrqEnable);
            submitted += 2;
        }
        io->write(io->context, CciQueueExec + offset, submitted);
        io->write(io->context, CciQueueStart, 1u << (master * 2u));
        status = burst_wait(io, master, threshold, final, observation);
        if (status != NezhaCciOk) {
            break;
        }
        prefix = 0;
        packets = burst.half_packets;
    }
    if ((io->read(io->context, CciIrqStatus1) & threshold) != 0) {
        io->write(io->context, CciIrqClear1, threshold);
        io->write(io->context, CciIrqGlobalClear, 1);
    }
    io->write(io->context, CciIrqMask1, mask);
    if (status == NezhaCciOk) {
        if (io->read(io->context, CciQueueCount + offset) != 0 ||
            io->read(io->context, CciIrqMask1) != mask) {
            return NezhaCciQueue;
        }
        observation->needs_recovery = false;
    }
    return status;
}

bool nezha_camera_cci_standard_timing(const struct NezhaCameraRegisterIo *io,
    unsigned master, uint32_t clock_hz)
{
    /* The stock CCI standard-mode values are also in Linux i2c-qcom-cci.c. */
    static const uint32_t at_19_2mhz[] = {0x004e0072, 0x001c001c, 0x000a004d, 118, 0x161};
    static const uint32_t at_37_5mhz[] = {0x00c900ae, 0x00cc00e7, 0x001600a2, 227, 0x163};
    const uint32_t *timing = clock_hz == 19200000 ? at_19_2mhz :
        clock_hz == 37500000 ? at_37_5mhz : NULL;
    if (io == NULL || io->write == NULL || master > 1 || timing == NULL) {
        return false;
    }
    const uint32_t offset = 0x100u * master;
    io->write(io->context, CciSclControl + offset, timing[0]);
    io->write(io->context, CciSdaControl0 + offset, timing[1]);
    io->write(io->context, CciSdaControl1 + offset, timing[2]);
    io->write(io->context, CciSdaControl2 + offset, timing[3]);
    io->write(io->context, CciMiscControl + offset, timing[4]);
    return true;
}

static bool wait_ack(const struct NezhaCameraRegisterIo *io, uint32_t ack,
                     struct NezhaCameraCciObservation *observation)
{
    for (unsigned ms = 0; ms <= CciTimeoutMs; ++ms) {
        observation->irq = io->read(io->context, CciIrqStatus);
        if ((observation->irq & ack) != 0) {
            clear_status(io, ack);
            return true;
        }
        if (ms != CciTimeoutMs) {
            io->wait_ms(io->context, 1);
        }
    }
    return false;
}

enum NezhaCameraCciStatus nezha_camera_cci_recover_master(
    const struct NezhaCameraRegisterIo *io, unsigned master, uint32_t clock_hz,
    struct NezhaCameraCciObservation *observation)
{
    if (io == NULL || io->read == NULL || io->write == NULL || io->wait_ms == NULL ||
        master > 1 || (clock_hz != 19200000 && clock_hz != 37500000) || observation == NULL) {
        return NezhaCciInvalid;
    }
    *observation = (struct NezhaCameraCciObservation){
        .phase = NezhaCciHalt, .needs_recovery = true,
    };
    /* Master halt and reset masks are the same as Linux i2c-qcom-cci.c. */
    const uint32_t halt_ack = 1u << (25u + master);
    clear_status(io, io->read(io->context, CciIrqStatus) & halt_ack);
    if ((io->read(io->context, CciIrqStatus) & halt_ack) != 0) {
        return NezhaCciBusy;
    }
    io->write(io->context, CciHaltRequest, 1u << master);
    if (!wait_ack(io, halt_ack, observation)) {
        return NezhaCciTimeout;
    }

    observation->phase = NezhaCciReset;
    clear_status(io, io->read(io->context, CciIrqStatus) & CciResetAck);
    if ((io->read(io->context, CciIrqStatus) & CciResetAck) != 0) {
        return NezhaCciBusy;
    }
    io->write(io->context, CciResetCommand, master == 0 ? 0x000003f1u : 0x0003f001u);
    if (!wait_ack(io, CciResetAck, observation)) {
        return NezhaCciTimeout;
    }
    const uint32_t master_status = (0x111u << (12u * master)) |
        (master == 0 ? 0x18000ee6u : 0x60ee6000u);
    clear_status(io, io->read(io->context, CciIrqStatus) & master_status);
    if (io->read(io->context, CciQueueCount + queue_offset(master, 0)) != 0 ||
        io->read(io->context, CciQueueCount + queue_offset(master, 1)) != 0) {
        return NezhaCciQueue;
    }
    observation->fifo_words = io->read(io->context, CciReadLevel + 0x100u * master);
    if (observation->fifo_words != 0) {
        return NezhaCciFifo;
    }
    if (!nezha_camera_cci_standard_timing(io, master, clock_hz)) {
        return NezhaCciInvalid;
    }
    observation->needs_recovery = false;
    return NezhaCciOk;
}
