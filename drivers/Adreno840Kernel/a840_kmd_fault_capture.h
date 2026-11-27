/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_FAULT_CAPTURE_H
#define A840_KMD_FAULT_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#if !defined(A840_KMD_CUSTOM_SYNC)
#include <pthread.h>
#endif

#ifndef A840_KMD_STATUS_DEFINED
#define A840_KMD_STATUS_DEFINED
typedef enum A840KmdStatus {
    A840_KMD_OK = 0,
    A840_KMD_PASS_TO_V2 = 1,
    A840_KMD_INVALID_ARG = 2,
    A840_KMD_RANGE = 3,
    A840_KMD_ALIGNMENT = 4,
    A840_KMD_PERMISSION_DENIED = 5,
    A840_KMD_CAPACITY = 6,
    A840_KMD_NOT_FOUND = 7,
    A840_KMD_STATE_CONFLICT = 8,
    A840_KMD_STACK_KEY_MISMATCH = 9,
    A840_KMD_UNSUPPORTED = 10,
    A840_KMD_POISONED = 11,
    A840_KMD_DISCONNECTED = 12,
    A840_KMD_IO_ERROR = 13,
    A840_KMD_NO_MEMORY = 14
} A840KmdStatus;
#endif

#define A840_KMD_FAULT_RING_CAPACITY 64u
#define A840_KMD_FAULT_MAX_POISONED_CHANNELS 256u

typedef enum A840KmdFaultKind {
    A840_KMD_FAULT_NONE = 0,
    A840_KMD_FAULT_GPU_HANG = 1,
    A840_KMD_FAULT_SMMU_TRANSLATION = 2,
    A840_KMD_FAULT_SMMU_PERMISSION = 3,
    A840_KMD_FAULT_IB_ERROR = 4
} A840KmdFaultKind;

typedef struct A840KmdFaultInput {
    uint64_t client_id;
    uint64_t queue_id;
    uint64_t fence;
    uint64_t ib1_pc;
    uint64_t ib2_pc;
    uint32_t rbbm_status;
    uint32_t cp_status;
    uint64_t smmu_fault_va;
    uint32_t smmu_fsr;
    uint32_t fault_kind;
    uint64_t timestamp_ticks;
} A840KmdFaultInput;

typedef struct A840KmdFaultSnapshot {
    uint64_t sequence;
    uint64_t client_id;
    uint64_t queue_id;
    uint64_t fence;
    uint64_t ib1_pc;
    uint64_t ib2_pc;
    uint32_t rbbm_status;
    uint32_t cp_status;
    uint64_t smmu_fault_va;
    uint32_t smmu_fsr;
    uint32_t fault_kind;
    uint64_t timestamp_ticks;
} A840KmdFaultSnapshot;

/* Invoked OUTSIDE the fault-capture mutex after a channel is poisoned so
 * WaitAny waiters blocked on that channel are woken without lock inversion.
 */
typedef void (*A840KmdFaultPoisonCallback)(
    void *cookie,
    uint64_t client_id,
    uint64_t queue_id,
    const A840KmdFaultSnapshot *snapshot);

typedef struct A840KmdPoisonedChannel {
    uint32_t active;
    uint32_t fault_count;
    uint64_t client_id;
    uint64_t queue_id;
    uint64_t last_sequence;
} A840KmdPoisonedChannel;

typedef struct A840KmdFaultCapture {
    uint32_t initialized;
    uint32_t write_index;
    uint32_t ring_count;
    uint32_t poisoned_count;
    uint64_t total_faults;
    A840KmdFaultPoisonCallback poison_cb;
    void *poison_cookie;
    A840KmdFaultSnapshot ring[A840_KMD_FAULT_RING_CAPACITY];
    A840KmdPoisonedChannel poisoned[A840_KMD_FAULT_MAX_POISONED_CHANNELS];
#if !defined(A840_KMD_CUSTOM_SYNC)
    pthread_mutex_t lock;
#endif
} A840KmdFaultCapture;

#ifdef __cplusplus
extern "C" {
#endif

A840KmdStatus a840_kmd_fault_init(A840KmdFaultCapture *fc);
void a840_kmd_fault_destroy(A840KmdFaultCapture *fc);

A840KmdStatus a840_kmd_fault_set_poison_callback(
    A840KmdFaultCapture *fc,
    A840KmdFaultPoisonCallback cb,
    void *cookie);

/* Capture a hardware hang / SMMU fault snapshot into the circular ring,
 * isolate ONLY (client_id, queue_id) as POISONED, and notify the poison
 * callback (e.g. WaitAny engine) outside the lock.
 */
A840KmdStatus a840_kmd_fault_record(
    A840KmdFaultCapture *fc,
    const A840KmdFaultInput *input,
    A840KmdFaultSnapshot *out_snapshot);

/* Returns 1 if (client_id, queue_id) is isolated as POISONED, else 0. */
int a840_kmd_fault_is_channel_poisoned(
    A840KmdFaultCapture *fc,
    uint64_t client_id,
    uint64_t queue_id);

/* Submit/Wait guard: returns A840_KMD_POISONED if channel is poisoned,
 * A840_KMD_OK if healthy.
 */
A840KmdStatus a840_kmd_fault_check_channel(
    A840KmdFaultCapture *fc,
    uint64_t client_id,
    uint64_t queue_id);

/* Clear active poison entries for a closing client while preserving historical
 * fault snapshots in the ring buffer for post-mortem inspection.
 */
size_t a840_kmd_fault_clear_client(
    A840KmdFaultCapture *fc,
    uint64_t client_id);

size_t a840_kmd_fault_ring_count(A840KmdFaultCapture *fc);
uint64_t a840_kmd_fault_total_count(A840KmdFaultCapture *fc);
size_t a840_kmd_fault_poisoned_channel_count(A840KmdFaultCapture *fc);

/* Read retained snapshot by 0-based index from oldest retained entry. */
A840KmdStatus a840_kmd_fault_get_snapshot(
    A840KmdFaultCapture *fc,
    size_t index_from_oldest,
    A840KmdFaultSnapshot *out_snapshot);

/* Read newest snapshot in the ring. */
A840KmdStatus a840_kmd_fault_get_latest(
    A840KmdFaultCapture *fc,
    A840KmdFaultSnapshot *out_snapshot);

/* Read newest retained snapshot matching (client_id, queue_id). */
A840KmdStatus a840_kmd_fault_get_channel_latest(
    A840KmdFaultCapture *fc,
    uint64_t client_id,
    uint64_t queue_id,
    A840KmdFaultSnapshot *out_snapshot);

#ifdef __cplusplus
}
#endif

#endif /* A840_KMD_FAULT_CAPTURE_H */
