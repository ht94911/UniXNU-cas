/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_IB_DIAG_H
#define A840_KMD_IB_DIAG_H

#include <stddef.h>
#include <stdint.h>
#include "Adreno840EmitABI.h"
#include "a840_kmd_fault_capture.h"

/* Alias matching Phase 2 Wave 4 diagnostic API naming while keeping exact
 * layout compatibility with Wave 3 A840KmdFaultSnapshot.
 */
typedef A840KmdFaultSnapshot A840KmdFaultRecord;

/* Worst-case single-line ASCII snapshot length (including NUL terminator) for
 * all-UINT64_MAX / UINT32_MAX fields.
 */
#define A840_KMD_FAULT_SNAPSHOT_LINE_MAX 320u
#define A840_KMD_IB_DIAG_LINE_MAX 224u

typedef enum A840KmdIbDiagStatus {
    A840_KMD_IB_OK = 0,
    A840_KMD_IB_INVALID_ARG = 1,
    A840_KMD_IB_EMPTY = 2,
    A840_KMD_IB_UNKNOWN_PACKET = 3,
    A840_KMD_IB_BAD_PARITY = 4,
    A840_KMD_IB_BAD_HEADER = 5,
    A840_KMD_IB_TRUNCATED = 6,
    A840_KMD_IB_BLOCKED_REG = 7
} A840KmdIbDiagStatus;

/* Detailed pre-submission structural and Gen8 hardware-guardrail audit result.
 * - On A840_KMD_IB_OK: offending_* fields are zeroed and all packet counters
 *   reflect the complete validated IB stream.
 * - On header/structural errors (UNKNOWN_PACKET, BAD_PARITY, BAD_HEADER,
 *   TRUNCATED): offending_dword_index == offending_header_index == header word
 *   index inside `words`, and offending_word == words[offending_dword_index].
 * - On A840_KMD_IB_BLOCKED_REG: offending_header_index is the Type4 header
 *   dword index, offending_dword_index is the exact payload dword index
 *   (`header + 1 + reg_offset`) targeting `offending_reg`, and offending_word
 *   is the value word at `words[offending_dword_index]`.
 */
typedef struct A840KmdIbDiagResult {
    A840KmdIbDiagStatus status;
    uint32_t offending_reg;
    uint32_t offending_word;
    uint32_t offending_opcode;
    size_t offending_dword_index;
    size_t offending_header_index;
    size_t offending_packet_index;
    size_t total_packets;
    size_t type4_packets;
    size_t type7_packets;
    size_t total_reg_writes;
    size_t validated_dwords;
} A840KmdIbDiagResult;

#ifdef __cplusplus
extern "C" {
#endif

/* Pure C11, allocation-free PM4 IB1/IB2 pre-submission structural validator
 * and Gen8 UMD-blocked register guardrail.
 *
 * Linearly walks every Type4/Type7 PM4 packet in `words[0..dword_count-1]`:
 *  - Rejects unknown packet types (anything other than Type4 or Type7),
 *    reserved header bits, zero-length Type4 packets, 18-bit register range
 *    wraparound, odd-parity mismatches, and truncated payloads.
 *  - Scans every Type4 target register (`base_reg + offset`) against
 *    `a840_reg_is_umd_blocked_on_a8xx()` and immediately rejects all 6 Gen8
 *    KMD-only non-context registers with A840_KMD_IB_BLOCKED_REG, reporting
 *    the exact `offending_dword_index` and `offending_reg`.
 */
A840KmdIbDiagStatus a840_kmd_validate_ib_words(
    const uint32_t *words,
    size_t dword_count,
    A840KmdIbDiagResult *out);

/* Byte-buffer convenience wrapper enforcing 4-byte alignment and size. */
A840KmdIbDiagStatus a840_kmd_validate_ib_bytes(
    const void *bytes,
    size_t byte_count,
    A840KmdIbDiagResult *out);

/* Deterministic, allocation-free, locale-independent single-line ASCII
 * formatter for GPU hang / SMMU fault snapshots, ready for kernel.rings.txt
 * or USB serial telemetry capture.
 *
 * Format:
 *   a840_fault seq=<dec> kind=<name> client=<dec> queue=<dec> fence=<dec>
 *   ib1_pc=0x<hex16> ib2_pc=0x<hex16> rbbm=0x<hex8> cp=0x<hex8>
 *   smmu_va=0x<hex16> smmu_fsr=0x<hex8> ticks=<dec>
 *
 * Truncation-safe and always NUL-terminated when `buf != NULL && cap > 0`.
 * Returns the exact number of ASCII bytes written excluding the NUL byte
 * (always `< cap` when `cap > 0`, or `0` on NULL / zero capacity).
 */
size_t a840_kmd_format_fault_snapshot(
    const A840KmdFaultRecord *rec,
    char *buf,
    size_t cap);

/* Deterministic, allocation-free single-line ASCII formatter for IB audit
 * failures / pass summaries, truncation-safe and always NUL-terminated.
 */
size_t a840_kmd_format_ib_diag(
    const A840KmdIbDiagResult *diag,
    char *buf,
    size_t cap);

const char *a840_kmd_ib_diag_status_name(A840KmdIbDiagStatus status);
const char *a840_kmd_fault_kind_name(uint32_t fault_kind);

#ifdef __cplusplus
}
#endif

#endif /* A840_KMD_IB_DIAG_H */
