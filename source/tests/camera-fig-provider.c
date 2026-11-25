/* SPDX-License-Identifier: MIT */
/* Actual provider exercised against small CF/CM objects. Dispatch and
 * Blocks use their bundled native runtimes; no camera hardware is modeled. */
#include "NezhaCameraFigProvider.h"
#include "NezhaCameraFormats.h"
#include "NezhaCameraFactory.h"
#include "NezhaCameraRenderer.h"
#include <Block.h>
#include <dispatch/dispatch.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { Array=1, Dictionary=2, String=3, Number=4, BooleanKind=5, Fig=6, Clock=7, Pool=8, Pixel=9, Callback=10, Format=11 };
struct Obj { CFTypeID type; unsigned refs; };
struct __CFString { struct Obj h; char text[128]; };
struct __CFNumber { struct Obj h; double value; };
struct __CFBoolean { struct Obj h; bool value; };
struct __CFArray { struct Obj h; CFIndex count; const void *values[32]; };
struct __CFDictionary { struct Obj h; unsigned count; const void *keys[32], *values[32]; };
struct OpaqueCMBaseObject { struct Obj h; const struct NezhaFigBaseClass *base; const void *class; unsigned char data[]; };
struct __CVPixelBufferPool { struct Obj h; bool fail; };
struct __CVBuffer { struct Obj h; bool locked; unsigned attachments; uint8_t data[36*6]; CFDictionaryRef metadata; };
struct opaqueCMFormatDescription { struct Obj h; uint32_t code;int32_t width,height; };
struct opaqueCMSampleBuffer { CVImageBufferRef image; CFDictionaryRef metadata; };
const CFArrayCallBacks kCFTypeArrayCallBacks={0};
const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks={0};
const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks={0};
static const struct __CFBoolean false_object={{BooleanKind,UINT32_MAX},false}, true_object={{BooleanKind,UINT32_MAX},true};
CFBooleanRef kCFBooleanFalse=&false_object, kCFBooleanTrue=&true_object;
static struct Obj clock_object={Clock,UINT32_MAX};
static struct __CFString literals[64];
static unsigned literal_count;
static unsigned begins, ends, live_objects, allocations, live_pixels, high_water, previews, secondary, stills;
static bool begin_fail, end_fail;
static bool backend_available;
static bool ae_reject;
static unsigned ae_requests;
static bool metadata_controls_expected;
/* The production renderer supplies this diagnostic callback. Darwin's
 * linker requires a definition even for the weak declaration; the graph
 * fixture validates the arguments without installing a system renderer. */
void nezha_camera_renderer_report_flow(uint64_t sequence,uint64_t ticks,unsigned generation)
{ assert(sequence && ticks && generation); }
static struct hakios_camera_command ae_command;
static NezhaFigStatus backend_exposure(void *p,const struct hakios_camera_command *c)
{ assert(p==&begins && c && c->command==HAKIOS_CAMERA_EXPOSURE && c->c>=3200);++ae_requests;ae_command=*c;return ae_reject ? -12781:0; }
struct QueuedRelease { dispatch_semaphore_t completed;unsigned count; };
static void queued_release(void *context)
{
    struct QueuedRelease *release=context;
    ++release->count;
    dispatch_semaphore_signal(release->completed);
}
static const struct __CFString color_keys[]={
    {{String,UINT32_MAX},"ColorPrimaries"},{{String,UINT32_MAX},"BT709"},
    {{String,UINT32_MAX},"TransferFunction"},{{String,UINT32_MAX},"sRGB"},
    {{String,UINT32_MAX},"YCbCrMatrix"},{{String,UINT32_MAX},"BT601"}};
const CFStringRef kCVImageBufferColorPrimariesKey=&color_keys[0];
const CFStringRef kCVImageBufferColorPrimaries_ITU_R_709_2=&color_keys[1];
const CFStringRef kCVImageBufferTransferFunctionKey=&color_keys[2];
const CFStringRef kCVImageBufferTransferFunction_sRGB=&color_keys[3];
const CFStringRef kCVImageBufferYCbCrMatrixKey=&color_keys[4];
const CFStringRef kCVImageBufferYCbCrMatrix_ITU_R_601_4=&color_keys[5];
extern void *_NSConcreteMallocBlock[32];
extern void *_NSConcreteStackBlock[32];
extern void *_NSConcreteGlobalBlock[32];
static bool is_block(CFTypeRef o)
{
    const void *isa=*(const void *const *)o;
    return isa==_NSConcreteMallocBlock || isa==_NSConcreteStackBlock || isa==_NSConcreteGlobalBlock;
}

static void *new(size_t bytes, unsigned type)
{
    struct Obj *o=calloc(1,bytes);assert(o);o->type=type;o->refs=1;++live_objects;return o;
}
CFTypeID CFGetTypeID(CFTypeRef o) { assert(o);return is_block(o) ? Callback : ((const struct Obj *)o)->type; }
CFTypeID CFArrayGetTypeID(void) { return Array; }
CFTypeID CFDictionaryGetTypeID(void) { return Dictionary; }
CFTypeID CFStringGetTypeID(void) { return String; }
CFTypeID CFNumberGetTypeID(void) { return Number; }
CFTypeID CFBooleanGetTypeID(void) { return BooleanKind; }
CFTypeID CVPixelBufferPoolGetTypeID(void) { return Pool; }
CFTypeRef CFRetain(CFTypeRef o)
{
    assert(o);if (is_block(o)) return Block_copy(o);
    struct Obj *p=(struct Obj *)o;assert(p->refs);
    if (p->refs!=UINT32_MAX) ++p->refs;return o;
}
void CFRelease(CFTypeRef o)
{
    assert(o);if (is_block(o)) { Block_release(o);return; }
    struct Obj *p=(struct Obj *)o;assert(p->refs);
    if (p->refs==UINT32_MAX || --p->refs) return;
    if (p->type==Array) {
        struct __CFArray *a=(void *)p;for (CFIndex i=0;i<a->count;++i) CFRelease(a->values[i]);
    } else if (p->type==Dictionary) {
        struct __CFDictionary *d=(void *)p;
        for (unsigned i=0;i<d->count;++i) { CFRelease(d->keys[i]);CFRelease(d->values[i]); }
    } else if (p->type==Fig) {
        struct OpaqueCMBaseObject *f=(void *)p;f->base->finalize(f);
    } else if (p->type==Pixel) {
        assert(!((CVPixelBufferRef)p)->locked);--live_pixels;
        if (((CVPixelBufferRef)p)->metadata) CFRelease(((CVPixelBufferRef)p)->metadata);
    }
    free(p);--live_objects;
}
CFStringRef nezha_test_cfstr(const char *text)
{
    for (unsigned i=0;i<literal_count;++i) if (!strcmp(literals[i].text,text)) return &literals[i];
    assert(literal_count<64 && strlen(text)<128);struct __CFString *s=&literals[literal_count++];
    s->h=(struct Obj){String,UINT32_MAX};strcpy(s->text,text);return s;
}
CFStringRef CFStringCreateWithCString(CFAllocatorRef a,const char *text,CFStringEncoding encoding)
{
    assert(!a && encoding==kCFStringEncodingUTF8 && strlen(text)<128);
    struct __CFString *s=new(sizeof(*s),String);strcpy(s->text,text);return s;
}
unsigned char CFStringGetCString(CFStringRef s,char *out,CFIndex bytes,CFStringEncoding encoding)
{
    assert(s->h.type==String && encoding==kCFStringEncodingUTF8);
    if ((size_t)bytes<=strlen(s->text)) return 0;strcpy(out,s->text);return 1;
}
CFNumberRef CFNumberCreate(CFAllocatorRef a,int type,const void *value)
{
    assert(!a && type==kCFNumberDoubleType);struct __CFNumber *n=new(sizeof(*n),Number);n->value=*(const double *)value;return n;
}
unsigned char CFNumberGetValue(CFNumberRef n,int type,void *out)
{ assert(n->h.type==Number && type==kCFNumberDoubleType);*(double *)out=n->value;return 1; }
unsigned char CFBooleanGetValue(CFBooleanRef b) { assert(b->h.type==BooleanKind);return b->value; }
CFArrayRef CFArrayCreate(CFAllocatorRef a,const void **values,CFIndex count,const CFArrayCallBacks *cb)
{
    (void)cb;assert(!a && count>=0 && count<=32);struct __CFArray *array=new(sizeof(*array),Array);array->count=count;
    for (CFIndex i=0;i<count;++i) array->values[i]=CFRetain(values[i]);return array;
}
CFIndex CFArrayGetCount(CFArrayRef a) { assert(a->h.type==Array);return a->count; }
const void *CFArrayGetValueAtIndex(CFArrayRef a,CFIndex i) { assert(a->h.type==Array && i>=0 && i<a->count);return a->values[i]; }
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a,CFIndex capacity,
    const CFDictionaryKeyCallBacks *k,const CFDictionaryValueCallBacks *v)
{ (void)k;(void)v;assert(!a && !capacity);return new(sizeof(struct __CFDictionary),Dictionary); }
static bool same(const void *a,const void *b)
{
    return a==b || (CFGetTypeID(a)==String && CFGetTypeID(b)==String && !strcmp(((CFStringRef)a)->text,((CFStringRef)b)->text));
}
CFTypeRef CFDictionaryGetValue(CFDictionaryRef d,const void *key)
{
    assert(d && d->h.type==Dictionary);
    for (unsigned i=0;i<d->count;++i) if (same(d->keys[i],key)) return d->values[i];return NULL;
}
void CFDictionarySetValue(CFMutableDictionaryRef d,const void *key,const void *value)
{
    assert(d->h.type==Dictionary);unsigned i=0;
    while (i<d->count && !same(d->keys[i],key)) ++i;
    assert(i<32);
    if (i==d->count) { d->keys[i]=CFRetain(key);++d->count; }
    else CFRelease(d->values[i]);
    d->values[i]=CFRetain(value);
}
CFDictionaryRef CFDictionaryCreate(CFAllocatorRef a,const void **keys,const void **values,CFIndex n,
    const CFDictionaryKeyCallBacks *k,const CFDictionaryValueCallBacks *v)
{
    CFMutableDictionaryRef d=CFDictionaryCreateMutable(a,0,k,v);
    for (CFIndex i=0;i<n;++i) CFDictionarySetValue(d,keys[i],values[i]);return d;
}
CFMutableDictionaryRef CFDictionaryCreateMutableCopy(CFAllocatorRef a,CFIndex n,CFDictionaryRef from)
{
    CFMutableDictionaryRef d=CFDictionaryCreateMutable(a,n,NULL,NULL);
    for (unsigned i=0;i<from->count;++i) CFDictionarySetValue(d,from->keys[i],from->values[i]);return d;
}
CMClockRef CMClockGetHostTimeClock(void) { return (CMClockRef)&clock_object; }
void *nezha_fig_capture24_storage(NezhaFigObject o) { return o ? o->data : NULL; }
static NezhaFigStatus fig_create(const void *table,const struct NezhaFigBaseClass *base,NezhaFigObject *out)
{
    struct OpaqueCMBaseObject *o=new(sizeof(*o)+base->storage_size,Fig);o->base=base;o->class=table;*out=o;return 0;
}
NezhaFigStatus nezha_fig_capture24_create_stream(NezhaFigAllocator a,const struct NezhaFigStreamVTable *t,NezhaFigObject *out)
{ (void)a;assert(t->stream->version==2 && t->base->version==1);return fig_create(t,t->base,out); }
NezhaFigStatus nezha_fig_capture24_create_device(NezhaFigAllocator a,const struct NezhaFigDeviceVTable *t,NezhaFigObject *out)
{ (void)a;assert(t->device->version==3 && t->base->version==1);return fig_create(t,t->base,out); }
size_t CVPixelBufferGetWidth(CVPixelBufferRef p) { (void)p;return 8; }
size_t CVPixelBufferGetHeight(CVPixelBufferRef p) { (void)p;return 6; }
uint32_t CVPixelBufferGetPixelFormatType(CVPixelBufferRef p) { (void)p;return 0x42475241; }
Boolean CVPixelBufferIsPlanar(CVPixelBufferRef p) { (void)p;return 0; }
size_t CVPixelBufferGetPlaneCount(CVPixelBufferRef p) { (void)p;return 0; }
size_t CVPixelBufferGetWidthOfPlane(CVPixelBufferRef p,size_t i) { (void)p;(void)i;abort(); }
size_t CVPixelBufferGetHeightOfPlane(CVPixelBufferRef p,size_t i) { (void)p;(void)i;abort(); }
size_t CVPixelBufferGetBytesPerRowOfPlane(CVPixelBufferRef p,size_t i) { (void)p;(void)i;abort(); }
void *CVPixelBufferGetBaseAddressOfPlane(CVPixelBufferRef p,size_t i) { (void)p;(void)i;abort(); }
size_t CVPixelBufferGetBytesPerRow(CVPixelBufferRef p) { assert(p->locked);return 36; }
void *CVPixelBufferGetBaseAddress(CVPixelBufferRef p) { assert(p->locked);return p->data; }
size_t CVPixelBufferGetDataSize(CVPixelBufferRef p) { return sizeof(p->data); }
CVReturn CVPixelBufferLockBaseAddress(CVPixelBufferRef p,CVPixelBufferLockFlags flags)
{ assert(!flags && !p->locked);p->locked=true;return 0; }
CVReturn CVPixelBufferUnlockBaseAddress(CVPixelBufferRef p,CVPixelBufferLockFlags flags)
{ assert(!flags && p->locked);p->locked=false;return 0; }
CVReturn CVPixelBufferPoolCreatePixelBuffer(CFAllocatorRef a,CVPixelBufferPoolRef pool,CVPixelBufferRef *out)
{
    assert(!a && pool->h.type==Pool);*out=NULL;++allocations;
    if (pool->fail) return -9;
    *out=new(sizeof(**out),Pixel);memset((*out)->data,0xcd,sizeof((*out)->data));
    ++live_pixels;if (live_pixels>high_water) high_water=live_pixels;return 0;
}
void CVBufferSetAttachment(CVPixelBufferRef p,CFStringRef key,CFTypeRef value,CVAttachmentMode mode)
{
    assert(!p->locked && mode==1);
    if (same(key,CFSTR("NezhaCameraCompletedRaw1x"))) {
        assert(CFGetTypeID(value)==CFBooleanGetTypeID() && CFBooleanGetValue(value));
        p->attachments|=8;return;
    }
    if (same(key,CFSTR("MetadataDictionary"))) {
        assert(CFGetTypeID(value)==Dictionary && CFDictionaryGetValue(value,CFSTR("FrameSequence")));
        assert(same(CFDictionaryGetValue(value,CFSTR("PortType")),CFSTR("PortTypeBack")));
        CFDictionaryRef time=CFDictionaryGetValue(value,CFSTR("OriginalPresentationTimeStamp"));
        double actual;
        assert(CFGetTypeID(time)==Dictionary &&
            CFNumberGetValue(CFDictionaryGetValue(time,CFSTR("value")),kCFNumberDoubleType,&actual) && actual==77);
        const char *names[]={"ExposureTime","AGC","sensorDGain","ispDGain","CurrentFrameRate","ZoomFactor"};
        const double expected[]={0.002,256,256,256,31.25,1};
        for (unsigned i=0;i<6;++i) {
            CFTypeRef number=CFDictionaryGetValue(value,CFSTR(names[i]));
            if (metadata_controls_expected || i==5) {
                assert(number && CFGetTypeID(number)==Number &&
                    CFNumberGetValue(number,kCFNumberDoubleType,&actual));
                assert(fabs(actual-expected[i])<1e-10);
            } else assert(!number);
        }
        const char *geometry_names[]={"RawSensorWidth","RawSensorHeight","SecondaryScalerZoomFactor"};
        const double geometry_values[]={4096,3072,1};
        for (unsigned i=0;i<3;++i) {
            CFTypeRef field=CFDictionaryGetValue(value,CFSTR(geometry_names[i]));
            assert(field && CFGetTypeID(field)==Number &&
                CFNumberGetValue(field,kCFNumberDoubleType,&actual) && actual==geometry_values[i]);
        }
        assert(!CFDictionaryGetValue(value,CFSTR("AEStable")) &&
            !CFDictionaryGetValue(value,CFSTR("AWBStable")) &&
            !CFDictionaryGetValue(value,CFSTR("AFStatus")));
        p->metadata=CFRetain(value);
        p->attachments|=4;
    } else if (key==kCVImageBufferColorPrimariesKey) {
        assert(value==kCVImageBufferColorPrimaries_ITU_R_709_2);p->attachments|=1;
    } else if (key==kCVImageBufferTransferFunctionKey) {
        assert(value==kCVImageBufferTransferFunction_sRGB);p->attachments|=2;
    } else abort();
}
CMTime CMClockMakeHostTimeFromSystemUnits(uint64_t time) { return (CMTime){(int64_t)time,19200000,1,0}; }
void CFDictionaryApplyFunction(CFDictionaryRef d,void (*apply)(const void *,const void *,void *),void *context)
{for(unsigned i=0;i<d->count;++i)apply(d->keys[i],d->values[i],context);}
CVImageBufferRef CMSampleBufferGetImageBuffer(CMSampleBufferRef sample) {return sample->image;}
CFTypeRef CVBufferCopyAttachment(CVImageBufferRef pixel,CFStringRef key,CVAttachmentMode *mode)
{assert(!mode && same(key,CFSTR("MetadataDictionary")));return pixel->metadata?CFRetain(pixel->metadata):NULL;}
CFTypeRef CMGetAttachment(CMSampleBufferRef sample,CFStringRef key,CMAttachmentMode *mode)
{assert(!mode && same(key,CFSTR("MetadataDictionary")));return sample->metadata;}
void CMSetAttachment(CMSampleBufferRef sample,CFStringRef key,CFTypeRef value,CMAttachmentMode mode)
{
 assert(mode==1 && same(key,CFSTR("MetadataDictionary")));
 if(sample->metadata)CFRelease(sample->metadata);
 sample->metadata=CFRetain(value);
}
CFDictionaryRef CMTimeCopyAsDictionary(CMTime time,CFAllocatorRef allocator)
{
    assert(!allocator && time.timescale==19200000);
    double value=(double)time.value;
    CFNumberRef number=CFNumberCreate(NULL,kCFNumberDoubleType,&value);
    const void *keys[]={CFSTR("value")},*values[]={number};
    CFDictionaryRef result=CFDictionaryCreate(NULL,keys,values,1,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
    CFRelease(number);return result;
}
int32_t CMVideoFormatDescriptionCreate(CFAllocatorRef a,uint32_t code,int32_t width,int32_t height,
    CFDictionaryRef extensions,CMVideoFormatDescriptionRef *out)
{
    assert(!a && !extensions);
    struct opaqueCMFormatDescription *d=new(sizeof(*d),Format);
    d->code=code;d->width=width;d->height=height;*out=d;return 0;
}

static CFDictionaryRef one(const char *key,const void *value)
{
    const void *keys[]={CFSTR(key)},*values[]={value};
    return CFDictionaryCreate(NULL,keys,values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
}
static CFDictionaryRef nest(const char *output,const char *key,const void *value)
{
    CFDictionaryRef inner=one(key,value),outer=one(output,inner);CFRelease(inner);return outer;
}
static void assert_pixel(CVPixelBufferRef pixel,CMTime time)
{
    assert(!pixel->locked && pixel->attachments==15 && time.value==77 && time.timescale==19200000);
    for (unsigned row=0;row<6;++row) {
        for (unsigned x=0;x<32;++x) assert(pixel->data[row*36+x]==255);
        for (unsigned x=32;x<36;++x) assert(pixel->data[row*36+x]==0xcd);
    }
}
static const struct NezhaFigStreamVTable *attachment_methods;
static NezhaFigObject attachment_stream;
static void check_sample_metadata(CVPixelBufferRef pixel)
{
 struct opaqueCMSampleBuffer sample={.image=pixel};
 assert(attachment_methods->stream->add_attachments(attachment_stream,&sample,NULL)==0 && sample.metadata);
 assert(same(CFDictionaryGetValue(sample.metadata,CFSTR("PortType")),CFSTR("PortTypeBack")));
 assert(CFDictionaryGetValue(sample.metadata,CFSTR("OriginalPresentationTimeStamp")));
 CFRelease(sample.metadata);
 sample.metadata=one("GraphField",kCFBooleanTrue);
 assert(attachment_methods->stream->add_attachments(attachment_stream,&sample,NULL)==0);
 assert(CFDictionaryGetValue(sample.metadata,CFSTR("GraphField"))==kCFBooleanTrue &&
        CFDictionaryGetValue(sample.metadata,CFSTR("FrameSequence")));
 CFRelease(sample.metadata);
 sample.metadata=one("PortType",CFSTR("graph-preserved"));
 assert(attachment_methods->stream->add_attachments(attachment_stream,&sample,NULL)==0);
 assert(same(CFDictionaryGetValue(sample.metadata,CFSTR("PortType")),CFSTR("graph-preserved")));
 CFRelease(sample.metadata);
}
static NezhaFigStatus backend_begin(void *p) { assert(p==&begins);++begins;return begin_fail ? -7 : 0; }
static NezhaFigStatus backend_end(void *p) { assert(p==&begins);++ends;return end_fail ? -8 : 0; }
static bool backend_ready(void *p) { assert(p==&begins);return backend_available; }

int main(int argc,char **argv)
{
    const bool ae_mode=argc==2 && !strcmp(argv[1],"ae");
    struct NezhaCameraFigBackend backend={.context=&begins,.begin=backend_begin,.end=backend_end,.exposure=ae_mode ? backend_exposure:NULL};
    NezhaFigObject unavailable=(void *)1;
    assert(nezha_camera_device_create(NULL,NULL,&unavailable)==-12783 && !unavailable);
    assert(nezha_camera_factory_bind(&backend,NULL)==-12780);
    assert(nezha_camera_factory_bind(&backend,backend_ready)==0);
    assert(nezha_camera_factory_bind(&backend,backend_ready)==-12781);
    assert(nezha_camera_device_create(NULL,NULL,&unavailable)==-12783 && !unavailable);
    backend_available=true;
    CFArrayRef formats=nezha_camera_main_formats();assert(formats && CFArrayGetCount(formats)==1);
    CFDictionaryRef format=CFArrayGetValueAtIndex(formats,0);
    const struct opaqueCMFormatDescription *description=CFDictionaryGetValue(format,CFSTR("FormatDescription"));
    assert(description && description->code==0x34323066 && description->width==4096 && description->height==3072);
    double value_number=0;
    assert(CFNumberGetValue(CFDictionaryGetValue(format,CFSTR("VideoMaxFrameRate")),kCFNumberDoubleType,&value_number) && value_number==30);
    assert(CFBooleanGetValue(CFDictionaryGetValue(format,CFSTR("VideoIsBinned"))));
    CFDictionaryRef supported_outputs=CFDictionaryGetValue(format,CFSTR("SupportedOutputs"));
    CFDictionaryRef primary_format=CFDictionaryGetValue(supported_outputs,CFSTR("PrimaryScaler"));
    assert(primary_format && CFDictionaryGetValue(supported_outputs,CFSTR("SecondaryScaler")) &&
        CFDictionaryGetValue(supported_outputs,CFSTR("StillImage")));
    assert(CFNumberGetValue(CFDictionaryGetValue(primary_format,CFSTR("NativeWidth")),kCFNumberDoubleType,&value_number) && value_number==4096);
    for (unsigned i=0;i<3;++i) {
        CFStringRef output_name=i==0 ? CFSTR("PrimaryScaler"):
            i==1 ? CFSTR("SecondaryScaler"):CFSTR("StillImage");
        CFDictionaryRef output_format=CFDictionaryGetValue(supported_outputs,output_name);
        CFArrayRef pixels=CFDictionaryGetValue(output_format,CFSTR("SupportedPixelFormats"));
        const double expected_pixels[]={0x42475241,0x34323066,0x34323076};
        assert(CFArrayGetCount(pixels)==3);
        for (unsigned j=0;j<3;++j)
            assert(CFNumberGetValue(CFArrayGetValueAtIndex(pixels,j),kCFNumberDoubleType,&value_number) && value_number==expected_pixels[j]);
    }
    NezhaFigObject device=NULL;
    assert(nezha_camera_device_create(NULL,CFSTR("Default"),&device)==0);
    NezhaFigObject rejected=(void *)1;
    assert(nezha_camera_device_create(NULL,(NezhaFigString)kCFBooleanTrue,&rejected)==-12780 && !rejected);
    CFRelease(formats);
    const struct NezhaFigDeviceVTable *dt=device->class;
    CFArrayRef streams=NULL;
    assert(dt->base->copy_property(device,CFSTR("StreamArray"),NULL,&streams)==0 && CFArrayGetCount(streams)==1);
    NezhaFigObject stream=(NezhaFigObject)CFArrayGetValueAtIndex(streams,0);
    const struct NezhaFigStreamVTable *st=stream->class;
    attachment_methods=st;attachment_stream=stream;
    CFTypeRef value=NULL;
    assert(st->base->copy_property(stream,CFSTR("SupportedPropertiesDictionary"),NULL,&value)==0);
    CFDictionaryRef zoom_property=CFDictionaryGetValue((CFDictionaryRef)value,CFSTR("ZoomFactor"));
    assert(zoom_property && same(CFDictionaryGetValue(zoom_property,CFSTR("PropertyType")),CFSTR("Number")) &&
        same(CFDictionaryGetValue(zoom_property,CFSTR("ReadWriteStatus")),CFSTR("ReadWrite")));
    CFRelease(value);
    double requested_zoom=1;
    CFNumberRef zoom=CFNumberCreate(NULL,kCFNumberDoubleType,&requested_zoom);
    assert(st->base->set_property(stream,CFSTR("ZoomFactor"),zoom)==0);CFRelease(zoom);
    assert(st->base->copy_property(stream,CFSTR("ZoomFactor"),NULL,&value)==0 &&
        CFNumberGetValue(value,kCFNumberDoubleType,&value_number) && value_number==1);CFRelease(value);
    requested_zoom=2;zoom=CFNumberCreate(NULL,kCFNumberDoubleType,&requested_zoom);
    assert(st->base->set_property(stream,CFSTR("ZoomFactor"),zoom)==-12782);CFRelease(zoom);
    assert(st->base->copy_property(stream,CFSTR("FormatDescription"),NULL,&value)==0 &&
        CFGetTypeID(value)==Format && ((CMVideoFormatDescriptionRef)value)->width==4096);
    CFRelease(value);
    assert(st->base->copy_property(stream,CFSTR("PortType"),NULL,&value)==0 && !strcmp(((CFStringRef)value)->text,"PortTypeBack"));CFRelease(value);
    assert(st->base->copy_property(stream,CFSTR("UniqueID"),NULL,&value)==0 &&
        !strcmp(((CFStringRef)value)->text,"com.apple.avfoundation.avcapturedevice.built-in_video:0"));CFRelease(value);
    assert(st->base->copy_property(stream,CFSTR("ModelID"),NULL,&value)==0 &&
        !strcmp(((CFStringRef)value)->text,"nezha.ovx10500u.main"));CFRelease(value);
    assert(st->base->copy_property(stream,CFSTR("NonLocalizedName"),NULL,&value)==0 &&
        !strcmp(((CFStringRef)value)->text,"Back Camera"));CFRelease(value);
    assert(dt->device->request_streams(device,(NezhaFigArray)streams,NULL)==0 && begins==1);
    assert(dt->device->request_streams(device,(NezhaFigArray)streams,NULL)==0 && begins==1);
    assert(dt->device->relinquish_streams(device,(NezhaFigArray)streams,NULL)==0 && ends==0);
    assert(dt->device->request_streams(device,(NezhaFigArray)streams,NULL)==0 && begins==1 && ends==0);
    assert(dt->device->relinquish_streams(device,(NezhaFigArray)streams,NULL)==0 && ends==0);
    uint8_t *raw=malloc(15728640);assert(raw);memset(raw,255,15728640);
    const struct NezhaCameraPreviewFrame frame={4096,3072,5120,3,64,1023};
    double sequence=5;
    CFNumberRef n=CFNumberCreate(NULL,kCFNumberDoubleType,&sequence);
    CFDictionaryRef metadata=one("FrameSequence",n);CFRelease(n);
    assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,metadata)==-12783);
    assert(st->stream->start(stream)==0 && begins==1);
    assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,metadata)==-12783 && !allocations);
    assert(st->base->set_property(stream,CFSTR("StillImageCaptureNow"),metadata)==-12783);
    assert(st->base->set_property(stream,CFSTR("StillImageCaptureNow"),NULL)==-12783);
    assert(st->stream->stop(stream)==0 && ends==1);
    assert(st->stream->stop(stream)==0 && ends==1);
    begin_fail=true;
    assert(st->stream->start(stream)==-7 && begins==2);
    assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,metadata)==-12783);
    begin_fail=false;
    assert(st->stream->start(stream)==0 && begins==3);
    double unsupported_fps=60;
    n=CFNumberCreate(NULL,kCFNumberDoubleType,&unsupported_fps);
    assert(st->base->set_property(stream,CFSTR("MaximumFrameRate"),n)==-12782);CFRelease(n);

    struct __CVPixelBufferPool *pool=new(sizeof(*pool),Pool),*still_pool=new(sizeof(*still_pool),Pool);
    NezhaCameraPixelHandler primary_handler=^(CVPixelBufferRef pixel,CMTime time) { assert_pixel(pixel,time);++previews; };
    NezhaCameraPixelHandler second_handler=^(CVPixelBufferRef pixel,CMTime time) { assert_pixel(pixel,time);++secondary; };
    NezhaCameraPixelHandler still_handler=^(CVPixelBufferRef pixel,CMTime time) {
        assert_pixel(pixel,time);check_sample_metadata(pixel);++stills;
    };
    CFDictionaryRef configs=nest("PrimaryScaler","BufferPool",pool),handlers=nest("PrimaryScaler","PixelBufferHandler",primary_handler);
    CFDictionaryRef enabled=one("PrimaryScaler",kCFBooleanTrue);
    assert(st->base->set_property(stream,CFSTR("VideoOutputConfigurations"),configs)==0);
    assert(st->base->set_property(stream,CFSTR("VideoOutputHandlers"),handlers)==0);
    assert(st->base->set_property(stream,CFSTR("VideoOutputsEnabled"),enabled)==0);
    value=NULL;assert(st->base->copy_property(stream,CFSTR("VideoOutputConfigurations"),NULL,&value)==0 &&
        CFDictionaryGetValue(value,CFSTR("PrimaryScaler"))==CFDictionaryGetValue(configs,CFSTR("PrimaryScaler")));CFRelease(value);
    assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728639,77,metadata)==-12780 && !allocations);
    assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,0,metadata)==-12780 && !allocations);
    assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,(CFDictionaryRef)kCFBooleanTrue)==-12780 && !allocations);
    for (unsigned i=0;i<7;++i) assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,metadata)==0);
    assert(previews==7 && live_pixels==3 && high_water==3);
    if(ae_mode) {
        assert(ae_requests==0); // sequence-only metadata does not drive sensor writes
        const char *names[]={"FrameSequence","ExposureLines","LineNanoseconds","AnalogGainQ8","GainUnity",
            "GainMaximum","FrameLengthLines","MinimumFrameLengthLines","MaximumFrameLengthLines","ExposureMargin"};
        const double control_values[]={5,200,10000,256,256,65280,3200,3200,6400,10};
        CFMutableDictionaryRef controls=CFDictionaryCreateMutable(NULL,0,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        for(unsigned i=0;i<10;++i){CFNumberRef number=CFNumberCreate(NULL,kCFNumberDoubleType,&control_values[i]);CFStringRef key=CFStringCreateWithCString(NULL,names[i],kCFStringEncodingUTF8);CFDictionarySetValue(controls,key,number);CFRelease(number);CFRelease(key);}
        metadata_controls_expected=true;
        ae_reject=true;
        assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,controls)==0 && ae_requests==1);
        assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,controls)==0 && ae_requests==2); // rejection does not leave asked=true
        ae_reject=false;
        assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,controls)==0 && ae_requests==3);
        assert(ae_command.a<200 && ae_command.b==256);
        assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,controls)==0 && ae_requests==3); // pending actualcontrols differs
        assert(!CFDictionaryGetValue(controls,CFSTR("ExposureTime")) &&
            !CFDictionaryGetValue(controls,CFSTR("RawSensorWidth")) &&
            !CFDictionaryGetValue(controls,CFSTR("ZoomFactor"))); // caller metadata unchanged
        metadata_controls_expected=false;
        CFMutableDictionaryRef partial=CFDictionaryCreateMutableCopy(NULL,0,controls);
        double zero=0;
        CFNumberRef missing=CFNumberCreate(NULL,kCFNumberDoubleType,&zero);
        CFDictionarySetValue(partial,CFSTR("LineNanoseconds"),missing);CFRelease(missing);
        assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,partial)==0 && ae_requests==3);
        CFRelease(partial);
        assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,metadata)==0 && ae_requests==3);
        assert(st->stream->stop(stream)==0);
        CFRelease(controls);CFRelease(configs);CFRelease(handlers);CFRelease(enabled);CFRelease(pool);CFRelease(still_pool);
        free(raw);CFRelease(metadata);CFRelease(streams);CFRelease(device);
        assert(!live_objects && !live_pixels);
        puts("Camera graph AE: actual frame exposure units, pending requested values excluded, transition fields absent and complete release passed");
        return 0;
    }

    assert(dt->device->relinquish_streams(device,(NezhaFigArray)streams,NULL)==0 && ends==1);
    assert(dt->device->request_streams(device,(NezhaFigArray)streams,NULL)==0 && begins==3);
    assert(st->stream->start(stream)==0 && begins==3 && live_pixels==3);

    /* An invalid new configuration must leave the working output intact. */
    CFDictionaryRef bad=nest("PrimaryScaler","BufferPool",kCFBooleanTrue);
    assert(st->base->set_property(stream,CFSTR("VideoOutputConfigurations"),bad)==-12780);CFRelease(bad);
    assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,metadata)==0 && previews==8);
    bad=one("PrimaryScaler",metadata);
    assert(st->base->set_property(stream,CFSTR("VideoOutputsEnabled"),bad)==-12780);CFRelease(bad);

    CFDictionaryRef off=one("PrimaryScaler",kCFBooleanFalse);
    assert(st->base->set_property(stream,CFSTR("VideoOutputsEnabled"),off)==0 && !live_pixels);CFRelease(off);
    assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,metadata)==-12783);
    assert(st->base->set_property(stream,CFSTR("VideoOutputsEnabled"),enabled)==0);
    pool->fail=true;
    assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,metadata)==-9 && previews==8);
    pool->fail=false;

    CFMutableDictionaryRef all_configs=CFDictionaryCreateMutableCopy(NULL,0,configs),all_handlers=CFDictionaryCreateMutableCopy(NULL,0,handlers);
    CFMutableDictionaryRef all_enabled=CFDictionaryCreateMutableCopy(NULL,0,enabled);
    CFDictionaryRef inner=one("BufferPool",still_pool);CFDictionarySetValue(all_configs,CFSTR("StillImage"),inner);CFRelease(inner);
    inner=one("BufferPool",pool);CFDictionarySetValue(all_configs,CFSTR("SecondaryScaler"),inner);CFRelease(inner);
    inner=one("PixelBufferHandler",still_handler);CFDictionarySetValue(all_handlers,CFSTR("StillImage"),inner);CFRelease(inner);
    inner=one("PixelBufferHandler",second_handler);CFDictionarySetValue(all_handlers,CFSTR("SecondaryScaler"),inner);CFRelease(inner);
    CFDictionarySetValue(all_enabled,CFSTR("SecondaryScaler"),kCFBooleanTrue);
    /* The real24A446 still preparation updates only that output. Neither
     * adding StillImage nor a later SecondaryScaler update may discard the
     * existing primary pool/handler/enabled bit or its three held buffers. */
    const unsigned held_before_update=live_pixels;
    inner=nest("StillImage","BufferPool",still_pool);
    assert(st->base->set_property(stream,CFSTR("VideoOutputConfigurations"),inner)==0);CFRelease(inner);
    inner=nest("StillImage","PixelBufferHandler",still_handler);
    assert(st->base->set_property(stream,CFSTR("VideoOutputHandlers"),inner)==0);CFRelease(inner);
    inner=nest("SecondaryScaler","BufferPool",pool);
    assert(st->base->set_property(stream,CFSTR("VideoOutputConfigurations"),inner)==0);CFRelease(inner);
    inner=nest("SecondaryScaler","PixelBufferHandler",second_handler);
    assert(st->base->set_property(stream,CFSTR("VideoOutputHandlers"),inner)==0);CFRelease(inner);
    inner=one("SecondaryScaler",kCFBooleanTrue);
    assert(st->base->set_property(stream,CFSTR("VideoOutputsEnabled"),inner)==0);CFRelease(inner);
    assert(live_pixels==held_before_update);
    value=NULL;assert(st->base->copy_property(stream,CFSTR("VideoOutputConfigurations"),NULL,&value)==0);
    assert(CFDictionaryGetValue(value,CFSTR("PrimaryScaler"))==CFDictionaryGetValue(configs,CFSTR("PrimaryScaler")));
    assert(CFDictionaryGetValue(CFDictionaryGetValue(value,CFSTR("StillImage")),CFSTR("BufferPool"))==still_pool);
    assert(CFDictionaryGetValue(CFDictionaryGetValue(value,CFSTR("SecondaryScaler")),CFSTR("BufferPool"))==pool);CFRelease(value);
    value=NULL;assert(st->base->copy_property(stream,CFSTR("VideoOutputsEnabled"),NULL,&value)==0);
    assert(CFDictionaryGetValue(value,CFSTR("PrimaryScaler"))==kCFBooleanTrue &&
        CFDictionaryGetValue(value,CFSTR("SecondaryScaler"))==kCFBooleanTrue);CFRelease(value);
    /* Original source preparation sends only StillImage.RetainedBufferCount.
     * Keep the graph pool and both scaler outputs when applying that delta. */
    double retained=4;n=CFNumberCreate(NULL,kCFNumberDoubleType,&retained);
    inner=nest("StillImage","RetainedBufferCount",n);
    assert(st->base->set_property(stream,CFSTR("VideoOutputConfigurations"),inner)==0);CFRelease(inner);
    value=NULL;assert(st->base->copy_property(stream,CFSTR("VideoOutputConfigurations"),NULL,&value)==0);
    CFDictionaryRef updated_still=CFDictionaryGetValue(value,CFSTR("StillImage"));
    assert(CFDictionaryGetValue(updated_still,CFSTR("BufferPool"))==still_pool &&
        CFDictionaryGetValue(updated_still,CFSTR("RetainedBufferCount"))==n);
    assert(CFDictionaryGetValue(value,CFSTR("PrimaryScaler"))==CFDictionaryGetValue(configs,CFSTR("PrimaryScaler")));
    CFRelease(value);CFRelease(n);assert(live_pixels==held_before_update);
    /* Stock24A446 converts an empty options dictionary to NULL. Keep its
     * one-frame command valid, while rejecting other CF kinds and preserving
     * the outstanding request through allocation failure and later retry. */
    assert(st->base->set_property(stream,CFSTR("StillImageCaptureNow"),kCFBooleanTrue)==-12780);
    assert(st->base->set_property(stream,CFSTR("StillImageCaptureNow"),NULL)==0);
    assert(st->base->set_property(stream,CFSTR("StillImageCaptureNow"),NULL)==-12781);
    assert(st->base->set_property(stream,CFSTR("StillImageCaptureNow"),metadata)==-12781);
    still_pool->fail=true;
    assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,metadata)==-9 && !stills);
    still_pool->fail=false;
    assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,metadata)==0 && stills==1);
    assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,metadata)==0 && stills==1 && secondary==3);
    assert(st->base->set_property(stream,CFSTR("StillImageCaptureNow"),metadata)==0);
    assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,metadata)==0 && stills==2 && secondary==4);

    struct QueuedRelease released={.completed=dispatch_semaphore_create(0)};
    struct QueuedRelease *release_context=&released;
    unsigned queued_previews=previews,queued_secondary=secondary;
    assert(nezha_camera_fig_stream_enqueue(stream,&frame,raw,15728640,77,metadata,queued_release,&released)==0);
    assert(dispatch_semaphore_wait(released.completed,dispatch_time(DISPATCH_TIME_NOW,2000000000))==0);
    assert(released.count==1 && previews==queued_previews+1 && secondary==queued_secondary+1 && stills==2);

    /* A graph handler may synchronously Stop/Start on the same queue.
     * Its current frame belongs to the old generation, including scalers
     * visited later by the provider's loop. */
    NezhaCameraPixelHandler restart_handler=^(CVPixelBufferRef pixel,CMTime time) {
        assert_pixel(pixel,time);++previews;
        assert(nezha_camera_fig_stream_enqueue(stream,&frame,raw,15728640,77,metadata,queued_release,release_context)==0);
        assert(st->stream->stop(stream)==0);
        assert(st->stream->start(stream)==0);
    };
    inner=one("PixelBufferHandler",restart_handler);CFDictionarySetValue(all_handlers,CFSTR("PrimaryScaler"),inner);CFRelease(inner);
    assert(st->base->set_property(stream,CFSTR("VideoOutputHandlers"),all_handlers)==0);
    unsigned before_secondary=secondary;
    assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,metadata)==0 && secondary==before_secondary && !live_pixels);
    unsigned after_restart=previews;
    assert(dispatch_semaphore_wait(released.completed,dispatch_time(DISPATCH_TIME_NOW,2000000000))==0);
    assert(released.count==2 && previews==after_restart && secondary==before_secondary && !live_pixels);
    dispatch_release(released.completed);
    assert(begins==4 && ends==2);
    end_fail=true;
    assert(st->stream->stop(stream)==-8 && ends==3);
    assert(nezha_camera_fig_provider_deliver(device,&frame,raw,15728640,77,metadata)==-12783);
    end_fail=false;
    assert(st->stream->stop(stream)==0 && ends==4);
    assert(dt->base->invalidate(device)==0);
    assert(st->stream->start(stream)==0 && begins==5);
    CFRelease(device);device=NULL; /* the retained stream survives its device */
    assert(st->stream->start(stream)==0 && begins==5);
    assert(st->base->invalidate(stream)==0 && ends==5);
    assert(st->stream->start(stream)==-12783 && begins==5);
    assert(st->base->set_property(stream,CFSTR("StillImageCaptureNow"),metadata)==-12783);
    assert(st->base->set_property(stream,CFSTR("StillImageCaptureNow"),NULL)==-12783);
    CFRelease(all_configs);CFRelease(all_handlers);CFRelease(all_enabled);
    CFRelease(configs);CFRelease(handlers);CFRelease(enabled);CFRelease(pool);CFRelease(still_pool);
    CFRelease(metadata);CFRelease(streams);
    free(raw);assert(!live_objects && !live_pixels);
    puts("camera Fig provider: ready-only factory, measured format, real RAW-to-pool callbacks, still retry, restart and complete release passed; payload-free and dictionary still requests preserve busy/invalid/stopped gates");
    return 0;
}
