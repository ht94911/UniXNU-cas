/* SPDX-License-Identifier: MIT */
#ifndef A840_BACKEND_TRANSPORT_H
#define A840_BACKEND_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "Adreno840EmitABI.h"
#include "NativeAdrenoABI_v3.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_TRANSPORT_MAX_SUBMIT_IBS = 1024u,
    A840_TRANSPORT_PAGE_BYTES = 4096u,
    A840_TRANSPORT_ZERO_PAGE_BYTES = 65536u
};

typedef enum A840TransportStatus {
    A840_TRANSPORT_OK = 0,
    A840_TRANSPORT_INVALID_ARG = 1,
    A840_TRANSPORT_OOM = 2,
    A840_TRANSPORT_NOT_FOUND = 3,
    A840_TRANSPORT_DANGLING_BO = 4,
    A840_TRANSPORT_OUT_OF_BOUNDS = 5,
    A840_TRANSPORT_BUSY = 6,
    A840_TRANSPORT_TIMEOUT = 7,
    A840_TRANSPORT_UNSUPPORTED = 8,
    A840_TRANSPORT_STATE_ERROR = 9
} A840TransportStatus;

enum {
    A840_BO_PERM_READ = 1u << 0,
    A840_BO_PERM_WRITE = 1u << 1,

    A840_BO_MAP_GPU = 1u << 0,
    A840_BO_MAP_CPU = 1u << 1,

    A840_BO_UNMAP_GPU = 1u << 0,
    A840_BO_UNMAP_CPU = 1u << 1,

    A840_BO_SHARE_OUTER = 2u,
    A840_BO_SHARE_INNER = 3u,

    A840_TRANSPORT_DEV_CLIENT_IOVA = 1u << 0,
    A840_TRANSPORT_DEV_CACHED_COHERENT = 1u << 1,

    /* Matches Metal MTLGPUPriority numbering used by NativeAdrenoABI v2. */
    A840_QUEUE_PRIORITY_HIGH = 0u,
    A840_QUEUE_PRIORITY_NORMAL = 1u,
    A840_QUEUE_PRIORITY_LOW = 2u,
    A840_QUEUE_PRIORITY_DISABLED = 3u,
    A840_QUEUE_PRIORITY_NORMAL_HIGH = 4u,
    A840_QUEUE_PRIORITY_REALTIME = 5u
};

/* Combined NativeAdrenoABI v2/v3 Query + GetDeviceInfo capability snapshot. */
typedef struct A840TransportCaps {
    uint32_t abi_version;
    uint32_t input_bits;
    uint32_t output_bits;
    uint32_t abi_flags;
    uint64_t max_bo_bytes;

    uint32_t chip_id;
    uint32_t gmem_bytes;
    uint64_t gmem_base;
    uint64_t uche_trap_base;
    uint64_t va_start;
    uint64_t va_size;
    uint32_t highest_bank_bit;
    uint32_t ubwc_swizzle;
    uint32_t macrotile_mode;
    uint32_t ubwc_version;
    uint32_t device_flags;
} A840TransportCaps;

typedef struct A840BoAllocDesc {
    uint64_t bytes;      /* 4 KiB multiple, <= max_bo_bytes */
    uint32_t align_log2; /* 12..24 */
    uint32_t contiguous; /* 0 or 1 */
} A840BoAllocDesc;

typedef struct A840BoImportDesc {
    void *host_ptr;      /* 4 KiB aligned caller memory */
    uint64_t bytes;      /* 4 KiB multiple, <= max_bo_bytes */
} A840BoImportDesc;

typedef struct A840BoMapDesc {
    uint64_t handle;
    uint64_t gpu_va;     /* Required when flags includes A840_BO_MAP_GPU */
    uint32_t flags;      /* Bitmask of A840_BO_MAP_* */
    uint32_t permissions;/* Bitmask of A840_BO_PERM_* (read required for GPU map) */
    uint32_t writeback;  /* 0 non-coherent, 1 WB/WA */
    uint32_t shareability; /* 0 default, 2 outer, 3 inner */
} A840BoMapDesc;

typedef struct A840BoMapReply {
    uint64_t gpu_va;
    void *cpu_ptr;
    uint32_t cpu_token;
    uint32_t reserved;
    uint64_t bytes;
} A840BoMapReply;

typedef struct A840BoUnmapDesc {
    uint64_t handle;
    uint32_t flags;      /* Bitmask of A840_BO_UNMAP_* */
    uint32_t cpu_token;  /* Required when flags includes A840_BO_UNMAP_CPU */
} A840BoUnmapDesc;

typedef struct A840SubmitIb {
    uint64_t bo_handle;
    uint64_t gpu_va;
    uint64_t bytes;      /* Positive multiple of 4 */
} A840SubmitIb;

typedef struct A840SubmitDesc {
    uint64_t queue;
    uint64_t wait_fence;
    uint64_t wait_timeout_ns;
    const A840SubmitIb *ibs;
    uint32_t ib_count;
    const A840Reloc *relocs;
    uint32_t reloc_count;
} A840SubmitDesc;

typedef struct A840SubmitReply {
    uint64_t fence;
    uint64_t submitted_ticks;
} A840SubmitReply;

/* Abstract kernel transport vtable mapping NativeAdrenoABI v2/v3. */
typedef struct A840BackendTransportOps {
    A840TransportStatus (*query_caps)(void *ctx, A840TransportCaps *out_caps);
    A840TransportStatus (*bo_alloc)(void *ctx, const A840BoAllocDesc *desc, uint64_t *out_handle);
    A840TransportStatus (*bo_import)(void *ctx, const A840BoImportDesc *desc, uint64_t *out_handle);
    A840TransportStatus (*bo_map)(void *ctx, const A840BoMapDesc *desc, A840BoMapReply *out_reply);
    A840TransportStatus (*bo_unmap)(void *ctx, const A840BoUnmapDesc *desc);
    A840TransportStatus (*bo_free)(void *ctx, uint64_t bo_handle);
    A840TransportStatus (*queue_create)(void *ctx, uint32_t priority, uint64_t *out_queue);
    A840TransportStatus (*queue_destroy)(void *ctx, uint64_t queue);
    A840TransportStatus (*queue_submit)(void *ctx, const A840SubmitDesc *desc, A840SubmitReply *out_reply);
    A840TransportStatus (*fence_wait)(void *ctx, uint64_t queue, uint64_t fence,
                                      uint64_t timeout_ns, uint64_t *out_ticks);
    A840TransportStatus (*wait_any_v3)(void *ctx, const struct NativeAdrenoAbiWaitAnyV3 *req,
                                       struct NativeAdrenoAbiWaitAnyReplyV3 *out_reply);
    A840TransportStatus (*get_zero_page_v3)(void *ctx, struct NativeAdrenoAbiZeroPageReplyV3 *out_reply);
    A840TransportStatus (*surface_meta_v3)(void *ctx, struct NativeAdrenoAbiSurfaceMetaV3 *io_meta);
} A840BackendTransportOps;

typedef struct A840BackendTransport {
    const A840BackendTransportOps *ops;
    void *ctx;
} A840BackendTransport;

/* Host mock GPU backend (implemented in a840_mock_transport.c; NOT linked into
 * production liba840_runtime_core.a). */
typedef struct A840MockTransport A840MockTransport;

typedef struct A840MockTransportConfig {
    uint32_t abi_version;
    uint32_t abi_flags;
    uint64_t va_start;
    uint64_t va_size;
    uint64_t max_bo_bytes;
    bool auto_complete_submits;
} A840MockTransportConfig;

typedef struct A840MockSubmissionView {
    uint64_t queue;
    uint64_t fence;
    uint32_t ib_count;
    uint32_t reloc_count;
    const uint32_t *words;
    size_t dword_count;
} A840MockSubmissionView;

void a840_mock_transport_default_config(A840MockTransportConfig *out_cfg);
A840TransportStatus a840_mock_transport_create(const A840MockTransportConfig *cfg,
                                               A840MockTransport **out_mock);
void a840_mock_transport_destroy(A840MockTransport *mock);
A840BackendTransport a840_mock_transport_interface(A840MockTransport *mock);

void a840_mock_transport_set_auto_complete(A840MockTransport *mock, bool enabled);
A840TransportStatus a840_mock_transport_complete_queue(A840MockTransport *mock,
                                                       uint64_t queue,
                                                       uint64_t up_to_fence);
void a840_mock_transport_complete_all(A840MockTransport *mock);

size_t a840_mock_transport_submission_count(const A840MockTransport *mock);
A840TransportStatus a840_mock_transport_get_submission(const A840MockTransport *mock,
                                                       size_t index,
                                                       A840MockSubmissionView *out_view);
size_t a840_mock_transport_total_captured_dwords(const A840MockTransport *mock);
const uint32_t *a840_mock_transport_captured_words(const A840MockTransport *mock,
                                                   size_t *out_count);
A840TransportStatus a840_mock_transport_dump_pm4(const A840MockTransport *mock,
                                                 const char *path);

size_t a840_mock_transport_live_bo_count(const A840MockTransport *mock);
size_t a840_mock_transport_live_queue_count(const A840MockTransport *mock);
bool a840_mock_transport_is_bo_live(const A840MockTransport *mock, uint64_t bo_handle);
uint64_t a840_mock_transport_zero_page_handle(const A840MockTransport *mock);

#ifdef __cplusplus
}
#endif

#endif /* A840_BACKEND_TRANSPORT_H */
