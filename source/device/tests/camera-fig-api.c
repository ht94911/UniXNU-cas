/* SPDX-License-Identifier: MIT */
#include "NezhaFigCapture24A446.h"
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct OpaqueCMBaseObject { unsigned char header[32]; unsigned char data[16]; };
static int core, capture, device_class, stream_class;
static unsigned opens, creates;
static bool missing;

void *nezha_test_dlopen(const char *name, int flags)
{
    assert(flags != 0); ++opens;
    if (!strcmp(name,"/System/Library/Frameworks/CoreMedia.framework/CoreMedia")) return &core;
    assert(!strcmp(name,"/System/Library/PrivateFrameworks/CMCaptureDevice.framework/CMCaptureDevice"));
    return &capture;
}
static const void *device_id(void) { return &device_class; }
static const void *stream_id(void) { return &stream_class; }
static void *get_storage(NezhaFigObject o) { return o->data; }
static NezhaFigStatus make(NezhaFigAllocator a, const void *vtable, const void *id, NezhaFigObject *out)
{
    (void)a;
    const struct NezhaFigDeviceVTable *table = vtable;
    assert(!table->reserved && table->base->storage_size == 16);
    if (id == &device_class) assert(table->device->version == 3);
    else assert(id == &stream_class && ((const struct NezhaFigStreamVTable *)vtable)->stream->version == 2);
    ++creates;
    *out = calloc(1,sizeof(**out));
    return *out ? 0 : -1;
}
void *nezha_test_dlsym(void *handle, const char *name)
{
    if (!strcmp(name,"CMDerivedObjectCreate")) { assert(handle == &core); return (void *)make; }
    if (!strcmp(name,"CMBaseObjectGetDerivedStorage")) { assert(handle == &core); return (void *)get_storage; }
    assert(handle == &capture);
    if (!strcmp(name,"FigCaptureDeviceGetClassID")) return (void *)device_id;
    assert(!strcmp(name,"FigCaptureStreamGetClassID"));
    return missing ? NULL : (void *)stream_id;
}
int main(int argc, char **argv)
{
    (void)argv; missing = argc > 1;
    const struct NezhaFigBaseClass base = {.version=1,.storage_size=16};
    const struct NezhaFigDeviceClass device = {.version=3};
    const struct NezhaFigStreamClass stream = {.version=2};
    const struct NezhaFigDeviceVTable dvt = {.base=&base,.device=&device};
    const struct NezhaFigStreamVTable svt = {.base=&base,.stream=&stream};
    NezhaFigObject d = NULL, s = NULL;
    if (missing) {
        assert(nezha_fig_capture24_create_device(NULL,&dvt,&d) == -12782 && !d);
        assert(nezha_fig_capture24_create_stream(NULL,&svt,&s) == -12782 && !s);
        assert(opens == 2 && !creates);
        puts("camera Fig factory: a missing actual ClassID export leaves both objects unavailable");
        return 0;
    }
    assert(nezha_fig_capture24_create_device(NULL,&dvt,&d) == 0 && d);
    assert(nezha_fig_capture24_create_stream(NULL,&svt,&s) == 0 && s);
    assert(opens == 2 && creates == 2);
    unsigned char *data = nezha_fig_capture24_storage(d);
    assert(data == d->data && !data[0]); data[0] = 0x51;
    assert(nezha_fig_capture24_storage(d) == data && data[0] == 0x51);
    assert(nezha_fig_capture24_storage(NULL) == NULL);
    struct NezhaFigDeviceClass wrong = {.version=2};
    const struct NezhaFigDeviceVTable bad = {.base=&base,.device=&wrong};
    NezhaFigObject rejected = d;
    assert(nezha_fig_capture24_create_device(NULL,&bad,&rejected) == -12780 && !rejected);
    assert(creates == 2 && opens == 2);
    free(s); free(d);
    puts("camera Fig factory: real framework paths, separate device/stream ClassIDs, zeroed derived storage and typed versions passed");
    return 0;
}
