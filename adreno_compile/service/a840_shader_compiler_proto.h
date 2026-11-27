/* SPDX-License-Identifier: MIT */
/* NezhaShaderCompiler wire protocol (v1): pure C11, transport-agnostic,
 * little-endian fixed header with SHA-256 payload integrity and stack/ABI
 * identity binding. Zero Apple private headers, zero device identifiers.
 */
#ifndef A840_SHADER_COMPILER_PROTO_H
#define A840_SHADER_COMPILER_PROTO_H

#include "../cache/a840_record_io.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_SC_PROTO_MAGIC = 0x31435338u, /* '8SC1' little-endian */
    A840_SC_PROTO_VERSION = 1u,
    A840_SC_HEADER_BYTES = 128u,
    A840_SC_MAX_PAYLOAD_BYTES = 16u * 1024u * 1024u
};

typedef enum A840ScMessageType {
    A840_SC_MSG_COMPILE_REQUEST = 1u,
    A840_SC_MSG_COMPILE_REPLY = 2u,
    A840_SC_MSG_CANCEL_REQUEST = 3u,
    A840_SC_MSG_PING_STATUS = 4u,
    A840_SC_MSG_PREWARM_HINT = 5u
} A840ScMessageType;

typedef enum A840ScPriority {
    A840_SC_PRIO_FG_SYNC = 0u,
    A840_SC_PRIO_BG_ASYNC = 1u,
    A840_SC_PRIO_WARMUP_LOW = 2u
} A840ScPriority;

typedef enum A840ScStatus {
    A840_SC_OK = 0,
    A840_SC_INVALID_ARG = 1,
    A840_SC_PROTO_ERROR = 2,
    A840_SC_TRUNCATED = 3,
    A840_SC_PAYLOAD_TOO_LARGE = 4,
    A840_SC_STACK_MISMATCH = 5,
    A840_SC_ABI_MISMATCH = 6,
    A840_SC_HASH_MISMATCH = 7,
    A840_SC_RESERVED_NONZERO = 8,
    A840_SC_UNKNOWN_PRIORITY = 9,
    A840_SC_UNKNOWN_MSG_TYPE = 10,
    A840_SC_COMPILE_FAILED = 11,
    A840_SC_TIMED_OUT = 12,
    A840_SC_CANCELED = 13,
    A840_SC_SHUTDOWN = 14,
    A840_SC_DAEMON_UNAVAILABLE = 15,
    A840_SC_NOT_FOUND = 16,
    A840_SC_DUPLICATE_REQUEST = 17,
    A840_SC_OOM = 18
} A840ScStatus;

/* Canonical wire layout (128 bytes, strictly little-endian, never memcpy'd):
 *   [  0..  4) magic           (uint32_t, A840_SC_PROTO_MAGIC)
 *   [  4..  8) version         (uint32_t, A840_SC_PROTO_VERSION)
 *   [  8.. 12) msg_type        (uint32_t, A840ScMessageType)
 *   [ 12.. 16) priority_class  (uint32_t, A840ScPriority)
 *   [ 16.. 24) stack_key       (uint64_t, nonzero)
 *   [ 24.. 32) abi_fingerprint (uint64_t, nonzero)
 *   [ 32.. 64) pipeline_key    (uint8_t[32]; zero for PING_STATUS)
 *   [ 64.. 72) request_id      (uint64_t, nonzero)
 *   [ 72.. 76) client_pid      (uint32_t, nonzero)
 *   [ 76.. 80) status          (uint32_t, A840ScStatus; 0 on requests)
 *   [ 80.. 84) payload_bytes   (uint32_t, <= 16 MiB)
 *   [ 84.. 88) reserved0       (uint32_t, MUST be 0)
 *   [ 88.. 96) reserved1       (uint64_t, MUST be 0)
 *   [ 96..128) payload_sha256  (uint8_t[32], SHA-256 of payload[0..payload_bytes))
 */
typedef struct A840ScFrameHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t msg_type;
    uint32_t priority_class;
    uint64_t stack_key;
    uint64_t abi_fingerprint;
    A840PipelineKey pipeline_key;
    uint64_t request_id;
    uint32_t client_pid;
    uint32_t status;
    uint32_t payload_bytes;
    uint32_t reserved0;
    uint64_t reserved1;
    uint8_t payload_sha256[32];
} A840ScFrameHeader;

#if defined(__cplusplus)
static_assert(sizeof(A840ScFrameHeader) == 128, "A840ScFrameHeader logical size");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(A840ScFrameHeader) == 128, "A840ScFrameHeader logical size");
#endif

const char *a840_sc_status_string(A840ScStatus status);
bool a840_sc_pipeline_key_is_zero(const A840PipelineKey *key);

/* Validate header semantics against expected stack_key and abi_fingerprint.
 * Does not inspect payload bytes or payload_sha256.
 */
A840ScStatus a840_sc_validate_header(const A840ScFrameHeader *hdr,
    uint64_t expected_stack_key, uint64_t expected_abi_fingerprint);

/* Encode a frame into caller-supplied buffer. Recomputes payload_sha256 and
 * enforces magic/version/reserved==0/semantic rules. Failure leaves *out_bytes
 * untouched and writes nothing to out_frame.
 */
A840ScStatus a840_sc_encode_frame(const A840ScFrameHeader *hdr,
    const void *payload, uint8_t *out_frame, size_t out_capacity,
    size_t *out_bytes);

/* Decode and strictly validate a complete frame. Rejects truncation, trailing
 * bytes, payload > 16 MiB, bad magic/version, unknown msg_type/priority,
 * reserved != 0, stack_key/abi_fingerprint mismatch, and SHA-256 mismatch.
 * On success, *out_payload borrows into frame + A840_SC_HEADER_BYTES (or NULL
 * if payload_bytes == 0). On failure, out_hdr and out_payload are untouched.
 */
A840ScStatus a840_sc_decode_frame(const uint8_t *frame, size_t frame_bytes,
    uint64_t expected_stack_key, uint64_t expected_abi_fingerprint,
    A840ScFrameHeader *out_hdr, const uint8_t **out_payload);

/* Helper to inspect total expected wire length from the first 128 bytes of a
 * stream without decoding payload. Rejects bad magic/version or oversized len.
 */
A840ScStatus a840_sc_peek_frame_length(const uint8_t *prefix, size_t prefix_bytes,
    size_t *out_total_bytes);

#ifdef __cplusplus
}
#endif
#endif /* A840_SHADER_COMPILER_PROTO_H */
