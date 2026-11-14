/* SPDX-License-Identifier: MIT */
/* Host-only declarations of the public CoreVideo calls used by the adapter.
 * The iOS build uses the real SDK. This fixture exercises buffer ownership
 * and plane geometry; it does not emulate CoreVideo or the capture graph. */
#ifndef NEZHA_TEST_CORE_VIDEO_H
#define NEZHA_TEST_CORE_VIDEO_H
#include <stddef.h>
#include <stdint.h>
#include <CoreFoundation/CoreFoundation.h>
typedef struct __CVBuffer *CVPixelBufferRef;
typedef struct __CVBuffer *CVImageBufferRef;
typedef struct __CVPixelBufferPool *CVPixelBufferPoolRef;
typedef int32_t CVReturn;
typedef unsigned char Boolean;
typedef uint64_t CVPixelBufferLockFlags;
typedef uint32_t CVAttachmentMode;
enum { kCVReturnSuccess = 0, kCVReturnInvalidArgument = -6661, kCVReturnAllocationFailed = -6662,
    kCVAttachmentMode_ShouldPropagate = 1 };
size_t CVPixelBufferGetWidth(CVPixelBufferRef);
size_t CVPixelBufferGetHeight(CVPixelBufferRef);
uint32_t CVPixelBufferGetPixelFormatType(CVPixelBufferRef);
Boolean CVPixelBufferIsPlanar(CVPixelBufferRef);
size_t CVPixelBufferGetPlaneCount(CVPixelBufferRef);
size_t CVPixelBufferGetWidthOfPlane(CVPixelBufferRef, size_t);
size_t CVPixelBufferGetHeightOfPlane(CVPixelBufferRef, size_t);
size_t CVPixelBufferGetBytesPerRowOfPlane(CVPixelBufferRef, size_t);
void *CVPixelBufferGetBaseAddressOfPlane(CVPixelBufferRef, size_t);
size_t CVPixelBufferGetBytesPerRow(CVPixelBufferRef);
void *CVPixelBufferGetBaseAddress(CVPixelBufferRef);
size_t CVPixelBufferGetDataSize(CVPixelBufferRef);
CVReturn CVPixelBufferLockBaseAddress(CVPixelBufferRef, CVPixelBufferLockFlags);
CVReturn CVPixelBufferUnlockBaseAddress(CVPixelBufferRef, CVPixelBufferLockFlags);
void CVBufferSetAttachment(CVPixelBufferRef, CFStringRef, CFTypeRef, CVAttachmentMode);
CFTypeRef CVBufferGetAttachment(CVImageBufferRef,CFStringRef,CVAttachmentMode *);
CFTypeID CVPixelBufferPoolGetTypeID(void);
CVReturn CVPixelBufferPoolCreatePixelBuffer(CFAllocatorRef, CVPixelBufferPoolRef, CVPixelBufferRef *);
extern const CFStringRef kCVImageBufferYCbCrMatrixKey;
extern const CFStringRef kCVImageBufferYCbCrMatrix_ITU_R_601_4;
extern const CFStringRef kCVImageBufferColorPrimariesKey;
extern const CFStringRef kCVImageBufferColorPrimaries_ITU_R_709_2;
extern const CFStringRef kCVImageBufferTransferFunctionKey;
extern const CFStringRef kCVImageBufferTransferFunction_sRGB;
#endif
