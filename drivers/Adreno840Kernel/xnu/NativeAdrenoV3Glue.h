/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_V3_GLUE_H
#define NATIVE_ADRENO_V3_GLUE_H

#include <stddef.h>
#include <stdint.h>

#include "NativeAdrenoABI_v3.h"
#include "a840_kmd_ib_diag.h"
#include "a840_kmd_userclient_v3.h"

#ifdef KERNEL
#include <IOKit/IOLocks.h>
#include <IOKit/IOReturn.h>
#include <IOKit/IOTypes.h>
#include "NativeAdrenoABI.h"
#include "NativeAdrenoMemory.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoTimer.h"
#include "NativeAdrenoUserClient.h"
#include "NativeAdrenoVm.h"
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

/* Exact wire-layout mirrors of frozen NativeAdrenoABI.h v2 records used on
 * Linux host builds (verified against gpu27-astra NativeAdrenoABI.h).
 */
enum : uint32_t {
    NativeAdrenoV2MethodQuery = 1u,
    NativeAdrenoV2MethodBoAlloc = 2u,
    NativeAdrenoV2MethodBoMap = 3u,
    NativeAdrenoV2MethodBoFree = 4u,
    NativeAdrenoV2MethodSubmit = 5u,
    NativeAdrenoV2MethodWaitFence = 6u,
    NativeAdrenoV2MethodGetTimestamp = 7u,
    NativeAdrenoV2MethodGetStatus = 8u,
    NativeAdrenoV2MethodSubmitBatch = 19u,
    NativeAdrenoV2MaxBuffers = 128u,
    NativeAdrenoV2BatchBegin = 1u,
    NativeAdrenoV2BatchAppend = 2u,
    NativeAdrenoV2BatchCommit = 3u,
    NativeAdrenoV2BatchAbort = 4u
};

struct NativeAdrenoV2ParamsWire {
    uint32_t abi_version;
    uint32_t input_bits;
    uint32_t output_bits;
    uint32_t flags;
    uint64_t max_bo_bytes;
};

struct NativeAdrenoV2SubmitBufferWire {
    uint64_t handle;
    uint64_t iova;
    uint64_t bytes;
};

struct NativeAdrenoV2SubmitWire {
    uint32_t buffer_count;
    uint32_t reserved;
    uint64_t queue;
    uint64_t wait_fence;
    uint64_t wait_timeout_ns;
    NativeAdrenoV2SubmitBufferWire buffers[NativeAdrenoV2MaxBuffers];
};

struct NativeAdrenoV2SubmitReplyWire {
    uint64_t fence;
};

struct NativeAdrenoV2SubmitBatchWire {
    uint64_t batch;
    uint64_t queue;
    uint32_t total_buffers;
    uint32_t offset;
    uint32_t buffer_count;
    uint32_t action;
    NativeAdrenoV2SubmitBufferWire buffers[NativeAdrenoV2MaxBuffers];
};

struct NativeAdrenoV2SubmitBatchReplyWire {
    uint64_t batch;
    uint64_t fence;
};

static_assert(sizeof(NativeAdrenoV2ParamsWire) == 24u, "v2 Params wire size");
static_assert(sizeof(NativeAdrenoV2SubmitBufferWire) == 24u, "v2 SubmitBuffer wire size");
static_assert(sizeof(NativeAdrenoV2SubmitWire) == NativeAdrenoAbiEnvelopeBytes, "v2 Submit envelope");
static_assert(sizeof(NativeAdrenoV2SubmitReplyWire) == 8u, "v2 SubmitReply wire size");
static_assert(sizeof(NativeAdrenoV2SubmitBatchWire) == NativeAdrenoAbiEnvelopeBytes, "v2 SubmitBatch envelope");
static_assert(sizeof(NativeAdrenoV2SubmitBatchReplyWire) == 16u, "v2 SubmitBatchReply wire size");
#endif /* KERNEL */

/* XNU IOKit v3 adapter glue connecting Wave 3 A840KmdUserClientV3 and Wave 4
 * a840_kmd_ib_diag to NativeAdrenoUserClient / NativeAdrenoVm / NativeAdrenoSubmit.
 *
 * Honest build/test boundary:
 *  - `#ifdef KERNEL`: binds to real XNU IOLock + IOLockSleepDeadline +
 *    thread_wakeup + mach_continuous_time + NativeAdrenoMemory +
 *    NativeAdrenoVm + NativeAdrenoUserClient + NativeAdrenoTimer. Compiled
 *    and qualified in Phase 2 Mac CI (XNU SDK, strict prelink/PAC/HMAC/QEMU)
 *    with `-DA840_KMD_CUSTOM_SYNC`.
 *  - `#ifndef KERNEL`: binds to real POSIX mutex/CLOCK_MONOTONIC condvar
 *    A840KmdWaitBackend + abstract host SMMU/v2/BO/IB callbacks so Linux host
 *    CI exercises full ASan/UBSan and GCC TSan 20x concurrent stress.
 */
class NativeAdrenoV3Glue final {
public:
#ifdef KERNEL
    using V2ClientHandle = NativeAdrenoUserClient *;
    using VmHandle = NativeAdrenoVm *;
    using SubmitHandle = NativeAdrenoSubmit *;
    using TimerHandle = const NativeAdrenoTimer *;
    using RingClockHandle = const NativeAdrenoRingClock *;
#else
    using V2ClientHandle = void *;
    using VmHandle = void *;
    using SubmitHandle = void *;
    using TimerHandle = const void *;
    using RingClockHandle = const void *;
#endif

    typedef void (*FaultLogCallback)(
        void *cookie,
        const char *ascii_line,
        size_t len,
        const A840KmdFaultRecord *record);

    typedef void (*IbRejectCallback)(
        void *cookie,
        uint64_t client_id,
        uint64_t queue_id,
        const A840KmdIbDiagResult *diag,
        const char *ascii_line);

#ifndef KERNEL
    /* Host-mode mock/adapter callbacks for Linux ASan/UBSan + TSan 20x. */
    struct HostOps {
        void *cookie = nullptr;

        /* Optional SMMU override. When all 4 pointers are NULL, the glue uses
         * its built-in 64 KiB aligned zero-page allocator & RO mapping tracker.
         */
        A840KmdSmmuOps smmu_ops{};

        /* Required for v2 methods 1..19 passthrough. */
        IOReturn (*v2_dispatch)(
            void *cookie,
            V2ClientHandle client,
            uint32_t selector,
            const void *in_buf,
            size_t in_bytes,
            void *out_buf,
            size_t out_bytes,
            size_t *out_written) = nullptr;

        /* Required for v3 Method 25 (SurfaceMetaV3) BO ownership & size check. */
        A840KmdStatus (*lookup_bo_size)(
            void *cookie,
            V2ClientHandle client,
            uint64_t bo_handle,
            uint64_t *out_bo_bytes) = nullptr;

        /* Optional IB word resolver for pre-submit audit on Method 5 / 19. */
        IOReturn (*resolve_ib_words)(
            void *cookie,
            V2ClientHandle client,
            VmHandle vm,
            uint64_t bo_handle,
            uint64_t iova,
            uint64_t bytes,
            const uint32_t **out_words,
            size_t *out_dwords) = nullptr;

        /* Optional monotonic clock (ns) override; NULL uses CLOCK_MONOTONIC. */
        uint64_t (*now_ns)(void *cookie) = nullptr;

        /* Optional hardware always-on counter ticks reader. */
        uint64_t (*read_ticks)(void *cookie) = nullptr;
    };
#endif

    struct Config {
        uint32_t cap_flags = 0u;             /* 0 selects A840_KMD_V3_DEFAULT_CAPS */
        uint32_t output_bits = 36u;          /* Physical address width for KMD alloc */
        uint64_t zero_page_default_va = 0u;  /* 64 KiB aligned 48-bit canonical GPU VA */
        bool audit_ib_on_submit = true;      /* Run a840_kmd_validate_ib_words before submit */
        TimerHandle hw_timer = nullptr;
        RingClockHandle ring_clock = nullptr;
        A840AbiV3PromiseObserver promise_observer = nullptr;
        void *promise_observer_cookie = nullptr;
        FaultLogCallback fault_log_cb = nullptr;
        void *fault_log_cookie = nullptr;
        IbRejectCallback ib_reject_cb = nullptr;
        void *ib_reject_cookie = nullptr;
#ifndef KERNEL
        HostOps host_ops{};
#endif
    };

    struct Stats {
        uint32_t open_clients = 0u;
        uint64_t v2_passthrough_calls = 0u;
        uint64_t v3_dispatch_calls = 0u;
        uint64_t ib_validated_ok = 0u;
        uint64_t ib_rejected_total = 0u;
        uint64_t ib_rejected_blocked_reg = 0u;
        uint64_t irq_signals = 0u;
        uint64_t faults_reported = 0u;
        uint64_t wait_backend_locks = 0u;
        uint64_t wait_backend_waits = 0u;
        uint64_t wait_backend_wakes = 0u;
        uint64_t wait_backend_timeouts = 0u;
        A840KmdZeroPageStats zero_page{};
        A840KmdWaitAnyStats wait_any{};
    };

    NativeAdrenoV3Glue() = default;
    ~NativeAdrenoV3Glue();

    NativeAdrenoV3Glue(const NativeAdrenoV3Glue &) = delete;
    NativeAdrenoV3Glue &operator=(const NativeAdrenoV3Glue &) = delete;
    NativeAdrenoV3Glue(NativeAdrenoV3Glue &&) = delete;
    NativeAdrenoV3Glue &operator=(NativeAdrenoV3Glue &&) = delete;

    IOReturn init(const Config &config);
    void destroy();

    bool initialized() const;
    uint32_t advertisedCaps() const;

    IOReturn configureSharedPack(
        const struct NativeAdrenoAbiSharedPackReplyV3 &pack,
        uint32_t gpu_readonly,
        uint32_t cpu_readonly);

    IOReturn setPromiseObserver(A840AbiV3PromiseObserver observer, void *cookie);

    IOReturn openClient(
        V2ClientHandle client,
        VmHandle vm,
        SubmitHandle submit,
        uint64_t requested_zero_page_va = 0u,
        uint64_t *out_client_id = nullptr);

    IOReturn closeClient(
        V2ClientHandle client,
        A840KmdClientCloseSummary *out_summary = nullptr);

    IOReturn externalMethod(
        V2ClientHandle client,
        uint32_t selector,
        const void *in_buf,
        size_t in_bytes,
        void *out_buf,
        size_t out_bytes,
        size_t *out_written = nullptr);

    IOReturn validateSubmitIb(
        V2ClientHandle client,
        uint64_t local_queue,
        const uint32_t *words,
        size_t dword_count,
        A840KmdIbDiagResult *out_diag = nullptr);

    IOReturn submitWithAudit(
        V2ClientHandle client,
        uint64_t local_queue,
        const uint32_t *ib_words,
        size_t ib_dwords,
        const void *submit_in,
        size_t submit_in_bytes,
        void *submit_out,
        size_t submit_out_bytes,
        A840KmdIbDiagResult *out_diag = nullptr,
        size_t *out_written = nullptr);

    IOReturn signalFenceIrq(
        V2ClientHandle client,
        uint64_t local_queue,
        uint64_t fence,
        uint64_t ticks);

    IOReturn signalFenceByClientId(
        uint64_t client_id,
        uint64_t local_queue,
        uint64_t fence,
        uint64_t ticks);

    IOReturn reportHardwareFault(
        V2ClientHandle client,
        const A840KmdFaultInput &fault,
        A840KmdFaultRecord *out_record = nullptr,
        char *out_ascii = nullptr,
        size_t out_ascii_cap = 0u,
        size_t *out_ascii_len = nullptr);

    IOReturn checkSubmitChannel(V2ClientHandle client, uint64_t local_queue);
    IOReturn getClientId(V2ClientHandle client, uint64_t *out_client_id) const;
    IOReturn getLastIbDiag(A840KmdIbDiagResult *out_diag) const;
    IOReturn getLatestFaultSnapshot(
        A840KmdFaultRecord *out_record,
        char *out_ascii = nullptr,
        size_t out_ascii_cap = 0u,
        size_t *out_ascii_len = nullptr);

    Stats getStats() const;

    static IOReturn kmdStatusToIOReturn(A840KmdStatus status);
    static IOReturn ibDiagStatusToIOReturn(A840KmdIbDiagStatus status);

private:
    enum ClientState : uint32_t {
        CLIENT_FREE = 0u,
        CLIENT_OPENING = 1u,
        CLIENT_OPEN = 2u,
        CLIENT_CLOSING = 3u
    };

    struct ClientSlot {
        uint32_t state = CLIENT_FREE;
        uint32_t in_flight = 0u;
        uint64_t client_id = 0u;
        V2ClientHandle v2_client = nullptr;
        VmHandle vm = nullptr;
        SubmitHandle submit = nullptr;
    };

    struct BoLookupSlot {
        uint32_t active = 0u;
        uint32_t reserved = 0u;
        uint64_t client_id = 0u;
        V2ClientHandle v2_client = nullptr;
    };

    struct VmMapRecord {
        uint32_t active = 0u;
        uint32_t permissions = 0u;
        uint64_t client_id = 0u;
        uint64_t gpu_va = 0u;
        uint64_t phys_addr = 0u;
        uint64_t bytes = 0u;
        uint64_t vm_handle = 0u;
        VmHandle vm = nullptr;
    };

    struct WaitBackendState {
        NativeAdrenoV3Glue *owner = nullptr;
        TimerHandle hw_timer = nullptr;
        uint64_t lock_calls = 0u;
        uint64_t wait_calls = 0u;
        uint64_t wake_all_calls = 0u;
        uint64_t timeout_count = 0u;
#ifdef KERNEL
        IOLock *lock = nullptr;
        uint32_t wait_event = 0u;
#else
        pthread_mutex_t lock{};
        pthread_cond_t cond{};
        bool sync_ready = false;
        uint64_t (*now_ns_cb)(void *) = nullptr;
        uint64_t (*read_ticks_cb)(void *) = nullptr;
        void *cb_cookie = nullptr;
#endif
    };

    struct SimpleLock {
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

    struct ClientSync {
#ifdef KERNEL
        IOLock *lock = nullptr;
        uint32_t drain_event = 0u;
#else
        pthread_mutex_t lock{};
        pthread_cond_t drain_cond{};
        bool ready = false;
#endif
        bool init();
        void destroy();
        void acquire();
        void release();
        void waitDrain();
        void wakeDrainAll();
    };

    static void waitLockThunk(void *cookie);
    static void waitUnlockThunk(void *cookie);
    static int waitSleepThunk(void *cookie, uint64_t deadline_ns, int infinite);
    static void waitWakeAllThunk(void *cookie);
    static uint64_t waitNowNsThunk(void *cookie);
    static uint64_t waitReadTicksThunk(void *cookie);

    static A840KmdStatus smmuAllocBackingThunk(
        void *cookie,
        uint64_t bytes,
        uint64_t *out_phys_addr,
        void **out_cpu_ptr,
        void **out_backing_handle);
    static void smmuFreeBackingThunk(
        void *cookie,
        uint64_t phys_addr,
        void *backing_handle,
        uint64_t bytes);
    static A840KmdStatus smmuMapClientRoThunk(
        void *cookie,
        uint64_t client_id,
        void *smmu_ctx,
        uint64_t gpu_va,
        uint64_t phys_addr,
        uint64_t bytes,
        uint32_t permissions);
    static A840KmdStatus smmuUnmapClientThunk(
        void *cookie,
        uint64_t client_id,
        void *smmu_ctx,
        uint64_t gpu_va,
        uint64_t bytes);

    static A840KmdStatus boSizeLookupThunk(
        void *cookie,
        uint64_t client_id,
        uint64_t bo_handle,
        uint64_t *out_bo_bytes);

    ClientSlot *findSlotLocked(V2ClientHandle client);
    const ClientSlot *findSlotLocked(V2ClientHandle client) const;
    void registerBoLookup(uint64_t client_id, V2ClientHandle client);
    void unregisterBoLookup(uint64_t client_id);
    V2ClientHandle lookupBoClient(uint64_t client_id);

    IOReturn auditIbBufferLockedFree(
        uint64_t client_id,
        uint64_t local_queue,
        const uint32_t *words,
        size_t dwords,
        A840KmdIbDiagResult *out_diag);

    IOReturn preSubmitGuardForMethod(
        uint64_t client_id,
        V2ClientHandle client,
        VmHandle vm,
        uint32_t selector,
        const void *in_buf,
        size_t in_bytes);

    IOReturn forwardV2Method(
        V2ClientHandle client,
        uint32_t selector,
        const void *in_buf,
        size_t in_bytes,
        void *out_buf,
        size_t out_bytes,
        size_t *out_written);

    void augmentQueryParams(void *out_buf, size_t out_bytes);

    bool initialized_ = false;
    bool audit_ib_on_submit_ = true;
    uint32_t output_bits_ = 36u;
    uint64_t next_client_id_ = 1u;

    RingClockHandle ring_clock_ = nullptr;
    FaultLogCallback fault_log_cb_ = nullptr;
    void *fault_log_cookie_ = nullptr;
    IbRejectCallback ib_reject_cb_ = nullptr;
    void *ib_reject_cookie_ = nullptr;

#ifdef KERNEL
    NativeAdrenoMemory zero_page_mem_{};
    bool zero_page_mem_live_ = false;
#else
    HostOps host_ops_{};
    bool use_custom_host_smmu_ = false;
#endif

    A840KmdUserClientV3 uc_{};
    A840KmdWaitBackend wait_backend_{};
    WaitBackendState wait_state_{};

    mutable ClientSync clients_sync_{};
    ClientSlot clients_[A840_KMD_MAX_CLIENTS]{};

    mutable SimpleLock bo_lookup_lock_{};
    BoLookupSlot bo_lookup_[A840_KMD_MAX_CLIENTS]{};

    mutable SimpleLock vm_map_lock_{};
    VmMapRecord vm_maps_[A840_KMD_ZERO_PAGE_MAX_CLIENTS]{};

    mutable SimpleLock diag_lock_{};
    bool has_last_ib_diag_ = false;
    A840KmdIbDiagResult last_ib_diag_{};
    uint64_t v2_passthrough_calls_ = 0u;
    uint64_t v3_dispatch_calls_ = 0u;
    uint64_t ib_validated_ok_ = 0u;
    uint64_t ib_rejected_total_ = 0u;
    uint64_t ib_rejected_blocked_reg_ = 0u;
    uint64_t irq_signals_ = 0u;
    uint64_t faults_reported_ = 0u;
};

#endif /* NATIVE_ADRENO_V3_GLUE_H */
