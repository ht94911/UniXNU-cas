/* SPDX-License-Identifier: MIT */
#include "NezhaCameraOvx10500u.h"

#include <assert.h>
#include <stdio.h>

enum { QueueExec = 0x500, QueueCount = 0x504, QueueLoad = 0x510, QueueStart = 8,
       IrqGlobal = 0xc00, Irq0Clear = 0xc08, Irq0Status = 0xc0c, Irq1Mask = 0xc10,
       Irq1Clear = 0xc14, Irq1Status = 0xc18, Threshold = 1 << 22, Report = 1 << 16 };

struct Model {
    uint32_t wire[300];
    unsigned length, queued, staged, exec, waits, starts, queue_words;
    uint32_t irq0, irq1, mask1;
    bool timeout;
};

static uint32_t read_reg(void *cookie, uint32_t offset)
{
    struct Model *m = cookie;
    switch (offset) {
    case QueueCount: return m->queued;
    case Irq0Status: return m->irq0;
    case Irq1Status: return m->irq1;
    case Irq1Mask: return m->mask1;
    case 0x604: case 0x21c: return 0;
    default: assert(0); return 0;
    }
}

static void write_reg(void *cookie, uint32_t offset, uint32_t value)
{
    struct Model *m = cookie;
    switch (offset) {
    case Irq1Mask:
        assert((value & ~Threshold) == (1u << 18));
        m->mask1 = value;
        break;
    case Irq0Clear:
        assert(value == Report);
        m->irq0 &= ~value;
        break;
    case Irq1Clear:
        assert(value == Threshold);
        m->irq1 &= ~value;
        break;
    case IrqGlobal:
        assert(value == 1);
        break;
    case QueueLoad:
        assert(m->length < sizeof(m->wire) / sizeof(m->wire[0]) && m->queued < m->queue_words);
        m->wire[m->length++] = value;
        ++m->queued;
        ++m->staged;
        break;
    case QueueExec:
        assert(value == m->staged);
        m->exec = value;
        break;
    case QueueStart:
        assert(value == 4 && m->exec == m->staged && m->queued != 0);
        ++m->starts;
        m->staged = 0;
        if (m->timeout) {
            break;
        }
        if (m->wire[m->length - 1] == 0x108 && m->wire[m->length - 2] == 7) {
            m->queued = 0;
            m->irq0 |= Report;
        } else {
            const unsigned half_words = (m->queue_words - 4u) / 6u * 3u;
            assert(m->queued >= half_words);
            m->queued = half_words;
            m->irq1 |= Threshold;
        }
        break;
    default: assert(0);
    }
}

static void wait_ms(void *cookie, unsigned ms)
{
    struct Model *m = cookie;
    m->waits += ms;
}

static uint8_t word_byte(const uint32_t *word, unsigned position)
{
    return (uint8_t)(word[position / 4] >> (8u * (position % 4)));
}

int main(void)
{
    const struct Model inherited = {.irq0 = 1u << 4, .irq1 = 1u << 18, .mask1 = 1u << 18, .queue_words = 64};
    struct Model m = inherited;
    const struct NezhaCameraRegisterIo io = {&m, read_reg, write_reg, wait_ms};
    uint8_t bytes[929];
    for (unsigned i = 0; i < sizeof(bytes); ++i) {
        bytes[i] = (uint8_t)(i * 37u + 11u);
    }
    const struct NezhaCameraOvxModeWrite mode = {.reg = 0xffff, .burst = bytes, .burst_bytes = sizeof(bytes)};
    struct NezhaCameraCciObservation observed;
    size_t applied;
    assert(nezha_camera_ovx10500u_apply_mode(&io, &mode, 1, 64, &applied, &observed) == NezhaCciOk);
    assert(applied == 1 && !observed.needs_recovery && m.starts > 2 && m.queued == 0);
    assert(m.mask1 == inherited.mask1 && m.irq0 == inherited.irq0 && m.irq1 == inherited.irq1);
    assert(m.wire[0] == 0x101 && m.wire[1] == 6);
    unsigned cursor = 2, sent = 0, packets = 0;
    while (sent < sizeof(bytes)) {
        const uint32_t *packet = &m.wire[cursor];
        const uint8_t op = word_byte(packet, 0);
        assert((op & 0x0f) == 9);
        unsigned n = op >> 4;
        ++packets;
        if (n == 14 || n == 15) {
            assert((n == 14) == (packets % 10 == 0));
            n = 11;
        } else {
            assert(sent + n == sizeof(bytes));
        }
        unsigned j = 1;
        if (sent == 0) {
            assert(word_byte(packet, 1) == 0xff && word_byte(packet, 2) == 0xff);
            j = 3;
        }
        for (; j <= n; ++j) {
            assert(sent < sizeof(bytes) && word_byte(packet, j) == bytes[sent++]);
        }
        cursor += (n + 4u) / 4u;
    }
    assert(cursor + 2 == m.length && m.wire[cursor] == 7 && m.wire[cursor + 1] == 0x108);

    const unsigned sizes[] = {16, 32, 128, NezhaCameraCciCanoeQueue0Words};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        m = inherited;
        m.queue_words = sizes[i];
        assert(nezha_camera_ovx10500u_apply_mode(&io, &mode, 1, sizes[i], &applied, &observed) == NezhaCciOk);
        assert(applied == 1 && m.queued == 0 && !observed.needs_recovery && m.mask1 == inherited.mask1);
        if (sizes[i] == NezhaCameraCciCanoeQueue0Words) {
            assert(m.starts == 1);
        }
    }

    m = inherited;
    m.timeout = true;
    assert(nezha_camera_ovx10500u_apply_mode(&io, &mode, 1, 64, &applied, &observed) == NezhaCciTimeout);
    assert(applied == 0 && observed.needs_recovery && m.starts == 1 && m.waits == 100);
    assert(m.mask1 == inherited.mask1 && m.irq1 == inherited.irq1);

    puts("CCI keeps the 929-byte register group continuous and restores the owned threshold mask");
}
