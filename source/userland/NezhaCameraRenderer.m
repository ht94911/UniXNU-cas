/* SPDX-License-Identifier: MIT */
/* Choose the original uncompressed transfer path when the M2M scaler has no
 * usable capabilities. Do not fabricate scaler limits or replace its methods. */
#include "NezhaCameraRenderer.h"
#include <CoreVideo/CoreVideo.h>
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
static IMP original_supports,original_initializer,original_transfer;
static bool capture_seen,installed;
static intptr_t capture_slide;
static const uint8_t uuid[16]={0x99,0x5f,0xc2,0x2b,0x41,0xbd,0x37,0xb4,0x81,0x2f,0x26,0x86,0xb8,0x1c,0x26,0x94};
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
    CFTypeRef metadata=CVBufferCopyAttachment(source,CFSTR("MetadataDictionary"),NULL);
    if (!metadata) return false;
    bool matched=CFGetTypeID(metadata)==CFDictionaryGetTypeID();
    const CFStringRef keys[]={CFSTR("FrameSequence"),CFSTR("RawSensorWidth"),
        CFSTR("RawSensorHeight"),CFSTR("ZoomFactor")};
    const double expected[]={0,4096,3072,1};
    for (unsigned i=0;i<4 && matched;++i) {
        CFTypeRef field=CFDictionaryGetValue((CFDictionaryRef)metadata,keys[i]);double value=0;
        matched=field && CFGetTypeID(field)==CFNumberGetTypeID() &&
            CFNumberGetValue((CFNumberRef)field,kCFNumberDoubleType,&value) && isfinite(value) &&
            (i ? value==expected[i] : value>0);
    }
    CFRelease(metadata);return matched;
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
    const int status=((int (*)(id,SEL,RendererRotationSession,CVPixelBufferRef,RendererRect,
        CVPixelBufferRef,RendererRect,bool))original_transfer)(self,selector,session,source,src,destination,dst,fill);
    if (report<6) fprintf(stderr,"nezha-camera: renderer transfer returned status=%d\n",status);
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
    if (installed) return true;
    if (match_uuid(header)) {capture_seen=true;capture_slide=slide;}
    if (!capture_seen) return false;
    Class type=objc_getClass("BWPixelBufferTransferRenderer");
    Method support=type ? class_getClassMethod(type,sel_registerName("supportsAcceleratedBorderFill")):NULL;
    Method init=type ? class_getInstanceMethod(type,sel_registerName("initWithIntermediateBufferPool:fractionalSourceRectEnabled:compressedFormatEnabled:")):NULL;
    Method transfer=type ? class_getInstanceMethod(type,sel_registerName("doPixelRotationTransferWithSession:sourceBuffer:sourceRect:destinationBuffer:destinationRect:blackFillUnalignedEdges:")):NULL;
    IMP sf=support ? method_getImplementation(support):NULL,cf=init ? method_getImplementation(init):NULL;
    IMP tf=transfer ? method_getImplementation(transfer):NULL;
    if (!sf || !cf || !tf || strcmp(method_getTypeEncoding(support),"B16@0:8") ||
        strcmp(method_getTypeEncoding(init),"@32@0:8@16B24B28") ||
        strcmp(method_getTypeEncoding(transfer),"i108@0:8^{OpaqueVTPixelRotationSession=}16^{__CVBuffer=}24{CGRect={CGPoint=dd}{CGSize=dd}}32^{__CVBuffer=}64{CGRect={CGPoint=dd}{CGSize=dd}}72B104") ||
        (uintptr_t)ptrauth_strip(tf,ptrauth_key_function_pointer)!=(uintptr_t)(0x1b0504290ULL+capture_slide) ||
        (uintptr_t)ptrauth_strip(sf,ptrauth_key_function_pointer)!=(uintptr_t)(0x1b06fec7cULL+capture_slide) ||
        (uintptr_t)ptrauth_strip(cf,ptrauth_key_function_pointer)!=(uintptr_t)(0x1b05f3e4cULL+capture_slide)) return false;
    original_supports=sf;original_initializer=cf;original_transfer=tf;
    method_setImplementation(support,(IMP)supports_accelerated);
    method_setImplementation(init,(IMP)initialize_renderer);
    method_setImplementation(transfer,(IMP)transfer_pixels);
    installed=true;
    fputs("nezha-camera: original uncompressed renderer fallback installed\n",stderr);
    return true;
}
static void image_added(const struct mach_header *header,intptr_t slide)
{ (void)nezha_camera_renderer_install(header,slide); }
void nezha_camera_renderer_observe(void)
{ _dyld_register_func_for_add_image(image_added); }
