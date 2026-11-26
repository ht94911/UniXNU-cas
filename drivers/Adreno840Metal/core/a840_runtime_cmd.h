/* SPDX-License-Identifier: MIT */
#ifndef A840_RUNTIME_CMD_H
#define A840_RUNTIME_CMD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "AdrenoCompileABI.h"
#include "a840_emit_ops.h"
#include "a840_emit_compute.h"
#include "a840_emit_3d.h"
#include "a840_runtime_device.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_RT_DEFAULT_IB_DWORDS = 4096u,
    A840_RT_DEFAULT_RELOC_CAPACITY = 256u
};

typedef enum A840CmdBufferState {
    A840_CB_RECORDING = 0,
    A840_CB_COMMITTED = 1,
    A840_CB_SCHEDULED = 2,
    A840_CB_COMPLETED = 3,
    A840_CB_ERROR = 4
} A840CmdBufferState;

typedef enum A840EncoderKind {
    A840_ENCODER_NONE = 0,
    A840_ENCODER_BLIT = 1,
    A840_ENCODER_COMPUTE = 2,
    A840_ENCODER_RENDER_3D = 3
} A840EncoderKind;

typedef struct A840RtQueue A840RtQueue;
typedef struct A840RtCmdBuffer A840RtCmdBuffer;
typedef struct A840RtBlitEncoder A840RtBlitEncoder;
typedef struct A840RtComputeEncoder A840RtComputeEncoder;
typedef struct A840RtRenderEncoder A840RtRenderEncoder;

typedef void (*A840RtCmdCallback)(A840RtCmdBuffer *cb, void *user_data);

typedef struct A840RtCmdBufferDesc {
    size_t ib_capacity_dwords; /* 0 defaults to A840_RT_DEFAULT_IB_DWORDS */
    size_t reloc_capacity;     /* 0 defaults to A840_RT_DEFAULT_RELOC_CAPACITY */
} A840RtCmdBufferDesc;

typedef struct A840RtComputePipelineDesc {
    const A840CompiledStage *stage;
    A840RtBuffer *code_buffer;
    uint64_t code_offset;
    A840RtBuffer *private_buffer; /* Optional (NULL if stage->private_mem_bytes == 0) */
    uint64_t private_offset;
    uint32_t textures;
    uint32_t samplers;
    uint32_t uavs;
    bool per_wave_private_memory;
    bool bindless_tex;
    bool bindless_samp;
    bool bindless_uav;
    bool bindless_ubo;
    uint8_t local_id_regid;
    uint8_t wg_id_regid;
    bool round_robin;
    bool linear_dispatch;
} A840RtComputePipelineDesc;

typedef struct A840RtRenderPassDesc {
    uint32_t color_count; /* 0..A840_MAX_COLOR_TARGETS */
    A840RtTexture *color_targets[A840_MAX_COLOR_TARGETS];
    bool color_clear[A840_MAX_COLOR_TARGETS];
    uint32_t color_clear_values[A840_MAX_COLOR_TARGETS][4];
    A840RtTexture *depth_target;   /* Optional; must be tiled */
    A840RtTexture *stencil_target; /* Optional; must be tiled S8 */
    uint32_t sample_count;         /* 0 or 1 for Phase 2 sysmem */
    bool has_memoryless_attachment;
    bool has_msaa_resolve;
    bool uses_fbfetch;
    bool uses_programmable_blend;
    bool uses_tessellation;
    bool uses_vertex_amplification;
    bool uses_indirect_draw;
    bool uses_icb;
    bool has_ycbcr_attachment;
} A840RtRenderPassDesc;

typedef struct A840RtGraphicsProgramDesc {
    A840GraphicsProgramDesc emit_desc; /* Stage metadata + IO links; GPU addrs filled from buffers below */
    A840RtBuffer *vs_code_buffer;
    uint64_t vs_code_offset;
    A840RtBuffer *vs_private_buffer;
    uint64_t vs_private_offset;
    A840RtBuffer *fs_code_buffer;
    uint64_t fs_code_offset;
    A840RtBuffer *fs_private_buffer;
    uint64_t fs_private_offset;
} A840RtGraphicsProgramDesc;

typedef struct A840RtRenderStateDesc {
    uint32_t viewport_count;
    uint32_t scissor_count;
    A840Viewport viewports[A840_MAX_VIEWPORTS];
    A840Rect2D scissors[A840_MAX_VIEWPORTS];
    float line_width;
    bool cull_front;
    bool cull_back;
    bool front_clockwise;
    bool depth_test;
    bool depth_write;
    bool depth_clamp;
    bool stencil_test;
    uint8_t depth_compare;
    A840StencilFace front;
    A840StencilFace back;
    uint16_t sample_mask;
    float blend_color[4];
    A840BlendDesc color_blends[A840_MAX_COLOR_TARGETS];
} A840RtRenderStateDesc;

typedef struct A840RtSdsGroupDesc {
    uint32_t group_id;
    uint32_t enable_mask;
    uint32_t state_dwords;
    A840RtBuffer *state_buffer; /* NULL when state_dwords == 0 (disable slot) */
    uint64_t state_offset;
} A840RtSdsGroupDesc;

typedef struct A840RtDrawDesc {
    uint32_t primitive; /* A840_PRIM_* */
    A840IndexType index_type;
    bool use_visibility;
    uint32_t count;
    uint32_t instance_count;
    uint32_t first_instance;
    uint32_t first_vertex;
    int32_t base_vertex;
    uint32_t first_index;
    uint32_t index_capacity;
    A840RtBuffer *index_buffer; /* Required when index_type != A840_INDEX_NONE */
    uint64_t index_offset;
} A840RtDrawDesc;

/* Queue API. */
A840RtStatus a840_rt_queue_create(A840RtDevice *device,
                                  uint32_t priority,
                                  A840RtQueue **out_queue);
void a840_rt_queue_destroy(A840RtQueue *queue);
uint64_t a840_rt_queue_handle(const A840RtQueue *queue);
size_t a840_rt_queue_pending_count(const A840RtQueue *queue);
A840RtStatus a840_rt_queue_poll_completions(A840RtQueue *queue);
A840RtStatus a840_rt_queue_wait_idle(A840RtQueue *queue, uint64_t timeout_ns);

/* Command Buffer API (refcounted). */
A840RtStatus a840_rt_cb_create(A840RtQueue *queue,
                               const A840RtCmdBufferDesc *desc,
                               A840RtCmdBuffer **out_cb);
void a840_rt_cb_retain(A840RtCmdBuffer *cb);
void a840_rt_cb_release(A840RtCmdBuffer *cb);

A840CmdBufferState a840_rt_cb_state(const A840RtCmdBuffer *cb);
size_t a840_rt_cb_used_dwords(const A840RtCmdBuffer *cb);
size_t a840_rt_cb_reloc_count(const A840RtCmdBuffer *cb);
uint64_t a840_rt_cb_submit_fence(const A840RtCmdBuffer *cb);
uint64_t a840_rt_cb_completion_marker(const A840RtCmdBuffer *cb);
A840RouteDecision a840_rt_cb_last_route(const A840RtCmdBuffer *cb);
size_t a840_rt_cb_retained_buffer_count(const A840RtCmdBuffer *cb);
size_t a840_rt_cb_retained_texture_count(const A840RtCmdBuffer *cb);

A840RtStatus a840_rt_cb_add_scheduled_handler(A840RtCmdBuffer *cb,
                                              A840RtCmdCallback fn,
                                              void *user_data);
A840RtStatus a840_rt_cb_add_completed_handler(A840RtCmdBuffer *cb,
                                              A840RtCmdCallback fn,
                                              void *user_data);

/* Explicit sync/timestamp commands between encoders. */
A840RtStatus a840_rt_cb_encode_barrier(A840RtCmdBuffer *cb,
                                       uint32_t cache_mask,
                                       bool wait_mem_writes,
                                       bool wfi);
A840RtStatus a840_rt_cb_encode_mem_write64(A840RtCmdBuffer *cb,
                                           A840RtBuffer *dst_buffer,
                                           uint64_t dst_offset,
                                           uint64_t value);
A840RtStatus a840_rt_cb_encode_timestamp(A840RtCmdBuffer *cb,
                                         A840RtBuffer *dst_buffer,
                                         uint64_t dst_offset,
                                         uint64_t payload,
                                         bool sample_gpu_clock);

/* Commit (auto-appends RB_DONE_TS + validates reloc BOs + FIFO callbacks) & Wait. */
A840RtStatus a840_rt_cb_commit(A840RtCmdBuffer *cb);
A840RtStatus a840_rt_cb_wait_until_completed(A840RtCmdBuffer *cb, uint64_t timeout_ns);

/* Blit Encoder API. */
A840RtStatus a840_rt_cb_begin_blit(A840RtCmdBuffer *cb, A840RtBlitEncoder **out_enc);
A840RtStatus a840_rt_blit_copy_buffer(A840RtBlitEncoder *enc,
                                      A840RtBuffer *src_buffer,
                                      uint64_t src_offset,
                                      A840RtBuffer *dst_buffer,
                                      uint64_t dst_offset,
                                      uint32_t bytes);
A840RtStatus a840_rt_blit_clear_2d(A840RtBlitEncoder *enc,
                                   A840RtTexture *dst_texture,
                                   const A840Rect2D *rect,
                                   const uint32_t color[4]);
A840RtStatus a840_rt_blit_2d(A840RtBlitEncoder *enc,
                             A840RtTexture *src_texture,
                             const A840Rect2D *src_rect,
                             A840RtTexture *dst_texture,
                             const A840Rect2D *dst_rect,
                             uint32_t filter);
A840RtStatus a840_rt_blit_barrier(A840RtBlitEncoder *enc,
                                  uint32_t cache_mask,
                                  bool wait_mem_writes,
                                  bool wfi);
A840RtStatus a840_rt_blit_end(A840RtBlitEncoder *enc);
void a840_rt_blit_destroy(A840RtBlitEncoder *enc);

/* Compute Encoder API. */
A840RtStatus a840_rt_cb_begin_compute(A840RtCmdBuffer *cb, A840RtComputeEncoder **out_enc);
A840RtStatus a840_rt_compute_set_pipeline(A840RtComputeEncoder *enc,
                                          const A840RtComputePipelineDesc *desc);
A840RtStatus a840_rt_compute_set_bindless(A840RtComputeEncoder *enc,
                                          uint32_t count,
                                          A840RtBuffer *const *tables,
                                          const uint64_t *offsets);
A840RtStatus a840_rt_compute_set_consts_direct(A840RtComputeEncoder *enc,
                                               uint32_t dst_vec4,
                                               uint32_t count_vec4,
                                               uint32_t constlen_vec4,
                                               const uint32_t *words);
A840RtStatus a840_rt_compute_set_consts_indirect(A840RtComputeEncoder *enc,
                                                 uint32_t dst_vec4,
                                                 uint32_t count_vec4,
                                                 uint32_t constlen_vec4,
                                                 A840RtBuffer *src_buffer,
                                                 uint64_t src_offset);
A840RtStatus a840_rt_compute_dispatch_threadgroups(A840RtComputeEncoder *enc,
                                                   uint32_t groups_x,
                                                   uint32_t groups_y,
                                                   uint32_t groups_z);
A840RtStatus a840_rt_compute_dispatch_threads(A840RtComputeEncoder *enc,
                                              uint32_t threads_x,
                                              uint32_t threads_y,
                                              uint32_t threads_z);
A840RtStatus a840_rt_compute_dispatch_indirect(A840RtComputeEncoder *enc,
                                               A840RtBuffer *indirect_buffer,
                                               uint64_t offset);
A840RtStatus a840_rt_compute_barrier(A840RtComputeEncoder *enc,
                                     uint32_t cache_mask,
                                     bool wait_mem_writes,
                                     bool wfi);
A840RtStatus a840_rt_compute_end(A840RtComputeEncoder *enc);
void a840_rt_compute_destroy(A840RtComputeEncoder *enc);

/* Basic Sysmem 3D Render Encoder API. */
A840RtStatus a840_rt_cb_begin_render_pass(A840RtCmdBuffer *cb,
                                          const A840RtRenderPassDesc *pass,
                                          A840RtRenderEncoder **out_enc);
A840RtStatus a840_rt_render_set_program(A840RtRenderEncoder *enc,
                                        const A840RtGraphicsProgramDesc *desc);
A840RtStatus a840_rt_render_set_state(A840RtRenderEncoder *enc,
                                      const A840RtRenderStateDesc *desc);
A840RtStatus a840_rt_render_set_sds(A840RtRenderEncoder *enc,
                                    const A840RtSdsGroupDesc *groups,
                                    size_t count);
A840RtStatus a840_rt_render_draw(A840RtRenderEncoder *enc,
                                 const A840RtDrawDesc *desc);
A840RtStatus a840_rt_render_end_pass(A840RtRenderEncoder *enc);
void a840_rt_render_destroy(A840RtRenderEncoder *enc);

#ifdef __cplusplus
}
#endif

#endif /* A840_RUNTIME_CMD_H */
