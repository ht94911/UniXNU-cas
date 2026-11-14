/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "PeachMhiFirmware.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* Validate the two actual ELF32 images before any DMA publication. Preserve
 * signed metadata/hash segments verbatim: do not rebuild/rebase these ELFs. */
static int check_elf(PeachFirmwareRead read, void *ctx, uint64_t base, uint64_t size, uint16_t machine)
{
    uint8_t h[52], p[32];
    if (size < sizeof(h)) return PeachMhiInvalid;
    if (read(ctx, base, h, sizeof(h))) return PeachMhiIoError;
    if (le32(h) != 0x464c457f || h[4] != 1 || h[5] != 1 || h[6] != 1 ||
        le16(h + 16) != 2 || le16(h + 18) != machine || le32(h + 20) != 1 ||
        le16(h + 40) != sizeof(h) || le16(h + 42) != sizeof(p)) return PeachMhiInvalid;
    uint32_t offset = le32(h + 28);
    unsigned count = le16(h + 44);
    if (!count || count > 128 || offset < sizeof(h) ||
        offset > size || (uint64_t)count * sizeof(p) > size - offset) return PeachMhiInvalid;
    unsigned loadable = 0;
    for (unsigned i = 0; i < count; i++) {
        if (read(ctx, base + offset + i * sizeof(p), p, sizeof(p))) return PeachMhiIoError;
        uint32_t off = le32(p + 4), bytes = le32(p + 16);
        if (off > size || bytes > size - off) return PeachMhiInvalid;
        if (le32(p) == 1) {
            if (bytes > le32(p + 20)) return PeachMhiInvalid;
            if (bytes) loadable++;
        }
    }
    return loadable ? PeachMhiOk : PeachMhiInvalid;
}

static int valid_dma(const PeachMhiDma *b, size_t size)
{
    return b->bytes && size && b->capacity >= size && b->iova && !(b->iova & 15) &&
           (uint64_t)(size - 1) <= UINT64_MAX - b->iova;
}

int peach_mhi_firmware_stage(PeachMhiFirmware *f, PeachFirmwareRead read, void *ctx, uint64_t bytes)
{
    if (!f || !read || f->phase || bytes <= PEACH_MHI_SEGMENT_BYTES ||
        bytes > (uint64_t)(PEACH_MHI_MAX_SEGMENTS + 1) * PEACH_MHI_SEGMENT_BYTES)
        return PeachMhiInvalid;
    uint64_t rest = bytes - PEACH_MHI_SEGMENT_BYTES;
    unsigned count = (unsigned)((rest + PEACH_MHI_SEGMENT_BYTES - 1) / PEACH_MHI_SEGMENT_BYTES);
    if (!valid_dma(&f->sbl, PEACH_MHI_SEGMENT_BYTES) || !valid_dma(&f->vectors, count * 16))
        return PeachMhiInvalid;
    for (unsigned i = 0; i < count; i++) {
        size_t n = rest > PEACH_MHI_SEGMENT_BYTES ? PEACH_MHI_SEGMENT_BYTES : (size_t)rest;
        if (!valid_dma(&f->amss[i], n)) return PeachMhiInvalid;
        rest -= n;
    }
    int error = check_elf(read, ctx, 0, PEACH_MHI_SEGMENT_BYTES, 40); /* ARM TME */
    if (!error) error = check_elf(read, ctx, PEACH_MHI_SEGMENT_BYTES,
                                bytes - PEACH_MHI_SEGMENT_BYTES, 164); /* QDSP6 */
    if (error) return error;
    if (read(ctx, 0, f->sbl.bytes, PEACH_MHI_SEGMENT_BYTES)) return PeachMhiIoError;
    rest = bytes - PEACH_MHI_SEGMENT_BYTES;
    uint64_t off = PEACH_MHI_SEGMENT_BYTES;
    for (unsigned i = 0; i < count; i++) {
        size_t n = rest > PEACH_MHI_SEGMENT_BYTES ? PEACH_MHI_SEGMENT_BYTES : (size_t)rest;
        if (read(ctx, off, f->amss[i].bytes, n)) return PeachMhiIoError;
        put64((uint8_t *)f->vectors.bytes + 16 * i, f->amss[i].iova);
        put64((uint8_t *)f->vectors.bytes + 16 * i + 8, n);
        off += n;
        rest -= n;
    }
    f->count = count;
    f->phase = 1;
    return PeachMhiOk;
}

static int valid_io(const PeachMhiIo *io, uint32_t session)
{
    return io && io->read32 && io->write32 && io->sync && io->now_us && io->sleep_us &&
           io->bar_bytes >= 0x100 && session && session <= 0x3fffffff;
}

static int discover(PeachMhiFirmware *f, const PeachMhiIo *io)
{
    if (io->read32(io->context, 0x28, &f->bhi) || io->read32(io->context, 0x2c, &f->bhie))
        return PeachMhiIoError;
    if ((f->bhi & 3) || (f->bhie & 3) || f->bhi < 0x100 || f->bhie < 0x100 ||
        f->bhi > io->bar_bytes - 0x40 || f->bhie > io->bar_bytes - 0x48)
        return PeachMhiInvalid;
    return PeachMhiOk;
}

static int write32(const PeachMhiIo *io, uint32_t offset, uint32_t value)
{
    return io->write32(io->context, offset, value) ? PeachMhiIoError : PeachMhiOk;
}

static int wait_status(PeachMhiFirmware *f, const PeachMhiIo *io, uint32_t offset, uint32_t session)
{
    uint64_t start = io->now_us(io->context);
    for (;;) {
        if (io->read32(io->context, offset, &f->last_status)) return PeachMhiIoError;
        unsigned state = f->last_status >> 30;
        if (state == 2 && (!session || (f->last_status & 0x3fffffff) == session)) return PeachMhiOk;
        if (state == 3) return PeachMhiDeviceError;
        if (io->now_us(io->context) - start >= 10000000) return PeachMhiTimeout;
        io->sleep_us(io->context, 1000);
    }
}

int peach_mhi_download_sbl(PeachMhiFirmware *f, const PeachMhiIo *io, uint32_t session)
{
    if (!f || f->phase != 1 || !valid_io(io, session)) return PeachMhiInvalid;
    int error = discover(f, io);
    if (error) return error;
    uint32_t ee;
    if (io->read32(io->context, f->bhi + 0x28, &ee)) return PeachMhiIoError;
    if (ee != 0) return PeachMhiWrongEnvironment;
    io->sync(io->context, &f->sbl, PEACH_MHI_SEGMENT_BYTES);
    f->phase = 4; /* An attempted transfer requires containment/reset on failure. */
    if ((error = write32(io, f->bhi + 0x2c, 0)) ||
        (error = write32(io, f->bhi + 0x0c, (uint32_t)(f->sbl.iova >> 32))) ||
        (error = write32(io, f->bhi + 0x08, (uint32_t)f->sbl.iova)) ||
        (error = write32(io, f->bhi + 0x10, PEACH_MHI_SEGMENT_BYTES)) ||
        (error = write32(io, f->bhi + 0x18, session))) return error;
    error = wait_status(f, io, f->bhi + 0x2c, 0);
    if (!error) f->phase = 2;
    return error;
}

int peach_mhi_download_amss(PeachMhiFirmware *f, const PeachMhiIo *io, uint32_t session)
{
    if (!f || f->phase != 2 || !valid_io(io, session)) return PeachMhiInvalid;
    uint32_t ee;
    if (io->read32(io->context, f->bhi + 0x28, &ee)) return PeachMhiIoError;
    if (ee != 1) return PeachMhiWrongEnvironment;
    const uint8_t *vectors = f->vectors.bytes;
    for (unsigned i = 0; i < f->count; i++)
        io->sync(io->context, &f->amss[i], le32(vectors + i * 16 + 8));
    io->sync(io->context, &f->vectors, f->count * 16);
    f->phase = 4;
    int error;
    if ((error = write32(io, f->bhie + 0x30, (uint32_t)(f->vectors.iova >> 32))) ||
        (error = write32(io, f->bhie + 0x2c, (uint32_t)f->vectors.iova)) ||
        (error = write32(io, f->bhie + 0x34, f->count * 16)) ||
        (error = write32(io, f->bhie + 0x3c, session))) return error;
    error = wait_status(f, io, f->bhie + 0x44, session);
    if (!error) f->phase = 3;
    return error;
}

int peach_mhi_prepare_rddm(PeachMhiFirmware *f, const PeachMhiIo *io, PeachMhiDma dump,
                           PeachMhiDma vectors, uint32_t session)
{
    if (!f || !valid_io(io, session) || io->bar_bytes < 0x7c ||
        f->bhie > io->bar_bytes - 0x7c || !dump.capacity ||
        dump.capacity > PEACH_MHI_MAX_SEGMENTS * PEACH_MHI_SEGMENT_BYTES ||
        !valid_dma(&dump, dump.capacity)) return PeachMhiInvalid;
    unsigned count = (unsigned)((dump.capacity + PEACH_MHI_SEGMENT_BYTES - 1) / PEACH_MHI_SEGMENT_BYTES);
    if (!valid_dma(&vectors, count * 16)) return PeachMhiInvalid;
    uint32_t ee;
    if (io->read32(io->context, f->bhi + 0x28, &ee)) return PeachMhiIoError;
    if (ee != 1) return PeachMhiWrongEnvironment;
    size_t offset = 0;
    for (unsigned i = 0; i < count; i++) {
        size_t bytes = dump.capacity - offset;
        if (bytes > PEACH_MHI_SEGMENT_BYTES) bytes = PEACH_MHI_SEGMENT_BYTES;
        put64((uint8_t *)vectors.bytes + i * 16, dump.iova + offset);
        put64((uint8_t *)vectors.bytes + i * 16 + 8, bytes);
        offset += bytes;
    }
    io->sync(io->context, &dump, dump.capacity);
    io->sync(io->context, &vectors, count * 16);
    int error;
    if ((error = write32(io, f->bhie + 0x64, (uint32_t)(vectors.iova >> 32))) ||
        (error = write32(io, f->bhie + 0x60, (uint32_t)vectors.iova)) ||
        (error = write32(io, f->bhie + 0x68, count * 16)) ||
        (error = write32(io, f->bhie + 0x70, session))) return error;
    return 0;
}

int peach_mhi_read_rddm(PeachMhiFirmware *f, const PeachMhiIo *io, uint32_t session)
{
    if (!f || !valid_io(io, session) || io->bar_bytes < 0x7c ||
        f->bhie > io->bar_bytes - 0x7c) return PeachMhiInvalid;
    uint32_t ee;
    if (io->read32(io->context, f->bhi + 0x28, &ee)) return PeachMhiIoError;
    if (ee != 3) return PeachMhiWrongEnvironment;
    return wait_status(f, io, f->bhie + 0x78, session);
}
