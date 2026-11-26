/* SPDX-License-Identifier: MIT */
#ifndef A840_RECORD_IO_H
#define A840_RECORD_IO_H

#include "AdrenoCompileABI.h"

enum {
    A840_RECORD_VERSION = 1,
    A840_RECORD_HEADER_BYTES = 64,
    A840_RECORD_PREFIX_BYTES = 128,
    A840_RECORD_MAX_STAGES = 2,
    A840_RECORD_MAX_BINDINGS = 128,
    A840_RECORD_MAX_BYTES = 16u * 1024u * 1024u,
    A840_RECORD_MAX_ISA_DWORDS = 1024u * 1024u,
    A840_RECORD_MAX_CONST_DWORDS = 64u * 1024u,
    A840_RECORD_MAX_SDS_DWORDS = 256u * 1024u,
    A840_RECORD_HBB = 16
};
#define A840_RECORD_CHIP_ID UINT64_C(0x44050a31)

typedef enum A840RecordStatus {
    A840_RECORD_OK = 0,
    A840_RECORD_INVALID_ARG,
    A840_RECORD_RANGE_ERROR,
    A840_RECORD_CAPACITY,
    A840_RECORD_MAGIC_ERROR,
    A840_RECORD_VERSION_ERROR,
    A840_RECORD_LENGTH_ERROR,
    A840_RECORD_ABI_MISMATCH,
    A840_RECORD_STACK_MISMATCH,
    A840_RECORD_CHIP_MISMATCH,
    A840_RECORD_HBB_MISMATCH,
    A840_RECORD_HASH_MISMATCH,
    A840_RECORD_FORMAT_ERROR,
    A840_RECORD_OOM,
    A840_RECORD_IO_ERROR,
    A840_RECORD_NOT_FOUND,
    A840_RECORD_ALIAS
} A840RecordStatus;

typedef struct A840PipelineKey { uint8_t sha256[32]; } A840PipelineKey;

/* Logical fixed header. The wire image is explicitly little-endian, never
 * memcpy of a native struct. Offsets: magic0, version8, prefix_bytes12,
 * total_bytes16, ABI24, stack32, chip40, HBB48, stages52, bindings56, flags60.
 * Extension [64,96) is pipeline_key; [96,128) is the integrity SHA-256.
 * Digest covers the WHOLE record with its own 32 bytes replaced by zero,
 * including identity/key metadata, all stage data and all reflection bindings.
 * No pointer, native padding, owner address or timestamp is stored. */
typedef struct A840RecordHeader {
    uint8_t magic[8];
    uint32_t version, prefix_bytes;
    uint64_t total_bytes, abi_fingerprint, stack_key, chip_id;
    uint32_t highest_bank_bit, stage_count, binding_count, flags;
} A840RecordHeader;

/* Exact NmbSpirvBinding scalar fields, independent of translator headers. */
typedef struct A840RecordBinding {
    uint32_t descriptor_set, binding, descriptor_type, descriptor_count;
    uint32_t stage_mask, buffer_index, texture_index, sampler_index;
} A840RecordBinding;

typedef struct A840RecordKeyStage {
    const char *entry_point;
    uint32_t stage;
    uint8_t spirv_sha256[32]; /* Digest of canonical little-endian SPIR-V bytes. */
    uint32_t color_rt_mask, half_rt_mask, sample_count, local_size[3], flags;
    uint32_t const_push_mask;
    uint32_t const_push_dwords[ADRENO_COMPILE_MAX_CONST_PUSH_DWORDS];
} A840RecordKeyStage;

typedef struct A840PipelineRecord {
    uint64_t abi_fingerprint, stack_key, chip_id;
    uint32_t highest_bank_bit, stage_count, binding_count, flags;
    A840PipelineKey pipeline_key;
    A840CompiledStage stages[A840_RECORD_MAX_STAGES];
    A840RecordBinding bindings[A840_RECORD_MAX_BINDINGS];
    /* NULL for borrowed compiler output. Deserialization owns one bounded
     * allocation containing ISA/const/SDS words; compiler stage owners are
     * NOT persisted or adopted. Use record_free, never backend stage_free,
     * for deserialized payloads. Freeing a borrowed record frees no stage. */
    void *owned_storage;
    size_t owned_bytes;
} A840PipelineRecord;

typedef struct A840PipelineMemCache A840PipelineMemCache;
typedef struct A840PipelineCacheStats {
    size_t capacity, byte_capacity, entries, bytes;
    uint64_t hits, misses, puts, evictions, replacements;
} A840PipelineCacheStats;

#if defined(__cplusplus)
static_assert(sizeof(A840RecordHeader) == 64, "A840PR01 fixed header size");
static_assert(sizeof(A840RecordBinding) == 32, "reflection binding size");
static_assert(offsetof(A840RecordHeader, flags) == 60, "A840PR01 header offsets");
extern "C" {
#else
_Static_assert(sizeof(A840RecordHeader) == 64, "A840PR01 fixed header size");
_Static_assert(sizeof(A840RecordBinding) == 32, "reflection binding size");
_Static_assert(offsetof(A840RecordHeader, flags) == 60, "A840PR01 header offsets");
#endif

const char *a840_record_status_string(A840RecordStatus status);
A840RecordStatus a840_record_sha256(const void *data, size_t bytes, uint8_t out[32]);
/* One compute stage or ordered VS+FS. Key is domain/length separated and
 * includes ALL listed specialization fields, including constant push data. */
A840RecordStatus a840_record_compute_key(const A840RecordKeyStage *stages,
    uint32_t stage_count, A840PipelineKey *out_key);
/* Allocates *out_data (caller free). Failure preserves both output slots. */
A840RecordStatus a840_record_serialize(const A840PipelineRecord *rec,
    uint8_t **out_data, size_t *out_bytes);
/* out_rec must be zero-initialized or previously record_free'd. Failure leaves
 * it unchanged. All validation, including full hash, precedes allocation.
 * expected_key may be NULL; identity expectations must be nonzero. */
A840RecordStatus a840_record_deserialize(const uint8_t *data, size_t bytes,
    uint64_t expected_stack_key, uint64_t expected_abi_fingerprint,
    const A840PipelineKey *expected_key, A840PipelineRecord *out_rec);
void a840_record_free(A840PipelineRecord *rec);

/* Existing directory, filename <64 lowercase key hex>.a840pr. Saves use a
 * unique same-directory temporary, fsync(file), rename, fsync(directory).
 * Readers reject symlinks/non-regular files, wrong keys and all stale identity.
 * A post-rename fsync error may leave a complete valid new file published;
 * it never publishes a partial file. No backend/compiler dependency here. */
A840RecordStatus a840_record_save_file(const char *dir, const A840PipelineRecord *rec);
A840RecordStatus a840_record_load_file(const char *dir, const A840PipelineKey *key,
    uint64_t expected_stack_key, uint64_t expected_abi_fingerprint,
    A840PipelineRecord *out_rec);

/* Single-threaded/caller-synchronized fixed-capacity cache. Hash buckets give
 * expected O(1) lookup; intrusive LRU and hash unlink are O(1). Byte budget is
 * the serialized size of resident records (fixed bookkeeping separately bound
 * by capacity). put clones; get borrows until replacement/eviction/destroy.
 * An invalid/oversize/OOM put leaves entries and statistics unchanged. */
A840RecordStatus a840_pipeline_mem_cache_create(size_t capacity, size_t byte_capacity,
    uint64_t stack_key, uint64_t abi_fingerprint, A840PipelineMemCache **out_cache);
void a840_pipeline_mem_cache_destroy(A840PipelineMemCache *cache);
const A840PipelineRecord *a840_pipeline_mem_cache_get(A840PipelineMemCache *cache,
    const A840PipelineKey *key);
A840RecordStatus a840_pipeline_mem_cache_put(A840PipelineMemCache *cache,
    const A840PipelineRecord *rec);
A840RecordStatus a840_pipeline_mem_cache_stats(const A840PipelineMemCache *cache,
    A840PipelineCacheStats *out_stats);

#if defined(__cplusplus)
}
#endif
#endif /* A840_RECORD_IO_H */
