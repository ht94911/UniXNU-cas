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
#include <unordered_map>
#include <vector>

#include "NativeAdrenoABI_v3.h"
#include "a840_kmd_bo_pool.h"
#include "a840_kmd_sparse_va.h"
#include "a840_kmd_zero_page.h"

namespace {

static int g_cases_passed = 0;
static int g_stress_rounds = 1;

#define CHECK(cond)                                                               \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            std::exit(1);                                                         \
        }                                                                         \
    } while (0)

#define RUN_CASE(fn)           \
    do {                       \
        fn();                  \
        ++g_cases_passed;      \
    } while (0)

/* ---------------- Custom Lock & Clock Mock ---------------- */

struct CustomSyncMock {
    std::mutex mu;
    std::atomic<int> lock_depth{0};
    std::atomic<uint64_t> locks{0};
    std::atomic<uint64_t> unlocks{0};
    std::atomic<uint64_t> clock_ns{1000};

    static void do_lock(void *cookie) {
        auto *self = static_cast<CustomSyncMock *>(cookie);
        self->mu.lock();
        self->lock_depth.fetch_add(1, std::memory_order_relaxed);
        self->locks.fetch_add(1, std::memory_order_relaxed);
    }

    static void do_unlock(void *cookie) {
        auto *self = static_cast<CustomSyncMock *>(cookie);
        self->lock_depth.fetch_sub(1, std::memory_order_relaxed);
        self->unlocks.fetch_add(1, std::memory_order_relaxed);
        self->mu.unlock();
    }

    static uint64_t do_now_ns(void *cookie) {
        auto *self = static_cast<CustomSyncMock *>(cookie);
        return self->clock_ns.load(std::memory_order_relaxed);
    }
};

/* ---------------- ZeroPage SMMU Mock ---------------- */

struct ZeroPageMock {
    std::mutex mu;
    uint64_t next_phys = UINT64_C(0x10000000);
    std::atomic<uint32_t> allocs{0};
    std::atomic<uint32_t> frees{0};
    std::atomic<uint32_t> maps{0};
    std::atomic<uint32_t> unmaps{0};

    static A840KmdStatus alloc_page(
        void *cookie, uint64_t bytes, uint64_t *out_phys,
        void **out_cpu, void **out_handle) {
        auto *self = static_cast<ZeroPageMock *>(cookie);
        std::lock_guard<std::mutex> lk(self->mu);
        auto *buf = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(bytes)));
        if (!buf)
            return A840_KMD_NO_MEMORY;
        std::memset(buf, 0xCC, static_cast<size_t>(bytes));
        *out_phys = self->next_phys;
        self->next_phys += bytes;
        *out_cpu = buf;
        *out_handle = buf;
        self->allocs.fetch_add(1, std::memory_order_relaxed);
        return A840_KMD_OK;
    }

    static void free_page(void *cookie, uint64_t, void *handle, uint64_t) {
        auto *self = static_cast<ZeroPageMock *>(cookie);
        std::free(handle);
        self->frees.fetch_add(1, std::memory_order_relaxed);
    }

    static A840KmdStatus map_ro(
        void *cookie, uint64_t, void *, uint64_t, uint64_t, uint64_t, uint32_t perms) {
        auto *self = static_cast<ZeroPageMock *>(cookie);
        CHECK(perms == A840_KMD_SMMU_PERM_READ_ONLY);
        self->maps.fetch_add(1, std::memory_order_relaxed);
        return A840_KMD_OK;
    }

    static A840KmdStatus unmap(void *cookie, uint64_t, void *, uint64_t, uint64_t) {
        auto *self = static_cast<ZeroPageMock *>(cookie);
        self->unmaps.fetch_add(1, std::memory_order_relaxed);
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

/* ---------------- Sparse SMMU & Overlap Mock ---------------- */

struct SparseSmmuMock {
    std::mutex mu;
    CustomSyncMock *sync_watch = nullptr;
    std::atomic<bool> fail_next_commit{false};
    std::atomic<uint64_t> commit_calls{0};
    std::atomic<uint64_t> range_updates{0};
    std::atomic<uint64_t> tlb_flushes{0};
    std::atomic<uint64_t> last_tlb_gen{0};
    std::atomic<uint64_t> overlap_checks{0};
    uint64_t blocked_va_start = 0;
    uint64_t blocked_va_len = 0;

    /* Shadow per-(client,tile_va) PTE map to prove zero dangling PTEs. */
    struct PteEntry {
        uint64_t phys_addr;
        uint32_t permissions;
        uint32_t is_zero_page;
    };
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, PteEntry>> shadow_ptes;

    static A840KmdStatus commit_ranges(
        void *cookie,
        uint64_t client_id,
        void *,
        const A840KmdSparsePteRange *ranges,
        size_t count) {
        auto *self = static_cast<SparseSmmuMock *>(cookie);
        if (self->sync_watch != nullptr) {
            /* Must be invoked strictly outside sparse manager lock. */
            CHECK(self->sync_watch->lock_depth.load(std::memory_order_relaxed) == 0);
        }
        if (self->fail_next_commit.exchange(false))
            return A840_KMD_IO_ERROR;

        std::lock_guard<std::mutex> lk(self->mu);
        self->commit_calls.fetch_add(1, std::memory_order_relaxed);
        self->range_updates.fetch_add(count, std::memory_order_relaxed);
        auto &table = self->shadow_ptes[client_id];
        for (size_t i = 0; i < count; ++i) {
            const auto &r = ranges[i];
            CHECK((r.gpu_va % A840_KMD_SPARSE_TILE_BYTES) == 0);
            CHECK((r.length_bytes % A840_KMD_SPARSE_TILE_BYTES) == 0);
            uint64_t tiles = r.length_bytes / A840_KMD_SPARSE_TILE_BYTES;
            for (uint64_t t = 0; t < tiles; ++t) {
                uint64_t va = r.gpu_va + t * A840_KMD_SPARSE_TILE_BYTES;
                if (r.unmap_region) {
                    table.erase(va);
                } else {
                    if (r.is_zero_page) {
                        CHECK(r.permissions == A840_KMD_SMMU_PERM_READ_ONLY);
                        table[va] = PteEntry{r.phys_addr, r.permissions, 1u};
                    } else {
                        table[va] = PteEntry{
                            r.phys_addr + t * A840_KMD_SPARSE_TILE_BYTES,
                            r.permissions,
                            0u};
                    }
                }
            }
        }
        return A840_KMD_OK;
    }

    static void tlb_flush(void *cookie, uint64_t, void *, uint64_t gen) {
        auto *self = static_cast<SparseSmmuMock *>(cookie);
        if (self->sync_watch != nullptr) {
            CHECK(self->sync_watch->lock_depth.load(std::memory_order_relaxed) == 0);
        }
        self->tlb_flushes.fetch_add(1, std::memory_order_relaxed);
        uint64_t prev = self->last_tlb_gen.load(std::memory_order_relaxed);
        while (gen > prev && !self->last_tlb_gen.compare_exchange_weak(prev, gen)) {
        }
    }

    static int external_overlap(
        void *cookie, uint64_t, uint64_t gpu_va, uint64_t length_bytes) {
        auto *self = static_cast<SparseSmmuMock *>(cookie);
        if (self->sync_watch != nullptr) {
            CHECK(self->sync_watch->lock_depth.load(std::memory_order_relaxed) == 0);
        }
        self->overlap_checks.fetch_add(1, std::memory_order_relaxed);
        if (self->blocked_va_len == 0)
            return 0;
        uint64_t a_end = gpu_va + length_bytes;
        uint64_t b_end = self->blocked_va_start + self->blocked_va_len;
        return (gpu_va < b_end && self->blocked_va_start < a_end) ? 1 : 0;
    }

    A840KmdSparseSmmuOps ops() {
        A840KmdSparseSmmuOps o{};
        o.cookie = this;
        o.commit_ranges = &SparseSmmuMock::commit_ranges;
        o.tlb_flush = &SparseSmmuMock::tlb_flush;
        return o;
    }
};

/* ---------------- BoPool Backing Mock ---------------- */

struct BoBackingAlloc {
    uint64_t client_id;
    uint64_t gpu_va;
    uint64_t phys_addr;
    uint64_t bytes;
    uint8_t *buf;
    bool purged;
};

struct BoBackingMock {
    std::mutex mu;
    CustomSyncMock *sync_watch = nullptr;
    uint64_t next_va = UINT64_C(0x20000000);
    uint64_t next_phys = UINT64_C(0x80000000);
    std::atomic<uint64_t> alloc_calls{0};
    std::atomic<uint64_t> free_calls{0};
    std::atomic<uint64_t> scrub_calls{0};
    std::atomic<uint64_t> purge_calls{0};
    std::atomic<uint64_t> rehydrate_calls{0};
    std::atomic<uint64_t> last_purge_zero_phys{0};
    std::atomic<int64_t> live_pages{0};
    std::atomic<bool> fail_next_scrub{false};
    std::atomic<bool> fail_next_rehydrate{false};
    bool allocate_real_cpu_mem = true;

    static uint64_t align_up(uint64_t v, uint64_t a) {
        return (v + (a - 1u)) & ~(a - 1u);
    }

    static A840KmdStatus alloc_and_map(
        void *cookie,
        uint64_t client_id,
        void *,
        uint64_t bytes,
        uint64_t alignment,
        uint32_t,
        uint32_t,
        uint64_t *out_gpu_va,
        uint64_t *out_phys_addr,
        void **out_cpu_ptr,
        void **out_backing_handle) {
        auto *self = static_cast<BoBackingMock *>(cookie);
        if (self->sync_watch != nullptr) {
            CHECK(self->sync_watch->lock_depth.load(std::memory_order_relaxed) == 0);
        }
        auto *rec = new BoBackingAlloc{};
        {
            std::lock_guard<std::mutex> lk(self->mu);
            uint64_t va = align_up(self->next_va, alignment);
            uint64_t pa = align_up(self->next_phys, alignment);
            self->next_va = va + bytes;
            self->next_phys = pa + bytes;
            rec->client_id = client_id;
            rec->gpu_va = va;
            rec->phys_addr = pa;
            rec->bytes = bytes;
        }
        if (self->allocate_real_cpu_mem && bytes <= UINT64_C(4194304)) {
            rec->buf = static_cast<uint8_t *>(std::calloc(1, static_cast<size_t>(bytes)));
            if (!rec->buf) {
                delete rec;
                return A840_KMD_NO_MEMORY;
            }
        }
        *out_gpu_va = rec->gpu_va;
        *out_phys_addr = rec->phys_addr;
        *out_cpu_ptr = rec->buf;
        *out_backing_handle = rec;
        self->alloc_calls.fetch_add(1, std::memory_order_relaxed);
        self->live_pages.fetch_add(1, std::memory_order_relaxed);
        return A840_KMD_OK;
    }

    static void unmap_and_free(
        void *cookie,
        uint64_t,
        void *,
        uint64_t,
        uint64_t,
        void *backing_handle,
        uint64_t) {
        auto *self = static_cast<BoBackingMock *>(cookie);
        if (self->sync_watch != nullptr) {
            CHECK(self->sync_watch->lock_depth.load(std::memory_order_relaxed) == 0);
        }
        self->free_calls.fetch_add(1, std::memory_order_relaxed);
        if (backing_handle != nullptr) {
            auto *rec = static_cast<BoBackingAlloc *>(backing_handle);
            std::free(rec->buf);
            delete rec;
            self->live_pages.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    static A840KmdStatus zero_scrub(
        void *cookie,
        uint64_t,
        uint64_t,
        void *cpu_ptr,
        void *,
        uint64_t bytes) {
        auto *self = static_cast<BoBackingMock *>(cookie);
        if (self->sync_watch != nullptr) {
            CHECK(self->sync_watch->lock_depth.load(std::memory_order_relaxed) == 0);
        }
        if (self->fail_next_scrub.exchange(false))
            return A840_KMD_IO_ERROR;
        if (cpu_ptr != nullptr)
            std::memset(cpu_ptr, 0, static_cast<size_t>(bytes));
        self->scrub_calls.fetch_add(1, std::memory_order_relaxed);
        return A840_KMD_OK;
    }

    static void purge_to_empty(
        void *cookie,
        uint64_t,
        void *,
        uint64_t,
        uint64_t,
        void *backing_handle,
        uint64_t,
        uint64_t zero_page_phys) {
        auto *self = static_cast<BoBackingMock *>(cookie);
        if (self->sync_watch != nullptr) {
            CHECK(self->sync_watch->lock_depth.load(std::memory_order_relaxed) == 0);
        }
        self->purge_calls.fetch_add(1, std::memory_order_relaxed);
        self->last_purge_zero_phys.store(zero_page_phys, std::memory_order_relaxed);
        if (backing_handle != nullptr) {
            auto *rec = static_cast<BoBackingAlloc *>(backing_handle);
            std::free(rec->buf);
            delete rec;
            self->live_pages.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    static A840KmdStatus rehydrate_from_empty(
        void *cookie,
        uint64_t client_id,
        void *,
        uint64_t gpu_va,
        uint64_t bytes,
        uint32_t,
        uint32_t,
        uint64_t *out_phys_addr,
        void **out_cpu_ptr,
        void **out_backing_handle) {
        auto *self = static_cast<BoBackingMock *>(cookie);
        if (self->sync_watch != nullptr) {
            CHECK(self->sync_watch->lock_depth.load(std::memory_order_relaxed) == 0);
        }
        if (self->fail_next_rehydrate.exchange(false))
            return A840_KMD_NO_MEMORY;
        auto *rec = new BoBackingAlloc{};
        {
            std::lock_guard<std::mutex> lk(self->mu);
            uint64_t pa = align_up(self->next_phys, UINT64_C(4096));
            self->next_phys = pa + bytes;
            rec->client_id = client_id;
            rec->gpu_va = gpu_va;
            rec->phys_addr = pa;
            rec->bytes = bytes;
        }
        if (self->allocate_real_cpu_mem && bytes <= UINT64_C(4194304)) {
            rec->buf = static_cast<uint8_t *>(std::calloc(1, static_cast<size_t>(bytes)));
            if (!rec->buf) {
                delete rec;
                return A840_KMD_NO_MEMORY;
            }
        }
        *out_phys_addr = rec->phys_addr;
        *out_cpu_ptr = rec->buf;
        *out_backing_handle = rec;
        self->rehydrate_calls.fetch_add(1, std::memory_order_relaxed);
        self->live_pages.fetch_add(1, std::memory_order_relaxed);
        return A840_KMD_OK;
    }

    A840KmdBoPoolBackingOps ops(bool with_rehydrate = true) {
        A840KmdBoPoolBackingOps o{};
        o.cookie = this;
        o.alloc_and_map = &BoBackingMock::alloc_and_map;
        o.unmap_and_free = &BoBackingMock::unmap_and_free;
        o.zero_scrub = &BoBackingMock::zero_scrub;
        o.purge_to_empty = &BoBackingMock::purge_to_empty;
        o.rehydrate_from_empty = with_rehydrate ? &BoBackingMock::rehydrate_from_empty : nullptr;
        return o;
    }
};

/* ---------------- Fixture Helpers ---------------- */

struct SparseFixture {
    ZeroPageMock zp_mock;
    SparseSmmuMock smmu_mock;
    CustomSyncMock sync_mock;
    A840KmdZeroPageManager zp;
    A840KmdSparseManager mgr;
    uint64_t zp_default_va = UINT64_C(0x00010000);

    void init(bool use_custom_sync = false, bool use_external_overlap = false) {
        A840KmdSmmuOps zp_ops = zp_mock.ops();
        CHECK(a840_kmd_zero_page_init(&zp, &zp_ops, zp_default_va) == A840_KMD_OK);

        A840KmdSparseSmmuOps smmu_ops = smmu_mock.ops();
        A840KmdSparseSyncOps sops{};
        sops.cookie = &sync_mock;
        sops.lock = &CustomSyncMock::do_lock;
        sops.unlock = &CustomSyncMock::do_unlock;

        A840KmdSparseConfig cfg{};
        cfg.zero_page_mgr = &zp;
        cfg.smmu_ops = &smmu_ops;
        if (use_custom_sync) {
            smmu_mock.sync_watch = &sync_mock;
            cfg.sync_ops = &sops;
        }
        if (use_external_overlap) {
            cfg.external_overlap_fn = &SparseSmmuMock::external_overlap;
            cfg.external_overlap_cookie = &smmu_mock;
        }
        CHECK(a840_kmd_sparse_init(&mgr, &cfg) == A840_KMD_OK);
    }

    uint64_t bind_client_zp(uint64_t client_id, void *ctx = nullptr, uint64_t va = 0) {
        NativeAdrenoAbiZeroPageReplyV3 reply{};
        CHECK(a840_kmd_zero_page_bind_client(&zp, client_id, ctx, va,
            A840_KMD_SMMU_PERM_READ_ONLY, &reply) == A840_KMD_OK);
        A840KmdZeroPageStats st{};
        CHECK(a840_kmd_zero_page_get_stats(&zp, &st) == A840_KMD_OK);
        return st.backing_phys_addr;
    }

    ~SparseFixture() {
        a840_kmd_sparse_destroy(&mgr);
        a840_kmd_zero_page_destroy(&zp);
    }
};

static A840KmdSparseBindReqV3 make_req(uint32_t op_count, uint64_t region_id = 0) {
    A840KmdSparseBindReqV3 req{};
    req.version = NativeAdrenoAbiVersionV3;
    req.op_count = op_count;
    req.region_id = region_id;
    return req;
}

static A840KmdBoAcquireReq make_acq(
    uint64_t client_id,
    uint64_t bytes,
    uint32_t perms = A840_KMD_SMMU_PERM_READ_WRITE,
    uint32_t cache_mode = A840_KMD_BO_CACHE_WRITE_COMBINE,
    uint64_t align = 4096u,
    void *ctx = nullptr) {
    A840KmdBoAcquireReq req{};
    req.client_id = client_id;
    req.smmu_ctx = ctx;
    req.requested_bytes = bytes;
    req.alignment = align;
    req.permissions = perms;
    req.cache_mode = cache_mode;
    return req;
}

/* ===================== GROUP 1: ABI & CONTRACT CHECKS ===================== */

static void case_01_abi_struct_sizes_and_offsets() {
    static_assert(sizeof(A840KmdSparseBindOp) == 48u, "op size");
    static_assert(sizeof(A840KmdSparseTileInfo) == 48u, "tile info size");
    static_assert(sizeof(A840KmdSparseBindReqV3) == 1568u, "req size");
    static_assert(sizeof(A840KmdSparseBindReplyV3) == 1568u, "reply size");
    static_assert(sizeof(A840KmdSparseBindReqV3) <= NativeAdrenoAbiEnvelopeBytes, "req <= 3104");
    static_assert(sizeof(A840KmdSparseBindReplyV3) <= NativeAdrenoAbiEnvelopeBytes, "reply <= 3104");
    CHECK(offsetof(A840KmdSparseBindOp, op) == 0u);
    CHECK(offsetof(A840KmdSparseBindOp, flags) == 4u);
    CHECK(offsetof(A840KmdSparseBindOp, gpu_va) == 8u);
    CHECK(offsetof(A840KmdSparseBindOp, length_bytes) == 16u);
    CHECK(offsetof(A840KmdSparseBindOp, bo_handle) == 24u);
    CHECK(offsetof(A840KmdSparseBindOp, bo_offset) == 32u);
    CHECK(offsetof(A840KmdSparseBindOp, reserved0) == 40u);
    CHECK(offsetof(A840KmdSparseBindReqV3, ops) == 32u);
    CHECK(offsetof(A840KmdSparseBindReplyV3, results) == 32u);
    CHECK(A840_KMD_SPARSE_TILE_BYTES == 65536u);
    CHECK(NativeAdrenoMethodMapSparseV3 == 22u);
    CHECK(NativeAdrenoParamsV3SparseVmBind == (1u << 10));
}

static void case_02_abi_envelope_size_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x40000, &rid) == A840_KMD_OK);
    auto req = make_req(1, rid);
    req.ops[0].op = A840_KMD_SPARSE_OP_QUERY_TILE;
    req.ops[0].gpu_va = 0x100000;
    req.ops[0].length_bytes = 0x10000;
    A840KmdSparseBindReplyV3 rep{};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, 0, &rep, sizeof(rep)) == A840_KMD_RANGE);
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req) - 1, &rep, sizeof(rep)) == A840_KMD_RANGE);
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep) + 1) == A840_KMD_RANGE);
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, NativeAdrenoAbiEnvelopeBytes + 1, &rep, sizeof(rep)) == A840_KMD_RANGE);
}

static void case_03_abi_wrong_version_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x40000, &rid) == A840_KMD_OK);
    auto req = make_req(1, rid);
    req.ops[0].op = A840_KMD_SPARSE_OP_QUERY_TILE;
    req.ops[0].gpu_va = 0x100000;
    req.ops[0].length_bytes = 0x10000;
    A840KmdSparseBindReplyV3 rep{};
    req.version = NativeAdrenoAbiVersionV2;
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_INVALID_ARG);
    req.version = 0;
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_INVALID_ARG);
}

static void case_04_abi_reserved_header_fields_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x40000, &rid) == A840_KMD_OK);
    A840KmdSparseBindReplyV3 rep{};
    auto req = make_req(1, rid);
    req.ops[0].op = A840_KMD_SPARSE_OP_QUERY_TILE;
    req.ops[0].gpu_va = 0x100000;
    req.ops[0].length_bytes = 0x10000;

    req.reserved0 = 1;
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_INVALID_ARG);
    req.reserved0 = 0;
    req.reserved1 = 2;
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_INVALID_ARG);
    req.reserved1 = 0;
    req.flags = 1;
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_INVALID_ARG);
}

static void case_05_abi_op_count_bounds_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    A840KmdSparseBindReplyV3 rep{};
    auto req0 = make_req(0);
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req0, sizeof(req0), &rep, sizeof(rep)) == A840_KMD_INVALID_ARG);
    auto req33 = make_req(33);
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req33, sizeof(req33), &rep, sizeof(rep)) == A840_KMD_INVALID_ARG);
}

static void case_06_abi_nonzero_trailing_ops_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x40000, &rid) == A840_KMD_OK);
    auto req = make_req(1, rid);
    req.ops[0].op = A840_KMD_SPARSE_OP_QUERY_TILE;
    req.ops[0].gpu_va = 0x100000;
    req.ops[0].length_bytes = 0x10000;
    req.ops[1].gpu_va = 0x110000; /* dirty unused tail slot */
    A840KmdSparseBindReplyV3 rep{};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_INVALID_ARG);
    req.ops[1].gpu_va = 0;
    req.ops[31].reserved0 = 9;
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_INVALID_ARG);
}

static void case_07_abi_op_reserved0_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x40000, &rid) == A840_KMD_OK);
    auto req = make_req(1, rid);
    req.ops[0].op = A840_KMD_SPARSE_OP_QUERY_TILE;
    req.ops[0].gpu_va = 0x100000;
    req.ops[0].length_bytes = 0x10000;
    req.ops[0].reserved0 = 1;
    A840KmdSparseBindReplyV3 rep{};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_INVALID_ARG);
}

static void case_08_abi_unknown_opcode_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x40000, &rid) == A840_KMD_OK);
    auto req = make_req(1, rid);
    req.ops[0].op = 99;
    req.ops[0].gpu_va = 0x100000;
    req.ops[0].length_bytes = 0x10000;
    A840KmdSparseBindReplyV3 rep{};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_INVALID_ARG);
}

static void case_09_abi_atomic_batch_fail_closed() {
    SparseFixture f;
    f.init();
    uint64_t zp_phys = f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x40000, &rid) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 1, 10, 0x500000, 0x20000, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);

    A840KmdSparseStats before{};
    CHECK(a840_kmd_sparse_get_stats(&f.mgr, &before) == A840_KMD_OK);

    auto req = make_req(2, rid);
    req.ops[0].op = A840_KMD_SPARSE_OP_BIND_BO;
    req.ops[0].flags = A840_KMD_SMMU_PERM_READ_WRITE;
    req.ops[0].gpu_va = 0x100000;
    req.ops[0].length_bytes = 0x10000;
    req.ops[0].bo_handle = 10;
    req.ops[1].op = A840_KMD_SPARSE_OP_BIND_BO;
    req.ops[1].flags = A840_KMD_SMMU_PERM_READ_WRITE;
    req.ops[1].gpu_va = 0x110000;
    req.ops[1].length_bytes = 0x10000;
    req.ops[1].bo_handle = 999; /* missing BO -> must fail whole batch */

    A840KmdSparseBindReplyV3 rep{};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_NOT_FOUND);

    /* Tile 0 must remain RO zero page and BO refcount must remain 0. */
    A840KmdSparseTileInfo ti{};
    CHECK(a840_kmd_sparse_query_tile(&f.mgr, 1, 0x100000, &ti) == A840_KMD_OK);
    CHECK(ti.is_zero_page == 1u);
    CHECK(ti.phys_addr == zp_phys);
    CHECK(ti.permissions == A840_KMD_SMMU_PERM_READ_ONLY);
    uint32_t rc = 99;
    CHECK(a840_kmd_sparse_get_bo_refcount(&f.mgr, 1, 10, &rc) == A840_KMD_OK);
    CHECK(rc == 0u);
    A840KmdSparseStats after{};
    CHECK(a840_kmd_sparse_get_stats(&f.mgr, &after) == A840_KMD_OK);
    CHECK(after.tlb_flush_generation == before.tlb_flush_generation);
}

static void case_10_sparse_init_validation() {
    A840KmdSparseManager m{};
    A840KmdSparseConfig cfg{};
    CHECK(a840_kmd_sparse_init(nullptr, &cfg) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_sparse_init(&m, nullptr) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_sparse_init(&m, &cfg) == A840_KMD_INVALID_ARG); /* NULL zero_page_mgr */

    A840KmdZeroPageManager zp{};
    cfg.zero_page_mgr = &zp;
    A840KmdSparseSyncOps bad_sync{};
    bad_sync.lock = &CustomSyncMock::do_lock;
    bad_sync.unlock = nullptr;
    cfg.sync_ops = &bad_sync;
    CHECK(a840_kmd_sparse_init(&m, &cfg) == A840_KMD_INVALID_ARG);

    A840KmdSparseStats st{};
    CHECK(a840_kmd_sparse_get_stats(&m, &st) == A840_KMD_STATE_CONFLICT);
}

/* ===================== GROUP 2: REGION CREATE & OVERLAP ===================== */

static void case_11_sparse_region_create_default_zero_page() {
    SparseFixture f;
    f.init(true, false);
    uint64_t zp_phys = f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, reinterpret_cast<void *>(0x1),
        0x100000, 8u * 65536u, &rid) == A840_KMD_OK);
    CHECK(rid != 0u);

    for (uint32_t i = 0; i < 8u; ++i) {
        A840KmdSparseTileInfo ti{};
        CHECK(a840_kmd_sparse_query_tile(&f.mgr, 1, 0x100000 + i * 65536u, &ti) == A840_KMD_OK);
        CHECK(ti.is_zero_page == 1u);
        CHECK(ti.state == A840_KMD_SPARSE_TILE_ZERO);
        CHECK(ti.permissions == A840_KMD_SMMU_PERM_READ_ONLY);
        CHECK(ti.phys_addr == zp_phys);
        CHECK(ti.bo_handle == 0u);
        CHECK(ti.bo_offset == 0u);
    }
    CHECK(f.smmu_mock.commit_calls.load() == 1u);
    CHECK(f.smmu_mock.tlb_flushes.load() == 1u);
    CHECK(f.smmu_mock.last_tlb_gen.load() == 1u);
    CHECK(f.smmu_mock.shadow_ptes[1].size() == 8u);
}

static void case_12_sparse_region_requires_bound_zero_page() {
    SparseFixture f;
    f.init();
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 42, nullptr, 0x100000, 0x10000, &rid) == A840_KMD_NOT_FOUND);
}

static void case_13_sparse_region_alignment_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x101000, 0x10000, &rid) == A840_KMD_ALIGNMENT);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x08000, &rid) == A840_KMD_ALIGNMENT);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x18000, &rid) == A840_KMD_ALIGNMENT);
}

static void case_14_sparse_region_va_window_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0, 0x10000, &rid) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0, &rid) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, A840_KMD_SPARSE_VA_LIMIT, 0x10000, &rid) == A840_KMD_RANGE);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, A840_KMD_SPARSE_VA_LIMIT - 0x10000, 0x20000, &rid) == A840_KMD_RANGE);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000,
        (A840_KMD_SPARSE_MAX_TILES_PER_REGION + 1u) * 65536u, &rid) == A840_KMD_RANGE);
}

static void case_15_sparse_region_overlap_existing_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    uint64_t r1 = 0, r2 = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x200000, 0x40000, &r1) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x200000, 0x40000, &r2) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x1F0000, 0x20000, &r2) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x230000, 0x20000, &r2) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x210000, 0x10000, &r2) == A840_KMD_STATE_CONFLICT);
    /* Adjacent non-overlapping succeeds. */
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x240000, 0x20000, &r2) == A840_KMD_OK);
    CHECK(r2 != r1);
}

static void case_16_sparse_region_overlap_zero_page_va_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1, nullptr, 0x300000);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x300000, 0x20000, &rid) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x310000, 0x20000, &rid) == A840_KMD_OK);
}

static void case_17_sparse_region_overlap_normal_range_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    CHECK(a840_kmd_sparse_register_normal_range(&f.mgr, 1, 0x400000, 0x20000) == A840_KMD_OK);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x410000, 0x20000, &rid) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_sparse_unregister_normal_range(&f.mgr, 1, 0x400000) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x410000, 0x20000, &rid) == A840_KMD_OK);
    /* Normal range overlapping existing sparse region rejected. */
    CHECK(a840_kmd_sparse_register_normal_range(&f.mgr, 1, 0x420000, 0x1000) == A840_KMD_STATE_CONFLICT);
}

static void case_18_sparse_external_overlap_callback_outside_lock() {
    SparseFixture f;
    f.init(true, true);
    f.bind_client_zp(1);
    f.smmu_mock.blocked_va_start = 0x500000;
    f.smmu_mock.blocked_va_len = 0x20000;
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x510000, 0x20000, &rid) == A840_KMD_STATE_CONFLICT);
    CHECK(f.smmu_mock.overlap_checks.load() == 1u);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x600000, 0x20000, &rid) == A840_KMD_OK);
    CHECK(f.smmu_mock.overlap_checks.load() == 2u);
}

static void case_19_sparse_same_va_different_clients_isolated() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    f.bind_client_zp(2);
    uint64_t r1 = 0, r2 = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x800000, 0x20000, &r1) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 2, nullptr, 0x800000, 0x20000, &r2) == A840_KMD_OK);
    CHECK(r1 != r2);
}

static void case_20_sparse_region_create_smmu_failure_rollback() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    f.smmu_mock.fail_next_commit.store(true);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x900000, 0x40000, &rid) == A840_KMD_IO_ERROR);
    A840KmdSparseStats st{};
    CHECK(a840_kmd_sparse_get_stats(&f.mgr, &st) == A840_KMD_OK);
    CHECK(st.active_regions == 0u);
    CHECK(st.active_zero_tiles == 0u);
    /* Retry succeeds cleanly. */
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x900000, 0x40000, &rid) == A840_KMD_OK);
}

/* ===================== GROUP 3: BIND, UNBIND, ZERO-PAGE & LIFECYCLE ===================== */

static void case_21_sparse_bind_bo_partial_tiles() {
    SparseFixture f;
    f.init(true);
    uint64_t zp_phys = f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 8u * 65536u, &rid) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 1, 77, 0x700000, 4u * 65536u, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);

    auto req = make_req(1, rid);
    req.ops[0].op = A840_KMD_SPARSE_OP_BIND_BO;
    req.ops[0].flags = A840_KMD_SMMU_PERM_READ_WRITE;
    req.ops[0].gpu_va = 0x100000 + 2u * 65536u;
    req.ops[0].length_bytes = 3u * 65536u;
    req.ops[0].bo_handle = 77;
    req.ops[0].bo_offset = 65536u;

    A840KmdSparseBindReplyV3 rep{};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_OK);
    CHECK(rep.completed_ops == 1u);
    CHECK(rep.tlb_flush_generation == 2u);
    CHECK(rep.zero_page_gpu_va == f.zp_default_va);

    for (uint32_t t = 0; t < 8u; ++t) {
        A840KmdSparseTileInfo ti{};
        CHECK(a840_kmd_sparse_query_tile(&f.mgr, 1, 0x100000 + t * 65536u, &ti) == A840_KMD_OK);
        if (t >= 2u && t <= 4u) {
            CHECK(ti.is_zero_page == 0u);
            CHECK(ti.bo_handle == 77u);
            CHECK(ti.bo_offset == (1u + (t - 2u)) * 65536u);
            CHECK(ti.phys_addr == 0x700000 + (1u + (t - 2u)) * 65536u);
            CHECK(ti.permissions == A840_KMD_SMMU_PERM_READ_WRITE);
        } else {
            CHECK(ti.is_zero_page == 1u);
            CHECK(ti.phys_addr == zp_phys);
            CHECK(ti.permissions == A840_KMD_SMMU_PERM_READ_ONLY);
        }
    }
    uint32_t rc = 0;
    CHECK(a840_kmd_sparse_get_bo_refcount(&f.mgr, 1, 77, &rc) == A840_KMD_OK);
    CHECK(rc == 3u);
}

static void case_22_sparse_bind_overwrite_updates_refcounts() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 4u * 65536u, &rid) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 1, 10, 0x500000, 4u * 65536u, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 1, 20, 0x600000, 4u * 65536u, A840_KMD_SMMU_PERM_READ_ONLY) == A840_KMD_OK);

    A840KmdSparseBindReplyV3 rep{};
    auto req = make_req(2, rid);
    req.ops[0] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, 0x100000, 3u * 65536u, 10, 0, 0};
    req.ops[1] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_ONLY, 0x110000, 65536u, 20, 65536u, 0};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_OK);

    uint32_t rc10 = 0, rc20 = 0;
    CHECK(a840_kmd_sparse_get_bo_refcount(&f.mgr, 1, 10, &rc10) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_get_bo_refcount(&f.mgr, 1, 20, &rc20) == A840_KMD_OK);
    CHECK(rc10 == 2u);
    CHECK(rc20 == 1u);
    A840KmdSparseStats st{};
    CHECK(a840_kmd_sparse_get_stats(&f.mgr, &st) == A840_KMD_OK);
    CHECK(st.active_bound_tiles == 3u);
    CHECK(st.active_zero_tiles == 1u);
}

static void case_23_sparse_readonly_bo_escalation_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x20000, &rid) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 1, 11, 0x500000, 0x20000, A840_KMD_SMMU_PERM_READ_ONLY) == A840_KMD_OK);

    A840KmdSparseBindReplyV3 rep{};
    auto req = make_req(1, rid);
    req.ops[0] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, 0x100000, 0x10000, 11, 0, 0};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_PERMISSION_DENIED);

    req.ops[0].flags = A840_KMD_SMMU_PERM_READ_ONLY;
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_OK);
}

static void case_24_sparse_bo_offset_bounds_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x40000, &rid) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 1, 11, 0x500000, 0x20000, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);

    A840KmdSparseBindReplyV3 rep{};
    auto req = make_req(1, rid);
    req.ops[0] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, 0x100000, 0x10000, 11, 4096u, 0};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_ALIGNMENT);

    req.ops[0].bo_offset = 0x20000;
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_RANGE);

    req.ops[0].bo_offset = 0x10000;
    req.ops[0].length_bytes = 0x20000;
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_RANGE);
}

static void case_25_sparse_zero_page_write_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x20000, &rid) == A840_KMD_OK);
    A840KmdSparseBindReplyV3 rep{};

    auto req = make_req(1, rid);
    req.ops[0] = {A840_KMD_SPARSE_OP_BIND_ZERO_PAGE, A840_KMD_SMMU_PERM_READ_WRITE, 0x100000, 0x10000, 0, 0, 0};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_PERMISSION_DENIED);

    req.ops[0].op = A840_KMD_SPARSE_OP_UNBIND_TO_ZERO;
    req.ops[0].flags = A840_KMD_SMMU_PERM_WRITE;
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_PERMISSION_DENIED);

    A840KmdSparseStats st{};
    CHECK(a840_kmd_sparse_get_stats(&f.mgr, &st) == A840_KMD_OK);
    CHECK(st.rejected_zero_page_writes == 2u);
}

static void case_26_sparse_unbind_to_zero_restores_ro_zero_page() {
    SparseFixture f;
    f.init(true);
    uint64_t zp_phys = f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x40000, &rid) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 1, 5, 0x500000, 0x40000, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);

    A840KmdSparseBindReplyV3 rep{};
    auto b = make_req(1, rid);
    b.ops[0] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, 0x100000, 0x40000, 5, 0, 0};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &b, sizeof(b), &rep, sizeof(rep)) == A840_KMD_OK);
    uint64_t gen_after_bind = rep.tlb_flush_generation;

    auto u = make_req(1, rid);
    u.ops[0] = {A840_KMD_SPARSE_OP_UNBIND_TO_ZERO, A840_KMD_SMMU_PERM_READ_ONLY, 0x110000, 0x20000, 0, 0, 0};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &u, sizeof(u), &rep, sizeof(rep)) == A840_KMD_OK);
    CHECK(rep.tlb_flush_generation == gen_after_bind + 1u);

    for (uint32_t i = 0; i < 4u; ++i) {
        A840KmdSparseTileInfo ti{};
        CHECK(a840_kmd_sparse_query_tile(&f.mgr, 1, 0x100000 + i * 65536u, &ti) == A840_KMD_OK);
        if (i == 1u || i == 2u) {
            CHECK(ti.is_zero_page == 1u);
            CHECK(ti.phys_addr == zp_phys);
            CHECK(ti.permissions == A840_KMD_SMMU_PERM_READ_ONLY);
            CHECK(f.smmu_mock.shadow_ptes[1][0x100000 + i * 65536u].is_zero_page == 1u);
        } else {
            CHECK(ti.is_zero_page == 0u);
            CHECK(ti.bo_handle == 5u);
        }
    }
    uint32_t rc = 0;
    CHECK(a840_kmd_sparse_get_bo_refcount(&f.mgr, 1, 5, &rc) == A840_KMD_OK);
    CHECK(rc == 2u);
}

static void case_27_sparse_bind_zero_page_op_semantics() {
    SparseFixture f;
    f.init();
    uint64_t zp_phys = f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x20000, &rid) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 1, 5, 0x500000, 0x20000, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);

    A840KmdSparseBindReplyV3 rep{};
    auto req = make_req(2, rid);
    req.ops[0] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, 0x100000, 0x20000, 5, 0, 0};
    req.ops[1] = {A840_KMD_SPARSE_OP_BIND_ZERO_PAGE, 0, 0x100000, 0x10000, 0, 0, 0};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_OK);

    A840KmdSparseTileInfo ti{};
    CHECK(a840_kmd_sparse_query_tile(&f.mgr, 1, 0x100000, &ti) == A840_KMD_OK);
    CHECK(ti.is_zero_page == 1u);
    CHECK(ti.phys_addr == zp_phys);
    CHECK(ti.permissions == A840_KMD_SMMU_PERM_READ_ONLY);
}

static void case_28_sparse_query_tile_mixed_batch() {
    SparseFixture f;
    f.init();
    uint64_t zp_phys = f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x20000, &rid) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 1, 8, 0x800000, 0x20000, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);

    auto req = make_req(4, rid);
    req.ops[0] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, 0x100000, 0x10000, 8, 0, 0};
    req.ops[1] = {A840_KMD_SPARSE_OP_QUERY_TILE, 0, 0x100000, 0x10000, 0, 0, 0};
    req.ops[2] = {A840_KMD_SPARSE_OP_UNBIND_TO_ZERO, 0, 0x100000, 0x10000, 0, 0, 0};
    req.ops[3] = {A840_KMD_SPARSE_OP_QUERY_TILE, 0, 0x100000, 0x10000, 0, 0, 0};

    A840KmdSparseBindReplyV3 rep{};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_OK);
    CHECK(rep.completed_ops == 4u);
    CHECK(rep.queried_count == 2u);
    CHECK(rep.results[1].is_zero_page == 0u);
    CHECK(rep.results[1].bo_handle == 8u);
    CHECK(rep.results[3].is_zero_page == 1u);
    CHECK(rep.results[3].phys_addr == zp_phys);
}

static void case_29_sparse_bo_early_destroy_auto_unbind() {
    SparseFixture f;
    f.init(true);
    uint64_t zp_phys = f.bind_client_zp(1);
    uint64_t r1 = 0, r2 = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x40000, &r1) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x200000, 0x40000, &r2) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 1, 55, 0x900000, 0x40000, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);

    A840KmdSparseBindReplyV3 rep{};
    auto req = make_req(3, 0);
    req.ops[0] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, 0x100000, 0x10000, 55, 0, 0};
    req.ops[1] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, 0x120000, 0x20000, 55, 0x10000, 0};
    req.ops[2] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, 0x210000, 0x20000, 55, 0, 0};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_OK);

    uint32_t unbound = 0;
    uint64_t gen = 0;
    CHECK(a840_kmd_sparse_unbind_bo(&f.mgr, 1, 55, 1u, &unbound, &gen) == A840_KMD_OK);
    CHECK(unbound == 5u);
    CHECK(gen > rep.tlb_flush_generation);

    /* Every tile across both regions must now point to RO zero page (zero dangling PTEs). */
    for (auto &kv : f.smmu_mock.shadow_ptes[1]) {
        CHECK(kv.second.is_zero_page == 1u);
        CHECK(kv.second.phys_addr == zp_phys);
        CHECK(kv.second.permissions == A840_KMD_SMMU_PERM_READ_ONLY);
    }

    /* Rebinding destroyed BO fails closed with NOT_FOUND. */
    auto retry = make_req(1, r1);
    retry.ops[0] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, 0x100000, 0x10000, 55, 0, 0};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &retry, sizeof(retry), &rep, sizeof(rep)) == A840_KMD_NOT_FOUND);
}

static void case_30_sparse_bo_unbind_keep_registered() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x20000, &rid) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 1, 33, 0x900000, 0x20000, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);

    A840KmdSparseBindReplyV3 rep{};
    auto req = make_req(1, rid);
    req.ops[0] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, 0x100000, 0x20000, 33, 0, 0};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_OK);

    uint32_t unbound = 0;
    CHECK(a840_kmd_sparse_unbind_bo(&f.mgr, 1, 33, 0u, &unbound, nullptr) == A840_KMD_OK);
    CHECK(unbound == 2u);
    uint32_t rc = 99;
    CHECK(a840_kmd_sparse_get_bo_refcount(&f.mgr, 1, 33, &rc) == A840_KMD_OK);
    CHECK(rc == 0u);
    /* Still registered -> can be rebound. */
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_OK);
}

static void case_31_sparse_cross_client_bo_forgery_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    f.bind_client_zp(2);
    uint64_t r1 = 0, r2 = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x20000, &r1) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 2, nullptr, 0x100000, 0x20000, &r2) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 1, 100, 0x500000, 0x20000, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);

    A840KmdSparseBindReplyV3 rep{};
    auto req = make_req(1, r2);
    req.ops[0] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, 0x100000, 0x10000, 100, 0, 0};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 2, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_PERMISSION_DENIED);
    CHECK(a840_kmd_sparse_unbind_bo(&f.mgr, 2, 100, 1u, nullptr, nullptr) == A840_KMD_PERMISSION_DENIED);
    uint32_t rc = 0;
    CHECK(a840_kmd_sparse_get_bo_refcount(&f.mgr, 2, 100, &rc) == A840_KMD_PERMISSION_DENIED);

    A840KmdSparseStats st{};
    CHECK(a840_kmd_sparse_get_stats(&f.mgr, &st) == A840_KMD_OK);
    CHECK(st.rejected_cross_client == 3u);
}

static void case_32_sparse_cross_client_region_and_query_reject() {
    SparseFixture f;
    f.init();
    f.bind_client_zp(1);
    f.bind_client_zp(2);
    uint64_t r1 = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x20000, &r1) == A840_KMD_OK);

    CHECK(a840_kmd_sparse_region_destroy(&f.mgr, 2, r1) == A840_KMD_PERMISSION_DENIED);
    A840KmdSparseTileInfo ti{};
    CHECK(a840_kmd_sparse_query_tile(&f.mgr, 2, 0x100000, &ti) == A840_KMD_PERMISSION_DENIED);
    auto req = make_req(1, r1);
    req.ops[0] = {A840_KMD_SPARSE_OP_QUERY_TILE, 0, 0x100000, 0x10000, 0, 0, 0};
    A840KmdSparseBindReplyV3 rep{};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 2, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_PERMISSION_DENIED);
}

static void case_33_sparse_client_teardown_coalesced_tlb_and_isolation() {
    SparseFixture f;
    f.init(true);
    f.bind_client_zp(1);
    f.bind_client_zp(2);
    void *ctx1 = reinterpret_cast<void *>(0x111);
    void *ctx2 = reinterpret_cast<void *>(0x222);

    uint64_t r1a = 0, r1b = 0, r1c = 0, r2 = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, ctx1, 0x100000, 0x40000, &r1a) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, ctx1, 0x200000, 0x40000, &r1b) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, ctx1, 0x300000, 0x40000, &r1c) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 2, ctx2, 0x100000, 0x20000, &r2) == A840_KMD_OK);

    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 1, 1, 0x500000, 0x40000, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 1, 2, 0x600000, 0x40000, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 2, 1, 0x700000, 0x20000, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);

    A840KmdSparseBindReplyV3 rep{};
    auto req = make_req(2, 0);
    req.ops[0] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, 0x100000, 0x30000, 1, 0, 0};
    req.ops[1] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, 0x200000, 0x20000, 2, 0, 0};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_OK);

    uint64_t flushes_before = f.smmu_mock.tlb_flushes.load();
    A840KmdSparseTeardownSummary sum{};
    CHECK(a840_kmd_sparse_client_teardown(&f.mgr, 1, &sum) == A840_KMD_OK);
    CHECK(sum.regions_reclaimed == 3u);
    CHECK(sum.tiles_reclaimed == 12u);
    CHECK(sum.bo_refs_released == 5u);
    CHECK(sum.bos_removed == 2u);
    CHECK(f.smmu_mock.tlb_flushes.load() == flushes_before + 1u); /* single coalesced TLB flush */
    CHECK(f.smmu_mock.shadow_ptes[1].empty());

    /* Client 2 untouched. */
    CHECK(f.smmu_mock.shadow_ptes[2].size() == 2u);
    A840KmdSparseStats st{};
    CHECK(a840_kmd_sparse_get_stats(&f.mgr, &st) == A840_KMD_OK);
    CHECK(st.active_regions == 1u);
    CHECK(st.active_bos == 1u);
    CHECK(st.active_bound_tiles == 0u);
    CHECK(st.active_zero_tiles == 2u);
}

/* ===================== GROUP 4: PARKED BO POOL ===================== */

static void case_34_bo_pool_bucket_sizes_and_selection() {
    const uint64_t expected[7] = {4096u, 16384u, 65536u, 262144u, 1048576u, 4194304u, 16777216u};
    for (uint32_t i = 0; i < 7u; ++i)
        CHECK(a840_kmd_bo_pool_bucket_size(i) == expected[i]);
    CHECK(a840_kmd_bo_pool_bucket_size(7u) == 0u);

    uint64_t b = 0;
    CHECK(a840_kmd_bo_pool_select_bucket(0, &b) == -1 && b == 0);
    CHECK(a840_kmd_bo_pool_select_bucket(1, &b) == 0 && b == 4096u);
    CHECK(a840_kmd_bo_pool_select_bucket(4096u, &b) == 0 && b == 4096u);
    CHECK(a840_kmd_bo_pool_select_bucket(4097u, &b) == 1 && b == 16384u);
    CHECK(a840_kmd_bo_pool_select_bucket(65536u, &b) == 2 && b == 65536u);
    CHECK(a840_kmd_bo_pool_select_bucket(16777216u, &b) == 6 && b == 16777216u);
    CHECK(a840_kmd_bo_pool_select_bucket(16777217u, &b) == -1 && b == 0);
}

static void case_35_bo_pool_init_and_arg_validation() {
    A840KmdBoPool pool{};
    A840KmdBoPoolConfig cfg{};
    cfg.max_parked_per_bucket = A840_KMD_BO_POOL_MAX_PER_BUCKET + 1u;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_RANGE);

    cfg.max_parked_per_bucket = 4u;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    A840KmdBoDesc desc{};
    auto req = make_acq(0, 4096u);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &desc) == A840_KMD_INVALID_ARG);
    req = make_acq(1, 0);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &desc) == A840_KMD_INVALID_ARG);
    req = make_acq(1, 4096u);
    req.reserved = 1;
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &desc) == A840_KMD_INVALID_ARG);
    req.reserved = 0;
    req.flags = 0x10;
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &desc) == A840_KMD_INVALID_ARG);
    req.flags = 0;
    req.alignment = 2048u;
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &desc) == A840_KMD_ALIGNMENT);
    req.alignment = 12288u; /* not pow2 */
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &desc) == A840_KMD_ALIGNMENT);
    req.alignment = 4096u;
    req.cache_mode = 99;
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &desc) == A840_KMD_INVALID_ARG);

    a840_kmd_bo_pool_destroy(&pool);
}

static void case_36_bo_pool_same_client_reuse_and_zero_scrub() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    cfg.scrub_on_park = 0u; /* scrub on acquire */
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    A840KmdBoDesc d1{}, d2{};
    auto req = make_acq(1, 60000u); /* rounds up to 64 KiB bucket */
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d1) == A840_KMD_OK);
    CHECK(d1.bucket_bytes == 65536u);
    CHECK(d1.from_parked_pool == 0u);
    CHECK(d1.cpu_ptr != nullptr);
    std::memset(d1.cpu_ptr, 0xA5, 65536u);

    CHECK(a840_kmd_bo_pool_release(&pool, 1, d1.bo_handle, 1u) == A840_KMD_OK);
    CHECK(mock.free_calls.load() == 0u);

    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d2) == A840_KMD_OK);
    CHECK(d2.from_parked_pool == 1u);
    CHECK(d2.is_scrubbed == 1u);
    CHECK(d2.gpu_va == d1.gpu_va);
    CHECK(d2.phys_addr == d1.phys_addr);
    CHECK(mock.alloc_calls.load() == 1u); /* SMMU map saved! */
    const auto *bytes = static_cast<const uint8_t *>(d2.cpu_ptr);
    for (size_t i = 0; i < 65536u; ++i)
        CHECK(bytes[i] == 0u);

    A840KmdBoPoolStats st{};
    CHECK(a840_kmd_bo_pool_get_stats(&pool, &st) == A840_KMD_OK);
    CHECK(st.park_hits == 1u);
    CHECK(st.park_misses == 1u);
    CHECK(st.smmu_map_saved == 1u);
    CHECK(st.zero_scrubs == 1u);

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_37_bo_pool_scrub_on_park_policy() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    cfg.scrub_on_park = 1u;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    A840KmdBoDesc d1{}, d2{};
    auto req = make_acq(1, 16384u);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d1) == A840_KMD_OK);
    std::memset(d1.cpu_ptr, 0x5A, 16384u);

    CHECK(a840_kmd_bo_pool_release(&pool, 1, d1.bo_handle, 1u) == A840_KMD_OK);
    CHECK(mock.scrub_calls.load() == 1u); /* scrubbed on park */

    req.flags = A840_KMD_BO_ACQUIRE_PRESCRUBBED_OK;
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d2) == A840_KMD_OK);
    CHECK(d2.from_parked_pool == 1u);
    CHECK(d2.is_scrubbed == 1u);
    CHECK(mock.scrub_calls.load() == 1u); /* no redundant double scrub */
    const auto *bytes = static_cast<const uint8_t *>(d2.cpu_ptr);
    for (size_t i = 0; i < 16384u; ++i)
        CHECK(bytes[i] == 0u);

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_38_bo_pool_scrub_failure_fail_closed() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    cfg.scrub_on_park = 0u;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    A840KmdBoDesc d1{}, d2{};
    auto req = make_acq(1, 4096u);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d1) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, d1.bo_handle, 1u) == A840_KMD_OK);

    mock.fail_next_scrub.store(true);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d2) == A840_KMD_IO_ERROR);
    CHECK(mock.free_calls.load() == 1u);
    CHECK(mock.live_pages.load() == 0);

    a840_kmd_bo_pool_destroy(&pool);
}

static void case_39_bo_pool_cross_client_park_strictly_blocked() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    A840KmdBoDesc d1{}, d2{};
    auto r1 = make_acq(1, 65536u);
    auto r2 = make_acq(2, 65536u);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &r1, &d1) == A840_KMD_OK);
    std::memset(d1.cpu_ptr, 0xEE, 65536u);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, d1.bo_handle, 1u) == A840_KMD_OK);

    /* Client 2 MUST NOT reuse Client 1's parked BO. */
    CHECK(a840_kmd_bo_pool_acquire(&pool, &r2, &d2) == A840_KMD_OK);
    CHECK(d2.from_parked_pool == 0u);
    CHECK(d2.gpu_va != d1.gpu_va);
    CHECK(d2.phys_addr != d1.phys_addr);

    A840KmdBoPoolStats st{};
    CHECK(a840_kmd_bo_pool_get_stats(&pool, &st) == A840_KMD_OK);
    CHECK(st.cross_client_park_blocked == 1u);
    CHECK(st.parked_bos == 1u); /* Client 1's BO still parked */

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_40_bo_pool_attribute_mismatch_no_reuse() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    void *ctxA = reinterpret_cast<void *>(0xA);
    void *ctxB = reinterpret_cast<void *>(0xB);
    A840KmdBoDesc d1{}, d2{};
    auto base = make_acq(1, 65536u, A840_KMD_SMMU_PERM_READ_WRITE, A840_KMD_BO_CACHE_WRITE_COMBINE, 4096u, ctxA);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &base, &d1) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, d1.bo_handle, 1u) == A840_KMD_OK);

    auto m_perm = make_acq(1, 65536u, A840_KMD_SMMU_PERM_READ_ONLY, A840_KMD_BO_CACHE_WRITE_COMBINE, 4096u, ctxA);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &m_perm, &d2) == A840_KMD_OK);
    CHECK(d2.from_parked_pool == 0u);

    auto m_cache = make_acq(1, 65536u, A840_KMD_SMMU_PERM_READ_WRITE, A840_KMD_BO_CACHE_COHERENT, 4096u, ctxA);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &m_cache, &d2) == A840_KMD_OK);
    CHECK(d2.from_parked_pool == 0u);

    auto m_align = make_acq(1, 65536u, A840_KMD_SMMU_PERM_READ_WRITE, A840_KMD_BO_CACHE_WRITE_COMBINE, 65536u, ctxA);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &m_align, &d2) == A840_KMD_OK);
    CHECK(d2.from_parked_pool == 0u);

    auto m_ctx = make_acq(1, 65536u, A840_KMD_SMMU_PERM_READ_WRITE, A840_KMD_BO_CACHE_WRITE_COMBINE, 4096u, ctxB);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &m_ctx, &d2) == A840_KMD_OK);
    CHECK(d2.from_parked_pool == 0u);

    /* Exact match hits! */
    CHECK(a840_kmd_bo_pool_acquire(&pool, &base, &d2) == A840_KMD_OK);
    CHECK(d2.from_parked_pool == 1u);

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_41_bo_pool_release_allow_park_zero_unmaps() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    A840KmdBoDesc d{};
    auto req = make_acq(1, 65536u);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, d.bo_handle, 0u) == A840_KMD_OK);
    CHECK(mock.free_calls.load() == 1u);
    CHECK(mock.live_pages.load() == 0);

    a840_kmd_bo_pool_destroy(&pool);
}

static void case_42_bo_pool_oversized_gt_16mib_not_parked() {
    BoBackingMock mock;
    mock.allocate_real_cpu_mem = false;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    A840KmdBoDesc d{};
    auto req = make_acq(1, 20u * 1024u * 1024u);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    CHECK(d.bucket_bytes == 20u * 1024u * 1024u);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, d.bo_handle, 1u) == A840_KMD_OK);
    CHECK(mock.free_calls.load() == 1u);
    A840KmdBoPoolStats st{};
    CHECK(a840_kmd_bo_pool_get_stats(&pool, &st) == A840_KMD_OK);
    CHECK(st.parked_bos == 0u);

    a840_kmd_bo_pool_destroy(&pool);
}

static void case_43_bo_pool_bucket_cap_lru_eviction() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    cfg.max_parked_per_bucket = 2u;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc a{}, b{}, c{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &a) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &b) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &c) == A840_KMD_OK);

    CHECK(a840_kmd_bo_pool_release(&pool, 1, a.bo_handle, 1u) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, b.bo_handle, 1u) == A840_KMD_OK);
    CHECK(mock.free_calls.load() == 0u);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, c.bo_handle, 1u) == A840_KMD_OK);
    CHECK(mock.free_calls.load() == 1u); /* A evicted */

    A840KmdBoPoolStats st{};
    CHECK(a840_kmd_bo_pool_get_stats(&pool, &st) == A840_KMD_OK);
    CHECK(st.parked_bos == 2u);
    CHECK(st.park_evictions_cap == 1u);

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_44_bo_pool_total_bytes_cap_lru_eviction() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    cfg.max_parked_bytes = 256u * 1024u;
    cfg.max_parked_per_bucket = 8u;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto r64 = make_acq(1, 65536u);
    A840KmdBoDesc d[4]{};
    for (int i = 0; i < 4; ++i)
        CHECK(a840_kmd_bo_pool_acquire(&pool, &r64, &d[i]) == A840_KMD_OK);
    for (int i = 0; i < 4; ++i)
        CHECK(a840_kmd_bo_pool_release(&pool, 1, d[i].bo_handle, 1u) == A840_KMD_OK);

    auto r256 = make_acq(1, 262144u);
    A840KmdBoDesc big{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &r256, &big) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, big.bo_handle, 1u) == A840_KMD_OK);

    A840KmdBoPoolStats st{};
    CHECK(a840_kmd_bo_pool_get_stats(&pool, &st) == A840_KMD_OK);
    CHECK(st.parked_bytes == 262144u);
    CHECK(st.parked_bos == 1u);
    CHECK(st.park_evictions_cap == 4u);

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_45_bo_pool_ttl_expiry_sweep_and_acquire() {
    CustomSyncMock sync;
    BoBackingMock mock;
    mock.sync_watch = &sync;
    A840KmdBoPoolSyncOps sops{&sync, &CustomSyncMock::do_lock, &CustomSyncMock::do_unlock, &CustomSyncMock::do_now_ns};
    A840KmdBoPoolBackingOps bops = mock.ops();

    A840KmdBoPool pool{};
    A840KmdBoPoolConfig cfg{};
    cfg.sync_ops = &sops;
    cfg.backing_ops = &bops;
    cfg.parked_ttl_ns = 1000u;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    sync.clock_ns.store(100u);
    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d1{}, d2{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d1) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, d1.bo_handle, 1u) == A840_KMD_OK);

    sync.clock_ns.store(900u);
    uint32_t evicted = 99;
    CHECK(a840_kmd_bo_pool_sweep_ttl(&pool, &evicted) == A840_KMD_OK);
    CHECK(evicted == 0u);

    sync.clock_ns.store(1100u);
    CHECK(a840_kmd_bo_pool_sweep_ttl(&pool, &evicted) == A840_KMD_OK);
    CHECK(evicted == 1u);

    /* Park another and expire automatically on acquire. */
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d1) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, d1.bo_handle, 1u) == A840_KMD_OK);
    sync.clock_ns.store(2500u);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d2) == A840_KMD_OK);
    CHECK(d2.from_parked_pool == 0u);

    A840KmdBoPoolStats st{};
    CHECK(a840_kmd_bo_pool_get_stats(&pool, &st) == A840_KMD_OK);
    CHECK(st.park_evictions_ttl == 2u);

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_46_bo_pool_synthetic_mode_no_backing_ops() {
    A840KmdBoPool pool{};
    CHECK(a840_kmd_bo_pool_init(&pool, nullptr) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d1{}, d2{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d1) == A840_KMD_OK);
    CHECK(d1.gpu_va != 0u && d1.phys_addr != 0u);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, d1.bo_handle, 1u) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d2) == A840_KMD_OK);
    CHECK(d2.from_parked_pool == 1u);
    CHECK(d2.gpu_va == d1.gpu_va);

    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d2.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_OK);
    A840KmdPressureReport rep{};
    CHECK(a840_kmd_bo_pool_memory_pressure(&pool, UINT64_MAX, &rep) == A840_KMD_OK);
    CHECK(rep.volatile_purged_count == 1u);
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d2.bo_handle, A840_PURGE_NONVOLATILE, &pr) == A840_KMD_OK);
    CHECK(pr.was_purged == 1u);

    a840_kmd_bo_pool_destroy(&pool);
}

static void case_47_bo_pool_custom_sync_lock_balance_and_order() {
    CustomSyncMock sync;
    BoBackingMock mock;
    mock.sync_watch = &sync;
    A840KmdBoPoolSyncOps sops{&sync, &CustomSyncMock::do_lock, &CustomSyncMock::do_unlock, &CustomSyncMock::do_now_ns};
    A840KmdBoPoolBackingOps bops = mock.ops();

    A840KmdBoPool pool{};
    A840KmdBoPoolConfig cfg{};
    cfg.sync_ops = &sops;
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, d.bo_handle, 1u) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_memory_pressure(&pool, UINT64_MAX, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_NONVOLATILE, &pr) == A840_KMD_OK);
    a840_kmd_bo_pool_destroy(&pool);

    CHECK(sync.lock_depth.load() == 0);
    CHECK(sync.locks.load() == sync.unlocks.load());
    CHECK(sync.locks.load() > 0u);
    CHECK(mock.live_pages.load() == 0);
}

static void case_48_bo_pool_cross_client_ops_reject() {
    A840KmdBoPool pool{};
    CHECK(a840_kmd_bo_pool_init(&pool, nullptr) == A840_KMD_OK);
    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d{}, q{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);

    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_query_bo(&pool, 2, d.bo_handle, &q) == A840_KMD_PERMISSION_DENIED);
    CHECK(a840_kmd_bo_pool_release(&pool, 2, d.bo_handle, 1u) == A840_KMD_PERMISSION_DENIED);
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 2, d.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_PERMISSION_DENIED);
    CHECK(a840_kmd_bo_pool_begin_submit(&pool, 2, d.bo_handle, nullptr) == A840_KMD_PERMISSION_DENIED);
    CHECK(a840_kmd_bo_pool_end_submit(&pool, 2, d.bo_handle, nullptr) == A840_KMD_PERMISSION_DENIED);

    a840_kmd_bo_pool_destroy(&pool);
}

static void case_49_bo_pool_client_teardown_reclaims_all() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto r1 = make_acq(1, 65536u);
    auto r2 = make_acq(2, 65536u);
    A840KmdBoDesc c1a{}, c1b{}, c1c{}, c2{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &r1, &c1a) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &r1, &c1b) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &r1, &c1c) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &r2, &c2) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, c1b.bo_handle, 1u) == A840_KMD_OK); /* parked */
    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, c1c.bo_handle, A840_PURGE_EMPTY, &pr) == A840_KMD_OK);

    uint32_t reclaimed = 0;
    CHECK(a840_kmd_bo_pool_client_teardown(&pool, 1, &reclaimed) == A840_KMD_OK);
    CHECK(reclaimed == 3u);

    A840KmdBoPoolStats st{};
    CHECK(a840_kmd_bo_pool_get_stats(&pool, &st) == A840_KMD_OK);
    CHECK(st.active_bos == 1u);
    CHECK(st.parked_bos == 0u);
    CHECK(st.empty_bos == 0u);
    CHECK(mock.live_pages.load() == 1); /* only c2 remains */

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_50_bo_pool_release_while_inflight_defers_destroy() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    uint32_t refs = 0;
    CHECK(a840_kmd_bo_pool_begin_submit(&pool, 1, d.bo_handle, &refs) == A840_KMD_OK);
    CHECK(refs == 1u);

    CHECK(a840_kmd_bo_pool_release(&pool, 1, d.bo_handle, 1u) == A840_KMD_OK);
    CHECK(mock.free_calls.load() == 0u); /* deferred while in flight */
    CHECK(a840_kmd_bo_pool_begin_submit(&pool, 1, d.bo_handle, &refs) == A840_KMD_INVALID_STATE);

    CHECK(a840_kmd_bo_pool_end_submit(&pool, 1, d.bo_handle, &refs) == A840_KMD_OK);
    CHECK(refs == 0u);
    CHECK(mock.free_calls.load() == 1u);
    CHECK(mock.live_pages.load() == 0);

    a840_kmd_bo_pool_destroy(&pool);
}

/* ===================== GROUP 5: VOLATILE HEAP & MEMORY PRESSURE ===================== */

static void case_51_volatile_initial_state_and_keep_current() {
    A840KmdBoPool pool{};
    CHECK(a840_kmd_bo_pool_init(&pool, nullptr) == A840_KMD_OK);
    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    CHECK(d.purgeable_state == A840_PURGE_NONVOLATILE);

    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_KEEP_CURRENT, &pr) == A840_KMD_OK);
    CHECK(pr.old_state == A840_PURGE_NONVOLATILE);
    CHECK(pr.new_state == A840_PURGE_NONVOLATILE);
    CHECK(pr.was_purged == 0u);
    a840_kmd_bo_pool_destroy(&pool);
}

static void case_52_volatile_roundtrip_without_pressure() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_OK);
    CHECK(pr.old_state == A840_PURGE_NONVOLATILE && pr.new_state == A840_PURGE_VOLATILE && pr.was_purged == 0u);

    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_NONVOLATILE, &pr) == A840_KMD_OK);
    CHECK(pr.old_state == A840_PURGE_VOLATILE && pr.new_state == A840_PURGE_NONVOLATILE && pr.was_purged == 0u);
    CHECK(mock.purge_calls.load() == 0u);
    CHECK(mock.rehydrate_calls.load() == 0u);

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_53_volatile_inflight_protected_from_pressure() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_begin_submit(&pool, 1, d.bo_handle, nullptr) == A840_KMD_OK);

    A840KmdPressureReport rep{};
    CHECK(a840_kmd_bo_pool_memory_pressure(&pool, UINT64_MAX, &rep) == A840_KMD_OK);
    CHECK(rep.volatile_purged_count == 0u);
    CHECK(rep.volatile_skipped_inflight == 1u);
    CHECK(mock.purge_calls.load() == 0u);

    A840KmdBoDesc q{};
    CHECK(a840_kmd_bo_pool_query_bo(&pool, 1, d.bo_handle, &q) == A840_KMD_OK);
    CHECK(q.purgeable_state == A840_PURGE_VOLATILE);
    CHECK(q.in_flight_refs == 1u);
    CHECK(q.phys_addr == d.phys_addr);

    CHECK(a840_kmd_bo_pool_end_submit(&pool, 1, d.bo_handle, nullptr) == A840_KMD_OK);
    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_54_volatile_multi_inflight_refcount_protection() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_OK);

    uint32_t refs = 0;
    CHECK(a840_kmd_bo_pool_begin_submit(&pool, 1, d.bo_handle, &refs) == A840_KMD_OK && refs == 1u);
    CHECK(a840_kmd_bo_pool_begin_submit(&pool, 1, d.bo_handle, &refs) == A840_KMD_OK && refs == 2u);
    CHECK(a840_kmd_bo_pool_end_submit(&pool, 1, d.bo_handle, &refs) == A840_KMD_OK && refs == 1u);

    A840KmdPressureReport rep{};
    CHECK(a840_kmd_bo_pool_memory_pressure(&pool, UINT64_MAX, &rep) == A840_KMD_OK);
    CHECK(rep.volatile_purged_count == 0u);
    CHECK(rep.volatile_skipped_inflight == 1u);

    CHECK(a840_kmd_bo_pool_end_submit(&pool, 1, d.bo_handle, &refs) == A840_KMD_OK && refs == 0u);
    CHECK(a840_kmd_bo_pool_memory_pressure(&pool, UINT64_MAX, &rep) == A840_KMD_OK);
    CHECK(rep.volatile_purged_count == 1u);
    CHECK(rep.volatile_skipped_inflight == 0u);

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_55_pressure_reclaims_parked_before_volatile() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc p1{}, p2{}, v1{}, v2{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &p1) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &p2) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &v1) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &v2) == A840_KMD_OK);

    CHECK(a840_kmd_bo_pool_release(&pool, 1, p1.bo_handle, 1u) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, p2.bo_handle, 1u) == A840_KMD_OK);
    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, v1.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, v2.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_OK);

    A840KmdPressureReport rep{};
    CHECK(a840_kmd_bo_pool_memory_pressure(&pool, 128u * 1024u, &rep) == A840_KMD_OK);
    CHECK(rep.parked_evicted_count == 2u);
    CHECK(rep.reclaimed_parked_bytes == 128u * 1024u);
    CHECK(rep.volatile_purged_count == 0u);
    CHECK(rep.reclaimed_volatile_bytes == 0u);

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_56_pressure_spills_from_parked_to_volatile_lru() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc p1{}, v1{}, v2{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &p1) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &v1) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &v2) == A840_KMD_OK);

    CHECK(a840_kmd_bo_pool_release(&pool, 1, p1.bo_handle, 1u) == A840_KMD_OK);
    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, v1.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, v2.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_OK);

    A840KmdPressureReport rep{};
    CHECK(a840_kmd_bo_pool_memory_pressure(&pool, 128u * 1024u, &rep) == A840_KMD_OK);
    CHECK(rep.parked_evicted_count == 1u);
    CHECK(rep.volatile_purged_count == 1u);

    /* v1 (older LRU) is EMPTY, v2 (newer) stays VOLATILE. */
    A840KmdBoDesc q1{}, q2{};
    CHECK(a840_kmd_bo_pool_query_bo(&pool, 1, v1.bo_handle, &q1) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_query_bo(&pool, 1, v2.bo_handle, &q2) == A840_KMD_OK);
    CHECK(q1.purgeable_state == A840_PURGE_EMPTY);
    CHECK(q2.purgeable_state == A840_PURGE_VOLATILE);

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_57_volatile_purge_receives_zero_page_phys() {
    ZeroPageMock zp_mock;
    A840KmdZeroPageManager zp{};
    A840KmdSmmuOps zp_ops = zp_mock.ops();
    CHECK(a840_kmd_zero_page_init(&zp, &zp_ops, 0x10000) == A840_KMD_OK);
    NativeAdrenoAbiZeroPageReplyV3 zrep{};
    CHECK(a840_kmd_zero_page_bind_client(&zp, 1, nullptr, 0, A840_KMD_SMMU_PERM_READ_ONLY, &zrep) == A840_KMD_OK);
    A840KmdZeroPageStats zst{};
    CHECK(a840_kmd_zero_page_get_stats(&zp, &zst) == A840_KMD_OK);

    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.zero_page_mgr = &zp;
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_memory_pressure(&pool, UINT64_MAX, nullptr) == A840_KMD_OK);
    CHECK(mock.last_purge_zero_phys.load() == zst.backing_phys_addr);
    CHECK(mock.last_purge_zero_phys.load() != 0u);

    a840_kmd_bo_pool_destroy(&pool);
    a840_kmd_zero_page_destroy(&zp);
    CHECK(mock.live_pages.load() == 0);
}

static void case_58_empty_bo_submit_rejected_fail_closed() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_memory_pressure(&pool, UINT64_MAX, nullptr) == A840_KMD_OK);

    uint32_t refs = 99;
    CHECK(a840_kmd_bo_pool_begin_submit(&pool, 1, d.bo_handle, &refs) == A840_KMD_INVALID_STATE);
    A840KmdBoPoolStats st{};
    CHECK(a840_kmd_bo_pool_get_stats(&pool, &st) == A840_KMD_OK);
    CHECK(st.empty_submit_rejects == 1u);

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_59_empty_to_nonvolatile_reports_was_purged_and_rehydrates() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops(true);
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_memory_pressure(&pool, UINT64_MAX, nullptr) == A840_KMD_OK);

    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_NONVOLATILE, &pr) == A840_KMD_OK);
    CHECK(pr.old_state == A840_PURGE_EMPTY);
    CHECK(pr.new_state == A840_PURGE_NONVOLATILE);
    CHECK(pr.was_purged == 1u);
    CHECK(mock.rehydrate_calls.load() == 1u);

    /* Now submit succeeds again! */
    uint32_t refs = 0;
    CHECK(a840_kmd_bo_pool_begin_submit(&pool, 1, d.bo_handle, &refs) == A840_KMD_OK && refs == 1u);
    CHECK(a840_kmd_bo_pool_end_submit(&pool, 1, d.bo_handle, &refs) == A840_KMD_OK && refs == 0u);

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_60_empty_to_volatile_stays_empty() {
    A840KmdBoPool pool{};
    CHECK(a840_kmd_bo_pool_init(&pool, nullptr) == A840_KMD_OK);
    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_EMPTY, &pr) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_OK);
    CHECK(pr.old_state == A840_PURGE_EMPTY);
    CHECK(pr.new_state == A840_PURGE_EMPTY);
    CHECK(pr.was_purged == 1u);
    a840_kmd_bo_pool_destroy(&pool);
}

static void case_61_empty_keep_current_reports_was_purged() {
    A840KmdBoPool pool{};
    CHECK(a840_kmd_bo_pool_init(&pool, nullptr) == A840_KMD_OK);
    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_EMPTY, &pr) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_KEEP_CURRENT, &pr) == A840_KMD_OK);
    CHECK(pr.old_state == A840_PURGE_EMPTY && pr.new_state == A840_PURGE_EMPTY && pr.was_purged == 1u);
    a840_kmd_bo_pool_destroy(&pool);
}

static void case_62_explicit_set_empty_respects_inflight() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_begin_submit(&pool, 1, d.bo_handle, nullptr) == A840_KMD_OK);

    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_EMPTY, &pr) == A840_KMD_STATE_CONFLICT);
    CHECK(mock.purge_calls.load() == 0u);

    CHECK(a840_kmd_bo_pool_end_submit(&pool, 1, d.bo_handle, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_EMPTY, &pr) == A840_KMD_OK);
    CHECK(pr.new_state == A840_PURGE_EMPTY && pr.was_purged == 1u);
    CHECK(mock.purge_calls.load() == 1u);

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_63_rehydrate_failure_keeps_bo_empty() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops(true);
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_EMPTY, &pr) == A840_KMD_OK);

    mock.fail_next_rehydrate.store(true);
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_NONVOLATILE, &pr) == A840_KMD_NO_MEMORY);
    CHECK(a840_kmd_bo_pool_begin_submit(&pool, 1, d.bo_handle, nullptr) == A840_KMD_INVALID_STATE);

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_64_nonvolatile_never_purged_under_pressure() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    A840KmdPressureReport rep{};
    CHECK(a840_kmd_bo_pool_memory_pressure(&pool, UINT64_MAX, &rep) == A840_KMD_OK);
    CHECK(rep.reclaimed_total_bytes == 0u);
    CHECK(mock.purge_calls.load() == 0u);
    CHECK(mock.live_pages.load() == 1);

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_65_integrated_sparse_and_bo_pool_purge_fallback() {
    SparseFixture sf;
    sf.init();
    uint64_t zp_phys = sf.bind_client_zp(1);

    BoBackingMock bmock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = bmock.ops();
    A840KmdBoPoolConfig bcfg{};
    bcfg.zero_page_mgr = &sf.zp;
    bcfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &bcfg) == A840_KMD_OK);

    /* Acquire 256 KiB BO, register & bind into Sparse VA, then purge -> unbind to RO zero page. */
    auto acq = make_acq(1, 262144u, A840_KMD_SMMU_PERM_READ_WRITE, A840_KMD_BO_CACHE_WRITE_COMBINE, 65536u);
    A840KmdBoDesc d{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &acq, &d) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&sf.mgr, 1, d.bo_handle, d.phys_addr, d.bucket_bytes, d.permissions) == A840_KMD_OK);

    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&sf.mgr, 1, nullptr, 0x1000000, 0x40000, &rid) == A840_KMD_OK);
    auto breq = make_req(1, rid);
    breq.ops[0] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, 0x1000000, 0x40000, d.bo_handle, 0, 0};
    A840KmdSparseBindReplyV3 brep{};
    CHECK(a840_kmd_sparse_dispatch_v3(&sf.mgr, 1, &breq, sizeof(breq), &brep, sizeof(brep)) == A840_KMD_OK);

    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_unbind_bo(&sf.mgr, 1, d.bo_handle, 1u, nullptr, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_memory_pressure(&pool, UINT64_MAX, nullptr) == A840_KMD_OK);

    for (uint32_t i = 0; i < 4u; ++i) {
        A840KmdSparseTileInfo ti{};
        CHECK(a840_kmd_sparse_query_tile(&sf.mgr, 1, 0x1000000 + i * 65536u, &ti) == A840_KMD_OK);
        CHECK(ti.is_zero_page == 1u && ti.phys_addr == zp_phys);
    }
    CHECK(a840_kmd_bo_pool_begin_submit(&pool, 1, d.bo_handle, nullptr) == A840_KMD_INVALID_STATE);

    a840_kmd_bo_pool_destroy(&pool);
    CHECK(bmock.live_pages.load() == 0);
}

/* ===================== GROUP 6: SCALE, CLEANUP & CONCURRENT STRESS ===================== */

static void case_66_sparse_custom_sync_lock_balance() {
    SparseFixture f;
    f.init(true, true);
    f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 0x40000, &rid) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 1, 1, 0x500000, 0x40000, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);
    auto req = make_req(1, rid);
    req.ops[0] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, 0x100000, 0x20000, 1, 0, 0};
    A840KmdSparseBindReplyV3 rep{};
    CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_client_teardown(&f.mgr, 1, nullptr) == A840_KMD_OK);
    CHECK(f.sync_mock.lock_depth.load() == 0);
    CHECK(f.sync_mock.locks.load() == f.sync_mock.unlocks.load());
}

static void case_67_sparse_32op_batch_and_checkerboard_chunked_unbind() {
    SparseFixture f;
    f.init(true);
    uint64_t zp_phys = f.bind_client_zp(1);
    uint64_t rid = 0;
    CHECK(a840_kmd_sparse_region_create(&f.mgr, 1, nullptr, 0x100000, 128u * 65536u, &rid) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(&f.mgr, 1, 1, 0x500000, 64u * 65536u, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);

    /* Two 32-op batches binding all 64 even tiles (64 disconnected runs). */
    for (uint32_t half = 0; half < 2u; ++half) {
        auto req = make_req(32u, rid);
        for (uint32_t i = 0; i < 32u; ++i) {
            uint32_t tile = (half * 32u + i) * 2u;
            req.ops[i] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE,
                0x100000 + tile * 65536u, 65536u, 1, (half * 32u + i) * 65536u, 0};
        }
        A840KmdSparseBindReplyV3 rep{};
        CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, 1, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_OK);
        CHECK(rep.completed_ops == 32u);
    }

    uint32_t unbound = 0;
    CHECK(a840_kmd_sparse_unbind_bo(&f.mgr, 1, 1, 1u, &unbound, nullptr) == A840_KMD_OK);
    CHECK(unbound == 64u);
    CHECK(f.smmu_mock.shadow_ptes[1].size() == 128u);
    for (auto &kv : f.smmu_mock.shadow_ptes[1]) {
        CHECK(kv.second.is_zero_page == 1u && kv.second.phys_addr == zp_phys);
    }
}

static void case_68_bo_pool_release_empty_bo_never_parks() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d) == A840_KMD_OK);
    A840KmdPurgeResult pr{};
    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, 1, d.bo_handle, A840_PURGE_EMPTY, &pr) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, d.bo_handle, 1u) == A840_KMD_OK);

    A840KmdBoPoolStats st{};
    CHECK(a840_kmd_bo_pool_get_stats(&pool, &st) == A840_KMD_OK);
    CHECK(st.parked_bos == 0u);
    CHECK(st.active_bos == 0u);
    CHECK(st.empty_bos == 0u);
    CHECK(mock.live_pages.load() == 0);
    a840_kmd_bo_pool_destroy(&pool);
}

static void case_69_sparse_destroy_lingering_regions() {
    ZeroPageMock zp_mock;
    SparseSmmuMock smmu_mock;
    A840KmdZeroPageManager zp{};
    A840KmdSparseManager mgr{};
    A840KmdSmmuOps zops = zp_mock.ops();
    CHECK(a840_kmd_zero_page_init(&zp, &zops, 0x10000) == A840_KMD_OK);
    NativeAdrenoAbiZeroPageReplyV3 zrep{};
    CHECK(a840_kmd_zero_page_bind_client(&zp, 1, nullptr, 0, A840_KMD_SMMU_PERM_READ_ONLY, &zrep) == A840_KMD_OK);

    A840KmdSparseSmmuOps sops = smmu_mock.ops();
    A840KmdSparseConfig cfg{};
    cfg.zero_page_mgr = &zp;
    cfg.smmu_ops = &sops;
    CHECK(a840_kmd_sparse_init(&mgr, &cfg) == A840_KMD_OK);

    uint64_t r1 = 0, r2 = 0;
    CHECK(a840_kmd_sparse_region_create(&mgr, 1, nullptr, 0x100000, 0x20000, &r1) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_region_create(&mgr, 1, nullptr, 0x200000, 0x20000, &r2) == A840_KMD_OK);
    a840_kmd_sparse_destroy(&mgr);
    CHECK(smmu_mock.shadow_ptes[1].empty());
    a840_kmd_zero_page_destroy(&zp);
}

static void case_70_bo_pool_destroy_lingering_bos() {
    BoBackingMock mock;
    A840KmdBoPool pool{};
    A840KmdBoPoolBackingOps bops = mock.ops();
    A840KmdBoPoolConfig cfg{};
    cfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &cfg) == A840_KMD_OK);

    auto req = make_acq(1, 65536u);
    A840KmdBoDesc d1{}, d2{};
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d1) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d2) == A840_KMD_OK);
    CHECK(a840_kmd_bo_pool_release(&pool, 1, d2.bo_handle, 1u) == A840_KMD_OK);
    a840_kmd_bo_pool_destroy(&pool);
    CHECK(mock.live_pages.load() == 0);
}

static void case_71_concurrent_sparse_4_clients() {
    SparseFixture f;
    f.init();
    for (uint64_t c = 1; c <= 4; ++c)
        f.bind_client_zp(c, reinterpret_cast<void *>(c));

    std::vector<std::thread> workers;
    for (uint64_t c = 1; c <= 4; ++c) {
        workers.emplace_back([&, c]() {
            void *ctx = reinterpret_cast<void *>(c);
            for (int iter = 0; iter < 15; ++iter) {
                uint64_t rid = 0;
                uint64_t base_va = 0x1000000u + static_cast<uint64_t>(iter % 2) * 0x100000u;
                CHECK(a840_kmd_sparse_region_create(&f.mgr, c, ctx, base_va, 4u * 65536u, &rid) == A840_KMD_OK);
                uint64_t bo1 = static_cast<uint64_t>(iter * 2 + 1);
                uint64_t bo2 = static_cast<uint64_t>(iter * 2 + 2);
                CHECK(a840_kmd_sparse_register_bo(&f.mgr, c, bo1, 0x10000000u + c * 0x1000000u, 2u * 65536u,
                    A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);
                CHECK(a840_kmd_sparse_register_bo(&f.mgr, c, bo2, 0x20000000u + c * 0x1000000u, 2u * 65536u,
                    A840_KMD_SMMU_PERM_READ_ONLY) == A840_KMD_OK);

                A840KmdSparseBindReplyV3 rep{};
                auto req = make_req(3, rid);
                req.ops[0] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, base_va, 2u * 65536u, bo1, 0, 0};
                req.ops[1] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_ONLY, base_va + 2u * 65536u, 2u * 65536u, bo2, 0, 0};
                req.ops[2] = {A840_KMD_SPARSE_OP_QUERY_TILE, 0, base_va, 65536u, 0, 0, 0};
                CHECK(a840_kmd_sparse_dispatch_v3(&f.mgr, c, &req, sizeof(req), &rep, sizeof(rep)) == A840_KMD_OK);
                CHECK(rep.results[2].bo_handle == bo1);

                uint32_t unbound = 0;
                CHECK(a840_kmd_sparse_unbind_bo(&f.mgr, c, bo1, 1u, &unbound, nullptr) == A840_KMD_OK);
                CHECK(unbound == 2u);

                A840KmdSparseTileInfo ti{};
                CHECK(a840_kmd_sparse_query_tile(&f.mgr, c, base_va, &ti) == A840_KMD_OK);
                CHECK(ti.is_zero_page == 1u);

                CHECK(a840_kmd_sparse_client_teardown(&f.mgr, c, nullptr) == A840_KMD_OK);
            }
        });
    }
    for (auto &t : workers)
        t.join();

    A840KmdSparseStats st{};
    CHECK(a840_kmd_sparse_get_stats(&f.mgr, &st) == A840_KMD_OK);
    CHECK(st.active_regions == 0u);
    CHECK(st.active_bos == 0u);
    CHECK(st.active_bound_tiles == 0u);
    CHECK(st.active_zero_tiles == 0u);
    for (uint64_t c = 1; c <= 4; ++c)
        CHECK(f.smmu_mock.shadow_ptes[c].empty());
}

static void run_combined_stress_round(int round_idx) {
    ZeroPageMock zp_mock;
    SparseSmmuMock smmu_mock;
    BoBackingMock bo_mock;
    A840KmdZeroPageManager zp{};
    A840KmdSparseManager sparse{};
    A840KmdBoPool pool{};

    A840KmdSmmuOps zops = zp_mock.ops();
    CHECK(a840_kmd_zero_page_init(&zp, &zops, 0x10000) == A840_KMD_OK);
    for (uint64_t c = 1; c <= 4; ++c) {
        NativeAdrenoAbiZeroPageReplyV3 zrep{};
        CHECK(a840_kmd_zero_page_bind_client(&zp, c, reinterpret_cast<void *>(c), 0,
            A840_KMD_SMMU_PERM_READ_ONLY, &zrep) == A840_KMD_OK);
    }

    A840KmdSparseSmmuOps sops = smmu_mock.ops();
    A840KmdSparseConfig scfg{};
    scfg.zero_page_mgr = &zp;
    scfg.smmu_ops = &sops;
    CHECK(a840_kmd_sparse_init(&sparse, &scfg) == A840_KMD_OK);

    A840KmdBoPoolBackingOps bops = bo_mock.ops(true);
    A840KmdBoPoolConfig bcfg{};
    bcfg.max_parked_bytes = 512u * 1024u;
    bcfg.max_parked_per_bucket = 4u;
    bcfg.scrub_on_park = static_cast<uint32_t>(round_idx % 2);
    bcfg.parked_ttl_ns = 0u;
    bcfg.zero_page_mgr = &zp;
    bcfg.backing_ops = &bops;
    CHECK(a840_kmd_bo_pool_init(&pool, &bcfg) == A840_KMD_OK);

    std::atomic<bool> stop_pressure{false};
    std::thread pressure_thread([&]() {
        while (!stop_pressure.load(std::memory_order_acquire)) {
            A840KmdPressureReport rep{};
            CHECK(a840_kmd_bo_pool_memory_pressure(&pool, 128u * 1024u, &rep) == A840_KMD_OK);
            CHECK(a840_kmd_bo_pool_sweep_ttl(&pool, nullptr) == A840_KMD_OK);
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> clients;
    for (uint64_t c = 1; c <= 4; ++c) {
        clients.emplace_back([&, c]() {
            void *ctx = reinterpret_cast<void *>(c);
            uint8_t tag = static_cast<uint8_t>(0x10u + c);
            for (int iter = 0; iter < 12; ++iter) {
                /* 1) Acquire parked/new BO and verify zero-scrubbed isolation. */
                auto req = make_acq(c, 65536u, A840_KMD_SMMU_PERM_READ_WRITE,
                    A840_KMD_BO_CACHE_WRITE_COMBINE, 65536u, ctx);
                A840KmdBoDesc d1{}, d2{};
                CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d1) == A840_KMD_OK);
                CHECK(d1.cpu_ptr != nullptr);
                {
                    const auto *p = static_cast<const uint8_t *>(d1.cpu_ptr);
                    CHECK(p[0] == 0u && p[4095] == 0u && p[65535] == 0u);
                    std::memset(d1.cpu_ptr, tag, 65536u);
                }

                /* 2) Volatile + in-flight submit protection under concurrent pressure. */
                CHECK(a840_kmd_bo_pool_acquire(&pool, &req, &d2) == A840_KMD_OK);
                A840KmdPurgeResult pr{};
                CHECK(a840_kmd_bo_pool_set_purgeable(&pool, c, d2.bo_handle, A840_PURGE_VOLATILE, &pr) == A840_KMD_OK);
                A840KmdStatus bst = a840_kmd_bo_pool_begin_submit(&pool, c, d2.bo_handle, nullptr);
                if (bst == A840_KMD_OK) {
                    /* While in_flight_refs > 0, pressure thread MUST NOT purge d2 to EMPTY! */
                    A840KmdBoDesc q{};
                    CHECK(a840_kmd_bo_pool_query_bo(&pool, c, d2.bo_handle, &q) == A840_KMD_OK);
                    CHECK(q.purgeable_state != A840_PURGE_EMPTY);
                    CHECK(q.in_flight_refs >= 1u);
                    CHECK(a840_kmd_bo_pool_end_submit(&pool, c, d2.bo_handle, nullptr) == A840_KMD_OK);
                } else {
                    /* Pressure thread raced and purged d2 before begin_submit -> fail-closed! */
                    CHECK(bst == A840_KMD_INVALID_STATE);
                    CHECK(a840_kmd_bo_pool_set_purgeable(&pool, c, d2.bo_handle, A840_PURGE_NONVOLATILE, &pr) == A840_KMD_OK);
                    CHECK(pr.was_purged == 1u);
                }

                /* 3) Sparse region + bind d1 + unbind + teardown. */
                uint64_t rid = 0;
                uint64_t sva = 0x2000000u + static_cast<uint64_t>(iter % 3) * 0x100000u;
                CHECK(a840_kmd_sparse_region_create(&sparse, c, ctx, sva, 2u * 65536u, &rid) == A840_KMD_OK);
                CHECK(a840_kmd_sparse_register_bo(&sparse, c, d1.bo_handle, d1.phys_addr, d1.bucket_bytes,
                    A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);

                A840KmdSparseBindReplyV3 srep{};
                auto sreq = make_req(2, rid);
                sreq.ops[0] = {A840_KMD_SPARSE_OP_BIND_BO, A840_KMD_SMMU_PERM_READ_WRITE, sva, 65536u, d1.bo_handle, 0, 0};
                sreq.ops[1] = {A840_KMD_SPARSE_OP_UNBIND_TO_ZERO, A840_KMD_SMMU_PERM_READ_ONLY, sva, 65536u, 0, 0, 0};
                CHECK(a840_kmd_sparse_dispatch_v3(&sparse, c, &sreq, sizeof(sreq), &srep, sizeof(srep)) == A840_KMD_OK);

                CHECK(a840_kmd_sparse_unbind_bo(&sparse, c, d1.bo_handle, 1u, nullptr, nullptr) == A840_KMD_OK);
                CHECK(a840_kmd_sparse_region_destroy(&sparse, c, rid) == A840_KMD_OK);

                /* 4) Release d1 into parked pool and d2 either parked or freed. */
                CHECK(a840_kmd_bo_pool_release(&pool, c, d1.bo_handle, 1u) == A840_KMD_OK);
                CHECK(a840_kmd_bo_pool_release(&pool, c, d2.bo_handle, static_cast<uint32_t>(iter % 2)) == A840_KMD_OK);

                if (iter == 5 || iter == 11) {
                    CHECK(a840_kmd_sparse_client_teardown(&sparse, c, nullptr) == A840_KMD_OK);
                    CHECK(a840_kmd_bo_pool_client_teardown(&pool, c, nullptr) == A840_KMD_OK);
                }
            }
        });
    }

    for (auto &t : clients)
        t.join();
    stop_pressure.store(true, std::memory_order_release);
    pressure_thread.join();

    A840KmdSparseStats sst{};
    CHECK(a840_kmd_sparse_get_stats(&sparse, &sst) == A840_KMD_OK);
    CHECK(sst.active_regions == 0u);
    CHECK(sst.active_bos == 0u);
    CHECK(sst.active_bound_tiles == 0u);
    CHECK(sst.active_zero_tiles == 0u);

    A840KmdBoPoolStats bst{};
    CHECK(a840_kmd_bo_pool_get_stats(&pool, &bst) == A840_KMD_OK);
    CHECK(bst.active_bos == 0u);
    CHECK(bst.parked_bos == 0u);
    CHECK(bst.parked_bytes == 0u);
    CHECK(bo_mock.live_pages.load() == 0);

    a840_kmd_bo_pool_destroy(&pool);
    a840_kmd_sparse_destroy(&sparse);
    a840_kmd_zero_page_destroy(&zp);
    CHECK(bo_mock.live_pages.load() == 0);
}

static void case_72_concurrent_combined_stress() {
    for (int r = 0; r < g_stress_rounds; ++r)
        run_combined_stress_round(r);
}

} /* namespace */

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tsan-stress") == 0 && (i + 1) < argc) {
            g_stress_rounds = std::atoi(argv[++i]);
            if (g_stress_rounds < 1)
                g_stress_rounds = 1;
        }
    }

    RUN_CASE(case_01_abi_struct_sizes_and_offsets);
    RUN_CASE(case_02_abi_envelope_size_reject);
    RUN_CASE(case_03_abi_wrong_version_reject);
    RUN_CASE(case_04_abi_reserved_header_fields_reject);
    RUN_CASE(case_05_abi_op_count_bounds_reject);
    RUN_CASE(case_06_abi_nonzero_trailing_ops_reject);
    RUN_CASE(case_07_abi_op_reserved0_reject);
    RUN_CASE(case_08_abi_unknown_opcode_reject);
    RUN_CASE(case_09_abi_atomic_batch_fail_closed);
    RUN_CASE(case_10_sparse_init_validation);

    RUN_CASE(case_11_sparse_region_create_default_zero_page);
    RUN_CASE(case_12_sparse_region_requires_bound_zero_page);
    RUN_CASE(case_13_sparse_region_alignment_reject);
    RUN_CASE(case_14_sparse_region_va_window_reject);
    RUN_CASE(case_15_sparse_region_overlap_existing_reject);
    RUN_CASE(case_16_sparse_region_overlap_zero_page_va_reject);
    RUN_CASE(case_17_sparse_region_overlap_normal_range_reject);
    RUN_CASE(case_18_sparse_external_overlap_callback_outside_lock);
    RUN_CASE(case_19_sparse_same_va_different_clients_isolated);
    RUN_CASE(case_20_sparse_region_create_smmu_failure_rollback);

    RUN_CASE(case_21_sparse_bind_bo_partial_tiles);
    RUN_CASE(case_22_sparse_bind_overwrite_updates_refcounts);
    RUN_CASE(case_23_sparse_readonly_bo_escalation_reject);
    RUN_CASE(case_24_sparse_bo_offset_bounds_reject);
    RUN_CASE(case_25_sparse_zero_page_write_reject);
    RUN_CASE(case_26_sparse_unbind_to_zero_restores_ro_zero_page);
    RUN_CASE(case_27_sparse_bind_zero_page_op_semantics);
    RUN_CASE(case_28_sparse_query_tile_mixed_batch);
    RUN_CASE(case_29_sparse_bo_early_destroy_auto_unbind);
    RUN_CASE(case_30_sparse_bo_unbind_keep_registered);
    RUN_CASE(case_31_sparse_cross_client_bo_forgery_reject);
    RUN_CASE(case_32_sparse_cross_client_region_and_query_reject);
    RUN_CASE(case_33_sparse_client_teardown_coalesced_tlb_and_isolation);

    RUN_CASE(case_34_bo_pool_bucket_sizes_and_selection);
    RUN_CASE(case_35_bo_pool_init_and_arg_validation);
    RUN_CASE(case_36_bo_pool_same_client_reuse_and_zero_scrub);
    RUN_CASE(case_37_bo_pool_scrub_on_park_policy);
    RUN_CASE(case_38_bo_pool_scrub_failure_fail_closed);
    RUN_CASE(case_39_bo_pool_cross_client_park_strictly_blocked);
    RUN_CASE(case_40_bo_pool_attribute_mismatch_no_reuse);
    RUN_CASE(case_41_bo_pool_release_allow_park_zero_unmaps);
    RUN_CASE(case_42_bo_pool_oversized_gt_16mib_not_parked);
    RUN_CASE(case_43_bo_pool_bucket_cap_lru_eviction);
    RUN_CASE(case_44_bo_pool_total_bytes_cap_lru_eviction);
    RUN_CASE(case_45_bo_pool_ttl_expiry_sweep_and_acquire);
    RUN_CASE(case_46_bo_pool_synthetic_mode_no_backing_ops);
    RUN_CASE(case_47_bo_pool_custom_sync_lock_balance_and_order);
    RUN_CASE(case_48_bo_pool_cross_client_ops_reject);
    RUN_CASE(case_49_bo_pool_client_teardown_reclaims_all);
    RUN_CASE(case_50_bo_pool_release_while_inflight_defers_destroy);

    RUN_CASE(case_51_volatile_initial_state_and_keep_current);
    RUN_CASE(case_52_volatile_roundtrip_without_pressure);
    RUN_CASE(case_53_volatile_inflight_protected_from_pressure);
    RUN_CASE(case_54_volatile_multi_inflight_refcount_protection);
    RUN_CASE(case_55_pressure_reclaims_parked_before_volatile);
    RUN_CASE(case_56_pressure_spills_from_parked_to_volatile_lru);
    RUN_CASE(case_57_volatile_purge_receives_zero_page_phys);
    RUN_CASE(case_58_empty_bo_submit_rejected_fail_closed);
    RUN_CASE(case_59_empty_to_nonvolatile_reports_was_purged_and_rehydrates);
    RUN_CASE(case_60_empty_to_volatile_stays_empty);
    RUN_CASE(case_61_empty_keep_current_reports_was_purged);
    RUN_CASE(case_62_explicit_set_empty_respects_inflight);
    RUN_CASE(case_63_rehydrate_failure_keeps_bo_empty);
    RUN_CASE(case_64_nonvolatile_never_purged_under_pressure);
    RUN_CASE(case_65_integrated_sparse_and_bo_pool_purge_fallback);

    RUN_CASE(case_66_sparse_custom_sync_lock_balance);
    RUN_CASE(case_67_sparse_32op_batch_and_checkerboard_chunked_unbind);
    RUN_CASE(case_68_bo_pool_release_empty_bo_never_parks);
    RUN_CASE(case_69_sparse_destroy_lingering_regions);
    RUN_CASE(case_70_bo_pool_destroy_lingering_bos);
    RUN_CASE(case_71_concurrent_sparse_4_clients);
    RUN_CASE(case_72_concurrent_combined_stress);

    std::printf("PASS phase4_memory: %d independent cases, %d stress rounds\n",
        g_cases_passed, g_stress_rounds);
    return 0;
}
