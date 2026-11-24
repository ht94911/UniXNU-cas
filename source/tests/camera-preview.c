/* SPDX-License-Identifier: MIT */
#include "NezhaCameraPreview.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

static void white_balance(void)
{
    uint8_t raw[52], before[40], after[40];
    struct NezhaCameraPreviewFrame frame={8,4,13,3,64,1023};
    struct NezhaCameraPreviewOutput output={NezhaCameraPreviewBgra,4,2,
        {before,NULL},{20,0},{sizeof(before),0}};
    for (unsigned bayer=0;bayer<4;++bayer) {
        struct NezhaCameraPreview preview;
        nezha_camera_preview_init(&preview);
        frame.bayer=bayer;
        raw_color(raw,bayer,320,192,128);
        assert(nezha_camera_preview_render(&preview,&frame,raw,sizeof(raw),&output));
        const float red=preview.processing.wb[0],blue=preview.processing.wb[2];
        assert(!nezha_camera_preview_update_white_balance(&preview,&frame,raw,48));
        assert(preview.processing.wb[0]==red && preview.processing.wb[2]==blue);
        assert(nezha_camera_preview_update_white_balance(&preview,&frame,raw,sizeof(raw)));
        assert(preview.processing.wb[0]>0.94f && preview.processing.wb[0]<0.96f);
        assert(preview.processing.wb[2]>1.09f && preview.processing.wb[2]<1.11f);
        for (unsigned sample=1;sample<45;++sample)
            assert(nezha_camera_preview_update_white_balance(&preview,&frame,raw,sizeof(raw)));
        assert(fabsf(preview.processing.wb[0]-0.5f)<0.01f &&
            fabsf(preview.processing.wb[2]-2.0f)<0.01f && preview.processing.wb[1]==1);
        output.plane[0]=after;
        assert(nezha_camera_preview_render(&preview,&frame,raw,sizeof(raw),&output));
        assert(before[2]>before[1]+15 && before[1]>before[0]+15);
        assert(abs((int)after[0]-(int)after[1])<=2 && abs((int)after[2]-(int)after[1])<=2);
        const float settled_red=preview.processing.wb[0],settled_blue=preview.processing.wb[2];
        raw_color(raw,bayer,64,64,64);
        assert(nezha_camera_preview_update_white_balance(&preview,&frame,raw,sizeof(raw)));
        assert(preview.processing.wb[0]==settled_red && preview.processing.wb[2]==settled_blue);
        nezha_camera_preview_init(&preview);
        assert(preview.processing.wb[0]==1 && preview.processing.wb[2]==1);
        output.plane[0]=before;
    }
}

static void still_detail(struct NezhaCameraPreview *processor)
{
    uint8_t raw[208], preview_y[176], still_y[176], preview_uv[88], still_uv[88];
    struct NezhaCameraPreviewFrame frame={16,8,26,3,64,1023};
    memset(raw,0xcd,sizeof(raw));
    for (unsigned y=0;y<8;++y) for (unsigned x=0;x<16;x+=4) {
        unsigned values[4];
        for (unsigned i=0;i<4;++i) {
            /* A neutral sensor-site gradient must survive the photograph;
             * the preview's cell reduction gives different fine detail. */
            const unsigned at=x+i;
            values[i]=64+20*at+8*y;
        }
        pack(raw+y*26+x/4*5,values);
    }
    struct NezhaCameraPreviewOutput output={NezhaCameraPreviewNv12Full,16,8,
        {preview_y,preview_uv},{22,22},{sizeof(preview_y),sizeof(preview_uv)}};
    memset(preview_y,0xcd,sizeof(preview_y));memset(preview_uv,0xcd,sizeof(preview_uv));
    memset(still_y,0xcd,sizeof(still_y));memset(still_uv,0xcd,sizeof(still_uv));
    assert(nezha_camera_preview_render(processor,&frame,raw,sizeof(raw),&output));
    output.plane[0]=still_y;output.plane[1]=still_uv;
    assert(nezha_camera_still_render(processor,&frame,raw,sizeof(raw),&output));
    unsigned differences=0;
    for (unsigned y=0;y<8;++y) {
        for (unsigned x=0;x<16;++x) differences+=preview_y[y*22+x]!=still_y[y*22+x];
        for (unsigned x=16;x<22;++x) assert(still_y[y*22+x]==0xcd);
    }
    for (unsigned y=0;y<4;++y) for (unsigned x=16;x<22;++x) assert(still_uv[y*22+x]==0xcd);
    assert(differences>32);
    memset(still_y,0xcd,sizeof(still_y));
    assert(!nezha_camera_still_render(processor,&frame,raw,190,&output) && still_y[0]==0xcd);
}

static void auto_exposure(void)
{
    struct NezhaCameraPreview preview;
    nezha_camera_preview_init(&preview);
    struct hakios_camera_frame actual={.sequence=1,.time=77,.exposure_lines=200,.line_ns=10000,
        .gain=256,.gain_unity=256,.gain_max=65280,.frame_lines=3200,.frame_lines_min=3200,
        .frame_lines_max=6400,.margin=10};
    struct isp_stats statistics={.cells=16,.mean={5,5,5},.bright=8};
    struct hakios_camera_command command={0};
    struct isp before=preview.processing;
    actual.line_ns=0;
    assert(!nezha_camera_preview_auto_exposure(&preview,&actual,&statistics,&command));
    assert(!memcmp(&before,&preview.processing,sizeof(before)));
    actual.line_ns=10000;
    assert(nezha_camera_preview_auto_exposure(&preview,&actual,&statistics,&command));
    assert(command.command==HAKIOS_CAMERA_EXPOSURE && !command.bytes && !command.d &&
        command.a>200 && command.c>=3200 && command.c>=command.a+10 && command.b>=256);
    assert(!nezha_camera_preview_auto_exposure(&preview,&actual,&statistics,&command));
    actual.exposure_lines=command.a;actual.gain=command.b;actual.frame_lines=command.c;
    actual.sequence++;
    assert(nezha_camera_preview_auto_exposure(&preview,&actual,&statistics,&command));
    actual.exposure_lines=command.a;actual.gain=command.b;actual.frame_lines=command.c;
    actual.sequence++;
    statistics.mean[1]=800;statistics.bright=950;
    const unsigned previous=actual.exposure_lines;
    assert(nezha_camera_preview_auto_exposure(&preview,&actual,&statistics,&command) && command.a<previous);
    nezha_camera_preview_init(&preview);
    assert(!preview.processing.asked);
    actual.gain_unity=0;
    assert(!nezha_camera_preview_auto_exposure(&preview,&actual,&statistics,&command));
    puts("Camera AE: real control metadata required, pending request hold, quantized gain and bright/dark responses passed");
}

static void auto_exposure_limits(void)
{
    struct NezhaCameraPreview preview;
    nezha_camera_preview_init(&preview);
    struct hakios_camera_frame actual={.sequence=1523,.time=77,.exposure_lines=4482,
        .line_ns=14841,.gain=65280,.gain_unity=256,.gain_max=65280,
        .frame_lines=4492,.frame_lines_min=2246,.frame_lines_max=12111192,.margin=10};
    struct isp_stats statistics={.cells=196608,.mean={4.3,5.96,4.88},.bright=24};
    struct hakios_camera_command command={0};
    preview.processing.digital=4; /* the actual146 settled software value */
    /* Real146's saturated dark stream must continue rendering without
     * issuing the same group hundreds of times. Software gain is still
     * allowed to respond when the hardware values cannot move further. */
    for (unsigned frame=0;frame<600;++frame) {
        ++actual.sequence;++actual.time;
        assert(!nezha_camera_preview_auto_exposure(&preview,&actual,&statistics,&command));
        assert(!preview.processing.asked && preview.processing.digital==4);
        assert(!command.command&&!command.bytes&&!command.a&&!command.b&&!command.c&&!command.d);
    }
    statistics.mean[1]=200;statistics.bright=220;
    assert(!nezha_camera_preview_auto_exposure(&preview,&actual,&statistics,&command));
    assert(preview.processing.digital>1 && preview.processing.digital<4 && !preview.processing.asked);
    statistics.mean[1]=800;statistics.bright=950;
    assert(nezha_camera_preview_auto_exposure(&preview,&actual,&statistics,&command));
    assert(command.a<=actual.exposure_lines && command.b<actual.gain);
    actual.exposure_lines=command.a;actual.gain=command.b;
    /* A matching exposure/gain with an old frame length is not a fully
     * applied request. The native two-frame/readback gate must finish. */
    actual.frame_lines=command.c+1;
    const struct isp waiting=preview.processing;
    assert(!nezha_camera_preview_auto_exposure(&preview,&actual,&statistics,&command));
    assert(!memcmp(&waiting,&preview.processing,sizeof waiting));
    nezha_camera_preview_init(&preview);
    actual.exposure_lines=4;actual.gain=256;actual.frame_lines=2246;
    for (unsigned frame=0;frame<60;++frame) {
        ++actual.sequence;
        assert(!nezha_camera_preview_auto_exposure(&preview,&actual,&statistics,&command));
        assert(!preview.processing.asked);
    }
    statistics.mean[1]=5;statistics.bright=8;
    assert(nezha_camera_preview_auto_exposure(&preview,&actual,&statistics,&command));
    assert(command.a>4 && command.c>=2246);
    puts("Camera AE limits: 600 saturated frames without repeated CCI requests, digital-only correction, full readback hold and bright/dark recovery passed");
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
    still_detail(&processor);
    white_balance();
    auto_exposure();
    auto_exposure_limits();
    puts("camera preview: four CFA layouts, padded RAW/BGRA/NV12, full/video levels and bounds passed");
    return 0;
}
