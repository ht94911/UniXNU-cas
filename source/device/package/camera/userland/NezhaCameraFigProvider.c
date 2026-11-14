/* SPDX-License-Identifier: MIT */
#include "NezhaCameraFigProvider.h"
#include <Block.h>
#include <dispatch/dispatch.h>
#include <string.h>

/* HakiOS f128f1d camera_device.c is the device/stream property and graph
 * sequencing reference. Tables use this port's measured 24A446 PAC ABI;
 * property/output strings were decoded from its CMCaptureDevice/Core.
 * This provider advertises one main stream and only implemented controls. */
enum { Invalid = -12780, Unsupported = -12782, Unavailable = -12783 };
static const char queue_key;
static const char *const output_names[] = {"PrimaryScaler", "SecondaryScaler", "StillImage"};

struct Stream {
    dispatch_queue_t queue;
    struct NezhaCameraFigBackend backend;
    struct NezhaCameraPreview preview;
    struct NezhaCameraGraphOutput output[3];
    CFDictionaryRef configurations, handlers, enabled, colors;
    CFArrayRef formats;
    CFDictionaryRef supported;
    unsigned stills, generation;
    bool collecting, running, invalid;
};
struct Device { NezhaFigObject stream; CFArrayRef streams; CFDictionaryRef supported; };

static struct Stream *stream_of(NezhaFigObject o) { return nezha_fig_capture24_storage(o); }
static struct Device *device_of(NezhaFigObject o) { return nezha_fig_capture24_storage(o); }

static void serial(struct Stream *stream, dispatch_block_t work)
{
    if (dispatch_get_specific(&queue_key) == stream) work();
    else dispatch_sync(stream->queue, work);
}

static CFNumberRef number(double value)
{
    return CFNumberCreate(NULL, kCFNumberDoubleType, &value);
}

static bool key_is(CFStringRef key, const char *name)
{
    char text[128];
    return key && CFGetTypeID(key) == CFStringGetTypeID() &&
        CFStringGetCString(key, text, sizeof(text), kCFStringEncodingUTF8) && !strcmp(text, name);
}

static CFStringRef string(const char *name)
{
    return CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
}

static CFDictionaryRef dictionary(void)
{
    return CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
}

static CFDictionaryRef supported(const char *const names[][3], size_t count)
{
    CFMutableDictionaryRef all = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!all) return NULL;
    for (size_t i=0; i<count; ++i) {
        CFStringRef name=string(names[i][0]), type=string(names[i][1]), access=string(names[i][2]);
        if (!name || !type || !access) {
            if (name) CFRelease(name);
            if (type) CFRelease(type);
            if (access) CFRelease(access);
            CFRelease(all);
            return NULL;
        }
        const void *keys[]={CFSTR("PropertyType"), CFSTR("ReadWriteStatus")};
        const void *values[]={type,access};
        CFDictionaryRef one=CFDictionaryCreate(NULL,keys,values,2,
            &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        if (one) CFDictionarySetValue(all,name,one);
        CFRelease(name);CFRelease(type);CFRelease(access);
        if (!one) { CFRelease(all); return NULL; }
        CFRelease(one);
    }
    return all;
}

static void replace(CFDictionaryRef *where, CFDictionaryRef value)
{
    CFRetain(value);
    if (*where) CFRelease(*where);
    *where=value;
}

static NezhaFigStatus validate_outputs(CFDictionaryRef configurations,
    CFDictionaryRef handlers, CFDictionaryRef enabled)
{
    for (unsigned i=0; i<3; ++i) {
        CFStringRef name=string(output_names[i]);
        if (!name) return Unavailable;
        CFTypeRef configuration=CFDictionaryGetValue(configurations,name);
        CFTypeRef callbacks=CFDictionaryGetValue(handlers,name);
        CFTypeRef on=CFDictionaryGetValue(enabled,name);
        CFRelease(name);
        if ((configuration && CFGetTypeID(configuration)!=CFDictionaryGetTypeID()) ||
            (callbacks && CFGetTypeID(callbacks)!=CFDictionaryGetTypeID()) ||
            (on && CFGetTypeID(on)!=CFBooleanGetTypeID() && CFGetTypeID(on)!=CFNumberGetTypeID()))
            return Invalid;
        CFTypeRef pool=configuration ? CFDictionaryGetValue(configuration,CFSTR("BufferPool")) : NULL;
        if (pool && CFGetTypeID(pool)!=CVPixelBufferPoolGetTypeID()) return Invalid;
    }
    return 0;
}

static NezhaFigStatus refresh_outputs(struct Stream *stream)
{
    for (unsigned i=0; i<3; ++i) {
        CFStringRef name=string(output_names[i]);
        if (!name) return Unavailable;
        CFTypeRef configuration=CFDictionaryGetValue(stream->configurations,name);
        CFTypeRef handlers=CFDictionaryGetValue(stream->handlers,name);
        CFTypeRef enabled=CFDictionaryGetValue(stream->enabled,name);
        CFRelease(name);
        if (!configuration || !handlers) { nezha_camera_graph_output_reset(&stream->output[i]); continue; }
        if (CFGetTypeID(configuration)!=CFDictionaryGetTypeID() ||
            CFGetTypeID(handlers)!=CFDictionaryGetTypeID()) return Invalid;
        CFTypeRef pool=CFDictionaryGetValue(configuration,CFSTR("BufferPool"));
        CFTypeRef handler=CFDictionaryGetValue(handlers,CFSTR("PixelBufferHandler"));
        if (!pool || !handler) { nezha_camera_graph_output_reset(&stream->output[i]); continue; }
        if (stream->output[i].pool!=(CVPixelBufferPoolRef)pool ||
            stream->output[i].handler!=(NezhaCameraPixelHandler)handler) {
            CVReturn status=nezha_camera_graph_output_configure(&stream->output[i],
                (CVPixelBufferPoolRef)pool,(NezhaCameraPixelHandler)handler,i!=2);
            if (status) return status;
        }
        bool on=enabled && CFGetTypeID(enabled)==CFBooleanGetTypeID() && CFBooleanGetValue(enabled);
        if (enabled && CFGetTypeID(enabled)==CFNumberGetTypeID()) {
            double n=0;
            on=CFNumberGetValue(enabled,kCFNumberDoubleType,&n) && n>0;
        }
        /* StillImage is requested one frame at a time. Scalers follow the
         * graph's enabled dictionary. Never deliver merely on reservation. */
        if (stream->running && (on || i==2)) (void)nezha_camera_graph_output_start(&stream->output[i]);
        else nezha_camera_graph_output_stop(&stream->output[i]);
    }
    return 0;
}

static NezhaFigStatus begin(struct Stream *s, bool graph)
{
    if (s->invalid) return Unavailable;
    if (graph && s->running) return 0;
    if (!s->collecting) {
        NezhaFigStatus result=s->backend.begin(s->backend.context);
        if (result) return result;
        s->collecting=true;
    }
    if (graph) {
        s->running=true;
        NezhaFigStatus result=refresh_outputs(s);
        if (result) {
            s->running=false;
            for (unsigned i=0;i<3;++i) nezha_camera_graph_output_stop(&s->output[i]);
        }
        return result;
    }
    return 0;
}

static NezhaFigStatus stop_on_queue(struct Stream *s)
{
    s->running=false;s->stills=0;++s->generation;
    for (unsigned i=0;i<3;++i) nezha_camera_graph_output_stop(&s->output[i]);
    if (!s->collecting) return 0;
    NezhaFigStatus result=s->backend.end(s->backend.context);
    if (!result) s->collecting=false;
    return result;
}

static NezhaFigStatus stream_start(NezhaFigObject o)
{
    struct Stream *s=stream_of(o);if (!s || !s->queue) return Unavailable;
    __block NezhaFigStatus result;
    serial(s,^{ result=begin(s,true); });return result;
}

static NezhaFigStatus stream_stop(NezhaFigObject o)
{
    struct Stream *s=stream_of(o);if (!s || !s->queue) return Unavailable;
    __block NezhaFigStatus result;
    serial(s,^{ result=stop_on_queue(s); });return result;
}

static NezhaFigStatus stream_invalidate(NezhaFigObject o)
{
    struct Stream *s=stream_of(o);if (!s || !s->queue) return Unavailable;
    __block NezhaFigStatus result;
    serial(s,^{ s->invalid=true; result=stop_on_queue(s); });return result;
}

static void stream_finalize(NezhaFigObject o)
{
    struct Stream *s=stream_of(o);if (!s) return;
    if (s->queue) {
        (void)stream_invalidate(o);
        serial(s,^{ for (unsigned i=0;i<3;++i) nezha_camera_graph_output_reset(&s->output[i]); });
        dispatch_release(s->queue);
    }
    CFTypeRef owned[]={s->configurations,s->handlers,s->enabled,s->colors,s->formats,s->supported};
    for (unsigned i=0;i<sizeof(owned)/sizeof(owned[0]);++i) if (owned[i]) CFRelease(owned[i]);
}

static NezhaFigString stream_description(NezhaFigObject o)
{
    (void)o;return CFRetain(CFSTR("<NezhaCaptureStream OVX10500U>"));
}

static NezhaFigStatus stream_copy(NezhaFigObject o,NezhaFigString key,NezhaFigAllocator allocator,void *out)
{
    (void)allocator;
    if (!out) return Invalid;
    *(CFTypeRef *)out=NULL;
    struct Stream *s=stream_of(o);if (!s || !s->queue) return Unavailable;
    __block CFTypeRef value=NULL;
    serial(s,^{
        if (key_is(key,"PortType")) value=CFRetain(CFSTR("PortTypeBack"));
        else if (key_is(key,"UniqueID") || key_is(key,"ModelID")) value=CFRetain(CFSTR("nezha.ovx10500u.main"));
        else if (key_is(key,"NonLocalizedName") || key_is(key,"LocalizedName")) value=CFRetain(CFSTR("nezha main camera"));
        else if (key_is(key,"Manufacturer")) value=CFRetain(CFSTR("Xiaomi"));
        else if (key_is(key,"SupportedFormatsArray")) value=CFRetain(s->formats);
        else if (key_is(key,"SupportedPropertiesDictionary")) value=CFRetain(s->supported);
        else if (key_is(key,"Clock")) value=CFRetain(CMClockGetHostTimeClock());
        else if (key_is(key,"FormatIndex")) value=number(0);
        else if (key_is(key,"MinimumFrameRate") || key_is(key,"MaximumFrameRate") ||
            key_is(key,"MaximumAllowedFrameRate")) value=number(30);
        else if (key_is(key,"FocalLength")) value=number(8.71);
        else if (key_is(key,"LensFNumber")) value=number(1.67);
        else if (key_is(key,"PixelSize")) value=number(3.2);
        else if (key_is(key,"MaxZoomFactor") || key_is(key,"ZoomFactor")) value=number(1);
        else if (key_is(key,"ColorSpace")) value=number(0);
        else if (key_is(key,"SupportedColorSpaces")) {
            CFNumberRef srgb=number(0);
            if (srgb) { value=CFArrayCreate(NULL,(const void **)&srgb,1,&kCFTypeArrayCallBacks); CFRelease(srgb); }
        } else if (key_is(key,"HasAutoFocusPositionSensor") || key_is(key,"DeferAdditionOfAttachments")) value=CFRetain(kCFBooleanFalse);
        else if (key_is(key,"VideoOutputConfigurations")) value=CFRetain(s->configurations);
        else if (key_is(key,"VideoOutputsEnabled")) value=CFRetain(s->enabled);
        else if (key_is(key,"VideoOutputsColorInfo")) value=CFRetain(s->colors);
    });
    *(CFTypeRef *)out=value;return value ? 0 : Unavailable;
}

static NezhaFigStatus stream_set(NezhaFigObject o,NezhaFigString key,const void *value)
{
    struct Stream *s=stream_of(o);if (!s || !s->queue) return Unavailable;
    __block NezhaFigStatus result=Unsupported;
    serial(s,^{
        if (s->invalid) { result=Unavailable;return; }
        if (key_is(key,"VideoOutputConfigurations") || key_is(key,"VideoOutputsEnabled") ||
            key_is(key,"VideoOutputHandlers") || key_is(key,"VideoOutputsColorInfo")) {
            if (!value || CFGetTypeID(value)!=CFDictionaryGetTypeID()) { result=Invalid;return; }
            CFDictionaryRef *where=key_is(key,"VideoOutputConfigurations") ? &s->configurations :
                key_is(key,"VideoOutputsEnabled") ? &s->enabled :
                key_is(key,"VideoOutputHandlers") ? &s->handlers : &s->colors;
            result=validate_outputs(where==&s->configurations ? value : s->configurations,
                where==&s->handlers ? value : s->handlers, where==&s->enabled ? value : s->enabled);
            if (result) return;
            replace(where,value);
            if (where==&s->colors) { result=0;return; }
            ++s->generation;
            result=refresh_outputs(s);
            if (result) {
                s->running=false;
                for (unsigned i=0;i<3;++i) nezha_camera_graph_output_stop(&s->output[i]);
            }
            return;
        }
        if (key_is(key,"StillImageCaptureNow")) {
            if (!value || CFGetTypeID(value)!=CFDictionaryGetTypeID()) result=Invalid;
            else if (!s->running || !s->output[2].running) result=Unavailable;
            else if (s->stills) result=-12781;
            else { s->stills=1;result=0; }
            return;
        }
        if (key_is(key,"DeferAdditionOfAttachments")) {
            result=value && CFGetTypeID(value)==CFBooleanGetTypeID() && !CFBooleanGetValue(value) ? 0 : Unsupported;
            return;
        }
        if (key_is(key,"FormatIndex") || key_is(key,"ColorSpace") || key_is(key,"ZoomFactor") ||
            key_is(key,"MinimumFrameRate") || key_is(key,"MaximumFrameRate") || key_is(key,"MaximumAllowedFrameRate")) {
            double n;
            if (!value || CFGetTypeID(value)!=CFNumberGetTypeID() ||
                !CFNumberGetValue(value,kCFNumberDoubleType,&n)) { result=Invalid;return; }
            const double allowed=(key_is(key,"FormatIndex") || key_is(key,"ColorSpace")) ? 0 :
                key_is(key,"ZoomFactor") ? 1 : 30;
            result=n==allowed ? 0 : Unsupported;
        }
    });return result;
}

static NezhaFigStatus stream_attachments(NezhaFigObject o,NezhaFigSample sample,NezhaFigDictionary options)
{
    (void)o;(void)options;
    return sample ? 0 : Invalid; /* GraphOutput already propagates per-frame metadata. */
}

static const struct NezhaFigBaseClass stream_base={.version=1,.storage_size=sizeof(struct Stream),
    .invalidate=stream_invalidate,.finalize=stream_finalize,.copy_description=stream_description,
    .copy_property=stream_copy,.set_property=stream_set};
static const struct NezhaFigStreamClass stream_class={.version=2,.start=stream_start,.stop=stream_stop,
    .add_attachments=stream_attachments};
static const struct NezhaFigStreamVTable stream_table={.base=&stream_base,.stream=&stream_class};

static NezhaFigStatus device_control(NezhaFigObject o,NezhaFigArray streams,bool release)
{
    struct Device *d=device_of(o);
    if (!d || !streams || CFGetTypeID(streams)!=CFArrayGetTypeID() || CFArrayGetCount(streams)!=1 ||
        CFArrayGetValueAtIndex(streams,0)!=d->stream) return Invalid;
    struct Stream *s=stream_of(d->stream);
    __block NezhaFigStatus result;
    serial(s,^{ result=release ? stop_on_queue(s) : begin(s,false); });return result;
}
static NezhaFigStatus request(NezhaFigObject o,NezhaFigArray streams,NezhaFigDictionary options)
{ (void)options;return device_control(o,streams,false); }
static NezhaFigStatus relinquish(NezhaFigObject o,NezhaFigArray streams,NezhaFigDictionary options)
{ (void)options;return device_control(o,streams,true); }
static NezhaFigStatus device_invalidate(NezhaFigObject o)
{ struct Device *d=device_of(o);return d && d->stream ? stream_invalidate(d->stream) : 0; }
static void device_finalize(NezhaFigObject o)
{
    struct Device *d=device_of(o);if (!d) return;
    if (d->streams) CFRelease(d->streams);
    if (d->supported) CFRelease(d->supported);
}
static NezhaFigString device_description(NezhaFigObject o)
{ (void)o;return CFRetain(CFSTR("<NezhaCaptureDevice main camera>")); }
static NezhaFigStatus device_copy(NezhaFigObject o,NezhaFigString key,NezhaFigAllocator allocator,void *out)
{
    (void)allocator;
    if (!out) return Invalid;
    *(CFTypeRef *)out=NULL;
    struct Device *d=device_of(o);if (!d) return Unavailable;
    if (key_is(key,"StreamArray")) *(CFTypeRef *)out=CFRetain(d->streams);
    else if (key_is(key,"Clock")) *(CFTypeRef *)out=CFRetain(CMClockGetHostTimeClock());
    else if (key_is(key,"SupportedPropertiesDictionary")) *(CFTypeRef *)out=CFRetain(d->supported);
    else return Unavailable;
    return 0;
}
static NezhaFigStatus device_set(NezhaFigObject o,NezhaFigString key,const void *value)
{
    struct Device *d=device_of(o);if (!d) return Unavailable;
    if (key_is(key,"ClientIsActive") && value && CFGetTypeID(value)==CFBooleanGetTypeID())
        return CFBooleanGetValue(value) ? 0 : stream_stop(d->stream);
    return Unsupported;
}
static const struct NezhaFigBaseClass device_base={.version=1,.storage_size=sizeof(struct Device),
    .invalidate=device_invalidate,.finalize=device_finalize,.copy_description=device_description,
    .copy_property=device_copy,.set_property=device_set};
static const struct NezhaFigDeviceClass device_class={.version=3,.request_streams=request,.relinquish_streams=relinquish};
static const struct NezhaFigDeviceVTable device_table={.base=&device_base,.device=&device_class};

NezhaFigStatus nezha_camera_fig_provider_create(NezhaFigAllocator allocator,
    const struct NezhaCameraFigBackend *backend,CFArrayRef formats,NezhaFigObject *out)
{
    if (!out) return Invalid;
    *out=NULL;
    if (!backend || !backend->begin || !backend->end || !formats ||
        CFGetTypeID(formats)!=CFArrayGetTypeID() || CFArrayGetCount(formats)!=1) return Invalid;
    static const char *const props[][3]={
        {"PortType","String","ReadOnly"},{"UniqueID","String","ReadOnly"},
        {"NonLocalizedName","String","ReadOnly"},{"Manufacturer","String","ReadOnly"},
        {"SupportedFormatsArray","Collection","ReadOnly"},{"Clock","Clock","ReadOnly"},
        {"FocalLength","Number","ReadOnly"},{"LensFNumber","Number","ReadOnly"},
        {"PixelSize","Number","ReadOnly"},{"MaxZoomFactor","Number","ReadOnly"},
        {"SupportedColorSpaces","Collection","ReadOnly"},{"ColorSpace","Number","ReadWrite"},
        {"FormatIndex","Number","ReadWrite"},{"MinimumFrameRate","Number","ReadWrite"},
        {"MaximumFrameRate","Number","ReadWrite"},{"MaximumAllowedFrameRate","Number","ReadWrite"},
        {"VideoOutputConfigurations","Collection","ReadWrite"},{"VideoOutputsEnabled","Collection","ReadWrite"},
        {"VideoOutputHandlers","Collection","WriteOnly"},{"VideoOutputsColorInfo","Collection","ReadWrite"},
        {"DeferAdditionOfAttachments","Boolean","ReadWrite"},{"StillImageCaptureNow","Collection","WriteOnly"},
    };
    static const char *const device_props[][3]={{"StreamArray","Collection","ReadOnly"},
        {"Clock","Clock","ReadOnly"},{"ClientIsActive","Boolean","ReadWrite"}};
    NezhaFigObject stream=NULL,device=NULL;
    NezhaFigStatus status=nezha_fig_capture24_create_stream(allocator,&stream_table,&stream);
    if (status) return status;
    struct Stream *s=stream_of(stream);
    s->backend=*backend;s->formats=CFRetain(formats);
    s->queue=dispatch_queue_create("org.unixnu.nezha.camera.main",DISPATCH_QUEUE_SERIAL);
    s->configurations=dictionary();s->handlers=dictionary();s->enabled=dictionary();s->colors=dictionary();
    s->supported=supported(props,sizeof(props)/sizeof(props[0]));
    if (!s->queue || !s->configurations || !s->handlers || !s->enabled || !s->colors || !s->supported) {
        CFRelease(stream);return Unavailable;
    }
    dispatch_queue_set_specific(s->queue,&queue_key,s,NULL);
    nezha_camera_preview_init(&s->preview);
    status=nezha_fig_capture24_create_device(allocator,&device_table,&device);
    if (status) { CFRelease(stream);return status; }
    struct Device *d=device_of(device);d->stream=stream;
    d->streams=CFArrayCreate(NULL,(const void **)&stream,1,&kCFTypeArrayCallBacks);
    d->supported=supported(device_props,sizeof(device_props)/sizeof(device_props[0]));
    CFRelease(stream);
    if (!d->streams || !d->supported) { CFRelease(device);return Unavailable; }
    *out=device;return 0;
}

NezhaFigStatus nezha_camera_fig_provider_deliver(NezhaFigObject o,
    const struct NezhaCameraPreviewFrame *frame,const uint8_t *raw,size_t bytes,
    uint64_t completed_time,CFDictionaryRef metadata)
{
    struct Device *d=device_of(o);
    if (!d || !frame || frame->width!=4096 || frame->height!=3072 || frame->stride!=5120 ||
        frame->bayer!=3 || frame->black!=64 || frame->white!=1023 || bytes!=15728640 ||
        !raw || !completed_time || !metadata || CFGetTypeID(metadata)!=CFDictionaryGetTypeID()) return Invalid;
    struct Stream *s=stream_of(d->stream);
    __block NezhaFigStatus result=Unavailable;
    serial(s,^{
        if (!s->running || s->invalid) return;
        const unsigned generation=s->generation;
        bool attempted=false;
        for (unsigned i=0;i<3 && s->running && s->generation==generation;++i) {
            if (!s->output[i].running || (i==2 && !s->stills)) continue;
            if (i==2) s->stills=0;
            NezhaFigStatus status=nezha_camera_graph_output_deliver(&s->output[i],&s->preview,frame,raw,bytes,completed_time,metadata);
            if (!attempted || (status && !result)) result=status;
            attempted=true;
            if (status && i==2 && s->running && s->generation==generation) s->stills=1;
        }
    });return result;
}
