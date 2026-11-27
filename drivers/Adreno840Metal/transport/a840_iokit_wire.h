/* SPDX-License-Identifier: MIT */
#ifndef A840_IOKIT_WIRE_H
#define A840_IOKIT_WIRE_H

#include "../core/a840_backend_transport.h"

/* NativeAdrenoABI.h v2, SHA-256 24b6d71df96a4849137ad6869c0bceea7e87602066df37b12c57f0cfa17c87a7.
 * IOKit copies these fixed-size little-endian records directly. */
enum {
    A840_WIRE_MAX_BUFFERS = 128,
    A840_WIRE_METADATA_BYTES = 64,
    A840_WIRE_NOTE_WORDS = 30,
    A840_WIRE_QUERY = 1,
    A840_WIRE_BO_ALLOC = 2,
    A840_WIRE_BO_MAP = 3,
    A840_WIRE_BO_FREE = 4,
    A840_WIRE_SUBMIT = 5,
    A840_WIRE_WAIT_FENCE = 6,
    A840_WIRE_GET_TIMESTAMP = 7,
    A840_WIRE_GET_STATUS = 8,
    A840_WIRE_SET_METADATA = 9,
    A840_WIRE_GET_METADATA = 10,
    A840_WIRE_CREATE_QUEUE = 11,
    A840_WIRE_CLOSE_QUEUE = 12,
    A840_WIRE_BO_CPU_MAP = 13,
    A840_WIRE_BO_CPU_UNMAP = 14,
    A840_WIRE_GET_DEVICE_INFO = 15,
    A840_WIRE_BO_IMPORT = 16,
    A840_WIRE_SET_PRIORITY = 17,
    A840_WIRE_NOTE = 18,
    A840_WIRE_SUBMIT_BATCH = 19,
    A840_WIRE_BATCH_BEGIN = 1,
    A840_WIRE_BATCH_APPEND = 2,
    A840_WIRE_BATCH_COMMIT = 3,
    A840_WIRE_BATCH_ABORT = 4,
};

typedef struct A840WireParams {
    uint32_t abi_version;
    uint32_t input_bits;          /* GPU VA bits below 2^input_bits */
    uint32_t output_bits;         /* physical address bits */
    uint32_t flags;               /* NativeAdrenoParamsHostImport or zero */
    uint64_t max_bo_bytes;
} A840WireParams;

typedef struct A840WireBoAlloc {
    uint64_t bytes;               /* 4KiB multiple, <= max_bo_bytes */
    uint32_t align_log2;          /* 12..24 */
    uint32_t contiguous;          /* 1 requires one raw physical span */
} A840WireBoAlloc;

typedef struct A840WireBoAllocReply {
    uint64_t handle;              /* opaque to userspace, nonzero */
} A840WireBoAllocReply;

typedef struct A840WireBoImport {
    uint64_t address;             /* 4KiB aligned */
    uint64_t bytes;               /* 4KiB multiple, <= max_bo_bytes */
} A840WireBoImport;

typedef struct A840WireBoMap {
    uint64_t handle;
    uint64_t iova;                /* 4KiB aligned, inside the mounted aperture */
    uint32_t permissions;         /* bit0 read, bit1 write; read required */
    uint32_t writeback;           /* 0 normal non-coherent, 1 WB/WA */
    uint32_t shareability;        /* 2 outer, 3 inner */
    uint32_t reserved;            /* zero */
} A840WireBoMap;

typedef struct A840WireBoMapReply {
    uint64_t iova;
} A840WireBoMapReply;

typedef struct A840WireBoFree {
    uint64_t handle;
} A840WireBoFree;

typedef struct A840WireSubmitBuffer {
    uint64_t handle;
    uint64_t iova;
    uint64_t bytes;               /* 4-byte multiple, dword count = bytes/4 */
} A840WireSubmitBuffer;

typedef struct A840WireSubmit {
    uint32_t buffer_count;        /* 1..A840_WIRE_MAX_BUFFERS */
    uint32_t reserved;            /* zero */
    uint64_t queue;               /* open queue; 0 is created by open() */
    uint64_t wait_fence;          /* zero or an earlier fence of this client */
    uint64_t wait_timeout_ns;     /* used only when wait_fence is nonzero */
    A840WireSubmitBuffer buffers[A840_WIRE_MAX_BUFFERS];
} A840WireSubmit;

typedef struct A840WireMetadata {
    uint64_t handle;              /* object identity */
    uint32_t bytes;               /* set: length; get: requested capacity */
    uint32_t reserved;            /* zero */
    char text[A840_WIRE_METADATA_BYTES]; /* raw bytes, not NUL terminated */
} A840WireMetadata;

typedef struct A840WireSubmitReply {
    uint64_t fence;               /* nonzero CP timestamp value */
} A840WireSubmitReply;

typedef struct A840WireSubmitBatch {
    uint64_t batch;
    uint64_t queue;
    uint32_t total_buffers;
    uint32_t offset;
    uint32_t buffer_count;
    uint32_t action;
    A840WireSubmitBuffer buffers[A840_WIRE_MAX_BUFFERS];
} A840WireSubmitBatch;

typedef struct A840WireSubmitBatchReply {
    uint64_t batch;
    uint64_t fence;               /* zero for staging; nonzero only after COMMIT */
} A840WireSubmitBatchReply;

typedef struct A840WireWaitFence {
    uint64_t fence;
    uint64_t timeout_ns;          /* 0 polls once */
} A840WireWaitFence;

typedef struct A840WireTimestamp {
    uint64_t ticks;               /* GPU always-on counter value */
} A840WireTimestamp;

typedef struct A840WireBoCpuMap {
    uint64_t handle;
} A840WireBoCpuMap;

typedef struct A840WireBoCpuMapReply {
    uint32_t token;               /* clientMemoryForType selector */
    uint32_t reserved;            /* zero */
    uint64_t bytes;               /* bytes the mapping covers */
} A840WireBoCpuMapReply;

typedef struct A840WireBoCpuUnmap {
    uint64_t handle;
    uint32_t token;               /* exactly the token returned by map */
    uint32_t reserved;            /* zero */
} A840WireBoCpuUnmap;

typedef struct A840WirePriority {
    uint32_t foreground;          /* 0..5, 3 refused */
    uint32_t background;          /* 0..5 */
} A840WirePriority;

typedef struct A840WireNote {
    uint32_t kind;
    uint32_t count;               /* words used, <= A840_WIRE_NOTE_WORDS */
    uint32_t words[A840_WIRE_NOTE_WORDS];
} A840WireNote;

typedef struct A840WireQueue {
    uint64_t queue;               /* zero for create, an open id for close */
} A840WireQueue;

typedef struct A840WireQueueReply {
    uint64_t queue;               /* nonzero id of the created queue */
} A840WireQueueReply;

typedef struct A840WireStatus {
    uint64_t faults;              /* monotonic; change means context lost */
    uint64_t suspends;            /* monotonic suspend/reset count */
} A840WireStatus;

typedef struct A840WireDeviceInfo {
    uint32_t chip_id;             /* ADRENO_CHIPID, e.g. 0x43050a01 */
    uint32_t gmem_bytes;
    uint64_t gmem_base;           /* UCHE GMEM window start (GPU address) */
    uint64_t uche_trap_base;
    uint64_t va_start;            /* first GPU address the client may map */
    uint64_t va_size;
    uint32_t highest_bank_bit;
    uint32_t ubwc_swizzle;        /* bit0 level 1, bit1 level 2, bit2 level 3 */
    uint32_t macrotile_mode;      /* 0 four channel, 1 eight channel */
    uint32_t ubwc_version;        /* encoder version, 4 for UBWC 4.0 */
    uint32_t flags;
    uint32_t reserved;            /* zero */
} A840WireDeviceInfo;

#ifdef __cplusplus
#define A840_WIRE_SIZE(type, n) static_assert(sizeof(type) == (n), #type " wire size")
#else
#define A840_WIRE_SIZE(type, n) _Static_assert(sizeof(type) == (n), #type " wire size")
#endif
A840_WIRE_SIZE(A840WireParams, 24);
A840_WIRE_SIZE(A840WireBoAlloc, 16);
A840_WIRE_SIZE(A840WireBoAllocReply, 8);
A840_WIRE_SIZE(A840WireBoImport, 16);
A840_WIRE_SIZE(A840WireBoMap, 32);
A840_WIRE_SIZE(A840WireBoMapReply, 8);
A840_WIRE_SIZE(A840WireBoFree, 8);
A840_WIRE_SIZE(A840WireSubmitBuffer, 24);
A840_WIRE_SIZE(A840WireSubmit, 3104);
A840_WIRE_SIZE(A840WireMetadata, 80);
A840_WIRE_SIZE(A840WireSubmitReply, 8);
A840_WIRE_SIZE(A840WireSubmitBatch, 3104);
A840_WIRE_SIZE(A840WireSubmitBatchReply, 16);
A840_WIRE_SIZE(A840WireWaitFence, 16);
A840_WIRE_SIZE(A840WireTimestamp, 8);
A840_WIRE_SIZE(A840WireBoCpuMap, 8);
A840_WIRE_SIZE(A840WireBoCpuMapReply, 16);
A840_WIRE_SIZE(A840WireBoCpuUnmap, 16);
A840_WIRE_SIZE(A840WirePriority, 8);
A840_WIRE_SIZE(A840WireNote, 128);
A840_WIRE_SIZE(A840WireQueue, 8);
A840_WIRE_SIZE(A840WireQueueReply, 8);
A840_WIRE_SIZE(A840WireStatus, 16);
A840_WIRE_SIZE(A840WireDeviceInfo, 64);

A840_WIRE_SIZE(struct NativeAdrenoAbiWaitAnyV3, 528);
A840_WIRE_SIZE(struct NativeAdrenoAbiWaitAnyReplyV3, 16);
A840_WIRE_SIZE(struct NativeAdrenoAbiZeroPageReplyV3, 16);
A840_WIRE_SIZE(struct NativeAdrenoAbiSharedPackReqV3, 16);
A840_WIRE_SIZE(struct NativeAdrenoAbiSharedPackReplyV3, 32);
A840_WIRE_SIZE(struct NativeAdrenoAbiWriterPromiseV3, 160);
A840_WIRE_SIZE(struct NativeAdrenoAbiWriterPromiseReplyV3, 16);
A840_WIRE_SIZE(struct NativeAdrenoAbiSurfaceMetaV3, 48);
#undef A840_WIRE_SIZE

#ifdef __cplusplus
extern "C" {
#endif

typedef struct A840IokitWire A840IokitWire;
typedef struct A840RtDevice A840RtDevice;

typedef A840TransportStatus (*A840WireCallFn)(void *cookie, uint32_t method,
    const void *input, size_t input_bytes, void *output, size_t output_bytes);

/* A CPU view is separate from the Method 13 lease. The mapper reports
 * success only for a usable mapping of at least the requested size. */
typedef A840TransportStatus (*A840WireMapMemoryFn)(void *cookie, uint32_t token,
    uint64_t bytes, bool coherent, void **out_cpu);
typedef A840TransportStatus (*A840WireUnmapMemoryFn)(void *cookie, uint32_t token);

A840TransportStatus a840_wire_create(A840WireCallFn call, A840WireMapMemoryFn map_cpu,
    A840WireUnmapMemoryFn unmap_cpu, void *cookie, A840IokitWire **out_wire);
void a840_wire_destroy(A840IokitWire *wire);
A840BackendTransport a840_wire_interface(A840IokitWire *wire);

/* Exact envelope for the frozen methods. Method 22 is reserved for Phase 4
 * and has no frozen record. This helper is also used by the native adapter. */
A840TransportStatus a840_wire_method_sizes(uint32_t method, size_t *input, size_t *output);
A840TransportStatus a840_wire_call(A840IokitWire *wire, uint32_t method,
    const void *input, size_t input_bytes, void *output, size_t output_bytes);

/* Attach after device creation, before submitting anything. A separate
 * GPU IB contains Context Init; real BR/BV ambles share its BO. This IB is
 * consumed by the first accepted submit, including a batch submit.
 * Drain queues and detach before destroying the device. */
A840TransportStatus a840_wire_attach_context(A840IokitWire *wire, A840RtDevice *device);
void a840_wire_detach_context(A840IokitWire *wire);

#ifdef __cplusplus
}
#endif

#endif /* A840_IOKIT_WIRE_H */
