/* SPDX-License-Identifier: MIT */
#include "NezhaCameraGraphOutput.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct Object { CFTypeID type; unsigned refs; };
struct __CVPixelBufferPool { struct Object object; };
struct __CVBuffer { struct Object object; bool rendered, metadata; };
struct __CFDictionary { struct Object object; };
struct __CFString { int value; };
static struct __CFString metadata_key;
static unsigned allocations, releases, delivered, high_water, live;
static bool pool_failure, render_failure;
CFTypeID CFGetTypeID(CFTypeRef v) { return ((const struct Object *)v)->type; }
CFTypeID CFDictionaryGetTypeID(void) { return 2; }
CFTypeID CVPixelBufferPoolGetTypeID(void) { return 1; }
CFTypeRef CFRetain(CFTypeRef v) { ++((struct Object *)v)->refs; return v; }
void CFRelease(CFTypeRef v)
{
    struct Object *object=(struct Object *)v;
    assert(object->refs);
    if (--object->refs) return;
    if (object->type==3) { ++releases; --live; }
    free(object);
}
CFMutableDictionaryRef CFDictionaryCreateMutableCopy(CFAllocatorRef a, CFIndex capacity, CFDictionaryRef original)
{
    assert(!a && !capacity && original->object.type==2);
    struct __CFDictionary *copy=malloc(sizeof(*copy));
    assert(copy); copy->object=(struct Object){2,1}; return copy;
}
CFStringRef nezha_test_cfstr(const char *name) { assert(!strcmp(name,"MetadataDictionary")); return &metadata_key; }
CVReturn CVPixelBufferPoolCreatePixelBuffer(CFAllocatorRef a, CVPixelBufferPoolRef pool, CVPixelBufferRef *out)
{
    assert(!a && pool->object.refs==2 && pool->object.type==1); ++allocations;
    *out=NULL;
    if (pool_failure) return -7;
    *out=calloc(1,sizeof(**out));assert(*out);(*out)->object=(struct Object){3,1};
    ++live; if (live>high_water) high_water=live;
    return 0;
}
CVReturn nezha_camera_preview_pixel_buffer(const struct NezhaCameraPreview *p,
    const struct NezhaCameraPreviewFrame *f, const uint8_t *raw, size_t bytes, CVPixelBufferRef pixel)
{
    assert(p && f && raw && bytes==10);pixel->rendered=!render_failure;
    return render_failure ? -8 : 0;
}
void CVBufferSetAttachment(CVPixelBufferRef pixel, CFStringRef key, CFTypeRef value, CVAttachmentMode mode)
{
    assert(pixel->rendered && key==&metadata_key && CFGetTypeID(value)==2 && mode==1);
    pixel->metadata=true;
}
CMTime CMClockMakeHostTimeFromSystemUnits(uint64_t timestamp)
{
    return (CMTime){(int64_t)timestamp,19200000,1,0};
}
int main(void)
{
    struct __CVPixelBufferPool pool={{1,1}};
    struct __CFDictionary metadata={{2,1}};
    struct NezhaCameraGraphOutput output={0};
    struct NezhaCameraPreview preview={0};
    struct NezhaCameraPreviewFrame frame={0};
    uint8_t raw[10]={0};
    NezhaCameraPixelHandler handler=^(CVPixelBufferRef pixel, CMTime time) {
        assert(pixel->rendered && pixel->metadata && time.timescale==19200000 && time.value==77);
        ++delivered;
    };
    assert(nezha_camera_graph_output_configure(&output,&pool,handler,true)==0 && pool.object.refs==2);
    assert(nezha_camera_graph_output_deliver(&output,&preview,&frame,raw,10,77,&metadata)==-12783 && !allocations);
    assert(nezha_camera_graph_output_start(&output)==0);
    for (unsigned i=0;i<7;++i) assert(nezha_camera_graph_output_deliver(&output,&preview,&frame,raw,10,77,&metadata)==0);
    assert(delivered==7 && live==3 && high_water==3 && releases==4 && output.count==3);
    nezha_camera_graph_output_stop(&output);
    assert(!live && releases==7 && output.count==0);
    assert(nezha_camera_graph_output_deliver(&output,&preview,&frame,raw,10,77,&metadata)==-12783 && allocations==7);
    assert(nezha_camera_graph_output_start(&output)==0);
    pool_failure=true;
    assert(nezha_camera_graph_output_deliver(&output,&preview,&frame,raw,10,77,&metadata)==-7 && !live && allocations==8);
    pool_failure=false;render_failure=true;
    assert(nezha_camera_graph_output_deliver(&output,&preview,&frame,raw,10,77,&metadata)==-8 && !live && delivered==7);
    render_failure=false;
    assert(nezha_camera_graph_output_configure(&output,&pool,handler,false)==0);
    assert(nezha_camera_graph_output_deliver(&output,&preview,&frame,raw,10,77,&metadata)==0 && !live && delivered==8);
    struct NezhaCameraGraphOutput *same=&output;
    handler=^(CVPixelBufferRef pixel, CMTime time) {
        assert(pixel->rendered && pixel->metadata && time.value==77);
        ++delivered;nezha_camera_graph_output_reset(same);
    };
    assert(nezha_camera_graph_output_configure(&output,&pool,handler,true)==0);
    assert(nezha_camera_graph_output_deliver(&output,&preview,&frame,raw,10,77,&metadata)==0);
    assert(!live && delivered==9 && !output.pool && !output.handler && pool.object.refs==1);
    puts("camera graph: render/unlock before callback, completed-frame time, three preview refs, stop gating and reentrant reset passed");
    return 0;
}
