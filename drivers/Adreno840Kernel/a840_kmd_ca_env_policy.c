/* SPDX-License-Identifier: MIT */
#include "a840_kmd_ca_env_policy.h"

#include <string.h>

static int is_printable_ascii_char(unsigned char ch)
{
    return ch >= 32u && ch <= 126u;
}

static int is_env_key_char(unsigned char ch)
{
    if (ch >= (unsigned char)'A' && ch <= (unsigned char)'Z')
        return 1;
    if (ch >= (unsigned char)'a' && ch <= (unsigned char)'z')
        return 1;
    if (ch >= (unsigned char)'0' && ch <= (unsigned char)'9')
        return 1;
    return ch == (unsigned char)'_';
}

static int is_label_char(unsigned char ch)
{
    if (ch >= (unsigned char)'A' && ch <= (unsigned char)'Z')
        return 1;
    if (ch >= (unsigned char)'a' && ch <= (unsigned char)'z')
        return 1;
    if (ch >= (unsigned char)'0' && ch <= (unsigned char)'9')
        return 1;
    return ch == (unsigned char)'.' || ch == (unsigned char)'-' || ch == (unsigned char)'_';
}

static unsigned char ascii_upper(unsigned char ch)
{
    if (ch >= (unsigned char)'a' && ch <= (unsigned char)'z')
        return (unsigned char)(ch - 32u);
    return ch;
}

static int ascii_eq_nocase_n(const char *a, size_t a_len, const char *b, size_t b_len)
{
    size_t i;
    if (a_len != b_len)
        return 0;
    for (i = 0u; i < a_len; ++i) {
        if (ascii_upper((unsigned char)a[i]) != ascii_upper((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

static void init_decision(A840KmdCaEnvDecision *d)
{
    if (d != NULL)
        memset(d, 0, sizeof(*d));
}

static void set_decision_reject(
    A840KmdCaEnvDecision *d,
    A840KmdCaEnvReason reason)
{
    if (d == NULL)
        return;
    memset(d, 0, sizeof(*d));
    d->reason = (uint32_t)reason;
}

A840KmdStatus a840_kmd_ca_env_evaluate(
    const A840KmdCaEnvPolicyInput *input,
    A840KmdCaEnvDecision *out_decision)
{
    size_t i;
    size_t label_len;

    init_decision(out_decision);

    if (input == NULL || out_decision == NULL || input->target_label == NULL) {
        set_decision_reject(out_decision, A840_KMD_CA_REJECT_INVALID_ARG);
        return A840_KMD_INVALID_ARG;
    }
    if (input->reserved != 0u ||
        (input->native_metal_enabled != 0u && input->native_metal_enabled != 1u) ||
        (input->phase4_pack_verified != 0u && input->phase4_pack_verified != 1u)) {
        set_decision_reject(out_decision, A840_KMD_CA_REJECT_INVALID_ARG);
        return A840_KMD_INVALID_ARG;
    }

    label_len = strlen(input->target_label);
    if (label_len == 0u || label_len > 256u) {
        set_decision_reject(out_decision, A840_KMD_CA_REJECT_TARGET_NOT_WHITELISTED);
        return A840_KMD_PERMISSION_DENIED;
    }
    for (i = 0u; i < label_len; ++i) {
        unsigned char ch = (unsigned char)input->target_label[i];
        if (!is_printable_ascii_char(ch) || !is_label_char(ch)) {
            set_decision_reject(out_decision, A840_KMD_CA_REJECT_ILLEGAL_CHAR);
            return A840_KMD_INVALID_ARG;
        }
    }

    if (strcmp(input->target_label, A840_KMD_CA_TARGET_BACKBOARDD) != 0) {
        set_decision_reject(out_decision, A840_KMD_CA_REJECT_TARGET_NOT_WHITELISTED);
        return A840_KMD_PERMISSION_DENIED;
    }

    if (input->native_metal_enabled != 1u) {
        set_decision_reject(out_decision, A840_KMD_CA_REJECT_NATIVE_DISABLED);
        return A840_KMD_STATE_CONFLICT;
    }

    if (input->phase4_pack_verified != 1u) {
        set_decision_reject(out_decision, A840_KMD_CA_REJECT_PACK_UNVERIFIED);
        return A840_KMD_STATE_CONFLICT;
    }

    out_decision->should_inject = 1u;
    out_decision->already_present = 0u;
    out_decision->reason = (uint32_t)A840_KMD_CA_ALLOW_INJECT;
    out_decision->env_key = A840_KMD_CA_ENV_KEY;
    out_decision->env_value = A840_KMD_CA_ENV_VALUE;
    out_decision->env_entry = A840_KMD_CA_ENV_ENTRY;
    return A840_KMD_OK;
}

static A840KmdStatus validate_existing_env_entries(
    const char *const *existing_env,
    size_t existing_count,
    uint32_t *out_already_present,
    size_t *out_existing_total_bytes,
    A840KmdCaEnvReason *out_reason)
{
    size_t key_len = strlen(A840_KMD_CA_ENV_KEY);
    size_t val_len = strlen(A840_KMD_CA_ENV_VALUE);
    size_t total_bytes = 0u;
    uint32_t found_enable = 0u;
    size_t i;

    *out_already_present = 0u;
    *out_existing_total_bytes = 0u;
    *out_reason = A840_KMD_CA_REASON_NONE;

    if (existing_count > A840_KMD_CA_MAX_ENV_ENTRIES) {
        *out_reason = A840_KMD_CA_REJECT_INVALID_ARG;
        return A840_KMD_RANGE;
    }
    if (existing_count > 0u && existing_env == NULL) {
        *out_reason = A840_KMD_CA_REJECT_INVALID_ARG;
        return A840_KMD_INVALID_ARG;
    }

    for (i = 0u; i < existing_count; ++i) {
        const char *entry = existing_env[i];
        const char *eq;
        size_t entry_len;
        size_t klen;
        size_t vlen;
        size_t j;

        if (entry == NULL) {
            *out_reason = A840_KMD_CA_REJECT_MALFORMED_ENV;
            return A840_KMD_INVALID_ARG;
        }

        entry_len = strlen(entry);
        if (entry_len == 0u) {
            *out_reason = A840_KMD_CA_REJECT_MALFORMED_ENV;
            return A840_KMD_INVALID_ARG;
        }

        for (j = 0u; j < entry_len; ++j) {
            if (!is_printable_ascii_char((unsigned char)entry[j])) {
                *out_reason = A840_KMD_CA_REJECT_ILLEGAL_CHAR;
                return A840_KMD_INVALID_ARG;
            }
        }

        eq = strchr(entry, '=');
        if (eq == NULL || eq == entry) {
            *out_reason = A840_KMD_CA_REJECT_MALFORMED_ENV;
            return A840_KMD_INVALID_ARG;
        }

        klen = (size_t)(eq - entry);
        for (j = 0u; j < klen; ++j) {
            if (!is_env_key_char((unsigned char)entry[j])) {
                *out_reason = A840_KMD_CA_REJECT_MALFORMED_ENV;
                return A840_KMD_INVALID_ARG;
            }
        }

        vlen = entry_len - klen - 1u;
        if (ascii_eq_nocase_n(entry, klen, A840_KMD_CA_ENV_KEY, key_len)) {
            /* Exact key match with value == "1" is idempotent; any case mismatch
             * in key or any value != "1" is a fail-closed conflict.
             */
            if (klen != key_len || memcmp(entry, A840_KMD_CA_ENV_KEY, key_len) != 0 ||
                vlen != val_len || memcmp(eq + 1, A840_KMD_CA_ENV_VALUE, val_len) != 0) {
                *out_reason = A840_KMD_CA_REJECT_CONFLICTING_VALUE;
                return A840_KMD_STATE_CONFLICT;
            }
            found_enable = 1u;
        }

        if ((entry_len + 1u) > (SIZE_MAX - total_bytes)) {
            *out_reason = A840_KMD_CA_REJECT_BUFFER_TOO_SMALL;
            return A840_KMD_RANGE;
        }
        total_bytes += entry_len + 1u;
    }

    *out_already_present = found_enable;
    *out_existing_total_bytes = total_bytes;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_ca_env_build_env_block(
    const A840KmdCaEnvPolicyInput *input,
    const char *const *existing_env,
    size_t existing_count,
    char *out_buf,
    size_t out_cap,
    size_t *out_written_bytes,
    size_t *out_entry_count,
    A840KmdCaEnvDecision *out_decision)
{
    A840KmdCaEnvDecision dec;
    A840KmdStatus st;
    A840KmdCaEnvReason reason = A840_KMD_CA_REASON_NONE;
    uint32_t already_present = 0u;
    size_t existing_bytes = 0u;
    size_t inject_bytes = 0u;
    size_t required_bytes = 0u;
    size_t offset = 0u;
    size_t i;

    init_decision(&dec);

    if (out_buf == NULL || out_cap == 0u) {
        set_decision_reject(&dec, A840_KMD_CA_REJECT_INVALID_ARG);
        if (out_decision != NULL)
            *out_decision = dec;
        return A840_KMD_INVALID_ARG;
    }

    st = a840_kmd_ca_env_evaluate(input, &dec);
    if (st != A840_KMD_OK) {
        if (out_decision != NULL)
            *out_decision = dec;
        return st;
    }

    st = validate_existing_env_entries(
        existing_env, existing_count, &already_present, &existing_bytes, &reason);
    if (st != A840_KMD_OK) {
        set_decision_reject(&dec, reason);
        if (out_decision != NULL)
            *out_decision = dec;
        return st;
    }

    if (!already_present)
        inject_bytes = strlen(A840_KMD_CA_ENV_ENTRY) + 1u;

    /* Required size: all NUL-terminated entries + trailing NUL terminator. */
    if (inject_bytes > (SIZE_MAX - existing_bytes) ||
        (existing_bytes + inject_bytes) >= SIZE_MAX) {
        set_decision_reject(&dec, A840_KMD_CA_REJECT_BUFFER_TOO_SMALL);
        if (out_decision != NULL)
            *out_decision = dec;
        return A840_KMD_RANGE;
    }
    required_bytes = existing_bytes + inject_bytes + 1u;
    if (required_bytes > out_cap) {
        set_decision_reject(&dec, A840_KMD_CA_REJECT_BUFFER_TOO_SMALL);
        if (out_decision != NULL)
            *out_decision = dec;
        return A840_KMD_RANGE;
    }

    /* All checks passed: serialize cleanly with zero truncation. */
    for (i = 0u; i < existing_count; ++i) {
        size_t len = strlen(existing_env[i]) + 1u;
        memcpy(out_buf + offset, existing_env[i], len);
        offset += len;
    }
    if (!already_present) {
        memcpy(out_buf + offset, A840_KMD_CA_ENV_ENTRY, inject_bytes);
        offset += inject_bytes;
    }
    out_buf[offset++] = '\0';

    if (already_present) {
        dec.already_present = 1u;
        dec.reason = (uint32_t)A840_KMD_CA_ALLOW_ALREADY_PRESENT;
    }
    if (out_written_bytes != NULL)
        *out_written_bytes = offset;
    if (out_entry_count != NULL)
        *out_entry_count = existing_count + (already_present ? 0u : 1u);
    if (out_decision != NULL)
        *out_decision = dec;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_ca_env_build_envp(
    const A840KmdCaEnvPolicyInput *input,
    const char *const *existing_env,
    size_t existing_count,
    const char **out_envp,
    size_t out_envp_cap,
    size_t *out_envp_count,
    A840KmdCaEnvDecision *out_decision)
{
    A840KmdCaEnvDecision dec;
    A840KmdStatus st;
    A840KmdCaEnvReason reason = A840_KMD_CA_REASON_NONE;
    uint32_t already_present = 0u;
    size_t existing_bytes = 0u;
    size_t total_entries;
    size_t i;

    init_decision(&dec);

    if (out_envp == NULL || out_envp_cap == 0u) {
        set_decision_reject(&dec, A840_KMD_CA_REJECT_INVALID_ARG);
        if (out_decision != NULL)
            *out_decision = dec;
        return A840_KMD_INVALID_ARG;
    }

    st = a840_kmd_ca_env_evaluate(input, &dec);
    if (st != A840_KMD_OK) {
        if (out_decision != NULL)
            *out_decision = dec;
        return st;
    }

    st = validate_existing_env_entries(
        existing_env, existing_count, &already_present, &existing_bytes, &reason);
    if (st != A840_KMD_OK) {
        set_decision_reject(&dec, reason);
        if (out_decision != NULL)
            *out_decision = dec;
        return st;
    }

    total_entries = existing_count + (already_present ? 0u : 1u);
    /* Must fit all pointers plus trailing NULL sentinel. */
    if ((total_entries + 1u) > out_envp_cap) {
        set_decision_reject(&dec, A840_KMD_CA_REJECT_BUFFER_TOO_SMALL);
        if (out_decision != NULL)
            *out_decision = dec;
        return A840_KMD_RANGE;
    }

    for (i = 0u; i < existing_count; ++i)
        out_envp[i] = existing_env[i];
    if (!already_present)
        out_envp[existing_count] = A840_KMD_CA_ENV_ENTRY;
    out_envp[total_entries] = NULL;

    if (already_present) {
        dec.already_present = 1u;
        dec.reason = (uint32_t)A840_KMD_CA_ALLOW_ALREADY_PRESENT;
    }
    if (out_envp_count != NULL)
        *out_envp_count = total_entries;
    if (out_decision != NULL)
        *out_decision = dec;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_ca_env_validate_boot_args(
    const char *boot_args,
    uint32_t *out_has_enable,
    A840KmdCaEnvReason *out_reason)
{
    size_t len;
    size_t i = 0u;
    size_t key_len = strlen(A840_KMD_CA_ENV_KEY);
    size_t val_len = strlen(A840_KMD_CA_ENV_VALUE);
    uint32_t found_enable = 0u;

    if (out_has_enable != NULL)
        *out_has_enable = 0u;
    if (out_reason != NULL)
        *out_reason = A840_KMD_CA_REASON_NONE;

    if (boot_args == NULL) {
        if (out_reason != NULL)
            *out_reason = A840_KMD_CA_REJECT_INVALID_ARG;
        return A840_KMD_INVALID_ARG;
    }

    len = strlen(boot_args);
    if (len > A840_KMD_CA_MAX_BOOT_ARGS_LEN) {
        if (out_reason != NULL)
            *out_reason = A840_KMD_CA_REJECT_BOOT_ARGS_TOO_LONG;
        return A840_KMD_RANGE;
    }

    for (i = 0u; i < len; ++i) {
        if (!is_printable_ascii_char((unsigned char)boot_args[i])) {
            if (out_reason != NULL)
                *out_reason = A840_KMD_CA_REJECT_ILLEGAL_CHAR;
            return A840_KMD_INVALID_ARG;
        }
    }

    i = 0u;
    while (i < len) {
        size_t start;
        size_t tok_len;
        const char *tok;
        const char *eq = NULL;
        size_t k;

        while (i < len && boot_args[i] == ' ')
            i++;
        if (i >= len)
            break;

        start = i;
        while (i < len && boot_args[i] != ' ')
            i++;
        tok = boot_args + start;
        tok_len = i - start;

        for (k = 0u; k < tok_len; ++k) {
            if (tok[k] == '=') {
                eq = tok + k;
                break;
            }
        }

        if (eq == NULL) {
            if (ascii_eq_nocase_n(tok, tok_len, A840_KMD_CA_ENV_KEY, key_len)) {
                if (out_reason != NULL)
                    *out_reason = A840_KMD_CA_REJECT_CONFLICTING_VALUE;
                return A840_KMD_STATE_CONFLICT;
            }
        } else {
            size_t klen = (size_t)(eq - tok);
            size_t vlen = tok_len - klen - 1u;
            if (ascii_eq_nocase_n(tok, klen, A840_KMD_CA_ENV_KEY, key_len)) {
                if (klen != key_len || memcmp(tok, A840_KMD_CA_ENV_KEY, key_len) != 0 ||
                    vlen != val_len || memcmp(eq + 1, A840_KMD_CA_ENV_VALUE, val_len) != 0) {
                    if (out_reason != NULL)
                        *out_reason = A840_KMD_CA_REJECT_CONFLICTING_VALUE;
                    return A840_KMD_STATE_CONFLICT;
                }
                found_enable = 1u;
            }
        }
    }

    if (out_has_enable != NULL)
        *out_has_enable = found_enable;
    return A840_KMD_OK;
}

A840KmdStatus a840_kmd_ca_env_append_boot_args(
    const A840KmdCaEnvPolicyInput *input,
    const char *existing_boot_args,
    char *out_buf,
    size_t out_cap,
    size_t *out_written_len,
    A840KmdCaEnvDecision *out_decision)
{
    A840KmdCaEnvDecision dec;
    A840KmdCaEnvReason reason = A840_KMD_CA_REASON_NONE;
    A840KmdStatus st;
    uint32_t already_present = 0u;
    size_t existing_len;
    size_t entry_len = strlen(A840_KMD_CA_ENV_ENTRY);
    size_t need_space = 0u;
    size_t final_len;
    char staging[A840_KMD_CA_MAX_BOOT_ARGS_LEN + 1u];

    init_decision(&dec);

    if (existing_boot_args == NULL || out_buf == NULL || out_cap == 0u) {
        set_decision_reject(&dec, A840_KMD_CA_REJECT_INVALID_ARG);
        if (out_decision != NULL)
            *out_decision = dec;
        return A840_KMD_INVALID_ARG;
    }

    st = a840_kmd_ca_env_evaluate(input, &dec);
    if (st != A840_KMD_OK) {
        if (out_decision != NULL)
            *out_decision = dec;
        return st;
    }

    st = a840_kmd_ca_env_validate_boot_args(existing_boot_args, &already_present, &reason);
    if (st != A840_KMD_OK) {
        set_decision_reject(&dec, reason);
        if (out_decision != NULL)
            *out_decision = dec;
        return st;
    }

    existing_len = strlen(existing_boot_args);
    if (already_present) {
        final_len = existing_len;
    } else {
        need_space = (existing_len > 0u && existing_boot_args[existing_len - 1u] != ' ') ? 1u : 0u;
        final_len = existing_len + need_space + entry_len;
    }

    if (final_len > A840_KMD_CA_MAX_BOOT_ARGS_LEN) {
        set_decision_reject(&dec, A840_KMD_CA_REJECT_BOOT_ARGS_TOO_LONG);
        if (out_decision != NULL)
            *out_decision = dec;
        return A840_KMD_RANGE;
    }
    if ((final_len + 1u) > out_cap) {
        set_decision_reject(&dec, A840_KMD_CA_REJECT_BUFFER_TOO_SMALL);
        if (out_decision != NULL)
            *out_decision = dec;
        return A840_KMD_RANGE;
    }

    /* Stage first so in-place (existing_boot_args == out_buf) is always safe and atomic. */
    if (existing_len > 0u)
        memcpy(staging, existing_boot_args, existing_len);
    if (!already_present) {
        if (need_space)
            staging[existing_len] = ' ';
        memcpy(staging + existing_len + need_space, A840_KMD_CA_ENV_ENTRY, entry_len);
    }
    staging[final_len] = '\0';

    memcpy(out_buf, staging, final_len + 1u);

    if (already_present) {
        dec.already_present = 1u;
        dec.reason = (uint32_t)A840_KMD_CA_ALLOW_ALREADY_PRESENT;
    }
    if (out_written_len != NULL)
        *out_written_len = final_len;
    if (out_decision != NULL)
        *out_decision = dec;
    return A840_KMD_OK;
}
