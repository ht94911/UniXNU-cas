/* SPDX-License-Identifier: MIT */
#ifndef A840_ABI_V3_HOST_H
#define A840_ABI_V3_HOST_H

#include <stddef.h>
#include <stdint.h>
#include "NativeAdrenoABI_v3.h"

#define A840_ABI_V3_PROMISE_SLOTS 64u
#define A840_ABI_V3_SURFACE_SLOTS 64u
#define A840_ABI_V3_PAGE_BYTES UINT64_C(4096)
#define A840_ABI_V3_ZERO_PAGE_BYTES UINT64_C(65536)
#define A840_ABI_V3_META_PITCH_ALIGNMENT 64u
#define A840_ABI_V3_GPU_VA_LIMIT (UINT64_C(1) << 48)

typedef enum {
    A840_ABI_V3_OK = 0,
    A840_ABI_V3_INVALID_ARG = 1,
    A840_ABI_V3_RANGE = 2,
    A840_ABI_V3_ALIGNMENT = 3,
    A840_ABI_V3_CAPACITY = 4,
    A840_ABI_V3_NOT_FOUND = 5,
    A840_ABI_V3_STATE_CONFLICT = 6,
    A840_ABI_V3_STACK_KEY_MISMATCH = 7,
    A840_ABI_V3_UNSUPPORTED = 8
} A840AbiV3Status;

enum {
    A840_ABI_V3_PROMISE_FREE = 0,
    A840_ABI_V3_PROMISE_PENDING = 1,
    A840_ABI_V3_PROMISE_FULFILLED = 2,
    A840_ABI_V3_PROMISE_CANCELED = 3
};

typedef A840AbiV3Status (*A840AbiV3FenceQuery)(
    void *cookie, uint64_t queue, uint64_t *completed_fence);

/* Caller-supplied observer consumes pending/terminal transitions; it must
 * not reenter this state. Phase 2 wires terminal notifications to MDSS.
 */
typedef void (*A840AbiV3PromiseObserver)(void *cookie, uint64_t client_id,
    const struct NativeAdrenoAbiWriterPromiseV3 *promise, uint32_t state);

typedef struct A840AbiV3PromiseSlot {
    uint64_t client_id;
    uint32_t state;
    struct NativeAdrenoAbiWriterPromiseV3 request;
} A840AbiV3PromiseSlot;

typedef struct A840AbiV3SurfaceSlot {
    uint64_t client_id;
    uint64_t bo_bytes;
    uint32_t valid;
    struct NativeAdrenoAbiSurfaceMetaV3 metadata;
} A840AbiV3SurfaceSlot;

/* Caller-owned state, no heap inside the core. Initialize a new instance
 * once, and serialize all access in the caller (e.g. UserClient lock).
 * Configuration VAs/RO permissions must be trusted per-client mappings.
 */
typedef struct A840AbiV3State {
    uint32_t initialized;
    uint32_t cap_flags;
    uint64_t last_promise_token;
    uint32_t zero_page_valid;
    uint32_t shared_pack_valid;
    struct NativeAdrenoAbiZeroPageReplyV3 zero_page;
    struct NativeAdrenoAbiSharedPackReplyV3 shared_pack;
    A840AbiV3PromiseObserver observer;
    void *observer_cookie;
    A840AbiV3PromiseSlot promises[A840_ABI_V3_PROMISE_SLOTS];
    A840AbiV3SurfaceSlot surfaces[A840_ABI_V3_SURFACE_SLOTS];
} A840AbiV3State;

#ifdef __cplusplus
extern "C" {
#endif

A840AbiV3Status a840_abi_v3_init(A840AbiV3State *st, uint32_t cap_flags);
A840AbiV3Status a840_abi_v3_check_method(const A840AbiV3State *st, uint32_t method);

/* Exact frozen input/output record sizes; Method 22 has no frozen request
 * layout and belongs to Phase 4. It and Method 26 remain UNSUPPORTED,
 * and their feature flags are not advertised by this core.
 */
A840AbiV3Status a840_abi_v3_validate_envelope(
    uint32_t method, size_t input_bytes, size_t output_bytes);

A840AbiV3Status a840_abi_v3_set_zero_page(A840AbiV3State *st,
    const struct NativeAdrenoAbiZeroPageReplyV3 *page, uint32_t readonly);
A840AbiV3Status a840_abi_v3_get_zero_page(const A840AbiV3State *st,
    struct NativeAdrenoAbiZeroPageReplyV3 *reply);

A840AbiV3Status a840_abi_v3_set_shared_pack(A840AbiV3State *st,
    const struct NativeAdrenoAbiSharedPackReplyV3 *pack,
    uint32_t gpu_readonly, uint32_t cpu_readonly);
A840AbiV3Status a840_abi_v3_map_shared_pack(const A840AbiV3State *st,
    const struct NativeAdrenoAbiSharedPackReqV3 *request,
    struct NativeAdrenoAbiSharedPackReplyV3 *reply);

/* One nonblocking evaluation; the adapter owns interrupt waiting/clocks.
 * Query returns a monotonic, unwrapped uint64 fence for the client's queue.
 * Queue 0 and fence 0 follow normal default-queue/timeline-zero semantics.
 * On callback/validation error the output is untouched; elapsed_ns is
 * measured by the caller. Satisfaction wins over timeout at a boundary.
 */
A840AbiV3Status a840_abi_v3_eval_wait_any(const A840AbiV3State *st,
    const struct NativeAdrenoAbiWaitAnyV3 *request,
    A840AbiV3FenceQuery query, void *cookie, uint64_t elapsed_ns,
    uint64_t observed_ticks, struct NativeAdrenoAbiWaitAnyReplyV3 *reply);

A840AbiV3Status a840_abi_v3_set_promise_observer(A840AbiV3State *st,
    A840AbiV3PromiseObserver observer, void *cookie);
A840AbiV3Status a840_abi_v3_writer_promise(A840AbiV3State *st, uint64_t client_id,
    const struct NativeAdrenoAbiWriterPromiseV3 *request,
    struct NativeAdrenoAbiWriterPromiseReplyV3 *reply);

/* 0 client_id queries all clients. Terminal slots can be recycled; tokens
 * never repeat, and exhaustion is rejected without state mutation.
 */
size_t a840_abi_v3_pending_promises(const A840AbiV3State *st, uint64_t client_id);

/* Notify cancellation exactly once for every pending client promise, then
 * erase all that client's slots and surface metadata. Retired tokens are
 * not reusable. Return the number of canceled pending promises.
 */
size_t a840_abi_v3_cancel_client_promises(A840AbiV3State *st, uint64_t client_id);
size_t a840_abi_v3_cancel_all_for_client(A840AbiV3State *st, uint64_t client_id);

/* Get uses request bo_handle/op only. Set requires Kaanapali 6/16/6/1 even
 * when UBWC is disabled, 4K offset and 64-byte pitch alignment. bo_bytes
 * comes from the caller's owned BO and must fit at least one metadata row;
 * full image geometry/BO ownership is checked by the Phase 2 adapter.
 */
A840AbiV3Status a840_abi_v3_surface_meta(A840AbiV3State *st, uint64_t client_id,
    const struct NativeAdrenoAbiSurfaceMetaV3 *request, uint64_t bo_bytes,
    struct NativeAdrenoAbiSurfaceMetaV3 *reply);

#ifdef __cplusplus
}
#endif
#endif /* A840_ABI_V3_HOST_H */
