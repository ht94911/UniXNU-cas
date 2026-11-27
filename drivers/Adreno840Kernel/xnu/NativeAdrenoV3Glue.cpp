/* SPDX-License-Identifier: MIT */
#ifndef KERNEL
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#endif

#include "NativeAdrenoV3Glue.h"

#include <string.h>

#ifdef KERNEL
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#else
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#endif

namespace {

constexpr uint64_t kNsPerSec = UINT64_C(1000000000);
constexpr uint64_t kMaxSleepChunkNs = UINT64_C(31536000) * kNsPerSec;
constexpr uint64_t kHostZeroPagePhysBase = UINT64_C(1) << 32u;

#ifdef KERNEL
constexpr uint32_t kV2MethodQuery = NativeAdrenoMethodQuery;
constexpr uint32_t kV2MethodSubmit = NativeAdrenoMethodSubmit;
constexpr uint32_t kV2MethodSubmitBatch = NativeAdrenoMethodSubmitBatch;
constexpr uint32_t kV2MaxBuffers = NativeAdrenoAbiMaxBuffers;
constexpr uint32_t kV2BatchAppend = NativeAdrenoBatchAppend;
constexpr uint32_t kV2BatchAbort = NativeAdrenoBatchAbort;
constexpr size_t kV2ParamsBytes = sizeof(NativeAdrenoAbiParams);
constexpr size_t kV2SubmitBytes = sizeof(NativeAdrenoAbiSubmit);
constexpr size_t kV2SubmitBatchBytes = sizeof(NativeAdrenoAbiSubmitBatch);
using V2ParamsRecord = NativeAdrenoAbiParams;
using V2SubmitBufferRecord = NativeAdrenoAbiSubmitBuffer;
#else
constexpr uint32_t kV2MethodQuery = NativeAdrenoV2MethodQuery;
constexpr uint32_t kV2MethodSubmit = NativeAdrenoV2MethodSubmit;
constexpr uint32_t kV2MethodSubmitBatch = NativeAdrenoV2MethodSubmitBatch;
constexpr uint32_t kV2MaxBuffers = NativeAdrenoV2MaxBuffers;
constexpr uint32_t kV2BatchAppend = NativeAdrenoV2BatchAppend;
constexpr uint32_t kV2BatchAbort = NativeAdrenoV2BatchAbort;
constexpr size_t kV2ParamsBytes = sizeof(NativeAdrenoV2ParamsWire);
constexpr size_t kV2SubmitBytes = sizeof(NativeAdrenoV2SubmitWire);
constexpr size_t kV2SubmitBatchBytes = sizeof(NativeAdrenoV2SubmitBatchWire);
using V2ParamsRecord = NativeAdrenoV2ParamsWire;
using V2SubmitBufferRecord = NativeAdrenoV2SubmitBufferWire;
#endif

struct SubmitPrefixWire {
    uint32_t buffer_count;
    uint32_t reserved;
    uint64_t queue;
    uint64_t wait_fence;
    uint64_t wait_timeout_ns;
};

struct SubmitBatchPrefixWire {
    uint64_t batch;
    uint64_t queue;
    uint32_t total_buffers;
    uint32_t offset;
    uint32_t buffer_count;
    uint32_t action;
};

static_assert(sizeof(SubmitPrefixWire) == 32u, "Submit prefix layout");
static_assert(sizeof(SubmitBatchPrefixWire) == 32u, "SubmitBatch prefix layout");

#ifndef KERNEL
uint64_t hostMonotonicNowNs()
{
    struct timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0u;
    return static_cast<uint64_t>(ts.tv_sec) * kNsPerSec +
           static_cast<uint64_t>(ts.tv_nsec);
}
#endif

} /* namespace */

bool NativeAdrenoV3Glue::SimpleLock::init()
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

void NativeAdrenoV3Glue::SimpleLock::destroy()
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

void NativeAdrenoV3Glue::SimpleLock::acquire()
{
#ifdef KERNEL
    IOLockLock(lock);
#else
    (void)pthread_mutex_lock(&lock);
#endif
}

void NativeAdrenoV3Glue::SimpleLock::release()
{
#ifdef KERNEL
    IOLockUnlock(lock);
#else
    (void)pthread_mutex_unlock(&lock);
#endif
}

bool NativeAdrenoV3Glue::ClientSync::init()
{
#ifdef KERNEL
    lock = IOLockAlloc();
    drain_event = 0u;
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
        pthread_cond_init(&drain_cond, &attr) != 0) {
#else
    if (pthread_cond_init(&drain_cond, &attr) != 0) {
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

void NativeAdrenoV3Glue::ClientSync::destroy()
{
#ifdef KERNEL
    if (lock != nullptr) {
        IOLockFree(lock);
        lock = nullptr;
    }
#else
    if (ready) {
        (void)pthread_cond_destroy(&drain_cond);
        (void)pthread_mutex_destroy(&lock);
        ready = false;
    }
#endif
}

void NativeAdrenoV3Glue::ClientSync::acquire()
{
#ifdef KERNEL
    IOLockLock(lock);
#else
    (void)pthread_mutex_lock(&lock);
#endif
}

void NativeAdrenoV3Glue::ClientSync::release()
{
#ifdef KERNEL
    IOLockUnlock(lock);
#else
    (void)pthread_mutex_unlock(&lock);
#endif
}

void NativeAdrenoV3Glue::ClientSync::waitDrain()
{
#ifdef KERNEL
    (void)IOLockSleep(lock, &drain_event, THREAD_UNINT);
#else
    (void)pthread_cond_wait(&drain_cond, &lock);
#endif
}

void NativeAdrenoV3Glue::ClientSync::wakeDrainAll()
{
#ifdef KERNEL
    thread_wakeup(reinterpret_cast<event_t>(&drain_event));
#else
    (void)pthread_cond_broadcast(&drain_cond);
#endif
}

IOReturn NativeAdrenoV3Glue::kmdStatusToIOReturn(A840KmdStatus status)
{
    switch (status) {
    case A840_KMD_OK: return kIOReturnSuccess;
    case A840_KMD_PASS_TO_V2: return kIOReturnSuccess;
    case A840_KMD_INVALID_ARG: return kIOReturnBadArgument;
    case A840_KMD_RANGE: return kIOReturnBadArgument;
    case A840_KMD_ALIGNMENT: return kIOReturnBadArgument;
    case A840_KMD_PERMISSION_DENIED: return kIOReturnNotPermitted;
    case A840_KMD_CAPACITY: return kIOReturnNoResources;
    case A840_KMD_NOT_FOUND: return kIOReturnNotFound;
    case A840_KMD_STATE_CONFLICT: return kIOReturnBusy;
    case A840_KMD_STACK_KEY_MISMATCH: return kIOReturnBadArgument;
    case A840_KMD_UNSUPPORTED: return kIOReturnUnsupported;
    case A840_KMD_POISONED: return kIOReturnAborted;
    case A840_KMD_DISCONNECTED: return kIOReturnNotReady;
    case A840_KMD_IO_ERROR: return kIOReturnIOError;
    case A840_KMD_NO_MEMORY: return kIOReturnNoMemory;
    default: return kIOReturnError;
    }
}

IOReturn NativeAdrenoV3Glue::ibDiagStatusToIOReturn(A840KmdIbDiagStatus status)
{
    switch (status) {
    case A840_KMD_IB_OK: return kIOReturnSuccess;
    case A840_KMD_IB_BLOCKED_REG: return kIOReturnNotPermitted;
    case A840_KMD_IB_INVALID_ARG:
    case A840_KMD_IB_EMPTY:
    case A840_KMD_IB_UNKNOWN_PACKET:
    case A840_KMD_IB_BAD_PARITY:
    case A840_KMD_IB_BAD_HEADER:
    case A840_KMD_IB_TRUNCATED:
        return kIOReturnBadArgument;
    default:
        return kIOReturnError;
    }
}

void NativeAdrenoV3Glue::waitLockThunk(void *cookie)
{
    auto *st = static_cast<WaitBackendState *>(cookie);
#ifdef KERNEL
    IOLockLock(st->lock);
#else
    (void)pthread_mutex_lock(&st->lock);
#endif
    st->lock_calls++;
}

void NativeAdrenoV3Glue::waitUnlockThunk(void *cookie)
{
    auto *st = static_cast<WaitBackendState *>(cookie);
#ifdef KERNEL
    IOLockUnlock(st->lock);
#else
    (void)pthread_mutex_unlock(&st->lock);
#endif
}

uint64_t NativeAdrenoV3Glue::waitNowNsThunk(void *cookie)
{
    auto *st = static_cast<WaitBackendState *>(cookie);
#ifdef KERNEL
    (void)st;
    uint64_t ns = 0u;
    absolutetime_to_nanoseconds(mach_continuous_time(), &ns);
    return ns;
#else
    if (st->now_ns_cb != nullptr)
        return st->now_ns_cb(st->cb_cookie);
    return hostMonotonicNowNs();
#endif
}

uint64_t NativeAdrenoV3Glue::waitReadTicksThunk(void *cookie)
{
    auto *st = static_cast<WaitBackendState *>(cookie);
#ifdef KERNEL
    if (st->hw_timer != nullptr) {
        uint64_t ticks = 0u;
        if (st->hw_timer->read(&ticks) == kIOReturnSuccess)
            return ticks;
    }
    return 0u;
#else
    if (st->read_ticks_cb != nullptr)
        return st->read_ticks_cb(st->cb_cookie);
    return 0u;
#endif
}

int NativeAdrenoV3Glue::waitSleepThunk(void *cookie, uint64_t deadline_ns, int infinite)
{
    auto *st = static_cast<WaitBackendState *>(cookie);
    st->wait_calls++;
#ifdef KERNEL
    if (infinite != 0) {
        const int res = IOLockSleep(st->lock, &st->wait_event, THREAD_ABORTSAFE);
        if (res == THREAD_TIMED_OUT) {
            st->timeout_count++;
            return 1;
        }
        return 0;
    }
    const uint64_t now_ns = waitNowNsThunk(cookie);
    if (deadline_ns <= now_ns) {
        st->timeout_count++;
        return 1;
    }
    const uint64_t delta_ns = deadline_ns - now_ns;
    uint64_t delta_abs = 0u;
    nanoseconds_to_absolutetime(delta_ns, &delta_abs);
    const uint64_t abs_now = mach_absolute_time();
    const uint64_t abs_deadline = (delta_abs > UINT64_MAX - abs_now)
        ? UINT64_MAX : (abs_now + delta_abs);
    const int res = IOLockSleepDeadline(st->lock, &st->wait_event,
        static_cast<AbsoluteTime>(abs_deadline), THREAD_ABORTSAFE);
    if (res == THREAD_TIMED_OUT) {
        st->timeout_count++;
        return 1;
    }
    return 0;
#else
    if (infinite != 0) {
        (void)pthread_cond_wait(&st->cond, &st->lock);
        return 0;
    }
    const uint64_t now_ns = waitNowNsThunk(cookie);
    if (deadline_ns <= now_ns) {
        st->timeout_count++;
        return 1;
    }
    const uint64_t rem = deadline_ns - now_ns;
    const uint64_t chunk = (rem > kMaxSleepChunkNs) ? kMaxSleepChunkNs : rem;
    struct timespec ts{};
#if defined(__APPLE__)
    ts.tv_sec = static_cast<time_t>(chunk / kNsPerSec);
    ts.tv_nsec = static_cast<long>(chunk % kNsPerSec);
    const int rc = pthread_cond_timedwait_relative_np(&st->cond, &st->lock, &ts);
#else
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 1;
    ts.tv_sec += static_cast<time_t>(chunk / kNsPerSec);
    ts.tv_nsec += static_cast<long>(chunk % kNsPerSec);
    if (ts.tv_nsec >= static_cast<long>(kNsPerSec)) {
        ts.tv_sec += 1;
        ts.tv_nsec -= static_cast<long>(kNsPerSec);
    }
    const int rc = pthread_cond_timedwait(&st->cond, &st->lock, &ts);
#endif
    if (rc == ETIMEDOUT && rem <= kMaxSleepChunkNs) {
        st->timeout_count++;
        return 1;
    }
    return 0;
#endif
}

void NativeAdrenoV3Glue::waitWakeAllThunk(void *cookie)
{
    auto *st = static_cast<WaitBackendState *>(cookie);
    st->wake_all_calls++;
#ifdef KERNEL
    thread_wakeup(reinterpret_cast<event_t>(&st->wait_event));
#else
    (void)pthread_cond_broadcast(&st->cond);
#endif
}

A840KmdStatus NativeAdrenoV3Glue::smmuAllocBackingThunk(
    void *cookie,
    uint64_t bytes,
    uint64_t *out_phys_addr,
    void **out_cpu_ptr,
    void **out_backing_handle)
{
    auto *self = static_cast<NativeAdrenoV3Glue *>(cookie);
    if (self == nullptr || out_phys_addr == nullptr ||
        out_cpu_ptr == nullptr || out_backing_handle == nullptr ||
        bytes < A840_KMD_ZERO_PAGE_BYTES)
        return A840_KMD_INVALID_ARG;

#ifdef KERNEL
    if (self->zero_page_mem_live_)
        return A840_KMD_STATE_CONFLICT;
    const IOReturn st = self->zero_page_mem_.allocate(
        static_cast<size_t>(bytes),
        self->output_bits_,
        true,
        static_cast<size_t>(bytes));
    if (st != kIOReturnSuccess)
        return (st == kIOReturnNoMemory) ? A840_KMD_NO_MEMORY : A840_KMD_IO_ERROR;
    if (self->zero_page_mem_.spanCount() != 1u ||
        self->zero_page_mem_.spans() == nullptr ||
        self->zero_page_mem_.spans()[0].bytes < bytes) {
        (void)self->zero_page_mem_.release();
        return A840_KMD_IO_ERROR;
    }
    const uint64_t phys = self->zero_page_mem_.spans()[0].physical;
    if ((phys % A840_KMD_ZERO_PAGE_ALIGN) != 0u) {
        (void)self->zero_page_mem_.release();
        return A840_KMD_ALIGNMENT;
    }
    self->zero_page_mem_live_ = true;
    *out_phys_addr = phys;
    *out_cpu_ptr = self->zero_page_mem_.mapping();
    *out_backing_handle = &self->zero_page_mem_;
    return A840_KMD_OK;
#else
    if (self->use_custom_host_smmu_) {
        return self->host_ops_.smmu_ops.alloc_backing_page(
            self->host_ops_.smmu_ops.cookie, bytes,
            out_phys_addr, out_cpu_ptr, out_backing_handle);
    }
    void *mem = calloc(1u, static_cast<size_t>(bytes));
    if (mem == nullptr)
        return A840_KMD_NO_MEMORY;
    *out_phys_addr = kHostZeroPagePhysBase;
    *out_cpu_ptr = mem;
    *out_backing_handle = mem;
    return A840_KMD_OK;
#endif
}

void NativeAdrenoV3Glue::smmuFreeBackingThunk(
    void *cookie,
    uint64_t phys_addr,
    void *backing_handle,
    uint64_t bytes)
{
    auto *self = static_cast<NativeAdrenoV3Glue *>(cookie);
    if (self == nullptr)
        return;

#ifdef KERNEL
    (void)phys_addr;
    (void)bytes;
    if (backing_handle == &self->zero_page_mem_ && self->zero_page_mem_live_) {
        (void)self->zero_page_mem_.release();
        self->zero_page_mem_live_ = false;
    }
#else
    if (self->use_custom_host_smmu_) {
        self->host_ops_.smmu_ops.free_backing_page(
            self->host_ops_.smmu_ops.cookie, phys_addr, backing_handle, bytes);
        return;
    }
    (void)phys_addr;
    (void)bytes;
    free(backing_handle);
#endif
}

A840KmdStatus NativeAdrenoV3Glue::smmuMapClientRoThunk(
    void *cookie,
    uint64_t client_id,
    void *smmu_ctx,
    uint64_t gpu_va,
    uint64_t phys_addr,
    uint64_t bytes,
    uint32_t permissions)
{
    auto *self = static_cast<NativeAdrenoV3Glue *>(cookie);
    if (self == nullptr)
        return A840_KMD_INVALID_ARG;
    if (permissions != A840_KMD_SMMU_PERM_READ_ONLY ||
        (permissions & A840_KMD_SMMU_PERM_WRITE) != 0u)
        return A840_KMD_PERMISSION_DENIED;

#ifdef KERNEL
    auto *vm = static_cast<NativeAdrenoVm *>(smmu_ctx);
    if (vm == nullptr || !self->zero_page_mem_live_)
        return A840_KMD_INVALID_ARG;
    if (self->zero_page_mem_.publishForDevice(
            0u, static_cast<size_t>(bytes), false,
            false, NativeAdrenoPhaseMap) != kIOReturnSuccess)
        return A840_KMD_IO_ERROR;

    uint64_t vm_handle = 0u;
    const IOReturn map_st = vm->map(&self->zero_page_mem_, 0u, gpu_va,
        1u, 0u, 2u, &vm_handle);
    if (map_st != kIOReturnSuccess)
        return (map_st == kIOReturnNoMemory) ? A840_KMD_NO_MEMORY : A840_KMD_IO_ERROR;

    self->vm_map_lock_.acquire();
    VmMapRecord *free_slot = nullptr;
    for (size_t i = 0u; i < A840_KMD_ZERO_PAGE_MAX_CLIENTS; ++i) {
        if (!self->vm_maps_[i].active) {
            free_slot = &self->vm_maps_[i];
            break;
        }
    }
    if (free_slot == nullptr) {
        self->vm_map_lock_.release();
        (void)vm->unmap(vm_handle);
        return A840_KMD_CAPACITY;
    }
    free_slot->active = 1u;
    free_slot->permissions = permissions;
    free_slot->client_id = client_id;
    free_slot->gpu_va = gpu_va;
    free_slot->phys_addr = phys_addr;
    free_slot->bytes = bytes;
    free_slot->vm_handle = vm_handle;
    free_slot->vm = vm;
    self->vm_map_lock_.release();
    return A840_KMD_OK;
#else
    if (self->use_custom_host_smmu_) {
        return self->host_ops_.smmu_ops.map_client_ro(
            self->host_ops_.smmu_ops.cookie, client_id, smmu_ctx,
            gpu_va, phys_addr, bytes, permissions);
    }
    self->vm_map_lock_.acquire();
    VmMapRecord *free_slot = nullptr;
    for (size_t i = 0u; i < A840_KMD_ZERO_PAGE_MAX_CLIENTS; ++i) {
        if (self->vm_maps_[i].active && self->vm_maps_[i].client_id == client_id) {
            self->vm_map_lock_.release();
            return A840_KMD_STATE_CONFLICT;
        }
        if (!self->vm_maps_[i].active && free_slot == nullptr)
            free_slot = &self->vm_maps_[i];
    }
    if (free_slot == nullptr) {
        self->vm_map_lock_.release();
        return A840_KMD_CAPACITY;
    }
    free_slot->active = 1u;
    free_slot->permissions = permissions;
    free_slot->client_id = client_id;
    free_slot->gpu_va = gpu_va;
    free_slot->phys_addr = phys_addr;
    free_slot->bytes = bytes;
    free_slot->vm_handle = client_id;
    free_slot->vm = smmu_ctx;
    self->vm_map_lock_.release();
    return A840_KMD_OK;
#endif
}

A840KmdStatus NativeAdrenoV3Glue::smmuUnmapClientThunk(
    void *cookie,
    uint64_t client_id,
    void *smmu_ctx,
    uint64_t gpu_va,
    uint64_t bytes)
{
    auto *self = static_cast<NativeAdrenoV3Glue *>(cookie);
    if (self == nullptr)
        return A840_KMD_INVALID_ARG;

#ifdef KERNEL
    auto *vm = static_cast<NativeAdrenoVm *>(smmu_ctx);
    uint64_t vm_handle = 0u;
    bool found = false;

    self->vm_map_lock_.acquire();
    for (size_t i = 0u; i < A840_KMD_ZERO_PAGE_MAX_CLIENTS; ++i) {
        if (self->vm_maps_[i].active &&
            self->vm_maps_[i].client_id == client_id &&
            self->vm_maps_[i].gpu_va == gpu_va) {
            vm_handle = self->vm_maps_[i].vm_handle;
            if (vm == nullptr)
                vm = self->vm_maps_[i].vm;
            self->vm_maps_[i] = VmMapRecord{};
            found = true;
            break;
        }
    }
    self->vm_map_lock_.release();
    (void)bytes;
    if (!found || vm == nullptr)
        return A840_KMD_NOT_FOUND;
    return (vm->unmap(vm_handle) == kIOReturnSuccess) ? A840_KMD_OK : A840_KMD_IO_ERROR;
#else
    if (self->use_custom_host_smmu_) {
        return self->host_ops_.smmu_ops.unmap_client(
            self->host_ops_.smmu_ops.cookie, client_id, smmu_ctx, gpu_va, bytes);
    }
    (void)smmu_ctx;
    (void)bytes;
    self->vm_map_lock_.acquire();
    bool found = false;
    for (size_t i = 0u; i < A840_KMD_ZERO_PAGE_MAX_CLIENTS; ++i) {
        if (self->vm_maps_[i].active &&
            self->vm_maps_[i].client_id == client_id &&
            self->vm_maps_[i].gpu_va == gpu_va) {
            self->vm_maps_[i] = VmMapRecord{};
            found = true;
            break;
        }
    }
    self->vm_map_lock_.release();
    return found ? A840_KMD_OK : A840_KMD_NOT_FOUND;
#endif
}

void NativeAdrenoV3Glue::registerBoLookup(uint64_t client_id, V2ClientHandle client)
{
    bo_lookup_lock_.acquire();
    for (size_t i = 0u; i < A840_KMD_MAX_CLIENTS; ++i) {
        if (!bo_lookup_[i].active) {
            bo_lookup_[i].active = 1u;
            bo_lookup_[i].client_id = client_id;
            bo_lookup_[i].v2_client = client;
            break;
        }
    }
    bo_lookup_lock_.release();
}

void NativeAdrenoV3Glue::unregisterBoLookup(uint64_t client_id)
{
    bo_lookup_lock_.acquire();
    for (size_t i = 0u; i < A840_KMD_MAX_CLIENTS; ++i) {
        if (bo_lookup_[i].active && bo_lookup_[i].client_id == client_id) {
            bo_lookup_[i] = BoLookupSlot{};
            break;
        }
    }
    bo_lookup_lock_.release();
}

NativeAdrenoV3Glue::V2ClientHandle NativeAdrenoV3Glue::lookupBoClient(uint64_t client_id)
{
    V2ClientHandle client = nullptr;
    bo_lookup_lock_.acquire();
    for (size_t i = 0u; i < A840_KMD_MAX_CLIENTS; ++i) {
        if (bo_lookup_[i].active && bo_lookup_[i].client_id == client_id) {
            client = bo_lookup_[i].v2_client;
            break;
        }
    }
    bo_lookup_lock_.release();
    return client;
}

A840KmdStatus NativeAdrenoV3Glue::boSizeLookupThunk(
    void *cookie,
    uint64_t client_id,
    uint64_t bo_handle,
    uint64_t *out_bo_bytes)
{
    auto *self = static_cast<NativeAdrenoV3Glue *>(cookie);
    if (self == nullptr || out_bo_bytes == nullptr || bo_handle == 0u)
        return A840_KMD_INVALID_ARG;
    *out_bo_bytes = 0u;

    V2ClientHandle client = self->lookupBoClient(client_id);
    if (client == nullptr)
        return A840_KMD_NOT_FOUND;

#ifdef KERNEL
    NativeAdrenoMemory *mem = client->memoryOf(bo_handle);
    if (mem == nullptr)
        return A840_KMD_NOT_FOUND;
    *out_bo_bytes = static_cast<uint64_t>(mem->bytes());
    return A840_KMD_OK;
#else
    if (self->host_ops_.lookup_bo_size == nullptr)
        return A840_KMD_UNSUPPORTED;
    return self->host_ops_.lookup_bo_size(
        self->host_ops_.cookie, client, bo_handle, out_bo_bytes);
#endif
}

NativeAdrenoV3Glue::~NativeAdrenoV3Glue()
{
    destroy();
}

IOReturn NativeAdrenoV3Glue::init(const Config &config)
{
    if (initialized_)
        return kIOReturnBusy;

    if (!clients_sync_.init())
        return kIOReturnNoMemory;
    if (!bo_lookup_lock_.init()) {
        clients_sync_.destroy();
        return kIOReturnNoMemory;
    }
    if (!vm_map_lock_.init()) {
        bo_lookup_lock_.destroy();
        clients_sync_.destroy();
        return kIOReturnNoMemory;
    }
    if (!diag_lock_.init()) {
        vm_map_lock_.destroy();
        bo_lookup_lock_.destroy();
        clients_sync_.destroy();
        return kIOReturnNoMemory;
    }

    wait_state_ = WaitBackendState{};
    wait_state_.owner = this;
    wait_state_.hw_timer = config.hw_timer;

#ifdef KERNEL
    wait_state_.lock = IOLockAlloc();
    if (wait_state_.lock == nullptr) {
        diag_lock_.destroy();
        vm_map_lock_.destroy();
        bo_lookup_lock_.destroy();
        clients_sync_.destroy();
        return kIOReturnNoMemory;
    }
#else
    {
        pthread_condattr_t attr;
        if (pthread_mutex_init(&wait_state_.lock, nullptr) != 0) {
            diag_lock_.destroy();
            vm_map_lock_.destroy();
            bo_lookup_lock_.destroy();
            clients_sync_.destroy();
            return kIOReturnNoMemory;
        }
        if (pthread_condattr_init(&attr) != 0) {
            (void)pthread_mutex_destroy(&wait_state_.lock);
            diag_lock_.destroy();
            vm_map_lock_.destroy();
            bo_lookup_lock_.destroy();
            clients_sync_.destroy();
            return kIOReturnNoMemory;
        }
#if !defined(__APPLE__)
        if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0 ||
            pthread_cond_init(&wait_state_.cond, &attr) != 0) {
#else
        if (pthread_cond_init(&wait_state_.cond, &attr) != 0) {
#endif
            (void)pthread_condattr_destroy(&attr);
            (void)pthread_mutex_destroy(&wait_state_.lock);
            diag_lock_.destroy();
            vm_map_lock_.destroy();
            bo_lookup_lock_.destroy();
            clients_sync_.destroy();
            return kIOReturnNoMemory;
        }
        (void)pthread_condattr_destroy(&attr);
        wait_state_.sync_ready = true;
        wait_state_.now_ns_cb = config.host_ops.now_ns;
        wait_state_.read_ticks_cb = config.host_ops.read_ticks;
        wait_state_.cb_cookie = config.host_ops.cookie;
    }
    host_ops_ = config.host_ops;
    use_custom_host_smmu_ =
        (host_ops_.smmu_ops.alloc_backing_page != nullptr &&
         host_ops_.smmu_ops.free_backing_page != nullptr &&
         host_ops_.smmu_ops.map_client_ro != nullptr &&
         host_ops_.smmu_ops.unmap_client != nullptr);
    if (!use_custom_host_smmu_ &&
        (host_ops_.smmu_ops.alloc_backing_page != nullptr ||
         host_ops_.smmu_ops.free_backing_page != nullptr ||
         host_ops_.smmu_ops.map_client_ro != nullptr ||
         host_ops_.smmu_ops.unmap_client != nullptr)) {
        (void)pthread_cond_destroy(&wait_state_.cond);
        (void)pthread_mutex_destroy(&wait_state_.lock);
        wait_state_.sync_ready = false;
        diag_lock_.destroy();
        vm_map_lock_.destroy();
        bo_lookup_lock_.destroy();
        clients_sync_.destroy();
        return kIOReturnBadArgument;
    }
#endif

    audit_ib_on_submit_ = config.audit_ib_on_submit;
    output_bits_ = (config.output_bits >= 32u && config.output_bits <= 48u)
        ? config.output_bits : 36u;
    ring_clock_ = config.ring_clock;
    fault_log_cb_ = config.fault_log_cb;
    fault_log_cookie_ = config.fault_log_cookie;
    ib_reject_cb_ = config.ib_reject_cb;
    ib_reject_cookie_ = config.ib_reject_cookie;
    next_client_id_ = 1u;

    for (size_t i = 0u; i < A840_KMD_MAX_CLIENTS; ++i) clients_[i] = ClientSlot{};
    for (size_t i = 0u; i < A840_KMD_MAX_CLIENTS; ++i) bo_lookup_[i] = BoLookupSlot{};
    for (size_t i = 0u; i < A840_KMD_ZERO_PAGE_MAX_CLIENTS; ++i) vm_maps_[i] = VmMapRecord{};
    has_last_ib_diag_ = false;
    memset(&last_ib_diag_, 0, sizeof(last_ib_diag_));
    v2_passthrough_calls_ = 0u;
    v3_dispatch_calls_ = 0u;
    ib_validated_ok_ = 0u;
    ib_rejected_total_ = 0u;
    ib_rejected_blocked_reg_ = 0u;
    irq_signals_ = 0u;
    faults_reported_ = 0u;

    memset(&wait_backend_, 0, sizeof(wait_backend_));
    wait_backend_.cookie = &wait_state_;
    wait_backend_.lock = &NativeAdrenoV3Glue::waitLockThunk;
    wait_backend_.unlock = &NativeAdrenoV3Glue::waitUnlockThunk;
    wait_backend_.wait = &NativeAdrenoV3Glue::waitSleepThunk;
    wait_backend_.wake_all = &NativeAdrenoV3Glue::waitWakeAllThunk;
    wait_backend_.now_ns = &NativeAdrenoV3Glue::waitNowNsThunk;
    wait_backend_.read_ticks = &NativeAdrenoV3Glue::waitReadTicksThunk;

    A840KmdUserClientConfig uc_cfg{};
    uc_cfg.cap_flags = config.cap_flags;
    uc_cfg.zero_page_default_va = config.zero_page_default_va;
    uc_cfg.smmu_ops.cookie = this;
    uc_cfg.smmu_ops.alloc_backing_page = &NativeAdrenoV3Glue::smmuAllocBackingThunk;
    uc_cfg.smmu_ops.free_backing_page = &NativeAdrenoV3Glue::smmuFreeBackingThunk;
    uc_cfg.smmu_ops.map_client_ro = &NativeAdrenoV3Glue::smmuMapClientRoThunk;
    uc_cfg.smmu_ops.unmap_client = &NativeAdrenoV3Glue::smmuUnmapClientThunk;
    uc_cfg.wait_backend = &wait_backend_;
    uc_cfg.bo_size_lookup = &NativeAdrenoV3Glue::boSizeLookupThunk;
    uc_cfg.bo_size_cookie = this;
    uc_cfg.promise_observer = config.promise_observer;
    uc_cfg.promise_observer_cookie = config.promise_observer_cookie;

    const A840KmdStatus uc_st = a840_kmd_userclient_v3_init(&uc_, &uc_cfg);
    if (uc_st != A840_KMD_OK) {
#ifdef KERNEL
        IOLockFree(wait_state_.lock);
        wait_state_.lock = nullptr;
#else
        if (wait_state_.sync_ready) {
            (void)pthread_cond_destroy(&wait_state_.cond);
            (void)pthread_mutex_destroy(&wait_state_.lock);
            wait_state_.sync_ready = false;
        }
#endif
        diag_lock_.destroy();
        vm_map_lock_.destroy();
        bo_lookup_lock_.destroy();
        clients_sync_.destroy();
        return kmdStatusToIOReturn(uc_st);
    }

    initialized_ = true;
    return kIOReturnSuccess;
}

void NativeAdrenoV3Glue::destroy()
{
    if (!initialized_)
        return;

    for (;;) {
        V2ClientHandle target = nullptr;
        clients_sync_.acquire();
        for (size_t i = 0u; i < A840_KMD_MAX_CLIENTS; ++i) {
            if (clients_[i].state == CLIENT_OPEN) {
                target = clients_[i].v2_client;
                break;
            }
        }
        clients_sync_.release();
        if (target == nullptr)
            break;
        (void)closeClient(target, nullptr);
    }

    initialized_ = false;
    a840_kmd_userclient_v3_destroy(&uc_);

#ifdef KERNEL
    if (wait_state_.lock != nullptr) {
        IOLockFree(wait_state_.lock);
        wait_state_.lock = nullptr;
    }
#else
    if (wait_state_.sync_ready) {
        (void)pthread_cond_destroy(&wait_state_.cond);
        (void)pthread_mutex_destroy(&wait_state_.lock);
        wait_state_.sync_ready = false;
    }
#endif

    diag_lock_.destroy();
    vm_map_lock_.destroy();
    bo_lookup_lock_.destroy();
    clients_sync_.destroy();
}

bool NativeAdrenoV3Glue::initialized() const
{
    return initialized_;
}

uint32_t NativeAdrenoV3Glue::advertisedCaps() const
{
    if (!initialized_)
        return 0u;
    return a840_kmd_userclient_v3_advertised_caps(&uc_);
}

IOReturn NativeAdrenoV3Glue::configureSharedPack(
    const struct NativeAdrenoAbiSharedPackReplyV3 &pack,
    uint32_t gpu_readonly,
    uint32_t cpu_readonly)
{
    if (!initialized_)
        return kIOReturnNotReady;
    return kmdStatusToIOReturn(
        a840_kmd_userclient_v3_configure_shared_pack(&uc_, &pack, gpu_readonly, cpu_readonly));
}

IOReturn NativeAdrenoV3Glue::setPromiseObserver(
    A840AbiV3PromiseObserver observer, void *cookie)
{
    if (!initialized_)
        return kIOReturnNotReady;
    return kmdStatusToIOReturn(
        a840_kmd_userclient_v3_set_promise_observer(&uc_, observer, cookie));
}

NativeAdrenoV3Glue::ClientSlot *NativeAdrenoV3Glue::findSlotLocked(V2ClientHandle client)
{
    if (client == nullptr)
        return nullptr;
    for (size_t i = 0u; i < A840_KMD_MAX_CLIENTS; ++i) {
        if (clients_[i].state != CLIENT_FREE && clients_[i].v2_client == client)
            return &clients_[i];
    }
    return nullptr;
}

const NativeAdrenoV3Glue::ClientSlot *NativeAdrenoV3Glue::findSlotLocked(V2ClientHandle client) const
{
    if (client == nullptr)
        return nullptr;
    for (size_t i = 0u; i < A840_KMD_MAX_CLIENTS; ++i) {
        if (clients_[i].state != CLIENT_FREE && clients_[i].v2_client == client)
            return &clients_[i];
    }
    return nullptr;
}

IOReturn NativeAdrenoV3Glue::openClient(
    V2ClientHandle client,
    VmHandle vm,
    SubmitHandle submit,
    uint64_t requested_zero_page_va,
    uint64_t *out_client_id)
{
    if (out_client_id != nullptr)
        *out_client_id = 0u;
    if (!initialized_)
        return kIOReturnNotReady;
    if (client == nullptr)
        return kIOReturnBadArgument;

    size_t slot_index = A840_KMD_MAX_CLIENTS;
    uint64_t assigned_id = 0u;

    clients_sync_.acquire();
    if (findSlotLocked(client) != nullptr) {
        clients_sync_.release();
        return kIOReturnBusy;
    }
    for (size_t i = 0u; i < A840_KMD_MAX_CLIENTS; ++i) {
        if (clients_[i].state == CLIENT_FREE) {
            slot_index = i;
            break;
        }
    }
    if (slot_index == A840_KMD_MAX_CLIENTS) {
        clients_sync_.release();
        return kIOReturnNoResources;
    }
    assigned_id = next_client_id_++;
    if (assigned_id == 0u)
        assigned_id = next_client_id_++;

    clients_[slot_index].state = CLIENT_OPENING;
    clients_[slot_index].in_flight = 0u;
    clients_[slot_index].client_id = assigned_id;
    clients_[slot_index].v2_client = client;
    clients_[slot_index].vm = vm;
    clients_[slot_index].submit = submit;
    clients_sync_.release();

    registerBoLookup(assigned_id, client);

    const A840KmdStatus uc_st = a840_kmd_userclient_v3_open_client(
        &uc_, assigned_id, static_cast<void *>(vm), requested_zero_page_va);
    if (uc_st != A840_KMD_OK) {
        unregisterBoLookup(assigned_id);
        clients_sync_.acquire();
        clients_[slot_index] = ClientSlot{};
        clients_sync_.release();
        return kmdStatusToIOReturn(uc_st);
    }

    clients_sync_.acquire();
    clients_[slot_index].state = CLIENT_OPEN;
    clients_sync_.release();

    if (out_client_id != nullptr)
        *out_client_id = assigned_id;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoV3Glue::closeClient(
    V2ClientHandle client,
    A840KmdClientCloseSummary *out_summary)
{
    if (!initialized_)
        return kIOReturnNotReady;
    if (client == nullptr)
        return kIOReturnBadArgument;

    size_t slot_index = A840_KMD_MAX_CLIENTS;
    uint64_t client_id = 0u;

    clients_sync_.acquire();
    for (size_t i = 0u; i < A840_KMD_MAX_CLIENTS; ++i) {
        if (clients_[i].state != CLIENT_FREE && clients_[i].v2_client == client) {
            if (clients_[i].state != CLIENT_OPEN) {
                clients_sync_.release();
                return kIOReturnBusy;
            }
            clients_[i].state = CLIENT_CLOSING;
            slot_index = i;
            client_id = clients_[i].client_id;
            break;
        }
    }
    clients_sync_.release();

    if (slot_index == A840_KMD_MAX_CLIENTS)
        return kIOReturnNotFound;

    A840KmdClientCloseSummary summary{};
    const A840KmdStatus uc_st = a840_kmd_userclient_v3_close_client(
        &uc_, client_id, &summary);

    clients_sync_.acquire();
    while (clients_[slot_index].in_flight > 0u)
        clients_sync_.waitDrain();
    clients_[slot_index] = ClientSlot{};
    clients_sync_.release();

    unregisterBoLookup(client_id);

    if (out_summary != nullptr)
        *out_summary = summary;
    return kmdStatusToIOReturn(uc_st);
}

IOReturn NativeAdrenoV3Glue::auditIbBufferLockedFree(
    uint64_t client_id,
    uint64_t local_queue,
    const uint32_t *words,
    size_t dwords,
    A840KmdIbDiagResult *out_diag)
{
    A840KmdIbDiagResult diag{};
    const A840KmdIbDiagStatus st = a840_kmd_validate_ib_words(words, dwords, &diag);

    diag_lock_.acquire();
    last_ib_diag_ = diag;
    has_last_ib_diag_ = true;
    if (st == A840_KMD_IB_OK) {
        ib_validated_ok_++;
    } else {
        ib_rejected_total_++;
        if (st == A840_KMD_IB_BLOCKED_REG)
            ib_rejected_blocked_reg_++;
    }
    diag_lock_.release();

    if (out_diag != nullptr)
        *out_diag = diag;

    if (st != A840_KMD_IB_OK && ib_reject_cb_ != nullptr) {
        char line[A840_KMD_IB_DIAG_LINE_MAX];
        (void)a840_kmd_format_ib_diag(&diag, line, sizeof(line));
        ib_reject_cb_(ib_reject_cookie_, client_id, local_queue, &diag, line);
    }

    return ibDiagStatusToIOReturn(st);
}

IOReturn NativeAdrenoV3Glue::preSubmitGuardForMethod(
    uint64_t client_id,
    V2ClientHandle client,
    VmHandle vm,
    uint32_t selector,
    const void *in_buf,
    size_t in_bytes)
{
    if (selector == kV2MethodSubmit) {
        if (in_buf == nullptr || in_bytes != kV2SubmitBytes)
            return kIOReturnBadArgument;

        SubmitPrefixWire hdr{};
        memcpy(&hdr, in_buf, sizeof(hdr));
        if (hdr.buffer_count == 0u || hdr.buffer_count > kV2MaxBuffers ||
            hdr.reserved != 0u)
            return kIOReturnBadArgument;

        const A840KmdStatus ch_st = a840_kmd_userclient_v3_check_submit_channel(
            &uc_, client_id, hdr.queue);
        if (ch_st != A840_KMD_OK)
            return kmdStatusToIOReturn(ch_st);

        if (audit_ib_on_submit_) {
            const auto *base = static_cast<const uint8_t *>(in_buf);
            for (uint32_t i = 0u; i < hdr.buffer_count; ++i) {
                V2SubmitBufferRecord entry{};
                memcpy(&entry, base + sizeof(hdr) + static_cast<size_t>(i) * sizeof(entry),
                    sizeof(entry));
                if (entry.bytes == 0u || (entry.bytes % sizeof(uint32_t)) != 0u)
                    return kIOReturnBadArgument;
#ifdef KERNEL
                NativeAdrenoMemory *mem = client->memoryOf(entry.handle);
                if (vm != nullptr && mem != nullptr && mem->mapping() != nullptr) {
                    const uint64_t map_handle = vm->handleAt(entry.iova);
                    uint64_t map_iova = 0u;
                    size_t map_bytes = 0u;
                    unsigned map_perms = 0u;
                    if (map_handle != 0u &&
                        vm->describe(map_handle, &map_iova, &map_bytes, &map_perms) == kIOReturnSuccess &&
                        entry.iova >= map_iova) {
                        const uint64_t sub_off = entry.iova - map_iova;
                        if (sub_off <= mem->bytes() &&
                            entry.bytes <= mem->bytes() - sub_off &&
                            (sub_off % sizeof(uint32_t)) == 0u) {
                            const auto *base_ptr = static_cast<const uint8_t *>(mem->mapping()) + sub_off;
                            const auto *words = reinterpret_cast<const uint32_t *>(base_ptr);
                            const size_t dwords = static_cast<size_t>(entry.bytes / sizeof(uint32_t));
                            const IOReturn audit_rc = auditIbBufferLockedFree(
                                client_id, hdr.queue, words, dwords, nullptr);
                            if (audit_rc != kIOReturnSuccess)
                                return audit_rc;
                        }
                    }
                }
#else
                if (host_ops_.resolve_ib_words != nullptr) {
                    const uint32_t *words = nullptr;
                    size_t dwords = 0u;
                    const IOReturn res_rc = host_ops_.resolve_ib_words(
                        host_ops_.cookie, client, vm, entry.handle, entry.iova,
                        entry.bytes, &words, &dwords);
                    if (res_rc != kIOReturnSuccess)
                        return res_rc;
                    const IOReturn audit_rc = auditIbBufferLockedFree(
                        client_id, hdr.queue, words, dwords, nullptr);
                    if (audit_rc != kIOReturnSuccess)
                        return audit_rc;
                }
#endif
            }
        }
        return kIOReturnSuccess;
    }

    if (selector == kV2MethodSubmitBatch) {
        if (in_buf == nullptr || in_bytes != kV2SubmitBatchBytes)
            return kIOReturnBadArgument;

        SubmitBatchPrefixWire hdr{};
        memcpy(&hdr, in_buf, sizeof(hdr));
        if (hdr.action != kV2BatchAbort) {
            const A840KmdStatus ch_st = a840_kmd_userclient_v3_check_submit_channel(
                &uc_, client_id, hdr.queue);
            if (ch_st != A840_KMD_OK)
                return kmdStatusToIOReturn(ch_st);
        }

        if (audit_ib_on_submit_ && hdr.action == kV2BatchAppend) {
            if (hdr.buffer_count == 0u || hdr.buffer_count > kV2MaxBuffers)
                return kIOReturnBadArgument;
            const auto *base = static_cast<const uint8_t *>(in_buf);
            for (uint32_t i = 0u; i < hdr.buffer_count; ++i) {
                V2SubmitBufferRecord entry{};
                memcpy(&entry, base + sizeof(hdr) + static_cast<size_t>(i) * sizeof(entry),
                    sizeof(entry));
                if (entry.bytes == 0u || (entry.bytes % sizeof(uint32_t)) != 0u)
                    return kIOReturnBadArgument;
#ifdef KERNEL
                NativeAdrenoMemory *mem = client->memoryOf(entry.handle);
                if (vm != nullptr && mem != nullptr && mem->mapping() != nullptr) {
                    const uint64_t map_handle = vm->handleAt(entry.iova);
                    uint64_t map_iova = 0u;
                    size_t map_bytes = 0u;
                    unsigned map_perms = 0u;
                    if (map_handle != 0u &&
                        vm->describe(map_handle, &map_iova, &map_bytes, &map_perms) == kIOReturnSuccess &&
                        entry.iova >= map_iova) {
                        const uint64_t sub_off = entry.iova - map_iova;
                        if (sub_off <= mem->bytes() &&
                            entry.bytes <= mem->bytes() - sub_off &&
                            (sub_off % sizeof(uint32_t)) == 0u) {
                            const auto *base_ptr = static_cast<const uint8_t *>(mem->mapping()) + sub_off;
                            const auto *words = reinterpret_cast<const uint32_t *>(base_ptr);
                            const size_t dwords = static_cast<size_t>(entry.bytes / sizeof(uint32_t));
                            const IOReturn audit_rc = auditIbBufferLockedFree(
                                client_id, hdr.queue, words, dwords, nullptr);
                            if (audit_rc != kIOReturnSuccess)
                                return audit_rc;
                        }
                    }
                }
#else
                if (host_ops_.resolve_ib_words != nullptr) {
                    const uint32_t *words = nullptr;
                    size_t dwords = 0u;
                    const IOReturn res_rc = host_ops_.resolve_ib_words(
                        host_ops_.cookie, client, vm, entry.handle, entry.iova,
                        entry.bytes, &words, &dwords);
                    if (res_rc != kIOReturnSuccess)
                        return res_rc;
                    const IOReturn audit_rc = auditIbBufferLockedFree(
                        client_id, hdr.queue, words, dwords, nullptr);
                    if (audit_rc != kIOReturnSuccess)
                        return audit_rc;
                }
#endif
            }
        }
        return kIOReturnSuccess;
    }

    (void)client;
    (void)vm;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoV3Glue::forwardV2Method(
    V2ClientHandle client,
    uint32_t selector,
    const void *in_buf,
    size_t in_bytes,
    void *out_buf,
    size_t out_bytes,
    size_t *out_written)
{
#ifdef KERNEL
    const IOReturn rc = client->dispatch(
        selector, in_buf, in_bytes, out_buf, out_bytes, ring_clock_);
    if (rc == kIOReturnSuccess && out_written != nullptr)
        *out_written = NativeAdrenoUserClient::outputSize(selector);
    return rc;
#else
    if (host_ops_.v2_dispatch == nullptr)
        return kIOReturnUnsupported;
    return host_ops_.v2_dispatch(
        host_ops_.cookie, client, selector, in_buf, in_bytes,
        out_buf, out_bytes, out_written);
#endif
}

void NativeAdrenoV3Glue::augmentQueryParams(void *out_buf, size_t out_bytes)
{
    if (out_buf == nullptr || out_bytes != kV2ParamsBytes)
        return;
    V2ParamsRecord params{};
    memcpy(&params, out_buf, sizeof(params));
    params.abi_version = NativeAdrenoAbiVersionV3;
    /* Zero Legacy Turnip Policy: strip any non-production compatibility bits
     * before advertising frozen NativeAdrenoABI v3 capabilities.
     */
    constexpr uint32_t kProdV2FlagsMask =
        static_cast<uint32_t>(NativeAdrenoParamsV2HostImport |
                              NativeAdrenoParamsV2OwnedCoherent |
                              NativeAdrenoParamsV2MemoryPressure |
                              NativeAdrenoParamsV2SubmitBatch);
    params.flags = (params.flags & kProdV2FlagsMask) |
                   a840_kmd_userclient_v3_advertised_caps(&uc_);
    memcpy(out_buf, &params, sizeof(params));
}

IOReturn NativeAdrenoV3Glue::externalMethod(
    V2ClientHandle client,
    uint32_t selector,
    const void *in_buf,
    size_t in_bytes,
    void *out_buf,
    size_t out_bytes,
    size_t *out_written)
{
    if (out_written != nullptr)
        *out_written = 0u;
    if (!initialized_)
        return kIOReturnNotReady;
    if (client == nullptr)
        return kIOReturnBadArgument;
    if (in_bytes > NativeAdrenoAbiEnvelopeBytes ||
        out_bytes > NativeAdrenoAbiEnvelopeBytes)
        return kIOReturnBadArgument;

    size_t slot_index = A840_KMD_MAX_CLIENTS;
    uint64_t client_id = 0u;
    VmHandle vm = nullptr;

    clients_sync_.acquire();
    for (size_t i = 0u; i < A840_KMD_MAX_CLIENTS; ++i) {
        if (clients_[i].state != CLIENT_FREE && clients_[i].v2_client == client) {
            if (clients_[i].state != CLIENT_OPEN) {
                clients_sync_.release();
                return kIOReturnNotReady;
            }
            clients_[i].in_flight++;
            slot_index = i;
            client_id = clients_[i].client_id;
            vm = clients_[i].vm;
            break;
        }
    }
    clients_sync_.release();

    if (slot_index == A840_KMD_MAX_CLIENTS)
        return kIOReturnNotFound;

    size_t written = 0u;
    IOReturn result = kIOReturnError;

    const A840KmdStatus v3_st = a840_kmd_userclient_v3_dispatch(
        &uc_, client_id, selector, in_buf, in_bytes, out_buf, out_bytes, &written);

    if (v3_st == A840_KMD_PASS_TO_V2) {
        const IOReturn guard_rc = preSubmitGuardForMethod(
            client_id, client, vm, selector, in_buf, in_bytes);
        if (guard_rc != kIOReturnSuccess) {
            result = guard_rc;
        } else {
            result = forwardV2Method(
                client, selector, in_buf, in_bytes, out_buf, out_bytes, &written);
            if (result == kIOReturnSuccess && selector == kV2MethodQuery)
                augmentQueryParams(out_buf, out_bytes);
            diag_lock_.acquire();
            v2_passthrough_calls_++;
            diag_lock_.release();
        }
    } else {
        result = kmdStatusToIOReturn(v3_st);
        diag_lock_.acquire();
        v3_dispatch_calls_++;
        diag_lock_.release();
    }

    if (out_written != nullptr && result == kIOReturnSuccess)
        *out_written = written;

    clients_sync_.acquire();
    if (clients_[slot_index].in_flight > 0u)
        clients_[slot_index].in_flight--;
    if (clients_[slot_index].in_flight == 0u &&
        clients_[slot_index].state == CLIENT_CLOSING)
        clients_sync_.wakeDrainAll();
    clients_sync_.release();

    return result;
}

IOReturn NativeAdrenoV3Glue::validateSubmitIb(
    V2ClientHandle client,
    uint64_t local_queue,
    const uint32_t *words,
    size_t dword_count,
    A840KmdIbDiagResult *out_diag)
{
    if (!initialized_)
        return kIOReturnNotReady;
    if (client == nullptr)
        return kIOReturnBadArgument;

    uint64_t client_id = 0u;
    const IOReturn id_rc = getClientId(client, &client_id);
    if (id_rc != kIOReturnSuccess)
        return id_rc;

    const A840KmdStatus ch_st = a840_kmd_userclient_v3_check_submit_channel(
        &uc_, client_id, local_queue);
    if (ch_st != A840_KMD_OK)
        return kmdStatusToIOReturn(ch_st);

    return auditIbBufferLockedFree(client_id, local_queue, words, dword_count, out_diag);
}

IOReturn NativeAdrenoV3Glue::submitWithAudit(
    V2ClientHandle client,
    uint64_t local_queue,
    const uint32_t *ib_words,
    size_t ib_dwords,
    const void *submit_in,
    size_t submit_in_bytes,
    void *submit_out,
    size_t submit_out_bytes,
    A840KmdIbDiagResult *out_diag,
    size_t *out_written)
{
    if (out_written != nullptr)
        *out_written = 0u;
    if (!initialized_)
        return kIOReturnNotReady;
    if (client == nullptr)
        return kIOReturnBadArgument;

    size_t slot_index = A840_KMD_MAX_CLIENTS;
    uint64_t client_id = 0u;

    clients_sync_.acquire();
    for (size_t i = 0u; i < A840_KMD_MAX_CLIENTS; ++i) {
        if (clients_[i].state != CLIENT_FREE && clients_[i].v2_client == client) {
            if (clients_[i].state != CLIENT_OPEN) {
                clients_sync_.release();
                return kIOReturnNotReady;
            }
            clients_[i].in_flight++;
            slot_index = i;
            client_id = clients_[i].client_id;
            break;
        }
    }
    clients_sync_.release();

    if (slot_index == A840_KMD_MAX_CLIENTS)
        return kIOReturnNotFound;

    IOReturn result = kIOReturnSuccess;
    const A840KmdStatus ch_st = a840_kmd_userclient_v3_check_submit_channel(
        &uc_, client_id, local_queue);
    if (ch_st != A840_KMD_OK) {
        result = kmdStatusToIOReturn(ch_st);
    } else if (audit_ib_on_submit_) {
        result = auditIbBufferLockedFree(
            client_id, local_queue, ib_words, ib_dwords, out_diag);
    }

    if (result == kIOReturnSuccess) {
        size_t written = 0u;
        result = forwardV2Method(
            client, kV2MethodSubmit, submit_in, submit_in_bytes,
            submit_out, submit_out_bytes, &written);
        if (result == kIOReturnSuccess) {
            if (out_written != nullptr)
                *out_written = written;
            diag_lock_.acquire();
            v2_passthrough_calls_++;
            diag_lock_.release();
        }
    }

    clients_sync_.acquire();
    if (clients_[slot_index].in_flight > 0u)
        clients_[slot_index].in_flight--;
    if (clients_[slot_index].in_flight == 0u &&
        clients_[slot_index].state == CLIENT_CLOSING)
        clients_sync_.wakeDrainAll();
    clients_sync_.release();

    return result;
}

IOReturn NativeAdrenoV3Glue::signalFenceIrq(
    V2ClientHandle client,
    uint64_t local_queue,
    uint64_t fence,
    uint64_t ticks)
{
    if (!initialized_)
        return kIOReturnNotReady;
    if (client == nullptr)
        return kIOReturnBadArgument;
    uint64_t client_id = 0u;
    const IOReturn rc = getClientId(client, &client_id);
    if (rc != kIOReturnSuccess)
        return rc;
    return signalFenceByClientId(client_id, local_queue, fence, ticks);
}

IOReturn NativeAdrenoV3Glue::signalFenceByClientId(
    uint64_t client_id,
    uint64_t local_queue,
    uint64_t fence,
    uint64_t ticks)
{
    if (!initialized_)
        return kIOReturnNotReady;
    const A840KmdStatus st = a840_kmd_userclient_v3_signal_fence(
        &uc_, client_id, local_queue, fence, ticks);
    if (st == A840_KMD_OK) {
        diag_lock_.acquire();
        irq_signals_++;
        diag_lock_.release();
    }
    return kmdStatusToIOReturn(st);
}

IOReturn NativeAdrenoV3Glue::reportHardwareFault(
    V2ClientHandle client,
    const A840KmdFaultInput &fault,
    A840KmdFaultRecord *out_record,
    char *out_ascii,
    size_t out_ascii_cap,
    size_t *out_ascii_len)
{
    if (out_ascii != nullptr && out_ascii_cap > 0u)
        out_ascii[0] = '\0';
    if (out_ascii_len != nullptr)
        *out_ascii_len = 0u;
    if (!initialized_)
        return kIOReturnNotReady;

    A840KmdFaultInput effective = fault;
    if (effective.client_id == 0u) {
        if (client == nullptr)
            return kIOReturnBadArgument;
        const IOReturn id_rc = getClientId(client, &effective.client_id);
        if (id_rc != kIOReturnSuccess)
            return id_rc;
    }

    A840KmdFaultRecord rec{};
    const A840KmdStatus st = a840_kmd_userclient_v3_report_fault(&uc_, &effective, &rec);
    if (st != A840_KMD_OK)
        return kmdStatusToIOReturn(st);

    char full_line[A840_KMD_FAULT_SNAPSHOT_LINE_MAX];
    const size_t full_len = a840_kmd_format_fault_snapshot(
        &rec, full_line, sizeof(full_line));

    diag_lock_.acquire();
    faults_reported_++;
    diag_lock_.release();

    if (out_record != nullptr)
        *out_record = rec;

    if (out_ascii != nullptr && out_ascii_cap > 0u) {
        const size_t written = a840_kmd_format_fault_snapshot(
            &rec, out_ascii, out_ascii_cap);
        if (out_ascii_len != nullptr)
            *out_ascii_len = written;
    } else if (out_ascii_len != nullptr) {
        *out_ascii_len = full_len;
    }

    if (fault_log_cb_ != nullptr)
        fault_log_cb_(fault_log_cookie_, full_line, full_len, &rec);

    return kIOReturnSuccess;
}

IOReturn NativeAdrenoV3Glue::checkSubmitChannel(
    V2ClientHandle client, uint64_t local_queue)
{
    if (!initialized_)
        return kIOReturnNotReady;
    uint64_t client_id = 0u;
    const IOReturn id_rc = getClientId(client, &client_id);
    if (id_rc != kIOReturnSuccess)
        return id_rc;
    return kmdStatusToIOReturn(
        a840_kmd_userclient_v3_check_submit_channel(&uc_, client_id, local_queue));
}

IOReturn NativeAdrenoV3Glue::getClientId(
    V2ClientHandle client, uint64_t *out_client_id) const
{
    if (out_client_id != nullptr)
        *out_client_id = 0u;
    if (!initialized_)
        return kIOReturnNotReady;
    if (client == nullptr || out_client_id == nullptr)
        return kIOReturnBadArgument;

    clients_sync_.acquire();
    const ClientSlot *slot = findSlotLocked(client);
    if (slot == nullptr) {
        clients_sync_.release();
        return kIOReturnNotFound;
    }
    if (slot->state != CLIENT_OPEN) {
        clients_sync_.release();
        return kIOReturnNotReady;
    }
    *out_client_id = slot->client_id;
    clients_sync_.release();
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoV3Glue::getLastIbDiag(A840KmdIbDiagResult *out_diag) const
{
    if (!initialized_)
        return kIOReturnNotReady;
    if (out_diag == nullptr)
        return kIOReturnBadArgument;
    diag_lock_.acquire();
    if (!has_last_ib_diag_) {
        diag_lock_.release();
        return kIOReturnNotFound;
    }
    *out_diag = last_ib_diag_;
    diag_lock_.release();
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoV3Glue::getLatestFaultSnapshot(
    A840KmdFaultRecord *out_record,
    char *out_ascii,
    size_t out_ascii_cap,
    size_t *out_ascii_len)
{
    if (out_ascii != nullptr && out_ascii_cap > 0u)
        out_ascii[0] = '\0';
    if (out_ascii_len != nullptr)
        *out_ascii_len = 0u;
    if (!initialized_)
        return kIOReturnNotReady;

    A840KmdFaultRecord rec{};
    const A840KmdStatus st = a840_kmd_fault_get_latest(&uc_.fault_capture, &rec);
    if (st != A840_KMD_OK)
        return kmdStatusToIOReturn(st);

    if (out_record != nullptr)
        *out_record = rec;
    if (out_ascii != nullptr && out_ascii_cap > 0u) {
        const size_t n = a840_kmd_format_fault_snapshot(&rec, out_ascii, out_ascii_cap);
        if (out_ascii_len != nullptr)
            *out_ascii_len = n;
    }
    return kIOReturnSuccess;
}

NativeAdrenoV3Glue::Stats NativeAdrenoV3Glue::getStats() const
{
    Stats s{};
    if (!initialized_)
        return s;

    auto *mutable_self = const_cast<NativeAdrenoV3Glue *>(this);
    s.open_clients = a840_kmd_userclient_v3_open_client_count(&mutable_self->uc_);

    diag_lock_.acquire();
    s.v2_passthrough_calls = v2_passthrough_calls_;
    s.v3_dispatch_calls = v3_dispatch_calls_;
    s.ib_validated_ok = ib_validated_ok_;
    s.ib_rejected_total = ib_rejected_total_;
    s.ib_rejected_blocked_reg = ib_rejected_blocked_reg_;
    s.irq_signals = irq_signals_;
    s.faults_reported = faults_reported_;
    diag_lock_.release();

    waitLockThunk(const_cast<WaitBackendState *>(&wait_state_));
    s.wait_backend_locks = wait_state_.lock_calls;
    s.wait_backend_waits = wait_state_.wait_calls;
    s.wait_backend_wakes = wait_state_.wake_all_calls;
    s.wait_backend_timeouts = wait_state_.timeout_count;
    waitUnlockThunk(const_cast<WaitBackendState *>(&wait_state_));

    if (mutable_self->uc_.zero_page_enabled)
        (void)a840_kmd_zero_page_get_stats(&mutable_self->uc_.zero_page, &s.zero_page);
    (void)a840_kmd_wait_any_get_stats(&mutable_self->uc_.wait_any, &s.wait_any);
    return s;
}
