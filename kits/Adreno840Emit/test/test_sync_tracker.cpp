/* SPDX-License-Identifier: MIT */
/* Wave 9 Lane 3 Host Test Suite: A8XX Sync Tracker & Redundant Barrier Eliminator
 * (>= 35 independent test cases + 4 realistic 120Hz composite scenarios comparing
 *  naive vs tracked PM4 emission with --dump-pm4 <dir> export).
 *
 * Strictly zero Type-4 writes to CP_PROTECT protected registers (e.g. 0x9e00).
 */
#include "a840_sync_tracker.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

unsigned g_cases = 0;
unsigned g_checks = 0;
const char *g_active_case = "init";

void begin_case(const char *name)
{
    g_active_case = name;
    ++g_cases;
}

void check_impl(bool ok, const char *expr, int line)
{
    ++g_checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL [%s] line %d: %s\n", g_active_case, line, expr);
        std::abort();
    }
}

#define CASE(name) begin_case(name)
#define CHECK(expr) check_impl((expr), #expr, __LINE__)

struct StreamMetrics {
    size_t dwords = 0u;
    size_t packets = 0u;
    size_t wfi_count = 0u;
    size_t wait_mem_count = 0u;
    size_t ccu_flush_color = 0u;
    size_t ccu_flush_depth = 0u;
    size_t ccu_inv_color = 0u;
    size_t uche_inv = 0u;
    size_t draw_count = 0u;
    size_t cs_count = 0u;
    size_t blit_count = 0u;
    bool touched_cp_protect_9e00 = false;
};

StreamMetrics inspect_pm4(const A840CmdStream &cs)
{
    StreamMetrics m{};
    m.dwords = cs.used_dwords;
    if (cs.used_dwords == 0u) {
        return m;
    }
    std::vector<A840PM4Packet> pkts(cs.used_dwords);
    size_t count = 0u;
    CHECK(a840_pm4_decode(cs.words, cs.used_dwords, pkts.data(), pkts.size(), &count) == A840_EMIT_OK);
    m.packets = count;
    for (size_t i = 0u; i < count; ++i) {
        const A840PM4Packet &p = pkts[i];
        if (p.type == 4u) {
            if (A840_REG_PC_DRAWCALL_CNTL >= p.identifier &&
                A840_REG_PC_DRAWCALL_CNTL < p.identifier + p.payload_words) {
                m.touched_cp_protect_9e00 = true;
            }
        } else if (p.type == 7u) {
            if (p.identifier == A840_CP_WAIT_FOR_IDLE) {
                m.wfi_count++;
            } else if (p.identifier == A840_CP_WAIT_MEM_WRITES) {
                m.wait_mem_count++;
            } else if (p.identifier == A840_CP_DRAW_INDX_OFFSET) {
                m.draw_count++;
            } else if (p.identifier == A840_CP_EXEC_CS) {
                m.cs_count++;
            } else if (p.identifier == A840_CP_BLIT) {
                m.blit_count++;
            } else if (p.identifier == A840_CP_EVENT_WRITE7 && p.payload_words >= 1u) {
                uint32_t ev = cs.words[p.header_word + 1u] & 0xffu;
                if (ev == A840_EVENT_CCU_FLUSH_COLOR) {
                    m.ccu_flush_color++;
                } else if (ev == A840_EVENT_CCU_FLUSH_DEPTH) {
                    m.ccu_flush_depth++;
                } else if (ev == A840_EVENT_CCU_INVALIDATE_COLOR) {
                    m.ccu_inv_color++;
                } else if (ev == A840_EVENT_CACHE_INVALIDATE7) {
                    m.uche_inv++;
                }
            }
        }
    }
    CHECK(!m.touched_cp_protect_9e00);
    return m;
}

void write_pm4_bin(const char *dir, const char *filename, const A840CmdStream &cs)
{
    if (!dir || !dir[0]) {
        return;
    }
    std::string path = std::string(dir) + "/" + filename;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    CHECK(out.is_open());
    out.write(reinterpret_cast<const char *>(cs.words),
              static_cast<std::streamsize>(cs.used_dwords * sizeof(uint32_t)));
    CHECK(out.good());
}

/* Safe A8XX GPU work helpers (zero CP_PROTECT Type-4 writes; genuine A8XX packets). */
void emit_safe_draw(A840CmdStream *cs, uint32_t vertex_count, uint32_t instance_count)
{
    uint32_t offsets[2] = {0u, 0u};
    CHECK(a840_cs_emit_pkt4(cs, A840_REG_VFD_INDEX_OFFSET, offsets, 2u) == A840_EMIT_OK);
    uint32_t initiator = (A840_PRIM_TRIANGLES << A840_DRAW_PRIMITIVE_SHIFT) |
                         (A840_DRAW_AUTO << A840_DRAW_SOURCE_SHIFT) |
                         (A840_DRAW_IGNORE_VIS << A840_DRAW_VISIBILITY_SHIFT);
    uint32_t draw_payload[3] = {initiator, instance_count, vertex_count};
    CHECK(a840_cs_emit_pkt7(cs, A840_CP_DRAW_INDX_OFFSET, draw_payload, 3u) == A840_EMIT_OK);
}

void emit_safe_compute(A840CmdStream *cs, uint32_t gx, uint32_t gy, uint32_t gz)
{
    uint32_t marker = A840_MODE_COMPUTE;
    CHECK(a840_cs_emit_pkt7(cs, A840_CP_SET_MARKER, &marker, 1u) == A840_EMIT_OK);
    uint32_t payload[4] = {0u, gx, gy, gz};
    CHECK(a840_cs_emit_pkt7(cs, A840_CP_EXEC_CS, payload, 4u) == A840_EMIT_OK);
}

void emit_safe_2d_blit(A840CmdStream *cs, uint32_t w, uint32_t h)
{
    uint32_t rect[2] = {0u, (w - 1u) | ((h - 1u) << A840_2D_COORD_Y_SHIFT)};
    CHECK(a840_cs_emit_pkt4(cs, A840_REG_GRAS_A2D_DEST_TL, rect, 2u) == A840_EMIT_OK);
    uint32_t op = A840_BLIT_OP_SCALE;
    CHECK(a840_cs_emit_pkt7(cs, A840_CP_BLIT, &op, 1u) == A840_EMIT_OK);
}

void emit_safe_cp_mem_write(A840CmdStream *cs, uint64_t bo, uint64_t va, uint64_t val)
{
    CHECK(a840_emit_mem_write64(cs, bo, va, val) == A840_EMIT_OK);
}

/* ============================================================================
 * Group 1: Core Dirty-Bit & Clean-State Unit Cases (Cases 1..22)
 * ============================================================================ */
void test_group1_core_rules()
{
    std::vector<uint32_t> words(512, 0u);
    std::vector<A840Reloc> relocs(32);

    CASE("init_and_invalid_args");
    {
        A840SyncTracker t{};
        CHECK(a840_sync_tracker_init(nullptr, A840_SYNC_MODE_TRACKED) == A840_EMIT_INVALID_ARG);
        CHECK(a840_sync_tracker_init(&t, static_cast<A840SyncMode>(99)) == A840_EMIT_INVALID_ARG);
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(t.dirty_domains == A840_SYNC_DIRTY_NONE);
        CHECK(t.clean_state == A840_SYNC_CLEAN_ALL);
        CHECK(!t.overflow_conservative);
    }

    CASE("redundant_wfi_elision_initial_clean");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        /* Stream starts clean -> WFI is elided. */
        CHECK(a840_sync_emit_wfi(&t, &cs, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(cs.used_dwords == 0u);
        CHECK(a840_sync_tracker_stats(&t).wfi_elided == 1u);
    }

    CASE("consecutive_wfi_deduplication_after_draw");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t rt = 10u;
        emit_safe_draw(&cs, 6u, 1u);
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_COLOR_CCU, &rt, 1u) == A840_EMIT_OK);
        CHECK(a840_sync_emit_wfi(&t, &cs, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(a840_sync_emit_wfi(&t, &cs, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(a840_sync_emit_wfi(&t, &cs, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        StreamMetrics m = inspect_pm4(cs);
        CHECK(m.wfi_count == 1u);
        CHECK(t.stats.wfi_emitted == 1u);
        CHECK(t.stats.wfi_elided == 2u);
        CHECK(t.stats.dwords_saved == 2u);
    }

    CASE("force_full_barrier_overrides_wfi_elision");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        CHECK(a840_sync_emit_wfi(&t, &cs, A840_SYNC_FORCE_FULL_BARRIER) == A840_EMIT_OK);
        CHECK(inspect_pm4(cs).wfi_count == 1u);
    }

    CASE("wait_mem_writes_only_emitted_when_cp_mem_write_dirty");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        /* Clean -> elided */
        CHECK(a840_sync_emit_wait_mem_writes(&t, &cs, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(cs.used_dwords == 0u);
        CHECK(t.stats.wait_mem_elided == 1u);

        /* After CP_MEM_WRITE -> emitted once, second consecutive elided */
        const uint64_t bo = 20u;
        emit_safe_cp_mem_write(&cs, bo, 0x10000000ULL, 0x1234ULL);
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_MEM_WRITE_CP, &bo, 1u) == A840_EMIT_OK);
        CHECK(a840_sync_emit_wait_mem_writes(&t, &cs, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(a840_sync_emit_wait_mem_writes(&t, &cs, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        StreamMetrics m = inspect_pm4(cs);
        CHECK(m.wait_mem_count == 1u);
        CHECK(t.stats.wait_mem_emitted == 1u);
        CHECK(t.stats.wait_mem_elided == 2u);
    }

    CASE("ccu_color_flush_elided_when_not_dirty");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        CHECK(a840_sync_emit_cache_barrier(&t, &cs, A840_CACHE_CCU_FLUSH_COLOR,
                                           A840_SYNC_CONSUME_UCHE_TEX, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(cs.used_dwords == 0u);
        CHECK(t.stats.ccu_flush_elided == 1u);
    }

    CASE("ccu_color_flush_emitted_when_dirty_then_deduped");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t rt = 30u;
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_COLOR_CCU, &rt, 1u) == A840_EMIT_OK);
        CHECK(a840_sync_emit_cache_barrier(&t, &cs, A840_CACHE_CCU_FLUSH_COLOR,
                                           A840_SYNC_CONSUME_UCHE_TEX, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(a840_sync_emit_cache_barrier(&t, &cs, A840_CACHE_CCU_FLUSH_COLOR,
                                           A840_SYNC_CONSUME_UCHE_TEX, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        StreamMetrics m = inspect_pm4(cs);
        CHECK(m.ccu_flush_color == 1u);
        CHECK(t.stats.ccu_flush_emitted == 1u);
        CHECK(t.stats.ccu_flush_elided == 1u);
    }

    CASE("ccu_depth_flush_only_when_depth_dirty");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t col = 31u;
        const uint64_t dep = 32u;
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_COLOR_CCU, &col, 1u) == A840_EMIT_OK);
        CHECK(a840_sync_emit_cache_barrier(&t, &cs, A840_CACHE_CCU_FLUSH_DEPTH,
                                           A840_SYNC_CONSUME_UCHE_TEX, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(cs.used_dwords == 0u);

        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_DEPTH_CCU, &dep, 1u) == A840_EMIT_OK);
        CHECK(a840_sync_emit_cache_barrier(&t, &cs, A840_CACHE_CCU_FLUSH_DEPTH,
                                           A840_SYNC_CONSUME_UCHE_TEX, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(inspect_pm4(cs).ccu_flush_depth == 1u);
    }

    CASE("uche_invalidate_dedup_without_intervening_writes");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t buf = 40u;
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_UCHE_SHADER_WR, &buf, 1u) == A840_EMIT_OK);
        CHECK(a840_sync_emit_cache_barrier(&t, &cs, A840_CACHE_UCHE_INV,
                                           A840_SYNC_CONSUME_UCHE_TEX, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(a840_sync_emit_cache_barrier(&t, &cs, A840_CACHE_UCHE_INV,
                                           A840_SYNC_CONSUME_UCHE_TEX, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(a840_sync_emit_cache_barrier(&t, &cs, A840_CACHE_UCHE_INV,
                                           A840_SYNC_CONSUME_UCHE_CONST, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(inspect_pm4(cs).uche_inv == 1u);
        CHECK(t.stats.uche_inv_emitted == 1u);
        CHECK(t.stats.uche_inv_elided == 2u);
    }

    CASE("uche_invalidate_rearms_after_new_write");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t buf = 41u;
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_UCHE_SHADER_WR, &buf, 1u) == A840_EMIT_OK);
        CHECK(a840_sync_emit_cache_barrier(&t, &cs, A840_CACHE_UCHE_INV,
                                           A840_SYNC_CONSUME_UCHE_TEX, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_UCHE_SHADER_WR, &buf, 1u) == A840_EMIT_OK);
        CHECK(a840_sync_emit_cache_barrier(&t, &cs, A840_CACHE_UCHE_INV,
                                           A840_SYNC_CONSUME_UCHE_TEX, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(inspect_pm4(cs).uche_inv == 2u);
    }

    CASE("ccu_flush_color_skipped_for_pure_color_rt_consumer");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t rt = 50u;
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_COLOR_CCU, &rt, 1u) == A840_EMIT_OK);
        /* Subsequent color RT draw stays inside CCU; no sysmem flush needed */
        CHECK(a840_sync_emit_cache_barrier(&t, &cs, A840_CACHE_CCU_FLUSH_COLOR,
                                           A840_SYNC_CONSUME_COLOR_RT, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(cs.used_dwords == 0u);
        /* Once sampled by UCHE_TEX, flush IS emitted */
        CHECK(a840_sync_emit_cache_barrier(&t, &cs, A840_CACHE_CCU_FLUSH_COLOR,
                                           A840_SYNC_CONSUME_UCHE_TEX, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(inspect_pm4(cs).ccu_flush_color == 1u);
    }

    CASE("unrelated_disjoint_pass_barrier_elided");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t a_wr[2] = {100u, 101u};
        const uint64_t b_rd[1] = {200u};
        const uint64_t b_wr[1] = {201u};
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_COLOR_CCU | A840_SYNC_DIRTY_DEPTH_CCU, a_wr, 2u) == A840_EMIT_OK);
        A840SyncPassAccess pass_b{};
        pass_b.read_ids = b_rd;
        pass_b.read_count = 1u;
        pass_b.write_ids = b_wr;
        pass_b.write_count = 1u;
        pass_b.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX | A840_SYNC_CONSUME_COLOR_RT;
        CHECK(a840_sync_emit_pass_barrier(&t, &cs, &pass_b) == A840_EMIT_OK);
        CHECK(cs.used_dwords == 0u);
        CHECK(t.stats.unrelated_pass_barriers_elided == 1u);
    }

    CASE("overlapping_raw_pass_barrier_preserved");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t rt = 300u;
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_COLOR_CCU, &rt, 1u) == A840_EMIT_OK);
        A840SyncPassAccess pass_b{};
        pass_b.read_ids = &rt;
        pass_b.read_count = 1u;
        pass_b.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX;
        CHECK(a840_sync_emit_pass_barrier(&t, &cs, &pass_b) == A840_EMIT_OK);
        StreamMetrics m = inspect_pm4(cs);
        CHECK(m.ccu_flush_color == 1u);
        CHECK(m.uche_inv == 1u);
        CHECK(m.wfi_count == 1u);
        CHECK(m.ccu_flush_depth == 0u);
        CHECK(m.wait_mem_count == 0u);
    }

    CASE("overlapping_war_pass_barrier_preserved");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t tex = 305u;
        CHECK(a840_sync_note_read(&t, A840_SYNC_CONSUME_UCHE_TEX, &tex, 1u) == A840_EMIT_OK);
        A840SyncPassAccess pass_b{};
        pass_b.write_ids = &tex;
        pass_b.write_count = 1u;
        pass_b.consumer_mask = A840_SYNC_CONSUME_COLOR_RT;
        CHECK(a840_sync_emit_pass_barrier(&t, &cs, &pass_b) == A840_EMIT_OK);
        /* WAR requires WFI to finish prior read before overwriting */
        CHECK(inspect_pm4(cs).wfi_count == 1u);
    }

    CASE("global_memory_flag_forces_barrier_even_for_disjoint_ids");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t a = 400u;
        const uint64_t b = 401u;
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_COLOR_CCU, &a, 1u) == A840_EMIT_OK);
        A840SyncPassAccess pass_b{};
        pass_b.read_ids = &b;
        pass_b.read_count = 1u;
        pass_b.consumer_mask = A840_SYNC_CONSUME_COMPUTE_RW;
        pass_b.flags = A840_SYNC_GLOBAL_MEMORY;
        CHECK(a840_sync_emit_pass_barrier(&t, &cs, &pass_b) == A840_EMIT_OK);
        StreamMetrics m = inspect_pm4(cs);
        CHECK(m.ccu_flush_color == 1u);
        CHECK(m.uche_inv == 1u);
        CHECK(m.wfi_count == 1u);
        CHECK(t.stats.conservative_fallbacks >= 1u);
    }

    CASE("overflow_64_resources_triggers_conservative_fallback");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        uint64_t ids[65];
        for (uint64_t i = 0u; i < 65u; ++i) {
            ids[i] = 1000u + i;
        }
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_COLOR_CCU, ids, 65u) == A840_EMIT_OK);
        CHECK(t.overflow_conservative);
        const uint64_t unrelated = 9999u;
        A840SyncPassAccess pass_b{};
        pass_b.read_ids = &unrelated;
        pass_b.read_count = 1u;
        pass_b.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX;
        CHECK(a840_sync_emit_pass_barrier(&t, &cs, &pass_b) == A840_EMIT_OK);
        StreamMetrics m = inspect_pm4(cs);
        CHECK(m.wfi_count == 1u);
        CHECK(m.ccu_flush_color == 1u);
        CHECK(m.uche_inv == 1u);
        CHECK(t.stats.conservative_fallbacks == 1u);
    }

    CASE("external_hazard_triggers_conservative_fallback_and_recovers");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        a840_sync_note_external_hazard(&t);
        CHECK(t.external_hazard_pending);
        const uint64_t id = 555u;
        A840SyncPassAccess p{};
        p.read_ids = &id;
        p.read_count = 1u;
        p.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX;
        CHECK(a840_sync_emit_pass_barrier(&t, &cs, &p) == A840_EMIT_OK);
        CHECK(inspect_pm4(cs).wfi_count == 1u);
        CHECK(!t.external_hazard_pending);
    }

    CASE("anonymous_write_triggers_conservative_mode");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_BLIT_2D_WR, nullptr, 0u) == A840_EMIT_OK);
        CHECK(t.overflow_conservative);
        const uint64_t id = 600u;
        A840SyncPassAccess p{};
        p.read_ids = &id;
        p.read_count = 1u;
        p.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX;
        CHECK(a840_sync_emit_pass_barrier(&t, &cs, &p) == A840_EMIT_OK);
        CHECK(inspect_pm4(cs).ccu_flush_color == 1u);
    }

    CASE("invalid_arguments_and_range_errors_rejected");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t zero_id = 0u;
        CHECK(a840_sync_note_write(nullptr, A840_SYNC_DIRTY_COLOR_CCU, nullptr, 0u) == A840_EMIT_INVALID_ARG);
        CHECK(a840_sync_note_write(&t, 1u << 20, nullptr, 0u) == A840_EMIT_RANGE_ERROR);
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_COLOR_CCU, nullptr, 1u) == A840_EMIT_INVALID_ARG);
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_COLOR_CCU, &zero_id, 1u) == A840_EMIT_INVALID_ARG);
        CHECK(a840_sync_note_read(&t, 1u << 20, nullptr, 0u) == A840_EMIT_RANGE_ERROR);
        CHECK(a840_sync_emit_wfi(&t, &cs, 1u << 20) == A840_EMIT_RANGE_ERROR);
        CHECK(a840_sync_emit_wait_mem_writes(&t, &cs, 1u << 20) == A840_EMIT_RANGE_ERROR);
        CHECK(a840_sync_emit_cache_barrier(&t, &cs, 1u << 20, 0u, 0u) == A840_EMIT_RANGE_ERROR);
        CHECK(a840_sync_emit_pass_barrier(&t, &cs, nullptr) == A840_EMIT_INVALID_ARG);
        CHECK(cs.used_dwords == 0u);
    }

    CASE("transactional_rollback_on_cs_capacity_exhausted");
    {
        std::vector<uint32_t> tiny(2, 0u);
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_NAIVE) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, tiny.data(), tiny.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t rt = 700u;
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_COLOR_CCU, &rt, 1u) == A840_EMIT_OK);
        CHECK(a840_sync_emit_cache_barrier(&t, &cs,
                                           A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_UCHE_INV,
                                           A840_SYNC_CONSUME_UCHE_TEX,
                                           A840_SYNC_REQUIRE_WFI) == A840_EMIT_CAPACITY);
        CHECK(cs.used_dwords == 0u);
        CHECK(t.stats.ccu_flush_emitted == 0u);
        CHECK(t.stats.wfi_emitted == 0u);
    }

    CASE("mode_switch_and_reset_preserves_mode");
    {
        A840SyncTracker t{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        a840_sync_tracker_set_mode(&t, A840_SYNC_MODE_NAIVE);
        CHECK(t.mode == A840_SYNC_MODE_NAIVE);
        const uint64_t id = 800u;
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_COLOR_CCU, &id, 1u) == A840_EMIT_OK);
        a840_sync_tracker_reset(&t);
        CHECK(t.mode == A840_SYNC_MODE_NAIVE);
        CHECK(t.dirty_domains == A840_SYNC_DIRTY_NONE);
        CHECK(t.resource_count == 0u);
    }

    CASE("gmem_resolve_dirty_requires_ccu_flush_before_present");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t surf = 900u;
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_GMEM_RESOLVE, &surf, 1u) == A840_EMIT_OK);
        CHECK(a840_sync_emit_cache_barrier(&t, &cs, A840_CACHE_CCU_FLUSH_COLOR,
                                           A840_SYNC_CONSUME_HOST_PRESENT, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(inspect_pm4(cs).ccu_flush_color == 1u);
    }
}

/* ============================================================================
 * Group 2: 4 Realistic 120Hz Composite Scenarios (Naive vs Tracked A/B + PM4 Dump)
 * ============================================================================ */

/* Scenario 1: 1200x2608 4-Pass Liquid Glass Composite */
void run_scenario_liquid_glass(A840SyncMode mode, A840CmdStream *cs, A840SyncTracker *t)
{
    const uint64_t bg_rt = 1001u;
    const uint64_t depth_rt = 1002u;
    const uint64_t blur_tmp = 1003u;
    const uint64_t final_rt = 1004u;

    CHECK(a840_sync_tracker_init(t, mode) == A840_EMIT_OK);

    /* Pass 1: Background opaque scene -> bg_rt + depth_rt (3 draws inside same RT) */
    emit_safe_draw(cs, 6u, 1u);
    CHECK(a840_sync_note_write(t, A840_SYNC_DIRTY_COLOR_CCU | A840_SYNC_DIRTY_DEPTH_CCU,
                               &bg_rt, 1u) == A840_EMIT_OK);
    CHECK(a840_sync_note_write(t, A840_SYNC_DIRTY_DEPTH_CCU, &depth_rt, 1u) == A840_EMIT_OK);
    /* Redundant intra-pass flush/WFI often emitted by naive encoders */
    CHECK(a840_sync_emit_cache_barrier(t, cs, A840_CACHE_CCU_FLUSH_COLOR,
                                       A840_SYNC_CONSUME_COLOR_RT, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
    emit_safe_draw(cs, 24u, 1u);
    CHECK(a840_sync_note_write(t, A840_SYNC_DIRTY_COLOR_CCU, &bg_rt, 1u) == A840_EMIT_OK);

    /* Pass 2: Dual Kawase / Gaussian compute blur reading bg_rt -> writing blur_tmp */
    A840SyncPassAccess p2{};
    p2.read_ids = &bg_rt;
    p2.read_count = 1u;
    p2.write_ids = &blur_tmp;
    p2.write_count = 1u;
    p2.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX | A840_SYNC_CONSUME_COMPUTE_RW;
    CHECK(a840_sync_emit_pass_barrier(t, cs, &p2) == A840_EMIT_OK);
    /* Extra back-to-back WFI & UCHE_INV requested by sub-encoder */
    CHECK(a840_sync_emit_wfi(t, cs, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
    CHECK(a840_sync_emit_cache_barrier(t, cs, A840_CACHE_UCHE_INV,
                                       A840_SYNC_CONSUME_COMPUTE_RW, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
    emit_safe_compute(cs, 75u, 163u, 1u);
    CHECK(a840_sync_note_write(t, A840_SYNC_DIRTY_UCHE_SHADER_WR, &blur_tmp, 1u) == A840_EMIT_OK);

    /* Pass 3: Liquid Glass refraction + specular tint reading bg_rt + blur_tmp -> final_rt */
    const uint64_t p3_rd[2] = {bg_rt, blur_tmp};
    A840SyncPassAccess p3{};
    p3.read_ids = p3_rd;
    p3.read_count = 2u;
    p3.write_ids = &final_rt;
    p3.write_count = 1u;
    p3.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX | A840_SYNC_CONSUME_COLOR_RT;
    CHECK(a840_sync_emit_pass_barrier(t, cs, &p3) == A840_EMIT_OK);
    emit_safe_draw(cs, 6u, 1u);
    CHECK(a840_sync_note_write(t, A840_SYNC_DIRTY_COLOR_CCU, &final_rt, 1u) == A840_EMIT_OK);

    /* Pass 4: UI HUD overlay onto final_rt (same CCU color target, no texture read) */
    emit_safe_draw(cs, 18u, 1u);
    CHECK(a840_sync_note_write(t, A840_SYNC_DIRTY_COLOR_CCU, &final_rt, 1u) == A840_EMIT_OK);
    /* Present flush */
    CHECK(a840_sync_emit_cache_barrier(t, cs,
                                       A840_CACHE_CCU_FLUSH_COLOR | A840_CACHE_CCU_FLUSH_DEPTH | A840_CACHE_UCHE_INV,
                                       A840_SYNC_CONSUME_HOST_PRESENT,
                                       A840_SYNC_REQUIRE_WAIT_MEM | A840_SYNC_REQUIRE_WFI) == A840_EMIT_OK);
    /* Redundant trailing WFI + wait_mem_writes */
    CHECK(a840_sync_emit_wait_mem_writes(t, cs, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
    CHECK(a840_sync_emit_wfi(t, cs, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
}

/* Scenario 2: Unrelated Dual-Pass Parallel (Completely Disjoint Resources) */
void run_scenario_disjoint_dual_pass(A840SyncMode mode, A840CmdStream *cs, A840SyncTracker *t)
{
    const uint64_t shadow_z = 2001u;
    const uint64_t ui_atlas = 2002u;
    const uint64_t minimap_rt = 2003u;
    const uint64_t particle_buf = 2004u;

    CHECK(a840_sync_tracker_init(t, mode) == A840_EMIT_OK);

    /* Pass A: Shadow map render -> shadow_z */
    emit_safe_draw(cs, 120u, 2u);
    CHECK(a840_sync_note_write(t, A840_SYNC_DIRTY_DEPTH_CCU, &shadow_z, 1u) == A840_EMIT_OK);

    /* Pass B: Disjoint UI minimap -> reads ui_atlas, writes minimap_rt */
    A840SyncPassAccess pb{};
    pb.read_ids = &ui_atlas;
    pb.read_count = 1u;
    pb.write_ids = &minimap_rt;
    pb.write_count = 1u;
    pb.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX | A840_SYNC_CONSUME_COLOR_RT;
    CHECK(a840_sync_emit_pass_barrier(t, cs, &pb) == A840_EMIT_OK);
    emit_safe_draw(cs, 36u, 1u);
    CHECK(a840_sync_note_write(t, A840_SYNC_DIRTY_COLOR_CCU, &minimap_rt, 1u) == A840_EMIT_OK);

    /* Pass C: Disjoint compute particle simulation -> particle_buf */
    A840SyncPassAccess pc{};
    pc.write_ids = &particle_buf;
    pc.write_count = 1u;
    pc.consumer_mask = A840_SYNC_CONSUME_COMPUTE_RW;
    CHECK(a840_sync_emit_pass_barrier(t, cs, &pc) == A840_EMIT_OK);
    emit_safe_compute(cs, 16u, 1u, 1u);
    CHECK(a840_sync_note_write(t, A840_SYNC_DIRTY_UCHE_SHADER_WR, &particle_buf, 1u) == A840_EMIT_OK);

    /* Final join pass reading all three outputs */
    const uint64_t join_rd[3] = {shadow_z, minimap_rt, particle_buf};
    const uint64_t swapchain = 2005u;
    A840SyncPassAccess pd{};
    pd.read_ids = join_rd;
    pd.read_count = 3u;
    pd.write_ids = &swapchain;
    pd.write_count = 1u;
    pd.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX | A840_SYNC_CONSUME_COLOR_RT;
    CHECK(a840_sync_emit_pass_barrier(t, cs, &pd) == A840_EMIT_OK);
    emit_safe_draw(cs, 6u, 1u);
    CHECK(a840_sync_note_write(t, A840_SYNC_DIRTY_COLOR_CCU, &swapchain, 1u) == A840_EMIT_OK);
}

/* Scenario 3: Compute -> Fragment Texture Dependency Chain */
void run_scenario_compute_to_fragment(A840SyncMode mode, A840CmdStream *cs, A840SyncTracker *t)
{
    const uint64_t lut_tex = 3001u;
    const uint64_t hdr_rt = 3002u;
    const uint64_t tone_rt = 3003u;

    CHECK(a840_sync_tracker_init(t, mode) == A840_EMIT_OK);

    /* Compute writes 3D/2D LUT texture via UCHE */
    emit_safe_compute(cs, 8u, 8u, 1u);
    CHECK(a840_sync_note_write(t, A840_SYNC_DIRTY_UCHE_SHADER_WR, &lut_tex, 1u) == A840_EMIT_OK);

    /* Fragment pass samples lut_tex -> writes hdr_rt (no CCU flush needed, only UCHE_INV + WFI) */
    A840SyncPassAccess pf1{};
    pf1.read_ids = &lut_tex;
    pf1.read_count = 1u;
    pf1.write_ids = &hdr_rt;
    pf1.write_count = 1u;
    pf1.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX | A840_SYNC_CONSUME_COLOR_RT;
    CHECK(a840_sync_emit_pass_barrier(t, cs, &pf1) == A840_EMIT_OK);
    /* Redundant WFI & wait_mem_writes */
    CHECK(a840_sync_emit_wfi(t, cs, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
    CHECK(a840_sync_emit_wait_mem_writes(t, cs, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
    emit_safe_draw(cs, 6u, 1u);
    CHECK(a840_sync_note_write(t, A840_SYNC_DIRTY_COLOR_CCU, &hdr_rt, 1u) == A840_EMIT_OK);

    /* Second fragment pass samples hdr_rt + lut_tex -> writes tone_rt */
    const uint64_t rd2[2] = {hdr_rt, lut_tex};
    A840SyncPassAccess pf2{};
    pf2.read_ids = rd2;
    pf2.read_count = 2u;
    pf2.write_ids = &tone_rt;
    pf2.write_count = 1u;
    pf2.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX | A840_SYNC_CONSUME_COLOR_RT;
    CHECK(a840_sync_emit_pass_barrier(t, cs, &pf2) == A840_EMIT_OK);
    emit_safe_draw(cs, 6u, 1u);
    CHECK(a840_sync_note_write(t, A840_SYNC_DIRTY_COLOR_CCU, &tone_rt, 1u) == A840_EMIT_OK);
}

/* Scenario 4: Blit + Camera NV12 Preview & Photo Pipeline */
void run_scenario_blit_camera_nv12(A840SyncMode mode, A840CmdStream *cs, A840SyncTracker *t)
{
    const uint64_t nv12_y = 4001u;
    const uint64_t nv12_uv = 4002u;
    const uint64_t bgra_preview = 4003u;
    const uint64_t scaled_thumb = 4004u;
    const uint64_t meta_bo = 4005u;

    CHECK(a840_sync_tracker_init(t, mode) == A840_EMIT_OK);

    /* Step 1: CP writes frame metadata timestamp */
    emit_safe_cp_mem_write(cs, meta_bo, 0x20000000ULL, 120u);
    CHECK(a840_sync_note_write(t, A840_SYNC_DIRTY_MEM_WRITE_CP, &meta_bo, 1u) == A840_EMIT_OK);

    /* Step 2: Compute YCbCr NV12 -> BGRA8 conversion (disjoint from meta_bo) */
    const uint64_t cam_rd[2] = {nv12_y, nv12_uv};
    A840SyncPassAccess p_conv{};
    p_conv.read_ids = cam_rd;
    p_conv.read_count = 2u;
    p_conv.write_ids = &bgra_preview;
    p_conv.write_count = 1u;
    p_conv.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX | A840_SYNC_CONSUME_COMPUTE_RW;
    CHECK(a840_sync_emit_pass_barrier(t, cs, &p_conv) == A840_EMIT_OK);
    emit_safe_compute(cs, 120u, 135u, 1u);
    CHECK(a840_sync_note_write(t, A840_SYNC_DIRTY_UCHE_SHADER_WR, &bgra_preview, 1u) == A840_EMIT_OK);

    /* Step 3: 2D Blit downscale bgra_preview -> scaled_thumb (needs UCHE_INV + WFI, no CCU depth flush) */
    A840SyncPassAccess p_blit{};
    p_blit.read_ids = &bgra_preview;
    p_blit.read_count = 1u;
    p_blit.write_ids = &scaled_thumb;
    p_blit.write_count = 1u;
    p_blit.consumer_mask = A840_SYNC_CONSUME_BLIT_2D;
    CHECK(a840_sync_emit_pass_barrier(t, cs, &p_blit) == A840_EMIT_OK);
    emit_safe_2d_blit(cs, 320u, 180u);
    CHECK(a840_sync_note_write(t, A840_SYNC_DIRTY_BLIT_2D_WR, &scaled_thumb, 1u) == A840_EMIT_OK);

    /* Step 4: Final host visibility for scaled_thumb + meta_bo */
    const uint64_t end_rd[2] = {scaled_thumb, meta_bo};
    A840SyncPassAccess p_end{};
    p_end.read_ids = end_rd;
    p_end.read_count = 2u;
    p_end.consumer_mask = A840_SYNC_CONSUME_HOST_PRESENT;
    p_end.flags = A840_SYNC_REQUIRE_WAIT_MEM | A840_SYNC_REQUIRE_WFI;
    CHECK(a840_sync_emit_pass_barrier(t, cs, &p_end) == A840_EMIT_OK);
    /* Redundant double WFI */
    CHECK(a840_sync_emit_wfi(t, cs, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
}

void test_group2_composite_scenarios(const char *dump_dir)
{
    struct ScenarioSpec {
        const char *name;
        void (*fn)(A840SyncMode, A840CmdStream *, A840SyncTracker *);
        bool expect_unrelated_elided;
    };
    const ScenarioSpec specs[4] = {
        {"s1_liquid_glass_4pass", &run_scenario_liquid_glass, false},
        {"s2_disjoint_dual_pass", &run_scenario_disjoint_dual_pass, true},
        {"s3_compute_to_fragment", &run_scenario_compute_to_fragment, false},
        {"s4_blit_camera_nv12", &run_scenario_blit_camera_nv12, true},
    };

    for (const auto &spec : specs) {
        CASE(spec.name);
        std::vector<uint32_t> naive_words(1024, 0u);
        std::vector<uint32_t> tracked_words(1024, 0u);
        std::vector<A840Reloc> naive_relocs(32);
        std::vector<A840Reloc> tracked_relocs(32);
        A840CmdStream cs_naive{}, cs_tracked{};
        A840SyncTracker t_naive{}, t_tracked{};

        CHECK(a840_cs_init(&cs_naive, naive_words.data(), naive_words.size(),
                           naive_relocs.data(), naive_relocs.size()) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs_tracked, tracked_words.data(), tracked_words.size(),
                           tracked_relocs.data(), tracked_relocs.size()) == A840_EMIT_OK);

        spec.fn(A840_SYNC_MODE_NAIVE, &cs_naive, &t_naive);
        spec.fn(A840_SYNC_MODE_TRACKED, &cs_tracked, &t_tracked);

        StreamMetrics mn = inspect_pm4(cs_naive);
        StreamMetrics mt = inspect_pm4(cs_tracked);

        /* Work packets (draws / computes / blits) MUST be 100% identical */
        CHECK(mn.draw_count == mt.draw_count);
        CHECK(mn.cs_count == mt.cs_count);
        CHECK(mn.blit_count == mt.blit_count);

        /* Tracked mode MUST reduce total dwords and sync overhead */
        CHECK(mt.dwords < mn.dwords);
        CHECK(mt.wfi_count <= mn.wfi_count);
        CHECK((mt.ccu_flush_color + mt.ccu_flush_depth + mt.ccu_inv_color + mt.uche_inv) <
              (mn.ccu_flush_color + mn.ccu_flush_depth + mn.ccu_inv_color + mn.uche_inv));
        CHECK(t_tracked.stats.dwords_saved > 0u);
        if (spec.expect_unrelated_elided) {
            CHECK(t_tracked.stats.unrelated_pass_barriers_elided >= 1u);
        }

        /* Must still preserve real dependency barriers (never 0 when real RAW exists) */
        CHECK(mt.wfi_count >= 1u);
        CHECK(mt.uche_inv >= 1u);

        std::string fn_naive = std::string(spec.name) + "_naive.pm4";
        std::string fn_tracked = std::string(spec.name) + "_tracked.pm4";
        write_pm4_bin(dump_dir, fn_naive.c_str(), cs_naive);
        write_pm4_bin(dump_dir, fn_tracked.c_str(), cs_tracked);

        double pct = 100.0 * static_cast<double>(mn.dwords - mt.dwords) /
                     static_cast<double>(mn.dwords);
        std::printf("  [A/B %s] naive=%zu dw (wfi=%zu ccu=%zu uche=%zu) -> tracked=%zu dw "
                    "(wfi=%zu ccu=%zu uche=%zu) | saved=%zu dw (%.1f%%) unrelated_elided=%llu\n",
                    spec.name, mn.dwords, mn.wfi_count,
                    mn.ccu_flush_color + mn.ccu_flush_depth + mn.ccu_inv_color, mn.uche_inv,
                    mt.dwords, mt.wfi_count,
                    mt.ccu_flush_color + mt.ccu_flush_depth + mt.ccu_inv_color, mt.uche_inv,
                    mn.dwords - mt.dwords, pct,
                    static_cast<unsigned long long>(t_tracked.stats.unrelated_pass_barriers_elided));
    }
}

/* ============================================================================
 * Group 3: Exhaustive Matrix & Edge-Case Coverage (Cases 27..42, total >= 40)
 * ============================================================================ */
void test_group3_edge_matrix()
{
    std::vector<uint32_t> words(512, 0u);

    /* 6 cases: each individual dirty domain triggers exact minimal flush */
    const uint32_t dirty_cases[6] = {
        A840_SYNC_DIRTY_COLOR_CCU,
        A840_SYNC_DIRTY_DEPTH_CCU,
        A840_SYNC_DIRTY_UCHE_SHADER_WR,
        A840_SYNC_DIRTY_MEM_WRITE_CP,
        A840_SYNC_DIRTY_BLIT_2D_WR,
        A840_SYNC_DIRTY_GMEM_RESOLVE,
    };
    for (uint32_t d : dirty_cases) {
        CASE("dirty_domain_minimal_flush_matrix");
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t id = 5000u + d;
        CHECK(a840_sync_note_write(&t, d, &id, 1u) == A840_EMIT_OK);
        A840SyncPassAccess next{};
        next.read_ids = &id;
        next.read_count = 1u;
        next.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX | A840_SYNC_CONSUME_HOST_PRESENT;
        CHECK(a840_sync_emit_pass_barrier(&t, &cs, &next) == A840_EMIT_OK);
        StreamMetrics m = inspect_pm4(cs);
        CHECK(m.wfi_count == 1u);
        if (d == A840_SYNC_DIRTY_DEPTH_CCU) {
            CHECK(m.ccu_flush_depth == 1u && m.ccu_flush_color == 0u);
        } else if (d == A840_SYNC_DIRTY_MEM_WRITE_CP) {
            CHECK(m.wait_mem_count == 1u && m.ccu_flush_color == 0u && m.ccu_flush_depth == 0u);
        } else if (d == A840_SYNC_DIRTY_UCHE_SHADER_WR) {
            CHECK(m.uche_inv == 1u && m.ccu_flush_color == 0u && m.ccu_flush_depth == 0u);
        } else {
            CHECK(m.ccu_flush_color == 1u && m.ccu_flush_depth == 0u);
        }
    }

    /* 4 cases: exact boundary at 64 tracked resources (still precise, no overflow) */
    for (uint32_t count : {1u, 16u, 32u, 64u}) {
        CASE("tracked_resource_boundary_no_overflow");
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        std::vector<uint64_t> ids(count);
        for (uint32_t i = 0u; i < count; ++i) {
            ids[i] = 7000u + i;
        }
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_COLOR_CCU, ids.data(), count) == A840_EMIT_OK);
        CHECK(!t.overflow_conservative);
        CHECK(t.resource_count == count);
        /* Updating an existing ID does not overflow even at 64 */
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_DEPTH_CCU, &ids[0], 1u) == A840_EMIT_OK);
        CHECK(!t.overflow_conservative);
        CHECK(t.resource_count == count);
        /* Disjoint read still elided cleanly */
        const uint64_t other = 99999u;
        A840SyncPassAccess next{};
        next.read_ids = &other;
        next.read_count = 1u;
        next.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX;
        CHECK(a840_sync_emit_pass_barrier(&t, &cs, &next) == A840_EMIT_OK);
        CHECK(cs.used_dwords == 0u);
    }

    CASE("external_import_flag_forces_full_barrier");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t id = 8100u;
        A840SyncPassAccess next{};
        next.read_ids = &id;
        next.read_count = 1u;
        next.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX;
        next.flags = A840_SYNC_EXTERNAL_IMPORT;
        CHECK(a840_sync_emit_pass_barrier(&t, &cs, &next) == A840_EMIT_OK);
        StreamMetrics m = inspect_pm4(cs);
        CHECK(m.wfi_count == 1u);
        CHECK(m.wait_mem_count == 1u);
        CHECK(m.ccu_flush_color == 1u);
        CHECK(m.ccu_flush_depth == 1u);
        CHECK(m.uche_inv == 1u);
    }

    CASE("anonymous_pass_access_forces_conservative");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        A840SyncPassAccess empty{};
        empty.consumer_mask = A840_SYNC_CONSUME_ALL;
        CHECK(a840_sync_emit_pass_barrier(&t, &cs, &empty) == A840_EMIT_OK);
        CHECK(inspect_pm4(cs).wfi_count == 1u);
    }

    CASE("ccu_inv_color_emitted_when_shader_wrote_to_next_rt");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t rt = 8200u;
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_UCHE_SHADER_WR, &rt, 1u) == A840_EMIT_OK);
        A840SyncPassAccess next{};
        next.write_ids = &rt;
        next.write_count = 1u;
        next.consumer_mask = A840_SYNC_CONSUME_COLOR_RT;
        CHECK(a840_sync_emit_pass_barrier(&t, &cs, &next) == A840_EMIT_OK);
        StreamMetrics m = inspect_pm4(cs);
        CHECK(m.ccu_inv_color == 1u);
        CHECK(m.uche_inv == 1u);
        CHECK(m.wfi_count == 1u);
    }

    CASE("noop_write_and_read_do_not_dirty_state");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        CHECK(a840_sync_note_write(&t, A840_SYNC_DIRTY_NONE, nullptr, 0u) == A840_EMIT_OK);
        CHECK(a840_sync_note_read(&t, A840_SYNC_CONSUME_NONE, nullptr, 0u) == A840_EMIT_OK);
        CHECK(a840_sync_emit_wfi(&t, &cs, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);
        CHECK(cs.used_dwords == 0u);
    }

    CASE("zero_cache_mask_with_require_wfi");
    {
        A840SyncTracker t{};
        A840CmdStream cs{};
        CHECK(a840_sync_tracker_init(&t, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);
        CHECK(a840_cs_init(&cs, words.data(), words.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t id = 8300u;
        CHECK(a840_sync_note_read(&t, A840_SYNC_CONSUME_UCHE_TEX, &id, 1u) == A840_EMIT_OK);
        CHECK(a840_sync_emit_cache_barrier(&t, &cs, 0u, 0u, A840_SYNC_REQUIRE_WFI) == A840_EMIT_OK);
        CHECK(inspect_pm4(cs).wfi_count == 1u);
        CHECK(a840_sync_emit_cache_barrier(&t, &cs, 0u, 0u, A840_SYNC_REQUIRE_WFI) == A840_EMIT_OK);
        CHECK(inspect_pm4(cs).wfi_count == 1u);
    }
}

} // namespace

int main(int argc, char **argv)
{
    const char *dump_dir = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-pm4") == 0 && i + 1 < argc) {
            dump_dir = argv[++i];
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    test_group1_core_rules();
    test_group2_composite_scenarios(dump_dir);
    test_group3_edge_matrix();

    CHECK(g_cases >= 35u);
    std::printf("PASS test_sync_tracker: %u cases, %u checks\n", g_cases, g_checks);
    return 0;
}
