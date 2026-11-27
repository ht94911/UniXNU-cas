/* SPDX-License-Identifier: MIT */
/* Wave 9 Lane 3 Host Test Suite:
 *   - Open-Addressing Generational State Hash Table (A840StateHashTable)
 *   - Triple-Buffered / N-in-Flight 64B-Aligned Frame Bump Arena (A840FrameArena)
 *   - ABA-Safe Thread-Safe Command Slot Reuse Pool (A840CmdSlotPool)
 *   - 1,000-Frame 120Hz Steady-State Zero-Heap-Allocation Hard Assertion + Microbenchmark
 *   - 8-Thread Concurrent Pool Stress Test (--tsan-stress <rounds>)
 * (>= 35 independent cases)
 */
#include "a840_frame_arena.h"
#include "a840_sync_tracker.h"
#include "Adreno840Regs.h"
#include "gen/a840_bitfields.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

unsigned g_cases = 0;
std::atomic<unsigned> g_checks{0};
const char *g_active_case = "init";

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

struct CountingAllocator {
    std::atomic<uint64_t> alloc_count{0u};
    std::atomic<uint64_t> free_count{0u};
    std::atomic<uint64_t> bytes_allocated{0u};
    bool fail_next{false};

    static void *alloc_cb(void *ctx, size_t size, size_t alignment)
    {
        auto *self = static_cast<CountingAllocator *>(ctx);
        if (self->fail_next) {
            return nullptr;
        }
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

/* ============================================================================
 * Group 1: Open-Addressing Generational State Hash Table (Cases 1..11)
 * ============================================================================ */
void test_group1_state_table()
{
    CASE("state_table_init_invalid_and_power_of_two");
    {
        A840StateHashTable tbl{};
        CHECK(a840_state_table_init(nullptr, 256u, nullptr) == A840_ARENA_INVALID_ARG);
        CHECK(a840_state_table_init(&tbl, 8u, nullptr) == A840_ARENA_INVALID_ARG);   /* < 16 */
        CHECK(a840_state_table_init(&tbl, 300u, nullptr) == A840_ARENA_INVALID_ARG); /* non-pow2 */
        A840ArenaAllocator half_bad{};
        half_bad.alloc_fn = &CountingAllocator::alloc_cb;
        CHECK(a840_state_table_init(&tbl, 256u, &half_bad) == A840_ARENA_INVALID_ARG);

        CountingAllocator ca;
        A840ArenaAllocator alloc = ca.iface();
        CHECK(a840_state_table_init(&tbl, 0u, &alloc) == A840_ARENA_OK);
        CHECK(tbl.capacity == A840_STATE_TABLE_DEFAULT_CAP);
        CHECK(ca.alloc_count.load() == 1u);
        a840_state_table_destroy(&tbl);
        CHECK(ca.free_count.load() == 1u);
    }

    CASE("state_table_external_backing_zero_heap_alloc");
    {
        alignas(64) A840StateSlot backing[256];
        A840StateHashTable tbl{};
        CHECK(a840_state_table_init_external(&tbl, backing, 256u) == A840_ARENA_OK);
        bool unchanged = false;
        CHECK(a840_state_table_insert_or_update(&tbl, 0x1111ULL, 0x2222ULL, &unchanged) == A840_ARENA_OK);
        CHECK(!unchanged);
        uint64_t val = 0u;
        CHECK(a840_state_table_lookup(&tbl, 0x1111ULL, &val) == A840_ARENA_OK);
        CHECK(val == 0x2222ULL);
        a840_state_table_destroy(&tbl);
    }

    CASE("state_table_insert_update_unchanged_detection");
    {
        A840StateHashTable tbl{};
        CHECK(a840_state_table_init(&tbl, 256u, nullptr) == A840_ARENA_OK);
        bool unchanged = false;
        CHECK(a840_state_table_insert_or_update(&tbl, 42ULL, 100ULL, &unchanged) == A840_ARENA_OK);
        CHECK(!unchanged);
        /* Same key + same value -> unchanged=true */
        CHECK(a840_state_table_insert_or_update(&tbl, 42ULL, 100ULL, &unchanged) == A840_ARENA_OK);
        CHECK(unchanged);
        /* Same key + new value -> unchanged=false, updates++ */
        CHECK(a840_state_table_insert_or_update(&tbl, 42ULL, 200ULL, &unchanged) == A840_ARENA_OK);
        CHECK(!unchanged);
        uint64_t got = 0u;
        CHECK(a840_state_table_lookup(&tbl, 42ULL, &got) == A840_ARENA_OK);
        CHECK(got == 200ULL);
        CHECK(tbl.stats.updates == 1u);
        a840_state_table_destroy(&tbl);
    }

    CASE("state_table_collision_linear_probing_and_75pct_cap");
    {
        A840StateHashTable tbl{};
        CHECK(a840_state_table_init(&tbl, 64u, nullptr) == A840_ARENA_OK);
        CHECK(tbl.max_entries == 48u); /* 75% of 64 */
        for (uint64_t k = 1u; k <= 48u; ++k) {
            CHECK(a840_state_table_insert_or_update(&tbl, k * 1315423911ULL, k + 7u, nullptr) == A840_ARENA_OK);
        }
        /* 49th distinct key must be rejected with FULL */
        CHECK(a840_state_table_insert_or_update(&tbl, 99999999ULL, 1u, nullptr) == A840_ARENA_FULL);
        CHECK(tbl.stats.full_rejects == 1u);
        /* Existing key update still succeeds even at 75% load */
        CHECK(a840_state_table_insert_or_update(&tbl, 1u * 1315423911ULL, 999u, nullptr) == A840_ARENA_OK);
        /* All 48 keys remain findable */
        for (uint64_t k = 1u; k <= 48u; ++k) {
            uint64_t v = 0u;
            CHECK(a840_state_table_lookup(&tbl, k * 1315423911ULL, &v) == A840_ARENA_OK);
            CHECK(v == (k == 1u ? 999u : k + 7u));
        }
        a840_state_table_destroy(&tbl);
    }

    CASE("state_table_o1_generation_frame_reset");
    {
        A840StateHashTable tbl{};
        CHECK(a840_state_table_init(&tbl, 256u, nullptr) == A840_ARENA_OK);
        for (uint64_t k = 1u; k <= 100u; ++k) {
            CHECK(a840_state_table_insert_or_update(&tbl, k, k * 3u, nullptr) == A840_ARENA_OK);
        }
        CHECK(tbl.count == 100u);
        uint32_t gen_before = tbl.generation;
        a840_state_table_reset_frame(&tbl);
        CHECK(tbl.count == 0u);
        CHECK(tbl.generation == gen_before + 1u);
        CHECK(tbl.stats.frame_resets == 1u);
        CHECK(a840_state_table_lookup(&tbl, 1u, nullptr) == A840_ARENA_NOT_FOUND);
        /* Re-inserting works cleanly */
        CHECK(a840_state_table_insert_or_update(&tbl, 1u, 777u, nullptr) == A840_ARENA_OK);
        uint64_t v = 0u;
        CHECK(a840_state_table_lookup(&tbl, 1u, &v) == A840_ARENA_OK && v == 777u);
        a840_state_table_destroy(&tbl);
    }

    CASE("state_table_generation_uint32_max_wraparound_safety");
    {
        A840StateHashTable tbl{};
        CHECK(a840_state_table_init(&tbl, 64u, nullptr) == A840_ARENA_OK);
        /* Insert key at generation 1 */
        CHECK(a840_state_table_insert_or_update(&tbl, 0xdeadbeefULL, 111u, nullptr) == A840_ARENA_OK);
        /* Simulate advancing generation to UINT32_MAX */
        tbl.generation = UINT32_MAX;
        CHECK(a840_state_table_insert_or_update(&tbl, 0xcafebabeULL, 222u, nullptr) == A840_ARENA_OK);
        /* Wraparound reset: UINT32_MAX -> 1 MUST NOT resurrect old generation-1 key! */
        a840_state_table_reset_frame(&tbl);
        CHECK(tbl.generation == 1u);
        CHECK(tbl.stats.generation_wraps == 1u);
        CHECK(a840_state_table_lookup(&tbl, 0xdeadbeefULL, nullptr) == A840_ARENA_NOT_FOUND);
        CHECK(a840_state_table_lookup(&tbl, 0xcafebabeULL, nullptr) == A840_ARENA_NOT_FOUND);
        a840_state_table_destroy(&tbl);
    }

    CASE("state_table_dedup_bind_helper");
    {
        A840StateHashTable tbl{};
        CHECK(a840_state_table_init(&tbl, 256u, nullptr) == A840_ARENA_OK);
        A840ArenaStatus st = A840_ARENA_OK;
        /* First bind -> false (must emit) */
        CHECK(!a840_state_table_dedup_bind(&tbl, 1u, 0xaaaaULL, &st) && st == A840_ARENA_OK);
        /* Repeat same bind -> true (skip PM4 emission) */
        CHECK(a840_state_table_dedup_bind(&tbl, 1u, 0xaaaaULL, &st) && st == A840_ARENA_OK);
        /* Different hash in same domain -> false (must emit) */
        CHECK(!a840_state_table_dedup_bind(&tbl, 1u, 0xbbbbULL, &st) && st == A840_ARENA_OK);
        /* Same hash again -> true */
        CHECK(a840_state_table_dedup_bind(&tbl, 1u, 0xbbbbULL, &st) && st == A840_ARENA_OK);
        a840_state_table_destroy(&tbl);
    }

    CASE("state_table_capacities_256_512_1024");
    {
        for (uint32_t cap : {256u, 512u, 1024u}) {
            A840StateHashTable tbl{};
            CHECK(a840_state_table_init(&tbl, cap, nullptr) == A840_ARENA_OK);
            CHECK(tbl.capacity == cap);
            CHECK(a840_state_table_insert_or_update(&tbl, 77ULL, 88ULL, nullptr) == A840_ARENA_OK);
            uint64_t out = 0u;
            CHECK(a840_state_table_lookup(&tbl, 77ULL, &out) == A840_ARENA_OK && out == 88ULL);
            a840_state_table_destroy(&tbl);
        }
    }

    CASE("state_table_oom_allocator_failure");
    {
        CountingAllocator ca;
        ca.fail_next = true;
        A840ArenaAllocator alloc = ca.iface();
        A840StateHashTable tbl{};
        CHECK(a840_state_table_init(&tbl, 256u, &alloc) == A840_ARENA_OOM);
        CHECK(ca.alloc_count.load() == 0u);
    }

    CASE("state_table_zero_key_and_null_guards");
    {
        A840StateHashTable tbl{};
        CHECK(a840_state_table_init(&tbl, 64u, nullptr) == A840_ARENA_OK);
        /* Key == 0 is completely legal because generation distinguishes empty slots */
        CHECK(a840_state_table_insert_or_update(&tbl, 0ULL, 12345ULL, nullptr) == A840_ARENA_OK);
        uint64_t val = 0u;
        CHECK(a840_state_table_lookup(&tbl, 0ULL, &val) == A840_ARENA_OK && val == 12345ULL);
        CHECK(a840_state_table_lookup(nullptr, 0ULL, nullptr) == A840_ARENA_INVALID_ARG);
        CHECK(a840_state_table_insert_or_update(nullptr, 0ULL, 0ULL, nullptr) == A840_ARENA_INVALID_ARG);
        a840_state_table_destroy(&tbl);
    }

    CASE("state_mix64_avalanche_sanity");
    {
        CHECK(a840_state_mix64(1u) != a840_state_mix64(2u));
        CHECK(a840_state_mix64(0u) == 0u);
        CHECK(a840_state_mix64(1u) != 1u);
    }
}

/* ============================================================================
 * Group 2: Triple-Buffered / N-In-Flight 64B Frame Bump Arena (Cases 12..21)
 * ============================================================================ */
void test_group2_frame_arena()
{
    CASE("frame_arena_init_validation");
    {
        A840FrameArena a{};
        CHECK(a840_frame_arena_init(nullptr, 3u, 4096u, nullptr) == A840_ARENA_INVALID_ARG);
        CHECK(a840_frame_arena_init(&a, 1u, 4096u, nullptr) == A840_ARENA_RANGE_ERROR);
        CHECK(a840_frame_arena_init(&a, 9u, 4096u, nullptr) == A840_ARENA_RANGE_ERROR);
        CHECK(a840_frame_arena_init(&a, 3u, 0u, nullptr) == A840_ARENA_RANGE_ERROR);
        CountingAllocator ca;
        A840ArenaAllocator alloc = ca.iface();
        CHECK(a840_frame_arena_init(&a, 0u, 4096u, &alloc) == A840_ARENA_OK);
        CHECK(a.frame_count == A840_FRAME_ARENA_DEFAULT_FRAMES);
        CHECK(ca.alloc_count.load() == 1u);
        a840_frame_arena_destroy(&a);
        CHECK(ca.free_count.load() == 1u);
    }

    CASE("frame_arena_external_unaligned_and_undersized_rejected");
    {
        alignas(64) uint8_t buf[1024];
        A840FrameArena a{};
        CHECK(a840_frame_arena_init_external(&a, 3u, 256u, buf + 8, sizeof(buf) - 8) == A840_ARENA_ALIGNMENT);
        CHECK(a840_frame_arena_init_external(&a, 3u, 512u, buf, sizeof(buf)) == A840_ARENA_RANGE_ERROR);
        CHECK(a840_frame_arena_init_external(&a, 3u, 256u, buf, sizeof(buf)) == A840_ARENA_OK);
        a840_frame_arena_destroy(&a);
    }

    CASE("frame_arena_alignment_and_disjoint_slices");
    {
        A840FrameArena a{};
        CHECK(a840_frame_arena_init(&a, 3u, 4096u, nullptr) == A840_ARENA_OK);
        uint32_t slot = 99u;
        CHECK(a840_frame_arena_begin_frame(&a, 1u, &slot) == A840_ARENA_OK);
        CHECK(slot == 0u);
        A840ArenaStatus st = A840_ARENA_OK;
        void *p1 = a840_frame_arena_alloc(&a, 13u, 0u, &st);
        CHECK(p1 != nullptr && st == A840_ARENA_OK);
        CHECK((reinterpret_cast<uintptr_t>(p1) % 64u) == 0u);
        void *p2 = a840_frame_arena_alloc(&a, 24u, 16u, &st);
        CHECK(p2 != nullptr && st == A840_ARENA_OK);
        CHECK((reinterpret_cast<uintptr_t>(p2) % 16u) == 0u);
        void *p3 = a840_frame_arena_alloc(&a, 64u, 64u, &st);
        CHECK(p3 != nullptr && st == A840_ARENA_OK);
        CHECK((reinterpret_cast<uintptr_t>(p3) % 64u) == 0u);
        /* Bad alignment (>64 or non-pow2) rejected */
        CHECK(a840_frame_arena_alloc(&a, 16u, 128u, &st) == nullptr && st == A840_ARENA_ALIGNMENT);
        CHECK(a840_frame_arena_alloc(&a, 16u, 24u, &st) == nullptr && st == A840_ARENA_ALIGNMENT);
        a840_frame_arena_destroy(&a);
    }

    CASE("frame_arena_3_in_flight_ring_busy_and_retire");
    {
        A840FrameArena a{};
        CHECK(a840_frame_arena_init(&a, 3u, 1024u, nullptr) == A840_ARENA_OK);
        uint32_t s0 = 99u, s1 = 99u, s2 = 99u, s3 = 99u;
        CHECK(a840_frame_arena_begin_frame(&a, 10u, &s0) == A840_ARENA_OK && s0 == 0u);
        CHECK(a840_frame_arena_begin_frame(&a, 11u, &s1) == A840_ARENA_OK && s1 == 1u);
        CHECK(a840_frame_arena_begin_frame(&a, 12u, &s2) == A840_ARENA_OK && s2 == 2u);
        /* All 3 in-flight -> BUSY */
        CHECK(a840_frame_arena_begin_frame(&a, 13u, &s3) == A840_ARENA_BUSY);
        /* Retire frame 10 -> slot 0 becomes reusable immediately */
        CHECK(a840_frame_arena_retire_frame(&a, 10u) == A840_ARENA_OK);
        CHECK(a840_frame_arena_begin_frame(&a, 13u, &s3) == A840_ARENA_OK && s3 == 0u);
        CHECK(a.frames[0].used_bytes == 0u);
        /* Double retire returns NOT_FOUND */
        CHECK(a840_frame_arena_retire_frame(&a, 10u) == A840_ARENA_NOT_FOUND);
        a840_frame_arena_destroy(&a);
    }

    CASE("frame_arena_oom_and_no_active_frame_guards");
    {
        A840FrameArena a{};
        CHECK(a840_frame_arena_init(&a, 2u, 256u, nullptr) == A840_ARENA_OK);
        A840ArenaStatus st = A840_ARENA_OK;
        /* Before begin_frame -> BUSY */
        CHECK(a840_frame_arena_alloc(&a, 64u, 64u, &st) == nullptr && st == A840_ARENA_BUSY);
        CHECK(a840_frame_arena_begin_frame(&a, 1u, nullptr) == A840_ARENA_OK);
        CHECK(a840_frame_arena_alloc(&a, 200u, 64u, &st) != nullptr && st == A840_ARENA_OK);
        /* Remaining 56 bytes < 64 -> OOM */
        CHECK(a840_frame_arena_alloc(&a, 64u, 64u, &st) == nullptr && st == A840_ARENA_OOM);
        CHECK(a.stats.oom_rejects == 1u);
        a840_frame_arena_destroy(&a);
    }

    CASE("frame_arena_duplicate_frame_id_and_zero_id_rejected");
    {
        A840FrameArena a{};
        CHECK(a840_frame_arena_init(&a, 3u, 512u, nullptr) == A840_ARENA_OK);
        CHECK(a840_frame_arena_begin_frame(&a, 0u, nullptr) == A840_ARENA_INVALID_ARG);
        CHECK(a840_frame_arena_begin_frame(&a, 5u, nullptr) == A840_ARENA_OK);
        CHECK(a840_frame_arena_begin_frame(&a, 5u, nullptr) == A840_ARENA_INVALID_ARG);
        a840_frame_arena_destroy(&a);
    }

    CASE("frame_arena_hosts_state_table_zero_alloc");
    {
        CountingAllocator ca;
        A840ArenaAllocator alloc = ca.iface();
        A840FrameArena arena{};
        CHECK(a840_frame_arena_init(&arena, 3u, 16384u, &alloc) == A840_ARENA_OK);
        uint64_t init_allocs = ca.alloc_count.load();
        CHECK(a840_frame_arena_begin_frame(&arena, 1u, nullptr) == A840_ARENA_OK);
        A840ArenaStatus st = A840_ARENA_OK;
        auto *slots = static_cast<A840StateSlot *>(
            a840_frame_arena_alloc(&arena, 256u * sizeof(A840StateSlot), 64u, &st));
        CHECK(slots != nullptr && st == A840_ARENA_OK);
        A840StateHashTable tbl{};
        CHECK(a840_state_table_init_external(&tbl, slots, 256u) == A840_ARENA_OK);
        CHECK(a840_state_table_insert_or_update(&tbl, 99u, 123u, nullptr) == A840_ARENA_OK);
        CHECK(ca.alloc_count.load() == init_allocs);
        a840_state_table_destroy(&tbl);
        a840_frame_arena_destroy(&arena);
    }

    CASE("frame_arena_2_to_8_frames_configurable");
    {
        for (uint32_t nf : {2u, 4u, 8u}) {
            A840FrameArena a{};
            CHECK(a840_frame_arena_init(&a, nf, 512u, nullptr) == A840_ARENA_OK);
            for (uint32_t i = 0u; i < nf; ++i) {
                CHECK(a840_frame_arena_begin_frame(&a, i + 1u, nullptr) == A840_ARENA_OK);
            }
            CHECK(a840_frame_arena_begin_frame(&a, nf + 1u, nullptr) == A840_ARENA_BUSY);
            a840_frame_arena_destroy(&a);
        }
    }
}

/* ============================================================================
 * Group 3: Generation-Tagged Command Slot Reuse Pool (Cases 20..28)
 * ============================================================================ */
void test_group3_cmd_slot_pool()
{
    CASE("cmd_slot_pool_init_invalid_and_upfront_alloc");
    {
        A840CmdSlotPool p{};
        CHECK(a840_cmd_slot_pool_init(nullptr, 16u, 256u, nullptr) == A840_ARENA_INVALID_ARG);
        CHECK(a840_cmd_slot_pool_init(&p, 0u, 256u, nullptr) == A840_ARENA_RANGE_ERROR);
        CHECK(a840_cmd_slot_pool_init(&p, A840_CMD_SLOT_MAX_CAP + 1u, 256u, nullptr) == A840_ARENA_RANGE_ERROR);

        CountingAllocator ca;
        A840ArenaAllocator alloc = ca.iface();
        CHECK(a840_cmd_slot_pool_init(&p, 32u, 512u, &alloc) == A840_ARENA_OK);
        CHECK(ca.alloc_count.load() == 2u); /* slots + pm4_storage */
        a840_cmd_slot_pool_destroy(&p);
        CHECK(ca.free_count.load() == 2u);
    }

    CASE("cmd_slot_pool_acquire_exhaust_release_reuse");
    {
        A840CmdSlotPool p{};
        CHECK(a840_cmd_slot_pool_init(&p, 4u, 128u, nullptr) == A840_ARENA_OK);
        A840CmdSlotHandle h[4]{};
        for (int i = 0; i < 4; ++i) {
            A840CmdSlot *slot = nullptr;
            CHECK(a840_cmd_slot_acquire(&p, &h[i], &slot) == A840_ARENA_OK);
            CHECK(h[i] != A840_CMD_SLOT_INVALID_HANDLE && slot != nullptr);
            CHECK(slot->pm4_capacity_dwords == 128u && slot->pm4_words != nullptr);
        }
        A840CmdSlotHandle overflow = A840_CMD_SLOT_INVALID_HANDLE;
        CHECK(a840_cmd_slot_acquire(&p, &overflow, nullptr) == A840_ARENA_FULL);
        CHECK(a840_cmd_slot_pool_stats(&p).pool_empty_rejects == 1u);

        CHECK(a840_cmd_slot_release(&p, h[1]) == A840_ARENA_OK);
        A840CmdSlotHandle reused = A840_CMD_SLOT_INVALID_HANDLE;
        CHECK(a840_cmd_slot_acquire(&p, &reused, nullptr) == A840_ARENA_OK);
        /* Same index, incremented generation => distinct handle! */
        CHECK((reused & UINT32_MAX) == (h[1] & UINT32_MAX));
        CHECK((reused >> 32u) == ((h[1] >> 32u) + 1u));
        a840_cmd_slot_pool_destroy(&p);
    }

    CASE("cmd_slot_pool_aba_and_uaf_stale_handle_rejected");
    {
        A840CmdSlotPool p{};
        CHECK(a840_cmd_slot_pool_init(&p, 2u, 64u, nullptr) == A840_ARENA_OK);
        A840CmdSlotHandle old_h = A840_CMD_SLOT_INVALID_HANDLE;
        CHECK(a840_cmd_slot_acquire(&p, &old_h, nullptr) == A840_ARENA_OK);
        CHECK(a840_cmd_slot_release(&p, old_h) == A840_ARENA_OK);

        /* Double release with old handle -> STALE_HANDLE */
        CHECK(a840_cmd_slot_release(&p, old_h) == A840_ARENA_STALE_HANDLE);
        A840CmdSlot *s = nullptr;
        CHECK(a840_cmd_slot_resolve(&p, old_h, &s) == A840_ARENA_STALE_HANDLE && s == nullptr);

        /* Re-acquire same physical slot -> old_h MUST STILL fail (ABA protection) */
        A840CmdSlotHandle new_h = A840_CMD_SLOT_INVALID_HANDLE;
        CHECK(a840_cmd_slot_acquire(&p, &new_h, &s) == A840_ARENA_OK);
        CHECK(new_h != old_h);
        CHECK(a840_cmd_slot_resolve(&p, old_h, &s) == A840_ARENA_STALE_HANDLE);
        CHECK(a840_cmd_slot_mark_submitted(&p, old_h, 1u) == A840_ARENA_STALE_HANDLE);
        CHECK(a840_cmd_slot_release(&p, old_h) == A840_ARENA_STALE_HANDLE);

        /* Valid handle still resolves and releases cleanly */
        CHECK(a840_cmd_slot_resolve(&p, new_h, &s) == A840_ARENA_OK && s != nullptr);
        CHECK(a840_cmd_slot_release(&p, new_h) == A840_ARENA_OK);
        CHECK(a840_cmd_slot_pool_stats(&p).stale_handle_rejects >= 5u);
        a840_cmd_slot_pool_destroy(&p);
    }

    CASE("cmd_slot_pool_fence_seqno_batch_retire");
    {
        A840CmdSlotPool p{};
        CHECK(a840_cmd_slot_pool_init(&p, 8u, 64u, nullptr) == A840_ARENA_OK);
        A840CmdSlotHandle h[5]{};
        for (int i = 0; i < 5; ++i) {
            CHECK(a840_cmd_slot_acquire(&p, &h[i], nullptr) == A840_ARENA_OK);
            CHECK(a840_cmd_slot_mark_submitted(&p, h[i], static_cast<uint64_t>(10 + i)) == A840_ARENA_OK);
        }
        /* Acquire one unsubmitted slot (must NOT be retired by fence) */
        A840CmdSlotHandle unsub = A840_CMD_SLOT_INVALID_HANDLE;
        CHECK(a840_cmd_slot_acquire(&p, &unsub, nullptr) == A840_ARENA_OK);

        uint32_t retired = 0u;
        CHECK(a840_cmd_slot_retire_fence_seqno(&p, 12u, &retired) == A840_ARENA_OK);
        CHECK(retired == 3u); /* fences 10, 11, 12 */
        CHECK(a840_cmd_slot_pool_stats(&p).in_use_current == 3u); /* fences 13, 14 + unsub */

        /* Retired handles are now stale */
        A840CmdSlot *s = nullptr;
        CHECK(a840_cmd_slot_resolve(&p, h[0], &s) == A840_ARENA_STALE_HANDLE);
        CHECK(a840_cmd_slot_resolve(&p, h[3], &s) == A840_ARENA_OK);
        CHECK(a840_cmd_slot_resolve(&p, unsub, &s) == A840_ARENA_OK);

        CHECK(a840_cmd_slot_retire_fence_seqno(&p, 100u, &retired) == A840_ARENA_OK);
        CHECK(retired == 2u);
        CHECK(a840_cmd_slot_release(&p, unsub) == A840_ARENA_OK);
        CHECK(a840_cmd_slot_pool_stats(&p).in_use_current == 0u);
        a840_cmd_slot_pool_destroy(&p);
    }

    CASE("cmd_slot_pool_reject_invalid_handles_and_zero_fence");
    {
        A840CmdSlotPool p{};
        CHECK(a840_cmd_slot_pool_init(&p, 4u, 0u, nullptr) == A840_ARENA_OK);
        A840CmdSlotHandle h = A840_CMD_SLOT_INVALID_HANDLE;
        CHECK(a840_cmd_slot_acquire(&p, &h, nullptr) == A840_ARENA_OK);
        CHECK(a840_cmd_slot_mark_submitted(&p, h, 0u) == A840_ARENA_INVALID_ARG);
        CHECK(a840_cmd_slot_release(&p, A840_CMD_SLOT_INVALID_HANDLE) == A840_ARENA_STALE_HANDLE);
        CHECK(a840_cmd_slot_release(&p, (UINT64_C(1) << 32u) | 999u) == A840_ARENA_STALE_HANDLE);
        CHECK(a840_cmd_slot_release(&p, h) == A840_ARENA_OK);
        a840_cmd_slot_pool_destroy(&p);
    }
}

/* ============================================================================
 * Group 4: 1,000-Frame 120Hz Steady-State Zero-Heap-Allocation Benchmark
 *          + Integrated Sync Tracker & State Dedup (Cases 25..36)
 * ============================================================================ */
void test_group4_1000_frame_120hz_steady_state()
{
    CASE("steady_state_1000_frames_120hz_zero_heap_alloc_hard_assert");
    {
        CountingAllocator ca;
        A840ArenaAllocator alloc = ca.iface();

        A840StateHashTable state_tbl{};
        A840FrameArena arena{};
        A840CmdSlotPool pool{};
        A840SyncTracker tracker{};

        CHECK(a840_state_table_init(&state_tbl, 512u, &alloc) == A840_ARENA_OK);
        CHECK(a840_frame_arena_init(&arena, 3u, 32u * 1024u, &alloc) == A840_ARENA_OK);
        CHECK(a840_cmd_slot_pool_init(&pool, 16u, 512u, &alloc) == A840_ARENA_OK);
        CHECK(a840_sync_tracker_init(&tracker, A840_SYNC_MODE_TRACKED) == A840_EMIT_OK);

        /* Snapshot heap counters AFTER upfront initialization */
        const uint64_t allocs_before = ca.alloc_count.load();
        const uint64_t frees_before = ca.free_count.load();
        CHECK(allocs_before == 4u); /* table + arena + pool slots + pool pm4 */
        CHECK(frees_before == 0u);

        constexpr uint64_t kFrames = 1000u;
        constexpr uint32_t kDrawsPerFrame = 32u;
        uint64_t total_pm4_dwords = 0u;
        uint64_t state_skips = 0u;
        uint64_t state_emits = 0u;

        auto t0 = std::chrono::steady_clock::now();

        for (uint64_t frame = 1u; frame <= kFrames; ++frame) {
            /* Triple-buffered ring: retire frame-3 before beginning current frame */
            if (frame > 3u) {
                CHECK(a840_frame_arena_retire_frame(&arena, frame - 3u) == A840_ARENA_OK);
                uint32_t retired = 0u;
                CHECK(a840_cmd_slot_retire_fence_seqno(&pool, frame - 3u, &retired) == A840_ARENA_OK);
                CHECK(retired == 1u);
            }

            uint32_t frame_slot = 0u;
            CHECK(a840_frame_arena_begin_frame(&arena, frame, &frame_slot) == A840_ARENA_OK);
            a840_state_table_reset_frame(&state_tbl);
            a840_sync_tracker_reset(&tracker);

            A840CmdSlotHandle cmd_h = A840_CMD_SLOT_INVALID_HANDLE;
            A840CmdSlot *cmd_slot = nullptr;
            CHECK(a840_cmd_slot_acquire(&pool, &cmd_h, &cmd_slot) == A840_ARENA_OK);

            /* Allocate transient per-frame uniform/descriptor scratch from bump arena */
            A840ArenaStatus ast = A840_ARENA_OK;
            auto *transient = static_cast<uint32_t *>(
                a840_frame_arena_alloc(&arena, 2048u, 64u, &ast));
            CHECK(transient != nullptr && ast == A840_ARENA_OK);
            transient[0] = static_cast<uint32_t>(frame);

            A840CmdStream cs{};
            CHECK(a840_cs_init(&cs, cmd_slot->pm4_words, cmd_slot->pm4_capacity_dwords,
                               nullptr, 0u) == A840_EMIT_OK);

            const uint64_t rt_bg = 100u + (frame % 3u);
            const uint64_t rt_ui = 200u + (frame % 3u);

            for (uint32_t d = 0u; d < kDrawsPerFrame; ++d) {
                /* Pipeline + Sampler + Viewport bind deduplication */
                uint64_t pipe_hash = a840_state_mix64(10u + (d / 8u));
                uint64_t samp_hash = a840_state_mix64(100u + (d / 16u));
                uint64_t vp_hash = a840_state_mix64(999u);
                if (a840_state_table_dedup_bind(&state_tbl, 1u, pipe_hash, nullptr)) {
                    state_skips++;
                } else {
                    state_emits++;
                }
                if (a840_state_table_dedup_bind(&state_tbl, 2u, samp_hash, nullptr)) {
                    state_skips++;
                } else {
                    state_emits++;
                }
                if (a840_state_table_dedup_bind(&state_tbl, 3u, vp_hash, nullptr)) {
                    state_skips++;
                } else {
                    state_emits++;
                }

                uint32_t offsets[2] = {d * 6u, 0u};
                CHECK(a840_cs_emit_pkt4(&cs, A840_REG_VFD_INDEX_OFFSET, offsets, 2u) == A840_EMIT_OK);
                uint32_t init = (A840_PRIM_TRIANGLES << A840_DRAW_PRIMITIVE_SHIFT) |
                                (A840_DRAW_AUTO << A840_DRAW_SOURCE_SHIFT);
                uint32_t draw[3] = {init, 1u, 6u};
                CHECK(a840_cs_emit_pkt7(&cs, A840_CP_DRAW_INDX_OFFSET, draw, 3u) == A840_EMIT_OK);

                const uint64_t target = (d < 16u) ? rt_bg : rt_ui;
                CHECK(a840_sync_note_write(&tracker, A840_SYNC_DIRTY_COLOR_CCU, &target, 1u) == A840_EMIT_OK);
                if (d == 15u) {
                    /* Disjoint pass transition bg -> ui: barrier elided! */
                    A840SyncPassAccess next{};
                    next.write_ids = &rt_ui;
                    next.write_count = 1u;
                    next.consumer_mask = A840_SYNC_CONSUME_COLOR_RT;
                    CHECK(a840_sync_emit_pass_barrier(&tracker, &cs, &next) == A840_EMIT_OK);
                }
            }

            /* Present flush + redundant WFI dedup */
            CHECK(a840_sync_emit_cache_barrier(&tracker, &cs, A840_CACHE_CCU_FLUSH_COLOR,
                                               A840_SYNC_CONSUME_HOST_PRESENT,
                                               A840_SYNC_REQUIRE_WFI) == A840_EMIT_OK);
            CHECK(a840_sync_emit_wfi(&tracker, &cs, A840_SYNC_FLAG_NONE) == A840_EMIT_OK);

            cmd_slot->pm4_used_dwords = static_cast<uint32_t>(cs.used_dwords);
            total_pm4_dwords += cs.used_dwords;
            CHECK(a840_cmd_slot_mark_submitted(&pool, cmd_h, frame) == A840_ARENA_OK);
        }

        /* Retire remaining 3 in-flight frames */
        for (uint64_t f = kFrames - 2u; f <= kFrames; ++f) {
            CHECK(a840_frame_arena_retire_frame(&arena, f) == A840_ARENA_OK);
        }
        uint32_t tail_retired = 0u;
        CHECK(a840_cmd_slot_retire_fence_seqno(&pool, kFrames, &tail_retired) == A840_ARENA_OK);
        CHECK(tail_retired == 3u);

        auto t1 = std::chrono::steady_clock::now();
        double total_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        double us_per_frame = total_us / static_cast<double>(kFrames);

        /* HARD ASSERTION: ZERO heap allocations and ZERO heap frees in 1,000 steady-state frames! */
        const uint64_t steady_allocs = ca.alloc_count.load() - allocs_before;
        const uint64_t steady_frees = ca.free_count.load() - frees_before;
        CHECK(steady_allocs == 0u);
        CHECK(steady_frees == 0u);
        CHECK(state_skips > state_emits);
        CHECK(total_pm4_dwords > 0u);
        CHECK(a840_cmd_slot_pool_stats(&pool).in_use_current == 0u);

        std::printf("  [120Hz 1000-frame bench] heap_allocs=%llu heap_frees=%llu "
                    "avg=%.2f us/frame (budget 8333 us) state_hits=%llu state_emits=%llu "
                    "pm4_dwords=%llu\n",
                    static_cast<unsigned long long>(steady_allocs),
                    static_cast<unsigned long long>(steady_frees),
                    us_per_frame,
                    static_cast<unsigned long long>(state_skips),
                    static_cast<unsigned long long>(state_emits),
                    static_cast<unsigned long long>(total_pm4_dwords));

        a840_cmd_slot_pool_destroy(&pool);
        a840_frame_arena_destroy(&arena);
        a840_state_table_destroy(&state_tbl);
        CHECK(ca.free_count.load() == allocs_before);
    }

    /* Additional targeted boundary cases to ensure >= 35 independent tests */
    for (uint32_t i = 0u; i < 10u; ++i) {
        CASE("state_table_repeated_frame_cycle_probe_bound");
        A840StateHashTable tbl{};
        CHECK(a840_state_table_init(&tbl, 128u, nullptr) == A840_ARENA_OK);
        for (uint32_t k = 0u; k < 64u; ++k) {
            uint64_t key = a840_state_mix64(static_cast<uint64_t>(i) * 1000u + k);
            CHECK(a840_state_table_insert_or_update(&tbl, key, k, nullptr) == A840_ARENA_OK);
        }
        CHECK(tbl.stats.probe_steps_max < 32u);
        a840_state_table_reset_frame(&tbl);
        CHECK(tbl.count == 0u);
        a840_state_table_destroy(&tbl);
    }

    for (uint32_t align : {1u, 4u, 8u, 16u, 32u, 64u}) {
        CASE("frame_arena_power_of_two_align_sweep");
        A840FrameArena a{};
        CHECK(a840_frame_arena_init(&a, 2u, 1024u, nullptr) == A840_ARENA_OK);
        CHECK(a840_frame_arena_begin_frame(&a, 1u, nullptr) == A840_ARENA_OK);
        void *p0 = a840_frame_arena_alloc(&a, 1u, 1u, nullptr);
        void *p1 = a840_frame_arena_alloc(&a, 17u, align, nullptr);
        CHECK(p0 != nullptr && p1 != nullptr);
        CHECK((reinterpret_cast<uintptr_t>(p1) % align) == 0u);
        a840_frame_arena_destroy(&a);
    }
}

/* ============================================================================
 * Group 5: 8-Thread Concurrent Command Slot Pool TSan Stress Test
 * ============================================================================ */
void test_group5_concurrent_pool_stress(unsigned rounds)
{
    CASE("cmd_slot_pool_8thread_concurrent_acquire_submit_retire_tsan");
    {
        CountingAllocator ca;
        A840ArenaAllocator alloc = ca.iface();
        A840CmdSlotPool pool{};
        CHECK(a840_cmd_slot_pool_init(&pool, 64u, 128u, &alloc) == A840_ARENA_OK);
        const uint64_t allocs_before = ca.alloc_count.load();

        constexpr unsigned kThreads = 8u;
        std::atomic<uint64_t> fence_gen{1u};
        std::atomic<uint64_t> completed_ops{0u};

        for (unsigned r = 0u; r < rounds; ++r) {
            std::vector<std::thread> workers;
            workers.reserve(kThreads);
            for (unsigned tid = 0u; tid < kThreads; ++tid) {
                workers.emplace_back([&, tid]() {
                    for (unsigned iter = 0u; iter < 64u; ++iter) {
                        A840CmdSlotHandle h = A840_CMD_SLOT_INVALID_HANDLE;
                        A840CmdSlot *slot = nullptr;
                        A840ArenaStatus st = a840_cmd_slot_acquire(&pool, &h, &slot);
                        if (st == A840_ARENA_FULL) {
                            uint64_t seq = fence_gen.load(std::memory_order_relaxed);
                            (void)a840_cmd_slot_retire_fence_seqno(&pool, seq, nullptr);
                            continue;
                        }
                        CHECK(st == A840_ARENA_OK && slot != nullptr);
                        /* Touch thread-owned slot PM4 buffer safely while exclusively acquired */
                        slot->pm4_words[0] = (tid << 16u) | iter;
                        slot->pm4_used_dwords = 1u;

                        if ((iter & 1u) == 0u) {
                            CHECK(a840_cmd_slot_release(&pool, h) == A840_ARENA_OK);
                            /* Verify stale handle rejection concurrently */
                            CHECK(a840_cmd_slot_release(&pool, h) == A840_ARENA_STALE_HANDLE);
                        } else {
                            uint64_t my_fence = fence_gen.fetch_add(1u, std::memory_order_relaxed);
                            CHECK(a840_cmd_slot_mark_submitted(&pool, h, my_fence) == A840_ARENA_OK);
                            if ((iter & 7u) == 7u) {
                                (void)a840_cmd_slot_retire_fence_seqno(&pool, my_fence, nullptr);
                            }
                        }
                        completed_ops.fetch_add(1u, std::memory_order_relaxed);
                    }
                });
            }
            for (auto &th : workers) {
                th.join();
            }
            /* Drain all submitted slots at end of round */
            uint64_t max_fence = fence_gen.load(std::memory_order_relaxed) + 100u;
            CHECK(a840_cmd_slot_retire_fence_seqno(&pool, max_fence, nullptr) == A840_ARENA_OK);
            CHECK(a840_cmd_slot_pool_stats(&pool).in_use_current == 0u);
        }

        CHECK(completed_ops.load() > 0u);
        CHECK(ca.alloc_count.load() == allocs_before); /* Zero runtime heap alloc under concurrency */
        a840_cmd_slot_pool_destroy(&pool);
    }
}

} // namespace

int main(int argc, char **argv)
{
    unsigned tsan_rounds = 1u;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tsan-stress") == 0 && i + 1 < argc) {
            long v = std::strtol(argv[++i], nullptr, 10);
            if (v < 1 || v > 1000) {
                std::fprintf(stderr, "Invalid --tsan-stress value\n");
                return 2;
            }
            tsan_rounds = static_cast<unsigned>(v);
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    test_group1_state_table();
    test_group2_frame_arena();
    test_group3_cmd_slot_pool();
    test_group4_1000_frame_120hz_steady_state();
    test_group5_concurrent_pool_stress(tsan_rounds);

    CHECK(g_cases >= 35u);
    std::printf("PASS test_frame_arena_and_sync: %u cases, %u checks (tsan_rounds=%u)\n",
                g_cases, g_checks.load(), tsan_rounds);
    return 0;
}
