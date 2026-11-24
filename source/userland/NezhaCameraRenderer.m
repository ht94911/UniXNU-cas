/* SPDX-License-Identifier: MIT */
/* Choose the original uncompressed transfer path when the M2M scaler has no
 * usable capabilities. Do not fabricate scaler limits or replace its methods. */
#include "NezhaCameraRenderer.h"
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreGraphics/CGGeometry.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach_time.h>
#include <mach-o/loader.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <ptrauth.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {double width,height;} RendererSize;
typedef struct {double x,y;} RendererPoint;
typedef struct {RendererPoint origin;RendererSize size;} RendererRect;
typedef struct {int32_t width,height;} RendererDimensions;
typedef struct OpaqueVTPixelRotationSession *RendererRotationSession;
static IMP original_supports,original_initializer,original_transfer,original_border;
static IMP original_scale,original_oriented_scale;
static IMP original_preview_sink;
static IMP original_input_rect;
static IMP original_pixel_transfer_node;
static IMP original_preview_stitcher;
static IMP original_node_output;
static bool capture_seen,installed,preview_sink_installed,input_rect_installed;
static bool pixel_transfer_node_installed;
static bool preview_stitcher_installed;
static bool node_output_installed;
static Class pixel_transfer_node_class;
static Class preview_sink_class,preview_stitcher_class;
static Class camera_source_class;
static intptr_t capture_slide;
static const uint8_t uuid[16]={0x99,0x5f,0xc2,0x2b,0x41,0xbd,0x37,0xb4,0x81,0x2f,0x26,0x86,0xb8,0x1c,0x26,0x94};
/* Keep the existing final-input stderr bounded, including restarted streams
 * and late inputs. Camera146 exhausted its first8 before the second stream.
 * These 25 active pixels and the original call's duration are observations;
 * they do not change attachments, pixels, timestamps or delivery. */
struct PreviewPixels {
    CVReturn lock_status,unlock_status;
    unsigned count,nonzero,minimum,maximum;
    uint8_t luma[25],u,v;
};
static struct PreviewPixels preview_pixels(CVPixelBufferRef image)
{
    struct PreviewPixels pixels={.lock_status=kCVReturnInvalidArgument,
        .unlock_status=kCVReturnInvalidArgument};
    if (!image) return pixels;
    const size_t width=CVPixelBufferGetWidth(image),height=CVPixelBufferGetHeight(image);
    const uint32_t format=CVPixelBufferGetPixelFormatType(image);
    if (!width || !height) return pixels;
    const bool nv12=format==kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ||
        format==kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    if (!nv12 && format!=kCVPixelFormatType_32BGRA) return pixels;
    pixels.lock_status=CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
    if (pixels.lock_status) return pixels;
    const uint8_t *base=NULL,*chroma=NULL;
    size_t row=0,chroma_row=0,chroma_width=0,chroma_height=0;
    if (nv12 && CVPixelBufferIsPlanar(image) && CVPixelBufferGetPlaneCount(image)==2 &&
        CVPixelBufferGetWidthOfPlane(image,0)==width &&
        CVPixelBufferGetHeightOfPlane(image,0)==height) {
        base=CVPixelBufferGetBaseAddressOfPlane(image,0);
        row=CVPixelBufferGetBytesPerRowOfPlane(image,0);
        chroma=CVPixelBufferGetBaseAddressOfPlane(image,1);
        chroma_row=CVPixelBufferGetBytesPerRowOfPlane(image,1);
        chroma_width=CVPixelBufferGetWidthOfPlane(image,1);
        chroma_height=CVPixelBufferGetHeightOfPlane(image,1);
    } else if (!nv12 && !CVPixelBufferIsPlanar(image) && width<=SIZE_MAX/4) {
        base=CVPixelBufferGetBaseAddress(image);row=CVPixelBufferGetBytesPerRow(image);
    }
    if (base && row<=SIZE_MAX/height && row>=width*(nv12?1:4)) {
        pixels.minimum=255;
        for (unsigned gy=0;gy<5;++gy) for (unsigned gx=0;gx<5;++gx) {
            const size_t y=((height-1)/4)*gy+((height-1)%4)*gy/4;
            const size_t x=((width-1)/4)*gx+((width-1)%4)*gx/4;
            const uint8_t *at=base+y*row+x*(nv12?1:4);
            const uint8_t luma=nv12?at[0]:(uint8_t)((77u*at[2]+150u*at[1]+29u*at[0])>>8);
            pixels.luma[pixels.count++]=luma;
            pixels.nonzero+=luma!=0;
            if (luma<pixels.minimum) pixels.minimum=luma;
            if (luma>pixels.maximum) pixels.maximum=luma;
        }
        if (chroma && chroma_width && chroma_height && chroma_width<=SIZE_MAX/2 &&
            chroma_row>=chroma_width*2 && chroma_row<=SIZE_MAX/chroma_height) {
            const uint8_t *at=chroma+(chroma_height/2)*chroma_row+(chroma_width/2)*2;
            pixels.u=at[0];pixels.v=at[1];
        }
    }
    pixels.unlock_status=CVPixelBufferUnlockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
    return pixels;
}
static int attachment_boolean(CFTypeRef value)
{
    if (!value) return -1;
    const CFTypeID type=CFGetTypeID(value);
    if (type==CFBooleanGetTypeID()) return CFBooleanGetValue((CFBooleanRef)value)?1:0;
    double number=0;
    return type==CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)value,kCFNumberDoubleType,&number) &&
        isfinite(number) ? (number!=0):-2;
}
static double sample_metadata_number(CMSampleBufferRef sample,CFStringRef key)
{
    CFTypeRef metadata=sample?CMGetAttachment(sample,CFSTR("MetadataDictionary"),NULL):NULL;
    if (!metadata || CFGetTypeID(metadata)!=CFDictionaryGetTypeID()) return -1;
    CFTypeRef value=CFDictionaryGetValue((CFDictionaryRef)metadata,key);
    double number=-1;
    return value && CFGetTypeID(value)==CFNumberGetTypeID() &&
        CFNumberGetValue((CFNumberRef)value,kCFNumberDoubleType,&number) && isfinite(number) ? number:-1;
}
/* The old ROT-import statistic counts texture construction, including cached
 * textures. These counters instead bracket the original calls. Snapshots use
 * only atomics and retain no node, sample, surface or semaphore. They are
 * sampled by the provider's existing output report; no extra worker/timer. */
enum FlowStage { FlowSource,FlowStitcher,FlowRotation,FlowTransfer,FlowSink,FlowStageCount };
struct PreviewFlow {
    uint64_t entered,returned,enter_ticks,return_ticks,sequence,generation;
    uintptr_t node,semaphore;
    uint64_t sink_received,sink_contexts;
    uint32_t sink_slot,sink_first,sink_fence;
};
static struct PreviewFlow preview_flow[FlowStageCount];
static const char *const flow_names[]={"source","stitcher","rotation","transfer","sink"};
static uint64_t flow_number(double value)
{ return value>=0 && value<=UINT32_MAX ? (uint64_t)value:0; }
static void flow_enter_values(enum FlowStage stage,id self,uint64_t sequence,uint64_t generation,uintptr_t semaphore)
{
    struct PreviewFlow *flow=&preview_flow[stage];
    __atomic_store_n(&flow->node,(uintptr_t)self,__ATOMIC_RELAXED);
    __atomic_store_n(&flow->semaphore,semaphore,__ATOMIC_RELAXED);
    __atomic_store_n(&flow->sequence,sequence,__ATOMIC_RELAXED);
    __atomic_store_n(&flow->generation,generation,__ATOMIC_RELAXED);
    __atomic_store_n(&flow->enter_ticks,mach_absolute_time(),__ATOMIC_RELAXED);
    __atomic_add_fetch(&flow->entered,1u,__ATOMIC_RELEASE);
}
static void flow_enter(enum FlowStage stage,id self,CMSampleBufferRef sample,uintptr_t semaphore)
{
    flow_enter_values(stage,self,
        flow_number(sample_metadata_number(sample,CFSTR("FrameSequence"))),
        flow_number(sample_metadata_number(sample,CFSTR("NezhaCameraGraphGeneration"))),semaphore);
}
static void flow_enter_rotation(id self,CVPixelBufferRef image)
{
    CFTypeRef metadata=image?CVBufferCopyAttachment(image,CFSTR("MetadataDictionary"),NULL):NULL;
    uint64_t sequence=0,generation=0;
    if (metadata && CFGetTypeID(metadata)==CFDictionaryGetTypeID()) {
        const CFStringRef keys[]={CFSTR("FrameSequence"),CFSTR("NezhaCameraGraphGeneration")};
        uint64_t *const values[]={&sequence,&generation};
        for (unsigned i=0;i<2;++i) {
            CFTypeRef value=CFDictionaryGetValue((CFDictionaryRef)metadata,keys[i]);
            double number=-1;
            if (value && CFGetTypeID(value)==CFNumberGetTypeID() &&
                CFNumberGetValue((CFNumberRef)value,kCFNumberDoubleType,&number)) *values[i]=flow_number(number);
        }
    }
    if (metadata) CFRelease(metadata);
    flow_enter_values(FlowRotation,self,sequence,generation,0);
}
static void flow_return(enum FlowStage stage)
{
    __atomic_store_n(&preview_flow[stage].return_ticks,mach_absolute_time(),__ATOMIC_RELAXED);
    __atomic_add_fetch(&preview_flow[stage].returned,1u,__ATOMIC_RELEASE);
}
static uintptr_t node_semaphore(id self,Class type,size_t offset)
{
    uintptr_t value=0;
    if (self && type && object_getClass(self)==type && class_getInstanceSize(type)>=offset+sizeof value)
        memcpy(&value,(const uint8_t *)(const void *)self+offset,sizeof value);
    return value;
}
static void sink_state(id self)
{
    if (!self || !preview_sink_class || object_getClass(self)!=preview_sink_class) return;
    const uint8_t *bytes=(const void *)self;
    struct PreviewFlow *flow=&preview_flow[FlowSink];
    uint64_t received=0,contexts=0;
    uint32_t slot=0;
    memcpy(&received,bytes+400,sizeof received);
    memcpy(&contexts,bytes+496,sizeof contexts);
    memcpy(&slot,bytes+232,sizeof slot);
    __atomic_store_n(&flow->sink_received,received,__ATOMIC_RELAXED);
    __atomic_store_n(&flow->sink_contexts,contexts,__ATOMIC_RELAXED);
    __atomic_store_n(&flow->sink_slot,slot,__ATOMIC_RELAXED);
    __atomic_store_n(&flow->sink_first,(uint32_t)bytes[568]|((uint32_t)bytes[592]<<8)|((uint32_t)bytes[569]<<16),__ATOMIC_RELAXED);
    __atomic_store_n(&flow->sink_fence,(uint32_t)bytes[666]|((uint32_t)bytes[667]<<8),__ATOMIC_RELAXED);
}
void nezha_camera_renderer_report_flow(uint64_t sequence,uint64_t ticks,unsigned generation)
{
    static unsigned reports;
    if (__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)>=40) return;
    const uint64_t observed=mach_absolute_time();
    for (unsigned stage=0;stage<FlowStageCount;++stage) {
        const struct PreviewFlow *flow=&preview_flow[stage];
        const uint64_t entered=__atomic_load_n(&flow->entered,__ATOMIC_ACQUIRE);
        const uint64_t returned=__atomic_load_n(&flow->returned,__ATOMIC_ACQUIRE);
        fprintf(stderr,"nezha-camera: preview flow stage=%s entered=%llu returned=%llu last-seq=%llu last-generation=%llu enter-ticks=%llu return-ticks=%llu node=%p semaphore=%p provider-seq=%llu provider-generation=%u provider-ticks=%llu observed-ticks=%llu received=%llu contexts=%llu slot=%u first=%x fence=%x snapshot=independent-atomics\n",
            flow_names[stage],(unsigned long long)entered,(unsigned long long)returned,
            (unsigned long long)__atomic_load_n(&flow->sequence,__ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&flow->generation,__ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&flow->enter_ticks,__ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&flow->return_ticks,__ATOMIC_RELAXED),
            (void *)__atomic_load_n(&flow->node,__ATOMIC_RELAXED),(void *)__atomic_load_n(&flow->semaphore,__ATOMIC_RELAXED),
            (unsigned long long)sequence,generation,(unsigned long long)ticks,(unsigned long long)observed,
            (unsigned long long)__atomic_load_n(&flow->sink_received,__ATOMIC_RELAXED),
            (unsigned long long)__atomic_load_n(&flow->sink_contexts,__ATOMIC_RELAXED),
            __atomic_load_n(&flow->sink_slot,__ATOMIC_RELAXED),
            __atomic_load_n(&flow->sink_first,__ATOMIC_RELAXED),__atomic_load_n(&flow->sink_fence,__ATOMIC_RELAXED));
    }
}
static bool preview_report(unsigned input,double sequence,unsigned *reports)
{
    const bool early=sequence>=1 && sequence<24;
    return (input<8 || early || input==31 || input==63 || input==127 || input%300==0) &&
        __atomic_fetch_add(reports,1u,__ATOMIC_RELAXED)<64;
}
static void render_preview_sample(id self,SEL selector,CMSampleBufferRef sample,id input)
{
    flow_enter(FlowSink,self,sample,0);
    sink_state(self);
    static unsigned inputs,reports;
    const unsigned report=__atomic_fetch_add(&inputs,1u,__ATOMIC_RELAXED);
    const double sequence=sample_metadata_number(sample,CFSTR("FrameSequence"));
    const double generation=sample_metadata_number(sample,CFSTR("NezhaCameraGraphGeneration"));
    const bool observe=preview_report(report,sequence,&reports);
    uint64_t before=0;
    if (observe) {
        const uint64_t started=mach_absolute_time();
        CVPixelBufferRef image=sample?CMSampleBufferGetImageBuffer(sample):NULL;
        IOSurfaceRef surface=image?CVPixelBufferGetIOSurface(image):NULL;
        const CMTime pts=sample?CMSampleBufferGetPresentationTimeStamp(sample):(CMTime){0};
        const int black=sample?attachment_boolean(CMGetAttachment(sample,CFSTR("FrameIsBlackened"),NULL)):-1;
        CFTypeRef marker=image?CVBufferCopyAttachment(image,CFSTR("NezhaCameraCompletedRaw1x"),NULL):NULL;
        const int own=attachment_boolean(marker);
        if (marker) CFRelease(marker);
        const struct PreviewPixels pixels=preview_pixels(image);
        fprintf(stderr,"nezha-camera: final preview input=%u sample=%p image=%p surface=%u size=%zux%zu format=%08x pts=%lld/%d/%u/%lld sequence=%.0f black=%d own=%d lock=%d unlock=%d luma=%u/%u/%u/%u uv=%u/%u grid=",
            report,(void *)sample,(void *)image,surface?IOSurfaceGetID(surface):0,
            image?CVPixelBufferGetWidth(image):0,image?CVPixelBufferGetHeight(image):0,
            image?CVPixelBufferGetPixelFormatType(image):0,
            (long long)pts.value,pts.timescale,pts.flags,(long long)pts.epoch,sequence,black,own,
            pixels.lock_status,pixels.unlock_status,pixels.count,pixels.nonzero,pixels.minimum,pixels.maximum,
            pixels.u,pixels.v);
        for (unsigned i=0;i<pixels.count;++i) fprintf(stderr,"%s%u",i?",":"",pixels.luma[i]);
        before=mach_absolute_time();
        fprintf(stderr," observe-ticks=%llu generation=%.0f sink=%p\n",
            (unsigned long long)(before-started),generation,(void *)self);
    }
    ((void (*)(id,SEL,CMSampleBufferRef,id))original_preview_sink)(self,selector,sample,input);
    sink_state(self);
    flow_return(FlowSink);
    if (observe) fprintf(stderr,"nezha-camera: final preview original returned input=%u ticks=%llu sequence=%.0f generation=%.0f\n",
        report,(unsigned long long)(mach_absolute_time()-before),sequence,generation);
}
static void install_preview_sink(void)
{
    if (preview_sink_installed) return;
    Class type=objc_getClass("BWImageQueueSinkNode");
    Method method=type?class_getInstanceMethod(type,sel_registerName("renderSampleBuffer:forInput:")):NULL;
    IMP original=method?method_getImplementation(method):NULL;
    if (!original || strcmp(method_getTypeEncoding(method),"v32@0:8^{opaqueCMSampleBuffer=}16@24") ||
        (uintptr_t)ptrauth_strip(original,ptrauth_key_function_pointer)!=(uintptr_t)(0x1b04a34c0ULL+capture_slide)) return;
    original_preview_sink=original;
    const char *fields[]={"_imageQueueSlot","_numFramesReceivedForCurrentConfiguration",
        "_enqueuedBufferContextCount","_didCallFirstFrameAtHostTimeCallback",
        "_didCallFirstFrameForCurrentConfiguration","_didCallFirstFrameCallback",
        "_fenceSupportEnabled","_fencePortGenerationIDWillChange"};
    const ptrdiff_t offsets[]={232,400,496,568,592,569,666,667};
    bool state_ok=class_getInstanceSize(type)>=668;
    for (unsigned i=0;i<sizeof offsets/sizeof offsets[0];++i) {
        Ivar ivar=class_getInstanceVariable(type,fields[i]);
        state_ok &= ivar && ivar_getOffset(ivar)==offsets[i];
    }
    if (state_ok) preview_sink_class=type;
    method_setImplementation(method,(IMP)render_preview_sample);
    preview_sink_installed=true;
    fprintf(stderr,"nezha-camera: original final preview sample observer installed limit=64 first=8 early-sequence=24 interval=300 grid=25 readonly=1 flow=1 sink-state=%u\n",(unsigned)state_ok);
}
static void render_preview_stitcher(id self,SEL selector,CMSampleBufferRef sample,id input)
{
    flow_enter(FlowStitcher,self,sample,node_semaphore(self,preview_stitcher_class,208));
    ((void (*)(id,SEL,CMSampleBufferRef,id))original_preview_stitcher)(self,selector,sample,input);
    flow_return(FlowStitcher);
}
static void install_preview_stitcher(void)
{
    if (preview_stitcher_installed) return;
    Class type=objc_getClass("BWPreviewStitcherNode");
    Method method=type?class_getInstanceMethod(type,sel_registerName("renderSampleBuffer:forInput:")):NULL;
    IMP original=method?method_getImplementation(method):NULL;
    Ivar semaphore=type?class_getInstanceVariable(type,"_emitSampleBufferSemaphore"):NULL;
    if (!original || strcmp(method_getTypeEncoding(method),"v32@0:8^{opaqueCMSampleBuffer=}16@24") ||
        (uintptr_t)ptrauth_strip(original,ptrauth_key_function_pointer)!=(uintptr_t)(0x1b04d0c5cULL+capture_slide) ||
        !semaphore || ivar_getOffset(semaphore)!=208 || class_getInstanceSize(type)<216) return;
    original_preview_stitcher=original;preview_stitcher_class=type;
    method_setImplementation(method,(IMP)render_preview_stitcher);
    preview_stitcher_installed=true;
    fputs("nezha-camera: original preview stitcher call counters installed readonly=1\n",stderr);
}
static void emit_node_output(id self,SEL selector,CMSampleBufferRef sample)
{
    /* The original getter loads its existing weak node. A source handler
     * returns before this servicing-thread emit, so count the emit itself. */
    id node=((id (*)(id,SEL))objc_msgSend)(self,sel_registerName("node"));
    const bool source=node && camera_source_class && object_getClass(node)==camera_source_class;
    if (source) flow_enter(FlowSource,node,sample,0);
    ((void (*)(id,SEL,CMSampleBufferRef))original_node_output)(self,selector,sample);
    if (source) flow_return(FlowSource);
}
static void install_node_output(void)
{
    if (node_output_installed) return;
    Class type=objc_getClass("BWNodeOutput");
    Method method=type?class_getInstanceMethod(type,sel_registerName("emitSampleBuffer:")):NULL;
    Method getter=type?class_getInstanceMethod(type,sel_registerName("node")):NULL;
    IMP original=method?method_getImplementation(method):NULL;
    IMP node=getter?method_getImplementation(getter):NULL;
    Class source=objc_getClass("BWMultiStreamCameraSourceNode");
    if (!source || !original || !node || strcmp(method_getTypeEncoding(method),"v24@0:8^{opaqueCMSampleBuffer=}16") ||
        strcmp(method_getTypeEncoding(getter),"@16@0:8") ||
        (uintptr_t)ptrauth_strip(original,ptrauth_key_function_pointer)!=(uintptr_t)(0x1b0525eecULL+capture_slide) ||
        (uintptr_t)ptrauth_strip(node,ptrauth_key_function_pointer)!=(uintptr_t)(0x1b0487becULL+capture_slide)) return;
    original_node_output=original;camera_source_class=source;
    method_setImplementation(method,(IMP)emit_node_output);
    node_output_installed=true;
    fputs("nezha-camera: original source output call counters installed readonly=1\n",stderr);
}
static bool scaler_available(Class type)
{
    if (!type) return false;
    SEL up=sel_registerName("maxUpscale"),down=sel_registerName("maxDownscale");
    Method um=class_getClassMethod(type,up),dm=class_getClassMethod(type,down);
    if (!um || !dm || strcmp(method_getTypeEncoding(um),"{CGSize=dd}16@0:8") ||
        strcmp(method_getTypeEncoding(dm),"{CGSize=dd}16@0:8")) return false;
    RendererSize u=((RendererSize (*)(id,SEL))objc_msgSend)((id)type,up);
    RendererSize d=((RendererSize (*)(id,SEL))objc_msgSend)((id)type,down);
    return isfinite(u.width) && isfinite(u.height) && isfinite(d.width) && isfinite(d.height) &&
        u.width>0 && u.height>0 && d.width>0 && d.height>0;
}
static bool supports_accelerated(id self,SEL selector)
{
    const bool original=((bool (*)(id,SEL))original_supports)(self,selector);
    const bool available=scaler_available(objc_getClass("FigM2MController"));
    static unsigned reports;
    if (__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)<4)
        fprintf(stderr,"nezha-camera: renderer border support original=%u native_scaler=%u selected=%u\n",
            (unsigned)original,(unsigned)available,(unsigned)(original && available));
    return original && available;
}
static id initialize_renderer(id self,SEL selector,id pool,bool fractional,bool compressed)
{
    const bool selected=compressed && scaler_available(objc_getClass("FigM2MController"));
    static unsigned reports;
    if (__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)<8)
        fprintf(stderr,"nezha-camera: renderer setup pool=%u fractional=%u compressed_requested=%u compressed_selected=%u\n",
            (unsigned)(pool!=NULL),(unsigned)fractional,(unsigned)compressed,(unsigned)selected);
    return ((id (*)(id,SEL,id,bool,bool))original_initializer)(self,selector,pool,fractional,selected);
}
/* The provider has already performed the sole implemented 1x RAW crop
 * into this graph buffer. An empty upstream rectangle cannot describe
 * any pixels in that completed output. Keep this repair scoped to the
 * provider's actual frame metadata; other sources retain the original. */
static bool own_full_source(CVPixelBufferRef source)
{
    if (!source || !CVPixelBufferGetWidth(source) || !CVPixelBufferGetHeight(source)) return false;
    CFTypeRef origin=CVBufferCopyAttachment(source,CFSTR("NezhaCameraCompletedRaw1x"),NULL);
    if (!origin) return false;
    const bool matched=CFGetTypeID(origin)==CFBooleanGetTypeID() &&
        CFBooleanGetValue((CFBooleanRef)origin);
    CFRelease(origin);return matched;
}
/* The real preview's downstream BWPixelTransferNode reads crop attachments
 * before computing both its input dimensions and its transfer rectangles.
 * A completed 1x image with an empty crop cannot be repaired only at the
 * renderer's by-value rotation call: the node still calculates a 0x0 input.
 * Normalize only the crop consumed by this node, on a sample copy. Its
 * original configuration, pixels and timestamp remain owned by the system. */
static bool repairable_node_rect(CFTypeRef value,bool normalized_final_crop,RendererRect *rect)
{
    CGRect decoded;
    if (!value || CFGetTypeID(value)!=CFDictionaryGetTypeID() ||
        !CGRectMakeWithDictionaryRepresentation((CFDictionaryRef)value,&decoded)) return false;
    *rect=(RendererRect){{decoded.origin.x,decoded.origin.y},
        {decoded.size.width,decoded.size.height}};
    const bool finite_empty=isfinite(rect->origin.x) && isfinite(rect->origin.y) &&
        rect->size.width==0 && rect->size.height==0;
    /* The actual Camera144 mode1 consumer received an all-NaN FinalCropRect
     * on completed 1x frames. It has no usable crop; the original dimension
     * conversion becomes 0x0 and selects the 16x16 intermediate. Do not
     * normalize partial, mixed nonfinite or absolute-rectangle requests. */
    const bool undefined_final=normalized_final_crop && isnan(rect->origin.x) &&
        isnan(rect->origin.y) && isnan(rect->size.width) && isnan(rect->size.height);
    return finite_empty || undefined_final;
}
static CMSampleBufferRef transfer_sample(CMSampleBufferRef sample,CVPixelBufferRef image,
    int crop_mode,bool applies_primary,unsigned *repaired,OSStatus *error,RendererRect *requested)
{
    *repaired=0;*error=0;*requested=(RendererRect){0};
    if (!sample || !own_full_source(image)) return sample;
    const bool primary=crop_mode==2 || (crop_mode==0 && applies_primary);
    CFStringRef key=primary?CFSTR("PrimaryCaptureRect"):
        crop_mode==1?CFSTR("FinalCropRect"):crop_mode==3?CFSTR("ValidBufferRect"):NULL;
    if (!key) return sample;
    CMAttachmentMode mode=kCMAttachmentMode_ShouldPropagate;
    CFTypeRef metadata=primary?NULL:CMGetAttachment(sample,CFSTR("MetadataDictionary"),&mode);
    if (!primary && (!metadata || CFGetTypeID(metadata)!=CFDictionaryGetTypeID())) return sample;
    CFTypeRef value=primary?CMGetAttachment(sample,key,&mode):
        CFDictionaryGetValue((CFDictionaryRef)metadata,key);
    if (!repairable_node_rect(value,crop_mode==1 && !primary,requested)) return sample;
    const CGRect full=CGRectMake(0,0,crop_mode==3?CVPixelBufferGetWidth(image):1,
        crop_mode==3?CVPixelBufferGetHeight(image):1);
    CFDictionaryRef rectangle=CGRectCreateDictionaryRepresentation(full);
    CFMutableDictionaryRef selected=metadata?CFDictionaryCreateMutableCopy(NULL,0,
        (CFDictionaryRef)metadata):NULL;
    CMSampleBufferRef copy=NULL;
    if (!rectangle || (metadata && !selected)) *error=kCVReturnAllocationFailed;
    else *error=CMSampleBufferCreateCopy(NULL,sample,&copy);
    if (!*error && copy) {
        if (primary) CMSetAttachment(copy,key,rectangle,mode);
        else {
            CFDictionarySetValue(selected,key,rectangle);
            CMSetAttachment(copy,CFSTR("MetadataDictionary"),selected,mode);
        }
        *repaired=primary?1:crop_mode==1?2:4;
    } else {
        if (copy) CFRelease(copy);
        copy=NULL;
        if (!*error) *error=kCVReturnAllocationFailed;
    }
    if (rectangle) CFRelease(rectangle);
    if (selected) CFRelease(selected);
    return copy?copy:sample;
}
static void render_pixel_transfer_sample(id self,SEL selector,CMSampleBufferRef sample,id input)
{
    flow_enter(FlowTransfer,self,sample,node_semaphore(self,pixel_transfer_node_class,408));
    CVPixelBufferRef image=sample?CMSampleBufferGetImageBuffer(sample):NULL;
    int crop_mode=-1;bool applies_primary=false,pending=false;
    RendererRect live={0};
    const bool owned=own_full_source(image);
    if (owned && self && object_getClass(self)==pixel_transfer_node_class) {
        const uint8_t *bytes=(const void *)self;
        pending=bytes[400]!=0;
        memcpy(&crop_mode,bytes+(pending?128:132),sizeof crop_mode);
        memcpy(&live,bytes+(pending?136:168),sizeof live);
        applies_primary=bytes[418]!=0;
    }
    unsigned repaired=0;OSStatus error=0;RendererRect requested={0};
    CMSampleBufferRef selected=transfer_sample(sample,image,crop_mode,applies_primary,
        &repaired,&error,&requested);
    static unsigned reports;
    if (owned && __atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED)<8)
        fprintf(stderr,"nezha-camera: pixel transfer node=%p sample=%p selected=%p source=%zux%zu crop_mode=%d pending=%u primary=%u configured_crop=%.9g,%.9g,%.9g,%.9g requested=%.9g,%.9g,%.9g,%.9g repaired=%u error=%d\n",
            (void *)self,(void *)sample,(void *)selected,CVPixelBufferGetWidth(image),
            CVPixelBufferGetHeight(image),crop_mode,(unsigned)pending,(unsigned)applies_primary,
            live.origin.x,live.origin.y,live.size.width,live.size.height,
            requested.origin.x,requested.origin.y,requested.size.width,requested.size.height,
            repaired,(int)error);
    ((void (*)(id,SEL,CMSampleBufferRef,id))original_pixel_transfer_node)(self,selector,selected,input);
    if (selected!=sample) CFRelease(selected);
    flow_return(FlowTransfer);
}
static void install_pixel_transfer_node(void)
{
    if (pixel_transfer_node_installed) return;
    Class type=objc_getClass("BWPixelTransferNode");
    Method method=type?class_getInstanceMethod(type,sel_registerName("renderSampleBuffer:forInput:")):NULL;
    IMP original=method?method_getImplementation(method):NULL;
    if (!original || strcmp(method_getTypeEncoding(method),"v32@0:8^{opaqueCMSampleBuffer=}16@24") ||
        (uintptr_t)ptrauth_strip(original,ptrauth_key_function_pointer)!=(uintptr_t)(0x1b04ae89cULL+capture_slide) ||
        class_getInstanceSize(type)<467) return;
    const char *names[]={"_cropMode","_liveCropMode","_inputCropRect","_liveInputCropRect",
        "_makeCurrentConfigurationLiveOnNextRenderCallback","_appliesPrimaryCaptureRect",
        "_emitSampleBufferSemaphore"};
    const ptrdiff_t offsets[]={128,132,136,168,400,418,408};
    for (unsigned i=0;i<sizeof offsets/sizeof offsets[0];++i) {
        Ivar ivar=class_getInstanceVariable(type,names[i]);
        if (!ivar || ivar_getOffset(ivar)!=offsets[i]) return;
    }
    pixel_transfer_node_class=type;original_pixel_transfer_node=original;
    method_setImplementation(method,(IMP)render_pixel_transfer_sample);
    pixel_transfer_node_installed=true;
    fputs("nezha-camera: original pixel transfer completed-frame crop repair installed readonly-node=1\n",stderr);
}
/* Camera141 verified that repairing the returned input alone leaves the
 * original output rectangle empty and submits a zero-sized GPU dispatch.
 * Normalize our completed 1x frame before the original calculation, so
 * input, output and applied shift are computed together. Foreign, partial
 * and invalid rectangles still take the original path once. */
static void get_input_rect(id self,SEL selector,RendererRect *input,RendererRect *output,
    RendererPoint *applied_shift,CVPixelBufferRef source,RendererRect src,
    RendererDimensions dimensions,RendererRect dst,RendererPoint shift,
    bool outside_allowed,bool ignore_bounds,float scale,bool final)
{
    const RendererRect requested=src;
    const bool valid_request=dimensions.width>0 && dimensions.height>0 &&
        isfinite(dst.origin.x) && isfinite(dst.origin.y) &&
        isfinite(dst.size.width) && isfinite(dst.size.height) &&
        dst.size.width>0 && dst.size.height>0 &&
        isfinite(shift.x) && isfinite(shift.y) && isfinite(scale) && scale>0;
    const bool owned=valid_request && own_full_source(source);
    const bool repaired=owned && src.size.width==0 && src.size.height==0 &&
        isfinite(src.origin.x) && isfinite(src.origin.y);
    if (repaired) src=(RendererRect){{0,0},{CVPixelBufferGetWidth(source),CVPixelBufferGetHeight(source)}};
    ((void (*)(id,SEL,RendererRect *,RendererRect *,RendererPoint *,CVPixelBufferRef,
        RendererRect,RendererDimensions,RendererRect,RendererPoint,bool,bool,float,bool))
        original_input_rect)(self,selector,input,output,applied_shift,source,src,
            dimensions,dst,shift,outside_allowed,ignore_bounds,scale,final);
    /* Preserve141's caller export repair when the original computed a
     * valid destination but clipped our completed source to empty. Do not
     * turn an empty destination into apparent success by changing input. */
    const bool complete_request=owned && src.origin.x==0 && src.origin.y==0 &&
        src.size.width==CVPixelBufferGetWidth(source) &&
        src.size.height==CVPixelBufferGetHeight(source);
    const bool repaired_export=!repaired && complete_request && input && output &&
        input->size.width==0 && input->size.height==0 &&
        isfinite(input->origin.x) && isfinite(input->origin.y) &&
        isfinite(output->origin.x) && isfinite(output->origin.y) &&
        isfinite(output->size.width) && isfinite(output->size.height) &&
        output->size.width>0 && output->size.height>0;
    if (repaired_export) *input=(RendererRect){{0,0},{CVPixelBufferGetWidth(source),CVPixelBufferGetHeight(source)}};
    static unsigned reports;
    const unsigned report=__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED);
    if (report<8) {
        const RendererRect actual_input=input?*input:(RendererRect){0};
        const RendererRect actual_output=output?*output:(RendererRect){0};
        const RendererPoint actual_shift=applied_shift?*applied_shift:(RendererPoint){0};
        fprintf(stderr,"nezha-camera: stitcher geometry input=%u repaired-before=%u repaired-export=%u source=%zux%zu requested=%.9g,%.9g,%.9g,%.9g selected_request=%.9g,%.9g,%.9g,%.9g destination=%dx%d rect=%.9g,%.9g,%.9g,%.9g shift=%.9g,%.9g outside=%u ignore=%u scale=%.9g final=%u outputs=%u/%u/%u source_rect=%.9g,%.9g,%.9g,%.9g destination_rect=%.9g,%.9g,%.9g,%.9g applied_shift=%.9g,%.9g\n",
            report,(unsigned)repaired,(unsigned)repaired_export,source?CVPixelBufferGetWidth(source):0,
            source?CVPixelBufferGetHeight(source):0,
            requested.origin.x,requested.origin.y,requested.size.width,requested.size.height,
            src.origin.x,src.origin.y,src.size.width,src.size.height,dimensions.width,dimensions.height,
            dst.origin.x,dst.origin.y,dst.size.width,dst.size.height,shift.x,shift.y,
            (unsigned)outside_allowed,(unsigned)ignore_bounds,(double)scale,(unsigned)final,
            (unsigned)(input!=NULL),(unsigned)(output!=NULL),(unsigned)(applied_shift!=NULL),
            actual_input.origin.x,actual_input.origin.y,actual_input.size.width,actual_input.size.height,
            actual_output.origin.x,actual_output.origin.y,actual_output.size.width,actual_output.size.height,
            actual_shift.x,actual_shift.y);
    }
}
static void install_input_rect(void)
{
    if (input_rect_installed) return;
    Class type=objc_getClass("BWPreviewStitcherRenderer");
    Method method=type?class_getInstanceMethod(type,sel_registerName("_getInputRect:outputRect:inputShiftAppliedInOutputRect:toTransferPixelBuffer:rect:intoPixelBufferDimensions:rect:withInputShift:shiftOutsideBoundingRectAllowed:ignoreBounds:scale:forFinalOutput:")):NULL;
    IMP original=method?method_getImplementation(method):NULL;
    if (!original || strcmp(method_getTypeEncoding(method),"v152@0:8^{CGRect={CGPoint=dd}{CGSize=dd}}16^{CGRect={CGPoint=dd}{CGSize=dd}}24^{CGPoint=dd}32^{__CVBuffer=}40{CGRect={CGPoint=dd}{CGSize=dd}}48{?=ii}80{CGRect={CGPoint=dd}{CGSize=dd}}88{CGPoint=dd}120B136B140f144B148") ||
        (uintptr_t)ptrauth_strip(original,ptrauth_key_function_pointer)!=(uintptr_t)(0x1b04cd674ULL+capture_slide)) return;
    original_input_rect=original;
    method_setImplementation(method,(IMP)get_input_rect);
    input_rect_installed=true;
    fputs("nezha-camera: original stitcher completed-frame geometry repair installed\n",stderr);
}
static bool own_uncompressed_border(CVPixelBufferRef source,CVPixelBufferRef destination,
    RendererRect rect,bool fill)
{
    if (!fill || !own_full_source(source) || !destination ||
        scaler_available(objc_getClass("FigM2MController"))) return false;
    const uint32_t format=CVPixelBufferGetPixelFormatType(destination);
    if (format!=kCVPixelFormatType_420YpCbCr8BiPlanarFullRange &&
        format!=kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) return false;
    const double width=CVPixelBufferGetWidth(destination),height=CVPixelBufferGetHeight(destination);
    const double x=rect.origin.x,y=rect.origin.y,w=rect.size.width,h=rect.size.height;
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) ||
        x<0 || y<0 || w<=0 || h<=0 || x>width || y>height ||
        w>width-x || h>height-y || fmod(x,2)!=0 || fmod(y,2)!=0 ||
        fmod(w,2)!=0 || fmod(h,2)!=0) return false;
    return x!=0 || y!=0 || w!=width || h!=height;
}
static int transfer_pixels(id self,SEL selector,RendererRotationSession session,
    CVPixelBufferRef source,RendererRect src,CVPixelBufferRef destination,RendererRect dst,bool fill)
{
    flow_enter_rotation(self,source);
    const bool repaired=src.size.width==0 && src.size.height==0 &&
        isfinite(src.origin.x) && isfinite(src.origin.y) && own_full_source(source);
    if (repaired) src=(RendererRect){{0,0},{CVPixelBufferGetWidth(source),CVPixelBufferGetHeight(source)}};
    static unsigned reports;
    const unsigned report=__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED);
    if (report<6 && repaired) fputs("nezha-camera: own completed frame empty source crop restored to full buffer\n",stderr);
    if (report<6) fprintf(stderr,"nezha-camera: renderer transfer source=%zux%zu rect=%.9g,%.9g,%.9g,%.9g destination=%zux%zu rect=%.9g,%.9g,%.9g,%.9g fill=%u\n",
        source ? CVPixelBufferGetWidth(source):0,source ? CVPixelBufferGetHeight(source):0,
        src.origin.x,src.origin.y,src.size.width,src.size.height,
        destination ? CVPixelBufferGetWidth(destination):0,destination ? CVPixelBufferGetHeight(destination):0,
        dst.origin.x,dst.origin.y,dst.size.width,dst.size.height,(unsigned)fill);
    int status=((int (*)(id,SEL,RendererRotationSession,CVPixelBufferRef,RendererRect,
        CVPixelBufferRef,RendererRect,bool))original_transfer)(self,selector,session,source,src,destination,dst,fill);
    /* Camera132 proved the successful uncompressed rotation leaves the
     * requested outer border unchanged. Its original standalone border
     * method fills those pixels correctly and preserves the valid image.
     * Reuse it only for our completed NV12 frame and integral chroma bounds. */
    if (!status && own_uncompressed_border(source,destination,dst,fill)) {
        status=((int (*)(id,SEL,CVPixelBufferRef,RendererRect))original_border)(self,
            sel_registerName("borderBlackFillPixelBuffer:validRect:"),destination,dst);
        if (report<6) fprintf(stderr,"nezha-camera: own uncompressed rotation border returned status=%d\n",status);
    }
    if (report<6) fprintf(stderr,"nezha-camera: renderer transfer returned status=%d\n",status);
    flow_return(FlowRotation);
    return status;
}
/* Reuse the renderer's bounded stderr to distinguish its real scaling
 * requests from a shader replay with an approximate uniform buffer. The
 * original session owns every buffer, crop, orientation and return value. */
static void report_scale(unsigned report,const char *operation,CVPixelBufferRef source,
    RendererRect src,CVPixelBufferRef destination,RendererRect dst,int orientation)
{
    if (report>=8) return;
    fprintf(stderr,"nezha-camera: original %s source=%zux%zu rect=%.17g,%.17g,%.17g,%.17g destination=%zux%zu rect=%.17g,%.17g,%.17g,%.17g orientation=%d\n",
        operation,source ? CVPixelBufferGetWidth(source):0,source ? CVPixelBufferGetHeight(source):0,
        src.origin.x,src.origin.y,src.size.width,src.size.height,
        destination ? CVPixelBufferGetWidth(destination):0,destination ? CVPixelBufferGetHeight(destination):0,
        dst.origin.x,dst.origin.y,dst.size.width,dst.size.height,orientation);
}
static int scale_pixels(id self,SEL selector,CVPixelBufferRef source,RendererRect src,
    CVPixelBufferRef destination,RendererRect dst)
{
    static unsigned reports;
    const unsigned report=__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED);
    report_scale(report,"BWScalingSession",source,src,destination,dst,0);
    const int status=((int (*)(id,SEL,CVPixelBufferRef,RendererRect,CVPixelBufferRef,RendererRect))
        original_scale)(self,selector,source,src,destination,dst);
    if (report<8) fprintf(stderr,"nezha-camera: original BWScalingSession returned status=%d\n",status);
    return status;
}
static int scale_oriented_pixels(id self,SEL selector,CVPixelBufferRef source,RendererRect src,
    int orientation,CVPixelBufferRef destination,RendererRect dst)
{
    static unsigned reports;
    const unsigned report=__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED);
    report_scale(report,"BWScalingSession oriented",source,src,destination,dst,orientation);
    const int status=((int (*)(id,SEL,CVPixelBufferRef,RendererRect,int,CVPixelBufferRef,RendererRect))
        original_oriented_scale)(self,selector,source,src,orientation,destination,dst);
    if (report<8) fprintf(stderr,"nezha-camera: original BWScalingSession oriented returned status=%d\n",status);
    return status;
}
static bool match_uuid(const struct mach_header *header)
{
    if (!header || header->magic!=MH_MAGIC_64) return false;
    const struct mach_header_64 *h=(const void *)header;
    const uint8_t *at=(const void *)(h+1),*end=at+h->sizeofcmds;
    for (uint32_t i=0;i<h->ncmds;++i) {
        if ((size_t)(end-at)<sizeof(struct load_command)) return false;
        const struct load_command *c=(const void *)at;
        if (c->cmdsize<sizeof(*c) || c->cmdsize>(size_t)(end-at)) return false;
        if (c->cmd==LC_UUID) return c->cmdsize>=sizeof(struct uuid_command) &&
            !memcmp(((const struct uuid_command *)c)->uuid,uuid,16);
        at+=c->cmdsize;
    }
    return false;
}
bool nezha_camera_renderer_install(const struct mach_header *header,intptr_t slide)
{
    if (match_uuid(header)) {capture_seen=true;capture_slide=slide;}
    if (!capture_seen) return false;
    install_preview_sink();
    install_preview_stitcher();
    install_node_output();
    install_input_rect();
    install_pixel_transfer_node();
    if (installed) return true;
    Class type=objc_getClass("BWPixelBufferTransferRenderer");
    Method support=type ? class_getClassMethod(type,sel_registerName("supportsAcceleratedBorderFill")):NULL;
    Method init=type ? class_getInstanceMethod(type,sel_registerName("initWithIntermediateBufferPool:fractionalSourceRectEnabled:compressedFormatEnabled:")):NULL;
    Method transfer=type ? class_getInstanceMethod(type,sel_registerName("doPixelRotationTransferWithSession:sourceBuffer:sourceRect:destinationBuffer:destinationRect:blackFillUnalignedEdges:")):NULL;
    Method border=type ? class_getInstanceMethod(type,sel_registerName("borderBlackFillPixelBuffer:validRect:")):NULL;
    IMP sf=support ? method_getImplementation(support):NULL,cf=init ? method_getImplementation(init):NULL;
    IMP tf=transfer ? method_getImplementation(transfer):NULL;
    IMP bf=border ? method_getImplementation(border):NULL;
    if (!sf || !cf || !tf || !bf || strcmp(method_getTypeEncoding(support),"B16@0:8") ||
        strcmp(method_getTypeEncoding(init),"@32@0:8@16B24B28") ||
        strcmp(method_getTypeEncoding(transfer),"i108@0:8^{OpaqueVTPixelRotationSession=}16^{__CVBuffer=}24{CGRect={CGPoint=dd}{CGSize=dd}}32^{__CVBuffer=}64{CGRect={CGPoint=dd}{CGSize=dd}}72B104") ||
        strcmp(method_getTypeEncoding(border),"i56@0:8^{__CVBuffer=}16{CGRect={CGPoint=dd}{CGSize=dd}}24") ||
        (uintptr_t)ptrauth_strip(bf,ptrauth_key_function_pointer)!=(uintptr_t)(0x1b0502be4ULL+capture_slide) ||
        (uintptr_t)ptrauth_strip(tf,ptrauth_key_function_pointer)!=(uintptr_t)(0x1b0504290ULL+capture_slide) ||
        (uintptr_t)ptrauth_strip(sf,ptrauth_key_function_pointer)!=(uintptr_t)(0x1b06fec7cULL+capture_slide) ||
        (uintptr_t)ptrauth_strip(cf,ptrauth_key_function_pointer)!=(uintptr_t)(0x1b05f3e4cULL+capture_slide)) return false;
    original_supports=sf;original_initializer=cf;original_transfer=tf;original_border=bf;
    method_setImplementation(support,(IMP)supports_accelerated);
    method_setImplementation(init,(IMP)initialize_renderer);
    method_setImplementation(transfer,(IMP)transfer_pixels);
    Class scaling=objc_getClass("BWScalingSession");
    Method scale=scaling ? class_getInstanceMethod(scaling,
        sel_registerName("scalePixelBuffer:rect:intoPixelBuffer:rect:")):NULL;
    Method oriented=scaling ? class_getInstanceMethod(scaling,
        sel_registerName("scalePixelBuffer:rect:exifOrientation:intoPixelBuffer:rect:")):NULL;
    IMP sc=scale ? method_getImplementation(scale):NULL;
    IMP oc=oriented ? method_getImplementation(oriented):NULL;
    if (sc && oc &&
        !strcmp(method_getTypeEncoding(scale),"i96@0:8^{__CVBuffer=}16{CGRect={CGPoint=dd}{CGSize=dd}}24^{__CVBuffer=}56{CGRect={CGPoint=dd}{CGSize=dd}}64") &&
        !strcmp(method_getTypeEncoding(oriented),"i100@0:8^{__CVBuffer=}16{CGRect={CGPoint=dd}{CGSize=dd}}24i56^{__CVBuffer=}60{CGRect={CGPoint=dd}{CGSize=dd}}68") &&
        (uintptr_t)ptrauth_strip(sc,ptrauth_key_function_pointer)==(uintptr_t)(0x1b0814da0ULL+capture_slide) &&
        (uintptr_t)ptrauth_strip(oc,ptrauth_key_function_pointer)==(uintptr_t)(0x1b0815248ULL+capture_slide)) {
        original_scale=sc;original_oriented_scale=oc;
        method_setImplementation(scale,(IMP)scale_pixels);
        method_setImplementation(oriented,(IMP)scale_oriented_pixels);
        fputs("nezha-camera: original scaling rectangle observers installed\n",stderr);
    }
    installed=true;
    fputs("nezha-camera: original uncompressed renderer fallback installed\n",stderr);
    return true;
}
static void image_added(const struct mach_header *header,intptr_t slide)
{ (void)nezha_camera_renderer_install(header,slide); }
void nezha_camera_renderer_observe(void)
{ _dyld_register_func_for_add_image(image_added); }
