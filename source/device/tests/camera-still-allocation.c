/* SPDX-License-Identifier: MIT */
#include "NezhaCameraPreview.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned allocations, fail_at;
void *still_test_malloc(size_t bytes)
{
    if (++allocations==fail_at) return NULL;
    return malloc(bytes);
}
void still_test_free(void *allocation) { free(allocation); }

int main(int argc,char **argv)
{
    assert(argc==2);
    fail_at=(unsigned)strtoul(argv[1],NULL,10);
    assert(fail_at>=1 && fail_at<=7);
    struct NezhaCameraPreview processor;
    nezha_camera_preview_init(&processor);
    uint8_t raw[40],y[32],uv[16];
    memset(raw,255,sizeof(raw));memset(y,0xcd,sizeof(y));memset(uv,0xcd,sizeof(uv));
    const struct NezhaCameraPreviewFrame frame={8,4,10,3,64,1023};
    const struct NezhaCameraPreviewOutput output={NezhaCameraPreviewNv12Full,8,4,
        {y,uv},{8,8},{sizeof(y),sizeof(uv)}};
    assert(!nezha_camera_still_render(&processor,&frame,raw,sizeof(raw),&output));
    for (unsigned i=0;i<sizeof(y);++i) assert(y[i]==0xcd);
    for (unsigned i=0;i<sizeof(uv);++i) assert(uv[i]==0xcd);
    fail_at=0;
    assert(nezha_camera_still_render(&processor,&frame,raw,sizeof(raw),&output));
    for (unsigned i=0;i<sizeof(y);++i) assert(y[i]==255);
    for (unsigned i=0;i<sizeof(uv);++i) assert(uv[i]==128);
    printf("Still scratch allocation %s: failure leaves output untouched; next request succeeds\n",argv[1]);
    return 0;
}
