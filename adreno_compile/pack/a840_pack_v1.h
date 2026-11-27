/* SPDX-License-Identifier: MIT */
#ifndef A840_PACK_V1_H
#define A840_PACK_V1_H

#include <stddef.h>
#include <stdint.h>
#include "../cache/a840_record_io.h"
#include "../layout/a840_binding_plan.h"

#define A840_PACK_V1_MAGIC UINT32_C(0x314b5038) /* wire bytes "8PK1"; container A840PK01 */
#define A840_PACK_V1_VERSION 1u
#define A840_PACK_V1_ISA_ALIGNMENT 128u
#define A840_PACK_V1_HEADER_BYTES 64u
#define A840_PACK_V1_ENTRY_BYTES 128u
#define A840_PACK_V1_STAGE_BYTES 640u
#define A840_PACK_V1_MAX_ENTRIES 32768u
#define A840_PACK_V1_MAX_BUCKETS 65536u
#define A840_PACK_V1_MAX_PROBES 16u
#define A840_PACK_V1_EMPTY UINT32_MAX
#define A840_PACK_V1_CHIP_ID UINT32_C(0x44050a31)
#define A840_PACK_V1_HBB 16u

typedef enum {
    A840_PACK_OK = 0,
    A840_PACK_INVALID_ARG,
    A840_PACK_RANGE,
    A840_PACK_CAPACITY,
    A840_PACK_MAGIC,
    A840_PACK_VERSION,
    A840_PACK_LENGTH,
    A840_PACK_ABI_MISMATCH,
    A840_PACK_STACK_MISMATCH,
    A840_PACK_CHIP_MISMATCH,
    A840_PACK_HBB_MISMATCH,
    A840_PACK_HASH_MISMATCH,
    A840_PACK_FORMAT,
    A840_PACK_NOT_FOUND,
    A840_PACK_ALIAS,
    A840_PACK_IO,
    A840_PACK_UNSUPPORTED,
    A840_PACK_KEY_CONFLICT
} A840PackV1Status;

/* Exact 64-byte header, encoded field-by-field in little endian.
 * control: version bits[1:0], tier bit[2], HBB bits[7:3].
 * hash_bucket_log2 encodes the power-of-two bucket count, 1..16.
 * entry_count is u16, <=32768; payload/file offsets are u32, file <=4GiB-1.
 * These explicit widths keep ALL identity fields plus SHA256 within 64B.
 * SHA covers every byte with file_sha256 [32,64) replaced by zeros.
 * No native struct image or padding is copied into the wire format.
 */
typedef struct A840PackV1Header {
    uint32_t magic;
    uint8_t control, hash_bucket_log2;
    uint16_t entry_count;
    uint64_t abi_fingerprint, stack_key;
    uint32_t chip_id, payload_bytes;
    uint8_t file_sha256[32];
} A840PackV1Header;

/* Hash buckets at byte64 store LE u32 directory IDs (UINT32_MAX=empty).
 * Directory is 128-aligned after buckets; every entry is exactly 128B.
 * Payload starts 128-aligned after the directory. Keys/directories are
 * strictly sorted; index load<=0.5, and every hit fits MAX_PROBES.
 */
typedef struct A840PackV1EntryWire {
    uint8_t pipeline_key_sha256[32];
    uint32_t flags, stage_count, stage_metadata_offset;
    uint32_t binding_offset, binding_count;
    uint32_t plan_offset, plan_bytes, payload_offset, payload_bytes;
    uint8_t reserved[60];
} A840PackV1EntryWire;

/* words[0..148] use A840PR01's scalar stage metadata order (149 words).
 * words[149..151] are absolute file offsets for ISA, const data and SDS.
 * words[152..159] are zero. ISA is 128-aligned; const/SDS follow it.
 */
typedef struct A840PackV1StageWire { uint32_t words[160]; } A840PackV1StageWire;

typedef struct A840PackV1StageView {
    const A840PackV1StageWire *metadata;
    const uint32_t *isa_words, *const_data_words, *sds_state_words;
    uint32_t stage, isa_dwords, const_data_dwords, sds_state_dwords;
    uint32_t instruction_groups, flags_out;
} A840PackV1StageView;

typedef struct A840PackV1Entry {
    const A840PipelineKey *pipeline_key;
    const A840RecordBinding *bindings;
    const A840BindingPlan *binding_plan;
    uint32_t flags, binding_count, stage_count;
    A840PackV1StageView stages[2];
} A840PackV1Entry;

typedef struct A840PackV1View {
    const uint8_t *base;
    size_t bytes;
    uint64_t stack_key, abi_fingerprint;
    uint32_t tier, entry_count, hash_bucket_count;
    uint32_t directory_offset, payload_offset, valid;
} A840PackV1View;

/* Builder borrows records/plans until build completes. Workspaces belong
 * to the caller. Read APIs do not allocate or copy payloads; cold builder
 * sorting and file publication use libc services.
 */
typedef struct A840PackV1BuilderItem {
    const A840PipelineRecord *record;
    const A840BindingPlan *plan;
    uint32_t payload_offset, payload_bytes, metadata_offset, binding_offset, plan_offset;
    uint32_t isa_offsets[2], const_offsets[2], sds_offsets[2];
} A840PackV1BuilderItem;
typedef struct A840PackV1Builder {
    A840PackV1BuilderItem *items;
    uint32_t *buckets;
    size_t capacity, bucket_capacity, count;
    uint64_t stack_key, abi_fingerprint;
    uint32_t tier, bucket_count, directory_offset, payload_offset, total_bytes, ready;
} A840PackV1Builder;

#if defined(__cplusplus)
static_assert(sizeof(A840PackV1Header)==64 && alignof(A840PackV1Header)==8, "pack header64/align8");
static_assert(offsetof(A840PackV1Header,file_sha256)==32, "pack hash field offset");
static_assert(sizeof(A840PackV1EntryWire)==128 && alignof(A840PackV1EntryWire)==4, "pack directory128");
static_assert(sizeof(A840PackV1StageWire)==640, "pack stage metadata640");
static_assert(sizeof(A840BindingPlan)==23632 && alignof(A840BindingPlan)==4, "binding plan scalar wire layout");
extern "C" {
#else
_Static_assert(sizeof(A840PackV1Header)==64 && _Alignof(A840PackV1Header)==8, "pack header64/align8");
_Static_assert(offsetof(A840PackV1Header,file_sha256)==32, "pack hash field offset");
_Static_assert(sizeof(A840PackV1EntryWire)==128 && _Alignof(A840PackV1EntryWire)==4, "pack directory128");
_Static_assert(sizeof(A840PackV1StageWire)==640, "pack stage metadata640");
_Static_assert(sizeof(A840BindingPlan)==23632 && _Alignof(A840BindingPlan)==4, "binding plan scalar wire layout");
#endif

const char *a840_pack_v1_status_string(A840PackV1Status status);
A840PackV1Status a840_pack_v1_sha256(const void *data, size_t bytes, uint8_t digest[32]);

/* Borrowed immutable view; base must remain RO/alive and 128-aligned.
 * Full bounds/canonical layout/SHA/index validation happens before publish.
 * Runtime zero-copy views support the target LE x86_64/arm64 hosts.
 * Failure preserves *view. No malloc, memcpy or payload ownership.
 */
A840PackV1Status a840_pack_v1_open_memory(const void *base, size_t bytes,
    uint64_t expected_stack_key, uint64_t expected_abi_fp, A840PackV1View *view);
A840PackV1Status a840_pack_v1_lookup(const A840PackV1View *view,
    const A840PipelineKey *key, A840PackV1Entry *entry);
A840PackV1Status a840_pack_v1_entry_at(const A840PackV1View *view,
    uint32_t index, A840PackV1Entry *entry);

/* Cold writer interop: reconstructs scalar record metadata, borrows mapped
 * payloads, owns nothing. This is NOT used by open/lookup hot paths.
 */
A840PackV1Status a840_pack_v1_entry_record(const A840PackV1View *view,
    const A840PackV1Entry *entry, A840PipelineRecord *record);

A840PackV1Status a840_pack_v1_builder_init(A840PackV1Builder *builder,
    uint32_t tier, uint64_t stack_key, uint64_t abi_fp,
    A840PackV1BuilderItem *items, size_t capacity,
    uint32_t *buckets, size_t bucket_capacity);
A840PackV1Status a840_pack_v1_builder_add(A840PackV1Builder *builder,
    const A840PipelineRecord *record, const A840BindingPlan *plan);
A840PackV1Status a840_pack_v1_builder_measure(A840PackV1Builder *builder, size_t *bytes);
A840PackV1Status a840_pack_v1_builder_build(A840PackV1Builder *builder,
    void *buffer, size_t capacity_bytes, size_t *bytes);
/* Atomic same-directory temporary + fsync + rename; output must resolve
 * under project .cache/<any-subdir>, never C:/mnt/c or a source folder.
 */
A840PackV1Status a840_pack_v1_save_file(const char *path,
    const void *buffer, size_t bytes, uint64_t stack_key, uint64_t abi_fp);

#ifdef __cplusplus
}
#endif
#endif /* A840_PACK_V1_H */
