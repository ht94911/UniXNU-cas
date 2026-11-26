/* SPDX-License-Identifier: MIT */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "NativeAdrenoABI_v3.h"
#include "a840_abi_v3_host.h"
#include "a840_kmd_zero_page.h"
#include "a840_kmd_wait_any.h"
#include "a840_kmd_fault_capture.h"
#include "a840_kmd_userclient_v3.h"

namespace {

static uint64_t g_checks = 0;
static uint64_t g_cases = 0;
static uint64_t g_failures = 0;

#define EXPECT(cond)                                                          \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,      \
                         #cond);                                              \
            ++g_failures;                                                     \
            return false;                                                     \
        }                                                                     \
    } while (0)

#define RUN_CASE(fn)                                                          \
    do {                                                                      \
        ++g_cases;                                                            \
        if (!fn()) {                                                          \
            std::fprintf(stderr, "CASE FAILED: %s\n", #fn);                   \
            return 1;                                                         \
        }                                                                     \
    } while (0)

constexpr uint64_t kDefaultZeroVa = UINT64_C(0x0000100000000000);
constexpr uint64_t kAltZeroVa = UINT64_C(0x0000100000010000);
constexpr uint64_t kPhysBase = UINT64_C(0x0000000080000000);
constexpr uint64_t kPackGpuVa = UINT64_C(0x0000200000000000);
constexpr uint64_t kPackCpuVa = UINT64_C(0x0000700000000000);
constexpr uint64_t kStackKey = UINT64_C(0x1122334455667788);

struct MockSmmu {
    std::mutex mu;
    bool fill_dirty_on_alloc = true;
    bool fail_alloc = false;
    bool return_bad_phys = false;
    bool fail_map = false;
    bool fail_unmap = false;
    uint32_t fail_map_on_call = 0;
    uint32_t alloc_calls = 0;
    uint32_t free_calls = 0;
    uint32_t map_calls = 0;
    uint32_t unmap_calls = 0;
    uint64_t last_mapped_phys = 0;
    uint32_t last_mapped_perms = 0;
    uint64_t live_allocs = 0;
    std::vector<uint8_t> *live_buffer = nullptr;
    std::unordered_map<uint64_t, uint64_t> active_maps;

    ~MockSmmu() {
        delete live_buffer;
        live_buffer = nullptr;
    }

    static A840KmdStatus alloc_cb(void *cookie, uint64_t bytes,
                                  uint64_t *out_phys, void **out_cpu,
                                  void **out_handle) {
        auto *self = static_cast<MockSmmu *>(cookie);
        std::lock_guard<std::mutex> g(self->mu);
        self->alloc_calls++;
        if (self->fail_alloc)
            return A840_KMD_NO_MEMORY;
        auto *buf = new std::vector<uint8_t>(static_cast<size_t>(bytes),
            self->fill_dirty_on_alloc ? static_cast<uint8_t>(0xA5) : static_cast<uint8_t>(0));
        self->live_allocs++;
        self->live_buffer = buf;
        *out_phys = self->return_bad_phys ? (kPhysBase + 4096u)
                                          : (kPhysBase + uint64_t(self->alloc_calls) * A840_KMD_ZERO_PAGE_BYTES);
        *out_cpu = buf->data();
        *out_handle = buf;
        return A840_KMD_OK;
    }

    static void free_cb(void *cookie, uint64_t phys, void *handle, uint64_t bytes) {
        (void)phys;
        (void)bytes;
        auto *self = static_cast<MockSmmu *>(cookie);
        std::lock_guard<std::mutex> g(self->mu);
        self->free_calls++;
        auto *buf = static_cast<std::vector<uint8_t> *>(handle);
        if (buf == self->live_buffer)
            self->live_buffer = nullptr;
        delete buf;
        if (self->live_allocs > 0)
            self->live_allocs--;
    }

    static A840KmdStatus map_cb(void *cookie, uint64_t client_id, void *smmu_ctx,
                                uint64_t gpu_va, uint64_t phys, uint64_t bytes,
                                uint32_t perms) {
        (void)smmu_ctx;
        (void)bytes;
        auto *self = static_cast<MockSmmu *>(cookie);
        std::lock_guard<std::mutex> g(self->mu);
        self->map_calls++;
        if (self->fail_map ||
            (self->fail_map_on_call != 0 && self->map_calls == self->fail_map_on_call))
            return A840_KMD_IO_ERROR;
        if (perms != A840_KMD_SMMU_PERM_READ_ONLY)
            return A840_KMD_PERMISSION_DENIED;
        self->last_mapped_phys = phys;
        self->last_mapped_perms = perms;
        self->active_maps[client_id] = gpu_va;
        return A840_KMD_OK;
    }

    static A840KmdStatus unmap_cb(void *cookie, uint64_t client_id, void *smmu_ctx,
                                  uint64_t gpu_va, uint64_t bytes) {
        (void)smmu_ctx;
        (void)bytes;
        auto *self = static_cast<MockSmmu *>(cookie);
        std::lock_guard<std::mutex> g(self->mu);
        if (self->fail_unmap)
            return A840_KMD_IO_ERROR;
        auto it = self->active_maps.find(client_id);
        if (it == self->active_maps.end() || it->second != gpu_va)
            return A840_KMD_NOT_FOUND;
        self->active_maps.erase(it);
        self->unmap_calls++;
        return A840_KMD_OK;
    }

    A840KmdSmmuOps ops() {
        A840KmdSmmuOps o{};
        o.cookie = this;
        o.alloc_backing_page = &MockSmmu::alloc_cb;
        o.free_backing_page = &MockSmmu::free_cb;
        o.map_client_ro = &MockSmmu::map_cb;
        o.unmap_client = &MockSmmu::unmap_cb;
        return o;
    }
};

struct MockBoTable {
    std::mutex mu;
    struct Key {
        uint64_t client_id;
        uint64_t bo_handle;
        bool operator==(const Key &o) const {
            return client_id == o.client_id && bo_handle == o.bo_handle;
        }
    };
    struct Hash {
        size_t operator()(const Key &k) const {
            return std::hash<uint64_t>{}(k.client_id * UINT64_C(11400714819323198485) ^ k.bo_handle);
        }
    };
    std::unordered_map<Key, uint64_t, Hash> bos;

    void add(uint64_t cid, uint64_t handle, uint64_t bytes) {
        std::lock_guard<std::mutex> g(mu);
        bos[{cid, handle}] = bytes;
    }

    static A840KmdStatus lookup_cb(void *cookie, uint64_t cid, uint64_t handle,
                                   uint64_t *out_bytes) {
        auto *self = static_cast<MockBoTable *>(cookie);
        std::lock_guard<std::mutex> g(self->mu);
        auto it = self->bos.find({cid, handle});
        if (it == self->bos.end())
            return A840_KMD_NOT_FOUND;
        *out_bytes = it->second;
        return A840_KMD_OK;
    }
};

struct PromiseEvent {
    uint64_t client_id;
    uint64_t token;
    uint32_t state;
    uint32_t op;
};

struct MockPromiseObserver {
    std::mutex mu;
    std::vector<PromiseEvent> events;

    static void on_event(void *cookie, uint64_t client_id,
                         const struct NativeAdrenoAbiWriterPromiseV3 *p,
                         uint32_t state) {
        auto *self = static_cast<MockPromiseObserver *>(cookie);
        std::lock_guard<std::mutex> g(self->mu);
        self->events.push_back({client_id, p->promise_token, state, p->op});
    }

    size_t count_state(uint64_t client_id, uint32_t state) {
        std::lock_guard<std::mutex> g(mu);
        size_t n = 0;
        for (const auto &e : events) {
            if ((client_id == 0 || e.client_id == client_id) && e.state == state)
                ++n;
        }
        return n;
    }
};

/* =========================================================================
 * Group A: ZeroPage Manager (14 cases)
 * ========================================================================= */

static bool test_zp_init_rejects_null_and_bad_va() {
    MockSmmu smmu;
    A840KmdSmmuOps ops = smmu.ops();
    A840KmdZeroPageManager mgr{};

    EXPECT(a840_kmd_zero_page_init(nullptr, &ops, kDefaultZeroVa) == A840_KMD_INVALID_ARG);
    EXPECT(a840_kmd_zero_page_init(&mgr, nullptr, kDefaultZeroVa) == A840_KMD_INVALID_ARG);

    A840KmdSmmuOps bad_ops = ops;
    bad_ops.map_client_ro = nullptr;
    EXPECT(a840_kmd_zero_page_init(&mgr, &bad_ops, kDefaultZeroVa) == A840_KMD_INVALID_ARG);

    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, 0u) == A840_KMD_INVALID_ARG);
    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, UINT64_C(0x1000)) == A840_KMD_ALIGNMENT);
    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, A840_KMD_GPU_VA_LIMIT) == A840_KMD_RANGE);
    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, A840_KMD_GPU_VA_LIMIT - 4096u) == A840_KMD_RANGE);

    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, A840_KMD_GPU_VA_LIMIT - A840_KMD_ZERO_PAGE_BYTES) == A840_KMD_OK);
    a840_kmd_zero_page_destroy(&mgr);
    return true;
}

static bool test_zp_lazy_alloc_zero_fills_dirty_backing_page() {
    MockSmmu smmu;
    smmu.fill_dirty_on_alloc = true;
    A840KmdSmmuOps ops = smmu.ops();
    A840KmdZeroPageManager mgr{};
    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, kDefaultZeroVa) == A840_KMD_OK);

    A840KmdZeroPageStats st{};
    EXPECT(a840_kmd_zero_page_get_stats(&mgr, &st) == A840_KMD_OK);
    EXPECT(st.active_clients == 0u);
    EXPECT(st.backing_allocated == 0u);
    EXPECT(smmu.alloc_calls == 0u);

    NativeAdrenoAbiZeroPageReplyV3 rep{};
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 101u, nullptr, 0u,
        A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_OK);
    EXPECT(rep.gpu_va == kDefaultZeroVa);
    EXPECT(rep.bytes == A840_KMD_ZERO_PAGE_BYTES);
    EXPECT(smmu.alloc_calls == 1u);
    EXPECT(smmu.live_buffer != nullptr);
    for (uint8_t b : *smmu.live_buffer) {
        EXPECT(b == 0u);
    }

    EXPECT(a840_kmd_zero_page_unbind_client(&mgr, 101u) == A840_KMD_OK);
    EXPECT(smmu.live_allocs == 0u);
    a840_kmd_zero_page_destroy(&mgr);
    return true;
}

static bool test_zp_rejects_write_and_rw_permissions() {
    MockSmmu smmu;
    A840KmdSmmuOps ops = smmu.ops();
    A840KmdZeroPageManager mgr{};
    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, kDefaultZeroVa) == A840_KMD_OK);

    NativeAdrenoAbiZeroPageReplyV3 rep{};
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 1u, nullptr, 0u,
        A840_KMD_SMMU_PERM_WRITE, &rep) == A840_KMD_PERMISSION_DENIED);
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 1u, nullptr, 0u,
        A840_KMD_SMMU_PERM_READ_WRITE, &rep) == A840_KMD_PERMISSION_DENIED);
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 1u, nullptr, 0u,
        0u, &rep) == A840_KMD_PERMISSION_DENIED);
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 1u, nullptr, 0u,
        (1u << 2) | A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_PERMISSION_DENIED);

    A840KmdZeroPageStats st{};
    EXPECT(a840_kmd_zero_page_get_stats(&mgr, &st) == A840_KMD_OK);
    EXPECT(st.rejected_write_maps == 2u);
    EXPECT(st.total_allocs == 0u);
    EXPECT(st.total_maps == 0u);
    EXPECT(st.active_clients == 0u);
    a840_kmd_zero_page_destroy(&mgr);
    return true;
}

static bool test_zp_multi_client_shares_single_physical_page() {
    MockSmmu smmu;
    A840KmdSmmuOps ops = smmu.ops();
    A840KmdZeroPageManager mgr{};
    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, kDefaultZeroVa) == A840_KMD_OK);

    uint64_t first_phys = 0;
    for (uint64_t cid = 1; cid <= 5; ++cid) {
        NativeAdrenoAbiZeroPageReplyV3 rep{};
        uint64_t req_va = (cid % 2 == 0) ? (kAltZeroVa + cid * A840_KMD_ZERO_PAGE_BYTES) : 0u;
        EXPECT(a840_kmd_zero_page_bind_client(&mgr, cid, reinterpret_cast<void *>(cid * 16u),
            req_va, A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_OK);
        EXPECT(rep.gpu_va == (req_va ? req_va : kDefaultZeroVa));
        EXPECT(rep.bytes == A840_KMD_ZERO_PAGE_BYTES);
        EXPECT(smmu.last_mapped_perms == A840_KMD_SMMU_PERM_READ_ONLY);
        if (cid == 1)
            first_phys = smmu.last_mapped_phys;
        else
            EXPECT(smmu.last_mapped_phys == first_phys);
    }

    A840KmdZeroPageStats st{};
    EXPECT(a840_kmd_zero_page_get_stats(&mgr, &st) == A840_KMD_OK);
    EXPECT(st.active_clients == 5u);
    EXPECT(st.backing_allocated == 1u);
    EXPECT(st.total_allocs == 1u);
    EXPECT(st.total_maps == 5u);
    EXPECT(smmu.alloc_calls == 1u);
    EXPECT(smmu.map_calls == 5u);

    for (uint64_t cid = 1; cid <= 5; ++cid)
        EXPECT(a840_kmd_zero_page_unbind_client(&mgr, cid) == A840_KMD_OK);
    a840_kmd_zero_page_destroy(&mgr);
    return true;
}

static bool test_zp_idempotent_bind_and_query_same_client() {
    MockSmmu smmu;
    A840KmdSmmuOps ops = smmu.ops();
    A840KmdZeroPageManager mgr{};
    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, kDefaultZeroVa) == A840_KMD_OK);

    NativeAdrenoAbiZeroPageReplyV3 r1{}, r2{}, r3{}, rq{};
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 7u, nullptr, kAltZeroVa,
        A840_KMD_SMMU_PERM_READ_ONLY, &r1) == A840_KMD_OK);
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 7u, nullptr, 0u,
        A840_KMD_SMMU_PERM_READ_ONLY, &r2) == A840_KMD_OK);
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 7u, nullptr, kAltZeroVa,
        A840_KMD_SMMU_PERM_READ_ONLY, &r3) == A840_KMD_OK);
    EXPECT(a840_kmd_zero_page_query_client(&mgr, 7u, &rq) == A840_KMD_OK);

    EXPECT(r1.gpu_va == kAltZeroVa && r2.gpu_va == kAltZeroVa &&
           r3.gpu_va == kAltZeroVa && rq.gpu_va == kAltZeroVa);
    EXPECT(smmu.map_calls == 1u);
    EXPECT(smmu.alloc_calls == 1u);

    A840KmdZeroPageStats st{};
    EXPECT(a840_kmd_zero_page_get_stats(&mgr, &st) == A840_KMD_OK);
    EXPECT(st.active_clients == 1u);
    a840_kmd_zero_page_destroy(&mgr);
    return true;
}

static bool test_zp_rebind_different_va_rejected_state_conflict() {
    MockSmmu smmu;
    A840KmdSmmuOps ops = smmu.ops();
    A840KmdZeroPageManager mgr{};
    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, kDefaultZeroVa) == A840_KMD_OK);

    NativeAdrenoAbiZeroPageReplyV3 rep{};
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 9u, nullptr, kDefaultZeroVa,
        A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_OK);
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 9u, nullptr, kAltZeroVa,
        A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_STATE_CONFLICT);
    EXPECT(smmu.map_calls == 1u);
    a840_kmd_zero_page_destroy(&mgr);
    return true;
}

static bool test_zp_unbind_refcount_and_last_client_frees_backing() {
    MockSmmu smmu;
    A840KmdSmmuOps ops = smmu.ops();
    A840KmdZeroPageManager mgr{};
    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, kDefaultZeroVa) == A840_KMD_OK);

    NativeAdrenoAbiZeroPageReplyV3 rep{};
    for (uint64_t cid = 1; cid <= 4; ++cid)
        EXPECT(a840_kmd_zero_page_bind_client(&mgr, cid, nullptr, 0u,
            A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_OK);

    A840KmdZeroPageStats st{};
    for (uint64_t cid = 1; cid <= 3; ++cid) {
        EXPECT(a840_kmd_zero_page_unbind_client(&mgr, cid) == A840_KMD_OK);
        EXPECT(a840_kmd_zero_page_get_stats(&mgr, &st) == A840_KMD_OK);
        EXPECT(st.active_clients == 4u - static_cast<uint32_t>(cid));
        EXPECT(st.backing_allocated == 1u);
        EXPECT(st.total_frees == 0u);
    }

    EXPECT(a840_kmd_zero_page_unbind_client(&mgr, 4u) == A840_KMD_OK);
    EXPECT(a840_kmd_zero_page_get_stats(&mgr, &st) == A840_KMD_OK);
    EXPECT(st.active_clients == 0u);
    EXPECT(st.backing_allocated == 0u);
    EXPECT(st.backing_phys_addr == 0u);
    EXPECT(st.total_allocs == 1u);
    EXPECT(st.total_frees == 1u);
    EXPECT(st.total_maps == 4u);
    EXPECT(st.total_unmaps == 4u);
    EXPECT(smmu.live_allocs == 0u);
    EXPECT(smmu.active_maps.empty());
    a840_kmd_zero_page_destroy(&mgr);
    return true;
}

static bool test_zp_rebind_after_zero_allocates_fresh_backing() {
    MockSmmu smmu;
    A840KmdSmmuOps ops = smmu.ops();
    A840KmdZeroPageManager mgr{};
    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, kDefaultZeroVa) == A840_KMD_OK);

    NativeAdrenoAbiZeroPageReplyV3 rep{};
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 1u, nullptr, 0u,
        A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_OK);
    EXPECT(a840_kmd_zero_page_unbind_client(&mgr, 1u) == A840_KMD_OK);

    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 2u, nullptr, 0u,
        A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_OK);
    A840KmdZeroPageStats st{};
    EXPECT(a840_kmd_zero_page_get_stats(&mgr, &st) == A840_KMD_OK);
    EXPECT(st.total_allocs == 2u);
    EXPECT(st.total_frees == 1u);
    EXPECT(st.backing_allocated == 1u);
    EXPECT(a840_kmd_zero_page_unbind_client(&mgr, 2u) == A840_KMD_OK);
    EXPECT(a840_kmd_zero_page_get_stats(&mgr, &st) == A840_KMD_OK);
    EXPECT(st.total_frees == 2u);
    EXPECT(smmu.live_allocs == 0u);
    a840_kmd_zero_page_destroy(&mgr);
    return true;
}

static bool test_zp_unbind_unknown_or_double_unbind_not_found() {
    MockSmmu smmu;
    A840KmdSmmuOps ops = smmu.ops();
    A840KmdZeroPageManager mgr{};
    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, kDefaultZeroVa) == A840_KMD_OK);

    EXPECT(a840_kmd_zero_page_unbind_client(&mgr, 0u) == A840_KMD_INVALID_ARG);
    EXPECT(a840_kmd_zero_page_unbind_client(&mgr, 55u) == A840_KMD_NOT_FOUND);

    NativeAdrenoAbiZeroPageReplyV3 rep{};
    EXPECT(a840_kmd_zero_page_query_client(&mgr, 55u, &rep) == A840_KMD_NOT_FOUND);
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 55u, nullptr, 0u,
        A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_OK);
    EXPECT(a840_kmd_zero_page_unbind_client(&mgr, 55u) == A840_KMD_OK);
    EXPECT(a840_kmd_zero_page_unbind_client(&mgr, 55u) == A840_KMD_NOT_FOUND);
    a840_kmd_zero_page_destroy(&mgr);
    return true;
}

static bool test_zp_rollback_on_first_client_map_failure() {
    MockSmmu smmu;
    smmu.fail_map = true;
    A840KmdSmmuOps ops = smmu.ops();
    A840KmdZeroPageManager mgr{};
    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, kDefaultZeroVa) == A840_KMD_OK);

    NativeAdrenoAbiZeroPageReplyV3 rep{};
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 1u, nullptr, 0u,
        A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_IO_ERROR);

    A840KmdZeroPageStats st{};
    EXPECT(a840_kmd_zero_page_get_stats(&mgr, &st) == A840_KMD_OK);
    EXPECT(st.active_clients == 0u);
    EXPECT(st.backing_allocated == 0u);
    EXPECT(st.total_allocs == 1u);
    EXPECT(st.total_frees == 1u);
    EXPECT(smmu.live_allocs == 0u);
    a840_kmd_zero_page_destroy(&mgr);
    return true;
}

static bool test_zp_rollback_on_alloc_failure_and_bad_phys() {
    MockSmmu smmu;
    smmu.fail_alloc = true;
    A840KmdSmmuOps ops = smmu.ops();
    A840KmdZeroPageManager mgr{};
    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, kDefaultZeroVa) == A840_KMD_OK);

    NativeAdrenoAbiZeroPageReplyV3 rep{};
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 1u, nullptr, 0u,
        A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_NO_MEMORY);
    EXPECT(smmu.live_allocs == 0u);

    smmu.fail_alloc = false;
    smmu.return_bad_phys = true;
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 1u, nullptr, 0u,
        A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_ALIGNMENT);
    EXPECT(smmu.live_allocs == 0u);
    a840_kmd_zero_page_destroy(&mgr);
    return true;
}

static bool test_zp_unmap_failure_preserves_backing_safety() {
    MockSmmu smmu;
    A840KmdSmmuOps ops = smmu.ops();
    A840KmdZeroPageManager mgr{};
    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, kDefaultZeroVa) == A840_KMD_OK);

    NativeAdrenoAbiZeroPageReplyV3 rep{};
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 1u, nullptr, 0u,
        A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_OK);
    smmu.fail_unmap = true;
    EXPECT(a840_kmd_zero_page_unbind_client(&mgr, 1u) == A840_KMD_IO_ERROR);
    EXPECT(smmu.live_allocs == 1u);

    smmu.fail_unmap = false;
    EXPECT(a840_kmd_zero_page_unbind_client(&mgr, 1u) == A840_KMD_OK);
    EXPECT(smmu.live_allocs == 0u);
    a840_kmd_zero_page_destroy(&mgr);
    return true;
}

static bool test_zp_capacity_limit_128_clients() {
    MockSmmu smmu;
    A840KmdSmmuOps ops = smmu.ops();
    A840KmdZeroPageManager mgr{};
    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, kDefaultZeroVa) == A840_KMD_OK);

    NativeAdrenoAbiZeroPageReplyV3 rep{};
    for (uint64_t cid = 1; cid <= A840_KMD_ZERO_PAGE_MAX_CLIENTS; ++cid)
        EXPECT(a840_kmd_zero_page_bind_client(&mgr, cid, nullptr, 0u,
            A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_OK);

    EXPECT(a840_kmd_zero_page_bind_client(&mgr, A840_KMD_ZERO_PAGE_MAX_CLIENTS + 1u,
        nullptr, 0u, A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_CAPACITY);

    for (uint64_t cid = 1; cid <= A840_KMD_ZERO_PAGE_MAX_CLIENTS; ++cid)
        EXPECT(a840_kmd_zero_page_unbind_client(&mgr, cid) == A840_KMD_OK);
    EXPECT(smmu.live_allocs == 0u);
    a840_kmd_zero_page_destroy(&mgr);
    return true;
}

static bool test_zp_uninitialized_and_destroy_cleanup() {
    A840KmdZeroPageManager uninit{};
    NativeAdrenoAbiZeroPageReplyV3 rep{};
    EXPECT(a840_kmd_zero_page_bind_client(&uninit, 1u, nullptr, 0u,
        A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_STATE_CONFLICT);

    MockSmmu smmu;
    A840KmdSmmuOps ops = smmu.ops();
    A840KmdZeroPageManager mgr{};
    EXPECT(a840_kmd_zero_page_init(&mgr, &ops, kDefaultZeroVa) == A840_KMD_OK);
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 1u, nullptr, 0u,
        A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_OK);
    EXPECT(a840_kmd_zero_page_bind_client(&mgr, 2u, nullptr, 0u,
        A840_KMD_SMMU_PERM_READ_ONLY, &rep) == A840_KMD_OK);
    a840_kmd_zero_page_destroy(&mgr);
    EXPECT(smmu.live_allocs == 0u);
    EXPECT(smmu.active_maps.empty());
    return true;
}

/* =========================================================================
 * Group B: Fault Capture & Single-Channel Isolation (10 cases)
 * ========================================================================= */

static bool test_fc_init_and_invalid_inputs() {
    A840KmdFaultCapture fc{};
    A840KmdFaultInput in{};
    EXPECT(a840_kmd_fault_init(nullptr) == A840_KMD_INVALID_ARG);
    EXPECT(a840_kmd_fault_record(&fc, &in, nullptr) == A840_KMD_STATE_CONFLICT);

    EXPECT(a840_kmd_fault_init(&fc) == A840_KMD_OK);
    EXPECT(a840_kmd_fault_record(&fc, nullptr, nullptr) == A840_KMD_INVALID_ARG);
    in.client_id = 0u;
    in.fault_kind = A840_KMD_FAULT_GPU_HANG;
    EXPECT(a840_kmd_fault_record(&fc, &in, nullptr) == A840_KMD_INVALID_ARG);
    in.client_id = 1u;
    in.fault_kind = A840_KMD_FAULT_NONE;
    EXPECT(a840_kmd_fault_record(&fc, &in, nullptr) == A840_KMD_INVALID_ARG);
    in.fault_kind = 99u;
    EXPECT(a840_kmd_fault_record(&fc, &in, nullptr) == A840_KMD_INVALID_ARG);
    a840_kmd_fault_destroy(&fc);
    return true;
}

static bool test_fc_record_smmu_and_ib_hang_all_fields() {
    A840KmdFaultCapture fc{};
    EXPECT(a840_kmd_fault_init(&fc) == A840_KMD_OK);

    A840KmdFaultInput in{};
    in.client_id = 42u;
    in.queue_id = 3u;
    in.fence = 12345u;
    in.ib1_pc = UINT64_C(0x0000100022223330);
    in.ib2_pc = UINT64_C(0x0000100044445550);
    in.rbbm_status = 0x80000001u;
    in.cp_status = 0x00001234u;
    in.smmu_fault_va = UINT64_C(0x00003000deadbeef);
    in.smmu_fsr = 0x40000002u;
    in.fault_kind = A840_KMD_FAULT_SMMU_TRANSLATION;
    in.timestamp_ticks = UINT64_C(9876543210);

    A840KmdFaultSnapshot out{}, latest{};
    EXPECT(a840_kmd_fault_record(&fc, &in, &out) == A840_KMD_OK);
    EXPECT(a840_kmd_fault_get_latest(&fc, &latest) == A840_KMD_OK);
    EXPECT(out.sequence == 1u && latest.sequence == 1u);
    EXPECT(latest.client_id == 42u);
    EXPECT(latest.queue_id == 3u);
    EXPECT(latest.fence == 12345u);
    EXPECT(latest.ib1_pc == in.ib1_pc);
    EXPECT(latest.ib2_pc == in.ib2_pc);
    EXPECT(latest.rbbm_status == in.rbbm_status);
    EXPECT(latest.cp_status == in.cp_status);
    EXPECT(latest.smmu_fault_va == in.smmu_fault_va);
    EXPECT(latest.smmu_fsr == in.smmu_fsr);
    EXPECT(latest.fault_kind == A840_KMD_FAULT_SMMU_TRANSLATION);
    EXPECT(latest.timestamp_ticks == in.timestamp_ticks);
    a840_kmd_fault_destroy(&fc);
    return true;
}

static bool test_fc_single_channel_isolation() {
    A840KmdFaultCapture fc{};
    EXPECT(a840_kmd_fault_init(&fc) == A840_KMD_OK);

    A840KmdFaultInput in{};
    in.client_id = 10u;
    in.queue_id = 0u;
    in.fault_kind = A840_KMD_FAULT_GPU_HANG;
    EXPECT(a840_kmd_fault_record(&fc, &in, nullptr) == A840_KMD_OK);

    EXPECT(a840_kmd_fault_is_channel_poisoned(&fc, 10u, 0u) == 1);
    EXPECT(a840_kmd_fault_check_channel(&fc, 10u, 0u) == A840_KMD_POISONED);
    EXPECT(a840_kmd_fault_is_channel_poisoned(&fc, 10u, 1u) == 0);
    EXPECT(a840_kmd_fault_check_channel(&fc, 10u, 1u) == A840_KMD_OK);
    EXPECT(a840_kmd_fault_is_channel_poisoned(&fc, 20u, 0u) == 0);
    EXPECT(a840_kmd_fault_check_channel(&fc, 20u, 0u) == A840_KMD_OK);
    a840_kmd_fault_destroy(&fc);
    return true;
}

static bool test_fc_circular_ring_wraparound_over_64_faults() {
    A840KmdFaultCapture fc{};
    EXPECT(a840_kmd_fault_init(&fc) == A840_KMD_OK);

    for (uint64_t i = 1; i <= 100u; ++i) {
        A840KmdFaultInput in{};
        in.client_id = 1u;
        in.queue_id = i % 4u;
        in.fence = i * 10u;
        in.fault_kind = A840_KMD_FAULT_IB_ERROR;
        in.timestamp_ticks = i * 1000u;
        EXPECT(a840_kmd_fault_record(&fc, &in, nullptr) == A840_KMD_OK);
    }

    EXPECT(a840_kmd_fault_total_count(&fc) == 100u);
    EXPECT(a840_kmd_fault_ring_count(&fc) == A840_KMD_FAULT_RING_CAPACITY);

    A840KmdFaultSnapshot oldest{}, newest{};
    EXPECT(a840_kmd_fault_get_snapshot(&fc, 0u, &oldest) == A840_KMD_OK);
    EXPECT(oldest.sequence == 37u);
    EXPECT(oldest.fence == 370u);

    EXPECT(a840_kmd_fault_get_snapshot(&fc, A840_KMD_FAULT_RING_CAPACITY - 1u, &newest) == A840_KMD_OK);
    EXPECT(newest.sequence == 100u);
    EXPECT(newest.fence == 1000u);

    EXPECT(a840_kmd_fault_get_snapshot(&fc, A840_KMD_FAULT_RING_CAPACITY, &newest) == A840_KMD_RANGE);
    a840_kmd_fault_destroy(&fc);
    return true;
}

static bool test_fc_get_channel_latest_filters_accurately() {
    A840KmdFaultCapture fc{};
    EXPECT(a840_kmd_fault_init(&fc) == A840_KMD_OK);

    A840KmdFaultInput in{};
    in.fault_kind = A840_KMD_FAULT_GPU_HANG;
    in.client_id = 1u; in.queue_id = 0u; in.fence = 11u;
    EXPECT(a840_kmd_fault_record(&fc, &in, nullptr) == A840_KMD_OK);
    in.client_id = 2u; in.queue_id = 5u; in.fence = 22u;
    EXPECT(a840_kmd_fault_record(&fc, &in, nullptr) == A840_KMD_OK);
    in.client_id = 1u; in.queue_id = 0u; in.fence = 33u;
    EXPECT(a840_kmd_fault_record(&fc, &in, nullptr) == A840_KMD_OK);

    A840KmdFaultSnapshot snap{};
    EXPECT(a840_kmd_fault_get_channel_latest(&fc, 1u, 0u, &snap) == A840_KMD_OK);
    EXPECT(snap.fence == 33u && snap.sequence == 3u);
    EXPECT(a840_kmd_fault_get_channel_latest(&fc, 2u, 5u, &snap) == A840_KMD_OK);
    EXPECT(snap.fence == 22u && snap.sequence == 2u);
    EXPECT(a840_kmd_fault_get_channel_latest(&fc, 3u, 0u, &snap) == A840_KMD_NOT_FOUND);
    a840_kmd_fault_destroy(&fc);
    return true;
}

static bool test_fc_poison_callback_invoked_outside_lock() {
    A840KmdFaultCapture fc{};
    EXPECT(a840_kmd_fault_init(&fc) == A840_KMD_OK);

    struct CbCtx {
        A840KmdFaultCapture *fc;
        int called = 0;
        int saw_poisoned = 0;
        uint64_t saw_seq = 0;
    } ctx{&fc};

    auto cb = [](void *cookie, uint64_t cid, uint64_t qid,
                 const A840KmdFaultSnapshot *snap) {
        auto *c = static_cast<CbCtx *>(cookie);
        c->called++;
        c->saw_poisoned = a840_kmd_fault_is_channel_poisoned(c->fc, cid, qid);
        A840KmdFaultSnapshot latest{};
        if (a840_kmd_fault_get_latest(c->fc, &latest) == A840_KMD_OK)
            c->saw_seq = latest.sequence;
        (void)snap;
    };

    EXPECT(a840_kmd_fault_set_poison_callback(&fc, cb, &ctx) == A840_KMD_OK);
    A840KmdFaultInput in{};
    in.client_id = 77u;
    in.queue_id = 2u;
    in.fault_kind = A840_KMD_FAULT_SMMU_PERMISSION;
    EXPECT(a840_kmd_fault_record(&fc, &in, nullptr) == A840_KMD_OK);
    EXPECT(ctx.called == 1);
    EXPECT(ctx.saw_poisoned == 1);
    EXPECT(ctx.saw_seq == 1u);
    a840_kmd_fault_destroy(&fc);
    return true;
}

static bool test_fc_clear_client_removes_poison_keeps_ring_history() {
    A840KmdFaultCapture fc{};
    EXPECT(a840_kmd_fault_init(&fc) == A840_KMD_OK);

    A840KmdFaultInput in{};
    in.fault_kind = A840_KMD_FAULT_GPU_HANG;
    in.client_id = 1u; in.queue_id = 0u;
    EXPECT(a840_kmd_fault_record(&fc, &in, nullptr) == A840_KMD_OK);
    in.client_id = 1u; in.queue_id = 1u;
    EXPECT(a840_kmd_fault_record(&fc, &in, nullptr) == A840_KMD_OK);
    in.client_id = 2u; in.queue_id = 0u;
    EXPECT(a840_kmd_fault_record(&fc, &in, nullptr) == A840_KMD_OK);

    EXPECT(a840_kmd_fault_poisoned_channel_count(&fc) == 3u);
    EXPECT(a840_kmd_fault_clear_client(&fc, 1u) == 2u);
    EXPECT(a840_kmd_fault_poisoned_channel_count(&fc) == 1u);
    EXPECT(a840_kmd_fault_is_channel_poisoned(&fc, 1u, 0u) == 0);
    EXPECT(a840_kmd_fault_is_channel_poisoned(&fc, 2u, 0u) == 1);
    EXPECT(a840_kmd_fault_ring_count(&fc) == 3u);
    a840_kmd_fault_destroy(&fc);
    return true;
}

static bool test_fc_repeated_fault_same_channel_no_duplicate_slot() {
    A840KmdFaultCapture fc{};
    EXPECT(a840_kmd_fault_init(&fc) == A840_KMD_OK);
    for (uint64_t i = 0; i < 10u; ++i) {
        A840KmdFaultInput in{};
        in.client_id = 5u;
        in.queue_id = 9u;
        in.fence = i + 1u;
        in.fault_kind = A840_KMD_FAULT_IB_ERROR;
        EXPECT(a840_kmd_fault_record(&fc, &in, nullptr) == A840_KMD_OK);
    }
    EXPECT(a840_kmd_fault_poisoned_channel_count(&fc) == 1u);
    EXPECT(a840_kmd_fault_ring_count(&fc) == 10u);
    a840_kmd_fault_destroy(&fc);
    return true;
}

static bool test_fc_empty_ring_queries() {
    A840KmdFaultCapture fc{};
    EXPECT(a840_kmd_fault_init(&fc) == A840_KMD_OK);
    A840KmdFaultSnapshot snap{};
    EXPECT(a840_kmd_fault_ring_count(&fc) == 0u);
    EXPECT(a840_kmd_fault_total_count(&fc) == 0u);
    EXPECT(a840_kmd_fault_get_latest(&fc, &snap) == A840_KMD_NOT_FOUND);
    EXPECT(a840_kmd_fault_get_snapshot(&fc, 0u, &snap) == A840_KMD_RANGE);
    a840_kmd_fault_destroy(&fc);
    return true;
}

static bool test_fc_destroy_detaches_state() {
    A840KmdFaultCapture fc{};
    EXPECT(a840_kmd_fault_init(&fc) == A840_KMD_OK);
    a840_kmd_fault_destroy(&fc);
    A840KmdFaultInput in{};
    in.client_id = 1u;
    in.fault_kind = A840_KMD_FAULT_GPU_HANG;
    EXPECT(a840_kmd_fault_record(&fc, &in, nullptr) == A840_KMD_STATE_CONFLICT);
    return true;
}

/* =========================================================================
 * Group C: Interrupt-Driven WaitAny / WaitAll Engine (14 cases)
 * ========================================================================= */

static bool test_wa_validation_invalid_args() {
    A840KmdWaitAnyEngine eng{};
    EXPECT(a840_kmd_wait_any_init(&eng, nullptr) == A840_KMD_OK);

    NativeAdrenoAbiWaitAnyV3 req{};
    NativeAdrenoAbiWaitAnyReplyV3 rep{};
    rep.satisfied_mask = 0xDEADBEEFu;

    EXPECT(a840_kmd_wait_any_wait(&eng, nullptr, &rep) == A840_KMD_INVALID_ARG);
    EXPECT(a840_kmd_wait_any_wait(&eng, &req, nullptr) == A840_KMD_INVALID_ARG);
    req.count = 0u;
    EXPECT(a840_kmd_wait_any_wait(&eng, &req, &rep) == A840_KMD_INVALID_ARG);
    req.count = 33u;
    EXPECT(a840_kmd_wait_any_wait(&eng, &req, &rep) == A840_KMD_INVALID_ARG);
    req.count = 1u;
    req.flags = 2u;
    EXPECT(a840_kmd_wait_any_wait(&eng, &req, &rep) == A840_KMD_INVALID_ARG);
    EXPECT(rep.satisfied_mask == 0xDEADBEEFu);

    a840_kmd_wait_any_destroy(&eng);
    return true;
}

static bool test_wa_fast_path_zero_sleep_already_satisfied() {
    A840KmdWaitAnyEngine eng{};
    EXPECT(a840_kmd_wait_any_init(&eng, nullptr) == A840_KMD_OK);
    EXPECT(a840_kmd_wait_any_signal_fence(&eng, 1u, 10u, 777u) == A840_KMD_OK);

    NativeAdrenoAbiWaitAnyV3 req{};
    req.count = 1u;
    req.flags = 0u;
    req.timeout_ns = UINT64_C(1000000000);
    req.queues[0] = 1u;
    req.fences[0] = 10u;

    NativeAdrenoAbiWaitAnyReplyV3 rep{};
    EXPECT(a840_kmd_wait_any_wait(&eng, &req, &rep) == A840_KMD_OK);
    EXPECT(rep.satisfied_mask == 1u);
    EXPECT(rep.timed_out == 0u);
    EXPECT(rep.observed_ticks == 777u);

    A840KmdWaitAnyStats st{};
    EXPECT(a840_kmd_wait_any_get_stats(&eng, &st) == A840_KMD_OK);
    EXPECT(st.fast_path_hits == 1u);
    EXPECT(st.sleep_entries == 0u);
    a840_kmd_wait_any_destroy(&eng);
    return true;
}

static bool test_wa_fast_path_fence_zero_default_timeline() {
    A840KmdWaitAnyEngine eng{};
    EXPECT(a840_kmd_wait_any_init(&eng, nullptr) == A840_KMD_OK);

    NativeAdrenoAbiWaitAnyV3 req{};
    req.count = 1u;
    req.queues[0] = 0u;
    req.fences[0] = 0u;
    req.timeout_ns = 1000000u;

    NativeAdrenoAbiWaitAnyReplyV3 rep{};
    EXPECT(a840_kmd_wait_any_wait(&eng, &req, &rep) == A840_KMD_OK);
    EXPECT(rep.satisfied_mask == 1u);
    EXPECT(rep.timed_out == 0u);

    A840KmdWaitAnyStats st{};
    EXPECT(a840_kmd_wait_any_get_stats(&eng, &st) == A840_KMD_OK);
    EXPECT(st.sleep_entries == 0u);
    a840_kmd_wait_any_destroy(&eng);
    return true;
}

static bool test_wa_zero_timeout_poll_unsatisfied() {
    A840KmdWaitAnyEngine eng{};
    EXPECT(a840_kmd_wait_any_init(&eng, nullptr) == A840_KMD_OK);

    NativeAdrenoAbiWaitAnyV3 req{};
    req.count = 2u;
    req.queues[0] = 1u; req.fences[0] = 5u;
    req.queues[1] = 2u; req.fences[1] = 6u;
    req.timeout_ns = 0u;

    NativeAdrenoAbiWaitAnyReplyV3 rep{};
    EXPECT(a840_kmd_wait_any_wait(&eng, &req, &rep) == A840_KMD_OK);
    EXPECT(rep.satisfied_mask == 0u);
    EXPECT(rep.timed_out == 1u);

    A840KmdWaitAnyStats st{};
    EXPECT(a840_kmd_wait_any_get_stats(&eng, &st) == A840_KMD_OK);
    EXPECT(st.sleep_entries == 0u);
    EXPECT(st.fast_path_hits == 1u);
    a840_kmd_wait_any_destroy(&eng);
    return true;
}

static bool test_wa_monotonic_fence_no_regression() {
    A840KmdWaitAnyEngine eng{};
    EXPECT(a840_kmd_wait_any_init(&eng, nullptr) == A840_KMD_OK);

    EXPECT(a840_kmd_wait_any_signal_fence(&eng, 3u, 100u, 5000u) == A840_KMD_OK);
    EXPECT(a840_kmd_wait_any_signal_fence(&eng, 3u, 50u, 4000u) == A840_KMD_OK);

    uint64_t done = 0;
    int poisoned = -1;
    EXPECT(a840_kmd_wait_any_query_queue(&eng, 3u, &done, &poisoned) == A840_KMD_OK);
    EXPECT(done == 100u);
    EXPECT(poisoned == 0);

    A840KmdWaitAnyStats st{};
    EXPECT(a840_kmd_wait_any_get_stats(&eng, &st) == A840_KMD_OK);
    EXPECT(st.latest_ticks == 5000u);
    a840_kmd_wait_any_destroy(&eng);
    return true;
}

static bool wait_for_active_waiters(A840KmdWaitAnyEngine *eng, uint32_t expected) {
    for (int i = 0; i < 400; ++i) {
        A840KmdWaitAnyStats st{};
        if (a840_kmd_wait_any_get_stats(eng, &st) == A840_KMD_OK &&
            st.active_waiters >= expected)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

static bool test_wa_mt_irq_wait_any_32_fences_bit31() {
    A840KmdWaitAnyEngine eng{};
    EXPECT(a840_kmd_wait_any_init(&eng, nullptr) == A840_KMD_OK);

    NativeAdrenoAbiWaitAnyV3 req{};
    req.count = 32u;
    req.flags = 0u; /* WAIT_ANY */
    req.timeout_ns = UINT64_C(2000000000);
    for (uint32_t i = 0; i < 32u; ++i) {
        req.queues[i] = 100u + i;
        req.fences[i] = 50u;
    }

    NativeAdrenoAbiWaitAnyReplyV3 rep{};
    A840KmdStatus wait_st = A840_KMD_IO_ERROR;
    std::thread waiter([&]() {
        wait_st = a840_kmd_wait_any_wait(&eng, &req, &rep);
    });

    EXPECT(wait_for_active_waiters(&eng, 1u));
    EXPECT(a840_kmd_wait_any_signal_fence(&eng, 131u, 50u, UINT64_C(99999)) == A840_KMD_OK);
    waiter.join();

    EXPECT(wait_st == A840_KMD_OK);
    EXPECT(rep.satisfied_mask == (UINT32_C(1) << 31));
    EXPECT(rep.timed_out == 0u);
    EXPECT(rep.observed_ticks == UINT64_C(99999));

    A840KmdWaitAnyStats st{};
    EXPECT(a840_kmd_wait_any_get_stats(&eng, &st) == A840_KMD_OK);
    EXPECT(st.sleep_entries >= 1u);
    EXPECT(st.active_waiters == 0u);
    a840_kmd_wait_any_destroy(&eng);
    return true;
}

static bool test_wa_mt_irq_wait_all_32_fences_full_mask() {
    A840KmdWaitAnyEngine eng{};
    EXPECT(a840_kmd_wait_any_init(&eng, nullptr) == A840_KMD_OK);

    NativeAdrenoAbiWaitAnyV3 req{};
    req.count = 32u;
    req.flags = 1u; /* WAIT_ALL */
    req.timeout_ns = UINT64_C(2000000000);
    for (uint32_t i = 0; i < 32u; ++i) {
        req.queues[i] = 200u + i;
        req.fences[i] = 10u + i;
    }

    std::atomic<bool> finished{false};
    NativeAdrenoAbiWaitAnyReplyV3 rep{};
    A840KmdStatus wait_st = A840_KMD_IO_ERROR;
    std::thread waiter([&]() {
        wait_st = a840_kmd_wait_any_wait(&eng, &req, &rep);
        finished.store(true, std::memory_order_release);
    });

    EXPECT(wait_for_active_waiters(&eng, 1u));
    for (uint32_t i = 0; i < 31u; ++i)
        EXPECT(a840_kmd_wait_any_signal_fence(&eng, 200u + i, 10u + i, 1000u + i) == A840_KMD_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT(!finished.load(std::memory_order_acquire));

    EXPECT(a840_kmd_wait_any_signal_fence(&eng, 231u, 41u, UINT64_C(88888)) == A840_KMD_OK);
    waiter.join();

    EXPECT(wait_st == A840_KMD_OK);
    EXPECT(rep.satisfied_mask == UINT32_MAX);
    EXPECT(rep.timed_out == 0u);
    EXPECT(rep.observed_ticks == UINT64_C(88888));
    a840_kmd_wait_any_destroy(&eng);
    return true;
}

static bool test_wa_mt_duplicate_queues_in_wait_all() {
    A840KmdWaitAnyEngine eng{};
    EXPECT(a840_kmd_wait_any_init(&eng, nullptr) == A840_KMD_OK);

    NativeAdrenoAbiWaitAnyV3 req{};
    req.count = 2u;
    req.flags = 1u;
    req.timeout_ns = UINT64_C(2000000000);
    req.queues[0] = 7u; req.fences[0] = 5u;
    req.queues[1] = 7u; req.fences[1] = 10u;

    std::atomic<bool> done{false};
    NativeAdrenoAbiWaitAnyReplyV3 rep{};
    A840KmdStatus wait_st = A840_KMD_IO_ERROR;
    std::thread waiter([&]() {
        wait_st = a840_kmd_wait_any_wait(&eng, &req, &rep);
        done.store(true, std::memory_order_release);
    });

    EXPECT(wait_for_active_waiters(&eng, 1u));
    EXPECT(a840_kmd_wait_any_signal_fence(&eng, 7u, 5u, 100u) == A840_KMD_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT(!done.load(std::memory_order_acquire));
    EXPECT(a840_kmd_wait_any_signal_fence(&eng, 7u, 10u, 200u) == A840_KMD_OK);
    waiter.join();

    EXPECT(wait_st == A840_KMD_OK);
    EXPECT(rep.satisfied_mask == 3u);
    EXPECT(rep.timed_out == 0u);
    EXPECT(rep.observed_ticks == 200u);
    a840_kmd_wait_any_destroy(&eng);
    return true;
}

static bool test_wa_mt_timeout_accurate_zero_busy_wait() {
    A840KmdWaitAnyEngine eng{};
    EXPECT(a840_kmd_wait_any_init(&eng, nullptr) == A840_KMD_OK);

    NativeAdrenoAbiWaitAnyV3 req{};
    req.count = 1u;
    req.queues[0] = 9u;
    req.fences[0] = 1u;
    req.timeout_ns = UINT64_C(30000000); /* 30 ms */

    NativeAdrenoAbiWaitAnyReplyV3 rep{};
    auto t0 = std::chrono::steady_clock::now();
    EXPECT(a840_kmd_wait_any_wait(&eng, &req, &rep) == A840_KMD_OK);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT(rep.satisfied_mask == 0u);
    EXPECT(rep.timed_out == 1u);
    EXPECT(elapsed_ms >= 20);

    A840KmdWaitAnyStats st{};
    EXPECT(a840_kmd_wait_any_get_stats(&eng, &st) == A840_KMD_OK);
    EXPECT(st.sleep_entries >= 1u && st.sleep_entries <= 3u);
    a840_kmd_wait_any_destroy(&eng);
    return true;
}

static bool test_wa_mt_targeted_wakeup_ignores_unrelated_queue() {
    A840KmdWaitAnyEngine eng{};
    EXPECT(a840_kmd_wait_any_init(&eng, nullptr) == A840_KMD_OK);

    NativeAdrenoAbiWaitAnyV3 req{};
    req.count = 1u;
    req.queues[0] = 10u;
    req.fences[0] = 1u;
    req.timeout_ns = UINT64_C(2000000000);

    NativeAdrenoAbiWaitAnyReplyV3 rep{};
    A840KmdStatus wait_st = A840_KMD_IO_ERROR;
    std::thread waiter([&]() {
        wait_st = a840_kmd_wait_any_wait(&eng, &req, &rep);
    });

    EXPECT(wait_for_active_waiters(&eng, 1u));
    for (uint64_t f = 1; f <= 20u; ++f)
        EXPECT(a840_kmd_wait_any_signal_fence(&eng, 99u, f, f * 10u) == A840_KMD_OK);

    A840KmdWaitAnyStats mid{};
    EXPECT(a840_kmd_wait_any_get_stats(&eng, &mid) == A840_KMD_OK);
    EXPECT(mid.targeted_wakes == 0u);

    EXPECT(a840_kmd_wait_any_signal_fence(&eng, 10u, 1u, 555u) == A840_KMD_OK);
    waiter.join();

    EXPECT(wait_st == A840_KMD_OK);
    EXPECT(rep.satisfied_mask == 1u);
    EXPECT(rep.observed_ticks == 555u);
    A840KmdWaitAnyStats end{};
    EXPECT(a840_kmd_wait_any_get_stats(&eng, &end) == A840_KMD_OK);
    EXPECT(end.targeted_wakes == 1u);
    a840_kmd_wait_any_destroy(&eng);
    return true;
}

static bool test_wa_mt_poison_queue_wakes_blocked_waiter_immediately() {
    A840KmdWaitAnyEngine eng{};
    EXPECT(a840_kmd_wait_any_init(&eng, nullptr) == A840_KMD_OK);

    NativeAdrenoAbiWaitAnyV3 req{};
    req.count = 1u;
    req.queues[0] = 5u;
    req.fences[0] = 999u;
    req.timeout_ns = A840_KMD_WAIT_INFINITE;

    NativeAdrenoAbiWaitAnyReplyV3 rep{};
    A840KmdStatus wait_st = A840_KMD_OK;
    std::thread waiter([&]() {
        wait_st = a840_kmd_wait_any_wait(&eng, &req, &rep);
    });

    EXPECT(wait_for_active_waiters(&eng, 1u));
    EXPECT(a840_kmd_wait_any_poison_queue(&eng, 5u) == A840_KMD_OK);
    waiter.join();

    EXPECT(wait_st == A840_KMD_POISONED);
    EXPECT(a840_kmd_wait_any_signal_fence(&eng, 5u, 1000u, 100u) == A840_KMD_POISONED);
    EXPECT(a840_kmd_wait_any_wait(&eng, &req, &rep) == A840_KMD_POISONED);
    a840_kmd_wait_any_destroy(&eng);
    return true;
}

static bool test_wa_mt_disconnect_queue_wakes_blocked_waiter_immediately() {
    A840KmdWaitAnyEngine eng{};
    EXPECT(a840_kmd_wait_any_init(&eng, nullptr) == A840_KMD_OK);

    NativeAdrenoAbiWaitAnyV3 req{};
    req.count = 1u;
    req.queues[0] = 6u;
    req.fences[0] = 1u;
    req.timeout_ns = A840_KMD_WAIT_INFINITE;

    NativeAdrenoAbiWaitAnyReplyV3 rep{};
    A840KmdStatus wait_st = A840_KMD_OK;
    std::thread waiter([&]() {
        wait_st = a840_kmd_wait_any_wait(&eng, &req, &rep);
    });

    EXPECT(wait_for_active_waiters(&eng, 1u));
    EXPECT(a840_kmd_wait_any_disconnect_queue(&eng, 6u) == A840_KMD_OK);
    waiter.join();

    EXPECT(wait_st == A840_KMD_DISCONNECTED);
    a840_kmd_wait_any_destroy(&eng);
    return true;
}

static bool test_wa_mt_multiple_waiters_same_queue_staged_wakes() {
    A840KmdWaitAnyEngine eng{};
    EXPECT(a840_kmd_wait_any_init(&eng, nullptr) == A840_KMD_OK);

    const uint64_t targets[4] = {10u, 20u, 20u, 30u};
    std::atomic<int> done_flags[4]{};
    A840KmdStatus sts[4]{};
    NativeAdrenoAbiWaitAnyReplyV3 reps[4]{};
    std::vector<std::thread> threads;

    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&, i]() {
            NativeAdrenoAbiWaitAnyV3 req{};
            req.count = 1u;
            req.queues[0] = 7u;
            req.fences[0] = targets[i];
            req.timeout_ns = UINT64_C(2000000000);
            sts[i] = a840_kmd_wait_any_wait(&eng, &req, &reps[i]);
            done_flags[i].store(1, std::memory_order_release);
        });
    }

    EXPECT(wait_for_active_waiters(&eng, 4u));
    EXPECT(a840_kmd_wait_any_signal_fence(&eng, 7u, 10u, 100u) == A840_KMD_OK);
    for (int k = 0; k < 200 && !done_flags[0].load(std::memory_order_acquire); ++k)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT(done_flags[0].load(std::memory_order_acquire) == 1);
    EXPECT(done_flags[1].load(std::memory_order_acquire) == 0);

    EXPECT(a840_kmd_wait_any_signal_fence(&eng, 7u, 20u, 200u) == A840_KMD_OK);
    for (int k = 0; k < 200 && (!done_flags[1].load(std::memory_order_acquire) ||
                                !done_flags[2].load(std::memory_order_acquire)); ++k)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT(done_flags[1].load(std::memory_order_acquire) == 1);
    EXPECT(done_flags[2].load(std::memory_order_acquire) == 1);
    EXPECT(done_flags[3].load(std::memory_order_acquire) == 0);

    EXPECT(a840_kmd_wait_any_signal_fence(&eng, 7u, 30u, 300u) == A840_KMD_OK);
    for (auto &t : threads)
        t.join();

    for (int i = 0; i < 4; ++i) {
        EXPECT(sts[i] == A840_KMD_OK);
        EXPECT(reps[i].satisfied_mask == 1u);
        EXPECT(reps[i].timed_out == 0u);
    }
    a840_kmd_wait_any_destroy(&eng);
    return true;
}

static bool test_wa_custom_xnu_style_backend_abstraction() {
    struct FakeXnuSync {
        pthread_mutex_t mu;
        pthread_cond_t cv;
        std::atomic<uint64_t> fake_now{1000u};
        std::atomic<uint64_t> hw_ticks{4242u};
        std::atomic<uint32_t> wait_calls{0};
        std::atomic<uint32_t> wake_calls{0};
        bool force_timeout = false;

        FakeXnuSync() {
            pthread_mutex_init(&mu, nullptr);
            pthread_cond_init(&cv, nullptr);
        }
        ~FakeXnuSync() {
            pthread_cond_destroy(&cv);
            pthread_mutex_destroy(&mu);
        }
    } sync;

    A840KmdWaitBackend backend{};
    backend.cookie = &sync;
    backend.lock = [](void *c) { pthread_mutex_lock(&static_cast<FakeXnuSync *>(c)->mu); };
    backend.unlock = [](void *c) { pthread_mutex_unlock(&static_cast<FakeXnuSync *>(c)->mu); };
    backend.wait = [](void *c, uint64_t deadline, int infinite) -> int {
        auto *s = static_cast<FakeXnuSync *>(c);
        s->wait_calls.fetch_add(1u, std::memory_order_relaxed);
        if (s->force_timeout && !infinite) {
            s->fake_now.store(deadline + 1u, std::memory_order_relaxed);
            return 1;
        }
        pthread_cond_wait(&s->cv, &s->mu);
        return 0;
    };
    backend.wake_all = [](void *c) {
        auto *s = static_cast<FakeXnuSync *>(c);
        s->wake_calls.fetch_add(1u, std::memory_order_relaxed);
        pthread_cond_broadcast(&s->cv);
    };
    backend.now_ns = [](void *c) -> uint64_t {
        return static_cast<FakeXnuSync *>(c)->fake_now.load(std::memory_order_relaxed);
    };
    backend.read_ticks = [](void *c) -> uint64_t {
        return static_cast<FakeXnuSync *>(c)->hw_ticks.load(std::memory_order_relaxed);
    };

    A840KmdWaitAnyEngine eng{};
    EXPECT(a840_kmd_wait_any_init(&eng, &backend) == A840_KMD_OK);

    NativeAdrenoAbiWaitAnyV3 req{};
    req.count = 1u;
    req.queues[0] = 1u;
    req.fences[0] = 5u;
    req.timeout_ns = A840_KMD_WAIT_INFINITE;

    NativeAdrenoAbiWaitAnyReplyV3 rep{};
    A840KmdStatus wst = A840_KMD_IO_ERROR;
    std::thread w([&]() { wst = a840_kmd_wait_any_wait(&eng, &req, &rep); });
    EXPECT(wait_for_active_waiters(&eng, 1u));
    EXPECT(a840_kmd_wait_any_signal_fence(&eng, 1u, 5u, 100u) == A840_KMD_OK);
    w.join();
    EXPECT(wst == A840_KMD_OK);
    EXPECT(rep.satisfied_mask == 1u);
    EXPECT(rep.observed_ticks == 4242u);
    EXPECT(sync.wait_calls.load() >= 1u);
    EXPECT(sync.wake_calls.load() >= 1u);

    sync.force_timeout = true;
    req.fences[0] = 99u;
    req.timeout_ns = 500u;
    EXPECT(a840_kmd_wait_any_wait(&eng, &req, &rep) == A840_KMD_OK);
    EXPECT(rep.satisfied_mask == 0u);
    EXPECT(rep.timed_out == 1u);

    a840_kmd_wait_any_destroy(&eng);
    return true;
}

/* =========================================================================
 * Group D: UserClient v3 3104B Envelope Dispatcher & E2E (19 cases)
 * ========================================================================= */

static A840KmdUserClientConfig make_default_uc_config(
    MockSmmu *smmu, MockBoTable *bos, MockPromiseObserver *obs) {
    A840KmdUserClientConfig cfg{};
    cfg.cap_flags = 0u; /* selects A840_KMD_V3_DEFAULT_CAPS */
    cfg.zero_page_default_va = kDefaultZeroVa;
    cfg.smmu_ops = smmu->ops();
    cfg.bo_size_lookup = bos ? &MockBoTable::lookup_cb : nullptr;
    cfg.bo_size_cookie = bos;
    cfg.promise_observer = obs ? &MockPromiseObserver::on_event : nullptr;
    cfg.promise_observer_cookie = obs;
    return cfg;
}

static bool test_uc_init_and_advertised_caps_excludes_phase4_5() {
    MockSmmu smmu;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, nullptr, nullptr);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);

    uint32_t caps = a840_kmd_userclient_v3_advertised_caps(&uc);
    EXPECT((caps & NativeAdrenoParamsV3TimelineWaitAny) != 0u);
    EXPECT((caps & NativeAdrenoParamsV3SharedZeroPage) != 0u);
    EXPECT((caps & NativeAdrenoParamsV3SharedTier0Pack) != 0u);
    EXPECT((caps & NativeAdrenoParamsV3WriterPromise) != 0u);
    EXPECT((caps & NativeAdrenoParamsV3UbwcSurfaceMeta) != 0u);
    EXPECT((caps & NativeAdrenoParamsV3SparseVmBind) == 0u);
    EXPECT((caps & NativeAdrenoParamsV3PreemptRings) == 0u);

    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

static bool test_uc_v2_methods_1_to_19_all_return_pass_to_v2() {
    MockSmmu smmu;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, nullptr, nullptr);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 1u, nullptr, 0u) == A840_KMD_OK);

    uint8_t in_buf[NativeAdrenoAbiEnvelopeBytes]{};
    uint8_t out_buf[NativeAdrenoAbiEnvelopeBytes]{};
    for (uint32_t m = 1u; m <= 19u; ++m) {
        size_t written = 99u;
        size_t in_sz = (m == 5u || m == 19u) ? static_cast<size_t>(NativeAdrenoAbiEnvelopeBytes) : static_cast<size_t>(16u);
        EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, m, in_buf, in_sz,
            out_buf, 16u, &written) == A840_KMD_PASS_TO_V2);
        EXPECT(written == 0u);
    }

    EXPECT(a840_kmd_userclient_v3_close_client(&uc, 1u, nullptr) == A840_KMD_OK);
    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

static bool test_uc_methods_22_26_0_and_99_return_unsupported() {
    MockSmmu smmu;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, nullptr, nullptr);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 1u, nullptr, 0u) == A840_KMD_OK);

    uint8_t buf[64]{};
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodMapSparseV3,
        buf, 16u, buf, 16u, nullptr) == A840_KMD_UNSUPPORTED);
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodQueueConfigV3,
        buf, 16u, buf, 16u, nullptr) == A840_KMD_UNSUPPORTED);
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, 0u,
        buf, 16u, buf, 16u, nullptr) == A840_KMD_UNSUPPORTED);
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, 27u,
        buf, 16u, buf, 16u, nullptr) == A840_KMD_UNSUPPORTED);
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, 99u,
        buf, 16u, buf, 16u, nullptr) == A840_KMD_UNSUPPORTED);

    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

static bool test_uc_envelope_over_3104_bytes_rejected() {
    MockSmmu smmu;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, nullptr, nullptr);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 1u, nullptr, 0u) == A840_KMD_OK);

    std::vector<uint8_t> big(NativeAdrenoAbiEnvelopeBytes + 16u, 0u);
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, 5u,
        big.data(), NativeAdrenoAbiEnvelopeBytes + 1u, big.data(), 16u, nullptr) == A840_KMD_RANGE);
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodWaitAnyV3,
        big.data(), NativeAdrenoAbiEnvelopeBytes + 1u, big.data(),
        sizeof(NativeAdrenoAbiWaitAnyReplyV3), nullptr) == A840_KMD_RANGE);
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodGetZeroPageV3,
        nullptr, 0u, big.data(), NativeAdrenoAbiEnvelopeBytes + 1u, nullptr) == A840_KMD_RANGE);

    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodWaitAnyV3,
        big.data(), sizeof(NativeAdrenoAbiWaitAnyV3) - 1u,
        big.data(), sizeof(NativeAdrenoAbiWaitAnyReplyV3), nullptr) == A840_KMD_INVALID_ARG);
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodGetZeroPageV3,
        big.data(), 4u, big.data(), sizeof(NativeAdrenoAbiZeroPageReplyV3), nullptr) == A840_KMD_INVALID_ARG);

    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

static bool test_uc_method21_get_zero_page_multi_client_idempotent() {
    MockSmmu smmu;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, nullptr, nullptr);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 1u, nullptr, 0u) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 2u, nullptr, kAltZeroVa) == A840_KMD_OK);

    NativeAdrenoAbiZeroPageReplyV3 r1a{}, r1b{}, r2{};
    size_t w = 0;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodGetZeroPageV3,
        nullptr, 0u, &r1a, sizeof(r1a), &w) == A840_KMD_OK);
    EXPECT(w == sizeof(r1a) && r1a.gpu_va == kDefaultZeroVa && r1a.bytes == A840_KMD_ZERO_PAGE_BYTES);

    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodGetZeroPageV3,
        nullptr, 0u, &r1b, sizeof(r1b), &w) == A840_KMD_OK);
    EXPECT(r1b.gpu_va == r1a.gpu_va && r1b.bytes == r1a.bytes);

    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 2u, NativeAdrenoMethodGetZeroPageV3,
        nullptr, 0u, &r2, sizeof(r2), &w) == A840_KMD_OK);
    EXPECT(r2.gpu_va == kAltZeroVa && r2.bytes == A840_KMD_ZERO_PAGE_BYTES);

    EXPECT(smmu.alloc_calls == 1u);
    EXPECT(smmu.map_calls == 2u);
    a840_kmd_userclient_v3_destroy(&uc);
    EXPECT(smmu.live_allocs == 0u);
    return true;
}

static bool test_uc_method23_map_shared_pack_match_mismatch_cpu_flag() {
    MockSmmu smmu;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, nullptr, nullptr);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 1u, nullptr, 0u) == A840_KMD_OK);

    NativeAdrenoAbiSharedPackReplyV3 pack{};
    pack.gpu_va = kPackGpuVa;
    pack.cpu_va = kPackCpuVa;
    pack.bytes = 131072u;
    pack.actual_stack_key = kStackKey;

    EXPECT(a840_kmd_userclient_v3_configure_shared_pack(&uc, &pack, 0u, 1u) == A840_KMD_INVALID_ARG);
    EXPECT(a840_kmd_userclient_v3_configure_shared_pack(&uc, &pack, 1u, 0u) == A840_KMD_INVALID_ARG);
    EXPECT(a840_kmd_userclient_v3_configure_shared_pack(&uc, &pack, 1u, 1u) == A840_KMD_OK);

    NativeAdrenoAbiSharedPackReqV3 req{};
    NativeAdrenoAbiSharedPackReplyV3 rep{};
    size_t w = 0;
    req.expected_stack_key = kStackKey;
    req.map_cpu = 0u;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodMapSharedPackV3,
        &req, sizeof(req), &rep, sizeof(rep), &w) == A840_KMD_OK);
    EXPECT(w == sizeof(rep) && rep.gpu_va == kPackGpuVa && rep.cpu_va == 0u &&
           rep.bytes == 131072u && rep.actual_stack_key == kStackKey);

    req.map_cpu = 1u;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodMapSharedPackV3,
        &req, sizeof(req), &rep, sizeof(rep), &w) == A840_KMD_OK);
    EXPECT(rep.cpu_va == kPackCpuVa);

    req.expected_stack_key = kStackKey ^ 1u;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodMapSharedPackV3,
        &req, sizeof(req), &rep, sizeof(rep), &w) == A840_KMD_STACK_KEY_MISMATCH);
    EXPECT(w == sizeof(rep));
    EXPECT(rep.actual_stack_key == kStackKey);
    EXPECT(rep.gpu_va == 0u && rep.cpu_va == 0u && rep.bytes == 0u);

    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

static bool test_uc_method24_writer_promise_make_fulfill_cancel_observer() {
    MockSmmu smmu;
    MockPromiseObserver obs;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, nullptr, &obs);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 1u, nullptr, 0u) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 2u, nullptr, 0u) == A840_KMD_OK);

    NativeAdrenoAbiWriterPromiseV3 req{};
    NativeAdrenoAbiWriterPromiseReplyV3 rep{};
    req.op = NativeAdrenoPromiseMake;
    req.surface_count = 2u;
    req.queue = 0u;
    req.surface_ids[0] = 1001u;
    req.surface_ids[1] = 1002u;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodWriterPromiseV3,
        &req, sizeof(req), &rep, sizeof(rep), nullptr) == A840_KMD_OK);
    EXPECT(rep.accepted == 1u && rep.promise_token == 1u);
    EXPECT(a840_kmd_userclient_v3_pending_promises(&uc, 1u) == 1u);

    req.op = NativeAdrenoPromiseFulfill;
    req.promise_token = rep.promise_token;
    req.fence = 55u;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 2u, NativeAdrenoMethodWriterPromiseV3,
        &req, sizeof(req), &rep, sizeof(rep), nullptr) == A840_KMD_NOT_FOUND);

    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodWriterPromiseV3,
        &req, sizeof(req), &rep, sizeof(rep), nullptr) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_pending_promises(&uc, 1u) == 0u);
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodWriterPromiseV3,
        &req, sizeof(req), &rep, sizeof(rep), nullptr) == A840_KMD_STATE_CONFLICT);

    EXPECT(obs.count_state(1u, A840_ABI_V3_PROMISE_PENDING) == 1u);
    EXPECT(obs.count_state(1u, A840_ABI_V3_PROMISE_FULFILLED) == 1u);
    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

static bool test_uc_method25_surface_meta_ubwc6_kaanapali() {
    MockSmmu smmu;
    MockBoTable bos;
    bos.add(1u, 500u, 1024u * 1024u);
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, &bos, nullptr);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 1u, nullptr, 0u) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 2u, nullptr, 0u) == A840_KMD_OK);

    NativeAdrenoAbiSurfaceMetaV3 req{}, rep{};
    req.bo_handle = 500u;
    req.op = 1u;
    req.ubwc_enabled = 1u;
    req.ubwc_version = 6u;
    req.highest_bank_bit = 16u;
    req.swizzle_levels = 6u;
    req.macrotile_mode = 1u;
    req.meta_offset = 4096u;
    req.meta_pitch = 256u;
    req.flags = 1u;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodSurfaceMetaV3,
        &req, sizeof(req), &rep, sizeof(rep), nullptr) == A840_KMD_OK);

    NativeAdrenoAbiSurfaceMetaV3 get_req{};
    get_req.bo_handle = 500u;
    get_req.op = 0u;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodSurfaceMetaV3,
        &get_req, sizeof(get_req), &rep, sizeof(rep), nullptr) == A840_KMD_OK);
    EXPECT(rep.ubwc_version == 6u && rep.highest_bank_bit == 16u &&
           rep.meta_offset == 4096u && rep.meta_pitch == 256u);

    req.highest_bank_bit = 15u;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodSurfaceMetaV3,
        &req, sizeof(req), &rep, sizeof(rep), nullptr) == A840_KMD_INVALID_ARG);

    req.highest_bank_bit = 16u;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 2u, NativeAdrenoMethodSurfaceMetaV3,
        &req, sizeof(req), &rep, sizeof(rep), nullptr) == A840_KMD_NOT_FOUND);

    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

static bool test_uc_method20_wait_any_isolates_same_local_queue_across_clients() {
    MockSmmu smmu;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, nullptr, nullptr);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 1u, nullptr, 0u) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 2u, nullptr, 0u) == A840_KMD_OK);

    std::atomic<bool> a_done{false};
    NativeAdrenoAbiWaitAnyReplyV3 rep_a{}, rep_b{};
    A840KmdStatus st_a = A840_KMD_IO_ERROR, st_b = A840_KMD_IO_ERROR;

    auto wait_q0 = [&](uint64_t cid, A840KmdStatus *out_st,
                       NativeAdrenoAbiWaitAnyReplyV3 *out_rep,
                       std::atomic<bool> *flag) {
        NativeAdrenoAbiWaitAnyV3 req{};
        req.count = 1u;
        req.queues[0] = 0u;
        req.fences[0] = 10u;
        req.timeout_ns = UINT64_C(2000000000);
        *out_st = a840_kmd_userclient_v3_dispatch(&uc, cid,
            NativeAdrenoMethodWaitAnyV3, &req, sizeof(req),
            out_rep, sizeof(*out_rep), nullptr);
        if (flag)
            flag->store(true, std::memory_order_release);
    };

    std::thread ta(wait_q0, 1u, &st_a, &rep_a, &a_done);
    std::thread tb(wait_q0, 2u, &st_b, &rep_b, nullptr);
    EXPECT(wait_for_active_waiters(&uc.wait_any, 2u));

    EXPECT(a840_kmd_userclient_v3_signal_fence(&uc, 2u, 0u, 10u, 2222u) == A840_KMD_OK);
    tb.join();
    EXPECT(st_b == A840_KMD_OK && rep_b.satisfied_mask == 1u && rep_b.observed_ticks == 2222u);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT(!a_done.load(std::memory_order_acquire));

    EXPECT(a840_kmd_userclient_v3_signal_fence(&uc, 1u, 0u, 10u, 3333u) == A840_KMD_OK);
    ta.join();
    EXPECT(st_a == A840_KMD_OK && rep_a.satisfied_mask == 1u && rep_a.observed_ticks == 3333u);

    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

static bool test_uc_e2e_fault_isolation_channel_a_poisoned_channel_b_unaffected() {
    MockSmmu smmu;
    MockPromiseObserver obs;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, nullptr, &obs);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 10u, nullptr, 0u) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 20u, nullptr, 0u) == A840_KMD_OK);

    NativeAdrenoAbiWriterPromiseV3 preq{};
    NativeAdrenoAbiWriterPromiseReplyV3 prep{};
    preq.op = NativeAdrenoPromiseMake;
    preq.surface_count = 1u;
    preq.queue = 0u;
    preq.surface_ids[0] = 777u;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 10u, NativeAdrenoMethodWriterPromiseV3,
        &preq, sizeof(preq), &prep, sizeof(prep), nullptr) == A840_KMD_OK);

    NativeAdrenoAbiWaitAnyReplyV3 rep_a{}, rep_b{};
    A840KmdStatus st_a = A840_KMD_OK, st_b = A840_KMD_IO_ERROR;

    std::thread ta([&]() {
        NativeAdrenoAbiWaitAnyV3 req{};
        req.count = 1u; req.queues[0] = 0u; req.fences[0] = 5u;
        req.timeout_ns = A840_KMD_WAIT_INFINITE;
        st_a = a840_kmd_userclient_v3_dispatch(&uc, 10u, NativeAdrenoMethodWaitAnyV3,
            &req, sizeof(req), &rep_a, sizeof(rep_a), nullptr);
    });
    std::thread tb([&]() {
        NativeAdrenoAbiWaitAnyV3 req{};
        req.count = 1u; req.queues[0] = 0u; req.fences[0] = 5u;
        req.timeout_ns = UINT64_C(2000000000);
        st_b = a840_kmd_userclient_v3_dispatch(&uc, 20u, NativeAdrenoMethodWaitAnyV3,
            &req, sizeof(req), &rep_b, sizeof(rep_b), nullptr);
    });

    EXPECT(wait_for_active_waiters(&uc.wait_any, 2u));

    A840KmdFaultInput fin{};
    fin.client_id = 10u;
    fin.queue_id = 0u;
    fin.fence = 5u;
    fin.ib1_pc = UINT64_C(0x0000100088880010);
    fin.ib2_pc = UINT64_C(0x0000100099990020);
    fin.rbbm_status = 0x80100001u;
    fin.cp_status = 0x0000cafeu;
    fin.smmu_fault_va = UINT64_C(0x00004000bad00000);
    fin.smmu_fsr = 0x40000012u;
    fin.fault_kind = A840_KMD_FAULT_SMMU_TRANSLATION;
    fin.timestamp_ticks = UINT64_C(543210);
    A840KmdFaultSnapshot snap{};
    EXPECT(a840_kmd_userclient_v3_report_fault(&uc, &fin, &snap) == A840_KMD_OK);

    ta.join();
    EXPECT(st_a == A840_KMD_POISONED);

    A840KmdFaultSnapshot ring_snap{};
    EXPECT(a840_kmd_fault_get_latest(&uc.fault_capture, &ring_snap) == A840_KMD_OK);
    EXPECT(ring_snap.ib1_pc == fin.ib1_pc && ring_snap.ib2_pc == fin.ib2_pc);
    EXPECT(ring_snap.rbbm_status == fin.rbbm_status && ring_snap.cp_status == fin.cp_status);
    EXPECT(ring_snap.smmu_fault_va == fin.smmu_fault_va && ring_snap.smmu_fsr == fin.smmu_fsr);

    EXPECT(a840_kmd_userclient_v3_check_submit_channel(&uc, 10u, 0u) == A840_KMD_POISONED);
    NativeAdrenoAbiWaitAnyV3 retry_req{};
    retry_req.count = 1u; retry_req.queues[0] = 0u; retry_req.fences[0] = 6u;
    retry_req.timeout_ns = 1000000u;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 10u, NativeAdrenoMethodWaitAnyV3,
        &retry_req, sizeof(retry_req), &rep_a, sizeof(rep_a), nullptr) == A840_KMD_POISONED);

    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 10u, NativeAdrenoMethodWriterPromiseV3,
        &preq, sizeof(preq), &prep, sizeof(prep), nullptr) == A840_KMD_POISONED);
    preq.op = NativeAdrenoPromiseCancel;
    preq.promise_token = prep.promise_token;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 10u, NativeAdrenoMethodWriterPromiseV3,
        &preq, sizeof(preq), &prep, sizeof(prep), nullptr) == A840_KMD_OK);

    EXPECT(a840_kmd_userclient_v3_check_submit_channel(&uc, 10u, 1u) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_check_submit_channel(&uc, 20u, 0u) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_signal_fence(&uc, 20u, 0u, 5u, UINT64_C(777777)) == A840_KMD_OK);
    tb.join();
    EXPECT(st_b == A840_KMD_OK);
    EXPECT(rep_b.satisfied_mask == 1u && rep_b.timed_out == 0u && rep_b.observed_ticks == UINT64_C(777777));

    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

static bool test_uc_close_one_stop_cleanup_promises_meta_zeropage_waiters() {
    MockSmmu smmu;
    MockBoTable bos;
    MockPromiseObserver obs;
    bos.add(1u, 10u, 65536u);
    bos.add(1u, 11u, 65536u);

    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, &bos, &obs);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 1u, nullptr, 0u) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 2u, nullptr, 0u) == A840_KMD_OK);

    for (uint64_t s = 1; s <= 3u; ++s) {
        NativeAdrenoAbiWriterPromiseV3 pr{};
        NativeAdrenoAbiWriterPromiseReplyV3 prep{};
        pr.op = NativeAdrenoPromiseMake;
        pr.surface_count = 1u;
        pr.surface_ids[0] = 100u + s;
        EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodWriterPromiseV3,
            &pr, sizeof(pr), &prep, sizeof(prep), nullptr) == A840_KMD_OK);
    }
    for (uint64_t h = 10; h <= 11u; ++h) {
        NativeAdrenoAbiSurfaceMetaV3 sm{}, srep{};
        sm.bo_handle = h; sm.op = 1u; sm.ubwc_enabled = 1u; sm.ubwc_version = 6u;
        sm.highest_bank_bit = 16u; sm.swizzle_levels = 6u; sm.macrotile_mode = 1u;
        sm.meta_offset = 0u; sm.meta_pitch = 64u;
        EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodSurfaceMetaV3,
            &sm, sizeof(sm), &srep, sizeof(srep), nullptr) == A840_KMD_OK);
    }

    A840KmdStatus st1 = A840_KMD_OK, st2 = A840_KMD_OK;
    std::thread w1([&]() {
        NativeAdrenoAbiWaitAnyV3 req{};
        NativeAdrenoAbiWaitAnyReplyV3 rep{};
        req.count = 1u; req.queues[0] = 0u; req.fences[0] = 1u;
        req.timeout_ns = A840_KMD_WAIT_INFINITE;
        st1 = a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodWaitAnyV3,
            &req, sizeof(req), &rep, sizeof(rep), nullptr);
    });
    std::thread w2([&]() {
        NativeAdrenoAbiWaitAnyV3 req{};
        NativeAdrenoAbiWaitAnyReplyV3 rep{};
        req.count = 1u; req.queues[0] = 1u; req.fences[0] = 1u;
        req.timeout_ns = A840_KMD_WAIT_INFINITE;
        st2 = a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodWaitAnyV3,
            &req, sizeof(req), &rep, sizeof(rep), nullptr);
    });

    EXPECT(wait_for_active_waiters(&uc.wait_any, 2u));

    A840KmdClientCloseSummary sum{};
    EXPECT(a840_kmd_userclient_v3_close_client(&uc, 1u, &sum) == A840_KMD_OK);
    w1.join();
    w2.join();

    EXPECT(st1 == A840_KMD_DISCONNECTED && st2 == A840_KMD_DISCONNECTED);
    EXPECT(sum.canceled_promises == 3u);
    EXPECT(sum.zero_page_unbound == 1u);
    EXPECT(obs.count_state(1u, A840_ABI_V3_PROMISE_CANCELED) == 3u);
    EXPECT(a840_kmd_userclient_v3_pending_promises(&uc, 1u) == 0u);

    A840KmdZeroPageStats zst{};
    EXPECT(a840_kmd_zero_page_get_stats(&uc.zero_page, &zst) == A840_KMD_OK);
    EXPECT(zst.active_clients == 1u && zst.backing_allocated == 1u);

    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 1u, nullptr, 0u) == A840_KMD_OK);
    NativeAdrenoAbiSurfaceMetaV3 get_sm{}, srep{};
    get_sm.bo_handle = 10u; get_sm.op = 0u;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodSurfaceMetaV3,
        &get_sm, sizeof(get_sm), &srep, sizeof(srep), nullptr) == A840_KMD_NOT_FOUND);

    EXPECT(a840_kmd_userclient_v3_close_client(&uc, 1u, nullptr) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_close_client(&uc, 2u, nullptr) == A840_KMD_OK);
    EXPECT(a840_kmd_zero_page_get_stats(&uc.zero_page, &zst) == A840_KMD_OK);
    EXPECT(zst.active_clients == 0u && zst.backing_allocated == 0u && smmu.live_allocs == 0u);

    A840KmdWaitAnyStats wst{};
    EXPECT(a840_kmd_wait_any_get_stats(&uc.wait_any, &wst) == A840_KMD_OK);
    EXPECT(wst.active_waiters == 0u && wst.active_queues == 0u);
    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

static bool test_uc_capability_gating_disabled_methods_unsupported() {
    A840KmdUserClientConfig cfg{};
    cfg.cap_flags = NativeAdrenoParamsV3TimelineWaitAny;
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 1u, nullptr, 0u) == A840_KMD_OK);

    NativeAdrenoAbiZeroPageReplyV3 zp{};
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodGetZeroPageV3,
        nullptr, 0u, &zp, sizeof(zp), nullptr) == A840_KMD_UNSUPPORTED);

    NativeAdrenoAbiSharedPackReqV3 spq{};
    NativeAdrenoAbiSharedPackReplyV3 spr{};
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodMapSharedPackV3,
        &spq, sizeof(spq), &spr, sizeof(spr), nullptr) == A840_KMD_UNSUPPORTED);

    NativeAdrenoAbiWriterPromiseV3 wp{};
    NativeAdrenoAbiWriterPromiseReplyV3 wpr{};
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodWriterPromiseV3,
        &wp, sizeof(wp), &wpr, sizeof(wpr), nullptr) == A840_KMD_UNSUPPORTED);

    NativeAdrenoAbiSurfaceMetaV3 sm{};
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodSurfaceMetaV3,
        &sm, sizeof(sm), &sm, sizeof(sm), nullptr) == A840_KMD_UNSUPPORTED);

    NativeAdrenoAbiWaitAnyV3 wa{};
    NativeAdrenoAbiWaitAnyReplyV3 war{};
    wa.count = 1u; wa.queues[0] = 0u; wa.fences[0] = 0u;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodWaitAnyV3,
        &wa, sizeof(wa), &war, sizeof(war), nullptr) == A840_KMD_OK);

    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

static bool test_uc_open_client_rollback_on_zero_page_map_failure() {
    MockSmmu smmu;
    smmu.fail_map = true;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, nullptr, nullptr);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 1u, nullptr, 0u) == A840_KMD_IO_ERROR);
    EXPECT(a840_kmd_userclient_v3_open_client_count(&uc) == 0u);
    EXPECT(smmu.live_allocs == 0u);
    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

static bool test_uc_unknown_or_closed_client_rejected() {
    MockSmmu smmu;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, nullptr, nullptr);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);

    NativeAdrenoAbiZeroPageReplyV3 zp{};
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 0u, NativeAdrenoMethodGetZeroPageV3,
        nullptr, 0u, &zp, sizeof(zp), nullptr) == A840_KMD_INVALID_ARG);
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 99u, NativeAdrenoMethodGetZeroPageV3,
        nullptr, 0u, &zp, sizeof(zp), nullptr) == A840_KMD_NOT_FOUND);
    EXPECT(a840_kmd_userclient_v3_signal_fence(&uc, 99u, 0u, 1u, 1u) == A840_KMD_NOT_FOUND);
    EXPECT(a840_kmd_userclient_v3_check_submit_channel(&uc, 99u, 0u) == A840_KMD_NOT_FOUND);
    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

static bool test_uc_duplicate_open_and_double_close_rejected() {
    MockSmmu smmu;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, nullptr, nullptr);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 1u, nullptr, 0u) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 1u, nullptr, 0u) == A840_KMD_STATE_CONFLICT);
    EXPECT(a840_kmd_userclient_v3_close_client(&uc, 1u, nullptr) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_close_client(&uc, 1u, nullptr) == A840_KMD_NOT_FOUND);
    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

static bool test_uc_destroy_cleans_up_open_clients_zero_leaks() {
    MockSmmu smmu;
    MockPromiseObserver obs;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, nullptr, &obs);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);

    for (uint64_t cid = 1; cid <= 3u; ++cid) {
        EXPECT(a840_kmd_userclient_v3_open_client(&uc, cid, nullptr, 0u) == A840_KMD_OK);
        NativeAdrenoAbiWriterPromiseV3 pr{};
        NativeAdrenoAbiWriterPromiseReplyV3 prep{};
        pr.op = NativeAdrenoPromiseMake;
        pr.surface_count = 1u;
        pr.surface_ids[0] = cid * 10u;
        EXPECT(a840_kmd_userclient_v3_dispatch(&uc, cid, NativeAdrenoMethodWriterPromiseV3,
            &pr, sizeof(pr), &prep, sizeof(prep), nullptr) == A840_KMD_OK);
    }

    a840_kmd_userclient_v3_destroy(&uc);
    EXPECT(smmu.live_allocs == 0u);
    EXPECT(smmu.active_maps.empty());
    EXPECT(obs.count_state(0u, A840_ABI_V3_PROMISE_CANCELED) == 3u);
    return true;
}

static bool test_uc_reopen_same_client_id_isolated_from_prior_fences_and_poison() {
    MockSmmu smmu;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, nullptr, nullptr);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);

    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 42u, nullptr, 0u) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_signal_fence(&uc, 42u, 0u, 50u, 1000u) == A840_KMD_OK);
    A840KmdFaultInput fin{};
    fin.client_id = 42u; fin.queue_id = 0u; fin.fault_kind = A840_KMD_FAULT_GPU_HANG;
    EXPECT(a840_kmd_userclient_v3_report_fault(&uc, &fin, nullptr) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_close_client(&uc, 42u, nullptr) == A840_KMD_OK);

    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 42u, nullptr, 0u) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_check_submit_channel(&uc, 42u, 0u) == A840_KMD_OK);

    NativeAdrenoAbiWaitAnyV3 req{};
    NativeAdrenoAbiWaitAnyReplyV3 rep{};
    req.count = 1u; req.queues[0] = 0u; req.fences[0] = 1u; req.timeout_ns = 0u;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 42u, NativeAdrenoMethodWaitAnyV3,
        &req, sizeof(req), &rep, sizeof(rep), nullptr) == A840_KMD_OK);
    EXPECT(rep.satisfied_mask == 0u && rep.timed_out == 1u);

    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

static bool test_uc_multi_queue_close_reclaims_all_wait_any_slots() {
    MockSmmu smmu;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, nullptr, nullptr);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);

    for (uint64_t iter = 1; iter <= 80u; ++iter) {
        EXPECT(a840_kmd_userclient_v3_open_client(&uc, iter, nullptr, 0u) == A840_KMD_OK);
        for (uint64_t q = 0; q < 10u; ++q)
            EXPECT(a840_kmd_userclient_v3_signal_fence(&uc, iter, q, 1u, iter * 10u + q) == A840_KMD_OK);
        EXPECT(a840_kmd_userclient_v3_close_client(&uc, iter, nullptr) == A840_KMD_OK);
    }

    A840KmdWaitAnyStats wst{};
    EXPECT(a840_kmd_wait_any_get_stats(&uc.wait_any, &wst) == A840_KMD_OK);
    EXPECT(wst.active_queues == 0u && wst.active_waiters == 0u);
    EXPECT(smmu.live_allocs == 0u);
    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

static bool test_uc_queue_key_and_surface_meta_without_lookup_cb() {
    MockSmmu smmu;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, nullptr, nullptr);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 1u, nullptr, 0u) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_open_client(&uc, 2u, nullptr, 0u) == A840_KMD_OK);

    uint64_t k1 = 0, k2 = 0;
    EXPECT(a840_kmd_userclient_v3_queue_key(&uc, 1u, 0u, &k1) == A840_KMD_OK);
    EXPECT(a840_kmd_userclient_v3_queue_key(&uc, 2u, 0u, &k2) == A840_KMD_OK);
    EXPECT(k1 != k2);
    EXPECT(a840_kmd_userclient_v3_queue_key(&uc, 1u, UINT64_C(1) << 33, &k1) == A840_KMD_RANGE);

    NativeAdrenoAbiSurfaceMetaV3 sm{};
    sm.bo_handle = 1u; sm.op = 0u;
    EXPECT(a840_kmd_userclient_v3_dispatch(&uc, 1u, NativeAdrenoMethodSurfaceMetaV3,
        &sm, sizeof(sm), &sm, sizeof(sm), nullptr) == A840_KMD_UNSUPPORTED);

    a840_kmd_userclient_v3_destroy(&uc);
    return true;
}

/* =========================================================================
 * Group E: High-Concurrency TSan Stress Suite (--tsan-stress N)
 * ========================================================================= */

static bool run_tsan_stress_rounds(int rounds) {
    MockSmmu smmu;
    MockBoTable bos;
    MockPromiseObserver obs;
    A840KmdUserClientConfig cfg = make_default_uc_config(&smmu, &bos, &obs);
    A840KmdUserClientV3 uc{};
    EXPECT(a840_kmd_userclient_v3_init(&uc, &cfg) == A840_KMD_OK);

    NativeAdrenoAbiSharedPackReplyV3 pack{};
    pack.gpu_va = kPackGpuVa;
    pack.cpu_va = kPackCpuVa;
    pack.bytes = 65536u;
    pack.actual_stack_key = kStackKey;
    EXPECT(a840_kmd_userclient_v3_configure_shared_pack(&uc, &pack, 1u, 1u) == A840_KMD_OK);

    uint64_t expected_faults = 0u;

    for (int r = 1; r <= rounds; ++r) {
        const uint64_t base_cid = static_cast<uint64_t>(r) * 100u;
        constexpr uint64_t kHealthyCount = 4u;
        constexpr uint64_t kFaultCount = 2u;
        constexpr uint64_t kChurnCount = 2u;
        constexpr uint64_t kQueuesPerClient = 4u;
        constexpr uint64_t kTargetFence = 12u;

        for (uint64_t i = 0; i < kHealthyCount; ++i) {
            uint64_t cid = base_cid + 1u + i;
            bos.add(cid, 1u, 131072u);
            EXPECT(a840_kmd_userclient_v3_open_client(&uc, cid, nullptr, 0u) == A840_KMD_OK);
        }
        for (uint64_t i = 0; i < kFaultCount; ++i) {
            uint64_t cid = base_cid + 20u + i;
            EXPECT(a840_kmd_userclient_v3_open_client(&uc, cid, nullptr, 0u) == A840_KMD_OK);
        }
        for (uint64_t i = 0; i < kChurnCount; ++i) {
            uint64_t cid = base_cid + 40u + i;
            EXPECT(a840_kmd_userclient_v3_open_client(&uc, cid, nullptr, 0u) == A840_KMD_OK);
        }

        std::atomic<int> round_errors{0};
        std::atomic<uint32_t> fault_waiters_ready{0};
        std::atomic<uint32_t> churn_waiters_ready{0};
        std::vector<std::thread> workers;

        /* 4 Healthy clients: WaitAny + WaitAll + Methods 21/23/24/25 + v2 passthrough */
        for (uint64_t i = 0; i < kHealthyCount; ++i) {
            workers.emplace_back([&, i]() {
                uint64_t cid = base_cid + 1u + i;

                NativeAdrenoAbiZeroPageReplyV3 zp{};
                if (a840_kmd_userclient_v3_dispatch(&uc, cid, NativeAdrenoMethodGetZeroPageV3,
                        nullptr, 0u, &zp, sizeof(zp), nullptr) != A840_KMD_OK ||
                    zp.gpu_va != kDefaultZeroVa || zp.bytes != A840_KMD_ZERO_PAGE_BYTES)
                    round_errors.fetch_add(1);

                NativeAdrenoAbiSharedPackReqV3 spq{};
                NativeAdrenoAbiSharedPackReplyV3 spr{};
                spq.expected_stack_key = kStackKey;
                spq.map_cpu = 1u;
                if (a840_kmd_userclient_v3_dispatch(&uc, cid, NativeAdrenoMethodMapSharedPackV3,
                        &spq, sizeof(spq), &spr, sizeof(spr), nullptr) != A840_KMD_OK ||
                    spr.gpu_va != kPackGpuVa)
                    round_errors.fetch_add(1);

                NativeAdrenoAbiSurfaceMetaV3 sm{}, srep{};
                sm.bo_handle = 1u; sm.op = 1u; sm.ubwc_enabled = 1u; sm.ubwc_version = 6u;
                sm.highest_bank_bit = 16u; sm.swizzle_levels = 6u; sm.macrotile_mode = 1u;
                sm.meta_offset = 4096u; sm.meta_pitch = 128u;
                if (a840_kmd_userclient_v3_dispatch(&uc, cid, NativeAdrenoMethodSurfaceMetaV3,
                        &sm, sizeof(sm), &srep, sizeof(srep), nullptr) != A840_KMD_OK)
                    round_errors.fetch_add(1);

                NativeAdrenoAbiWriterPromiseV3 pr{};
                NativeAdrenoAbiWriterPromiseReplyV3 prep{};
                pr.op = NativeAdrenoPromiseMake;
                pr.surface_count = 1u;
                pr.queue = 0u;
                pr.surface_ids[0] = cid * 10u + 1u;
                if (a840_kmd_userclient_v3_dispatch(&uc, cid, NativeAdrenoMethodWriterPromiseV3,
                        &pr, sizeof(pr), &prep, sizeof(prep), nullptr) != A840_KMD_OK)
                    round_errors.fetch_add(1);

                uint8_t dummy[16]{};
                if (a840_kmd_userclient_v3_dispatch(&uc, cid, 5u, dummy, 16u,
                        dummy, 16u, nullptr) != A840_KMD_PASS_TO_V2)
                    round_errors.fetch_add(1);

                NativeAdrenoAbiWaitAnyV3 wa{};
                NativeAdrenoAbiWaitAnyReplyV3 war{};
                wa.count = kQueuesPerClient;
                wa.flags = 0u;
                wa.timeout_ns = UINT64_C(5000000000);
                for (uint32_t q = 0; q < kQueuesPerClient; ++q) {
                    wa.queues[q] = q;
                    wa.fences[q] = kTargetFence / 2u;
                }
                if (a840_kmd_userclient_v3_dispatch(&uc, cid, NativeAdrenoMethodWaitAnyV3,
                        &wa, sizeof(wa), &war, sizeof(war), nullptr) != A840_KMD_OK ||
                    war.satisfied_mask == 0u || war.timed_out != 0u)
                    round_errors.fetch_add(1);

                wa.flags = 1u; /* WAIT_ALL */
                for (uint32_t q = 0; q < kQueuesPerClient; ++q)
                    wa.fences[q] = kTargetFence;
                if (a840_kmd_userclient_v3_dispatch(&uc, cid, NativeAdrenoMethodWaitAnyV3,
                        &wa, sizeof(wa), &war, sizeof(war), nullptr) != A840_KMD_OK ||
                    war.satisfied_mask != ((UINT32_C(1) << kQueuesPerClient) - 1u) ||
                    war.timed_out != 0u)
                    round_errors.fetch_add(1);

                pr.op = NativeAdrenoPromiseFulfill;
                pr.promise_token = prep.promise_token;
                pr.fence = kTargetFence;
                if (a840_kmd_userclient_v3_dispatch(&uc, cid, NativeAdrenoMethodWriterPromiseV3,
                        &pr, sizeof(pr), &prep, sizeof(prep), nullptr) != A840_KMD_OK)
                    round_errors.fetch_add(1);

                if (a840_kmd_userclient_v3_check_submit_channel(&uc, cid, 0u) != A840_KMD_OK)
                    round_errors.fetch_add(1);
            });
        }

        /* 2 Fault-victim clients: block infinitely on Q0 until poisoned */
        for (uint64_t i = 0; i < kFaultCount; ++i) {
            workers.emplace_back([&, i]() {
                uint64_t cid = base_cid + 20u + i;
                NativeAdrenoAbiWaitAnyV3 wa{};
                NativeAdrenoAbiWaitAnyReplyV3 war{};
                wa.count = 1u;
                wa.queues[0] = 0u;
                wa.fences[0] = 9999u;
                wa.timeout_ns = A840_KMD_WAIT_INFINITE;
                fault_waiters_ready.fetch_add(1u, std::memory_order_release);
                A840KmdStatus st = a840_kmd_userclient_v3_dispatch(&uc, cid,
                    NativeAdrenoMethodWaitAnyV3, &wa, sizeof(wa), &war, sizeof(war), nullptr);
                if (st != A840_KMD_POISONED)
                    round_errors.fetch_add(1);
                if (a840_kmd_userclient_v3_check_submit_channel(&uc, cid, 0u) != A840_KMD_POISONED)
                    round_errors.fetch_add(1);
            });
        }

        /* 2 Churn clients: create pending promise + block infinitely until closed */
        for (uint64_t i = 0; i < kChurnCount; ++i) {
            workers.emplace_back([&, i]() {
                uint64_t cid = base_cid + 40u + i;
                NativeAdrenoAbiWriterPromiseV3 pr{};
                NativeAdrenoAbiWriterPromiseReplyV3 prep{};
                pr.op = NativeAdrenoPromiseMake;
                pr.surface_count = 1u;
                pr.surface_ids[0] = cid * 10u + 7u;
                (void)a840_kmd_userclient_v3_dispatch(&uc, cid, NativeAdrenoMethodWriterPromiseV3,
                    &pr, sizeof(pr), &prep, sizeof(prep), nullptr);

                NativeAdrenoAbiWaitAnyV3 wa{};
                NativeAdrenoAbiWaitAnyReplyV3 war{};
                wa.count = 1u;
                wa.queues[0] = 0u;
                wa.fences[0] = 9999u;
                wa.timeout_ns = A840_KMD_WAIT_INFINITE;
                churn_waiters_ready.fetch_add(1u, std::memory_order_release);
                A840KmdStatus st = a840_kmd_userclient_v3_dispatch(&uc, cid,
                    NativeAdrenoMethodWaitAnyV3, &wa, sizeof(wa), &war, sizeof(war), nullptr);
                if (st != A840_KMD_DISCONNECTED && st != A840_KMD_NOT_FOUND)
                    round_errors.fetch_add(1);
            });
        }

        /* Rapid Open/GetZeroPage/Close churner thread */
        workers.emplace_back([&]() {
            for (uint64_t k = 0; k < 6u; ++k) {
                uint64_t cid = base_cid + 60u + k;
                if (a840_kmd_userclient_v3_open_client(&uc, cid, nullptr, 0u) != A840_KMD_OK) {
                    round_errors.fetch_add(1);
                    continue;
                }
                NativeAdrenoAbiZeroPageReplyV3 zp{};
                if (a840_kmd_userclient_v3_dispatch(&uc, cid, NativeAdrenoMethodGetZeroPageV3,
                        nullptr, 0u, &zp, sizeof(zp), nullptr) != A840_KMD_OK ||
                    zp.gpu_va != kDefaultZeroVa)
                    round_errors.fetch_add(1);
                if (a840_kmd_userclient_v3_close_client(&uc, cid, nullptr) != A840_KMD_OK)
                    round_errors.fetch_add(1);
            }
        });

        /* IRQ Signaler thread */
        workers.emplace_back([&]() {
            for (uint64_t f = 1u; f <= kTargetFence; ++f) {
                for (uint64_t i = 0; i < kHealthyCount; ++i) {
                    uint64_t cid = base_cid + 1u + i;
                    for (uint64_t q = 0; q < kQueuesPerClient; ++q) {
                        uint64_t ticks = static_cast<uint64_t>(r) * 100000u + f * 100u + i * 10u + q;
                        if (a840_kmd_userclient_v3_signal_fence(&uc, cid, q, f, ticks) != A840_KMD_OK)
                            round_errors.fetch_add(1);
                    }
                }
                std::this_thread::yield();
            }
        });

        /* Fault injector thread */
        workers.emplace_back([&]() {
            while (fault_waiters_ready.load(std::memory_order_acquire) < kFaultCount)
                std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            for (uint64_t i = 0; i < kFaultCount; ++i) {
                A840KmdFaultInput fin{};
                fin.client_id = base_cid + 20u + i;
                fin.queue_id = 0u;
                fin.fence = 9999u;
                fin.ib1_pc = UINT64_C(0x0000100010000000) + i * 256u;
                fin.ib2_pc = UINT64_C(0x0000100020000000) + i * 256u;
                fin.rbbm_status = 0x80000001u;
                fin.cp_status = 0x00000042u;
                fin.smmu_fault_va = UINT64_C(0x0000500000000000) + i * 4096u;
                fin.smmu_fsr = 0x40000002u;
                fin.fault_kind = (i % 2u == 0u) ? A840_KMD_FAULT_SMMU_TRANSLATION
                                                : A840_KMD_FAULT_GPU_HANG;
                fin.timestamp_ticks = static_cast<uint64_t>(r) * 100000u + 9000u + i;
                if (a840_kmd_userclient_v3_report_fault(&uc, &fin, nullptr) != A840_KMD_OK)
                    round_errors.fetch_add(1);
            }
        });

        /* Concurrent client closer thread */
        workers.emplace_back([&]() {
            while (churn_waiters_ready.load(std::memory_order_acquire) < kChurnCount)
                std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            for (uint64_t i = 0; i < kChurnCount; ++i) {
                uint64_t cid = base_cid + 40u + i;
                A840KmdClientCloseSummary sum{};
                if (a840_kmd_userclient_v3_close_client(&uc, cid, &sum) != A840_KMD_OK)
                    round_errors.fetch_add(1);
            }
        });

        for (auto &t : workers)
            t.join();

        EXPECT(round_errors.load() == 0);
        expected_faults += kFaultCount;

        for (uint64_t i = 0; i < kHealthyCount; ++i)
            EXPECT(a840_kmd_userclient_v3_close_client(&uc, base_cid + 1u + i, nullptr) == A840_KMD_OK);
        for (uint64_t i = 0; i < kFaultCount; ++i)
            EXPECT(a840_kmd_userclient_v3_close_client(&uc, base_cid + 20u + i, nullptr) == A840_KMD_OK);

        EXPECT(a840_kmd_userclient_v3_open_client_count(&uc) == 0u);
        EXPECT(a840_kmd_userclient_v3_pending_promises(&uc, 0u) == 0u);
        EXPECT(a840_kmd_fault_poisoned_channel_count(&uc.fault_capture) == 0u);
        EXPECT(a840_kmd_fault_total_count(&uc.fault_capture) == expected_faults);

        A840KmdZeroPageStats zst{};
        EXPECT(a840_kmd_zero_page_get_stats(&uc.zero_page, &zst) == A840_KMD_OK);
        EXPECT(zst.active_clients == 0u && zst.backing_allocated == 0u);

        A840KmdWaitAnyStats wst{};
        EXPECT(a840_kmd_wait_any_get_stats(&uc.wait_any, &wst) == A840_KMD_OK);
        EXPECT(wst.active_waiters == 0u && wst.active_queues == 0u);
    }

    a840_kmd_userclient_v3_destroy(&uc);
    EXPECT(smmu.live_allocs == 0u);
    return true;
}

} /* namespace */

int main(int argc, char **argv) {
    int tsan_rounds = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tsan-stress") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for --tsan-stress\n");
                return 2;
            }
            tsan_rounds = std::atoi(argv[++i]);
            if (tsan_rounds <= 0) {
                std::fprintf(stderr, "invalid --tsan-stress count\n");
                return 2;
            }
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    /* Group A: ZeroPage (14 cases) */
    RUN_CASE(test_zp_init_rejects_null_and_bad_va);
    RUN_CASE(test_zp_lazy_alloc_zero_fills_dirty_backing_page);
    RUN_CASE(test_zp_rejects_write_and_rw_permissions);
    RUN_CASE(test_zp_multi_client_shares_single_physical_page);
    RUN_CASE(test_zp_idempotent_bind_and_query_same_client);
    RUN_CASE(test_zp_rebind_different_va_rejected_state_conflict);
    RUN_CASE(test_zp_unbind_refcount_and_last_client_frees_backing);
    RUN_CASE(test_zp_rebind_after_zero_allocates_fresh_backing);
    RUN_CASE(test_zp_unbind_unknown_or_double_unbind_not_found);
    RUN_CASE(test_zp_rollback_on_first_client_map_failure);
    RUN_CASE(test_zp_rollback_on_alloc_failure_and_bad_phys);
    RUN_CASE(test_zp_unmap_failure_preserves_backing_safety);
    RUN_CASE(test_zp_capacity_limit_128_clients);
    RUN_CASE(test_zp_uninitialized_and_destroy_cleanup);

    /* Group B: Fault Capture & Isolation (10 cases) */
    RUN_CASE(test_fc_init_and_invalid_inputs);
    RUN_CASE(test_fc_record_smmu_and_ib_hang_all_fields);
    RUN_CASE(test_fc_single_channel_isolation);
    RUN_CASE(test_fc_circular_ring_wraparound_over_64_faults);
    RUN_CASE(test_fc_get_channel_latest_filters_accurately);
    RUN_CASE(test_fc_poison_callback_invoked_outside_lock);
    RUN_CASE(test_fc_clear_client_removes_poison_keeps_ring_history);
    RUN_CASE(test_fc_repeated_fault_same_channel_no_duplicate_slot);
    RUN_CASE(test_fc_empty_ring_queries);
    RUN_CASE(test_fc_destroy_detaches_state);

    /* Group C: Interrupt-Driven WaitAny / WaitAll (14 cases) */
    RUN_CASE(test_wa_validation_invalid_args);
    RUN_CASE(test_wa_fast_path_zero_sleep_already_satisfied);
    RUN_CASE(test_wa_fast_path_fence_zero_default_timeline);
    RUN_CASE(test_wa_zero_timeout_poll_unsatisfied);
    RUN_CASE(test_wa_monotonic_fence_no_regression);
    RUN_CASE(test_wa_mt_irq_wait_any_32_fences_bit31);
    RUN_CASE(test_wa_mt_irq_wait_all_32_fences_full_mask);
    RUN_CASE(test_wa_mt_duplicate_queues_in_wait_all);
    RUN_CASE(test_wa_mt_timeout_accurate_zero_busy_wait);
    RUN_CASE(test_wa_mt_targeted_wakeup_ignores_unrelated_queue);
    RUN_CASE(test_wa_mt_poison_queue_wakes_blocked_waiter_immediately);
    RUN_CASE(test_wa_mt_disconnect_queue_wakes_blocked_waiter_immediately);
    RUN_CASE(test_wa_mt_multiple_waiters_same_queue_staged_wakes);
    RUN_CASE(test_wa_custom_xnu_style_backend_abstraction);

    /* Group D: UserClient v3 3104B Envelope Dispatcher & E2E (19 cases) */
    RUN_CASE(test_uc_init_and_advertised_caps_excludes_phase4_5);
    RUN_CASE(test_uc_v2_methods_1_to_19_all_return_pass_to_v2);
    RUN_CASE(test_uc_methods_22_26_0_and_99_return_unsupported);
    RUN_CASE(test_uc_envelope_over_3104_bytes_rejected);
    RUN_CASE(test_uc_method21_get_zero_page_multi_client_idempotent);
    RUN_CASE(test_uc_method23_map_shared_pack_match_mismatch_cpu_flag);
    RUN_CASE(test_uc_method24_writer_promise_make_fulfill_cancel_observer);
    RUN_CASE(test_uc_method25_surface_meta_ubwc6_kaanapali);
    RUN_CASE(test_uc_method20_wait_any_isolates_same_local_queue_across_clients);
    RUN_CASE(test_uc_e2e_fault_isolation_channel_a_poisoned_channel_b_unaffected);
    RUN_CASE(test_uc_close_one_stop_cleanup_promises_meta_zeropage_waiters);
    RUN_CASE(test_uc_capability_gating_disabled_methods_unsupported);
    RUN_CASE(test_uc_open_client_rollback_on_zero_page_map_failure);
    RUN_CASE(test_uc_unknown_or_closed_client_rejected);
    RUN_CASE(test_uc_duplicate_open_and_double_close_rejected);
    RUN_CASE(test_uc_destroy_cleans_up_open_clients_zero_leaks);
    RUN_CASE(test_uc_reopen_same_client_id_isolated_from_prior_fences_and_poison);
    RUN_CASE(test_uc_multi_queue_close_reclaims_all_wait_any_slots);
    RUN_CASE(test_uc_queue_key_and_surface_meta_without_lookup_cb);

    /* Group E: Multi-round concurrency stress */
    int effective_rounds = (tsan_rounds > 0) ? tsan_rounds : 2;
    ++g_cases;
    if (!run_tsan_stress_rounds(effective_rounds)) {
        std::fprintf(stderr, "STRESS FAILED (%d rounds)\n", effective_rounds);
        return 1;
    }

    std::printf("PASS test_kmd_v3: %llu cases, %llu checks, %d stress rounds, 0 failures\n",
                static_cast<unsigned long long>(g_cases),
                static_cast<unsigned long long>(g_checks),
                effective_rounds);
    return 0;
}
