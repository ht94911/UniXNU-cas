/* SPDX-License-Identifier: MIT */
/* Local C3 utility for a completed RAW10 file, without a camera device. */
#include "NezhaCameraPreview.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t number(const char *text)
{
    char *end;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno || !*text || *end || value == 0 || value > 65536) return 0;
    return (uint32_t)value;
}

int main(int argc, char **argv)
{
    if (argc != 9) {
        fprintf(stderr, "Usage: %s RAW10 WIDTH HEIGHT STRIDE BAYER OUTPUT_WIDTH OUTPUT_HEIGHT OUTPUT.ppm\n",
            argv[0]);
        return 2;
    }
    const uint32_t width = number(argv[2]), height = number(argv[3]), stride = number(argv[4]);
    const uint32_t out_width = number(argv[6]), out_height = number(argv[7]);
    const char *patterns[] = {"RGGB", "GRBG", "GBRG", "BGGR"};
    unsigned bayer;
    for (bayer = 0; bayer < 4 && strcmp(argv[5], patterns[bayer]); ++bayer) {}
    if (!width || width > 16384 || width % 4 || height < 4 || height > 16384 || height % 2 ||
        stride < width / 4 * 5 || bayer == 4 || out_width < 2 || out_width > 8192 || out_width % 2 ||
        out_height < 2 || out_height > 8192 || out_height % 2) {
        fputs("Invalid RAW10 or output geometry\n", stderr);
        return 2;
    }
    const size_t raw_bytes = (size_t)stride * height;
    const size_t bgra_bytes = (size_t)out_width * out_height * 4;
    uint8_t *raw = malloc(raw_bytes), *bgra = malloc(bgra_bytes);
    FILE *input = NULL, *output = NULL;
    int result = 1;
    if (!raw || !bgra) goto done;
    input = fopen(argv[1], "rb");
    if (!input) { perror("RAW10 input"); goto done; }
    if (fread(raw, 1, raw_bytes, input) != raw_bytes || fgetc(input) != EOF || ferror(input)) {
        fputs("Input must contain exactly one complete RAW10 frame\n", stderr);
        goto done;
    }
    struct NezhaCameraPreview processor;
    nezha_camera_preview_init(&processor);
    const struct NezhaCameraPreviewFrame frame = {width, height, stride, bayer, 64, 1023};
    const struct NezhaCameraPreviewOutput target = {
        NezhaCameraPreviewBgra, out_width, out_height, {bgra, NULL},
        {(size_t)out_width * 4, 0}, {bgra_bytes, 0},
    };
    const clock_t start = clock();
    if (!nezha_camera_preview_render(&processor, &frame, raw, raw_bytes, &target)) goto done;
    const double cpu_ms = (double)(clock() - start) * 1000 / CLOCKS_PER_SEC;
    output = fopen(argv[8], "wbx");
    if (!output) { perror("New preview output"); goto done; }
    if (fprintf(output, "P6\n%u %u\n255\n", out_width, out_height) < 0) goto done;
    for (size_t pixel = 0; pixel < (size_t)out_width * out_height; ++pixel) {
        const uint8_t rgb[3] = {bgra[pixel * 4 + 2], bgra[pixel * 4 + 1], bgra[pixel * 4]};
        if (fwrite(rgb, 1, 3, output) != 3) goto done;
    }
    if (fclose(output)) { output = NULL; goto done; }
    output = NULL;
    printf("RAW10 %ux%u stride %u %s -> %ux%u preview; CPU %.3f ms (local, hardware unverified)\n",
        width, height, stride, argv[5], out_width, out_height, cpu_ms);
    result = 0;
done:
    if (input) fclose(input);
    if (output) fclose(output);
    free(raw);
    free(bgra);
    return result;
}
