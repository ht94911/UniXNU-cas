/* SPDX-License-Identifier: MIT */
/* Adreno 840 Native Metal Driver — Wave 12 System Frontend Core Bridge
 *
 * Pure C11, zero-fallback, zero steady-state heap allocation core uniting all
 * Phase 3/4/5/6 engines for iOS 27 full-system Metal takeover:
 *   - Process role (backboardd/CARenderServer vs App) & NativeAdrenoMetal.publish policy
 *   - IOSurface zero-copy binding (BGRA8/sRGB/RGBA16F/RGB10A2/XR10/NV12/P010 -> UBWC6 HBB=16 / Linear)
 *   - Unified 18 MiB GMEM TBDR + FBFetch + SyncTracker + CB Chunker + Early Promise + Frame Arena
 *   - Unified thread-safe handle tables for Heap, SharedEvent, ICB, DepthStencil, BinaryArchive
 *   - Truthful Adreno 840 capability queries aligned with Adreno840CapsContract.md
 */
#ifndef A840_SYSTEM_FRONTEND_CORE_H
#define A840_SYSTEM_FRONTEND_CORE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Adreno840Caps.h"
#include "Adreno840EmitABI.h"
#include "NativeAdrenoABI_v3.h"
#include "a840_argbuf_icb.h"
#include "a840_ca_render_bridge.h"
#include "a840_cb_chunker.h"
#include "a840_early_promise_sched.h"
#include "a840_emit_3d.h"
#include "a840_emit_compute.h"
#include "a840_emit_gmem.h"
#include "a840_emit_msaa_tess_vis.h"
#include "a840_frame_arena.h"
#include "a840_heap_allocator.h"
#include "a840_pack_cache_rt.h"
#include "a840_phase4_pipeline.h"
#include "a840_production_router.h"
#include "a840_render_pass_gmem.h"
#include "a840_runtime_cmd.h"
#include "a840_runtime_device.h"
#include "a840_sync_tracker.h"
#include "a840_ubwc_texture.h"
#include "chip_facts_a840.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_SYS_MAX_SURFACES = 128u,
    A840_SYS_MAX_HEAPS = 64u,
    A840_SYS_MAX_EVENTS = 128u,
    A840_SYS_MAX_ICBS = 64u,
    A840_SYS_MAX_DEPTH_STENCILS = 128u,
    A840_SYS_MAX_ARCHIVES = 32u,
    A840_SYS_MAX_ARCHIVE_KEYS = 128u,
    A840_SYS_MAX_EVENT_WAITERS = 16u,
    A840_SYS_MAX_FRAME_SURFACES = 32u,
    A840_SYS_MAX_PATH_BYTES = 512u,
    A840_SYS_DEFAULT_ARENA_FRAMES = 3u,
    A840_SYS_DEFAULT_ARENA_BYTES = 131072u,
    A840_SYS_DEFAULT_CMD_SLOTS = 32u,
    A840_SYS_DEFAULT_SLOT_DWORDS = 8192u,
    A840_SYS_DEFAULT_STATE_TABLE_CAP = 256u,
    A840_SYS_IOSURFACE_RO_ALIGN = 64u,
    A840_SYS_SPARSE_TILE_BYTES = 65536u,
    A840_SYS_DEFAULT_REFRESH_HZ = 120u
};

/* Generation-tagged opaque 64-bit handles (0 is always invalid). */
typedef uint64_t A840SysSurfaceHandle;
typedef uint64_t A840SysHeapHandle;
typedef uint64_t A840SysEventHandle;
typedef uint64_t A840SysIcbHandle;
typedef uint64_t A840SysDepthStencilHandle;
typedef uint64_t A840SysArchiveHandle;

#define A840_SYS_INVALID_HANDLE UINT64_C(0)

typedef enum A840SysStatus {
    A840_SYS_OK = 0,
    A840_SYS_ERR_INVALID_ARG = 1,
    A840_SYS_ERR_NOT_PUBLISHED = 2,
    A840_SYS_ERR_INVALID_STATE = 3,
    A840_SYS_ERR_CAPACITY = 4,
    A840_SYS_ERR_NOT_FOUND = 5,
    A840_SYS_ERR_STALE_HANDLE = 6,
    A840_SYS_ERR_UNSUPPORTED = 7,
    A840_SYS_ERR_FALLBACK_FORBIDDEN = 8,
    A840_SYS_ERR_PERMISSION_DENIED = 9,
    A840_SYS_ERR_OOM = 10,
    A840_SYS_ERR_EMIT_FAILED = 11,
    A840_SYS_ERR_RUNTIME_FAILED = 12,
    A840_SYS_ERR_TIMEOUT = 13,
    A840_SYS_ERR_CROSS_DEVICE = 14
} A840SysStatus;

typedef enum A840SysProcRole {
    A840_SYS_ROLE_APP = 0u,
    A840_SYS_ROLE_RENDER_SERVER = 1u
} A840SysProcRole;

typedef enum A840SysPublishState {
    A840_SYS_PUBLISH_DISABLED = 0u,
    A840_SYS_PUBLISH_ENABLED = 1u
} A840SysPublishState;

typedef enum A840SysCacheTierPolicy {
    A840_SYS_CACHE_PRIVATE_PER_PROC = 0u,
    A840_SYS_CACHE_SHARED_SYSTEM_PACK = 1u
} A840SysCacheTierPolicy;

typedef enum A840SysScanoutMode {
    A840_SYS_SCANOUT_DIRECT_UBWC6 = 0u,
    A840_SYS_SCANOUT_NATIVE_LINEAR = 1u
} A840SysScanoutMode;

typedef struct A840SysPublishPolicy {
    A840SysPublishState publish_state;
    A840SysProcRole proc_role;
    A840SysCacheTierPolicy cache_policy;
    bool allow_device_creation;
    bool allow_early_promise;
    bool pack_tier0_readonly;
    bool scanout_bridge_enabled;
    uint32_t preferred_ring_id;
    uint32_t fallback_compiled_out;
    char marker_path[A840_SYS_MAX_PATH_BYTES];
    char pack_path[A840_SYS_MAX_PATH_BYTES];
    char cache_dir[A840_SYS_MAX_PATH_BYTES];
} A840SysPublishPolicy;

typedef struct A840SysDeviceCaps {
    uint64_t chip_id;
    uint32_t gmem_bytes;
    uint32_t highest_bank_bit;
    uint32_t ubwc_version;
    uint32_t ubwc_swizzle;
    uint32_t macrotile_mode;
    uint32_t max_samples;
    uint32_t max_color_attachments;
    uint32_t max_vertex_attributes;
    uint32_t max_texture_size_2d;
    uint32_t max_texture_size_3d;
    uint32_t max_texture_size_cube;
    uint32_t max_array_layers;
    uint32_t max_mip_levels_2d;
    uint32_t max_threadgroup_memory_bytes;
    uint32_t max_threads_per_threadgroup;
    uint32_t thread_execution_width;
    uint32_t iosurface_readonly_align_bytes;
    uint32_t sparse_tile_bytes;
    uint32_t argument_buffers_tier;
    uint32_t read_write_texture_tier;
    uint64_t recommended_max_working_set_bytes;
    uint64_t max_buffer_length;
    bool has_unified_memory;
    bool supports_raster_order_groups;
    bool supports_programmable_sample_positions;
    bool supports_32bit_float_filtering;
    bool supports_32bit_msaa;
    bool supports_query_texture_lod;
    bool supports_bc_compression;
    bool supports_raytracing;
    bool supports_barycentrics;
    bool supports_depth24_stencil8;
} A840SysDeviceCaps;

/* IOSurface descriptor passed from ObjC or Linux host harness. */
typedef struct A840SysIOSurfaceDesc {
    uint64_t iosurface_id;
    uint32_t plane_index;
    uint32_t mtl_pixel_format;
    A840YcbcrFormat ycbcr_format;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_row;
    uint64_t alloc_size;
    uint32_t usage_flags;
    bool is_cross_process;
    bool for_scanout;
    bool request_ubwc6;
    bool framebuffer_only;
    bool external_backing;
    void *cpu_base;
    const A840MdssScanoutCaps *mdss_caps;
} A840SysIOSurfaceDesc;

typedef struct A840SysSurfaceInfo {
    A840SysSurfaceHandle handle;
    uint64_t iosurface_id;
    uint32_t plane_index;
    uint32_t mtl_pixel_format;
    A840YcbcrFormat ycbcr_format;
    uint32_t width;
    uint32_t height;
    uint32_t pitch_bytes;
    uint32_t plane_count;
    uint32_t tile_mode;
    uint32_t ubwc_version;
    uint32_t highest_bank_bit;
    uint32_t swizzle_levels;
    uint32_t macrotile_mode;
    uint64_t gpu_va;
    uint64_t bo_handle;
    uint64_t generation;
    uint32_t active_use_count;
    bool is_cross_process;
    bool for_scanout;
    bool scanout_eligible;
    bool ubwc_enabled;
    bool fallback_linear_required;
    struct NativeAdrenoAbiSurfaceMetaV3 surface_meta;
} A840SysSurfaceInfo;

/* SharedEvent listener callback invoked strictly OUTSIDE internal mutex. */
typedef void (*A840SysEventNotifyFn)(void *ctx, A840SysEventHandle event, uint64_t value);

/* Scanout handoff callback invoked strictly OUTSIDE internal mutex. */
typedef void (*A840SysScanoutNotifyFn)(void *ctx,
                                       const A840EarlyPromiseEvent *promise_event,
                                       const struct NativeAdrenoAbiWriterPromiseV3 *abi_v3);

typedef struct A840SysFrontendConfig {
    uint64_t client_id;
    uint64_t registry_id;
    A840SysProcRole proc_role;
    bool strict_reject_app_early;
    uint32_t arena_frame_count;
    size_t arena_per_frame_bytes;
    uint32_t cmd_slot_count;
    uint32_t cmd_slot_dwords;
    uint32_t chunk_max_passes;
    uint32_t chunk_max_dwords;
    uint32_t chunk_max_est_gpu_us;
    uint64_t pack_stack_key;
    uint64_t pack_abi_fingerprint;
    A840SyncMode sync_mode;
    const A840ArenaAllocator *arena_allocator;
    A840SysScanoutNotifyFn scanout_notify_fn;
    void *scanout_notify_ctx;
} A840SysFrontendConfig;

typedef struct A840SysGlassPassInput {
    uint32_t pass_count;                  /* 1..A840_CA_MAX_GLASS_PASSES */
    bool uses_fbfetch;
    bool has_memoryless_depth;
    bool force_sysmem;
    bool force_96x32_tiles;
    uint32_t sample_count;                /* 1, 2, or 4 */
    A840SysSurfaceHandle color_surface;
    A840SysSurfaceHandle resolve_surface; /* Optional 1x resolve surface */
    A840SysDepthStencilHandle depth_stencil;
    uint32_t clear_color[4];
    uint32_t est_gpu_us;
    uint32_t est_cpu_us;
    bool emit_early_promise;
} A840SysGlassPassInput;

typedef struct A840SysComputePassInput {
    uint32_t groups_x;
    uint32_t groups_y;
    uint32_t groups_z;
    uint32_t local_x;
    uint32_t local_y;
    uint32_t local_z;
    bool indirect_dispatch;
    uint64_t indirect_gpu_va;
    uint64_t indirect_bo_handle;
    const uint64_t *read_resource_ids;
    size_t read_count;
    const uint64_t *write_resource_ids;
    size_t write_count;
    uint32_t est_gpu_us;
    uint32_t est_cpu_us;
} A840SysComputePassInput;

typedef struct A840SysCamera155PassInput {
    A840SysSurfaceHandle preview_ycbcr_surface;
    A840SysSurfaceHandle photo_bgra8_surface; /* Optional 4096x3072 photo surface */
    bool encode_photo_capture;
    uint32_t est_gpu_us;
    uint32_t est_cpu_us;
} A840SysCamera155PassInput;

typedef struct A840SysScanoutRequest {
    A840SysSurfaceHandle surface;
    A840SysSurfaceHandle linear_dst_surface; /* Optional for NATIVE_LINEAR resolve */
    A840SysScanoutMode mode;
    uint64_t explicit_generation;
    bool request_early_promise;
} A840SysScanoutRequest;

typedef struct A840SysFrameReceipt {
    uint64_t frame_id;
    uint64_t cb_id;
    uint32_t ring_id;
    uint32_t chunks_kicked;
    uint32_t early_kicks;
    uint32_t passes_encoded;
    uint32_t gmem_passes;
    uint32_t sysmem_passes;
    uint32_t memoryless_depth_passes;
    uint32_t fbfetch_draws;
    uint32_t tile_width;
    uint32_t tile_height;
    uint32_t tiles_x;
    uint32_t tiles_y;
    uint32_t mdss_direct_ubwc;
    uint32_t mdss_native_linear;
    uint32_t pm4_dwords_captured;
    uint32_t pm4_chunks_truncated;
    uint64_t last_chunk_seqno;
    uint64_t wfi_elided;
    uint64_t ccu_flush_elided;
    uint64_t uche_inv_elided;
    uint64_t early_promises_published;
    uint64_t deferred_promises_armed;
    uint64_t retired_promises_ready;
    uint64_t frame_arena_used_bytes;
} A840SysFrameReceipt;

typedef struct A840SysFrontendStats {
    uint64_t frames_begun;
    uint64_t frames_committed;
    uint64_t frames_retired;
    uint64_t surfaces_bound;
    uint64_t surfaces_unbound;
    uint64_t heaps_created;
    uint64_t heap_alias_barriers_emitted;
    uint64_t events_created;
    uint64_t event_signals;
    uint64_t icbs_created;
    uint64_t icbs_executed;
    uint64_t depth_stencils_created;
    uint64_t archives_created;
    uint64_t gmem_passes_total;
    uint64_t sysmem_passes_total;
    uint64_t fbfetch_draws_total;
    uint64_t camera155_passes_total;
    uint64_t scanout_handoffs_total;
    uint64_t fallback_attempts_denied;
    uint64_t chunks_kicked_total;
    uint64_t early_kicks_total;
    uint64_t wfi_elided_total;
    uint64_t ccu_flush_elided_total;
    uint64_t uche_inv_elided_total;
} A840SysFrontendStats;

typedef struct A840SysFrontend A840SysFrontend;

/* ---------- 1. Process Classification, Publish Marker & Truthful Caps ---------- */

A840SysProcRole a840_sys_classify_process(const char *proc_name);

A840SysStatus a840_sys_resolve_publish_marker(const char *bundle_exec_path,
                                              const char *proc_name,
                                              bool marker_readable,
                                              A840SysPublishPolicy *out_policy);

void a840_sys_query_caps(const A840RtDevice *rt_device, A840SysDeviceCaps *out_caps);

/* Honest Apple GPU family / sample count queries per Adreno840CapsContract.md.
 * apple_family_id matches MTLGPUFamilyApple1 (1001)..Apple9 (1009) and
 * MTLGPUFamilyCommon1 (3001)..Common3 (3003). */
bool a840_sys_supports_gpu_family(uint32_t mtl_gpu_family);
bool a840_sys_supports_sample_count(uint32_t sample_count);
bool a840_sys_supports_pixel_format(uint32_t mtl_pixel_format);

/* ---------- 2. System Frontend Lifecycle ---------- */

void a840_sys_frontend_default_config(A840SysFrontendConfig *cfg, A840SysProcRole role);

A840SysStatus a840_sys_frontend_create(A840RtDevice *rt_device,
                                       const A840SysFrontendConfig *cfg,
                                       A840SysFrontend **out_fe);

void a840_sys_frontend_destroy(A840SysFrontend *fe);

A840SysProcRole a840_sys_frontend_role(const A840SysFrontend *fe);
A840ProdRouter *a840_sys_frontend_router(A840SysFrontend *fe);
A840PackCacheRt *a840_sys_frontend_pack_cache(A840SysFrontend *fe);
A840SysFrontendStats a840_sys_frontend_stats(A840SysFrontend *fe);

/* Permanently forbidden in Wave 12 zero-fallback system stack. */
A840SysStatus a840_sys_frontend_request_fallback(A840SysFrontend *fe);

/* ---------- 3. IOSurface Zero-Copy Binding State Machine ---------- */

A840SysStatus a840_sys_bind_iosurface(A840SysFrontend *fe,
                                      const A840SysIOSurfaceDesc *desc,
                                      A840SysSurfaceHandle *out_handle,
                                      A840SysSurfaceInfo *out_info);

A840SysStatus a840_sys_query_surface(A840SysFrontend *fe,
                                     A840SysSurfaceHandle handle,
                                     A840SysSurfaceInfo *out_info);

A840UbwcTexture *a840_sys_surface_ubwc_texture(A840SysFrontend *fe,
                                               A840SysSurfaceHandle handle);

/* Explicit per-CB IOSurface use-count acquire/release (prevents Camera pool leak). */
A840SysStatus a840_sys_surface_increment_use(A840SysFrontend *fe, A840SysSurfaceHandle handle);
A840SysStatus a840_sys_surface_decrement_use(A840SysFrontend *fe, A840SysSurfaceHandle handle);

A840SysStatus a840_sys_unbind_iosurface(A840SysFrontend *fe, A840SysSurfaceHandle handle);

/* ---------- 4. Unified Handle Tables: Heap / SharedEvent / ICB / DepthStencil / Archive ---------- */

A840SysStatus a840_sys_heap_create(A840SysFrontend *fe,
                                   const A840HeapDesc *desc,
                                   A840SysHeapHandle *out_handle);
A840Heap *a840_sys_heap_resolve(A840SysFrontend *fe, A840SysHeapHandle handle);
A840SysStatus a840_sys_heap_destroy(A840SysFrontend *fe, A840SysHeapHandle handle);

A840SysStatus a840_sys_event_create(A840SysFrontend *fe,
                                    bool is_shared,
                                    uint64_t initial_value,
                                    uint32_t mach_port_id,
                                    A840SysEventHandle *out_handle);
A840SysStatus a840_sys_event_signal(A840SysFrontend *fe,
                                    A840SysEventHandle handle,
                                    uint64_t value);
A840SysStatus a840_sys_event_query(A840SysFrontend *fe,
                                   A840SysEventHandle handle,
                                   uint64_t *out_value,
                                   bool *out_is_shared,
                                   uint32_t *out_mach_port_id);
A840SysStatus a840_sys_event_register_waiter(A840SysFrontend *fe,
                                             A840SysEventHandle handle,
                                             uint64_t target_value,
                                             A840SysEventNotifyFn fn,
                                             void *ctx);
A840SysStatus a840_sys_event_destroy(A840SysFrontend *fe, A840SysEventHandle handle);

A840SysStatus a840_sys_icb_create(A840SysFrontend *fe,
                                  const A840IcbDesc *desc,
                                  A840SysIcbHandle *out_handle);
A840IndirectCmdBuffer *a840_sys_icb_resolve(A840SysFrontend *fe, A840SysIcbHandle handle);
A840SysStatus a840_sys_icb_destroy(A840SysFrontend *fe, A840SysIcbHandle handle);

A840SysStatus a840_sys_depth_stencil_create(A840SysFrontend *fe,
                                            const A840DepthStencilVariantDesc *desc,
                                            A840SysDepthStencilHandle *out_handle);
A840SysStatus a840_sys_depth_stencil_get(A840SysFrontend *fe,
                                         A840SysDepthStencilHandle handle,
                                         A840DepthStencilVariantDesc *out_desc);
A840SysStatus a840_sys_depth_stencil_destroy(A840SysFrontend *fe,
                                             A840SysDepthStencilHandle handle);

A840SysStatus a840_sys_archive_create(A840SysFrontend *fe,
                                      uint64_t stack_key,
                                      uint64_t abi_fingerprint,
                                      A840SysArchiveHandle *out_handle);
A840SysStatus a840_sys_archive_add_key(A840SysFrontend *fe,
                                       A840SysArchiveHandle handle,
                                       const A840PipelineKey *key);
A840SysStatus a840_sys_archive_contains_key(A840SysFrontend *fe,
                                            A840SysArchiveHandle handle,
                                            const A840PipelineKey *key,
                                            bool *out_found);
A840SysStatus a840_sys_archive_count(A840SysFrontend *fe,
                                     A840SysArchiveHandle handle,
                                     uint32_t *out_count);
A840SysStatus a840_sys_archive_destroy(A840SysFrontend *fe, A840SysArchiveHandle handle);

/* ---------- 5. Unified Frame Pipeline (GMEM TBDR + Sync + Chunking + Early Promise) ---------- */

/* Begin a unified frame. Optional pm4_capture_buf receives the exact concatenated
 * A8XX PM4 stream kicked across all chunks in this frame (for PM4 audit / submission). */
A840SysStatus a840_sys_begin_frame(A840SysFrontend *fe,
                                   uint64_t frame_id,
                                   uint64_t cb_id,
                                   uint32_t *pm4_capture_buf,
                                   size_t pm4_capture_cap_dwords);

/* Encode a RenderPass (GMEM 18 MiB TBDR + optional Memoryless Depth + optional
 * FBFetch Liquid Glass multi-pass + optional MSAA 4x Resolve + DepthStencil). */
A840SysStatus a840_sys_encode_glass_gmem_pass(A840SysFrontend *fe,
                                              const A840SysGlassPassInput *input);

/* Encode a Compute pass (supports regular or indirect dispatch + sync tracking). */
A840SysStatus a840_sys_encode_compute_pass(A840SysFrontend *fe,
                                           const A840SysComputePassInput *input);

/* Encode a Camera 155 NV12/P010 preview + optional 4096x3072 photo pass. */
A840SysStatus a840_sys_encode_camera155_pass(A840SysFrontend *fe,
                                             const A840SysCamera155PassInput *input);

/* Encode a Heap aliasing transition barrier (CCU/UCHE + UBWC metadata invalidation). */
A840SysStatus a840_sys_encode_heap_alias_barrier(A840SysFrontend *fe,
                                                 const A840HeapAliasTransition *transition);

/* Encode compiled ICB playback into the active frame stream. */
A840SysStatus a840_sys_encode_icb_execute(A840SysFrontend *fe,
                                          A840SysIcbHandle icb_handle,
                                          uint32_t start,
                                          uint32_t count,
                                          A840RtBuffer *scratch_ib_bo);

/* Encode scanout handoff (UBWC 6.0 direct or native linear resolve) and schedule
 * Early Writer Promise (RenderServer) or Defer-Until-Retired (App). */
A840SysStatus a840_sys_encode_scanout_handoff(A840SysFrontend *fe,
                                              const A840SysScanoutRequest *req);

/* Commit final chunk, collect receipt, and release per-frame surface references
 * upon retirement. */
A840SysStatus a840_sys_commit_frame(A840SysFrontend *fe,
                                    A840SysFrameReceipt *out_receipt);

/* Retire GPU completion up to completed_chunk_seqno, advancing APP deferred
 * promises to RETIRED_READY and resetting the active frame arena slice. */
A840SysStatus a840_sys_retire_frame(A840SysFrontend *fe,
                                    uint64_t frame_id,
                                    uint64_t completed_chunk_seqno,
                                    A840SysFrameReceipt *io_receipt);

/* Abort an active frame (e.g. GPU fault / reset), canceling unretired promises. */
void a840_sys_abort_frame(A840SysFrontend *fe, A840EarlyPromiseCancelReason reason);

#ifdef __cplusplus
}
#endif

#endif /* A840_SYSTEM_FRONTEND_CORE_H */
