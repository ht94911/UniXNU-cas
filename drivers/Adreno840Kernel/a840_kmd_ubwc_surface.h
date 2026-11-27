/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_UBWC_SURFACE_H
#define A840_KMD_UBWC_SURFACE_H

#include "a840_kmd_zero_page.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_MDSS_UBWC6 = 1u,
    A840_SURFACE_SCANOUT_ELIGIBLE = 1u,
    A840_SURFACE_BINDINGS = 2 * A840_ABI_V3_SURFACE_SLOTS
};

/* Both managers require a sleepable mutex shared by all callers of that
 * instance. Callbacks run under this mutex and must not reenter it. */
typedef struct A840KmdSurfaceOps {
    void *cookie;
    void (*lock)(void *);
    void (*unlock)(void *);
} A840KmdSurfaceOps;

typedef struct A840KmdSurfaceLayout {
    uint64_t bytes, display_base; /* Trusted MDSS IOVA, zero if unavailable. */
    uint32_t format, width, height;
    uint32_t meta_rows; /* Allocator's actual number of metadata rows. */
} A840KmdSurfaceLayout;

/* Supplied by NativeMobileDisplay for the current output, separately from
 * userspace metadata. Zero extra alignments select the 4 KiB/64 B minima. */
typedef struct A840KmdMdssCaps {
    uint32_t flags, max_width, max_height;
    uint32_t base_alignment, pitch_alignment;
    const uint32_t *formats;
    size_t format_count;
} A840KmdMdssCaps;

typedef struct A840KmdSurfaceResult {
    struct NativeAdrenoAbiSurfaceMetaV3 meta;
    uint32_t scanout_eligible, fallback_linear_required;
    /* True if meta is a registered layout. A rejected SET requiring a
     * separate linear surface never replaces the last valid layout. */
    uint32_t layout_valid;
} A840KmdSurfaceResult;

typedef struct A840KmdSurfaceRecord {
    uint64_t identity;
    A840KmdSurfaceLayout layout;
    struct NativeAdrenoAbiSurfaceMetaV3 meta;
    uint32_t bindings, has_meta;
} A840KmdSurfaceRecord;

typedef struct A840KmdSurfaceBinding {
    uint64_t client, handle;
    size_t record;
    uint32_t owner;
} A840KmdSurfaceBinding;

typedef struct A840KmdSurfaceManager {
    A840KmdSurfaceOps ops;
    A840KmdSurfaceRecord records[A840_ABI_V3_SURFACE_SLOTS];
    A840KmdSurfaceBinding bindings[A840_SURFACE_BINDINGS];
} A840KmdSurfaceManager;

A840KmdStatus a840_kmd_surface_init(A840KmdSurfaceManager *, const A840KmdSurfaceOps *);
/* Trusted BO lifecycle callbacks only. identity is the kernel object's
 * incarnation, never an external-method handle. Import follows an authorized
 * IOSurface lookup. Closing the creator preserves any live import. */
A840KmdStatus a840_kmd_surface_create(A840KmdSurfaceManager *, uint64_t client,
    uint64_t handle, uint64_t identity, const A840KmdSurfaceLayout *);
A840KmdStatus a840_kmd_surface_import(A840KmdSurfaceManager *, uint64_t client,
    uint64_t handle, uint64_t identity);
void a840_kmd_surface_release(A840KmdSurfaceManager *, uint64_t client, uint64_t handle);
void a840_kmd_surface_close_client(A840KmdSurfaceManager *, uint64_t client);
void a840_kmd_surface_destroy_bo(A840KmdSurfaceManager *, uint64_t identity);

/* Method 25. GET consumes only op and the caller's local handle. flags bit
 * zero is always derived, never trusted. Non-scanout queries do not certify
 * display eligibility. A fallback requires a separate linear allocation/copy.
 * A noncompressible format returns UNSUPPORTED with fallback set for scanout. */
A840KmdStatus a840_kmd_surface_meta(A840KmdSurfaceManager *, uint64_t client,
    const struct NativeAdrenoAbiSurfaceMetaV3 *, uint32_t for_scanout,
    const A840KmdMdssCaps *, A840KmdSurfaceResult *);

#ifdef __cplusplus
}
#endif
#endif
