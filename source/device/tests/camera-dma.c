/* SPDX-License-Identifier: MIT */
#include "NezhaCameraDma.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint64_t tables[20 * 512];

static uint64_t translation(const ArmSmmu4KConfig *config,
                            const ArmSmmu4KResult *result, uint64_t iova)
{
    uint64_t table = result->root_physical;
    for (unsigned level = result->start_level; level < 4; ++level) {
        assert(table >= config->table_physical);
        uint64_t offset = table - config->table_physical;
        assert(offset < result->table_pages * 4096u);
        unsigned shift = 12u + 9u * (3u - level);
        uint64_t desc = tables[offset / 8u + ((iova >> shift) & 511u)];
        if ((desc & 3u) != 3u) {
            return 0;
        }
        if (level == 3) {
            /* The leaf must be non-executable and normal non-cacheable. */
            assert((desc & (UINT64_C(3) << 53)) == (UINT64_C(3) << 53));
            assert((desc & (7u << 2)) == 0);
            return (desc & UINT64_C(0x0000fffffffff000)) | (iova & 4095u);
        }
        table = desc & UINT64_C(0x0000fffffffff000);
    }
    return 0;
}

int main(void)
{
    const ArmSmmu4KConfig config = {32, 40, 0x80400000, 6000};
    struct NezhaCameraDmaBacking backing[2] = {
        {UINT64_C(0x8c1000000), 0x01000000, 0x00c00000},
        {UINT64_C(0x8c2000000), 0x02000000, 0x00c00000},
    };
    struct NezhaCameraDmaPlan plan;
    assert(nezha_camera_build_frame_tables(&config, backing, 4096, 2304,
        tables, 20, &plan) == ArmSmmu4KSuccess);
    assert(plan.tables.leaf_pages == 5760 && plan.frames[0].mapped_bytes == 11796480);
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t start = plan.frames[i].iova;
        uint64_t size = plan.frames[i].mapped_bytes;
        assert(translation(&config, &plan.tables, start) == backing[i].physical);
        assert(translation(&config, &plan.tables, start + size - 1) == backing[i].physical + size - 1);
        assert(translation(&config, &plan.tables, start + size) == 0);
        assert(translation(&config, &plan.tables, start - 1) == 0);
    }
    assert(translation(&config, &plan.tables, 0) == 0);

    memset(tables, 0x5a, sizeof(tables));
    backing[1].physical = backing[0].physical + 4096;
    assert(nezha_camera_build_frame_tables(&config, backing, 4096, 2304,
        tables, 20, &plan) == ArmSmmu4KOverlap);
    assert(plan.frames[0].mapped_bytes == 0 && tables[0] == UINT64_C(0x5a5a5a5a5a5a5a5a));

    /* Actual mode1: two 15 MiB RAW10 frames, each backed by separate
     * physical spans while their device IOVAs remain contiguous. */
    const uint64_t frame_bytes = UINT64_C(4096) * 3072 * 10 / 8;
    const struct NezhaCameraFrameMapping frames[] = {{0x1000000, frame_bytes}, {0x2000000, frame_bytes}};
    ArmSmmu4KRange spans[] = {
        {0x1000000, UINT64_C(0x8c3000000), 4096, 3, 0, 2},
        {0x1001000, UINT64_C(0x8c5000000), frame_bytes - 4096, 3, 0, 2},
        {0x2000000, UINT64_C(0x8c7000000), 8 * 1024 * 1024, 3, 0, 2},
        {0x2800000, UINT64_C(0x8c9000000), frame_bytes - 8 * 1024 * 1024, 3, 0, 2}};
    ArmSmmu4KConfig main_config = config;
    main_config.leaf_budget = 8000;
    assert(nezha_camera_build_frame_span_tables(&main_config, frames, 4096, 3072,
        spans, 4, tables, 20, &plan) == ArmSmmu4KSuccess);
    assert(plan.tables.leaf_pages == 7680);
    for (unsigned i = 0; i < 4; ++i) {
        assert(translation(&main_config, &plan.tables, spans[i].iova) == spans[i].physical);
        assert(translation(&main_config, &plan.tables, spans[i].iova + spans[i].bytes - 1) ==
               spans[i].physical + spans[i].bytes - 1);
    }
    assert(translation(&main_config, &plan.tables, frames[0].iova + frame_bytes) == 0);
    assert(translation(&main_config, &plan.tables, frames[1].iova + frame_bytes) == 0);
    memset(tables, 0x5a, sizeof(tables));
    spans[1].iova += 4096;
    assert(nezha_camera_build_frame_span_tables(&main_config, frames, 4096, 3072,
        spans, 4, tables, 20, &plan) == ArmSmmu4KBadRange);
    spans[1].iova -= 4096;
    spans[3].physical = spans[0].physical;
    assert(nezha_camera_build_frame_span_tables(&main_config, frames, 4096, 3072,
        spans, 4, tables, 20, &plan) == ArmSmmu4KOverlap);
    assert(plan.frames[0].mapped_bytes == 0 && tables[0] == UINT64_C(0x5a5a5a5a5a5a5a5a));

    puts("Camera IFE SMMU tables map contiguous or fragmented RAW10 buffers and leave guards unmapped");
}
