/* SPDX-License-Identifier: MIT */
#define A840_EMIT_OPS_INTERNAL
#include "a840_emit_ops.h"
#include "a840_emit_gmem.h"
#include "Adreno840Caps.h"
#include "a840_context_init.h"
#include "gen/a840_bitfields.h"
#include "gen/a840_gmem_bitfields.h"

#define TRY A840_OPS_TRY
/* Worst operation: markers + 10 transfers with independent flag BOs. */
typedef struct GmemBatch {
    uint32_t words[640];
    A840Reloc relocs[2 * A840_GMEM_PLANES];
    A840CmdStream cs;
} GmemBatch;

static A840EmitStatus begin(GmemBatch *b)
{
    return a840_cs_init(&b->cs, b->words, sizeof(b->words)/sizeof(b->words[0]),
                        b->relocs, 2 * A840_GMEM_PLANES);
}

static A840EmitStatus finish(A840CmdStream *dst, const GmemBatch *b)
{
    A840OpsSpan words, relocs, metadata;
    TRY(a840_ops_cs_storage(dst, &words, &relocs, &metadata));
    size_t n = b->cs.used_dwords, nr = dst->relocs ? b->cs.reloc_count : 0;
    if (n > dst->capacity_dwords - dst->used_dwords || nr > dst->reloc_capacity - dst->reloc_count)
        return A840_EMIT_CAPACITY;
    if (nr && dst->used_dwords > UINT32_MAX - n) return A840_EMIT_RANGE_ERROR;
    if (n) memcpy(dst->words + dst->used_dwords, b->words, n * sizeof(uint32_t));
    for (size_t i = 0; i < nr; ++i) {
        A840Reloc r = b->relocs[i];
        r.dword_offset += (uint32_t)dst->used_dwords;
        dst->relocs[dst->reloc_count++] = r;
    }
    dst->used_dwords += n;
    return A840_EMIT_OK;
}

static A840EmitStatus reg(A840CmdStream *cs, uint32_t id, uint32_t v)
{ return a840_cs_emit_pkt4(cs, id, &v, 1); }
static A840EmitStatus op(A840CmdStream *cs, uint32_t id, uint32_t v)
{ return a840_cs_emit_pkt7(cs, id, &v, 1); }
static A840EmitStatus event(A840CmdStream *cs, uint32_t v)
{ return op(cs, A840_CP_EVENT_WRITE7, v); }
static uint32_t xy(uint32_t x, uint32_t y) { return x | (y << GM_Y_SHIFT); }
static uint32_t bin_size(const A840GmemPlan *p)
{ return (p->tile_w >> GM_BIN_W_SHR) | ((p->tile_h >> GM_BIN_H_SHR) << GM_BIN_H_SHIFT); }
static uint32_t min_u(uint32_t a, uint32_t b) { return a < b ? a : b; }

static A840EmitStatus bin_regs(A840CmdStream *cs, const A840GmemPlan *p, uint32_t mode, bool hardware_binning)
{
    uint32_t size = bin_size(p), ctrl = size | (mode << GM_BIN_MODE_SHIFT);
    uint32_t feedback = mode == GM_RENDERING && hardware_binning ?
        GM_EARLY_OR_EARLY_LATE_Z : GM_EARLY_LATE_Z;
    ctrl |= feedback << GM_LRZ_FEEDBACK_SHIFT;
    uint32_t dimensions[A840_GMEM_COLORS], dim = xy(p->tile_w, p->tile_h);
    for (unsigned i = 0; i < A840_GMEM_COLORS; ++i) dimensions[i] = dim;
    TRY(reg(cs, A840_REG_GRAS_SC_BIN_CNTL, ctrl));
    TRY(reg(cs, A840_REG_RB_CNTL, ctrl));
    TRY(reg(cs, A840_REG_TPL1_BIN_SIZE, size));
    TRY(reg(cs, A840_REG_TPL1_A2D_BIN_SIZE, size));
    TRY(reg(cs, A840_REG_SP_BIN_SIZE, size));
    TRY(a840_cs_emit_pkt4(cs, A840_REG_RB_MRT_GMEM_DIMENSION_0_REG, dimensions, A840_GMEM_COLORS));
    TRY(reg(cs, A840_REG_RB_DEPTH_GMEM_DIMENSION, dim));
    TRY(reg(cs, A840_REG_RB_STENCIL_GMEM_DIMENSION, dim));
    return reg(cs, A840_REG_RB_RESOLVE_CNTL_3, size);
}

static A840EmitStatus window(A840CmdStream *cs, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t rect[] = {xy(x,y), xy(x+w-1,y+h-1)};
    TRY(a840_cs_emit_pkt4(cs, A840_REG_GRAS_SC_WINDOW_SCISSOR_TL, rect, 2));
    TRY(a840_cs_emit_pkt4(cs, A840_REG_GRAS_A2D_SCISSOR_TL, rect, 2));
    /* Keep draw/scissor edges exact. Only resolve the padded 16x4 blocks. */
    uint32_t right = (x+w + A840_LIMIT_GMEM_ALIGN_W-1) / A840_LIMIT_GMEM_ALIGN_W * A840_LIMIT_GMEM_ALIGN_W;
    uint32_t bottom = (y+h + A840_LIMIT_GMEM_ALIGN_H-1) / A840_LIMIT_GMEM_ALIGN_H * A840_LIMIT_GMEM_ALIGN_H;
    rect[1] = xy(right-1,bottom-1);
    TRY(a840_cs_emit_pkt4(cs, A840_REG_RB_RESOLVE_CNTL_1, rect, 2));
    const uint32_t offsets[] = {A840_REG_RB_WINDOW_OFFSET, A840_REG_RB_RESOLVE_WINDOW_OFFSET,
        A840_REG_SP_WINDOW_OFFSET, A840_REG_TPL1_WINDOW_OFFSET, A840_REG_TPL1_A2D_WINDOW_OFFSET};
    for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); ++i)
        TRY(reg(cs, offsets[i], rect[0]));
    return A840_EMIT_OK;
}

/* fd6_gmem_cache.h:39-98; tu_cmd_buffer.cc:596-748. VPC programming
 * belongs to both threads, RB_CCU_CACHE_CNTL to BR. No WFI for this state. */
static A840EmitStatus cache_mode(A840CmdStream *cs, bool gmem)
{
    uint32_t attr = gmem ? A840_LIMIT_GMEM_VPC_ATTR_BUF_SIZE : A840_LIMIT_SYSMEM_VPC_ATTR_BUF_SIZE;
    uint32_t pos = gmem ? 24576u : 65536u, bv = 32768u;
    uint32_t bv_base = A840_KAANAPALI_GMEM_BYTES - A840_LIMIT_NUM_CCU * bv;
    uint32_t attr_base = bv_base - A840_LIMIT_NUM_CCU * attr;
    uint32_t pos_base = attr_base - A840_LIMIT_NUM_CCU * pos;
    uint32_t color = pos_base - A840_LIMIT_NUM_CCU * (gmem ?
        A840_LIMIT_GMEM_PER_CCU_COLOR_CACHE_SIZE : A840_LIMIT_SYSMEM_PER_CCU_COLOR_CACHE_SIZE);
    uint32_t depth = color - A840_LIMIT_NUM_CCU * (gmem ?
        A840_LIMIT_GMEM_PER_CCU_DEPTH_CACHE_SIZE : A840_LIMIT_SYSMEM_PER_CCU_DEPTH_CACHE_SIZE);
    uint32_t ctrl = (color >> GM_CCU_OFFSET_SHR) | ((depth >> GM_CCU_OFFSET_SHR) << GM_CCU_DEPTH_OFFSET_SHIFT) |
        (gmem ? (uint32_t)GM_CCU_EIGHTH << GM_CCU_COLOR_SIZE_SHIFT :
                (uint32_t)GM_CCU_THREE_QUARTER << GM_CCU_DEPTH_SIZE_SHIFT);
    TRY(op(cs,A840_CP_THREAD_CONTROL, GM_THREAD_BR | GM_NONCONCURRENT));
    if (gmem) TRY(a840_emit_cache_flush_inv(cs,A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_CCU_FLUSH_DEPTH));
    TRY(event(cs,A840_EVENT_CCU_INVALIDATE_COLOR));
    TRY(event(cs,A840_CTX_EVENT_CCU_INVALIDATE_DEPTH));
    TRY(reg(cs,A840_REG_RB_CCU_CACHE_CNTL,ctrl));
    TRY(op(cs,A840_CP_THREAD_CONTROL,GM_THREAD_BOTH));
    TRY(reg(cs,A840_REG_VPC_ATTR_BUF_GMEM_SIZE,attr));
    TRY(reg(cs,A840_REG_VPC_ATTR_BUF_GMEM_BASE,attr_base));
    TRY(reg(cs,A840_REG_PC_ATTR_BUF_GMEM_SIZE,attr));
    TRY(reg(cs,A840_REG_VPC_POS_BUF_GMEM_SIZE,pos));
    TRY(reg(cs,A840_REG_VPC_POS_BUF_GMEM_BASE,pos_base));
    TRY(reg(cs,A840_REG_PC_POS_BUF_GMEM_SIZE,pos));
    TRY(reg(cs,A840_REG_VPC_BV_POS_BUF_GMEM_BASE,bv_base));
    TRY(reg(cs,A840_REG_VPC_BV_POS_BUF_GMEM_SIZE,bv));
    TRY(reg(cs,A840_REG_PC_BV_POS_BUF_GMEM_SIZE,bv));
    return op(cs,A840_CP_THREAD_CONTROL,GM_THREAD_BR | GM_NONCONCURRENT);
}

static A840EmitStatus check_range(A840GmemRange r, uint64_t need, uint32_t align)
{
    if (need > r.bytes) return A840_EMIT_RANGE_ERROR;
    return a840_ops_va(r.va, need, align);
}

static A840EmitStatus check_vsc(const A840GmemVsc *v)
{
    if (v->draw_stride <= 64 || v->primitive_stride <= 64 ||
        (v->draw_stride | v->primitive_stride) % 64 ||
        v->draw_stride > UINT32_MAX / A840_GMEM_PIPES ||
        v->primitive_stride > UINT32_MAX / A840_GMEM_PIPES) return A840_EMIT_RANGE_ERROR;
    TRY(check_range(v->draw, v->draw_stride * A840_GMEM_PIPES, 64));
    TRY(check_range(v->primitive, v->primitive_stride * A840_GMEM_PIPES, 64));
    return check_range(v->sizes, A840_GMEM_PIPES * sizeof(uint32_t), 64);
}

static A840EmitStatus vsc_streams(GmemBatch *b, const A840GmemVsc *v)
{
    const A840GmemRange ranges[] = {v->draw, v->sizes, v->primitive};
    const uint32_t ids[] = {GM_DRAW_STREAM, GM_SIZE_STREAM, GM_PRIM_STREAM};
    uint32_t payload[9];
    size_t start = b->cs.used_dwords;
    TRY(check_vsc(v));
    for (unsigned i = 0; i < 3; ++i) {
        payload[3*i] = ids[i];
        payload[3*i+1] = (uint32_t)ranges[i].va;
        payload[3*i+2] = (uint32_t)(ranges[i].va >> 32);
    }
    TRY(a840_cs_emit_pkt7(&b->cs, A840_CP_SET_PSEUDO_REG, payload, 9));
    for (unsigned i = 0; i < 3; ++i)
        b->relocs[b->cs.reloc_count++] = (A840Reloc){(uint32_t)start+2+3*i,
            A840_RELOC_READ | A840_RELOC_WRITE, ranges[i].bo, ranges[i].va};
    uint32_t draw[] = {v->draw_stride, v->draw_stride - 64};
    uint32_t prim[] = {v->primitive_stride, v->primitive_stride - 64};
    TRY(a840_cs_emit_pkt4(&b->cs, A840_REG_VSC_PIPE_DATA_DRAW_STRIDE, draw, 2));
    return a840_cs_emit_pkt4(&b->cs, A840_REG_VSC_PIPE_DATA_PRIM_STRIDE, prim, 2);
}

A840EmitStatus a840_emit_gmem_binning_begin(A840CmdStream *dst, const A840GmemPlan *p,
                                             const A840GmemVsc *v)
{
    GmemBatch b;
    uint32_t pipes[A840_GMEM_PIPES] = {0};
    TRY(a840_gmem_validate(p));
    TRY(begin(&b));
    TRY(cache_mode(&b.cs, true));
    TRY(reg(&b.cs,A840_REG_RB_BUFFER_CNTL,0));
    TRY(reg(&b.cs,A840_REG_RB_CLEAR_TARGET,GM_CLEAR_TARGET));
    TRY(window(&b.cs,0,0,p->pass.width,p->pass.height));
    TRY(op(&b.cs,A840_CP_SET_MARKER,GM_VISIBILITY));
    TRY(bin_regs(&b.cs,p,GM_BINNING,v != NULL));
    TRY(reg(&b.cs,A840_REG_VSC_BIN_SIZE,bin_size(p)));
    TRY(reg(&b.cs,A840_REG_VSC_EXPANDED_BIN_CNTL,
            (p->tiles_x << GM_VSC_NX_SHIFT) | (p->tiles_y << GM_VSC_NY_SHIFT)));
    for (uint32_t py = 0; py < p->pipes_y; ++py) {
        for (uint32_t px = 0; px < p->pipes_x; ++px) {
            uint32_t x = px*p->pipe_w, y = py*p->pipe_h;
            uint32_t w = min_u(p->pipe_w,p->tiles_x-x), h = min_u(p->pipe_h,p->tiles_y-y);
            if (v && w*h > 32) return A840_EMIT_UNSUPPORTED;
            pipes[py*p->pipes_x+px] = x | (y << GM_PIPE_Y_SHIFT) |
                (w << GM_PIPE_W_SHIFT) | (h << GM_PIPE_H_SHIFT);
        }
    }
    TRY(a840_cs_emit_pkt4(&b.cs,A840_REG_VSC_PIPE_CONFIG_0_REG,pipes,A840_GMEM_PIPES));
    if (v) {
        TRY(vsc_streams(&b,v));
        TRY(reg(&b.cs,A840_REG_VSC_UNKNOWN_0D08,0));
        TRY(op(&b.cs,A840_CP_SET_VISIBILITY_OVERRIDE,1));
        TRY(op(&b.cs,A840_CP_SET_MODE,1));
        TRY(a840_emit_wfi(&b.cs)); /* tu6_emit_binning_pass: before VFD changes */
        TRY(reg(&b.cs,A840_REG_VFD_RENDER_MODE,GM_BINNING));
        TRY(event(&b.cs,GM_EV_VSC_START));
    }
    return finish(dst,&b);
}

A840EmitStatus a840_emit_gmem_binning_end(A840CmdStream *dst, const A840GmemVsc *v)
{
    GmemBatch b;
    TRY(begin(&b));
    if (v) {
        TRY(check_vsc(v));
        TRY(event(&b.cs,GM_EV_VSC_END));
        TRY(event(&b.cs,GM_EV_CACHE_CLEAN));
        TRY(a840_emit_wfi(&b.cs)); /* publish the VSC stream to CP */
        TRY(a840_cs_emit_pkt7(&b.cs,A840_CP_WAIT_FOR_ME,NULL,0));
    }
    TRY(op(&b.cs,A840_CP_SET_MARKER,GM_VISIBILITY_END));
    return finish(dst,&b);
}

A840EmitStatus a840_emit_gmem_tile_select(A840CmdStream *dst, const A840GmemPlan *p,
                                           uint32_t tx, uint32_t ty, const A840GmemVsc *v)
{
    GmemBatch b;
    A840GmemTile t;
    const uint32_t bases[] = {A840_REG_RB_MRT_0_BASE_GMEM,A840_REG_RB_MRT_1_BASE_GMEM,
        A840_REG_RB_MRT_2_BASE_GMEM,A840_REG_RB_MRT_3_BASE_GMEM,A840_REG_RB_MRT_4_BASE_GMEM,
        A840_REG_RB_MRT_5_BASE_GMEM,A840_REG_RB_MRT_6_BASE_GMEM,A840_REG_RB_MRT_7_BASE_GMEM,
        A840_REG_RB_DEPTH_GMEM_BASE,A840_REG_RB_STENCIL_GMEM_BASE};
    TRY(a840_gmem_tile(p,tx,ty,&t));
    TRY(begin(&b));
    TRY(op(&b.cs,A840_CP_SET_MARKER,GM_RENDER_START | GM_USES_GMEM));
    TRY(bin_regs(&b.cs,p,GM_RENDERING,v != NULL));
    TRY(reg(&b.cs,A840_REG_VFD_RENDER_MODE,GM_RENDERING));
    TRY(window(&b.cs,t.x,t.y,t.width,t.height));
    for (unsigned i = 0; i < A840_GMEM_PLANES; ++i)
        TRY(reg(&b.cs,bases[i],p->planes[i].offset));
    if (v) {
        TRY(check_vsc(v));
        uint32_t w = min_u(p->pipe_w,p->tiles_x-(tx/p->pipe_w)*p->pipe_w);
        uint32_t h = min_u(p->pipe_h,p->tiles_y-(ty/p->pipe_h)*p->pipe_h);
        if (w*h > 32) return A840_EMIT_UNSUPPORTED;
        uint32_t payload[] = {1u | ((w*h) << GM_VSC_SIZE_SHIFT) |
            (t.slot << GM_VSC_SLOT_SHIFT) | GM_VSC_ABS, UINT32_C(1) << t.slot,
            t.pipe*v->draw_stride,t.pipe*4,t.pipe*v->primitive_stride};
        TRY(a840_cs_emit_pkt7(&b.cs,A840_CP_WAIT_FOR_ME,NULL,0));
        TRY(op(&b.cs,A840_CP_SET_MODE,0));
        TRY(a840_cs_emit_pkt7(&b.cs,A840_CP_SET_BIN_DATA5_OFFSET,payload,5));
    }
    TRY(op(&b.cs,A840_CP_SET_VISIBILITY_OVERRIDE,!v));
    TRY(op(&b.cs,A840_CP_SET_MODE,0));
    return finish(dst,&b);
}

static uint32_t rounded(uint32_t value, uint32_t alignment)
{ return (value + alignment - 1) / alignment * alignment; }

static uint32_t sample_bits(uint32_t samples)
{ return (samples >> 1) << GM_SAMPLES_SHIFT; }

static const A840FormatCaps *plane_format(uint32_t id, unsigned slot, uint32_t cpp, bool srgb)
{
    uint32_t aspect = slot == A840_GMEM_DEPTH ? A840_FORMAT_DEPTH :
                      slot == A840_GMEM_STENCIL ? A840_FORMAT_STENCIL : 0;
    for (size_t i = 0; i < A840_FORMAT_CAPS_COUNT; ++i) {
        const A840FormatCaps *f = &a840_format_caps_data[i];
        if (!!(f->flags & A840_FORMAT_SRGB) == srgb && f->color_fmt_id == id && f->renderable && f->block_bits == (uint64_t)cpp * 8 &&
            (f->flags & (A840_FORMAT_DEPTH | A840_FORMAT_STENCIL | A840_FORMAT_COMPRESSED)) == aspect)
            return f;
    }
    return NULL;
}

/* Padded, one-layer FDL6 fast path. Treat data and flag BO ranges separately
 * even when the FDL allocator placed the two planes in the same BO. */
static A840EmitStatus surface_state( const A840GmemPlan *p, unsigned slot,
                                const A840GmemBinding *b, const A840FormatCaps *format, uint32_t type,
                                uint32_t *sys_info, uint32_t *flag_pitch)
{
    const A840GmemAttachment *a = &p->pass.attachments[slot];
    bool resolving = type == GM_STORE && a->store == A840_STORE_RESOLVE;
    const A840GmemSurface *s = resolving ? &b->resolve : &b->image;
    uint32_t samples = resolving ? 1u : a->samples, cpp = a->cpp * samples;
    if (cpp > 64 || (cpp & (cpp-1)) || !b->format || b->format > UINT8_MAX ||
        s->swap > 3 || (a->ubwc && !s->tiled) || (!s->tiled && slot >= A840_GMEM_DEPTH))
        return A840_EMIT_UNSUPPORTED;
    bool rg8 = (format->flags & A840_FORMAT_R8G8_FAULTY_FAST_CLEAR_QUIRK) != 0;
    if (a->ubwc && !format->ubwc_supported) return A840_EMIT_UNSUPPORTED;
    uint32_t pitch_align = s->tiled && rg8 ? 128 : s->tiled ? 64 * (cpp < 4 ? cpp * 2 : cpp) : 64;
    uint32_t height_align = s->tiled ? (cpp == 1 || rg8 ? 32 : 16) : A840_LIMIT_GMEM_ALIGN_H;
    uint32_t padded_w = rounded(p->pass.width,A840_LIMIT_GMEM_ALIGN_W);
    uint32_t padded_h = rounded(p->pass.height,height_align);
    if (s->pitch % pitch_align || s->pitch < padded_w * cpp ||
        (s->pitch >> A840_MRT_PITCH_SHR) > A840_MRT_PITCH_MAX)
        return A840_EMIT_RANGE_ERROR;
    TRY(check_range(s->data,(uint64_t)padded_h*s->pitch,s->tiled ? 4096 : 64));
    uint32_t info = (s->tiled ? A840_TILE6_3 : A840_TILE6_LINEAR) | sample_bits(samples) |
        (b->format << GM_FORMAT_SHIFT) | (s->swap << GM_SWAP_SHIFT);
    uint32_t meta = 0;
    if (a->ubwc) {
        if (cpp > 32) return A840_EMIT_UNSUPPORTED;
        uint32_t bw = cpp <= 4 ? 16 : (cpp == 8 ? 8 : 4);
        if (a->cpp <= 2 && samples > 1) bw = 16 / samples;
        uint32_t bh = rg8 ? 8 : (cpp == 32 ? 2 : 4);
        if (rg8) bw = 16;
        uint32_t rows = rounded((p->pass.height+bh-1)/bh,16);
        if (s->flag_pitch % 64 || s->flag_pitch < (p->pass.width+bw-1)/bw ||
            (s->flag_pitch >> A840_2D_FLAG_PITCH_SHR) > A840_2D_FLAG_PITCH_MAX ||
            s->flag_array_pitch % 4096 || (uint64_t)s->flag_pitch*rows > s->flag_array_pitch ||
            (s->flag_array_pitch >> A840_2D_FLAG_ARRAY_SHR) > A840_2D_FLAG_ARRAY_MAX)
            return A840_EMIT_RANGE_ERROR;
        TRY(check_range(s->flags,s->flag_array_pitch,4096));
        uint64_t data_end = s->data.va + (uint64_t)padded_h*s->pitch;
        if (s->data.va < s->flags.va+s->flag_array_pitch && s->flags.va < data_end)
            return A840_EMIT_ALIAS;
        meta = (s->flag_pitch >> A840_2D_FLAG_PITCH_SHR) |
            ((s->flag_array_pitch >> A840_2D_FLAG_ARRAY_SHR) << A840_2D_FLAG_ARRAY_SHIFT);
        info |= GM_FLAGS;
    }
    *sys_info = info;
    *flag_pitch = meta;
    return A840_EMIT_OK;
}

static A840EmitStatus transfer(A840CmdStream *cs, const A840GmemPlan *p, unsigned slot,
                               const A840GmemBinding *b, const A840FormatCaps *format, uint32_t type)
{
    const A840GmemAttachment *a = &p->pass.attachments[slot];
    const A840GmemSurface *s = type == GM_STORE && a->store == A840_STORE_RESOLVE ? &b->resolve : &b->image;
    uint32_t info, meta, flags = type == GM_LOAD ? A840_RELOC_READ : A840_RELOC_WRITE;
    TRY(surface_state(p,slot,b,format,type,&info,&meta));
    TRY(reg(cs,A840_REG_RB_RESOLVE_SYSTEM_BUFFER_INFO,info));
    TRY(a840_cs_emit_reg64_va(cs,A840_REG_RB_RESOLVE_SYSTEM_BUFFER_BASE,s->data.bo,s->data.va,flags));
    TRY(reg(cs,A840_REG_RB_RESOLVE_SYSTEM_BUFFER_PITCH,s->pitch >> A840_MRT_PITCH_SHR));
    if (a->ubwc) {
        TRY(a840_cs_emit_reg64_va(cs,A840_REG_RB_RESOLVE_SYSTEM_FLAG_BUFFER_BASE,
                                  s->flags.bo,s->flags.va,flags));
        TRY(reg(cs,A840_REG_RB_RESOLVE_SYSTEM_FLAG_BUFFER_PITCH,meta));
    }
    return A840_EMIT_OK;
}

/* tu_clear_blit.cc:5526-5558. CCU_RESOLVE cannot average signed-normalized,
 * wide-channel or RG8 color samples. The native A2D engine can. */
static bool event_resolvable(uint32_t format)
{
    switch (format) {
    case A840_FMT6_A8_UNORM:
    case A840_FMT6_8_UNORM: case A840_FMT6_8_UINT: case A840_FMT6_8_SINT:
    case A840_FMT6_8_8_8_8_UNORM: case A840_FMT6_8_8_8_8_UINT: case A840_FMT6_8_8_8_8_SINT:
    case A840_FMT6_8_8_8_X8_UNORM:
    case A840_FMT6_10_10_10_2_UNORM: case A840_FMT6_10_10_10_2_UINT:
        return true;
    default:
        return false;
    }
}

/* store_cp_blit<A8XX>, tu_clear_blit.cc:5791-5887. TILE6_2 addresses
 * on Gen8 are GMEM-relative. Only the real sysmem destination is relocated. */
static A840EmitStatus resolve_2d(A840CmdStream *cs, const A840GmemPlan *p, unsigned i,
                                 const A840GmemBinding *b, const A840FormatCaps *format)
{
    const A840OpsFormat *f = a840_ops_format_info(b->format);
    if (!f || p->planes[i].pitch > A840_2D_SRC_PITCH_MAX) return A840_EMIT_UNSUPPORTED;
    uint32_t info, meta;
    TRY(surface_state(p,i,b,format,GM_STORE,&info,&meta));
    const A840GmemSurface *dst = &b->resolve;
    uint32_t ifmt = b->srgb ? A840_R2D_UNORM8_SRGB : f->ifmt;
    uint32_t ctl = (b->format << A840_2D_FORMAT_SHIFT) | GM_A2D_SCISSOR |
        (GM_RGBA_MASK << A840_2D_MASK_SHIFT) | (ifmt << A840_2D_IFMT_SHIFT);
    uint32_t output = (b->format << A840_2D_SP_FORMAT_SHIFT) |
        (f->output_type << A840_2D_SP_TYPE_SHIFT) | (GM_RGBA_MASK << A840_2D_SP_MASK_SHIFT) |
        (b->srgb ? A840_2D_SP_SRGB : 0) | (f->half ? A840_2D_SP_HALF_PRECISION : 0);
    TRY(reg(cs,A840_REG_RB_A2D_PIXEL_CNTL,0));
    TRY(reg(cs,A840_REG_RB_A2D_BLT_CNTL,ctl));
    TRY(reg(cs,A840_REG_GRAS_A2D_BLT_CNTL,ctl));
    TRY(reg(cs,A840_REG_TPL1_A2D_BLT_CNTL,A840_A6XX_TEX_2D << A840_2D_TPL1_TYPE_SHIFT));
    TRY(reg(cs,A840_REG_SP_A2D_OUTPUT_INFO,output));
    uint32_t dst_info = b->format | (dst->swap << A840_2D_DST_SWAP_SHIFT) |
        ((dst->tiled ? A840_TILE6_3 : A840_TILE6_LINEAR) << A840_2D_DST_TILE_SHIFT) |
        ((info & GM_FLAGS) ? A840_2D_DST_FLAGS : 0) | (b->srgb ? A840_2D_DST_SRGB : 0);
    TRY(reg(cs,A840_REG_RB_A2D_DEST_BUFFER_INFO,dst_info));
    TRY(a840_cs_emit_reg64_va(cs,A840_REG_RB_A2D_DEST_BUFFER_BASE,dst->data.bo,dst->data.va,A840_RELOC_WRITE));
    TRY(reg(cs,A840_REG_RB_A2D_DEST_BUFFER_PITCH,dst->pitch >> A840_2D_DST_PITCH_SHR));
    if (info & GM_FLAGS) {
        TRY(a840_cs_emit_reg64_va(cs,A840_REG_RB_A2D_DEST_FLAG_BUFFER_BASE,
                                  dst->flags.bo,dst->flags.va,A840_RELOC_WRITE));
        TRY(reg(cs,A840_REG_RB_A2D_DEST_FLAG_BUFFER_PITCH,meta));
    }
    uint32_t source[] = {format->tiled_fmt_id | (GM_TILE6_2 << A840_2D_SRC_TILE_SHIFT) |
        (format->tiled_swap << A840_2D_SRC_SWAP_SHIFT) | (b->srgb ? A840_2D_SRC_SRGB : 0) |
        ((p->pass.attachments[i].samples >> 1) << GM_A2D_SAMPLES_SHIFT) |
        (f->output_type == A840_OUTPUT_IFMT_2D_FLOAT ? GM_A2D_AVERAGE : 0) |
        A840_2D_SRC_UNK20 | A840_2D_SRC_UNK22,
        p->pass.width | (p->pass.height << A840_2D_SRC_HEIGHT_SHIFT), p->planes[i].offset,0,
        p->planes[i].pitch << A840_2D_SRC_PITCH_SHIFT};
    TRY(a840_cs_emit_pkt4(cs,A840_REG_TPL1_A2D_SRC_TEXTURE_INFO,source,5));
    const uint32_t right = p->pass.width-1, bottom = p->pass.height-1;
    uint32_t rect[] = {0,xy(right,bottom)};
    uint32_t coords[] = {0,right << A840_2D_SRC_COORD_SHIFT,0,bottom << A840_2D_SRC_COORD_SHIFT};
    TRY(a840_cs_emit_pkt4(cs,A840_REG_GRAS_A2D_DEST_TL,rect,2));
    TRY(a840_cs_emit_pkt4(cs,A840_REG_GRAS_A2D_SRC_XMIN,coords,4));
    TRY(event(cs,A840_EVENT_CACHE_INVALIDATE7));
    TRY(event(cs,GM_EV_RESOLVE_CLEAN));
    TRY(a840_emit_wfi(cs)); /* Required between the cache events and A2D reads. */
    TRY(op(cs,A840_CP_BLIT,A840_BLIT_OP_SCALE));
    return event(cs,A840_EVENT_CCU_FLUSH_COLOR);
}

static A840EmitStatus attachments(A840CmdStream *dst, const A840GmemPlan *p,
                                   const A840GmemBinding *bindings, uint32_t type)
{
    GmemBatch b;
    bool emitted = false;
    TRY(a840_gmem_validate(p));
    TRY(begin(&b));
    if (type == GM_STORE) {
        TRY(op(&b.cs,A840_CP_SET_MARKER,GM_END_DRAWS | GM_USES_GMEM));
        TRY(op(&b.cs,A840_CP_SET_MARKER,GM_RESOLVE | GM_USES_GMEM));
    }
    for (unsigned i = 0; i < A840_GMEM_PLANES; ++i) {
        const A840GmemAttachment *a = &p->pass.attachments[i];
        if (!a->cpp) continue;
        if (type == GM_CLEAR && a->load != A840_LOAD_CLEAR) continue;
        if (type == GM_LOAD && (a->memoryless || a->load != A840_LOAD_LOAD)) continue;
        if (type == GM_STORE && (a->memoryless || a->store == A840_STORE_DONT_CARE)) continue;
        if (!bindings || !bindings[i].format || bindings[i].format > UINT8_MAX)
            return A840_EMIT_INVALID_ARG;
        const A840GmemBinding *surface = &bindings[i];
        const A840FormatCaps *format = plane_format(surface->format,i,a->cpp,surface->srgb);
        if (!format) return A840_EMIT_UNSUPPORTED;
        if (type == GM_STORE && a->store == A840_STORE_RESOLVE &&
            i < A840_GMEM_COLORS && !event_resolvable(surface->format)) {
            if (emitted) TRY(event(&b.cs,GM_EV_GROUP_END));
            emitted = false;
            TRY(resolve_2d(&b.cs,p,i,surface,format));
            continue;
        }
        uint32_t operation = type | (i << GM_BUFFER_ID_SHIFT);
        TRY(reg(&b.cs,A840_REG_RB_RESOLVE_GMEM_BUFFER_INFO,sample_bits(a->samples)));
        TRY(reg(&b.cs,A840_REG_RB_RESOLVE_CNTL_0,0));
        TRY(reg(&b.cs,A840_REG_RB_RESOLVE_GMEM_BUFFER_BASE,p->planes[i].offset));
        if (type == GM_CLEAR) {
            operation |= GM_RGBA_MASK << GM_MASK_SHIFT;
            TRY(reg(&b.cs,A840_REG_RB_RESOLVE_SYSTEM_BUFFER_INFO,surface->format << GM_FORMAT_SHIFT));
            TRY(a840_cs_emit_pkt4(&b.cs,A840_REG_RB_RESOLVE_CLEAR_COLOR_DW0,surface->clear,4));
        } else {
            if (i >= A840_GMEM_DEPTH) operation |= GM_DEPTH | GM_SAMPLE_ZERO;
            const A840OpsFormat *f = a840_ops_format_info(surface->format);
            if (surface->sample_zero || (f && f->output_type != A840_OUTPUT_IFMT_2D_FLOAT))
                operation |= GM_SAMPLE_ZERO;
            TRY(transfer(&b.cs,p,i,surface,format,type));
        }
        TRY(reg(&b.cs,A840_REG_RB_RESOLVE_OPERATION,operation));
        TRY(event(&b.cs,GM_EV_RESOLVE));
        emitted = true;
    }
    if (emitted) TRY(event(&b.cs,GM_EV_GROUP_END));
    return finish(dst,&b);
}

A840EmitStatus a840_emit_gmem_tile_clear(A840CmdStream *cs,const A840GmemPlan *p,const A840GmemBinding b[])
{ return attachments(cs,p,b,GM_CLEAR); }
A840EmitStatus a840_emit_gmem_tile_load(A840CmdStream *cs,const A840GmemPlan *p,const A840GmemBinding b[])
{ return attachments(cs,p,b,GM_LOAD); }
A840EmitStatus a840_emit_gmem_tile_store(A840CmdStream *cs,const A840GmemPlan *p,const A840GmemBinding b[])
{ return attachments(cs,p,b,GM_STORE); }

A840EmitStatus a840_emit_gmem_tile_end(A840CmdStream *dst)
{
    GmemBatch b;
    TRY(begin(&b));
    /* tu_cmd_buffer.cc: tile_store_cs -> WFI -> BIN_RENDER_END. Do not
     * reuse the identical SRAM offsets until this tile's stores finish. */
    TRY(a840_emit_wfi(&b.cs));
    TRY(op(&b.cs,A840_CP_SET_MARKER,GM_RENDER_END));
    return finish(dst,&b);
}

A840EmitStatus a840_emit_gmem_pass_end(A840CmdStream *dst)
{
    GmemBatch b;
    TRY(begin(&b));
    TRY(a840_emit_gmem_tile_end(&b.cs));
    TRY(event(&b.cs,GM_EV_RESOLVE_CLEAN));
    TRY(cache_mode(&b.cs,false));
    TRY(reg(&b.cs,A840_REG_RB_CLEAR_TARGET,0));
    TRY(reg(&b.cs,A840_REG_RB_BUFFER_CNTL,GM_SYSMEM_BUFFERS));
    return finish(dst,&b);
}
