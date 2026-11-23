/* SPDX-License-Identifier: MIT */
/* Choose the original uncompressed transfer path when the M2M scaler has no
 * usable capabilities. Do not fabricate scaler limits or replace its methods. */
#include "NezhaCameraRenderer.h"
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach_time.h>
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
typedef struct OpaqueVTPixelRotationSession *RendererRotationSession;
static IMP original_supports,original_initializer,original_transfer,original_border;
static IMP original_scale,original_oriented_scale;
static IMP original_preview_sink;
static bool capture_seen,installed,preview_sink_installed;
static intptr_t capture_slide;
static const uint8_t uuid[16]={0x99,0x5f,0xc2,0x2b,0x41,0xbd,0x37,0xb4,0x81,0x2f,0x26,0x86,0xb8,0x1c,0x26,0x94};
/* The existing renderer stderr records only the first eight final inputs.
 * A successful intermediate transfer does not show which image reaches the
 * original display queue. Observe that queue's actual sample without changing
 * its attachments, pixels, timestamp or delivery. No full-frame readback. */
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
static void render_preview_sample(id self,SEL selector,CMSampleBufferRef sample,id input)
{
    static unsigned reports;
    const unsigned report=__atomic_fetch_add(&reports,1u,__ATOMIC_RELAXED);
    uint64_t before=0;
    if (report<8) {
        const uint64_t started=mach_absolute_time();
        CVPixelBufferRef image=sample?CMSampleBufferGetImageBuffer(sample):NULL;
        IOSurfaceRef surface=image?CVPixelBufferGetIOSurface(image):NULL;
        const CMTime pts=sample?CMSampleBufferGetPresentationTimeStamp(sample):(CMTime){0};
        const int black=sample?attachment_boolean(CMGetAttachment(sample,CFSTR("FrameIsBlackened"),NULL)):-1;
        CFTypeRef marker=image?CVBufferCopyAttachment(image,CFSTR("NezhaCameraCompletedRaw1x"),NULL):NULL;
        const int own=attachment_boolean(marker);
        if (marker) CFRelease(marker);
        CFTypeRef metadata=sample?CMGetAttachment(sample,CFSTR("MetadataDictionary"),NULL):NULL;
        double sequence=-1;
        if (metadata && CFGetTypeID(metadata)==CFDictionaryGetTypeID()) {
            CFTypeRef value=CFDictionaryGetValue((CFDictionaryRef)metadata,CFSTR("FrameSequence"));
            if (value && CFGetTypeID(value)==CFNumberGetTypeID())
                (void)CFNumberGetValue((CFNumberRef)value,kCFNumberDoubleType,&sequence);
        }
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
        fprintf(stderr," observe-ticks=%llu\n",(unsigned long long)(before-started));
    }
    ((void (*)(id,SEL,CMSampleBufferRef,id))original_preview_sink)(self,selector,sample,input);
    if (report<8) fprintf(stderr,"nezha-camera: final preview original returned input=%u ticks=%llu\n",
        report,(unsigned long long)(mach_absolute_time()-before));
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
    method_setImplementation(method,(IMP)render_preview_sample);
    preview_sink_installed=true;
    fputs("nezha-camera: original final preview sample observer installed limit=8 grid=25 readonly=1\n",stderr);
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
