/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include "a840_shader_compiler_ir3_backend.h"

#ifdef A840_SC_IR3_ENABLE_TRANSLATE
#include "NativeMetalTranslate.h"
#endif

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

enum {
    IR3P_OFF_MAGIC = 0u,
    IR3P_OFF_VERSION = 4u,
    IR3P_OFF_PIPELINE_KIND = 8u,
    IR3P_OFF_SOURCE_KIND = 12u,
    IR3P_OFF_COMPILE_FLAGS = 16u,
    IR3P_OFF_COLOR_RT_MASK = 20u,
    IR3P_OFF_HALF_RT_MASK = 24u,
    IR3P_OFF_SAMPLE_COUNT = 28u,
    IR3P_OFF_LOCAL_SIZE = 32u,
    IR3P_OFF_COLOR_FORMATS = 44u,
    IR3P_OFF_DEPTH_FORMAT = 76u,
    IR3P_OFF_STENCIL_FORMAT = 80u,
    IR3P_OFF_STAGE0_BYTES = 84u,
    IR3P_OFF_STAGE1_BYTES = 88u,
    IR3P_OFF_RESERVED0 = 92u,
    IR3P_OFF_RESERVED1 = 96u,
    IR3P_OFF_RESERVED2 = 104u,
    IR3P_OFF_ENTRY_NAME = 128u,
    IR3P_OFF_FS_ENTRY_NAME = 192u,
    IR3P_RESERVED2_BYTES = 24u,
    IR3P_SPIRV_MAGIC = 0x07230203u,
    IR3P_KNOWN_COMPILE_FLAGS =
        A840_COMPILE_FLAG_ROBUST_OFF |
        A840_COMPILE_FLAG_EARLY_PREAMBLE |
        A840_COMPILE_FLAG_FBFETCH_GMEM |
        A840_COMPILE_FLAG_HALF_ROUND_RTZ |
        A840_COMPILE_FLAG_DISABLE_MERGED_REGS
};

struct A840ScIr3Backend {
    A840ScIr3BackendConfig cfg;
    uint64_t abi_fingerprint;
    pthread_mutex_t stats_mutex;
    pthread_mutex_t ir3_mutex;
    A840ScIr3BackendStats stats;
#ifdef A840_SC_IR3_ENABLE_TRANSLATE
    NmbDevice *nmb_dev;
#endif
};

static void put_le32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xffu);
    dst[1] = (uint8_t)((v >> 8) & 0xffu);
    dst[2] = (uint8_t)((v >> 16) & 0xffu);
    dst[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_le64(uint8_t *dst, uint64_t v)
{
    put_le32(dst, (uint32_t)(v & UINT32_MAX));
    put_le32(dst + 4, (uint32_t)(v >> 32));
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static uint64_t get_le64(const uint8_t *src)
{
    return (uint64_t)get_le32(src) | ((uint64_t)get_le32(src + 4) << 32);
}

static bool regions_overlap(const void *a, size_t a_len, const void *b, size_t b_len)
{
    uintptr_t xa = (uintptr_t)a;
    uintptr_t xb = (uintptr_t)b;
    return a_len != 0u && b_len != 0u &&
           (xa <= xb ? (xb - xa) < a_len : (xa - xb) < b_len);
}

static bool valid_sample_count(uint32_t n)
{
    return n == 1u || n == 2u || n == 4u;
}

static bool valid_name_field(const char field[A840_SC_IR3_ENTRY_NAME_MAX],
    bool require_nonempty)
{
    size_t nul_pos = A840_SC_IR3_ENTRY_NAME_MAX;
    for (size_t i = 0; i < A840_SC_IR3_ENTRY_NAME_MAX; ++i) {
        if (field[i] == '\0') {
            nul_pos = i;
            break;
        }
    }
    if (nul_pos == A840_SC_IR3_ENTRY_NAME_MAX)
        return false;
    if (require_nonempty && nul_pos == 0u)
        return false;
    if (!require_nonempty && nul_pos != 0u)
        return false;
    for (size_t i = nul_pos; i < A840_SC_IR3_ENTRY_NAME_MAX; ++i) {
        if (field[i] != '\0')
            return false;
    }
    return true;
}

static A840ScStatus validate_payload_header(const A840ScIr3CompilePayload *hdr)
{
    if (!hdr)
        return A840_SC_INVALID_ARG;
    if (hdr->magic != A840_SC_IR3_PAYLOAD_MAGIC ||
        hdr->version != A840_SC_IR3_PAYLOAD_VERSION)
        return A840_SC_PROTO_ERROR;
    if (hdr->reserved0 != 0u || hdr->reserved1 != 0u)
        return A840_SC_RESERVED_NONZERO;
    if ((hdr->compile_flags & ~(uint32_t)IR3P_KNOWN_COMPILE_FLAGS) != 0u)
        return A840_SC_PROTO_ERROR;
    if (hdr->pipeline_kind != A840_SC_IR3_PIPELINE_COMPUTE &&
        hdr->pipeline_kind != A840_SC_IR3_PIPELINE_RENDER_VS_FS)
        return A840_SC_PROTO_ERROR;
    if (hdr->source_kind != A840_SC_IR3_SRC_METALLIB &&
        hdr->source_kind != A840_SC_IR3_SRC_SPIRV)
        return A840_SC_PROTO_ERROR;
    if (!valid_sample_count(hdr->sample_count))
        return A840_SC_PROTO_ERROR;
    if (hdr->color_rt_mask > 0xffu || (hdr->half_rt_mask & ~hdr->color_rt_mask) != 0u)
        return A840_SC_PROTO_ERROR;
    if (!valid_name_field(hdr->entry_name, true))
        return A840_SC_PROTO_ERROR;

    if (hdr->pipeline_kind == A840_SC_IR3_PIPELINE_COMPUTE) {
        if (!valid_name_field(hdr->fs_entry_name, false))
            return A840_SC_PROTO_ERROR;
        if (hdr->stage1_bytes != 0u || hdr->color_rt_mask != 0u ||
            hdr->half_rt_mask != 0u || hdr->sample_count != 1u ||
            hdr->depth_mtl_format != 0u || hdr->stencil_mtl_format != 0u)
            return A840_SC_PROTO_ERROR;
        for (size_t i = 0; i < A840_SC_IR3_MAX_COLOR_RTS; ++i) {
            if (hdr->color_mtl_formats[i] != 0u)
                return A840_SC_PROTO_ERROR;
        }
    } else {
        if (!valid_name_field(hdr->fs_entry_name, true))
            return A840_SC_PROTO_ERROR;
        if (hdr->local_size[0] != 0u || hdr->local_size[1] != 0u || hdr->local_size[2] != 0u)
            return A840_SC_PROTO_ERROR;
    }

    if (hdr->stage0_bytes == 0u)
        return A840_SC_PROTO_ERROR;

    if (hdr->source_kind == A840_SC_IR3_SRC_METALLIB) {
        if (hdr->stage1_bytes != 0u)
            return A840_SC_PROTO_ERROR;
    } else {
        if (hdr->stage0_bytes < 20u || (hdr->stage0_bytes % 4u) != 0u)
            return A840_SC_PROTO_ERROR;
        if (hdr->pipeline_kind == A840_SC_IR3_PIPELINE_RENDER_VS_FS) {
            if (hdr->stage1_bytes < 20u || (hdr->stage1_bytes % 4u) != 0u)
                return A840_SC_PROTO_ERROR;
        } else if (hdr->stage1_bytes != 0u) {
            return A840_SC_PROTO_ERROR;
        }
    }

    const uint64_t body_bytes = (uint64_t)hdr->stage0_bytes + (uint64_t)hdr->stage1_bytes;
    const uint64_t total_bytes = (uint64_t)A840_SC_IR3_PAYLOAD_HEADER_BYTES + body_bytes;
    if (total_bytes > A840_SC_MAX_PAYLOAD_BYTES)
        return A840_SC_PAYLOAD_TOO_LARGE;
    return A840_SC_OK;
}

A840ScStatus a840_sc_ir3_payload_encode(const A840ScIr3CompilePayload *hdr,
    const void *stage0_data, const void *stage1_data,
    uint8_t *out_buf, size_t out_capacity, size_t *out_bytes)
{
    if (!hdr || !out_buf || !out_bytes || !stage0_data ||
        (hdr->stage1_bytes != 0u && !stage1_data))
        return A840_SC_INVALID_ARG;

    A840ScIr3CompilePayload norm = *hdr;
    if (norm.magic == 0u)
        norm.magic = A840_SC_IR3_PAYLOAD_MAGIC;
    if (norm.version == 0u)
        norm.version = A840_SC_IR3_PAYLOAD_VERSION;
    if (norm.sample_count == 0u)
        norm.sample_count = 1u;

    /* Zero any trailing bytes after first NUL in entry names for canonical wire form. */
    bool seen_nul = false;
    for (size_t i = 0; i < A840_SC_IR3_ENTRY_NAME_MAX; ++i) {
        if (seen_nul)
            norm.entry_name[i] = '\0';
        else if (norm.entry_name[i] == '\0')
            seen_nul = true;
    }
    seen_nul = false;
    for (size_t i = 0; i < A840_SC_IR3_ENTRY_NAME_MAX; ++i) {
        if (seen_nul)
            norm.fs_entry_name[i] = '\0';
        else if (norm.fs_entry_name[i] == '\0')
            seen_nul = true;
    }

    A840ScStatus st = validate_payload_header(&norm);
    if (st != A840_SC_OK)
        return st;

    const size_t total = (size_t)A840_SC_IR3_PAYLOAD_HEADER_BYTES +
                         (size_t)norm.stage0_bytes + (size_t)norm.stage1_bytes;
    if (out_capacity < total)
        return A840_SC_INVALID_ARG;
    if (regions_overlap(hdr, sizeof(*hdr), out_buf, total) ||
        regions_overlap(stage0_data, norm.stage0_bytes, out_buf, total) ||
        regions_overlap(stage1_data, norm.stage1_bytes, out_buf, total) ||
        regions_overlap(out_bytes, sizeof(*out_bytes), out_buf, total))
        return A840_SC_INVALID_ARG;

    put_le32(out_buf + IR3P_OFF_MAGIC, norm.magic);
    put_le32(out_buf + IR3P_OFF_VERSION, norm.version);
    put_le32(out_buf + IR3P_OFF_PIPELINE_KIND, norm.pipeline_kind);
    put_le32(out_buf + IR3P_OFF_SOURCE_KIND, norm.source_kind);
    put_le32(out_buf + IR3P_OFF_COMPILE_FLAGS, norm.compile_flags);
    put_le32(out_buf + IR3P_OFF_COLOR_RT_MASK, norm.color_rt_mask);
    put_le32(out_buf + IR3P_OFF_HALF_RT_MASK, norm.half_rt_mask);
    put_le32(out_buf + IR3P_OFF_SAMPLE_COUNT, norm.sample_count);
    for (size_t i = 0; i < 3u; ++i)
        put_le32(out_buf + IR3P_OFF_LOCAL_SIZE + i * 4u, norm.local_size[i]);
    for (size_t i = 0; i < A840_SC_IR3_MAX_COLOR_RTS; ++i)
        put_le32(out_buf + IR3P_OFF_COLOR_FORMATS + i * 4u, norm.color_mtl_formats[i]);
    put_le32(out_buf + IR3P_OFF_DEPTH_FORMAT, norm.depth_mtl_format);
    put_le32(out_buf + IR3P_OFF_STENCIL_FORMAT, norm.stencil_mtl_format);
    put_le32(out_buf + IR3P_OFF_STAGE0_BYTES, norm.stage0_bytes);
    put_le32(out_buf + IR3P_OFF_STAGE1_BYTES, norm.stage1_bytes);
    put_le32(out_buf + IR3P_OFF_RESERVED0, 0u);
    put_le64(out_buf + IR3P_OFF_RESERVED1, 0u);
    memset(out_buf + IR3P_OFF_RESERVED2, 0, IR3P_RESERVED2_BYTES);
    memcpy(out_buf + IR3P_OFF_ENTRY_NAME, norm.entry_name, A840_SC_IR3_ENTRY_NAME_MAX);
    memcpy(out_buf + IR3P_OFF_FS_ENTRY_NAME, norm.fs_entry_name, A840_SC_IR3_ENTRY_NAME_MAX);

    memcpy(out_buf + A840_SC_IR3_PAYLOAD_HEADER_BYTES, stage0_data, norm.stage0_bytes);
    if (norm.stage1_bytes != 0u) {
        memcpy(out_buf + A840_SC_IR3_PAYLOAD_HEADER_BYTES + norm.stage0_bytes,
               stage1_data, norm.stage1_bytes);
    }
    *out_bytes = total;
    return A840_SC_OK;
}

A840ScStatus a840_sc_ir3_payload_decode(const uint8_t *payload,
    size_t payload_bytes, A840ScIr3CompilePayload *out_hdr,
    const uint8_t **out_stage0, const uint8_t **out_stage1)
{
    if (!payload || !out_hdr || !out_stage0 || !out_stage1)
        return A840_SC_INVALID_ARG;
    if (payload_bytes < A840_SC_IR3_PAYLOAD_HEADER_BYTES)
        return A840_SC_TRUNCATED;
    if (payload_bytes > A840_SC_MAX_PAYLOAD_BYTES)
        return A840_SC_PAYLOAD_TOO_LARGE;

    for (size_t i = 0; i < IR3P_RESERVED2_BYTES; ++i) {
        if (payload[IR3P_OFF_RESERVED2 + i] != 0u)
            return A840_SC_RESERVED_NONZERO;
    }

    A840ScIr3CompilePayload decoded;
    memset(&decoded, 0, sizeof(decoded));
    decoded.magic = get_le32(payload + IR3P_OFF_MAGIC);
    decoded.version = get_le32(payload + IR3P_OFF_VERSION);
    decoded.pipeline_kind = get_le32(payload + IR3P_OFF_PIPELINE_KIND);
    decoded.source_kind = get_le32(payload + IR3P_OFF_SOURCE_KIND);
    decoded.compile_flags = get_le32(payload + IR3P_OFF_COMPILE_FLAGS);
    decoded.color_rt_mask = get_le32(payload + IR3P_OFF_COLOR_RT_MASK);
    decoded.half_rt_mask = get_le32(payload + IR3P_OFF_HALF_RT_MASK);
    decoded.sample_count = get_le32(payload + IR3P_OFF_SAMPLE_COUNT);
    for (size_t i = 0; i < 3u; ++i)
        decoded.local_size[i] = get_le32(payload + IR3P_OFF_LOCAL_SIZE + i * 4u);
    for (size_t i = 0; i < A840_SC_IR3_MAX_COLOR_RTS; ++i)
        decoded.color_mtl_formats[i] = get_le32(payload + IR3P_OFF_COLOR_FORMATS + i * 4u);
    decoded.depth_mtl_format = get_le32(payload + IR3P_OFF_DEPTH_FORMAT);
    decoded.stencil_mtl_format = get_le32(payload + IR3P_OFF_STENCIL_FORMAT);
    decoded.stage0_bytes = get_le32(payload + IR3P_OFF_STAGE0_BYTES);
    decoded.stage1_bytes = get_le32(payload + IR3P_OFF_STAGE1_BYTES);
    decoded.reserved0 = get_le32(payload + IR3P_OFF_RESERVED0);
    decoded.reserved1 = get_le64(payload + IR3P_OFF_RESERVED1);
    memcpy(decoded.entry_name, payload + IR3P_OFF_ENTRY_NAME, A840_SC_IR3_ENTRY_NAME_MAX);
    memcpy(decoded.fs_entry_name, payload + IR3P_OFF_FS_ENTRY_NAME, A840_SC_IR3_ENTRY_NAME_MAX);

    A840ScStatus st = validate_payload_header(&decoded);
    if (st != A840_SC_OK)
        return st;

    const size_t expected_total = (size_t)A840_SC_IR3_PAYLOAD_HEADER_BYTES +
                                  (size_t)decoded.stage0_bytes + (size_t)decoded.stage1_bytes;
    if (payload_bytes < expected_total)
        return A840_SC_TRUNCATED;
    if (payload_bytes > expected_total)
        return A840_SC_PROTO_ERROR;

    const uint8_t *s0 = payload + A840_SC_IR3_PAYLOAD_HEADER_BYTES;
    const uint8_t *s1 = decoded.stage1_bytes ? (s0 + decoded.stage0_bytes) : NULL;

    if (decoded.source_kind == A840_SC_IR3_SRC_SPIRV) {
        if (get_le32(s0) != IR3P_SPIRV_MAGIC)
            return A840_SC_PROTO_ERROR;
        if (s1 && get_le32(s1) != IR3P_SPIRV_MAGIC)
            return A840_SC_PROTO_ERROR;
    }

    *out_hdr = decoded;
    *out_stage0 = s0;
    *out_stage1 = s1;
    return A840_SC_OK;
}

A840ScStatus a840_sc_ir3_payload_key(const uint8_t *payload,
    size_t payload_bytes, A840PipelineKey *out_key)
{
    if (!payload || !out_key)
        return A840_SC_INVALID_ARG;
    A840ScIr3CompilePayload hdr;
    const uint8_t *s0 = NULL;
    const uint8_t *s1 = NULL;
    A840ScStatus st = a840_sc_ir3_payload_decode(payload, payload_bytes, &hdr, &s0, &s1);
    if (st != A840_SC_OK)
        return st;

    uint8_t digest[32];
    if (a840_record_sha256(payload, payload_bytes, digest) != A840_RECORD_OK)
        return A840_SC_INVALID_ARG;
    memcpy(out_key->sha256, digest, sizeof(digest));
    if (a840_sc_pipeline_key_is_zero(out_key))
        out_key->sha256[31] = 1u;
    return A840_SC_OK;
}

A840ScStatus a840_sc_ir3_backend_create(const A840ScIr3BackendConfig *cfg,
    A840ScIr3Backend **out_backend)
{
    if (!cfg || !out_backend || cfg->stack_key == 0u)
        return A840_SC_INVALID_ARG;
    if ((cfg->default_compile_flags & ~(uint32_t)IR3P_KNOWN_COMPILE_FLAGS) != 0u)
        return A840_SC_INVALID_ARG;

    const uint64_t abi_fp = cfg->abi_fingerprint ? cfg->abi_fingerprint
                                                 : a840_compile_abi_fingerprint();
    if (abi_fp != a840_compile_abi_fingerprint())
        return A840_SC_ABI_MISMATCH;

    A840ScIr3Backend *be = (A840ScIr3Backend *)calloc(1, sizeof(*be));
    if (!be)
        return A840_SC_OOM;
    be->cfg = *cfg;
    be->abi_fingerprint = abi_fp;

    if (pthread_mutex_init(&be->stats_mutex, NULL) != 0) {
        free(be);
        return A840_SC_OOM;
    }
    if (pthread_mutex_init(&be->ir3_mutex, NULL) != 0) {
        pthread_mutex_destroy(&be->stats_mutex);
        free(be);
        return A840_SC_OOM;
    }

#ifdef A840_SC_IR3_ENABLE_TRANSLATE
    if (cfg->enable_translate) {
        be->nmb_dev = nmb_device_create_a840(cfg->nmb_device_flags);
        if (!be->nmb_dev) {
            pthread_mutex_destroy(&be->ir3_mutex);
            pthread_mutex_destroy(&be->stats_mutex);
            free(be);
            return A840_SC_COMPILE_FAILED;
        }
    }
#endif

    *out_backend = be;
    return A840_SC_OK;
}

void a840_sc_ir3_backend_destroy(A840ScIr3Backend *backend)
{
    if (!backend)
        return;
#ifdef A840_SC_IR3_ENABLE_TRANSLATE
    if (backend->nmb_dev)
        nmb_device_destroy(backend->nmb_dev);
#endif
    pthread_mutex_destroy(&backend->ir3_mutex);
    pthread_mutex_destroy(&backend->stats_mutex);
    free(backend);
}

void a840_sc_ir3_backend_get_stats(A840ScIr3Backend *backend,
    A840ScIr3BackendStats *out_stats)
{
    if (!backend || !out_stats)
        return;
    pthread_mutex_lock(&backend->stats_mutex);
    *out_stats = backend->stats;
    pthread_mutex_unlock(&backend->stats_mutex);
}

static uint32_t *copy_aligned_spirv(const uint8_t *bytes, size_t len)
{
    if (!bytes || len < 20u || (len % 4u) != 0u)
        return NULL;
    size_t words = len / 4u;
    uint32_t *buf = (uint32_t *)malloc(words * sizeof(uint32_t));
    if (!buf)
        return NULL;
    for (size_t i = 0; i < words; ++i)
        buf[i] = get_le32(bytes + i * 4u);
    return buf;
}

static A840ScStatus compile_stages_to_record(A840ScIr3Backend *be,
    const A840ScFrameHeader *request,
    const A840ScIr3CompilePayload *hdr,
    const uint32_t *spv_words[2],
    const size_t spv_bytes[2],
    const char *entry_names[2],
    uint32_t stage_count,
    const A840RecordBinding *bindings,
    uint32_t binding_count,
    uint8_t **out_record_blob,
    size_t *out_record_bytes)
{
    const A840CompileTarget target = {
        .abi_version = ADRENO_COMPILE_ABI_VERSION,
        .reserved0 = 0u,
        .chip_id = A840_RECORD_CHIP_ID,
        .num_slices = 3u,
        .num_ccu = 6u,
        .highest_bank_bit = A840_RECORD_HBB,
        .ubwc_swizzle = 6u,
        .macrotile_mode = 1u,
        .flags = be->cfg.default_compile_flags | hdr->compile_flags
    };

    A840PipelineRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.abi_fingerprint = be->abi_fingerprint;
    rec.stack_key = be->cfg.stack_key;
    rec.chip_id = A840_RECORD_CHIP_ID;
    rec.highest_bank_bit = A840_RECORD_HBB;
    rec.stage_count = stage_count;
    rec.binding_count = binding_count;
    rec.pipeline_key = request->pipeline_key;
    if (binding_count > 0u && bindings) {
        memcpy(rec.bindings, bindings,
               (size_t)binding_count * sizeof(A840RecordBinding));
    }

    uint32_t compiled = 0u;
    A840ScStatus result = A840_SC_OK;

    for (uint32_t i = 0; i < stage_count; ++i) {
        A840StageInput input;
        memset(&input, 0, sizeof(input));
        input.abi_version = ADRENO_COMPILE_ABI_VERSION;
        input.stage = (stage_count == 1u) ? A840_STAGE_COMPUTE : i;
        input.spirv_words = spv_words[i];
        input.spirv_bytes = spv_bytes[i];
        input.entry_point = entry_names[i];
        input.sample_count = hdr->sample_count;
        input.flags = hdr->compile_flags;
        if (input.stage == A840_STAGE_COMPUTE) {
            input.local_size[0] = hdr->local_size[0];
            input.local_size[1] = hdr->local_size[1];
            input.local_size[2] = hdr->local_size[2];
        } else if (input.stage == A840_STAGE_FRAGMENT) {
            input.color_rt_mask = hdr->color_rt_mask;
            input.half_rt_mask = hdr->half_rt_mask;
        }

        char err[256];
        err[0] = '\0';
        /* Serialize Mesa ir3 entry across workers so Mesa internal globals and
         * ralloc/glsl_type caches remain completely data-race-free under TSan.
         */
        pthread_mutex_lock(&be->ir3_mutex);
        A840CompileStatus cst = a840_compile_stage(&target, &input,
            &rec.stages[i], err, sizeof(err));
        pthread_mutex_unlock(&be->ir3_mutex);

        if (cst != A840_COMPILE_OK) {
            pthread_mutex_lock(&be->stats_mutex);
            ++be->stats.failed_ir3;
            pthread_mutex_unlock(&be->stats_mutex);
            result = (cst == A840_COMPILE_OOM) ? A840_SC_OOM : A840_SC_COMPILE_FAILED;
            goto cleanup;
        }
        ++compiled;
    }

    uint8_t *blob = NULL;
    size_t blob_bytes = 0u;
    A840RecordStatus rst = a840_record_serialize(&rec, &blob, &blob_bytes);
    if (rst != A840_RECORD_OK) {
        pthread_mutex_lock(&be->stats_mutex);
        ++be->stats.failed_record;
        pthread_mutex_unlock(&be->stats_mutex);
        result = (rst == A840_RECORD_OOM) ? A840_SC_OOM : A840_SC_COMPILE_FAILED;
        goto cleanup;
    }

    *out_record_blob = blob;
    *out_record_bytes = blob_bytes;

cleanup:
    for (uint32_t i = 0; i < compiled; ++i)
        a840_compile_stage_free(&rec.stages[i]);
    return result;
}

A840ScStatus a840_sc_ir3_compile_callback(void *user_ctx, uint32_t worker_id,
    const A840ScFrameHeader *request, const uint8_t *payload,
    uint8_t **out_record_blob, size_t *out_record_bytes)
{
    (void)worker_id;
    if (!user_ctx || !request || !out_record_blob || !out_record_bytes)
        return A840_SC_INVALID_ARG;
    *out_record_blob = NULL;
    *out_record_bytes = 0u;

    A840ScIr3Backend *be = (A840ScIr3Backend *)user_ctx;
    pthread_mutex_lock(&be->stats_mutex);
    ++be->stats.total_calls;
    pthread_mutex_unlock(&be->stats_mutex);

    if (request->stack_key != be->cfg.stack_key)
        return A840_SC_STACK_MISMATCH;
    if (request->abi_fingerprint != be->abi_fingerprint)
        return A840_SC_ABI_MISMATCH;
    if (!payload || request->payload_bytes == 0u) {
        pthread_mutex_lock(&be->stats_mutex);
        ++be->stats.failed_payload;
        pthread_mutex_unlock(&be->stats_mutex);
        return A840_SC_COMPILE_FAILED;
    }

    A840ScIr3CompilePayload hdr;
    const uint8_t *stage0_raw = NULL;
    const uint8_t *stage1_raw = NULL;
    A840ScStatus st = a840_sc_ir3_payload_decode(payload, request->payload_bytes,
        &hdr, &stage0_raw, &stage1_raw);
    if (st != A840_SC_OK) {
        pthread_mutex_lock(&be->stats_mutex);
        ++be->stats.failed_payload;
        pthread_mutex_unlock(&be->stats_mutex);
        return A840_SC_COMPILE_FAILED;
    }

    const uint32_t *spv_words[2] = {NULL, NULL};
    size_t spv_bytes[2] = {0u, 0u};
    const char *entry_names[2] = {NULL, NULL};
    A840RecordBinding bindings[A840_RECORD_MAX_BINDINGS];
    uint32_t binding_count = 0u;
    uint32_t stage_count = (hdr.pipeline_kind == A840_SC_IR3_PIPELINE_COMPUTE) ? 1u : 2u;

    if (hdr.source_kind == A840_SC_IR3_SRC_SPIRV) {
        uint32_t *w0 = copy_aligned_spirv(stage0_raw, hdr.stage0_bytes);
        uint32_t *w1 = (stage_count == 2u) ? copy_aligned_spirv(stage1_raw, hdr.stage1_bytes) : NULL;
        if (!w0 || (stage_count == 2u && !w1)) {
            free(w0);
            free(w1);
            return A840_SC_OOM;
        }
        spv_words[0] = w0;
        spv_bytes[0] = hdr.stage0_bytes;
        entry_names[0] = hdr.entry_name;
        if (stage_count == 2u) {
            spv_words[1] = w1;
            spv_bytes[1] = hdr.stage1_bytes;
            entry_names[1] = hdr.fs_entry_name;
        }

        st = compile_stages_to_record(be, request, &hdr, spv_words, spv_bytes,
            entry_names, stage_count, NULL, 0u, out_record_blob, out_record_bytes);
        free(w0);
        free(w1);
    } else {
#ifdef A840_SC_IR3_ENABLE_TRANSLATE
        if (!be->nmb_dev) {
            pthread_mutex_lock(&be->stats_mutex);
            ++be->stats.failed_translate;
            pthread_mutex_unlock(&be->stats_mutex);
            return A840_SC_COMPILE_FAILED;
        }

        NmbLibrary *lib = nmb_library_parse(be->nmb_dev, stage0_raw, hdr.stage0_bytes);
        if (!lib) {
            pthread_mutex_lock(&be->stats_mutex);
            ++be->stats.failed_translate;
            pthread_mutex_unlock(&be->stats_mutex);
            return A840_SC_COMPILE_FAILED;
        }

        NmbSpirvResult *spv = NULL;
        if (hdr.pipeline_kind == A840_SC_IR3_PIPELINE_COMPUTE) {
            NmbComputePipelineDesc cdesc;
            memset(&cdesc, 0, sizeof(cdesc));
            cdesc.local_size[0] = hdr.local_size[0];
            cdesc.local_size[1] = hdr.local_size[1];
            cdesc.local_size[2] = hdr.local_size[2];
            cdesc.zeroed_threadgroup_memory = 1u;
            spv = nmb_compile_compute_spirv_ex(be->nmb_dev, lib, hdr.entry_name, &cdesc);
        } else {
            NmbRenderPipelineDesc rdesc;
            memset(&rdesc, 0, sizeof(rdesc));
            uint32_t max_rt = 0u;
            for (uint32_t slot = 0; slot < A840_SC_IR3_MAX_COLOR_RTS; ++slot) {
                if (hdr.color_mtl_formats[slot] != 0u) {
                    rdesc.color_mtl_formats[slot] = hdr.color_mtl_formats[slot];
                    max_rt = slot + 1u;
                }
            }
            if (max_rt > 0u) {
                rdesc.color_attachment_count = max_rt;
                rdesc.depth_mtl_format = hdr.depth_mtl_format;
                rdesc.stencil_mtl_format = hdr.stencil_mtl_format;
                rdesc.sample_count = hdr.sample_count;
                spv = nmb_compile_render_spirv_ex(be->nmb_dev, lib,
                    hdr.entry_name, hdr.fs_entry_name, &rdesc);
            } else {
                if (nmb_library_default_render_desc(be->nmb_dev, lib,
                        hdr.entry_name, hdr.fs_entry_name, &rdesc)) {
                    if (hdr.sample_count > 1u)
                        rdesc.sample_count = hdr.sample_count;
                    for (uint32_t slot = 0; slot < rdesc.color_attachment_count && slot < 8u; ++slot) {
                        uint32_t fmt = rdesc.color_mtl_formats[slot];
                        if (fmt != 0u)
                            hdr.color_rt_mask |= (1u << slot);
                        if (fmt == 25u || fmt == 65u || fmt == 115u)
                            hdr.half_rt_mask |= (1u << slot);
                    }
                    spv = nmb_compile_render_spirv_ex(be->nmb_dev, lib,
                        hdr.entry_name, hdr.fs_entry_name, &rdesc);
                }
            }
        }
        nmb_library_destroy(lib);

        if (!spv || nmb_spirv_stage_count(spv) != stage_count) {
            nmb_spirv_result_free(spv);
            pthread_mutex_lock(&be->stats_mutex);
            ++be->stats.failed_translate;
            pthread_mutex_unlock(&be->stats_mutex);
            return A840_SC_COMPILE_FAILED;
        }

        for (uint32_t i = 0; i < stage_count; ++i) {
            spv_words[i] = nmb_spirv_words(spv, i, &spv_bytes[i]);
            entry_names[i] = nmb_spirv_entry_point(spv, i);
            if (!spv_words[i] || spv_bytes[i] < 20u || !entry_names[i] || !entry_names[i][0]) {
                nmb_spirv_result_free(spv);
                pthread_mutex_lock(&be->stats_mutex);
                ++be->stats.failed_translate;
                pthread_mutex_unlock(&be->stats_mutex);
                return A840_SC_COMPILE_FAILED;
            }
        }

        size_t nbind = nmb_spirv_binding_count(spv);
        if (nbind > A840_RECORD_MAX_BINDINGS) {
            nmb_spirv_result_free(spv);
            pthread_mutex_lock(&be->stats_mutex);
            ++be->stats.failed_record;
            pthread_mutex_unlock(&be->stats_mutex);
            return A840_SC_COMPILE_FAILED;
        }
        binding_count = (uint32_t)nbind;
        for (uint32_t i = 0; i < binding_count; ++i) {
            NmbSpirvBinding b;
            if (!nmb_spirv_binding(spv, i, &b)) {
                nmb_spirv_result_free(spv);
                pthread_mutex_lock(&be->stats_mutex);
                ++be->stats.failed_translate;
                pthread_mutex_unlock(&be->stats_mutex);
                return A840_SC_COMPILE_FAILED;
            }
            bindings[i].descriptor_set = b.descriptor_set;
            bindings[i].binding = b.binding;
            bindings[i].descriptor_type = b.descriptor_type;
            bindings[i].descriptor_count = b.descriptor_count;
            bindings[i].stage_mask = b.stage_mask;
            bindings[i].buffer_index = b.buffer_index;
            bindings[i].texture_index = b.texture_index;
            bindings[i].sampler_index = b.sampler_index;
        }

        st = compile_stages_to_record(be, request, &hdr, spv_words, spv_bytes,
            entry_names, stage_count, bindings, binding_count,
            out_record_blob, out_record_bytes);
        nmb_spirv_result_free(spv);
#else
        (void)bindings;
        (void)binding_count;
        pthread_mutex_lock(&be->stats_mutex);
        ++be->stats.failed_translate;
        pthread_mutex_unlock(&be->stats_mutex);
        return A840_SC_COMPILE_FAILED;
#endif
    }

    if (st == A840_SC_OK) {
        pthread_mutex_lock(&be->stats_mutex);
        ++be->stats.succeeded;
        if (hdr.pipeline_kind == A840_SC_IR3_PIPELINE_COMPUTE)
            ++be->stats.compute_compiled;
        else
            ++be->stats.render_compiled;
        pthread_mutex_unlock(&be->stats_mutex);
    }
    return st;
}
