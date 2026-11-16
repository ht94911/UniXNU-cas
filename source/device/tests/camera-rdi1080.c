/* SPDX-License-Identifier: MIT */
#include "NezhaCameraRdi1080.h"

#include <assert.h>
#include <stdio.h>

struct Writes {
    uint32_t offsets[8];
    uint32_t values[8];
    unsigned count;
};

static void capture_write(void *context, uint32_t offset, uint32_t value)
{
    struct Writes *writes = context;
    assert(writes->count < 8);
    writes->offsets[writes->count] = offset;
    writes->values[writes->count++] = value;
}

int main(void)
{
    struct NezhaCameraRdi1080 program;
    struct NezhaCameraFrameMapping buffers[2] = {
        {0x20000000, 0x01000000}, {0x21000000, 0x01000000},
    };

    /* Qualcomm v24's tested S5KJN5 RAW10 mode. */
    assert(nezha_camera_rdi1080_layout(4096, 3072, buffers, &program));
    assert(program.layout.stride == 5120);
    assert(program.layout.frame_bytes == 15728640);
    assert(program.frame_increment == 61440);
    assert(program.image_address[0] == 0x200000 && program.image_address[1] == 0x210000);

    /* The width and height present in the public main sensor trace. */
    assert(nezha_camera_rdi1080_layout(4096, 2304, buffers, &program));
    assert(program.layout.frame_bytes == 11796480 && program.frame_increment == 46080);
    struct Writes writes = {0};
    nezha_camera_rdi1080_program_geometry(&program, &writes, capture_write);
    assert(writes.count == 8);
    assert(writes.offsets[0] == 0x2c08 && writes.values[0] == 46080);
    assert(writes.offsets[1] == 0x2c0c && writes.values[1] == 0xffff);
    for (unsigned i = 0; i < writes.count; ++i) {
        assert(writes.offsets[i] != 0x2c00 && writes.offsets[i] != 0x2c04);
    }

    buffers[1].iova = 0x20ff0000;
    assert(!nezha_camera_rdi1080_layout(4096, 2304, buffers, &program));
    buffers[1].iova = 0x21000001;
    assert(!nezha_camera_rdi1080_layout(4096, 2304, buffers, &program));
    buffers[1].iova = 0x21000000;
    buffers[1].mapped_bytes = 11796479;
    assert(!nezha_camera_rdi1080_layout(4096, 2304, buffers, &program));
    buffers[1].mapped_bytes = 0x01000000;
    buffers[1].iova = 0xffff0000;
    assert(!nezha_camera_rdi1080_layout(4096, 2304, buffers, &program));
    buffers[1].iova = 0x21000000;
    assert(!nezha_camera_rdi1080_layout(4095, 2304, buffers, &program));
    assert(!nezha_camera_rdi1080_layout(4096, UINT32_MAX, buffers, &program));
    assert(!nezha_camera_rdi1080_layout(4, 1, buffers, &program));

    puts("Full VFE1080 RDI0 passed RAW10 geometry and DMA mapping checks");
}
