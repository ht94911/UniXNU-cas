
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using VkResult=int;
constexpr int VK_SUCCESS=0,VK_ERROR_OUT_OF_HOST_MEMORY=-1,VK_ERROR_OUT_OF_DEVICE_MEMORY=-2,VK_ERROR_DEVICE_LOST=-4;
enum chip {A6XX=6,A7XX=7,A8XX=8};
#define VKAPI_ATTR
#define VKAPI_CALL
#define VK_FROM_HANDLE(type,name,handle) auto *name=handle
#define TU_CPU_PHASE(name) ((void)0)
#define TU_CALLX(device,name) name<A8XX>
#define TU_DEBUG(name) (false)
#define TU_DEBUG_START(name) (false)
#define vk_find_struct_const(next,name) nullptr
#define MIN2(a,b) std::min((a),(b))
#define global_iova(cmd,name) UINT64_C(0)
constexpr uint32_t VK_ATTACHMENT_UNUSED=~0u,MAX_VIEWS=8,TU_CMD_DIRTY_FDM=1;
constexpr int VK_COMMAND_BUFFER_LEVEL_PRIMARY=0,TU_CMD_FLAG_CCU_CLEAN_COLOR=1,TU_CMD_FLAG_CCU_CLEAN_DEPTH=2,TU_CMD_BUFFER_STATUS_IDLE=0,TU_CPU_END_PASS=0;
struct VkOffset2D {int32_t x,y;};struct VkExtent2D {uint32_t width,height;};
struct VkSubpassEndInfo {const void *pNext=nullptr;};
struct VkRenderPassFragmentDensityMapOffsetEndInfoEXT {uint32_t fragmentDensityOffsetCount;const VkOffset2D *pFragmentDensityOffsets;};
struct TuCpuCall {TuCpuCall(int,uintptr_t) {}};
struct tu_cs {bool writeable=false;int tag=0;uint32_t *cur=nullptr;std::array<uint32_t,32> storage{};std::vector<uint32_t> words;};
struct tu_subpass {};
struct tu_render_pass {uint32_t subpass_count=1;tu_subpass subpasses[1];bool has_fdm=false;struct {uint32_t attachment=VK_ATTACHMENT_UNUSED;} fragment_density_map;int end_barrier=0;};
struct tu_tiling_config {};struct tu_image_view {};struct tu_framebuffer {};
struct tu_vsc_config {VkExtent2D pipe_count{0,0},pipe0{1,1},tile_count{1,1};};
struct tu_tile_config {struct {uint32_t x,y;} pos;uint32_t pipe,slot_mask;VkExtent2D sysmem_extent,gmem_extent;bool merged_tile=false;uint32_t visible_views=1;};
namespace tu_autotune {using rp_key_opt=int;using rp_ctx_t=void*;}
struct FakeAutotune {template<class T> int emit_preempt_latency_tracking_rp_hash(T*){return 0;}};
struct tu_device {FakeAutotune *autotune;};
struct FakePool {int alloc=0;};static FakePool pool;
struct FakeVk {VkResult record_result=0;int level=0;FakePool *pool=&::pool;};
struct FakeCache {uint32_t pending_flush_bits=0,flush_bits=0;};
struct tu_cmd_buffer {
    tu_device *device;
    FakeVk vk;
    struct {tu_render_pass *pass;tu_tiling_config *tiling;tu_framebuffer *framebuffer;tu_image_view **attachments;
        bool tessfactor_addr_set=false,fdm_subsampled=false;uint32_t dirty=0,total_renderpasses=0;
        struct {bool has_tess=false;}rp;FakeCache cache,renderpass_cache;
    }state;
    tu_cs cs,draw_cs,draw_epilogue_cs,tile_store_cs;
    struct {size_t size=0;}fdm_bin_patchpoints;
    struct {void snapshot_preempt_data(tu_cs*){}}autotune_ctx;
    int trace=0,rp_trace=0;
};
using VkCommandBuffer=tu_cmd_buffer*;
struct u_trace_address {uint64_t offset;};
struct tu_resolve_group {};
enum {RM6_BIN_RESOLVE=2};struct Mode {int mode;bool uses_gmem;};
static struct Counts {unsigned marker=0,render_begin=0,render_end=0,stamps=0,barriers=0,freed=0,resets=0,flushes=0,end=0,tile_alloc=0,tile_free=0,reserves=0;}counts;
static int fail_reserve=0;static bool sysmem=false;
static VkResult tu_cs_reserve_space(tu_cs *cs,uint32_t size) {
    assert(size==2);++counts.reserves;if(cs->tag==1 && fail_reserve)return fail_reserve;
    if(!cs->cur)cs->cur=cs->storage.data();return 0;
}
static void tu_cs_set_writeable(tu_cs *cs,bool value){cs->writeable=value;}
static void emit(tu_cs *cs,uint32_t word){assert(cs->cur);*cs->cur++=word;cs->words.push_back(word);}
template<chip C>static void tu_set_render_mode(tu_cs *cs,Mode mode) {
    if(!cs->cur)assert(!tu_cs_reserve_space(cs,2));++counts.marker;
    emit(cs,0x70c18001);emit(cs,uint32_t(mode.mode)|(mode.uses_gmem?0x100:0));
}
template<chip C>static void tu6_emit_gmem_stores(tu_cmd_buffer*,tu_cs *cs,tu_resolve_group*,const tu_subpass*) {emit(cs,0x12345678);}
template<chip C>static void tu_emit_resolve_group(tu_cmd_buffer*,tu_cs *cs,tu_resolve_group*) {emit(cs,0x9abcdef0);}
static tu_vsc_config vsc;
static const tu_vsc_config *tu_vsc_config(tu_cmd_buffer*,const tu_tiling_config*){return &vsc;}
static VkResult tu_allocate_transient_attachments(tu_cmd_buffer*,bool){return 0;}
static tu_tile_config *tu_calc_tile_config(tu_cmd_buffer*,const struct tu_vsc_config*,const tu_image_view*,const VkOffset2D*) {
    ++counts.tile_alloc;return static_cast<tu_tile_config*>(std::calloc(1,sizeof(tu_tile_config)));
}
static void host_free(void *p){if(p)++counts.tile_free;std::free(p);}
static void vk_command_buffer_set_error(FakeVk *vk,VkResult r){if(!vk->record_result)vk->record_result=r;}
static void tu_cs_begin(tu_cs*){}static void tu_cs_end(tu_cs*){}static void tu_cs_discard_entries(tu_cs*){}
template<chip C,class... T>static void tu6_tile_render_begin(T...){++counts.render_begin;}
template<chip C,class... T>static void tu6_tile_render_end(T...){++counts.render_end;}
template<chip C,class... T>static void tu6_render_tile(T...){}
template<class... T>static void tu_calc_bin_visibility(T...){}
template<class... T>static void tu_identity_frag_area(T...){}
template<class... T>static void tu_disable_draw_states(T...){}
template<chip C,class... T>static void tu_emit_subsampled(T...){}
template<chip C,class... T>static void tu_trace_end_render_pass(T...){}
template<chip C,class... T>static void tu6_lazy_emit_tessfactor_addr(T...){}
static bool use_sysmem_rendering(tu_cmd_buffer*,tu_autotune::rp_ctx_t*,tu_autotune::rp_key_opt){return sysmem;}
static bool tu_pass_trace_on(){return true;}static uint32_t tu_note_render_mode(tu_cmd_buffer*,bool){return 1;}
static uint64_t tu_pass_ts_slot(tu_cmd_buffer*,uint32_t){return 0x10000;}
template<chip C>static void tu_pass_stamp(tu_cs*,uint64_t){++counts.stamps;}
template<chip C>static void tu_cmd_render_sysmem(tu_cmd_buffer*,tu_autotune::rp_ctx_t){++counts.render_begin;++counts.render_end;}
static void tu_clean_all_pending(FakeCache*){}
template<chip C>static void tu_emit_cache_flush_renderpass(tu_cmd_buffer*){++counts.flushes;}
template<chip C>static void tu_emit_cache_flush(tu_cmd_buffer*){++counts.flushes;}
template<class... T>static void trace_end_cmd_buffer(T...){}
template<class... T>static void trace_end_secondary_cmd_buffer(T...){}
static void tu_cmd_buffer_status_gpu_write(tu_cmd_buffer*,int){}
static VkResult vk_command_buffer_end(FakeVk *vk){++counts.end;return vk->record_result;}
static void vk_common_CmdEndRenderPass2(VkCommandBuffer,const VkSubpassEndInfo*){std::abort();}
static unsigned tu_fdm_num_layers(tu_cmd_buffer*){return 1;}
template<chip C>static void tu_emit_custom_resolve_end(tu_cmd_buffer*){}
template<chip C>static void tu_subpass_barrier(tu_cmd_buffer*,int*,bool){++counts.barriers;}
static void vk_free(int*,void*){++counts.freed;}
static void tu_reset_render_pass(tu_cmd_buffer *cmd){++counts.resets;cmd->state.pass=nullptr;cmd->state.attachments=nullptr;}
#define free host_free

namespace old {
template <chip CHIP>
static void
tu6_emit_tile_store_cs(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   const struct tu_render_pass *pass = cmd->state.pass;
   uint32_t subpass_idx = pass->subpass_count - 1;
   const struct tu_subpass *subpass = &pass->subpasses[subpass_idx];

   if (pass->has_fdm)
      tu_cs_set_writeable(cs, true);

   /* We believe setting the marker affects what state HW blocks save/restore
    * during preemption.  So we only emit it before the stores at the end of the
    * last subpass, not other resolves.
    */
   tu_set_render_mode<CHIP>(cs, { .mode = RM6_BIN_RESOLVE, .uses_gmem = true });

   struct tu_resolve_group resolve_group = {};

   tu6_emit_gmem_stores<CHIP>(cmd, cs, &resolve_group, subpass);

   tu_emit_resolve_group<CHIP>(cmd, cs, &resolve_group);

   if (pass->has_fdm)
      tu_cs_set_writeable(cs, false);

}
template <chip CHIP>
static void
tu_cmd_render_tiles(struct tu_cmd_buffer *cmd,
                    tu_autotune::rp_ctx_t rp_ctx,
                    const VkOffset2D *fdm_offsets)
{
   const struct tu_tiling_config *tiling = cmd->state.tiling;
   const struct tu_vsc_config *vsc = tu_vsc_config(cmd, tiling);
   const struct tu_image_view *fdm = NULL;

   /* Preamble save/restore for BINs doesn't handle PC_TESS_BASE, so we
    * assume that PC_TESS_BASE is invalid after any GMEM pass.
    */
   cmd->state.tessfactor_addr_set = false;

   VkResult result = tu_allocate_transient_attachments(cmd, false);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return;
   }

   if (cmd->state.pass->fragment_density_map.attachment != VK_ATTACHMENT_UNUSED) {
      fdm = cmd->state.attachments[cmd->state.pass->fragment_density_map.attachment];
   }

   bool has_fdm = fdm || (TU_DEBUG(FDM) && cmd->state.pass->has_fdm);

   /* If not using FDM make sure not to accidentally apply the offsets */
   if (!has_fdm)
      fdm_offsets = NULL;

   struct tu_tile_config *tiles = NULL;
   if (has_fdm)
      tiles = tu_calc_tile_config(cmd, vsc, fdm, fdm_offsets);

   /* Create gmem stores now (at EndRenderPass time)) because they needed to
    * know whether to allow their conditional execution, which was tied to a
    * state that was known only at the end of the renderpass.  They will be
    * called from tu6_render_tile().
    */
   tu_cs_begin(&cmd->tile_store_cs);
   tu6_emit_tile_store_cs<CHIP>(cmd, &cmd->tile_store_cs);
   tu_cs_end(&cmd->tile_store_cs);

   tu6_tile_render_begin<CHIP>(cmd, &cmd->cs, rp_ctx, fdm_offsets);

   /* Note: we reverse the order of walking the pipes and tiles on every
    * other row, to improve texture cache locality compared to raster order.
    */
   for (uint32_t py = 0; py < vsc->pipe_count.height; py++) {
      uint32_t pipe_row = py * vsc->pipe_count.width;
      for (uint32_t pipe_row_i = 0; pipe_row_i < vsc->pipe_count.width; pipe_row_i++) {
         uint32_t px;
         if (py & 1)
            px = vsc->pipe_count.width - 1 - pipe_row_i;
         else
            px = pipe_row_i;
         uint32_t pipe = pipe_row + px;
         uint32_t tx1 = px * vsc->pipe0.width;
         uint32_t ty1 = py * vsc->pipe0.height;
         uint32_t tx2 = MIN2(tx1 + vsc->pipe0.width, vsc->tile_count.width);
         uint32_t ty2 = MIN2(ty1 + vsc->pipe0.height, vsc->tile_count.height);

         uint32_t tile_row_stride = tx2 - tx1;
         uint32_t slot_row = 0;
         for (uint32_t ty = ty1; ty < ty2; ty++) {
            for (uint32_t tile_row_i = 0; tile_row_i < tile_row_stride; tile_row_i++) {
               uint32_t tx;
               if (ty & 1)
                  tx = tile_row_stride - 1 - tile_row_i;
               else
                  tx = tile_row_i;

               struct tu_tile_config _tile = {
                  .pos = { tx1 + tx, ty },
                  .pipe = pipe,
                  .slot_mask = 1u << (slot_row + tx),
                  .sysmem_extent = { 1, 1 },
                  .gmem_extent = { 1, 1 },
               };
               struct tu_tile_config *tile = &_tile;
               if (has_fdm) {
                  tile = &tiles[ty * vsc->tile_count.width + (tx1 + tx)];
                  if (tile->merged_tile || !tile->visible_views)
                     continue;
               } else {
                  tu_calc_bin_visibility(cmd, tile, fdm_offsets);
                  tu_identity_frag_area(cmd, tile);
               }

               tu6_render_tile<CHIP>(cmd, &cmd->cs, tile, fdm_offsets, rp_ctx, vsc);
            }
            slot_row += tile_row_stride;
         }
      }
   }

   tu6_tile_render_end<CHIP>(cmd, &cmd->cs, rp_ctx);

   /* Outside of renderpasses we assume all draw states are disabled. We do
    * this outside the draw CS for the normal case where 3d gmem stores aren't
    * used. Do this before emitting subsampled blits.
    */
   tu_disable_draw_states(cmd, &cmd->cs);

   if (cmd->state.fdm_subsampled) {
      tu_emit_subsampled<CHIP>(cmd, tiles, tiling, vsc, cmd->state.framebuffer,
                               fdm_offsets);
   }

   tu_trace_end_render_pass<CHIP>(cmd, true);

   /* We have trashed the dynamically-emitted viewport, scissor, and FS params
    * via the patchpoints, so we need to re-emit them if they are reused for a
    * later render pass.
    */
   if (cmd->fdm_bin_patchpoints.size != 0)
      cmd->state.dirty |= TU_CMD_DIRTY_FDM;

   /* Reset the gmem store CS entry lists so that the next render pass
    * does its own stores.
    */
   tu_cs_discard_entries(&cmd->tile_store_cs);

   free(tiles);
}
template <chip CHIP>
void
tu_cmd_render(struct tu_cmd_buffer *cmd_buffer,
              const VkOffset2D *fdm_offsets)
{
   if (cmd_buffer->state.rp.has_tess)
      tu6_lazy_emit_tessfactor_addr<CHIP>(cmd_buffer);

   tu_autotune::rp_key_opt rp_key = cmd_buffer->device->autotune->emit_preempt_latency_tracking_rp_hash(cmd_buffer);

   tu_autotune::rp_ctx_t rp_ctx = NULL;
   const bool sysmem = use_sysmem_rendering(cmd_buffer, &rp_ctx, rp_key);
   uint32_t seq;
   uint64_t stamps;
   { TU_CPU_PHASE(TU_CPU_RENDER_TRACE);
     seq = tu_pass_trace_on() ? tu_note_render_mode(cmd_buffer, sysmem) : 0;
     stamps = seq ? tu_pass_ts_slot(cmd_buffer, seq) : 0;
     if (stamps) tu_pass_stamp<CHIP>(&cmd_buffer->cs, stamps);
   }
   if (sysmem) {
      TU_CPU_PHASE(TU_CPU_RENDER_SYSMEM);
      tu_cmd_render_sysmem<CHIP>(cmd_buffer, rp_ctx);
   } else {
      TU_CPU_PHASE(TU_CPU_RENDER_GMEM);
      tu_cmd_render_tiles<CHIP>(cmd_buffer, rp_ctx, fdm_offsets);
   }
   if (stamps) {
      TU_CPU_PHASE(TU_CPU_RENDER_TRACE);
      tu_pass_stamp<CHIP>(&cmd_buffer->cs, stamps + 8);
   }
}
template <chip CHIP>
VKAPI_ATTR VkResult VKAPI_CALL
tu_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);

   /* We currently flush CCU at the end of the command buffer, like
    * what the blob does. There's implicit synchronization around every
    * vkQueueSubmit, but the kernel only flushes the UCHE, and we don't
    * know yet if this command buffer will be the last in the submit so we
    * have to defensively flush everything else.
    *
    * TODO: We could definitely do better than this, since these flushes
    * aren't required by Vulkan, but we'd need kernel support to do that.
    * Ideally, we'd like the kernel to flush everything afterwards, so that we
    * wouldn't have to do any flushes here, and when submitting multiple
    * command buffers there wouldn't be any unnecessary flushes in between.
    */
   if (cmd_buffer->state.pass) {
      tu_clean_all_pending(&cmd_buffer->state.renderpass_cache);
      tu_emit_cache_flush_renderpass<CHIP>(cmd_buffer);
   } else {
      tu_clean_all_pending(&cmd_buffer->state.cache);
      cmd_buffer->state.cache.flush_bits |=
         TU_CMD_FLAG_CCU_CLEAN_COLOR |
         TU_CMD_FLAG_CCU_CLEAN_DEPTH;
      tu_emit_cache_flush<CHIP>(cmd_buffer);
   }

   if (cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
      struct u_trace_address addr_preempt_latency = {};
      addr_preempt_latency.offset = global_iova(cmd_buffer, max_preemption_latency);

      struct u_trace_address addr_preempt_latency_rp_hash = {};
      addr_preempt_latency_rp_hash.offset = global_iova(cmd_buffer, max_preemption_latency_rp_hash);

      trace_end_cmd_buffer(&cmd_buffer->trace, &cmd_buffer->cs, cmd_buffer, addr_preempt_latency, addr_preempt_latency_rp_hash);

      cmd_buffer->autotune_ctx.snapshot_preempt_data(&cmd_buffer->cs);
   } else {
      trace_end_secondary_cmd_buffer(
         cmd_buffer->state.pass ? &cmd_buffer->rp_trace : &cmd_buffer->trace,
         cmd_buffer->state.pass ? &cmd_buffer->draw_cs : &cmd_buffer->cs);
   }

   if (TU_DEBUG_START(CHECK_CMD_BUFFER_STATUS))
      tu_cmd_buffer_status_gpu_write(cmd_buffer, TU_CMD_BUFFER_STATUS_IDLE);

   tu_cs_end(&cmd_buffer->cs);
   tu_cs_end(&cmd_buffer->draw_cs);
   tu_cs_end(&cmd_buffer->draw_epilogue_cs);

   return vk_command_buffer_end(&cmd_buffer->vk);
}
VKAPI_ATTR void VKAPI_CALL
tu_CmdEndRenderPass2(VkCommandBuffer commandBuffer,
                     const VkSubpassEndInfo *pSubpassEndInfo)
{
   TuCpuCall cpu_call(TU_CPU_END_PASS, reinterpret_cast<uintptr_t>(commandBuffer));
   VK_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);

   if (TU_DEBUG(DYNAMIC)) {
      vk_common_CmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
      return;
   }

   const VkRenderPassFragmentDensityMapOffsetEndInfoEXT *fdm_offset_info =
      vk_find_struct_const(pSubpassEndInfo->pNext,
                           RENDER_PASS_FRAGMENT_DENSITY_MAP_OFFSET_END_INFO_EXT);
   const VkOffset2D *fdm_offsets =
      (fdm_offset_info && fdm_offset_info->fragmentDensityOffsetCount > 0) ?
      fdm_offset_info->pFragmentDensityOffsets : NULL;

   VkOffset2D test_offsets[MAX_VIEWS];
   if (TU_DEBUG(FDM) && TU_DEBUG(FDM_OFFSET)) {
      for (unsigned i = 0; i < tu_fdm_num_layers(cmd_buffer); i++) {
         test_offsets[i] = { 64, 64 };
      }
      fdm_offsets = test_offsets;
   }

   { TU_CPU_PHASE(TU_CPU_END_SUBPASS);
   TU_CALLX(cmd_buffer->device, tu_emit_custom_resolve_end)(cmd_buffer);
   }

   tu_cs_end(&cmd_buffer->draw_cs);
   tu_cs_end(&cmd_buffer->draw_epilogue_cs);
   { TU_CPU_PHASE(TU_CPU_RENDER);
   TU_CALLX(cmd_buffer->device, tu_cmd_render)(cmd_buffer, fdm_offsets);
   }

   cmd_buffer->state.cache.pending_flush_bits |=
      cmd_buffer->state.renderpass_cache.pending_flush_bits;
   { TU_CPU_PHASE(TU_CPU_SUBPASS_BARRIER);
   TU_CALLX(cmd_buffer->device, tu_subpass_barrier)(cmd_buffer, &cmd_buffer->state.pass->end_barrier, true);
   }

   { TU_CPU_PHASE(TU_CPU_HOST_ALLOC);
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->state.attachments);
   }

   tu_reset_render_pass(cmd_buffer);

   cmd_buffer->state.total_renderpasses++;
}
}

namespace candidate {
template <chip CHIP>
static VkResult
tu6_emit_tile_store_cs(struct tu_cmd_buffer *cmd, struct tu_cs *cs)
{
   const struct tu_render_pass *pass = cmd->state.pass;
   uint32_t subpass_idx = pass->subpass_count - 1;
   const struct tu_subpass *subpass = &pass->subpasses[subpass_idx];

   if (pass->has_fdm)
      tu_cs_set_writeable(cs, true);

   /* The first marker may allocate this stream's first BO. GPU032 observed
    * BoMap fail here; tu_cs_reserve's release build discarded that error and
    * the marker wrote through a null cur. Preserve the Vulkan error before
    * entering the unchecked packet emitters. The successful packets are the
    * same; no failed or partial stream may proceed to a tile or submission. */
   VkResult result = tu_cs_reserve_space(cs, 2);
   if (result != VK_SUCCESS) {
      if (pass->has_fdm)
         tu_cs_set_writeable(cs, false);
      return result;
   }

   /* We believe setting the marker affects what state HW blocks save/restore
    * during preemption.  So we only emit it before the stores at the end of the
    * last subpass, not other resolves.
    */
   tu_set_render_mode<CHIP>(cs, { .mode = RM6_BIN_RESOLVE, .uses_gmem = true });

   struct tu_resolve_group resolve_group = {};

   tu6_emit_gmem_stores<CHIP>(cmd, cs, &resolve_group, subpass);

   tu_emit_resolve_group<CHIP>(cmd, cs, &resolve_group);

   if (pass->has_fdm)
      tu_cs_set_writeable(cs, false);

   return VK_SUCCESS;
}
template <chip CHIP>
static void
tu_cmd_render_tiles(struct tu_cmd_buffer *cmd,
                    tu_autotune::rp_ctx_t rp_ctx,
                    const VkOffset2D *fdm_offsets)
{
   const struct tu_tiling_config *tiling = cmd->state.tiling;
   const struct tu_vsc_config *vsc = tu_vsc_config(cmd, tiling);
   const struct tu_image_view *fdm = NULL;

   /* Preamble save/restore for BINs doesn't handle PC_TESS_BASE, so we
    * assume that PC_TESS_BASE is invalid after any GMEM pass.
    */
   cmd->state.tessfactor_addr_set = false;

   VkResult result = tu_allocate_transient_attachments(cmd, false);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd->vk, result);
      return;
   }

   if (cmd->state.pass->fragment_density_map.attachment != VK_ATTACHMENT_UNUSED) {
      fdm = cmd->state.attachments[cmd->state.pass->fragment_density_map.attachment];
   }

   bool has_fdm = fdm || (TU_DEBUG(FDM) && cmd->state.pass->has_fdm);

   /* If not using FDM make sure not to accidentally apply the offsets */
   if (!has_fdm)
      fdm_offsets = NULL;

   struct tu_tile_config *tiles = NULL;
   if (has_fdm)
      tiles = tu_calc_tile_config(cmd, vsc, fdm, fdm_offsets);

   /* Create gmem stores now (at EndRenderPass time)) because they needed to
    * know whether to allow their conditional execution, which was tied to a
    * state that was known only at the end of the renderpass.  They will be
    * called from tu6_render_tile().
    */
   tu_cs_begin(&cmd->tile_store_cs);
   result = tu6_emit_tile_store_cs<CHIP>(cmd, &cmd->tile_store_cs);
   if (result != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd->vk, result);
      free(tiles);
      return;
   }
   tu_cs_end(&cmd->tile_store_cs);

   tu6_tile_render_begin<CHIP>(cmd, &cmd->cs, rp_ctx, fdm_offsets);

   /* Note: we reverse the order of walking the pipes and tiles on every
    * other row, to improve texture cache locality compared to raster order.
    */
   for (uint32_t py = 0; py < vsc->pipe_count.height; py++) {
      uint32_t pipe_row = py * vsc->pipe_count.width;
      for (uint32_t pipe_row_i = 0; pipe_row_i < vsc->pipe_count.width; pipe_row_i++) {
         uint32_t px;
         if (py & 1)
            px = vsc->pipe_count.width - 1 - pipe_row_i;
         else
            px = pipe_row_i;
         uint32_t pipe = pipe_row + px;
         uint32_t tx1 = px * vsc->pipe0.width;
         uint32_t ty1 = py * vsc->pipe0.height;
         uint32_t tx2 = MIN2(tx1 + vsc->pipe0.width, vsc->tile_count.width);
         uint32_t ty2 = MIN2(ty1 + vsc->pipe0.height, vsc->tile_count.height);

         uint32_t tile_row_stride = tx2 - tx1;
         uint32_t slot_row = 0;
         for (uint32_t ty = ty1; ty < ty2; ty++) {
            for (uint32_t tile_row_i = 0; tile_row_i < tile_row_stride; tile_row_i++) {
               uint32_t tx;
               if (ty & 1)
                  tx = tile_row_stride - 1 - tile_row_i;
               else
                  tx = tile_row_i;

               struct tu_tile_config _tile = {
                  .pos = { tx1 + tx, ty },
                  .pipe = pipe,
                  .slot_mask = 1u << (slot_row + tx),
                  .sysmem_extent = { 1, 1 },
                  .gmem_extent = { 1, 1 },
               };
               struct tu_tile_config *tile = &_tile;
               if (has_fdm) {
                  tile = &tiles[ty * vsc->tile_count.width + (tx1 + tx)];
                  if (tile->merged_tile || !tile->visible_views)
                     continue;
               } else {
                  tu_calc_bin_visibility(cmd, tile, fdm_offsets);
                  tu_identity_frag_area(cmd, tile);
               }

               tu6_render_tile<CHIP>(cmd, &cmd->cs, tile, fdm_offsets, rp_ctx, vsc);
            }
            slot_row += tile_row_stride;
         }
      }
   }

   tu6_tile_render_end<CHIP>(cmd, &cmd->cs, rp_ctx);

   /* Outside of renderpasses we assume all draw states are disabled. We do
    * this outside the draw CS for the normal case where 3d gmem stores aren't
    * used. Do this before emitting subsampled blits.
    */
   tu_disable_draw_states(cmd, &cmd->cs);

   if (cmd->state.fdm_subsampled) {
      tu_emit_subsampled<CHIP>(cmd, tiles, tiling, vsc, cmd->state.framebuffer,
                               fdm_offsets);
   }

   tu_trace_end_render_pass<CHIP>(cmd, true);

   /* We have trashed the dynamically-emitted viewport, scissor, and FS params
    * via the patchpoints, so we need to re-emit them if they are reused for a
    * later render pass.
    */
   if (cmd->fdm_bin_patchpoints.size != 0)
      cmd->state.dirty |= TU_CMD_DIRTY_FDM;

   /* Reset the gmem store CS entry lists so that the next render pass
    * does its own stores.
    */
   tu_cs_discard_entries(&cmd->tile_store_cs);

   free(tiles);
}
template <chip CHIP>
void
tu_cmd_render(struct tu_cmd_buffer *cmd_buffer,
              const VkOffset2D *fdm_offsets)
{
   if (cmd_buffer->vk.record_result != VK_SUCCESS)
      return;
   if (cmd_buffer->state.rp.has_tess)
      tu6_lazy_emit_tessfactor_addr<CHIP>(cmd_buffer);

   tu_autotune::rp_key_opt rp_key = cmd_buffer->device->autotune->emit_preempt_latency_tracking_rp_hash(cmd_buffer);

   tu_autotune::rp_ctx_t rp_ctx = NULL;
   const bool sysmem = use_sysmem_rendering(cmd_buffer, &rp_ctx, rp_key);
   uint32_t seq;
   uint64_t stamps;
   { TU_CPU_PHASE(TU_CPU_RENDER_TRACE);
     seq = tu_pass_trace_on() ? tu_note_render_mode(cmd_buffer, sysmem) : 0;
     stamps = seq ? tu_pass_ts_slot(cmd_buffer, seq) : 0;
     if (stamps) tu_pass_stamp<CHIP>(&cmd_buffer->cs, stamps);
   }
   if (sysmem) {
      TU_CPU_PHASE(TU_CPU_RENDER_SYSMEM);
      tu_cmd_render_sysmem<CHIP>(cmd_buffer, rp_ctx);
   } else {
      TU_CPU_PHASE(TU_CPU_RENDER_GMEM);
      tu_cmd_render_tiles<CHIP>(cmd_buffer, rp_ctx, fdm_offsets);
   }
   if (stamps && cmd_buffer->vk.record_result == VK_SUCCESS) {
      TU_CPU_PHASE(TU_CPU_RENDER_TRACE);
      tu_pass_stamp<CHIP>(&cmd_buffer->cs, stamps + 8);
   }
}
template <chip CHIP>
VKAPI_ATTR VkResult VKAPI_CALL
tu_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   VK_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);

   if (cmd_buffer->vk.record_result != VK_SUCCESS)
      return vk_command_buffer_end(&cmd_buffer->vk);

   /* We currently flush CCU at the end of the command buffer, like
    * what the blob does. There's implicit synchronization around every
    * vkQueueSubmit, but the kernel only flushes the UCHE, and we don't
    * know yet if this command buffer will be the last in the submit so we
    * have to defensively flush everything else.
    *
    * TODO: We could definitely do better than this, since these flushes
    * aren't required by Vulkan, but we'd need kernel support to do that.
    * Ideally, we'd like the kernel to flush everything afterwards, so that we
    * wouldn't have to do any flushes here, and when submitting multiple
    * command buffers there wouldn't be any unnecessary flushes in between.
    */
   if (cmd_buffer->state.pass) {
      tu_clean_all_pending(&cmd_buffer->state.renderpass_cache);
      tu_emit_cache_flush_renderpass<CHIP>(cmd_buffer);
   } else {
      tu_clean_all_pending(&cmd_buffer->state.cache);
      cmd_buffer->state.cache.flush_bits |=
         TU_CMD_FLAG_CCU_CLEAN_COLOR |
         TU_CMD_FLAG_CCU_CLEAN_DEPTH;
      tu_emit_cache_flush<CHIP>(cmd_buffer);
   }

   if (cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
      struct u_trace_address addr_preempt_latency = {};
      addr_preempt_latency.offset = global_iova(cmd_buffer, max_preemption_latency);

      struct u_trace_address addr_preempt_latency_rp_hash = {};
      addr_preempt_latency_rp_hash.offset = global_iova(cmd_buffer, max_preemption_latency_rp_hash);

      trace_end_cmd_buffer(&cmd_buffer->trace, &cmd_buffer->cs, cmd_buffer, addr_preempt_latency, addr_preempt_latency_rp_hash);

      cmd_buffer->autotune_ctx.snapshot_preempt_data(&cmd_buffer->cs);
   } else {
      trace_end_secondary_cmd_buffer(
         cmd_buffer->state.pass ? &cmd_buffer->rp_trace : &cmd_buffer->trace,
         cmd_buffer->state.pass ? &cmd_buffer->draw_cs : &cmd_buffer->cs);
   }

   if (TU_DEBUG_START(CHECK_CMD_BUFFER_STATUS))
      tu_cmd_buffer_status_gpu_write(cmd_buffer, TU_CMD_BUFFER_STATUS_IDLE);

   tu_cs_end(&cmd_buffer->cs);
   tu_cs_end(&cmd_buffer->draw_cs);
   tu_cs_end(&cmd_buffer->draw_epilogue_cs);

   return vk_command_buffer_end(&cmd_buffer->vk);
}
VKAPI_ATTR void VKAPI_CALL
tu_CmdEndRenderPass2(VkCommandBuffer commandBuffer,
                     const VkSubpassEndInfo *pSubpassEndInfo)
{
   TuCpuCall cpu_call(TU_CPU_END_PASS, reinterpret_cast<uintptr_t>(commandBuffer));
   VK_FROM_HANDLE(tu_cmd_buffer, cmd_buffer, commandBuffer);

   if (TU_DEBUG(DYNAMIC)) {
      vk_common_CmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
      return;
   }

   const VkRenderPassFragmentDensityMapOffsetEndInfoEXT *fdm_offset_info =
      vk_find_struct_const(pSubpassEndInfo->pNext,
                           RENDER_PASS_FRAGMENT_DENSITY_MAP_OFFSET_END_INFO_EXT);
   const VkOffset2D *fdm_offsets =
      (fdm_offset_info && fdm_offset_info->fragmentDensityOffsetCount > 0) ?
      fdm_offset_info->pFragmentDensityOffsets : NULL;

   VkOffset2D test_offsets[MAX_VIEWS];
   if (TU_DEBUG(FDM) && TU_DEBUG(FDM_OFFSET)) {
      for (unsigned i = 0; i < tu_fdm_num_layers(cmd_buffer); i++) {
         test_offsets[i] = { 64, 64 };
      }
      fdm_offsets = test_offsets;
   }

   { TU_CPU_PHASE(TU_CPU_END_SUBPASS);
   TU_CALLX(cmd_buffer->device, tu_emit_custom_resolve_end)(cmd_buffer);
   }

   tu_cs_end(&cmd_buffer->draw_cs);
   tu_cs_end(&cmd_buffer->draw_epilogue_cs);
   { TU_CPU_PHASE(TU_CPU_RENDER);
   TU_CALLX(cmd_buffer->device, tu_cmd_render)(cmd_buffer, fdm_offsets);
   }

   cmd_buffer->state.cache.pending_flush_bits |=
      cmd_buffer->state.renderpass_cache.pending_flush_bits;
   { TU_CPU_PHASE(TU_CPU_SUBPASS_BARRIER);
   if (cmd_buffer->vk.record_result == VK_SUCCESS)
      TU_CALLX(cmd_buffer->device, tu_subpass_barrier)(cmd_buffer, &cmd_buffer->state.pass->end_barrier, true);
   }

   { TU_CPU_PHASE(TU_CPU_HOST_ALLOC);
   vk_free(&cmd_buffer->vk.pool->alloc, cmd_buffer->state.attachments);
   }

   tu_reset_render_pass(cmd_buffer);

   cmd_buffer->state.total_renderpasses++;
}
}

#undef free
static FakeAutotune autotune;static tu_device device{&autotune};static tu_tiling_config tiling;static tu_framebuffer framebuffer;static tu_image_view image;static tu_image_view *attachments[1]={&image};
static unsigned cases;
static std::vector<uint32_t> exercise(bool modified,bool fdm,int failure,bool system=false) {
    counts={};fail_reserve=failure;sysmem=system;tu_render_pass pass;pass.has_fdm=fdm;if(fdm)pass.fragment_density_map.attachment=0;
    tu_cmd_buffer cmd{};cmd.device=&device;cmd.state.pass=&pass;cmd.state.tiling=&tiling;cmd.state.framebuffer=&framebuffer;cmd.state.attachments=attachments;cmd.tile_store_cs.tag=1;
    VkSubpassEndInfo info;
    if(modified)candidate::tu_CmdEndRenderPass2(&cmd,&info);else old::tu_CmdEndRenderPass2(&cmd,&info);
    VkResult result=modified?candidate::tu_EndCommandBuffer<A8XX>(&cmd):old::tu_EndCommandBuffer<A8XX>(&cmd);
    assert(result==failure && cmd.vk.record_result==failure && !cmd.tile_store_cs.writeable);
    assert(counts.freed==1 && counts.resets==1 && counts.end==1);
    assert(counts.tile_alloc==counts.tile_free);
    if(failure)assert(!counts.marker && !counts.render_begin && !counts.render_end && counts.stamps==1 && !counts.barriers && !counts.flushes && cmd.tile_store_cs.words.empty());
    else assert(counts.marker==unsigned(!system) && counts.render_begin==1 && counts.render_end==1 && counts.stamps==2 && counts.barriers==1 && counts.flushes==1);
    ++cases;return cmd.tile_store_cs.words;
}
int main() {
    for(bool fdm:{false,true}) {
        auto original=exercise(false,fdm,0);auto fixed=exercise(true,fdm,0);assert(original==fixed);
        for(int failure:{-1,-2,-4})exercise(true,fdm,failure);
    }
    assert(exercise(false,false,0,true)==exercise(true,false,0,true));
    counts={};tu_cmd_buffer failed{};failed.vk.record_result=-4;
    candidate::tu_cmd_render<A8XX>(&failed,nullptr);assert(candidate::tu_EndCommandBuffer<A8XX>(&failed)==-4);
    assert(!counts.stamps && !counts.flushes && !counts.reserves && counts.end==1);++cases;
    std::printf("PASS %u cases: unchanged successful emitter inputs/words; initial tile-store reserve failures propagate through real EndRenderPass/EndCommandBuffer functions; FDM cleanup and no post-failure emit\n",cases);
}
