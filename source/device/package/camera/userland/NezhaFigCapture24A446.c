/* SPDX-License-Identifier: MIT */
#include "NezhaFigCapture24A446.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>

/* CoreMedia UUID a1e3885b22be3c7695c17821276212f6 and CMCaptureDevice
 * UUID 909cdfeb39563ad68f6f462d31023c89, from the selected 24A446 cache.
 * CMDerivedObjectCreate / GetDerivedStorage signatures were checked in
 * that original code; ClassID access goes through the library exports.
 * The 16.5 HakiOS factory is the reference for the construction sequence. */
static pthread_once_t once = PTHREAD_ONCE_INIT;
static NezhaFigStatus (*create)(NezhaFigAllocator, const void *, const void *, NezhaFigObject *);
static void *(*storage)(NezhaFigObject);
static const void *(*device_id)(void), *(*stream_id)(void);
enum { Invalid = -12780, Unavailable = -12782 };

static void bind(void)
{
    /* Retain the framework handles for the lifetime of their objects. */
    void *core = dlopen("/System/Library/Frameworks/CoreMedia.framework/CoreMedia", RTLD_NOW | RTLD_LOCAL);
    void *capture = dlopen("/System/Library/PrivateFrameworks/CMCaptureDevice.framework/CMCaptureDevice",
        RTLD_NOW | RTLD_LOCAL);
    if (!core || !capture) return;
    create = (NezhaFigStatus (*)(NezhaFigAllocator, const void *, const void *, NezhaFigObject *))
        dlsym(core, "CMDerivedObjectCreate");
    storage = (void *(*)(NezhaFigObject))dlsym(core, "CMBaseObjectGetDerivedStorage");
    device_id = (const void *(*)(void))dlsym(capture, "FigCaptureDeviceGetClassID");
    stream_id = (const void *(*)(void))dlsym(capture, "FigCaptureStreamGetClassID");
}

static bool available(void)
{
    return pthread_once(&once, bind) == 0 && create && storage && device_id && stream_id;
}

NezhaFigStatus nezha_fig_capture24_create_device(NezhaFigAllocator allocator,
    const struct NezhaFigDeviceVTable *table, NezhaFigObject *out)
{
    if (!out) return Invalid;
    *out = NULL;
    if (!table || table->reserved || !table->base || !table->device ||
        table->base->version != 1 || !table->base->storage_size ||
        table->device->version != 3) return Invalid;
    if (!available()) return Unavailable;
    return create(allocator, table, device_id(), out);
}

NezhaFigStatus nezha_fig_capture24_create_stream(NezhaFigAllocator allocator,
    const struct NezhaFigStreamVTable *table, NezhaFigObject *out)
{
    if (!out) return Invalid;
    *out = NULL;
    if (!table || table->reserved || !table->base || !table->stream ||
        table->base->version != 1 || !table->base->storage_size ||
        table->stream->version != 2) return Invalid;
    if (!available()) return Unavailable;
    return create(allocator, table, stream_id(), out);
}

void *nezha_fig_capture24_storage(NezhaFigObject object)
{
    return object && available() ? storage(object) : NULL;
}
