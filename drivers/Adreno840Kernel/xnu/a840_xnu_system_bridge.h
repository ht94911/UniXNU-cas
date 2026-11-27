/* SPDX-License-Identifier: MIT */
#ifndef A840_XNU_SYSTEM_BRIDGE_H
#define A840_XNU_SYSTEM_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "NativeAdrenoABI_v3.h"
#include "chip_facts_a840.h"
#include "a840_abi_v3_host.h"
#include "a840_kmd_zero_page.h"
#include "a840_kmd_wait_any.h"
#include "a840_kmd_fault_capture.h"
#include "a840_kmd_userclient_v3.h"
#include "a840_kmd_ib_diag.h"
#include "a840_kmd_ubwc_surface.h"
#include "a840_kmd_tier0_pack.h"
#include "a840_kmd_writer_promise.h"
#include "a840_kmd_display_handoff.h"
#include "a840_kmd_sparse_va.h"
#include "a840_kmd_bo_pool.h"
#include "a840_kmd_quota_recovery.h"
#include "a840_kmd_ca_env_policy.h"
#include "a840_kmd_preempt_ring.h"
#include "a840_kmd_wakeup_latency.h"
#include "a840_kmd_gmu_dcvs.h"
#include "a840_kmd_mdss_sync.h"
#include "a840_kmd_v3_freeze.h"

#ifdef KERNEL
#include <IOKit/IOLocks.h>
#include <IOKit/IOReturn.h>
#include <IOKit/IOTypes.h>
#include "NativeAdrenoABI.h"
#else
#include <pthread.h>

#ifndef NATIVE_ADRENO_HOST_IORETURN_DEFINED
#define NATIVE_ADRENO_HOST_IORETURN_DEFINED
typedef int32_t IOReturn;
#define A840_IOKIT_COMMON_ERR(code) \
    (static_cast<IOReturn>((UINT32_C(14) << 28u) | static_cast<uint32_t>(code)))
enum : IOReturn {
    kIOReturnSuccess = 0,
    kIOReturnError = A840_IOKIT_COMMON_ERR(700u),
    kIOReturnNoMemory = A840_IOKIT_COMMON_ERR(701u),
    kIOReturnNoResources = A840_IOKIT_COMMON_ERR(702u),
    kIOReturnBadArgument = A840_IOKIT_COMMON_ERR(706u),
    kIOReturnUnsupported = A840_IOKIT_COMMON_ERR(711u),
    kIOReturnIOError = A840_IOKIT_COMMON_ERR(714u),
    kIOReturnBusy = A840_IOKIT_COMMON_ERR(725u),
    kIOReturnTimeout = A840_IOKIT_COMMON_ERR(726u),
    kIOReturnNotReady = A840_IOKIT_COMMON_ERR(728u),
    kIOReturnAborted = A840_IOKIT_COMMON_ERR(731u),
    kIOReturnNotPermitted = A840_IOKIT_COMMON_ERR(738u),
    kIOReturnNotFound = A840_IOKIT_COMMON_ERR(752u)
};
#undef A840_IOKIT_COMMON_ERR
#endif
#endif /* KERNEL */

/* ============================================================================
 * Fixed Architectural Constants for XNU System-Wide KMD v3 Integration Bridge
 * ============================================================================ */

enum : uint32_t {
    A840_SYS_MAX_CLIENTS = 64u,
    A840_SYS_MAX_QUEUES_PER_CLIENT = 8u,
    A840_SYS_MAX_METADATA_SLOTS = 64u,
    A840_SYS_MAX_METADATA_BYTES = 256u,
    A840_SYS_MAX_PENDING_RING_TRACK = 64u,
    A840_SYS_MAX_MDSS_FORMATS = 16u,
    A840_SYS_DISPLAY_WIDTH_NEZHA = 1200u,
    A840_SYS_DISPLAY_HEIGHT_NEZHA = 2608u,
    A840_SYS_CLIENT_LABEL_CAP = 64u,
    A840_SYS_PACK_SLOT_BYTES = 65536u
};

/* Dual-ABI operational mode for Native065 v2 coexistence vs KMD v3 cutover. */
enum A840SystemAbiMode : uint32_t {
    A840_SYS_ABI_MODE_V2_COMPAT = 2u,
    A840_SYS_ABI_MODE_V3_NATIVE = 3u
};

/* Method 26 (NativeAdrenoMethodQueueConfigV3) sub-operations. */
enum A840SysQueueConfigOp : uint32_t {
    A840_SYS_QCFG_NONE = 0u,
    A840_SYS_QCFG_PREEMPT_RING_CONFIG = 1u,
    A840_SYS_QCFG_GMU_DCVS_HINT = 2u,
    A840_SYS_QCFG_DISPLAY_HANDOFF_120HZ = 3u,
    A840_SYS_QCFG_KMD_V3_QUALIFY = 4u,
    A840_SYS_QCFG_ABI_NEGOTIATE = 5u,
    A840_SYS_QCFG_SPARSE_REGION_CREATE = 6u,
    A840_SYS_QCFG_SPARSE_REGION_DESTROY = 7u,
    A840_SYS_QCFG_TIER0_PACK_PUBLISH = 8u,
    A840_SYS_QCFG_BO_IMPORT_UBWC6 = 9u
};

/* Method 18 (NOTE) sub-operations for power/thermal/touch/pressure hints. */
enum A840SysNoteKind : uint32_t {
    A840_SYS_NOTE_NONE = 0u,
    A840_SYS_NOTE_TOUCH_BOOST = 1u,
    A840_SYS_NOTE_THERMAL_SAMPLE = 2u,
    A840_SYS_NOTE_COLD_LEASE_REQ = 3u,
    A840_SYS_NOTE_COLD_LEASE_RELEASE = 4u,
    A840_SYS_NOTE_MEMORY_PRESSURE = 5u,
    A840_SYS_NOTE_BOOT_COMPLETE = 6u
};

/* ============================================================================
 * Fixed-Size Wire Structures (Strictly <= NativeAdrenoAbiEnvelopeBytes = 3104)
 * ============================================================================ */

struct A840SysParamsWire {
    uint32_t abi_version;
    uint32_t input_bits;
    uint32_t output_bits;
    uint32_t flags;
    uint64_t max_bo_bytes;
};

struct A840SysDeviceInfoWire {
    uint32_t chip_id;
    uint32_t gmem_bytes;
    uint64_t gmem_base;
    uint64_t uche_trap_base;
    uint64_t va_start;
    uint64_t va_size;
    uint32_t highest_bank_bit;
    uint32_t ubwc_version;
    uint32_t ubwc_swizzle;
    uint32_t macrotile_mode;
    uint32_t smmu_page_shift;
    uint32_t flags;
};

struct A840SysBoAllocWire {
    uint64_t bytes;
    uint64_t alignment;
    uint32_t permissions;
    uint32_t cache_mode;
    uint32_t flags;
    uint32_t reserved;
};

struct A840SysBoAllocReplyWire {
    uint64_t handle;
    uint64_t gpu_va;
    uint64_t phys_addr;
    uint64_t bytes;
};

struct A840SysBoMapWire {
    uint64_t handle;
    uint64_t iova;
    uint32_t permissions;
    uint32_t reserved;
};

struct A840SysBoMapReplyWire {
    uint64_t gpu_va;
    uint64_t bytes;
};

struct A840SysBoFreeWire {
    uint64_t handle;
    uint32_t allow_park;
    uint32_t reserved;
};

struct A840SysBoImportUbwc6Wire {
    uint64_t surface_identity;
    uint64_t bytes;
    uint64_t display_base;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t meta_rows;
    uint32_t ubwc_enabled;
    uint32_t ubwc_version;
    uint32_t highest_bank_bit;
    uint32_t swizzle_levels;
    uint32_t macrotile_mode;
    uint32_t meta_pitch;
    uint64_t meta_offset;
    uint32_t flags;
    uint32_t reserved;
};

struct A840SysBoImportUbwc6ReplyWire {
    uint64_t bo_handle;
    uint64_t surface_identity;
    uint64_t gpu_va;
    uint64_t phys_addr;
    uint32_t scanout_eligible;
    uint32_t fallback_linear_required;
    uint32_t layout_valid;
    uint32_t reserved;
};

struct A840SysSubmitWire {
    uint64_t queue;
    uint64_t wait_fence;
    uint64_t wait_timeout_ns;
    uint64_t ib_va;
    uint64_t enqueue_ns;
    uint64_t frame_deadline_ns;
    uint64_t estimated_gpu_busy_ns;
    uint64_t surface_identity;
    uint64_t surface_handle;
    uint64_t bo_handles[A840_KMD_SUBMIT_MAX_BOS];
    uint32_t bo_count;
    uint32_t ib_dwords;
    uint32_t priority_hint;
    uint32_t has_dependency;
    uint32_t dep_ring;
    uint32_t attach_display_sync;
    uint64_t dep_seqno;
    uint64_t frame_id;
    uint32_t flags;
    uint32_t reserved;
};

struct A840SysSubmitReplyWire {
    uint64_t fence;
    uint64_t ring_seqno;
    uint32_t assigned_ring;
    uint32_t preempt_triggered;
    uint32_t doorbell_emitted;
    uint32_t mdss_decision;
    uint32_t extra_vsync_waits;
    uint32_t confirmed_gpu_level;
    uint32_t confirmed_gmu_level;
    uint32_t reserved;
};

struct A840SysWaitFenceWire {
    uint64_t queue;
    uint64_t fence;
    uint64_t timeout_ns;
};

struct A840SysTimestampReplyWire {
    uint64_t ticks;
    uint64_t now_ns;
};

struct A840SysStatusReplyWire {
    uint64_t faults;
    uint64_t recoveries;
    uint32_t abi_mode;
    uint32_t qualified;
};

struct A840SysMetadataWire {
    uint64_t handle;
    uint32_t bytes;
    uint32_t reserved;
    uint8_t data[A840_SYS_MAX_METADATA_BYTES];
};

struct A840SysQueueWire {
    uint64_t queue;
    uint32_t priority;
    uint32_t reserved;
};

struct A840SysBoCpuMapWire {
    uint64_t handle;
    uint32_t write_access;
    uint32_t reserved;
};

struct A840SysBoCpuMapReplyWire {
    uint64_t token;
    uint64_t bytes;
    uint32_t active_leases;
    uint32_t reserved;
};

struct A840SysSetPriorityWire {
    uint64_t queue;
    uint32_t priority;
    uint32_t reserved;
};

struct A840SysNoteWire {
    uint32_t kind;
    uint32_t param_u32;
    int32_t  param_i32;
    uint32_t reserved;
    uint64_t now_ns;
    uint64_t value_u64;
};

struct A840SysAbiNegotiateReq {
    uint32_t client_abi_version;
    uint32_t reserved;
    uint64_t requested_flags;
};

struct A840SysAbiNegotiateReply {
    uint32_t active_abi_version;
    uint32_t kernel_abi_mode;
    uint32_t qualified;
    uint32_t legacy_paths_purged;
    uint64_t granted_flags;
    uint64_t legacy_rejected_count;
};

struct A840SysQueueConfigReqV3 {
    uint32_t op;               /* A840SysQueueConfigOp */
    uint32_t flags;
    uint64_t now_ns;
    union {
        struct {
            uint32_t ring_id;
            uint32_t preempt_level;
            uint32_t wakeup_mode;
            uint32_t reserved;
        } preempt_cfg;
        struct {
            uint32_t action;          /* 0=snapshot, 1=frame_sample, 2=touch, 3=thermal, 4=lease */
            uint32_t gpu_level_req;
            int32_t  temp_mc;
            uint32_t ring0_submits;
            uint64_t gpu_busy_ns;
            uint64_t frame_period_ns;
            uint64_t duration_ns;
            int64_t  scanout_margin_ns;
            uint32_t has_scanout_margin;
            uint32_t missed_deadline;
        } dcvs_hint;
        struct {
            uint64_t frame_id;
            uint64_t surface_handle;
            uint64_t surface_identity;
            uint64_t min_generation;
            uint64_t timeout_ns;
            uint64_t estimated_gpu_done_ns;
            uint64_t linear_resolve_handle;
            uint64_t linear_resolve_identity;
            uint32_t handoff_flags;
            uint32_t has_early_promise;
            uint32_t is_retired_ready;
            uint32_t reserved;
        } display_handoff;
        A840SysAbiNegotiateReq abi_negotiate;
        struct {
            uint64_t gpu_va;
            uint64_t length_bytes;
            uint64_t region_id;
        } sparse_region;
        struct {
            uint64_t stack_key;
            uint64_t bytes;
            uint8_t  sha256[A840_PACK_SHA256_BYTES];
        } pack_publish;
        A840SysBoImportUbwc6Wire bo_import;
    } u;
};

struct A840SysQueueConfigReplyV3 {
    uint32_t op;
    uint32_t status_u32;
    union {
        struct {
            uint32_t ring_id;
            uint32_t preempt_level;
            uint32_t effective_priority;
            uint32_t health_state;
        } preempt_cfg;
        struct {
            uint32_t confirmed_gpu_level;
            uint32_t desired_gpu_level;
            uint32_t confirmed_gmu_level;
            uint32_t thermal_tier;
            uint64_t confirmed_gpu_freq_hz;
            uint64_t confirmed_gmu_freq_hz;
            uint64_t inter_frame_hold_hits;
            uint64_t total_upscales;
            uint64_t total_downscales;
        } dcvs_hint;
        struct {
            uint32_t handoff_outcome;
            uint32_t scanout_mode;
            uint32_t sync_mode;
            uint32_t mdss_decision;
            uint32_t extra_vsync_waits;
            uint32_t fallback_linear_required;
            uint64_t target_vsync_ns;
            int64_t  scanout_margin_ns;
        } display_handoff;
        A840KmdProdQualReport qual_report;
        A840SysAbiNegotiateReply abi_negotiate;
        struct {
            uint64_t region_id;
            uint64_t gpu_va;
            uint64_t length_bytes;
        } sparse_region;
        struct {
            uint64_t actual_stack_key;
            uint64_t bytes;
            uint32_t replacement_pending;
            uint32_t reserved;
        } pack_publish;
        A840SysBoImportUbwc6ReplyWire bo_import;
    } u;
};

static_assert(sizeof(A840SysParamsWire) == 24u, "SysParamsWire locked");
static_assert(sizeof(A840SysDeviceInfoWire) <= NativeAdrenoAbiEnvelopeBytes, "DeviceInfo fits");
static_assert(sizeof(A840SysBoImportUbwc6Wire) <= NativeAdrenoAbiEnvelopeBytes, "BoImport fits");
static_assert(sizeof(A840SysSubmitWire) <= NativeAdrenoAbiEnvelopeBytes, "Submit fits");
static_assert(sizeof(A840SysQueueConfigReqV3) <= NativeAdrenoAbiEnvelopeBytes, "QueueConfigReq fits");
static_assert(sizeof(A840SysQueueConfigReplyV3) <= NativeAdrenoAbiEnvelopeBytes, "QueueConfigReply fits");

/* ============================================================================
 * A840XnuSystemBridge: Unified Phase 2-6 XNU KMD v3 System Integration Bridge
 * ============================================================================ */

class A840XnuSystemBridge final {
public:
    struct Config {
        uint32_t abi_mode = A840_SYS_ABI_MODE_V3_NATIVE;
        uint32_t cap_flags = 0u; /* 0 -> full production v3 mask in v3 mode */
        uint32_t input_bits = 48u;
        uint32_t output_bits = 36u;
        uint64_t zero_page_default_va = UINT64_C(65536);
        uint64_t global_hard_quota_bytes = A840_KMD_QUOTA_DEFAULT_GLOBAL_HARD_BYTES;
        uint64_t global_soft_quota_bytes = A840_KMD_QUOTA_DEFAULT_GLOBAL_SOFT_BYTES;
        uint64_t compositor_reserve_bytes = A840_KMD_QUOTA_DEFAULT_COMPOSITOR_RESERVE_BYTES;
        uint64_t app_soft_quota_bytes = UINT64_C(536870912);   /* 512 MiB */
        uint64_t app_hard_quota_bytes = UINT64_C(1073741824);  /* 1 GiB */
        uint64_t compositor_soft_quota_bytes = UINT64_C(2147483648); /* 2 GiB */
        uint64_t compositor_hard_quota_bytes = UINT64_C(4294967296); /* 4 GiB */
        uint64_t initial_vsync_ns = UINT64_C(10000000);
        uint32_t initial_gpu_opp_level = A840_GPU_OPP_IDLE_IDX;
        uint32_t max_recoveries_before_ban = A840_KMD_RECOVERY_DEFAULT_MAX_RECOVERIES;
        bool audit_ib_on_submit = true;
        bool enforce_backboardd_ca_policy = true;
        uint64_t preempt_record_base_va = UINT64_C(16777216); /* 16 MiB, 64 KiB aligned */
        uint64_t preempt_record_bytes = A840_PREEMPT_RECORD_MIN_BYTES;
        uint64_t (*now_ns_cb)(void *) = nullptr;
        uint64_t (*read_ticks_cb)(void *) = nullptr;
        void *clock_cookie = nullptr;
        const A840KmdSmmuOps *smmu_ops = nullptr;
        const A840KmdMdssCaps *mdss_caps = nullptr;
        A840KmdMdssCommitFn mdss_commit_fn = nullptr;
        void *mdss_commit_cookie = nullptr;
    };

    struct ClientCloseReport {
        uint64_t client_id = 0u;
        uint32_t pid = 0u;
        uint32_t is_backboardd = 0u;
        A840KmdClientCloseSummary uc_summary{};
        size_t canceled_promises = 0u;
        A840KmdSparseTeardownSummary sparse_summary{};
        uint32_t bo_pool_reclaimed = 0u;
        uint32_t reserved = 0u;
    };

    struct Stats {
        uint32_t initialized = 0u;
        uint32_t abi_mode = 0u;
        uint32_t qualified = 0u;
        uint32_t active_clients = 0u;
        uint32_t active_backboardd_clients = 0u;
        uint32_t active_app_clients = 0u;
        uint64_t total_dispatches = 0u;
        uint64_t total_v2_dispatches = 0u;
        uint64_t total_v3_dispatches = 0u;
        uint64_t total_submits = 0u;
        uint64_t total_ring0_submits = 0u;
        uint64_t total_app_submits = 0u;
        uint64_t total_fence_irqs = 0u;
        uint64_t total_vblanks = 0u;
        uint64_t total_faults_isolated = 0u;
        uint64_t total_ib_rejects = 0u;
        uint64_t total_role_denials = 0u;
        uint64_t total_ubwc6_imports = 0u;
        uint64_t legacy_rejected_count = 0u;
        A840KmdV3FreezeStats freeze_stats{};
        A840KmdDisplayHandoffStats display_handoff_stats{};
        A840MdssSyncStats mdss_sync_stats{};
        A840KmdBoPoolStats bo_pool_stats{};
        A840KmdSparseStats sparse_stats{};
        A840KmdQuotaRecoveryStats quota_stats{};
        A840KmdPromiseStats promise_stats{};
        A840KmdWaitAnyStats wait_any_stats{};
        A840KmdZeroPageStats zero_page_stats{};
    };

    A840XnuSystemBridge() = default;
    ~A840XnuSystemBridge();

    A840XnuSystemBridge(const A840XnuSystemBridge &) = delete;
    A840XnuSystemBridge &operator=(const A840XnuSystemBridge &) = delete;
    A840XnuSystemBridge(A840XnuSystemBridge &&) = delete;
    A840XnuSystemBridge &operator=(A840XnuSystemBridge &&) = delete;

    /* Deterministic System Lifecycle */
    IOReturn init(const Config &config);
    IOReturn qualify_production(A840KmdProdQualReport *out_report = nullptr);
    IOReturn qualify_production_with_input(
        const A840KmdProdQualInput &input,
        A840KmdProdQualReport *out_report = nullptr);

    IOReturn register_client(
        uint32_t pid,
        bool is_backboardd,
        uint64_t *out_client_id,
        uint64_t requested_zero_page_va = 0u,
        void *smmu_ctx = nullptr,
        const char *process_label = nullptr);

    IOReturn register_client_with_id(
        uint64_t explicit_client_id,
        uint32_t pid,
        bool is_backboardd,
        uint64_t requested_zero_page_va = 0u,
        void *smmu_ctx = nullptr,
        const char *process_label = nullptr);

    IOReturn unregister_client(
        uint64_t client_id,
        ClientCloseReport *out_report = nullptr);

    IOReturn dispatch_method(
        uint64_t client_id,
        uint32_t selector,
        const void *in_buf,
        size_t in_len,
        void *out_buf,
        size_t out_len,
        size_t *out_written = nullptr);

    IOReturn on_gpu_fence_irq(
        uint32_t ring,
        uint64_t fence,
        uint64_t now_ns = 0u,
        uint64_t ticks = 0u);

    IOReturn on_client_fence_irq(
        uint64_t client_id,
        uint64_t local_queue,
        uint32_t ring,
        uint64_t fence,
        uint64_t now_ns = 0u,
        uint64_t ticks = 0u);

    IOReturn on_mdss_vblank(uint64_t ts_ns, uint64_t te_seq = 0u);
    IOReturn shutdown();

    /* Direct System Operations (also exposed through dispatch_method) */
    IOReturn negotiate_abi(
        uint32_t client_abi_version,
        uint64_t requested_flags,
        A840SysAbiNegotiateReply *out_reply);

    IOReturn publish_tier0_pack(
        uint64_t client_id,
        uint64_t stack_key,
        uint64_t bytes,
        const uint8_t sha256[A840_PACK_SHA256_BYTES],
        uint32_t *out_replacement_pending = nullptr);

    IOReturn import_iosurface_ubwc6(
        uint64_t client_id,
        const A840SysBoImportUbwc6Wire &req,
        A840SysBoImportUbwc6ReplyWire *out_reply);

    IOReturn validate_submit_ib(
        uint64_t client_id,
        uint64_t local_queue,
        const uint32_t *ib_words,
        size_t ib_dwords,
        A840KmdIbDiagResult *out_diag = nullptr);

    IOReturn report_client_fault(
        uint64_t client_id,
        uint64_t local_queue,
        const A840KmdFaultInput &fault,
        uint32_t fault_code,
        A840KmdRecoveryReport *out_report = nullptr);

    IOReturn evaluate_backboardd_ca_env(
        const char *target_label,
        const char *existing_boot_args,
        char *out_boot_args,
        size_t out_cap,
        size_t *out_written_len,
        A840KmdCaEnvDecision *out_decision = nullptr) const;

    /* Queries & Telemetry */
    bool initialized() const;
    bool is_qualified() const;
    uint32_t abi_mode() const;
    uint32_t advertised_caps() const;
    IOReturn get_stats(Stats *out_stats);
    IOReturn get_dcvs_snapshot(A840GmuDcvsSnapshot *out_snapshot);
    IOReturn get_preempt_snapshot(A840PreemptSnapshot *out_snapshot);
    IOReturn get_wakeup_snapshot(A840WakeupSnapshot *out_snapshot);
    IOReturn get_mdss_stats(A840MdssSyncStats *out_stats);
    IOReturn get_freeze_stats(A840KmdV3FreezeStats *out_stats);
    IOReturn query_client_quota(uint64_t client_id, A840KmdClientQuotaInfo *out_info);
    IOReturn query_channel_state(uint64_t client_id, uint64_t queue_id, A840KmdChannelInfo *out_info);

    static IOReturn kmdStatusToIOReturn(A840KmdStatus status);
    static IOReturn dispatchThunk(
        void *bridge_ctx,
        uint64_t client_id,
        uint32_t selector,
        const void *in_buf,
        size_t in_bytes,
        void *out_buf,
        size_t out_bytes,
        size_t *out_written);

private:
    struct BridgeLock {
#ifdef KERNEL
        IOLock *lock = nullptr;
#else
        pthread_mutex_t lock{};
        bool ready = false;
#endif
        bool init();
        void destroy();
        void acquire();
        void release();
    };

    struct BridgeCondLock {
#ifdef KERNEL
        IOLock *lock = nullptr;
        uint32_t event_token = 0u;
#else
        pthread_mutex_t lock{};
        pthread_cond_t cond{};
        bool ready = false;
#endif
        bool init();
        void destroy();
        void acquire();
        void release();
        void waitIndefinite();
        int waitTimed(uint64_t deadline_ns, int infinite, uint64_t now_ns);
        void wakeAll();
    };

    enum ClientSlotState : uint32_t {
        SLOT_FREE = 0u,
        SLOT_OPENING = 1u,
        SLOT_OPEN = 2u,
        SLOT_CLOSING = 3u
    };

    struct ClientRecord {
        uint32_t state = SLOT_FREE;
        uint32_t in_flight = 0u;
        uint32_t pid = 0u;
        uint32_t is_backboardd = 0u;
        uint32_t default_priority = A840_PRIORITY_NORMAL;
        uint32_t queue_count = 0u;
        uint64_t client_id = 0u;
        void *smmu_ctx = nullptr;
        uint64_t active_queues[A840_SYS_MAX_QUEUES_PER_CLIENT]{};
        uint32_t queue_priorities[A840_SYS_MAX_QUEUES_PER_CLIENT]{};
        uint64_t next_fence[A840_SYS_MAX_QUEUES_PER_CLIENT]{};
        char label[A840_SYS_CLIENT_LABEL_CAP]{};
    };

    struct MetadataSlot {
        uint32_t active = 0u;
        uint32_t bytes = 0u;
        uint64_t client_id = 0u;
        uint64_t bo_handle = 0u;
        uint8_t data[A840_SYS_MAX_METADATA_BYTES]{};
    };

    struct CpuMapLeaseSlot {
        uint32_t active = 0u;
        uint32_t leases = 0u;
        uint64_t client_id = 0u;
        uint64_t bo_handle = 0u;
        uint64_t token = 0u;
        uint64_t bytes = 0u;
    };

    struct RingTrackEntry {
        uint32_t active = 0u;
        uint32_t ring_id = 0u;
        uint64_t client_id = 0u;
        uint64_t local_queue = 0u;
        uint64_t client_fence = 0u;
        uint64_t ring_seqno = 0u;
    };

    struct PackSourceSlot {
        uint32_t active = 0u;
        uint32_t reserved = 0u;
        A840KmdPackIdentity identity{};
    };

    struct BuiltinZeroPageState {
        uint32_t allocated = 0u;
        uint32_t mapped_count = 0u;
        uint64_t backing_phys = 0u;
        uint64_t next_phys = 0u;
        alignas(64) uint8_t storage[A840_KMD_ZERO_PAGE_BYTES]{};
    };

    /* Static callback adapters (lock-safe, zero heap allocation). */
    static void cbLock(void *cookie);
    static void cbUnlock(void *cookie);
    static int cbWait(void *cookie, uint64_t deadline_ns, int infinite);
    static void cbWakeAll(void *cookie);
    static uint64_t cbNowNs(void *cookie);
    static uint64_t cbReadTicks(void *cookie);

    static A840KmdStatus cbZeroAlloc(
        void *cookie, uint64_t bytes, uint64_t *out_phys,
        void **out_cpu, void **out_handle);
    static void cbZeroFree(
        void *cookie, uint64_t phys, void *handle, uint64_t bytes);
    static A840KmdStatus cbZeroMapRo(
        void *cookie, uint64_t client_id, void *smmu_ctx, uint64_t gpu_va,
        uint64_t phys_addr, uint64_t bytes, uint32_t permissions);
    static A840KmdStatus cbZeroUnmap(
        void *cookie, uint64_t client_id, void *smmu_ctx, uint64_t gpu_va,
        uint64_t bytes);

    static A840KmdStatus cbBoSizeLookup(
        void *cookie, uint64_t client_id, uint64_t bo_handle, uint64_t *out_bo_bytes);

    static A840KmdStatus cbPackAcquire(void *cookie, void *source, A840KmdPackImage *out_img);
    static void cbPackRelease(void *cookie, void *retained_source);
    static A840KmdStatus cbPackWireRo(
        void *cookie, const A840KmdPackImage *img, uint32_t perm, void **out_backing);
    static void cbPackUnwire(void *cookie, void *backing);
    static A840KmdStatus cbPackBindRo(
        void *cookie, void *backing, uint64_t client, uint32_t perm,
        uint32_t map_cpu, A840KmdPackViews *views);
    static A840KmdStatus cbPackRevoke(void *cookie, void *backing, uint64_t client);

    static A840KmdStatus cbPreemptDoorbell(
        void *cookie, uint32_t ring_id, uint32_t wptr, uint64_t seqno, uint32_t reg_wptr_offset);
    static A840KmdStatus cbPreemptTrigger(
        void *cookie, uint32_t from_ring, uint32_t to_ring,
        uint32_t preempt_level, uint64_t preempt_record_va,
        uint64_t preempt_record_bytes, uint32_t reg_ctx_switch_cntl);
    static void cbPreemptPiBoost(
        void *cookie, uint32_t ring_id, uint32_t boosted,
        uint32_t boosted_by_ring, uint64_t boost_until_seqno, uint64_t now_ns);
    static void cbPreemptLatency(
        void *cookie, uint32_t latency_kind, uint32_t ring_id,
        uint64_t latency_ns, uint64_t now_ns);

    static A840KmdStatus cbGmuHfiSend(void *cookie, const A840GmuHfiRequest *req);
    static void cbMdssBoostHint(void *cookie, const A840MdssBoostHint *hint);

    static A840KmdStatus cbQrHwChannelReset(
        void *cookie, uint64_t client_id, uint64_t queue_id,
        uint64_t recovery_generation, uint32_t fault_kind);

    /* Internal helpers */
    uint64_t currentNowNs() const;
    uint64_t currentTicks() const;
    IOReturn acquireClientRef(
        uint64_t client_id,
        size_t *out_slot,
        bool *out_is_backboardd,
        void **out_smmu_ctx);
    void releaseClientRef(size_t slot);
    uint32_t resolveClientPriority(size_t slot, uint64_t queue_id, uint32_t hint) const;
    IOReturn ensureClientQueue(size_t slot, uint64_t queue_id, uint32_t *out_prio);

    IOReturn handleMethodQuery(void *out_buf, size_t out_len, size_t *out_written);
    IOReturn handleMethodDeviceInfo(void *out_buf, size_t out_len, size_t *out_written);
    IOReturn handleMethodBoAlloc(
        uint64_t client_id, void *smmu_ctx,
        const void *in_buf, size_t in_len, void *out_buf, size_t out_len, size_t *out_written);
    IOReturn handleMethodBoMap(
        uint64_t client_id,
        const void *in_buf, size_t in_len, void *out_buf, size_t out_len, size_t *out_written);
    IOReturn handleMethodBoFree(
        uint64_t client_id, const void *in_buf, size_t in_len);
    IOReturn handleMethodBoImport(
        uint64_t client_id,
        const void *in_buf, size_t in_len, void *out_buf, size_t out_len, size_t *out_written);
    IOReturn handleMethodSubmit(
        size_t slot, uint64_t client_id, bool is_backboardd,
        const void *in_buf, size_t in_len, void *out_buf, size_t out_len, size_t *out_written);
    IOReturn handleMethodWaitFence(
        uint64_t client_id, const void *in_buf, size_t in_len);
    IOReturn handleMethodGetTimestamp(void *out_buf, size_t out_len, size_t *out_written);
    IOReturn handleMethodGetStatus(void *out_buf, size_t out_len, size_t *out_written);
    IOReturn handleMethodMetadata(
        uint64_t client_id, bool is_set,
        const void *in_buf, size_t in_len, void *out_buf, size_t out_len, size_t *out_written);
    IOReturn handleMethodCreateQueue(
        size_t slot, uint64_t client_id, bool is_backboardd,
        const void *in_buf, size_t in_len, void *out_buf, size_t out_len, size_t *out_written);
    IOReturn handleMethodCloseQueue(
        size_t slot, uint64_t client_id, const void *in_buf, size_t in_len);
    IOReturn handleMethodBoCpuMap(
        uint64_t client_id, bool is_unmap,
        const void *in_buf, size_t in_len, void *out_buf, size_t out_len, size_t *out_written);
    IOReturn handleMethodSetPriority(
        size_t slot, bool is_backboardd, const void *in_buf, size_t in_len);
    IOReturn handleMethodNote(
        bool is_backboardd, const void *in_buf, size_t in_len);
    IOReturn handleMethodSparse(
        uint64_t client_id,
        const void *in_buf, size_t in_len, void *out_buf, size_t out_len, size_t *out_written);
    IOReturn handleMethodSharedPack(
        uint64_t client_id,
        const void *in_buf, size_t in_len, void *out_buf, size_t out_len, size_t *out_written);
    IOReturn handleMethodWriterPromise(
        uint64_t client_id, bool is_backboardd,
        const void *in_buf, size_t in_len, void *out_buf, size_t out_len, size_t *out_written);
    IOReturn handleMethodSurfaceMeta(
        uint64_t client_id, bool is_backboardd,
        const void *in_buf, size_t in_len, void *out_buf, size_t out_len, size_t *out_written);
    IOReturn handleMethodQueueConfig(
        uint64_t client_id, bool is_backboardd, void *smmu_ctx,
        const void *in_buf, size_t in_len, void *out_buf, size_t out_len, size_t *out_written);

    void populateDefaultQualInput(A840KmdProdQualInput *out_in) const;

    /* Core configuration & state */
    bool initialized_ = false;
    uint32_t init_mask_ = 0u;
    bool qualified_ = false;
    bool tier0_pack_published_ = false;
    Config config_{};
    uint32_t abi_mode_ = A840_SYS_ABI_MODE_V3_NATIVE;
    uint32_t advertised_caps_ = 0u;
    uint64_t next_client_id_ = 1u;
    uint64_t next_cpu_token_ = 1u;
    uint64_t next_import_bo_handle_ = UINT64_C(1048576);
    uint64_t next_pack_gpu_va_ = UINT64_C(268435456); /* 256 MiB */
    uint64_t next_pack_cpu_va_ = UINT64_C(536870912); /* 512 MiB */

    /* Active MDSS capability profile (copied into fixed storage, zero heap). */
    uint32_t mdss_formats_[A840_SYS_MAX_MDSS_FORMATS]{};
    A840KmdMdssCaps active_mdss_caps_{};

    /* Dedicated synchronization locks (never nested across blocking waits). */
    BridgeCondLock clients_sync_{};
    BridgeCondLock wait_any_sync_{};
    BridgeCondLock promise_sync_{};
    BridgeLock stats_lock_{};
    BridgeLock surface_lock_{};
    BridgeLock pack_lock_{};
    BridgeLock display_lock_{};
    BridgeLock sparse_lock_{};
    BridgeLock bo_pool_lock_{};
    BridgeLock quota_lock_{};
    BridgeLock preempt_lock_{};
    BridgeLock wakeup_lock_{};
    BridgeLock gmu_lock_{};
    BridgeLock mdss_lock_{};
    BridgeLock freeze_lock_{};
    BridgeLock aux_lock_{};

    /* Sync ops descriptors handed to C11 KMD subsystems */
    A840KmdWaitBackend wait_backend_{};
    A840KmdSurfaceOps surface_ops_{};
    A840KmdPackMapOps pack_ops_{};
    A840KmdPromiseSyncOps promise_sync_ops_{};
    A840KmdDisplaySyncOps display_sync_ops_{};
    A840KmdSparseSyncOps sparse_sync_ops_{};
    A840KmdBoPoolSyncOps bo_pool_sync_ops_{};
    A840KmdQuotaSyncOps quota_sync_ops_{};
    A840KmdRecoveryOps quota_recovery_ops_{};
    A840PreemptSyncOps preempt_sync_ops_{};
    A840PreemptHwOps preempt_hw_ops_{};
    A840WakeupSyncOps wakeup_sync_ops_{};
    A840GmuDcvsSyncOps gmu_sync_ops_{};
    A840GmuHfiOps gmu_hfi_ops_{};
    A840MdssSyncLockOps mdss_sync_ops_{};
    A840MdssSyncCallbacks mdss_callbacks_{};
    A840KmdFreezeSyncOps freeze_sync_ops_{};

    /* Unified Phase 2-6 KMD Subsystems (all 18 modules integrated, zero heap) */
    A840KmdUserClientV3 uc_{};              /* ZeroPage + WaitAny + FaultCapture + UserClientV3 */
    A840KmdSurfaceManager surface_mgr_{};   /* UBWC 6.0 / HBB 16 IOSurface layout */
    A840KmdPackManager pack_mgr_{};         /* Shared RO Tier-0 AOT shader pack */
    A840KmdWriterPromiseRegistry promise_reg_{}; /* Method 24 writer promise */
    A840KmdDisplayHandoff display_handoff_{};    /* UBWC 6.0 + WriterPromise MDSS handoff */
    A840KmdSparseManager sparse_mgr_{};     /* Method 22 64 KiB Sparse VA */
    A840KmdBoPool bo_pool_{};               /* 7-bucket Parked / Volatile BO pool */
    A840KmdQuotaRecovery quota_recovery_{}; /* Multi-client quotas & channel recovery */
    A840KmdPreemptScheduler preempt_{};     /* 4-Ring L1A/L1B preemption scheduler */
    A840KmdWakeupEngine wakeup_{};          /* Immediate/coalesced wakeup + PI boost */
    A840KmdGmuDcvs gmu_dcvs_{};             /* 18-level GPU / 4-level GMU DCVS + 5-tier thermal */
    A840KmdMdssSync mdss_sync_{};           /* 120Hz 8.333ms scanout sync */
    A840KmdV3FreezePolicy freeze_policy_{}; /* ABI v3 freeze + Zero Legacy audit */

    /* Fixed-capacity runtime tables */
    BuiltinZeroPageState builtin_zp_{};
    ClientRecord clients_[A840_SYS_MAX_CLIENTS]{};
    MetadataSlot metadata_slots_[A840_SYS_MAX_METADATA_SLOTS]{};
    CpuMapLeaseSlot cpu_leases_[A840_SYS_MAX_METADATA_SLOTS]{};
    RingTrackEntry ring_tracks_[A840_SYS_MAX_PENDING_RING_TRACK]{};
    PackSourceSlot pack_sources_[4]{};
    uint32_t active_pack_slot_ = 0u;
    uint32_t next_pack_slot_ = 0u;
    A840KmdProdQualReport last_qual_report_{};

    /* Telemetry counters */
    uint64_t total_dispatches_ = 0u;
    uint64_t total_v2_dispatches_ = 0u;
    uint64_t total_v3_dispatches_ = 0u;
    uint64_t total_submits_ = 0u;
    uint64_t total_ring0_submits_ = 0u;
    uint64_t total_app_submits_ = 0u;
    uint64_t total_fence_irqs_ = 0u;
    uint64_t total_vblanks_ = 0u;
    uint64_t total_faults_isolated_ = 0u;
    uint64_t total_ib_rejects_ = 0u;
    uint64_t total_role_denials_ = 0u;
    uint64_t total_ubwc6_imports_ = 0u;
    uint64_t total_recoveries_ = 0u;
};

#endif /* A840_XNU_SYSTEM_BRIDGE_H */
