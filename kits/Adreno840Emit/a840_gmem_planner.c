/* SPDX-License-Identifier: MIT */
#include "a840_gmem_planner.h"
#include "Adreno840Caps.h"
#include <string.h>

enum {
    ccu_budget = A840_KAANAPALI_GMEM_BYTES - A840_LIMIT_NUM_CCU *
        (A840_LIMIT_GMEM_PER_CCU_COLOR_CACHE_SIZE + A840_LIMIT_GMEM_PER_CCU_DEPTH_CACHE_SIZE),
    /* fd6_gmem_cache.h:39-98, freedreno_devices.py:1295-1301.
     * RB_CCU_CACHE_CNTL.depth_offset is the first reserved byte. */
    attachment_budget = ccu_budget - A840_LIMIT_NUM_CCU *
        (A840_LIMIT_GMEM_VPC_ATTR_BUF_SIZE + 24576u + 32768u),
    max_fb = 16384
};
_Static_assert(ccu_budget == 17203200 && attachment_budget == 16564224, "Kaanapali carveouts");

static uint32_t div_up(uint32_t n, uint32_t d) { return n / d + (n % d != 0); }
static uint32_t min_u(uint32_t a, uint32_t b) { return a < b ? a : b; }

static A840EmitStatus inspect(const A840GmemPass *pass, uint32_t *align_w,
                              bool *required, bool *prefer)
{
    bool active = false;
    if (!pass || !pass->width || !pass->height || pass->width > max_fb ||
        pass->height > max_fb || pass->covered_pixels > (uint64_t)pass->width * pass->height)
        return A840_EMIT_INVALID_ARG;
    *align_w = A840_LIMIT_TILE_ALIGN_W;
    *required = pass->uses_fbfetch || pass->has_msaa_resolve;
    *prefer = pass->draw_count >= 2;
    for (unsigned i = 0; i < A840_GMEM_PLANES; ++i) {
        const A840GmemAttachment *a = &pass->attachments[i];
        if (!a->cpp) continue;
        active = true;
        if ((a->samples != 1 && a->samples != 2 && a->samples != 4) ||
            (unsigned)a->load > A840_LOAD_LOAD || (unsigned)a->store > A840_STORE_RESOLVE ||
            (a->store == A840_STORE_RESOLVE && a->samples == 1))
            return A840_EMIT_INVALID_ARG;
        /* tu_pass.cc:780-790: 1-byte input attachment rows must align to 64. */
        if (a->cpp == 1 && a->samples == 1) *align_w = 2 * A840_LIMIT_TILE_ALIGN_W;
        *required |= a->memoryless || a->store == A840_STORE_RESOLVE;
        *prefer |= a->load != A840_LOAD_LOAD || a->store == A840_STORE_DONT_CARE;
    }
    return active ? A840_EMIT_OK : A840_EMIT_INVALID_ARG;
}

static uint32_t layout(const A840GmemPass *pass, uint32_t w, uint32_t h,
                       A840GmemSlice *planes)
{
    uint32_t end = 0;
    for (unsigned i = 0; i < A840_GMEM_PLANES; ++i) {
        const A840GmemAttachment *a = &pass->attachments[i];
        if (!a->cpp) continue;
        uint64_t pitch = (uint64_t)w * a->cpp * a->samples;
        if (pitch > attachment_budget / h) return 0;
        uint32_t alignment = (a->samples > 1 || i >= A840_GMEM_DEPTH)
                           ? 4 * A840_GMEM_BYTES_ALIGNMENT : A840_GMEM_BYTES_ALIGNMENT;
        uint32_t offset = div_up(end, alignment) * alignment;
        uint32_t bytes = (uint32_t)pitch * h;
        if (offset + bytes > attachment_budget) return 0;
        if (planes) planes[i] = (A840GmemSlice){offset, (uint32_t)pitch, bytes};
        end = offset + bytes;
    }
    return end;
}

/* Search widths exhaustively. For a given width, capacity is monotone in
 * height including per-plane padding, so binary search finds its tallest fit. */
A840EmitStatus a840_gmem_plan(const A840GmemPass *pass, A840GmemPlan *out)
{
    A840GmemPlan p = {0};
    uint32_t step, best_area = 0, best_count = UINT32_MAX;
    bool required, prefer;
    A840EmitStatus st = inspect(pass, &step, &required, &prefer);
    if (st != A840_EMIT_OK || !out) return A840_EMIT_INVALID_ARG;
    p.pass = *pass;
    p.ccu_budget = ccu_budget;
    p.attachment_budget = attachment_budget;
    uint32_t max_w = min_u(div_up(pass->width, step) * step, A840_LIMIT_TILE_MAX_W);
    uint32_t rows = div_up(pass->height, A840_LIMIT_TILE_ALIGN_H);
    uint64_t coverage = pass->covered_pixels ? pass->covered_pixels : (uint64_t)pass->width * pass->height;
    if (required || prefer || coverage > A840_LIMIT_TILE_ALIGN_W * A840_LIMIT_TILE_ALIGN_H) {
        for (uint32_t w = step; w <= max_w; w += step) {
            uint32_t low = 0, high = rows;
            while (low < high) {
                uint32_t mid = (low + high + 1) / 2;
                if (layout(pass, w, mid * A840_LIMIT_TILE_ALIGN_H, NULL)) low = mid;
                else high = mid - 1;
            }
            uint32_t h = low * A840_LIMIT_TILE_ALIGN_H;
            if (!h) continue;
            uint32_t count = div_up(pass->width, w) * div_up(pass->height, h);
            if (w * h > best_area || (w * h == best_area && count < best_count)) {
                best_area = w * h;
                best_count = count;
                p.tile_w = w; p.tile_h = h;
            }
        }
    }
    if (!best_area && required) return A840_EMIT_UNSUPPORTED;
    if (best_area) {
        p.path = A840_PATH_GMEM;
        p.used_bytes = layout(pass, p.tile_w, p.tile_h, p.planes);
        p.tiles_x = div_up(pass->width, p.tile_w);
        p.tiles_y = div_up(pass->height, p.tile_h);
        /* Mesa tu_util.cc:443-462. Grow the rectangular VSC pipe footprint. */
        p.pipe_w = p.pipe_h = 1;
        while (div_up(p.tiles_x,p.pipe_w) * div_up(p.tiles_y,p.pipe_h) > A840_GMEM_PIPES) {
            if (p.pipe_w < p.pipe_h && p.pipe_w < p.tiles_x) ++p.pipe_w;
            else if (p.pipe_h < p.tiles_y) ++p.pipe_h;
            else ++p.pipe_w;
        }
        p.pipes_x = div_up(p.tiles_x, p.pipe_w);
        p.pipes_y = div_up(p.tiles_y, p.pipe_h);
        if (p.pipe_w > 63 || p.pipe_h > 63) return A840_EMIT_UNSUPPORTED;
    }
    *out = p;
    return A840_EMIT_OK;
}

A840EmitStatus a840_gmem_validate(const A840GmemPlan *p)
{
    A840GmemSlice planes[A840_GMEM_PLANES] = {{0}};
    uint32_t step;
    bool required, prefer;
    if (!p || inspect(&p->pass, &step, &required, &prefer) != A840_EMIT_OK ||
        p->path != A840_PATH_GMEM || p->ccu_budget != ccu_budget ||
        p->attachment_budget != attachment_budget || !p->tile_w || p->tile_w % step ||
        p->tile_w > A840_LIMIT_TILE_MAX_W || !p->tile_h || p->tile_h % A840_LIMIT_TILE_ALIGN_H ||
        p->tile_h > A840_LIMIT_TILE_MAX_H || p->tiles_x != div_up(p->pass.width, p->tile_w) ||
        p->tiles_y != div_up(p->pass.height, p->tile_h) ||
        !p->pipe_w || p->pipe_w > 63 || !p->pipe_h || p->pipe_h > 63 ||
        p->pipes_x != div_up(p->tiles_x, p->pipe_w) || p->pipes_y != div_up(p->tiles_y, p->pipe_h) ||
        p->pipes_x * p->pipes_y > A840_GMEM_PIPES || !p->used_bytes ||
        layout(&p->pass, p->tile_w, p->tile_h, planes) != p->used_bytes ||
        memcmp(planes, p->planes, sizeof(planes))) return A840_EMIT_INVALID_ARG;
    return A840_EMIT_OK;
}

A840EmitStatus a840_gmem_tile(const A840GmemPlan *p, uint32_t tx, uint32_t ty, A840GmemTile *out)
{
    if (!out || a840_gmem_validate(p) != A840_EMIT_OK || tx >= p->tiles_x || ty >= p->tiles_y)
        return A840_EMIT_INVALID_ARG;
    uint32_t x = tx * p->tile_w, y = ty * p->tile_h;
    uint32_t pipe_x = tx / p->pipe_w, pipe_y = ty / p->pipe_h;
    uint32_t pipe_width = min_u(p->pipe_w, p->tiles_x - pipe_x * p->pipe_w);
    *out = (A840GmemTile){x, y, min_u(p->tile_w, p->pass.width - x),
        min_u(p->tile_h, p->pass.height - y), pipe_y * p->pipes_x + pipe_x,
        (ty % p->pipe_h) * pipe_width + tx % p->pipe_w};
    return A840_EMIT_OK;
}
