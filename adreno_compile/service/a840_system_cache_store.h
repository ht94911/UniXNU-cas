/* SPDX-License-Identifier: MIT */
/* Wave 12 / Lane 2: iOS 27 Three-Tier System Shader Cache Store
 *
 * Hierarchy:
 *   L0: Read-only precompiled A840PK01 pack.bin (zero-copy mmap, O(1) lookup < 1 us)
 *   L1: Role-separated atomic disk cache:
 *         - RENDER_SERVER (backboardd / CARenderServer / NezhaShaderCompiler):
 *             reads & atomically writes shared cache directory (with LRU cap)
 *         - APP_SANDBOX (ordinary iOS 27 sandboxed apps):
 *             READ-ONLY on shared cache directory (zero write attempts),
 *             reads & atomically writes process-private container cache directory
 *         - READONLY:
 *             reads L0 + L1 only, rejects all writes
 *   Miss: Returns A840_SYS_CACHE_MISS so caller dispatches live compilation
 *         through NezhaShaderCompiler over AF_UNIX.
 *
 * Safety & Darwin compatibility:
 *   - Pure C11, zero Vulkan/Turnip/bridge dependencies, zero Linux-only headers.
 *   - Atomic persistence via same-directory temporary + fsync/F_FULLFSYNC + rename.
 *   - Dual integrity verification: 40-byte A840SC01 CRC32+ABI envelope + full
 *     A840PR01 SHA-256 and A840CompileABI fingerprint validation.
 *   - Corrupt/truncated/symlink/ABI-mismatched files are rejected without crash,
 *     never pollute L0 pack.bin, and are quarantined only by writable roles.
 */
#ifndef A840_SYSTEM_CACHE_STORE_H
#define A840_SYSTEM_CACHE_STORE_H

#include "../cache/a840_record_io.h"
#include "../pack/a840_pack_v1.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    A840_SYS_CACHE_DEFAULT_MAX_BYTES = 128u * 1024u * 1024u, /* 128 MiB */
    A840_SYS_CACHE_ENVELOPE_MAGIC    = 0x31534338u,          /* "8CS1" LE */
    A840_SYS_CACHE_ENVELOPE_VERSION  = 1u,
    A840_SYS_CACHE_ENVELOPE_BYTES    = 40u
};

typedef enum A840SystemCacheRole {
    A840_CACHE_ROLE_RENDER_SERVER = 0, /* backboardd / daemon: shared R/W */
    A840_CACHE_ROLE_APP_SANDBOX   = 1, /* App process: shared RO + private R/W */
    A840_CACHE_ROLE_READONLY      = 2  /* Pure read-only L0 + L1 */
} A840SystemCacheRole;

typedef enum A840SystemCacheTier {
    A840_CACHE_TIER_MISS       = 0,
    A840_CACHE_TIER_L0_PACK    = 1,
    A840_CACHE_TIER_L1_SHARED  = 2,
    A840_CACHE_TIER_L1_PRIVATE = 3
} A840SystemCacheTier;

typedef enum A840SystemCacheStatus {
    A840_SYS_CACHE_OK = 0,
    A840_SYS_CACHE_MISS = 1,
    A840_SYS_CACHE_INVALID_ARG = 2,
    A840_SYS_CACHE_ABI_MISMATCH = 3,
    A840_SYS_CACHE_CORRUPT = 4,
    A840_SYS_CACHE_READONLY = 5,
    A840_SYS_CACHE_IO_ERROR = 6,
    A840_SYS_CACHE_OOM = 7,
    A840_SYS_CACHE_PATH_FORBIDDEN = 8
} A840SystemCacheStatus;

typedef struct A840SystemCacheConfig {
    uint64_t stack_key;
    uint64_t abi_fingerprint;        /* 0 -> a840_compile_abi_fingerprint() */
    A840SystemCacheRole role;
    const char *pack_path;           /* Optional L0 read-only pack.bin */
    const char *shared_cache_dir;    /* Optional L1 shared cache root */
    const char *private_cache_dir;   /* Optional L1 process-private container dir */
    size_t max_cache_bytes;          /* 0 -> A840_SYS_CACHE_DEFAULT_MAX_BYTES (128 MiB) */
    bool isolate_by_abi;             /* Namespace L1 subdir by (abi_fingerprint, stack_key) */
    bool quarantine_corrupt;         /* Unlink corrupt files when directory is writable */
} A840SystemCacheConfig;

typedef struct A840SystemCacheStats {
    uint64_t l0_pack_hits;
    uint64_t l1_shared_hits;
    uint64_t l1_private_hits;
    uint64_t misses;
    uint64_t corrupt_rejected;
    uint64_t evictions;
    uint64_t puts_shared;
    uint64_t puts_private;
    uint64_t abi_mismatch_rejected;
    uint64_t l0_pack_entries;
    uint64_t l1_current_bytes;
    uint64_t l1_max_bytes;
} A840SystemCacheStats;

typedef struct A840SystemCacheStore A840SystemCacheStore;

const char *a840_system_cache_status_string(A840SystemCacheStatus status);

/* Standard IEEE 802.3 CRC32 used for fast envelope corruption screening. */
uint32_t a840_system_cache_crc32(const void *data, size_t bytes);

/* Create a thread-safe three-tier system cache store.
 * If pack_path is set, attempts read-only mmap mount; a missing or corrupt
 * pack_path degrades gracefully to L1-only (recording corrupt_rejected on
 * corruption) instead of failing startup.
 */
A840SystemCacheStatus a840_system_cache_create(
    const A840SystemCacheConfig *cfg,
    A840SystemCacheStore **out_store);

void a840_system_cache_destroy(A840SystemCacheStore *store);

/* Mount or replace the read-only L0 pack.bin mapping. Validates full SHA-256,
 * chip_id, HBB, stack_key, and abi_fingerprint before publishing. On failure,
 * any existing valid pack mapping remains untouched.
 */
A840SystemCacheStatus a840_system_cache_mount_pack(
    A840SystemCacheStore *store,
    const char *pack_path);

bool a840_system_cache_has_pack(const A840SystemCacheStore *store);
const char *a840_system_cache_active_shared_dir(const A840SystemCacheStore *store);
const char *a840_system_cache_active_private_dir(const A840SystemCacheStore *store);

/* Fast O(1) zero-copy L0 pack.bin lookup (< 1 us). Borrows pointers directly
 * into the read-only mmap'd pack.bin for the lifetime of the mount.
 */
A840SystemCacheStatus a840_system_cache_lookup_pack_view(
    A840SystemCacheStore *store,
    const A840PipelineKey *key,
    A840PackV1Entry *out_entry);

/* Full L0 -> L1 shared -> L1 private lookup returning an owned
 * A840PipelineRecord (caller must free with a840_record_free()).
 */
A840SystemCacheStatus a840_system_cache_lookup(
    A840SystemCacheStore *store,
    const A840PipelineKey *key,
    A840PipelineRecord *out_record,
    A840SystemCacheTier *out_tier);

/* Full L0 -> L1 shared -> L1 private lookup returning a malloc-owned
 * validated A840PR01 wire blob (caller must free()).
 */
A840SystemCacheStatus a840_system_cache_lookup_blob(
    A840SystemCacheStore *store,
    const A840PipelineKey *key,
    uint8_t **out_blob,
    size_t *out_bytes,
    A840SystemCacheTier *out_tier);

/* Atomically persist a validated A840PipelineRecord to the role-permitted L1
 * directory (shared for RENDER_SERVER, private container for APP_SANDBOX),
 * enforcing LRU capacity limits.
 */
A840SystemCacheStatus a840_system_cache_put(
    A840SystemCacheStore *store,
    const A840PipelineRecord *rec);

/* Atomically persist a validated A840PR01 wire blob to the role-permitted L1
 * directory, verifying SHA-256, key, stack_key, and abi_fingerprint before
 * writing and enforcing LRU capacity limits.
 */
A840SystemCacheStatus a840_system_cache_put_blob(
    A840SystemCacheStore *store,
    const A840PipelineKey *key,
    const uint8_t *blob,
    size_t bytes);

/* Explicitly enforce the LRU byte cap on the role's writable L1 directory. */
A840SystemCacheStatus a840_system_cache_enforce_limit(
    A840SystemCacheStore *store,
    uint64_t *out_evicted);

void a840_system_cache_get_stats(
    const A840SystemCacheStore *store,
    A840SystemCacheStats *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* A840_SYSTEM_CACHE_STORE_H */
