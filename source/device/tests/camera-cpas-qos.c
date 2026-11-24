/* SPDX-License-Identifier: MIT */
#include "NezhaCameraCpas.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct Model {
    uint32_t words[0x3000 / 4];
    unsigned reads, writes, restored_writes;
    uint32_t invalid, refuse;
};
static const uint32_t offsets[] = {0x2c30, 0x2c34, 0x2c38, 0x2c40, 0x2c48};
static const uint32_t values[] = {0x66655554, 0x66666666, 0x1e40, 0xffffff00, 0xf};

static unsigned slot(uint32_t offset)
{
    for (unsigned i = 0; i < NezhaCameraCpasRt0QosWords; ++i)
        if (offset == offsets[i]) return i;
    assert(0); return 0;
}
static uint32_t read(void *context, uint32_t offset)
{
    struct Model *m = context;
    (void)slot(offset);
    ++m->reads;
    return m->invalid == offset ? UINT32_MAX : m->words[offset / 4];
}
static void write(void *context, uint32_t offset, uint32_t value)
{
    struct Model *m = context;
    const unsigned i = slot(offset);
    assert(m->reads >= NezhaCameraCpasRt0QosWords); // Snapshot before any mutation.
    ++m->writes;
    if (value != values[i]) ++m->restored_writes;
    if (m->refuse != offset) m->words[offset / 4] = value;
}
static void initialize(struct Model *m)
{
    memset(m, 0, sizeof(*m));
    for (unsigned i = 0; i < sizeof(m->words) / sizeof(m->words[0]); ++i)
        m->words[i] = 0x11000000 + i;
}

int main(void)
{
    struct Model m;
    initialize(&m);
    const struct Model inherited = m;
    const struct NezhaCameraRegisterIo io = {&m, read, write, NULL};
    struct NezhaCameraCpas cpas = {0};
    struct NezhaCameraCpasRt0Qos q = {0};
    assert(nezha_camera_cpas_rt0_qos_start(&io, &cpas, &q) == NezhaCameraCpasBusy);
    assert(!m.reads && !m.writes);
    cpas.acquired = cpas.ready = true;
    assert(nezha_camera_cpas_rt0_qos_start(&io, &cpas, &q) == NezhaCameraCpasOk);
    assert(q.applied && q.prepared && q.touched == 31 && q.changed == 31 && m.writes == 5);
    for (unsigned i = 0; i < sizeof(m.words) / sizeof(m.words[0]); ++i) {
        bool owned = false;
        for (unsigned j = 0; j < 5; ++j) owned |= i * 4 == offsets[j];
        if (!owned) assert(m.words[i] == inherited.words[i]);
    }
    const unsigned active_reads = m.reads, active_writes = m.writes;
    assert(nezha_camera_cpas_rt0_qos_start(&io, &cpas, &q) == NezhaCameraCpasBusy);
    assert(nezha_camera_cpas_rt0_qos_restore(&io, &cpas, true, &q) == NezhaCameraCpasBusy);
    cpas.drained = true;
    assert(nezha_camera_cpas_rt0_qos_restore(&io, &cpas, false, &q) == NezhaCameraCpasBusy);
    assert(m.reads == active_reads && m.writes == active_writes);
    assert(nezha_camera_cpas_rt0_qos_restore(&io, &cpas, true, &q) == NezhaCameraCpasOk);
    assert(q.restored && !q.touched && q.changed == 31 && !q.prepared && m.restored_writes == 5);
    assert(!memcmp(m.words, inherited.words, sizeof(m.words)));

    /* Existing correct settings incur no writes or restore claim. */
    initialize(&m);
    for (unsigned i = 0; i < 5; ++i) m.words[offsets[i] / 4] = values[i];
    q = (struct NezhaCameraCpasRt0Qos){0}; cpas.drained = false;
    assert(nezha_camera_cpas_rt0_qos_start(&io, &cpas, &q) == NezhaCameraCpasOk);
    assert(!m.writes && !q.changed);
    cpas.drained = true;
    assert(nezha_camera_cpas_rt0_qos_restore(&io, &cpas, true, &q) == NezhaCameraCpasOk && !m.writes);

    /* All failed reads reject before any write. Every failed write still
     * permits exact restoration of the already changed prefix. */
    for (unsigned failed = 0; failed < 5; ++failed) {
        initialize(&m); q = (struct NezhaCameraCpasRt0Qos){0}; cpas.drained = false;
        m.invalid = offsets[failed];
        assert(nezha_camera_cpas_rt0_qos_start(&io, &cpas, &q) == NezhaCameraCpasReadback);
        assert(!m.writes && !q.prepared && !q.touched && q.failed_offset == offsets[failed]);
        initialize(&m); q = (struct NezhaCameraCpasRt0Qos){0}; m.refuse = offsets[failed];
        assert(nezha_camera_cpas_rt0_qos_start(&io, &cpas, &q) == NezhaCameraCpasReadback);
        assert(q.prepared && !q.applied && q.failed_offset == offsets[failed] && m.writes == failed + 1);
        assert(q.touched == ((1u << (failed + 1)) - 1));
        m.refuse = 0; cpas.drained = true;
        assert(nezha_camera_cpas_rt0_qos_restore(&io, &cpas, true, &q) == NezhaCameraCpasOk);
        assert(!memcmp(m.words, inherited.words, sizeof(m.words)) && q.failed_offset == offsets[failed]);
    }

    initialize(&m); q = (struct NezhaCameraCpasRt0Qos){0}; cpas.drained = false;
    assert(nezha_camera_cpas_rt0_qos_start(&io, &cpas, &q) == NezhaCameraCpasOk);
    cpas.drained = true; m.refuse = offsets[4];
    assert(nezha_camera_cpas_rt0_qos_restore(&io, &cpas, true, &q) == NezhaCameraCpasReadback);
    assert(q.prepared && q.touched == 31 && !q.restored);
    m.refuse = 0; m.words[offsets[4] / 4] = 0x33445566;
    const unsigned foreign_writes = m.writes;
    assert(nezha_camera_cpas_rt0_qos_restore(&io, &cpas, true, &q) == NezhaCameraCpasBusy);
    assert(m.writes == foreign_writes && m.words[offsets[4] / 4] == 0x33445566);
    m.words[offsets[4] / 4] = values[4];
    assert(nezha_camera_cpas_rt0_qos_restore(&io, &cpas, true, &q) == NezhaCameraCpasOk);
    assert(!memcmp(m.words, inherited.words, sizeof(m.words)));
    puts("Camera RT0 QoS: exact owned words, pre-DMA gate, partial rejection, drain/detach, failed restore and foreign changes protected");
}
