/* SPDX-License-Identifier: MIT */
/* Comprehensive C++17 host test suite for Wave 10 / Lane 1:
 *   - Adreno 840 GMU/HFI 18-Level Anti-Downclocking DCVS Governor (a840_kmd_gmu_dcvs)
 *   - MDSS 120Hz Scanout Sync & Zero-Extra-VSYNC Coordinator (a840_kmd_mdss_sync)
 *
 * 58 independent deterministic test cases + --tsan-stress N (8-thread
 * concurrent VSYNC / commit / frame / touch / thermal / lease / HFI stress).
 * Zero sleep-based timing flakiness (100% injected monotonic now_ns clock).
 */

#include "a840_kmd_gmu_dcvs.h"
#include "a840_kmd_mdss_sync.h"

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

std::atomic<uint64_t> g_checks{0};
unsigned g_cases_passed = 0;
const char *g_current_case = "";

#define CHECK(expr)                                                            \
    do {                                                                       \
        g_checks.fetch_add(1u, std::memory_order_relaxed);                     \
        if (!(expr)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__,  \
                         g_current_case, #expr);                               \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

template <typename Fn>
void run_case(const char *name, Fn &&fn)
{
    g_current_case = name;
    fn();
    ++g_cases_passed;
}

constexpr uint64_t kNsPerUs = UINT64_C(1000);
constexpr uint64_t kNsPerMs = UINT64_C(1000000);
constexpr uint64_t kFrame120Hz = A840_MDSS_PERIOD_120HZ_NS;

/* Tracked mutex verifying lock balance and strictly unlocked callbacks. */
struct TrackedLock {
    std::mutex mu;
    std::atomic<uint64_t> lock_count{0};
    std::atomic<uint64_t> unlock_count{0};
    std::atomic<int> depth{0};

    static void do_lock(void *c)
    {
        auto *self = static_cast<TrackedLock *>(c);
        self->mu.lock();
        self->lock_count.fetch_add(1u, std::memory_order_relaxed);
        self->depth.fetch_add(1, std::memory_order_relaxed);
    }

    static void do_unlock(void *c)
    {
        auto *self = static_cast<TrackedLock *>(c);
        self->depth.fetch_sub(1, std::memory_order_relaxed);
        self->unlock_count.fetch_add(1u, std::memory_order_relaxed);
        self->mu.unlock();
    }

    A840GmuDcvsSyncOps dcvs_ops()
    {
        A840GmuDcvsSyncOps ops{};
        ops.cookie = this;
        ops.lock = &TrackedLock::do_lock;
        ops.unlock = &TrackedLock::do_unlock;
        return ops;
    }

    A840MdssSyncLockOps mdss_ops()
    {
        A840MdssSyncLockOps ops{};
        ops.cookie = this;
        ops.lock = &TrackedLock::do_lock;
        ops.unlock = &TrackedLock::do_unlock;
        return ops;
    }
};

struct MockHfiSink {
    std::mutex mu;
    TrackedLock *lock_tracker = nullptr;
    A840KmdGmuDcvs *reenter_gov = nullptr;
    bool reenter_snapshot = false;
    bool reenter_thermal = false;
    A840KmdStatus next_send_status = A840_KMD_OK;
    std::vector<A840GmuHfiRequest> requests;
    std::vector<A840GmuDcvsEvent> events;
    std::atomic<uint32_t> fault_count{0};
    uint32_t last_fault_confirmed = 0;
    uint32_t last_fault_desired = 0;
    A840KmdStatus last_fault_err = A840_KMD_OK;
    std::atomic<bool> saw_locked_callback{false};

    static A840KmdStatus on_send(void *c, const A840GmuHfiRequest *req)
    {
        auto *self = static_cast<MockHfiSink *>(c);
        if (self->lock_tracker && self->lock_tracker->depth.load(std::memory_order_relaxed) != 0)
            self->saw_locked_callback.store(true, std::memory_order_relaxed);
        if (self->reenter_gov && self->reenter_snapshot) {
            A840GmuDcvsSnapshot snap{};
            CHECK(a840_gmu_dcvs_get_snapshot(self->reenter_gov, &snap) == A840_KMD_OK);
        }
        std::lock_guard<std::mutex> g(self->mu);
        self->requests.push_back(*req);
        return self->next_send_status;
    }

    static void on_change(void *c, const A840GmuDcvsEvent *ev)
    {
        auto *self = static_cast<MockHfiSink *>(c);
        if (self->lock_tracker && self->lock_tracker->depth.load(std::memory_order_relaxed) != 0)
            self->saw_locked_callback.store(true, std::memory_order_relaxed);
        if (self->reenter_gov && self->reenter_snapshot) {
            A840GmuDcvsSnapshot snap{};
            CHECK(a840_gmu_dcvs_get_snapshot(self->reenter_gov, &snap) == A840_KMD_OK);
        }
        std::lock_guard<std::mutex> g(self->mu);
        self->events.push_back(*ev);
    }

    static void on_fault(
        void *c,
        uint32_t hfi_state,
        A840KmdStatus err,
        uint32_t confirmed_gpu_level,
        uint32_t desired_gpu_level,
        uint64_t now_ns)
    {
        (void)hfi_state;
        auto *self = static_cast<MockHfiSink *>(c);
        if (self->lock_tracker && self->lock_tracker->depth.load(std::memory_order_relaxed) != 0)
            self->saw_locked_callback.store(true, std::memory_order_relaxed);
        if (self->reenter_gov && self->reenter_thermal) {
            CHECK(a840_gmu_dcvs_update_thermal(self->reenter_gov, 45000, now_ns) == A840_KMD_OK);
        }
        std::lock_guard<std::mutex> g(self->mu);
        self->fault_count.fetch_add(1u, std::memory_order_relaxed);
        self->last_fault_confirmed = confirmed_gpu_level;
        self->last_fault_desired = desired_gpu_level;
        self->last_fault_err = err;
    }

    A840GmuHfiOps ops()
    {
        A840GmuHfiOps o{};
        o.cookie = this;
        o.send_dcvs_request = &MockHfiSink::on_send;
        o.on_state_change = &MockHfiSink::on_change;
        o.on_hfi_fault = &MockHfiSink::on_fault;
        return o;
    }
};

struct MockMdssSink {
    std::mutex mu;
    TrackedLock *lock_tracker = nullptr;
    A840KmdMdssSync *reenter_sync = nullptr;
    std::vector<A840MdssBoostHint> hints;
    std::atomic<uint64_t> commits{0};
    std::atomic<bool> saw_locked_callback{false};

    static void on_boost(void *c, const A840MdssBoostHint *hint)
    {
        auto *self = static_cast<MockMdssSink *>(c);
        if (self->lock_tracker && self->lock_tracker->depth.load(std::memory_order_relaxed) != 0)
            self->saw_locked_callback.store(true, std::memory_order_relaxed);
        if (self->reenter_sync) {
            A840MdssSyncStats st{};
            CHECK(a840_mdss_sync_get_stats(self->reenter_sync, &st) == A840_KMD_OK);
            A840MdssDeadlineInfo dl{};
            CHECK(a840_mdss_sync_query_deadline(self->reenter_sync, hint->now_ns, &dl) == A840_KMD_OK);
        }
        std::lock_guard<std::mutex> g(self->mu);
        self->hints.push_back(*hint);
    }

    static void on_commit(
        void *c,
        uint64_t frame_id,
        uint32_t decision,
        uint64_t target_vsync_ns,
        int64_t scanout_margin_ns,
        uint32_t extra_vsync_waits)
    {
        (void)frame_id;
        (void)decision;
        (void)target_vsync_ns;
        (void)scanout_margin_ns;
        (void)extra_vsync_waits;
        auto *self = static_cast<MockMdssSink *>(c);
        if (self->lock_tracker && self->lock_tracker->depth.load(std::memory_order_relaxed) != 0)
            self->saw_locked_callback.store(true, std::memory_order_relaxed);
        if (self->reenter_sync) {
            A840MdssSyncStats st{};
            CHECK(a840_mdss_sync_get_stats(self->reenter_sync, &st) == A840_KMD_OK);
        }
        self->commits.fetch_add(1u, std::memory_order_relaxed);
    }

    A840MdssSyncCallbacks callbacks()
    {
        A840MdssSyncCallbacks cb{};
        cb.cookie = this;
        cb.on_boost_hint = &MockMdssSink::on_boost;
        cb.on_commit_planned = &MockMdssSink::on_commit;
        return cb;
    }
};

/* ==================== Group A: OPP Tables, Regs & Lifecycle (01-08) ==================== */

void test_01_sm8850_18_gpu_opp_table_exact_values()
{
    size_t count = 0;
    const A840GpuOppEntry *tbl = a840_gmu_dcvs_get_opp_table(&count);
    CHECK(tbl != nullptr);
    CHECK(count == 18u);

    const uint32_t expected_mhz[18] = {
        160u, 191u, 222u, 282u, 342u, 382u, 422u, 461u, 500u,
        578u, 646u, 726u, 826u, 902u, 1000u, 1050u, 1100u, 1200u
    };
    const uint32_t expected_vote[18] = {
        0x32u, 0x33u, 0x34u, 0x36u, 0x38u, 0x3cu, 0x40u, 0x4cu, 0x50u,
        0x80u, 0x90u, 0xc0u, 0xe0u, 0x100u, 0x180u, 0x1a0u, 0x1b0u, 0x1c0u
    };

    for (size_t i = 0; i < 18u; ++i) {
        CHECK(tbl[i].level_index == i);
        CHECK(tbl[i].freq_mhz == expected_mhz[i]);
        CHECK(tbl[i].freq_hz == static_cast<uint64_t>(expected_mhz[i]) * UINT64_C(1000000));
        CHECK(tbl[i].hfi_voltage_level == expected_vote[i]);
        CHECK(tbl[i].min_gmu_level < A840_GMU_OPP_COUNT);
        if (i > 0u) {
            CHECK(tbl[i].freq_hz > tbl[i - 1u].freq_hz);
            CHECK(tbl[i].hfi_voltage_level >= tbl[i - 1u].hfi_voltage_level);
            CHECK(tbl[i].min_gmu_level >= tbl[i - 1u].min_gmu_level);
        }
    }
    CHECK(tbl[A840_GPU_OPP_IDLE_IDX].freq_mhz == 222u);
    CHECK(tbl[A840_GPU_OPP_TOUCH_FLOOR_IDX].freq_mhz == 578u);
    CHECK(tbl[A840_GPU_OPP_LEASE_LOW_IDX].freq_mhz == 726u);
    CHECK(tbl[A840_GPU_OPP_LEASE_HIGH_IDX].freq_mhz == 902u);
    CHECK(tbl[A840_GPU_OPP_MAX_IDX].freq_mhz == 1200u);
}

void test_02_sm8850_4_gmu_opp_table_exact_values()
{
    size_t gmu_n = 0, gpu_n = 0;
    const A840GmuOppEntry *gmu = a840_gmu_dcvs_get_gmu_table(&gmu_n);
    const A840GpuOppEntry *gpu = a840_gmu_dcvs_get_opp_table(&gpu_n);
    CHECK(gmu != nullptr && gmu_n == 4u);
    CHECK(a840_gmu_dcvs_validate_opp_tables(gpu, gpu_n, gmu, gmu_n) == A840_KMD_OK);

    const uint32_t exp_mhz[4] = { 475u, 575u, 700u, 750u };
    const uint32_t exp_vote[4] = { 0x38u, 0x40u, 0x80u, 0x100u };
    for (size_t i = 0; i < 4u; ++i) {
        CHECK(gmu[i].level_index == i);
        CHECK(gmu[i].freq_mhz == exp_mhz[i]);
        CHECK(gmu[i].freq_hz == static_cast<uint64_t>(exp_mhz[i]) * UINT64_C(1000000));
        CHECK(gmu[i].hfi_voltage_level == exp_vote[i]);
    }
    CHECK(a840_gmu_dcvs_required_gmu_level(A840_GPU_OPP_IDLE_IDX) == 0u);
    CHECK(a840_gmu_dcvs_required_gmu_level(6u) == 1u);
    CHECK(a840_gmu_dcvs_required_gmu_level(A840_GPU_OPP_TOUCH_FLOOR_IDX) == 2u);
    CHECK(a840_gmu_dcvs_required_gmu_level(A840_GPU_OPP_LEASE_HIGH_IDX) == 3u);
    CHECK(a840_gmu_dcvs_required_gmu_level(99u) == 3u);
}

void test_03_opp_table_validator_rejects_corrupt_tables()
{
    size_t gpu_n = 0, gmu_n = 0;
    const A840GpuOppEntry *gpu = a840_gmu_dcvs_get_opp_table(&gpu_n);
    const A840GmuOppEntry *gmu = a840_gmu_dcvs_get_gmu_table(&gmu_n);

    CHECK(a840_gmu_dcvs_validate_opp_tables(nullptr, gpu_n, gmu, gmu_n) == A840_KMD_INVALID_ARG);
    CHECK(a840_gmu_dcvs_validate_opp_tables(gpu, gpu_n, nullptr, gmu_n) == A840_KMD_INVALID_ARG);
    CHECK(a840_gmu_dcvs_validate_opp_tables(gpu, 0u, gmu, gmu_n) == A840_KMD_RANGE);
    CHECK(a840_gmu_dcvs_validate_opp_tables(gpu, gpu_n, gmu, 0u) == A840_KMD_RANGE);

    A840GpuOppEntry bad_gpu[18];
    std::memcpy(bad_gpu, gpu, sizeof(bad_gpu));
    bad_gpu[5].freq_mhz = 300u;
    bad_gpu[5].freq_hz = UINT64_C(300000000);
    CHECK(a840_gmu_dcvs_validate_opp_tables(bad_gpu, 18u, gmu, 4u) == A840_KMD_STATE_CONFLICT);

    std::memcpy(bad_gpu, gpu, sizeof(bad_gpu));
    bad_gpu[4].min_gmu_level = 9u;
    CHECK(a840_gmu_dcvs_validate_opp_tables(bad_gpu, 18u, gmu, 4u) == A840_KMD_RANGE);

    std::memcpy(bad_gpu, gpu, sizeof(bad_gpu));
    bad_gpu[3].freq_hz = UINT64_C(281000000);
    CHECK(a840_gmu_dcvs_validate_opp_tables(bad_gpu, 18u, gmu, 4u) == A840_KMD_INVALID_ARG);

    A840GmuOppEntry bad_gmu[4];
    std::memcpy(bad_gmu, gmu, sizeof(bad_gmu));
    bad_gmu[2].hfi_voltage_level = 0x20u;
    CHECK(a840_gmu_dcvs_validate_opp_tables(gpu, 18u, bad_gmu, 4u) == A840_KMD_STATE_CONFLICT);
}

void test_04_find_level_for_freq_and_hw_reg_map()
{
    CHECK(a840_gmu_dcvs_find_level_for_freq(0u) == 0u);
    CHECK(a840_gmu_dcvs_find_level_for_freq(UINT64_C(160000000)) == 0u);
    CHECK(a840_gmu_dcvs_find_level_for_freq(UINT64_C(222000000)) == 2u);
    CHECK(a840_gmu_dcvs_find_level_for_freq(UINT64_C(223000000)) == 3u);
    CHECK(a840_gmu_dcvs_find_level_for_freq(UINT64_C(578000000)) == 9u);
    CHECK(a840_gmu_dcvs_find_level_for_freq(UINT64_C(1200000000)) == 17u);
    CHECK(a840_gmu_dcvs_find_level_for_freq(UINT64_C(2500000000)) == 17u);

    A840GmuDcvsHwRegMap rm{};
    a840_gmu_dcvs_get_hw_reg_map(nullptr); /* safe */
    a840_gmu_dcvs_get_hw_reg_map(&rm);
    CHECK(rm.cp_aperture_cntl_gmu == A840_REG_CP_APERTURE_CNTL_GMU);
    CHECK(rm.cp_cp2gmu_status == A840_REG_CP_CP2GMU_STATUS);
    CHECK(rm.rbbm_clock_cntl_global == A840_REG_RBBM_CLOCK_CNTL_GLOBAL);
    CHECK(rm.always_on_counter_lo == A840_REG_CP_ALWAYS_ON_COUNTER);
    CHECK(rm.always_on_counter_hi == A840_REG_CP_ALWAYS_ON_COUNTER_HI);
    CHECK(rm.cp_in_gmu_interrupt == A840_CP_IN_GMU_INTERRUPT);
    CHECK(rm.chip_id == A840_KAANAPALI_CHIP_ID);
    CHECK(rm.speed_bin == 0xfcu);
    CHECK(rm.gpu_opp_count == 18u);
    CHECK(rm.gmu_opp_count == 4u);
}

void test_05_dcvs_init_defaults_and_bad_config_rejection()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(nullptr, nullptr) == A840_KMD_INVALID_ARG);

    A840GmuDcvsConfig bad{};
    bad.thermal_hysteresis_mc = -100;
    CHECK(a840_gmu_dcvs_init(&gov, &bad) == A840_KMD_RANGE);

    bad = {};
    bad.low_util_permille = 800u;
    bad.high_util_permille = 700u;
    CHECK(a840_gmu_dcvs_init(&gov, &bad) == A840_KMD_RANGE);

    bad = {};
    bad.initial_gpu_level = 18u;
    CHECK(a840_gmu_dcvs_init(&gov, &bad) == A840_KMD_RANGE);

    A840GmuDcvsSyncOps broken_sync{};
    bad = {};
    bad.sync_ops = &broken_sync;
    CHECK(a840_gmu_dcvs_init(&gov, &bad) == A840_KMD_INVALID_ARG);

    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);
    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_level == A840_GPU_OPP_IDLE_IDX);
    CHECK(snap.confirmed_gpu_freq_hz == UINT64_C(222000000));
    CHECK(snap.confirmed_gmu_level == 0u);
    CHECK(snap.confirmed_gmu_freq_hz == UINT64_C(475000000));
    CHECK(snap.thermal_cap_level == A840_GPU_OPP_MAX_IDX);
    CHECK(snap.hfi_state == A840_GMU_HFI_HEALTHY);
    a840_gmu_dcvs_destroy(&gov);

    A840GmuDcvsConfig explicit0{};
    explicit0.use_explicit_init_level = 1u;
    explicit0.initial_gpu_level = 0u;
    CHECK(a840_gmu_dcvs_init(&gov, &explicit0) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_level == 0u);
    CHECK(snap.confirmed_gpu_freq_hz == UINT64_C(160000000));
    a840_gmu_dcvs_destroy(&gov);
}

void test_06_dcvs_uninitialized_and_null_args_fail_closed()
{
    A840KmdGmuDcvs gov{};
    A840GmuDcvsSnapshot snap{};
    A840DcvsFrameSample sample{};
    A840MdssBoostHint hint{};

    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_gmu_dcvs_record_frame(&gov, &sample) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_gmu_dcvs_on_ring0_submit(&gov, 1000u, 1u, 0u) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_gmu_dcvs_notify_touch(&gov, 1000u, 0u) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_gmu_dcvs_request_lease(&gov, A840_GPU_OPP_LEASE_LOW_IDX, 1000u, kNsPerMs) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_gmu_dcvs_release_lease(&gov, 1000u) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_gmu_dcvs_update_thermal(&gov, 40000, 1000u) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_gmu_dcvs_tick(&gov, 1000u) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_gmu_dcvs_apply_mdss_hint(&gov, &hint) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_gmu_dcvs_on_hfi_ack(&gov, 1u, 1000u) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_gmu_dcvs_on_hfi_error(&gov, 1u, A840_KMD_IO_ERROR, 1000u) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_gmu_dcvs_on_hfi_recovered(&gov, 1000u) == A840_KMD_STATE_CONFLICT);

    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, nullptr) == A840_KMD_INVALID_ARG);
    CHECK(a840_gmu_dcvs_record_frame(&gov, nullptr) == A840_KMD_INVALID_ARG);
    CHECK(a840_gmu_dcvs_apply_mdss_hint(&gov, nullptr) == A840_KMD_INVALID_ARG);
    CHECK(a840_gmu_dcvs_on_hfi_ack(&gov, 0u, 1000u) == A840_KMD_INVALID_ARG);
    CHECK(a840_gmu_dcvs_on_hfi_error(&gov, 1u, A840_KMD_OK, 1000u) == A840_KMD_INVALID_ARG);
    a840_gmu_dcvs_destroy(&gov);
    a840_gmu_dcvs_destroy(&gov); /* idempotent */
    a840_gmu_dcvs_destroy(nullptr);
}

void test_07_dcvs_gmu_scales_with_gpu_and_ring0_pressure()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);
    A840GmuDcvsSnapshot snap{};

    /* Wake into 578 MHz (idx 9) -> GMU idx 2 (700 MHz). */
    A840DcvsFrameSample s{};
    s.now_ns = 10u * kNsPerMs;
    s.gpu_busy_ns = 4u * kNsPerMs;
    s.frame_period_ns = kFrame120Hz;
    CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_level == A840_GPU_OPP_TOUCH_FLOOR_IDX);
    CHECK(snap.confirmed_gmu_level == 2u);
    CHECK(snap.confirmed_gmu_freq_hz == UINT64_C(700000000));

    /* Heavy Ring 0 burst bumps GMU to 750 MHz (idx 3) to prevent HFI bottleneck. */
    CHECK(a840_gmu_dcvs_on_ring0_submit(&gov, 18u * kNsPerMs, 6u, 0u) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_level >= 10u);
    CHECK(snap.confirmed_gmu_level == 3u);
    CHECK(snap.confirmed_gmu_freq_hz == UINT64_C(750000000));
    a840_gmu_dcvs_destroy(&gov);
}

void test_08_dcvs_wakeup_burst_max_2x_cur_or_578mhz()
{
    /* Case 1: waking from 222 MHz -> max(444, 578) = 578 MHz. */
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_notify_touch(&gov, 10u * kNsPerMs, 50u * kNsPerMs) == A840_KMD_OK);
    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_freq_hz == UINT64_C(578000000));
    a840_gmu_dcvs_destroy(&gov);

    /* Case 2: touch from 342 MHz (idx 4) -> 2*342 = 684 -> 726 MHz (idx 11). */
    A840GmuDcvsConfig cfg{};
    cfg.initial_gpu_level = 4u; /* 342 MHz */
    CHECK(a840_gmu_dcvs_init(&gov, &cfg) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_notify_touch(&gov, 10u * kNsPerMs, 50u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_level == A840_GPU_OPP_LEASE_LOW_IDX);
    CHECK(snap.confirmed_gpu_freq_hz == UINT64_C(726000000));
    a840_gmu_dcvs_destroy(&gov);
}

/* ============== Group B: Anti-Oscillation, 120Hz & Idle Decay (09-18) ============== */

void test_09_anti_oscillation_vs_naive_simple_ondemand()
{
    /* Simulate 10 seconds (1200 frames @ 120Hz) of bursty light/heavy UI frames.
     * Naive simple_ondemand snaps to 1200 MHz on heavy bursts and back to 160 MHz
     * on light gaps (>100 transitions / 10s). Our governor stays bounded and
     * never collapses to 160/222 MHz during continuous 120Hz cadence.
     */
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);

    uint32_t naive_transitions = 0u;
    uint32_t naive_lvl = 0u;
    uint32_t gov_idle_drops = 0u;
    uint64_t t = 100u * kNsPerMs;

    for (uint32_t f = 0; f < 1200u; ++f) {
        /* Alternating 4 heavy frames (7.2 ms) and 6 light frames (1.6 ms). */
        bool heavy = (f % 10u) < 4u;
        uint64_t busy = heavy ? UINT64_C(7200000) : UINT64_C(1600000);

        uint32_t next_naive = heavy ? A840_GPU_OPP_MAX_IDX : A840_GPU_OPP_MIN_IDX;
        if (next_naive != naive_lvl) {
            ++naive_transitions;
            naive_lvl = next_naive;
        }

        A840DcvsFrameSample s{};
        s.now_ns = t;
        s.gpu_busy_ns = busy;
        s.frame_period_ns = kFrame120Hz;
        s.ring0_submits = heavy ? 3u : 1u;
        CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);

        /* Mid-frame tick 4 ms later (inside 16.7 ms hold window). */
        CHECK(a840_gmu_dcvs_tick(&gov, t + 4u * kNsPerMs) == A840_KMD_OK);

        A840GmuDcvsSnapshot snap{};
        CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
        if (snap.confirmed_gpu_level <= A840_GPU_OPP_IDLE_IDX)
            ++gov_idle_drops;

        t += kFrame120Hz;
    }

    A840GmuDcvsSnapshot final_snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &final_snap) == A840_KMD_OK);
    CHECK(naive_transitions >= 200u);
    CHECK(gov_idle_drops == 0u);
    CHECK(final_snap.transitions_in_last_10s <= A840_DCVS_DEFAULT_MAX_OSC_10S);
    CHECK(final_snap.total_upscales + final_snap.total_downscales < 30u);
    a840_gmu_dcvs_destroy(&gov);
}

void test_10_120hz_240_frames_2s_liquid_glass_zero_interframe_drop()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);

    uint64_t t = 50u * kNsPerMs;
    uint32_t min_seen_lvl = A840_GPU_OPP_MAX_IDX;

    for (uint32_t f = 0; f < 240u; ++f) {
        A840DcvsFrameSample s{};
        s.now_ns = t;
        /* Realistic liquid-glass blur + folder compositing busy times (3.8..6.8 ms). */
        s.gpu_busy_ns = UINT64_C(3800000) + static_cast<uint64_t>((f % 7u) * 500000u);
        s.frame_period_ns = kFrame120Hz;
        s.ring0_submits = 2u + (f % 3u);
        CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);
        CHECK(a840_gmu_dcvs_tick(&gov, t + 4u * kNsPerMs) == A840_KMD_OK);

        A840GmuDcvsSnapshot snap{};
        CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
        if (snap.confirmed_gpu_level < min_seen_lvl)
            min_seen_lvl = snap.confirmed_gpu_level;

        t += kFrame120Hz;
    }

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.total_frames_recorded == 240u);
    CHECK(min_seen_lvl >= A840_GPU_OPP_TOUCH_FLOOR_IDX);
    CHECK(snap.inter_frame_hold_hits == 240u);
    a840_gmu_dcvs_destroy(&gov);
}

void test_11_inter_frame_gap_under_16_7ms_never_drops_to_idle()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);

    uint64_t t = 100u * kNsPerMs;
    A840DcvsFrameSample s{};
    s.now_ns = t;
    s.gpu_busy_ns = 6u * kNsPerMs;
    s.frame_period_ns = kFrame120Hz;
    CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    uint32_t active_lvl = snap.confirmed_gpu_level;
    CHECK(active_lvl >= A840_GPU_OPP_TOUCH_FLOOR_IDX);

    /* Ticks within 16.7 ms and even up to 79 ms (< 80 ms idle_decay_ns) stay held. */
    const uint64_t offsets_ms[5] = { 4u, 8u, 12u, 16u, 79u };
    for (uint64_t off : offsets_ms) {
        CHECK(a840_gmu_dcvs_tick(&gov, t + off * kNsPerMs) == A840_KMD_OK);
        CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
        CHECK(snap.confirmed_gpu_level == active_lvl);
    }
    CHECK(snap.inter_frame_hold_hits == 4u); /* 4, 8, 12, 16 ms <= 16.7 ms */
    CHECK(snap.idle_decay_steps == 0u);
    a840_gmu_dcvs_destroy(&gov);
}

void test_12_true_idle_80ms_smooth_stairstep_decay_to_222mhz()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);

    uint64_t t = 100u * kNsPerMs;
    A840DcvsFrameSample s{};
    s.now_ns = t;
    s.gpu_busy_ns = 6u * kNsPerMs;
    s.frame_period_ns = kFrame120Hz;
    CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    uint32_t prev = snap.confirmed_gpu_level;
    CHECK(prev == A840_GPU_OPP_TOUCH_FLOOR_IDX); /* idx 9 = 578 MHz */

    /* Wait 80 ms true silence, then tick every 20 ms -> decays exactly 1 level per step. */
    uint64_t tick_t = t + 80u * kNsPerMs;
    for (uint32_t step = 0; step < 12u; ++step) {
        CHECK(a840_gmu_dcvs_tick(&gov, tick_t) == A840_KMD_OK);
        CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
        CHECK(prev - snap.confirmed_gpu_level <= 1u); /* Never cliff-drop */
        CHECK(snap.confirmed_gpu_level >= A840_GPU_OPP_IDLE_IDX);
        prev = snap.confirmed_gpu_level;
        tick_t += 20u * kNsPerMs;
    }

    CHECK(snap.confirmed_gpu_level == A840_GPU_OPP_IDLE_IDX);
    CHECK(snap.confirmed_gpu_freq_hz == UINT64_C(222000000));
    CHECK(snap.idle_decay_steps == (A840_GPU_OPP_TOUCH_FLOOR_IDX - A840_GPU_OPP_IDLE_IDX));
    a840_gmu_dcvs_destroy(&gov);
}

void test_13_frame_util_upscale_fast_vs_downscale_hysteresis_and_dwell()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);

    uint64_t t = 10u * kNsPerMs;
    /* Frame 1: wakeup + >=90% busy -> immediate fast jump. */
    A840DcvsFrameSample s{};
    s.now_ns = t;
    s.gpu_busy_ns = UINT64_C(7800000); /* 93.6% */
    s.frame_period_ns = kFrame120Hz;
    CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);

    /* Frame 2: another 93.6% busy -> +3 levels immediately. */
    t += kFrame120Hz;
    s.now_ns = t;
    CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    uint32_t peak = snap.confirmed_gpu_level;
    CHECK(peak >= 12u);

    /* Now feed 7 low-util frames (20% busy): must NOT downscale yet (<8 streak / window). */
    for (uint32_t i = 0; i < 7u; ++i) {
        t += kFrame120Hz;
        s.now_ns = t;
        s.gpu_busy_ns = UINT64_C(1500000);
        CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);
    }
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_level == peak);

    /* Continue low-util frames until rolling util <= 35% and streak >= 8 -> single -1 step. */
    bool saw_single_step = false;
    for (uint32_t i = 0; i < 20u; ++i) {
        t += kFrame120Hz;
        s.now_ns = t;
        s.gpu_busy_ns = UINT64_C(1200000);
        CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);
        CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
        if (snap.confirmed_gpu_level == peak - 1u) {
            saw_single_step = true;
            break;
        }
    }
    CHECK(saw_single_step);
    a840_gmu_dcvs_destroy(&gov);
}

void test_14_oscillation_10s_budget_throttles_downscale_but_never_blocks_upscale()
{
    A840KmdGmuDcvs gov{};
    A840GmuDcvsConfig cfg{};
    cfg.max_transitions_per_10s = 2u;
    cfg.downscale_hyst_frames = 2u;
    cfg.min_dwell_ns = 1u * kNsPerMs;
    CHECK(a840_gmu_dcvs_init(&gov, &cfg) == A840_KMD_OK);

    uint64_t t = 10u * kNsPerMs;
    A840DcvsFrameSample s{};
    s.frame_period_ns = kFrame120Hz;

    /* Two upscales fill the 10s budget of 2. */
    s.now_ns = t;
    s.gpu_busy_ns = UINT64_C(7800000);
    CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);
    t += kFrame120Hz;
    s.now_ns = t;
    CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    uint32_t held_lvl = snap.confirmed_gpu_level;
    CHECK(snap.transitions_in_last_10s >= 2u);

    /* Feed 24 very light frames within the same 10s window -> downscale must be throttled. */
    for (uint32_t i = 0; i < 24u; ++i) {
        t += kFrame120Hz;
        s.now_ns = t;
        s.gpu_busy_ns = UINT64_C(500000);
        CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);
    }
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_level == held_lvl);
    CHECK(snap.osc_budget_throttles > 0u);

    /* Emergency upscale (missed deadline) is NEVER blocked by oscillation budget! */
    t += kFrame120Hz;
    s.now_ns = t;
    s.gpu_busy_ns = UINT64_C(8000000);
    s.missed_deadline = 1u;
    CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_level > held_lvl);
    a840_gmu_dcvs_destroy(&gov);
}

void test_15_ring0_density_and_urgent_120hz_boost()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);
    A840GmuDcvsSnapshot snap{};

    CHECK(a840_gmu_dcvs_on_ring0_submit(&gov, 10u * kNsPerMs, 4u, 0u) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_level >= A840_GPU_OPP_TOUCH_FLOOR_IDX);

    CHECK(a840_gmu_dcvs_on_ring0_submit(&gov, 14u * kNsPerMs, 2u, 1u) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_level >= A840_GPU_OPP_LEASE_LOW_IDX); /* >= 726 MHz */
    a840_gmu_dcvs_destroy(&gov);
}

void test_16_frame_sample_reserved_nonzero_rejected()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);
    A840DcvsFrameSample s{};
    s.now_ns = 10u * kNsPerMs;
    s.gpu_busy_ns = 5u * kNsPerMs;
    s.reserved = 1u;
    CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_INVALID_ARG);
    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.total_frames_recorded == 0u);
    a840_gmu_dcvs_destroy(&gov);
}

void test_17_frame_sample_low_margin_and_missed_deadline_inline_upscale()
{
    A840KmdGmuDcvs gov{};
    A840GmuDcvsConfig cfg{};
    cfg.initial_gpu_level = 9u; /* 578 MHz */
    CHECK(a840_gmu_dcvs_init(&gov, &cfg) == A840_KMD_OK);

    A840DcvsFrameSample s{};
    s.now_ns = 10u * kNsPerMs;
    s.gpu_busy_ns = 4u * kNsPerMs;
    s.frame_period_ns = kFrame120Hz;
    s.has_scanout_margin = 1u;
    s.scanout_margin_ns = 900000; /* 900 us < 1.5 ms -> +2 levels */
    CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_level == 11u); /* 726 MHz */

    s.now_ns = 18u * kNsPerMs;
    s.missed_deadline = 1u; /* +3 levels */
    CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_level == 14u); /* 1000 MHz */
    a840_gmu_dcvs_destroy(&gov);
}

void test_18_active_120hz_low_util_never_drops_below_active_non_idle_floor()
{
    A840KmdGmuDcvs gov{};
    A840GmuDcvsConfig cfg{};
    cfg.downscale_hyst_frames = 2u;
    cfg.min_dwell_ns = 8u * kNsPerMs;
    cfg.max_transitions_per_10s = 60u;
    CHECK(a840_gmu_dcvs_init(&gov, &cfg) == A840_KMD_OK);

    uint64_t t = 10u * kNsPerMs;
    for (uint32_t f = 0; f < 100u; ++f) {
        A840DcvsFrameSample s{};
        s.now_ns = t;
        s.gpu_busy_ns = 400u * kNsPerUs; /* very light 4.8% */
        s.frame_period_ns = kFrame120Hz;
        CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);
        A840GmuDcvsSnapshot snap{};
        CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
        CHECK(snap.confirmed_gpu_level >= A840_GPU_OPP_IDLE_IDX + 1u); /* >= 282 MHz */
        t += kFrame120Hz;
    }
    a840_gmu_dcvs_destroy(&gov);
}

/* ======== Group C: Touch Floor, Cold Lease & 5-Tier Thermal Cap (19-30) ======== */

void test_19_touch_event_578mhz_floor_and_expiry()
{
    A840KmdGmuDcvs gov{};
    A840GmuDcvsConfig cfg{};
    cfg.downscale_hyst_frames = 2u;
    cfg.min_dwell_ns = 8u * kNsPerMs;
    CHECK(a840_gmu_dcvs_init(&gov, &cfg) == A840_KMD_OK);

    uint64_t t = 10u * kNsPerMs;
    CHECK(a840_gmu_dcvs_notify_touch(&gov, t, 100u * kNsPerMs) == A840_KMD_OK);

    /* During 100 ms touch window, light frames cannot drop below 578 MHz. */
    for (uint32_t f = 0; f < 10u; ++f) {
        t += kFrame120Hz;
        A840DcvsFrameSample s{};
        s.now_ns = t;
        s.gpu_busy_ns = 300u * kNsPerUs;
        s.frame_period_ns = kFrame120Hz;
        CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);
        A840GmuDcvsSnapshot snap{};
        CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
        CHECK(snap.touch_floor_active == 1u);
        CHECK(snap.confirmed_gpu_level >= A840_GPU_OPP_TOUCH_FLOOR_IDX);
    }

    /* After touch expires + 80 ms true idle, decay proceeds below 578 MHz. */
    CHECK(a840_gmu_dcvs_tick(&gov, t + 100u * kNsPerMs) == A840_KMD_OK);
    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.touch_floor_active == 0u);
    CHECK(snap.confirmed_gpu_level < A840_GPU_OPP_TOUCH_FLOOR_IDX);
    a840_gmu_dcvs_destroy(&gov);
}

void test_20_touch_extends_expiry_monotonically_and_default_duration()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);

    /* duration_ns == 0 -> default 120 ms */
    CHECK(a840_gmu_dcvs_notify_touch(&gov, 10u * kNsPerMs, 0u) == A840_KMD_OK);
    /* Second shorter touch at 20 ms must NOT shorten expiry below 130 ms. */
    CHECK(a840_gmu_dcvs_notify_touch(&gov, 20u * kNsPerMs, 10u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_tick(&gov, 120u * kNsPerMs) == A840_KMD_OK);
    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.touch_floor_active == 1u);
    CHECK(snap.confirmed_gpu_level >= A840_GPU_OPP_TOUCH_FLOOR_IDX);

    CHECK(a840_gmu_dcvs_tick(&gov, 131u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.touch_floor_active == 0u);
    a840_gmu_dcvs_destroy(&gov);
}

void test_21_lease_726mhz_granted_with_fresh_cold_sensor()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_update_thermal(&gov, 42000, 10u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_request_lease(&gov, A840_GPU_OPP_LEASE_LOW_IDX, 15u * kNsPerMs, 200u * kNsPerMs) == A840_KMD_OK);

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.lease_floor_active == 1u);
    CHECK(snap.lease_target_level == A840_GPU_OPP_LEASE_LOW_IDX);
    CHECK(snap.confirmed_gpu_freq_hz == UINT64_C(726000000));
    CHECK(snap.lease_granted_count == 1u);
    a840_gmu_dcvs_destroy(&gov);
}

void test_22_lease_902mhz_granted_with_fresh_cold_sensor()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_update_thermal(&gov, 40000, 10u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_request_lease(&gov, A840_GPU_OPP_LEASE_HIGH_IDX, 20u * kNsPerMs, 150u * kNsPerMs) == A840_KMD_OK);

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.lease_floor_active == 1u);
    CHECK(snap.confirmed_gpu_freq_hz == UINT64_C(902000000));
    CHECK(snap.confirmed_gmu_level == 3u);
    CHECK(snap.confirmed_gmu_freq_hz == UINT64_C(750000000));
    a840_gmu_dcvs_destroy(&gov);
}

void test_23_lease_fail_closed_when_no_thermal_sample()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_request_lease(&gov, A840_GPU_OPP_LEASE_LOW_IDX, 10u * kNsPerMs, 100u * kNsPerMs) == A840_KMD_PERMISSION_DENIED);
    CHECK(a840_gmu_dcvs_request_lease(&gov, A840_GPU_OPP_LEASE_HIGH_IDX, 10u * kNsPerMs, 100u * kNsPerMs) == A840_KMD_PERMISSION_DENIED);

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.lease_floor_active == 0u);
    CHECK(snap.lease_fail_closed_count == 2u);
    CHECK(snap.confirmed_gpu_level == A840_GPU_OPP_IDLE_IDX);
    a840_gmu_dcvs_destroy(&gov);
}

void test_24_lease_fail_closed_when_sensor_stale_on_request_and_tick()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);

    CHECK(a840_gmu_dcvs_update_thermal(&gov, 38000, 100u * kNsPerMs) == A840_KMD_OK);
    /* 600 ms later (> 500 ms stale threshold) -> request denied. */
    CHECK(a840_gmu_dcvs_request_lease(&gov, A840_GPU_OPP_LEASE_LOW_IDX, 700u * kNsPerMs, 500u * kNsPerMs) == A840_KMD_PERMISSION_DENIED);

    /* Fresh sample -> granted, then sensor becomes stale during tick -> auto-revoked! */
    CHECK(a840_gmu_dcvs_update_thermal(&gov, 38000, 700u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_request_lease(&gov, A840_GPU_OPP_LEASE_LOW_IDX, 710u * kNsPerMs, 2000u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_tick(&gov, 1250u * kNsPerMs) == A840_KMD_OK);

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.lease_floor_active == 0u);
    CHECK(snap.lease_fail_closed_count == 2u);
    a840_gmu_dcvs_destroy(&gov);
}

void test_25_lease_fail_closed_when_temperature_too_warm()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);

    /* 56 C -> rejects 726 MHz lease (> 55 C). */
    CHECK(a840_gmu_dcvs_update_thermal(&gov, 56000, 10u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_request_lease(&gov, A840_GPU_OPP_LEASE_LOW_IDX, 15u * kNsPerMs, 100u * kNsPerMs) == A840_KMD_PERMISSION_DENIED);

    /* 52 C -> allows 726 MHz, rejects 902 MHz (> 50 C). */
    CHECK(a840_gmu_dcvs_update_thermal(&gov, 52000, 20u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_request_lease(&gov, A840_GPU_OPP_LEASE_HIGH_IDX, 25u * kNsPerMs, 100u * kNsPerMs) == A840_KMD_PERMISSION_DENIED);
    CHECK(a840_gmu_dcvs_request_lease(&gov, A840_GPU_OPP_LEASE_LOW_IDX, 25u * kNsPerMs, 500u * kNsPerMs) == A840_KMD_OK);

    /* Heating to 58 C automatically revokes active 726 MHz lease. */
    CHECK(a840_gmu_dcvs_update_thermal(&gov, 58000, 40u * kNsPerMs) == A840_KMD_OK);
    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.lease_floor_active == 0u);
    CHECK(snap.lease_fail_closed_count == 3u);
    a840_gmu_dcvs_destroy(&gov);
}

void test_26_lease_invalid_level_and_zero_duration_rejected_and_manual_release()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_update_thermal(&gov, 35000, 10u * kNsPerMs) == A840_KMD_OK);

    CHECK(a840_gmu_dcvs_request_lease(&gov, 9u, 15u * kNsPerMs, 100u * kNsPerMs) == A840_KMD_RANGE);
    CHECK(a840_gmu_dcvs_request_lease(&gov, 17u, 15u * kNsPerMs, 100u * kNsPerMs) == A840_KMD_RANGE);
    CHECK(a840_gmu_dcvs_request_lease(&gov, A840_GPU_OPP_LEASE_LOW_IDX, 15u * kNsPerMs, 0u) == A840_KMD_INVALID_ARG);

    CHECK(a840_gmu_dcvs_request_lease(&gov, A840_GPU_OPP_LEASE_LOW_IDX, 15u * kNsPerMs, 100u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_release_lease(&gov, 20u * kNsPerMs) == A840_KMD_OK);
    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.lease_floor_active == 0u);
    a840_gmu_dcvs_destroy(&gov);
}

void test_27_thermal_5_tiers_exact_hard_caps_75_to_95c()
{
    A840KmdGmuDcvs gov{};
    A840GmuDcvsConfig cfg{};
    cfg.initial_gpu_level = A840_GPU_OPP_MAX_IDX; /* Start at 1200 MHz */
    CHECK(a840_gmu_dcvs_init(&gov, &cfg) == A840_KMD_OK);

    const struct { int32_t temp_mc; uint32_t tier; uint32_t exp_mhz; } steps[6] = {
        { 74000, 0u, 1200u },
        { 75000, 1u, 1000u },
        { 80000, 2u,  902u },
        { 85000, 3u,  826u },
        { 90000, 4u,  726u },
        { 95000, 5u,  578u }
    };

    uint64_t t = 10u * kNsPerMs;
    for (const auto &st : steps) {
        CHECK(a840_gmu_dcvs_update_thermal(&gov, st.temp_mc, t) == A840_KMD_OK);
        A840GmuDcvsSnapshot snap{};
        CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
        CHECK(snap.thermal_tier == st.tier);
        CHECK(snap.confirmed_gpu_freq_hz == static_cast<uint64_t>(st.exp_mhz) * UINT64_C(1000000));
        t += 10u * kNsPerMs;
    }
    a840_gmu_dcvs_destroy(&gov);
}

void test_28_thermal_hard_cap_overrides_all_boosts_touch_lease_mdss_ring0()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_update_thermal(&gov, 95000, 10u * kNsPerMs) == A840_KMD_OK);

    /* Try every possible boost source: none may exceed 578 MHz (idx 9). */
    CHECK(a840_gmu_dcvs_notify_touch(&gov, 12u * kNsPerMs, 200u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_on_ring0_submit(&gov, 14u * kNsPerMs, 12u, 1u) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_request_lease(&gov, A840_GPU_OPP_LEASE_LOW_IDX, 15u * kNsPerMs, 100u * kNsPerMs) == A840_KMD_PERMISSION_DENIED);

    A840MdssBoostHint hint{};
    hint.frame_id = 1u;
    hint.now_ns = 16u * kNsPerMs;
    hint.severity = A840_MDSS_BOOST_CONSECUTIVE_MISS;
    hint.recommended_step_up = 5u;
    CHECK(a840_gmu_dcvs_apply_mdss_hint(&gov, &hint) == A840_KMD_OK);

    A840DcvsFrameSample s{};
    s.now_ns = 20u * kNsPerMs;
    s.gpu_busy_ns = kFrame120Hz;
    s.frame_period_ns = kFrame120Hz;
    s.missed_deadline = 1u;
    s.ring0_submits = 10u;
    CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.thermal_cap_level == A840_GPU_OPP_TOUCH_FLOOR_IDX);
    CHECK(snap.confirmed_gpu_level <= A840_GPU_OPP_TOUCH_FLOOR_IDX);
    CHECK(snap.desired_gpu_level <= A840_GPU_OPP_TOUCH_FLOOR_IDX);
    CHECK(snap.thermal_clamp_events > 0u);
    a840_gmu_dcvs_destroy(&gov);
}

void test_29_thermal_release_hysteresis_prevents_boundary_pingpong()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);
    A840GmuDcvsSnapshot snap{};

    CHECK(a840_gmu_dcvs_update_thermal(&gov, 75000, 10u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.thermal_tier == 1u);
    CHECK(snap.thermal_cap_level == A840_GPU_OPP_THERMAL_1000_IDX);

    /* 74 C is inside 2 C hysteresis band -> stays tier 1. */
    CHECK(a840_gmu_dcvs_update_thermal(&gov, 74000, 20u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.thermal_tier == 1u);

    /* 73 C meets (75 - 2) C release threshold -> returns to tier 0 (1200 MHz). */
    CHECK(a840_gmu_dcvs_update_thermal(&gov, 73000, 30u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.thermal_tier == 0u);
    CHECK(snap.thermal_cap_level == A840_GPU_OPP_MAX_IDX);
    a840_gmu_dcvs_destroy(&gov);
}

void test_30_thermal_multi_tier_cooling_stepdown()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);
    A840GmuDcvsSnapshot snap{};

    CHECK(a840_gmu_dcvs_update_thermal(&gov, 96000, 10u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.thermal_tier == 5u);

    /* Cool to 82 C: <= 93 (t5), <= 88 (t4), <= 83 (t3), > 78 (t2) -> settles at tier 2 (902 MHz). */
    CHECK(a840_gmu_dcvs_update_thermal(&gov, 82000, 20u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.thermal_tier == 2u);
    CHECK(snap.thermal_cap_level == A840_GPU_OPP_LEASE_HIGH_IDX);
    a840_gmu_dcvs_destroy(&gov);
}

/* ======== Group D: HFI Fault Recovery & Unlocked Re-entrancy (31-37) ======== */

void test_31_hfi_sync_send_error_enters_degraded_preserves_confirmed_level()
{
    MockHfiSink sink;
    A840GmuHfiOps hops = sink.ops();
    A840GmuDcvsConfig cfg{};
    cfg.hfi_ops = &hops;

    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, &cfg) == A840_KMD_OK);

    /* First raise succeeds and confirms 578 MHz. */
    CHECK(a840_gmu_dcvs_notify_touch(&gov, 10u * kNsPerMs, 100u * kNsPerMs) == A840_KMD_OK);
    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_level == 9u);

    /* Inject HFI transport error on next upscale. */
    sink.next_send_status = A840_KMD_IO_ERROR;
    CHECK(a840_gmu_dcvs_on_ring0_submit(&gov, 15u * kNsPerMs, 6u, 1u) == A840_KMD_IO_ERROR);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.hfi_state == A840_GMU_HFI_DEGRADED_RECOVERY);
    CHECK(snap.confirmed_gpu_level == 9u); /* Preserved last confirmed! */
    CHECK(snap.desired_gpu_level > 9u);    /* Desired retained */
    CHECK(snap.total_hfi_failures == 1u);
    CHECK(sink.fault_count.load() == 1u);
    CHECK(sink.last_fault_confirmed == 9u);
    CHECK(sink.last_fault_err == A840_KMD_IO_ERROR);
    a840_gmu_dcvs_destroy(&gov);
}

void test_32_hfi_degraded_suppresses_spam_and_recovery_reapplies_latest_target()
{
    MockHfiSink sink;
    A840GmuHfiOps hops = sink.ops();
    A840GmuDcvsConfig cfg{};
    cfg.hfi_ops = &hops;

    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, &cfg) == A840_KMD_OK);

    sink.next_send_status = A840_KMD_IO_ERROR;
    CHECK(a840_gmu_dcvs_notify_touch(&gov, 10u * kNsPerMs, 200u * kNsPerMs) == A840_KMD_IO_ERROR);
    size_t reqs_after_fault = sink.requests.size();

    /* Further frames during degraded mode update desired_gpu_level without spamming HFI. */
    A840DcvsFrameSample s{};
    s.now_ns = 18u * kNsPerMs;
    s.gpu_busy_ns = kFrame120Hz;
    s.frame_period_ns = kFrame120Hz;
    s.missed_deadline = 1u;
    CHECK(a840_gmu_dcvs_record_frame(&gov, &s) == A840_KMD_OK);
    CHECK(sink.requests.size() == reqs_after_fault);

    /* HFI recovers -> immediately re-dispatches latest desired level with is_recovery_retry=1. */
    sink.next_send_status = A840_KMD_OK;
    CHECK(a840_gmu_dcvs_on_hfi_recovered(&gov, 25u * kNsPerMs) == A840_KMD_OK);
    CHECK(sink.requests.size() == reqs_after_fault + 1u);
    CHECK(sink.requests.back().is_recovery_retry == 1u);
    CHECK(sink.requests.back().reason == A840_DCVS_REASON_HFI_RECOVERY);

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.hfi_state == A840_GMU_HFI_HEALTHY);
    CHECK(snap.confirmed_gpu_level == snap.desired_gpu_level);
    CHECK(snap.confirmed_gpu_level >= 12u);
    CHECK(snap.total_hfi_recoveries == 1u);
    a840_gmu_dcvs_destroy(&gov);
}

void test_33_hfi_async_ack_normal_and_wrong_seq_rejection()
{
    MockHfiSink sink;
    A840GmuHfiOps hops = sink.ops();
    A840GmuDcvsConfig cfg{};
    cfg.hfi_ops = &hops;
    cfg.async_hfi_ack = 1u;

    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, &cfg) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_notify_touch(&gov, 10u * kNsPerMs, 100u * kNsPerMs) == A840_KMD_OK);

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.hfi_state == A840_GMU_HFI_PENDING_ACK);
    CHECK(snap.confirmed_gpu_level == A840_GPU_OPP_IDLE_IDX); /* Not confirmed yet */
    CHECK(snap.desired_gpu_level == 9u);

    uint64_t seq = sink.requests.back().seq_id;
    CHECK(a840_gmu_dcvs_on_hfi_ack(&gov, seq + 99u, 11u * kNsPerMs) == A840_KMD_NOT_FOUND);
    CHECK(a840_gmu_dcvs_on_hfi_ack(&gov, seq, 11u * kNsPerMs) == A840_KMD_OK);

    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.hfi_state == A840_GMU_HFI_HEALTHY);
    CHECK(snap.confirmed_gpu_level == 9u);
    CHECK(sink.events.size() == 1u);
    a840_gmu_dcvs_destroy(&gov);
}

void test_34_hfi_async_timeout_via_tick_enters_degraded_and_recovers()
{
    MockHfiSink sink;
    A840GmuHfiOps hops = sink.ops();
    A840GmuDcvsConfig cfg{};
    cfg.hfi_ops = &hops;
    cfg.async_hfi_ack = 1u;
    cfg.hfi_timeout_ns = 5u * kNsPerMs;

    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, &cfg) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_notify_touch(&gov, 10u * kNsPerMs, 200u * kNsPerMs) == A840_KMD_OK);

    /* Tick at 13 ms -> not yet timed out. */
    CHECK(a840_gmu_dcvs_tick(&gov, 13u * kNsPerMs) == A840_KMD_OK);
    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.hfi_state == A840_GMU_HFI_PENDING_ACK);

    /* Tick at 16 ms (>= 5 ms elapsed) -> timeout triggers DEGRADED_RECOVERY. */
    CHECK(a840_gmu_dcvs_tick(&gov, 16u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.hfi_state == A840_GMU_HFI_DEGRADED_RECOVERY);
    CHECK(snap.confirmed_gpu_level == A840_GPU_OPP_IDLE_IDX);
    CHECK(sink.fault_count.load() == 1u);

    /* Recover and ack retry. */
    CHECK(a840_gmu_dcvs_on_hfi_recovered(&gov, 20u * kNsPerMs) == A840_KMD_OK);
    uint64_t retry_seq = sink.requests.back().seq_id;
    CHECK(a840_gmu_dcvs_on_hfi_ack(&gov, retry_seq, 21u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.hfi_state == A840_GMU_HFI_HEALTHY);
    CHECK(snap.confirmed_gpu_level == 9u);
    a840_gmu_dcvs_destroy(&gov);
}

void test_35_hfi_explicit_on_hfi_error_and_invalid_recovery_state_conflict()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);

    CHECK(a840_gmu_dcvs_on_hfi_recovered(&gov, 10u * kNsPerMs) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_gmu_dcvs_on_hfi_error(&gov, 0u, A840_KMD_DISCONNECTED, 10u * kNsPerMs) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_on_hfi_ack(&gov, 1u, 11u * kNsPerMs) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_gmu_dcvs_on_hfi_recovered(&gov, 12u * kNsPerMs) == A840_KMD_OK);
    a840_gmu_dcvs_destroy(&gov);
}

void test_36_dcvs_all_callbacks_strictly_unlocked_and_reentrant()
{
    TrackedLock tlock;
    A840GmuDcvsSyncOps sops = tlock.dcvs_ops();
    MockHfiSink sink;
    sink.lock_tracker = &tlock;
    sink.reenter_snapshot = true;
    sink.reenter_thermal = true;
    A840GmuHfiOps hops = sink.ops();

    A840GmuDcvsConfig cfg{};
    cfg.sync_ops = &sops;
    cfg.hfi_ops = &hops;

    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, &cfg) == A840_KMD_OK);
    sink.reenter_gov = &gov;

    CHECK(a840_gmu_dcvs_notify_touch(&gov, 10u * kNsPerMs, 100u * kNsPerMs) == A840_KMD_OK);
    sink.next_send_status = A840_KMD_IO_ERROR;
    CHECK(a840_gmu_dcvs_on_ring0_submit(&gov, 15u * kNsPerMs, 6u, 1u) == A840_KMD_IO_ERROR);
    sink.next_send_status = A840_KMD_OK;
    CHECK(a840_gmu_dcvs_on_hfi_recovered(&gov, 20u * kNsPerMs) == A840_KMD_OK);

    CHECK(!sink.saw_locked_callback.load());
    CHECK(sink.requests.size() >= 3u);
    CHECK(sink.events.size() >= 2u);
    CHECK(sink.fault_count.load() == 1u);
    CHECK(tlock.depth.load() == 0);
    CHECK(tlock.lock_count.load() == tlock.unlock_count.load());
    a840_gmu_dcvs_destroy(&gov);
}

void test_37_dcvs_recovery_respects_thermal_cap_changed_during_outage()
{
    MockHfiSink sink;
    A840GmuHfiOps hops = sink.ops();
    A840GmuDcvsConfig cfg{};
    cfg.hfi_ops = &hops;

    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, &cfg) == A840_KMD_OK);

    sink.next_send_status = A840_KMD_IO_ERROR;
    A840MdssBoostHint hint{};
    hint.now_ns = 10u * kNsPerMs;
    hint.severity = A840_MDSS_BOOST_CONSECUTIVE_MISS;
    hint.recommended_step_up = 15u;
    CHECK(a840_gmu_dcvs_apply_mdss_hint(&gov, &hint) == A840_KMD_IO_ERROR);

    /* Thermal spikes to 90 C (cap 726 MHz = idx 11) during HFI outage. */
    CHECK(a840_gmu_dcvs_update_thermal(&gov, 90000, 15u * kNsPerMs) == A840_KMD_OK);

    sink.next_send_status = A840_KMD_OK;
    CHECK(a840_gmu_dcvs_on_hfi_recovered(&gov, 20u * kNsPerMs) == A840_KMD_OK);
    CHECK(sink.requests.back().gpu_level == A840_GPU_OPP_LEASE_LOW_IDX);
    CHECK(sink.requests.back().gpu_freq_hz == UINT64_C(726000000));
    a840_gmu_dcvs_destroy(&gov);
}

/* ========= Group E: MDSS 120Hz Sync & Zero Extra VSYNC Waits (38-50) ========= */

void test_38_mdss_init_defaults_and_bad_config_rejection()
{
    A840KmdMdssSync sync{};
    CHECK(a840_mdss_sync_init(nullptr, nullptr) == A840_KMD_INVALID_ARG);

    A840MdssSyncConfig bad{};
    bad.nominal_period_ns = 100u; /* too small */
    CHECK(a840_mdss_sync_init(&sync, &bad) == A840_KMD_RANGE);

    bad = {};
    bad.commit_lead_time_ns = 5u * kNsPerMs; /* >= period/2 */
    CHECK(a840_mdss_sync_init(&sync, &bad) == A840_KMD_RANGE);

    bad = {};
    bad.low_margin_threshold_ns = 3u * kNsPerMs;
    bad.comfortable_margin_ns = 2u * kNsPerMs;
    CHECK(a840_mdss_sync_init(&sync, &bad) == A840_KMD_RANGE);

    bad = {};
    bad.pll_alpha_permille = 1001u;
    CHECK(a840_mdss_sync_init(&sync, &bad) == A840_KMD_RANGE);

    A840MdssSyncLockOps bad_lock{};
    bad = {};
    bad.sync_ops = &bad_lock;
    CHECK(a840_mdss_sync_init(&sync, &bad) == A840_KMD_INVALID_ARG);

    CHECK(a840_mdss_sync_init(&sync, nullptr) == A840_KMD_OK);
    A840MdssSyncStats st{};
    CHECK(a840_mdss_sync_get_stats(&sync, &st) == A840_KMD_OK);
    CHECK(st.nominal_period_ns == kFrame120Hz);
    CHECK(st.tracked_period_ns == kFrame120Hz);
    CHECK(st.phase_locked == 0u);
    a840_mdss_sync_destroy(&sync);
}

void test_39_mdss_uninitialized_and_unlocked_phase_fail_closed()
{
    A840KmdMdssSync sync{};
    A840MdssDeadlineInfo dl{};
    A840MdssCommitRequest req{};
    A840MdssCommitPlan plan{};
    A840MdssSyncStats st{};

    CHECK(a840_mdss_sync_on_vsync(&sync, 1000u, 1u) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_mdss_sync_query_deadline(&sync, 1000u, &dl) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_mdss_sync_get_stats(&sync, &st) == A840_KMD_STATE_CONFLICT);

    CHECK(a840_mdss_sync_init(&sync, nullptr) == A840_KMD_OK);
    /* Phase not locked yet -> fail-closed STATE_CONFLICT. */
    CHECK(a840_mdss_sync_query_deadline(&sync, 1000u, &dl) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_STATE_CONFLICT);

    req.reserved = 1u;
    CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_INVALID_ARG);
    a840_mdss_sync_destroy(&sync);
    a840_mdss_sync_destroy(&sync);
}

void test_40_mdss_vsync_120hz_phase_lock_and_deadline_math()
{
    A840KmdMdssSync sync{};
    CHECK(a840_mdss_sync_init(&sync, nullptr) == A840_KMD_OK);

    const uint64_t t0 = 100u * kNsPerMs;
    CHECK(a840_mdss_sync_on_vsync(&sync, t0, 10u) == A840_KMD_OK);
    /* Backward VSYNC rejected. */
    CHECK(a840_mdss_sync_on_vsync(&sync, t0 - 1u, 11u) == A840_KMD_RANGE);

    A840MdssDeadlineInfo dl{};
    CHECK(a840_mdss_sync_query_deadline(&sync, t0 + 3u * kNsPerMs, &dl) == A840_KMD_OK);
    CHECK(dl.current_vsync_ns == t0 + kFrame120Hz);
    CHECK(dl.latch_cutoff_ns == t0 + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS);
    CHECK(dl.scanout_margin_ns == static_cast<int64_t>(kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS - 3u * kNsPerMs));
    CHECK(dl.past_cutoff == 0u);
    CHECK(dl.urgent_for_ring0 == 0u);
    CHECK(dl.vsync_epoch == 11u);

    /* Multi-epoch projection 3 frames forward. */
    CHECK(a840_mdss_sync_query_deadline(&sync, t0 + 3u * kFrame120Hz + 1u * kNsPerMs, &dl) == A840_KMD_OK);
    CHECK(dl.current_vsync_ns == t0 + 4u * kFrame120Hz);
    CHECK(dl.vsync_epoch == 14u);
    a840_mdss_sync_destroy(&sync);
}

void test_41_mdss_pll_tracks_small_drift_and_rejects_outlier_gaps()
{
    A840KmdMdssSync sync{};
    CHECK(a840_mdss_sync_init(&sync, nullptr) == A840_KMD_OK);

    uint64_t ts = 100u * kNsPerMs;
    const uint64_t drifted_period = kFrame120Hz + 10u * kNsPerUs; /* +10 us drift */
    CHECK(a840_mdss_sync_on_vsync(&sync, ts, 1u) == A840_KMD_OK);
    for (uint64_t i = 2u; i <= 25u; ++i) {
        ts += drifted_period;
        CHECK(a840_mdss_sync_on_vsync(&sync, ts, i) == A840_KMD_OK);
    }

    A840MdssSyncStats st{};
    CHECK(a840_mdss_sync_get_stats(&sync, &st) == A840_KMD_OK);
    CHECK(st.pll_drift_adjustments > 0u);
    CHECK(st.tracked_period_ns > kFrame120Hz);
    CHECK(st.tracked_period_ns <= drifted_period);
    uint64_t locked_period = st.tracked_period_ns;

    /* Inject dropped TE IRQ gap (2 periods, outside +/-5% sanity band): period stays intact! */
    ts += 2u * drifted_period;
    CHECK(a840_mdss_sync_on_vsync(&sync, ts, 27u) == A840_KMD_OK);
    CHECK(a840_mdss_sync_get_stats(&sync, &st) == A840_KMD_OK);
    CHECK(st.tracked_period_ns == locked_period);
    CHECK(st.last_vsync_ns == ts);
    a840_mdss_sync_destroy(&sync);
}

void test_42_mdss_zero_extra_vsync_waits_early_writer_promise()
{
    A840KmdMdssSync sync{};
    CHECK(a840_mdss_sync_init(&sync, nullptr) == A840_KMD_OK);
    const uint64_t t0 = 100u * kNsPerMs;
    CHECK(a840_mdss_sync_on_vsync(&sync, t0, 1u) == A840_KMD_OK);

    A840MdssCommitRequest req{};
    req.frame_id = 42u;
    req.now_ns = t0 + 2u * kNsPerMs;
    req.estimated_gpu_done_ns = t0 + 5u * kNsPerMs; /* 2.533 ms before latch cutoff */
    req.has_early_writer_promise = 1u;
    req.ring0_compositor = 1u;

    A840MdssCommitPlan plan{};
    CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
    CHECK(plan.decision == A840_MDSS_COMMIT_HIT_CURRENT_VSYNC);
    CHECK(plan.extra_vsync_waits == 0u);
    CHECK(plan.target_vsync_ns == t0 + kFrame120Hz);
    CHECK(plan.scanout_margin_ns > 2500000);
    CHECK(plan.boost_hint_emitted == 0u);

    A840MdssSyncStats st{};
    CHECK(a840_mdss_sync_get_stats(&sync, &st) == A840_KMD_OK);
    CHECK(st.frames_on_time == 1u);
    CHECK(st.extra_vsync_waits_avoided == 1u);
    CHECK(st.total_extra_vsync_waits == 0u);
    a840_mdss_sync_destroy(&sync);
}

void test_43_mdss_zero_extra_vsync_waits_retired_ready_at_exact_cutoff()
{
    A840KmdMdssSync sync{};
    CHECK(a840_mdss_sync_init(&sync, nullptr) == A840_KMD_OK);
    const uint64_t t0 = 100u * kNsPerMs;
    CHECK(a840_mdss_sync_on_vsync(&sync, t0, 1u) == A840_KMD_OK);

    uint64_t cutoff = t0 + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
    A840MdssCommitRequest req{};
    req.frame_id = 43u;
    req.now_ns = cutoff;
    req.estimated_gpu_done_ns = cutoff + 999999u; /* ignored because is_retired_ready=1 */
    req.is_retired_ready = 1u;

    A840MdssCommitPlan plan{};
    CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
    CHECK(plan.decision == A840_MDSS_COMMIT_HIT_CURRENT_VSYNC);
    CHECK(plan.extra_vsync_waits == 0u);
    CHECK(plan.scanout_margin_ns == 0);
    CHECK(plan.boost_hint_emitted == 1u);
    CHECK(plan.boost_hint.severity == A840_MDSS_BOOST_LOW_MARGIN);
    a840_mdss_sync_destroy(&sync);
}

void test_44_mdss_past_cutoff_defers_cleanly_no_tear_no_drop()
{
    A840KmdMdssSync sync{};
    CHECK(a840_mdss_sync_init(&sync, nullptr) == A840_KMD_OK);
    const uint64_t t0 = 100u * kNsPerMs;
    CHECK(a840_mdss_sync_on_vsync(&sync, t0, 1u) == A840_KMD_OK);

    uint64_t cutoff = t0 + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
    A840MdssCommitRequest req{};
    req.frame_id = 44u;
    req.now_ns = cutoff + 1u; /* 1 ns past latch cutoff */
    req.is_retired_ready = 1u;

    A840MdssCommitPlan plan{};
    CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
    CHECK(plan.decision == A840_MDSS_COMMIT_DEFER_NEXT_VSYNC);
    CHECK(plan.extra_vsync_waits == 1u);
    CHECK(plan.target_vsync_ns == t0 + 2u * kFrame120Hz);
    CHECK(plan.scanout_margin_ns == -1);
    CHECK(plan.target_vsync_margin_ns > 0);
    CHECK(plan.boost_hint_emitted == 1u);
    CHECK(plan.boost_hint.severity == A840_MDSS_BOOST_MISSED_VSYNC);

    A840MdssSyncStats st{};
    CHECK(a840_mdss_sync_get_stats(&sync, &st) == A840_KMD_OK);
    CHECK(st.frames_missed_vsync == 1u);
    CHECK(st.missed_cutoff_count == 1u);
    CHECK(st.total_extra_vsync_waits == 1u);
    a840_mdss_sync_destroy(&sync);
}

void test_45_mdss_heavy_gpu_overshoot_defers_multiple_vsyncs()
{
    A840KmdMdssSync sync{};
    CHECK(a840_mdss_sync_init(&sync, nullptr) == A840_KMD_OK);
    const uint64_t t0 = 100u * kNsPerMs;
    CHECK(a840_mdss_sync_on_vsync(&sync, t0, 1u) == A840_KMD_OK);

    uint64_t cutoff = t0 + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
    A840MdssCommitRequest req{};
    req.frame_id = 45u;
    req.now_ns = t0 + 2u * kNsPerMs;
    req.estimated_gpu_done_ns = cutoff + kFrame120Hz + 1u * kNsPerMs;

    A840MdssCommitPlan plan{};
    CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
    CHECK(plan.decision == A840_MDSS_COMMIT_DEFER_NEXT_VSYNC);
    CHECK(plan.extra_vsync_waits == 2u);
    CHECK(plan.target_vsync_ns == t0 + 3u * kFrame120Hz);
    CHECK(plan.target_vsync_margin_ns >= 0);
    a840_mdss_sync_destroy(&sync);
}

void test_46_mdss_low_margin_under_1_5ms_emits_boost_hint()
{
    MockMdssSink sink;
    A840MdssSyncCallbacks cbs = sink.callbacks();
    A840MdssSyncConfig cfg{};
    cfg.callbacks = &cbs;
    cfg.has_initial_vsync = 1u;
    cfg.initial_vsync_ns = 100u * kNsPerMs;

    A840KmdMdssSync sync{};
    CHECK(a840_mdss_sync_init(&sync, &cfg) == A840_KMD_OK);

    uint64_t cutoff = cfg.initial_vsync_ns + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
    A840MdssCommitRequest req{};
    req.frame_id = 46u;
    req.now_ns = cfg.initial_vsync_ns + 2u * kNsPerMs;
    req.estimated_gpu_done_ns = cutoff - 900u * kNsPerUs; /* 900 us < 1.5 ms */

    A840MdssCommitPlan plan{};
    CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
    CHECK(plan.decision == A840_MDSS_COMMIT_HIT_CURRENT_VSYNC);
    CHECK(plan.extra_vsync_waits == 0u);
    CHECK(plan.scanout_margin_ns == 900000);
    CHECK(sink.hints.size() == 1u);
    CHECK(sink.hints[0].severity == A840_MDSS_BOOST_LOW_MARGIN);
    CHECK(sink.hints[0].recommended_step_up == 2u);
    CHECK(sink.hints[0].release_pressure == 0u);
    a840_mdss_sync_destroy(&sync);
}

void test_47_mdss_consecutive_misses_escalate_boost_severity()
{
    MockMdssSink sink;
    A840MdssSyncCallbacks cbs = sink.callbacks();
    A840MdssSyncConfig cfg{};
    cfg.callbacks = &cbs;
    cfg.has_initial_vsync = 1u;
    cfg.initial_vsync_ns = 100u * kNsPerMs;

    A840KmdMdssSync sync{};
    CHECK(a840_mdss_sync_init(&sync, &cfg) == A840_KMD_OK);

    for (uint32_t i = 0; i < 2u; ++i) {
        uint64_t base = cfg.initial_vsync_ns + i * kFrame120Hz;
        uint64_t cutoff = base + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
        A840MdssCommitRequest req{};
        req.frame_id = 100u + i;
        req.now_ns = cutoff + 200u * kNsPerUs;
        req.is_retired_ready = 1u;
        A840MdssCommitPlan plan{};
        CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
    }

    CHECK(sink.hints.size() == 2u);
    CHECK(sink.hints[0].severity == A840_MDSS_BOOST_MISSED_VSYNC);
    CHECK(sink.hints[0].recommended_step_up == 3u);
    CHECK(sink.hints[1].severity == A840_MDSS_BOOST_CONSECUTIVE_MISS);
    CHECK(sink.hints[1].recommended_step_up == 5u);
    CHECK(sink.hints[1].consecutive_missed == 2u);
    a840_mdss_sync_destroy(&sync);
}

void test_48_mdss_hysteresis_band_and_comfortable_release_after_n_frames()
{
    MockMdssSink sink;
    A840MdssSyncCallbacks cbs = sink.callbacks();
    A840MdssSyncConfig cfg{};
    cfg.callbacks = &cbs;
    cfg.has_initial_vsync = 1u;
    cfg.initial_vsync_ns = 100u * kNsPerMs;
    cfg.boost_release_frames = 6u;

    A840KmdMdssSync sync{};
    CHECK(a840_mdss_sync_init(&sync, &cfg) == A840_KMD_OK);

    /* Frame 0: low margin triggers boost_pressure_active. */
    uint64_t base = cfg.initial_vsync_ns;
    uint64_t cutoff = base + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
    A840MdssCommitRequest req{};
    A840MdssCommitPlan plan{};
    req.frame_id = 1u;
    req.now_ns = cutoff - 800u * kNsPerUs;
    req.is_retired_ready = 1u;
    CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
    CHECK(sink.hints.size() == 1u);

    /* Frame 1: hysteresis band (2.0 ms in [1.5, 2.5)) -> no hint, no release. */
    base += kFrame120Hz;
    cutoff = base + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
    req.frame_id = 2u;
    req.now_ns = cutoff - 2000u * kNsPerUs;
    CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
    CHECK(sink.hints.size() == 1u);

    /* Frames 2..6 (5 comfortable frames at 3.2 ms): boost still held. */
    for (uint32_t i = 0; i < 5u; ++i) {
        base += kFrame120Hz;
        cutoff = base + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
        req.frame_id = 3u + i;
        req.now_ns = cutoff - 3200u * kNsPerUs;
        CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
    }
    CHECK(sink.hints.size() == 1u);

    /* 6th comfortable frame -> emits release_pressure = 1! */
    base += kFrame120Hz;
    cutoff = base + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
    req.frame_id = 8u;
    req.now_ns = cutoff - 3200u * kNsPerUs;
    CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
    CHECK(sink.hints.size() == 2u);
    CHECK(sink.hints[1].release_pressure == 1u);
    CHECK(sink.hints[1].severity == A840_MDSS_BOOST_NONE);

    A840MdssSyncStats st{};
    CHECK(a840_mdss_sync_get_stats(&sync, &st) == A840_KMD_OK);
    CHECK(st.dcvs_boost_releases_sent == 1u);
    CHECK(st.boost_pressure_active == 0u);
    a840_mdss_sync_destroy(&sync);
}

void test_49_mdss_runtime_threshold_updates_and_reset_stats()
{
    A840KmdMdssSync sync{};
    A840MdssSyncConfig cfg{};
    cfg.has_initial_vsync = 1u;
    cfg.initial_vsync_ns = 100u * kNsPerMs;
    CHECK(a840_mdss_sync_init(&sync, &cfg) == A840_KMD_OK);

    CHECK(a840_mdss_sync_set_thresholds(&sync, 5u * kNsPerMs, 0u, 0u) == A840_KMD_RANGE);
    CHECK(a840_mdss_sync_set_thresholds(&sync, 600u * kNsPerUs, 1200u * kNsPerUs, 2200u * kNsPerUs) == A840_KMD_OK);

    A840MdssCommitRequest req{};
    req.frame_id = 1u;
    req.now_ns = cfg.initial_vsync_ns + 2u * kNsPerMs;
    req.is_retired_ready = 1u;
    A840MdssCommitPlan plan{};
    CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
    CHECK(plan.latch_cutoff_ns == cfg.initial_vsync_ns + kFrame120Hz - 600u * kNsPerUs);

    CHECK(a840_mdss_sync_reset_stats(&sync) == A840_KMD_OK);
    A840MdssSyncStats st{};
    CHECK(a840_mdss_sync_get_stats(&sync, &st) == A840_KMD_OK);
    CHECK(st.frames_planned == 0u);
    CHECK(st.phase_locked == 1u);
    a840_mdss_sync_destroy(&sync);
}

void test_50_mdss_callbacks_strictly_unlocked_and_reentrant()
{
    TrackedLock tlock;
    A840MdssSyncLockOps lops = tlock.mdss_ops();
    MockMdssSink sink;
    sink.lock_tracker = &tlock;
    A840MdssSyncCallbacks cbs = sink.callbacks();

    A840MdssSyncConfig cfg{};
    cfg.sync_ops = &lops;
    cfg.callbacks = &cbs;
    cfg.has_initial_vsync = 1u;
    cfg.initial_vsync_ns = 100u * kNsPerMs;

    A840KmdMdssSync sync{};
    CHECK(a840_mdss_sync_init(&sync, &cfg) == A840_KMD_OK);
    sink.reenter_sync = &sync;

    uint64_t cutoff = cfg.initial_vsync_ns + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
    A840MdssCommitRequest req{};
    req.frame_id = 50u;
    req.now_ns = cutoff - 500u * kNsPerUs;
    req.is_retired_ready = 1u;
    A840MdssCommitPlan plan{};
    CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);

    CHECK(!sink.saw_locked_callback.load());
    CHECK(sink.hints.size() == 1u);
    CHECK(sink.commits.load() == 1u);
    CHECK(tlock.depth.load() == 0);
    CHECK(tlock.lock_count.load() == tlock.unlock_count.load());
    a840_mdss_sync_destroy(&sync);
}

/* ==== Group F: Closed-Loop MDSS <-> GMU DCVS, 120Hz Proof & TSan Stress (51-58) ==== */

void test_51_closed_loop_mdss_low_margin_triggers_gmu_upscale()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);

    A840MdssSyncCallbacks cbs{};
    cbs.cookie = &gov;
    cbs.on_boost_hint = &a840_gmu_dcvs_bridge_mdss_boost;

    A840MdssSyncConfig mcfg{};
    mcfg.callbacks = &cbs;
    mcfg.has_initial_vsync = 1u;
    mcfg.initial_vsync_ns = 100u * kNsPerMs;

    A840KmdMdssSync sync{};
    CHECK(a840_mdss_sync_init(&sync, &mcfg) == A840_KMD_OK);

    /* Starting at 222 MHz idle, narrow 800 us scanout margin immediately boosts GMU DCVS! */
    uint64_t cutoff = mcfg.initial_vsync_ns + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
    A840MdssCommitRequest req{};
    req.frame_id = 51u;
    req.now_ns = mcfg.initial_vsync_ns + 2u * kNsPerMs;
    req.estimated_gpu_done_ns = cutoff - 800u * kNsPerUs;
    req.has_early_writer_promise = 1u;

    A840MdssCommitPlan plan{};
    CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
    CHECK(plan.decision == A840_MDSS_COMMIT_HIT_CURRENT_VSYNC);
    CHECK(plan.extra_vsync_waits == 0u);

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_level >= A840_GPU_OPP_TOUCH_FLOOR_IDX);
    CHECK(snap.confirmed_gmu_level >= 2u);
    CHECK(snap.mdss_boost_active == 1u);
    CHECK(snap.mdss_boost_hints_applied == 1u);

    a840_mdss_sync_destroy(&sync);
    a840_gmu_dcvs_destroy(&gov);
}

void test_52_closed_loop_missed_vsync_escalates_then_recovers_and_releases()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);

    A840MdssSyncCallbacks cbs{};
    cbs.cookie = &gov;
    cbs.on_boost_hint = &a840_gmu_dcvs_bridge_mdss_boost;

    A840MdssSyncConfig mcfg{};
    mcfg.callbacks = &cbs;
    mcfg.has_initial_vsync = 1u;
    mcfg.initial_vsync_ns = 100u * kNsPerMs;
    mcfg.boost_release_frames = 6u;

    A840KmdMdssSync sync{};
    CHECK(a840_mdss_sync_init(&sync, &mcfg) == A840_KMD_OK);

    /* 2 consecutive missed cutoffs -> GPU >= 902 MHz, GMU 750 MHz. */
    uint64_t base = mcfg.initial_vsync_ns;
    for (uint32_t i = 0; i < 2u; ++i) {
        uint64_t cutoff = base + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
        A840MdssCommitRequest req{};
        req.frame_id = 1u + i;
        req.now_ns = cutoff + 300u * kNsPerUs;
        req.is_retired_ready = 1u;
        A840MdssCommitPlan plan{};
        CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
        base += kFrame120Hz;
    }

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_level >= A840_GPU_OPP_LEASE_HIGH_IDX); /* >= 902 MHz */
    CHECK(snap.confirmed_gmu_level == 3u);
    uint32_t escalated_lvl = snap.confirmed_gpu_level;

    /* 6 consecutive comfortable frames -> MDSS sends release -> DCVS clears urgent flag cleanly
     * without cliff-dropping frequency.
     */
    for (uint32_t i = 0; i < 6u; ++i) {
        uint64_t cutoff = base + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
        A840MdssCommitRequest req{};
        req.frame_id = 10u + i;
        req.now_ns = cutoff - 3500u * kNsPerUs;
        req.is_retired_ready = 1u;
        A840MdssCommitPlan plan{};
        CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
        base += kFrame120Hz;
    }

    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.mdss_boost_active == 0u);
    CHECK(snap.mdss_boost_releases_applied == 1u);
    CHECK(snap.confirmed_gpu_level == escalated_lvl); /* Smooth release, no cliff-drop */

    a840_mdss_sync_destroy(&sync);
    a840_gmu_dcvs_destroy(&gov);
}

void test_53_closed_loop_thermal_cap_constrains_mdss_emergency_boost()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_update_thermal(&gov, 90000, 90u * kNsPerMs) == A840_KMD_OK); /* cap 726 MHz */

    A840MdssSyncCallbacks cbs{};
    cbs.cookie = &gov;
    cbs.on_boost_hint = &a840_gmu_dcvs_bridge_mdss_boost;

    A840MdssSyncConfig mcfg{};
    mcfg.callbacks = &cbs;
    mcfg.has_initial_vsync = 1u;
    mcfg.initial_vsync_ns = 100u * kNsPerMs;

    A840KmdMdssSync sync{};
    CHECK(a840_mdss_sync_init(&sync, &mcfg) == A840_KMD_OK);

    for (uint32_t i = 0; i < 3u; ++i) {
        uint64_t base = mcfg.initial_vsync_ns + i * kFrame120Hz;
        uint64_t cutoff = base + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
        A840MdssCommitRequest req{};
        req.frame_id = 1u + i;
        req.now_ns = cutoff + 400u * kNsPerUs;
        req.is_retired_ready = 1u;
        A840MdssCommitPlan plan{};
        CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
    }

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &snap) == A840_KMD_OK);
    CHECK(snap.confirmed_gpu_level == A840_GPU_OPP_LEASE_LOW_IDX); /* clamped at 726 MHz */
    a840_mdss_sync_destroy(&sync);
    a840_gmu_dcvs_destroy(&gov);
}

void test_54_closed_loop_null_bridge_safe_and_urgent_ring0_flag()
{
    A840MdssBoostHint dummy{};
    a840_gmu_dcvs_bridge_mdss_boost(nullptr, &dummy);
    a840_gmu_dcvs_bridge_mdss_boost(&dummy, nullptr);

    A840KmdMdssSync sync{};
    A840MdssSyncConfig cfg{};
    cfg.has_initial_vsync = 1u;
    cfg.initial_vsync_ns = 100u * kNsPerMs;
    CHECK(a840_mdss_sync_init(&sync, &cfg) == A840_KMD_OK);

    uint64_t cutoff = cfg.initial_vsync_ns + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
    A840MdssDeadlineInfo dl{};
    CHECK(a840_mdss_sync_query_deadline(&sync, cutoff - 3000u * kNsPerUs, &dl) == A840_KMD_OK);
    CHECK(dl.urgent_for_ring0 == 0u);
    CHECK(a840_mdss_sync_query_deadline(&sync, cutoff - 2000u * kNsPerUs, &dl) == A840_KMD_OK);
    CHECK(dl.urgent_for_ring0 == 1u);
    a840_mdss_sync_destroy(&sync);
}

void test_55_120hz_full_240_frame_closed_loop_liquid_glass_scroll_proof()
{
    A840KmdGmuDcvs gov{};
    CHECK(a840_gmu_dcvs_init(&gov, nullptr) == A840_KMD_OK);

    A840MdssSyncCallbacks cbs{};
    cbs.cookie = &gov;
    cbs.on_boost_hint = &a840_gmu_dcvs_bridge_mdss_boost;

    A840MdssSyncConfig mcfg{};
    mcfg.callbacks = &cbs;
    A840KmdMdssSync sync{};
    CHECK(a840_mdss_sync_init(&sync, &mcfg) == A840_KMD_OK);

    uint64_t vsync_ts = 100u * kNsPerMs;
    CHECK(a840_mdss_sync_on_vsync(&sync, vsync_ts, 1u) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_update_thermal(&gov, 43000, vsync_ts) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_notify_touch(&gov, vsync_ts, 200u * kNsPerMs) == A840_KMD_OK);

    uint32_t min_active_lvl = A840_GPU_OPP_MAX_IDX;
    for (uint32_t f = 0; f < 240u; ++f) {
        uint64_t commit_now = vsync_ts + 2u * kNsPerMs;
        /* Heavy liquid glass frames in middle that get rescued by GMU upscale */
        A840GmuDcvsSnapshot cur_snap{};
        CHECK(a840_gmu_dcvs_get_snapshot(&gov, &cur_snap) == A840_KMD_OK);
        /* Higher GPU frequency reduces GPU render time! */
        uint64_t base_work_ns = (cur_snap.confirmed_gpu_level >= 11u)
                                    ? UINT64_C(3200000)
                                    : UINT64_C(4600000);

        A840MdssCommitRequest req{};
        req.frame_id = f + 1u;
        req.now_ns = commit_now;
        req.estimated_gpu_done_ns = commit_now + base_work_ns;
        req.has_early_writer_promise = 1u;
        req.ring0_compositor = 1u;

        A840MdssCommitPlan plan{};
        CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
        CHECK(plan.decision == A840_MDSS_COMMIT_HIT_CURRENT_VSYNC);
        CHECK(plan.extra_vsync_waits == 0u);

        A840DcvsFrameSample fs{};
        fs.now_ns = commit_now + base_work_ns;
        fs.gpu_busy_ns = base_work_ns;
        fs.frame_period_ns = kFrame120Hz;
        fs.has_scanout_margin = 1u;
        fs.scanout_margin_ns = plan.scanout_margin_ns;
        fs.ring0_submits = 3u;
        CHECK(a840_gmu_dcvs_record_frame(&gov, &fs) == A840_KMD_OK);

        CHECK(a840_gmu_dcvs_get_snapshot(&gov, &cur_snap) == A840_KMD_OK);
        if (cur_snap.confirmed_gpu_level < min_active_lvl)
            min_active_lvl = cur_snap.confirmed_gpu_level;

        vsync_ts += kFrame120Hz;
        CHECK(a840_mdss_sync_on_vsync(&sync, vsync_ts, static_cast<uint64_t>(f) + 2u) == A840_KMD_OK);
    }

    A840MdssSyncStats mst{};
    CHECK(a840_mdss_sync_get_stats(&sync, &mst) == A840_KMD_OK);
    CHECK(mst.frames_planned == 240u);
    CHECK(mst.frames_on_time == 240u);
    CHECK(mst.frames_missed_vsync == 0u);
    CHECK(mst.extra_vsync_waits_avoided == 240u);
    CHECK(mst.total_extra_vsync_waits == 0u);
    CHECK(min_active_lvl >= A840_GPU_OPP_TOUCH_FLOOR_IDX);

    /* Post-scroll true idle > 80 ms smoothly decays to 222 MHz. */
    uint64_t idle_t = vsync_ts + 100u * kNsPerMs;
    for (uint32_t step = 0; step < 20u; ++step) {
        CHECK(a840_gmu_dcvs_tick(&gov, idle_t) == A840_KMD_OK);
        idle_t += 20u * kNsPerMs;
    }
    A840GmuDcvsSnapshot gsnap{};
    CHECK(a840_gmu_dcvs_get_snapshot(&gov, &gsnap) == A840_KMD_OK);
    CHECK(gsnap.confirmed_gpu_level == A840_GPU_OPP_IDLE_IDX);

    a840_mdss_sync_destroy(&sync);
    a840_gmu_dcvs_destroy(&gov);
}

void test_56_mdss_stats_min_max_avg_margin_accuracy()
{
    A840KmdMdssSync sync{};
    A840MdssSyncConfig cfg{};
    cfg.has_initial_vsync = 1u;
    cfg.initial_vsync_ns = 100u * kNsPerMs;
    CHECK(a840_mdss_sync_init(&sync, &cfg) == A840_KMD_OK);

    const int64_t margins[4] = { 3000000, 2000000, 1000000, -400000 };
    uint64_t base = cfg.initial_vsync_ns;
    for (int i = 0; i < 4; ++i) {
        uint64_t cutoff = base + kFrame120Hz - A840_MDSS_DEFAULT_COMMIT_LEAD_NS;
        A840MdssCommitRequest req{};
        req.frame_id = static_cast<uint64_t>(i + 1);
        req.now_ns = static_cast<uint64_t>(static_cast<int64_t>(cutoff) - margins[i]);
        req.is_retired_ready = 1u;
        A840MdssCommitPlan plan{};
        CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
        CHECK(plan.scanout_margin_ns == margins[i]);
        base += kFrame120Hz;
    }

    A840MdssSyncStats st{};
    CHECK(a840_mdss_sync_get_stats(&sync, &st) == A840_KMD_OK);
    CHECK(st.frames_planned == 4u);
    CHECK(st.frames_on_time == 3u);
    CHECK(st.frames_missed_vsync == 1u);
    CHECK(st.min_scanout_margin_ns == -400000);
    CHECK(st.max_scanout_margin_ns == 3000000);
    CHECK(st.avg_scanout_margin_ns == (3000000 + 2000000 + 1000000 - 400000) / 4);
    CHECK(st.avg_on_time_margin_ns == (3000000 + 2000000 + 1000000) / 3);
    a840_mdss_sync_destroy(&sync);
}

void test_57_dcvs_and_mdss_struct_zero_heap_stack_lifetime()
{
    for (int iter = 0; iter < 4; ++iter) {
        TrackedLock lock_a, lock_b;
        A840GmuDcvsSyncOps sops = lock_a.dcvs_ops();
        A840MdssSyncLockOps mops = lock_b.mdss_ops();

        A840GmuDcvsConfig gcfg{};
        gcfg.sync_ops = &sops;
        A840KmdGmuDcvs gov{};
        CHECK(a840_gmu_dcvs_init(&gov, &gcfg) == A840_KMD_OK);

        A840MdssSyncConfig mcfg{};
        mcfg.sync_ops = &mops;
        mcfg.has_initial_vsync = 1u;
        mcfg.initial_vsync_ns = 50u * kNsPerMs;
        A840KmdMdssSync sync{};
        CHECK(a840_mdss_sync_init(&sync, &mcfg) == A840_KMD_OK);

        CHECK(a840_gmu_dcvs_notify_touch(&gov, 55u * kNsPerMs, 30u * kNsPerMs) == A840_KMD_OK);
        A840MdssDeadlineInfo dl{};
        CHECK(a840_mdss_sync_query_deadline(&sync, 55u * kNsPerMs, &dl) == A840_KMD_OK);
        CHECK(dl.current_vsync_ns == 50u * kNsPerMs + kFrame120Hz);

        a840_mdss_sync_destroy(&sync);
        a840_gmu_dcvs_destroy(&gov);
        CHECK(lock_a.lock_count.load() == lock_a.unlock_count.load());
        CHECK(lock_b.lock_count.load() == lock_b.unlock_count.load());
    }
}

void test_58_tsan_8_thread_concurrent_stress(unsigned stress_rounds)
{
    for (unsigned round = 0; round < stress_rounds; ++round) {
        MockHfiSink hfi_sink;
        A840GmuHfiOps hops = hfi_sink.ops();
        A840GmuDcvsConfig gcfg{};
        gcfg.hfi_ops = &hops;
        A840KmdGmuDcvs gov{};
        CHECK(a840_gmu_dcvs_init(&gov, &gcfg) == A840_KMD_OK);

        A840MdssSyncCallbacks cbs{};
        cbs.cookie = &gov;
        cbs.on_boost_hint = &a840_gmu_dcvs_bridge_mdss_boost;
        A840MdssSyncConfig mcfg{};
        mcfg.callbacks = &cbs;
        mcfg.has_initial_vsync = 1u;
        mcfg.initial_vsync_ns = 100u * kNsPerMs;
        A840KmdMdssSync sync{};
        CHECK(a840_mdss_sync_init(&sync, &mcfg) == A840_KMD_OK);

        std::atomic<uint64_t> shared_clock{100u * kNsPerMs};
        const uint32_t iters = 80u;

        std::vector<std::thread> threads;
        threads.reserve(8u);

        /* T0: 120Hz VSYNC producer */
        threads.emplace_back([&]() {
            uint64_t ts = 100u * kNsPerMs;
            for (uint32_t i = 1u; i <= iters; ++i) {
                ts += kFrame120Hz + static_cast<uint64_t>((i % 3u) * 5u * kNsPerUs);
                shared_clock.store(ts, std::memory_order_relaxed);
                A840KmdStatus st = a840_mdss_sync_on_vsync(&sync, ts, i + 1u);
                CHECK(st == A840_KMD_OK || st == A840_KMD_RANGE);
            }
        });

        /* T1: RenderServer MDSS commit planner -> closed-loop GMU bridge */
        threads.emplace_back([&]() {
            for (uint32_t i = 0u; i < iters; ++i) {
                uint64_t now = shared_clock.load(std::memory_order_relaxed) + 2u * kNsPerMs;
                A840MdssCommitRequest req{};
                req.frame_id = static_cast<uint64_t>(round) * 1000u + i;
                req.now_ns = now;
                req.estimated_gpu_done_ns = now + ((i % 5u == 0u) ? (7u * kNsPerMs) : (3u * kNsPerMs));
                req.has_early_writer_promise = 1u;
                req.ring0_compositor = 1u;
                A840MdssCommitPlan plan{};
                CHECK(a840_mdss_sync_plan_commit(&sync, &req, &plan) == A840_KMD_OK);
            }
        });

        /* T2: GPU frame workload recorder */
        threads.emplace_back([&]() {
            for (uint32_t i = 0u; i < iters; ++i) {
                uint64_t now = shared_clock.load(std::memory_order_relaxed) + 3u * kNsPerMs;
                A840DcvsFrameSample s{};
                s.now_ns = now;
                s.gpu_busy_ns = ((i % 4u) + 2u) * kNsPerMs;
                s.frame_period_ns = kFrame120Hz;
                s.ring0_submits = (i % 6u);
                s.missed_deadline = (i % 19u == 0u) ? 1u : 0u;
                A840KmdStatus st = a840_gmu_dcvs_record_frame(&gov, &s);
                CHECK(st == A840_KMD_OK || st == A840_KMD_IO_ERROR);
            }
        });

        /* T3: Ring 0 burst & touch event injector */
        threads.emplace_back([&]() {
            for (uint32_t i = 0u; i < iters; ++i) {
                uint64_t now = shared_clock.load(std::memory_order_relaxed) + 1u * kNsPerMs;
                if ((i & 1u) == 0u) {
                    (void)a840_gmu_dcvs_notify_touch(&gov, now, 40u * kNsPerMs);
                } else {
                    (void)a840_gmu_dcvs_on_ring0_submit(&gov, now, (i % 7u), (i % 9u == 0u) ? 1u : 0u);
                }
            }
        });

        /* T4: Thermal sensor & cold lease manager */
        threads.emplace_back([&]() {
            for (uint32_t i = 0u; i < iters; ++i) {
                uint64_t now = shared_clock.load(std::memory_order_relaxed) + 500u * kNsPerUs;
                int32_t temp = 42000 + static_cast<int32_t>((i % 12u) * 5000); /* 42 C .. 97 C */
                (void)a840_gmu_dcvs_update_thermal(&gov, temp, now);
                if (temp <= 48000) {
                    (void)a840_gmu_dcvs_request_lease(&gov, A840_GPU_OPP_LEASE_LOW_IDX, now, 30u * kNsPerMs);
                } else if (i % 11u == 0u) {
                    (void)a840_gmu_dcvs_release_lease(&gov, now);
                }
            }
        });

        /* T5: Periodic DCVS tick & HFI fault/recovery injector */
        threads.emplace_back([&]() {
            for (uint32_t i = 0u; i < iters; ++i) {
                uint64_t now = shared_clock.load(std::memory_order_relaxed) + 4u * kNsPerMs;
                (void)a840_gmu_dcvs_tick(&gov, now);
                if (i % 17u == 0u) {
                    (void)a840_gmu_dcvs_on_hfi_error(&gov, 0u, A840_KMD_IO_ERROR, now);
                    (void)a840_gmu_dcvs_on_hfi_recovered(&gov, now + 1u * kNsPerMs);
                }
            }
        });

        /* T6: Concurrent snapshot & deadline reader */
        threads.emplace_back([&]() {
            for (uint32_t i = 0u; i < iters; ++i) {
                uint64_t now = shared_clock.load(std::memory_order_relaxed) + 2500u * kNsPerUs;
                A840GmuDcvsSnapshot gsnap{};
                CHECK(a840_gmu_dcvs_get_snapshot(&gov, &gsnap) == A840_KMD_OK);
                CHECK(gsnap.confirmed_gpu_level < A840_GPU_OPP_COUNT);
                CHECK(gsnap.desired_gpu_level < A840_GPU_OPP_COUNT);
                CHECK(gsnap.confirmed_gmu_level < A840_GMU_OPP_COUNT);

                A840MdssSyncStats mst{};
                CHECK(a840_mdss_sync_get_stats(&sync, &mst) == A840_KMD_OK);
                CHECK(mst.frames_on_time + mst.frames_missed_vsync == mst.frames_planned);

                A840MdssDeadlineInfo dl{};
                CHECK(a840_mdss_sync_query_deadline(&sync, now, &dl) == A840_KMD_OK);
                CHECK(dl.tracked_period_ns > 0u);
            }
        });

        /* T7: Threshold adjuster & direct MDSS hint stressor */
        threads.emplace_back([&]() {
            for (uint32_t i = 0u; i < iters; ++i) {
                uint64_t now = shared_clock.load(std::memory_order_relaxed) + 3500u * kNsPerUs;
                uint64_t lead = (i & 1u) ? (750u * kNsPerUs) : (850u * kNsPerUs);
                CHECK(a840_mdss_sync_set_thresholds(&sync, lead, 1500u * kNsPerUs, 2500u * kNsPerUs) == A840_KMD_OK);
                A840MdssBoostHint h{};
                h.frame_id = i;
                h.now_ns = now;
                h.severity = (i % 3u == 0u) ? A840_MDSS_BOOST_LOW_MARGIN : A840_MDSS_BOOST_NONE;
                h.release_pressure = (h.severity == A840_MDSS_BOOST_NONE) ? 1u : 0u;
                (void)a840_gmu_dcvs_apply_mdss_hint(&gov, &h);
            }
        });

        for (auto &th : threads)
            th.join();

        A840GmuDcvsSnapshot gsnap{};
        CHECK(a840_gmu_dcvs_get_snapshot(&gov, &gsnap) == A840_KMD_OK);
        CHECK(gsnap.total_frames_recorded == iters);
        CHECK(gsnap.confirmed_gpu_level < A840_GPU_OPP_COUNT);

        A840MdssSyncStats mst{};
        CHECK(a840_mdss_sync_get_stats(&sync, &mst) == A840_KMD_OK);
        CHECK(mst.frames_planned == iters);
        CHECK(mst.frames_on_time + mst.frames_missed_vsync == iters);

        a840_mdss_sync_destroy(&sync);
        a840_gmu_dcvs_destroy(&gov);
    }
}

} /* namespace */

int main(int argc, char **argv)
{
    unsigned stress_rounds = 1u;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tsan-stress") == 0 && i + 1 < argc) {
            long val = std::strtol(argv[++i], nullptr, 10);
            if (val > 0 && val <= 1000)
                stress_rounds = static_cast<unsigned>(val);
        }
    }

    run_case("01_sm8850_18_gpu_opp_table_exact_values", test_01_sm8850_18_gpu_opp_table_exact_values);
    run_case("02_sm8850_4_gmu_opp_table_exact_values", test_02_sm8850_4_gmu_opp_table_exact_values);
    run_case("03_opp_table_validator_rejects_corrupt_tables", test_03_opp_table_validator_rejects_corrupt_tables);
    run_case("04_find_level_for_freq_and_hw_reg_map", test_04_find_level_for_freq_and_hw_reg_map);
    run_case("05_dcvs_init_defaults_and_bad_config_rejection", test_05_dcvs_init_defaults_and_bad_config_rejection);
    run_case("06_dcvs_uninitialized_and_null_args_fail_closed", test_06_dcvs_uninitialized_and_null_args_fail_closed);
    run_case("07_dcvs_gmu_scales_with_gpu_and_ring0_pressure", test_07_dcvs_gmu_scales_with_gpu_and_ring0_pressure);
    run_case("08_dcvs_wakeup_burst_max_2x_cur_or_578mhz", test_08_dcvs_wakeup_burst_max_2x_cur_or_578mhz);
    run_case("09_anti_oscillation_vs_naive_simple_ondemand", test_09_anti_oscillation_vs_naive_simple_ondemand);
    run_case("10_120hz_240_frames_2s_liquid_glass_zero_interframe_drop", test_10_120hz_240_frames_2s_liquid_glass_zero_interframe_drop);
    run_case("11_inter_frame_gap_under_16_7ms_never_drops_to_idle", test_11_inter_frame_gap_under_16_7ms_never_drops_to_idle);
    run_case("12_true_idle_80ms_smooth_stairstep_decay_to_222mhz", test_12_true_idle_80ms_smooth_stairstep_decay_to_222mhz);
    run_case("13_frame_util_upscale_fast_vs_downscale_hysteresis_and_dwell", test_13_frame_util_upscale_fast_vs_downscale_hysteresis_and_dwell);
    run_case("14_oscillation_10s_budget_throttles_downscale_but_never_blocks_upscale", test_14_oscillation_10s_budget_throttles_downscale_but_never_blocks_upscale);
    run_case("15_ring0_density_and_urgent_120hz_boost", test_15_ring0_density_and_urgent_120hz_boost);
    run_case("16_frame_sample_reserved_nonzero_rejected", test_16_frame_sample_reserved_nonzero_rejected);
    run_case("17_frame_sample_low_margin_and_missed_deadline_inline_upscale", test_17_frame_sample_low_margin_and_missed_deadline_inline_upscale);
    run_case("18_active_120hz_low_util_never_drops_below_active_non_idle_floor", test_18_active_120hz_low_util_never_drops_below_active_non_idle_floor);
    run_case("19_touch_event_578mhz_floor_and_expiry", test_19_touch_event_578mhz_floor_and_expiry);
    run_case("20_touch_extends_expiry_monotonically_and_default_duration", test_20_touch_extends_expiry_monotonically_and_default_duration);
    run_case("21_lease_726mhz_granted_with_fresh_cold_sensor", test_21_lease_726mhz_granted_with_fresh_cold_sensor);
    run_case("22_lease_902mhz_granted_with_fresh_cold_sensor", test_22_lease_902mhz_granted_with_fresh_cold_sensor);
    run_case("23_lease_fail_closed_when_no_thermal_sample", test_23_lease_fail_closed_when_no_thermal_sample);
    run_case("24_lease_fail_closed_when_sensor_stale_on_request_and_tick", test_24_lease_fail_closed_when_sensor_stale_on_request_and_tick);
    run_case("25_lease_fail_closed_when_temperature_too_warm", test_25_lease_fail_closed_when_temperature_too_warm);
    run_case("26_lease_invalid_level_and_zero_duration_rejected_and_manual_release", test_26_lease_invalid_level_and_zero_duration_rejected_and_manual_release);
    run_case("27_thermal_5_tiers_exact_hard_caps_75_to_95c", test_27_thermal_5_tiers_exact_hard_caps_75_to_95c);
    run_case("28_thermal_hard_cap_overrides_all_boosts_touch_lease_mdss_ring0", test_28_thermal_hard_cap_overrides_all_boosts_touch_lease_mdss_ring0);
    run_case("29_thermal_release_hysteresis_prevents_boundary_pingpong", test_29_thermal_release_hysteresis_prevents_boundary_pingpong);
    run_case("30_thermal_multi_tier_cooling_stepdown", test_30_thermal_multi_tier_cooling_stepdown);
    run_case("31_hfi_sync_send_error_enters_degraded_preserves_confirmed_level", test_31_hfi_sync_send_error_enters_degraded_preserves_confirmed_level);
    run_case("32_hfi_degraded_suppresses_spam_and_recovery_reapplies_latest_target", test_32_hfi_degraded_suppresses_spam_and_recovery_reapplies_latest_target);
    run_case("33_hfi_async_ack_normal_and_wrong_seq_rejection", test_33_hfi_async_ack_normal_and_wrong_seq_rejection);
    run_case("34_hfi_async_timeout_via_tick_enters_degraded_and_recovers", test_34_hfi_async_timeout_via_tick_enters_degraded_and_recovers);
    run_case("35_hfi_explicit_on_hfi_error_and_invalid_recovery_state_conflict", test_35_hfi_explicit_on_hfi_error_and_invalid_recovery_state_conflict);
    run_case("36_dcvs_all_callbacks_strictly_unlocked_and_reentrant", test_36_dcvs_all_callbacks_strictly_unlocked_and_reentrant);
    run_case("37_dcvs_recovery_respects_thermal_cap_changed_during_outage", test_37_dcvs_recovery_respects_thermal_cap_changed_during_outage);
    run_case("38_mdss_init_defaults_and_bad_config_rejection", test_38_mdss_init_defaults_and_bad_config_rejection);
    run_case("39_mdss_uninitialized_and_unlocked_phase_fail_closed", test_39_mdss_uninitialized_and_unlocked_phase_fail_closed);
    run_case("40_mdss_vsync_120hz_phase_lock_and_deadline_math", test_40_mdss_vsync_120hz_phase_lock_and_deadline_math);
    run_case("41_mdss_pll_tracks_small_drift_and_rejects_outlier_gaps", test_41_mdss_pll_tracks_small_drift_and_rejects_outlier_gaps);
    run_case("42_mdss_zero_extra_vsync_waits_early_writer_promise", test_42_mdss_zero_extra_vsync_waits_early_writer_promise);
    run_case("43_mdss_zero_extra_vsync_waits_retired_ready_at_exact_cutoff", test_43_mdss_zero_extra_vsync_waits_retired_ready_at_exact_cutoff);
    run_case("44_mdss_past_cutoff_defers_cleanly_no_tear_no_drop", test_44_mdss_past_cutoff_defers_cleanly_no_tear_no_drop);
    run_case("45_mdss_heavy_gpu_overshoot_defers_multiple_vsyncs", test_45_mdss_heavy_gpu_overshoot_defers_multiple_vsyncs);
    run_case("46_mdss_low_margin_under_1_5ms_emits_boost_hint", test_46_mdss_low_margin_under_1_5ms_emits_boost_hint);
    run_case("47_mdss_consecutive_misses_escalate_boost_severity", test_47_mdss_consecutive_misses_escalate_boost_severity);
    run_case("48_mdss_hysteresis_band_and_comfortable_release_after_n_frames", test_48_mdss_hysteresis_band_and_comfortable_release_after_n_frames);
    run_case("49_mdss_runtime_threshold_updates_and_reset_stats", test_49_mdss_runtime_threshold_updates_and_reset_stats);
    run_case("50_mdss_callbacks_strictly_unlocked_and_reentrant", test_50_mdss_callbacks_strictly_unlocked_and_reentrant);
    run_case("51_closed_loop_mdss_low_margin_triggers_gmu_upscale", test_51_closed_loop_mdss_low_margin_triggers_gmu_upscale);
    run_case("52_closed_loop_missed_vsync_escalates_then_recovers_and_releases", test_52_closed_loop_missed_vsync_escalates_then_recovers_and_releases);
    run_case("53_closed_loop_thermal_cap_constrains_mdss_emergency_boost", test_53_closed_loop_thermal_cap_constrains_mdss_emergency_boost);
    run_case("54_closed_loop_null_bridge_safe_and_urgent_ring0_flag", test_54_closed_loop_null_bridge_safe_and_urgent_ring0_flag);
    run_case("55_120hz_full_240_frame_closed_loop_liquid_glass_scroll_proof", test_55_120hz_full_240_frame_closed_loop_liquid_glass_scroll_proof);
    run_case("56_mdss_stats_min_max_avg_margin_accuracy", test_56_mdss_stats_min_max_avg_margin_accuracy);
    run_case("57_dcvs_and_mdss_struct_zero_heap_stack_lifetime", test_57_dcvs_and_mdss_struct_zero_heap_stack_lifetime);
    run_case("58_tsan_8_thread_concurrent_stress", [&]() {
        test_58_tsan_8_thread_concurrent_stress(stress_rounds);
    });

    std::printf("PASS test_kmd_phase5_gmu_mdss: %u independent cases, %" PRIu64 " checks (stress_rounds=%u)\n",
                g_cases_passed, g_checks.load(), stress_rounds);
    return EXIT_SUCCESS;
}
