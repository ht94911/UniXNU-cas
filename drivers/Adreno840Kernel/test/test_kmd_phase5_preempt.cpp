/* SPDX-License-Identifier: MIT */
/* Comprehensive C++17 host test suite for Wave 9 / Lane 1:
 *   - Adreno 840 4-Ring Hardware Preemption Scheduler (a840_kmd_preempt_ring)
 *   - Low-Latency Interrupt Wakeup & Nanosecond Percentile Engine (a840_kmd_wakeup_latency)
 *
 * >=55 deterministic independent test cases + --tsan-stress N (8-thread
 * concurrent submit / preempt / IRQ / wakeup / fault-recovery stress).
 * Zero sleep-based timing flakiness (100% injected monotonic now_ns clock).
 */

#include "a840_kmd_preempt_ring.h"
#include "a840_kmd_wakeup_latency.h"

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

/* Custom lock tracker to strictly verify lock balance and lock-free callbacks. */
struct TrackedLock {
    std::mutex mu;
    std::atomic<uint64_t> lock_count{0};
    std::atomic<uint64_t> unlock_count{0};
    std::atomic<int> depth{0};
    std::atomic<uint64_t> injected_now_ns{1000000u};

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

    static uint64_t get_now(void *c)
    {
        auto *self = static_cast<TrackedLock *>(c);
        return self->injected_now_ns.load(std::memory_order_relaxed);
    }

    A840PreemptSyncOps preempt_ops()
    {
        A840PreemptSyncOps ops{};
        ops.cookie = this;
        ops.lock = &TrackedLock::do_lock;
        ops.unlock = &TrackedLock::do_unlock;
        return ops;
    }

    A840WakeupSyncOps wakeup_ops()
    {
        A840WakeupSyncOps ops{};
        ops.cookie = this;
        ops.lock = &TrackedLock::do_lock;
        ops.unlock = &TrackedLock::do_unlock;
        ops.now_ns = &TrackedLock::get_now;
        return ops;
    }
};

struct MockHwRecorder {
    TrackedLock *lock_tracker = nullptr;
    A840KmdPreemptScheduler *reenter_sched = nullptr;
    A840KmdWakeupEngine *wakeup_bridge = nullptr;

    std::mutex mu;
    uint32_t doorbell_calls = 0;
    uint32_t last_doorbell_ring = A840_RING_NONE;
    uint32_t last_doorbell_wptr = 0;
    uint64_t last_doorbell_seqno = 0;
    uint32_t last_doorbell_reg = 0;

    uint32_t trigger_calls = 0;
    uint32_t last_from_ring = A840_RING_NONE;
    uint32_t last_to_ring = A840_RING_NONE;
    uint32_t last_level = 0;
    uint64_t last_record_va = 0;
    uint64_t last_record_bytes = 0;
    uint32_t last_ctx_reg = 0;

    uint32_t retire_calls = 0;
    uint32_t pi_calls = 0;
    uint32_t last_pi_ring = A840_RING_NONE;
    uint32_t last_pi_boosted = 0;
    uint32_t last_pi_by = A840_RING_NONE;
    uint64_t last_pi_until = 0;

    uint32_t timeout_calls = 0;
    uint64_t last_timeout_elapsed = 0;

    uint32_t latency_calls = 0;
    uint32_t last_lat_kind = 0;
    uint64_t last_lat_ns = 0;

    uint32_t callback_saw_lock_held = 0;
    uint32_t reenter_ok_calls = 0;
    A840KmdStatus fail_doorbell_status = A840_KMD_OK;
    A840KmdStatus fail_trigger_status = A840_KMD_OK;

    void check_unlocked()
    {
        if (lock_tracker != nullptr &&
            lock_tracker->depth.load(std::memory_order_relaxed) != 0) {
            ++callback_saw_lock_held;
        }
        if (reenter_sched != nullptr) {
            A840PreemptSnapshot snap{};
            if (a840_kmd_preempt_get_snapshot(reenter_sched, &snap) == A840_KMD_OK)
                ++reenter_ok_calls;
        }
    }

    static A840KmdStatus on_doorbell(void *c, uint32_t ring_id, uint32_t wptr,
                                     uint64_t seqno, uint32_t reg_offset)
    {
        auto *self = static_cast<MockHwRecorder *>(c);
        self->check_unlocked();
        std::lock_guard<std::mutex> g(self->mu);
        ++self->doorbell_calls;
        self->last_doorbell_ring = ring_id;
        self->last_doorbell_wptr = wptr;
        self->last_doorbell_seqno = seqno;
        self->last_doorbell_reg = reg_offset;
        return self->fail_doorbell_status;
    }

    static A840KmdStatus on_trigger(void *c, uint32_t from_ring, uint32_t to_ring,
                                    uint32_t level, uint64_t rec_va,
                                    uint64_t rec_bytes, uint32_t reg_cntl)
    {
        auto *self = static_cast<MockHwRecorder *>(c);
        self->check_unlocked();
        std::lock_guard<std::mutex> g(self->mu);
        ++self->trigger_calls;
        self->last_from_ring = from_ring;
        self->last_to_ring = to_ring;
        self->last_level = level;
        self->last_record_va = rec_va;
        self->last_record_bytes = rec_bytes;
        self->last_ctx_reg = reg_cntl;
        return self->fail_trigger_status;
    }

    static void on_retire(void *c, uint32_t ring_id, uint64_t completed_seqno,
                          uint64_t now_ns)
    {
        auto *self = static_cast<MockHwRecorder *>(c);
        self->check_unlocked();
        {
            std::lock_guard<std::mutex> g(self->mu);
            ++self->retire_calls;
        }
        if (self->wakeup_bridge != nullptr) {
            (void)a840_kmd_wakeup_on_irq(self->wakeup_bridge, ring_id,
                                         completed_seqno, now_ns, nullptr);
        }
    }

    static void on_pi(void *c, uint32_t ring_id, uint32_t boosted,
                      uint32_t boosted_by_ring, uint64_t boost_until_seqno,
                      uint64_t now_ns)
    {
        auto *self = static_cast<MockHwRecorder *>(c);
        self->check_unlocked();
        {
            std::lock_guard<std::mutex> g(self->mu);
            ++self->pi_calls;
            self->last_pi_ring = ring_id;
            self->last_pi_boosted = boosted;
            self->last_pi_by = boosted_by_ring;
            self->last_pi_until = boost_until_seqno;
        }
        if (self->wakeup_bridge != nullptr) {
            a840_kmd_wakeup_bridge_pi_boost(self->wakeup_bridge, ring_id, boosted,
                                            boosted_by_ring, boost_until_seqno, now_ns);
        }
    }

    static void on_timeout(void *c, uint32_t from_ring, uint32_t to_ring,
                           uint64_t elapsed_ns)
    {
        (void)from_ring;
        (void)to_ring;
        auto *self = static_cast<MockHwRecorder *>(c);
        self->check_unlocked();
        std::lock_guard<std::mutex> g(self->mu);
        ++self->timeout_calls;
        self->last_timeout_elapsed = elapsed_ns;
    }

    static void on_latency(void *c, uint32_t kind, uint32_t ring_id,
                           uint64_t latency_ns, uint64_t now_ns)
    {
        auto *self = static_cast<MockHwRecorder *>(c);
        self->check_unlocked();
        {
            std::lock_guard<std::mutex> g(self->mu);
            ++self->latency_calls;
            self->last_lat_kind = kind;
            self->last_lat_ns = latency_ns;
        }
        if (self->wakeup_bridge != nullptr) {
            a840_kmd_wakeup_bridge_latency(self->wakeup_bridge, kind, ring_id,
                                           latency_ns, now_ns);
        }
    }

    A840PreemptHwOps ops()
    {
        A840PreemptHwOps o{};
        o.cookie = this;
        o.write_doorbell = &MockHwRecorder::on_doorbell;
        o.trigger_preempt = &MockHwRecorder::on_trigger;
        o.on_retire = &MockHwRecorder::on_retire;
        o.on_pi_boost_change = &MockHwRecorder::on_pi;
        o.on_timeout_recovery = &MockHwRecorder::on_timeout;
        o.on_latency_sample = &MockHwRecorder::on_latency;
        return o;
    }
};

struct WakeEventLog {
    TrackedLock *lock_tracker = nullptr;
    A840KmdWakeupEngine *reenter_engine = nullptr;
    std::mutex mu;
    uint32_t wake_count = 0;
    uint32_t last_ring = A840_RING_NONE;
    uint64_t last_handle = 0;
    uint64_t last_target = 0;
    uint64_t last_completed = 0;
    uint32_t last_reason = 999u;
    uint64_t last_lat_ns = 0;
    uint32_t alert_count = 0;
    uint32_t last_alert_metric = 999u;
    uint64_t last_alert_lat_ns = 0;
    uint32_t saw_lock_held = 0;
    uint32_t reenter_ok = 0;

    static void on_wake(void *c, uint64_t handle, uint32_t ring_id,
                        uint64_t target_seqno, uint64_t completed_seqno,
                        uint32_t reason, uint64_t irq_to_wake_ns)
    {
        auto *self = static_cast<WakeEventLog *>(c);
        if (self->lock_tracker != nullptr &&
            self->lock_tracker->depth.load(std::memory_order_relaxed) != 0) {
            ++self->saw_lock_held;
        }
        if (self->reenter_engine != nullptr) {
            A840WakeupSnapshot snap{};
            if (a840_kmd_wakeup_get_snapshot(self->reenter_engine, &snap) == A840_KMD_OK)
                ++self->reenter_ok;
        }
        std::lock_guard<std::mutex> g(self->mu);
        ++self->wake_count;
        self->last_handle = handle;
        self->last_ring = ring_id;
        self->last_target = target_seqno;
        self->last_completed = completed_seqno;
        self->last_reason = reason;
        self->last_lat_ns = irq_to_wake_ns;
    }

    static void on_alert(void *c, uint32_t metric_kind, uint32_t ring_id,
                         uint64_t latency_ns, uint64_t threshold_ns,
                         uint64_t now_ns)
    {
        (void)ring_id;
        (void)threshold_ns;
        (void)now_ns;
        auto *self = static_cast<WakeEventLog *>(c);
        if (self->lock_tracker != nullptr &&
            self->lock_tracker->depth.load(std::memory_order_relaxed) != 0) {
            ++self->saw_lock_held;
        }
        std::lock_guard<std::mutex> g(self->mu);
        ++self->alert_count;
        self->last_alert_metric = metric_kind;
        self->last_alert_lat_ns = latency_ns;
    }
};

A840PreemptConfig make_default_preempt_config(const A840PreemptHwOps *hw_ops = nullptr,
                                              const A840PreemptSyncOps *sync_ops = nullptr)
{
    A840PreemptConfig cfg{};
    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        cfg.rings[r].preempt_record_va = UINT64_C(0x10000000) + static_cast<uint64_t>(r) * UINT64_C(0x100000);
        cfg.rings[r].preempt_record_bytes = A840_PREEMPT_RECORD_MIN_BYTES;
        cfg.rings[r].rb_base_va = UINT64_C(0x20000000) + static_cast<uint64_t>(r) * UINT64_C(0x10000);
        cfg.rings[r].slot_capacity = 64u;
        cfg.rings[r].preempt_level = (r == A840_RING_2_CLIENT_3D)
                                         ? A840_PREEMPT_LEVEL_L1B_DRAW_BOUNDARY
                                         : A840_PREEMPT_LEVEL_L1A_BIN_BOUNDARY;
    }
    cfg.min_slice_ns = A840_PREEMPT_DEFAULT_MIN_SLICE_NS;
    cfg.preempt_timeout_ns = A840_PREEMPT_DEFAULT_TIMEOUT_NS;
    cfg.frame_budget_ns = A840_PREEMPT_FRAME_BUDGET_120HZ_NS;
    cfg.deadline_urgent_ns = A840_PREEMPT_DEFAULT_URGENT_BUDGET_NS;
    cfg.sync_ops = sync_ops;
    cfg.hw_ops = hw_ops;
    return cfg;
}

A840PreemptSubmitReq make_req(uint32_t ring_id, uint64_t enqueue_ns,
                              uint64_t deadline_ns = 0u,
                              uint32_t has_dep = 0u, uint32_t dep_ring = 0u,
                              uint64_t dep_seqno = 0u)
{
    A840PreemptSubmitReq req{};
    req.ring_id = ring_id;
    req.ib_va = UINT64_C(0x40000000) + static_cast<uint64_t>(ring_id) * 4096u;
    req.ib_dwords = 64u;
    req.enqueue_ns = enqueue_ns;
    req.frame_deadline_ns = deadline_ns;
    req.has_dependency = has_dep;
    req.dep_ring = dep_ring;
    req.dep_seqno = dep_seqno;
    return req;
}

A840KmdStatus submit_req(A840KmdPreemptScheduler *sched,
                         const A840PreemptSubmitReq &req,
                         A840PreemptSubmitResult *out_res = nullptr)
{
    return a840_kmd_preempt_submit(sched, &req, out_res);
}

/* ======================== GROUP 1: HW Regs, Mapping & Validation ======================== */

void test_01_hw_reg_map_symbols()
{
    A840PreemptHwRegMap map{};
    CHECK(a840_kmd_preempt_get_hw_reg_map(nullptr) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_preempt_get_hw_reg_map(&map) == A840_KMD_OK);
    CHECK(map.rb_base_lo == A840_REG_CP_RB_BASE);
    CHECK(map.rb_wptr == A840_REG_CP_RB_WPTR);
    CHECK(map.rb_rptr == A840_REG_CP_RB_RPTR);
    CHECK(map.context_switch_cntl == A840_REG_CP_CONTEXT_SWITCH_CNTL);
    CHECK(map.context_switch_level_status == A840_REG_CP_CONTEXT_SWITCH_LEVEL_STATUS);
    CHECK(map.smmu_info_lo == A840_REG_CP_CONTEXT_SWITCH_SMMU_INFO);
    CHECK(map.preempt_threshold == A840_REG_CP_PREEMPT_THRESHOLD);
    CHECK(map.rbbm_int_0_status == A840_REG_RBBM_INT_0_STATUS);
    CHECK(map.always_on_counter_lo == A840_REG_CP_ALWAYS_ON_COUNTER);
    CHECK(map.cp_in_preempt_opcode == A840_CP_IN_PREEMPT);
    CHECK(map.abi_preempt_cap_flag == NativeAdrenoParamsV3PreemptRings);
}

void test_02_priority_to_ring_mapping()
{
    uint32_t rid = 99u;
    CHECK(a840_kmd_preempt_priority_to_ring(A840_PRIORITY_COMPOSITOR, nullptr) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_preempt_priority_to_ring(A840_PRIORITY_COMPOSITOR, &rid) == A840_KMD_OK);
    CHECK(rid == A840_RING_0_COMPOSITOR);
    CHECK(a840_kmd_preempt_priority_to_ring(A840_PRIORITY_HIGH, &rid) == A840_KMD_OK);
    CHECK(rid == A840_RING_1_INTERACTIVE);
    CHECK(a840_kmd_preempt_priority_to_ring(A840_PRIORITY_NORMAL, &rid) == A840_KMD_OK);
    CHECK(rid == A840_RING_2_CLIENT_3D);
    CHECK(a840_kmd_preempt_priority_to_ring(A840_PRIORITY_BACKGROUND, &rid) == A840_KMD_OK);
    CHECK(rid == A840_RING_3_BG_COMPUTE);
    CHECK(a840_kmd_preempt_priority_to_ring(A840_PRIORITY_COUNT, &rid) == A840_KMD_RANGE);
    CHECK(a840_kmd_preempt_priority_to_ring(999u, &rid) == A840_KMD_RANGE);
}

void test_03_ring_to_priority_mapping()
{
    uint32_t prio = 99u;
    CHECK(a840_kmd_preempt_ring_to_priority(A840_RING_0_COMPOSITOR, nullptr) == A840_KMD_INVALID_ARG);
    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        CHECK(a840_kmd_preempt_ring_to_priority(r, &prio) == A840_KMD_OK);
        CHECK(prio == r);
    }
    CHECK(a840_kmd_preempt_ring_to_priority(A840_RING_COUNT, &prio) == A840_KMD_RANGE);
}

void test_04_record_validation_alignment_and_size()
{
    CHECK(a840_kmd_preempt_validate_record(0u, 65536u) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_preempt_validate_record(0x10000u, 0u) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_preempt_validate_record(0x10001u, 65536u) == A840_KMD_ALIGNMENT);
    CHECK(a840_kmd_preempt_validate_record(0x10000u, 65536u + 64u) == A840_KMD_ALIGNMENT);
    CHECK(a840_kmd_preempt_validate_record(0x10000u, 4096u) == A840_KMD_RANGE);
    CHECK(a840_kmd_preempt_validate_record(UINT64_C(1) << 49, 65536u) == A840_KMD_RANGE);
    CHECK(a840_kmd_preempt_validate_record(UINT64_C(0x10000000), 65536u) == A840_KMD_OK);
}

void test_05_init_rejects_bad_configs_and_overlapping_records()
{
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(nullptr, nullptr) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_preempt_init(&sched, nullptr) == A840_KMD_INVALID_ARG);

    A840PreemptConfig cfg = make_default_preempt_config();
    cfg.rings[1].preempt_record_va += 128u; /* unaligned */
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_ALIGNMENT);

    cfg = make_default_preempt_config();
    cfg.rings[2].preempt_record_va = cfg.rings[1].preempt_record_va + 4096u; /* overlaps ring 1 */
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_STATE_CONFLICT);

    cfg = make_default_preempt_config();
    cfg.rings[0].slot_capacity = 30u; /* not power-of-2 */
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_RANGE);

    cfg = make_default_preempt_config();
    cfg.rings[0].preempt_level = 99u;
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_INVALID_ARG);

    cfg = make_default_preempt_config();
    cfg.rings[3].rb_base_va = 0x1004u; /* unaligned */
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_ALIGNMENT);

    A840PreemptSyncOps bad_sync{};
    cfg = make_default_preempt_config(nullptr, &bad_sync);
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_INVALID_ARG);
}

void test_06_uninitialized_scheduler_fail_closed()
{
    A840KmdPreemptScheduler sched{};
    A840PreemptSnapshot snap{};
    A840PreemptSubmitReq req = make_req(0u, 1000u);
    CHECK(a840_kmd_preempt_submit(&sched, &req, nullptr) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_preempt_on_hw_in_progress(&sched, 1000u) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_preempt_on_switch_done(&sched, 1000u) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_preempt_mark_ring_faulted(&sched, 0u, 1000u) == A840_KMD_STATE_CONFLICT);
    a840_kmd_preempt_destroy(&sched);
}

void test_07_basic_single_ring_submit_and_retire()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    A840PreemptSubmitResult res{};
    A840PreemptSubmitReq req = make_req(A840_RING_2_CLIENT_3D, 10000u);
    CHECK(a840_kmd_preempt_submit(&sched, &req, &res) == A840_KMD_OK);
    CHECK(res.seqno == 1u && res.wptr == 1u && res.doorbell_emitted == 1u && res.preempt_triggered == 0u);
    CHECK(hw.doorbell_calls == 1u && hw.last_doorbell_ring == A840_RING_2_CLIENT_3D);
    CHECK(hw.last_doorbell_reg == A840_REG_CP_RB_WPTR);

    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_2_CLIENT_3D, 1u, 12000u) == A840_KMD_OK);
    A840PreemptSnapshot snap{};
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.active_ring == A840_RING_NONE);
    CHECK(snap.rings[A840_RING_2_CLIENT_3D].inflight_count == 0u);
    CHECK(snap.rings[A840_RING_2_CLIENT_3D].completed_seqno == 1u);
    CHECK(snap.total_retires == 1u);

    a840_kmd_preempt_destroy(&sched);
}

void test_08_submit_priority_wrapper_and_validation()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    A840PreemptSubmitReq tmpl = make_req(99u, 5000u);
    A840PreemptSubmitResult res{};
    CHECK(a840_kmd_preempt_submit_priority(&sched, A840_PRIORITY_COMPOSITOR, &tmpl, &res) == A840_KMD_OK);
    CHECK(res.seqno == 1u && hw.last_doorbell_ring == A840_RING_0_COMPOSITOR);
    CHECK(a840_kmd_preempt_submit_priority(&sched, 99u, &tmpl, &res) == A840_KMD_RANGE);
    CHECK(a840_kmd_preempt_submit_priority(&sched, A840_PRIORITY_HIGH, nullptr, &res) == A840_KMD_INVALID_ARG);

    A840PreemptSubmitReq bad_ib = make_req(A840_RING_1_INTERACTIVE, 6000u);
    bad_ib.ib_va = 0x1002u; /* misaligned */
    CHECK(a840_kmd_preempt_submit(&sched, &bad_ib, nullptr) == A840_KMD_ALIGNMENT);

    a840_kmd_preempt_destroy(&sched);
}

void test_09_ring_capacity_wraparound_and_full_rejection()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    cfg.rings[A840_RING_3_BG_COMPUTE].slot_capacity = 4u;
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    for (uint32_t i = 0u; i < 4u; ++i) {
        A840PreemptSubmitReq req = make_req(A840_RING_3_BG_COMPUTE, 1000u + i * 100u);
        CHECK(a840_kmd_preempt_submit(&sched, &req, nullptr) == A840_KMD_OK);
    }
    A840PreemptSubmitReq overflow = make_req(A840_RING_3_BG_COMPUTE, 2000u);
    CHECK(a840_kmd_preempt_submit(&sched, &overflow, nullptr) == A840_KMD_CAPACITY);

    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_3_BG_COMPUTE, 2u, 2500u) == A840_KMD_OK);
    for (uint32_t i = 0u; i < 2u; ++i) {
        A840PreemptSubmitReq req = make_req(A840_RING_3_BG_COMPUTE, 3000u + i * 100u);
        CHECK(a840_kmd_preempt_submit(&sched, &req, nullptr) == A840_KMD_OK);
    }
    A840PreemptSnapshot snap{};
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.rings[A840_RING_3_BG_COMPUTE].inflight_count == 4u);
    CHECK(snap.rings[A840_RING_3_BG_COMPUTE].submitted_seqno == 6u);

    a840_kmd_preempt_destroy(&sched);
}

void test_10_retire_range_and_regression_errors()
{
    A840PreemptConfig cfg = make_default_preempt_config();
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    A840PreemptSubmitReq req = make_req(A840_RING_0_COMPOSITOR, 1000u);
    CHECK(a840_kmd_preempt_submit(&sched, &req, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_submit(&sched, &req, nullptr) == A840_KMD_OK);

    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_COUNT, 1u, 2000u) == A840_KMD_RANGE);
    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_0_COMPOSITOR, 3u, 2000u) == A840_KMD_RANGE);
    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_0_COMPOSITOR, 2u, 2000u) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_0_COMPOSITOR, 1u, 2500u) == A840_KMD_STATE_CONFLICT);

    a840_kmd_preempt_destroy(&sched);
}

/* ======================== GROUP 2: Preemption State Machine & Levels ======================== */

void test_11_ring0_preempts_saturated_ring3_l1a()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    for (uint32_t i = 0u; i < 8u; ++i) {
        A840PreemptSubmitReq r3 = make_req(A840_RING_3_BG_COMPUTE, 10000u + i * 10u);
        CHECK(a840_kmd_preempt_submit(&sched, &r3, nullptr) == A840_KMD_OK);
    }

    /* RING_0 arrives only 100 ns after RING_3 started -> exempt from min_slice_ns! */
    A840PreemptSubmitReq r0 = make_req(A840_RING_0_COMPOSITOR, 10100u, 10100u + A840_PREEMPT_FRAME_BUDGET_120HZ_NS);
    A840PreemptSubmitResult res{};
    CHECK(a840_kmd_preempt_submit(&sched, &r0, &res) == A840_KMD_OK);
    CHECK(res.preempt_triggered == 1u);
    CHECK(res.deadline_exempted == 1u);
    CHECK(res.doorbell_emitted == 0u); /* waits for switch_done */
    CHECK(hw.trigger_calls == 1u);
    CHECK(hw.last_from_ring == A840_RING_3_BG_COMPUTE);
    CHECK(hw.last_to_ring == A840_RING_0_COMPOSITOR);
    CHECK(hw.last_level == A840_PREEMPT_LEVEL_L1A_BIN_BOUNDARY);
    CHECK(hw.last_ctx_reg == A840_REG_CP_CONTEXT_SWITCH_CNTL);

    CHECK(a840_kmd_preempt_on_hw_in_progress(&sched, 10150u) == A840_KMD_OK);
    uint32_t db_before = hw.doorbell_calls;
    CHECK(a840_kmd_preempt_on_switch_done(&sched, 10300u) == A840_KMD_OK);
    CHECK(hw.doorbell_calls == db_before + 1u);
    CHECK(hw.last_doorbell_ring == A840_RING_0_COMPOSITOR);
    CHECK(hw.last_lat_kind == A840_PREEMPT_LAT_TRIGGER_TO_ACTIVE);
    CHECK(hw.last_lat_ns == 200u);

    CHECK(a840_kmd_preempt_ack_complete(&sched) == A840_KMD_OK);
    A840PreemptSnapshot snap{};
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.state == A840_PREEMPT_IDLE);
    CHECK(snap.active_ring == A840_RING_0_COMPOSITOR);

    a840_kmd_preempt_destroy(&sched);
}

void test_12_ring0_preempts_ring2_l1b_draw_boundary()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    A840PreemptSubmitReq r2 = make_req(A840_RING_2_CLIENT_3D, 50000u);
    CHECK(a840_kmd_preempt_submit(&sched, &r2, nullptr) == A840_KMD_OK);

    A840PreemptSubmitReq r0 = make_req(A840_RING_0_COMPOSITOR, 50050u);
    A840PreemptSubmitResult res{};
    CHECK(a840_kmd_preempt_submit(&sched, &r0, &res) == A840_KMD_OK);
    CHECK(res.preempt_triggered == 1u);
    CHECK(hw.last_level == A840_PREEMPT_LEVEL_L1B_DRAW_BOUNDARY);
    CHECK(hw.last_record_va == cfg.rings[A840_RING_0_COMPOSITOR].preempt_record_va);

    CHECK(a840_kmd_preempt_on_hw_in_progress(&sched, 50060u) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_on_switch_done(&sched, 50120u) == A840_KMD_OK);

    /* Retire RING_0 -> automatically resumes RING_2! */
    uint32_t db_before = hw.doorbell_calls;
    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_0_COMPOSITOR, 1u, 51000u) == A840_KMD_OK);
    A840PreemptSnapshot snap{};
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.active_ring == A840_RING_2_CLIENT_3D);
    CHECK(hw.doorbell_calls == db_before); /* RING_2 doorbell already sent earlier */

    a840_kmd_preempt_destroy(&sched);
}

void test_13_set_ring_level_dynamic()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    CHECK(a840_kmd_preempt_set_ring_level(&sched, A840_RING_3_BG_COMPUTE, A840_PREEMPT_LEVEL_L1B_DRAW_BOUNDARY) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_set_ring_level(&sched, A840_RING_COUNT, A840_PREEMPT_LEVEL_L1A_BIN_BOUNDARY) == A840_KMD_RANGE);
    CHECK(a840_kmd_preempt_set_ring_level(&sched, A840_RING_0_COMPOSITOR, 0u) == A840_KMD_INVALID_ARG);

    A840PreemptSubmitReq r3 = make_req(A840_RING_3_BG_COMPUTE, 1000u);
    CHECK(a840_kmd_preempt_submit(&sched, &r3, nullptr) == A840_KMD_OK);
    A840PreemptSubmitReq r0 = make_req(A840_RING_0_COMPOSITOR, 1010u);
    CHECK(a840_kmd_preempt_submit(&sched, &r0, nullptr) == A840_KMD_OK);
    CHECK(hw.last_level == A840_PREEMPT_LEVEL_L1B_DRAW_BOUNDARY);

    a840_kmd_preempt_destroy(&sched);
}

void test_14_illegal_state_transitions_rejected()
{
    A840PreemptConfig cfg = make_default_preempt_config();
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    /* From IDLE: in_progress, switch_done, ack_complete, recover_from_timeout all fail */
    CHECK(a840_kmd_preempt_on_hw_in_progress(&sched, 1000u) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_preempt_on_switch_done(&sched, 1000u) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_preempt_ack_complete(&sched) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_preempt_recover_from_timeout(&sched, 0u, 1000u) == A840_KMD_STATE_CONFLICT);

    /* Move to TRIGGERED */
    A840PreemptSubmitReq r3 = make_req(A840_RING_3_BG_COMPUTE, 1000u);
    A840PreemptSubmitReq r0 = make_req(A840_RING_0_COMPOSITOR, 1010u);
    CHECK(a840_kmd_preempt_submit(&sched, &r3, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_submit(&sched, &r0, nullptr) == A840_KMD_OK);

    /* Cannot skip IN_PROGRESS to switch_done */
    CHECK(a840_kmd_preempt_on_switch_done(&sched, 1050u) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_preempt_on_hw_in_progress(&sched, 1050u) == A840_KMD_OK);
    /* Duplicate in_progress rejected */
    CHECK(a840_kmd_preempt_on_hw_in_progress(&sched, 1060u) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_preempt_on_switch_done(&sched, 1100u) == A840_KMD_OK);
    /* Duplicate switch_done rejected */
    CHECK(a840_kmd_preempt_on_switch_done(&sched, 1110u) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_preempt_ack_complete(&sched) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_ack_complete(&sched) == A840_KMD_STATE_CONFLICT);

    a840_kmd_preempt_destroy(&sched);
}

void test_15_min_slice_anti_thrash_defers_ring1_then_tick_triggers()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    cfg.min_slice_ns = 500000u; /* 500 us */
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    A840PreemptSubmitReq r2 = make_req(A840_RING_2_CLIENT_3D, 1000000u);
    CHECK(a840_kmd_preempt_submit(&sched, &r2, nullptr) == A840_KMD_OK);

    /* RING_1 arrives 100 us later (no urgent deadline) -> deferred by min_slice_ns */
    A840PreemptSubmitReq r1 = make_req(A840_RING_1_INTERACTIVE, 1100000u);
    A840PreemptSubmitResult res{};
    CHECK(a840_kmd_preempt_submit(&sched, &r1, &res) == A840_KMD_OK);
    CHECK(res.preempt_triggered == 0u);
    CHECK(res.preempt_deferred_min_slice == 1u);
    CHECK(hw.trigger_calls == 0u);

    /* Tick at +300 us: still below 500 us */
    CHECK(a840_kmd_preempt_tick(&sched, 1300000u) == A840_KMD_OK);
    CHECK(hw.trigger_calls == 0u);

    /* Tick at +500 us: min slice satisfied -> preemption triggers */
    CHECK(a840_kmd_preempt_tick(&sched, 1500000u) == A840_KMD_OK);
    CHECK(hw.trigger_calls == 1u);
    CHECK(hw.last_from_ring == A840_RING_2_CLIENT_3D);
    CHECK(hw.last_to_ring == A840_RING_1_INTERACTIVE);

    a840_kmd_preempt_destroy(&sched);
}

void test_16_urgent_120hz_deadline_exempts_ring1_from_min_slice()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    cfg.min_slice_ns = 1000000u;
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    A840PreemptSubmitReq r3 = make_req(A840_RING_3_BG_COMPUTE, 1000000u);
    CHECK(a840_kmd_preempt_submit(&sched, &r3, nullptr) == A840_KMD_OK);

    /* RING_1 has tight deadline (1 ms left < 2.5 ms urgent threshold) -> immediate preemption */
    A840PreemptSubmitReq r1 = make_req(A840_RING_1_INTERACTIVE, 1050000u, 2050000u);
    A840PreemptSubmitResult res{};
    CHECK(a840_kmd_preempt_submit(&sched, &r1, &res) == A840_KMD_OK);
    CHECK(res.preempt_triggered == 1u);
    CHECK(res.deadline_exempted == 1u);
    CHECK(hw.trigger_calls == 1u);

    a840_kmd_preempt_destroy(&sched);
}

void test_17_triggered_target_upgraded_when_ring0_arrives()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    cfg.min_slice_ns = 0u;
    cfg.allow_zero_min_slice = 1u;
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    CHECK(submit_req(&sched, make_req(A840_RING_3_BG_COMPUTE, 1000u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_1_INTERACTIVE, 1100u), nullptr) == A840_KMD_OK);
    CHECK(hw.trigger_calls == 1u && hw.last_to_ring == A840_RING_1_INTERACTIVE);

    /* While still TRIGGERED, RING_0 arrives -> target_ring upgrades without double trigger */
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1120u), nullptr) == A840_KMD_OK);
    CHECK(hw.trigger_calls == 1u);

    CHECK(a840_kmd_preempt_on_hw_in_progress(&sched, 1150u) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_on_switch_done(&sched, 1200u) == A840_KMD_OK);

    A840PreemptSnapshot snap{};
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.active_ring == A840_RING_0_COMPOSITOR);

    a840_kmd_preempt_destroy(&sched);
}

void test_18_preempt_timeout_recovery_and_stalled_ring_isolation()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    cfg.preempt_timeout_ns = 2000000u; /* 2 ms */
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    CHECK(submit_req(&sched, make_req(A840_RING_2_CLIENT_3D, 1000000u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1010000u), nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_on_hw_in_progress(&sched, 1020000u) == A840_KMD_OK);

    uint32_t timed_out = 99u;
    CHECK(a840_kmd_preempt_check_timeout(&sched, 2500000u, &timed_out) == A840_KMD_OK);
    CHECK(timed_out == 0u);

    CHECK(a840_kmd_preempt_check_timeout(&sched, 3010000u, &timed_out) == A840_KMD_OK);
    CHECK(timed_out == 1u);
    CHECK(hw.timeout_calls == 1u && hw.last_timeout_elapsed == 2000000u);

    /* Recover and fault the stalled RING_2 -> RING_0 immediately becomes active & rings doorbell! */
    CHECK(a840_kmd_preempt_recover_from_timeout(&sched, 1u, 3050000u) == A840_KMD_OK);
    A840PreemptSnapshot snap{};
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.state == A840_PREEMPT_IDLE);
    CHECK(snap.active_ring == A840_RING_0_COMPOSITOR);
    CHECK(snap.rings[A840_RING_2_CLIENT_3D].health_state == A840_RING_STATE_FAULTED);
    CHECK(hw.last_doorbell_ring == A840_RING_0_COMPOSITOR);

    a840_kmd_preempt_destroy(&sched);
}

void test_19_lower_prio_submission_does_not_preempt_higher_ring()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    cfg.min_slice_ns = 0u;
    cfg.allow_zero_min_slice = 1u;
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1000u), nullptr) == A840_KMD_OK);
    A840PreemptSubmitResult res{};
    CHECK(submit_req(&sched, make_req(A840_RING_3_BG_COMPUTE, 5000u), &res) == A840_KMD_OK);
    CHECK(res.preempt_triggered == 0u && res.doorbell_emitted == 0u);
    CHECK(hw.trigger_calls == 0u);

    /* Once RING_0 retires, RING_3 activates and emits doorbell */
    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_0_COMPOSITOR, 1u, 6000u) == A840_KMD_OK);
    CHECK(hw.last_doorbell_ring == A840_RING_3_BG_COMPUTE);

    a840_kmd_preempt_destroy(&sched);
}

void test_20_hw_error_propagation_from_doorbell_and_trigger()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    hw.fail_doorbell_status = A840_KMD_IO_ERROR;
    CHECK(submit_req(&sched, make_req(A840_RING_3_BG_COMPUTE, 1000u), nullptr) == A840_KMD_IO_ERROR);
    hw.fail_doorbell_status = A840_KMD_OK;

    hw.fail_trigger_status = A840_KMD_IO_ERROR;
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1050u), nullptr) == A840_KMD_IO_ERROR);

    a840_kmd_preempt_destroy(&sched);
}

/* ======================== GROUP 3: Cross-Ring Priority Inheritance & Cycles ======================== */

void test_21_pi_boost_ring0_depends_on_ring2_prevents_inversion()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    /* RING_2 has pending writer_promise work (seqno 1), RING_1 is running */
    CHECK(submit_req(&sched, make_req(A840_RING_1_INTERACTIVE, 10000u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_2_CLIENT_3D, 10100u), nullptr) == A840_KMD_OK);

    /* RING_0 compositor submits work depending on RING_2 seqno 1 -> boosts RING_2 to RING_0! */
    A840PreemptSubmitReq r0 = make_req(A840_RING_0_COMPOSITOR, 10200u, 0u, 1u, A840_RING_2_CLIENT_3D, 1u);
    A840PreemptSubmitResult res{};
    CHECK(a840_kmd_preempt_submit(&sched, &r0, &res) == A840_KMD_OK);
    CHECK(res.pi_boost_applied == 1u);
    CHECK(res.preempt_triggered == 1u);
    CHECK(hw.last_from_ring == A840_RING_1_INTERACTIVE);
    CHECK(hw.last_to_ring == A840_RING_2_CLIENT_3D); /* Boosted RING_2 preempts RING_1! */
    CHECK(hw.last_pi_ring == A840_RING_2_CLIENT_3D && hw.last_pi_boosted == 1u &&
          hw.last_pi_by == A840_RING_0_COMPOSITOR && hw.last_pi_until == 1u);

    A840PreemptSnapshot snap{};
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.rings[A840_RING_2_CLIENT_3D].effective_priority == A840_RING_0_COMPOSITOR);
    CHECK(snap.rings[A840_RING_2_CLIENT_3D].pi_boost_active == 1u);

    /* Switch completes -> RING_2 runs -> RING_2 retires seqno 1 -> PI drops & RING_0 runs! */
    CHECK(a840_kmd_preempt_on_hw_in_progress(&sched, 10250u) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_on_switch_done(&sched, 10300u) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_2_CLIENT_3D, 1u, 10800u) == A840_KMD_OK);

    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.rings[A840_RING_2_CLIENT_3D].pi_boost_active == 0u);
    CHECK(snap.rings[A840_RING_2_CLIENT_3D].effective_priority == A840_RING_2_CLIENT_3D);
    CHECK(snap.active_ring == A840_RING_0_COMPOSITOR);
    CHECK(hw.last_pi_boosted == 0u);

    a840_kmd_preempt_destroy(&sched);
}

void test_22_transitive_pi_chain_r0_r2_r3()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    CHECK(submit_req(&sched, make_req(A840_RING_1_INTERACTIVE, 1000u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_3_BG_COMPUTE, 1010u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_2_CLIENT_3D, 1020u, 0u, 1u, A840_RING_3_BG_COMPUTE, 1u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1030u, 0u, 1u, A840_RING_2_CLIENT_3D, 1u), nullptr) == A840_KMD_OK);

    A840PreemptSnapshot snap{};
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.rings[A840_RING_3_BG_COMPUTE].effective_priority == A840_RING_0_COMPOSITOR);
    CHECK(snap.rings[A840_RING_2_CLIENT_3D].effective_priority == A840_RING_0_COMPOSITOR);
    CHECK(hw.last_to_ring == A840_RING_3_BG_COMPUTE); /* Leaf dependency runs first */

    a840_kmd_preempt_destroy(&sched);
}

void test_23_self_dependency_and_cycle_rejection()
{
    A840PreemptConfig cfg = make_default_preempt_config();
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    /* Self dependency rejected */
    CHECK(submit_req(&sched, make_req(A840_RING_2_CLIENT_3D, 1000u, 0u, 1u, A840_RING_2_CLIENT_3D, 1u), nullptr)
          == A840_KMD_STATE_CONFLICT);

    /* 2-ring cycle: R2 -> R3, then R3 -> R2 rejected */
    CHECK(submit_req(&sched, make_req(A840_RING_3_BG_COMPUTE, 1000u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_2_CLIENT_3D, 1010u, 0u, 1u, A840_RING_3_BG_COMPUTE, 1u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_3_BG_COMPUTE, 1020u, 0u, 1u, A840_RING_2_CLIENT_3D, 1u), nullptr)
          == A840_KMD_STATE_CONFLICT);

    /* 3-ring cycle: R0 -> R2 -> R3, then R3 -> R0 rejected */
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1030u, 0u, 1u, A840_RING_2_CLIENT_3D, 1u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_3_BG_COMPUTE, 1040u, 0u, 1u, A840_RING_0_COMPOSITOR, 1u), nullptr)
          == A840_KMD_STATE_CONFLICT);

    A840PreemptSnapshot snap{};
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.total_cycle_rejects == 3u);

    a840_kmd_preempt_destroy(&sched);
}

void test_24_already_satisfied_dependency_no_boost_needed()
{
    A840PreemptConfig cfg = make_default_preempt_config();
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    CHECK(submit_req(&sched, make_req(A840_RING_2_CLIENT_3D, 1000u), nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_2_CLIENT_3D, 1u, 1500u) == A840_KMD_OK);

    A840PreemptSubmitResult res{};
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 2000u, 0u, 1u, A840_RING_2_CLIENT_3D, 1u), &res) == A840_KMD_OK);
    CHECK(res.pi_boost_applied == 0u);
    CHECK(res.doorbell_emitted == 1u);

    a840_kmd_preempt_destroy(&sched);
}

void test_25_invalid_dependency_arguments_rejected()
{
    A840PreemptConfig cfg = make_default_preempt_config();
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1000u, 0u, 1u, A840_RING_COUNT, 1u), nullptr) == A840_KMD_RANGE);
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1000u, 0u, 1u, A840_RING_2_CLIENT_3D, 0u), nullptr) == A840_KMD_INVALID_ARG);

    a840_kmd_preempt_destroy(&sched);
}

void test_26_pi_boost_extends_until_max_seqno()
{
    A840PreemptConfig cfg = make_default_preempt_config();
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    CHECK(submit_req(&sched, make_req(A840_RING_2_CLIENT_3D, 1000u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_2_CLIENT_3D, 1010u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1020u, 0u, 1u, A840_RING_2_CLIENT_3D, 1u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1030u, 0u, 1u, A840_RING_2_CLIENT_3D, 2u), nullptr) == A840_KMD_OK);

    /* Retiring seqno 1 keeps boost active until seqno 2 finishes */
    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_2_CLIENT_3D, 1u, 1100u) == A840_KMD_OK);
    A840PreemptSnapshot snap{};
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.rings[A840_RING_2_CLIENT_3D].pi_boost_active == 1u);
    CHECK(snap.rings[A840_RING_2_CLIENT_3D].boost_until_seqno == 2u);

    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_2_CLIENT_3D, 2u, 1200u) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.rings[A840_RING_2_CLIENT_3D].pi_boost_active == 0u);

    a840_kmd_preempt_destroy(&sched);
}

/* ======================== GROUP 4: Fault Isolation & Lock-Free Callbacks ======================== */

void test_27_faulted_ring3_never_blocks_ring0()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    CHECK(submit_req(&sched, make_req(A840_RING_3_BG_COMPUTE, 1000u), nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_mark_ring_faulted(&sched, A840_RING_3_BG_COMPUTE, 2000u) == A840_KMD_OK);

    /* Submitting to faulted RING_3 returns POISONED */
    CHECK(submit_req(&sched, make_req(A840_RING_3_BG_COMPUTE, 2100u), nullptr) == A840_KMD_POISONED);

    /* RING_0 submits and rings doorbell immediately without hindrance */
    A840PreemptSubmitResult res{};
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 2200u), &res) == A840_KMD_OK);
    CHECK(res.doorbell_emitted == 1u);
    CHECK(hw.last_doorbell_ring == A840_RING_0_COMPOSITOR);

    a840_kmd_preempt_destroy(&sched);
}

void test_28_fault_reset_lifecycle_and_invalid_transitions()
{
    A840PreemptConfig cfg = make_default_preempt_config();
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    /* Invalid transitions from ACTIVE */
    CHECK(a840_kmd_preempt_begin_ring_reset(&sched, A840_RING_2_CLIENT_3D) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_preempt_complete_ring_reset(&sched, A840_RING_2_CLIENT_3D, 1000u) == A840_KMD_STATE_CONFLICT);

    CHECK(a840_kmd_preempt_mark_ring_faulted(&sched, A840_RING_2_CLIENT_3D, 1000u) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_mark_ring_faulted(&sched, A840_RING_2_CLIENT_3D, 1100u) == A840_KMD_STATE_CONFLICT);

    CHECK(a840_kmd_preempt_begin_ring_reset(&sched, A840_RING_2_CLIENT_3D) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_2_CLIENT_3D, 1200u), nullptr) == A840_KMD_POISONED);

    CHECK(a840_kmd_preempt_complete_ring_reset(&sched, A840_RING_2_CLIENT_3D, 1300u) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_2_CLIENT_3D, 1400u), nullptr) == A840_KMD_OK);

    A840PreemptSnapshot snap{};
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.rings[A840_RING_2_CLIENT_3D].fault_count == 1u);
    CHECK(snap.rings[A840_RING_2_CLIENT_3D].reset_count == 1u);

    a840_kmd_preempt_destroy(&sched);
}

void test_29_faulting_dep_ring_unblocks_ring0_and_rejects_new_dep()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    CHECK(submit_req(&sched, make_req(A840_RING_2_CLIENT_3D, 1000u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1050u, 0u, 1u, A840_RING_2_CLIENT_3D, 1u), nullptr) == A840_KMD_OK);

    /* Fault RING_2 -> clears dep edge and activates RING_0 immediately! */
    CHECK(a840_kmd_preempt_mark_ring_faulted(&sched, A840_RING_2_CLIENT_3D, 1500u) == A840_KMD_OK);
    A840PreemptSnapshot snap{};
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.active_ring == A840_RING_0_COMPOSITOR);

    /* New submission depending on faulted RING_2 rejected with POISONED */
    CHECK(submit_req(&sched, make_req(A840_RING_1_INTERACTIVE, 1600u, 0u, 1u, A840_RING_2_CLIENT_3D, 99u), nullptr)
          == A840_KMD_POISONED);

    a840_kmd_preempt_destroy(&sched);
}

void test_30_all_preempt_callbacks_run_strictly_unlocked_and_reentrant()
{
    TrackedLock tlock;
    MockHwRecorder hw;
    hw.lock_tracker = &tlock;

    A840PreemptSyncOps sops = tlock.preempt_ops();
    A840PreemptHwOps hops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&hops, &sops);
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);
    hw.reenter_sched = &sched;

    CHECK(submit_req(&sched, make_req(A840_RING_3_BG_COMPUTE, 1000u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1100u, 0u, 1u, A840_RING_3_BG_COMPUTE, 1u), nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_3_BG_COMPUTE, 1u, 1500u) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_2_CLIENT_3D, 1600u), nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_0_COMPOSITOR, 1u, 1700u) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1750u), nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_on_hw_in_progress(&sched, 1800u) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_on_switch_done(&sched, 1900u) == A840_KMD_OK);

    CHECK(hw.callback_saw_lock_held == 0u);
    CHECK(hw.reenter_ok_calls > 0u);
    CHECK(tlock.depth.load() == 0);
    CHECK(tlock.lock_count.load() == tlock.unlock_count.load());

    a840_kmd_preempt_destroy(&sched);
}

/* ======================== GROUP 5: Wakeup Latency Engine ======================== */

void test_31_wakeup_init_defaults_and_mode_enforcement()
{
    A840KmdWakeupEngine eng{};
    CHECK(a840_kmd_wakeup_init(nullptr, nullptr) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_wakeup_init(&eng, nullptr) == A840_KMD_OK);

    A840WakeupSnapshot snap{};
    CHECK(a840_kmd_wakeup_get_snapshot(&eng, &snap) == A840_KMD_OK);
    CHECK(snap.rings[A840_RING_0_COMPOSITOR].effective_mode == A840_WAKE_MODE_IMMEDIATE);
    CHECK(snap.rings[A840_RING_1_INTERACTIVE].effective_mode == A840_WAKE_MODE_IMMEDIATE);
    CHECK(snap.rings[A840_RING_2_CLIENT_3D].effective_mode == A840_WAKE_MODE_COALESCED);
    CHECK(snap.rings[A840_RING_3_BG_COMPUTE].effective_mode == A840_WAKE_MODE_COALESCED);

    /* Fail-closed: RING_0 and RING_1 reject COALESCED mode */
    A840WakeupRingConfig bad{};
    bad.base_mode = A840_WAKE_MODE_COALESCED;
    CHECK(a840_kmd_wakeup_configure_ring(&eng, A840_RING_0_COMPOSITOR, &bad) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_wakeup_configure_ring(&eng, A840_RING_1_INTERACTIVE, &bad) == A840_KMD_INVALID_ARG);

    a840_kmd_wakeup_destroy(&eng);
}

void test_32_wakeup_init_rejects_bad_custom_config()
{
    A840KmdWakeupEngine eng{};
    A840WakeupConfig cfg{};
    cfg.use_custom_ring_cfg = 1u;
    cfg.rings[A840_RING_0_COMPOSITOR].base_mode = A840_WAKE_MODE_COALESCED;
    CHECK(a840_kmd_wakeup_init(&eng, &cfg) == A840_KMD_INVALID_ARG);

    A840WakeupSyncOps bad_sync{};
    A840WakeupConfig cfg2{};
    cfg2.sync_ops = &bad_sync;
    CHECK(a840_kmd_wakeup_init(&eng, &cfg2) == A840_KMD_INVALID_ARG);
}

void test_33_immediate_wake_ring0_and_ring1_zero_coalescing()
{
    A840KmdWakeupEngine eng{};
    CHECK(a840_kmd_wakeup_init(&eng, nullptr) == A840_KMD_OK);

    WakeEventLog log;
    A840WakeupRegisterResult r0{}, r1{};
    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_0_COMPOSITOR, 1u, 1000u,
                                          &WakeEventLog::on_wake, &log, &r0) == A840_KMD_OK);
    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_1_INTERACTIVE, 1u, 1000u,
                                          &WakeEventLog::on_wake, &log, &r1) == A840_KMD_OK);

    uint32_t woken = 0u;
    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_0_COMPOSITOR, 1u, 1200u, &woken) == A840_KMD_OK);
    CHECK(woken == 1u && log.wake_count == 1u && log.last_ring == A840_RING_0_COMPOSITOR && log.last_lat_ns == 0u);

    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_1_INTERACTIVE, 1u, 1300u, &woken) == A840_KMD_OK);
    CHECK(woken == 1u && log.wake_count == 2u && log.last_ring == A840_RING_1_INTERACTIVE);

    a840_kmd_wakeup_destroy(&eng);
}

void test_34_coalesced_ring3_holds_until_max_batch()
{
    A840KmdWakeupEngine eng{};
    A840WakeupConfig cfg{};
    cfg.use_custom_ring_cfg = 1u;
    cfg.rings[0].base_mode = A840_WAKE_MODE_IMMEDIATE;
    cfg.rings[1].base_mode = A840_WAKE_MODE_IMMEDIATE;
    cfg.rings[2].base_mode = A840_WAKE_MODE_COALESCED;
    cfg.rings[2].coalesce_max_batch = 3u;
    cfg.rings[2].coalesce_window_ns = 1000000u;
    cfg.rings[3].base_mode = A840_WAKE_MODE_COALESCED;
    cfg.rings[3].coalesce_max_batch = 3u;
    cfg.rings[3].coalesce_window_ns = 1000000u;
    CHECK(a840_kmd_wakeup_init(&eng, &cfg) == A840_KMD_OK);

    WakeEventLog log;
    for (uint64_t seq = 1u; seq <= 3u; ++seq) {
        CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_3_BG_COMPUTE, seq, 1000u,
                                              &WakeEventLog::on_wake, &log, nullptr) == A840_KMD_OK);
    }

    uint32_t woken = 99u;
    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_3_BG_COMPUTE, 1u, 2000u, &woken) == A840_KMD_OK);
    CHECK(woken == 0u && log.wake_count == 0u);
    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_3_BG_COMPUTE, 2u, 2100u, &woken) == A840_KMD_OK);
    CHECK(woken == 0u && log.wake_count == 0u);
    /* 3rd IRQ hits coalesce_max_batch == 3 -> flushes all 3 waiters at once! */
    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_3_BG_COMPUTE, 3u, 2200u, &woken) == A840_KMD_OK);
    CHECK(woken == 3u && log.wake_count == 3u);

    a840_kmd_wakeup_destroy(&eng);
}

void test_35_coalesced_ring2_flushes_on_window_expiry_via_poll_timers()
{
    A840KmdWakeupEngine eng{};
    A840WakeupConfig cfg{};
    cfg.use_custom_ring_cfg = 1u;
    cfg.rings[0].base_mode = A840_WAKE_MODE_IMMEDIATE;
    cfg.rings[1].base_mode = A840_WAKE_MODE_IMMEDIATE;
    cfg.rings[2].base_mode = A840_WAKE_MODE_COALESCED;
    cfg.rings[2].coalesce_max_batch = 8u;
    cfg.rings[2].coalesce_window_ns = 400000u; /* 400 us */
    cfg.rings[3].base_mode = A840_WAKE_MODE_COALESCED;
    CHECK(a840_kmd_wakeup_init(&eng, &cfg) == A840_KMD_OK);

    WakeEventLog log;
    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_2_CLIENT_3D, 1u, 100000u,
                                          &WakeEventLog::on_wake, &log, nullptr) == A840_KMD_OK);

    uint32_t woken = 0u;
    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_2_CLIENT_3D, 1u, 200000u, &woken) == A840_KMD_OK);
    CHECK(woken == 0u);

    /* Poll before window expires */
    CHECK(a840_kmd_wakeup_poll_timers(&eng, 500000u, &woken) == A840_KMD_OK);
    CHECK(woken == 0u);

    /* Poll at window boundary (200k + 400k = 600k) -> flushes waiter with 400 us latency */
    CHECK(a840_kmd_wakeup_poll_timers(&eng, 600000u, &woken) == A840_KMD_OK);
    CHECK(woken == 1u && log.wake_count == 1u && log.last_lat_ns == 400000u);

    a840_kmd_wakeup_destroy(&eng);
}

void test_36_pi_boost_auto_switches_coalesced_ring_to_immediate_and_flushes()
{
    A840KmdWakeupEngine eng{};
    CHECK(a840_kmd_wakeup_init(&eng, nullptr) == A840_KMD_OK);

    WakeEventLog log;
    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_2_CLIENT_3D, 1u, 1000u,
                                          &WakeEventLog::on_wake, &log, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_2_CLIENT_3D, 2u, 1000u,
                                          &WakeEventLog::on_wake, &log, nullptr) == A840_KMD_OK);

    /* Seqno 1 arrives while coalesced -> held */
    uint32_t woken = 0u;
    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_2_CLIENT_3D, 1u, 2000u, &woken) == A840_KMD_OK);
    CHECK(woken == 0u);

    /* Enter PI boost -> immediately flushes pending waiter 1 with PI_FLUSH reason! */
    uint32_t flushed = 0u;
    CHECK(a840_kmd_wakeup_set_pi_boost(&eng, A840_RING_2_CLIENT_3D, 1u, 2050u, &flushed) == A840_KMD_OK);
    CHECK(flushed == 1u && log.wake_count == 1u && log.last_reason == A840_WAKE_REASON_PI_FLUSH);
    CHECK(log.last_lat_ns == 50u);

    /* While boosted, seqno 2 wakes IMMEDIATELY on IRQ */
    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_2_CLIENT_3D, 2u, 2100u, &woken) == A840_KMD_OK);
    CHECK(woken == 1u && log.wake_count == 2u);

    /* Clear PI boost -> returns to COALESCED */
    CHECK(a840_kmd_wakeup_set_pi_boost(&eng, A840_RING_2_CLIENT_3D, 0u, 2200u, &flushed) == A840_KMD_OK);
    A840WakeupSnapshot snap{};
    CHECK(a840_kmd_wakeup_get_snapshot(&eng, &snap) == A840_KMD_OK);
    CHECK(snap.rings[A840_RING_2_CLIENT_3D].effective_mode == A840_WAKE_MODE_COALESCED);

    a840_kmd_wakeup_destroy(&eng);
}

void test_37_waiter_fast_path_when_seqno_already_retired()
{
    A840KmdWakeupEngine eng{};
    CHECK(a840_kmd_wakeup_init(&eng, nullptr) == A840_KMD_OK);

    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_0_COMPOSITOR, 10u, 5000u, nullptr) == A840_KMD_OK);

    WakeEventLog log;
    A840WakeupRegisterResult res{};
    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_0_COMPOSITOR, 8u, 5100u,
                                          &WakeEventLog::on_wake, &log, &res) == A840_KMD_OK);
    CHECK(res.already_satisfied == 1u && res.waiter_handle == 0u);
    CHECK(log.wake_count == 1u && log.last_reason == A840_WAKE_REASON_FAST_PATH);

    A840WakeupSnapshot snap{};
    CHECK(a840_kmd_wakeup_get_snapshot(&eng, &snap) == A840_KMD_OK);
    CHECK(snap.active_waiter_count == 0u && snap.total_fast_path_hits == 1u);

    a840_kmd_wakeup_destroy(&eng);
}

void test_38_fixed_pool_128_capacity_zero_heap_and_reuse()
{
    A840KmdWakeupEngine eng{};
    CHECK(a840_kmd_wakeup_init(&eng, nullptr) == A840_KMD_OK);

    WakeEventLog log;
    std::vector<uint64_t> handles;
    handles.reserve(A840_WAKEUP_MAX_WAITERS);

    for (uint32_t i = 0u; i < A840_WAKEUP_MAX_WAITERS; ++i) {
        A840WakeupRegisterResult res{};
        CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_0_COMPOSITOR, i + 10u, 1000u,
                                              &WakeEventLog::on_wake, &log, &res) == A840_KMD_OK);
        CHECK(res.waiter_handle != 0u);
        handles.push_back(res.waiter_handle);
    }

    /* 129th waiter rejected with CAPACITY */
    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_0_COMPOSITOR, 999u, 1000u,
                                          &WakeEventLog::on_wake, &log, nullptr) == A840_KMD_CAPACITY);

    /* Cancel 1 waiter -> slot reused with distinct generation handle */
    uint64_t canceled = handles[0];
    CHECK(a840_kmd_wakeup_cancel_waiter(&eng, canceled) == A840_KMD_OK);
    CHECK(a840_kmd_wakeup_cancel_waiter(&eng, canceled) == A840_KMD_NOT_FOUND);

    A840WakeupRegisterResult reused{};
    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_0_COMPOSITOR, 500u, 1100u,
                                          &WakeEventLog::on_wake, &log, &reused) == A840_KMD_OK);
    CHECK(reused.waiter_handle != canceled);
    /* Stale handle cannot cancel new occupant */
    CHECK(a840_kmd_wakeup_cancel_waiter(&eng, canceled) == A840_KMD_NOT_FOUND);

    A840WakeupSnapshot snap{};
    CHECK(a840_kmd_wakeup_get_snapshot(&eng, &snap) == A840_KMD_OK);
    CHECK(snap.peak_waiter_count == A840_WAKEUP_MAX_WAITERS);
    CHECK(snap.total_pool_rejects == 1u);

    a840_kmd_wakeup_destroy(&eng);
}

void test_39_wakeup_fault_ring_drains_only_faulted_ring_waiters()
{
    A840KmdWakeupEngine eng{};
    CHECK(a840_kmd_wakeup_init(&eng, nullptr) == A840_KMD_OK);

    WakeEventLog log0, log2;
    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_0_COMPOSITOR, 5u, 1000u,
                                          &WakeEventLog::on_wake, &log0, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_2_CLIENT_3D, 5u, 1000u,
                                          &WakeEventLog::on_wake, &log2, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_2_CLIENT_3D, 9u, 1000u,
                                          &WakeEventLog::on_wake, &log2, nullptr) == A840_KMD_OK);

    uint32_t woken = 0u;
    CHECK(a840_kmd_wakeup_fault_ring(&eng, A840_RING_2_CLIENT_3D, 2000u, &woken) == A840_KMD_OK);
    CHECK(woken == 2u && log2.wake_count == 2u && log2.last_reason == A840_WAKE_REASON_RING_FAULTED);
    CHECK(log0.wake_count == 0u); /* RING_0 untouched */

    a840_kmd_wakeup_destroy(&eng);
}

void test_40_exact_p50_p95_p99_percentile_accuracy()
{
    A840KmdWakeupEngine eng{};
    CHECK(a840_kmd_wakeup_init(&eng, nullptr) == A840_KMD_OK);

    /* Insert 100 samples: 1000, 2000, ..., 100000 ns in scrambled order */
    for (uint32_t i = 0u; i < 100u; ++i) {
        uint32_t perm = (i * 37u) % 100u;
        uint64_t val = static_cast<uint64_t>(perm + 1u) * 1000u;
        CHECK(a840_kmd_wakeup_record_latency(&eng, A840_LATENCY_ENQUEUE_TO_DOORBELL,
                                             A840_RING_0_COMPOSITOR, val, 1000u + i) == A840_KMD_OK);
    }

    A840LatencyStats st{};
    CHECK(a840_kmd_wakeup_get_latency_stats(&eng, A840_LATENCY_ENQUEUE_TO_DOORBELL, &st) == A840_KMD_OK);
    CHECK(st.count == 100u);
    CHECK(st.min_ns == 1000u);
    CHECK(st.max_ns == 100000u);
    CHECK(st.p50_ns == 50000u);
    CHECK(st.p95_ns == 95000u);
    CHECK(st.p99_ns == 99000u);
    CHECK(st.sum_ns == 5050000u);

    a840_kmd_wakeup_destroy(&eng);
}

void test_41_budget_alert_threshold_and_compositor_breach_count()
{
    WakeEventLog log;
    A840WakeupConfig cfg{};
    cfg.budget_alert_ns[A840_LATENCY_PREEMPT_TRIGGER_TO_ACTIVE] = 500000u; /* 500 us */
    cfg.on_budget_alert = &WakeEventLog::on_alert;
    cfg.budget_alert_cookie = &log;

    A840KmdWakeupEngine eng{};
    CHECK(a840_kmd_wakeup_init(&eng, &cfg) == A840_KMD_OK);

    /* Below budget: no alert */
    CHECK(a840_kmd_wakeup_record_latency(&eng, A840_LATENCY_PREEMPT_TRIGGER_TO_ACTIVE,
                                         A840_RING_0_COMPOSITOR, 450000u, 1000u) == A840_KMD_OK);
    CHECK(log.alert_count == 0u);

    /* Above budget on RING_2: increments total breach, not compositor breach */
    CHECK(a840_kmd_wakeup_record_latency(&eng, A840_LATENCY_PREEMPT_TRIGGER_TO_ACTIVE,
                                         A840_RING_2_CLIENT_3D, 600000u, 2000u) == A840_KMD_OK);
    /* Above budget on RING_0: increments both */
    CHECK(a840_kmd_wakeup_record_latency(&eng, A840_LATENCY_PREEMPT_TRIGGER_TO_ACTIVE,
                                         A840_RING_0_COMPOSITOR, 750000u, 3000u) == A840_KMD_OK);

    A840LatencyStats st{};
    CHECK(a840_kmd_wakeup_get_latency_stats(&eng, A840_LATENCY_PREEMPT_TRIGGER_TO_ACTIVE, &st) == A840_KMD_OK);
    CHECK(st.budget_breach_count == 2u);
    CHECK(st.compositor_budget_breach_count == 1u);
    CHECK(log.alert_count == 2u && log.last_alert_lat_ns == 750000u);

    /* Reset preserves threshold */
    CHECK(a840_kmd_wakeup_reset_latency_stats(&eng) == A840_KMD_OK);
    CHECK(a840_kmd_wakeup_get_latency_stats(&eng, A840_LATENCY_PREEMPT_TRIGGER_TO_ACTIVE, &st) == A840_KMD_OK);
    CHECK(st.count == 0u && st.budget_threshold_ns == 500000u);

    a840_kmd_wakeup_destroy(&eng);
}

void test_42_wakeup_callbacks_run_strictly_unlocked_and_reentrant()
{
    TrackedLock tlock;
    WakeEventLog log;
    log.lock_tracker = &tlock;

    A840WakeupSyncOps sops = tlock.wakeup_ops();
    A840WakeupConfig cfg{};
    cfg.sync_ops = &sops;
    cfg.budget_alert_ns[A840_LATENCY_IRQ_TO_WAITER_WAKE] = 10u;
    cfg.on_budget_alert = &WakeEventLog::on_alert;
    cfg.budget_alert_cookie = &log;

    A840KmdWakeupEngine eng{};
    CHECK(a840_kmd_wakeup_init(&eng, &cfg) == A840_KMD_OK);
    log.reenter_engine = &eng;

    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_2_CLIENT_3D, 1u, 1000u,
                                          &WakeEventLog::on_wake, &log, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_2_CLIENT_3D, 1u, 2000u, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_wakeup_flush_coalesced(&eng, A840_RING_2_CLIENT_3D, 2500u, 1u, nullptr) == A840_KMD_OK);

    CHECK(log.wake_count == 1u && log.alert_count == 1u);
    CHECK(log.saw_lock_held == 0u);
    CHECK(log.reenter_ok == 1u);
    CHECK(tlock.depth.load() == 0);
    CHECK(tlock.lock_count.load() == tlock.unlock_count.load());

    a840_kmd_wakeup_destroy(&eng);
}

/* ======================== GROUP 6: Integrated Preempt + Wakeup End-to-End ======================== */

void test_43_end_to_end_bridge_pi_and_latency_metrics()
{
    A840KmdWakeupEngine eng{};
    CHECK(a840_kmd_wakeup_init(&eng, nullptr) == A840_KMD_OK);

    MockHwRecorder hw;
    hw.wakeup_bridge = &eng;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    WakeEventLog wlog;
    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_2_CLIENT_3D, 1u, 1000u,
                                          &WakeEventLog::on_wake, &wlog, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_0_COMPOSITOR, 1u, 1050u,
                                          &WakeEventLog::on_wake, &wlog, nullptr) == A840_KMD_OK);

    CHECK(submit_req(&sched, make_req(A840_RING_1_INTERACTIVE, 1000u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_2_CLIENT_3D, 1010u), nullptr) == A840_KMD_OK);
    /* RING_0 depends on RING_2 seqno 1 -> bridges PI boost to wakeup engine! */
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1050u, 0u, 1u, A840_RING_2_CLIENT_3D, 1u), nullptr) == A840_KMD_OK);

    A840WakeupSnapshot wsnap{};
    CHECK(a840_kmd_wakeup_get_snapshot(&eng, &wsnap) == A840_KMD_OK);
    CHECK(wsnap.rings[A840_RING_2_CLIENT_3D].pi_boost_active == 1u);
    CHECK(wsnap.rings[A840_RING_2_CLIENT_3D].effective_mode == A840_WAKE_MODE_IMMEDIATE);

    CHECK(a840_kmd_preempt_on_hw_in_progress(&sched, 1100u) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_on_switch_done(&sched, 1200u) == A840_KMD_OK);
    /* Retiring RING_2 wakes RING_2 waiter immediately (not coalesced!) */
    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_2_CLIENT_3D, 1u, 1500u) == A840_KMD_OK);
    CHECK(wlog.wake_count == 1u && wlog.last_ring == A840_RING_2_CLIENT_3D);

    /* Retiring RING_0 wakes RING_0 waiter */
    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_0_COMPOSITOR, 1u, 1800u) == A840_KMD_OK);
    CHECK(wlog.wake_count == 2u && wlog.last_ring == A840_RING_0_COMPOSITOR);

    A840LatencyStats s_db{}, s_pr{}, s_wk{};
    CHECK(a840_kmd_wakeup_get_latency_stats(&eng, A840_LATENCY_ENQUEUE_TO_DOORBELL, &s_db) == A840_KMD_OK);
    CHECK(a840_kmd_wakeup_get_latency_stats(&eng, A840_LATENCY_PREEMPT_TRIGGER_TO_ACTIVE, &s_pr) == A840_KMD_OK);
    CHECK(a840_kmd_wakeup_get_latency_stats(&eng, A840_LATENCY_IRQ_TO_WAITER_WAKE, &s_wk) == A840_KMD_OK);
    CHECK(s_db.count >= 2u);
    CHECK(s_pr.count == 1u && s_pr.max_ns == 150u);
    CHECK(s_wk.count == 2u && s_wk.max_ns == 0u);

    a840_kmd_preempt_destroy(&sched);
    a840_kmd_wakeup_destroy(&eng);
}

/* ======================== GROUP 7: Additional Edge Cases (44..56) ======================== */

void test_44_wakeup_uninitialized_and_range_errors()
{
    A840KmdWakeupEngine eng{};
    A840LatencyStats st{};
    CHECK(a840_kmd_wakeup_get_latency_stats(&eng, 0u, &st) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_wakeup_init(&eng, nullptr) == A840_KMD_OK);

    CHECK(a840_kmd_wakeup_get_latency_stats(&eng, A840_LATENCY_METRIC_COUNT, &st) == A840_KMD_RANGE);
    CHECK(a840_kmd_wakeup_get_latency_stats(&eng, 0u, nullptr) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_wakeup_set_budget_threshold(&eng, A840_LATENCY_METRIC_COUNT, 100u) == A840_KMD_RANGE);
    CHECK(a840_kmd_wakeup_record_latency(&eng, A840_LATENCY_METRIC_COUNT, 0u, 10u, 10u) == A840_KMD_RANGE);
    CHECK(a840_kmd_wakeup_record_latency(&eng, 0u, A840_RING_COUNT, 10u, 10u) == A840_KMD_RANGE);
    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_COUNT, 1u, 10u, &WakeEventLog::on_wake, nullptr, nullptr) == A840_KMD_RANGE);
    CHECK(a840_kmd_wakeup_register_waiter(&eng, 0u, 0u, 10u, &WakeEventLog::on_wake, nullptr, nullptr) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_wakeup_register_waiter(&eng, 0u, 1u, 10u, nullptr, nullptr, nullptr) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_wakeup_cancel_waiter(&eng, 0u) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_COUNT, 1u, 10u, nullptr) == A840_KMD_RANGE);
    CHECK(a840_kmd_wakeup_flush_coalesced(&eng, A840_RING_COUNT, 10u, 1u, nullptr) == A840_KMD_RANGE);
    CHECK(a840_kmd_wakeup_set_pi_boost(&eng, A840_RING_COUNT, 1u, 10u, nullptr) == A840_KMD_RANGE);
    CHECK(a840_kmd_wakeup_fault_ring(&eng, A840_RING_COUNT, 10u, nullptr) == A840_KMD_RANGE);

    a840_kmd_wakeup_destroy(&eng);
}

void test_45_wakeup_irq_seqno_regression_rejected()
{
    A840KmdWakeupEngine eng{};
    CHECK(a840_kmd_wakeup_init(&eng, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_0_COMPOSITOR, 5u, 1000u, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_0_COMPOSITOR, 4u, 2000u, nullptr) == A840_KMD_STATE_CONFLICT);
    a840_kmd_wakeup_destroy(&eng);
}

void test_46_wakeup_injected_clock_callback_when_now_zero()
{
    TrackedLock tlock;
    tlock.injected_now_ns.store(500000u);
    A840WakeupSyncOps sops = tlock.wakeup_ops();
    A840WakeupConfig cfg{};
    cfg.sync_ops = &sops;

    A840KmdWakeupEngine eng{};
    CHECK(a840_kmd_wakeup_init(&eng, &cfg) == A840_KMD_OK);

    WakeEventLog log;
    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_2_CLIENT_3D, 1u, 0u,
                                          &WakeEventLog::on_wake, &log, nullptr) == A840_KMD_OK);
    tlock.injected_now_ns.store(600000u);
    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_2_CLIENT_3D, 1u, 0u, nullptr) == A840_KMD_OK);
    tlock.injected_now_ns.store(600250u);
    CHECK(a840_kmd_wakeup_flush_coalesced(&eng, A840_RING_2_CLIENT_3D, 0u, 1u, nullptr) == A840_KMD_OK);
    CHECK(log.wake_count == 1u && log.last_lat_ns == 250u);

    a840_kmd_wakeup_destroy(&eng);
}

void test_47_wakeup_zero_window_coalescing_literal()
{
    A840KmdWakeupEngine eng{};
    A840WakeupConfig cfg{};
    cfg.use_custom_ring_cfg = 1u;
    cfg.rings[0].base_mode = A840_WAKE_MODE_IMMEDIATE;
    cfg.rings[1].base_mode = A840_WAKE_MODE_IMMEDIATE;
    cfg.rings[2].base_mode = A840_WAKE_MODE_COALESCED;
    cfg.rings[2].coalesce_max_batch = 16u;
    cfg.rings[2].coalesce_window_ns = 0u;
    cfg.rings[2].allow_zero_window = 1u;
    cfg.rings[3].base_mode = A840_WAKE_MODE_COALESCED;
    CHECK(a840_kmd_wakeup_init(&eng, &cfg) == A840_KMD_OK);

    WakeEventLog log;
    CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_2_CLIENT_3D, 1u, 100u,
                                          &WakeEventLog::on_wake, &log, nullptr) == A840_KMD_OK);
    uint32_t woken = 0u;
    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_2_CLIENT_3D, 1u, 200u, &woken) == A840_KMD_OK);
    CHECK(woken == 1u);

    a840_kmd_wakeup_destroy(&eng);
}

void test_48_wakeup_window_wraparound_over_2048_samples()
{
    A840KmdWakeupEngine eng{};
    CHECK(a840_kmd_wakeup_init(&eng, nullptr) == A840_KMD_OK);

    for (uint32_t i = 0u; i < A840_WAKEUP_EXACT_WINDOW_SAMPLES + 500u; ++i) {
        uint64_t lat = (i < 500u) ? 900000u : 10000u;
        CHECK(a840_kmd_wakeup_record_latency(&eng, A840_LATENCY_IRQ_TO_WAITER_WAKE,
                                             A840_RING_0_COMPOSITOR, lat, i) == A840_KMD_OK);
    }

    A840LatencyStats st{};
    CHECK(a840_kmd_wakeup_get_latency_stats(&eng, A840_LATENCY_IRQ_TO_WAITER_WAKE, &st) == A840_KMD_OK);
    CHECK(st.count == A840_WAKEUP_EXACT_WINDOW_SAMPLES + 500u);
    CHECK(st.max_ns == 900000u);
    CHECK(st.min_ns == 10000u);
    /* Sliding window of last 2048 samples contains only 10000 ns samples */
    CHECK(st.p50_ns == 10000u && st.p95_ns == 10000u && st.p99_ns == 10000u);

    a840_kmd_wakeup_destroy(&eng);
}

void test_49_wakeup_set_budget_threshold_disables_alerts_when_zero()
{
    WakeEventLog log;
    A840WakeupConfig cfg{};
    cfg.on_budget_alert = &WakeEventLog::on_alert;
    cfg.budget_alert_cookie = &log;

    A840KmdWakeupEngine eng{};
    CHECK(a840_kmd_wakeup_init(&eng, &cfg) == A840_KMD_OK);
    CHECK(a840_kmd_wakeup_set_budget_threshold(&eng, A840_LATENCY_ENQUEUE_TO_DOORBELL, 0u) == A840_KMD_OK);
    CHECK(a840_kmd_wakeup_record_latency(&eng, A840_LATENCY_ENQUEUE_TO_DOORBELL,
                                         A840_RING_0_COMPOSITOR, 9999999u, 1000u) == A840_KMD_OK);
    CHECK(log.alert_count == 0u);
    a840_kmd_wakeup_destroy(&eng);
}

void test_50_multiple_waiters_same_seqno_all_wake()
{
    A840KmdWakeupEngine eng{};
    CHECK(a840_kmd_wakeup_init(&eng, nullptr) == A840_KMD_OK);

    WakeEventLog log;
    for (int i = 0; i < 8; ++i) {
        CHECK(a840_kmd_wakeup_register_waiter(&eng, A840_RING_0_COMPOSITOR, 7u, 100u,
                                              &WakeEventLog::on_wake, &log, nullptr) == A840_KMD_OK);
    }
    uint32_t woken = 0u;
    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_0_COMPOSITOR, 6u, 200u, &woken) == A840_KMD_OK);
    CHECK(woken == 0u);
    CHECK(a840_kmd_wakeup_on_irq(&eng, A840_RING_0_COMPOSITOR, 7u, 300u, &woken) == A840_KMD_OK);
    CHECK(woken == 8u && log.wake_count == 8u);

    a840_kmd_wakeup_destroy(&eng);
}

void test_51_preempt_tick_detects_timeout_and_recovers()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    cfg.preempt_timeout_ns = 100000u;
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    CHECK(submit_req(&sched, make_req(A840_RING_3_BG_COMPUTE, 1000u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1100u), nullptr) == A840_KMD_OK);
    /* Tick past timeout while still in TRIGGERED */
    CHECK(a840_kmd_preempt_tick(&sched, 102000u) == A840_KMD_OK);
    CHECK(hw.timeout_calls == 1u);

    A840PreemptSnapshot snap{};
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.state == A840_PREEMPT_TIMEOUT_RECOVERY);

    CHECK(a840_kmd_preempt_recover_from_timeout(&sched, 0u, 103000u) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.state == A840_PREEMPT_IDLE && snap.active_ring == A840_RING_0_COMPOSITOR);

    a840_kmd_preempt_destroy(&sched);
}

void test_52_preempt_switch_done_fallbacks_if_target_faulted_midflight()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    cfg.min_slice_ns = 0u;
    cfg.allow_zero_min_slice = 1u;
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    CHECK(submit_req(&sched, make_req(A840_RING_3_BG_COMPUTE, 1000u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_2_CLIENT_3D, 1100u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_1_INTERACTIVE, 1200u), nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_on_hw_in_progress(&sched, 1250u) == A840_KMD_OK);

    /* Fault target RING_1 mid-switch -> scheduler resets to IDLE and dispatches RING_2 */
    CHECK(a840_kmd_preempt_mark_ring_faulted(&sched, A840_RING_1_INTERACTIVE, 1300u) == A840_KMD_OK);
    A840PreemptSnapshot snap{};
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.state == A840_PREEMPT_TRIGGERED || snap.active_ring == A840_RING_2_CLIENT_3D);

    a840_kmd_preempt_destroy(&sched);
}

void test_53_complete_auto_settles_on_subsequent_submit()
{
    A840PreemptConfig cfg = make_default_preempt_config();
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    CHECK(submit_req(&sched, make_req(A840_RING_3_BG_COMPUTE, 1000u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1100u), nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_on_hw_in_progress(&sched, 1150u) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_on_switch_done(&sched, 1200u) == A840_KMD_OK);

    /* Without calling ack_complete, submitting another RING_0 job auto-settles COMPLETE -> IDLE */
    A840PreemptSubmitResult res{};
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1300u), &res) == A840_KMD_OK);
    CHECK(res.doorbell_emitted == 1u);
    A840PreemptSnapshot snap{};
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.state == A840_PREEMPT_IDLE);

    a840_kmd_preempt_destroy(&sched);
}

void test_54_all_four_rings_strict_priority_drain_order()
{
    MockHwRecorder hw;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    cfg.min_slice_ns = 0u;
    cfg.allow_zero_min_slice = 1u;
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    /* RING_0 starts active, then RING_3, RING_2, RING_1 enqueue behind it */
    CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, 1000u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_3_BG_COMPUTE, 1010u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_2_CLIENT_3D, 1020u), nullptr) == A840_KMD_OK);
    CHECK(submit_req(&sched, make_req(A840_RING_1_INTERACTIVE, 1030u), nullptr) == A840_KMD_OK);

    A840PreemptSnapshot snap{};
    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_0_COMPOSITOR, 1u, 2000u) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.active_ring == A840_RING_1_INTERACTIVE);

    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_1_INTERACTIVE, 1u, 3000u) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.active_ring == A840_RING_2_CLIENT_3D);

    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_2_CLIENT_3D, 1u, 4000u) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.active_ring == A840_RING_3_BG_COMPUTE);

    CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_3_BG_COMPUTE, 1u, 5000u) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK);
    CHECK(snap.active_ring == A840_RING_NONE);

    a840_kmd_preempt_destroy(&sched);
}

void test_55_null_bridge_helpers_safe()
{
    a840_kmd_wakeup_bridge_pi_boost(nullptr, 0u, 1u, 0u, 1u, 100u);
    a840_kmd_wakeup_bridge_latency(nullptr, A840_PREEMPT_LAT_ENQUEUE_TO_DOORBELL, 0u, 10u, 100u);
    a840_kmd_preempt_destroy(nullptr);
    a840_kmd_wakeup_destroy(nullptr);
    CHECK(true);
}

void test_56_120hz_frame_burst_compositor_under_500us_budget()
{
    A840KmdWakeupEngine eng{};
    CHECK(a840_kmd_wakeup_init(&eng, nullptr) == A840_KMD_OK);

    MockHwRecorder hw;
    hw.wakeup_bridge = &eng;
    A840PreemptHwOps ops = hw.ops();
    A840PreemptConfig cfg = make_default_preempt_config(&ops);
    A840KmdPreemptScheduler sched{};
    CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

    /* Simulate 120 frames (1 second at 120Hz) where RING_3 saturates background
     * and RING_0 compositor preempts every 8.333 ms with 80 us switch latency.
     */
    uint64_t t = 1000000u;
    for (uint32_t f = 1u; f <= 120u; ++f) {
        CHECK(submit_req(&sched, make_req(A840_RING_3_BG_COMPUTE, t), nullptr) == A840_KMD_OK);
        t += 100000u;
        CHECK(submit_req(&sched, make_req(A840_RING_0_COMPOSITOR, t, t + A840_PREEMPT_FRAME_BUDGET_120HZ_NS), nullptr) == A840_KMD_OK);
        CHECK(a840_kmd_preempt_on_hw_in_progress(&sched, t + 20000u) == A840_KMD_OK);
        CHECK(a840_kmd_preempt_on_switch_done(&sched, t + 80000u) == A840_KMD_OK);
        CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_0_COMPOSITOR, f, t + 2000000u) == A840_KMD_OK);
        CHECK(a840_kmd_preempt_on_ring_retire(&sched, A840_RING_3_BG_COMPUTE, f, t + 4000000u) == A840_KMD_OK);
        t += A840_PREEMPT_FRAME_BUDGET_120HZ_NS;
    }

    A840LatencyStats pr{};
    CHECK(a840_kmd_wakeup_get_latency_stats(&eng, A840_LATENCY_PREEMPT_TRIGGER_TO_ACTIVE, &pr) == A840_KMD_OK);
    CHECK(pr.count == 120u);
    CHECK(pr.p99_ns == 80000u);
    CHECK(pr.compositor_budget_breach_count == 0u);

    a840_kmd_preempt_destroy(&sched);
    a840_kmd_wakeup_destroy(&eng);
}

void test_57_tsan_8_thread_concurrent_stress(uint32_t rounds)
{
    for (uint32_t round = 0u; round < rounds; ++round) {
        A840KmdWakeupEngine eng{};
        CHECK(a840_kmd_wakeup_init(&eng, nullptr) == A840_KMD_OK);

        MockHwRecorder hw;
        hw.wakeup_bridge = &eng;
        A840PreemptHwOps ops = hw.ops();
        A840PreemptConfig cfg = make_default_preempt_config(&ops);
        cfg.min_slice_ns = 1000u;
        cfg.preempt_timeout_ns = 50000u;
        A840KmdPreemptScheduler sched{};
        CHECK(a840_kmd_preempt_init(&sched, &cfg) == A840_KMD_OK);

        std::atomic<bool> stop{false};
        std::atomic<uint64_t> clk{100000u};
        WakeEventLog wlog;

        /* 4 submitter threads (one per ring), 1 HW switch/tick thread,
         * 1 retire/IRQ thread, 1 waiter/cancel thread, 1 fault/reset thread.
         */
        std::vector<std::thread> threads;

        for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
            threads.emplace_back([&, r]() {
                for (uint32_t i = 0u; i < 40u; ++i) {
                    uint64_t now = clk.fetch_add(500u, std::memory_order_relaxed);
                    A840PreemptSubmitReq req = make_req(r, now, (r == 0u) ? (now + 2000000u) : 0u);
                    A840PreemptSubmitResult res{};
                    A840KmdStatus st = a840_kmd_preempt_submit(&sched, &req, &res);
                    CHECK(st == A840_KMD_OK || st == A840_KMD_CAPACITY || st == A840_KMD_POISONED);
                }
            });
        }

        /* Thread 5: HW preemption driver + tick */
        threads.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                uint64_t now = clk.fetch_add(1000u, std::memory_order_relaxed);
                (void)a840_kmd_preempt_on_hw_in_progress(&sched, now);
                (void)a840_kmd_preempt_on_switch_done(&sched, now + 200u);
                (void)a840_kmd_preempt_tick(&sched, now + 400u);
                (void)a840_kmd_preempt_recover_from_timeout(&sched, 0u, now + 500u);
                (void)a840_kmd_wakeup_poll_timers(&eng, now + 600u, nullptr);
            }
        });

        /* Thread 6: Retire / IRQ driver */
        threads.emplace_back([&]() {
            while (!stop.load(std::memory_order_acquire)) {
                A840PreemptSnapshot snap{};
                if (a840_kmd_preempt_get_snapshot(&sched, &snap) == A840_KMD_OK) {
                    uint64_t now = clk.fetch_add(700u, std::memory_order_relaxed);
                    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
                        if (snap.rings[r].submitted_seqno > snap.rings[r].completed_seqno) {
                            (void)a840_kmd_preempt_on_ring_retire(&sched, r, snap.rings[r].submitted_seqno, now);
                        }
                    }
                }
            }
        });

        /* Thread 7: Waiter register + cancel + stats reader */
        threads.emplace_back([&]() {
            for (uint32_t i = 0u; i < 60u; ++i) {
                uint32_t r = i % A840_RING_COUNT;
                uint64_t now = clk.fetch_add(300u, std::memory_order_relaxed);
                A840WakeupRegisterResult wres{};
                A840KmdStatus st = a840_kmd_wakeup_register_waiter(
                    &eng, r, i + 1u, now, &WakeEventLog::on_wake, &wlog, &wres);
                if (st == A840_KMD_OK && !wres.already_satisfied && (i % 3u == 0u)) {
                    (void)a840_kmd_wakeup_cancel_waiter(&eng, wres.waiter_handle);
                }
                A840LatencyStats ls{};
                (void)a840_kmd_wakeup_get_latency_stats(&eng, i % A840_LATENCY_METRIC_COUNT, &ls);
            }
        });

        /* Thread 8: Fault & reset isolation on RING_3 only (never RING_0) */
        threads.emplace_back([&]() {
            for (uint32_t i = 0u; i < 8u; ++i) {
                uint64_t now = clk.fetch_add(900u, std::memory_order_relaxed);
                if (a840_kmd_preempt_mark_ring_faulted(&sched, A840_RING_3_BG_COMPUTE, now) == A840_KMD_OK) {
                    (void)a840_kmd_wakeup_fault_ring(&eng, A840_RING_3_BG_COMPUTE, now, nullptr);
                    (void)a840_kmd_preempt_begin_ring_reset(&sched, A840_RING_3_BG_COMPUTE);
                    (void)a840_kmd_preempt_complete_ring_reset(&sched, A840_RING_3_BG_COMPUTE, now + 100u);
                }
            }
        });

        /* Join submitter, waiter, and fault threads first */
        for (size_t i = 0; i < 4; ++i)
            threads[i].join();
        threads[6].join();
        threads[7].join();

        stop.store(true, std::memory_order_release);
        threads[4].join();
        threads[5].join();

        A840PreemptSnapshot final_snap{};
        CHECK(a840_kmd_preempt_get_snapshot(&sched, &final_snap) == A840_KMD_OK);
        CHECK(final_snap.rings[A840_RING_0_COMPOSITOR].health_state == A840_RING_STATE_ACTIVE);
        CHECK(final_snap.total_submits >= 120u);

        a840_kmd_preempt_destroy(&sched);
        a840_kmd_wakeup_destroy(&eng);
    }
}

} // namespace

int main(int argc, char **argv)
{
    uint32_t stress_rounds = 1u;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tsan-stress") == 0 && i + 1 < argc) {
            stress_rounds = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            if (stress_rounds == 0u)
                stress_rounds = 1u;
        } else {
            std::fprintf(stderr, "usage: %s [--tsan-stress <N>]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    run_case("01_hw_reg_map_symbols", test_01_hw_reg_map_symbols);
    run_case("02_priority_to_ring_mapping", test_02_priority_to_ring_mapping);
    run_case("03_ring_to_priority_mapping", test_03_ring_to_priority_mapping);
    run_case("04_record_validation_alignment_and_size", test_04_record_validation_alignment_and_size);
    run_case("05_init_rejects_bad_configs_and_overlapping_records", test_05_init_rejects_bad_configs_and_overlapping_records);
    run_case("06_uninitialized_scheduler_fail_closed", test_06_uninitialized_scheduler_fail_closed);
    run_case("07_basic_single_ring_submit_and_retire", test_07_basic_single_ring_submit_and_retire);
    run_case("08_submit_priority_wrapper_and_validation", test_08_submit_priority_wrapper_and_validation);
    run_case("09_ring_capacity_wraparound_and_full_rejection", test_09_ring_capacity_wraparound_and_full_rejection);
    run_case("10_retire_range_and_regression_errors", test_10_retire_range_and_regression_errors);
    run_case("11_ring0_preempts_saturated_ring3_l1a", test_11_ring0_preempts_saturated_ring3_l1a);
    run_case("12_ring0_preempts_ring2_l1b_draw_boundary", test_12_ring0_preempts_ring2_l1b_draw_boundary);
    run_case("13_set_ring_level_dynamic", test_13_set_ring_level_dynamic);
    run_case("14_illegal_state_transitions_rejected", test_14_illegal_state_transitions_rejected);
    run_case("15_min_slice_anti_thrash_defers_ring1_then_tick_triggers", test_15_min_slice_anti_thrash_defers_ring1_then_tick_triggers);
    run_case("16_urgent_120hz_deadline_exempts_ring1_from_min_slice", test_16_urgent_120hz_deadline_exempts_ring1_from_min_slice);
    run_case("17_triggered_target_upgraded_when_ring0_arrives", test_17_triggered_target_upgraded_when_ring0_arrives);
    run_case("18_preempt_timeout_recovery_and_stalled_ring_isolation", test_18_preempt_timeout_recovery_and_stalled_ring_isolation);
    run_case("19_lower_prio_submission_does_not_preempt_higher_ring", test_19_lower_prio_submission_does_not_preempt_higher_ring);
    run_case("20_hw_error_propagation_from_doorbell_and_trigger", test_20_hw_error_propagation_from_doorbell_and_trigger);
    run_case("21_pi_boost_ring0_depends_on_ring2_prevents_inversion", test_21_pi_boost_ring0_depends_on_ring2_prevents_inversion);
    run_case("22_transitive_pi_chain_r0_r2_r3", test_22_transitive_pi_chain_r0_r2_r3);
    run_case("23_self_dependency_and_cycle_rejection", test_23_self_dependency_and_cycle_rejection);
    run_case("24_already_satisfied_dependency_no_boost_needed", test_24_already_satisfied_dependency_no_boost_needed);
    run_case("25_invalid_dependency_arguments_rejected", test_25_invalid_dependency_arguments_rejected);
    run_case("26_pi_boost_extends_until_max_seqno", test_26_pi_boost_extends_until_max_seqno);
    run_case("27_faulted_ring3_never_blocks_ring0", test_27_faulted_ring3_never_blocks_ring0);
    run_case("28_fault_reset_lifecycle_and_invalid_transitions", test_28_fault_reset_lifecycle_and_invalid_transitions);
    run_case("29_faulting_dep_ring_unblocks_ring0_and_rejects_new_dep", test_29_faulting_dep_ring_unblocks_ring0_and_rejects_new_dep);
    run_case("30_all_preempt_callbacks_run_strictly_unlocked_and_reentrant", test_30_all_preempt_callbacks_run_strictly_unlocked_and_reentrant);
    run_case("31_wakeup_init_defaults_and_mode_enforcement", test_31_wakeup_init_defaults_and_mode_enforcement);
    run_case("32_wakeup_init_rejects_bad_custom_config", test_32_wakeup_init_rejects_bad_custom_config);
    run_case("33_immediate_wake_ring0_and_ring1_zero_coalescing", test_33_immediate_wake_ring0_and_ring1_zero_coalescing);
    run_case("34_coalesced_ring3_holds_until_max_batch", test_34_coalesced_ring3_holds_until_max_batch);
    run_case("35_coalesced_ring2_flushes_on_window_expiry_via_poll_timers", test_35_coalesced_ring2_flushes_on_window_expiry_via_poll_timers);
    run_case("36_pi_boost_auto_switches_coalesced_ring_to_immediate_and_flushes", test_36_pi_boost_auto_switches_coalesced_ring_to_immediate_and_flushes);
    run_case("37_waiter_fast_path_when_seqno_already_retired", test_37_waiter_fast_path_when_seqno_already_retired);
    run_case("38_fixed_pool_128_capacity_zero_heap_and_reuse", test_38_fixed_pool_128_capacity_zero_heap_and_reuse);
    run_case("39_wakeup_fault_ring_drains_only_faulted_ring_waiters", test_39_wakeup_fault_ring_drains_only_faulted_ring_waiters);
    run_case("40_exact_p50_p95_p99_percentile_accuracy", test_40_exact_p50_p95_p99_percentile_accuracy);
    run_case("41_budget_alert_threshold_and_compositor_breach_count", test_41_budget_alert_threshold_and_compositor_breach_count);
    run_case("42_wakeup_callbacks_run_strictly_unlocked_and_reentrant", test_42_wakeup_callbacks_run_strictly_unlocked_and_reentrant);
    run_case("43_end_to_end_bridge_pi_and_latency_metrics", test_43_end_to_end_bridge_pi_and_latency_metrics);
    run_case("44_wakeup_uninitialized_and_range_errors", test_44_wakeup_uninitialized_and_range_errors);
    run_case("45_wakeup_irq_seqno_regression_rejected", test_45_wakeup_irq_seqno_regression_rejected);
    run_case("46_wakeup_injected_clock_callback_when_now_zero", test_46_wakeup_injected_clock_callback_when_now_zero);
    run_case("47_wakeup_zero_window_coalescing_literal", test_47_wakeup_zero_window_coalescing_literal);
    run_case("48_wakeup_window_wraparound_over_2048_samples", test_48_wakeup_window_wraparound_over_2048_samples);
    run_case("49_wakeup_set_budget_threshold_disables_alerts_when_zero", test_49_wakeup_set_budget_threshold_disables_alerts_when_zero);
    run_case("50_multiple_waiters_same_seqno_all_wake", test_50_multiple_waiters_same_seqno_all_wake);
    run_case("51_preempt_tick_detects_timeout_and_recovers", test_51_preempt_tick_detects_timeout_and_recovers);
    run_case("52_preempt_switch_done_fallbacks_if_target_faulted_midflight", test_52_preempt_switch_done_fallbacks_if_target_faulted_midflight);
    run_case("53_complete_auto_settles_on_subsequent_submit", test_53_complete_auto_settles_on_subsequent_submit);
    run_case("54_all_four_rings_strict_priority_drain_order", test_54_all_four_rings_strict_priority_drain_order);
    run_case("55_null_bridge_helpers_safe", test_55_null_bridge_helpers_safe);
    run_case("56_120hz_frame_burst_compositor_under_500us_budget", test_56_120hz_frame_burst_compositor_under_500us_budget);
    run_case("57_tsan_8_thread_concurrent_stress", [&]() {
        test_57_tsan_8_thread_concurrent_stress(stress_rounds);
    });

    std::printf("PASS test_kmd_phase5_preempt: %u independent cases, %" PRIu64 " checks (stress_rounds=%u)\n",
                g_cases_passed, g_checks.load(), stress_rounds);
    return EXIT_SUCCESS;
}
