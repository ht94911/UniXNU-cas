/* SPDX-License-Identifier: MIT */
#include "NezhaCameraFigProvider.h"
#include "NezhaCameraRenderer.h"
#include <Block.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

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
    uint64_t delivered[3], first_time[3], last_time[3];
    bool collecting, running, invalid, bound;
};
struct Device { NezhaFigObject stream; CFArrayRef streams; CFDictionaryRef supported; };

static struct Stream *stream_of(NezhaFigObject o) { return nezha_fig_capture24_storage(o); }
static struct Device *device_of(NezhaFigObject o) { return nezha_fig_capture24_storage(o); }

/* The original HakiOS provider records graph start/stop and reservation
 * churn. Keep this existing daemon stderr bounded while locating the
 * actual close before native058's first completed frame. */
static void report_lifecycle(const char *operation, const struct Stream *s, NezhaFigStatus status)
{
    static unsigned reports;
    if (__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)>=48) return;
    fprintf(stderr,"nezha-camera: lifecycle %s status=%d collecting=%u running=%u invalid=%u generation=%u\n",
        operation,status,(unsigned)s->collecting,(unsigned)s->running,(unsigned)s->invalid,s->generation);
}

static void report_output(const struct Stream *s, unsigned index)
{
    static unsigned reports;
    if (!s->delivered[index] || __atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)>=40) return;
    fprintf(stderr,"nezha-camera: output=%s callbacks=%llu first_ticks=%llu last_ticks=%llu generation=%u\n",
        output_names[index],(unsigned long long)s->delivered[index],
        (unsigned long long)s->first_time[index],(unsigned long long)s->last_time[index],s->generation);
    if (index==0 && nezha_camera_renderer_report_flow)
        nezha_camera_renderer_report_flow(s->preview.completed_sequence,s->last_time[index],s->generation);
}

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

/* Keep the existing daemon stderr bounded while observing the original
 * source loader's requests. This does not change a property or its error. */
static void report_property(bool stream, CFStringRef key, NezhaFigStatus status, CFTypeRef value)
{
    static unsigned reports[2];
    if (__atomic_fetch_add(&reports[stream],1u,__ATOMIC_RELAXED) >= (stream ? 96u : 32u)) return;
    char name[128]="<invalid>", text[128]="";
    if (key && CFGetTypeID(key)==CFStringGetTypeID())
        (void)CFStringGetCString(key,name,sizeof(name),kCFStringEncodingUTF8);
    const char *kind="null";
    long count=-1;
    double scalar=0;
    if (value) {
        CFTypeID type=CFGetTypeID(value);
        kind="object";
        if (type==CFStringGetTypeID()) {
            kind="string";
            (void)CFStringGetCString(value,text,sizeof(text),kCFStringEncodingUTF8);
        } else if (type==CFArrayGetTypeID()) {
            kind="array";count=CFArrayGetCount(value);
        } else if (type==CFDictionaryGetTypeID()) kind="dictionary";
        else if (type==CFNumberGetTypeID()) {
            kind="number";(void)CFNumberGetValue(value,kCFNumberDoubleType,&scalar);
        } else if (type==CFBooleanGetTypeID()) {
            kind="boolean";scalar=CFBooleanGetValue(value);
        }
    }
    fprintf(stderr,"nezha-camera: %s copy key=%s status=%d kind=%s count=%ld scalar=%.3f text=%s\n",
        stream ? "stream" : "device",name,status,kind,count,scalar,text);
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

static void update_entry(const void *key,const void *value,void *context)
{
    CFDictionarySetValue((CFMutableDictionaryRef)context,key,value);
}

struct OutputUpdate { CFMutableDictionaryRef merged; bool nested,failed; };
static void mark_entry(const void *key,const void *value,void *context)
{
    (void)key;(void)value;*(bool *)context=true;
}

static void update_output(const void *key,const void *value,void *context)
{
    struct OutputUpdate *update=context;
    if (update->failed) return;
    CFTypeRef current=CFDictionaryGetValue(update->merged,key);
    bool nonempty=false;
    if (update->nested && CFGetTypeID(value)==CFDictionaryGetTypeID())
        CFDictionaryApplyFunction(value,mark_entry,&nonempty);
    if (nonempty && current && CFGetTypeID(current)==CFDictionaryGetTypeID()) {
        CFMutableDictionaryRef fields=CFDictionaryCreateMutableCopy(NULL,0,current);
        if (!fields) { update->failed=true;return; }
        CFDictionaryApplyFunction(value,update_entry,fields);
        CFDictionarySetValue(update->merged,key,fields);CFRelease(fields);
    } else CFDictionarySetValue(update->merged,key,value);
}

/* HakiOS updates each named output independently. The original24A446
 * prepareForStillImageCapture path likewise sets a dictionary containing
 * only StillImage when changing its retained-buffer count. Omitted outputs
 * keep their existing pool, handler and enabled state. */
static CFMutableDictionaryRef updated_outputs(CFDictionaryRef current,CFDictionaryRef value,bool nested)
{
    struct OutputUpdate update={CFDictionaryCreateMutableCopy(NULL,0,current),nested,false};
    if (!update.merged) return NULL;
    CFDictionaryApplyFunction(value,update_output,&update);
    if (update.failed) { CFRelease(update.merged);return NULL; }
    return update.merged;
}

static void report_output_state(const struct Stream *s,const char *operation,NezhaFigStatus status)
{
    static unsigned reports;
    if (__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)>=48) return;
    fprintf(stderr,"nezha-camera: output-update %s status=%d sequence=%llu generation=%u running=%u "
        "primary=%u/%u/%u secondary=%u/%u/%u still=%u/%u/%u pending=%u\n",operation,status,
        (unsigned long long)s->preview.completed_sequence,s->generation,(unsigned)s->running,
        (unsigned)(s->output[0].pool!=NULL),(unsigned)(s->output[0].handler!=NULL),(unsigned)s->output[0].running,
        (unsigned)(s->output[1].pool!=NULL),(unsigned)(s->output[1].handler!=NULL),(unsigned)s->output[1].running,
        (unsigned)(s->output[2].pool!=NULL),(unsigned)(s->output[2].handler!=NULL),(unsigned)s->output[2].running,s->stills);
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
        nezha_camera_preview_init(&s->preview);
        memset(s->delivered,0,sizeof(s->delivered));
        memset(s->first_time,0,sizeof(s->first_time));
        memset(s->last_time,0,sizeof(s->last_time));
        __atomic_add_fetch(&s->generation,1u,__ATOMIC_RELEASE);
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
    if (s->running) for (unsigned i=0;i<3;++i) report_output(s,i);
    s->running=false;s->stills=0;__atomic_add_fetch(&s->generation,1u,__ATOMIC_RELEASE);
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
    serial(s,^{ result=begin(s,true); report_lifecycle("graph-start",s,result); });return result;
}

static NezhaFigStatus stream_stop(NezhaFigObject o)
{
    struct Stream *s=stream_of(o);if (!s || !s->queue) return Unavailable;
    __block NezhaFigStatus result;
    serial(s,^{ report_lifecycle("graph-stop-enter",s,0); result=stop_on_queue(s);
        report_lifecycle("graph-stop-done",s,result); });return result;
}

static NezhaFigStatus stream_invalidate(NezhaFigObject o)
{
    struct Stream *s=stream_of(o);if (!s || !s->queue) return Unavailable;
    __block NezhaFigStatus result;
    serial(s,^{ report_lifecycle("invalidate-enter",s,0); s->invalid=true; result=stop_on_queue(s);
        report_lifecycle("invalidate-done",s,result); });return result;
}

static void stream_finalize(NezhaFigObject o)
{
    struct Stream *s=stream_of(o);if (!s) return;
    if (s->queue) {
        (void)stream_invalidate(o);
        if (s->bound && s->backend.bind_stream)
            (void)s->backend.bind_stream(s->backend.context,NULL);
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
        /* CMCapture's measured model loader looks up this built-in rear
         * role; ModelID separately names the actual sensor implementation. */
        else if (key_is(key,"UniqueID")) value=CFRetain(CFSTR("com.apple.avfoundation.avcapturedevice.built-in_video:0"));
        else if (key_is(key,"ModelID")) value=CFRetain(CFSTR("nezha.ovx10500u.main"));
        else if (key_is(key,"NonLocalizedName") || key_is(key,"LocalizedName")) value=CFRetain(CFSTR("Back Camera"));
        else if (key_is(key,"Manufacturer")) value=CFRetain(CFSTR("Xiaomi"));
        else if (key_is(key,"SupportedFormatsArray")) value=CFRetain(s->formats);
        else if (key_is(key,"FormatDescription")) {
            CFDictionaryRef format=CFArrayGetValueAtIndex(s->formats,0);
            CFTypeRef description=CFDictionaryGetValue(format,CFSTR("FormatDescription"));
            if (description) value=CFRetain(description);
        }
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
    *(CFTypeRef *)out=value;
    const NezhaFigStatus status=value ? 0 : Unavailable;
    report_property(true,key,status,value);
    return status;
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
            CFMutableDictionaryRef merged=updated_outputs(*where,value,where==&s->configurations);
            if (!merged) { result=Unavailable;return; }
            result=validate_outputs(where==&s->configurations ? merged : s->configurations,
                where==&s->handlers ? merged : s->handlers, where==&s->enabled ? merged : s->enabled);
            if (result) { CFRelease(merged);return; }
            replace(where,merged);CFRelease(merged);
            if (where==&s->colors) { result=0;return; }
            __atomic_add_fetch(&s->generation,1u,__ATOMIC_RELEASE);
            result=refresh_outputs(s);
            if (result) {
                s->running=false;
                for (unsigned i=0;i<3;++i) nezha_camera_graph_output_stop(&s->output[i]);
            }
            report_output_state(s,where==&s->configurations ? "configurations" :
                where==&s->handlers ? "handlers" : "enabled",result);
            return;
        }
        if (key_is(key,"StillImageCaptureNow")) {
            /* Original24A446 discards an empty options dictionary before
             * sending this command. NULL requests one ordinary photograph;
             * the device/stream still attaches its own capture settings. */
            if (value && CFGetTypeID(value)!=CFDictionaryGetTypeID()) result=Invalid;
            else if (!s->running || !s->output[2].running) result=Unavailable;
            else if (s->stills) result=-12781;
            else { s->stills=1;result=0; }
            static unsigned requests;
            if (__atomic_fetch_add(&requests,1u,__ATOMIC_RELAXED)<16)
                fprintf(stderr,"nezha-camera: still request payload=%s status=%d sequence=%llu generation=%u pending=%u\n",
                    !value ? "none" : CFGetTypeID(value)==CFDictionaryGetTypeID() ? "dictionary" : "other",
                    result,(unsigned long long)s->preview.completed_sequence,s->generation,s->stills);
            report_output_state(s,"StillImageCaptureNow",result);
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

static void add_missing(const void *key,const void *value,void *context)
{
    CFMutableDictionaryRef dictionary=context;
    if (!CFDictionaryGetValue(dictionary,key)) CFDictionarySetValue(dictionary,key,value);
}

static NezhaFigStatus stream_attachments(NezhaFigObject o,NezhaFigSample sample,NezhaFigDictionary options)
{
    (void)o;(void)options;
    if (!sample) return Invalid;
    /* HakiOS's AddAttachmentsToSampleBuffer moves the actual pixel's
     * metadata to the sample the capture graph created, preserving its
     * own fields. A last-frame fallback could attach a different frame's
     * timestamp, so this provider only accepts this image's dictionary. */
    CVImageBufferRef image=CMSampleBufferGetImageBuffer(sample);
    if (!image) return 0;
    CFTypeRef own=CVBufferCopyAttachment(image,CFSTR("MetadataDictionary"),NULL);
    if (!own) return 0;
    if (CFGetTypeID(own)!=CFDictionaryGetTypeID()) { CFRelease(own);return 0; }
    CFTypeRef existing=CMGetAttachment(sample,CFSTR("MetadataDictionary"),NULL);
    if (existing==own) { CFRelease(own);return 0; }
    CFMutableDictionaryRef merged=CFDictionaryCreateMutableCopy(NULL,0,
        existing && CFGetTypeID(existing)==CFDictionaryGetTypeID() ? existing:own);
    if (!merged) { CFRelease(own);return Unavailable; }
    if (existing && CFGetTypeID(existing)==CFDictionaryGetTypeID())
        CFDictionaryApplyFunction(own,add_missing,merged);
    CFRelease(own);
    CMSetAttachment(sample,CFSTR("MetadataDictionary"),merged,kCMAttachmentMode_ShouldPropagate);
    CFRelease(merged);
    return 0;
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
    serial(s,^{
        if (s->invalid) {result=Unavailable;return;}
        /* HakiOS f128f1d keeps the frame worker across the system's
         * discovery/reservation churn. Only actual graph Stop, inactive
         * client, invalidate or finalize shuts down this owned sensor. */
        result=release ? 0:begin(s,false);
        report_lifecycle(release ? "relinquish":"request",s,result);
    });return result;
}
static NezhaFigStatus request(NezhaFigObject o,NezhaFigArray streams,NezhaFigDictionary options)
{ (void)options;return device_control(o,streams,false); }
static NezhaFigStatus relinquish(NezhaFigObject o,NezhaFigArray streams,NezhaFigDictionary options)
{ (void)options;return device_control(o,streams,true); }
static NezhaFigStatus device_invalidate(NezhaFigObject o)
{
    struct Device *d=device_of(o);
    if (d && d->stream) {
        struct Stream *s=stream_of(d->stream);
        serial(s,^{ report_lifecycle("device-invalidate",s,0); });
    }
    /* HakiOS f128f1d keeps the independent stream alive when discovery
     * invalidates its temporary device. Actual062 closed before frame1
     * here; stream Stop/invalidate/finalize still owns hardware release. */
    return 0;
}
static void device_finalize(NezhaFigObject o)
{
    struct Device *d=device_of(o);if (!d) return;
    /* The capture graph may retain StreamArray after releasing this
     * device. Its last stream reference, rather than this device, ends
     * and unbinds the native reader, as the original provider does. */
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
    const NezhaFigStatus status=*(CFTypeRef *)out ? 0 : Unavailable;
    report_property(false,key,status,*(CFTypeRef *)out);
    return status;
}
static NezhaFigStatus device_set(NezhaFigObject o,NezhaFigString key,const void *value)
{
    struct Device *d=device_of(o);if (!d) return Unavailable;
    if (key_is(key,"ClientIsActive") && value && CFGetTypeID(value)==CFBooleanGetTypeID()) {
        struct Stream *s=stream_of(d->stream);
        const bool active=CFBooleanGetValue(value);
        if (s && s->queue)
            serial(s,^{ report_lifecycle(active ? "client-active":"client-inactive",s,0); });
        return active ? 0 : stream_stop(d->stream);
    }
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
        {"FormatDescription","FormatDescription","ReadOnly"},
        {"FocalLength","Number","ReadOnly"},{"LensFNumber","Number","ReadOnly"},
        {"PixelSize","Number","ReadOnly"},{"MaxZoomFactor","Number","ReadOnly"},
        {"ZoomFactor","Number","ReadWrite"},
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
    struct Device *d=device_of(device);
    d->streams=CFArrayCreate(NULL,(const void **)&stream,1,&kCFTypeArrayCallBacks);
    d->stream=d->streams ? stream : NULL;
    d->supported=supported(device_props,sizeof(device_props)/sizeof(device_props[0]));
    CFRelease(stream);
    if (!d->streams || !d->supported) { CFRelease(device);return Unavailable; }
    if (backend->bind_stream) {
        status=backend->bind_stream(backend->context,d->stream);
        if (status) { CFRelease(device);return status; }
        s->bound=true;
    }
    *out=device;return 0;
}

static bool frame_controls(CFDictionaryRef metadata, uint64_t completed_time,
    struct hakios_camera_frame *actual)
{
    const char *names[]={"FrameSequence","ExposureLines","LineNanoseconds","AnalogGainQ8",
        "GainUnity","GainMaximum","FrameLengthLines","MinimumFrameLengthLines",
        "MaximumFrameLengthLines","ExposureMargin"};
    uint32_t values[10]={0};bool complete=true;
    for (unsigned i=0;i<10;++i) {
        CFStringRef key=string(names[i]);
        CFTypeRef value=key ? CFDictionaryGetValue(metadata,key):NULL;
        double n=0;
        complete &= value && CFGetTypeID(value)==CFNumberGetTypeID() &&
            CFNumberGetValue(value,kCFNumberDoubleType,&n) && isfinite(n) &&
            n>=1 && n<=UINT32_MAX && n==(double)(uint32_t)n;
        if (complete) values[i]=(uint32_t)n;
        if (key) CFRelease(key);
    }
    if (!complete) return false;
    *actual=(struct hakios_camera_frame){.sequence=values[0],.time=completed_time,
        .exposure_lines=values[1],.line_ns=values[2],.gain=values[3],.gain_unity=values[4],
        .gain_max=values[5],.frame_lines=values[6],.frame_lines_min=values[7],
        .frame_lines_max=values[8],.margin=values[9]};
    return true;
}

static CFDictionaryRef rendered_metadata(const struct Stream *s, CFDictionaryRef original,
    const struct NezhaCameraPreviewFrame *frame, const struct hakios_camera_frame *actual, bool complete,
    const struct isp_stats *statistics)
{
    /* HakiOS frame_metadata converts the actual frame's line time and
     * Q8 gains to Apple's seconds, Q8 gains and frames/second. These key
     * values were read from original24A446 CMCaptureCore CFStrings.
     * The native065 header only carries these fields after real eleven-
     * byte readback, the two-frame delay and sensor digital unity. Do not
     * replace transition frames with requested exposure or stable flags. */
    /* These describe the completed RAW and our actual software crop, even
     * while sensor exposure is in transition. The original 24A446 source
     * and transfer node read RawSensorWidth/Height and per-output zoom when
     * constructing TotalSensorCropRect/PrimaryCaptureRect. Their units are
     * this binned sensor mode, not the scaled graph buffer's dimensions. */
    CFMutableDictionaryRef result=CFDictionaryCreateMutableCopy(NULL,0,original);
    if (!result) return NULL;
    const CFStringRef geometry_keys[]={CFSTR("RawSensorWidth"),CFSTR("RawSensorHeight"),
        CFSTR("ZoomFactor"),CFSTR("SecondaryScalerZoomFactor")};
    const double geometry_values[]={frame->width,frame->height,
        s->preview.processing.zoom,s->preview.processing.zoom};
    for (unsigned i=0;i<sizeof(geometry_keys)/sizeof(geometry_keys[0]);++i) {
        CFNumberRef value=number(geometry_values[i]);
        if (!value) { CFRelease(result);return NULL; }
        CFDictionarySetValue(result,geometry_keys[i],value);CFRelease(value);
    }
    CFNumberRef generation=number(s->generation);
    if (!generation) { CFRelease(result);return NULL; }
    CFDictionarySetValue(result,CFSTR("NezhaCameraGraphGeneration"),generation);
    CFRelease(generation);
    static unsigned geometry_reports[2];
    const unsigned report=__atomic_fetch_add(&geometry_reports[complete?1:0],1u,__ATOMIC_RELAXED);
    if (report<2 || (report && report%300==0))
        fprintf(stderr,"nezha-camera: rendered metadata RAW=%ux%u applied_zoom=%.9g sensor_fields_present=%u "
            "sample=%u mean=%.9g/%.9g/%.9g bright=%.9g cells=%u "
            "seq=%llu exposure=%u line_ns=%u gain=%u/%u frame=%u "
            "digital=%.9g wb=%.9g/%.9g/%.9g pending=%u asked=%u/%u/%u\n",
            frame->width,frame->height,(double)s->preview.processing.zoom,(unsigned)complete,
            report,statistics->mean[0],statistics->mean[1],statistics->mean[2],
            statistics->bright,statistics->cells,(unsigned long long)s->preview.completed_sequence,actual->exposure_lines,
            actual->line_ns,actual->gain,actual->gain_unity,actual->frame_lines,
            (double)s->preview.processing.digital,(double)s->preview.processing.wb[0],
            (double)s->preview.processing.wb[1],(double)s->preview.processing.wb[2],
            (unsigned)s->preview.processing.asked,s->preview.processing.asked_lines,
            s->preview.processing.asked_gain,s->preview.processing.asked_frame_lines);
    if (!complete || actual->margin!=10 || actual->gain_unity!=256 ||
        actual->gain<256 || actual->gain>actual->gain_max || actual->gain_max>65280 ||
        actual->frame_lines_min<=10 || actual->frame_lines<actual->frame_lines_min ||
        actual->frame_lines_max<actual->frame_lines || actual->frame_lines_max>0xffffff ||
        actual->exposure_lines>actual->frame_lines-actual->margin) return result;
    const CFStringRef keys[]={CFSTR("ExposureTime"),CFSTR("AGC"),CFSTR("sensorDGain"),
        CFSTR("ispDGain"),CFSTR("CurrentFrameRate")};
    const double values[]={actual->exposure_lines*(double)actual->line_ns/1e9,
        actual->gain*(256.0/actual->gain_unity),256,
        floor(s->preview.processing.digital*256.0+0.5),
        1e9/(actual->frame_lines*(double)actual->line_ns)};
    for (unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);++i) {
        CFNumberRef value=number(values[i]);
        if (!value) { CFRelease(result);return NULL; }
        CFDictionarySetValue(result,keys[i],value);CFRelease(value);
    }
    return result;
}

static NezhaFigStatus deliver_stream(struct Stream *s,
    const struct NezhaCameraPreviewFrame *frame,const uint8_t *raw,size_t bytes,
    uint64_t completed_time,CFDictionaryRef metadata)
{
    if (!s || !frame || frame->width!=4096 || frame->height!=3072 || frame->stride!=5120 ||
        frame->bayer!=3 || frame->black!=64 || frame->white!=1023 || bytes!=15728640 ||
        !raw || !completed_time || !metadata || CFGetTypeID(metadata)!=CFDictionaryGetTypeID()) return Invalid;
    __block NezhaFigStatus result=Unavailable;
    serial(s,^{
        if (!s->running || s->invalid) return;
        const unsigned generation=s->generation;
        s->preview.graph_generation=generation;
        CFTypeRef frame_sequence=CFDictionaryGetValue(metadata,CFSTR("FrameSequence"));
        double sequence=0;
        s->preview.completed_sequence=frame_sequence && CFGetTypeID(frame_sequence)==CFNumberGetTypeID() &&
            CFNumberGetValue(frame_sequence,kCFNumberDoubleType,&sequence) && isfinite(sequence) &&
            sequence>=1 && sequence<=UINT32_MAX ? (uint32_t)sequence:0;
        bool attempted=false;
        bool enabled=false;
        for (unsigned i=0;i<3;++i)
            enabled |= s->output[i].running && (i!=2 || s->stills);
        struct isp_stats statistics;
        if (!enabled || !nezha_camera_preview_measure(&s->preview,frame,raw,bytes,&statistics)) return;
        struct hakios_camera_frame actual={0};
        const bool complete=frame_controls(metadata,completed_time,&actual);
        if (s->backend.exposure && complete) {
            struct hakios_camera_command command;
            const struct isp previous=s->preview.processing;
            if (nezha_camera_preview_auto_exposure(&s->preview,&actual,&statistics,&command) &&
                s->backend.exposure(s->backend.context,&command)) {
                /* Retain AWB from this frame, restore only AE when
                 * the native queue rejects the request. */
                s->preview.processing.digital=previous.digital;
                s->preview.processing.asked=previous.asked;
                s->preview.processing.asked_lines=previous.asked_lines;
                s->preview.processing.asked_gain=previous.asked_gain;
                s->preview.processing.asked_frame_lines=previous.asked_frame_lines;
            }
        }
        CFDictionaryRef attached=rendered_metadata(s,metadata,frame,&actual,complete,&statistics);
        if (!attached) { result=Unavailable;return; }
        for (unsigned i=0;i<3 && s->running && s->generation==generation;++i) {
            if (!s->output[i].running || (i==2 && !s->stills)) continue;
            if (i==2) s->stills=0;
            NezhaFigStatus status=nezha_camera_graph_output_deliver(&s->output[i],&s->preview,frame,raw,bytes,completed_time,attached);
            /* HakiOS reports the first completed output and output_rate
             * after successful callbacks. These counters use the actual
             * completed frame clock, no new timer, IRQ trace or frame
             * reads. A reentrant Stop/Start cannot enter the next epoch. */
            if (!status && s->running && s->generation==generation) {
                s->last_time[i]=completed_time;
                if (!s->delivered[i]++) s->first_time[i]=completed_time;
                if (s->delivered[i]==1 || s->delivered[i]==32 || s->delivered[i]==64 ||
                    s->delivered[i]==128 || s->delivered[i]%300==0) report_output(s,i);
            }
            if (!attempted || (status && !result)) result=status;
            attempted=true;
            if (status && i==2 && s->running && s->generation==generation) s->stills=1;
        }
        CFRelease(attached);
    });return result;
}

NezhaFigStatus nezha_camera_fig_provider_deliver(NezhaFigObject o,
    const struct NezhaCameraPreviewFrame *frame,const uint8_t *raw,size_t bytes,
    uint64_t ticks,CFDictionaryRef metadata)
{
    struct Device *d=device_of(o);
    return d && d->stream ? deliver_stream(stream_of(d->stream),frame,raw,bytes,ticks,metadata) : Invalid;
}

NezhaFigStatus nezha_camera_fig_stream_enqueue(NezhaFigObject stream,
    const struct NezhaCameraPreviewFrame *frame,const uint8_t *raw,size_t bytes,
    uint64_t ticks,CFDictionaryRef metadata,void (*release)(void *),void *context)
{
    struct Stream *s=stream_of(stream);
    if (!s || !frame || !raw || !bytes || !ticks || !metadata || !release ||
        CFGetTypeID(metadata)!=CFDictionaryGetTypeID()) return Invalid;
    const unsigned generation=__atomic_load_n(&s->generation,__ATOMIC_ACQUIRE);
    const struct NezhaCameraPreviewFrame geometry=*frame;
    CFRetain(stream);CFRetain(metadata);
    dispatch_async(s->queue,^{
        if (s->generation==generation)
            (void)deliver_stream(s,&geometry,raw,bytes,ticks,metadata);
        CFRelease(metadata);CFRelease(stream);
        release(context);
    });
    return 0;
}
