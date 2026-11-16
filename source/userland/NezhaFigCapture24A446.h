/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_FIG_CAPTURE_24A446_H
#define NEZHA_FIG_CAPTURE_24A446_H

#include <stddef.h>
#include <stdint.h>

#if defined(__arm64e__)
#include <ptrauth.h>
#define NEZHA_FIG_IA(discriminator) __ptrauth(ptrauth_key_asia, 1, discriminator)
#define NEZHA_FIG_DA(discriminator) __ptrauth(ptrauth_key_asda, 1, discriminator)
#else
#define NEZHA_FIG_IA(discriminator)
#define NEZHA_FIG_DA(discriminator)
#endif

/* Selected CoreMedia/CMCaptureDevice ABI of the iPhone14,4 24A446 cache.
 * The SDK types remain opaque to the layout. The provider may initialize
 * the base at its own storage size, then pass the tables to CoreMedia. */
typedef struct OpaqueCMBaseObject *NezhaFigObject;
typedef const struct __CFAllocator *NezhaFigAllocator;
typedef const struct __CFString *NezhaFigString;
typedef const struct __CFArray *NezhaFigArray;
typedef const struct __CFDictionary *NezhaFigDictionary;
typedef struct opaqueCMSampleBuffer *NezhaFigSample;
typedef int32_t NezhaFigStatus;

struct NezhaFigBaseClass {
    uint64_t version, storage_size;
    uint64_t unused_equal;
    NezhaFigStatus (* NEZHA_FIG_IA(0x3af8) invalidate)(NezhaFigObject);
    void (* NEZHA_FIG_IA(0x5680) finalize)(NezhaFigObject);
    NezhaFigString (* NEZHA_FIG_IA(0xf18e) copy_description)(NezhaFigObject);
    NezhaFigStatus (* NEZHA_FIG_IA(0x9725) copy_property)(NezhaFigObject, NezhaFigString, NezhaFigAllocator, void *);
    NezhaFigStatus (* NEZHA_FIG_IA(0x2724) set_property)(NezhaFigObject, NezhaFigString, const void *);
    uint64_t unused[4];
};

struct NezhaFigDeviceClass {
    uint64_t version;
    uint64_t unused_processing_session;
    NezhaFigStatus (* NEZHA_FIG_IA(0x0d64) request_streams)(NezhaFigObject, NezhaFigArray, NezhaFigDictionary);
    NezhaFigStatus (* NEZHA_FIG_IA(0x8abf) relinquish_streams)(NezhaFigObject, NezhaFigArray, NezhaFigDictionary);
    uint64_t unused[4];
};

struct NezhaFigStreamClass {
    uint64_t version;
    NezhaFigStatus (* NEZHA_FIG_IA(0x0d9f) start)(NezhaFigObject);
    NezhaFigStatus (* NEZHA_FIG_IA(0x82f5) stop)(NezhaFigObject);
    NezhaFigStatus (* NEZHA_FIG_IA(0xd980) add_attachments)(NezhaFigObject, NezhaFigSample, NezhaFigDictionary);
    uint64_t unused[2];
};

struct NezhaFigDeviceVTable {
    uint64_t reserved;
    const struct NezhaFigBaseClass * NEZHA_FIG_DA(0xb911) base;
    const struct NezhaFigDeviceClass * NEZHA_FIG_DA(0xe0cc) device;
};

struct NezhaFigStreamVTable {
    uint64_t reserved;
    const struct NezhaFigBaseClass * NEZHA_FIG_DA(0xb911) base;
    const struct NezhaFigStreamClass * NEZHA_FIG_DA(0x49c6) stream;
};

_Static_assert(sizeof(struct NezhaFigBaseClass) == 0x60 &&
    offsetof(struct NezhaFigBaseClass, copy_property) == 0x30, "24A446 base layout");
_Static_assert(sizeof(struct NezhaFigDeviceClass) == 0x40 &&
    offsetof(struct NezhaFigDeviceClass, request_streams) == 0x10, "24A446 device layout");
_Static_assert(sizeof(struct NezhaFigStreamClass) == 0x30 &&
    offsetof(struct NezhaFigStreamClass, add_attachments) == 0x18, "24A446 stream layout");
_Static_assert(sizeof(struct NezhaFigDeviceVTable) == 0x18 &&
    offsetof(struct NezhaFigDeviceVTable, device) == 0x10, "24A446 device vtable layout");
_Static_assert(sizeof(struct NezhaFigStreamVTable) == 0x18 &&
    offsetof(struct NezhaFigStreamVTable, stream) == 0x10, "24A446 stream vtable layout");

/* Tables remain immutable and live for the object's lifetime. CoreMedia
 * allocates and zeroes base.storage_size bytes; the provider initializes
 * that storage before exposing the object. The real 24A446 ClassID
 * exports live in CMCaptureDevice, separate from CMCapture. */
NezhaFigStatus nezha_fig_capture24_create_device(NezhaFigAllocator allocator,
    const struct NezhaFigDeviceVTable *table, NezhaFigObject *out);
NezhaFigStatus nezha_fig_capture24_create_stream(NezhaFigAllocator allocator,
    const struct NezhaFigStreamVTable *table, NezhaFigObject *out);
void *nezha_fig_capture24_storage(NezhaFigObject object);

#undef NEZHA_FIG_IA
#undef NEZHA_FIG_DA
#endif
