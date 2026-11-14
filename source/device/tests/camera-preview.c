/* SPDX-License-Identifier: MIT */
#include "NezhaCameraPreview.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void pack(uint8_t *out, const unsigned pixels[4])
{
    out[4] = 0;
    for (unsigned i = 0; i < 4; ++i) {
        out[i] = (uint8_t)(pixels[i] >> 2);
        out[4] |= (uint8_t)((pixels[i] & 3u) << (i * 2));
    }
}

static void raw_color(uint8_t raw[52], unsigned bayer, unsigned red, unsigned green, unsigned blue)
{
    const unsigned red_at[] = {0, 1, 2, 3}, blue_at[] = {3, 2, 1, 0};
    memset(raw, 0xcd, 52);
    for (unsigned y = 0; y < 4; ++y) {
        for (unsigned x = 0; x < 8; x += 4) {
            unsigned values[4];
            for (unsigned j = 0; j < 4; ++j) {
                unsigned at = (y % 2) * 2 + j % 2;
                values[j] = at == red_at[bayer] ? red : at == blue_at[bayer] ? blue : green;
            }
            pack(raw + y * 13 + x / 4 * 5, values);
        }
    }
}

static void near(unsigned actual, unsigned wanted)
{
    assert(actual + 1 >= wanted && actual <= wanted + 1);
}

int main(void)
{
    struct NezhaCameraPreview processor;
    nezha_camera_preview_init(&processor);
    uint8_t raw[52], bgra[40], y[12], uv[6];
    struct NezhaCameraPreviewFrame frame = {8, 4, 13, 3, 64, 1023};
    struct NezhaCameraPreviewOutput output = {
        NezhaCameraPreviewBgra, 4, 2, {bgra, NULL}, {20, 0}, {sizeof(bgra), 0},
    };
    for (unsigned bayer = 0; bayer < 4; ++bayer) {
        frame.bayer = bayer;
        raw_color(raw, bayer, 1023, 64, 64);
        memset(bgra, 0xcd, sizeof(bgra));
        assert(nezha_camera_preview_render(&processor, &frame, raw, sizeof(raw), &output));
        for (unsigned row = 0; row < 2; ++row) {
            for (unsigned x = 0; x < 4; ++x) {
                const uint8_t *pixel = bgra + row * 20 + x * 4;
                assert(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 255 && pixel[3] == 255);
            }
            for (unsigned i = 16; i < 20; ++i) assert(bgra[row * 20 + i] == 0xcd);
        }
    }
    frame.bayer = 3;
    output = (struct NezhaCameraPreviewOutput){
        NezhaCameraPreviewNv12Full, 4, 2, {y, uv}, {6, 6}, {sizeof(y), sizeof(uv)},
    };
    memset(y, 0xcd, sizeof(y));
    memset(uv, 0xcd, sizeof(uv));
    assert(nezha_camera_preview_render(&processor, &frame, raw, sizeof(raw), &output));
    near(y[0], 76); near(uv[0], 85); near(uv[1], 255);
    assert(y[4] == 0xcd && y[5] == 0xcd && y[10] == 0xcd && uv[4] == 0xcd);
    output.format = NezhaCameraPreviewNv12Video;
    assert(nezha_camera_preview_render(&processor, &frame, raw, sizeof(raw), &output));
    near(y[0], 82); near(uv[0], 90); near(uv[1], 240);
    raw_color(raw, 3, 64, 64, 64);
    assert(nezha_camera_preview_render(&processor, &frame, raw, sizeof(raw), &output));
    assert(y[0] == 16 && uv[0] == 128 && uv[1] == 128);
    raw_color(raw, 3, 1023, 1023, 1023);
    assert(nezha_camera_preview_render(&processor, &frame, raw, sizeof(raw), &output));
    assert(y[0] == 235 && uv[0] == 128 && uv[1] == 128);

    /* Reject partial RAW and undersized NV12 planes before writing. */
    memset(y, 0xcd, sizeof(y));
    assert(!nezha_camera_preview_render(&processor, &frame, raw, 48, &output));
    assert(y[0] == 0xcd);
    output.capacity[1] = 3;
    assert(!nezha_camera_preview_render(&processor, &frame, raw, sizeof(raw), &output));
    output.capacity[1] = sizeof(uv);
    output.width = 3;
    assert(!nezha_camera_preview_render(&processor, &frame, raw, sizeof(raw), &output));
    output.width = 4;
    frame.black = 128;
    assert(!nezha_camera_preview_render(&processor, &frame, raw, sizeof(raw), &output));
    frame.black = 64;
    frame.bayer = 4;
    assert(!nezha_camera_preview_render(&processor, &frame, raw, sizeof(raw), &output));
    frame.bayer = 3;
    frame.stride = 9;
    assert(!nezha_camera_preview_render(&processor, &frame, raw, sizeof(raw), &output));
    puts("camera preview: four CFA layouts, padded RAW/BGRA/NV12, full/video levels and bounds passed");
    return 0;
}
