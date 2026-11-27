/* SPDX-License-Identifier: MIT */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "NativeAdrenoABI_v3.h"
#include "a840_kmd_bo_pool.h"
#include "a840_kmd_ca_env_policy.h"
#include "a840_kmd_fault_capture.h"
#include "a840_kmd_quota_recovery.h"
#include "a840_kmd_sparse_va.h"
#include "a840_kmd_zero_page.h"

namespace {

static int g_cases_passed = 0;
static int g_stress_rounds = 1;

static constexpr uint64_t kKiB = UINT64_C(1024);
static constexpr uint64_t kMiB = UINT64_C(1048576);
static constexpr uint64_t kGiB = UINT64_C(1073741824);

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

#define RUN_CASE(fn)      \
    do {                  \
        fn();             \
        ++g_cases_passed; \
    } while (0)

/* ---------------- Custom Sync Lock Mock ---------------- */

struct CustomSyncMock {
    std::mutex mu;
    std::atomic<int> lock_depth{0};
    std::atomic<uint64_t> locks{0};
    std::atomic<uint64_t> unlocks{0};
    std::atomic<uint64_t> clock_ns{1000};
    std::mutex owner_mu;
    std::thread::id owner_tid{};

    bool held_by_current_thread() {
        std::lock_guard<std::mutex> lk(owner_mu);
        return owner_tid == std::this_thread::get_id();
    }

    static void do_lock(void *cookie) {
        auto *self = static_cast<CustomSyncMock *>(cookie);
        self->mu.lock();
        {
            std::lock_guard<std::mutex> lk(self->owner_mu);
            self->owner_tid = std::this_thread::get_id();
        }
        self->lock_depth.fetch_add(1, std::memory_order_relaxed);
        self->locks.fetch_add(1, std::memory_order_relaxed);
    }

    static void do_unlock(void *cookie) {
        auto *self = static_cast<CustomSyncMock *>(cookie);
        self->lock_depth.fetch_sub(1, std::memory_order_relaxed);
        self->unlocks.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(self->owner_mu);
            self->owner_tid = std::thread::id{};
        }
        self->mu.unlock();
    }

    static uint64_t do_now_ns(void *cookie) {
        auto *self = static_cast<CustomSyncMock *>(cookie);
        return self->clock_ns.load(std::memory_order_relaxed);
    }

    A840KmdQuotaSyncOps ops() {
        A840KmdQuotaSyncOps o{};
        o.cookie = this;
        o.lock = &CustomSyncMock::do_lock;
        o.unlock = &CustomSyncMock::do_unlock;
        o.now_ns = &CustomSyncMock::do_now_ns;
        return o;
    }
};

/* ---------------- Reclaim Callback Mock ---------------- */

struct ReclaimEvent {
    std::string kind;
    uint64_t client_id;
    uint64_t bo_handle;
    uint64_t bytes;
};

struct ReclaimMock {
    std::mutex mu;
    CustomSyncMock *sync_to_check = nullptr;
    std::atomic<uint32_t> saw_lock_held{0};
    std::vector<ReclaimEvent> events;
    bool fail_parked = false;
    bool fail_volatile = false;
    uint64_t bulk_parked_return = 0;
    uint64_t bulk_volatile_return = 0;
    uint64_t bg_evict_cap = UINT64_MAX;

    void check_unlocked() {
        if (sync_to_check != nullptr && sync_to_check->held_by_current_thread()) {
            saw_lock_held.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static A840KmdStatus on_reclaim_parked(
        void *cookie, uint64_t client_id, uint64_t bo_handle,
        uint64_t bytes, uint64_t *out_reclaimed) {
        auto *self = static_cast<ReclaimMock *>(cookie);
        self->check_unlocked();
        std::lock_guard<std::mutex> lk(self->mu);
        self->events.push_back({"parked", client_id, bo_handle, bytes});
        if (self->fail_parked) {
            *out_reclaimed = 0;
            return A840_KMD_IO_ERROR;
        }
        *out_reclaimed = bytes;
        return A840_KMD_OK;
    }

    static A840KmdStatus on_purge_volatile(
        void *cookie, uint64_t client_id, uint64_t bo_handle,
        uint64_t bytes, uint64_t *out_reclaimed) {
        auto *self = static_cast<ReclaimMock *>(cookie);
        self->check_unlocked();
        std::lock_guard<std::mutex> lk(self->mu);
        self->events.push_back({"volatile", client_id, bo_handle, bytes});
        if (self->fail_volatile) {
            *out_reclaimed = 0;
            return A840_KMD_IO_ERROR;
        }
        *out_reclaimed = bytes;
        return A840_KMD_OK;
    }

    static A840KmdStatus on_sweep_parked(
        void *cookie, uint64_t requester_client_id, uint32_t max_victim_prio,
        uint64_t need_bytes, uint64_t *out_reclaimed) {
        auto *self = static_cast<ReclaimMock *>(cookie);
        self->check_unlocked();
        std::lock_guard<std::mutex> lk(self->mu);
        self->events.push_back({"sweep_parked", requester_client_id, max_victim_prio, need_bytes});
        uint64_t r = self->bulk_parked_return;
        if (r > need_bytes)
            r = need_bytes;
        self->bulk_parked_return -= r;
        *out_reclaimed = r;
        return A840_KMD_OK;
    }

    static A840KmdStatus on_sweep_volatile(
        void *cookie, uint64_t requester_client_id, uint32_t max_victim_prio,
        uint64_t need_bytes, uint64_t *out_reclaimed) {
        auto *self = static_cast<ReclaimMock *>(cookie);
        self->check_unlocked();
        std::lock_guard<std::mutex> lk(self->mu);
        self->events.push_back({"sweep_volatile", requester_client_id, max_victim_prio, need_bytes});
        uint64_t r = self->bulk_volatile_return;
        if (r > need_bytes)
            r = need_bytes;
        self->bulk_volatile_return -= r;
        *out_reclaimed = r;
        return A840_KMD_OK;
    }

    static A840KmdStatus on_evict_bg(
        void *cookie, uint64_t victim_client_id, uint64_t need_bytes,
        uint64_t *out_reclaimed) {
        auto *self = static_cast<ReclaimMock *>(cookie);
        self->check_unlocked();
        std::lock_guard<std::mutex> lk(self->mu);
        self->events.push_back({"evict_bg", victim_client_id, 0, need_bytes});
        uint64_t r = (need_bytes < self->bg_evict_cap) ? need_bytes : self->bg_evict_cap;
        *out_reclaimed = r;
        return A840_KMD_OK;
    }

    A840KmdQuotaReclaimOps ops(bool include_bulk = false, bool include_bg_evict = false) {
        A840KmdQuotaReclaimOps o{};
        o.cookie = this;
        o.reclaim_parked_bo = &ReclaimMock::on_reclaim_parked;
        o.purge_volatile_bo = &ReclaimMock::on_purge_volatile;
        if (include_bulk) {
            o.sweep_parked_pool = &ReclaimMock::on_sweep_parked;
            o.sweep_volatile_heap = &ReclaimMock::on_sweep_volatile;
        }
        if (include_bg_evict) {
            o.evict_background_client = &ReclaimMock::on_evict_bg;
        }
        return o;
    }
};

/* ---------------- Recovery Callback Mock ---------------- */

struct FenceNotifyRecord {
    uint64_t client_id;
    uint64_t queue_id;
    uint64_t fence;
    A840KmdStatus status;
    uint32_t fault_kind;
    uint64_t recovery_generation;
};

struct RecoveryMock {
    std::mutex mu;
    CustomSyncMock *sync_to_check = nullptr;
    std::atomic<uint32_t> saw_lock_held{0};
    A840KmdStatus hw_reset_ret = A840_KMD_OK;
    std::vector<FenceNotifyRecord> fence_events;
    std::vector<std::pair<uint64_t, uint64_t>> reset_calls;
    std::vector<std::pair<uint64_t, uint32_t>> sparse_rollbacks;
    std::vector<std::pair<uint64_t, uint64_t>> bo_end_calls;

    void check_unlocked() {
        if (sync_to_check != nullptr && sync_to_check->held_by_current_thread()) {
            saw_lock_held.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static void on_fence_notify(
        void *cookie, uint64_t client_id, uint64_t queue_id, uint64_t fence,
        A840KmdStatus status, uint32_t fault_kind, uint64_t gen) {
        auto *self = static_cast<RecoveryMock *>(cookie);
        self->check_unlocked();
        std::lock_guard<std::mutex> lk(self->mu);
        self->fence_events.push_back({client_id, queue_id, fence, status, fault_kind, gen});
    }

    static A840KmdStatus on_hw_reset(
        void *cookie, uint64_t client_id, uint64_t queue_id,
        uint64_t gen, uint32_t fault_kind) {
        auto *self = static_cast<RecoveryMock *>(cookie);
        (void)gen;
        (void)fault_kind;
        self->check_unlocked();
        std::lock_guard<std::mutex> lk(self->mu);
        self->reset_calls.push_back({client_id, queue_id});
        return self->hw_reset_ret;
    }

    static void on_sparse_rollback(
        void *cookie, uint64_t client_id, uint64_t queue_id,
        uint64_t bo_handle, uint32_t unbound_tiles) {
        auto *self = static_cast<RecoveryMock *>(cookie);
        (void)client_id;
        (void)queue_id;
        self->check_unlocked();
        std::lock_guard<std::mutex> lk(self->mu);
        self->sparse_rollbacks.push_back({bo_handle, unbound_tiles});
    }

    static void on_bo_end_submit(
        void *cookie, uint64_t client_id, uint64_t bo_handle) {
        auto *self = static_cast<RecoveryMock *>(cookie);
        self->check_unlocked();
        std::lock_guard<std::mutex> lk(self->mu);
        self->bo_end_calls.push_back({client_id, bo_handle});
    }

    A840KmdRecoveryOps ops() {
        A840KmdRecoveryOps o{};
        o.cookie = this;
        o.fence_notify = &RecoveryMock::on_fence_notify;
        o.hw_channel_reset = &RecoveryMock::on_hw_reset;
        o.sparse_rollback_bo = &RecoveryMock::on_sparse_rollback;
        o.bo_end_submit = &RecoveryMock::on_bo_end_submit;
        return o;
    }
};

/* ---------------- ZeroPage + BoPool + Sparse Mock Helpers ---------------- */

struct ZeroPageMock {
    std::mutex mu;
    uint64_t next_phys = UINT64_C(268435456);
    std::atomic<uint32_t> allocs{0};
    std::atomic<uint32_t> frees{0};

    static A840KmdStatus alloc_page(
        void *cookie, uint64_t bytes, uint64_t *out_phys,
        void **out_cpu, void **out_handle) {
        auto *self = static_cast<ZeroPageMock *>(cookie);
        std::lock_guard<std::mutex> lk(self->mu);
        auto *buf = static_cast<uint8_t *>(std::calloc(1, static_cast<size_t>(bytes)));
        if (!buf)
            return A840_KMD_NO_MEMORY;
        *out_phys = self->next_phys;
        self->next_phys += bytes;
        *out_cpu = buf;
        *out_handle = buf;
        self->allocs.fetch_add(1, std::memory_order_relaxed);
        return A840_KMD_OK;
    }

    static void free_page(void *cookie, uint64_t phys, void *handle, uint64_t bytes) {
        auto *self = static_cast<ZeroPageMock *>(cookie);
        (void)phys;
        (void)bytes;
        std::free(handle);
        self->frees.fetch_add(1, std::memory_order_relaxed);
    }

    static A840KmdStatus map_ro(
        void *cookie, uint64_t cid, void *ctx, uint64_t va,
        uint64_t phys, uint64_t bytes, uint32_t perm) {
        (void)cookie; (void)cid; (void)ctx; (void)va; (void)phys; (void)bytes;
        return (perm == A840_KMD_SMMU_PERM_READ_ONLY) ? A840_KMD_OK : A840_KMD_PERMISSION_DENIED;
    }

    static A840KmdStatus unmap(void *cookie, uint64_t cid, void *ctx, uint64_t va, uint64_t bytes) {
        (void)cookie; (void)cid; (void)ctx; (void)va; (void)bytes;
        return A840_KMD_OK;
    }

    A840KmdSmmuOps ops() {
        A840KmdSmmuOps o{};
        o.cookie = this;
        o.alloc_backing_page = &ZeroPageMock::alloc_page;
        o.free_backing_page = &ZeroPageMock::free_page;
        o.map_client_ro = &ZeroPageMock::map_ro;
        o.unmap_client = &ZeroPageMock::unmap;
        return o;
    }
};

struct BoBackingMock {
    std::mutex mu;
    uint64_t next_va = UINT64_C(67108864);
    uint64_t next_phys = UINT64_C(536870912);
    std::atomic<uint32_t> allocs{0};
    std::atomic<uint32_t> frees{0};
    std::atomic<uint32_t> purges{0};
    std::atomic<uint64_t> last_purge_zero_phys{0};

    static A840KmdStatus alloc_map(
        void *cookie, uint64_t cid, void *ctx, uint64_t bytes, uint64_t align,
        uint32_t perm, uint32_t cache, uint64_t *out_va, uint64_t *out_phys,
        void **out_cpu, void **out_handle) {
        auto *self = static_cast<BoBackingMock *>(cookie);
        (void)cid; (void)ctx; (void)align; (void)perm; (void)cache;
        std::lock_guard<std::mutex> lk(self->mu);
        auto *buf = static_cast<uint8_t *>(std::calloc(1, static_cast<size_t>(bytes)));
        if (!buf)
            return A840_KMD_NO_MEMORY;
        *out_va = self->next_va;
        self->next_va += bytes + 65536u;
        *out_phys = self->next_phys;
        self->next_phys += bytes + 65536u;
        *out_cpu = buf;
        *out_handle = buf;
        self->allocs.fetch_add(1, std::memory_order_relaxed);
        return A840_KMD_OK;
    }

    static void unmap_free(
        void *cookie, uint64_t cid, void *ctx, uint64_t va,
        uint64_t phys, void *handle, uint64_t bytes) {
        auto *self = static_cast<BoBackingMock *>(cookie);
        (void)cid; (void)ctx; (void)va; (void)phys; (void)bytes;
        std::free(handle);
        self->frees.fetch_add(1, std::memory_order_relaxed);
    }

    static void purge_empty(
        void *cookie, uint64_t cid, void *ctx, uint64_t va, uint64_t phys,
        void *handle, uint64_t bytes, uint64_t zero_phys) {
        auto *self = static_cast<BoBackingMock *>(cookie);
        (void)cid; (void)ctx; (void)va; (void)phys; (void)bytes;
        std::free(handle);
        self->last_purge_zero_phys.store(zero_phys, std::memory_order_relaxed);
        self->purges.fetch_add(1, std::memory_order_relaxed);
    }

    A840KmdBoPoolBackingOps ops() {
        A840KmdBoPoolBackingOps o{};
        o.cookie = this;
        o.alloc_and_map = &BoBackingMock::alloc_map;
        o.unmap_and_free = &BoBackingMock::unmap_free;
        o.purge_to_empty = &BoBackingMock::purge_empty;
        return o;
    }
};

static A840KmdClientQuotaConfig make_client_cfg(
    uint64_t cid, A840KmdClientPriority prio, uint64_t soft, uint64_t hard) {
    A840KmdClientQuotaConfig c{};
    c.client_id = cid;
    c.priority = static_cast<uint32_t>(prio);
    c.soft_limit_bytes = soft;
    c.hard_limit_bytes = hard;
    return c;
}

static A840KmdFaultInput make_fault(
    uint64_t cid, uint64_t qid, uint64_t fence, A840KmdFaultKind kind) {
    A840KmdFaultInput f{};
    f.client_id = cid;
    f.queue_id = qid;
    f.fence = fence;
    f.ib1_pc = UINT64_C(4294967296) + fence;
    f.rbbm_status = 1234u;
    f.cp_status = 5678u;
    f.smmu_fault_va = UINT64_C(134217728);
    f.smmu_fsr = 99u;
    f.fault_kind = static_cast<uint32_t>(kind);
    f.timestamp_ticks = 900000u + fence;
    return f;
}

/* ========================================================================
 * Part 1: Lifecycle, Configuration & Multi-Client Quota (Cases 01..12)
 * ======================================================================== */

static void case_01_init_defaults_and_invalid_configs() {
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(nullptr, nullptr) == A840_KMD_INVALID_ARG);

    A840KmdQuotaRecoveryConfig bad{};
    bad.reserved = 1u;
    CHECK(a840_kmd_qr_init(&qr, &bad) == A840_KMD_INVALID_ARG);

    bad = A840KmdQuotaRecoveryConfig{};
    bad.global_hard_limit_bytes = 8u * kMiB;
    bad.global_soft_limit_bytes = 9u * kMiB;
    CHECK(a840_kmd_qr_init(&qr, &bad) == A840_KMD_INVALID_ARG);

    bad = A840KmdQuotaRecoveryConfig{};
    bad.global_hard_limit_bytes = 8u * kMiB;
    bad.compositor_reserve_bytes = 8u * kMiB;
    CHECK(a840_kmd_qr_init(&qr, &bad) == A840_KMD_INVALID_ARG);

    A840KmdQuotaSyncOps half_sync{};
    half_sync.lock = &CustomSyncMock::do_lock;
    bad = A840KmdQuotaRecoveryConfig{};
    bad.sync_ops = &half_sync;
    CHECK(a840_kmd_qr_init(&qr, &bad) == A840_KMD_INVALID_ARG);

    CHECK(a840_kmd_qr_init(&qr, nullptr) == A840_KMD_OK);
    A840KmdQuotaRecoveryStats st{};
    CHECK(a840_kmd_qr_get_stats(&qr, &st) == A840_KMD_OK);
    CHECK(st.global_hard_limit_bytes == 12u * kGiB);
    CHECK(st.global_soft_limit_bytes == 9u * kGiB);
    CHECK(st.compositor_reserve_bytes == 512u * kMiB);
    CHECK(qr.max_recoveries_before_ban == A840_KMD_RECOVERY_DEFAULT_MAX_RECOVERIES);
    a840_kmd_qr_destroy(&qr);
}

static void case_02_uninitialized_operations_fail_closed() {
    A840KmdQuotaRecovery qr{};
    std::memset(&qr, 0, sizeof(qr));
    auto cc = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &cc) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_qr_request_alloc(&qr, 1, kKiB, nullptr) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 1, nullptr, 0) == A840_KMD_STATE_CONFLICT);

    CHECK(a840_kmd_qr_init(&qr, nullptr) == A840_KMD_OK);
    a840_kmd_qr_destroy(&qr);
    CHECK(a840_kmd_qr_register_client(&qr, &cc) == A840_KMD_STATE_CONFLICT);
    a840_kmd_qr_destroy(&qr); /* idempotent double-destroy */
}

static void case_03_register_64_clients_and_capacity_reject() {
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, nullptr) == A840_KMD_OK);

    auto bad_cid = make_client_cfg(0, A840_KMD_PRIO_BACKGROUND, kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &bad_cid) == A840_KMD_INVALID_ARG);
    auto bad_lim = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 3u * kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &bad_lim) == A840_KMD_INVALID_ARG);
    auto bad_prio = make_client_cfg(1, A840_KMD_PRIO_COUNT, kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &bad_prio) == A840_KMD_INVALID_ARG);

    for (uint64_t i = 1; i <= A840_KMD_QUOTA_MAX_CLIENTS; ++i) {
        auto c = make_client_cfg(i, static_cast<A840KmdClientPriority>(i % 4u), kMiB, 4u * kMiB);
        CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    }
    auto dup = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &dup) == A840_KMD_STATE_CONFLICT);
    auto extra = make_client_cfg(65, A840_KMD_PRIO_BACKGROUND, kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &extra) == A840_KMD_CAPACITY);

    A840KmdQuotaRecoveryStats st{};
    CHECK(a840_kmd_qr_get_stats(&qr, &st) == A840_KMD_OK);
    CHECK(st.active_clients == 64u);
    a840_kmd_qr_destroy(&qr);
}

static void case_04_update_client_quota_and_unregister_reclaims_bytes() {
    RecoveryMock rec_mock;
    CustomSyncMock sync_mock;
    rec_mock.sync_to_check = &sync_mock;
    auto sync_ops = sync_mock.ops();
    auto rec_ops = rec_mock.ops();

    A840KmdQuotaRecoveryConfig cfg{};
    cfg.sync_ops = &sync_ops;
    cfg.recovery_ops = &rec_ops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(10, A840_KMD_PRIO_FOREGROUND_APP, 2u * kMiB, 4u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_update_client_quota(&qr, 10, 3u * kMiB, 6u * kMiB) == A840_KMD_OK);
    CHECK(a840_kmd_qr_update_client_quota(&qr, 10, 7u * kMiB, 6u * kMiB) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_qr_request_alloc(&qr, 10, 5u * kMiB, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 10, 1001, 2u * kMiB, 0, A840_PURGE_NONVOLATILE, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 10, 0) == A840_KMD_OK);
    uint64_t bo = 1001;
    CHECK(a840_kmd_qr_submit_begin(&qr, 10, 0, 7, &bo, 1) == A840_KMD_OK);

    CHECK(a840_kmd_qr_unregister_client(&qr, 10) == A840_KMD_OK);
    CHECK(rec_mock.saw_lock_held.load() == 0u);
    CHECK(rec_mock.fence_events.size() == 1u);
    CHECK(rec_mock.fence_events[0].status == A840_KMD_DISCONNECTED);
    CHECK(rec_mock.fence_events[0].fence == 7u);

    A840KmdQuotaRecoveryStats st{};
    CHECK(a840_kmd_qr_get_stats(&qr, &st) == A840_KMD_OK);
    CHECK(st.active_clients == 0u);
    CHECK(st.active_tracked_bos == 0u);
    CHECK(st.active_channels == 0u);
    CHECK(st.global_used_bytes == 0u);
    CHECK(sync_mock.locks.load() == sync_mock.unlocks.load());
    a840_kmd_qr_destroy(&qr);
}

static void case_05_basic_alloc_release_and_overflow_guards() {
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, nullptr) == A840_KMD_OK);
    auto c = make_client_cfg(1, A840_KMD_PRIO_FOREGROUND_APP, 4u * kMiB, 8u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);

    A840KmdAllocResult res{};
    CHECK(a840_kmd_qr_request_alloc(&qr, 1, 3u * kMiB, &res) == A840_KMD_OK);
    CHECK(res.admitted == 1u);
    CHECK(res.client_used_after == 3u * kMiB);
    CHECK(a840_kmd_qr_request_alloc(&qr, 1, UINT64_MAX - kMiB, &res) == A840_KMD_OOM);
    CHECK(res.admitted == 0u);
    CHECK(a840_kmd_qr_release_bytes(&qr, 1, 4u * kMiB) == A840_KMD_RANGE);
    CHECK(a840_kmd_qr_release_bytes(&qr, 1, 3u * kMiB) == A840_KMD_OK);

    A840KmdClientQuotaInfo info{};
    CHECK(a840_kmd_qr_query_client(&qr, 1, &info) == A840_KMD_OK);
    CHECK(info.used_bytes == 0u);
    CHECK(info.peak_used_bytes == 3u * kMiB);
    a840_kmd_qr_destroy(&qr);
}

static void case_06_track_bo_validation_and_cross_client_reject() {
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, nullptr) == A840_KMD_OK);
    auto c1 = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 4u * kMiB, 8u * kMiB);
    auto c2 = make_client_cfg(2, A840_KMD_PRIO_BACKGROUND, 4u * kMiB, 8u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &c2) == A840_KMD_OK);

    CHECK(a840_kmd_qr_track_bo(&qr, 1, 100, kMiB, 1, A840_PURGE_EMPTY, 0) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 100, kMiB, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 100, kMiB, 0, A840_PURGE_VOLATILE, 0) == A840_KMD_STATE_CONFLICT);

    CHECK(a840_kmd_qr_set_bo_parked(&qr, 2, 100, 1) == A840_KMD_PERMISSION_DENIED);
    CHECK(a840_kmd_qr_set_bo_purgeable(&qr, 2, 100, A840_PURGE_NONVOLATILE) == A840_KMD_PERMISSION_DENIED);
    CHECK(a840_kmd_qr_bo_begin_submit(&qr, 2, 100, nullptr) == A840_KMD_PERMISSION_DENIED);
    CHECK(a840_kmd_qr_untrack_bo(&qr, 2, 100, 1) == A840_KMD_PERMISSION_DENIED);
    CHECK(a840_kmd_qr_untrack_bo(&qr, 1, 100, 1) == A840_KMD_OK);
    a840_kmd_qr_destroy(&qr);
}

static void case_07_soft_limit_triggers_parked_reclaim_before_volatile() {
    ReclaimMock rm;
    auto rops = rm.ops();
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.reclaim_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 3u * kMiB, 4u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 101, 2u * kMiB, 1, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 102, 2u * kMiB, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);

    A840KmdAllocResult res{};
    CHECK(a840_kmd_qr_request_alloc(&qr, 1, 1u * kMiB, &res) == A840_KMD_OK);
    CHECK(res.admitted == 1u);
    CHECK(res.reclaimed_parked_bytes == 2u * kMiB);
    CHECK(res.parked_reclaimed_count == 1u);
    CHECK(res.reclaimed_volatile_bytes == 0u);
    CHECK(res.volatile_purged_count == 0u);
    CHECK(res.client_used_after == 3u * kMiB);
    CHECK(rm.events.size() == 1u);
    CHECK(rm.events[0].kind == "parked" && rm.events[0].bo_handle == 101u);
    a840_kmd_qr_destroy(&qr);
}

static void case_08_ladder_parked_then_volatile_lru_order() {
    ReclaimMock rm;
    auto rops = rm.ops();
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.reclaim_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(1, A840_KMD_PRIO_FOREGROUND_APP, 2u * kMiB, 4u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 201, 1u * kMiB, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 202, 1u * kMiB, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 203, 1u * kMiB, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 204, 1u * kMiB, 1, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);

    /* Touch 201 so LRU order becomes: 202 (oldest), 203, 201 (newest). */
    CHECK(a840_kmd_qr_set_bo_purgeable(&qr, 1, 201, A840_PURGE_VOLATILE) == A840_KMD_OK);

    /* Request 1 MiB -> needs 3 MiB to get back <= soft(2 MiB):
     * Stage 1 takes Parked 204 (1M), Stage 2 takes VOLATILE 202 (1M) then 203 (1M), leaving 201! */
    A840KmdAllocResult res{};
    CHECK(a840_kmd_qr_request_alloc(&qr, 1, 1u * kMiB, &res) == A840_KMD_OK);
    CHECK(res.parked_reclaimed_count == 1u);
    CHECK(res.volatile_purged_count == 2u);
    CHECK(res.client_used_after == 2u * kMiB);
    CHECK(rm.events.size() == 3u);
    CHECK(rm.events[0].kind == "parked" && rm.events[0].bo_handle == 204u);
    CHECK(rm.events[1].kind == "volatile" && rm.events[1].bo_handle == 202u);
    CHECK(rm.events[2].kind == "volatile" && rm.events[2].bo_handle == 203u);
    a840_kmd_qr_destroy(&qr);
}

static void case_09_inflight_volatile_bo_strictly_immune_to_oom() {
    ReclaimMock rm;
    auto rops = rm.ops();
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.reclaim_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 2u * kMiB, 4u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 301, 4u * kMiB, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);
    uint32_t refs = 0;
    CHECK(a840_kmd_qr_bo_begin_submit(&qr, 1, 301, &refs) == A840_KMD_OK);
    CHECK(refs == 1u);

    A840KmdAllocResult res{};
    CHECK(a840_kmd_qr_request_alloc(&qr, 1, 1u * kMiB, &res) == A840_KMD_OOM);
    CHECK(res.volatile_purged_count == 0u);
    CHECK(res.volatile_skipped_inflight == 1u);
    CHECK(rm.events.empty());

    /* Once GPU finishes, in_flight_refs == 0 and BO becomes reclaimable. */
    CHECK(a840_kmd_qr_bo_end_submit(&qr, 1, 301, &refs) == A840_KMD_OK);
    CHECK(refs == 0u);
    CHECK(a840_kmd_qr_request_alloc(&qr, 1, 1u * kMiB, &res) == A840_KMD_OK);
    CHECK(res.volatile_purged_count == 1u);
    CHECK(res.reclaimed_volatile_bytes == 4u * kMiB);
    a840_kmd_qr_destroy(&qr);
}

static void case_10_inflight_multi_refcount_protection() {
    ReclaimMock rm;
    auto rops = rm.ops();
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.reclaim_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 1u * kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 310, 2u * kMiB, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);

    uint32_t refs = 0;
    CHECK(a840_kmd_qr_bo_begin_submit(&qr, 1, 310, &refs) == A840_KMD_OK && refs == 1u);
    CHECK(a840_kmd_qr_bo_begin_submit(&qr, 1, 310, &refs) == A840_KMD_OK && refs == 2u);
    CHECK(a840_kmd_qr_bo_end_submit(&qr, 1, 310, &refs) == A840_KMD_OK && refs == 1u);

    CHECK(a840_kmd_qr_request_alloc(&qr, 1, 1u * kMiB, nullptr) == A840_KMD_OOM);
    CHECK(rm.events.empty());

    CHECK(a840_kmd_qr_bo_end_submit(&qr, 1, 310, &refs) == A840_KMD_OK && refs == 0u);
    CHECK(a840_kmd_qr_bo_end_submit(&qr, 1, 310, &refs) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_qr_request_alloc(&qr, 1, 1u * kMiB, nullptr) == A840_KMD_OK);
    CHECK(rm.events.size() == 1u);
    a840_kmd_qr_destroy(&qr);
}

static void case_11_nonvolatile_never_purged_and_bg_oom_reject() {
    ReclaimMock rm;
    auto rops = rm.ops();
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.reclaim_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 2u * kMiB, 4u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 401, 4u * kMiB, 0, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);

    CHECK(a840_kmd_qr_request_alloc(&qr, 1, kMiB, nullptr) == A840_KMD_OOM);
    CHECK(rm.events.empty());

    A840KmdClientQuotaInfo ci{};
    CHECK(a840_kmd_qr_query_client(&qr, 1, &ci) == A840_KMD_OK);
    CHECK(ci.oom_reject_count == 1u);
    A840KmdQuotaRecoveryStats st{};
    CHECK(a840_kmd_qr_get_stats(&qr, &st) == A840_KMD_OK);
    CHECK(st.oom_rejects_by_prio[A840_KMD_PRIO_BACKGROUND] == 1u);
    a840_kmd_qr_destroy(&qr);
}

static void case_12_reclaim_callback_failure_rolls_back_reclaiming_flag() {
    ReclaimMock rm;
    rm.fail_parked = true;
    rm.fail_volatile = true;
    auto rops = rm.ops();
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.reclaim_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 1u * kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 501, 1u * kMiB, 1, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 502, 1u * kMiB, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);

    CHECK(a840_kmd_qr_request_alloc(&qr, 1, 1u * kMiB, nullptr) == A840_KMD_OOM);

    /* Slots must NOT be stuck reclaiming=1: untrack/re-alloc must work once callbacks succeed. */
    rm.fail_parked = false;
    rm.fail_volatile = false;
    CHECK(a840_kmd_qr_request_alloc(&qr, 1, 1u * kMiB, nullptr) == A840_KMD_OK);
    a840_kmd_qr_destroy(&qr);
}

/* ========================================================================
 * Part 2: Priority Hierarchy & COMPOSITOR Anti-Starvation (Cases 13..22)
 * ======================================================================== */

static void case_13_compositor_reserve_blocks_non_compositor_clients() {
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.global_hard_limit_bytes = 16u * kMiB;
    cfg.global_soft_limit_bytes = 12u * kMiB;
    cfg.compositor_reserve_bytes = 4u * kMiB;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto bg = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 16u * kMiB, 16u * kMiB);
    auto fg = make_client_cfg(2, A840_KMD_PRIO_FOREGROUND_APP, 16u * kMiB, 16u * kMiB);
    auto cam = make_client_cfg(3, A840_KMD_PRIO_CAMERA, 16u * kMiB, 16u * kMiB);
    auto comp = make_client_cfg(4, A840_KMD_PRIO_COMPOSITOR, 16u * kMiB, 16u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &bg) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &fg) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &cam) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &comp) == A840_KMD_OK);

    CHECK(a840_kmd_qr_request_alloc(&qr, 1, 6u * kMiB, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_qr_request_alloc(&qr, 2, 6u * kMiB, nullptr) == A840_KMD_OK);
    /* Global used is 12 MiB == 16 MiB - 4 MiB reserve: non-compositors MUST get OOM! */
    CHECK(a840_kmd_qr_request_alloc(&qr, 1, kMiB, nullptr) == A840_KMD_OOM);
    CHECK(a840_kmd_qr_request_alloc(&qr, 2, kMiB, nullptr) == A840_KMD_OOM);
    CHECK(a840_kmd_qr_request_alloc(&qr, 3, kMiB, nullptr) == A840_KMD_OOM);

    /* COMPOSITOR MUST succeed using its protected 4 MiB reserve! */
    A840KmdAllocResult res{};
    CHECK(a840_kmd_qr_request_alloc(&qr, 4, 4u * kMiB, &res) == A840_KMD_OK);
    CHECK(res.admitted == 1u);
    CHECK(res.used_compositor_reserve == 1u);
    CHECK(res.global_used_after == 16u * kMiB);
    A840KmdQuotaRecoveryStats st{};
    CHECK(a840_kmd_qr_get_stats(&qr, &st) == A840_KMD_OK);
    CHECK(st.compositor_protected_allocs == 1u);
    a840_kmd_qr_destroy(&qr);
}

static void case_14_compositor_reclaims_lower_prio_parked_in_priority_order() {
    ReclaimMock rm;
    auto rops = rm.ops();
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.global_hard_limit_bytes = 8u * kMiB;
    cfg.global_soft_limit_bytes = 6u * kMiB;
    cfg.compositor_reserve_bytes = 2u * kMiB;
    cfg.reclaim_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto bg = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 8u * kMiB, 8u * kMiB);
    auto fg = make_client_cfg(2, A840_KMD_PRIO_FOREGROUND_APP, 8u * kMiB, 8u * kMiB);
    auto cam = make_client_cfg(3, A840_KMD_PRIO_CAMERA, 8u * kMiB, 8u * kMiB);
    auto comp = make_client_cfg(4, A840_KMD_PRIO_COMPOSITOR, 8u * kMiB, 8u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &bg) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &fg) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &cam) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &comp) == A840_KMD_OK);

    CHECK(a840_kmd_qr_track_bo(&qr, 4, 44, 2u * kMiB, 1, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 3, 33, 1u * kMiB, 1, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 2, 22, 1u * kMiB, 1, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 11, 1u * kMiB, 1, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);
    /* Global used = 5 MiB; COMPOSITOR asks for 4 MiB -> needs 3 MiB to reach soft(6M). */
    CHECK(a840_kmd_qr_request_alloc(&qr, 4, 4u * kMiB, nullptr) == A840_KMD_OK);
    CHECK(rm.events.size() == 3u);
    CHECK(rm.events[0].client_id == 1u && rm.events[0].bo_handle == 11u); /* BACKGROUND first */
    CHECK(rm.events[1].client_id == 2u && rm.events[1].bo_handle == 22u); /* FOREGROUND second */
    CHECK(rm.events[2].client_id == 3u && rm.events[2].bo_handle == 33u); /* CAMERA third, COMPOSITOR 44 untouched! */
    a840_kmd_qr_destroy(&qr);
}

static void case_15_compositor_reclaims_lower_prio_volatile_before_own() {
    ReclaimMock rm;
    auto rops = rm.ops();
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.global_hard_limit_bytes = 6u * kMiB;
    cfg.global_soft_limit_bytes = 4u * kMiB;
    cfg.compositor_reserve_bytes = 2u * kMiB;
    cfg.reclaim_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto bg = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 6u * kMiB, 6u * kMiB);
    auto comp = make_client_cfg(4, A840_KMD_PRIO_COMPOSITOR, 6u * kMiB, 6u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &bg) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &comp) == A840_KMD_OK);

    CHECK(a840_kmd_qr_track_bo(&qr, 4, 400, 2u * kMiB, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 100, 2u * kMiB, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);

    CHECK(a840_kmd_qr_request_alloc(&qr, 4, 2u * kMiB, nullptr) == A840_KMD_OK);
    CHECK(rm.events.size() == 1u);
    CHECK(rm.events[0].kind == "volatile" && rm.events[0].client_id == 1u);
    a840_kmd_qr_destroy(&qr);
}

static void case_16_lower_prio_never_reclaims_higher_prio_or_compositor() {
    ReclaimMock rm;
    auto rops = rm.ops();
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.global_hard_limit_bytes = 6u * kMiB;
    cfg.global_soft_limit_bytes = 4u * kMiB;
    cfg.compositor_reserve_bytes = 2u * kMiB;
    cfg.reclaim_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto bg = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 6u * kMiB, 6u * kMiB);
    auto cam = make_client_cfg(3, A840_KMD_PRIO_CAMERA, 6u * kMiB, 6u * kMiB);
    auto comp = make_client_cfg(4, A840_KMD_PRIO_COMPOSITOR, 6u * kMiB, 6u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &bg) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &cam) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &comp) == A840_KMD_OK);

    CHECK(a840_kmd_qr_track_bo(&qr, 4, 401, 2u * kMiB, 1, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 3, 301, 2u * kMiB, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);

    /* Non-compositor ceiling is 4 MiB; BG tries to allocate 1 MiB -> must OOM without touching CAM or COMP! */
    CHECK(a840_kmd_qr_request_alloc(&qr, 1, kMiB, nullptr) == A840_KMD_OOM);
    CHECK(rm.events.empty());
    a840_kmd_qr_destroy(&qr);
}

static void case_17_camera_reclaims_fg_and_bg_but_never_compositor() {
    ReclaimMock rm;
    auto rops = rm.ops();
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.global_hard_limit_bytes = 8u * kMiB;
    cfg.global_soft_limit_bytes = 4u * kMiB;
    cfg.compositor_reserve_bytes = 2u * kMiB;
    cfg.reclaim_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto bg = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 8u * kMiB, 8u * kMiB);
    auto fg = make_client_cfg(2, A840_KMD_PRIO_FOREGROUND_APP, 8u * kMiB, 8u * kMiB);
    auto cam = make_client_cfg(3, A840_KMD_PRIO_CAMERA, 8u * kMiB, 8u * kMiB);
    auto comp = make_client_cfg(4, A840_KMD_PRIO_COMPOSITOR, 8u * kMiB, 8u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &bg) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &fg) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &cam) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &comp) == A840_KMD_OK);

    CHECK(a840_kmd_qr_track_bo(&qr, 4, 41, 2u * kMiB, 1, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 2, 21, 2u * kMiB, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 11, 2u * kMiB, 1, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);

    /* Global used = 6 MiB (ceiling for CAM = 6 MiB); CAM asks for 3 MiB -> reclaims BG parked(2M) + FG volatile(2M), never COMP! */
    CHECK(a840_kmd_qr_request_alloc(&qr, 3, 3u * kMiB, nullptr) == A840_KMD_OK);
    CHECK(rm.events.size() == 2u);
    CHECK(rm.events[0].client_id == 1u && rm.events[0].kind == "parked");
    CHECK(rm.events[1].client_id == 2u && rm.events[1].kind == "volatile");
    a840_kmd_qr_destroy(&qr);
}

static void case_18_compositor_last_resort_bg_evict_callback() {
    ReclaimMock rm;
    auto rops = rm.ops(false, true);
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.global_hard_limit_bytes = 8u * kMiB;
    cfg.global_soft_limit_bytes = 6u * kMiB;
    cfg.compositor_reserve_bytes = 2u * kMiB;
    cfg.reclaim_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto bg = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 8u * kMiB, 8u * kMiB);
    auto comp = make_client_cfg(4, A840_KMD_PRIO_COMPOSITOR, 8u * kMiB, 8u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &bg) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &comp) == A840_KMD_OK);

    CHECK(a840_kmd_qr_track_bo(&qr, 1, 10, 6u * kMiB, 0, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);
    A840KmdAllocResult res{};
    CHECK(a840_kmd_qr_request_alloc(&qr, 4, 4u * kMiB, &res) == A840_KMD_OK);
    CHECK(res.admitted == 1u);
    CHECK(res.reclaimed_bg_evict_bytes == 2u * kMiB);
    CHECK(rm.events.size() == 1u);
    CHECK(rm.events[0].kind == "evict_bg" && rm.events[0].client_id == 1u);
    a840_kmd_qr_destroy(&qr);
}

static void case_19_bulk_sweep_parked_and_volatile_callbacks() {
    ReclaimMock rm;
    rm.bulk_parked_return = 1u * kMiB;
    rm.bulk_volatile_return = 2u * kMiB;
    auto rops = rm.ops(true, false);
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.reclaim_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(1, A840_KMD_PRIO_FOREGROUND_APP, 2u * kMiB, 4u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_request_alloc(&qr, 1, 4u * kMiB, nullptr) == A840_KMD_OK);

    A840KmdAllocResult res{};
    CHECK(a840_kmd_qr_request_alloc(&qr, 1, 2u * kMiB, &res) == A840_KMD_OK);
    CHECK(res.reclaimed_parked_bytes == 1u * kMiB);
    CHECK(res.reclaimed_volatile_bytes == 2u * kMiB);
    CHECK(res.client_used_after == 3u * kMiB);
    CHECK(rm.events.size() == 2u);
    CHECK(rm.events[0].kind == "sweep_parked");
    CHECK(rm.events[1].kind == "sweep_volatile");
    a840_kmd_qr_destroy(&qr);
}

static void case_20_custom_sync_lock_balance_and_all_reclaim_callbacks_outside_lock() {
    CustomSyncMock sm;
    ReclaimMock rm;
    rm.sync_to_check = &sm;
    rm.bulk_parked_return = 512u * kKiB;
    rm.bulk_volatile_return = 512u * kKiB;
    auto sops = sm.ops();
    auto rops = rm.ops(true, true);

    A840KmdQuotaRecoveryConfig cfg{};
    cfg.global_hard_limit_bytes = 6u * kMiB;
    cfg.global_soft_limit_bytes = 3u * kMiB;
    cfg.compositor_reserve_bytes = 1u * kMiB;
    cfg.sync_ops = &sops;
    cfg.reclaim_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto bg = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 6u * kMiB, 6u * kMiB);
    auto comp = make_client_cfg(4, A840_KMD_PRIO_COMPOSITOR, 6u * kMiB, 6u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &bg) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &comp) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 1, 1u * kMiB, 1, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 2, 1u * kMiB, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 3, 3u * kMiB, 0, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);

    CHECK(a840_kmd_qr_request_alloc(&qr, 4, 5u * kMiB, nullptr) == A840_KMD_OK);
    CHECK(rm.saw_lock_held.load() == 0u);
    CHECK(sm.locks.load() == sm.unlocks.load());
    CHECK(sm.locks.load() > 0u);
    a840_kmd_qr_destroy(&qr);
}

static void case_21_real_bo_pool_linkage_parked_and_volatile_purge() {
    ZeroPageMock zpm;
    auto zops = zpm.ops();
    A840KmdZeroPageManager zp{};
    CHECK(a840_kmd_zero_page_init(&zp, &zops, UINT64_C(65536)) == A840_KMD_OK);
    NativeAdrenoAbiZeroPageReplyV3 zrep{};
    CHECK(a840_kmd_zero_page_bind_client(&zp, 1, nullptr, 0, A840_KMD_SMMU_PERM_READ_ONLY, &zrep) == A840_KMD_OK);

    BoBackingMock bbm;
    auto bops = bbm.ops();
    A840KmdBoPoolConfig bcfg{};
    bcfg.zero_page_mgr = &zp;
    bcfg.backing_ops = &bops;
    A840KmdBoPool pool{};
    CHECK(a840_kmd_bo_pool_init(&pool, &bcfg) == A840_KMD_OK);

    A840KmdBoAcquireReq req{};
    req.client_id = 1;
    req.requested_bytes = 1u * kMiB;
    req.alignment = 4096u;
    req.permissions = A840_KMD_SMMU_PERM_READ_WRITE;
    req.cache_mode = A840_KMD_BO_CACHE_COHERENT;

    A840KmdBoDesc d_vol{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d_vol) == A840_KMD_OK);
    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d_vol.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_OK);

    A840KmdQuotaRecoveryConfig qcfg{};
    qcfg.bo_pool = &pool;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &qcfg) == A840_KMD_OK);
    auto c = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 512u * kKiB, 1u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, d_vol.bo_handle, d_vol.bucket_bytes, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);

    /* Requesting 1 MiB triggers real a840_kmd_bo_pool_set_purgeable(EMPTY) outside lock! */
    A840KmdAllocResult ares{};
    CHECK(a840_kmd_qr_request_alloc(&qr, 1, 1u * kMiB, &ares) == A840_KMD_OK);
    CHECK(ares.volatile_purged_count == 1u);
    CHECK(bbm.purges.load() == 1u);
    A840KmdBoDesc after{};
    CHECK(a840_kmd_bo_pool_query_bo(&pool, 1, d_vol.bo_handle, &after) == A840_KMD_OK);
    CHECK(after.purgeable_state == A840_PURGE_EMPTY);

    a840_kmd_qr_destroy(&qr);
    a840_kmd_bo_pool_destroy(&pool);
    a840_kmd_zero_page_destroy(&zp);
    CHECK(zpm.allocs.load() == zpm.frees.load());
}

static void case_22_empty_bo_submit_rejected_and_untrack_while_inflight_rejected() {
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, nullptr) == A840_KMD_OK);
    auto c = make_client_cfg(1, A840_KMD_PRIO_FOREGROUND_APP, 4u * kMiB, 8u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_OK);

    CHECK(a840_kmd_qr_track_bo(&qr, 1, 10, kMiB, 0, A840_PURGE_EMPTY, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 11, kMiB, 1, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 12, kMiB, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);

    uint64_t b10 = 10, b11 = 11, b12 = 12;
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 1, &b10, 1) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 1, &b11, 1) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 1, &b12, 1) == A840_KMD_OK);

    CHECK(a840_kmd_qr_untrack_bo(&qr, 1, 12, 1) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_qr_set_bo_purgeable(&qr, 1, 12, A840_PURGE_EMPTY) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_qr_set_bo_parked(&qr, 1, 12, 1) == A840_KMD_STATE_CONFLICT);

    CHECK(a840_kmd_qr_submit_complete(&qr, 1, 0, 1, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_qr_untrack_bo(&qr, 1, 12, 1) == A840_KMD_OK);
    a840_kmd_qr_destroy(&qr);
}

/* ========================================================================
 * Part 3: Single-Channel Fault Isolation & Recovery (Cases 23..38)
 * ======================================================================== */

static void case_23_channel_open_close_and_validation() {
    RecoveryMock rm;
    auto rops = rm.ops();
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.recovery_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    CHECK(a840_kmd_qr_open_channel(&qr, 99, 0) == A840_KMD_NOT_FOUND);
    auto c = make_client_cfg(1, A840_KMD_PRIO_FOREGROUND_APP, kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 1, nullptr, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_close_channel(&qr, 1, 0) == A840_KMD_OK);
    CHECK(rm.fence_events.size() == 1u && rm.fence_events[0].status == A840_KMD_DISCONNECTED);
    CHECK(a840_kmd_qr_close_channel(&qr, 1, 0) == A840_KMD_NOT_FOUND);
    a840_kmd_qr_destroy(&qr);
}

static void case_24_healthy_submit_and_cumulative_fence_complete() {
    RecoveryMock rm;
    auto rops = rm.ops();
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.recovery_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(1, A840_KMD_PRIO_COMPOSITOR, 4u * kMiB, 8u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_OK);

    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 10, nullptr, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 10, nullptr, 0) == A840_KMD_RANGE);
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 9, nullptr, 0) == A840_KMD_RANGE);
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 20, nullptr, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 30, nullptr, 0) == A840_KMD_OK);

    uint32_t done = 0;
    CHECK(a840_kmd_qr_submit_complete(&qr, 1, 0, 20, &done) == A840_KMD_OK);
    CHECK(done == 2u);
    CHECK(rm.fence_events.size() == 2u);
    CHECK(rm.fence_events[0].status == A840_KMD_OK && rm.fence_events[1].status == A840_KMD_OK);

    A840KmdChannelInfo ci{};
    CHECK(a840_kmd_qr_query_channel(&qr, 1, 0, &ci) == A840_KMD_OK);
    CHECK(ci.active_inflight_count == 1u);
    CHECK(ci.completed_ok_count == 2u);
    CHECK(ci.last_completed_fence == 20u);
    a840_kmd_qr_destroy(&qr);
}

static void case_25_step_by_step_faulting_resetting_recovered_transitions() {
    RecoveryMock rm;
    auto rops = rm.ops();
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.recovery_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(1, A840_KMD_PRIO_FOREGROUND_APP, 4u * kMiB, 8u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 5, nullptr, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 8, nullptr, 0) == A840_KMD_OK);

    auto fi = make_fault(1, 0, 5, A840_KMD_FAULT_GPU_HANG);
    A840KmdRecoveryReport r1{}, r2{}, r3{};
    CHECK(a840_kmd_qr_report_channel_fault(&qr, &fi, 42u, &r1) == A840_KMD_OK);
    CHECK(r1.old_state == A840_KMD_CHANNEL_HEALTHY);
    CHECK(r1.final_state == A840_KMD_CHANNEL_FAULTING);
    CHECK(r1.fault_code == 42u);
    CHECK(r1.damaged_fence_start == 5u && r1.damaged_fence_end == 8u);

    CHECK(a840_kmd_qr_begin_channel_reset(&qr, 1, 0, &r2) == A840_KMD_OK);
    CHECK(r2.old_state == A840_KMD_CHANNEL_FAULTING);
    CHECK(r2.final_state == A840_KMD_CHANNEL_RESETTING);
    CHECK(r2.aborted_submits == 2u);
    CHECK(r2.hw_reset_status == A840_KMD_OK);

    CHECK(a840_kmd_qr_finish_channel_reset(&qr, 1, 0, r2.hw_reset_status, &r3) == A840_KMD_OK);
    CHECK(r3.old_state == A840_KMD_CHANNEL_RESETTING);
    CHECK(r3.final_state == A840_KMD_CHANNEL_RECOVERED);
    CHECK(r3.recovery_generation == 1u);

    A840KmdChannelInfo ci{};
    CHECK(a840_kmd_qr_query_channel(&qr, 1, 0, &ci) == A840_KMD_OK);
    CHECK(ci.state == A840_KMD_CHANNEL_RECOVERED);
    CHECK(ci.recovery_generation == 1u);
    CHECK(ci.completed_err_count == 2u);
    CHECK(ci.active_inflight_count == 0u);
    a840_kmd_qr_destroy(&qr);
}

static void case_26_submits_rejected_during_faulting_and_resetting_then_allowed_in_recovered() {
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, nullptr) == A840_KMD_OK);
    auto c = make_client_cfg(1, A840_KMD_PRIO_FOREGROUND_APP, kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 1, nullptr, 0) == A840_KMD_OK);

    auto fi = make_fault(1, 0, 1, A840_KMD_FAULT_IB_ERROR);
    CHECK(a840_kmd_qr_report_channel_fault(&qr, &fi, 7u, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 2, nullptr, 0) == A840_KMD_POISONED);
    CHECK(a840_kmd_qr_submit_complete(&qr, 1, 0, 1, nullptr) == A840_KMD_POISONED);

    CHECK(a840_kmd_qr_begin_channel_reset(&qr, 1, 0, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 2, nullptr, 0) == A840_KMD_STATE_CONFLICT);

    CHECK(a840_kmd_qr_finish_channel_reset(&qr, 1, 0, A840_KMD_OK, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 2, nullptr, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_submit_complete(&qr, 1, 0, 2, nullptr) == A840_KMD_OK);
    a840_kmd_qr_destroy(&qr);
}

static void case_27_invalid_state_transitions_rejected() {
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, nullptr) == A840_KMD_OK);
    auto c = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_OK);

    CHECK(a840_kmd_qr_begin_channel_reset(&qr, 1, 0, nullptr) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_qr_finish_channel_reset(&qr, 1, 0, A840_KMD_OK, nullptr) == A840_KMD_STATE_CONFLICT);

    auto fi = make_fault(1, 0, 1, A840_KMD_FAULT_GPU_HANG);
    CHECK(a840_kmd_qr_report_channel_fault(&qr, &fi, 1u, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_qr_report_channel_fault(&qr, &fi, 1u, nullptr) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_qr_finish_channel_reset(&qr, 1, 0, A840_KMD_OK, nullptr) == A840_KMD_STATE_CONFLICT);
    a840_kmd_qr_destroy(&qr);
}

static void case_28_damaged_fence_range_and_deterministic_error_notifications() {
    RecoveryMock rm;
    auto rops = rm.ops();
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.recovery_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(1, A840_KMD_PRIO_FOREGROUND_APP, 4u * kMiB, 8u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 900, kMiB, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);

    uint64_t bo = 900;
    const uint64_t fences[5] = {10, 12, 15, 18, 22};
    for (uint64_t f : fences)
        CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, f, &bo, 1) == A840_KMD_OK);

    auto fi = make_fault(1, 0, 12, A840_KMD_FAULT_SMMU_TRANSLATION);
    A840KmdRecoveryReport rep{};
    CHECK(a840_kmd_qr_handle_channel_fault(&qr, &fi, 99u, &rep) == A840_KMD_OK);
    CHECK(rep.damaged_fence_start == 10u && rep.damaged_fence_end == 22u);
    CHECK(rep.aborted_submits == 5u);
    CHECK(rep.released_bo_refs == 5u);
    CHECK(rm.fence_events.size() == 5u);
    for (const auto &ev : rm.fence_events) {
        CHECK(ev.status == A840_KMD_POISONED);
        CHECK(ev.fault_kind == A840_KMD_FAULT_SMMU_TRANSLATION);
        CHECK(ev.recovery_generation == 1u);
    }
    /* All 5 BO in_flight_refs must be released -> BO can now be untracked cleanly! */
    CHECK(a840_kmd_qr_untrack_bo(&qr, 1, 900, 1) == A840_KMD_OK);
    a840_kmd_qr_destroy(&qr);
}

static void case_29_healthy_client_zero_interruption_during_peer_fault() {
    RecoveryMock rm;
    auto rops = rm.ops();
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.recovery_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto comp = make_client_cfg(1, A840_KMD_PRIO_COMPOSITOR, 8u * kMiB, 16u * kMiB);
    auto cam = make_client_cfg(2, A840_KMD_PRIO_CAMERA, 8u * kMiB, 16u * kMiB);
    auto bad = make_client_cfg(3, A840_KMD_PRIO_BACKGROUND, 4u * kMiB, 8u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &comp) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &cam) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &bad) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 2, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 3, 0) == A840_KMD_OK);

    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 1, nullptr, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_submit_begin(&qr, 2, 0, 1, nullptr, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_submit_begin(&qr, 3, 0, 1, nullptr, 0) == A840_KMD_OK);

    auto fi = make_fault(3, 0, 1, A840_KMD_FAULT_GPU_HANG);
    CHECK(a840_kmd_qr_report_channel_fault(&qr, &fi, 1u, nullptr) == A840_KMD_OK);

    /* During FAULTING: healthy clients 1 and 2 continue uninterrupted! */
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 2, nullptr, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_submit_complete(&qr, 2, 0, 1, nullptr) == A840_KMD_OK);

    CHECK(a840_kmd_qr_begin_channel_reset(&qr, 3, 0, nullptr) == A840_KMD_OK);
    /* During RESETTING: healthy clients 1 and 2 continue uninterrupted! */
    CHECK(a840_kmd_qr_submit_complete(&qr, 1, 0, 2, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_qr_submit_begin(&qr, 2, 0, 2, nullptr, 0) == A840_KMD_OK);

    CHECK(a840_kmd_qr_finish_channel_reset(&qr, 3, 0, A840_KMD_OK, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_qr_submit_complete(&qr, 2, 0, 2, nullptr) == A840_KMD_OK);

    A840KmdChannelInfo ci1{}, ci2{};
    CHECK(a840_kmd_qr_query_channel(&qr, 1, 0, &ci1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_query_channel(&qr, 2, 0, &ci2) == A840_KMD_OK);
    CHECK(ci1.state == A840_KMD_CHANNEL_HEALTHY && ci1.completed_ok_count == 2u && ci1.completed_err_count == 0u);
    CHECK(ci2.state == A840_KMD_CHANNEL_HEALTHY && ci2.completed_ok_count == 2u && ci2.completed_err_count == 0u);

    for (const auto &ev : rm.fence_events) {
        if (ev.client_id == 1u || ev.client_id == 2u)
            CHECK(ev.status == A840_KMD_OK);
    }
    a840_kmd_qr_destroy(&qr);
}

static void case_30_same_client_second_queue_zero_crosstalk() {
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, nullptr) == A840_KMD_OK);
    auto c = make_client_cfg(5, A840_KMD_PRIO_FOREGROUND_APP, 4u * kMiB, 8u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 5, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 5, 1) == A840_KMD_OK);

    CHECK(a840_kmd_qr_submit_begin(&qr, 5, 0, 1, nullptr, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_submit_begin(&qr, 5, 1, 1, nullptr, 0) == A840_KMD_OK);

    auto fi = make_fault(5, 0, 1, A840_KMD_FAULT_IB_ERROR);
    CHECK(a840_kmd_qr_handle_channel_fault(&qr, &fi, 3u, nullptr) == A840_KMD_OK);

    A840KmdChannelInfo q1{};
    CHECK(a840_kmd_qr_query_channel(&qr, 5, 1, &q1) == A840_KMD_OK);
    CHECK(q1.state == A840_KMD_CHANNEL_HEALTHY && q1.active_inflight_count == 1u);
    CHECK(a840_kmd_qr_submit_complete(&qr, 5, 1, 1, nullptr) == A840_KMD_OK);
    a840_kmd_qr_destroy(&qr);
}

static void case_31_consecutive_faults_trigger_banned_circuit_breaker() {
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.max_recoveries_before_ban = 3u;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_OK);

    for (uint64_t iter = 1; iter <= 3; ++iter) {
        CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, iter, nullptr, 0) == A840_KMD_OK);
        auto fi = make_fault(1, 0, iter, A840_KMD_FAULT_GPU_HANG);
        A840KmdRecoveryReport rep{};
        CHECK(a840_kmd_qr_handle_channel_fault(&qr, &fi, 10u, &rep) == A840_KMD_OK);
        CHECK(rep.final_state == A840_KMD_CHANNEL_RECOVERED);
        CHECK(rep.recovery_generation == iter);
    }

    /* 4th fault exceeds threshold=3 -> BANNED! */
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 4, nullptr, 0) == A840_KMD_OK);
    auto fi4 = make_fault(1, 0, 4, A840_KMD_FAULT_GPU_HANG);
    A840KmdRecoveryReport rep4{};
    CHECK(a840_kmd_qr_handle_channel_fault(&qr, &fi4, 10u, &rep4) == A840_KMD_OK);
    CHECK(rep4.final_state == A840_KMD_CHANNEL_BANNED);
    CHECK(rep4.recovery_generation == 4u);

    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 5, nullptr, 0) == A840_KMD_POISONED);
    CHECK(a840_kmd_qr_report_channel_fault(&qr, &fi4, 10u, nullptr) == A840_KMD_POISONED);
    a840_kmd_qr_destroy(&qr);
}

static void case_32_ban_on_first_fault_threshold_zero() {
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.ban_on_first_fault = 1u;
    cfg.max_recoveries_before_ban = 0u;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_OK);

    auto fi = make_fault(1, 0, 1, A840_KMD_FAULT_SMMU_PERMISSION);
    A840KmdRecoveryReport rep{};
    CHECK(a840_kmd_qr_handle_channel_fault(&qr, &fi, 55u, &rep) == A840_KMD_OK);
    CHECK(rep.final_state == A840_KMD_CHANNEL_BANNED);
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 2, nullptr, 0) == A840_KMD_POISONED);
    a840_kmd_qr_destroy(&qr);
}

static void case_33_hw_reset_failure_forces_banned() {
    RecoveryMock rm;
    rm.hw_reset_ret = A840_KMD_IO_ERROR;
    auto rops = rm.ops();
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.recovery_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(1, A840_KMD_PRIO_FOREGROUND_APP, kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_OK);

    auto fi = make_fault(1, 0, 1, A840_KMD_FAULT_GPU_HANG);
    A840KmdRecoveryReport rep{};
    CHECK(a840_kmd_qr_handle_channel_fault(&qr, &fi, 77u, &rep) == A840_KMD_OK);
    CHECK(rep.hw_reset_status == A840_KMD_IO_ERROR);
    CHECK(rep.final_state == A840_KMD_CHANNEL_BANNED);
    a840_kmd_qr_destroy(&qr);
}

static void case_34_fault_capture_linkage_records_hw_snapshot_outside_lock() {
    CustomSyncMock sm;
    auto sops = sm.ops();
    A840KmdFaultCapture fc{};
    CHECK(a840_kmd_fault_init(&fc) == A840_KMD_OK);

    A840KmdQuotaRecoveryConfig cfg{};
    cfg.sync_ops = &sops;
    cfg.fault_capture = &fc;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(7, A840_KMD_PRIO_FOREGROUND_APP, kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 7, 2) == A840_KMD_OK);

    auto fi = make_fault(7, 2, 19, A840_KMD_FAULT_SMMU_TRANSLATION);
    A840KmdRecoveryReport rep{};
    CHECK(a840_kmd_qr_handle_channel_fault(&qr, &fi, 101u, &rep) == A840_KMD_OK);
    CHECK(rep.fault_snapshot.sequence == 1u);
    CHECK(rep.fault_snapshot.smmu_fault_va == UINT64_C(134217728));
    CHECK(a840_kmd_fault_ring_count(&fc) == 1u);
    CHECK(sm.locks.load() == sm.unlocks.load());

    a840_kmd_qr_destroy(&qr);
    a840_kmd_fault_destroy(&fc);
}

static void case_35_sparse_va_linkage_rolls_back_faulting_bos_to_ro_zero_page() {
    ZeroPageMock zpm;
    auto zops = zpm.ops();
    A840KmdZeroPageManager zp{};
    CHECK(a840_kmd_zero_page_init(&zp, &zops, UINT64_C(65536)) == A840_KMD_OK);
    NativeAdrenoAbiZeroPageReplyV3 zrep{};
    CHECK(a840_kmd_zero_page_bind_client(&zp, 1, nullptr, 0, A840_KMD_SMMU_PERM_READ_ONLY, &zrep) == A840_KMD_OK);
    CHECK(a840_kmd_zero_page_bind_client(&zp, 2, nullptr, 0, A840_KMD_SMMU_PERM_READ_ONLY, &zrep) == A840_KMD_OK);

    A840KmdSparseConfig scfg{};
    scfg.zero_page_mgr = &zp;
    A840KmdSparseManager smgr{};
    CHECK(a840_kmd_sparse_init(&smgr, &scfg) == A840_KMD_OK);

    /* Client 1 (faulting) and Client 2 (healthy) both bind 2 sparse tiles. */
    uint64_t r1 = 0, r2 = 0;
    CHECK(a840_kmd_sparse_register_bo(&smgr, 1, 701, UINT64_C(536870912), 2u * 65536u, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&smgr, 2, 702, UINT64_C(537001984), 2u * 65536u, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_region_create(&smgr, 1, nullptr, UINT64_C(1048576), 2u * 65536u, &r1) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_region_create(&smgr, 2, nullptr, UINT64_C(1048576), 2u * 65536u, &r2) == A840_KMD_OK);

    auto bind_two = [&](uint64_t cid, uint64_t rid, uint64_t bo) {
        A840KmdSparseBindReqV3 req{};
        A840KmdSparseBindReplyV3 rep{};
        req.version = NativeAdrenoAbiVersionV3;
        req.op_count = 1u;
        req.region_id = rid;
        req.ops[0].op = A840_KMD_SPARSE_OP_BIND_BO;
        req.ops[0].flags = A840_KMD_SMMU_PERM_READ_WRITE;
        req.ops[0].gpu_va = UINT64_C(1048576);
        req.ops[0].length_bytes = 2u * 65536u;
        req.ops[0].bo_handle = bo;
        req.ops[0].bo_offset = 0u;
        CHECK(a840_kmd_sparse_dispatch_v3(&smgr, cid, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_OK);
    };
    bind_two(1, r1, 701);
    bind_two(2, r2, 702);

    A840KmdQuotaRecoveryConfig qcfg{};
    qcfg.sparse_mgr = &smgr;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &qcfg) == A840_KMD_OK);
    auto c1 = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 4u * kMiB, 8u * kMiB);
    auto c2 = make_client_cfg(2, A840_KMD_PRIO_COMPOSITOR, 4u * kMiB, 8u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &c2) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 2, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_channel_attach_sparse_bo(&qr, 1, 0, 701) == A840_KMD_OK);

    auto fi = make_fault(1, 0, 1, A840_KMD_FAULT_SMMU_TRANSLATION);
    A840KmdRecoveryReport rep{};
    CHECK(a840_kmd_qr_handle_channel_fault(&qr, &fi, 200u, &rep) == A840_KMD_OK);
    CHECK(rep.rolled_back_sparse_bos == 1u);
    CHECK(rep.rolled_back_sparse_tiles == 2u);

    /* Client 1 tiles rolled back to RO zero page; Client 2 tiles remain bound! */
    A840KmdSparseTileInfo ti1{}, ti2{};
    CHECK(a840_kmd_sparse_query_tile(&smgr, 1, UINT64_C(1048576), &ti1) == A840_KMD_OK);
    CHECK(ti1.is_zero_page == 1u && ti1.permissions == A840_KMD_SMMU_PERM_READ_ONLY);
    uint32_t rc1 = 99u;
    CHECK(a840_kmd_sparse_get_bo_refcount(&smgr, 1, 701, &rc1) == A840_KMD_OK && rc1 == 0u);

    CHECK(a840_kmd_sparse_query_tile(&smgr, 2, UINT64_C(1048576), &ti2) == A840_KMD_OK);
    CHECK(ti2.is_zero_page == 0u && ti2.bo_handle == 702u);

    a840_kmd_qr_destroy(&qr);
    a840_kmd_sparse_destroy(&smgr);
    a840_kmd_zero_page_destroy(&zp);
}

static void case_36_bo_pool_inflight_sync_zero_leak_on_fault() {
    BoBackingMock bbm;
    auto bops = bbm.ops();
    A840KmdBoPoolConfig bcfg{};
    bcfg.backing_ops = &bops;
    A840KmdBoPool pool{};
    CHECK(a840_kmd_bo_pool_init(&pool, &bcfg) == A840_KMD_OK);

    A840KmdBoAcquireReq req{};
    req.client_id = 1;
    req.requested_bytes = 64u * kKiB;
    req.alignment = 4096u;
    req.permissions = A840_KMD_SMMU_PERM_READ_WRITE;
    req.cache_mode = A840_KMD_BO_CACHE_COHERENT;
    A840KmdBoDesc desc{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &desc) == A840_KMD_OK);

    A840KmdQuotaRecoveryConfig qcfg{};
    qcfg.bo_pool = &pool;
    qcfg.sync_bo_pool_inflight = 1u;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &qcfg) == A840_KMD_OK);
    auto c = make_client_cfg(1, A840_KMD_PRIO_FOREGROUND_APP, kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_OK);

    uint64_t bo = desc.bo_handle;
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 1, &bo, 1) == A840_KMD_OK);
    A840KmdBoDesc mid{};
    CHECK(a840_kmd_bo_pool_query_bo(&pool, 1, bo, &mid) == A840_KMD_OK);
    CHECK(mid.in_flight_refs == 1u);

    auto fi = make_fault(1, 0, 1, A840_KMD_FAULT_GPU_HANG);
    CHECK(a840_kmd_qr_handle_channel_fault(&qr, &fi, 11u, nullptr) == A840_KMD_OK);

    A840KmdBoDesc after{};
    CHECK(a840_kmd_bo_pool_query_bo(&pool, 1, bo, &after) == A840_KMD_OK);
    CHECK(after.in_flight_refs == 0u);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, bo, 1u) == A840_KMD_OK);

    a840_kmd_qr_destroy(&qr);
    a840_kmd_bo_pool_destroy(&pool);
    CHECK(bbm.allocs.load() == bbm.frees.load());
}

static void case_37_recovery_callbacks_strictly_outside_custom_lock() {
    CustomSyncMock sm;
    RecoveryMock rm;
    rm.sync_to_check = &sm;
    auto sops = sm.ops();
    auto rops = rm.ops();

    A840KmdQuotaRecoveryConfig cfg{};
    cfg.sync_ops = &sops;
    cfg.recovery_ops = &rops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(1, A840_KMD_PRIO_FOREGROUND_APP, 2u * kMiB, 4u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 801, kMiB, 0, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);
    uint64_t bo = 801;
    CHECK(a840_kmd_qr_submit_begin(&qr, 1, 0, 1, &bo, 1) == A840_KMD_OK);

    auto fi = make_fault(1, 0, 1, A840_KMD_FAULT_GPU_HANG);
    CHECK(a840_kmd_qr_handle_channel_fault(&qr, &fi, 1u, nullptr) == A840_KMD_OK);
    CHECK(rm.saw_lock_held.load() == 0u);
    CHECK(rm.fence_events.size() == 1u);
    CHECK(rm.reset_calls.size() == 1u);
    CHECK(rm.sparse_rollbacks.size() == 1u);
    CHECK(rm.bo_end_calls.size() == 1u);
    CHECK(sm.locks.load() == sm.unlocks.load());
    a840_kmd_qr_destroy(&qr);
}

static void case_38_all_fault_kinds_hang_smmu_perm_ib_error() {
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.max_recoveries_before_ban = 10u;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);
    auto c = make_client_cfg(1, A840_KMD_PRIO_FOREGROUND_APP, kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_OK);

    auto bad_none = make_fault(1, 0, 1, A840_KMD_FAULT_NONE);
    CHECK(a840_kmd_qr_handle_channel_fault(&qr, &bad_none, 0u, nullptr) == A840_KMD_INVALID_ARG);
    A840KmdFaultInput bad_hi = bad_none;
    bad_hi.fault_kind = 99u;
    CHECK(a840_kmd_qr_handle_channel_fault(&qr, &bad_hi, 0u, nullptr) == A840_KMD_INVALID_ARG);

    const A840KmdFaultKind kinds[4] = {
        A840_KMD_FAULT_GPU_HANG,
        A840_KMD_FAULT_SMMU_TRANSLATION,
        A840_KMD_FAULT_SMMU_PERMISSION,
        A840_KMD_FAULT_IB_ERROR
    };
    for (uint32_t i = 0; i < 4u; ++i) {
        auto fi = make_fault(1, 0, i + 1u, kinds[i]);
        A840KmdRecoveryReport rep{};
        CHECK(a840_kmd_qr_handle_channel_fault(&qr, &fi, i + 10u, &rep) == A840_KMD_OK);
        CHECK(rep.fault_kind == static_cast<uint32_t>(kinds[i]));
        CHECK(rep.final_state == A840_KMD_CHANNEL_RECOVERED);
        CHECK(rep.recovery_generation == i + 1u);
    }
    a840_kmd_qr_destroy(&qr);
}

/* ========================================================================
 * Part 4: backboardd CA_DISABLE_GENERIC_SHADERS=1 Policy (Cases 39..54)
 * ======================================================================== */

static A840KmdCaEnvPolicyInput make_valid_ca_input() {
    A840KmdCaEnvPolicyInput in{};
    in.target_label = A840_KMD_CA_TARGET_BACKBOARDD;
    in.native_metal_enabled = 1u;
    in.phase4_pack_verified = 1u;
    in.reserved = 0u;
    return in;
}

static void case_39_ca_env_evaluate_backboardd_allow() {
    auto in = make_valid_ca_input();
    A840KmdCaEnvDecision dec{};
    CHECK(a840_kmd_ca_env_evaluate(&in, &dec) == A840_KMD_OK);
    CHECK(dec.should_inject == 1u);
    CHECK(dec.already_present == 0u);
    CHECK(dec.reason == A840_KMD_CA_ALLOW_INJECT);
    CHECK(std::strcmp(dec.env_key, "CA_DISABLE_GENERIC_SHADERS") == 0);
    CHECK(std::strcmp(dec.env_value, "1") == 0);
    CHECK(std::strcmp(dec.env_entry, "CA_DISABLE_GENERIC_SHADERS=1") == 0);
}

static void case_40_ca_env_evaluate_rejects_non_whitelisted_targets() {
    const char *bad_targets[] = {
        "com.apple.SpringBoard",
        "com.apple.backboardd.helper",
        "COM.APPLE.BACKBOARDD",
        "backboardd",
        "",
        "com.apple.mediaserverd"
    };
    for (const char *lbl : bad_targets) {
        auto in = make_valid_ca_input();
        in.target_label = lbl;
        A840KmdCaEnvDecision dec{};
        CHECK(a840_kmd_ca_env_evaluate(&in, &dec) == A840_KMD_PERMISSION_DENIED);
        CHECK(dec.should_inject == 0u);
        CHECK(dec.reason == A840_KMD_CA_REJECT_TARGET_NOT_WHITELISTED);
        CHECK(dec.env_entry == nullptr);
    }
}

static void case_41_ca_env_evaluate_rejects_unmet_preconditions() {
    A840KmdCaEnvDecision dec{};
    auto in = make_valid_ca_input();
    in.native_metal_enabled = 0u;
    CHECK(a840_kmd_ca_env_evaluate(&in, &dec) == A840_KMD_STATE_CONFLICT);
    CHECK(dec.reason == A840_KMD_CA_REJECT_NATIVE_DISABLED && dec.should_inject == 0u);

    in = make_valid_ca_input();
    in.phase4_pack_verified = 0u;
    CHECK(a840_kmd_ca_env_evaluate(&in, &dec) == A840_KMD_STATE_CONFLICT);
    CHECK(dec.reason == A840_KMD_CA_REJECT_PACK_UNVERIFIED && dec.should_inject == 0u);

    in = make_valid_ca_input();
    in.native_metal_enabled = 2u;
    CHECK(a840_kmd_ca_env_evaluate(&in, &dec) == A840_KMD_INVALID_ARG);
    CHECK(dec.reason == A840_KMD_CA_REJECT_INVALID_ARG);

    in = make_valid_ca_input();
    in.reserved = 1u;
    CHECK(a840_kmd_ca_env_evaluate(&in, &dec) == A840_KMD_INVALID_ARG);
}

static void case_42_ca_env_evaluate_rejects_illegal_control_chars() {
    const char *bad[] = {
        "com.apple.backboardd\n",
        "com.apple.backboardd\r",
        "com.apple.backboardd\t",
        "com.apple.backboardd ",
        "com.apple.\001backboardd",
        "com.apple.backboardd\x7f"
    };
    for (const char *lbl : bad) {
        auto in = make_valid_ca_input();
        in.target_label = lbl;
        A840KmdCaEnvDecision dec{};
        CHECK(a840_kmd_ca_env_evaluate(&in, &dec) == A840_KMD_INVALID_ARG);
        CHECK(dec.reason == A840_KMD_CA_REJECT_ILLEGAL_CHAR);
    }
}

static void case_43_ca_env_build_env_block_appends_cleanly() {
    auto in = make_valid_ca_input();
    const char *existing[] = {"PATH=/usr/bin", "MTL_HUD_ENABLED=0"};
    char buf[256];
    size_t written = 0, entries = 0;
    A840KmdCaEnvDecision dec{};
    CHECK(a840_kmd_ca_env_build_env_block(&in, existing, 2, buf, sizeof(buf), &written, &entries, &dec) == A840_KMD_OK);
    CHECK(entries == 3u);
    CHECK(dec.reason == A840_KMD_CA_ALLOW_INJECT && dec.already_present == 0u);
    const char *p = buf;
    CHECK(std::strcmp(p, "PATH=/usr/bin") == 0);
    p += std::strlen(p) + 1;
    CHECK(std::strcmp(p, "MTL_HUD_ENABLED=0") == 0);
    p += std::strlen(p) + 1;
    CHECK(std::strcmp(p, "CA_DISABLE_GENERIC_SHADERS=1") == 0);
    p += std::strlen(p) + 1;
    CHECK(*p == '\0');
    CHECK(static_cast<size_t>((p + 1) - buf) == written);
}

static void case_44_ca_env_build_env_block_idempotent_when_already_1() {
    auto in = make_valid_ca_input();
    const char *existing[] = {"FOO=bar", "CA_DISABLE_GENERIC_SHADERS=1"};
    char buf[128];
    size_t written = 0, entries = 0;
    A840KmdCaEnvDecision dec{};
    CHECK(a840_kmd_ca_env_build_env_block(&in, existing, 2, buf, sizeof(buf), &written, &entries, &dec) == A840_KMD_OK);
    CHECK(entries == 2u);
    CHECK(dec.already_present == 1u && dec.reason == A840_KMD_CA_ALLOW_ALREADY_PRESENT);
    CHECK(written == std::strlen("FOO=bar") + 1u + std::strlen("CA_DISABLE_GENERIC_SHADERS=1") + 2u);
}

static void case_45_ca_env_build_env_block_rejects_conflicts_and_preserves_buf() {
    auto in = make_valid_ca_input();
    const char *conflicts[] = {
        "CA_DISABLE_GENERIC_SHADERS=0",
        "CA_DISABLE_GENERIC_SHADERS=",
        "CA_DISABLE_GENERIC_SHADERS=true",
        "CA_DISABLE_GENERIC_SHADERS=1 ",
        "ca_disable_generic_shaders=1"
    };
    for (const char *cstr : conflicts) {
        const char *existing[] = {"A=1", cstr};
        char buf[128];
        std::memset(buf, 0x5A, sizeof(buf));
        char snap[128];
        std::memcpy(snap, buf, sizeof(buf));
        A840KmdCaEnvDecision dec{};
        CHECK(a840_kmd_ca_env_build_env_block(&in, existing, 2, buf, sizeof(buf), nullptr, nullptr, &dec) == A840_KMD_STATE_CONFLICT);
        CHECK(dec.reason == A840_KMD_CA_REJECT_CONFLICTING_VALUE);
        CHECK(std::memcmp(buf, snap, sizeof(buf)) == 0);
    }
}

static void case_46_ca_env_build_env_block_rejects_malformed_and_control_chars() {
    auto in = make_valid_ca_input();
    const char *malformed[] = {"", "NO_EQUALS", "=noname", "BAD-KEY=1", "KEY=bad\nval", "KEY=bad\tval", "KEY=\x80"};
    for (const char *m : malformed) {
        const char *existing[] = {m};
        char buf[128];
        std::memset(buf, 0x33, sizeof(buf));
        char snap[128];
        std::memcpy(snap, buf, sizeof(buf));
        A840KmdCaEnvDecision dec{};
        CHECK(a840_kmd_ca_env_build_env_block(&in, existing, 1, buf, sizeof(buf), nullptr, nullptr, &dec) == A840_KMD_INVALID_ARG);
        CHECK(std::memcmp(buf, snap, sizeof(buf)) == 0);
    }
}

static void case_47_ca_env_build_env_block_rejects_small_buffer_zero_truncation() {
    auto in = make_valid_ca_input();
    const char *existing[] = {"A=1"};
    const size_t exact = std::strlen("A=1") + 1u + std::strlen(A840_KMD_CA_ENV_ENTRY) + 2u;
    char buf[128];
    std::memset(buf, 0x77, sizeof(buf));
    char snap[128];
    std::memcpy(snap, buf, sizeof(buf));
    A840KmdCaEnvDecision dec{};
    CHECK(a840_kmd_ca_env_build_env_block(&in, existing, 1, buf, exact - 1u, nullptr, nullptr, &dec) == A840_KMD_RANGE);
    CHECK(dec.reason == A840_KMD_CA_REJECT_BUFFER_TOO_SMALL);
    CHECK(std::memcmp(buf, snap, sizeof(buf)) == 0);

    size_t written = 0;
    CHECK(a840_kmd_ca_env_build_env_block(&in, existing, 1, buf, exact, &written, nullptr, &dec) == A840_KMD_OK);
    CHECK(written == exact);
}

static void case_48_ca_env_build_envp_allow_idempotent_and_rejects() {
    auto in = make_valid_ca_input();
    const char *existing[] = {"X=1", "Y=2"};
    const char *out_envp[4] = {"SENTINEL", "SENTINEL", "SENTINEL", "SENTINEL"};
    size_t cnt = 0;
    A840KmdCaEnvDecision dec{};

    /* Cap 3 too small for 3 entries + NULL sentinel: must leave out_envp untouched! */
    CHECK(a840_kmd_ca_env_build_envp(&in, existing, 2, out_envp, 3, &cnt, &dec) == A840_KMD_RANGE);
    CHECK(std::strcmp(out_envp[0], "SENTINEL") == 0);

    CHECK(a840_kmd_ca_env_build_envp(&in, existing, 2, out_envp, 4, &cnt, &dec) == A840_KMD_OK);
    CHECK(cnt == 3u);
    CHECK(std::strcmp(out_envp[0], "X=1") == 0);
    CHECK(std::strcmp(out_envp[1], "Y=2") == 0);
    CHECK(std::strcmp(out_envp[2], "CA_DISABLE_GENERIC_SHADERS=1") == 0);
    CHECK(out_envp[3] == nullptr);

    /* Idempotent when already present. */
    const char *already[] = {"X=1", "CA_DISABLE_GENERIC_SHADERS=1"};
    CHECK(a840_kmd_ca_env_build_envp(&in, already, 2, out_envp, 3, &cnt, &dec) == A840_KMD_OK);
    CHECK(cnt == 2u && dec.already_present == 1u && out_envp[2] == nullptr);
}

static void case_49_ca_env_append_boot_args_empty_and_existing() {
    auto in = make_valid_ca_input();
    char out[256];
    size_t len = 0;
    A840KmdCaEnvDecision dec{};

    CHECK(a840_kmd_ca_env_append_boot_args(&in, "", out, sizeof(out), &len, &dec) == A840_KMD_OK);
    CHECK(std::strcmp(out, "CA_DISABLE_GENERIC_SHADERS=1") == 0 && len == std::strlen(out));

    CHECK(a840_kmd_ca_env_append_boot_args(&in, "-v serial=3", out, sizeof(out), &len, &dec) == A840_KMD_OK);
    CHECK(std::strcmp(out, "-v serial=3 CA_DISABLE_GENERIC_SHADERS=1") == 0 && len == std::strlen(out));

    CHECK(a840_kmd_ca_env_append_boot_args(&in, "-v ", out, sizeof(out), &len, &dec) == A840_KMD_OK);
    CHECK(std::strcmp(out, "-v CA_DISABLE_GENERIC_SHADERS=1") == 0);
}

static void case_50_ca_env_append_boot_args_inplace_and_idempotent() {
    auto in = make_valid_ca_input();
    char buf[256];
    std::strcpy(buf, "-v debug=0x1");
    size_t len = 0;
    A840KmdCaEnvDecision dec{};
    CHECK(a840_kmd_ca_env_append_boot_args(&in, buf, buf, sizeof(buf), &len, &dec) == A840_KMD_OK);
    CHECK(std::strcmp(buf, "-v debug=0x1 CA_DISABLE_GENERIC_SHADERS=1") == 0);
    CHECK(dec.already_present == 0u);

    /* Second in-place call is idempotent. */
    CHECK(a840_kmd_ca_env_append_boot_args(&in, buf, buf, sizeof(buf), &len, &dec) == A840_KMD_OK);
    CHECK(std::strcmp(buf, "-v debug=0x1 CA_DISABLE_GENERIC_SHADERS=1") == 0);
    CHECK(dec.already_present == 1u && dec.reason == A840_KMD_CA_ALLOW_ALREADY_PRESENT);
}

static void case_51_ca_env_append_boot_args_rejects_conflict_preserves_inplace() {
    auto in = make_valid_ca_input();
    const char *bad[] = {
        "-v CA_DISABLE_GENERIC_SHADERS=0 serial=3",
        "-v ca_disable_generic_shaders=1",
        "-v CA_DISABLE_GENERIC_SHADERS"
    };
    for (const char *s : bad) {
        char buf[256];
        std::strcpy(buf, s);
        A840KmdCaEnvDecision dec{};
        CHECK(a840_kmd_ca_env_append_boot_args(&in, buf, buf, sizeof(buf), nullptr, &dec) == A840_KMD_STATE_CONFLICT);
        CHECK(dec.reason == A840_KMD_CA_REJECT_CONFLICTING_VALUE);
        CHECK(std::strcmp(buf, s) == 0);
    }
}

static void case_52_ca_env_append_boot_args_rejects_control_chars_preserves_inplace() {
    auto in = make_valid_ca_input();
    const char *bad[] = {"-v\nserial=3", "-v\tserial=3", "-v\rserial=3", "-v \x1b", "-v \x80"};
    for (const char *s : bad) {
        char buf[128];
        std::strcpy(buf, s);
        A840KmdCaEnvDecision dec{};
        CHECK(a840_kmd_ca_env_append_boot_args(&in, buf, buf, sizeof(buf), nullptr, &dec) == A840_KMD_INVALID_ARG);
        CHECK(dec.reason == A840_KMD_CA_REJECT_ILLEGAL_CHAR);
        CHECK(std::strcmp(buf, s) == 0);
    }
}

static void case_53_ca_env_append_boot_args_darwinboot_1023_boundary() {
    auto in = make_valid_ca_input();
    const size_t entry_len = std::strlen(A840_KMD_CA_ENV_ENTRY);
    /* Exact fit: prefix + ' ' + entry == 1023 */
    const size_t fit_prefix_len = A840_KMD_CA_MAX_BOOT_ARGS_LEN - 1u - entry_len;
    std::string fit_prefix(fit_prefix_len, 'a');
    char out[1024];
    size_t len = 0;
    A840KmdCaEnvDecision dec{};
    CHECK(a840_kmd_ca_env_append_boot_args(&in, fit_prefix.c_str(), out, sizeof(out), &len, &dec) == A840_KMD_OK);
    CHECK(len == A840_KMD_CA_MAX_BOOT_ARGS_LEN);

    /* Overflow by 1 byte -> 1024 exceeds 1023 limit -> fail-closed, preserve out! */
    std::string over_prefix(fit_prefix_len + 1u, 'b');
    char inplace[1024];
    std::strcpy(inplace, over_prefix.c_str());
    CHECK(a840_kmd_ca_env_append_boot_args(&in, inplace, inplace, sizeof(inplace), nullptr, &dec) == A840_KMD_RANGE);
    CHECK(dec.reason == A840_KMD_CA_REJECT_BOOT_ARGS_TOO_LONG);
    CHECK(std::strcmp(inplace, over_prefix.c_str()) == 0);
}

static void case_54_ca_env_append_boot_args_unmet_precondition_preserves_existing() {
    char buf[128];
    std::strcpy(buf, "-v keep=1");
    A840KmdCaEnvDecision dec{};

    auto in = make_valid_ca_input();
    in.phase4_pack_verified = 0u;
    CHECK(a840_kmd_ca_env_append_boot_args(&in, buf, buf, sizeof(buf), nullptr, &dec) == A840_KMD_STATE_CONFLICT);
    CHECK(std::strcmp(buf, "-v keep=1") == 0);

    in = make_valid_ca_input();
    in.target_label = "com.apple.SpringBoard";
    CHECK(a840_kmd_ca_env_append_boot_args(&in, buf, buf, sizeof(buf), nullptr, &dec) == A840_KMD_PERMISSION_DENIED);
    CHECK(std::strcmp(buf, "-v keep=1") == 0);

    uint32_t has_en = 99u;
    A840KmdCaEnvReason reason = A840_KMD_CA_REASON_NONE;
    CHECK(a840_kmd_ca_env_validate_boot_args("-v CA_DISABLE_GENERIC_SHADERS=1", &has_en, &reason) == A840_KMD_OK);
    CHECK(has_en == 1u);
    CHECK(a840_kmd_ca_env_validate_boot_args("-v other=1", &has_en, &reason) == A840_KMD_OK);
    CHECK(has_en == 0u);
}

/* ========================================================================
 * Part 5: End-to-End Integration & Multi-Threaded Stress (Cases 55..62)
 * ======================================================================== */

static void case_55_e2e_quota_bo_pool_sparse_fault_combined_lifecycle() {
    ZeroPageMock zpm;
    auto zops = zpm.ops();
    A840KmdZeroPageManager zp{};
    CHECK(a840_kmd_zero_page_init(&zp, &zops, UINT64_C(65536)) == A840_KMD_OK);
    NativeAdrenoAbiZeroPageReplyV3 zrep{};
    for (uint64_t cid = 1; cid <= 4; ++cid)
        CHECK(a840_kmd_zero_page_bind_client(&zp, cid, nullptr, 0, A840_KMD_SMMU_PERM_READ_ONLY, &zrep) == A840_KMD_OK);

    BoBackingMock bbm;
    auto bops = bbm.ops();
    A840KmdBoPoolConfig bcfg{};
    bcfg.zero_page_mgr = &zp;
    bcfg.backing_ops = &bops;
    A840KmdBoPool pool{};
    CHECK(a840_kmd_bo_pool_init(&pool, &bcfg) == A840_KMD_OK);

    A840KmdSparseConfig scfg{};
    scfg.zero_page_mgr = &zp;
    A840KmdSparseManager smgr{};
    CHECK(a840_kmd_sparse_init(&smgr, &scfg) == A840_KMD_OK);

    A840KmdFaultCapture fc{};
    CHECK(a840_kmd_fault_init(&fc) == A840_KMD_OK);

    RecoveryMock rm;
    auto rec_ops = rm.ops();
    A840KmdQuotaRecoveryConfig qcfg{};
    qcfg.global_hard_limit_bytes = 8u * kMiB;
    qcfg.global_soft_limit_bytes = 5u * kMiB;
    qcfg.compositor_reserve_bytes = 2u * kMiB;
    qcfg.sync_bo_pool_inflight = 1u;
    qcfg.bo_pool = &pool;
    qcfg.sparse_mgr = &smgr;
    qcfg.fault_capture = &fc;
    qcfg.recovery_ops = &rec_ops;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &qcfg) == A840_KMD_OK);

    auto c_bg = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 4u * kMiB, 6u * kMiB);
    auto c_cam = make_client_cfg(3, A840_KMD_PRIO_CAMERA, 4u * kMiB, 6u * kMiB);
    auto c_comp = make_client_cfg(4, A840_KMD_PRIO_COMPOSITOR, 6u * kMiB, 8u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c_bg) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &c_cam) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &c_comp) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 3, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 4, 0) == A840_KMD_OK);

    /* BG acquires a 4 MiB VOLATILE BO in real BoPool. */
    A840KmdBoAcquireReq breq{};
    breq.client_id = 1;
    breq.requested_bytes = 4u * kMiB;
    breq.alignment = 4096u;
    breq.permissions = A840_KMD_SMMU_PERM_READ_WRITE;
    breq.cache_mode = A840_KMD_BO_CACHE_COHERENT;
    A840KmdBoDesc bg_bo{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &breq, &bg_bo) == A840_KMD_OK);
    A840KmdPurgeResult bg_pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, bg_bo.bo_handle, A840_PURGE_VOLATILE, &bg_pr) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, bg_bo.bo_handle, bg_bo.bucket_bytes, 0, A840_PURGE_VOLATILE, 1) == A840_KMD_OK);

    /* CAMERA acquires a 1 MiB BO, maps it into Sparse VA, and submits it. */
    breq.client_id = 3;
    breq.requested_bytes = 1u * kMiB;
    A840KmdBoDesc cam_bo{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &breq, &cam_bo) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 3, cam_bo.bo_handle, cam_bo.bucket_bytes, 0, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&smgr, 3, cam_bo.bo_handle, UINT64_C(536870912), 1u * kMiB, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);
    uint64_t cam_reg = 0;
    CHECK(a840_kmd_sparse_region_create(&smgr, 3, nullptr, UINT64_C(2097152), 2u * 65536u, &cam_reg) == A840_KMD_OK);
    A840KmdSparseBindReqV3 sreq{};
    A840KmdSparseBindReplyV3 srep{};
    sreq.version = NativeAdrenoAbiVersionV3;
    sreq.op_count = 1u;
    sreq.region_id = cam_reg;
    sreq.ops[0].op = A840_KMD_SPARSE_OP_BIND_BO;
    sreq.ops[0].flags = A840_KMD_SMMU_PERM_READ_WRITE;
    sreq.ops[0].gpu_va = UINT64_C(2097152);
    sreq.ops[0].length_bytes = 2u * 65536u;
    sreq.ops[0].bo_handle = cam_bo.bo_handle;
    CHECK(a840_kmd_sparse_dispatch_v3(&smgr, 3, &sreq, sizeof(sreq), &srep, sizeof(srep)) == A840_KMD_OK);

    /* COMPOSITOR submits healthy frame & requests 3 MiB (reclaims BG's 4 MiB VOLATILE BO!). */
    CHECK(a840_kmd_qr_submit_begin(&qr, 4, 0, 1, nullptr, 0) == A840_KMD_OK);
    A840KmdAllocResult comp_res{};
    CHECK(a840_kmd_qr_request_alloc(&qr, 4, 3u * kMiB, &comp_res) == A840_KMD_OK);
    CHECK(comp_res.volatile_purged_count == 1u);

    /* CAMERA faults while in-flight -> Sparse tiles roll back to RO zero page, COMPOSITOR unaffected! */
    uint64_t cam_h = cam_bo.bo_handle;
    CHECK(a840_kmd_qr_submit_begin(&qr, 3, 0, 1, &cam_h, 1) == A840_KMD_OK);
    auto fi = make_fault(3, 0, 1, A840_KMD_FAULT_SMMU_TRANSLATION);
    A840KmdRecoveryReport rrep{};
    CHECK(a840_kmd_qr_handle_channel_fault(&qr, &fi, 404u, &rrep) == A840_KMD_OK);
    CHECK(rrep.final_state == A840_KMD_CHANNEL_RECOVERED);
    CHECK(rrep.rolled_back_sparse_tiles == 2u);

    CHECK(a840_kmd_qr_submit_complete(&qr, 4, 0, 1, nullptr) == A840_KMD_OK);

    a840_kmd_qr_destroy(&qr);
    a840_kmd_fault_destroy(&fc);
    a840_kmd_sparse_destroy(&smgr);
    a840_kmd_bo_pool_destroy(&pool);
    a840_kmd_zero_page_destroy(&zp);
    CHECK(zpm.allocs.load() == zpm.frees.load());
}

static void case_56_multi_client_channel_capacity_and_reuse() {
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, nullptr) == A840_KMD_OK);
    auto c = make_client_cfg(1, A840_KMD_PRIO_FOREGROUND_APP, kMiB, 2u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    for (uint64_t q = 0; q < A840_KMD_QUOTA_MAX_CHANNELS; ++q)
        CHECK(a840_kmd_qr_open_channel(&qr, 1, q) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 999) == A840_KMD_CAPACITY);
    CHECK(a840_kmd_qr_close_channel(&qr, 1, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 999) == A840_KMD_OK);
    a840_kmd_qr_destroy(&qr);
}

static void case_57_submit_cross_client_bo_forgery_rejected() {
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, nullptr) == A840_KMD_OK);
    auto c1 = make_client_cfg(1, A840_KMD_PRIO_COMPOSITOR, 4u * kMiB, 8u * kMiB);
    auto c2 = make_client_cfg(2, A840_KMD_PRIO_BACKGROUND, 4u * kMiB, 8u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(&qr, &c2) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 2, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_track_bo(&qr, 1, 555, kMiB, 0, A840_PURGE_NONVOLATILE, 1) == A840_KMD_OK);

    uint64_t forged = 555;
    CHECK(a840_kmd_qr_submit_begin(&qr, 2, 0, 1, &forged, 1) == A840_KMD_PERMISSION_DENIED);
    CHECK(a840_kmd_qr_untrack_bo(&qr, 1, 555, 1) == A840_KMD_OK);
    a840_kmd_qr_destroy(&qr);
}

static void case_58_client_teardown_cleans_banned_channel_stats_and_quota() {
    A840KmdQuotaRecoveryConfig cfg{};
    cfg.ban_on_first_fault = 1u;
    cfg.max_recoveries_before_ban = 0u;
    A840KmdQuotaRecovery qr{};
    CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

    auto c = make_client_cfg(1, A840_KMD_PRIO_BACKGROUND, 2u * kMiB, 4u * kMiB);
    CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
    CHECK(a840_kmd_qr_open_channel(&qr, 1, 0) == A840_KMD_OK);
    CHECK(a840_kmd_qr_request_alloc(&qr, 1, 2u * kMiB, nullptr) == A840_KMD_OK);

    auto fi = make_fault(1, 0, 1, A840_KMD_FAULT_GPU_HANG);
    CHECK(a840_kmd_qr_handle_channel_fault(&qr, &fi, 1u, nullptr) == A840_KMD_OK);

    A840KmdQuotaRecoveryStats st{};
    CHECK(a840_kmd_qr_get_stats(&qr, &st) == A840_KMD_OK);
    CHECK(st.banned_channels == 1u);

    CHECK(a840_kmd_qr_unregister_client(&qr, 1) == A840_KMD_OK);
    CHECK(a840_kmd_qr_get_stats(&qr, &st) == A840_KMD_OK);
    CHECK(st.banned_channels == 0u && st.active_channels == 0u && st.global_used_bytes == 0u);
    a840_kmd_qr_destroy(&qr);
}

static void case_59_concurrent_4_threads_quota_oom_reclaim_stress() {
    for (int round = 0; round < g_stress_rounds; ++round) {
        ReclaimMock rm;
        auto rops = rm.ops();
        A840KmdQuotaRecoveryConfig cfg{};
        cfg.global_hard_limit_bytes = 32u * kMiB;
        cfg.global_soft_limit_bytes = 20u * kMiB;
        cfg.compositor_reserve_bytes = 8u * kMiB;
        cfg.reclaim_ops = &rops;
        A840KmdQuotaRecovery qr{};
        CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

        const A840KmdClientPriority prios[4] = {
            A840_KMD_PRIO_BACKGROUND,
            A840_KMD_PRIO_FOREGROUND_APP,
            A840_KMD_PRIO_CAMERA,
            A840_KMD_PRIO_COMPOSITOR
        };
        for (uint64_t i = 0; i < 4; ++i) {
            auto c = make_client_cfg(i + 1u, prios[i], 8u * kMiB, 12u * kMiB);
            CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
        }

        std::atomic<uint32_t> comp_oom{0};
        std::vector<std::thread> workers;
        for (uint64_t tid = 0; tid < 4; ++tid) {
            workers.emplace_back([&, tid]() {
                uint64_t cid = tid + 1u;
                for (uint64_t iter = 0; iter < 40; ++iter) {
                    uint64_t bo_base = (cid * 1000u) + (iter * 2u) + 1u;
                    uint64_t sz = (tid == 3u) ? (512u * kKiB) : (1u * kMiB);
                    A840KmdStatus st = a840_kmd_qr_request_alloc(&qr, cid, sz, nullptr);
                    if (st == A840_KMD_OK) {
                        uint32_t parked = (iter % 2u == 0u) ? 1u : 0u;
                        A840KmdPurgeableState pstate = parked ? A840_PURGE_NONVOLATILE : A840_PURGE_VOLATILE;
                        A840KmdStatus ts = a840_kmd_qr_track_bo(&qr, cid, bo_base, sz, parked, pstate, 0u);
                        if (ts == A840_KMD_OK && !parked) {
                            if (a840_kmd_qr_bo_begin_submit(&qr, cid, bo_base, nullptr) == A840_KMD_OK)
                                (void)a840_kmd_qr_bo_end_submit(&qr, cid, bo_base, nullptr);
                        }
                        if (ts == A840_KMD_OK) {
                            A840KmdStatus us = a840_kmd_qr_untrack_bo(&qr, cid, bo_base, 1u);
                            if (us != A840_KMD_OK && us != A840_KMD_NOT_FOUND) {
                                /* If reclaimed concurrently, ignore. */
                            }
                        } else {
                            (void)a840_kmd_qr_release_bytes(&qr, cid, sz);
                        }
                    } else if (tid == 3u) {
                        comp_oom.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto &t : workers)
            t.join();
        CHECK(comp_oom.load() == 0u);
        a840_kmd_qr_destroy(&qr);
    }
}

static void case_60_concurrent_4_threads_fault_recovery_vs_healthy_clients() {
    for (int round = 0; round < g_stress_rounds; ++round) {
        RecoveryMock rm;
        auto rops = rm.ops();
        A840KmdQuotaRecoveryConfig cfg{};
        cfg.max_recoveries_before_ban = 100u;
        cfg.recovery_ops = &rops;
        A840KmdQuotaRecovery qr{};
        CHECK(a840_kmd_qr_init(&qr, &cfg) == A840_KMD_OK);

        for (uint64_t cid = 1; cid <= 4; ++cid) {
            auto c = make_client_cfg(cid, static_cast<A840KmdClientPriority>(cid - 1u), 4u * kMiB, 8u * kMiB);
            CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
            CHECK(a840_kmd_qr_open_channel(&qr, cid, 0) == A840_KMD_OK);
        }

        std::vector<std::thread> workers;
        /* Threads 3 & 4: healthy COMPOSITOR and CAMERA submitting continuously. */
        for (uint64_t cid = 3; cid <= 4; ++cid) {
            workers.emplace_back([&, cid]() {
                for (uint64_t f = 1; f <= 40; ++f) {
                    CHECK(a840_kmd_qr_submit_begin(&qr, cid, 0, f, nullptr, 0) == A840_KMD_OK);
                    CHECK(a840_kmd_qr_submit_complete(&qr, cid, 0, f, nullptr) == A840_KMD_OK);
                }
            });
        }
        /* Threads 1 & 2: faulting BG and FG channels repeatedly recovering. */
        for (uint64_t cid = 1; cid <= 2; ++cid) {
            workers.emplace_back([&, cid]() {
                for (uint64_t f = 1; f <= 20; ++f) {
                    CHECK(a840_kmd_qr_submit_begin(&qr, cid, 0, f, nullptr, 0) == A840_KMD_OK);
                    auto fi = make_fault(cid, 0, f, A840_KMD_FAULT_GPU_HANG);
                    A840KmdRecoveryReport rep{};
                    CHECK(a840_kmd_qr_handle_channel_fault(&qr, &fi, 1u, &rep) == A840_KMD_OK);
                    CHECK(rep.final_state == A840_KMD_CHANNEL_RECOVERED);
                }
            });
        }
        for (auto &t : workers)
            t.join();

        for (uint64_t cid = 3; cid <= 4; ++cid) {
            A840KmdChannelInfo ci{};
            CHECK(a840_kmd_qr_query_channel(&qr, cid, 0, &ci) == A840_KMD_OK);
            CHECK(ci.state == A840_KMD_CHANNEL_HEALTHY);
            CHECK(ci.completed_ok_count == 40u);
            CHECK(ci.completed_err_count == 0u);
        }
        for (const auto &ev : rm.fence_events) {
            if (ev.client_id == 3u || ev.client_id == 4u)
                CHECK(ev.status == A840_KMD_OK);
        }
        a840_kmd_qr_destroy(&qr);
    }
}

static void case_61_concurrent_4_threads_combined_oom_recovery_sparse_bo_pool() {
    for (int round = 0; round < g_stress_rounds; ++round) {
        ZeroPageMock zpm;
        auto zops = zpm.ops();
        A840KmdZeroPageManager zp{};
        CHECK(a840_kmd_zero_page_init(&zp, &zops, UINT64_C(65536)) == A840_KMD_OK);
        NativeAdrenoAbiZeroPageReplyV3 zrep{};
        for (uint64_t cid = 1; cid <= 4; ++cid)
            CHECK(a840_kmd_zero_page_bind_client(&zp, cid, nullptr, 0, A840_KMD_SMMU_PERM_READ_ONLY, &zrep) == A840_KMD_OK);

        BoBackingMock bbm;
        auto bops = bbm.ops();
        A840KmdBoPoolConfig bcfg{};
        bcfg.zero_page_mgr = &zp;
        bcfg.backing_ops = &bops;
        A840KmdBoPool pool{};
        CHECK(a840_kmd_bo_pool_init(&pool, &bcfg) == A840_KMD_OK);

        A840KmdSparseConfig scfg{};
        scfg.zero_page_mgr = &zp;
        A840KmdSparseManager smgr{};
        CHECK(a840_kmd_sparse_init(&smgr, &scfg) == A840_KMD_OK);

        A840KmdFaultCapture fc{};
        CHECK(a840_kmd_fault_init(&fc) == A840_KMD_OK);

        CustomSyncMock sm;
        RecoveryMock rm;
        rm.sync_to_check = &sm;
        auto sops = sm.ops();
        auto rops = rm.ops();

        A840KmdQuotaRecoveryConfig qcfg{};
        qcfg.global_hard_limit_bytes = 24u * kMiB;
        qcfg.global_soft_limit_bytes = 12u * kMiB;
        qcfg.compositor_reserve_bytes = 6u * kMiB;
        qcfg.max_recoveries_before_ban = 50u;
        qcfg.sync_bo_pool_inflight = 1u;
        qcfg.bo_pool = &pool;
        qcfg.sparse_mgr = &smgr;
        qcfg.fault_capture = &fc;
        qcfg.sync_ops = &sops;
        qcfg.recovery_ops = &rops;
        A840KmdQuotaRecovery qr{};
        CHECK(a840_kmd_qr_init(&qr, &qcfg) == A840_KMD_OK);

        for (uint64_t cid = 1; cid <= 4; ++cid) {
            auto c = make_client_cfg(cid, static_cast<A840KmdClientPriority>(cid - 1u), 4u * kMiB, 8u * kMiB);
            CHECK(a840_kmd_qr_register_client(&qr, &c) == A840_KMD_OK);
            CHECK(a840_kmd_qr_open_channel(&qr, cid, 0) == A840_KMD_OK);
        }

        std::vector<std::thread> workers;
        for (uint64_t cid = 1; cid <= 4; ++cid) {
            workers.emplace_back([&, cid]() {
                for (uint64_t iter = 1; iter <= 16; ++iter) {
                    A840KmdBoAcquireReq breq{};
                    breq.client_id = cid;
                    breq.requested_bytes = 64u * kKiB;
                    breq.alignment = 4096u;
                    breq.permissions = A840_KMD_SMMU_PERM_READ_WRITE;
                    breq.cache_mode = A840_KMD_BO_CACHE_COHERENT;
                    A840KmdBoDesc desc{};
                    if (a840_kmd_bo_pool_acquire(&pool, &breq, &desc) != A840_KMD_OK)
                        continue;

                    uint64_t bh = desc.bo_handle;
                    A840KmdStatus ast = a840_kmd_qr_request_alloc(&qr, cid, desc.bucket_bytes, nullptr);
                    if (ast == A840_KMD_OK) {
                        if (a840_kmd_qr_submit_begin(&qr, cid, 0, iter, &bh, 1) == A840_KMD_OK) {
                            if (cid == 1u && (iter % 4u == 0u)) {
                                auto fi = make_fault(cid, 0, iter, A840_KMD_FAULT_SMMU_TRANSLATION);
                                (void)a840_kmd_qr_handle_channel_fault(&qr, &fi, 7u, nullptr);
                            } else {
                                (void)a840_kmd_qr_submit_complete(&qr, cid, 0, iter, nullptr);
                            }
                        }
                        (void)a840_kmd_qr_release_bytes(&qr, cid, desc.bucket_bytes);
                    }
                    (void)a840_kmd_bo_pool_release(&pool, cid, bh, 1u);
                }
            });
        }
        for (auto &t : workers)
            t.join();

        CHECK(rm.saw_lock_held.load() == 0u);
        CHECK(sm.locks.load() == sm.unlocks.load());

        a840_kmd_qr_destroy(&qr);
        a840_kmd_fault_destroy(&fc);
        a840_kmd_sparse_destroy(&smgr);
        a840_kmd_bo_pool_destroy(&pool);
        a840_kmd_zero_page_destroy(&zp);
        CHECK(zpm.allocs.load() == zpm.frees.load());
    }
}

static void case_62_concurrent_4_threads_ca_env_policy_stress() {
    for (int round = 0; round < g_stress_rounds; ++round) {
        std::vector<std::thread> workers;
        for (int t = 0; t < 4; ++t) {
            workers.emplace_back([t]() {
                for (int i = 0; i < 64; ++i) {
                    auto in = make_valid_ca_input();
                    char env_buf[256];
                    const char *existing[] = {"PATH=/usr/bin", "HOME=/var/mobile"};
                    size_t wb = 0, ec = 0;
                    A840KmdCaEnvDecision dec{};
                    CHECK(a840_kmd_ca_env_build_env_block(&in, existing, 2, env_buf, sizeof(env_buf), &wb, &ec, &dec) == A840_KMD_OK);
                    CHECK(ec == 3u);

                    char bbuf[256];
                    std::strcpy(bbuf, (t % 2 == 0) ? "-v serial=3" : "-v CA_DISABLE_GENERIC_SHADERS=1");
                    CHECK(a840_kmd_ca_env_append_boot_args(&in, bbuf, bbuf, sizeof(bbuf), nullptr, &dec) == A840_KMD_OK);

                    char bad_buf[64];
                    std::strcpy(bad_buf, "-v CA_DISABLE_GENERIC_SHADERS=0");
                    CHECK(a840_kmd_ca_env_append_boot_args(&in, bad_buf, bad_buf, sizeof(bad_buf), nullptr, &dec) == A840_KMD_STATE_CONFLICT);
                    CHECK(std::strcmp(bad_buf, "-v CA_DISABLE_GENERIC_SHADERS=0") == 0);
                }
            });
        }
        for (auto &w : workers)
            w.join();
    }
}

} /* namespace */

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tsan-stress") == 0 && i + 1 < argc) {
            g_stress_rounds = std::atoi(argv[++i]);
            if (g_stress_rounds < 1)
                g_stress_rounds = 1;
        }
    }

    RUN_CASE(case_01_init_defaults_and_invalid_configs);
    RUN_CASE(case_02_uninitialized_operations_fail_closed);
    RUN_CASE(case_03_register_64_clients_and_capacity_reject);
    RUN_CASE(case_04_update_client_quota_and_unregister_reclaims_bytes);
    RUN_CASE(case_05_basic_alloc_release_and_overflow_guards);
    RUN_CASE(case_06_track_bo_validation_and_cross_client_reject);
    RUN_CASE(case_07_soft_limit_triggers_parked_reclaim_before_volatile);
    RUN_CASE(case_08_ladder_parked_then_volatile_lru_order);
    RUN_CASE(case_09_inflight_volatile_bo_strictly_immune_to_oom);
    RUN_CASE(case_10_inflight_multi_refcount_protection);
    RUN_CASE(case_11_nonvolatile_never_purged_and_bg_oom_reject);
    RUN_CASE(case_12_reclaim_callback_failure_rolls_back_reclaiming_flag);

    RUN_CASE(case_13_compositor_reserve_blocks_non_compositor_clients);
    RUN_CASE(case_14_compositor_reclaims_lower_prio_parked_in_priority_order);
    RUN_CASE(case_15_compositor_reclaims_lower_prio_volatile_before_own);
    RUN_CASE(case_16_lower_prio_never_reclaims_higher_prio_or_compositor);
    RUN_CASE(case_17_camera_reclaims_fg_and_bg_but_never_compositor);
    RUN_CASE(case_18_compositor_last_resort_bg_evict_callback);
    RUN_CASE(case_19_bulk_sweep_parked_and_volatile_callbacks);
    RUN_CASE(case_20_custom_sync_lock_balance_and_all_reclaim_callbacks_outside_lock);
    RUN_CASE(case_21_real_bo_pool_linkage_parked_and_volatile_purge);
    RUN_CASE(case_22_empty_bo_submit_rejected_and_untrack_while_inflight_rejected);

    RUN_CASE(case_23_channel_open_close_and_validation);
    RUN_CASE(case_24_healthy_submit_and_cumulative_fence_complete);
    RUN_CASE(case_25_step_by_step_faulting_resetting_recovered_transitions);
    RUN_CASE(case_26_submits_rejected_during_faulting_and_resetting_then_allowed_in_recovered);
    RUN_CASE(case_27_invalid_state_transitions_rejected);
    RUN_CASE(case_28_damaged_fence_range_and_deterministic_error_notifications);
    RUN_CASE(case_29_healthy_client_zero_interruption_during_peer_fault);
    RUN_CASE(case_30_same_client_second_queue_zero_crosstalk);
    RUN_CASE(case_31_consecutive_faults_trigger_banned_circuit_breaker);
    RUN_CASE(case_32_ban_on_first_fault_threshold_zero);
    RUN_CASE(case_33_hw_reset_failure_forces_banned);
    RUN_CASE(case_34_fault_capture_linkage_records_hw_snapshot_outside_lock);
    RUN_CASE(case_35_sparse_va_linkage_rolls_back_faulting_bos_to_ro_zero_page);
    RUN_CASE(case_36_bo_pool_inflight_sync_zero_leak_on_fault);
    RUN_CASE(case_37_recovery_callbacks_strictly_outside_custom_lock);
    RUN_CASE(case_38_all_fault_kinds_hang_smmu_perm_ib_error);

    RUN_CASE(case_39_ca_env_evaluate_backboardd_allow);
    RUN_CASE(case_40_ca_env_evaluate_rejects_non_whitelisted_targets);
    RUN_CASE(case_41_ca_env_evaluate_rejects_unmet_preconditions);
    RUN_CASE(case_42_ca_env_evaluate_rejects_illegal_control_chars);
    RUN_CASE(case_43_ca_env_build_env_block_appends_cleanly);
    RUN_CASE(case_44_ca_env_build_env_block_idempotent_when_already_1);
    RUN_CASE(case_45_ca_env_build_env_block_rejects_conflicts_and_preserves_buf);
    RUN_CASE(case_46_ca_env_build_env_block_rejects_malformed_and_control_chars);
    RUN_CASE(case_47_ca_env_build_env_block_rejects_small_buffer_zero_truncation);
    RUN_CASE(case_48_ca_env_build_envp_allow_idempotent_and_rejects);
    RUN_CASE(case_49_ca_env_append_boot_args_empty_and_existing);
    RUN_CASE(case_50_ca_env_append_boot_args_inplace_and_idempotent);
    RUN_CASE(case_51_ca_env_append_boot_args_rejects_conflict_preserves_inplace);
    RUN_CASE(case_52_ca_env_append_boot_args_rejects_control_chars_preserves_inplace);
    RUN_CASE(case_53_ca_env_append_boot_args_darwinboot_1023_boundary);
    RUN_CASE(case_54_ca_env_append_boot_args_unmet_precondition_preserves_existing);

    RUN_CASE(case_55_e2e_quota_bo_pool_sparse_fault_combined_lifecycle);
    RUN_CASE(case_56_multi_client_channel_capacity_and_reuse);
    RUN_CASE(case_57_submit_cross_client_bo_forgery_rejected);
    RUN_CASE(case_58_client_teardown_cleans_banned_channel_stats_and_quota);
    RUN_CASE(case_59_concurrent_4_threads_quota_oom_reclaim_stress);
    RUN_CASE(case_60_concurrent_4_threads_fault_recovery_vs_healthy_clients);
    RUN_CASE(case_61_concurrent_4_threads_combined_oom_recovery_sparse_bo_pool);
    RUN_CASE(case_62_concurrent_4_threads_ca_env_policy_stress);

    std::printf("PASS phase4_quota_recovery: %d independent cases, %d stress rounds\n",
        g_cases_passed, g_stress_rounds);
    return 0;
}
