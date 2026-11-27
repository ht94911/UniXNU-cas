/* SPDX-License-Identifier: MIT */
/* Wave 10 Lane 3 Host Test Suite:
 *   - Long CommandBuffer Early-Kick Pipeline Chunking Engine (a840_cb_chunker)
 *   - RenderServer Early Writer Promise Scheduler (a840_early_promise_sched)
 *   - Safe Pass Boundary Enforcement & Mid-GMEM Split Rejection
 *   - Multi-Pass Dense Liquid Glass Overlap Proof & A8XX PM4 Export (--dump-pm4)
 *   - 1,000-Frame 120Hz Steady-State Zero-Heap-Allocation & < 500 us CB Hard Assert
 *   - 8-Thread Concurrent Stress & Lock-Free Callback Re-Entrancy (--tsan-stress N)
 * (50 independent cases)
 */
#include "a840_cb_chunker.h"
#include "a840_early_promise_sched.h"
#include "a840_kmd_writer_promise.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

unsigned g_cases = 0;
std::atomic<unsigned> g_checks{0};
const char *g_active_case = "init";
std::string g_dump_pm4_dir;
unsigned g_tsan_rounds = 1;

void begin_case(const char *name)
{
    g_active_case = name;
    ++g_cases;
}

void check_impl(bool ok, const char *expr, int line)
{
    g_checks.fetch_add(1u, std::memory_order_relaxed);
    if (!ok) {
        std::fprintf(stderr, "FAIL [%s] line %d: %s\n", g_active_case, line, expr);
        std::abort();
    }
}

#define CASE(name) begin_case(name)
#define CHECK(expr) check_impl((expr), #expr, __LINE__)

static_assert(static_cast<uint32_t>(A840_EARLY_ROLE_APP) ==
                  static_cast<uint32_t>(A840_CLIENT_ROLE_APP),
              "APP role ABI mismatch");
static_assert(static_cast<uint32_t>(A840_EARLY_ROLE_RENDER_SERVER) ==
                  static_cast<uint32_t>(A840_CLIENT_ROLE_RENDER_SERVER),
              "RENDER_SERVER role ABI mismatch");
static_assert(static_cast<uint32_t>(A840_EARLY_POLICY_NONE) ==
                  static_cast<uint32_t>(A840_PROMISE_POLICY_NONE),
              "NONE policy ABI mismatch");
static_assert(static_cast<uint32_t>(A840_EARLY_POLICY_EARLY_SCHEDULED) ==
                  static_cast<uint32_t>(A840_PROMISE_POLICY_EARLY_SCHEDULED),
              "EARLY_SCHEDULED policy ABI mismatch");
static_assert(static_cast<uint32_t>(A840_EARLY_POLICY_DEFER_UNTIL_RETIRED) ==
                  static_cast<uint32_t>(A840_PROMISE_POLICY_DEFER_UNTIL_RETIRED),
              "DEFER_UNTIL_RETIRED policy ABI mismatch");

struct CountingAllocator {
    std::atomic<uint64_t> alloc_count{0u};
    std::atomic<uint64_t> free_count{0u};
    std::atomic<uint64_t> bytes_allocated{0u};

    static void *alloc_cb(void *ctx, size_t size, size_t alignment)
    {
        auto *self = static_cast<CountingAllocator *>(ctx);
        size_t eff_align = alignment < sizeof(void *) ? sizeof(void *) : alignment;
        void *ptr = nullptr;
        if (posix_memalign(&ptr, eff_align, size) != 0) {
            return nullptr;
        }
        std::memset(ptr, 0, size);
        self->alloc_count.fetch_add(1u, std::memory_order_relaxed);
        self->bytes_allocated.fetch_add(size, std::memory_order_relaxed);
        return ptr;
    }

    static void free_cb(void *ctx, void *ptr)
    {
        auto *self = static_cast<CountingAllocator *>(ctx);
        if (ptr) {
            self->free_count.fetch_add(1u, std::memory_order_relaxed);
            std::free(ptr);
        }
    }

    A840ArenaAllocator iface()
    {
        A840ArenaAllocator a{};
        a.ctx = this;
        a.alloc_fn = &alloc_cb;
        a.free_fn = &free_cb;
        return a;
    }
};

struct RecordedChunk {
    uint64_t frame_id{0u};
    uint64_t cb_id{0u};
    uint32_t chunk_index{0u};
    uint32_t ring_id{0u};
    uint64_t chunk_seqno{0u};
    uint64_t dep_chunk_seqno{0u};
    uint32_t flags{0u};
    A840ChunkLifecycleStatus status{A840_CHUNK_STATUS_IDLE};
    A840ChunkKickReason kick_reason{A840_CHUNK_KICK_NONE};
    uint32_t pass_count{0u};
    uint32_t est_gpu_us{0u};
    uint32_t est_cpu_us{0u};
    A840CmdSlotHandle slot_handle{A840_CMD_SLOT_INVALID_HANDLE};
    std::vector<uint32_t> pm4;
    std::vector<A840Reloc> relocs;
    std::vector<uint64_t> surfaces;
};

struct MockKickSink {
    std::vector<RecordedChunk> chunks;
    int fail_on_chunk_index{-1};
    A840EarlyPromiseSched *linked_sched{nullptr};
    bool cancel_sched_on_fail{false};

    static A840ChunkStatus on_kick(void *ctx, const A840ChunkDesc *desc)
    {
        auto *self = static_cast<MockKickSink *>(ctx);
        if (self->fail_on_chunk_index >= 0 &&
            static_cast<int>(desc->chunk_index) == self->fail_on_chunk_index) {
            if (self->linked_sched && self->cancel_sched_on_fail) {
                (void)a840_early_promise_cancel_cb(self->linked_sched,
                                                   desc->cb_id,
                                                   A840_EARLY_CANCEL_CHUNK_SUBMIT_FAILED);
            }
            return A840_CHUNK_ERR_KICK_FAILED;
        }
        RecordedChunk rc;
        rc.frame_id = desc->frame_id;
        rc.cb_id = desc->cb_id;
        rc.chunk_index = desc->chunk_index;
        rc.ring_id = desc->ring_id;
        rc.chunk_seqno = desc->chunk_seqno;
        rc.dep_chunk_seqno = desc->dep_chunk_seqno;
        rc.flags = desc->flags;
        rc.status = desc->status;
        rc.kick_reason = desc->kick_reason;
        rc.pass_count = desc->pass_count;
        rc.est_gpu_us = desc->est_gpu_us;
        rc.est_cpu_us = desc->est_cpu_us;
        rc.slot_handle = desc->slot_handle;
        rc.pm4.assign(desc->pm4_words, desc->pm4_words + desc->pm4_dwords);
        if (desc->relocs && desc->reloc_count > 0u) {
            rc.relocs.assign(desc->relocs, desc->relocs + desc->reloc_count);
        }
        for (uint32_t i = 0u; i < desc->written_surface_count; ++i) {
            rc.surfaces.push_back(desc->written_surface_ids[i]);
        }
        self->chunks.push_back(std::move(rc));

        if (self->linked_sched) {
            bool is_final = (desc->flags & A840_CHUNK_FLAG_FINAL) != 0u;
            A840EarlyPromiseStatus pst = a840_early_promise_on_chunk_kicked(
                self->linked_sched,
                desc->cb_id,
                desc->chunk_seqno,
                desc->ring_id,
                desc->written_surface_count > 0u ? desc->written_surface_ids : nullptr,
                desc->written_surface_count,
                is_final);
            if (pst != A840_PROMISE_SCHED_OK) {
                return A840_CHUNK_ERR_KICK_FAILED;
            }
        }
        return A840_CHUNK_OK;
    }
};

struct PromiseRecorder {
    std::vector<A840EarlyPromiseEvent> events;
    A840EarlyPromiseSched *reentrant_sched{nullptr};
    bool do_reentrant_query{false};
    bool do_reentrant_stats{false};
    uint32_t reentrant_queries_ok{0u};

    static void on_event(void *ctx, const A840EarlyPromiseEvent *ev)
    {
        auto *self = static_cast<PromiseRecorder *>(ctx);
        self->events.push_back(*ev);
        if (self->reentrant_sched && self->do_reentrant_query) {
            A840EarlyPromiseSurfaceSnapshot snap{};
            if (a840_early_promise_query_surface(self->reentrant_sched,
                                                 ev->surface_id,
                                                 &snap) == A840_PROMISE_SCHED_OK &&
                snap.found) {
                self->reentrant_queries_ok++;
            }
        }
        if (self->reentrant_sched && self->do_reentrant_stats) {
            (void)a840_early_promise_get_stats(self->reentrant_sched);
        }
    }
};

void write_pm4_if_enabled(const std::string &name, const std::vector<uint32_t> &words)
{
    if (g_dump_pm4_dir.empty() || words.empty()) {
        return;
    }
    std::string path = g_dump_pm4_dir + "/" + name;
    FILE *fp = std::fopen(path.c_str(), "wb");
    CHECK(fp != nullptr);
    size_t written = std::fwrite(words.data(), sizeof(uint32_t), words.size(), fp);
    CHECK(written == words.size());
    CHECK(std::fclose(fp) == 0);
}

void verify_chunk_pm4_valid_type7_only(const std::vector<uint32_t> &words)
{
    CHECK(!words.empty());
    std::vector<A840PM4Packet> pkts(words.size());
    size_t count = 0u;
    CHECK(a840_pm4_decode(words.data(), words.size(), pkts.data(), pkts.size(), &count) ==
          A840_EMIT_OK);
    CHECK(count > 0u);
    for (size_t i = 0u; i < count; ++i) {
        CHECK(pkts[i].type == 7u);
    }
}

struct ChunkerEnv {
    CountingAllocator alloc;
    A840FrameArena arena{};
    A840CmdSlotPool pool{};
    A840CbChunker chunker{};
    MockKickSink sink{};

    void init(uint32_t frame_count = 3u,
              size_t per_frame_bytes = 65536u,
              uint32_t slot_cap = 32u,
              uint32_t dwords_per_slot = 1024u,
              const A840ChunkerConfig *override_cfg = nullptr)
    {
        A840ArenaAllocator a = alloc.iface();
        CHECK(a840_frame_arena_init(&arena, frame_count, per_frame_bytes, &a) == A840_ARENA_OK);
        CHECK(a840_cmd_slot_pool_init(&pool, slot_cap, dwords_per_slot, &a) == A840_ARENA_OK);
        A840ChunkerConfig cfg{};
        if (override_cfg) {
            cfg = *override_cfg;
        } else {
            a840_cb_chunker_default_config(&cfg);
        }
        if (!cfg.kick_fn) {
            cfg.kick_fn = &MockKickSink::on_kick;
            cfg.kick_ctx = &sink;
        }
        CHECK(a840_cb_chunker_init(&chunker, &arena, &pool, &cfg) == A840_CHUNK_OK);
        uint32_t slot_idx = 0u;
        CHECK(a840_frame_arena_begin_frame(&arena, 1u, &slot_idx) == A840_ARENA_OK);
    }

    ~ChunkerEnv()
    {
        a840_cmd_slot_pool_destroy(&pool);
        a840_frame_arena_destroy(&arena);
    }
};

void test_group1_chunker_boundaries()
{
    CASE("chunker_init_invalid_args_and_capacity");
    {
        CountingAllocator alloc;
        A840ArenaAllocator a = alloc.iface();
        A840FrameArena arena{};
        A840CmdSlotPool pool{}, tiny_pool{};
        A840CbChunker ch{};
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);

        CHECK(a840_frame_arena_init(&arena, 3u, 8192u, &a) == A840_ARENA_OK);
        CHECK(a840_cmd_slot_pool_init(&pool, 8u, 256u, &a) == A840_ARENA_OK);
        CHECK(a840_cmd_slot_pool_init(&tiny_pool, 4u, 16u, &a) == A840_ARENA_OK);

        CHECK(a840_cb_chunker_init(nullptr, &arena, &pool, &cfg) == A840_CHUNK_ERR_INVALID_ARG);
        CHECK(a840_cb_chunker_init(&ch, nullptr, &pool, &cfg) == A840_CHUNK_ERR_INVALID_ARG);
        CHECK(a840_cb_chunker_init(&ch, &arena, nullptr, &cfg) == A840_CHUNK_ERR_INVALID_ARG);

        cfg.ring_id = 4u;
        CHECK(a840_cb_chunker_init(&ch, &arena, &pool, &cfg) == A840_CHUNK_ERR_INVALID_ARG);
        cfg.ring_id = 0u;

        cfg.marker_gpu_va = UINT64_C(0x1004);
        CHECK(a840_cb_chunker_init(&ch, &arena, &pool, &cfg) == A840_CHUNK_ERR_INVALID_ARG);
        cfg.marker_gpu_va = UINT64_C(1) << 49;
        CHECK(a840_cb_chunker_init(&ch, &arena, &pool, &cfg) == A840_CHUNK_ERR_INVALID_ARG);
        cfg.marker_gpu_va = 0u;

        CHECK(a840_cb_chunker_init(&ch, &arena, &tiny_pool, &cfg) == A840_CHUNK_ERR_CAPACITY);
        CHECK(a840_cb_chunker_init(&ch, &arena, &pool, nullptr) == A840_CHUNK_OK);

        a840_cmd_slot_pool_destroy(&tiny_pool);
        a840_cmd_slot_pool_destroy(&pool);
        a840_frame_arena_destroy(&arena);
    }

    CASE("chunker_begin_cb_and_empty_cb_finish");
    {
        ChunkerEnv env;
        env.init();
        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 0u) == A840_CHUNK_ERR_INVALID_ARG);
        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 100u) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 101u) == A840_CHUNK_ERR_INVALID_STATE);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);
        CHECK(env.sink.chunks.empty());
        CHECK(a840_cmd_slot_pool_stats(&env.pool).in_use_current == 0u);
    }

    CASE("chunker_mid_gmem_force_kick_rejected");
    {
        ChunkerEnv env;
        env.init();
        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 101u) == A840_CHUNK_OK);
        uint64_t surf = 7001u;
        A840ChunkPassDesc p{};
        p.kind = A840_CHUNK_PASS_RENDER_GMEM;
        p.produce_dirty_mask = A840_SYNC_DIRTY_COLOR_CCU | A840_SYNC_DIRTY_GMEM_RESOLVE;
        p.est_gpu_us = 600u;
        p.est_cpu_us = 120u;
        p.written_surface_ids = &surf;
        p.written_surface_count = 1u;
        p.sync_access.write_ids = &surf;
        p.sync_access.write_count = 1u;

        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_is_in_gmem_pass(&env.chunker));
        CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 12u, 10u) == A840_CHUNK_OK);

        CHECK(a840_cb_chunker_force_kick(&env.chunker) == A840_CHUNK_ERR_MID_GMEM_PASS);
        CHECK(env.sink.chunks.empty());
        CHECK(a840_cb_chunker_is_in_gmem_pass(&env.chunker));
        CHECK(a840_cb_chunker_stats(&env.chunker).rejected_mid_gmem_kicks == 1u);

        CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 8u, 50u) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        CHECK(!a840_cb_chunker_is_in_gmem_pass(&env.chunker));
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);
        CHECK(env.sink.chunks.size() == 1u);
        verify_chunk_pm4_valid_type7_only(env.sink.chunks[0].pm4);
    }

    CASE("chunker_mid_gmem_finish_rejected");
    {
        ChunkerEnv env;
        env.init();
        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 102u) == A840_CHUNK_OK);
        A840ChunkPassDesc p{};
        p.kind = A840_CHUNK_PASS_RENDER_GMEM;
        p.produce_dirty_mask = A840_SYNC_DIRTY_COLOR_CCU;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_ERR_MID_GMEM_PASS);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);
        CHECK(env.sink.chunks.size() == 1u);
    }

    CASE("chunker_mid_compute_and_blit_force_kick_rejected");
    {
        ChunkerEnv env;
        env.init();
        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 103u) == A840_CHUNK_OK);

        A840ChunkPassDesc cp{};
        cp.kind = A840_CHUNK_PASS_COMPUTE;
        cp.produce_dirty_mask = A840_SYNC_DIRTY_UCHE_SHADER_WR;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &cp) == A840_CHUNK_OK);
        CHECK(!a840_cb_chunker_is_in_gmem_pass(&env.chunker));
        CHECK(a840_cb_chunker_force_kick(&env.chunker) == A840_CHUNK_ERR_MID_PASS);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_ERR_MID_PASS);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);

        A840ChunkPassDesc bp{};
        bp.kind = A840_CHUNK_PASS_BLIT;
        bp.produce_dirty_mask = A840_SYNC_DIRTY_BLIT_2D_WR;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &bp) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_force_kick(&env.chunker) == A840_CHUNK_ERR_MID_PASS);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);

        CHECK(a840_cb_chunker_stats(&env.chunker).rejected_mid_pass_kicks == 3u);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);
    }

    CASE("chunker_nested_pass_rejected");
    {
        ChunkerEnv env;
        env.init();
        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 104u) == A840_CHUNK_OK);
        A840ChunkPassDesc p{};
        p.kind = A840_CHUNK_PASS_COMPUTE;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_ERR_PASS_ACTIVE);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);
    }

    CASE("chunker_invalid_pass_args_and_zero_surface_rejected");
    {
        ChunkerEnv env;
        env.init();
        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 105u) == A840_CHUNK_OK);
        A840ChunkPassDesc bad{};
        bad.kind = A840_CHUNK_PASS_NONE;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &bad) == A840_CHUNK_ERR_INVALID_ARG);
        uint64_t zero_surf = 0u;
        bad.kind = A840_CHUNK_PASS_BLIT;
        bad.written_surface_ids = &zero_surf;
        bad.written_surface_count = 1u;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &bad) == A840_CHUNK_ERR_INVALID_ARG);
        bad.written_surface_ids = nullptr;
        bad.written_surface_count = 2u;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &bad) == A840_CHUNK_ERR_INVALID_ARG);
        CHECK(a840_cb_chunker_cmd_stream(&env.chunker) == nullptr);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);
    }

    CASE("chunker_manual_force_kick_at_safe_boundary");
    {
        ChunkerEnv env;
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 16u;
        cfg.max_dwords_per_chunk = 0u;
        cfg.max_est_gpu_us_per_chunk = 0u;
        env.init(3u, 65536u, 16u, 512u, &cfg);

        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 106u) == A840_CHUNK_OK);
        A840ChunkPassDesc p{};
        p.kind = A840_CHUNK_PASS_RENDER_SYSMEM;
        p.produce_dirty_mask = A840_SYNC_DIRTY_COLOR_CCU;
        p.est_gpu_us = 300u;
        p.est_cpu_us = 100u;

        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 8u, 1u) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        CHECK(env.sink.chunks.empty());

        CHECK(a840_cb_chunker_force_kick(&env.chunker) == A840_CHUNK_OK);
        CHECK(env.sink.chunks.size() == 1u);
        CHECK((env.sink.chunks[0].flags & A840_CHUNK_FLAG_EARLY_KICK) != 0u);
        CHECK((env.sink.chunks[0].flags & A840_CHUNK_FLAG_MANUAL_KICK) != 0u);
        CHECK(env.sink.chunks[0].dep_chunk_seqno == 0u);

        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 8u, 2u) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);

        CHECK(env.sink.chunks.size() == 2u);
        CHECK((env.sink.chunks[1].flags & A840_CHUNK_FLAG_FINAL) != 0u);
        CHECK(env.sink.chunks[1].dep_chunk_seqno == env.sink.chunks[0].chunk_seqno);
    }

    CASE("chunker_is_final_pass_hint_avoids_extra_chunk");
    {
        ChunkerEnv env;
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 2u;
        env.init(3u, 65536u, 16u, 512u, &cfg);

        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 107u) == A840_CHUNK_OK);
        for (int i = 0; i < 4; ++i) {
            A840ChunkPassDesc p{};
            p.kind = A840_CHUNK_PASS_COMPUTE;
            p.produce_dirty_mask = A840_SYNC_DIRTY_UCHE_SHADER_WR;
            p.is_final_pass = (i == 3);
            CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
            CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 6u, static_cast<uint32_t>(i)) == A840_CHUNK_OK);
            CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        }
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);
        CHECK(env.sink.chunks.size() == 2u);
        CHECK((env.sink.chunks[0].flags & A840_CHUNK_FLAG_EARLY_KICK) != 0u);
        CHECK((env.sink.chunks[1].flags & A840_CHUNK_FLAG_FINAL) != 0u);
        CHECK((env.sink.chunks[1].flags & A840_CHUNK_FLAG_FINAL_BARRIER_ONLY) == 0u);
    }

    CASE("chunker_finish_after_early_kick_emits_final_marker_chunk");
    {
        ChunkerEnv env;
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 2u;
        env.init(3u, 65536u, 16u, 512u, &cfg);

        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 108u) == A840_CHUNK_OK);
        for (int i = 0; i < 2; ++i) {
            A840ChunkPassDesc p{};
            p.kind = A840_CHUNK_PASS_BLIT;
            p.produce_dirty_mask = A840_SYNC_DIRTY_BLIT_2D_WR;
            CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
            CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 4u, static_cast<uint32_t>(i)) == A840_CHUNK_OK);
            CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        }
        CHECK(env.sink.chunks.size() == 1u);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);
        CHECK(env.sink.chunks.size() == 2u);
        CHECK((env.sink.chunks[1].flags & A840_CHUNK_FLAG_FINAL) != 0u);
        CHECK((env.sink.chunks[1].flags & A840_CHUNK_FLAG_FINAL_BARRIER_ONLY) != 0u);
        verify_chunk_pm4_valid_type7_only(env.sink.chunks[1].pm4);
    }
}
void test_group2_thresholds_and_sync()
{
    CASE("threshold_max_passes_per_chunk_triggers_early_kick");
    {
        ChunkerEnv env;
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 2u;
        cfg.max_dwords_per_chunk = 0u;
        cfg.max_est_gpu_us_per_chunk = 0u;
        env.init(3u, 65536u, 16u, 512u, &cfg);

        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 201u) == A840_CHUNK_OK);
        for (int i = 0; i < 5; ++i) {
            A840ChunkPassDesc p{};
            p.kind = A840_CHUNK_PASS_RENDER_GMEM;
            p.produce_dirty_mask = A840_SYNC_DIRTY_COLOR_CCU;
            p.est_gpu_us = 250u;
            p.est_cpu_us = 60u;
            CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
            CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 10u, static_cast<uint32_t>(i)) == A840_CHUNK_OK);
            CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        }
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);
        CHECK(env.sink.chunks.size() == 3u);
        CHECK(env.sink.chunks[0].pass_count == 2u);
        CHECK(env.sink.chunks[0].kick_reason == A840_CHUNK_KICK_THRESHOLD_PASSES);
        CHECK(env.sink.chunks[1].pass_count == 2u);
        CHECK(env.sink.chunks[1].kick_reason == A840_CHUNK_KICK_THRESHOLD_PASSES);
        CHECK(env.sink.chunks[2].pass_count == 1u);
        CHECK(env.sink.chunks[2].kick_reason == A840_CHUNK_KICK_FINAL);
        CHECK(a840_cb_chunker_stats(&env.chunker).threshold_pass_kicks == 2u);
    }

    CASE("threshold_max_dwords_per_chunk_triggers_early_kick");
    {
        ChunkerEnv env;
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 0u;
        cfg.max_dwords_per_chunk = 128u;
        cfg.max_est_gpu_us_per_chunk = 0u;
        env.init(3u, 65536u, 16u, 512u, &cfg);

        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 202u) == A840_CHUNK_OK);
        A840ChunkPassDesc p{};
        p.kind = A840_CHUNK_PASS_COMPUTE;
        p.produce_dirty_mask = A840_SYNC_DIRTY_UCHE_SHADER_WR;
        p.est_gpu_us = 100u;
        p.est_cpu_us = 40u;

        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 140u, 100u) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        CHECK(env.sink.chunks.size() == 1u);
        CHECK(env.sink.chunks[0].kick_reason == A840_CHUNK_KICK_THRESHOLD_DWORDS);
        CHECK(env.sink.chunks[0].pm4.size() >= 128u);

        p.is_final_pass = true;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 16u, 200u) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_stats(&env.chunker).threshold_dword_kicks == 1u);
    }

    CASE("threshold_max_est_gpu_us_triggers_early_kick");
    {
        ChunkerEnv env;
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 0u;
        cfg.max_dwords_per_chunk = 0u;
        cfg.max_est_gpu_us_per_chunk = 1500u;
        env.init(3u, 65536u, 16u, 512u, &cfg);

        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 203u) == A840_CHUNK_OK);
        A840ChunkPassDesc p{};
        p.kind = A840_CHUNK_PASS_RENDER_GMEM;
        p.produce_dirty_mask = A840_SYNC_DIRTY_COLOR_CCU;
        p.est_cpu_us = 120u;

        p.est_gpu_us = 900u;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        CHECK(env.sink.chunks.empty());

        p.est_gpu_us = 800u;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        CHECK(env.sink.chunks.size() == 1u);
        CHECK(env.sink.chunks[0].kick_reason == A840_CHUNK_KICK_THRESHOLD_GPU_US);
        CHECK(env.sink.chunks[0].est_gpu_us == 1700u);

        p.est_gpu_us = 400u;
        p.is_final_pass = true;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_stats(&env.chunker).threshold_gpu_us_kicks == 1u);
    }

    CASE("chunk_seqno_monotonic_and_dep_chain_across_multiple_cbs");
    {
        ChunkerEnv env;
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 1u;
        env.init(3u, 65536u, 32u, 512u, &cfg);

        for (uint64_t cb = 1u; cb <= 2u; ++cb) {
            CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, cb) == A840_CHUNK_OK);
            for (int i = 0; i < 3; ++i) {
                A840ChunkPassDesc p{};
                p.kind = A840_CHUNK_PASS_COMPUTE;
                p.produce_dirty_mask = A840_SYNC_DIRTY_UCHE_SHADER_WR;
                p.is_final_pass = (i == 2);
                CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
                CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
            }
            CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);
        }
        CHECK(env.sink.chunks.size() == 6u);
        for (size_t i = 0u; i < 6u; ++i) {
            CHECK(env.sink.chunks[i].chunk_seqno == i + 1u);
            if (i == 0u || i == 3u) {
                CHECK(env.sink.chunks[i].dep_chunk_seqno == 0u);
                CHECK((env.sink.chunks[i].flags & A840_CHUNK_FLAG_HAS_ORDER_DEP) == 0u);
            } else {
                CHECK(env.sink.chunks[i].dep_chunk_seqno == env.sink.chunks[i - 1u].chunk_seqno);
                CHECK((env.sink.chunks[i].flags & A840_CHUNK_FLAG_HAS_ORDER_DEP) != 0u);
            }
        }
    }

    CASE("cross_chunk_sync_tracker_clears_dirty_and_elides_redundant_flush");
    {
        auto run_mode = [](A840SyncMode mode, uint64_t &out_dwords, A840SyncStats &out_sync) {
            ChunkerEnv env;
            A840ChunkerConfig cfg{};
            a840_cb_chunker_default_config(&cfg);
            cfg.max_passes_per_chunk = 1u;
            cfg.sync_mode = mode;
            env.init(3u, 65536u, 16u, 512u, &cfg);

            uint64_t res_a = 1001u, res_b = 1002u;
            CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 300u) == A840_CHUNK_OK);

            A840ChunkPassDesc p0{};
            p0.kind = A840_CHUNK_PASS_RENDER_GMEM;
            p0.sync_access.write_ids = &res_a;
            p0.sync_access.write_count = 1u;
            p0.produce_dirty_mask = A840_SYNC_DIRTY_COLOR_CCU;
            CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p0) == A840_CHUNK_OK);
            CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 8u, 1u) == A840_CHUNK_OK);
            CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);

            A840ChunkPassDesc p1{};
            p1.kind = A840_CHUNK_PASS_RENDER_GMEM;
            p1.sync_access.read_ids = &res_a;
            p1.sync_access.read_count = 1u;
            p1.sync_access.write_ids = &res_b;
            p1.sync_access.write_count = 1u;
            p1.sync_access.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX;
            p1.produce_dirty_mask = A840_SYNC_DIRTY_COLOR_CCU;
            p1.is_final_pass = true;
            CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p1) == A840_CHUNK_OK);
            CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 8u, 2u) == A840_CHUNK_OK);
            CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
            CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);

            out_dwords = a840_cb_chunker_stats(&env.chunker).total_pm4_dwords;
            out_sync = a840_cb_chunker_sync_stats(&env.chunker);
        };

        uint64_t dw_tracked = 0u, dw_naive = 0u;
        A840SyncStats st_tracked{}, st_naive{};
        run_mode(A840_SYNC_MODE_TRACKED, dw_tracked, st_tracked);
        run_mode(A840_SYNC_MODE_NAIVE, dw_naive, st_naive);
        CHECK(dw_tracked < dw_naive);
        CHECK(st_tracked.ccu_flush_elided > 0u || st_tracked.uche_inv_elided > 0u ||
              st_tracked.dwords_saved > 0u);
    }

    CASE("disjoint_passes_across_chunks_elide_intra_chunk_barriers");
    {
        ChunkerEnv env;
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 2u;
        env.init(3u, 65536u, 16u, 512u, &cfg);

        uint64_t r1 = 501u, r2 = 502u;
        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 301u) == A840_CHUNK_OK);

        A840ChunkPassDesc p0{};
        p0.kind = A840_CHUNK_PASS_RENDER_GMEM;
        p0.sync_access.write_ids = &r1;
        p0.sync_access.write_count = 1u;
        p0.produce_dirty_mask = A840_SYNC_DIRTY_COLOR_CCU;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p0) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);

        A840ChunkPassDesc p1{};
        p1.kind = A840_CHUNK_PASS_RENDER_GMEM;
        p1.sync_access.write_ids = &r2;
        p1.sync_access.write_count = 1u;
        p1.sync_access.consumer_mask = A840_SYNC_CONSUME_COLOR_RT;
        p1.produce_dirty_mask = A840_SYNC_DIRTY_COLOR_CCU;
        p1.is_final_pass = true;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p1) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);

        CHECK(a840_cb_chunker_sync_stats(&env.chunker).unrelated_pass_barriers_elided >= 1u);
    }

    CASE("marker_gpu_va_emits_genuine_cache_flush_ts_and_reloc");
    {
        ChunkerEnv env;
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 1u;
        cfg.marker_bo_handle = 9001u;
        cfg.marker_gpu_va = UINT64_C(0x40000000);
        env.init(3u, 65536u, 16u, 512u, &cfg);

        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 302u) == A840_CHUNK_OK);
        A840ChunkPassDesc p{};
        p.kind = A840_CHUNK_PASS_COMPUTE;
        p.produce_dirty_mask = A840_SYNC_DIRTY_UCHE_SHADER_WR;
        p.is_final_pass = true;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_emit_pass_timestamp(&env.chunker,
                                                  A840_EVENT_RB_DONE_TS,
                                                  9002u,
                                                  UINT64_C(0x40001000),
                                                  42u) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);

        CHECK(env.sink.chunks.size() == 1u);
        CHECK((env.sink.chunks[0].flags & A840_CHUNK_FLAG_HAS_SEQNO_MARKER) != 0u);
        CHECK(env.sink.chunks[0].relocs.size() == 2u);
        CHECK(env.sink.chunks[0].relocs[0].bo_handle == 9002u);
        CHECK(env.sink.chunks[0].relocs[1].bo_handle == 9001u);
        CHECK(env.sink.chunks[0].relocs[1].gpu_va == UINT64_C(0x40000000));
        verify_chunk_pm4_valid_type7_only(env.sink.chunks[0].pm4);
    }

    CASE("chunk_pm4_structural_decode_and_zero_type4_cp_protect");
    {
        ChunkerEnv env;
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 2u;
        cfg.marker_bo_handle = 88u;
        cfg.marker_gpu_va = UINT64_C(0x50000000);
        env.init(3u, 65536u, 16u, 512u, &cfg);

        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 303u) == A840_CHUNK_OK);
        for (int i = 0; i < 4; ++i) {
            A840ChunkPassDesc p{};
            p.kind = (i % 2 == 0) ? A840_CHUNK_PASS_RENDER_GMEM : A840_CHUNK_PASS_COMPUTE;
            p.produce_dirty_mask = A840_SYNC_DIRTY_COLOR_CCU | A840_SYNC_DIRTY_UCHE_SHADER_WR;
            p.is_final_pass = (i == 3);
            CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
            CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 20u, static_cast<uint32_t>(i * 100)) == A840_CHUNK_OK);
            CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        }
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);
        CHECK(env.sink.chunks.size() == 2u);
        for (const auto &rc : env.sink.chunks) {
            verify_chunk_pm4_valid_type7_only(rc.pm4);
        }
    }

    CASE("slot_pool_exhaustion_aborts_cleanly_zero_orphan_chunks");
    {
        ChunkerEnv env;
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 1u;
        env.init(3u, 65536u, 2u, 256u, &cfg);

        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 304u) == A840_CHUNK_OK);
        A840ChunkPassDesc p{};
        p.kind = A840_CHUNK_PASS_COMPUTE;
        p.produce_dirty_mask = A840_SYNC_DIRTY_UCHE_SHADER_WR;

        for (int i = 0; i < 2; ++i) {
            CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
            CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        }
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_ERR_SLOT_EXHAUSTED);
        CHECK(a840_cb_chunker_is_aborted(&env.chunker));
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_ERR_ABORTED);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_ERR_ABORTED);
        CHECK(env.sink.chunks.size() == 2u);
    }

    CASE("arena_oom_aborts_cleanly_and_releases_slot");
    {
        ChunkerEnv env;
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 1u;
        cfg.reloc_capacity_per_chunk = 64u;
        env.init(3u, 2560u, 8u, 256u, &cfg);

        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 305u) == A840_CHUNK_OK);
        A840ChunkPassDesc p{};
        p.kind = A840_CHUNK_PASS_BLIT;
        p.produce_dirty_mask = A840_SYNC_DIRTY_BLIT_2D_WR;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);

        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_ERR_ARENA_OOM);
        CHECK(a840_cb_chunker_is_aborted(&env.chunker));
        CHECK(a840_cmd_slot_pool_stats(&env.pool).in_use_current == 1u);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_ERR_ABORTED);
    }
}

void test_group3_liquid_glass_and_faults()
{
    CASE("dense_liquid_glass_8pass_monolithic_vs_chunked_overlap_gain");
    {
        ChunkerEnv env;
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 2u;
        cfg.max_dwords_per_chunk = 512u;
        cfg.max_est_gpu_us_per_chunk = 1500u;
        cfg.ring_id = 0u;
        cfg.marker_bo_handle = 777u;
        cfg.marker_gpu_va = UINT64_C(0x80000000);
        env.init(3u, 131072u, 32u, 1024u, &cfg);

        const uint64_t folder_s[4] = {2001u, 2002u, 2003u, 2004u};
        const uint64_t blur_tmp[2] = {2101u, 2102u};
        const uint64_t final_disp = 2200u;

        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 401u) == A840_CHUNK_OK);
        for (int pass = 0; pass < 8; ++pass) {
            A840ChunkPassDesc p{};
            uint64_t r_id = 0u, w_id = 0u, s_id = 0u;
            if (pass < 4) {
                p.kind = A840_CHUNK_PASS_RENDER_GMEM;
                w_id = folder_s[pass];
                s_id = folder_s[pass];
                p.produce_dirty_mask = A840_SYNC_DIRTY_COLOR_CCU | A840_SYNC_DIRTY_GMEM_RESOLVE;
                p.est_gpu_us = 650u;
                p.est_cpu_us = 220u;
            } else if (pass < 6) {
                p.kind = A840_CHUNK_PASS_COMPUTE;
                r_id = folder_s[(pass - 4) * 2];
                w_id = blur_tmp[pass - 4];
                p.sync_access.read_ids = &r_id;
                p.sync_access.read_count = 1u;
                p.sync_access.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX | A840_SYNC_CONSUME_COMPUTE_RW;
                p.produce_dirty_mask = A840_SYNC_DIRTY_UCHE_SHADER_WR;
                p.est_gpu_us = 700u;
                p.est_cpu_us = 180u;
            } else {
                p.kind = (pass == 6) ? A840_CHUNK_PASS_BLIT : A840_CHUNK_PASS_RENDER_GMEM;
                r_id = blur_tmp[pass - 6];
                w_id = final_disp;
                s_id = final_disp;
                p.sync_access.read_ids = &r_id;
                p.sync_access.read_count = 1u;
                p.sync_access.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX;
                p.produce_dirty_mask = (pass == 6) ? A840_SYNC_DIRTY_BLIT_2D_WR
                                                   : (A840_SYNC_DIRTY_COLOR_CCU | A840_SYNC_DIRTY_GMEM_RESOLVE);
                p.est_gpu_us = 550u;
                p.est_cpu_us = 190u;
                p.is_final_pass = (pass == 7);
            }
            p.sync_access.write_ids = &w_id;
            p.sync_access.write_count = 1u;
            if (s_id != 0u) {
                p.written_surface_ids = &s_id;
                p.written_surface_count = 1u;
            }
            CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
            CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 36u, static_cast<uint32_t>(1000 + pass * 100)) == A840_CHUNK_OK);
            CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        }
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);

        CHECK(env.sink.chunks.size() == 4u);
        A840ChunkerStats st = a840_cb_chunker_stats(&env.chunker);
        CHECK(st.early_kicks == 3u);
        CHECK(st.final_kicks == 1u);
        CHECK(st.monolithic_total_us == st.monolithic_cpu_wait_us + 5100u);
        CHECK(st.chunked_critical_path_us < st.monolithic_total_us);
        CHECK(st.gpu_overlap_gain_us >= 1000u);

        std::printf("  [LiquidGlass 8-pass] chunks=%llu early=%llu monolithic=%llu us -> chunked=%llu us | overlap_gain=%llu us\n",
                    static_cast<unsigned long long>(st.last_cb_chunks_kicked),
                    static_cast<unsigned long long>(st.last_cb_early_kicks),
                    static_cast<unsigned long long>(st.monolithic_total_us),
                    static_cast<unsigned long long>(st.chunked_critical_path_us),
                    static_cast<unsigned long long>(st.gpu_overlap_gain_us));

        for (size_t c = 0u; c < env.sink.chunks.size(); ++c) {
            verify_chunk_pm4_valid_type7_only(env.sink.chunks[c].pm4);
            write_pm4_if_enabled("w10_liquid_glass_chunk_" + std::to_string(c) + ".pm4",
                                 env.sink.chunks[c].pm4);
        }
    }

    CASE("liquid_glass_tracked_vs_naive_sync_ab_comparison");
    {
        auto run_ab = [](A840SyncMode mode, const char *prefix) -> uint64_t {
            ChunkerEnv env;
            A840ChunkerConfig cfg{};
            a840_cb_chunker_default_config(&cfg);
            cfg.max_passes_per_chunk = 2u;
            cfg.sync_mode = mode;
            cfg.marker_bo_handle = 778u;
            cfg.marker_gpu_va = UINT64_C(0x81000000);
            env.init(3u, 65536u, 16u, 512u, &cfg);

            uint64_t bufs[4] = {3001u, 3002u, 3003u, 3004u};
            CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 402u) == A840_CHUNK_OK);
            for (int i = 0; i < 4; ++i) {
                A840ChunkPassDesc p{};
                p.kind = A840_CHUNK_PASS_RENDER_GMEM;
                p.sync_access.write_ids = &bufs[i];
                p.sync_access.write_count = 1u;
                p.sync_access.consumer_mask = A840_SYNC_CONSUME_COLOR_RT;
                p.produce_dirty_mask = A840_SYNC_DIRTY_COLOR_CCU;
                p.est_gpu_us = 400u;
                p.est_cpu_us = 120u;
                p.is_final_pass = (i == 3);
                CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
                CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 16u, static_cast<uint32_t>(i)) == A840_CHUNK_OK);
                CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
            }
            CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);
            for (size_t c = 0u; c < env.sink.chunks.size(); ++c) {
                verify_chunk_pm4_valid_type7_only(env.sink.chunks[c].pm4);
                write_pm4_if_enabled(std::string(prefix) + "_chunk_" + std::to_string(c) + ".pm4",
                                     env.sink.chunks[c].pm4);
            }
            return a840_cb_chunker_stats(&env.chunker).total_pm4_dwords;
        };

        uint64_t dw_tracked = run_ab(A840_SYNC_MODE_TRACKED, "w10_ab_tracked");
        uint64_t dw_naive = run_ab(A840_SYNC_MODE_NAIVE, "w10_ab_naive");
        CHECK(dw_tracked < dw_naive);
    }

    CASE("camera_nv12_to_glass_mixed_gmem_compute_blit_chunking");
    {
        ChunkerEnv env;
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 2u;
        cfg.max_est_gpu_us_per_chunk = 1200u;
        cfg.marker_bo_handle = 779u;
        cfg.marker_gpu_va = UINT64_C(0x82000000);
        env.init(3u, 65536u, 16u, 512u, &cfg);

        uint64_t nv12 = 4001u, rgba = 4002u, glass = 4003u;
        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 403u) == A840_CHUNK_OK);

        A840ChunkPassDesc p0{};
        p0.kind = A840_CHUNK_PASS_BLIT;
        p0.sync_access.read_ids = &nv12;
        p0.sync_access.read_count = 1u;
        p0.sync_access.write_ids = &rgba;
        p0.sync_access.write_count = 1u;
        p0.sync_access.consumer_mask = A840_SYNC_CONSUME_BLIT_2D;
        p0.produce_dirty_mask = A840_SYNC_DIRTY_BLIT_2D_WR;
        p0.est_gpu_us = 450u;
        p0.est_cpu_us = 100u;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p0) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 24u, 11u) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);

        A840ChunkPassDesc p1{};
        p1.kind = A840_CHUNK_PASS_COMPUTE;
        p1.sync_access.read_ids = &rgba;
        p1.sync_access.read_count = 1u;
        p1.sync_access.write_ids = &rgba;
        p1.sync_access.write_count = 1u;
        p1.sync_access.consumer_mask = A840_SYNC_CONSUME_COMPUTE_RW;
        p1.produce_dirty_mask = A840_SYNC_DIRTY_UCHE_SHADER_WR;
        p1.est_gpu_us = 800u;
        p1.est_cpu_us = 140u;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p1) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 24u, 22u) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);

        A840ChunkPassDesc p2{};
        p2.kind = A840_CHUNK_PASS_RENDER_GMEM;
        p2.sync_access.read_ids = &rgba;
        p2.sync_access.read_count = 1u;
        p2.sync_access.write_ids = &glass;
        p2.sync_access.write_count = 1u;
        p2.sync_access.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX;
        p2.produce_dirty_mask = A840_SYNC_DIRTY_COLOR_CCU | A840_SYNC_DIRTY_GMEM_RESOLVE;
        p2.written_surface_ids = &glass;
        p2.written_surface_count = 1u;
        p2.est_gpu_us = 600u;
        p2.est_cpu_us = 150u;
        p2.is_final_pass = true;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p2) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 24u, 33u) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);

        CHECK(env.sink.chunks.size() == 2u);
        for (size_t c = 0u; c < env.sink.chunks.size(); ++c) {
            verify_chunk_pm4_valid_type7_only(env.sink.chunks[c].pm4);
            write_pm4_if_enabled("w10_camera_glass_chunk_" + std::to_string(c) + ".pm4",
                                 env.sink.chunks[c].pm4);
        }
    }

    CASE("chunk_kick_failure_propagates_abort_immediately");
    {
        ChunkerEnv env;
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 1u;
        env.init(3u, 65536u, 16u, 512u, &cfg);
        env.sink.fail_on_chunk_index = 1;

        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 404u) == A840_CHUNK_OK);
        A840ChunkPassDesc p{};
        p.kind = A840_CHUNK_PASS_COMPUTE;
        p.produce_dirty_mask = A840_SYNC_DIRTY_UCHE_SHADER_WR;

        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        CHECK(env.sink.chunks.size() == 1u);

        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_ERR_KICK_FAILED);
        CHECK(a840_cb_chunker_is_aborted(&env.chunker));
        CHECK(env.chunker.last_chunk_status == A840_CHUNK_STATUS_ABORTED);

        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_ERR_ABORTED);
        CHECK(a840_cb_chunker_force_kick(&env.chunker) == A840_CHUNK_ERR_ABORTED);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_ERR_ABORTED);
        CHECK(env.sink.chunks.size() == 1u);
        CHECK(a840_cb_chunker_stats(&env.chunker).aborted_chunks == 1u);
    }

    CASE("explicit_abort_mid_gmem_releases_unsubmitted_slot_safely");
    {
        ChunkerEnv env;
        env.init();
        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 405u) == A840_CHUNK_OK);
        A840ChunkPassDesc p{};
        p.kind = A840_CHUNK_PASS_RENDER_GMEM;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cmd_slot_pool_stats(&env.pool).in_use_current == 1u);
        a840_cb_chunker_abort(&env.chunker);
        CHECK(a840_cb_chunker_is_aborted(&env.chunker));
        CHECK(!a840_cb_chunker_is_in_gmem_pass(&env.chunker));
        CHECK(a840_cmd_slot_pool_stats(&env.pool).in_use_current == 0u);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_ERR_ABORTED);
    }

    CASE("chunk_surface_deduplication_and_capacity_limit");
    {
        ChunkerEnv env;
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 16u;
        env.init(3u, 65536u, 16u, 512u, &cfg);

        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 406u) == A840_CHUNK_OK);
        uint64_t repeated[3] = {5001u, 5001u, 5002u};
        A840ChunkPassDesc p{};
        p.kind = A840_CHUNK_PASS_RENDER_SYSMEM;
        p.written_surface_ids = repeated;
        p.written_surface_count = 3u;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);

        uint64_t thirty[30];
        for (uint32_t i = 0u; i < 30u; ++i) {
            thirty[i] = 5100u + i;
        }
        p.written_surface_ids = thirty;
        p.written_surface_count = 30u;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);

        uint64_t overflow_s = 5999u;
        p.written_surface_ids = &overflow_s;
        p.written_surface_count = 1u;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_ERR_CAPACITY);

        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);
        CHECK(env.sink.chunks.size() == 1u);
        CHECK(env.sink.chunks[0].surfaces.size() == 32u);
    }
}
void test_group4_early_promise_scheduler()
{
    CASE("promise_sched_abi_alignment_with_kmd_writer_promise");
    {
        A840EarlyPromiseEvent ev{};
        ev.kind = A840_PROMISE_EVENT_EARLY_SCHEDULED;
        ev.client_role = A840_EARLY_ROLE_RENDER_SERVER;
        ev.policy = A840_EARLY_POLICY_EARLY_SCHEDULED;
        ev.surface_id = 9100u;
        ev.chunk_seqno = 77u;
        ev.promise_token = 5u;
        ev.ring_id = 0u;

        struct NativeAdrenoAbiWriterPromiseV3 abi{};
        CHECK(a840_early_promise_fill_abi_v3(&ev, &abi) == A840_PROMISE_SCHED_OK);
        CHECK(abi.op == NativeAdrenoPromiseFulfill);
        CHECK(abi.surface_count == 1u);
        CHECK(abi.promise_token == 5u);
        CHECK(abi.queue == 0u);
        CHECK(abi.fence == 77u);
        CHECK(abi.surface_ids[0] == 9100u);

        ev.kind = A840_PROMISE_EVENT_PROMISE_CANCELED;
        CHECK(a840_early_promise_fill_abi_v3(&ev, &abi) == A840_PROMISE_SCHED_OK);
        CHECK(abi.op == NativeAdrenoPromiseCancel);

        ev.kind = A840_PROMISE_EVENT_NONE;
        CHECK(a840_early_promise_fill_abi_v3(&ev, &abi) == A840_PROMISE_SCHED_ERR_INVALID_ARG);
    }

    CASE("promise_sched_init_invalid_args_rejected");
    {
        A840EarlyPromiseSched s{};
        A840EarlyPromiseConfig cfg{};
        CHECK(a840_early_promise_init(nullptr, &cfg) == A840_PROMISE_SCHED_ERR_INVALID_ARG);
        CHECK(a840_early_promise_init(&s, nullptr) == A840_PROMISE_SCHED_ERR_INVALID_ARG);
        cfg.client_id = 0u;
        cfg.client_role = A840_EARLY_ROLE_RENDER_SERVER;
        CHECK(a840_early_promise_init(&s, &cfg) == A840_PROMISE_SCHED_ERR_INVALID_ARG);
        cfg.client_id = 1u;
        cfg.client_role = static_cast<A840EarlyClientRole>(99u);
        CHECK(a840_early_promise_init(&s, &cfg) == A840_PROMISE_SCHED_ERR_INVALID_ARG);
    }

    CASE("render_server_single_surface_last_chunk_emits_early_scheduled_immediately");
    {
        A840EarlyPromiseSched sched{};
        PromiseRecorder rec;
        A840EarlyPromiseConfig cfg{};
        cfg.client_id = 10u;
        cfg.client_role = A840_EARLY_ROLE_RENDER_SERVER;
        cfg.callback = &PromiseRecorder::on_event;
        cfg.callback_ctx = &rec;

        CHECK(a840_early_promise_init(&sched, &cfg) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_begin_frame(&sched, 1u, 100u, 0u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 8001u, 1u, 0u, true) == A840_PROMISE_SCHED_OK);

        uint64_t sid = 8001u;
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 100u, 11u, 0u, &sid, 1u, true) ==
              A840_PROMISE_SCHED_OK);

        CHECK(rec.events.size() == 1u);
        CHECK(rec.events[0].kind == A840_PROMISE_EVENT_EARLY_SCHEDULED);
        CHECK(rec.events[0].policy == A840_EARLY_POLICY_EARLY_SCHEDULED);
        CHECK(rec.events[0].surface_id == 8001u);
        CHECK(rec.events[0].generation == 1u);
        CHECK(rec.events[0].chunk_seqno == 11u);
        CHECK(rec.events[0].ring_id == 0u);
        CHECK(rec.events[0].display_ready == 1u);

        A840EarlyPromiseSurfaceSnapshot snap{};
        CHECK(a840_early_promise_query_surface(&sched, 8001u, &snap) == A840_PROMISE_SCHED_OK);
        CHECK(snap.display_ready == 1u);
        CHECK(snap.generation == 1u);
        a840_early_promise_destroy(&sched);
    }

    CASE("render_server_intermediate_chunk_does_not_publish_prematurely");
    {
        A840EarlyPromiseSched sched{};
        PromiseRecorder rec;
        A840EarlyPromiseConfig cfg{};
        cfg.client_id = 10u;
        cfg.client_role = A840_EARLY_ROLE_RENDER_SERVER;
        cfg.callback = &PromiseRecorder::on_event;
        cfg.callback_ctx = &rec;

        CHECK(a840_early_promise_init(&sched, &cfg) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_begin_frame(&sched, 1u, 101u, 0u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 8002u, 3u, 0u, true) == A840_PROMISE_SCHED_OK);

        uint64_t sid = 8002u;
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 101u, 1u, 0u, &sid, 1u, false) == A840_PROMISE_SCHED_OK);
        CHECK(rec.events.empty());
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 101u, 2u, 0u, &sid, 1u, false) == A840_PROMISE_SCHED_OK);
        CHECK(rec.events.empty());
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 101u, 3u, 0u, &sid, 1u, true) == A840_PROMISE_SCHED_OK);
        CHECK(rec.events.size() == 1u);
        CHECK(rec.events[0].kind == A840_PROMISE_EVENT_EARLY_SCHEDULED);
        CHECK(rec.events[0].chunk_seqno == 3u);
        a840_early_promise_destroy(&sched);
    }

    CASE("render_server_dynamic_unplanned_surfaces_hold_until_final_chunk");
    {
        A840EarlyPromiseSched sched{};
        PromiseRecorder rec;
        A840EarlyPromiseConfig cfg{};
        cfg.client_id = 10u;
        cfg.client_role = A840_EARLY_ROLE_RENDER_SERVER;
        cfg.callback = &PromiseRecorder::on_event;
        cfg.callback_ctx = &rec;

        CHECK(a840_early_promise_init(&sched, &cfg) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_begin_frame(&sched, 1u, 102u, 0u) == A840_PROMISE_SCHED_OK);

        uint64_t s1 = 8010u, s2 = 8011u;
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 102u, 10u, 0u, &s1, 1u, false) == A840_PROMISE_SCHED_OK);
        CHECK(rec.events.empty());
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 102u, 11u, 0u, &s2, 1u, true) == A840_PROMISE_SCHED_OK);
        CHECK(rec.events.size() == 2u);
        CHECK(rec.events[0].surface_id == 8010u);
        CHECK(rec.events[0].chunk_seqno == 10u);
        CHECK(rec.events[1].surface_id == 8011u);
        CHECK(rec.events[1].chunk_seqno == 11u);
        a840_early_promise_destroy(&sched);
    }

    CASE("render_server_multi_surface_independent_last_chunks");
    {
        A840EarlyPromiseSched sched{};
        PromiseRecorder rec;
        A840EarlyPromiseConfig cfg{};
        cfg.client_id = 10u;
        cfg.client_role = A840_EARLY_ROLE_RENDER_SERVER;
        cfg.callback = &PromiseRecorder::on_event;
        cfg.callback_ctx = &rec;

        CHECK(a840_early_promise_init(&sched, &cfg) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_begin_frame(&sched, 1u, 103u, 0u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 8020u, 1u, 0u, true) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 8021u, 2u, 0u, true) == A840_PROMISE_SCHED_OK);

        uint64_t both[2] = {8020u, 8021u};
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 103u, 20u, 0u, both, 2u, false) == A840_PROMISE_SCHED_OK);
        CHECK(rec.events.size() == 1u);
        CHECK(rec.events[0].surface_id == 8020u);
        CHECK(rec.events[0].chunk_seqno == 20u);

        uint64_t s2 = 8021u;
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 103u, 21u, 0u, &s2, 1u, true) == A840_PROMISE_SCHED_OK);
        CHECK(rec.events.size() == 2u);
        CHECK(rec.events[1].surface_id == 8021u);
        CHECK(rec.events[1].chunk_seqno == 21u);
        a840_early_promise_destroy(&sched);
    }

    CASE("render_server_32_surfaces_max_capacity_and_33rd_rejected");
    {
        A840EarlyPromiseSched sched{};
        PromiseRecorder rec;
        A840EarlyPromiseConfig cfg{};
        cfg.client_id = 10u;
        cfg.client_role = A840_EARLY_ROLE_RENDER_SERVER;
        cfg.callback = &PromiseRecorder::on_event;
        cfg.callback_ctx = &rec;

        CHECK(a840_early_promise_init(&sched, &cfg) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_begin_frame(&sched, 1u, 104u, 0u) == A840_PROMISE_SCHED_OK);

        uint64_t sids[32];
        for (uint32_t i = 0u; i < 32u; ++i) {
            sids[i] = 8100u + i;
            CHECK(a840_early_promise_plan_surface(&sched, sids[i], 1u, 0u, true) == A840_PROMISE_SCHED_OK);
        }
        CHECK(a840_early_promise_plan_surface(&sched, 8999u, 1u, 0u, true) ==
              A840_PROMISE_SCHED_ERR_CAPACITY);

        CHECK(a840_early_promise_on_chunk_kicked(&sched, 104u, 30u, 0u, sids, 32u, true) ==
              A840_PROMISE_SCHED_OK);
        CHECK(rec.events.size() == 32u);
        a840_early_promise_destroy(&sched);
    }

    CASE("app_role_forced_defer_until_retired_on_early_request_downgrade");
    {
        A840EarlyPromiseSched sched{};
        PromiseRecorder rec;
        A840EarlyPromiseConfig cfg{};
        cfg.client_id = 20u;
        cfg.client_role = A840_EARLY_ROLE_APP;
        cfg.strict_reject_app_early = false;
        cfg.callback = &PromiseRecorder::on_event;
        cfg.callback_ctx = &rec;

        CHECK(a840_early_promise_init(&sched, &cfg) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_begin_frame(&sched, 1u, 200u, 2u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 9001u, 1u, 0u, true) == A840_PROMISE_SCHED_OK);

        uint64_t sid = 9001u;
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 200u, 50u, 2u, &sid, 1u, true) ==
              A840_PROMISE_SCHED_OK);

        CHECK(rec.events.size() == 1u);
        CHECK(rec.events[0].kind == A840_PROMISE_EVENT_DEFERRED_ARMED);
        CHECK(rec.events[0].policy == A840_EARLY_POLICY_DEFER_UNTIL_RETIRED);
        CHECK(rec.events[0].display_ready == 0u);
        CHECK(a840_early_promise_get_stats(&sched).app_early_downgraded_count == 1u);
        CHECK(a840_early_promise_get_stats(&sched).early_scheduled_count == 0u);

        CHECK(a840_early_promise_on_chunk_retired(&sched, 2u, 49u) == 0u);
        CHECK(a840_early_promise_on_chunk_retired(&sched, 2u, 50u) == 1u);
        CHECK(rec.events.size() == 2u);
        CHECK(rec.events[1].kind == A840_PROMISE_EVENT_RETIRED_READY);
        CHECK(rec.events[1].display_ready == 1u);
        a840_early_promise_destroy(&sched);
    }

    CASE("app_role_strict_reject_early_request_permission_denied");
    {
        A840EarlyPromiseSched sched{};
        A840EarlyPromiseConfig cfg{};
        cfg.client_id = 21u;
        cfg.client_role = A840_EARLY_ROLE_APP;
        cfg.strict_reject_app_early = true;

        CHECK(a840_early_promise_init(&sched, &cfg) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_begin_frame(&sched, 1u, 201u, 2u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 9002u, 1u, 0u, true) ==
              A840_PROMISE_SCHED_ERR_PERMISSION_DENIED);
        CHECK(a840_early_promise_get_stats(&sched).app_early_rejected_count == 1u);
        CHECK(a840_early_promise_get_stats(&sched).tracked_frame_surfaces == 0u);
        CHECK(a840_early_promise_plan_surface(&sched, 9002u, 1u, 0u, false) == A840_PROMISE_SCHED_OK);
        a840_early_promise_destroy(&sched);
    }

    CASE("app_role_normal_deferred_promise_retire_ordering");
    {
        A840EarlyPromiseSched sched{};
        PromiseRecorder rec;
        A840EarlyPromiseConfig cfg{};
        cfg.client_id = 22u;
        cfg.client_role = A840_EARLY_ROLE_APP;
        cfg.callback = &PromiseRecorder::on_event;
        cfg.callback_ctx = &rec;

        CHECK(a840_early_promise_init(&sched, &cfg) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_begin_frame(&sched, 1u, 202u, 2u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 9010u, 1u, 0u, false) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 9011u, 1u, 0u, false) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 9012u, 1u, 0u, false) == A840_PROMISE_SCHED_OK);

        uint64_t a = 9010u, b = 9011u, c = 9012u;
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 202u, 5u, 2u, &a, 1u, false) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 202u, 8u, 2u, &b, 1u, false) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 202u, 12u, 2u, &c, 1u, true) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_get_stats(&sched).active_pending_count == 3u);

        CHECK(a840_early_promise_on_chunk_retired(&sched, 2u, 8u) == 2u);
        CHECK(a840_early_promise_get_stats(&sched).active_pending_count == 1u);
        CHECK(a840_early_promise_on_chunk_retired(&sched, 2u, 12u) == 1u);
        CHECK(a840_early_promise_get_stats(&sched).active_pending_count == 0u);
        a840_early_promise_destroy(&sched);
    }

    CASE("same_chunk_duplicate_surface_writes_deduplicated");
    {
        A840EarlyPromiseSched sched{};
        PromiseRecorder rec;
        A840EarlyPromiseConfig cfg{};
        cfg.client_id = 10u;
        cfg.client_role = A840_EARLY_ROLE_RENDER_SERVER;
        cfg.callback = &PromiseRecorder::on_event;
        cfg.callback_ctx = &rec;

        CHECK(a840_early_promise_init(&sched, &cfg) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_begin_frame(&sched, 1u, 105u, 0u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 8050u, 2u, 0u, true) == A840_PROMISE_SCHED_OK);

        uint64_t dup[2] = {8050u, 8050u};
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 105u, 1u, 0u, dup, 2u, false) == A840_PROMISE_SCHED_OK);
        CHECK(rec.events.empty());
        CHECK(a840_early_promise_get_stats(&sched).duplicate_writes_deduped == 1u);

        CHECK(a840_early_promise_on_chunk_kicked(&sched, 105u, 2u, 0u, &dup[0], 1u, true) == A840_PROMISE_SCHED_OK);
        CHECK(rec.events.size() == 1u);
        CHECK(rec.events[0].chunk_seqno == 2u);
        a840_early_promise_destroy(&sched);
    }

    CASE("monotonic_generation_auto_and_explicit_enforcement");
    {
        A840EarlyPromiseSched sched{};
        PromiseRecorder rec;
        A840EarlyPromiseConfig cfg{};
        cfg.client_id = 10u;
        cfg.client_role = A840_EARLY_ROLE_RENDER_SERVER;
        cfg.callback = &PromiseRecorder::on_event;
        cfg.callback_ctx = &rec;

        CHECK(a840_early_promise_init(&sched, &cfg) == A840_PROMISE_SCHED_OK);
        uint64_t sid = 8060u;

        CHECK(a840_early_promise_begin_frame(&sched, 1u, 110u, 0u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, sid, 1u, 0u, true) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 110u, 1u, 0u, &sid, 1u, true) == A840_PROMISE_SCHED_OK);
        CHECK(rec.events.back().generation == 1u);

        CHECK(a840_early_promise_begin_frame(&sched, 2u, 111u, 0u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, sid, 1u, 10u, true) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 111u, 2u, 0u, &sid, 1u, true) == A840_PROMISE_SCHED_OK);
        CHECK(rec.events.back().generation == 10u);

        CHECK(a840_early_promise_begin_frame(&sched, 3u, 112u, 0u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, sid, 1u, 10u, true) == A840_PROMISE_SCHED_ERR_NON_MONOTONIC_GEN);
        CHECK(a840_early_promise_plan_surface(&sched, sid, 1u, 9u, true) == A840_PROMISE_SCHED_ERR_NON_MONOTONIC_GEN);
        CHECK(a840_early_promise_get_stats(&sched).non_monotonic_gen_rejected == 2u);
        a840_early_promise_destroy(&sched);
    }

    CASE("cancel_cb_broadcasts_promise_canceled_for_active_and_unpub_surfaces");
    {
        A840EarlyPromiseSched sched{};
        PromiseRecorder rec;
        A840EarlyPromiseConfig cfg{};
        cfg.client_id = 10u;
        cfg.client_role = A840_EARLY_ROLE_RENDER_SERVER;
        cfg.callback = &PromiseRecorder::on_event;
        cfg.callback_ctx = &rec;

        CHECK(a840_early_promise_init(&sched, &cfg) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_begin_frame(&sched, 1u, 120u, 0u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 8070u, 1u, 0u, true) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 8071u, 2u, 0u, true) == A840_PROMISE_SCHED_OK);

        uint64_t both[2] = {8070u, 8071u};
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 120u, 1u, 0u, both, 2u, false) == A840_PROMISE_SCHED_OK);
        CHECK(rec.events.size() == 1u);

        size_t canceled = a840_early_promise_cancel_cb(&sched, 120u, A840_EARLY_CANCEL_CHUNK_SUBMIT_FAILED);
        CHECK(canceled == 2u);
        CHECK(rec.events.size() == 3u);
        CHECK(rec.events[1].kind == A840_PROMISE_EVENT_PROMISE_CANCELED);
        CHECK(rec.events[1].cancel_reason == A840_EARLY_CANCEL_CHUNK_SUBMIT_FAILED);
        CHECK(rec.events[2].kind == A840_PROMISE_EVENT_PROMISE_CANCELED);

        A840EarlyPromiseSurfaceSnapshot snap{};
        CHECK(a840_early_promise_query_surface(&sched, 8070u, &snap) == A840_PROMISE_SCHED_OK);
        CHECK(snap.last_event == A840_PROMISE_EVENT_PROMISE_CANCELED);
        CHECK(snap.display_ready == 0u);
        a840_early_promise_destroy(&sched);
    }

    CASE("cancel_ring_poisons_channel_and_reset_recovers");
    {
        A840EarlyPromiseSched sched{};
        PromiseRecorder rec;
        A840EarlyPromiseConfig cfg{};
        cfg.client_id = 10u;
        cfg.client_role = A840_EARLY_ROLE_RENDER_SERVER;
        cfg.callback = &PromiseRecorder::on_event;
        cfg.callback_ctx = &rec;

        CHECK(a840_early_promise_init(&sched, &cfg) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_begin_frame(&sched, 1u, 121u, 1u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 8080u, 1u, 0u, true) == A840_PROMISE_SCHED_OK);

        CHECK(a840_early_promise_cancel_ring(&sched, 1u, A840_EARLY_CANCEL_GPU_FAULT, true) == 1u);
        CHECK(a840_early_promise_begin_frame(&sched, 2u, 122u, 1u) ==
              A840_PROMISE_SCHED_ERR_CHANNEL_POISONED);
        CHECK(a840_early_promise_reset_ring(&sched, 1u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_begin_frame(&sched, 2u, 122u, 1u) == A840_PROMISE_SCHED_OK);
        a840_early_promise_destroy(&sched);
    }

    CASE("callback_strictly_outside_mutex_reentrant_query_and_stats");
    {
        A840EarlyPromiseSched sched{};
        PromiseRecorder rec;
        rec.reentrant_sched = &sched;
        rec.do_reentrant_query = true;
        rec.do_reentrant_stats = true;

        A840EarlyPromiseConfig cfg{};
        cfg.client_id = 10u;
        cfg.client_role = A840_EARLY_ROLE_RENDER_SERVER;
        cfg.callback = &PromiseRecorder::on_event;
        cfg.callback_ctx = &rec;

        CHECK(a840_early_promise_init(&sched, &cfg) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_begin_frame(&sched, 1u, 123u, 0u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 8090u, 1u, 0u, true) == A840_PROMISE_SCHED_OK);
        uint64_t sid = 8090u;
        CHECK(a840_early_promise_on_chunk_kicked(&sched, 123u, 1u, 0u, &sid, 1u, true) == A840_PROMISE_SCHED_OK);
        CHECK(rec.reentrant_queries_ok == 1u);
        CHECK(a840_early_promise_on_chunk_retired(&sched, 0u, 1u) == 1u);
        CHECK(rec.reentrant_queries_ok == 2u);
        a840_early_promise_destroy(&sched);
    }
}
void test_group5_e2e_integration()
{
    CASE("e2e_renderserver_chunker_early_kick_to_early_promise_pipeline");
    {
        ChunkerEnv env;
        A840EarlyPromiseSched sched{};
        PromiseRecorder rec;
        A840EarlyPromiseConfig pcfg{};
        pcfg.client_id = 1u;
        pcfg.client_role = A840_EARLY_ROLE_RENDER_SERVER;
        pcfg.callback = &PromiseRecorder::on_event;
        pcfg.callback_ctx = &rec;
        CHECK(a840_early_promise_init(&sched, &pcfg) == A840_PROMISE_SCHED_OK);

        A840ChunkerConfig ccfg{};
        a840_cb_chunker_default_config(&ccfg);
        ccfg.max_passes_per_chunk = 2u;
        ccfg.ring_id = 0u;
        env.init(3u, 65536u, 16u, 512u, &ccfg);
        env.sink.linked_sched = &sched;

        CHECK(a840_early_promise_begin_frame(&sched, 1u, 500u, 0u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 6001u, 1u, 0u, true) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 6002u, 2u, 0u, true) == A840_PROMISE_SCHED_OK);
        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 500u) == A840_CHUNK_OK);

        uint64_t s1 = 6001u, s2 = 6002u;
        A840ChunkPassDesc p{};
        p.kind = A840_CHUNK_PASS_RENDER_GMEM;
        p.produce_dirty_mask = A840_SYNC_DIRTY_COLOR_CCU;
        p.est_gpu_us = 500u;
        p.est_cpu_us = 150u;

        p.written_surface_ids = &s1;
        p.written_surface_count = 1u;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 16u, 1u) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);

        p.written_surface_ids = &s2;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 16u, 2u) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);

        CHECK(env.sink.chunks.size() == 1u);
        CHECK(rec.events.size() == 1u);
        CHECK(rec.events[0].surface_id == 6001u);
        CHECK(rec.events[0].kind == A840_PROMISE_EVENT_EARLY_SCHEDULED);

        p.is_final_pass = true;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_emit_pass_nop(&env.chunker, 16u, 3u) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);

        CHECK(env.sink.chunks.size() == 2u);
        CHECK(rec.events.size() == 2u);
        CHECK(rec.events[1].surface_id == 6002u);
        CHECK(rec.events[1].kind == A840_PROMISE_EVENT_EARLY_SCHEDULED);

        uint32_t retired_slots = 0u;
        uint64_t last_seq = env.sink.chunks.back().chunk_seqno;
        CHECK(a840_cmd_slot_retire_fence_seqno(&env.pool, last_seq, &retired_slots) == A840_ARENA_OK);
        CHECK(retired_slots == 2u);
        CHECK(a840_early_promise_on_chunk_retired(&sched, 0u, last_seq) == 2u);
        CHECK(a840_early_promise_get_stats(&sched).active_pending_count == 0u);
        a840_early_promise_destroy(&sched);
    }

    CASE("e2e_app_chunker_early_kick_still_defers_promise_until_gpu_retire");
    {
        ChunkerEnv env;
        A840EarlyPromiseSched sched{};
        PromiseRecorder rec;
        A840EarlyPromiseConfig pcfg{};
        pcfg.client_id = 2u;
        pcfg.client_role = A840_EARLY_ROLE_APP;
        pcfg.strict_reject_app_early = false;
        pcfg.callback = &PromiseRecorder::on_event;
        pcfg.callback_ctx = &rec;
        CHECK(a840_early_promise_init(&sched, &pcfg) == A840_PROMISE_SCHED_OK);

        A840ChunkerConfig ccfg{};
        a840_cb_chunker_default_config(&ccfg);
        ccfg.max_passes_per_chunk = 1u;
        ccfg.ring_id = 2u;
        env.init(3u, 65536u, 16u, 512u, &ccfg);
        env.sink.linked_sched = &sched;

        CHECK(a840_early_promise_begin_frame(&sched, 1u, 501u, 2u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 6100u, 1u, 0u, true) == A840_PROMISE_SCHED_OK);
        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 501u) == A840_CHUNK_OK);

        uint64_t s = 6100u;
        A840ChunkPassDesc p{};
        p.kind = A840_CHUNK_PASS_RENDER_GMEM;
        p.written_surface_ids = &s;
        p.written_surface_count = 1u;
        p.is_final_pass = true;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_finish(&env.chunker) == A840_CHUNK_OK);

        CHECK(rec.events.size() == 1u);
        CHECK(rec.events[0].kind == A840_PROMISE_EVENT_DEFERRED_ARMED);
        CHECK(rec.events[0].display_ready == 0u);

        CHECK(a840_early_promise_on_chunk_retired(&sched, 2u, env.sink.chunks[0].chunk_seqno) == 1u);
        CHECK(rec.events.size() == 2u);
        CHECK(rec.events[1].kind == A840_PROMISE_EVENT_RETIRED_READY);
        CHECK(rec.events[1].display_ready == 1u);
        a840_early_promise_destroy(&sched);
    }

    CASE("e2e_chunk_submit_fault_aborts_chunker_and_cancels_all_promises");
    {
        ChunkerEnv env;
        A840EarlyPromiseSched sched{};
        PromiseRecorder rec;
        A840EarlyPromiseConfig pcfg{};
        pcfg.client_id = 1u;
        pcfg.client_role = A840_EARLY_ROLE_RENDER_SERVER;
        pcfg.callback = &PromiseRecorder::on_event;
        pcfg.callback_ctx = &rec;
        CHECK(a840_early_promise_init(&sched, &pcfg) == A840_PROMISE_SCHED_OK);

        A840ChunkerConfig ccfg{};
        a840_cb_chunker_default_config(&ccfg);
        ccfg.max_passes_per_chunk = 1u;
        env.init(3u, 65536u, 16u, 512u, &ccfg);
        env.sink.linked_sched = &sched;
        env.sink.fail_on_chunk_index = 1;
        env.sink.cancel_sched_on_fail = true;

        CHECK(a840_early_promise_begin_frame(&sched, 1u, 502u, 0u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 6201u, 1u, 0u, true) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched, 6202u, 1u, 0u, true) == A840_PROMISE_SCHED_OK);
        CHECK(a840_cb_chunker_begin_cb(&env.chunker, 1u, 502u) == A840_CHUNK_OK);

        uint64_t s1 = 6201u, s2 = 6202u;
        A840ChunkPassDesc p{};
        p.kind = A840_CHUNK_PASS_RENDER_GMEM;
        p.written_surface_ids = &s1;
        p.written_surface_count = 1u;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_OK);

        p.written_surface_ids = &s2;
        CHECK(a840_cb_chunker_begin_pass(&env.chunker, &p) == A840_CHUNK_OK);
        CHECK(a840_cb_chunker_end_pass(&env.chunker) == A840_CHUNK_ERR_KICK_FAILED);
        CHECK(a840_cb_chunker_is_aborted(&env.chunker));

        size_t canceled_ev = 0u;
        for (const auto &ev : rec.events) {
            if (ev.kind == A840_PROMISE_EVENT_PROMISE_CANCELED) {
                canceled_ev++;
            }
        }
        CHECK(canceled_ev == 2u);
        a840_early_promise_destroy(&sched);
    }

    CASE("e2e_triple_buffered_frame_arena_recycle_across_10_frames");
    {
        CountingAllocator alloc;
        A840ArenaAllocator a = alloc.iface();
        A840FrameArena arena{};
        A840CmdSlotPool pool{};
        A840CbChunker chunker{};
        MockKickSink sink;

        CHECK(a840_frame_arena_init(&arena, 3u, 32768u, &a) == A840_ARENA_OK);
        CHECK(a840_cmd_slot_pool_init(&pool, 16u, 512u, &a) == A840_ARENA_OK);
        A840ChunkerConfig cfg{};
        a840_cb_chunker_default_config(&cfg);
        cfg.max_passes_per_chunk = 2u;
        cfg.kick_fn = &MockKickSink::on_kick;
        cfg.kick_ctx = &sink;
        CHECK(a840_cb_chunker_init(&chunker, &arena, &pool, &cfg) == A840_CHUNK_OK);

        uint64_t frame_last_seq[11] = {};
        for (uint64_t f = 1u; f <= 10u; ++f) {
            if (f > 3u) {
                uint32_t ret = 0u;
                CHECK(a840_cmd_slot_retire_fence_seqno(&pool, frame_last_seq[f - 3u], &ret) == A840_ARENA_OK);
                CHECK(a840_frame_arena_retire_frame(&arena, f - 3u) == A840_ARENA_OK);
            }
            uint32_t slot_idx = 0u;
            CHECK(a840_frame_arena_begin_frame(&arena, f, &slot_idx) == A840_ARENA_OK);
            CHECK(a840_cb_chunker_begin_cb(&chunker, f, 1000u + f) == A840_CHUNK_OK);
            for (int p_idx = 0; p_idx < 4; ++p_idx) {
                A840ChunkPassDesc p{};
                p.kind = A840_CHUNK_PASS_RENDER_GMEM;
                p.produce_dirty_mask = A840_SYNC_DIRTY_COLOR_CCU;
                p.is_final_pass = (p_idx == 3);
                CHECK(a840_cb_chunker_begin_pass(&chunker, &p) == A840_CHUNK_OK);
                CHECK(a840_cb_chunker_emit_pass_nop(&chunker, 12u, static_cast<uint32_t>(p_idx)) == A840_CHUNK_OK);
                CHECK(a840_cb_chunker_end_pass(&chunker) == A840_CHUNK_OK);
            }
            CHECK(a840_cb_chunker_finish(&chunker) == A840_CHUNK_OK);
            frame_last_seq[f] = sink.chunks.back().chunk_seqno;
        }
        uint32_t ret = 0u;
        CHECK(a840_cmd_slot_retire_fence_seqno(&pool, frame_last_seq[10], &ret) == A840_ARENA_OK);
        CHECK(a840_cmd_slot_pool_stats(&pool).in_use_current == 0u);
        a840_cmd_slot_pool_destroy(&pool);
        a840_frame_arena_destroy(&arena);
    }

    CASE("e2e_multi_ring_isolation_compositor_vs_client_rings");
    {
        A840EarlyPromiseSched sched_rs{}, sched_app{};
        PromiseRecorder rec_rs, rec_app;

        A840EarlyPromiseConfig c_rs{};
        c_rs.client_id = 1u;
        c_rs.client_role = A840_EARLY_ROLE_RENDER_SERVER;
        c_rs.callback = &PromiseRecorder::on_event;
        c_rs.callback_ctx = &rec_rs;
        CHECK(a840_early_promise_init(&sched_rs, &c_rs) == A840_PROMISE_SCHED_OK);

        A840EarlyPromiseConfig c_app{};
        c_app.client_id = 2u;
        c_app.client_role = A840_EARLY_ROLE_APP;
        c_app.callback = &PromiseRecorder::on_event;
        c_app.callback_ctx = &rec_app;
        CHECK(a840_early_promise_init(&sched_app, &c_app) == A840_PROMISE_SCHED_OK);

        CHECK(a840_early_promise_begin_frame(&sched_rs, 1u, 600u, 0u) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_begin_frame(&sched_app, 1u, 601u, 2u) == A840_PROMISE_SCHED_OK);
        uint64_t s_rs = 7101u, s_app = 7102u;
        CHECK(a840_early_promise_plan_surface(&sched_rs, s_rs, 1u, 0u, true) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_plan_surface(&sched_app, s_app, 1u, 0u, false) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_on_chunk_kicked(&sched_rs, 600u, 1u, 0u, &s_rs, 1u, true) == A840_PROMISE_SCHED_OK);
        CHECK(a840_early_promise_on_chunk_kicked(&sched_app, 601u, 1u, 2u, &s_app, 1u, true) == A840_PROMISE_SCHED_OK);

        CHECK(a840_early_promise_cancel_ring(&sched_app, 2u, A840_EARLY_CANCEL_GPU_FAULT, true) == 1u);
        CHECK(a840_early_promise_cancel_ring(&sched_rs, 2u, A840_EARLY_CANCEL_GPU_FAULT, false) == 0u);
        A840EarlyPromiseSurfaceSnapshot snap{};
        CHECK(a840_early_promise_query_surface(&sched_rs, s_rs, &snap) == A840_PROMISE_SCHED_OK);
        CHECK(snap.display_ready == 1u);
        a840_early_promise_destroy(&sched_app);
        a840_early_promise_destroy(&sched_rs);
    }
}

struct BenchKickCtx {
    A840EarlyPromiseSched *sched{nullptr};
    uint64_t last_seqno{0u};
    uint64_t total_kicks{0u};

    static A840ChunkStatus on_kick(void *ctx, const A840ChunkDesc *desc)
    {
        auto *self = static_cast<BenchKickCtx *>(ctx);
        self->last_seqno = desc->chunk_seqno;
        self->total_kicks++;
        bool is_final = (desc->flags & A840_CHUNK_FLAG_FINAL) != 0u;
        A840EarlyPromiseStatus pst = a840_early_promise_on_chunk_kicked(
            self->sched,
            desc->cb_id,
            desc->chunk_seqno,
            desc->ring_id,
            desc->written_surface_count > 0u ? desc->written_surface_ids : nullptr,
            desc->written_surface_count,
            is_final);
        return (pst == A840_PROMISE_SCHED_OK) ? A840_CHUNK_OK : A840_CHUNK_ERR_KICK_FAILED;
    }
};

struct BenchPromiseCounter {
    uint64_t early_events{0u};
    uint64_t retired_events{0u};

    static void on_event(void *ctx, const A840EarlyPromiseEvent *ev)
    {
        auto *self = static_cast<BenchPromiseCounter *>(ctx);
        if (ev->kind == A840_PROMISE_EVENT_EARLY_SCHEDULED) {
            self->early_events++;
        } else if (ev->kind == A840_PROMISE_EVENT_RETIRED_READY) {
            self->retired_events++;
        }
    }
};

void test_group6_bench_and_tsan()
{
    CountingAllocator alloc;
    A840ArenaAllocator a = alloc.iface();
    A840FrameArena arena{};
    A840CmdSlotPool pool{};
    A840CbChunker chunker{};
    A840EarlyPromiseSched sched{};
    BenchPromiseCounter pcounter{};
    BenchKickCtx kick_ctx{};
    kick_ctx.sched = &sched;

    CHECK(a840_frame_arena_init(&arena, 3u, 65536u, &a) == A840_ARENA_OK);
    CHECK(a840_cmd_slot_pool_init(&pool, 24u, 512u, &a) == A840_ARENA_OK);

    A840EarlyPromiseConfig pcfg{};
    pcfg.client_id = 1u;
    pcfg.client_role = A840_EARLY_ROLE_RENDER_SERVER;
    pcfg.callback = &BenchPromiseCounter::on_event;
    pcfg.callback_ctx = &pcounter;
    CHECK(a840_early_promise_init(&sched, &pcfg) == A840_PROMISE_SCHED_OK);

    A840ChunkerConfig ccfg{};
    a840_cb_chunker_default_config(&ccfg);
    ccfg.max_passes_per_chunk = 2u;
    ccfg.max_dwords_per_chunk = 512u;
    ccfg.max_est_gpu_us_per_chunk = 1500u;
    ccfg.ring_id = 0u;
    ccfg.marker_bo_handle = 999u;
    ccfg.marker_gpu_va = UINT64_C(0x90000000);
    ccfg.kick_fn = &BenchKickCtx::on_kick;
    ccfg.kick_ctx = &kick_ctx;
    CHECK(a840_cb_chunker_init(&chunker, &arena, &pool, &ccfg) == A840_CHUNK_OK);

    CASE("bench_1000_frames_120hz_zero_heap_alloc_and_sub_500us_single_cb");
    {
        constexpr uint64_t kFrames = 1000u;
        std::vector<double> cb_us(kFrames, 0.0);
        uint64_t frame_last_seq[kFrames + 1u] = {};

        const uint64_t allocs_before = alloc.alloc_count.load(std::memory_order_relaxed);
        const uint64_t frees_before = alloc.free_count.load(std::memory_order_relaxed);

        const uint64_t surfs[3] = {9501u, 9502u, 9503u};
        const auto t_start = std::chrono::steady_clock::now();

        for (uint64_t f = 1u; f <= kFrames; ++f) {
            if (f > 3u) {
                uint64_t old_f = f - 3u;
                uint32_t retired = 0u;
                CHECK(a840_cmd_slot_retire_fence_seqno(&pool, frame_last_seq[old_f], &retired) == A840_ARENA_OK);
                (void)a840_early_promise_on_chunk_retired(&sched, 0u, frame_last_seq[old_f]);
                CHECK(a840_frame_arena_retire_frame(&arena, old_f) == A840_ARENA_OK);
            }
            uint32_t slot_idx = 0u;
            CHECK(a840_frame_arena_begin_frame(&arena, f, &slot_idx) == A840_ARENA_OK);
            CHECK(a840_early_promise_begin_frame(&sched, f, 10000u + f, 0u) == A840_PROMISE_SCHED_OK);
            CHECK(a840_early_promise_plan_surface(&sched, surfs[0], 1u, 0u, true) == A840_PROMISE_SCHED_OK);
            CHECK(a840_early_promise_plan_surface(&sched, surfs[1], 1u, 0u, true) == A840_PROMISE_SCHED_OK);
            CHECK(a840_early_promise_plan_surface(&sched, surfs[2], 1u, 0u, true) == A840_PROMISE_SCHED_OK);

            CHECK(a840_cb_chunker_begin_cb(&chunker, f, 10000u + f) == A840_CHUNK_OK);
            for (int p_idx = 0; p_idx < 6; ++p_idx) {
                A840ChunkPassDesc p{};
                uint64_t w_id = surfs[p_idx / 2];
                uint64_t r_id = (p_idx > 0) ? surfs[(p_idx - 1) / 2] : 0u;
                p.kind = (p_idx % 2 == 0) ? A840_CHUNK_PASS_RENDER_GMEM : A840_CHUNK_PASS_COMPUTE;
                p.sync_access.write_ids = &w_id;
                p.sync_access.write_count = 1u;
                if (p_idx > 0) {
                    p.sync_access.read_ids = &r_id;
                    p.sync_access.read_count = 1u;
                    p.sync_access.consumer_mask = A840_SYNC_CONSUME_UCHE_TEX;
                }
                p.produce_dirty_mask = (p_idx % 2 == 0)
                                           ? (A840_SYNC_DIRTY_COLOR_CCU | A840_SYNC_DIRTY_GMEM_RESOLVE)
                                           : A840_SYNC_DIRTY_UCHE_SHADER_WR;
                p.est_gpu_us = 550u;
                p.est_cpu_us = 120u;
                if (p_idx % 2 == 1) {
                    p.written_surface_ids = &w_id;
                    p.written_surface_count = 1u;
                }
                p.is_final_pass = (p_idx == 5);
                CHECK(a840_cb_chunker_begin_pass(&chunker, &p) == A840_CHUNK_OK);
                CHECK(a840_cb_chunker_emit_pass_nop(&chunker, 16u, static_cast<uint32_t>(p_idx)) == A840_CHUNK_OK);
                CHECK(a840_cb_chunker_end_pass(&chunker) == A840_CHUNK_OK);
            }
            CHECK(a840_cb_chunker_finish(&chunker) == A840_CHUNK_OK);
            frame_last_seq[f] = kick_ctx.last_seqno;
            cb_us[f - 1u] = a840_cb_chunker_stats(&chunker).single_cb_encode_us;
        }

        const auto t_end = std::chrono::steady_clock::now();
        const uint64_t steady_allocs = alloc.alloc_count.load(std::memory_order_relaxed) - allocs_before;
        const uint64_t steady_frees = alloc.free_count.load(std::memory_order_relaxed) - frees_before;

        CHECK(steady_allocs == 0u);
        CHECK(steady_frees == 0u);

        double total_us = std::chrono::duration<double, std::micro>(t_end - t_start).count();
        double avg_cb_us = total_us / static_cast<double>(kFrames);
        std::sort(cb_us.begin(), cb_us.end());
        double p50_us = cb_us[kFrames / 2u];
        double p99_us = cb_us[(kFrames * 99u) / 100u];

        A840ChunkerStats cst = a840_cb_chunker_stats(&chunker);
        std::printf("  [120Hz 1000-frame chunker+promise bench] heap_allocs=%llu heap_frees=%llu "
                    "avg=%.2f us/CB p50=%.2f us p99=%.2f us (budget <500 us) "
                    "chunks=%llu early_kicks=%llu early_promises=%llu overlap_gain=%llu us\n",
                    static_cast<unsigned long long>(steady_allocs),
                    static_cast<unsigned long long>(steady_frees),
                    avg_cb_us,
                    p50_us,
                    p99_us,
                    static_cast<unsigned long long>(cst.chunks_kicked),
                    static_cast<unsigned long long>(cst.early_kicks),
                    static_cast<unsigned long long>(pcounter.early_events),
                    static_cast<unsigned long long>(cst.gpu_overlap_gain_us));

        CHECK(avg_cb_us < 500.0);
        CHECK(p99_us < 500.0);
        CHECK(cst.chunks_kicked == 3000u);
        CHECK(cst.early_kicks == 2000u);
        CHECK(pcounter.early_events == 3000u);

        for (uint64_t old_f = kFrames - 2u; old_f <= kFrames; ++old_f) {
            uint32_t retired = 0u;
            CHECK(a840_cmd_slot_retire_fence_seqno(&pool, frame_last_seq[old_f], &retired) == A840_ARENA_OK);
            (void)a840_early_promise_on_chunk_retired(&sched, 0u, frame_last_seq[old_f]);
            CHECK(a840_frame_arena_retire_frame(&arena, old_f) == A840_ARENA_OK);
        }
    }

    CASE("bench_verify_zero_dangling_slots_and_zero_pending_promises_after_1000_frames");
    {
        CHECK(a840_cmd_slot_pool_stats(&pool).in_use_current == 0u);
        CHECK(a840_early_promise_get_stats(&sched).active_pending_count == 0u);
        CHECK(pcounter.retired_events == 3000u);
    }

    a840_early_promise_destroy(&sched);
    a840_cmd_slot_pool_destroy(&pool);
    a840_frame_arena_destroy(&arena);

    CASE("tsan_8thread_concurrent_chunker_and_slot_pool_stress");
    {
        constexpr unsigned kThreads = 8u;
        A840CmdSlotPool shared_pool{};
        CHECK(a840_cmd_slot_pool_init(&shared_pool, 128u, 256u, nullptr) == A840_ARENA_OK);

        std::atomic<uint64_t> total_kicked{0u};
        std::vector<std::thread> workers;
        workers.reserve(kThreads);

        for (unsigned tid = 0u; tid < kThreads; ++tid) {
            workers.emplace_back([&, tid]() {
                A840FrameArena local_arena{};
                CHECK(a840_frame_arena_init(&local_arena, 2u, 16384u, nullptr) == A840_ARENA_OK);

                struct LocalSink {
                    A840CmdSlotPool *pool;
                    std::atomic<uint64_t> *kicked;
                    static A840ChunkStatus kick(void *ctx, const A840ChunkDesc *desc)
                    {
                        auto *s = static_cast<LocalSink *>(ctx);
                        s->kicked->fetch_add(1u, std::memory_order_relaxed);
                        CHECK(a840_cmd_slot_release(s->pool, desc->slot_handle) == A840_ARENA_OK);
                        return A840_CHUNK_OK;
                    }
                } ls{&shared_pool, &total_kicked};

                A840CbChunker ch{};
                A840ChunkerConfig cfg{};
                a840_cb_chunker_default_config(&cfg);
                cfg.max_passes_per_chunk = 2u;
                cfg.ring_id = tid % 4u;
                cfg.kick_fn = &LocalSink::kick;
                cfg.kick_ctx = &ls;
                CHECK(a840_cb_chunker_init(&ch, &local_arena, &shared_pool, &cfg) == A840_CHUNK_OK);

                const unsigned iters = 10u * g_tsan_rounds;
                for (unsigned it = 0u; it < iters; ++it) {
                    uint64_t fid = static_cast<uint64_t>(tid) * 100000u + it + 1u;
                    uint32_t sidx = 0u;
                    CHECK(a840_frame_arena_begin_frame(&local_arena, fid, &sidx) == A840_ARENA_OK);
                    CHECK(a840_cb_chunker_begin_cb(&ch, fid, fid) == A840_CHUNK_OK);
                    for (int p_idx = 0; p_idx < 4; ++p_idx) {
                        A840ChunkPassDesc p{};
                        p.kind = (p_idx % 2 == 0) ? A840_CHUNK_PASS_RENDER_GMEM : A840_CHUNK_PASS_COMPUTE;
                        p.produce_dirty_mask = A840_SYNC_DIRTY_COLOR_CCU;
                        p.is_final_pass = (p_idx == 3);
                        CHECK(a840_cb_chunker_begin_pass(&ch, &p) == A840_CHUNK_OK);
                        CHECK(a840_cb_chunker_emit_pass_nop(&ch, 8u, static_cast<uint32_t>(p_idx)) == A840_CHUNK_OK);
                        CHECK(a840_cb_chunker_end_pass(&ch) == A840_CHUNK_OK);
                    }
                    CHECK(a840_cb_chunker_finish(&ch) == A840_CHUNK_OK);
                    CHECK(a840_frame_arena_retire_frame(&local_arena, fid) == A840_ARENA_OK);
                }
                a840_frame_arena_destroy(&local_arena);
            });
        }
        for (auto &w : workers) {
            w.join();
        }
        CHECK(total_kicked.load(std::memory_order_relaxed) ==
              static_cast<uint64_t>(kThreads) * 10u * g_tsan_rounds * 2u);
        CHECK(a840_cmd_slot_pool_stats(&shared_pool).in_use_current == 0u);
        a840_cmd_slot_pool_destroy(&shared_pool);
    }

    CASE("tsan_concurrent_callback_reentrancy_and_ring_cancel_stress");
    {
        struct AtomicPromiseCb {
            A840EarlyPromiseSched *sched;
            std::atomic<uint64_t> cb_count{0u};
            static void on_ev(void *ctx, const A840EarlyPromiseEvent *ev)
            {
                auto *self = static_cast<AtomicPromiseCb *>(ctx);
                self->cb_count.fetch_add(1u, std::memory_order_relaxed);
                A840EarlyPromiseSurfaceSnapshot snap{};
                (void)a840_early_promise_query_surface(self->sched, ev->surface_id, &snap);
                (void)a840_early_promise_get_stats(self->sched);
            }
        };

        A840EarlyPromiseSched sched{};
        AtomicPromiseCb acb{};
        acb.sched = &sched;

        A840EarlyPromiseConfig cfg{};
        cfg.client_id = 1u;
        cfg.client_role = A840_EARLY_ROLE_RENDER_SERVER;
        cfg.callback = &AtomicPromiseCb::on_ev;
        cfg.callback_ctx = &acb;
        CHECK(a840_early_promise_init(&sched, &cfg) == A840_PROMISE_SCHED_OK);

        std::atomic<bool> stop{false};
        std::atomic<uint64_t> seqno_gen{1u};

        std::thread producer([&]() {
            const unsigned frames = 25u * g_tsan_rounds;
            for (unsigned f = 1u; f <= frames; ++f) {
                uint64_t cb_id = f;
                uint64_t s1 = 7500u + (f % 8u);
                uint64_t s2 = 7510u + (f % 8u);
                if (a840_early_promise_begin_frame(&sched, f, cb_id, 0u) != A840_PROMISE_SCHED_OK) {
                    continue;
                }
                (void)a840_early_promise_plan_surface(&sched, s1, 1u, 0u, true);
                (void)a840_early_promise_plan_surface(&sched, s2, 1u, 0u, true);
                uint64_t sq1 = seqno_gen.fetch_add(1u, std::memory_order_relaxed);
                (void)a840_early_promise_on_chunk_kicked(&sched, cb_id, sq1, 0u, &s1, 1u, false);
                if (f % 7u == 0u) {
                    (void)a840_early_promise_cancel_cb(&sched, cb_id, A840_EARLY_CANCEL_GPU_FAULT);
                } else {
                    uint64_t sq2 = seqno_gen.fetch_add(1u, std::memory_order_relaxed);
                    (void)a840_early_promise_on_chunk_kicked(&sched, cb_id, sq2, 0u, &s2, 1u, true);
                }
            }
            stop.store(true, std::memory_order_release);
        });

        std::vector<std::thread> readers;
        for (int r = 0; r < 7; ++r) {
            readers.emplace_back([&, r]() {
                while (!stop.load(std::memory_order_acquire)) {
                    if (r < 3) {
                        uint64_t cur = seqno_gen.load(std::memory_order_relaxed);
                        if (cur > 2u) {
                            (void)a840_early_promise_on_chunk_retired(&sched, 0u, cur - 1u);
                        }
                    } else {
                        A840EarlyPromiseSurfaceSnapshot snap{};
                        (void)a840_early_promise_query_surface(&sched, 7500u + (r % 8), &snap);
                        (void)a840_early_promise_get_stats(&sched);
                    }
                }
            });
        }

        producer.join();
        for (auto &th : readers) {
            th.join();
        }
        (void)a840_early_promise_on_chunk_retired(&sched, 0u, seqno_gen.load() + 10u);
        CHECK(acb.cb_count.load(std::memory_order_relaxed) > 0u);
        a840_early_promise_destroy(&sched);
    }
}

} // namespace

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-pm4") == 0 && i + 1 < argc) {
            g_dump_pm4_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--tsan-stress") == 0 && i + 1 < argc) {
            unsigned v = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
            g_tsan_rounds = (v > 0u) ? v : 1u;
        }
    }

    test_group1_chunker_boundaries();
    test_group2_thresholds_and_sync();
    test_group3_liquid_glass_and_faults();
    test_group4_early_promise_scheduler();
    test_group5_e2e_integration();
    test_group6_bench_and_tsan();

    std::printf("PASS test_cb_chunking_and_early_promise: %u cases, %u checks (tsan_rounds=%u)\n",
                g_cases,
                g_checks.load(std::memory_order_relaxed),
                g_tsan_rounds);
    return 0;
}
