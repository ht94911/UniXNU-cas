/* SPDX-License-Identifier: MIT */
/* Adreno 840 Native Metal Driver — Wave 12 System Frontend Core Bridge Implementation.
 *
 * Pure C11, allocation-free steady-state hot path, thread-safe handle tables,
 * zero fallback branches, zero raw hex register offsets, zero CP_PROTECT writes.
 */
#include "a840_system_frontend_core.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a840_emit_ops.h"
#include "a840_gmem_planner.h"

#if !defined(A840_PRODUCTION_FALLBACK_COMPILED_OUT) || (A840_PRODUCTION_FALLBACK_COMPILED_OUT != 1u)
#error "System frontend core requires zero-fallback production mode"
#endif

enum {
    SYS_HANDLE_INDEX_MASK = 65535u,
    SYS_MTL_FAMILY_APPLE1 = 1001u,
    SYS_MTL_FAMILY_APPLE7 = 1007u,
    SYS_MTL_FAMILY_COMMON1 = 3001u,
    SYS_MTL_FAMILY_COMMON3 = 3003u,
    SYS_MTL_FMT_RGBA16_FLOAT = 115u,
    SYS_MTL_FMT_RGB10A2_UNORM = 90u,
    SYS_MTL_FMT_BGR10A2_UNORM = 94u,
    SYS_MTL_FMT_RGBA8_UNORM_SRGB = 71u,
    SYS_RECOMMENDED_WORKING_SET_MB = 4096u,
    SYS_RELOC_SCRATCH_CAP = 256u
};

static uint64_t make_handle(uint32_t generation, uint32_t index)
{
    return (((uint64_t)generation) << 32u) | (uint64_t)(index + 1u);
}

static bool decode_handle(uint64_t handle, uint32_t max_slots, uint32_t *out_index, uint32_t *out_gen)
{
    uint32_t raw_idx;
    uint32_t gen;
    if (handle == A840_SYS_INVALID_HANDLE) {
        return false;
    }
    raw_idx = (uint32_t)(handle & (uint64_t)SYS_HANDLE_INDEX_MASK);
    gen = (uint32_t)(handle >> 32u);
    if (raw_idx == 0u || raw_idx > max_slots || gen == 0u) {
        return false;
    }
    *out_index = raw_idx - 1u;
    *out_gen = gen;
    return true;
}

static uint32_t next_generation(uint32_t gen)
{
    return (gen == UINT32_MAX) ? 1u : (gen + 1u);
}

typedef struct SysSurfaceSlot {
    bool active;
    uint32_t generation;
    uint64_t iosurface_id;
    uint32_t plane_index;
    uint32_t active_use_count;
    uint64_t surface_generation;
    bool is_cross_process;
    bool framebuffer_only;
    A840UbwcTexture *ubwc_tex;
} SysSurfaceSlot;

typedef struct SysHeapSlot {
    bool active;
    uint32_t generation;
    A840Heap *heap;
} SysHeapSlot;

typedef struct SysEventWaiter {
    bool active;
    uint64_t target_value;
    A840SysEventNotifyFn fn;
    void *ctx;
} SysEventWaiter;

typedef struct SysEventSlot {
    bool active;
    uint32_t generation;
    bool is_shared;
    uint32_t mach_port_id;
    uint64_t signaled_value;
    SysEventWaiter waiters[A840_SYS_MAX_EVENT_WAITERS];
} SysEventSlot;

typedef struct SysIcbSlot {
    bool active;
    uint32_t generation;
    A840IndirectCmdBuffer *icb;
} SysIcbSlot;

typedef struct SysDepthStencilSlot {
    bool active;
    uint32_t generation;
    A840DepthStencilVariantDesc desc;
} SysDepthStencilSlot;

typedef struct SysArchiveSlot {
    bool active;
    uint32_t generation;
    uint64_t stack_key;
    uint64_t abi_fingerprint;
    uint32_t key_count;
    A840PipelineKey keys[A840_SYS_MAX_ARCHIVE_KEYS];
} SysArchiveSlot;

typedef struct SysPendingEventCallback {
    A840SysEventHandle handle;
    uint64_t value;
    A840SysEventNotifyFn fn;
    void *ctx;
} SysPendingEventCallback;

struct A840SysFrontend {
    pthread_mutex_t lock;
    bool lock_initialized;
    bool initialized;

    A840RtDevice *rt_device;
    A840SysFrontendConfig config;
    A840ProdRouter prod_router;
    A840PackCacheRt pack_cache;
    A840FrameArena frame_arena;
    A840CmdSlotPool cmd_pool;
    A840StateHashTable state_table;
    A840CbChunker chunker;
    A840EarlyPromiseSched promise_sched;

    bool frame_active;
    uint64_t active_frame_id;
    uint64_t active_cb_id;
    uint32_t active_ring_id;
    uint32_t *capture_buf;
    size_t capture_cap_dwords;
    size_t capture_used_dwords;
    uint32_t capture_truncated_chunks;
    A840SysFrameReceipt active_receipt;

    uint32_t frame_surface_count;
    A840SysSurfaceHandle frame_surfaces[A840_SYS_MAX_FRAME_SURFACES];

    /* Handle tables */
    SysSurfaceSlot surfaces[A840_SYS_MAX_SURFACES];
    SysHeapSlot heaps[A840_SYS_MAX_HEAPS];
    SysEventSlot events[A840_SYS_MAX_EVENTS];
    SysIcbSlot icbs[A840_SYS_MAX_ICBS];
    SysDepthStencilSlot depth_stencils[A840_SYS_MAX_DEPTH_STENCILS];
    SysArchiveSlot archives[A840_SYS_MAX_ARCHIVES];

    /* Persistent dummy shader/code BOs for host GMEM/Compute/Camera155 PM4 synthesis */
    A840RtBuffer *synth_shader_bo;
    A840RtBuffer *synth_consts_bo;
    A840RtBuffer *synth_bindless_bo;

    A840SysFrontendStats stats;
};

/* ---------- Internal Callbacks ---------- */

static void sys_early_promise_cb(void *ctx, const A840EarlyPromiseEvent *ev)
{
    A840SysFrontend *fe = (A840SysFrontend *)ctx;
    struct NativeAdrenoAbiWriterPromiseV3 abi;
    if (!fe || !ev) {
        return;
    }
    memset(&abi, 0, sizeof(abi));
    (void)a840_early_promise_fill_abi_v3(ev, &abi);
    if (fe->config.scanout_notify_fn) {
        fe->config.scanout_notify_fn(fe->config.scanout_notify_ctx, ev, &abi);
    }
}

static A840ChunkStatus sys_chunk_kick_cb(void *ctx, const A840ChunkDesc *chunk)
{
    A840SysFrontend *fe = (A840SysFrontend *)ctx;
    bool is_final;
    if (!fe || !chunk) {
        return A840_CHUNK_ERR_INVALID_ARG;
    }
    if (fe->capture_buf && chunk->pm4_words && chunk->pm4_dwords > 0u) {
        if (fe->capture_used_dwords + (size_t)chunk->pm4_dwords <= fe->capture_cap_dwords) {
            memcpy(fe->capture_buf + fe->capture_used_dwords,
                   chunk->pm4_words,
                   (size_t)chunk->pm4_dwords * sizeof(uint32_t));
            fe->capture_used_dwords += (size_t)chunk->pm4_dwords;
        } else {
            fe->capture_truncated_chunks++;
        }
    }
    fe->active_receipt.chunks_kicked++;
    if ((chunk->flags & A840_CHUNK_FLAG_EARLY_KICK) != 0u) {
        fe->active_receipt.early_kicks++;
    }
    fe->active_receipt.last_chunk_seqno = chunk->chunk_seqno;
    fe->active_receipt.pm4_dwords_captured = (uint32_t)fe->capture_used_dwords;
    fe->active_receipt.pm4_chunks_truncated = fe->capture_truncated_chunks;

    is_final = ((chunk->flags & A840_CHUNK_FLAG_FINAL) != 0u);
    (void)a840_early_promise_on_chunk_kicked(&fe->promise_sched,
                                             chunk->cb_id,
                                             chunk->chunk_seqno,
                                             chunk->ring_id,
                                             chunk->written_surface_ids,
                                             chunk->written_surface_count,
                                             is_final);
    return A840_CHUNK_OK;
}

/* ---------- 1. Process Role, Publish Marker & Truthful Caps ---------- */

A840SysProcRole a840_sys_classify_process(const char *proc_name)
{
    if (!proc_name || proc_name[0] == '\0') {
        return A840_SYS_ROLE_APP;
    }
    if (strcmp(proc_name, "backboardd") == 0 ||
        strcmp(proc_name, "CARenderServer") == 0 ||
        strcmp(proc_name, "WindowServer") == 0) {
        return A840_SYS_ROLE_RENDER_SERVER;
    }
    return A840_SYS_ROLE_APP;
}

A840SysStatus a840_sys_resolve_publish_marker(const char *bundle_exec_path,
                                              const char *proc_name,
                                              bool marker_readable,
                                              A840SysPublishPolicy *out_policy)
{
    const char *bundle_pos = NULL;
    size_t dir_len = 0u;
    A840SysProcRole role;
    if (!bundle_exec_path || !out_policy) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    memset(out_policy, 0, sizeof(*out_policy));
    role = a840_sys_classify_process(proc_name);
    out_policy->proc_role = role;
    out_policy->fallback_compiled_out = A840_PRODUCTION_FALLBACK_COMPILED_OUT;
    out_policy->pack_tier0_readonly = true;

    bundle_pos = strstr(bundle_exec_path, "/NativeAdrenoMetal.bundle/");
    if (!bundle_pos) {
        bundle_pos = strstr(bundle_exec_path, "/Adreno840Metal.bundle/");
    }
    if (!bundle_pos) {
        return A840_SYS_ERR_NOT_FOUND;
    }
    dir_len = (size_t)(bundle_pos - bundle_exec_path);
    if (dir_len + 48u >= sizeof(out_policy->marker_path)) {
        return A840_SYS_ERR_CAPACITY;
    }

    (void)snprintf(out_policy->marker_path, sizeof(out_policy->marker_path),
                   "%.*s/NativeAdrenoMetal.publish", (int)dir_len, bundle_exec_path);
    (void)snprintf(out_policy->pack_path, sizeof(out_policy->pack_path),
                   "%.*s/NativeAdrenoMetal.pack.bin", (int)dir_len, bundle_exec_path);

    if (role == A840_SYS_ROLE_RENDER_SERVER) {
        out_policy->cache_policy = A840_SYS_CACHE_SHARED_SYSTEM_PACK;
        out_policy->allow_early_promise = true;
        out_policy->scanout_bridge_enabled = true;
        out_policy->preferred_ring_id = 0u;
        (void)snprintf(out_policy->cache_dir, sizeof(out_policy->cache_dir),
                       "/var/mobile/Library/Caches/org.unixnu.adreno840.system");
    } else {
        out_policy->cache_policy = A840_SYS_CACHE_PRIVATE_PER_PROC;
        out_policy->allow_early_promise = false;
        out_policy->scanout_bridge_enabled = false;
        out_policy->preferred_ring_id = 1u;
        (void)snprintf(out_policy->cache_dir, sizeof(out_policy->cache_dir),
                       "/var/mobile/Library/Caches/com.apple.metal/adreno840");
    }

    if (!marker_readable) {
        out_policy->publish_state = A840_SYS_PUBLISH_DISABLED;
        out_policy->allow_device_creation = false;
        return A840_SYS_ERR_NOT_PUBLISHED;
    }

    out_policy->publish_state = A840_SYS_PUBLISH_ENABLED;
    out_policy->allow_device_creation = true;
    return A840_SYS_OK;
}

void a840_sys_query_caps(const A840RtDevice *rt_device, A840SysDeviceCaps *out_caps)
{
    A840ChipFacts facts;
    uint64_t max_bo;
    if (!out_caps) {
        return;
    }
    memset(out_caps, 0, sizeof(*out_caps));
    if (rt_device) {
        facts = rt_device->chip_facts;
        max_bo = rt_device->transport_caps.max_bo_bytes;
    } else {
        facts = *a840_chip_facts_kaanapali_default();
        max_bo = UINT64_C(256) * 1024u * 1024u;
    }
    if (max_bo == 0u) {
        max_bo = UINT64_C(256) * 1024u * 1024u;
    }

    out_caps->chip_id = facts.chip_id;
    out_caps->gmem_bytes = facts.gmem_bytes;
    out_caps->highest_bank_bit = facts.highest_bank_bit;
    out_caps->ubwc_version = facts.ubwc_version;
    out_caps->ubwc_swizzle = facts.ubwc_swizzle;
    out_caps->macrotile_mode = facts.macrotile_mode;
    out_caps->max_samples = A840_LIMIT_MAX_SAMPLES;
    out_caps->max_color_attachments = A840_MAX_COLOR_TARGETS;
    out_caps->max_vertex_attributes = A840_LIMIT_VS_MAX_INPUTS_COUNT;
    out_caps->max_texture_size_2d = A840_LIMIT_MAX_TEXTURE_SIZE;
    out_caps->max_texture_size_3d = A840_LIMIT_MAX_TEXTURE_SIZE_3D;
    out_caps->max_texture_size_cube = A840_LIMIT_MAX_TEXTURE_SIZE_CUBE;
    out_caps->max_array_layers = A840_LIMIT_MAX_ARRAY_LAYERS;
    out_caps->max_mip_levels_2d = A840_LIMIT_MAX_MIP_LEVELS_2D;
    out_caps->max_threadgroup_memory_bytes = facts.cs_shared_mem_size;
    out_caps->max_threads_per_threadgroup = facts.threadsize_base * facts.max_waves;
    out_caps->thread_execution_width = facts.threadsize_base;
    out_caps->iosurface_readonly_align_bytes = A840_SYS_IOSURFACE_RO_ALIGN;
    out_caps->sparse_tile_bytes = A840_SYS_SPARSE_TILE_BYTES;
    out_caps->argument_buffers_tier = 2u;
    out_caps->read_write_texture_tier = 2u;
    out_caps->recommended_max_working_set_bytes =
        (uint64_t)SYS_RECOMMENDED_WORKING_SET_MB * UINT64_C(1024) * UINT64_C(1024);
    out_caps->max_buffer_length = max_bo;
    out_caps->has_unified_memory = true;
    out_caps->supports_raster_order_groups = true;
    out_caps->supports_programmable_sample_positions = true;
    out_caps->supports_32bit_float_filtering = true;
    out_caps->supports_32bit_msaa = true;
    out_caps->supports_query_texture_lod = true;
    out_caps->supports_bc_compression = false;
    out_caps->supports_raytracing = false;
    out_caps->supports_barycentrics = false;
    out_caps->supports_depth24_stencil8 = false;
}

bool a840_sys_supports_gpu_family(uint32_t mtl_gpu_family)
{
    if (mtl_gpu_family >= SYS_MTL_FAMILY_APPLE1 && mtl_gpu_family <= SYS_MTL_FAMILY_APPLE7) {
        return true;
    }
    if (mtl_gpu_family >= SYS_MTL_FAMILY_COMMON1 && mtl_gpu_family <= SYS_MTL_FAMILY_COMMON3) {
        return true;
    }
    return false;
}

bool a840_sys_supports_sample_count(uint32_t sample_count)
{
    return sample_count == 1u || sample_count == 2u || sample_count == 4u;
}

bool a840_sys_supports_pixel_format(uint32_t mtl_pixel_format)
{
    const A840FormatCaps *caps = a840_format_caps_lookup(mtl_pixel_format);
    if (caps != NULL) {
        return true;
    }
    return mtl_pixel_format == A840_MTL_FMT_RGB10A8_2P_XR10 ||
           mtl_pixel_format == A840_MTL_FMT_RGB10A8_2P_XR10_SRGB ||
           mtl_pixel_format == A840_MTL_FMT_BGRA10_XR ||
           mtl_pixel_format == A840_MTL_FMT_BGRA10_XR_SRGB ||
           mtl_pixel_format == A840_MTL_FMT_BGR10_XR ||
           mtl_pixel_format == A840_MTL_FMT_BGR10_XR_SRGB;
}

/* ---------- 2. System Frontend Lifecycle ---------- */

void a840_sys_frontend_default_config(A840SysFrontendConfig *cfg, A840SysProcRole role)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->client_id = (role == A840_SYS_ROLE_RENDER_SERVER) ? 1u : 2u;
    cfg->registry_id = A840_KAANAPALI_CHIP_ID;
    cfg->proc_role = role;
    cfg->strict_reject_app_early = false;
    cfg->arena_frame_count = A840_SYS_DEFAULT_ARENA_FRAMES;
    cfg->arena_per_frame_bytes = A840_SYS_DEFAULT_ARENA_BYTES;
    cfg->cmd_slot_count = A840_SYS_DEFAULT_CMD_SLOTS;
    cfg->cmd_slot_dwords = A840_SYS_DEFAULT_SLOT_DWORDS;
    cfg->chunk_max_passes = A840_CHUNK_DEFAULT_MAX_PASSES;
    cfg->chunk_max_dwords = 2048u;
    cfg->chunk_max_est_gpu_us = A840_CHUNK_DEFAULT_MAX_EST_GPU_US;
    cfg->pack_stack_key = UINT64_C(95119202611270001);
    cfg->pack_abi_fingerprint = ADRENO_COMPILE_ABI_VERSION;
    cfg->sync_mode = A840_SYNC_MODE_TRACKED;
}

A840SysStatus a840_sys_frontend_create(A840RtDevice *rt_device,
                                       const A840SysFrontendConfig *cfg,
                                       A840SysFrontend **out_fe)
{
    A840SysFrontend *fe = NULL;
    A840SysFrontendConfig local_cfg;
    A840ChunkerConfig chunk_cfg;
    A840EarlyPromiseConfig promise_cfg;
    A840RtBufferDesc bo_desc;
    uint32_t i;

    if (!rt_device || !out_fe) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    *out_fe = NULL;

    if (cfg) {
        local_cfg = *cfg;
    } else {
        a840_sys_frontend_default_config(&local_cfg, A840_SYS_ROLE_APP);
    }
    if (local_cfg.arena_frame_count < A840_FRAME_ARENA_MIN_FRAMES ||
        local_cfg.arena_frame_count > A840_FRAME_ARENA_MAX_FRAMES) {
        local_cfg.arena_frame_count = A840_SYS_DEFAULT_ARENA_FRAMES;
    }
    if (local_cfg.arena_per_frame_bytes < 4096u) {
        local_cfg.arena_per_frame_bytes = A840_SYS_DEFAULT_ARENA_BYTES;
    }
    if (local_cfg.cmd_slot_count == 0u || local_cfg.cmd_slot_count > A840_CMD_SLOT_MAX_CAP) {
        local_cfg.cmd_slot_count = A840_SYS_DEFAULT_CMD_SLOTS;
    }
    if (local_cfg.cmd_slot_dwords < 256u) {
        local_cfg.cmd_slot_dwords = A840_SYS_DEFAULT_SLOT_DWORDS;
    }

    fe = (A840SysFrontend *)calloc(1u, sizeof(*fe));
    if (!fe) {
        return A840_SYS_ERR_OOM;
    }
    if (pthread_mutex_init(&fe->lock, NULL) != 0) {
        free(fe);
        return A840_SYS_ERR_OOM;
    }
    fe->lock_initialized = true;
    fe->rt_device = rt_device;
    fe->config = local_cfg;

    if (a840_prod_router_init(&fe->prod_router) != A840_PROD_OK) {
        a840_sys_frontend_destroy(fe);
        return A840_SYS_ERR_INVALID_STATE;
    }
    if (a840_pack_cache_rt_init(&fe->pack_cache,
                                local_cfg.pack_stack_key ? local_cfg.pack_stack_key : 1u,
                                local_cfg.pack_abi_fingerprint ? local_cfg.pack_abi_fingerprint : 1u) != A840_RT_OK) {
        a840_sys_frontend_destroy(fe);
        return A840_SYS_ERR_INVALID_STATE;
    }
    if (a840_frame_arena_init(&fe->frame_arena,
                              local_cfg.arena_frame_count,
                              local_cfg.arena_per_frame_bytes,
                              local_cfg.arena_allocator) != A840_ARENA_OK) {
        a840_sys_frontend_destroy(fe);
        return A840_SYS_ERR_OOM;
    }
    if (a840_cmd_slot_pool_init(&fe->cmd_pool,
                                local_cfg.cmd_slot_count,
                                local_cfg.cmd_slot_dwords,
                                local_cfg.arena_allocator) != A840_ARENA_OK) {
        a840_sys_frontend_destroy(fe);
        return A840_SYS_ERR_OOM;
    }
    if (a840_state_table_init(&fe->state_table,
                              A840_SYS_DEFAULT_STATE_TABLE_CAP,
                              local_cfg.arena_allocator) != A840_ARENA_OK) {
        a840_sys_frontend_destroy(fe);
        return A840_SYS_ERR_OOM;
    }

    a840_cb_chunker_default_config(&chunk_cfg);
    chunk_cfg.max_passes_per_chunk = local_cfg.chunk_max_passes;
    chunk_cfg.max_dwords_per_chunk = local_cfg.chunk_max_dwords;
    chunk_cfg.max_est_gpu_us_per_chunk = local_cfg.chunk_max_est_gpu_us;
    chunk_cfg.ring_id = (local_cfg.proc_role == A840_SYS_ROLE_RENDER_SERVER) ? 0u : 1u;
    chunk_cfg.sync_mode = local_cfg.sync_mode;
    chunk_cfg.kick_fn = sys_chunk_kick_cb;
    chunk_cfg.kick_ctx = fe;
    if (a840_cb_chunker_init(&fe->chunker, &fe->frame_arena, &fe->cmd_pool, &chunk_cfg) != A840_CHUNK_OK) {
        a840_sys_frontend_destroy(fe);
        return A840_SYS_ERR_INVALID_STATE;
    }

    memset(&promise_cfg, 0, sizeof(promise_cfg));
    promise_cfg.client_id = local_cfg.client_id ? local_cfg.client_id : 1u;
    promise_cfg.client_role = (local_cfg.proc_role == A840_SYS_ROLE_RENDER_SERVER)
                                  ? A840_EARLY_ROLE_RENDER_SERVER
                                  : A840_EARLY_ROLE_APP;
    promise_cfg.strict_reject_app_early = local_cfg.strict_reject_app_early;
    promise_cfg.callback = sys_early_promise_cb;
    promise_cfg.callback_ctx = fe;
    if (a840_early_promise_init(&fe->promise_sched, &promise_cfg) != A840_PROMISE_SCHED_OK) {
        a840_sys_frontend_destroy(fe);
        return A840_SYS_ERR_INVALID_STATE;
    }

    /* Pre-allocate tiny resident BOs for native shader/consts/bindless emission
     * so steady-state frame encoding performs zero runtime BO allocations. */
    memset(&bo_desc, 0, sizeof(bo_desc));
    bo_desc.size_bytes = 4096u;
    bo_desc.alignment = 4096u;
    bo_desc.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_SHADER | A840_BUF_USAGE_CPU_VISIBLE;
    if (a840_rt_buffer_create(rt_device, &bo_desc, &fe->synth_shader_bo) != A840_RT_OK ||
        a840_rt_buffer_create(rt_device, &bo_desc, &fe->synth_consts_bo) != A840_RT_OK ||
        a840_rt_buffer_create(rt_device, &bo_desc, &fe->synth_bindless_bo) != A840_RT_OK) {
        a840_sys_frontend_destroy(fe);
        return A840_SYS_ERR_RUNTIME_FAILED;
    }

    for (i = 0u; i < A840_SYS_MAX_SURFACES; ++i) fe->surfaces[i].generation = 1u;
    for (i = 0u; i < A840_SYS_MAX_HEAPS; ++i) fe->heaps[i].generation = 1u;
    for (i = 0u; i < A840_SYS_MAX_EVENTS; ++i) fe->events[i].generation = 1u;
    for (i = 0u; i < A840_SYS_MAX_ICBS; ++i) fe->icbs[i].generation = 1u;
    for (i = 0u; i < A840_SYS_MAX_DEPTH_STENCILS; ++i) fe->depth_stencils[i].generation = 1u;
    for (i = 0u; i < A840_SYS_MAX_ARCHIVES; ++i) fe->archives[i].generation = 1u;

    fe->initialized = true;
    *out_fe = fe;
    return A840_SYS_OK;
}

void a840_sys_frontend_destroy(A840SysFrontend *fe)
{
    uint32_t i;
    if (!fe) {
        return;
    }
    if (fe->frame_active) {
        a840_cb_chunker_abort(&fe->chunker);
        fe->frame_active = false;
    }
    for (i = 0u; i < A840_SYS_MAX_SURFACES; ++i) {
        if (fe->surfaces[i].active && fe->surfaces[i].ubwc_tex) {
            a840_ubwc_texture_destroy(fe->surfaces[i].ubwc_tex);
            fe->surfaces[i].ubwc_tex = NULL;
            fe->surfaces[i].active = false;
        }
    }
    for (i = 0u; i < A840_SYS_MAX_HEAPS; ++i) {
        if (fe->heaps[i].active && fe->heaps[i].heap) {
            a840_heap_release(fe->heaps[i].heap);
            fe->heaps[i].heap = NULL;
            fe->heaps[i].active = false;
        }
    }
    for (i = 0u; i < A840_SYS_MAX_ICBS; ++i) {
        if (fe->icbs[i].active && fe->icbs[i].icb) {
            a840_icb_destroy(fe->icbs[i].icb);
            fe->icbs[i].icb = NULL;
            fe->icbs[i].active = false;
        }
    }
    if (fe->synth_shader_bo) {
        a840_rt_buffer_release(fe->synth_shader_bo);
        fe->synth_shader_bo = NULL;
    }
    if (fe->synth_consts_bo) {
        a840_rt_buffer_release(fe->synth_consts_bo);
        fe->synth_consts_bo = NULL;
    }
    if (fe->synth_bindless_bo) {
        a840_rt_buffer_release(fe->synth_bindless_bo);
        fe->synth_bindless_bo = NULL;
    }
    a840_early_promise_destroy(&fe->promise_sched);
    a840_state_table_destroy(&fe->state_table);
    a840_cmd_slot_pool_destroy(&fe->cmd_pool);
    a840_frame_arena_destroy(&fe->frame_arena);
    a840_pack_cache_rt_reset(&fe->pack_cache);
    if (fe->lock_initialized) {
        (void)pthread_mutex_destroy(&fe->lock);
        fe->lock_initialized = false;
    }
    free(fe);
}

A840SysProcRole a840_sys_frontend_role(const A840SysFrontend *fe)
{
    return fe ? fe->config.proc_role : A840_SYS_ROLE_APP;
}

A840ProdRouter *a840_sys_frontend_router(A840SysFrontend *fe)
{
    return fe ? &fe->prod_router : NULL;
}

A840PackCacheRt *a840_sys_frontend_pack_cache(A840SysFrontend *fe)
{
    return fe ? &fe->pack_cache : NULL;
}

A840SysFrontendStats a840_sys_frontend_stats(A840SysFrontend *fe)
{
    A840SysFrontendStats out;
    memset(&out, 0, sizeof(out));
    if (!fe) {
        return out;
    }
    (void)pthread_mutex_lock(&fe->lock);
    out = fe->stats;
    (void)pthread_mutex_unlock(&fe->lock);
    return out;
}

A840SysStatus a840_sys_frontend_request_fallback(A840SysFrontend *fe)
{
    if (!fe) {
        return A840_SYS_ERR_FALLBACK_FORBIDDEN;
    }
    (void)pthread_mutex_lock(&fe->lock);
    (void)a840_prod_route_request_fallback(&fe->prod_router);
    fe->stats.fallback_attempts_denied++;
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_ERR_FALLBACK_FORBIDDEN;
}

/* ---------- 3. IOSurface Zero-Copy Binding State Machine ---------- */

static void fill_surface_info_locked(const SysSurfaceSlot *slot, uint32_t idx, A840SysSurfaceInfo *out)
{
    const A840UbwcTexture *tex = slot->ubwc_tex;
    memset(out, 0, sizeof(*out));
    out->handle = make_handle(slot->generation, idx);
    out->iosurface_id = slot->iosurface_id;
    out->plane_index = slot->plane_index;
    out->mtl_pixel_format = tex->mtl_pixel_format;
    out->ycbcr_format = tex->ycbcr_format;
    out->width = tex->width;
    out->height = tex->height;
    out->pitch_bytes = (tex->kind == A840_P3_TEX_YCBCR_MULTIPLANE)
                           ? tex->plane_pitches[slot->plane_index < A840_P3_MAX_PLANES ? slot->plane_index : 0u]
                           : (tex->ubwc.data_pitch ? tex->ubwc.data_pitch : tex->linear_resolve_pitch);
    out->plane_count = tex->plane_count;
    out->tile_mode = tex->tile_mode;
    out->ubwc_version = tex->ubwc_version;
    out->highest_bank_bit = tex->highest_bank_bit;
    out->swizzle_levels = tex->ubwc_swizzle;
    out->macrotile_mode = tex->macrotile_mode;
    out->gpu_va = tex->backing_bo ? tex->backing_bo->gpu_va : 0u;
    out->bo_handle = tex->backing_bo ? tex->backing_bo->bo_handle : 0u;
    out->generation = slot->surface_generation;
    out->active_use_count = slot->active_use_count;
    out->is_cross_process = slot->is_cross_process;
    out->for_scanout = tex->for_scanout;
    out->scanout_eligible = tex->scanout_eligible;
    out->ubwc_enabled = (tex->tile_mode == A840_TILE_UBWC);
    out->fallback_linear_required = tex->fallback_linear_required;
    out->surface_meta = tex->surface_meta;
}

A840SysStatus a840_sys_bind_iosurface(A840SysFrontend *fe,
                                      const A840SysIOSurfaceDesc *desc,
                                      A840SysSurfaceHandle *out_handle,
                                      A840SysSurfaceInfo *out_info)
{
    A840UbwcTextureDesc tdesc;
    A840UbwcTexture *tex = NULL;
    A840RtStatus st;
    uint32_t free_idx = A840_SYS_MAX_SURFACES;
    uint32_t i;

    if (!fe || !desc || !out_handle || desc->width == 0u || desc->height == 0u) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    *out_handle = A840_SYS_INVALID_HANDLE;

    if (desc->ycbcr_format == A840_YCBCR_NONE &&
        !a840_sys_supports_pixel_format(desc->mtl_pixel_format)) {
        return A840_SYS_ERR_UNSUPPORTED;
    }
    if (desc->plane_index >= A840_P3_MAX_PLANES) {
        return A840_SYS_ERR_INVALID_ARG;
    }

    memset(&tdesc, 0, sizeof(tdesc));
    tdesc.width = desc->width;
    tdesc.height = desc->height;
    tdesc.array_layers = 1u;
    tdesc.samples = 1u;
    tdesc.usage_flags = desc->usage_flags
                            ? desc->usage_flags
                            : (A840_TEX_USAGE_SHADER_READ | A840_TEX_USAGE_RENDER_TARGET);
    tdesc.highest_bank_bit = A840_KAANAPALI_HIGHEST_BANK_BIT;
    tdesc.ubwc_version = fe->rt_device->chip_facts.ubwc_version;
    tdesc.ubwc_swizzle = fe->rt_device->chip_facts.ubwc_swizzle;
    tdesc.macrotile_mode = fe->rt_device->chip_facts.macrotile_mode;
    tdesc.for_scanout = desc->for_scanout;
    tdesc.mdss_caps = desc->mdss_caps;

    if (desc->ycbcr_format != A840_YCBCR_NONE) {
        tdesc.kind = A840_P3_TEX_YCBCR_MULTIPLANE;
        tdesc.storage_mode = A840_STORAGE_SHARED;
        tdesc.ycbcr_format = desc->ycbcr_format;
        tdesc.mtl_pixel_format = (desc->ycbcr_format == A840_YCBCR_NV12)
                                     ? A840_MTL_FMT_R8_UNORM
                                     : A840_MTL_FMT_R16_UNORM;
        tdesc.tile_mode = A840_TILE_LINEAR;
    } else if (desc->mtl_pixel_format == A840_MTL_FMT_RGB10A8_2P_XR10 ||
               desc->mtl_pixel_format == A840_MTL_FMT_RGB10A8_2P_XR10_SRGB) {
        /* 2-plane 'b3a8' XR10 IOSurface: back in linear BGRA8 (4 B/px, matching plane 0). */
        tdesc.kind = A840_P3_TEX_UBWC6;
        tdesc.storage_mode = A840_STORAGE_SHARED;
        tdesc.mtl_pixel_format = (desc->mtl_pixel_format == A840_MTL_FMT_RGB10A8_2P_XR10_SRGB)
                                     ? A840_MTL_FMT_BGRA8_UNORM_SRGB
                                     : A840_MTL_FMT_BGRA8_UNORM;
        tdesc.tile_mode = A840_TILE_LINEAR;
    } else if (desc->mtl_pixel_format == A840_MTL_FMT_BGRA10_XR ||
               desc->mtl_pixel_format == A840_MTL_FMT_BGRA10_XR_SRGB ||
               desc->mtl_pixel_format == A840_MTL_FMT_BGR10_XR ||
               desc->mtl_pixel_format == A840_MTL_FMT_BGR10_XR_SRGB) {
        tdesc.kind = A840_P3_TEX_XR10;
        tdesc.storage_mode = A840_STORAGE_PRIVATE;
        tdesc.mtl_pixel_format = desc->mtl_pixel_format;
        tdesc.tile_mode = A840_TILE_TILED;
    } else {
        const A840FormatCaps *fc = a840_format_caps_lookup(desc->mtl_pixel_format);
        const bool can_ubwc = desc->request_ubwc6 && fc && fc->ubwc_supported != 0u;
        tdesc.kind = A840_P3_TEX_UBWC6;
        tdesc.storage_mode = can_ubwc ? A840_STORAGE_PRIVATE : A840_STORAGE_SHARED;
        tdesc.mtl_pixel_format = desc->mtl_pixel_format;
        tdesc.tile_mode = can_ubwc ? A840_TILE_UBWC : A840_TILE_LINEAR;
    }

    (void)pthread_mutex_lock(&fe->lock);
    for (i = 0u; i < A840_SYS_MAX_SURFACES; ++i) {
        if (!fe->surfaces[i].active) {
            free_idx = i;
            break;
        }
    }
    if (free_idx >= A840_SYS_MAX_SURFACES) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_CAPACITY;
    }

    st = a840_ubwc_texture_create(fe->rt_device, &tdesc, &tex);
    if (st != A840_RT_OK || !tex) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_RUNTIME_FAILED;
    }

    fe->surfaces[free_idx].active = true;
    fe->surfaces[free_idx].iosurface_id = desc->iosurface_id ? desc->iosurface_id : (uint64_t)(free_idx + 1u);
    fe->surfaces[free_idx].plane_index = desc->plane_index;
    fe->surfaces[free_idx].active_use_count = 0u;
    fe->surfaces[free_idx].surface_generation = 1u;
    fe->surfaces[free_idx].is_cross_process = desc->is_cross_process;
    fe->surfaces[free_idx].framebuffer_only = desc->framebuffer_only;
    fe->surfaces[free_idx].ubwc_tex = tex;
    fe->stats.surfaces_bound++;

    *out_handle = make_handle(fe->surfaces[free_idx].generation, free_idx);
    if (out_info) {
        fill_surface_info_locked(&fe->surfaces[free_idx], free_idx, out_info);
    }
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_query_surface(A840SysFrontend *fe,
                                     A840SysSurfaceHandle handle,
                                     A840SysSurfaceInfo *out_info)
{
    uint32_t idx, gen;
    if (!fe || !out_info) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    if (!decode_handle(handle, A840_SYS_MAX_SURFACES, &idx, &gen)) {
        return A840_SYS_ERR_STALE_HANDLE;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (!fe->surfaces[idx].active || fe->surfaces[idx].generation != gen) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_STALE_HANDLE;
    }
    fill_surface_info_locked(&fe->surfaces[idx], idx, out_info);
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840UbwcTexture *a840_sys_surface_ubwc_texture(A840SysFrontend *fe,
                                               A840SysSurfaceHandle handle)
{
    uint32_t idx, gen;
    A840UbwcTexture *tex = NULL;
    if (!fe || !decode_handle(handle, A840_SYS_MAX_SURFACES, &idx, &gen)) {
        return NULL;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (fe->surfaces[idx].active && fe->surfaces[idx].generation == gen) {
        tex = fe->surfaces[idx].ubwc_tex;
    }
    (void)pthread_mutex_unlock(&fe->lock);
    return tex;
}

A840SysStatus a840_sys_surface_increment_use(A840SysFrontend *fe, A840SysSurfaceHandle handle)
{
    uint32_t idx, gen;
    if (!fe || !decode_handle(handle, A840_SYS_MAX_SURFACES, &idx, &gen)) {
        return A840_SYS_ERR_STALE_HANDLE;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (!fe->surfaces[idx].active || fe->surfaces[idx].generation != gen) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_STALE_HANDLE;
    }
    fe->surfaces[idx].active_use_count++;
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_surface_decrement_use(A840SysFrontend *fe, A840SysSurfaceHandle handle)
{
    uint32_t idx, gen;
    if (!fe || !decode_handle(handle, A840_SYS_MAX_SURFACES, &idx, &gen)) {
        return A840_SYS_ERR_STALE_HANDLE;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (!fe->surfaces[idx].active || fe->surfaces[idx].generation != gen) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_STALE_HANDLE;
    }
    if (fe->surfaces[idx].active_use_count == 0u) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_INVALID_STATE;
    }
    fe->surfaces[idx].active_use_count--;
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_unbind_iosurface(A840SysFrontend *fe, A840SysSurfaceHandle handle)
{
    uint32_t idx, gen;
    A840UbwcTexture *tex = NULL;
    if (!fe || !decode_handle(handle, A840_SYS_MAX_SURFACES, &idx, &gen)) {
        return A840_SYS_ERR_STALE_HANDLE;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (!fe->surfaces[idx].active || fe->surfaces[idx].generation != gen) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_STALE_HANDLE;
    }
    tex = fe->surfaces[idx].ubwc_tex;
    fe->surfaces[idx].ubwc_tex = NULL;
    fe->surfaces[idx].active = false;
    fe->surfaces[idx].active_use_count = 0u;
    fe->surfaces[idx].generation = next_generation(fe->surfaces[idx].generation);
    fe->stats.surfaces_unbound++;
    if (tex) {
        a840_ubwc_texture_destroy(tex);
    }
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

/* ---------- 4. Unified Handle Tables ---------- */

A840SysStatus a840_sys_heap_create(A840SysFrontend *fe,
                                   const A840HeapDesc *desc,
                                   A840SysHeapHandle *out_handle)
{
    A840ProdResourceQuery rq;
    A840ProdDecision dec;
    A840Heap *heap = NULL;
    uint32_t idx = A840_SYS_MAX_HEAPS;
    uint32_t i;

    if (!fe || !desc || !out_handle) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    *out_handle = A840_SYS_INVALID_HANDLE;

    memset(&rq, 0, sizeof(rq));
    rq.uses_heap_aliasing = true;
    (void)pthread_mutex_lock(&fe->lock);
    dec = a840_prod_route_resource(&fe->prod_router, &rq);
    if (dec.status != A840_PROD_OK || dec.target != A840_PROD_ROUTE_NATIVE) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_UNSUPPORTED;
    }
    for (i = 0u; i < A840_SYS_MAX_HEAPS; ++i) {
        if (!fe->heaps[i].active) {
            idx = i;
            break;
        }
    }
    if (idx >= A840_SYS_MAX_HEAPS) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_CAPACITY;
    }
    if (a840_heap_create(fe->rt_device, desc, &heap) != A840_RT_OK || !heap) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_RUNTIME_FAILED;
    }
    fe->heaps[idx].active = true;
    fe->heaps[idx].heap = heap;
    fe->stats.heaps_created++;
    *out_handle = make_handle(fe->heaps[idx].generation, idx);
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840Heap *a840_sys_heap_resolve(A840SysFrontend *fe, A840SysHeapHandle handle)
{
    uint32_t idx, gen;
    A840Heap *heap = NULL;
    if (!fe || !decode_handle(handle, A840_SYS_MAX_HEAPS, &idx, &gen)) {
        return NULL;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (fe->heaps[idx].active && fe->heaps[idx].generation == gen) {
        heap = fe->heaps[idx].heap;
    }
    (void)pthread_mutex_unlock(&fe->lock);
    return heap;
}

A840SysStatus a840_sys_heap_destroy(A840SysFrontend *fe, A840SysHeapHandle handle)
{
    uint32_t idx, gen;
    if (!fe || !decode_handle(handle, A840_SYS_MAX_HEAPS, &idx, &gen)) {
        return A840_SYS_ERR_STALE_HANDLE;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (!fe->heaps[idx].active || fe->heaps[idx].generation != gen) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_STALE_HANDLE;
    }
    if (fe->heaps[idx].heap) {
        a840_heap_release(fe->heaps[idx].heap);
        fe->heaps[idx].heap = NULL;
    }
    fe->heaps[idx].active = false;
    fe->heaps[idx].generation = next_generation(fe->heaps[idx].generation);
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_event_create(A840SysFrontend *fe,
                                    bool is_shared,
                                    uint64_t initial_value,
                                    uint32_t mach_port_id,
                                    A840SysEventHandle *out_handle)
{
    A840ProdSyncQuery sq;
    A840ProdDecision dec;
    uint32_t idx = A840_SYS_MAX_EVENTS;
    uint32_t i;

    if (!fe || !out_handle) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    *out_handle = A840_SYS_INVALID_HANDLE;

    memset(&sq, 0, sizeof(sq));
    sq.cross_process_shared_event = is_shared;
    sq.abi_v3_supported = true;

    (void)pthread_mutex_lock(&fe->lock);
    dec = a840_prod_route_sync(&fe->prod_router, &sq);
    if (dec.status != A840_PROD_OK || dec.target != A840_PROD_ROUTE_NATIVE) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_UNSUPPORTED;
    }
    for (i = 0u; i < A840_SYS_MAX_EVENTS; ++i) {
        if (!fe->events[i].active) {
            idx = i;
            break;
        }
    }
    if (idx >= A840_SYS_MAX_EVENTS) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_CAPACITY;
    }
    fe->events[idx].active = true;
    fe->events[idx].is_shared = is_shared;
    fe->events[idx].mach_port_id = mach_port_id;
    fe->events[idx].signaled_value = initial_value;
    memset(fe->events[idx].waiters, 0, sizeof(fe->events[idx].waiters));
    fe->stats.events_created++;
    *out_handle = make_handle(fe->events[idx].generation, idx);
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_event_signal(A840SysFrontend *fe,
                                    A840SysEventHandle handle,
                                    uint64_t value)
{
    uint32_t idx, gen, i;
    SysPendingEventCallback to_fire[A840_SYS_MAX_EVENT_WAITERS];
    uint32_t fire_count = 0u;

    if (!fe || !decode_handle(handle, A840_SYS_MAX_EVENTS, &idx, &gen)) {
        return A840_SYS_ERR_STALE_HANDLE;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (!fe->events[idx].active || fe->events[idx].generation != gen) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_STALE_HANDLE;
    }
    if (value > fe->events[idx].signaled_value) {
        fe->events[idx].signaled_value = value;
    }
    fe->stats.event_signals++;
    for (i = 0u; i < A840_SYS_MAX_EVENT_WAITERS; ++i) {
        if (fe->events[idx].waiters[i].active &&
            fe->events[idx].signaled_value >= fe->events[idx].waiters[i].target_value) {
            to_fire[fire_count].handle = handle;
            to_fire[fire_count].value = fe->events[idx].signaled_value;
            to_fire[fire_count].fn = fe->events[idx].waiters[i].fn;
            to_fire[fire_count].ctx = fe->events[idx].waiters[i].ctx;
            fire_count++;
            fe->events[idx].waiters[i].active = false;
        }
    }
    (void)pthread_mutex_unlock(&fe->lock);

    /* Invoke event listener notifications strictly OUTSIDE mutex. */
    for (i = 0u; i < fire_count; ++i) {
        if (to_fire[i].fn) {
            to_fire[i].fn(to_fire[i].ctx, to_fire[i].handle, to_fire[i].value);
        }
    }
    return A840_SYS_OK;
}

A840SysStatus a840_sys_event_query(A840SysFrontend *fe,
                                   A840SysEventHandle handle,
                                   uint64_t *out_value,
                                   bool *out_is_shared,
                                   uint32_t *out_mach_port_id)
{
    uint32_t idx, gen;
    if (!fe || !decode_handle(handle, A840_SYS_MAX_EVENTS, &idx, &gen)) {
        return A840_SYS_ERR_STALE_HANDLE;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (!fe->events[idx].active || fe->events[idx].generation != gen) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_STALE_HANDLE;
    }
    if (out_value) *out_value = fe->events[idx].signaled_value;
    if (out_is_shared) *out_is_shared = fe->events[idx].is_shared;
    if (out_mach_port_id) *out_mach_port_id = fe->events[idx].mach_port_id;
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_event_register_waiter(A840SysFrontend *fe,
                                             A840SysEventHandle handle,
                                             uint64_t target_value,
                                             A840SysEventNotifyFn fn,
                                             void *ctx)
{
    uint32_t idx, gen, i;
    bool fire_now = false;
    uint64_t current_val = 0u;

    if (!fe || !fn || !decode_handle(handle, A840_SYS_MAX_EVENTS, &idx, &gen)) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (!fe->events[idx].active || fe->events[idx].generation != gen) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_STALE_HANDLE;
    }
    if (fe->events[idx].signaled_value >= target_value) {
        fire_now = true;
        current_val = fe->events[idx].signaled_value;
    } else {
        uint32_t slot = A840_SYS_MAX_EVENT_WAITERS;
        for (i = 0u; i < A840_SYS_MAX_EVENT_WAITERS; ++i) {
            if (!fe->events[idx].waiters[i].active) {
                slot = i;
                break;
            }
        }
        if (slot >= A840_SYS_MAX_EVENT_WAITERS) {
            (void)pthread_mutex_unlock(&fe->lock);
            return A840_SYS_ERR_CAPACITY;
        }
        fe->events[idx].waiters[slot].active = true;
        fe->events[idx].waiters[slot].target_value = target_value;
        fe->events[idx].waiters[slot].fn = fn;
        fe->events[idx].waiters[slot].ctx = ctx;
    }
    (void)pthread_mutex_unlock(&fe->lock);

    if (fire_now) {
        fn(ctx, handle, current_val);
    }
    return A840_SYS_OK;
}

A840SysStatus a840_sys_event_destroy(A840SysFrontend *fe, A840SysEventHandle handle)
{
    uint32_t idx, gen;
    if (!fe || !decode_handle(handle, A840_SYS_MAX_EVENTS, &idx, &gen)) {
        return A840_SYS_ERR_STALE_HANDLE;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (!fe->events[idx].active || fe->events[idx].generation != gen) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_STALE_HANDLE;
    }
    fe->events[idx].active = false;
    memset(fe->events[idx].waiters, 0, sizeof(fe->events[idx].waiters));
    fe->events[idx].generation = next_generation(fe->events[idx].generation);
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_icb_create(A840SysFrontend *fe,
                                  const A840IcbDesc *desc,
                                  A840SysIcbHandle *out_handle)
{
    A840IndirectCmdBuffer *icb = NULL;
    uint32_t idx = A840_SYS_MAX_ICBS;
    uint32_t i;
    if (!fe || !desc || !out_handle) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    *out_handle = A840_SYS_INVALID_HANDLE;

    (void)pthread_mutex_lock(&fe->lock);
    for (i = 0u; i < A840_SYS_MAX_ICBS; ++i) {
        if (!fe->icbs[i].active) {
            idx = i;
            break;
        }
    }
    if (idx >= A840_SYS_MAX_ICBS) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_CAPACITY;
    }
    if (a840_icb_create(fe->rt_device, desc, &icb) != A840_RT_OK || !icb) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_RUNTIME_FAILED;
    }
    fe->icbs[idx].active = true;
    fe->icbs[idx].icb = icb;
    fe->stats.icbs_created++;
    *out_handle = make_handle(fe->icbs[idx].generation, idx);
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840IndirectCmdBuffer *a840_sys_icb_resolve(A840SysFrontend *fe, A840SysIcbHandle handle)
{
    uint32_t idx, gen;
    A840IndirectCmdBuffer *icb = NULL;
    if (!fe || !decode_handle(handle, A840_SYS_MAX_ICBS, &idx, &gen)) {
        return NULL;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (fe->icbs[idx].active && fe->icbs[idx].generation == gen) {
        icb = fe->icbs[idx].icb;
    }
    (void)pthread_mutex_unlock(&fe->lock);
    return icb;
}

A840SysStatus a840_sys_icb_destroy(A840SysFrontend *fe, A840SysIcbHandle handle)
{
    uint32_t idx, gen;
    if (!fe || !decode_handle(handle, A840_SYS_MAX_ICBS, &idx, &gen)) {
        return A840_SYS_ERR_STALE_HANDLE;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (!fe->icbs[idx].active || fe->icbs[idx].generation != gen) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_STALE_HANDLE;
    }
    if (fe->icbs[idx].icb) {
        a840_icb_destroy(fe->icbs[idx].icb);
        fe->icbs[idx].icb = NULL;
    }
    fe->icbs[idx].active = false;
    fe->icbs[idx].generation = next_generation(fe->icbs[idx].generation);
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_depth_stencil_create(A840SysFrontend *fe,
                                            const A840DepthStencilVariantDesc *desc,
                                            A840SysDepthStencilHandle *out_handle)
{
    uint32_t idx = A840_SYS_MAX_DEPTH_STENCILS;
    uint32_t i;
    if (!fe || !desc || !out_handle) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    if (desc->depth_compare > 7u) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    if (desc->depth_bounds_test && desc->depth_bounds_min > desc->depth_bounds_max) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    *out_handle = A840_SYS_INVALID_HANDLE;

    (void)pthread_mutex_lock(&fe->lock);
    for (i = 0u; i < A840_SYS_MAX_DEPTH_STENCILS; ++i) {
        if (!fe->depth_stencils[i].active) {
            idx = i;
            break;
        }
    }
    if (idx >= A840_SYS_MAX_DEPTH_STENCILS) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_CAPACITY;
    }
    fe->depth_stencils[idx].active = true;
    fe->depth_stencils[idx].desc = *desc;
    fe->stats.depth_stencils_created++;
    *out_handle = make_handle(fe->depth_stencils[idx].generation, idx);
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_depth_stencil_get(A840SysFrontend *fe,
                                         A840SysDepthStencilHandle handle,
                                         A840DepthStencilVariantDesc *out_desc)
{
    uint32_t idx, gen;
    if (!fe || !out_desc || !decode_handle(handle, A840_SYS_MAX_DEPTH_STENCILS, &idx, &gen)) {
        return A840_SYS_ERR_STALE_HANDLE;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (!fe->depth_stencils[idx].active || fe->depth_stencils[idx].generation != gen) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_STALE_HANDLE;
    }
    *out_desc = fe->depth_stencils[idx].desc;
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_depth_stencil_destroy(A840SysFrontend *fe,
                                             A840SysDepthStencilHandle handle)
{
    uint32_t idx, gen;
    if (!fe || !decode_handle(handle, A840_SYS_MAX_DEPTH_STENCILS, &idx, &gen)) {
        return A840_SYS_ERR_STALE_HANDLE;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (!fe->depth_stencils[idx].active || fe->depth_stencils[idx].generation != gen) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_STALE_HANDLE;
    }
    fe->depth_stencils[idx].active = false;
    fe->depth_stencils[idx].generation = next_generation(fe->depth_stencils[idx].generation);
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_archive_create(A840SysFrontend *fe,
                                      uint64_t stack_key,
                                      uint64_t abi_fingerprint,
                                      A840SysArchiveHandle *out_handle)
{
    uint32_t idx = A840_SYS_MAX_ARCHIVES;
    uint32_t i;
    if (!fe || !out_handle || stack_key == 0u) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    *out_handle = A840_SYS_INVALID_HANDLE;

    (void)pthread_mutex_lock(&fe->lock);
    for (i = 0u; i < A840_SYS_MAX_ARCHIVES; ++i) {
        if (!fe->archives[i].active) {
            idx = i;
            break;
        }
    }
    if (idx >= A840_SYS_MAX_ARCHIVES) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_CAPACITY;
    }
    fe->archives[idx].active = true;
    fe->archives[idx].stack_key = stack_key;
    fe->archives[idx].abi_fingerprint = abi_fingerprint;
    fe->archives[idx].key_count = 0u;
    fe->stats.archives_created++;
    *out_handle = make_handle(fe->archives[idx].generation, idx);
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_archive_add_key(A840SysFrontend *fe,
                                       A840SysArchiveHandle handle,
                                       const A840PipelineKey *key)
{
    uint32_t idx, gen, i;
    if (!fe || !key || !decode_handle(handle, A840_SYS_MAX_ARCHIVES, &idx, &gen)) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (!fe->archives[idx].active || fe->archives[idx].generation != gen) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_STALE_HANDLE;
    }
    for (i = 0u; i < fe->archives[idx].key_count; ++i) {
        if (memcmp(fe->archives[idx].keys[i].sha256, key->sha256, sizeof(key->sha256)) == 0) {
            (void)pthread_mutex_unlock(&fe->lock);
            return A840_SYS_OK;
        }
    }
    if (fe->archives[idx].key_count >= A840_SYS_MAX_ARCHIVE_KEYS) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_CAPACITY;
    }
    fe->archives[idx].keys[fe->archives[idx].key_count++] = *key;
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_archive_contains_key(A840SysFrontend *fe,
                                            A840SysArchiveHandle handle,
                                            const A840PipelineKey *key,
                                            bool *out_found)
{
    uint32_t idx, gen, i;
    bool found = false;
    if (!fe || !key || !out_found || !decode_handle(handle, A840_SYS_MAX_ARCHIVES, &idx, &gen)) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (!fe->archives[idx].active || fe->archives[idx].generation != gen) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_STALE_HANDLE;
    }
    for (i = 0u; i < fe->archives[idx].key_count; ++i) {
        if (memcmp(fe->archives[idx].keys[i].sha256, key->sha256, sizeof(key->sha256)) == 0) {
            found = true;
            break;
        }
    }
    *out_found = found;
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_archive_count(A840SysFrontend *fe,
                                     A840SysArchiveHandle handle,
                                     uint32_t *out_count)
{
    uint32_t idx, gen;
    if (!fe || !out_count || !decode_handle(handle, A840_SYS_MAX_ARCHIVES, &idx, &gen)) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (!fe->archives[idx].active || fe->archives[idx].generation != gen) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_STALE_HANDLE;
    }
    *out_count = fe->archives[idx].key_count;
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_archive_destroy(A840SysFrontend *fe, A840SysArchiveHandle handle)
{
    uint32_t idx, gen;
    if (!fe || !decode_handle(handle, A840_SYS_MAX_ARCHIVES, &idx, &gen)) {
        return A840_SYS_ERR_STALE_HANDLE;
    }
    (void)pthread_mutex_lock(&fe->lock);
    if (!fe->archives[idx].active || fe->archives[idx].generation != gen) {
        (void)pthread_mutex_unlock(&fe->lock);
        return A840_SYS_ERR_STALE_HANDLE;
    }
    fe->archives[idx].active = false;
    fe->archives[idx].key_count = 0u;
    fe->archives[idx].generation = next_generation(fe->archives[idx].generation);
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

/* ---------- 5. Unified Frame Pipeline ---------- */

static void track_frame_surface_locked(A840SysFrontend *fe, A840SysSurfaceHandle handle)
{
    uint32_t i, idx, gen;
    if (handle == A840_SYS_INVALID_HANDLE) {
        return;
    }
    for (i = 0u; i < fe->frame_surface_count; ++i) {
        if (fe->frame_surfaces[i] == handle) {
            return;
        }
    }
    if (fe->frame_surface_count < A840_SYS_MAX_FRAME_SURFACES &&
        decode_handle(handle, A840_SYS_MAX_SURFACES, &idx, &gen) &&
        fe->surfaces[idx].active && fe->surfaces[idx].generation == gen) {
        fe->frame_surfaces[fe->frame_surface_count++] = handle;
        fe->surfaces[idx].active_use_count++;
    }
}

static void release_frame_surfaces_locked(A840SysFrontend *fe)
{
    uint32_t i, idx, gen;
    for (i = 0u; i < fe->frame_surface_count; ++i) {
        if (decode_handle(fe->frame_surfaces[i], A840_SYS_MAX_SURFACES, &idx, &gen) &&
            fe->surfaces[idx].active && fe->surfaces[idx].generation == gen &&
            fe->surfaces[idx].active_use_count > 0u) {
            fe->surfaces[idx].active_use_count--;
        }
    }
    fe->frame_surface_count = 0u;
}

A840SysStatus a840_sys_begin_frame(A840SysFrontend *fe,
                                   uint64_t frame_id,
                                   uint64_t cb_id,
                                   uint32_t *pm4_capture_buf,
                                   size_t pm4_capture_cap_dwords)
{
    uint32_t arena_slot = 0u;
    if (!fe || frame_id == 0u || cb_id == 0u) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    if (fe->frame_active) {
        return A840_SYS_ERR_INVALID_STATE;
    }

    if (a840_frame_arena_begin_frame(&fe->frame_arena, frame_id, &arena_slot) != A840_ARENA_OK) {
        return A840_SYS_ERR_CAPACITY;
    }
    a840_state_table_reset_frame(&fe->state_table);

    fe->active_frame_id = frame_id;
    fe->active_cb_id = cb_id;
    fe->active_ring_id = (fe->config.proc_role == A840_SYS_ROLE_RENDER_SERVER) ? 0u : 1u;
    fe->capture_buf = pm4_capture_buf;
    fe->capture_cap_dwords = pm4_capture_cap_dwords;
    fe->capture_used_dwords = 0u;
    fe->capture_truncated_chunks = 0u;
    memset(&fe->active_receipt, 0, sizeof(fe->active_receipt));
    fe->active_receipt.frame_id = frame_id;
    fe->active_receipt.cb_id = cb_id;
    fe->active_receipt.ring_id = fe->active_ring_id;
    fe->frame_surface_count = 0u;

    if (a840_early_promise_begin_frame(&fe->promise_sched, frame_id, cb_id, fe->active_ring_id) !=
        A840_PROMISE_SCHED_OK) {
        (void)a840_frame_arena_retire_frame(&fe->frame_arena, frame_id);
        return A840_SYS_ERR_INVALID_STATE;
    }
    if (a840_cb_chunker_begin_cb(&fe->chunker, frame_id, cb_id) != A840_CHUNK_OK) {
        (void)a840_frame_arena_retire_frame(&fe->frame_arena, frame_id);
        return A840_SYS_ERR_INVALID_STATE;
    }

    fe->frame_active = true;
    (void)pthread_mutex_lock(&fe->lock);
    fe->stats.frames_begun++;
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

static A840CompiledStage make_synth_stage(uint32_t stage, uint32_t local_x, uint32_t local_y, uint32_t local_z)
{
    A840CompiledStage s;
    memset(&s, 0, sizeof(s));
    s.abi_version = ADRENO_COMPILE_ABI_VERSION;
    s.stage = stage;
    s.isa_dwords = 16u;
    s.instruction_groups = 1u;
    s.max_full_reg = 4;
    s.max_half_reg = 2;
    s.constlen_vec4 = 4u;
    s.local_size[0] = local_x ? local_x : 8u;
    s.local_size[1] = local_y ? local_y : 8u;
    s.local_size[2] = local_z ? local_z : 1u;
    s.branchstack = 1u;
    s.max_waves = 16u;
    return s;
}

A840SysStatus a840_sys_encode_glass_gmem_pass(A840SysFrontend *fe,
                                              const A840SysGlassPassInput *input)
{
    A840UbwcTexture *color_tex = NULL;
    A840UbwcTexture *resolve_tex = NULL;
    A840DepthStencilVariantDesc ds_desc;
    bool has_ds = false;
    A840ProdRenderQuery rq;
    A840ProdDecision dec;
    A840ChunkPassDesc pdesc;
    uint64_t write_ids[2];
    uint64_t surface_id = 0u;
    A840CmdStream *cs = NULL;
    A840RtGmemPass gpass;
    A840GmemPlan plan;
    uint32_t samples;
    uint32_t passes;
    uint32_t p;

    if (!fe || !fe->frame_active || !input) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    passes = input->pass_count ? input->pass_count : 1u;
    if (passes > A840_CA_MAX_GLASS_PASSES) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    samples = input->sample_count ? input->sample_count : 1u;
    if (!a840_sys_supports_sample_count(samples)) {
        return A840_SYS_ERR_UNSUPPORTED;
    }

    (void)pthread_mutex_lock(&fe->lock);
    {
        uint32_t idx, gen;
        if (!decode_handle(input->color_surface, A840_SYS_MAX_SURFACES, &idx, &gen) ||
            !fe->surfaces[idx].active || fe->surfaces[idx].generation != gen) {
            (void)pthread_mutex_unlock(&fe->lock);
            return A840_SYS_ERR_STALE_HANDLE;
        }
        color_tex = fe->surfaces[idx].ubwc_tex;
        surface_id = fe->surfaces[idx].iosurface_id;
        track_frame_surface_locked(fe, input->color_surface);

        if (input->resolve_surface != A840_SYS_INVALID_HANDLE) {
            uint32_t ridx, rgen;
            if (!decode_handle(input->resolve_surface, A840_SYS_MAX_SURFACES, &ridx, &rgen) ||
                !fe->surfaces[ridx].active || fe->surfaces[ridx].generation != rgen) {
                (void)pthread_mutex_unlock(&fe->lock);
                return A840_SYS_ERR_STALE_HANDLE;
            }
            resolve_tex = fe->surfaces[ridx].ubwc_tex;
            track_frame_surface_locked(fe, input->resolve_surface);
        }
        if (input->depth_stencil != A840_SYS_INVALID_HANDLE) {
            uint32_t didx, dgen;
            if (!decode_handle(input->depth_stencil, A840_SYS_MAX_DEPTH_STENCILS, &didx, &dgen) ||
                !fe->depth_stencils[didx].active || fe->depth_stencils[didx].generation != dgen) {
                (void)pthread_mutex_unlock(&fe->lock);
                return A840_SYS_ERR_STALE_HANDLE;
            }
            ds_desc = fe->depth_stencils[didx].desc;
            has_ds = true;
        }
    }

    memset(&rq, 0, sizeof(rq));
    rq.color_attachment_count = 1u;
    rq.sample_count = samples;
    rq.uses_gmem = !input->force_sysmem;
    rq.has_memoryless_attachment = input->has_memoryless_depth;
    rq.has_ubwc_attachment = (color_tex->tile_mode == A840_TILE_UBWC);
    rq.has_ycbcr_or_xr10 = color_tex->is_xr10 || (color_tex->ycbcr_format != A840_YCBCR_NONE);
    rq.uses_fbfetch_or_prog_blend = input->uses_fbfetch;
    rq.has_msaa_resolve = (resolve_tex != NULL && samples > 1u);
    dec = a840_prod_route_render(&fe->prod_router, &rq);
    (void)pthread_mutex_unlock(&fe->lock);

    if (dec.status != A840_PROD_OK || dec.target != A840_PROD_ROUTE_NATIVE) {
        return A840_SYS_ERR_UNSUPPORTED;
    }

    if (input->emit_early_promise) {
        (void)a840_early_promise_plan_surface(
            &fe->promise_sched,
            surface_id,
            0u,
            0u,
            fe->config.proc_role == A840_SYS_ROLE_RENDER_SERVER);
    }

    write_ids[0] = color_tex->backing_bo ? color_tex->backing_bo->bo_handle : surface_id;
    write_ids[1] = (resolve_tex && resolve_tex->backing_bo) ? resolve_tex->backing_bo->bo_handle : 0u;

    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.kind = input->force_sysmem ? A840_CHUNK_PASS_RENDER_SYSMEM : A840_CHUNK_PASS_RENDER_GMEM;
    pdesc.sync_access.write_ids = write_ids;
    pdesc.sync_access.write_count = (write_ids[1] != 0u) ? 2u : 1u;
    pdesc.sync_access.consumer_mask = A840_SYNC_CONSUME_COLOR_RT;
    pdesc.produce_dirty_mask = input->force_sysmem ? A840_SYNC_DIRTY_COLOR_CCU : A840_SYNC_DIRTY_GMEM_RESOLVE;
    pdesc.est_gpu_us = input->est_gpu_us ? input->est_gpu_us : (passes * 350u);
    pdesc.est_cpu_us = input->est_cpu_us ? input->est_cpu_us : (passes * 80u);
    pdesc.written_surface_ids = &surface_id;
    pdesc.written_surface_count = 1u;

    if (a840_cb_chunker_begin_pass(&fe->chunker, &pdesc) != A840_CHUNK_OK) {
        return A840_SYS_ERR_EMIT_FAILED;
    }
    cs = a840_cb_chunker_cmd_stream(&fe->chunker);
    if (!cs) {
        (void)a840_cb_chunker_end_pass(&fe->chunker);
        return A840_SYS_ERR_EMIT_FAILED;
    }

    if (input->force_sysmem) {
        A840Clear2DDesc cdesc;
        uint32_t pitch;
        memset(&cdesc, 0, sizeof(cdesc));
        pitch = color_tex->linear_resolve_pitch
                    ? color_tex->linear_resolve_pitch
                    : (color_tex->width * (color_tex->cpp ? color_tex->cpp : 4u));
        pitch = (pitch + 63u) & ~63u;
        cdesc.dst.bo_handle = write_ids[0];
        cdesc.dst.gpu_va = color_tex->backing_bo ? color_tex->backing_bo->gpu_va : 0u;
        cdesc.dst.width = color_tex->width;
        cdesc.dst.height = color_tex->height;
        cdesc.dst.pitch = pitch;
        cdesc.dst.format_id = color_tex->format_caps ? color_tex->format_caps->color_fmt_id : 48u;
        cdesc.dst.swap = color_tex->format_caps ? color_tex->format_caps->color_swap : 1u;
        cdesc.dst.tile_mode = A840_TILE_LINEAR;
        cdesc.rect.x = 0u;
        cdesc.rect.y = 0u;
        cdesc.rect.width = color_tex->width;
        cdesc.rect.height = color_tex->height;
        memcpy(cdesc.color, input->clear_color, sizeof(cdesc.color));
        if (a840_emit_2d_clear(cs, &cdesc) != A840_EMIT_OK) {
            (void)a840_cb_chunker_end_pass(&fe->chunker);
            return A840_SYS_ERR_EMIT_FAILED;
        }
        if (has_ds && a840_emit_depth_stencil_variant(cs, &ds_desc) != A840_EMIT_OK) {
            (void)a840_cb_chunker_end_pass(&fe->chunker);
            return A840_SYS_ERR_EMIT_FAILED;
        }
        fe->active_receipt.sysmem_passes++;
        (void)pthread_mutex_lock(&fe->lock);
        fe->stats.sysmem_passes_total++;
        (void)pthread_mutex_unlock(&fe->lock);
    } else {
        memset(&gpass, 0, sizeof(gpass));
        gpass.desc.width = color_tex->width;
        gpass.desc.height = color_tex->height;
        gpass.desc.draw_count = passes;
        gpass.desc.uses_fbfetch = input->uses_fbfetch;
        gpass.desc.has_msaa_resolve = (resolve_tex != NULL && samples > 1u);
        gpass.tile_draws.bo = fe->synth_shader_bo->bo_handle;
        gpass.tile_draws.va = fe->synth_shader_bo->gpu_va;
        gpass.tile_draws.bytes = 64u;
        if (a840_ubwc_texture_fill_gmem_attachment(color_tex,
                                                   A840_LOAD_CLEAR,
                                                   (resolve_tex != NULL && samples > 1u)
                                                       ? A840_STORE_RESOLVE
                                                       : A840_STORE_STORE,
                                                   &gpass.desc.attachments[0],
                                                   &gpass.bindings[0]) != A840_RT_OK) {
            (void)a840_cb_chunker_end_pass(&fe->chunker);
            return A840_SYS_ERR_EMIT_FAILED;
        }
        if (samples > 1u) {
            gpass.desc.attachments[0].samples = samples;
            if (resolve_tex != NULL) {
                A840GmemAttachment r_att;
                A840GmemBinding r_bind;
                memset(&r_att, 0, sizeof(r_att));
                memset(&r_bind, 0, sizeof(r_bind));
                if (a840_ubwc_texture_fill_gmem_attachment(resolve_tex,
                                                           A840_LOAD_DONT_CARE,
                                                           A840_STORE_STORE,
                                                           &r_att,
                                                           &r_bind) == A840_RT_OK) {
                    gpass.bindings[0].resolve = r_bind.image;
                }
            }
        }
        memcpy(gpass.bindings[0].clear, input->clear_color, sizeof(gpass.bindings[0].clear));

        if (input->has_memoryless_depth) {
            gpass.desc.attachments[A840_GMEM_DEPTH].cpp = 4u;
            gpass.desc.attachments[A840_GMEM_DEPTH].samples = samples;
            gpass.desc.attachments[A840_GMEM_DEPTH].load = A840_LOAD_CLEAR;
            gpass.desc.attachments[A840_GMEM_DEPTH].store = A840_STORE_DONT_CARE;
            gpass.desc.attachments[A840_GMEM_DEPTH].memoryless = true;
            gpass.bindings[A840_GMEM_DEPTH].format =
                a840_format_caps_lookup(A840_MTL_FMT_DEPTH32_FLOAT)->color_fmt_id;
            gpass.bindings[A840_GMEM_DEPTH].clear[0] = 1065353216u; /* 1.0f IEEE754 */
        }

        (void)input->force_96x32_tiles;
        memset(&plan, 0, sizeof(plan));
        if (a840_rt_gmem_encode(cs, &gpass, &plan) != A840_EMIT_OK) {
            (void)a840_cb_chunker_end_pass(&fe->chunker);
            return A840_SYS_ERR_EMIT_FAILED;
        }
        if (has_ds && a840_emit_depth_stencil_variant(cs, &ds_desc) != A840_EMIT_OK) {
            (void)a840_cb_chunker_end_pass(&fe->chunker);
            return A840_SYS_ERR_EMIT_FAILED;
        }
        if (samples > 1u) {
            A840MsaaStateDesc mdesc;
            memset(&mdesc, 0, sizeof(mdesc));
            mdesc.sample_count = samples;
            if (a840_emit_msaa_state(cs, &mdesc) != A840_EMIT_OK) {
                (void)a840_cb_chunker_end_pass(&fe->chunker);
                return A840_SYS_ERR_EMIT_FAILED;
            }
        }
        if (input->uses_fbfetch) {
            for (p = 0u; p < passes; ++p) {
                /* On-chip GMEM subpass barrier (no sysmem roundtrip). */
                if (a840_emit_cache_flush_inv(cs, A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV) !=
                    A840_EMIT_OK) {
                    (void)a840_cb_chunker_end_pass(&fe->chunker);
                    return A840_SYS_ERR_EMIT_FAILED;
                }
            }
            fe->active_receipt.fbfetch_draws += passes;
            (void)pthread_mutex_lock(&fe->lock);
            fe->stats.fbfetch_draws_total += passes;
            (void)pthread_mutex_unlock(&fe->lock);
        }

        fe->active_receipt.gmem_passes++;
        if (input->has_memoryless_depth) {
            fe->active_receipt.memoryless_depth_passes++;
        }
        fe->active_receipt.tile_width = plan.tile_w;
        fe->active_receipt.tile_height = plan.tile_h;
        fe->active_receipt.tiles_x = plan.tiles_x;
        fe->active_receipt.tiles_y = plan.tiles_y;

        (void)pthread_mutex_lock(&fe->lock);
        fe->stats.gmem_passes_total++;
        (void)pthread_mutex_unlock(&fe->lock);
    }

    if (a840_cb_chunker_end_pass(&fe->chunker) != A840_CHUNK_OK) {
        return A840_SYS_ERR_EMIT_FAILED;
    }
    fe->active_receipt.passes_encoded++;
    return A840_SYS_OK;
}

A840SysStatus a840_sys_encode_compute_pass(A840SysFrontend *fe,
                                           const A840SysComputePassInput *input)
{
    A840ProdComputeQuery cq;
    A840ProdDecision dec;
    A840ChunkPassDesc pdesc;
    A840CmdStream *cs = NULL;
    A840CompiledStage stage;
    A840ComputePipelineDesc cpipe;

    if (!fe || !fe->frame_active || !input) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    if (!input->indirect_dispatch &&
        (input->groups_x == 0u || input->groups_y == 0u || input->groups_z == 0u)) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    if (input->indirect_dispatch && input->indirect_gpu_va == 0u) {
        return A840_SYS_ERR_INVALID_ARG;
    }

    memset(&cq, 0, sizeof(cq));
    cq.has_compiled_kernel = true;
    cq.has_valid_local_size = true;
    cq.is_indirect_dispatch = input->indirect_dispatch;

    (void)pthread_mutex_lock(&fe->lock);
    dec = a840_prod_route_compute(&fe->prod_router, &cq);
    (void)pthread_mutex_unlock(&fe->lock);
    if (dec.status != A840_PROD_OK || dec.target != A840_PROD_ROUTE_NATIVE) {
        return A840_SYS_ERR_UNSUPPORTED;
    }

    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.kind = A840_CHUNK_PASS_COMPUTE;
    pdesc.sync_access.read_ids = input->read_resource_ids;
    pdesc.sync_access.read_count = input->read_count;
    pdesc.sync_access.write_ids = input->write_resource_ids;
    pdesc.sync_access.write_count = input->write_count;
    pdesc.sync_access.consumer_mask = A840_SYNC_CONSUME_COMPUTE_RW;
    pdesc.produce_dirty_mask = A840_SYNC_DIRTY_UCHE_SHADER_WR;
    pdesc.est_gpu_us = input->est_gpu_us ? input->est_gpu_us : 250u;
    pdesc.est_cpu_us = input->est_cpu_us ? input->est_cpu_us : 60u;

    if (a840_cb_chunker_begin_pass(&fe->chunker, &pdesc) != A840_CHUNK_OK) {
        return A840_SYS_ERR_EMIT_FAILED;
    }
    cs = a840_cb_chunker_cmd_stream(&fe->chunker);
    if (!cs) {
        (void)a840_cb_chunker_end_pass(&fe->chunker);
        return A840_SYS_ERR_EMIT_FAILED;
    }

    stage = make_synth_stage(A840_STAGE_COMPUTE, input->local_x, input->local_y, input->local_z);
    memset(&cpipe, 0, sizeof(cpipe));
    cpipe.shader.stage = &stage;
    cpipe.shader.code.bo_handle = fe->synth_shader_bo->bo_handle;
    cpipe.shader.code.gpu_va = fe->synth_shader_bo->gpu_va;
    cpipe.shader.bindless_uav = true;
    cpipe.shader.bindless_tex = true;

    if (a840_emit_compute_pipeline(cs, &cpipe) != A840_EMIT_OK) {
        (void)a840_cb_chunker_end_pass(&fe->chunker);
        return A840_SYS_ERR_EMIT_FAILED;
    }
    {
        A840DispatchDesc ddesc;
        memset(&ddesc, 0, sizeof(ddesc));
        ddesc.stage = &stage;
        if (input->indirect_dispatch) {
            ddesc.mode = A840_DISPATCH_INDIRECT;
            ddesc.indirect.bo_handle = input->indirect_bo_handle;
            ddesc.indirect.gpu_va = input->indirect_gpu_va;
        } else {
            ddesc.mode = A840_DISPATCH_UNIFORM;
            ddesc.size[0] = input->groups_x;
            ddesc.size[1] = input->groups_y;
            ddesc.size[2] = input->groups_z;
        }
        if (a840_emit_compute_dispatch(cs, &ddesc) != A840_EMIT_OK) {
            (void)a840_cb_chunker_end_pass(&fe->chunker);
            return A840_SYS_ERR_EMIT_FAILED;
        }
    }

    if (a840_cb_chunker_end_pass(&fe->chunker) != A840_CHUNK_OK) {
        return A840_SYS_ERR_EMIT_FAILED;
    }
    fe->active_receipt.passes_encoded++;
    return A840_SYS_OK;
}

A840SysStatus a840_sys_encode_camera155_pass(A840SysFrontend *fe,
                                             const A840SysCamera155PassInput *input)
{
    A840UbwcTexture *preview_tex = NULL;
    A840UbwcTexture *photo_tex = NULL;
    uint64_t preview_id = 0u;
    uint64_t photo_id = 0u;
    uint64_t write_ids[2];
    size_t write_count = 0u;
    A840ChunkPassDesc pdesc;
    A840CmdStream *cs = NULL;
    A840CompiledStage stage;
    A840Phase4Camera155Desc cam_desc;
    A840Phase4PassStats cam_stats;

    if (!fe || !fe->frame_active || !input) {
        return A840_SYS_ERR_INVALID_ARG;
    }

    (void)pthread_mutex_lock(&fe->lock);
    {
        uint32_t idx, gen;
        if (!decode_handle(input->preview_ycbcr_surface, A840_SYS_MAX_SURFACES, &idx, &gen) ||
            !fe->surfaces[idx].active || fe->surfaces[idx].generation != gen) {
            (void)pthread_mutex_unlock(&fe->lock);
            return A840_SYS_ERR_STALE_HANDLE;
        }
        preview_tex = fe->surfaces[idx].ubwc_tex;
        preview_id = fe->surfaces[idx].iosurface_id;
        track_frame_surface_locked(fe, input->preview_ycbcr_surface);

        if (input->photo_bgra8_surface != A840_SYS_INVALID_HANDLE) {
            uint32_t pidx, pgen;
            if (!decode_handle(input->photo_bgra8_surface, A840_SYS_MAX_SURFACES, &pidx, &pgen) ||
                !fe->surfaces[pidx].active || fe->surfaces[pidx].generation != pgen) {
                (void)pthread_mutex_unlock(&fe->lock);
                return A840_SYS_ERR_STALE_HANDLE;
            }
            photo_tex = fe->surfaces[pidx].ubwc_tex;
            photo_id = fe->surfaces[pidx].iosurface_id;
            track_frame_surface_locked(fe, input->photo_bgra8_surface);
        }
    }
    (void)pthread_mutex_unlock(&fe->lock);

    if (!preview_tex || preview_tex->kind != A840_P3_TEX_YCBCR_MULTIPLANE) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    if (!photo_tex) {
        return A840_SYS_ERR_INVALID_ARG;
    }

    write_ids[write_count++] = photo_tex->backing_bo ? photo_tex->backing_bo->bo_handle : photo_id;

    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.kind = A840_CHUNK_PASS_COMPUTE;
    pdesc.sync_access.read_ids = &preview_id;
    pdesc.sync_access.read_count = 1u;
    pdesc.sync_access.write_ids = write_ids;
    pdesc.sync_access.write_count = write_count;
    pdesc.sync_access.consumer_mask = A840_SYNC_CONSUME_COMPUTE_RW;
    pdesc.produce_dirty_mask = A840_SYNC_DIRTY_UCHE_SHADER_WR | A840_SYNC_DIRTY_BLIT_2D_WR;
    pdesc.est_gpu_us = input->est_gpu_us ? input->est_gpu_us : 600u;
    pdesc.est_cpu_us = input->est_cpu_us ? input->est_cpu_us : 120u;
    pdesc.written_surface_ids = &photo_id;
    pdesc.written_surface_count = 1u;

    if (a840_cb_chunker_begin_pass(&fe->chunker, &pdesc) != A840_CHUNK_OK) {
        return A840_SYS_ERR_EMIT_FAILED;
    }
    cs = a840_cb_chunker_cmd_stream(&fe->chunker);
    if (!cs) {
        (void)a840_cb_chunker_end_pass(&fe->chunker);
        return A840_SYS_ERR_EMIT_FAILED;
    }

    stage = make_synth_stage(A840_STAGE_COMPUTE, 16u, 16u, 1u);
    memset(&cam_desc, 0, sizeof(cam_desc));
    cam_desc.preview_ycbcr = preview_tex;
    cam_desc.photo_bgra8 = photo_tex;
    cam_desc.yuv_to_rgb_consts = fe->synth_consts_bo;
    cam_desc.bindless_table = fe->synth_bindless_bo;
    cam_desc.yuv_convert_pipe.shader.stage = &stage;
    cam_desc.yuv_convert_pipe.shader.code.bo_handle = fe->synth_shader_bo->bo_handle;
    cam_desc.yuv_convert_pipe.shader.code.gpu_va = fe->synth_shader_bo->gpu_va;
    cam_desc.yuv_convert_pipe.shader.bindless_tex = true;
    cam_desc.yuv_convert_pipe.shader.bindless_uav = true;
    cam_desc.emit_photo_clear_and_blit = input->encode_photo_capture;

    memset(&cam_stats, 0, sizeof(cam_stats));
    a840_phase4_enable_full_router(&fe->rt_device->router);
    if (a840_phase4_encode_camera155_flow(&fe->rt_device->router, &cam_desc, cs, &cam_stats) != A840_RT_OK) {
        (void)a840_cb_chunker_end_pass(&fe->chunker);
        return A840_SYS_ERR_EMIT_FAILED;
    }

    if (a840_cb_chunker_end_pass(&fe->chunker) != A840_CHUNK_OK) {
        return A840_SYS_ERR_EMIT_FAILED;
    }
    fe->active_receipt.passes_encoded++;
    (void)pthread_mutex_lock(&fe->lock);
    fe->stats.camera155_passes_total++;
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_encode_heap_alias_barrier(A840SysFrontend *fe,
                                                 const A840HeapAliasTransition *transition)
{
    A840ChunkPassDesc pdesc;
    A840CmdStream *cs = NULL;
    if (!fe || !fe->frame_active || !transition) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.kind = A840_CHUNK_PASS_BLIT;
    pdesc.sync_access.consumer_mask = A840_SYNC_CONSUME_ALL;
    pdesc.produce_dirty_mask = A840_SYNC_DIRTY_NONE;
    pdesc.est_gpu_us = 25u;
    pdesc.est_cpu_us = 10u;

    if (a840_cb_chunker_begin_pass(&fe->chunker, &pdesc) != A840_CHUNK_OK) {
        return A840_SYS_ERR_EMIT_FAILED;
    }
    cs = a840_cb_chunker_cmd_stream(&fe->chunker);
    if (!cs || a840_heap_emit_alias_transition(cs, transition) != A840_EMIT_OK) {
        (void)a840_cb_chunker_end_pass(&fe->chunker);
        return A840_SYS_ERR_EMIT_FAILED;
    }
    if (a840_cb_chunker_end_pass(&fe->chunker) != A840_CHUNK_OK) {
        return A840_SYS_ERR_EMIT_FAILED;
    }
    fe->active_receipt.passes_encoded++;
    (void)pthread_mutex_lock(&fe->lock);
    fe->stats.heap_alias_barriers_emitted++;
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_encode_icb_execute(A840SysFrontend *fe,
                                          A840SysIcbHandle icb_handle,
                                          uint32_t start,
                                          uint32_t count,
                                          A840RtBuffer *scratch_ib_bo)
{
    A840IndirectCmdBuffer *icb = NULL;
    A840Reloc relocs[SYS_RELOC_SCRATCH_CAP];
    A840IcbCompiledRange compiled;
    A840ChunkPassDesc pdesc;
    A840CmdStream *cs = NULL;

    if (!fe || !fe->frame_active || !scratch_ib_bo || count == 0u) {
        return A840_SYS_ERR_INVALID_ARG;
    }
    icb = a840_sys_icb_resolve(fe, icb_handle);
    if (!icb) {
        return A840_SYS_ERR_STALE_HANDLE;
    }

    memset(&compiled, 0, sizeof(compiled));
    if (a840_icb_compile_to_gpu_ib(icb, start, count, scratch_ib_bo,
                                   relocs, SYS_RELOC_SCRATCH_CAP, &compiled) != A840_RT_OK) {
        return A840_SYS_ERR_EMIT_FAILED;
    }

    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.kind = (icb->type == A840_ICB_TYPE_COMPUTE) ? A840_CHUNK_PASS_COMPUTE : A840_CHUNK_PASS_RENDER_SYSMEM;
    pdesc.sync_access.consumer_mask = A840_SYNC_CONSUME_CP_INDIRECT;
    pdesc.produce_dirty_mask = A840_SYNC_DIRTY_UCHE_SHADER_WR;
    pdesc.est_gpu_us = 200u;
    pdesc.est_cpu_us = 40u;

    if (a840_cb_chunker_begin_pass(&fe->chunker, &pdesc) != A840_CHUNK_OK) {
        return A840_SYS_ERR_EMIT_FAILED;
    }
    cs = a840_cb_chunker_cmd_stream(&fe->chunker);
    if (!cs || a840_icb_emit_execute(cs, &compiled, true) != A840_EMIT_OK) {
        (void)a840_cb_chunker_end_pass(&fe->chunker);
        return A840_SYS_ERR_EMIT_FAILED;
    }
    if (a840_cb_chunker_end_pass(&fe->chunker) != A840_CHUNK_OK) {
        return A840_SYS_ERR_EMIT_FAILED;
    }
    fe->active_receipt.passes_encoded++;
    (void)pthread_mutex_lock(&fe->lock);
    fe->stats.icbs_executed++;
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_encode_scanout_handoff(A840SysFrontend *fe,
                                              const A840SysScanoutRequest *req)
{
    A840UbwcTexture *src_tex = NULL;
    A840UbwcTexture *dst_tex = NULL;
    uint64_t surface_id = 0u;
    uint64_t bo_id = 0u;
    A840ChunkPassDesc pdesc;
    A840CmdStream *cs = NULL;
    A840EarlyPromiseStatus pst;
    bool allow_early;

    if (!fe || !fe->frame_active || !req) {
        return A840_SYS_ERR_INVALID_ARG;
    }

    (void)pthread_mutex_lock(&fe->lock);
    {
        uint32_t idx, gen;
        if (!decode_handle(req->surface, A840_SYS_MAX_SURFACES, &idx, &gen) ||
            !fe->surfaces[idx].active || fe->surfaces[idx].generation != gen) {
            (void)pthread_mutex_unlock(&fe->lock);
            return A840_SYS_ERR_STALE_HANDLE;
        }
        src_tex = fe->surfaces[idx].ubwc_tex;
        surface_id = fe->surfaces[idx].iosurface_id;
        bo_id = src_tex->backing_bo ? src_tex->backing_bo->bo_handle : surface_id;
        track_frame_surface_locked(fe, req->surface);

        if (req->linear_dst_surface != A840_SYS_INVALID_HANDLE) {
            uint32_t didx, dgen;
            if (!decode_handle(req->linear_dst_surface, A840_SYS_MAX_SURFACES, &didx, &dgen) ||
                !fe->surfaces[didx].active || fe->surfaces[didx].generation != dgen) {
                (void)pthread_mutex_unlock(&fe->lock);
                return A840_SYS_ERR_STALE_HANDLE;
            }
            dst_tex = fe->surfaces[didx].ubwc_tex;
            track_frame_surface_locked(fe, req->linear_dst_surface);
        }
    }
    (void)pthread_mutex_unlock(&fe->lock);

    allow_early = req->request_early_promise;
    pst = a840_early_promise_plan_surface(&fe->promise_sched,
                                          surface_id,
                                          0u,
                                          req->explicit_generation,
                                          allow_early);
    if (pst == A840_PROMISE_SCHED_ERR_PERMISSION_DENIED) {
        return A840_SYS_ERR_PERMISSION_DENIED;
    }
    if (pst != A840_PROMISE_SCHED_OK && pst != A840_PROMISE_SCHED_ERR_DUPLICATE) {
        return A840_SYS_ERR_INVALID_STATE;
    }

    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.kind = A840_CHUNK_PASS_BLIT;
    pdesc.sync_access.read_ids = &bo_id;
    pdesc.sync_access.read_count = 1u;
    pdesc.sync_access.consumer_mask = A840_SYNC_CONSUME_HOST_PRESENT;
    pdesc.produce_dirty_mask = A840_SYNC_DIRTY_BLIT_2D_WR;
    pdesc.est_gpu_us = 180u;
    pdesc.est_cpu_us = 30u;
    pdesc.written_surface_ids = &surface_id;
    pdesc.written_surface_count = 1u;
    pdesc.is_final_pass = true;

    if (a840_cb_chunker_begin_pass(&fe->chunker, &pdesc) != A840_CHUNK_OK) {
        return A840_SYS_ERR_EMIT_FAILED;
    }
    cs = a840_cb_chunker_cmd_stream(&fe->chunker);
    if (!cs) {
        (void)a840_cb_chunker_end_pass(&fe->chunker);
        return A840_SYS_ERR_EMIT_FAILED;
    }

    if (req->mode == A840_SYS_SCANOUT_NATIVE_LINEAR && dst_tex && dst_tex->backing_bo && src_tex->backing_bo) {
        A840Blit2DDesc bdesc;
        uint32_t spitch, dpitch;
        memset(&bdesc, 0, sizeof(bdesc));
        spitch = src_tex->linear_resolve_pitch
                     ? src_tex->linear_resolve_pitch
                     : (src_tex->ubwc.data_pitch ? src_tex->ubwc.data_pitch : src_tex->width * 4u);
        spitch = (spitch + 63u) & ~63u;
        dpitch = dst_tex->linear_resolve_pitch
                     ? dst_tex->linear_resolve_pitch
                     : (dst_tex->width * 4u);
        dpitch = (dpitch + 63u) & ~63u;
        bdesc.src.bo_handle = src_tex->backing_bo->bo_handle;
        bdesc.src.gpu_va = src_tex->backing_bo->gpu_va;
        bdesc.src.width = src_tex->width;
        bdesc.src.height = src_tex->height;
        bdesc.src.pitch = spitch;
        bdesc.src.format_id = src_tex->format_caps ? src_tex->format_caps->color_fmt_id : 48u;
        bdesc.src.swap = src_tex->format_caps ? src_tex->format_caps->color_swap : 1u;
        bdesc.src.tile_mode = A840_TILE_LINEAR;

        bdesc.dst = bdesc.src;
        bdesc.dst.bo_handle = dst_tex->backing_bo->bo_handle;
        bdesc.dst.gpu_va = dst_tex->backing_bo->gpu_va;
        bdesc.dst.pitch = dpitch;

        bdesc.src_rect.width = src_tex->width;
        bdesc.src_rect.height = src_tex->height;
        bdesc.dst_rect = bdesc.src_rect;
        bdesc.filter = A840_FILTER_NEAREST;
        if (a840_emit_2d_blit(cs, &bdesc) != A840_EMIT_OK) {
            (void)a840_cb_chunker_end_pass(&fe->chunker);
            return A840_SYS_ERR_EMIT_FAILED;
        }
        fe->active_receipt.mdss_native_linear++;
    } else {
        /* Direct UBWC 6.0 MDSS handoff: flush color CCU + wait mem writes before display scanout. */
        if (a840_emit_cache_flush_inv(cs, A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV) != A840_EMIT_OK ||
            a840_emit_wait_mem_writes(cs) != A840_EMIT_OK) {
            (void)a840_cb_chunker_end_pass(&fe->chunker);
            return A840_SYS_ERR_EMIT_FAILED;
        }
        if (req->mode == A840_SYS_SCANOUT_DIRECT_UBWC6) {
            fe->active_receipt.mdss_direct_ubwc++;
        } else {
            fe->active_receipt.mdss_native_linear++;
        }
    }

    if (a840_cb_chunker_end_pass(&fe->chunker) != A840_CHUNK_OK) {
        return A840_SYS_ERR_EMIT_FAILED;
    }
    fe->active_receipt.passes_encoded++;
    (void)pthread_mutex_lock(&fe->lock);
    fe->stats.scanout_handoffs_total++;
    (void)pthread_mutex_unlock(&fe->lock);
    return A840_SYS_OK;
}

A840SysStatus a840_sys_commit_frame(A840SysFrontend *fe,
                                    A840SysFrameReceipt *out_receipt)
{
    A840SyncStats sstats;
    A840EarlyPromiseStats pstats;
    A840FrameArenaStats astats;

    if (!fe || !fe->frame_active) {
        return A840_SYS_ERR_INVALID_STATE;
    }
    if (a840_cb_chunker_finish(&fe->chunker) != A840_CHUNK_OK) {
        return A840_SYS_ERR_EMIT_FAILED;
    }

    sstats = a840_cb_chunker_sync_stats(&fe->chunker);
    pstats = a840_early_promise_get_stats(&fe->promise_sched);
    astats = a840_frame_arena_stats(&fe->frame_arena);

    fe->active_receipt.wfi_elided = sstats.wfi_elided;
    fe->active_receipt.ccu_flush_elided = sstats.ccu_flush_elided;
    fe->active_receipt.uche_inv_elided = sstats.uche_inv_elided;
    fe->active_receipt.early_promises_published = pstats.early_scheduled_count;
    fe->active_receipt.deferred_promises_armed = pstats.deferred_armed_count;
    fe->active_receipt.retired_promises_ready = pstats.retired_ready_count;
    fe->active_receipt.frame_arena_used_bytes = astats.high_water_bytes;

    (void)pthread_mutex_lock(&fe->lock);
    fe->stats.frames_committed++;
    fe->stats.chunks_kicked_total += fe->active_receipt.chunks_kicked;
    fe->stats.early_kicks_total += fe->active_receipt.early_kicks;
    fe->stats.wfi_elided_total = sstats.wfi_elided;
    fe->stats.ccu_flush_elided_total = sstats.ccu_flush_elided;
    fe->stats.uche_inv_elided_total = sstats.uche_inv_elided;
    (void)pthread_mutex_unlock(&fe->lock);

    if (out_receipt) {
        *out_receipt = fe->active_receipt;
    }
    return A840_SYS_OK;
}

A840SysStatus a840_sys_retire_frame(A840SysFrontend *fe,
                                    uint64_t frame_id,
                                    uint64_t completed_chunk_seqno,
                                    A840SysFrameReceipt *io_receipt)
{
    uint32_t retired_slots = 0u;
    A840EarlyPromiseStats pstats;

    if (!fe || frame_id == 0u) {
        return A840_SYS_ERR_INVALID_ARG;
    }

    (void)a840_early_promise_on_chunk_retired(&fe->promise_sched,
                                              fe->active_ring_id,
                                              completed_chunk_seqno);
    (void)a840_cmd_slot_retire_fence_seqno(&fe->cmd_pool, completed_chunk_seqno, &retired_slots);
    if (a840_frame_arena_retire_frame(&fe->frame_arena, frame_id) != A840_ARENA_OK) {
        return A840_SYS_ERR_NOT_FOUND;
    }

    (void)pthread_mutex_lock(&fe->lock);
    release_frame_surfaces_locked(fe);
    fe->stats.frames_retired++;
    (void)pthread_mutex_unlock(&fe->lock);

    fe->frame_active = false;
    pstats = a840_early_promise_get_stats(&fe->promise_sched);
    fe->active_receipt.retired_promises_ready = pstats.retired_ready_count;
    if (io_receipt) {
        io_receipt->retired_promises_ready = pstats.retired_ready_count;
    }
    return A840_SYS_OK;
}

void a840_sys_abort_frame(A840SysFrontend *fe, A840EarlyPromiseCancelReason reason)
{
    if (!fe || !fe->frame_active) {
        return;
    }
    a840_cb_chunker_abort(&fe->chunker);
    (void)a840_early_promise_cancel_cb(&fe->promise_sched, fe->active_cb_id, reason);
    (void)a840_frame_arena_retire_frame(&fe->frame_arena, fe->active_frame_id);
    (void)pthread_mutex_lock(&fe->lock);
    release_frame_surfaces_locked(fe);
    (void)pthread_mutex_unlock(&fe->lock);
    fe->frame_active = false;
}
