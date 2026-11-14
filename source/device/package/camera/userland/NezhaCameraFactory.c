/* SPDX-License-Identifier: MIT */
#include "NezhaCameraFactory.h"
#include "NezhaCameraFormats.h"
#include <pthread.h>

/* HakiOS camera_device.c supplies the vendor factory sequence. The
 * selected H13ISPCaptureDeviceCreate stores x0/x1/x2 as allocator/options/
 * output, and BWFigCaptureDeviceVendor retains the supplied callback.
 * This binding waits for an actual backend; it cannot use a diagnostic
 * journal, TPG frame or a file renderer as a live camera. */
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static struct NezhaCameraFigBackend backend;
static bool (*backend_ready)(void *);
static bool bound;
enum { Invalid=-12780, Unavailable=-12783, Busy=-12781 };

NezhaFigStatus nezha_camera_factory_bind(const struct NezhaCameraFigBackend *value,
    bool (*ready)(void *))
{
    if (!value || !value->begin || !value->end || !ready) return Invalid;
    pthread_mutex_lock(&mutex);
    const NezhaFigStatus status=bound ? Busy:0;
    if (!status) { backend=*value;backend_ready=ready;bound=true; }
    pthread_mutex_unlock(&mutex);
    return status;
}

NezhaFigStatus nezha_camera_device_create(NezhaFigAllocator allocator,
    NezhaFigDictionary options, NezhaFigObject *device)
{
    if (!device) return Invalid;
    *device=NULL;
    if (options && CFGetTypeID(options)!=CFDictionaryGetTypeID()) return Invalid;
    pthread_mutex_lock(&mutex);
    const bool available=bound;
    const struct NezhaCameraFigBackend selected=backend;
    bool (*ready)(void *)=backend_ready;
    pthread_mutex_unlock(&mutex);
    if (!available || !ready(selected.context)) return Unavailable;
    CFArrayRef formats=nezha_camera_main_formats();
    if (!formats) return Unavailable;
    const NezhaFigStatus status=nezha_camera_fig_provider_create(allocator,&selected,formats,device);
    CFRelease(formats);
    return status;
}
