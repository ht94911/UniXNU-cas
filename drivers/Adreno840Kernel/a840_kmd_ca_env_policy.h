/* SPDX-License-Identifier: MIT */
#ifndef A840_KMD_CA_ENV_POLICY_H
#define A840_KMD_CA_ENV_POLICY_H

#include <stddef.h>
#include <stdint.h>

#include "a840_kmd_zero_page.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Strict whitelist target and CoreAnimation environment token. */
#define A840_KMD_CA_TARGET_BACKBOARDD "com.apple.backboardd"
#define A840_KMD_CA_ENV_KEY "CA_DISABLE_GENERIC_SHADERS"
#define A840_KMD_CA_ENV_VALUE "1"
#define A840_KMD_CA_ENV_ENTRY "CA_DISABLE_GENERIC_SHADERS=1"

/* DarwinBoot fixed 1024-byte NUL-terminated boot-args limit (1023 printable ASCII). */
#define A840_KMD_CA_MAX_BOOT_ARGS_LEN 1023u
#define A840_KMD_CA_MAX_ENV_ENTRIES 256u

typedef enum A840KmdCaEnvReason {
    A840_KMD_CA_REASON_NONE = 0u,
    A840_KMD_CA_ALLOW_INJECT = 1u,
    A840_KMD_CA_ALLOW_ALREADY_PRESENT = 2u,
    A840_KMD_CA_REJECT_INVALID_ARG = 3u,
    A840_KMD_CA_REJECT_ILLEGAL_CHAR = 4u,
    A840_KMD_CA_REJECT_TARGET_NOT_WHITELISTED = 5u,
    A840_KMD_CA_REJECT_NATIVE_DISABLED = 6u,
    A840_KMD_CA_REJECT_PACK_UNVERIFIED = 7u,
    A840_KMD_CA_REJECT_CONFLICTING_VALUE = 8u,
    A840_KMD_CA_REJECT_MALFORMED_ENV = 9u,
    A840_KMD_CA_REJECT_BUFFER_TOO_SMALL = 10u,
    A840_KMD_CA_REJECT_BOOT_ARGS_TOO_LONG = 11u
} A840KmdCaEnvReason;

typedef struct A840KmdCaEnvPolicyInput {
    const char *target_label;        /* MUST be exact "com.apple.backboardd" */
    uint32_t native_metal_enabled;   /* MUST be 1u (boolean 0 or 1; other values fail-closed) */
    uint32_t phase4_pack_verified;   /* MUST be 1u (boolean 0 or 1; other values fail-closed) */
    uint32_t reserved;               /* MUST be 0u */
} A840KmdCaEnvPolicyInput;

typedef struct A840KmdCaEnvDecision {
    uint32_t should_inject;          /* 1 = policy permits injecting CA_DISABLE_GENERIC_SHADERS=1 */
    uint32_t already_present;        /* 1 = existing env/boot-args already has exact =1 */
    uint32_t reason;                 /* A840KmdCaEnvReason */
    uint32_t reserved;
    const char *env_key;             /* Non-NULL only when allowed */
    const char *env_value;           /* Non-NULL only when allowed */
    const char *env_entry;           /* Non-NULL only when allowed */
} A840KmdCaEnvDecision;

/* Pure policy evaluation for target process & Phase 4 readiness preconditions.
 * Fail-closed: returns A840_KMD_OK only when target is exactly
 * "com.apple.backboardd", native_metal_enabled == 1, and phase4_pack_verified == 1.
 */
A840KmdStatus a840_kmd_ca_env_evaluate(
    const A840KmdCaEnvPolicyInput *input,
    A840KmdCaEnvDecision *out_decision);

/* Construct a double-NUL-terminated contiguous environment block
 * ("K1=V1\0K2=V2\0CA_DISABLE_GENERIC_SHADERS=1\0\0") for LaunchDaemon/kernel reuse.
 *
 * Fail-closed invariants:
 * - Rejects unmet preconditions, conflicting CA_DISABLE_GENERIC_SHADERS value,
 *   malformed env entries, illegal control/non-ASCII chars, or insufficient `out_cap`.
 * - Zero silent truncation: on ANY error, `out_buf` is left byte-for-byte untouched.
 */
A840KmdStatus a840_kmd_ca_env_build_env_block(
    const A840KmdCaEnvPolicyInput *input,
    const char *const *existing_env,
    size_t existing_count,
    char *out_buf,
    size_t out_cap,
    size_t *out_written_bytes,
    size_t *out_entry_count,
    A840KmdCaEnvDecision *out_decision);

/* Construct a NULL-terminated pointer array `out_envp` referencing `existing_env`
 * entries plus the static string "CA_DISABLE_GENERIC_SHADERS=1" (if not already
 * present). On ANY error, `out_envp` is left completely untouched.
 */
A840KmdStatus a840_kmd_ca_env_build_envp(
    const A840KmdCaEnvPolicyInput *input,
    const char *const *existing_env,
    size_t existing_count,
    const char **out_envp,
    size_t out_envp_cap,
    size_t *out_envp_count,
    A840KmdCaEnvDecision *out_decision);

/* Append "CA_DISABLE_GENERIC_SHADERS=1" to space-separated ASCII `existing_boot_args`
 * while preserving every existing byte verbatim and enforcing DarwinBoot's 1023-char
 * ASCII limit. Safe for in-place (`existing_boot_args == out_buf`) or separate buffer.
 *
 * Fail-closed invariants:
 * - Rejects unmet preconditions, conflicting token (case-insensitive key match with
 *   value != "1" or bare key), control/non-ASCII characters, capacity overflow, or
 *   exceeding A840_KMD_CA_MAX_BOOT_ARGS_LEN (1023).
 * - On ANY error, `out_buf` is NOT modified (byte-for-byte preserved).
 */
A840KmdStatus a840_kmd_ca_env_append_boot_args(
    const A840KmdCaEnvPolicyInput *input,
    const char *existing_boot_args,
    char *out_buf,
    size_t out_cap,
    size_t *out_written_len,
    A840KmdCaEnvDecision *out_decision);

/* Validate an already-constructed `boot-args` string against illegal characters,
 * length bounds, and conflicting CA_DISABLE_GENERIC_SHADERS tokens.
 * Sets `*out_has_enable = 1` iff an exact `CA_DISABLE_GENERIC_SHADERS=1` token exists.
 */
A840KmdStatus a840_kmd_ca_env_validate_boot_args(
    const char *boot_args,
    uint32_t *out_has_enable,
    A840KmdCaEnvReason *out_reason);

#ifdef __cplusplus
}
#endif

#endif /* A840_KMD_CA_ENV_POLICY_H */
