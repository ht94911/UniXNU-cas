/* SPDX-License-Identifier: MIT */
#ifndef KERNEL
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#endif

#include "a840_xnu_system_bridge.h"

#include <string.h>

#ifdef KERNEL
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#else
#include <errno.h>
#include <time.h>
#endif

namespace {

constexpr uint64_t kNsPerSec = UINT64_C(1000000000);
constexpr uint64_t kAonClockHz = A840_PREEMPT_AON_CLOCK_HZ;
constexpr uint64_t kHostPhysBase = UINT64_C(4294967296); /* 4 GiB */
constexpr uint64_t kDefaultMaxBoBytes = UINT64_C(2147483648); /* 2 GiB */
constexpr uint64_t kDefaultVaStart = UINT64_C(1048576); /* 1 MiB */
constexpr uint64_t kDefaultVaSize = (UINT64_C(1) << 40u); /* 1 TiB */
constexpr uint32_t kDefaultMdssMaxDim = 8192u;
constexpr uint32_t kDefaultMdssFormats[] = {
    70u,  /* RGBA8Unorm */
    71u,  /* RGBA8Unorm_sRGB */
    80u,  /* BGRA8Unorm */
    81u,  /* BGRA8Unorm_sRGB */
    90u,  /* RGB10A2Unorm */
    94u,  /* BGR10A2Unorm */
    115u  /* RGBA16Float */
};

constexpr uint32_t kProdSelectorAuditList[26] = {
    1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u,
    11u, 12u, 13u, 14u, 15u, 16u, 17u, 18u, 19u, 20u,
    21u, 22u, 23u, 24u, 25u, 26u
};

constexpr uint32_t kCoreUcCapsMask =
    static_cast<uint32_t>(
        NativeAdrenoParamsV2HostImport |
        NativeAdrenoParamsV2OwnedCoherent |
        NativeAdrenoParamsV2MemoryPressure |
        NativeAdrenoParamsV2SubmitBatch |
        NativeAdrenoParamsV3TimelineWaitAny |
        NativeAdrenoParamsV3SharedZeroPage |
        NativeAdrenoParamsV3SharedTier0Pack |
        NativeAdrenoParamsV3WriterPromise |
        NativeAdrenoParamsV3UbwcSurfaceMeta);

#ifndef KERNEL
uint64_t hostMonotonicNs()
{
    struct timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0u;
    return static_cast<uint64_t>(ts.tv_sec) * kNsPerSec +
           static_cast<uint64_t>(ts.tv_nsec);
}
#endif

void copyCStringBounded(char *dst, size_t dst_cap, const char *src)
{
    if (dst == nullptr || dst_cap == 0u)
        return;
    dst[0] = '\0';
    if (src == nullptr)
        return;
    size_t i = 0u;
    while (i + 1u < dst_cap && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

uint32_t roleDefaultPriority(bool is_backboardd)
{
    return is_backboardd ? A840_PRIORITY_COMPOSITOR : A840_PRIORITY_NORMAL;
}

uint32_t roleQuotaPriority(bool is_backboardd)
{
    return is_backboardd ? A840_KMD_PRIO_COMPOSITOR : A840_KMD_PRIO_FOREGROUND_APP;
}

} /* namespace */

/* ============================================================================
 * Platform Sync Primitives (#ifdef KERNEL vs POSIX / Darwin Host)
 * ============================================================================ */

bool A840XnuSystemBridge::BridgeLock::init()
{
#ifdef KERNEL
    lock = IOLockAlloc();
    return lock != nullptr;
#else
    if (pthread_mutex_init(&lock, nullptr) != 0)
        return false;
    ready = true;
    return true;
#endif
}

void A840XnuSystemBridge::BridgeLock::destroy()
{
#ifdef KERNEL
    if (lock != nullptr) {
        IOLockFree(lock);
        lock = nullptr;
    }
#else
    if (ready) {
        (void)pthread_mutex_destroy(&lock);
        ready = false;
    }
#endif
}

void A840XnuSystemBridge::BridgeLock::acquire()
{
#ifdef KERNEL
    IOLockLock(lock);
#else
    (void)pthread_mutex_lock(&lock);
#endif
}

void A840XnuSystemBridge::BridgeLock::release()
{
#ifdef KERNEL
    IOLockUnlock(lock);
#else
    (void)pthread_mutex_unlock(&lock);
#endif
}

bool A840XnuSystemBridge::BridgeCondLock::init()
{
#ifdef KERNEL
    lock = IOLockAlloc();
    event_token = 0u;
    return lock != nullptr;
#else
    pthread_condattr_t attr;
    if (pthread_mutex_init(&lock, nullptr) != 0)
        return false;
    if (pthread_condattr_init(&attr) != 0) {
        (void)pthread_mutex_destroy(&lock);
        return false;
    }
#if !defined(__APPLE__)
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0 ||
        pthread_cond_init(&cond, &attr) != 0) {
#else
    if (pthread_cond_init(&cond, &attr) != 0) {
#endif
        (void)pthread_condattr_destroy(&attr);
        (void)pthread_mutex_destroy(&lock);
        return false;
    }
    (void)pthread_condattr_destroy(&attr);
    ready = true;
    return true;
#endif
}

void A840XnuSystemBridge::BridgeCondLock::destroy()
{
#ifdef KERNEL
    if (lock != nullptr) {
        IOLockFree(lock);
        lock = nullptr;
    }
#else
    if (ready) {
        (void)pthread_cond_destroy(&cond);
        (void)pthread_mutex_destroy(&lock);
        ready = false;
    }
#endif
}

void A840XnuSystemBridge::BridgeCondLock::acquire()
{
#ifdef KERNEL
    IOLockLock(lock);
#else
    (void)pthread_mutex_lock(&lock);
#endif
}

void A840XnuSystemBridge::BridgeCondLock::release()
{
#ifdef KERNEL
    IOLockUnlock(lock);
#else
    (void)pthread_mutex_unlock(&lock);
#endif
}

void A840XnuSystemBridge::BridgeCondLock::waitIndefinite()
{
#ifdef KERNEL
    (void)IOLockSleep(lock, &event_token, THREAD_UNINT);
#else
    (void)pthread_cond_wait(&cond, &lock);
#endif
}

int A840XnuSystemBridge::BridgeCondLock::waitTimed(
    uint64_t deadline_ns, int infinite, uint64_t now_ns)
{
#ifdef KERNEL
    if (infinite != 0) {
        const int wr = IOLockSleep(lock, &event_token, THREAD_INTERRUPTIBLE);
        return (wr == THREAD_AWAKENED) ? 0 : 1;
    }
    if (deadline_ns <= now_ns)
        return 1;
    const uint64_t delta_ns = deadline_ns - now_ns;
    uint64_t abs_deadline = 0u;
    clock_interval_to_deadline(delta_ns, 1u, &abs_deadline);
    const int wr = IOLockSleepDeadline(
        lock, &event_token, abs_deadline, THREAD_INTERRUPTIBLE);
    return (wr == THREAD_TIMED_OUT) ? 1 : 0;
#else
    if (infinite != 0)
        return (pthread_cond_wait(&cond, &lock) == 0) ? 0 : 1;
    if (deadline_ns <= now_ns)
        return 1;
    const uint64_t rem_ns = deadline_ns - now_ns;
#if defined(__APPLE__)
    struct timespec rel{};
    rel.tv_sec = static_cast<time_t>(rem_ns / kNsPerSec);
    rel.tv_nsec = static_cast<long>(rem_ns % kNsPerSec);
    const int rc = pthread_cond_timedwait_relative_np(&cond, &lock, &rel);
    return (rc == ETIMEDOUT) ? 1 : 0;
#else
    const uint64_t real_now = hostMonotonicNs();
    const uint64_t target_ns = (UINT64_MAX - real_now > rem_ns) ?
                               (real_now + rem_ns) : UINT64_MAX;
    struct timespec abs_ts{};
    abs_ts.tv_sec = static_cast<time_t>(target_ns / kNsPerSec);
    abs_ts.tv_nsec = static_cast<long>(target_ns % kNsPerSec);
    const int rc = pthread_cond_timedwait(&cond, &lock, &abs_ts);
    return (rc == ETIMEDOUT) ? 1 : 0;
#endif
#endif
}

void A840XnuSystemBridge::BridgeCondLock::wakeAll()
{
#ifdef KERNEL
    IOLockWakeup(lock, &event_token, false);
#else
    (void)pthread_cond_broadcast(&cond);
#endif
}

/* ============================================================================
 * Static Callback Adapters
 * ============================================================================ */

void A840XnuSystemBridge::cbLock(void *cookie)
{
    static_cast<BridgeLock *>(cookie)->acquire();
}

void A840XnuSystemBridge::cbUnlock(void *cookie)
{
    static_cast<BridgeLock *>(cookie)->release();
}

int A840XnuSystemBridge::cbWait(void *cookie, uint64_t deadline_ns, int infinite)
{
    auto *self = static_cast<A840XnuSystemBridge *>(cookie);
    return self->wait_any_sync_.waitTimed(deadline_ns, infinite, self->currentNowNs());
}

void A840XnuSystemBridge::cbWakeAll(void *cookie)
{
    auto *self = static_cast<A840XnuSystemBridge *>(cookie);
    self->wait_any_sync_.wakeAll();
}

uint64_t A840XnuSystemBridge::cbNowNs(void *cookie)
{
    auto *self = static_cast<A840XnuSystemBridge *>(cookie);
    return self->currentNowNs();
}

uint64_t A840XnuSystemBridge::cbReadTicks(void *cookie)
{
    auto *self = static_cast<A840XnuSystemBridge *>(cookie);
    return self->currentTicks();
}

A840KmdStatus A840XnuSystemBridge::cbZeroAlloc(
    void *cookie, uint64_t bytes, uint64_t *out_phys,
    void **out_cpu, void **out_handle)
{
    auto *self = static_cast<A840XnuSystemBridge *>(cookie);
    if (self == nullptr || bytes != A840_KMD_ZERO_PAGE_BYTES || out_phys == nullptr)
        return A840_KMD_INVALID_ARG;
    memset(self->builtin_zp_.storage, 0, sizeof(self->builtin_zp_.storage));
    self->builtin_zp_.allocated = 1u;
    if (self->builtin_zp_.backing_phys == 0u)
        self->builtin_zp_.backing_phys = kHostPhysBase;
    *out_phys = self->builtin_zp_.backing_phys;
    if (out_cpu != nullptr)
        *out_cpu = self->builtin_zp_.storage;
    if (out_handle != nullptr)
        *out_handle = &self->builtin_zp_;
    return A840_KMD_OK;
}

void A840XnuSystemBridge::cbZeroFree(
    void *cookie, uint64_t phys, void *handle, uint64_t bytes)
{
    auto *self = static_cast<A840XnuSystemBridge *>(cookie);
    (void)phys;
    (void)handle;
    (void)bytes;
    if (self != nullptr)
        self->builtin_zp_.allocated = 0u;
}

A840KmdStatus A840XnuSystemBridge::cbZeroMapRo(
    void *cookie, uint64_t client_id, void *smmu_ctx, uint64_t gpu_va,
    uint64_t phys_addr, uint64_t bytes, uint32_t permissions)
{
    auto *self = static_cast<A840XnuSystemBridge *>(cookie);
    (void)client_id;
    (void)smmu_ctx;
    (void)gpu_va;
    (void)phys_addr;
    (void)bytes;
    if (self == nullptr)
        return A840_KMD_INVALID_ARG;
    if (permissions != A840_KMD_SMMU_PERM_READ_ONLY)
        return A840_KMD_PERMISSION_DENIED;
    self->builtin_zp_.mapped_count++;
    return A840_KMD_OK;
}

A840KmdStatus A840XnuSystemBridge::cbZeroUnmap(
    void *cookie, uint64_t client_id, void *smmu_ctx, uint64_t gpu_va,
    uint64_t bytes)
{
    auto *self = static_cast<A840XnuSystemBridge *>(cookie);
    (void)client_id;
    (void)smmu_ctx;
    (void)gpu_va;
    (void)bytes;
    if (self != nullptr && self->builtin_zp_.mapped_count > 0u)
        self->builtin_zp_.mapped_count--;
    return A840_KMD_OK;
}

A840KmdStatus A840XnuSystemBridge::cbBoSizeLookup(
    void *cookie, uint64_t client_id, uint64_t bo_handle, uint64_t *out_bo_bytes)
{
    auto *self = static_cast<A840XnuSystemBridge *>(cookie);
    if (self == nullptr || out_bo_bytes == nullptr)
        return A840_KMD_INVALID_ARG;
    A840KmdBoDesc desc{};
    const A840KmdStatus st = a840_kmd_bo_pool_query_bo(
        &self->bo_pool_, client_id, bo_handle, &desc);
    if (st != A840_KMD_OK)
        return st;
    *out_bo_bytes = desc.requested_bytes;
    return A840_KMD_OK;
}

A840KmdStatus A840XnuSystemBridge::cbPackAcquire(
    void *cookie, void *source, A840KmdPackImage *out_img)
{
    (void)cookie;
    auto *slot = static_cast<PackSourceSlot *>(source);
    if (slot == nullptr || out_img == nullptr || slot->active == 0u)
        return A840_KMD_INVALID_ARG;
    out_img->source = slot;
    out_img->identity = slot->identity;
    return A840_KMD_OK;
}

void A840XnuSystemBridge::cbPackRelease(void *cookie, void *retained_source)
{
    (void)cookie;
    auto *slot = static_cast<PackSourceSlot *>(retained_source);
    if (slot != nullptr)
        slot->active = 0u;
}

A840KmdStatus A840XnuSystemBridge::cbPackWireRo(
    void *cookie, const A840KmdPackImage *img, uint32_t perm, void **out_backing)
{
    (void)cookie;
    if (img == nullptr || out_backing == nullptr)
        return A840_KMD_INVALID_ARG;
    if (perm != A840_KMD_SMMU_PERM_READ_ONLY)
        return A840_KMD_PERMISSION_DENIED;
    *out_backing = img->source;
    return A840_KMD_OK;
}

void A840XnuSystemBridge::cbPackUnwire(void *cookie, void *backing)
{
    (void)cookie;
    (void)backing;
}

A840KmdStatus A840XnuSystemBridge::cbPackBindRo(
    void *cookie, void *backing, uint64_t client, uint32_t perm,
    uint32_t map_cpu, A840KmdPackViews *views)
{
    auto *self = static_cast<A840XnuSystemBridge *>(cookie);
    (void)backing;
    if (self == nullptr || views == nullptr || client == 0u)
        return A840_KMD_INVALID_ARG;
    if (perm != A840_KMD_SMMU_PERM_READ_ONLY)
        return A840_KMD_PERMISSION_DENIED;
    if (views->gpu_va == 0u) {
        views->gpu_va = self->next_pack_gpu_va_ +
                        ((client & UINT64_C(63)) * UINT64_C(65536));
    }
    if (map_cpu != 0u && views->cpu_va == 0u) {
        views->cpu_va = self->next_pack_cpu_va_ +
                        ((client & UINT64_C(63)) * UINT64_C(65536));
    }
    return A840_KMD_OK;
}

A840KmdStatus A840XnuSystemBridge::cbPackRevoke(
    void *cookie, void *backing, uint64_t client)
{
    (void)cookie;
    (void)backing;
    (void)client;
    return A840_KMD_OK;
}

A840KmdStatus A840XnuSystemBridge::cbPreemptDoorbell(
    void *cookie, uint32_t ring_id, uint32_t wptr,
    uint64_t seqno, uint32_t reg_wptr_offset)
{
    (void)cookie;
    (void)ring_id;
    (void)wptr;
    (void)seqno;
    (void)reg_wptr_offset;
    return A840_KMD_OK;
}

A840KmdStatus A840XnuSystemBridge::cbPreemptTrigger(
    void *cookie, uint32_t from_ring, uint32_t to_ring,
    uint32_t preempt_level, uint64_t preempt_record_va,
    uint64_t preempt_record_bytes, uint32_t reg_ctx_switch_cntl)
{
    (void)cookie;
    (void)from_ring;
    (void)to_ring;
    (void)preempt_level;
    (void)preempt_record_va;
    (void)preempt_record_bytes;
    (void)reg_ctx_switch_cntl;
    return A840_KMD_OK;
}

void A840XnuSystemBridge::cbPreemptPiBoost(
    void *cookie, uint32_t ring_id, uint32_t boosted,
    uint32_t boosted_by_ring, uint64_t boost_until_seqno, uint64_t now_ns)
{
    auto *self = static_cast<A840XnuSystemBridge *>(cookie);
    if (self == nullptr)
        return;
    a840_kmd_wakeup_bridge_pi_boost(
        &self->wakeup_, ring_id, boosted, boosted_by_ring,
        boost_until_seqno, now_ns);
}

void A840XnuSystemBridge::cbPreemptLatency(
    void *cookie, uint32_t latency_kind, uint32_t ring_id,
    uint64_t latency_ns, uint64_t now_ns)
{
    auto *self = static_cast<A840XnuSystemBridge *>(cookie);
    if (self == nullptr)
        return;
    a840_kmd_wakeup_bridge_latency(
        &self->wakeup_, latency_kind, ring_id, latency_ns, now_ns);
}

A840KmdStatus A840XnuSystemBridge::cbGmuHfiSend(
    void *cookie, const A840GmuHfiRequest *req)
{
    (void)cookie;
    if (req == nullptr)
        return A840_KMD_INVALID_ARG;
    return A840_KMD_OK;
}

void A840XnuSystemBridge::cbMdssBoostHint(
    void *cookie, const A840MdssBoostHint *hint)
{
    auto *self = static_cast<A840XnuSystemBridge *>(cookie);
    if (self == nullptr || hint == nullptr)
        return;
    a840_gmu_dcvs_bridge_mdss_boost(&self->gmu_dcvs_, hint);
}

A840KmdStatus A840XnuSystemBridge::cbQrHwChannelReset(
    void *cookie, uint64_t client_id, uint64_t queue_id,
    uint64_t recovery_generation, uint32_t fault_kind)
{
    auto *self = static_cast<A840XnuSystemBridge *>(cookie);
    (void)client_id;
    (void)queue_id;
    (void)recovery_generation;
    (void)fault_kind;
    if (self == nullptr)
        return A840_KMD_INVALID_ARG;
    self->stats_lock_.acquire();
    self->total_recoveries_++;
    self->stats_lock_.release();
    return A840_KMD_OK;
}

/* ============================================================================
 * Status Mapping & Clock Helpers
 * ============================================================================ */

IOReturn A840XnuSystemBridge::kmdStatusToIOReturn(A840KmdStatus status)
{
    switch (status) {
    case A840_KMD_OK:
        return kIOReturnSuccess;
    case A840_KMD_INVALID_ARG:
    case A840_KMD_RANGE:
    case A840_KMD_ALIGNMENT:
        return kIOReturnBadArgument;
    case A840_KMD_PERMISSION_DENIED:
        return kIOReturnNotPermitted;
    case A840_KMD_CAPACITY:
        return kIOReturnNoResources;
    case A840_KMD_NOT_FOUND:
        return kIOReturnNotFound;
    case A840_KMD_STATE_CONFLICT:
    case A840_KMD_STACK_KEY_MISMATCH:
        return kIOReturnBusy;
    case A840_KMD_UNSUPPORTED:
        return kIOReturnUnsupported;
    case A840_KMD_POISONED:
    case A840_KMD_DISCONNECTED:
        return kIOReturnAborted;
    case A840_KMD_IO_ERROR:
        return kIOReturnIOError;
    case A840_KMD_NO_MEMORY:
        return kIOReturnNoMemory;
    default:
        return kIOReturnError;
    }
}

uint64_t A840XnuSystemBridge::currentNowNs() const
{
    if (config_.now_ns_cb != nullptr)
        return config_.now_ns_cb(config_.clock_cookie);
#ifdef KERNEL
    uint64_t abs_time = mach_continuous_time();
    uint64_t ns = 0u;
    absolutetime_to_nanoseconds(abs_time, &ns);
    return ns;
#else
    return hostMonotonicNs();
#endif
}

uint64_t A840XnuSystemBridge::currentTicks() const
{
    if (config_.read_ticks_cb != nullptr)
        return config_.read_ticks_cb(config_.clock_cookie);
    const uint64_t ns = currentNowNs();
    return (ns / kNsPerSec) * kAonClockHz +
           ((ns % kNsPerSec) * kAonClockHz) / kNsPerSec;
}

/* ============================================================================
 * Lifecycle: init / qualify_production / shutdown
 * ============================================================================ */

A840XnuSystemBridge::~A840XnuSystemBridge()
{
    (void)shutdown();
}

IOReturn A840XnuSystemBridge::init(const Config &config)
{
    if (initialized_)
        return kIOReturnBusy;

    if (config.abi_mode != A840_SYS_ABI_MODE_V2_COMPAT &&
        config.abi_mode != A840_SYS_ABI_MODE_V3_NATIVE)
        return kIOReturnBadArgument;

    if (config.zero_page_default_va == 0u ||
        (config.zero_page_default_va % A840_KMD_ZERO_PAGE_ALIGN) != 0u ||
        config.zero_page_default_va >= A840_KMD_GPU_VA_LIMIT)
        return kIOReturnBadArgument;

    if (config.preempt_record_bytes < A840_PREEMPT_RECORD_MIN_BYTES ||
        (config.preempt_record_bytes % A840_PREEMPT_RECORD_ALIGN_BYTES) != 0u ||
        config.preempt_record_base_va == 0u ||
        (config.preempt_record_base_va % A840_PREEMPT_RECORD_ALIGN_BYTES) != 0u)
        return kIOReturnBadArgument;

    /* Reject any legacy compat or unknown flags in v3 mode fail-closed. */
    if (config.abi_mode == A840_SYS_ABI_MODE_V3_NATIVE && config.cap_flags != 0u) {
        if (a840_kmd_v3_reject_legacy_turnip_flags(config.cap_flags) != A840_KMD_OK)
            return kIOReturnUnsupported;
    }

    config_ = config;
    abi_mode_ = config.abi_mode;
    qualified_ = false;
    tier0_pack_published_ = false;
    next_client_id_ = 1u;
    next_cpu_token_ = 1u;
    next_import_bo_handle_ = UINT64_C(1048576);
    active_pack_slot_ = 0u;
    next_pack_slot_ = 0u;

    builtin_zp_ = BuiltinZeroPageState{};
    builtin_zp_.backing_phys = kHostPhysBase;
    builtin_zp_.next_phys = kHostPhysBase + UINT64_C(65536);
    for (size_t i = 0u; i < A840_SYS_MAX_CLIENTS; ++i) clients_[i] = ClientRecord{};
    for (size_t i = 0u; i < A840_SYS_MAX_METADATA_SLOTS; ++i) metadata_slots_[i] = MetadataSlot{};
    for (size_t i = 0u; i < A840_SYS_MAX_METADATA_SLOTS; ++i) cpu_leases_[i] = CpuMapLeaseSlot{};
    for (size_t i = 0u; i < A840_SYS_MAX_PENDING_RING_TRACK; ++i) ring_tracks_[i] = RingTrackEntry{};
    for (size_t i = 0u; i < 4u; ++i) pack_sources_[i] = PackSourceSlot{};
    memset(&last_qual_report_, 0, sizeof(last_qual_report_));

    total_dispatches_ = 0u;
    total_v2_dispatches_ = 0u;
    total_v3_dispatches_ = 0u;
    total_submits_ = 0u;
    total_ring0_submits_ = 0u;
    total_app_submits_ = 0u;
    total_fence_irqs_ = 0u;
    total_vblanks_ = 0u;
    total_faults_isolated_ = 0u;
    total_ib_rejects_ = 0u;
    total_role_denials_ = 0u;
    total_ubwc6_imports_ = 0u;
    total_recoveries_ = 0u;

    /* Initialize MDSS capabilities copy (zero heap). */
    memset(mdss_formats_, 0, sizeof(mdss_formats_));
    memset(&active_mdss_caps_, 0, sizeof(active_mdss_caps_));
    if (config_.mdss_caps != nullptr && config_.mdss_caps->format_count > 0u &&
        config_.mdss_caps->formats != nullptr) {
        size_t cnt = config_.mdss_caps->format_count;
        if (cnt > A840_SYS_MAX_MDSS_FORMATS)
            cnt = A840_SYS_MAX_MDSS_FORMATS;
        for (size_t i = 0u; i < cnt; ++i)
            mdss_formats_[i] = config_.mdss_caps->formats[i];
        active_mdss_caps_ = *config_.mdss_caps;
        active_mdss_caps_.formats = mdss_formats_;
        active_mdss_caps_.format_count = cnt;
    } else {
        const size_t cnt = sizeof(kDefaultMdssFormats) / sizeof(kDefaultMdssFormats[0]);
        for (size_t i = 0u; i < cnt; ++i)
            mdss_formats_[i] = kDefaultMdssFormats[i];
        active_mdss_caps_.flags = A840_MDSS_UBWC6;
        active_mdss_caps_.max_width = kDefaultMdssMaxDim;
        active_mdss_caps_.max_height = kDefaultMdssMaxDim;
        active_mdss_caps_.base_alignment = A840_KMD_V3_FREEZE_PAGE_BYTES;
        active_mdss_caps_.pitch_alignment = A840_ABI_V3_META_PITCH_ALIGNMENT;
        active_mdss_caps_.formats = mdss_formats_;
        active_mdss_caps_.format_count = cnt;
    }

    /* Initialize all dedicated synchronization primitives. */
    if (!clients_sync_.init() || !wait_any_sync_.init() || !promise_sync_.init() ||
        !stats_lock_.init() || !surface_lock_.init() || !pack_lock_.init() ||
        !display_lock_.init() || !sparse_lock_.init() || !bo_pool_lock_.init() ||
        !quota_lock_.init() || !preempt_lock_.init() || !wakeup_lock_.init() ||
        !gmu_lock_.init() || !mdss_lock_.init() || !freeze_lock_.init() ||
        !aux_lock_.init()) {
        (void)shutdown();
        return kIOReturnNoMemory;
    }

    /* 1. Freeze Policy */
    freeze_sync_ops_ = { &freeze_lock_, &cbLock, &cbUnlock };
    if (a840_kmd_v3_freeze_policy_init(&freeze_policy_, &freeze_sync_ops_) != A840_KMD_OK) {
        (void)shutdown();
        return kIOReturnError;
    }

    init_mask_ |= (UINT32_C(1) << 0u);

    /* 2. UserClient v3 (ZeroPage + WaitAny + FaultCapture) */
    wait_backend_.cookie = this;
    wait_backend_.lock = [](void *c) {
        static_cast<A840XnuSystemBridge *>(c)->wait_any_sync_.acquire();
    };
    wait_backend_.unlock = [](void *c) {
        static_cast<A840XnuSystemBridge *>(c)->wait_any_sync_.release();
    };
    wait_backend_.wait = &cbWait;
    wait_backend_.wake_all = &cbWakeAll;
    wait_backend_.now_ns = &cbNowNs;
    wait_backend_.read_ticks = &cbReadTicks;

    A840KmdUserClientConfig uc_cfg{};
    if (abi_mode_ == A840_SYS_ABI_MODE_V2_COMPAT) {
        advertised_caps_ = static_cast<uint32_t>(A840_KMD_V3_PROD_V2_CAP_MASK);
        uc_cfg.cap_flags = advertised_caps_;
    } else {
        const uint32_t full_prod = static_cast<uint32_t>(A840_KMD_V3_PROD_CAP_MASK);
        advertised_caps_ = (config_.cap_flags != 0u) ? config_.cap_flags : full_prod;
        uc_cfg.cap_flags = advertised_caps_ & kCoreUcCapsMask;
    }
    uc_cfg.zero_page_default_va = config_.zero_page_default_va;
    if (config_.smmu_ops != nullptr) {
        uc_cfg.smmu_ops = *config_.smmu_ops;
    } else {
        uc_cfg.smmu_ops.cookie = this;
        uc_cfg.smmu_ops.alloc_backing_page = &cbZeroAlloc;
        uc_cfg.smmu_ops.free_backing_page = &cbZeroFree;
        uc_cfg.smmu_ops.map_client_ro = &cbZeroMapRo;
        uc_cfg.smmu_ops.unmap_client = &cbZeroUnmap;
    }
    uc_cfg.wait_backend = &wait_backend_;
    uc_cfg.bo_size_lookup = &cbBoSizeLookup;
    uc_cfg.bo_size_cookie = this;

    if (a840_kmd_userclient_v3_init(&uc_, &uc_cfg) != A840_KMD_OK) {
        (void)shutdown();
        return kIOReturnError;
    }

    init_mask_ |= (UINT32_C(1) << 1u);

    /* 3. UBWC 6.0 Surface Manager */
    surface_ops_ = { &surface_lock_, &cbLock, &cbUnlock };
    if (a840_kmd_surface_init(&surface_mgr_, &surface_ops_) != A840_KMD_OK) {
        (void)shutdown();
        return kIOReturnError;
    }

    init_mask_ |= (UINT32_C(1) << 2u);

    /* 4. Tier-0 Shared RO Pack Manager */
    pack_ops_.cookie = this;
    pack_ops_.lock = [](void *c) {
        static_cast<A840XnuSystemBridge *>(c)->pack_lock_.acquire();
    };
    pack_ops_.unlock = [](void *c) {
        static_cast<A840XnuSystemBridge *>(c)->pack_lock_.release();
    };
    pack_ops_.acquire = &cbPackAcquire;
    pack_ops_.release = &cbPackRelease;
    pack_ops_.wire_ro = &cbPackWireRo;
    pack_ops_.unwire = &cbPackUnwire;
    pack_ops_.bind_ro = &cbPackBindRo;
    pack_ops_.revoke = &cbPackRevoke;
    if (a840_kmd_pack_init(&pack_mgr_, &pack_ops_) != A840_KMD_OK) {
        (void)shutdown();
        return kIOReturnError;
    }
    a840_kmd_pack_set_params(&pack_mgr_, advertised_caps_);

    init_mask_ |= (UINT32_C(1) << 3u);

    /* 5. WriterPromise Registry (linked to WaitAny + FaultCapture) */
    promise_sync_ops_.cookie = this;
    promise_sync_ops_.lock = [](void *c) {
        static_cast<A840XnuSystemBridge *>(c)->promise_sync_.acquire();
    };
    promise_sync_ops_.unlock = [](void *c) {
        static_cast<A840XnuSystemBridge *>(c)->promise_sync_.release();
    };
    promise_sync_ops_.wait = [](void *c, uint64_t deadline_ns, int infinite) {
        auto *self = static_cast<A840XnuSystemBridge *>(c);
        return self->promise_sync_.waitTimed(deadline_ns, infinite, self->currentNowNs());
    };
    promise_sync_ops_.wake_all = [](void *c) {
        static_cast<A840XnuSystemBridge *>(c)->promise_sync_.wakeAll();
    };
    promise_sync_ops_.now_ns = &cbNowNs;

    A840KmdPromiseConfig prom_cfg{};
    prom_cfg.sync_ops = &promise_sync_ops_;
    prom_cfg.wait_any = &uc_.wait_any;
    prom_cfg.fault_capture = &uc_.fault_capture;
    if (a840_kmd_writer_promise_init(&promise_reg_, &prom_cfg) != A840_KMD_OK) {
        (void)shutdown();
        return kIOReturnError;
    }

    init_mask_ |= (UINT32_C(1) << 4u);

    /* 6. Display Handoff Coordinator */
    display_sync_ops_ = { &display_lock_, &cbLock, &cbUnlock };
    A840KmdDisplayHandoffConfig dh_cfg{};
    dh_cfg.surface_mgr = &surface_mgr_;
    dh_cfg.promise_reg = &promise_reg_;
    dh_cfg.sync_ops = &display_sync_ops_;
    dh_cfg.commit_fn = config_.mdss_commit_fn;
    dh_cfg.commit_cookie = config_.mdss_commit_cookie;
    if (a840_kmd_display_handoff_init(&display_handoff_, &dh_cfg) != A840_KMD_OK) {
        (void)shutdown();
        return kIOReturnError;
    }

    init_mask_ |= (UINT32_C(1) << 5u);

    /* 7. Sparse VA Manager (linked to shared RO zero page) */
    sparse_sync_ops_ = { &sparse_lock_, &cbLock, &cbUnlock };
    A840KmdSparseConfig sp_cfg{};
    sp_cfg.zero_page_mgr = &uc_.zero_page;
    sp_cfg.sync_ops = &sparse_sync_ops_;
    if (a840_kmd_sparse_init(&sparse_mgr_, &sp_cfg) != A840_KMD_OK) {
        (void)shutdown();
        return kIOReturnError;
    }

    init_mask_ |= (UINT32_C(1) << 6u);

    /* 8. 7-Bucket Parked / Volatile BO Pool */
    bo_pool_sync_ops_.cookie = this;
    bo_pool_sync_ops_.lock = [](void *c) {
        static_cast<A840XnuSystemBridge *>(c)->bo_pool_lock_.acquire();
    };
    bo_pool_sync_ops_.unlock = [](void *c) {
        static_cast<A840XnuSystemBridge *>(c)->bo_pool_lock_.release();
    };
    bo_pool_sync_ops_.now_ns = &cbNowNs;
    A840KmdBoPoolConfig bp_cfg{};
    bp_cfg.zero_page_mgr = &uc_.zero_page;
    bp_cfg.sync_ops = &bo_pool_sync_ops_;
    bp_cfg.scrub_on_park = 1u;
    if (a840_kmd_bo_pool_init(&bo_pool_, &bp_cfg) != A840_KMD_OK) {
        (void)shutdown();
        return kIOReturnError;
    }

    init_mask_ |= (UINT32_C(1) << 7u);

    /* 9. Multi-Client Quota & Single-Channel Fault Recovery */
    quota_sync_ops_.cookie = this;
    quota_sync_ops_.lock = [](void *c) {
        static_cast<A840XnuSystemBridge *>(c)->quota_lock_.acquire();
    };
    quota_sync_ops_.unlock = [](void *c) {
        static_cast<A840XnuSystemBridge *>(c)->quota_lock_.release();
    };
    quota_sync_ops_.now_ns = &cbNowNs;

    memset(&quota_recovery_ops_, 0, sizeof(quota_recovery_ops_));
    quota_recovery_ops_.cookie = this;
    quota_recovery_ops_.hw_channel_reset = &cbQrHwChannelReset;

    A840KmdQuotaRecoveryConfig qr_cfg{};
    qr_cfg.global_soft_limit_bytes = config_.global_soft_quota_bytes;
    qr_cfg.global_hard_limit_bytes = config_.global_hard_quota_bytes;
    qr_cfg.compositor_reserve_bytes = config_.compositor_reserve_bytes;
    qr_cfg.max_recoveries_before_ban = config_.max_recoveries_before_ban;
    qr_cfg.sync_bo_pool_inflight = 1u;
    qr_cfg.bo_pool = &bo_pool_;
    qr_cfg.sparse_mgr = &sparse_mgr_;
    qr_cfg.fault_capture = &uc_.fault_capture;
    qr_cfg.sync_ops = &quota_sync_ops_;
    qr_cfg.recovery_ops = &quota_recovery_ops_;
    if (a840_kmd_qr_init(&quota_recovery_, &qr_cfg) != A840_KMD_OK) {
        (void)shutdown();
        return kIOReturnError;
    }

    init_mask_ |= (UINT32_C(1) << 8u);

    /* 10. Wakeup Latency Engine */
    wakeup_sync_ops_.cookie = this;
    wakeup_sync_ops_.lock = [](void *c) {
        static_cast<A840XnuSystemBridge *>(c)->wakeup_lock_.acquire();
    };
    wakeup_sync_ops_.unlock = [](void *c) {
        static_cast<A840XnuSystemBridge *>(c)->wakeup_lock_.release();
    };
    wakeup_sync_ops_.now_ns = &cbNowNs;
    A840WakeupConfig wk_cfg{};
    wk_cfg.sync_ops = &wakeup_sync_ops_;
    if (a840_kmd_wakeup_init(&wakeup_, &wk_cfg) != A840_KMD_OK) {
        (void)shutdown();
        return kIOReturnError;
    }

    init_mask_ |= (UINT32_C(1) << 9u);

    /* 11. 4-Ring Preemption Scheduler (wired to Wakeup Latency Engine) */
    preempt_sync_ops_ = { &preempt_lock_, &cbLock, &cbUnlock };
    memset(&preempt_hw_ops_, 0, sizeof(preempt_hw_ops_));
    preempt_hw_ops_.cookie = this;
    preempt_hw_ops_.write_doorbell = &cbPreemptDoorbell;
    preempt_hw_ops_.trigger_preempt = &cbPreemptTrigger;
    preempt_hw_ops_.on_pi_boost_change = &cbPreemptPiBoost;
    preempt_hw_ops_.on_latency_sample = &cbPreemptLatency;

    A840PreemptConfig pr_cfg{};
    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        pr_cfg.rings[r].preempt_record_va =
            config_.preempt_record_base_va +
            static_cast<uint64_t>(r) * config_.preempt_record_bytes;
        pr_cfg.rings[r].preempt_record_bytes = config_.preempt_record_bytes;
        pr_cfg.rings[r].preempt_level = (r == A840_RING_0_COMPOSITOR)
            ? A840_PREEMPT_LEVEL_L1B_DRAW_BOUNDARY
            : A840_PREEMPT_LEVEL_L1A_BIN_BOUNDARY;
    }
    pr_cfg.sync_ops = &preempt_sync_ops_;
    pr_cfg.hw_ops = &preempt_hw_ops_;
    if (a840_kmd_preempt_init(&preempt_, &pr_cfg) != A840_KMD_OK) {
        (void)shutdown();
        return kIOReturnError;
    }

    init_mask_ |= (UINT32_C(1) << 10u);

    /* 12. 18-level GPU / 4-level GMU DCVS Governor */
    gmu_sync_ops_ = { &gmu_lock_, &cbLock, &cbUnlock };
    memset(&gmu_hfi_ops_, 0, sizeof(gmu_hfi_ops_));
    gmu_hfi_ops_.cookie = this;
    gmu_hfi_ops_.send_dcvs_request = &cbGmuHfiSend;

    A840GmuDcvsConfig dcvs_cfg{};
    dcvs_cfg.initial_gpu_level = config_.initial_gpu_opp_level;
    dcvs_cfg.use_explicit_init_level = (config_.initial_gpu_opp_level == 0u) ? 1u : 0u;
    dcvs_cfg.sync_ops = &gmu_sync_ops_;
    dcvs_cfg.hfi_ops = &gmu_hfi_ops_;
    if (a840_gmu_dcvs_init(&gmu_dcvs_, &dcvs_cfg) != A840_KMD_OK) {
        (void)shutdown();
        return kIOReturnError;
    }

    init_mask_ |= (UINT32_C(1) << 11u);

    /* 13. MDSS 120Hz Sync Coordinator (closed-loop boost into GMU DCVS) */
    mdss_sync_ops_ = { &mdss_lock_, &cbLock, &cbUnlock };
    memset(&mdss_callbacks_, 0, sizeof(mdss_callbacks_));
    mdss_callbacks_.cookie = this;
    mdss_callbacks_.on_boost_hint = &cbMdssBoostHint;

    A840MdssSyncConfig mdss_cfg{};
    mdss_cfg.nominal_period_ns = A840_MDSS_PERIOD_120HZ_NS;
    mdss_cfg.initial_vsync_ns = config_.initial_vsync_ns;
    mdss_cfg.has_initial_vsync = (config_.initial_vsync_ns != 0u) ? 1u : 0u;
    mdss_cfg.sync_ops = &mdss_sync_ops_;
    mdss_cfg.callbacks = &mdss_callbacks_;
    if (a840_mdss_sync_init(&mdss_sync_, &mdss_cfg) != A840_KMD_OK) {
        (void)shutdown();
        return kIOReturnError;
    }

    init_mask_ |= (UINT32_C(1) << 12u);
    initialized_ = true;
    return kIOReturnSuccess;
}

void A840XnuSystemBridge::populateDefaultQualInput(A840KmdProdQualInput *out_in) const
{
    memset(out_in, 0, sizeof(*out_in));
    out_in->abi_version = (abi_mode_ == A840_SYS_ABI_MODE_V3_NATIVE)
                              ? NativeAdrenoAbiVersionV3
                              : NativeAdrenoAbiVersionV2;
    out_in->envelope_bytes = NativeAdrenoAbiEnvelopeBytes;
    out_in->advertised_cap_flags = advertised_caps_;

    out_in->chip_id = A840_KAANAPALI_CHIP_ID;
    out_in->gmem_bytes = A840_KAANAPALI_GMEM_BYTES;
    out_in->highest_bank_bit = A840_KAANAPALI_HIGHEST_BANK_BIT;
    out_in->ubwc_version = A840_KMD_V3_FREEZE_UBWC_VERSION;
    out_in->smmu_page_shift = A840_KMD_V3_FREEZE_SMMU_PAGE_SHIFT;

    out_in->zero_page_ready = 1u;
    out_in->zero_page_readonly = 1u;
    out_in->zero_page_gpu_va = config_.zero_page_default_va;
    out_in->zero_page_phys_addr =
        (builtin_zp_.backing_phys != 0u) ? builtin_zp_.backing_phys : kHostPhysBase;
    out_in->zero_page_bytes = A840_KMD_ZERO_PAGE_BYTES;

    out_in->ca_target_label = A840_KMD_CA_TARGET_BACKBOARDD;
    out_in->ca_env_entry = A840_KMD_CA_ENV_ENTRY;
    out_in->ca_native_metal_enabled =
        (abi_mode_ == A840_SYS_ABI_MODE_V3_NATIVE) ? 1u : 0u;
    out_in->ca_pack_verified = 1u;
    out_in->ca_policy_enforced = config_.enforce_backboardd_ca_policy ? 1u : 0u;

    out_in->preempt_ring_count = A840_RING_COUNT;
    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        out_in->preempt_records[r].record_va =
            config_.preempt_record_base_va +
            static_cast<uint64_t>(r) * config_.preempt_record_bytes;
        out_in->preempt_records[r].record_bytes = config_.preempt_record_bytes;
    }

    size_t gpu_cnt = 0u;
    size_t gmu_cnt = 0u;
    out_in->gpu_opp_table = a840_gmu_dcvs_get_opp_table(&gpu_cnt);
    out_in->gpu_opp_count = gpu_cnt;
    out_in->gmu_opp_table = a840_gmu_dcvs_get_gmu_table(&gmu_cnt);
    out_in->gmu_opp_count = gmu_cnt;
    out_in->thermal_ladder_active = 1u;
    out_in->thermal_tier_count = A840_KMD_V3_FREEZE_THERMAL_TIERS;
    out_in->thermal_min_throttle_mc = A840_DCVS_THERMAL_TIER0_MC;
    out_in->thermal_cap_75c_mhz = A840_KMD_V3_FREEZE_THERMAL_CAP_75C_MHZ;
    out_in->thermal_cap_95c_mhz = A840_KMD_V3_FREEZE_THERMAL_CAP_95C_MHZ;

    out_in->legacy_turnip_shim_flags = 0u;
    out_in->legacy_turnip_paths_present = 0u;
    out_in->selector_audit_list = kProdSelectorAuditList;
    out_in->selector_audit_count =
        sizeof(kProdSelectorAuditList) / sizeof(kProdSelectorAuditList[0]);
}

IOReturn A840XnuSystemBridge::qualify_production(A840KmdProdQualReport *out_report)
{
    if (!initialized_)
        return kIOReturnNotReady;
    A840KmdProdQualInput in{};
    populateDefaultQualInput(&in);
    return qualify_production_with_input(in, out_report);
}

IOReturn A840XnuSystemBridge::qualify_production_with_input(
    const A840KmdProdQualInput &input,
    A840KmdProdQualReport *out_report)
{
    if (!initialized_)
        return kIOReturnNotReady;

    A840KmdProdQualReport report{};
    const A840KmdStatus st = a840_kmd_v3_freeze_policy_qualify(
        &freeze_policy_, &input, &report);

    stats_lock_.acquire();
    last_qual_report_ = report;
    if (st == A840_KMD_OK && report.qualified == 1u)
        qualified_ = true;
    stats_lock_.release();

    if (out_report != nullptr)
        *out_report = report;
    return kmdStatusToIOReturn(st);
}

IOReturn A840XnuSystemBridge::shutdown()
{
    if (initialized_) {
        uint64_t active_ids[A840_SYS_MAX_CLIENTS]{};
        size_t active_cnt = 0u;
        clients_sync_.acquire();
        for (size_t i = 0u; i < A840_SYS_MAX_CLIENTS; ++i) {
            if (clients_[i].state == SLOT_OPEN)
                active_ids[active_cnt++] = clients_[i].client_id;
        }
        clients_sync_.release();

        for (size_t i = 0u; i < active_cnt; ++i)
            (void)unregister_client(active_ids[i], nullptr);

        initialized_ = false;
    }

    if ((init_mask_ & (UINT32_C(1) << 12u)) != 0u) a840_mdss_sync_destroy(&mdss_sync_);
    if ((init_mask_ & (UINT32_C(1) << 11u)) != 0u) a840_gmu_dcvs_destroy(&gmu_dcvs_);
    if ((init_mask_ & (UINT32_C(1) << 10u)) != 0u) a840_kmd_preempt_destroy(&preempt_);
    if ((init_mask_ & (UINT32_C(1) << 9u)) != 0u)  a840_kmd_wakeup_destroy(&wakeup_);
    if ((init_mask_ & (UINT32_C(1) << 8u)) != 0u)  a840_kmd_qr_destroy(&quota_recovery_);
    if ((init_mask_ & (UINT32_C(1) << 7u)) != 0u)  a840_kmd_bo_pool_destroy(&bo_pool_);
    if ((init_mask_ & (UINT32_C(1) << 6u)) != 0u)  a840_kmd_sparse_destroy(&sparse_mgr_);
    if ((init_mask_ & (UINT32_C(1) << 5u)) != 0u)  a840_kmd_display_handoff_destroy(&display_handoff_);
    if ((init_mask_ & (UINT32_C(1) << 4u)) != 0u)  a840_kmd_writer_promise_destroy(&promise_reg_);
    if ((init_mask_ & (UINT32_C(1) << 3u)) != 0u)  (void)a840_kmd_pack_destroy(&pack_mgr_);
    if ((init_mask_ & (UINT32_C(1) << 1u)) != 0u)  a840_kmd_userclient_v3_destroy(&uc_);
    if ((init_mask_ & (UINT32_C(1) << 0u)) != 0u)  a840_kmd_v3_freeze_policy_destroy(&freeze_policy_);
    init_mask_ = 0u;

    aux_lock_.destroy();
    freeze_lock_.destroy();
    mdss_lock_.destroy();
    gmu_lock_.destroy();
    wakeup_lock_.destroy();
    preempt_lock_.destroy();
    quota_lock_.destroy();
    bo_pool_lock_.destroy();
    sparse_lock_.destroy();
    display_lock_.destroy();
    pack_lock_.destroy();
    surface_lock_.destroy();
    stats_lock_.destroy();
    promise_sync_.destroy();
    wait_any_sync_.destroy();
    clients_sync_.destroy();

    qualified_ = false;
    tier0_pack_published_ = false;
    return kIOReturnSuccess;
}

/* ============================================================================
 * Client Lifecycle & Role Isolation
 * ============================================================================ */

IOReturn A840XnuSystemBridge::register_client(
    uint32_t pid,
    bool is_backboardd,
    uint64_t *out_client_id,
    uint64_t requested_zero_page_va,
    void *smmu_ctx,
    const char *process_label)
{
    if (!initialized_ || out_client_id == nullptr || pid == 0u)
        return kIOReturnBadArgument;

    clients_sync_.acquire();
    const uint64_t cid = next_client_id_++;
    clients_sync_.release();

    const IOReturn rc = register_client_with_id(
        cid, pid, is_backboardd, requested_zero_page_va, smmu_ctx, process_label);
    if (rc == kIOReturnSuccess)
        *out_client_id = cid;
    return rc;
}

IOReturn A840XnuSystemBridge::register_client_with_id(
    uint64_t explicit_client_id,
    uint32_t pid,
    bool is_backboardd,
    uint64_t requested_zero_page_va,
    void *smmu_ctx,
    const char *process_label)
{
    if (!initialized_)
        return kIOReturnNotReady;
    if (explicit_client_id == 0u || pid == 0u)
        return kIOReturnBadArgument;

    const char *effective_label = process_label;
    if (effective_label == nullptr) {
        effective_label = is_backboardd
            ? A840_KMD_CA_TARGET_BACKBOARDD
            : "com.apple.app.client";
    }

    if (is_backboardd && config_.enforce_backboardd_ca_policy &&
        abi_mode_ == A840_SYS_ABI_MODE_V3_NATIVE) {
        A840KmdCaEnvPolicyInput ca_in{};
        ca_in.target_label = effective_label;
        ca_in.native_metal_enabled = 1u;
        ca_in.phase4_pack_verified = 1u;
        ca_in.reserved = 0u;
        A840KmdCaEnvDecision ca_dec{};
        const A840KmdStatus ca_st = a840_kmd_ca_env_evaluate(&ca_in, &ca_dec);
        if (ca_st != A840_KMD_OK) {
            stats_lock_.acquire();
            total_role_denials_++;
            stats_lock_.release();
            return kIOReturnNotPermitted;
        }
    }

    size_t slot = A840_SYS_MAX_CLIENTS;
    clients_sync_.acquire();
    for (size_t i = 0u; i < A840_SYS_MAX_CLIENTS; ++i) {
        if (clients_[i].state != SLOT_FREE &&
            clients_[i].client_id == explicit_client_id) {
            clients_sync_.release();
            return kIOReturnBusy;
        }
        if (slot == A840_SYS_MAX_CLIENTS && clients_[i].state == SLOT_FREE)
            slot = i;
    }
    if (slot == A840_SYS_MAX_CLIENTS) {
        clients_sync_.release();
        return kIOReturnNoResources;
    }
    clients_[slot] = ClientRecord{};
    clients_[slot].state = SLOT_OPENING;
    clients_[slot].client_id = explicit_client_id;
    clients_[slot].pid = pid;
    clients_[slot].is_backboardd = is_backboardd ? 1u : 0u;
    clients_[slot].default_priority = roleDefaultPriority(is_backboardd);
    clients_[slot].smmu_ctx = smmu_ctx;
    copyCStringBounded(clients_[slot].label, sizeof(clients_[slot].label), effective_label);
    if (explicit_client_id >= next_client_id_)
        next_client_id_ = explicit_client_id + 1u;
    clients_sync_.release();

    A840KmdStatus st = a840_kmd_userclient_v3_open_client(
        &uc_, explicit_client_id, smmu_ctx, requested_zero_page_va);
    if (st != A840_KMD_OK) {
        clients_sync_.acquire();
        clients_[slot] = ClientRecord{};
        clients_sync_.release();
        return kmdStatusToIOReturn(st);
    }

    st = a840_kmd_pack_open_client(&pack_mgr_, explicit_client_id);
    if (st != A840_KMD_OK) {
        (void)a840_kmd_userclient_v3_close_client(&uc_, explicit_client_id, nullptr);
        clients_sync_.acquire();
        clients_[slot] = ClientRecord{};
        clients_sync_.release();
        return kmdStatusToIOReturn(st);
    }

    st = a840_kmd_writer_promise_register_client(
        &promise_reg_, explicit_client_id,
        is_backboardd ? A840_CLIENT_ROLE_RENDER_SERVER : A840_CLIENT_ROLE_APP);
    if (st != A840_KMD_OK) {
        (void)a840_kmd_pack_close_client(&pack_mgr_, explicit_client_id);
        (void)a840_kmd_userclient_v3_close_client(&uc_, explicit_client_id, nullptr);
        clients_sync_.acquire();
        clients_[slot] = ClientRecord{};
        clients_sync_.release();
        return kmdStatusToIOReturn(st);
    }

    A840KmdClientQuotaConfig qcfg{};
    qcfg.client_id = explicit_client_id;
    qcfg.priority = roleQuotaPriority(is_backboardd);
    qcfg.soft_limit_bytes = is_backboardd
        ? config_.compositor_soft_quota_bytes
        : config_.app_soft_quota_bytes;
    qcfg.hard_limit_bytes = is_backboardd
        ? config_.compositor_hard_quota_bytes
        : config_.app_hard_quota_bytes;
    st = a840_kmd_qr_register_client(&quota_recovery_, &qcfg);
    if (st != A840_KMD_OK) {
        (void)a840_kmd_writer_promise_close_client(&promise_reg_, explicit_client_id);
        (void)a840_kmd_pack_close_client(&pack_mgr_, explicit_client_id);
        (void)a840_kmd_userclient_v3_close_client(&uc_, explicit_client_id, nullptr);
        clients_sync_.acquire();
        clients_[slot] = ClientRecord{};
        clients_sync_.release();
        return kmdStatusToIOReturn(st);
    }

    (void)a840_kmd_qr_open_channel(&quota_recovery_, explicit_client_id, 0u);

    clients_sync_.acquire();
    clients_[slot].queue_count = 1u;
    clients_[slot].active_queues[0] = 0u;
    clients_[slot].queue_priorities[0] = roleDefaultPriority(is_backboardd);
    clients_[slot].next_fence[0] = 0u;
    clients_[slot].state = SLOT_OPEN;
    clients_sync_.release();

    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::unregister_client(
    uint64_t client_id,
    ClientCloseReport *out_report)
{
    if (!initialized_)
        return kIOReturnNotReady;
    if (client_id == 0u)
        return kIOReturnBadArgument;

    size_t slot = A840_SYS_MAX_CLIENTS;
    ClientCloseReport report{};

    clients_sync_.acquire();
    for (size_t i = 0u; i < A840_SYS_MAX_CLIENTS; ++i) {
        if (clients_[i].state != SLOT_FREE && clients_[i].client_id == client_id) {
            if (clients_[i].state != SLOT_OPEN) {
                clients_sync_.release();
                return kIOReturnBusy;
            }
            clients_[i].state = SLOT_CLOSING;
            slot = i;
            report.client_id = client_id;
            report.pid = clients_[i].pid;
            report.is_backboardd = clients_[i].is_backboardd;
            break;
        }
    }
    if (slot == A840_SYS_MAX_CLIENTS) {
        clients_sync_.release();
        return kIOReturnNotFound;
    }

    clients_sync_.release();
    (void)a840_kmd_userclient_v3_close_client(&uc_, client_id, &report.uc_summary);

    clients_sync_.acquire();
    while (clients_[slot].in_flight > 0u)
        clients_sync_.waitIndefinite();
    clients_sync_.release();

    report.canceled_promises = a840_kmd_writer_promise_close_client(&promise_reg_, client_id);
    a840_kmd_surface_close_client(&surface_mgr_, client_id);
    (void)a840_kmd_pack_close_client(&pack_mgr_, client_id);
    (void)a840_kmd_sparse_client_teardown(&sparse_mgr_, client_id, &report.sparse_summary);
    (void)a840_kmd_qr_unregister_client(&quota_recovery_, client_id);
    (void)a840_kmd_bo_pool_client_teardown(&bo_pool_, client_id, &report.bo_pool_reclaimed);

    aux_lock_.acquire();
    for (size_t i = 0u; i < A840_SYS_MAX_METADATA_SLOTS; ++i) {
        if (metadata_slots_[i].active && metadata_slots_[i].client_id == client_id)
            metadata_slots_[i] = MetadataSlot{};
        if (cpu_leases_[i].active && cpu_leases_[i].client_id == client_id)
            cpu_leases_[i] = CpuMapLeaseSlot{};
    }
    for (size_t i = 0u; i < A840_SYS_MAX_PENDING_RING_TRACK; ++i) {
        if (ring_tracks_[i].active && ring_tracks_[i].client_id == client_id)
            ring_tracks_[i] = RingTrackEntry{};
    }
    aux_lock_.release();

    clients_sync_.acquire();
    clients_[slot] = ClientRecord{};
    clients_sync_.release();

    if (out_report != nullptr)
        *out_report = report;
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::acquireClientRef(
    uint64_t client_id,
    size_t *out_slot,
    bool *out_is_backboardd,
    void **out_smmu_ctx)
{
    clients_sync_.acquire();
    for (size_t i = 0u; i < A840_SYS_MAX_CLIENTS; ++i) {
        if (clients_[i].state != SLOT_FREE && clients_[i].client_id == client_id) {
            if (clients_[i].state != SLOT_OPEN) {
                clients_sync_.release();
                return kIOReturnNotReady;
            }
            clients_[i].in_flight++;
            *out_slot = i;
            *out_is_backboardd = (clients_[i].is_backboardd != 0u);
            *out_smmu_ctx = clients_[i].smmu_ctx;
            clients_sync_.release();
            return kIOReturnSuccess;
        }
    }
    clients_sync_.release();
    return kIOReturnNotFound;
}

void A840XnuSystemBridge::releaseClientRef(size_t slot)
{
    clients_sync_.acquire();
    if (clients_[slot].in_flight > 0u)
        clients_[slot].in_flight--;
    if (clients_[slot].in_flight == 0u && clients_[slot].state == SLOT_CLOSING)
        clients_sync_.wakeAll();
    clients_sync_.release();
}

uint32_t A840XnuSystemBridge::resolveClientPriority(
    size_t slot, uint64_t queue_id, uint32_t hint) const
{
    const bool is_bb = (clients_[slot].is_backboardd != 0u);
    uint32_t chosen = clients_[slot].default_priority;
    for (uint32_t i = 0u; i < clients_[slot].queue_count; ++i) {
        if (clients_[slot].active_queues[i] == queue_id) {
            chosen = clients_[slot].queue_priorities[i];
            break;
        }
    }
    if (hint < A840_PRIORITY_COUNT)
        chosen = hint;
    if (!is_bb && chosen == A840_PRIORITY_COMPOSITOR)
        chosen = A840_PRIORITY_HIGH;
    return chosen;
}

IOReturn A840XnuSystemBridge::ensureClientQueue(
    size_t slot, uint64_t queue_id, uint32_t *out_prio)
{
    uint64_t cid = 0u;
    bool need_open = false;
    uint32_t prio = A840_PRIORITY_NORMAL;

    clients_sync_.acquire();
    cid = clients_[slot].client_id;
    bool found = false;
    for (uint32_t i = 0u; i < clients_[slot].queue_count; ++i) {
        if (clients_[slot].active_queues[i] == queue_id) {
            prio = clients_[slot].queue_priorities[i];
            found = true;
            break;
        }
    }
    if (!found) {
        if (clients_[slot].queue_count >= A840_SYS_MAX_QUEUES_PER_CLIENT) {
            clients_sync_.release();
            return kIOReturnNoResources;
        }
        const uint32_t idx = clients_[slot].queue_count++;
        clients_[slot].active_queues[idx] = queue_id;
        clients_[slot].queue_priorities[idx] = clients_[slot].default_priority;
        clients_[slot].next_fence[idx] = 0u;
        prio = clients_[slot].default_priority;
        need_open = true;
    }
    clients_sync_.release();

    if (need_open) {
        const A840KmdStatus st = a840_kmd_qr_open_channel(&quota_recovery_, cid, queue_id);
        if (st != A840_KMD_OK && st != A840_KMD_STATE_CONFLICT)
            return kmdStatusToIOReturn(st);
    }
    if (out_prio != nullptr)
        *out_prio = prio;
    return kIOReturnSuccess;
}

/* ============================================================================
 * Method Handlers (Methods 1..19)
 * ============================================================================ */

IOReturn A840XnuSystemBridge::handleMethodQuery(
    void *out_buf, size_t out_len, size_t *out_written)
{
    if (out_buf == nullptr || out_len != sizeof(A840SysParamsWire))
        return kIOReturnBadArgument;
    A840SysParamsWire p{};
    p.abi_version = (abi_mode_ == A840_SYS_ABI_MODE_V3_NATIVE)
                        ? NativeAdrenoAbiVersionV3
                        : NativeAdrenoAbiVersionV2;
    p.input_bits = config_.input_bits;
    p.output_bits = config_.output_bits;
    p.flags = advertised_caps_ & static_cast<uint32_t>(A840_KMD_V3_PROD_CAP_MASK);
    p.max_bo_bytes = kDefaultMaxBoBytes;
    memcpy(out_buf, &p, sizeof(p));
    if (out_written != nullptr)
        *out_written = sizeof(p);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodDeviceInfo(
    void *out_buf, size_t out_len, size_t *out_written)
{
    if (out_buf == nullptr || out_len != sizeof(A840SysDeviceInfoWire))
        return kIOReturnBadArgument;
    const A840ChipFacts *facts = a840_chip_facts_kaanapali_default();
    A840SysDeviceInfoWire info{};
    info.chip_id = static_cast<uint32_t>(facts->chip_id);
    info.gmem_bytes = facts->gmem_bytes;
    info.gmem_base = facts->gmem_range_min;
    info.uche_trap_base = facts->uche_trap_base;
    info.va_start = kDefaultVaStart;
    info.va_size = kDefaultVaSize;
    info.highest_bank_bit = facts->highest_bank_bit;
    info.ubwc_version = facts->ubwc_version;
    info.ubwc_swizzle = facts->ubwc_swizzle;
    info.macrotile_mode = facts->macrotile_mode;
    info.smmu_page_shift = facts->smmu_page_shift;
    info.flags = advertised_caps_;
    memcpy(out_buf, &info, sizeof(info));
    if (out_written != nullptr)
        *out_written = sizeof(info);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodBoAlloc(
    uint64_t client_id, void *smmu_ctx,
    const void *in_buf, size_t in_len,
    void *out_buf, size_t out_len, size_t *out_written)
{
    if (in_buf == nullptr || out_buf == nullptr ||
        in_len != sizeof(A840SysBoAllocWire) ||
        out_len != sizeof(A840SysBoAllocReplyWire))
        return kIOReturnBadArgument;

    A840SysBoAllocWire req{};
    memcpy(&req, in_buf, sizeof(req));
    if (req.reserved != 0u || req.bytes == 0u)
        return kIOReturnBadArgument;

    A840KmdAllocResult qres{};
    const A840KmdStatus qst = a840_kmd_qr_request_alloc(
        &quota_recovery_, client_id, req.bytes, &qres);
    if (qst != A840_KMD_OK)
        return kmdStatusToIOReturn(qst);

    A840KmdBoAcquireReq areq{};
    areq.client_id = client_id;
    areq.smmu_ctx = smmu_ctx;
    areq.requested_bytes = req.bytes;
    areq.alignment = req.alignment;
    areq.permissions = (req.permissions != 0u)
                           ? req.permissions
                           : static_cast<uint32_t>(A840_KMD_SMMU_PERM_READ_WRITE);
    areq.cache_mode = req.cache_mode;
    areq.flags = req.flags;
    areq.reserved = 0u;

    A840KmdBoDesc desc{};
    const A840KmdStatus bst = a840_kmd_bo_pool_acquire(&bo_pool_, &areq, &desc);
    if (bst != A840_KMD_OK) {
        (void)a840_kmd_qr_release_bytes(&quota_recovery_, client_id, req.bytes);
        return kmdStatusToIOReturn(bst);
    }

    (void)a840_kmd_qr_track_bo(
        &quota_recovery_, client_id, desc.bo_handle, desc.requested_bytes,
        0u, A840_PURGE_NONVOLATILE, 0u);

    if ((desc.phys_addr % A840_KMD_SPARSE_TILE_ALIGN) == 0u &&
        (desc.bucket_bytes % A840_KMD_SPARSE_TILE_BYTES) == 0u) {
        (void)a840_kmd_sparse_register_bo(
            &sparse_mgr_, client_id, desc.bo_handle,
            desc.phys_addr, desc.bucket_bytes, desc.permissions);
    }

    A840SysBoAllocReplyWire rep{};
    rep.handle = desc.bo_handle;
    rep.gpu_va = desc.gpu_va;
    rep.phys_addr = desc.phys_addr;
    rep.bytes = desc.requested_bytes;
    memcpy(out_buf, &rep, sizeof(rep));
    if (out_written != nullptr)
        *out_written = sizeof(rep);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodBoMap(
    uint64_t client_id,
    const void *in_buf, size_t in_len,
    void *out_buf, size_t out_len, size_t *out_written)
{
    if (in_buf == nullptr || out_buf == nullptr ||
        in_len != sizeof(A840SysBoMapWire) ||
        out_len != sizeof(A840SysBoMapReplyWire))
        return kIOReturnBadArgument;

    A840SysBoMapWire req{};
    memcpy(&req, in_buf, sizeof(req));
    if (req.reserved != 0u || req.handle == 0u)
        return kIOReturnBadArgument;

    A840KmdBoDesc desc{};
    const A840KmdStatus st = a840_kmd_bo_pool_query_bo(
        &bo_pool_, client_id, req.handle, &desc);
    if (st != A840_KMD_OK)
        return kmdStatusToIOReturn(st);

    const uint64_t mapped_va = (req.iova != 0u) ? req.iova : desc.gpu_va;
    if ((mapped_va % A840_KMD_V3_FREEZE_PAGE_BYTES) != 0u ||
        mapped_va >= A840_KMD_GPU_VA_LIMIT)
        return kIOReturnBadArgument;

    A840SysBoMapReplyWire rep{};
    rep.gpu_va = mapped_va;
    rep.bytes = desc.requested_bytes;
    memcpy(out_buf, &rep, sizeof(rep));
    if (out_written != nullptr)
        *out_written = sizeof(rep);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodBoFree(
    uint64_t client_id, const void *in_buf, size_t in_len)
{
    if (in_buf == nullptr || in_len != sizeof(A840SysBoFreeWire))
        return kIOReturnBadArgument;
    A840SysBoFreeWire req{};
    memcpy(&req, in_buf, sizeof(req));
    if (req.reserved != 0u || req.handle == 0u)
        return kIOReturnBadArgument;

    (void)a840_kmd_sparse_unbind_bo(&sparse_mgr_, client_id, req.handle, 1u, nullptr, nullptr);
    a840_kmd_surface_release(&surface_mgr_, client_id, req.handle);
    a840_kmd_writer_promise_unbind_surface(&promise_reg_, client_id, req.handle);
    (void)a840_kmd_qr_untrack_bo(&quota_recovery_, client_id, req.handle, 1u);

    const A840KmdStatus st = a840_kmd_bo_pool_release(
        &bo_pool_, client_id, req.handle, req.allow_park);
    return kmdStatusToIOReturn(st);
}

IOReturn A840XnuSystemBridge::import_iosurface_ubwc6(
    uint64_t client_id,
    const A840SysBoImportUbwc6Wire &req,
    A840SysBoImportUbwc6ReplyWire *out_reply)
{
    if (!initialized_ || out_reply == nullptr)
        return kIOReturnBadArgument;
    if (req.reserved != 0u || req.surface_identity == 0u || req.bytes == 0u ||
        req.width == 0u || req.height == 0u)
        return kIOReturnBadArgument;

    size_t slot = A840_SYS_MAX_CLIENTS;
    bool is_bb = false;
    void *smmu_ctx = nullptr;
    const IOReturn ref_rc = acquireClientRef(client_id, &slot, &is_bb, &smmu_ctx);
    if (ref_rc != kIOReturnSuccess)
        return ref_rc;

    A840KmdBoAcquireReq areq{};
    areq.client_id = client_id;
    areq.smmu_ctx = smmu_ctx;
    areq.requested_bytes = req.bytes;
    areq.alignment = A840_KMD_ZERO_PAGE_ALIGN;
    areq.permissions = A840_KMD_SMMU_PERM_READ_WRITE;
    areq.cache_mode = A840_KMD_BO_CACHE_WRITE_COMBINE;
    areq.flags = A840_KMD_BO_ACQUIRE_NONE;
    areq.reserved = 0u;

    A840KmdBoDesc desc{};
    const A840KmdStatus bst = a840_kmd_bo_pool_acquire(&bo_pool_, &areq, &desc);
    if (bst != A840_KMD_OK) {
        releaseClientRef(slot);
        return kmdStatusToIOReturn(bst);
    }

    A840KmdSurfaceLayout layout{};
    layout.bytes = req.bytes;
    layout.display_base = (req.display_base != 0u) ? req.display_base : desc.phys_addr;
    layout.format = req.format;
    layout.width = req.width;
    layout.height = req.height;
    layout.meta_rows = (req.meta_rows != 0u) ? req.meta_rows : 64u;

    bool is_creator = true;
    A840KmdStatus sst = a840_kmd_surface_create(
        &surface_mgr_, client_id, desc.bo_handle, req.surface_identity, &layout);
    if (sst == A840_KMD_STATE_CONFLICT) {
        is_creator = false;
        sst = a840_kmd_surface_import(
            &surface_mgr_, client_id, desc.bo_handle, req.surface_identity);
    }
    if (sst != A840_KMD_OK) {
        (void)a840_kmd_bo_pool_release(&bo_pool_, client_id, desc.bo_handle, 0u);
        releaseClientRef(slot);
        return kmdStatusToIOReturn(sst);
    }

    (void)a840_kmd_writer_promise_bind_surface(
        &promise_reg_, client_id, desc.bo_handle, req.surface_identity);

    A840KmdSurfaceResult sres{};
    if (req.ubwc_enabled != 0u || req.ubwc_version != 0u) {
        struct NativeAdrenoAbiSurfaceMetaV3 meta{};
        meta.bo_handle = desc.bo_handle;
        meta.op = is_creator ? 1u : 0u; /* SET for creator, GET for shared importer */
        meta.ubwc_enabled = req.ubwc_enabled;
        meta.ubwc_version = (req.ubwc_version != 0u)
                                ? req.ubwc_version
                                : A840_KMD_V3_FREEZE_UBWC_VERSION;
        meta.highest_bank_bit = (req.highest_bank_bit != 0u)
                                    ? req.highest_bank_bit
                                    : static_cast<uint32_t>(A840_KAANAPALI_HIGHEST_BANK_BIT);
        meta.swizzle_levels = (req.swizzle_levels != 0u)
                                  ? req.swizzle_levels
                                  : A840_KMD_V3_FREEZE_UBWC_SWIZZLE;
        meta.macrotile_mode = (req.macrotile_mode != 0u)
                                  ? req.macrotile_mode
                                  : A840_KMD_V3_FREEZE_MACROTILE_MODE;
        meta.meta_offset = req.meta_offset;
        meta.meta_pitch = req.meta_pitch;
        meta.flags = req.flags;

        const A840KmdStatus mst = a840_kmd_surface_meta(
            &surface_mgr_, client_id, &meta,
            is_bb ? 1u : 0u, &active_mdss_caps_, &sres);
        if (mst != A840_KMD_OK) {
            a840_kmd_writer_promise_unbind_surface(&promise_reg_, client_id, desc.bo_handle);
            a840_kmd_surface_release(&surface_mgr_, client_id, desc.bo_handle);
            (void)a840_kmd_bo_pool_release(&bo_pool_, client_id, desc.bo_handle, 0u);
            releaseClientRef(slot);
            return kmdStatusToIOReturn(mst);
        }
    }

    stats_lock_.acquire();
    total_ubwc6_imports_++;
    stats_lock_.release();

    memset(out_reply, 0, sizeof(*out_reply));
    out_reply->bo_handle = desc.bo_handle;
    out_reply->surface_identity = req.surface_identity;
    out_reply->gpu_va = desc.gpu_va;
    out_reply->phys_addr = desc.phys_addr;
    out_reply->scanout_eligible = sres.scanout_eligible;
    out_reply->fallback_linear_required = sres.fallback_linear_required;
    out_reply->layout_valid = sres.layout_valid;

    releaseClientRef(slot);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodBoImport(
    uint64_t client_id,
    const void *in_buf, size_t in_len,
    void *out_buf, size_t out_len, size_t *out_written)
{
    if (in_buf == nullptr || out_buf == nullptr ||
        in_len != sizeof(A840SysBoImportUbwc6Wire) ||
        out_len != sizeof(A840SysBoImportUbwc6ReplyWire))
        return kIOReturnBadArgument;

    A840SysBoImportUbwc6Wire req{};
    memcpy(&req, in_buf, sizeof(req));
    A840SysBoImportUbwc6ReplyWire rep{};
    const IOReturn rc = import_iosurface_ubwc6(client_id, req, &rep);
    if (rc != kIOReturnSuccess)
        return rc;
    memcpy(out_buf, &rep, sizeof(rep));
    if (out_written != nullptr)
        *out_written = sizeof(rep);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodSubmit(
    size_t slot, uint64_t client_id, bool is_backboardd,
    const void *in_buf, size_t in_len,
    void *out_buf, size_t out_len, size_t *out_written)
{
    if (in_buf == nullptr || out_buf == nullptr ||
        in_len != sizeof(A840SysSubmitWire) ||
        out_len != sizeof(A840SysSubmitReplyWire))
        return kIOReturnBadArgument;

    A840SysSubmitWire req{};
    memcpy(&req, in_buf, sizeof(req));
    if (req.reserved != 0u || req.bo_count > A840_KMD_SUBMIT_MAX_BOS)
        return kIOReturnBadArgument;

    if (!is_backboardd && req.priority_hint == A840_PRIORITY_COMPOSITOR) {
        stats_lock_.acquire();
        total_role_denials_++;
        stats_lock_.release();
        return kIOReturnNotPermitted;
    }

    const A840KmdStatus ch_st = a840_kmd_userclient_v3_check_submit_channel(
        &uc_, client_id, req.queue);
    if (ch_st != A840_KMD_OK)
        return kmdStatusToIOReturn(ch_st);

    A840KmdChannelInfo qinfo{};
    if (a840_kmd_qr_query_channel(&quota_recovery_, client_id, req.queue, &qinfo) == A840_KMD_OK) {
        if (qinfo.state == A840_KMD_CHANNEL_FAULTING ||
            qinfo.state == A840_KMD_CHANNEL_RESETTING ||
            qinfo.state == A840_KMD_CHANNEL_BANNED)
            return kIOReturnAborted;
    }

    uint32_t queue_prio = A840_PRIORITY_NORMAL;
    const IOReturn qrc = ensureClientQueue(slot, req.queue, &queue_prio);
    if (qrc != kIOReturnSuccess)
        return qrc;

    uint32_t prio = queue_prio;
    if (req.priority_hint < A840_PRIORITY_COUNT)
        prio = req.priority_hint;
    if (!is_backboardd && prio == A840_PRIORITY_COMPOSITOR)
        prio = A840_PRIORITY_HIGH;
    if (is_backboardd && req.priority_hint == 0u)
        prio = A840_PRIORITY_COMPOSITOR;

    uint32_t assigned_ring = A840_RING_2_CLIENT_3D;
    const A840KmdStatus rmap_st = a840_kmd_preempt_priority_to_ring(prio, &assigned_ring);
    if (rmap_st != A840_KMD_OK)
        return kmdStatusToIOReturn(rmap_st);

    uint64_t client_fence = 0u;
    clients_sync_.acquire();
    for (uint32_t i = 0u; i < clients_[slot].queue_count; ++i) {
        if (clients_[slot].active_queues[i] == req.queue) {
            client_fence = ++clients_[slot].next_fence[i];
            break;
        }
    }
    clients_sync_.release();
    if (client_fence == 0u)
        client_fence = 1u;

    const A840KmdStatus qsub_st = a840_kmd_qr_submit_begin(
        &quota_recovery_, client_id, req.queue, client_fence,
        (req.bo_count > 0u) ? req.bo_handles : nullptr, req.bo_count);
    if (qsub_st != A840_KMD_OK)
        return kmdStatusToIOReturn(qsub_st);

    const uint64_t now_ns = (req.enqueue_ns != 0u) ? req.enqueue_ns : currentNowNs();

    A840PreemptSubmitReq preq{};
    preq.ring_id = assigned_ring;
    preq.ib_dwords = req.ib_dwords;
    preq.ib_va = req.ib_va;
    preq.enqueue_ns = now_ns;
    preq.frame_deadline_ns = req.frame_deadline_ns;
    preq.has_dependency = req.has_dependency;
    preq.dep_ring = req.dep_ring;
    preq.dep_seqno = req.dep_seqno;

    A840PreemptSubmitResult pres{};
    const A840KmdStatus pst = a840_kmd_preempt_submit(&preempt_, &preq, &pres);
    if (pst != A840_KMD_OK) {
        (void)a840_kmd_qr_submit_complete(&quota_recovery_, client_id, req.queue, client_fence, nullptr);
        return kmdStatusToIOReturn(pst);
    }

    aux_lock_.acquire();
    size_t track_slot = A840_SYS_MAX_PENDING_RING_TRACK;
    for (size_t i = 0u; i < A840_SYS_MAX_PENDING_RING_TRACK; ++i) {
        if (!ring_tracks_[i].active) {
            track_slot = i;
            break;
        }
    }
    if (track_slot == A840_SYS_MAX_PENDING_RING_TRACK)
        track_slot = static_cast<size_t>(pres.seqno % A840_SYS_MAX_PENDING_RING_TRACK);
    ring_tracks_[track_slot].active = 1u;
    ring_tracks_[track_slot].ring_id = assigned_ring;
    ring_tracks_[track_slot].client_id = client_id;
    ring_tracks_[track_slot].local_queue = req.queue;
    ring_tracks_[track_slot].client_fence = client_fence;
    ring_tracks_[track_slot].ring_seqno = pres.seqno;
    aux_lock_.release();

    uint32_t mdss_decision = A840_MDSS_COMMIT_HIT_CURRENT_VSYNC;
    uint32_t extra_vsync_waits = 0u;
    int64_t scanout_margin_ns = 0;
    bool has_margin = false;

    if (is_backboardd && assigned_ring == A840_RING_0_COMPOSITOR) {
        const uint32_t urgent = (req.frame_deadline_ns != 0u &&
                                 req.frame_deadline_ns > now_ns &&
                                 (req.frame_deadline_ns - now_ns) <= A840_PREEMPT_DEFAULT_URGENT_BUDGET_NS)
                                    ? 1u : 0u;
        (void)a840_gmu_dcvs_on_ring0_submit(&gmu_dcvs_, now_ns, 1u, urgent);

        if (req.attach_display_sync != 0u) {
            A840MdssCommitRequest mreq{};
            mreq.frame_id = (req.frame_id != 0u) ? req.frame_id : pres.seqno;
            mreq.now_ns = now_ns;
            mreq.estimated_gpu_done_ns = (req.estimated_gpu_busy_ns != 0u)
                                             ? (now_ns + req.estimated_gpu_busy_ns)
                                             : now_ns;
            mreq.has_early_writer_promise = (req.surface_identity != 0u) ? 1u : 0u;
            mreq.is_retired_ready = 0u;
            mreq.ring0_compositor = 1u;
            mreq.reserved = 0u;

            A840MdssCommitPlan mplan{};
            if (a840_mdss_sync_plan_commit(&mdss_sync_, &mreq, &mplan) == A840_KMD_OK) {
                mdss_decision = mplan.decision;
                extra_vsync_waits = mplan.extra_vsync_waits;
                scanout_margin_ns = mplan.scanout_margin_ns;
                has_margin = true;
            }
        }
    }

    if (req.estimated_gpu_busy_ns > 0u) {
        A840DcvsFrameSample fsample{};
        fsample.now_ns = now_ns;
        fsample.gpu_busy_ns = req.estimated_gpu_busy_ns;
        fsample.frame_period_ns = A840_DCVS_FRAME_BUDGET_120HZ_NS;
        fsample.scanout_margin_ns = scanout_margin_ns;
        fsample.has_scanout_margin = has_margin ? 1u : 0u;
        fsample.ring0_submits = (assigned_ring == A840_RING_0_COMPOSITOR) ? 1u : 0u;
        fsample.missed_deadline = (extra_vsync_waits > 0u) ? 1u : 0u;
        (void)a840_gmu_dcvs_record_frame(&gmu_dcvs_, &fsample);
    }

    A840GmuDcvsSnapshot dsnap{};
    (void)a840_gmu_dcvs_get_snapshot(&gmu_dcvs_, &dsnap);

    stats_lock_.acquire();
    total_submits_++;
    if (assigned_ring == A840_RING_0_COMPOSITOR)
        total_ring0_submits_++;
    else
        total_app_submits_++;
    stats_lock_.release();

    A840SysSubmitReplyWire rep{};
    rep.fence = client_fence;
    rep.ring_seqno = pres.seqno;
    rep.assigned_ring = assigned_ring;
    rep.preempt_triggered = pres.preempt_triggered;
    rep.doorbell_emitted = pres.doorbell_emitted;
    rep.mdss_decision = mdss_decision;
    rep.extra_vsync_waits = extra_vsync_waits;
    rep.confirmed_gpu_level = dsnap.confirmed_gpu_level;
    rep.confirmed_gmu_level = dsnap.confirmed_gmu_level;
    memcpy(out_buf, &rep, sizeof(rep));
    if (out_written != nullptr)
        *out_written = sizeof(rep);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodWaitFence(
    uint64_t client_id, const void *in_buf, size_t in_len)
{
    if (in_buf == nullptr || in_len != sizeof(A840SysWaitFenceWire))
        return kIOReturnBadArgument;
    A840SysWaitFenceWire req{};
    memcpy(&req, in_buf, sizeof(req));

    struct NativeAdrenoAbiWaitAnyV3 wa{};
    wa.count = 1u;
    wa.flags = 1u;
    wa.timeout_ns = req.timeout_ns;
    wa.queues[0] = req.queue;
    wa.fences[0] = req.fence;

    struct NativeAdrenoAbiWaitAnyReplyV3 rep{};
    size_t written = 0u;
    const A840KmdStatus st = a840_kmd_userclient_v3_dispatch(
        &uc_, client_id, NativeAdrenoMethodWaitAnyV3,
        &wa, sizeof(wa), &rep, sizeof(rep), &written);
    if (st != A840_KMD_OK)
        return kmdStatusToIOReturn(st);
    return (rep.timed_out != 0u) ? kIOReturnTimeout : kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodGetTimestamp(
    void *out_buf, size_t out_len, size_t *out_written)
{
    if (out_buf == nullptr || out_len != sizeof(A840SysTimestampReplyWire))
        return kIOReturnBadArgument;
    A840SysTimestampReplyWire rep{};
    rep.ticks = currentTicks();
    rep.now_ns = currentNowNs();
    memcpy(out_buf, &rep, sizeof(rep));
    if (out_written != nullptr)
        *out_written = sizeof(rep);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodGetStatus(
    void *out_buf, size_t out_len, size_t *out_written)
{
    if (out_buf == nullptr || out_len != sizeof(A840SysStatusReplyWire))
        return kIOReturnBadArgument;
    A840SysStatusReplyWire rep{};
    stats_lock_.acquire();
    rep.faults = total_faults_isolated_;
    rep.recoveries = total_recoveries_;
    rep.abi_mode = abi_mode_;
    rep.qualified = qualified_ ? 1u : 0u;
    stats_lock_.release();
    memcpy(out_buf, &rep, sizeof(rep));
    if (out_written != nullptr)
        *out_written = sizeof(rep);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodMetadata(
    uint64_t client_id, bool is_set,
    const void *in_buf, size_t in_len,
    void *out_buf, size_t out_len, size_t *out_written)
{
    if (in_buf == nullptr || in_len != sizeof(A840SysMetadataWire))
        return kIOReturnBadArgument;
    A840SysMetadataWire req{};
    memcpy(&req, in_buf, sizeof(req));
    if (req.reserved != 0u || req.handle == 0u || req.bytes > A840_SYS_MAX_METADATA_BYTES)
        return kIOReturnBadArgument;

    if (is_set) {
        aux_lock_.acquire();
        size_t free_idx = A840_SYS_MAX_METADATA_SLOTS;
        for (size_t i = 0u; i < A840_SYS_MAX_METADATA_SLOTS; ++i) {
            if (metadata_slots_[i].active &&
                metadata_slots_[i].client_id == client_id &&
                metadata_slots_[i].bo_handle == req.handle) {
                free_idx = i;
                break;
            }
            if (free_idx == A840_SYS_MAX_METADATA_SLOTS && !metadata_slots_[i].active)
                free_idx = i;
        }
        if (free_idx == A840_SYS_MAX_METADATA_SLOTS) {
            aux_lock_.release();
            return kIOReturnNoResources;
        }
        metadata_slots_[free_idx].active = 1u;
        metadata_slots_[free_idx].client_id = client_id;
        metadata_slots_[free_idx].bo_handle = req.handle;
        metadata_slots_[free_idx].bytes = req.bytes;
        memset(metadata_slots_[free_idx].data, 0, sizeof(metadata_slots_[free_idx].data));
        if (req.bytes > 0u)
            memcpy(metadata_slots_[free_idx].data, req.data, req.bytes);
        aux_lock_.release();
        return kIOReturnSuccess;
    }

    if (out_buf == nullptr || out_len != sizeof(A840SysMetadataWire))
        return kIOReturnBadArgument;

    A840SysMetadataWire rep{};
    rep.handle = req.handle;
    aux_lock_.acquire();
    bool found = false;
    for (size_t i = 0u; i < A840_SYS_MAX_METADATA_SLOTS; ++i) {
        if (metadata_slots_[i].active &&
            metadata_slots_[i].client_id == client_id &&
            metadata_slots_[i].bo_handle == req.handle) {
            rep.bytes = metadata_slots_[i].bytes;
            if (rep.bytes > 0u)
                memcpy(rep.data, metadata_slots_[i].data, rep.bytes);
            found = true;
            break;
        }
    }
    aux_lock_.release();
    if (!found)
        return kIOReturnNotFound;
    memcpy(out_buf, &rep, sizeof(rep));
    if (out_written != nullptr)
        *out_written = sizeof(rep);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodCreateQueue(
    size_t slot, uint64_t client_id, bool is_backboardd,
    const void *in_buf, size_t in_len,
    void *out_buf, size_t out_len, size_t *out_written)
{
    if (in_buf == nullptr || out_buf == nullptr ||
        in_len != sizeof(A840SysQueueWire) ||
        out_len != sizeof(A840SysQueueWire))
        return kIOReturnBadArgument;

    A840SysQueueWire req{};
    memcpy(&req, in_buf, sizeof(req));
    if (req.reserved != 0u || req.priority >= A840_PRIORITY_COUNT)
        return kIOReturnBadArgument;
    if (!is_backboardd && req.priority == A840_PRIORITY_COMPOSITOR) {
        stats_lock_.acquire();
        total_role_denials_++;
        stats_lock_.release();
        return kIOReturnNotPermitted;
    }

    uint64_t qid = req.queue;
    clients_sync_.acquire();
    if (qid == 0u)
        qid = static_cast<uint64_t>(clients_[slot].queue_count) + 1u;
    for (uint32_t i = 0u; i < clients_[slot].queue_count; ++i) {
        if (clients_[slot].active_queues[i] == qid) {
            clients_sync_.release();
            return kIOReturnBusy;
        }
    }
    if (clients_[slot].queue_count >= A840_SYS_MAX_QUEUES_PER_CLIENT) {
        clients_sync_.release();
        return kIOReturnNoResources;
    }
    const uint32_t idx = clients_[slot].queue_count++;
    clients_[slot].active_queues[idx] = qid;
    clients_[slot].queue_priorities[idx] = req.priority;
    clients_[slot].next_fence[idx] = 0u;
    clients_sync_.release();

    const A840KmdStatus st = a840_kmd_qr_open_channel(&quota_recovery_, client_id, qid);
    if (st != A840_KMD_OK && st != A840_KMD_STATE_CONFLICT)
        return kmdStatusToIOReturn(st);

    A840SysQueueWire rep{};
    rep.queue = qid;
    rep.priority = req.priority;
    memcpy(out_buf, &rep, sizeof(rep));
    if (out_written != nullptr)
        *out_written = sizeof(rep);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodCloseQueue(
    size_t slot, uint64_t client_id, const void *in_buf, size_t in_len)
{
    if (in_buf == nullptr || in_len != sizeof(A840SysQueueWire))
        return kIOReturnBadArgument;
    A840SysQueueWire req{};
    memcpy(&req, in_buf, sizeof(req));
    if (req.reserved != 0u)
        return kIOReturnBadArgument;

    clients_sync_.acquire();
    bool removed = false;
    for (uint32_t i = 0u; i < clients_[slot].queue_count; ++i) {
        if (clients_[slot].active_queues[i] == req.queue) {
            const uint32_t last = clients_[slot].queue_count - 1u;
            clients_[slot].active_queues[i] = clients_[slot].active_queues[last];
            clients_[slot].queue_priorities[i] = clients_[slot].queue_priorities[last];
            clients_[slot].next_fence[i] = clients_[slot].next_fence[last];
            clients_[slot].queue_count--;
            removed = true;
            break;
        }
    }
    clients_sync_.release();

    if (!removed)
        return kIOReturnNotFound;
    (void)a840_kmd_qr_close_channel(&quota_recovery_, client_id, req.queue);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodBoCpuMap(
    uint64_t client_id, bool is_unmap,
    const void *in_buf, size_t in_len,
    void *out_buf, size_t out_len, size_t *out_written)
{
    if (in_buf == nullptr || in_len != sizeof(A840SysBoCpuMapWire))
        return kIOReturnBadArgument;
    A840SysBoCpuMapWire req{};
    memcpy(&req, in_buf, sizeof(req));
    if (req.reserved != 0u || req.handle == 0u)
        return kIOReturnBadArgument;

    A840KmdBoDesc desc{};
    const A840KmdStatus bst = a840_kmd_bo_pool_query_bo(
        &bo_pool_, client_id, req.handle, &desc);
    if (bst != A840_KMD_OK)
        return kmdStatusToIOReturn(bst);

    if (!is_unmap) {
        if (out_buf == nullptr || out_len != sizeof(A840SysBoCpuMapReplyWire))
            return kIOReturnBadArgument;
        if (req.write_access != 0u &&
            (desc.permissions & A840_KMD_SMMU_PERM_WRITE) == 0u)
            return kIOReturnNotPermitted;

        aux_lock_.acquire();
        size_t idx = A840_SYS_MAX_METADATA_SLOTS;
        for (size_t i = 0u; i < A840_SYS_MAX_METADATA_SLOTS; ++i) {
            if (cpu_leases_[i].active &&
                cpu_leases_[i].client_id == client_id &&
                cpu_leases_[i].bo_handle == req.handle) {
                idx = i;
                break;
            }
            if (idx == A840_SYS_MAX_METADATA_SLOTS && !cpu_leases_[i].active)
                idx = i;
        }
        if (idx == A840_SYS_MAX_METADATA_SLOTS) {
            aux_lock_.release();
            return kIOReturnNoResources;
        }
        if (!cpu_leases_[idx].active) {
            cpu_leases_[idx].active = 1u;
            cpu_leases_[idx].client_id = client_id;
            cpu_leases_[idx].bo_handle = req.handle;
            cpu_leases_[idx].token = next_cpu_token_++;
            cpu_leases_[idx].bytes = desc.requested_bytes;
            cpu_leases_[idx].leases = 0u;
        }
        cpu_leases_[idx].leases++;
        A840SysBoCpuMapReplyWire rep{};
        rep.token = cpu_leases_[idx].token;
        rep.bytes = cpu_leases_[idx].bytes;
        rep.active_leases = cpu_leases_[idx].leases;
        aux_lock_.release();

        memcpy(out_buf, &rep, sizeof(rep));
        if (out_written != nullptr)
            *out_written = sizeof(rep);
        return kIOReturnSuccess;
    }

    aux_lock_.acquire();
    bool found = false;
    for (size_t i = 0u; i < A840_SYS_MAX_METADATA_SLOTS; ++i) {
        if (cpu_leases_[i].active &&
            cpu_leases_[i].client_id == client_id &&
            cpu_leases_[i].bo_handle == req.handle) {
            if (cpu_leases_[i].leases > 0u)
                cpu_leases_[i].leases--;
            if (cpu_leases_[i].leases == 0u)
                cpu_leases_[i] = CpuMapLeaseSlot{};
            found = true;
            break;
        }
    }
    aux_lock_.release();
    return found ? kIOReturnSuccess : kIOReturnNotFound;
}

IOReturn A840XnuSystemBridge::handleMethodSetPriority(
    size_t slot, bool is_backboardd, const void *in_buf, size_t in_len)
{
    if (in_buf == nullptr || in_len != sizeof(A840SysSetPriorityWire))
        return kIOReturnBadArgument;
    A840SysSetPriorityWire req{};
    memcpy(&req, in_buf, sizeof(req));
    if (req.reserved != 0u || req.priority >= A840_PRIORITY_COUNT)
        return kIOReturnBadArgument;

    if (!is_backboardd && req.priority == A840_PRIORITY_COMPOSITOR) {
        stats_lock_.acquire();
        total_role_denials_++;
        stats_lock_.release();
        return kIOReturnNotPermitted;
    }

    clients_sync_.acquire();
    bool updated = false;
    for (uint32_t i = 0u; i < clients_[slot].queue_count; ++i) {
        if (clients_[slot].active_queues[i] == req.queue) {
            clients_[slot].queue_priorities[i] = req.priority;
            updated = true;
            break;
        }
    }
    if (!updated)
        clients_[slot].default_priority = req.priority;
    clients_sync_.release();
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodNote(
    bool is_backboardd, const void *in_buf, size_t in_len)
{
    if (in_buf == nullptr || in_len != sizeof(A840SysNoteWire))
        return kIOReturnBadArgument;
    A840SysNoteWire req{};
    memcpy(&req, in_buf, sizeof(req));
    if (req.reserved != 0u)
        return kIOReturnBadArgument;

    const uint64_t now_ns = (req.now_ns != 0u) ? req.now_ns : currentNowNs();
    switch (req.kind) {
    case A840_SYS_NOTE_TOUCH_BOOST:
        return kmdStatusToIOReturn(
            a840_gmu_dcvs_notify_touch(&gmu_dcvs_, now_ns, req.value_u64));
    case A840_SYS_NOTE_THERMAL_SAMPLE:
        if (!is_backboardd) {
            stats_lock_.acquire();
            total_role_denials_++;
            stats_lock_.release();
            return kIOReturnNotPermitted;
        }
        return kmdStatusToIOReturn(
            a840_gmu_dcvs_update_thermal(&gmu_dcvs_, req.param_i32, now_ns));
    case A840_SYS_NOTE_COLD_LEASE_REQ:
        if (!is_backboardd) {
            stats_lock_.acquire();
            total_role_denials_++;
            stats_lock_.release();
            return kIOReturnNotPermitted;
        }
        return kmdStatusToIOReturn(
            a840_gmu_dcvs_request_lease(&gmu_dcvs_, req.param_u32, now_ns, req.value_u64));
    case A840_SYS_NOTE_COLD_LEASE_RELEASE:
        return kmdStatusToIOReturn(a840_gmu_dcvs_release_lease(&gmu_dcvs_, now_ns));
    case A840_SYS_NOTE_MEMORY_PRESSURE: {
        A840KmdPressureReport prep{};
        a840_kmd_pack_set_params(
            &pack_mgr_,
            (req.param_u32 != 0u)
                ? (advertised_caps_ | NativeAdrenoParamsV2MemoryPressure)
                : (advertised_caps_ & ~static_cast<uint32_t>(NativeAdrenoParamsV2MemoryPressure)));
        return kmdStatusToIOReturn(
            a840_kmd_bo_pool_memory_pressure(&bo_pool_, req.value_u64, &prep));
    }
    case A840_SYS_NOTE_BOOT_COMPLETE:
        if (!is_backboardd) {
            stats_lock_.acquire();
            total_role_denials_++;
            stats_lock_.release();
            return kIOReturnNotPermitted;
        }
        a840_kmd_pack_boot_complete(&pack_mgr_);
        return kIOReturnSuccess;
    default:
        return kIOReturnUnsupported;
    }
}

/* ============================================================================
 * Method Handlers (Methods 22..26) & Unified Dispatcher
 * ============================================================================ */

IOReturn A840XnuSystemBridge::handleMethodSparse(
    uint64_t client_id,
    const void *in_buf, size_t in_len,
    void *out_buf, size_t out_len, size_t *out_written)
{
    if (in_buf == nullptr || out_buf == nullptr ||
        in_len != sizeof(A840KmdSparseBindReqV3) ||
        out_len != sizeof(A840KmdSparseBindReplyV3))
        return kIOReturnBadArgument;

    A840KmdSparseBindReqV3 req{};
    A840KmdSparseBindReplyV3 rep{};
    memcpy(&req, in_buf, sizeof(req));
    const A840KmdStatus st = a840_kmd_sparse_dispatch_v3(
        &sparse_mgr_, client_id, &req, sizeof(req), &rep, sizeof(rep));
    if (st != A840_KMD_OK)
        return kmdStatusToIOReturn(st);
    memcpy(out_buf, &rep, sizeof(rep));
    if (out_written != nullptr)
        *out_written = sizeof(rep);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodSharedPack(
    uint64_t client_id,
    const void *in_buf, size_t in_len,
    void *out_buf, size_t out_len, size_t *out_written)
{
    if (in_buf == nullptr || out_buf == nullptr ||
        in_len != sizeof(struct NativeAdrenoAbiSharedPackReqV3) ||
        out_len != sizeof(struct NativeAdrenoAbiSharedPackReplyV3))
        return kIOReturnBadArgument;

    struct NativeAdrenoAbiSharedPackReqV3 req{};
    struct NativeAdrenoAbiSharedPackReplyV3 rep{};
    memcpy(&req, in_buf, sizeof(req));

    const A840KmdStatus st = a840_kmd_pack_map_shared_pack(
        &pack_mgr_, client_id, &req, A840_KMD_SMMU_PERM_READ_ONLY, &rep);
    if (st != A840_KMD_OK)
        return kmdStatusToIOReturn(st);
    memcpy(out_buf, &rep, sizeof(rep));
    if (out_written != nullptr)
        *out_written = sizeof(rep);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodWriterPromise(
    uint64_t client_id, bool is_backboardd,
    const void *in_buf, size_t in_len,
    void *out_buf, size_t out_len, size_t *out_written)
{
    if (in_buf == nullptr || out_buf == nullptr ||
        in_len != sizeof(struct NativeAdrenoAbiWriterPromiseV3) ||
        out_len != sizeof(struct NativeAdrenoAbiWriterPromiseReplyV3))
        return kIOReturnBadArgument;

    A840KmdWriterPromiseCall call{};
    call.writer_client = client_id;
    call.generation = 0u;
    call.flags = is_backboardd
                     ? A840_PROMISE_FLAG_EARLY_HANDOFF
                     : A840_PROMISE_FLAG_NONE;
    call.reserved = 0u;
    call.has_surface_identities = 0u;
    call.reserved2 = 0u;
    memcpy(&call.abi, in_buf, sizeof(call.abi));

    struct NativeAdrenoAbiWriterPromiseReplyV3 rep{};
    A840PromiseHandoffPolicy policy = A840_PROMISE_POLICY_NONE;
    const A840KmdStatus st = a840_kmd_writer_promise_dispatch(
        &promise_reg_, &call, &rep, &policy);
    if (st != A840_KMD_OK)
        return kmdStatusToIOReturn(st);
    memcpy(out_buf, &rep, sizeof(rep));
    if (out_written != nullptr)
        *out_written = sizeof(rep);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodSurfaceMeta(
    uint64_t client_id, bool is_backboardd,
    const void *in_buf, size_t in_len,
    void *out_buf, size_t out_len, size_t *out_written)
{
    if (in_buf == nullptr || out_buf == nullptr ||
        in_len != sizeof(struct NativeAdrenoAbiSurfaceMetaV3) ||
        out_len != sizeof(struct NativeAdrenoAbiSurfaceMetaV3))
        return kIOReturnBadArgument;

    struct NativeAdrenoAbiSurfaceMetaV3 req{};
    memcpy(&req, in_buf, sizeof(req));
    A840KmdSurfaceResult res{};
    const A840KmdStatus st = a840_kmd_surface_meta(
        &surface_mgr_, client_id, &req, is_backboardd ? 1u : 0u,
        &active_mdss_caps_, &res);
    if (st != A840_KMD_OK)
        return kmdStatusToIOReturn(st);
    memcpy(out_buf, &res.meta, sizeof(res.meta));
    if (out_written != nullptr)
        *out_written = sizeof(res.meta);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::handleMethodQueueConfig(
    uint64_t client_id, bool is_backboardd, void *smmu_ctx,
    const void *in_buf, size_t in_len,
    void *out_buf, size_t out_len, size_t *out_written)
{
    if (in_buf == nullptr || out_buf == nullptr ||
        in_len != sizeof(A840SysQueueConfigReqV3) ||
        out_len != sizeof(A840SysQueueConfigReplyV3))
        return kIOReturnBadArgument;

    A840SysQueueConfigReqV3 req{};
    memcpy(&req, in_buf, sizeof(req));

    if (req.flags != 0u &&
        a840_kmd_v3_freeze_policy_check_flags(&freeze_policy_, req.flags) != A840_KMD_OK)
        return kIOReturnUnsupported;

    A840SysQueueConfigReplyV3 rep{};
    rep.op = req.op;
    const uint64_t now_ns = (req.now_ns != 0u) ? req.now_ns : currentNowNs();

    switch (req.op) {
    case A840_SYS_QCFG_PREEMPT_RING_CONFIG: {
        if (req.u.preempt_cfg.reserved != 0u ||
            req.u.preempt_cfg.ring_id >= A840_RING_COUNT)
            return kIOReturnBadArgument;
        if (!is_backboardd && req.u.preempt_cfg.ring_id == A840_RING_0_COMPOSITOR) {
            stats_lock_.acquire();
            total_role_denials_++;
            stats_lock_.release();
            return kIOReturnNotPermitted;
        }
        const A840KmdStatus pst = a840_kmd_preempt_set_ring_level(
            &preempt_, req.u.preempt_cfg.ring_id, req.u.preempt_cfg.preempt_level);
        if (pst != A840_KMD_OK)
            return kmdStatusToIOReturn(pst);

        if (req.u.preempt_cfg.wakeup_mode <= A840_WAKE_MODE_COALESCED) {
            A840WakeupRingConfig wrc{};
            wrc.base_mode = req.u.preempt_cfg.wakeup_mode;
            const A840KmdStatus wst = a840_kmd_wakeup_configure_ring(
                &wakeup_, req.u.preempt_cfg.ring_id, &wrc);
            if (wst != A840_KMD_OK)
                return kmdStatusToIOReturn(wst);
        }

        A840PreemptSnapshot snap{};
        (void)a840_kmd_preempt_get_snapshot(&preempt_, &snap);
        const auto &rs = snap.rings[req.u.preempt_cfg.ring_id];
        rep.status_u32 = static_cast<uint32_t>(A840_KMD_OK);
        rep.u.preempt_cfg.ring_id = rs.ring_id;
        rep.u.preempt_cfg.preempt_level = rs.preempt_level;
        rep.u.preempt_cfg.effective_priority = rs.effective_priority;
        rep.u.preempt_cfg.health_state = rs.health_state;
        break;
    }

    case A840_SYS_QCFG_GMU_DCVS_HINT: {
        A840KmdStatus dst = A840_KMD_OK;
        switch (req.u.dcvs_hint.action) {
        case 0u:
            break;
        case 1u: {
            A840DcvsFrameSample smp{};
            smp.now_ns = now_ns;
            smp.gpu_busy_ns = req.u.dcvs_hint.gpu_busy_ns;
            smp.frame_period_ns = req.u.dcvs_hint.frame_period_ns;
            smp.scanout_margin_ns = req.u.dcvs_hint.scanout_margin_ns;
            smp.has_scanout_margin = req.u.dcvs_hint.has_scanout_margin;
            smp.ring0_submits = is_backboardd ? req.u.dcvs_hint.ring0_submits : 0u;
            smp.missed_deadline = req.u.dcvs_hint.missed_deadline;
            dst = a840_gmu_dcvs_record_frame(&gmu_dcvs_, &smp);
            break;
        }
        case 2u:
            dst = a840_gmu_dcvs_notify_touch(&gmu_dcvs_, now_ns, req.u.dcvs_hint.duration_ns);
            break;
        case 3u:
            if (!is_backboardd) {
                stats_lock_.acquire();
                total_role_denials_++;
                stats_lock_.release();
                return kIOReturnNotPermitted;
            }
            dst = a840_gmu_dcvs_update_thermal(&gmu_dcvs_, req.u.dcvs_hint.temp_mc, now_ns);
            break;
        case 4u:
            if (!is_backboardd) {
                stats_lock_.acquire();
                total_role_denials_++;
                stats_lock_.release();
                return kIOReturnNotPermitted;
            }
            dst = a840_gmu_dcvs_request_lease(
                &gmu_dcvs_, req.u.dcvs_hint.gpu_level_req, now_ns,
                req.u.dcvs_hint.duration_ns);
            break;
        default:
            return kIOReturnBadArgument;
        }
        if (dst != A840_KMD_OK)
            return kmdStatusToIOReturn(dst);

        A840GmuDcvsSnapshot dsnap{};
        (void)a840_gmu_dcvs_get_snapshot(&gmu_dcvs_, &dsnap);
        rep.status_u32 = static_cast<uint32_t>(A840_KMD_OK);
        rep.u.dcvs_hint.confirmed_gpu_level = dsnap.confirmed_gpu_level;
        rep.u.dcvs_hint.desired_gpu_level = dsnap.desired_gpu_level;
        rep.u.dcvs_hint.confirmed_gmu_level = dsnap.confirmed_gmu_level;
        rep.u.dcvs_hint.thermal_tier = dsnap.thermal_tier;
        rep.u.dcvs_hint.confirmed_gpu_freq_hz = dsnap.confirmed_gpu_freq_hz;
        rep.u.dcvs_hint.confirmed_gmu_freq_hz = dsnap.confirmed_gmu_freq_hz;
        rep.u.dcvs_hint.inter_frame_hold_hits = dsnap.inter_frame_hold_hits;
        rep.u.dcvs_hint.total_upscales = dsnap.total_upscales;
        rep.u.dcvs_hint.total_downscales = dsnap.total_downscales;
        break;
    }

    case A840_SYS_QCFG_DISPLAY_HANDOFF_120HZ: {
        if (!is_backboardd) {
            stats_lock_.acquire();
            total_role_denials_++;
            stats_lock_.release();
            return kIOReturnNotPermitted;
        }
        if (req.u.display_handoff.reserved != 0u)
            return kIOReturnBadArgument;

        A840KmdDisplayFrameRequest dfreq{};
        dfreq.consumer_client = client_id;
        dfreq.surface_handle = req.u.display_handoff.surface_handle;
        dfreq.surface_identity = req.u.display_handoff.surface_identity;
        dfreq.min_generation = req.u.display_handoff.min_generation;
        dfreq.timeout_ns = req.u.display_handoff.timeout_ns;
        dfreq.linear_resolve_handle = req.u.display_handoff.linear_resolve_handle;
        dfreq.linear_resolve_identity = req.u.display_handoff.linear_resolve_identity;
        dfreq.flags = req.u.display_handoff.handoff_flags;
        dfreq.reserved = 0u;
        dfreq.mdss_caps = &active_mdss_caps_;

        A840KmdDisplayHandoffResult dres{};
        const A840KmdStatus hst = a840_kmd_display_handoff_submit_frame(
            &display_handoff_, &dfreq, &dres);
        if (hst != A840_KMD_OK)
            return kmdStatusToIOReturn(hst);

        A840MdssCommitRequest mreq{};
        mreq.frame_id = (req.u.display_handoff.frame_id != 0u)
                            ? req.u.display_handoff.frame_id
                            : 1u;
        mreq.now_ns = now_ns;
        mreq.estimated_gpu_done_ns = req.u.display_handoff.estimated_gpu_done_ns;
        mreq.has_early_writer_promise = req.u.display_handoff.has_early_promise;
        mreq.is_retired_ready = req.u.display_handoff.is_retired_ready;
        mreq.ring0_compositor = 1u;
        mreq.reserved = 0u;

        A840MdssCommitPlan mplan{};
        const A840KmdStatus mst = a840_mdss_sync_plan_commit(&mdss_sync_, &mreq, &mplan);
        if (mst != A840_KMD_OK)
            return kmdStatusToIOReturn(mst);

        rep.status_u32 = static_cast<uint32_t>(A840_KMD_OK);
        rep.u.display_handoff.handoff_outcome = dres.outcome;
        rep.u.display_handoff.scanout_mode = dres.scanout_mode;
        rep.u.display_handoff.sync_mode = dres.sync_mode;
        rep.u.display_handoff.mdss_decision = mplan.decision;
        rep.u.display_handoff.extra_vsync_waits = mplan.extra_vsync_waits;
        rep.u.display_handoff.fallback_linear_required = dres.fallback_linear_required;
        rep.u.display_handoff.target_vsync_ns = mplan.target_vsync_ns;
        rep.u.display_handoff.scanout_margin_ns = mplan.scanout_margin_ns;
        break;
    }

    case A840_SYS_QCFG_KMD_V3_QUALIFY: {
        A840KmdProdQualReport qrep{};
        const IOReturn qrc = qualify_production(&qrep);
        rep.status_u32 = (qrc == kIOReturnSuccess) ? 0u : 1u;
        rep.u.qual_report = qrep;
        memcpy(out_buf, &rep, sizeof(rep));
        if (out_written != nullptr)
            *out_written = sizeof(rep);
        return qrc;
    }

    case A840_SYS_QCFG_ABI_NEGOTIATE: {
        A840SysAbiNegotiateReply nrep{};
        const IOReturn nrc = negotiate_abi(
            req.u.abi_negotiate.client_abi_version,
            req.u.abi_negotiate.requested_flags, &nrep);
        if (nrc != kIOReturnSuccess)
            return nrc;
        rep.status_u32 = 0u;
        rep.u.abi_negotiate = nrep;
        break;
    }

    case A840_SYS_QCFG_SPARSE_REGION_CREATE: {
        uint64_t region_id = 0u;
        const A840KmdStatus sst = a840_kmd_sparse_region_create(
            &sparse_mgr_, client_id, smmu_ctx,
            req.u.sparse_region.gpu_va, req.u.sparse_region.length_bytes,
            &region_id);
        if (sst != A840_KMD_OK)
            return kmdStatusToIOReturn(sst);
        rep.status_u32 = 0u;
        rep.u.sparse_region.region_id = region_id;
        rep.u.sparse_region.gpu_va = req.u.sparse_region.gpu_va;
        rep.u.sparse_region.length_bytes = req.u.sparse_region.length_bytes;
        break;
    }

    case A840_SYS_QCFG_SPARSE_REGION_DESTROY: {
        const A840KmdStatus sst = a840_kmd_sparse_region_destroy(
            &sparse_mgr_, client_id, req.u.sparse_region.region_id);
        if (sst != A840_KMD_OK)
            return kmdStatusToIOReturn(sst);
        rep.status_u32 = 0u;
        rep.u.sparse_region.region_id = req.u.sparse_region.region_id;
        break;
    }

    case A840_SYS_QCFG_TIER0_PACK_PUBLISH: {
        uint32_t pending = 0u;
        const IOReturn prc = publish_tier0_pack(
            client_id, req.u.pack_publish.stack_key,
            req.u.pack_publish.bytes, req.u.pack_publish.sha256, &pending);
        if (prc != kIOReturnSuccess)
            return prc;
        rep.status_u32 = 0u;
        rep.u.pack_publish.actual_stack_key = req.u.pack_publish.stack_key;
        rep.u.pack_publish.bytes = req.u.pack_publish.bytes;
        rep.u.pack_publish.replacement_pending = pending;
        break;
    }

    case A840_SYS_QCFG_BO_IMPORT_UBWC6: {
        A840SysBoImportUbwc6ReplyWire irep{};
        const IOReturn irc = import_iosurface_ubwc6(
            client_id, req.u.bo_import, &irep);
        if (irc != kIOReturnSuccess)
            return irc;
        rep.status_u32 = 0u;
        rep.u.bo_import = irep;
        break;
    }

    default:
        return kIOReturnUnsupported;
    }

    memcpy(out_buf, &rep, sizeof(rep));
    if (out_written != nullptr)
        *out_written = sizeof(rep);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::dispatchThunk(
    void *bridge_ctx,
    uint64_t client_id,
    uint32_t selector,
    const void *in_buf,
    size_t in_bytes,
    void *out_buf,
    size_t out_bytes,
    size_t *out_written)
{
    auto *bridge = static_cast<A840XnuSystemBridge *>(bridge_ctx);
    if (bridge == nullptr)
        return kIOReturnBadArgument;
    return bridge->dispatch_method(
        client_id, selector, in_buf, in_bytes, out_buf, out_bytes, out_written);
}

IOReturn A840XnuSystemBridge::dispatch_method(
    uint64_t client_id,
    uint32_t selector,
    const void *in_buf,
    size_t in_len,
    void *out_buf,
    size_t out_len,
    size_t *out_written)
{
    if (out_written != nullptr)
        *out_written = 0u;
    if (!initialized_)
        return kIOReturnNotReady;
    if (client_id == 0u)
        return kIOReturnBadArgument;
    if (in_len > NativeAdrenoAbiEnvelopeBytes ||
        out_len > NativeAdrenoAbiEnvelopeBytes)
        return kIOReturnBadArgument;

    const A840KmdStatus sel_st = a840_kmd_v3_freeze_policy_check_selector(
        &freeze_policy_, selector);
    if (sel_st != A840_KMD_OK)
        return kIOReturnUnsupported;

    if (abi_mode_ == A840_SYS_ABI_MODE_V2_COMPAT && selector > A840_KMD_V2_METHOD_MAX)
        return kIOReturnUnsupported;

    size_t slot = A840_SYS_MAX_CLIENTS;
    bool is_backboardd = false;
    void *smmu_ctx = nullptr;
    const IOReturn ref_rc = acquireClientRef(client_id, &slot, &is_backboardd, &smmu_ctx);
    if (ref_rc != kIOReturnSuccess)
        return ref_rc;

    size_t written = 0u;
    IOReturn result = kIOReturnUnsupported;

    switch (selector) {
    case A840_KMD_V3_METHOD_QUERY:
        result = handleMethodQuery(out_buf, out_len, &written);
        break;
    case A840_KMD_V3_METHOD_BO_ALLOC:
        result = handleMethodBoAlloc(
            client_id, smmu_ctx, in_buf, in_len, out_buf, out_len, &written);
        break;
    case A840_KMD_V3_METHOD_BO_MAP:
        result = handleMethodBoMap(
            client_id, in_buf, in_len, out_buf, out_len, &written);
        break;
    case A840_KMD_V3_METHOD_BO_FREE:
        result = handleMethodBoFree(client_id, in_buf, in_len);
        break;
    case A840_KMD_V3_METHOD_SUBMIT:
    case A840_KMD_V3_METHOD_SUBMIT_BATCH:
        result = handleMethodSubmit(
            slot, client_id, is_backboardd, in_buf, in_len, out_buf, out_len, &written);
        break;
    case A840_KMD_V3_METHOD_WAIT_FENCE:
        result = handleMethodWaitFence(client_id, in_buf, in_len);
        break;
    case A840_KMD_V3_METHOD_GET_TIMESTAMP:
        result = handleMethodGetTimestamp(out_buf, out_len, &written);
        break;
    case A840_KMD_V3_METHOD_GET_STATUS:
        result = handleMethodGetStatus(out_buf, out_len, &written);
        break;
    case A840_KMD_V3_METHOD_SET_METADATA:
        result = handleMethodMetadata(
            client_id, true, in_buf, in_len, out_buf, out_len, &written);
        break;
    case A840_KMD_V3_METHOD_GET_METADATA:
        result = handleMethodMetadata(
            client_id, false, in_buf, in_len, out_buf, out_len, &written);
        break;
    case A840_KMD_V3_METHOD_CREATE_QUEUE:
        result = handleMethodCreateQueue(
            slot, client_id, is_backboardd, in_buf, in_len, out_buf, out_len, &written);
        break;
    case A840_KMD_V3_METHOD_CLOSE_QUEUE:
        result = handleMethodCloseQueue(slot, client_id, in_buf, in_len);
        break;
    case A840_KMD_V3_METHOD_BO_CPU_MAP:
        result = handleMethodBoCpuMap(
            client_id, false, in_buf, in_len, out_buf, out_len, &written);
        break;
    case A840_KMD_V3_METHOD_BO_CPU_UNMAP:
        result = handleMethodBoCpuMap(
            client_id, true, in_buf, in_len, out_buf, out_len, &written);
        break;
    case A840_KMD_V3_METHOD_GET_DEVICE_INFO:
        result = handleMethodDeviceInfo(out_buf, out_len, &written);
        break;
    case A840_KMD_V3_METHOD_BO_IMPORT:
        result = handleMethodBoImport(
            client_id, in_buf, in_len, out_buf, out_len, &written);
        break;
    case A840_KMD_V3_METHOD_SET_PRIORITY:
        result = handleMethodSetPriority(slot, is_backboardd, in_buf, in_len);
        break;
    case A840_KMD_V3_METHOD_NOTE:
        result = handleMethodNote(is_backboardd, in_buf, in_len);
        break;
    case NativeAdrenoMethodWaitAnyV3:
    case NativeAdrenoMethodGetZeroPageV3: {
        const A840KmdStatus ust = a840_kmd_userclient_v3_dispatch(
            &uc_, client_id, selector, in_buf, in_len, out_buf, out_len, &written);
        result = kmdStatusToIOReturn(ust);
        break;
    }
    case NativeAdrenoMethodMapSparseV3:
        result = handleMethodSparse(
            client_id, in_buf, in_len, out_buf, out_len, &written);
        break;
    case NativeAdrenoMethodMapSharedPackV3:
        result = handleMethodSharedPack(
            client_id, in_buf, in_len, out_buf, out_len, &written);
        break;
    case NativeAdrenoMethodWriterPromiseV3:
        result = handleMethodWriterPromise(
            client_id, is_backboardd, in_buf, in_len, out_buf, out_len, &written);
        break;
    case NativeAdrenoMethodSurfaceMetaV3:
        result = handleMethodSurfaceMeta(
            client_id, is_backboardd, in_buf, in_len, out_buf, out_len, &written);
        break;
    case NativeAdrenoMethodQueueConfigV3:
        result = handleMethodQueueConfig(
            client_id, is_backboardd, smmu_ctx, in_buf, in_len, out_buf, out_len, &written);
        break;
    default:
        result = kIOReturnUnsupported;
        break;
    }

    stats_lock_.acquire();
    total_dispatches_++;
    if (selector <= A840_KMD_V2_METHOD_MAX)
        total_v2_dispatches_++;
    else
        total_v3_dispatches_++;
    stats_lock_.release();

    if (out_written != nullptr && result == kIOReturnSuccess)
        *out_written = written;

    releaseClientRef(slot);
    return result;
}

IOReturn A840XnuSystemBridge::on_gpu_fence_irq(
    uint32_t ring, uint64_t fence, uint64_t now_ns, uint64_t ticks)
{
    if (!initialized_)
        return kIOReturnNotReady;
    if (ring >= A840_RING_COUNT || fence == 0u)
        return kIOReturnBadArgument;

    const uint64_t ts_ns = (now_ns != 0u) ? now_ns : currentNowNs();
    const uint64_t ts_ticks = (ticks != 0u) ? ticks : currentTicks();

    A840PreemptSnapshot psnap{};
    if (a840_kmd_preempt_get_snapshot(&preempt_, &psnap) == A840_KMD_OK) {
        if (psnap.state == A840_PREEMPT_TRIGGERED) {
            (void)a840_kmd_preempt_on_hw_in_progress(&preempt_, ts_ns);
            (void)a840_kmd_preempt_on_switch_done(&preempt_, ts_ns);
            (void)a840_kmd_preempt_ack_complete(&preempt_);
        } else if (psnap.state == A840_PREEMPT_IN_PROGRESS) {
            (void)a840_kmd_preempt_on_switch_done(&preempt_, ts_ns);
            (void)a840_kmd_preempt_ack_complete(&preempt_);
        }
    }

    (void)a840_kmd_preempt_on_ring_retire(&preempt_, ring, fence, ts_ns);

    uint32_t woken = 0u;
    (void)a840_kmd_wakeup_on_irq(&wakeup_, ring, fence, ts_ns, &woken);

    RingTrackEntry completed[A840_SYS_MAX_PENDING_RING_TRACK]{};
    size_t completed_cnt = 0u;

    aux_lock_.acquire();
    for (size_t i = 0u; i < A840_SYS_MAX_PENDING_RING_TRACK; ++i) {
        if (ring_tracks_[i].active &&
            ring_tracks_[i].ring_id == ring &&
            ring_tracks_[i].ring_seqno <= fence) {
            completed[completed_cnt++] = ring_tracks_[i];
            ring_tracks_[i].active = 0u;
        }
    }
    aux_lock_.release();

    for (size_t i = 0u; i < completed_cnt; ++i) {
        (void)a840_kmd_userclient_v3_signal_fence(
            &uc_, completed[i].client_id, completed[i].local_queue,
            completed[i].client_fence, ts_ticks);
        (void)a840_kmd_writer_promise_signal_fence(
            &promise_reg_, completed[i].client_id, completed[i].local_queue,
            completed[i].client_fence, ts_ticks);
        (void)a840_kmd_qr_submit_complete(
            &quota_recovery_, completed[i].client_id, completed[i].local_queue,
            completed[i].client_fence, nullptr);
    }

    stats_lock_.acquire();
    total_fence_irqs_++;
    stats_lock_.release();
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::on_client_fence_irq(
    uint64_t client_id, uint64_t local_queue,
    uint32_t ring, uint64_t fence,
    uint64_t now_ns, uint64_t ticks)
{
    if (!initialized_)
        return kIOReturnNotReady;
    if (client_id == 0u || ring >= A840_RING_COUNT || fence == 0u)
        return kIOReturnBadArgument;

    const uint64_t ts_ns = (now_ns != 0u) ? now_ns : currentNowNs();
    const uint64_t ts_ticks = (ticks != 0u) ? ticks : currentTicks();

    (void)a840_kmd_userclient_v3_signal_fence(&uc_, client_id, local_queue, fence, ts_ticks);
    (void)a840_kmd_writer_promise_signal_fence(&promise_reg_, client_id, local_queue, fence, ts_ticks);
    (void)a840_kmd_qr_submit_complete(&quota_recovery_, client_id, local_queue, fence, nullptr);

    return on_gpu_fence_irq(ring, fence, ts_ns, ts_ticks);
}

IOReturn A840XnuSystemBridge::on_mdss_vblank(uint64_t ts_ns, uint64_t te_seq)
{
    if (!initialized_)
        return kIOReturnNotReady;
    if (ts_ns == 0u)
        return kIOReturnBadArgument;

    const A840KmdStatus mst = a840_mdss_sync_on_vsync(&mdss_sync_, ts_ns, te_seq);
    if (mst != A840_KMD_OK)
        return kmdStatusToIOReturn(mst);

    (void)a840_gmu_dcvs_tick(&gmu_dcvs_, ts_ns);
    (void)a840_kmd_preempt_tick(&preempt_, ts_ns);
    uint32_t flushed = 0u;
    (void)a840_kmd_wakeup_poll_timers(&wakeup_, ts_ns, &flushed);
    uint32_t evicted = 0u;
    (void)a840_kmd_bo_pool_sweep_ttl(&bo_pool_, &evicted);

    stats_lock_.acquire();
    total_vblanks_++;
    stats_lock_.release();
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::negotiate_abi(
    uint32_t client_abi_version,
    uint64_t requested_flags,
    A840SysAbiNegotiateReply *out_reply)
{
    if (!initialized_ || out_reply == nullptr)
        return kIOReturnBadArgument;
    if (client_abi_version != NativeAdrenoAbiVersionV2 &&
        client_abi_version != NativeAdrenoAbiVersionV3)
        return kIOReturnUnsupported;

    const A840KmdStatus fst = a840_kmd_v3_freeze_policy_check_flags(
        &freeze_policy_, requested_flags);
    if (fst != A840_KMD_OK)
        return kIOReturnUnsupported;

    A840KmdV3FreezeStats fstats{};
    (void)a840_kmd_v3_freeze_policy_get_stats(&freeze_policy_, &fstats);

    memset(out_reply, 0, sizeof(*out_reply));
    out_reply->kernel_abi_mode = abi_mode_;
    out_reply->legacy_rejected_count = fstats.legacy_rejected_count;

    if (abi_mode_ == A840_SYS_ABI_MODE_V2_COMPAT ||
        client_abi_version == NativeAdrenoAbiVersionV2) {
        out_reply->active_abi_version = NativeAdrenoAbiVersionV2;
        out_reply->granted_flags =
            (requested_flags == 0u ? A840_KMD_V3_PROD_V2_CAP_MASK : requested_flags) &
            A840_KMD_V3_PROD_V2_CAP_MASK;
        out_reply->qualified = (abi_mode_ == A840_SYS_ABI_MODE_V3_NATIVE && qualified_) ? 1u : 0u;
        out_reply->legacy_paths_purged = (abi_mode_ == A840_SYS_ABI_MODE_V3_NATIVE) ? 1u : 0u;
        return kIOReturnSuccess;
    }

    out_reply->active_abi_version = NativeAdrenoAbiVersionV3;
    out_reply->granted_flags =
        (requested_flags == 0u ? static_cast<uint64_t>(advertised_caps_) : requested_flags) &
        static_cast<uint64_t>(advertised_caps_);
    out_reply->qualified = qualified_ ? 1u : 0u;
    out_reply->legacy_paths_purged = 1u;
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::publish_tier0_pack(
    uint64_t client_id,
    uint64_t stack_key,
    uint64_t bytes,
    const uint8_t sha256[A840_PACK_SHA256_BYTES],
    uint32_t *out_replacement_pending)
{
    if (!initialized_ || stack_key == 0u || bytes == 0u || sha256 == nullptr)
        return kIOReturnBadArgument;

    size_t slot = A840_SYS_MAX_CLIENTS;
    bool is_backboardd = false;
    void *smmu_ctx = nullptr;
    const IOReturn ref_rc = acquireClientRef(client_id, &slot, &is_backboardd, &smmu_ctx);
    if (ref_rc != kIOReturnSuccess)
        return ref_rc;

    if (!is_backboardd) {
        stats_lock_.acquire();
        total_role_denials_++;
        stats_lock_.release();
        releaseClientRef(slot);
        return kIOReturnNotPermitted;
    }

    aux_lock_.acquire();
    const uint32_t pslot = next_pack_slot_ % 4u;
    next_pack_slot_++;
    pack_sources_[pslot].active = 1u;
    pack_sources_[pslot].identity.actual_stack_key = stack_key;
    pack_sources_[pslot].identity.bytes = bytes;
    memcpy(pack_sources_[pslot].identity.sha256, sha256, A840_PACK_SHA256_BYTES);
    active_pack_slot_ = pslot;
    void *src_ptr = &pack_sources_[pslot];
    aux_lock_.release();

    const A840KmdStatus pst = a840_kmd_pack_publish(&pack_mgr_, src_ptr);
    if (pst != A840_KMD_OK) {
        releaseClientRef(slot);
        return kmdStatusToIOReturn(pst);
    }

    struct NativeAdrenoAbiSharedPackReplyV3 uc_pack{};
    uc_pack.gpu_va = next_pack_gpu_va_;
    uc_pack.cpu_va = next_pack_cpu_va_;
    uc_pack.bytes = bytes;
    uc_pack.actual_stack_key = stack_key;
    (void)a840_kmd_userclient_v3_configure_shared_pack(&uc_, &uc_pack, 1u, 1u);

    tier0_pack_published_ = true;
    if (out_replacement_pending != nullptr) {
        A840KmdPackStats pstats{};
        if (a840_kmd_pack_stats(&pack_mgr_, client_id, &pstats) == A840_KMD_OK)
            *out_replacement_pending = pstats.replacement_pending;
    }

    releaseClientRef(slot);
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::validate_submit_ib(
    uint64_t client_id,
    uint64_t local_queue,
    const uint32_t *ib_words,
    size_t ib_dwords,
    A840KmdIbDiagResult *out_diag)
{
    if (!initialized_)
        return kIOReturnNotReady;
    if (client_id == 0u || ib_words == nullptr || ib_dwords == 0u)
        return kIOReturnBadArgument;

    const A840KmdStatus ch_st = a840_kmd_userclient_v3_check_submit_channel(
        &uc_, client_id, local_queue);
    if (ch_st != A840_KMD_OK)
        return kmdStatusToIOReturn(ch_st);

    A840KmdIbDiagResult diag{};
    const A840KmdIbDiagStatus dst = a840_kmd_validate_ib_words(
        ib_words, ib_dwords, &diag);
    if (out_diag != nullptr)
        *out_diag = diag;
    if (dst != A840_KMD_IB_OK) {
        stats_lock_.acquire();
        total_ib_rejects_++;
        stats_lock_.release();
        return (dst == A840_KMD_IB_BLOCKED_REG)
                   ? kIOReturnNotPermitted
                   : kIOReturnBadArgument;
    }
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::report_client_fault(
    uint64_t client_id,
    uint64_t local_queue,
    const A840KmdFaultInput &fault,
    uint32_t fault_code,
    A840KmdRecoveryReport *out_report)
{
    if (!initialized_)
        return kIOReturnNotReady;
    if (client_id == 0u)
        return kIOReturnBadArgument;

    size_t slot = A840_SYS_MAX_CLIENTS;
    bool is_backboardd = false;
    void *smmu_ctx = nullptr;
    const IOReturn ref_rc = acquireClientRef(client_id, &slot, &is_backboardd, &smmu_ctx);
    if (ref_rc != kIOReturnSuccess)
        return ref_rc;

    A840KmdFaultInput f = fault;
    f.client_id = client_id;
    f.queue_id = local_queue;

    A840KmdFaultSnapshot snap{};
    (void)a840_kmd_userclient_v3_report_fault(&uc_, &f, &snap);

    (void)a840_kmd_writer_promise_fail_channel(
        &promise_reg_, client_id, local_queue, A840_PROMISE_CANCEL_GPU_FAULT);

    A840KmdRecoveryReport rrep{};
    const A840KmdStatus qst = a840_kmd_qr_handle_channel_fault(
        &quota_recovery_, &f, fault_code, &rrep);

    if (!is_backboardd) {
        const uint32_t prio = resolveClientPriority(slot, local_queue, A840_PRIORITY_COUNT);
        uint32_t ring_id = A840_RING_2_CLIENT_3D;
        if (a840_kmd_preempt_priority_to_ring(prio, &ring_id) == A840_KMD_OK &&
            ring_id != A840_RING_0_COMPOSITOR) {
            const uint64_t now_ns = currentNowNs();
            uint32_t woken = 0u;
            (void)a840_kmd_preempt_mark_ring_faulted(&preempt_, ring_id, now_ns);
            (void)a840_kmd_wakeup_fault_ring(&wakeup_, ring_id, now_ns, &woken);
            (void)a840_kmd_preempt_begin_ring_reset(&preempt_, ring_id);
            (void)a840_kmd_preempt_complete_ring_reset(&preempt_, ring_id, now_ns);
        }
    }

    stats_lock_.acquire();
    total_faults_isolated_++;
    stats_lock_.release();

    if (out_report != nullptr)
        *out_report = rrep;

    releaseClientRef(slot);
    return kmdStatusToIOReturn(qst);
}

IOReturn A840XnuSystemBridge::evaluate_backboardd_ca_env(
    const char *target_label,
    const char *existing_boot_args,
    char *out_boot_args,
    size_t out_cap,
    size_t *out_written_len,
    A840KmdCaEnvDecision *out_decision) const
{
    if (!initialized_)
        return kIOReturnNotReady;
    A840KmdCaEnvPolicyInput in{};
    in.target_label = target_label;
    in.native_metal_enabled = (abi_mode_ == A840_SYS_ABI_MODE_V3_NATIVE) ? 1u : 0u;
    in.phase4_pack_verified = 1u;
    in.reserved = 0u;
    const A840KmdStatus st = a840_kmd_ca_env_append_boot_args(
        &in, existing_boot_args, out_boot_args, out_cap, out_written_len, out_decision);
    return kmdStatusToIOReturn(st);
}

bool A840XnuSystemBridge::initialized() const { return initialized_; }
bool A840XnuSystemBridge::is_qualified() const { return qualified_; }
uint32_t A840XnuSystemBridge::abi_mode() const { return abi_mode_; }
uint32_t A840XnuSystemBridge::advertised_caps() const { return advertised_caps_; }

IOReturn A840XnuSystemBridge::get_stats(Stats *out_stats)
{
    if (!initialized_ || out_stats == nullptr)
        return kIOReturnBadArgument;

    Stats s{};
    s.initialized = initialized_ ? 1u : 0u;
    s.abi_mode = abi_mode_;

    clients_sync_.acquire();
    for (size_t i = 0u; i < A840_SYS_MAX_CLIENTS; ++i) {
        if (clients_[i].state == SLOT_OPEN) {
            s.active_clients++;
            if (clients_[i].is_backboardd)
                s.active_backboardd_clients++;
            else
                s.active_app_clients++;
        }
    }
    clients_sync_.release();

    stats_lock_.acquire();
    s.qualified = qualified_ ? 1u : 0u;
    s.total_dispatches = total_dispatches_;
    s.total_v2_dispatches = total_v2_dispatches_;
    s.total_v3_dispatches = total_v3_dispatches_;
    s.total_submits = total_submits_;
    s.total_ring0_submits = total_ring0_submits_;
    s.total_app_submits = total_app_submits_;
    s.total_fence_irqs = total_fence_irqs_;
    s.total_vblanks = total_vblanks_;
    s.total_faults_isolated = total_faults_isolated_;
    s.total_ib_rejects = total_ib_rejects_;
    s.total_role_denials = total_role_denials_;
    s.total_ubwc6_imports = total_ubwc6_imports_;
    stats_lock_.release();

    (void)a840_kmd_v3_freeze_policy_get_stats(&freeze_policy_, &s.freeze_stats);
    s.legacy_rejected_count = s.freeze_stats.legacy_rejected_count;
    (void)a840_kmd_display_handoff_get_stats(&display_handoff_, &s.display_handoff_stats);
    (void)a840_mdss_sync_get_stats(&mdss_sync_, &s.mdss_sync_stats);
    (void)a840_kmd_bo_pool_get_stats(&bo_pool_, &s.bo_pool_stats);
    (void)a840_kmd_sparse_get_stats(&sparse_mgr_, &s.sparse_stats);
    (void)a840_kmd_qr_get_stats(&quota_recovery_, &s.quota_stats);
    (void)a840_kmd_writer_promise_get_stats(&promise_reg_, &s.promise_stats);
    (void)a840_kmd_wait_any_get_stats(&uc_.wait_any, &s.wait_any_stats);
    (void)a840_kmd_zero_page_get_stats(&uc_.zero_page, &s.zero_page_stats);

    *out_stats = s;
    return kIOReturnSuccess;
}

IOReturn A840XnuSystemBridge::get_dcvs_snapshot(A840GmuDcvsSnapshot *out_snapshot)
{
    if (!initialized_ || out_snapshot == nullptr)
        return kIOReturnBadArgument;
    return kmdStatusToIOReturn(a840_gmu_dcvs_get_snapshot(&gmu_dcvs_, out_snapshot));
}

IOReturn A840XnuSystemBridge::get_preempt_snapshot(A840PreemptSnapshot *out_snapshot)
{
    if (!initialized_ || out_snapshot == nullptr)
        return kIOReturnBadArgument;
    return kmdStatusToIOReturn(a840_kmd_preempt_get_snapshot(&preempt_, out_snapshot));
}

IOReturn A840XnuSystemBridge::get_wakeup_snapshot(A840WakeupSnapshot *out_snapshot)
{
    if (!initialized_ || out_snapshot == nullptr)
        return kIOReturnBadArgument;
    return kmdStatusToIOReturn(a840_kmd_wakeup_get_snapshot(&wakeup_, out_snapshot));
}

IOReturn A840XnuSystemBridge::get_mdss_stats(A840MdssSyncStats *out_stats)
{
    if (!initialized_ || out_stats == nullptr)
        return kIOReturnBadArgument;
    return kmdStatusToIOReturn(a840_mdss_sync_get_stats(&mdss_sync_, out_stats));
}

IOReturn A840XnuSystemBridge::get_freeze_stats(A840KmdV3FreezeStats *out_stats)
{
    if (!initialized_ || out_stats == nullptr)
        return kIOReturnBadArgument;
    return kmdStatusToIOReturn(a840_kmd_v3_freeze_policy_get_stats(&freeze_policy_, out_stats));
}

IOReturn A840XnuSystemBridge::query_client_quota(
    uint64_t client_id, A840KmdClientQuotaInfo *out_info)
{
    if (!initialized_ || out_info == nullptr)
        return kIOReturnBadArgument;
    return kmdStatusToIOReturn(
        a840_kmd_qr_query_client(&quota_recovery_, client_id, out_info));
}

IOReturn A840XnuSystemBridge::query_channel_state(
    uint64_t client_id, uint64_t queue_id, A840KmdChannelInfo *out_info)
{
    if (!initialized_ || out_info == nullptr)
        return kIOReturnBadArgument;
    return kmdStatusToIOReturn(
        a840_kmd_qr_query_channel(&quota_recovery_, client_id, queue_id, out_info));
}
