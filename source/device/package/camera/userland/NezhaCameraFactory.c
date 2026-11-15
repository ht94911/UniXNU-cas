/* SPDX-License-Identifier: MIT */
#include "NezhaCameraFactory.h"
#include "NezhaCameraFormats.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* HakiOS camera_device.c supplies the vendor factory sequence. The
 * selected H13ISPCaptureDeviceCreate stores x0/x1/x2 as allocator/deviceID/
 * output, reads x1 with CFStringGetCStringPtr/GetCString, and the vendor
 * retains the supplied callback. The measured built-in device ID is Default.
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
    NezhaFigString device_id, NezhaFigObject *device)
{
    static unsigned reported;
    if (!device) return Invalid;
    *device=NULL;
    if (device_id) {
        char name[32];
        if (CFGetTypeID(device_id)!=CFStringGetTypeID() ||
            !CFStringGetCString(device_id,name,sizeof(name),kCFStringEncodingUTF8) ||
            strcmp(name,"Default")) return Invalid;
    }
    pthread_mutex_lock(&mutex);
    const bool available=bound;
    const struct NezhaCameraFigBackend selected=backend;
    bool (*ready)(void *)=backend_ready;
    pthread_mutex_unlock(&mutex);
    const bool hardware_ready=available && ready(selected.context);
    const bool report=__atomic_fetch_add(&reported,1u,__ATOMIC_RELAXED)<4;
    if (report) fprintf(stderr,"nezha-camera: device factory bound=%u node=%u\n",
        (unsigned)available,(unsigned)hardware_ready);
    if (!hardware_ready) return Unavailable;
    CFArrayRef formats=nezha_camera_main_formats();
    if (!formats) return Unavailable;
    const NezhaFigStatus status=nezha_camera_fig_provider_create(allocator,&selected,formats,device);
    if (report) fprintf(stderr,"nezha-camera: Fig device create status=%d object=%u\n",status,(unsigned)(*device!=NULL));
    CFRelease(formats);
    return status;
}
