/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Derived from audio27 platform/AdspFirmware.c; see NbPas.h for the changes. */
#include "NbPas.h"

static void zero(uint8_t *p, uint64_t n) { for (uint64_t i = 0; i < n; ++i) p[i] = 0; }

static int failed(NbPas *l, int r)
{
    l->stage = NbPasFault;
    l->error = r < 0 ? r : NB_IO;
    return l->error;
}

int nb_pas_begin(NbPas *l, uint32_t pas, NbRange region, void *mapped, uint64_t file_size, QcomMdtRead read, void *read_context,
                 void *metadata, uint64_t metadata_pa, uint64_t capacity, const NbPasOps *ops, uint64_t now, uint64_t timeout)
{
    if (!l || !mapped || !read || !metadata || !metadata_pa || (metadata_pa & 4095) || ((uintptr_t)metadata & 4095) ||
        !capacity || (capacity & 4095) || capacity > 0x100000 || !timeout || timeout > 600000 || now > UINT64_MAX - timeout ||
        !ops || !ops->pas || !ops->sync || !nb_range_valid(region) || (region.base & 4095) ||
        nb_ranges_overlap((NbRange){metadata_pa, capacity}, region)) return NB_ARGUMENT;
    /* The metadata the secure world reads must lie below 4 GiB: canoe_mpss_resource sets dma_phys_below_32b (the phone's
     * qcom_q6v5_pas), and the SM8850 Pro Max port keeps its modem metadata in low RAM for the same reason. The same rule
     * is kept for every image here; the board's buffer comes from low memory. */
    if (metadata_pa >= 0x100000000ull || capacity > 0x100000000ull - metadata_pa) return NB_ARGUMENT;
    *l = (NbPas){0};
    if (qcom_mdt_parse(&l->image, file_size, read, read_context)) return NB_WIRE;
    const QcomMdtImage *im = &l->image;
    /* Fixed images must lie inside the carveout by address; relocatable ones by span. */
    const uint64_t span = qcom_mdt_span(im);
    if (!span || span > region.bytes) return NB_OWNERSHIP;
    if (!im->relocatable && !nb_range_inside((NbRange){im->min_address, span}, region)) return NB_OWNERSHIP;
    for (unsigned i = 0; i < im->count; ++i) {
        if (!im->segment[i].load) continue;
        uint64_t at;
        if (qcom_mdt_place(im, i, region.base, region.bytes, &at)) return NB_OWNERSHIP;
        if (im->segment[i].file_size > im->segment[i].memory_size) return NB_WIRE;
        for (unsigned j = 0; j < i; ++j) {
            if (!im->segment[j].load) continue;
            uint64_t other;
            qcom_mdt_place(im, j, region.base, region.bytes, &other);
            if (nb_ranges_overlap((NbRange){at, im->segment[i].memory_size}, (NbRange){other, im->segment[j].memory_size}))
                return NB_WIRE;
        }
    }
    if (qcom_mdt_metadata_size(im) > capacity) return NB_OWNERSHIP;
    zero(metadata, capacity);
    if (qcom_mdt_metadata(im, read, read_context, metadata, capacity)) return NB_WIRE;
    const int r = ops->sync(ops->context, metadata, metadata_pa, capacity);
    if (r) return r < 0 ? r : NB_IO;
    l->read = read; l->read_context = read_context; l->ops = *ops; l->region = region; l->mapped = mapped;
    l->metadata = metadata; l->metadata_pa = metadata_pa; l->metadata_capacity = capacity; l->span = span; l->pas = pas;
    l->stage = NbPasInit; l->timeout = timeout; l->deadline = now + timeout;
    return NB_OK;
}

int nb_pas_poll(NbPas *l, uint64_t now)
{
    if (!l || l->stage == NbPasIdle) return NB_ARGUMENT;
    if (l->stage == NbPasFault) return l->error;
    if (l->stage == NbPasReady) return NB_OK;
    if (now >= l->deadline) return failed(l, NB_TIMEOUT);
    const QcomMdtImage *im = &l->image;
    int r = NB_OK;
    switch (l->stage) {
    case NbPasInit:
        r = l->ops.pas(l->ops.context, NB_PAS_INIT_IMAGE, l->pas, l->metadata_pa, 0);
        if (r == NB_BUSY) return NB_PENDING;
        l->secure_touched = 1;
        if (!r) l->stage = im->relocatable ? NbPasMemory : NbPasCopy;
        break;
    case NbPasMemory:
        r = l->ops.pas(l->ops.context, NB_PAS_MEM_SETUP, l->pas, l->region.base, l->span);
        if (r == NB_BUSY) return NB_PENDING;
        if (!r) l->stage = NbPasCopy;
        break;
    case NbPasCopy: {
        while (l->segment < im->count && !im->segment[l->segment].load) ++l->segment;
        if (l->segment == im->count) { l->stage = NbPasAuthenticate; return NB_PENDING; }
        const QcomMdtSegment *p = &im->segment[l->segment];
        uint64_t at;
        if (qcom_mdt_place(im, l->segment, l->region.base, l->region.bytes, &at)) return failed(l, NB_WIRE);
        uint64_t bytes = p->memory_size - l->offset;
        if (bytes > NB_PAS_STEP_BYTES) bytes = NB_PAS_STEP_BYTES;
        uint64_t file = l->offset < p->file_size ? p->file_size - l->offset : 0;
        if (file > bytes) file = bytes;
        uint8_t *to = l->mapped + at + l->offset;
        if (file && l->read(l->read_context, p->file_offset + l->offset, to, (size_t)file)) return failed(l, NB_IO);
        if (bytes > file) zero(to + file, bytes - file);
        r = l->ops.sync(l->ops.context, to, l->region.base + at + l->offset, bytes);
        if (!r) {
            l->offset += bytes;
            if (l->offset == p->memory_size) { ++l->segment; l->offset = 0; }
        }
        break;
    }
    case NbPasAuthenticate:
        /* The SM8850 hypervisor sets up the PIL metadata bridge itself (audio27 finding); no extra assign here. */
        r = l->ops.pas(l->ops.context, NB_PAS_AUTH_AND_RESET, l->pas, 0, 0);
        if (r == NB_BUSY) return NB_PENDING;
        if (!r) l->stage = NbPasReady;
        break;
    default:
        return failed(l, NB_OWNERSHIP);
    }
    if (r) return failed(l, r);
    return l->stage == NbPasReady ? NB_OK : NB_PENDING;
}

void nb_pas_arm(NbPas *l, uint64_t now)
{
    if (l && l->stage == NbPasInit && now <= UINT64_MAX - l->timeout) l->deadline = now + l->timeout;
}
