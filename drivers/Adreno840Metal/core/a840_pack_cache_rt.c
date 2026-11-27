/* SPDX-License-Identifier: MIT */
#include "a840_pack_cache_rt.h"

#include <limits.h>
#include <string.h>

enum {
    RT_PACK_VIEW_MAGIC = 827738960u /* "PKV1" LE decimal */
};

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64(const uint8_t *p)
{
    return (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4u) << 32);
}

static int32_t rd_i32(uint32_t v)
{
    return (v <= (uint32_t)INT32_MAX) ? (int32_t)v : -(int32_t)(UINT32_MAX - v) - 1;
}

static uint32_t align_up_u32(uint32_t v, uint32_t align)
{
    return (v + align - 1u) & ~(align - 1u);
}

static bool within_buf(size_t bytes, uint32_t off, uint64_t len)
{
    return (size_t)off <= bytes && len <= (uint64_t)(bytes - (size_t)off);
}

static bool all_zero(const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0u; i < n; ++i) {
        if (p[i] != 0u) {
            return false;
        }
    }
    return true;
}

static int cmp_bytes(const uint8_t *a, const uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0u; i < n; ++i) {
        if (a[i] != b[i]) {
            return (a[i] < b[i]) ? -1 : 1;
        }
    }
    return 0;
}

static uint64_t fnv1a_key_hash(const uint8_t key[32])
{
    uint64_t hash = UINT64_C(14695981039346656037);
    uint32_t i;
    for (i = 0u; i < 32u; ++i) {
        hash ^= (uint64_t)key[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

typedef struct RtSha256 {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
} RtSha256;

static uint32_t rotr32(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32u - n));
}

static void sha256_block(RtSha256 *s, const uint8_t *p)
{
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    uint32_t w[64], a, b, c, d, e, f, g, h;
    size_t i;
    for (i = 0u; i < 16u; ++i) {
        w[i] = ((uint32_t)p[i * 4u] << 24) | ((uint32_t)p[i * 4u + 1u] << 16) |
               ((uint32_t)p[i * 4u + 2u] << 8) | (uint32_t)p[i * 4u + 3u];
    }
    for (; i < 64u; ++i) {
        uint32_t x = w[i - 15u], y = w[i - 2u];
        w[i] = w[i - 16u] + (rotr32(x, 7u) ^ rotr32(x, 18u) ^ (x >> 3)) +
               w[i - 7u] + (rotr32(y, 17u) ^ rotr32(y, 19u) ^ (y >> 10));
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0u; i < 64u; ++i) {
        uint32_t t1 = h + (rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u)) +
                      ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u)) +
                      ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_init(RtSha256 *s)
{
    static const uint32_t iv[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    size_t i;
    for (i = 0u; i < 8u; ++i) {
        s->h[i] = iv[i];
    }
    s->bytes = 0u;
    s->used = 0u;
}

static void sha256_update(RtSha256 *s, const uint8_t *p, size_t n)
{
    s->bytes += (uint64_t)n;
    while (n > 0u) {
        size_t take = 64u - s->used;
        size_t i;
        if (take > n) {
            take = n;
        }
        for (i = 0u; i < take; ++i) {
            s->block[s->used + i] = p[i];
        }
        s->used += take;
        p += take;
        n -= take;
        if (s->used == 64u) {
            sha256_block(s, s->block);
            s->used = 0u;
        }
    }
}

static void sha256_finish(RtSha256 *s, uint8_t out[32])
{
    uint64_t bits = s->bytes * 8u;
    size_t i;
    s->block[s->used++] = 128u;
    if (s->used > 56u) {
        for (i = s->used; i < 64u; ++i) {
            s->block[i] = 0u;
        }
        sha256_block(s, s->block);
        s->used = 0u;
    }
    for (i = s->used; i < 56u; ++i) {
        s->block[i] = 0u;
    }
    for (i = 0u; i < 8u; ++i) {
        s->block[63u - i] = (uint8_t)(bits >> (i * 8u));
    }
    sha256_block(s, s->block);
    for (i = 0u; i < 32u; ++i) {
        out[i] = (uint8_t)(s->h[i / 4u] >> (24u - (i % 4u) * 8u));
    }
}

static void pack_file_digest(const uint8_t *p, size_t bytes, uint8_t digest[32])
{
    static const uint8_t zeros[32] = {0};
    RtSha256 sha;
    sha256_init(&sha);
    sha256_update(&sha, p, 32u);
    sha256_update(&sha, zeros, 32u);
    sha256_update(&sha, p + 64u, bytes - 64u);
    sha256_finish(&sha, digest);
}

static bool gpu_range_valid(uint64_t gpu_va_base, size_t bytes)
{
    const uint64_t va_limit = UINT64_C(1) << A840_RT_VA_BITS;
    if (gpu_va_base == 0u) {
        return true;
    }
    if ((gpu_va_base % A840_PACK_V1_ISA_ALIGNMENT) != 0u) {
        return false;
    }
    if (gpu_va_base >= va_limit || (uint64_t)bytes > va_limit - gpu_va_base) {
        return false;
    }
    return true;
}

static A840RtStatus open_pack_view_ro(const void *cpu_base,
                                      size_t bytes,
                                      uint64_t expected_stack,
                                      uint64_t expected_abi,
                                      A840PackV1View *out)
{
    const uint8_t *p = (const uint8_t *)cpu_base;
    uint32_t version, tier, hbb, log2_b, buckets, entries;
    uint32_t directory, payload, occupied = 0u, i;
    uint8_t digest[32];
    A840PackV1View v;

    if (!p || !out || bytes < A840_PACK_V1_HEADER_BYTES || bytes > UINT32_MAX ||
        ((uintptr_t)p % A840_PACK_V1_ISA_ALIGNMENT) != 0u) {
        return A840_RT_INVALID_ARG;
    }
    if (rd_u32(p) != A840_PACK_V1_MAGIC) {
        return A840_RT_UNSUPPORTED;
    }
    version = p[4] & 3u;
    tier = (p[4] >> 2) & 1u;
    hbb = p[4] >> 3;
    log2_b = p[5];
    if (version != A840_PACK_V1_VERSION || hbb != A840_PACK_V1_HBB) {
        return A840_RT_UNSUPPORTED;
    }
    if (rd_u64(p + 8u) != expected_abi || rd_u64(p + 16u) != expected_stack ||
        rd_u32(p + 24u) != A840_PACK_V1_CHIP_ID) {
        return A840_RT_UNSUPPORTED;
    }
    if (log2_b < 1u || log2_b > 16u) {
        return A840_RT_UNSUPPORTED;
    }
    buckets = UINT32_C(1) << log2_b;
    entries = rd_u16(p + 6u);
    if (entries > A840_PACK_V1_MAX_ENTRIES || entries > buckets / 2u) {
        return A840_RT_UNSUPPORTED;
    }
    directory = align_up_u32(64u + buckets * 4u, 128u);
    payload = align_up_u32(directory + entries * A840_PACK_V1_ENTRY_BYTES, 128u);
    if ((size_t)payload > bytes || rd_u32(p + 28u) != (uint32_t)(bytes - (size_t)payload)) {
        return A840_RT_RANGE_ERROR;
    }

    pack_file_digest(p, bytes, digest);
    if (cmp_bytes(digest, p + 32u, 32u) != 0) {
        return A840_RT_UNSUPPORTED;
    }
    if (!all_zero(p + 64u + (size_t)buckets * 4u, (size_t)(directory - 64u - buckets * 4u)) ||
        !all_zero(p + directory + (size_t)entries * A840_PACK_V1_ENTRY_BYTES,
                  (size_t)(payload - directory - entries * A840_PACK_V1_ENTRY_BYTES))) {
        return A840_RT_UNSUPPORTED;
    }

    for (i = 0u; i < entries; ++i) {
        const uint8_t *e = p + directory + (size_t)i * A840_PACK_V1_ENTRY_BYTES;
        uint32_t sc = rd_u32(e + 36u);
        uint32_t meta = rd_u32(e + 40u);
        uint32_t boff = rd_u32(e + 44u);
        uint32_t bn = rd_u32(e + 48u);
        uint32_t plan = rd_u32(e + 52u);
        uint32_t pbytes = rd_u32(e + 56u);
        uint32_t s;
        if (all_zero(e, 32u) || sc < 1u || sc > 2u || bn > A840_RECORD_MAX_BINDINGS ||
            (meta % 128u) != 0u || pbytes != sizeof(A840BindingPlan) ||
            !within_buf(bytes, meta, (uint64_t)sc * A840_PACK_V1_STAGE_BYTES) ||
            (bn > 0u && !within_buf(bytes, boff, (uint64_t)bn * sizeof(A840RecordBinding))) ||
            !within_buf(bytes, plan, pbytes)) {
            return A840_RT_UNSUPPORTED;
        }
        for (s = 0u; s < sc; ++s) {
            const uint8_t *st = p + meta + (size_t)s * A840_PACK_V1_STAGE_BYTES;
            uint32_t isa_dw = rd_u32(st + 8u);
            uint32_t cst_dw = rd_u32(st + 32u);
            uint32_t sds_dw = rd_u32(st + 145u * 4u);
            uint32_t isa_off = rd_u32(st + 149u * 4u);
            uint32_t cst_off = rd_u32(st + 150u * 4u);
            uint32_t sds_off = rd_u32(st + 151u * 4u);
            if (!isa_dw || (isa_off % A840_PACK_V1_ISA_ALIGNMENT) != 0u ||
                !within_buf(bytes, isa_off, (uint64_t)isa_dw * 4u) ||
                (cst_dw > 0u && ((cst_off % 4u) != 0u || !within_buf(bytes, cst_off, (uint64_t)cst_dw * 4u))) ||
                (sds_dw > 0u && ((sds_off % 4u) != 0u || !within_buf(bytes, sds_off, (uint64_t)sds_dw * 4u)))) {
                return A840_RT_UNSUPPORTED;
            }
        }
    }

    for (i = 0u; i < buckets; ++i) {
        uint32_t idx = rd_u32(p + 64u + (size_t)i * 4u);
        if (idx != A840_PACK_V1_EMPTY) {
            if (idx >= entries) {
                return A840_RT_UNSUPPORTED;
            }
            ++occupied;
        }
    }
    if (occupied != entries) {
        return A840_RT_UNSUPPORTED;
    }

    memset(&v, 0, sizeof(v));
    v.base = p;
    v.bytes = bytes;
    v.stack_key = expected_stack;
    v.abi_fingerprint = expected_abi;
    v.tier = tier;
    v.entry_count = entries;
    v.hash_bucket_count = buckets;
    v.directory_offset = directory;
    v.payload_offset = payload;
    v.valid = RT_PACK_VIEW_MAGIC;
    *out = v;
    return A840_RT_OK;
}

static void populate_entry_view(const A840PackV1View *v, uint32_t index, A840PackV1Entry *out)
{
    const uint8_t *e = v->base + v->directory_offset + (size_t)index * A840_PACK_V1_ENTRY_BYTES;
    uint32_t count = rd_u32(e + 36u);
    uint32_t meta = rd_u32(e + 40u);
    uint32_t i;

    memset(out, 0, sizeof(*out));
    out->pipeline_key = (const A840PipelineKey *)(const void *)e;
    out->flags = rd_u32(e + 32u);
    out->stage_count = count;
    out->binding_count = rd_u32(e + 48u);
    out->bindings = out->binding_count ? (const A840RecordBinding *)(const void *)(v->base + rd_u32(e + 44u)) : NULL;
    out->binding_plan = (const A840BindingPlan *)(const void *)(v->base + rd_u32(e + 52u));
    for (i = 0u; i < count && i < 2u; ++i) {
        const uint8_t *s = v->base + meta + (size_t)i * A840_PACK_V1_STAGE_BYTES;
        A840PackV1StageView *sv = &out->stages[i];
        sv->metadata = (const A840PackV1StageWire *)(const void *)s;
        sv->stage = rd_u32(s + 4u);
        sv->isa_dwords = rd_u32(s + 8u);
        sv->instruction_groups = rd_u32(s + 12u);
        sv->const_data_dwords = rd_u32(s + 32u);
        sv->flags_out = rd_u32(s + 56u);
        sv->sds_state_dwords = rd_u32(s + 145u * 4u);
        sv->isa_words = (const uint32_t *)(const void *)(v->base + rd_u32(s + 149u * 4u));
        sv->const_data_words = sv->const_data_dwords ?
            (const uint32_t *)(const void *)(v->base + rd_u32(s + 150u * 4u)) : NULL;
        sv->sds_state_words = sv->sds_state_dwords ?
            (const uint32_t *)(const void *)(v->base + rd_u32(s + 151u * 4u)) : NULL;
    }
}

static A840RtStatus lookup_view_o1(const A840PackV1View *v,
                                   const A840PipelineKey *key,
                                   A840PackV1Entry *out)
{
    uint32_t bucket, probe;
    if (!v || v->valid != RT_PACK_VIEW_MAGIC || !v->base || !key || !out ||
        all_zero(key->sha256, 32u)) {
        return A840_RT_INVALID_ARG;
    }
    bucket = (uint32_t)fnv1a_key_hash(key->sha256) & (v->hash_bucket_count - 1u);
    for (probe = 0u; probe < A840_PACK_V1_MAX_PROBES; ++probe) {
        uint32_t idx = rd_u32(v->base + 64u + (size_t)bucket * 4u);
        const uint8_t *stored;
        if (idx == A840_PACK_V1_EMPTY) {
            return A840_RT_FALLBACK_REQUIRED;
        }
        if (idx >= v->entry_count) {
            return A840_RT_UNSUPPORTED;
        }
        stored = v->base + v->directory_offset + (size_t)idx * A840_PACK_V1_ENTRY_BYTES;
        if (cmp_bytes(stored, key->sha256, 32u) == 0) {
            populate_entry_view(v, idx, out);
            return A840_RT_OK;
        }
        bucket = (bucket + 1u) & (v->hash_bucket_count - 1u);
    }
    return A840_RT_FALLBACK_REQUIRED;
}

static A840RtStatus mount_tier(A840PackCacheRt *cache,
                               A840PackTierMount *slot,
                               uint32_t expected_tier,
                               const void *cpu_base,
                               size_t bytes,
                               uint64_t gpu_va_base,
                               uint64_t bo_handle)
{
    A840PackV1View candidate;
    A840RtStatus st;

    if (!cache || !slot || !cpu_base || bytes == 0u ||
        cache->expected_stack_key == 0u || cache->expected_abi_fingerprint == 0u) {
        return A840_RT_INVALID_ARG;
    }
    if (!gpu_range_valid(gpu_va_base, bytes)) {
        return A840_RT_ALIGNMENT;
    }

    memset(&candidate, 0, sizeof(candidate));
    st = open_pack_view_ro(cpu_base, bytes,
                           cache->expected_stack_key,
                           cache->expected_abi_fingerprint,
                           &candidate);
    if (st != A840_RT_OK) {
        return st;
    }
    if (candidate.tier != expected_tier) {
        return A840_RT_INVALID_ARG;
    }

    slot->view = candidate;
    slot->gpu_va_base = gpu_va_base;
    slot->bo_handle = bo_handle;
    slot->mounted = true;
    return A840_RT_OK;
}

static A840RtStatus resolve_stage_gpu_addrs(const A840PackTierMount *mount,
                                            const A840PackV1StageView *stage,
                                            A840PackStageGpuAddrs *out)
{
    uintptr_t base_addr = (uintptr_t)mount->view.base;
    uintptr_t end_addr = base_addr + mount->view.bytes;

    memset(out, 0, sizeof(*out));
    out->bo_handle = mount->bo_handle;

    if (stage->isa_words && stage->isa_dwords > 0u) {
        uintptr_t p = (uintptr_t)stage->isa_words;
        size_t len = (size_t)stage->isa_dwords * sizeof(uint32_t);
        if (p < base_addr || p > end_addr || len > (size_t)(end_addr - p)) {
            return A840_RT_RANGE_ERROR;
        }
        out->isa_offset = (uint64_t)(p - base_addr);
        if ((out->isa_offset % A840_PACK_V1_ISA_ALIGNMENT) != 0u) {
            return A840_RT_ALIGNMENT;
        }
        out->isa_dwords = stage->isa_dwords;
        if (mount->gpu_va_base != 0u) {
            out->isa_gpu_va = mount->gpu_va_base + out->isa_offset;
        }
    }

    if (stage->const_data_words && stage->const_data_dwords > 0u) {
        uintptr_t p = (uintptr_t)stage->const_data_words;
        size_t len = (size_t)stage->const_data_dwords * sizeof(uint32_t);
        if (p < base_addr || p > end_addr || len > (size_t)(end_addr - p)) {
            return A840_RT_RANGE_ERROR;
        }
        out->const_offset = (uint64_t)(p - base_addr);
        out->const_dwords = stage->const_data_dwords;
        if (mount->gpu_va_base != 0u) {
            out->const_gpu_va = mount->gpu_va_base + out->const_offset;
        }
    }

    if (stage->sds_state_words && stage->sds_state_dwords > 0u) {
        uintptr_t p = (uintptr_t)stage->sds_state_words;
        size_t len = (size_t)stage->sds_state_dwords * sizeof(uint32_t);
        if (p < base_addr || p > end_addr || len > (size_t)(end_addr - p)) {
            return A840_RT_RANGE_ERROR;
        }
        out->sds_offset = (uint64_t)(p - base_addr);
        out->sds_dwords = stage->sds_state_dwords;
        if (mount->gpu_va_base != 0u) {
            out->sds_gpu_va = mount->gpu_va_base + out->sds_offset;
        }
    }

    return A840_RT_OK;
}

static A840RtStatus lookup_in_mount(const A840PackCacheRt *cache,
                                    const A840PackTierMount *mount,
                                    A840PackHitTier tier_tag,
                                    const A840PipelineKey *key,
                                    A840PackLookupResult *out)
{
    A840PackV1Entry entry;
    A840RtStatus st;
    uint32_t s;

    if (!mount->mounted) {
        return A840_RT_FALLBACK_REQUIRED;
    }
    if (mount->view.stack_key != cache->expected_stack_key ||
        mount->view.abi_fingerprint != cache->expected_abi_fingerprint) {
        return A840_RT_UNSUPPORTED;
    }

    st = lookup_view_o1(&mount->view, key, &entry);
    if (st != A840_RT_OK) {
        return st;
    }

    memset(out, 0, sizeof(*out));
    out->hit_tier = tier_tag;
    out->entry = entry;
    for (s = 0u; s < entry.stage_count && s < 2u; ++s) {
        st = resolve_stage_gpu_addrs(mount, &entry.stages[s], &out->stage_gpu[s]);
        if (st != A840_RT_OK) {
            memset(out, 0, sizeof(*out));
            return st;
        }
    }
    return A840_RT_OK;
}

A840RtStatus a840_pack_cache_rt_init(A840PackCacheRt *cache,
                                     uint64_t expected_stack_key,
                                     uint64_t expected_abi_fp)
{
    if (!cache || expected_stack_key == 0u || expected_abi_fp == 0u) {
        return A840_RT_INVALID_ARG;
    }
    memset(cache, 0, sizeof(*cache));
    cache->expected_stack_key = expected_stack_key;
    cache->expected_abi_fingerprint = expected_abi_fp;
    return A840_RT_OK;
}

void a840_pack_cache_rt_reset(A840PackCacheRt *cache)
{
    if (cache) {
        memset(cache, 0, sizeof(*cache));
    }
}

A840RtStatus a840_pack_cache_rt_mount_tier0(A840PackCacheRt *cache,
                                            const void *cpu_base,
                                            size_t bytes,
                                            uint64_t gpu_va_base,
                                            uint64_t bo_handle)
{
    if (!cache) {
        return A840_RT_INVALID_ARG;
    }
    return mount_tier(cache, &cache->tier0, 0u, cpu_base, bytes, gpu_va_base, bo_handle);
}

A840RtStatus a840_pack_cache_rt_mount_tier0_reply(
    A840PackCacheRt *cache,
    const struct NativeAdrenoAbiSharedPackReplyV3 *reply,
    uint64_t bo_handle)
{
    if (!cache || !reply || reply->cpu_va == 0u || reply->gpu_va == 0u ||
        reply->bytes == 0u || reply->bytes > SIZE_MAX) {
        return A840_RT_INVALID_ARG;
    }
    if (reply->actual_stack_key != cache->expected_stack_key) {
        return A840_RT_UNSUPPORTED;
    }
    return mount_tier(cache, &cache->tier0, 0u,
                      (const void *)(uintptr_t)reply->cpu_va,
                      (size_t)reply->bytes,
                      reply->gpu_va,
                      bo_handle);
}

A840RtStatus a840_pack_cache_rt_mount_tier1(A840PackCacheRt *cache,
                                            const void *cpu_base,
                                            size_t bytes,
                                            uint64_t gpu_va_base,
                                            uint64_t bo_handle)
{
    if (!cache) {
        return A840_RT_INVALID_ARG;
    }
    return mount_tier(cache, &cache->tier1, 1u, cpu_base, bytes, gpu_va_base, bo_handle);
}

void a840_pack_cache_rt_unmount_tier0(A840PackCacheRt *cache)
{
    if (cache) {
        memset(&cache->tier0, 0, sizeof(cache->tier0));
    }
}

void a840_pack_cache_rt_unmount_tier1(A840PackCacheRt *cache)
{
    if (cache) {
        memset(&cache->tier1, 0, sizeof(cache->tier1));
    }
}

A840RtStatus a840_pack_cache_rt_lookup(A840PackCacheRt *cache,
                                       const A840PipelineKey *key,
                                       A840PackLookupResult *out_result)
{
    A840RtStatus st;

    if (!cache || !key || !out_result) {
        return A840_RT_INVALID_ARG;
    }

    if (cache->tier0.mounted) {
        st = lookup_in_mount(cache, &cache->tier0, A840_PACK_HIT_TIER0, key, out_result);
        if (st == A840_RT_OK) {
            cache->stats.tier0_hits++;
            return A840_RT_OK;
        }
        if (st != A840_RT_FALLBACK_REQUIRED) {
            return st;
        }
    }

    if (cache->tier1.mounted) {
        st = lookup_in_mount(cache, &cache->tier1, A840_PACK_HIT_TIER1, key, out_result);
        if (st == A840_RT_OK) {
            cache->stats.tier1_hits++;
            return A840_RT_OK;
        }
        if (st != A840_RT_FALLBACK_REQUIRED) {
            return st;
        }
    }

    memset(out_result, 0, sizeof(*out_result));
    out_result->hit_tier = A840_PACK_HIT_NONE;
    cache->stats.misses++;
    return A840_RT_FALLBACK_REQUIRED;
}

A840RtStatus a840_pack_cache_rt_reconstruct_record(const A840PackCacheRt *cache,
                                                   const A840PackLookupResult *lookup,
                                                   A840PipelineRecord *out)
{
    const A840PackV1Entry *e;
    uint32_t i, j;

    if (!cache || !lookup || !out || lookup->hit_tier == A840_PACK_HIT_NONE) {
        return A840_RT_INVALID_ARG;
    }
    e = &lookup->entry;
    if (!e->pipeline_key || e->stage_count < 1u || e->stage_count > 2u ||
        e->binding_count > A840_RECORD_MAX_BINDINGS) {
        return A840_RT_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->abi_fingerprint = cache->expected_abi_fingerprint;
    out->stack_key = cache->expected_stack_key;
    out->chip_id = A840_PACK_V1_CHIP_ID;
    out->highest_bank_bit = A840_PACK_V1_HBB;
    out->flags = e->flags;
    out->stage_count = e->stage_count;
    out->binding_count = e->binding_count;
    memcpy(out->pipeline_key.sha256, e->pipeline_key->sha256, 32u);

    for (i = 0u; i < e->stage_count; ++i) {
        const uint8_t *p = (const uint8_t *)(const void *)e->stages[i].metadata;
        A840CompiledStage *s = &out->stages[i];
        if (!p) {
            return A840_RT_INVALID_ARG;
        }
        s->abi_version = rd_u32(p);
        s->stage = rd_u32(p + 4u);
        s->isa_dwords = rd_u32(p + 8u);
        s->instruction_groups = rd_u32(p + 12u);
        s->max_full_reg = rd_i32(rd_u32(p + 16u));
        s->max_half_reg = rd_i32(rd_u32(p + 20u));
        s->branchstack = rd_u32(p + 24u);
        s->constlen_vec4 = rd_u32(p + 28u);
        s->const_data_dwords = rd_u32(p + 32u);
        s->private_mem_bytes = rd_u32(p + 36u);
        s->shared_mem_bytes = rd_u32(p + 40u);
        for (j = 0u; j < 3u; ++j) {
            s->local_size[j] = rd_u32(p + (11u + j) * 4u);
        }
        s->flags_out = rd_u32(p + 56u);
        s->ubo_push_count = rd_u32(p + 60u);
        for (j = 0u; j < ADRENO_COMPILE_MAX_UBO_PUSH; ++j) {
            const uint8_t *b = p + (16u + j * 6u) * 4u;
            s->ubo_push[j].set_index = rd_u32(b);
            s->ubo_push[j].binding_index = rd_u32(b + 4u);
            s->ubo_push[j].src_offset_vec4 = rd_u32(b + 8u);
            s->ubo_push[j].dst_const_vec4 = rd_u32(b + 12u);
            s->ubo_push[j].size_vec4 = rd_u32(b + 16u);
            s->ubo_push[j].reserved = rd_u32(b + 20u);
        }
        s->vpc_varying_count = rd_u32(p + 112u * 4u);
        for (j = 0u; j < ADRENO_COMPILE_MAX_VARYINGS; ++j) {
            s->vpc_varyings[j] = rd_u32(p + (113u + j) * 4u);
        }
        s->sds_state_dwords = rd_u32(p + 145u * 4u);
        s->reg_spills = rd_u32(p + 146u * 4u);
        s->reg_fills = rd_u32(p + 147u * 4u);
        s->max_waves = rd_u32(p + 148u * 4u);
        s->isa_words = e->stages[i].isa_words;
        s->const_data_words = e->stages[i].const_data_words;
        s->sds_state_words = e->stages[i].sds_state_words;
    }
    for (i = 0u; i < e->binding_count; ++i) {
        out->bindings[i] = e->bindings[i];
    }
    return A840_RT_OK;
}

A840RtStatus a840_pack_cache_rt_make_shader_code(const A840PackLookupResult *lookup,
                                                 uint32_t stage_index,
                                                 const A840CompiledStage *stage_meta,
                                                 A840ShaderCode *out_code)
{
    const A840PackStageGpuAddrs *ga;
    if (!lookup || !stage_meta || !out_code ||
        lookup->hit_tier == A840_PACK_HIT_NONE ||
        stage_index >= lookup->entry.stage_count || stage_index >= 2u) {
        return A840_RT_INVALID_ARG;
    }
    ga = &lookup->stage_gpu[stage_index];
    if (ga->isa_gpu_va == 0u || ga->isa_dwords == 0u) {
        return A840_RT_STATE_ERROR;
    }
    if ((ga->isa_gpu_va % A840_PACK_V1_ISA_ALIGNMENT) != 0u) {
        return A840_RT_ALIGNMENT;
    }
    memset(out_code, 0, sizeof(*out_code));
    out_code->stage = stage_meta;
    out_code->code.bo_handle = ga->bo_handle;
    out_code->code.gpu_va = ga->isa_gpu_va;
    return A840_RT_OK;
}

A840RtStatus a840_pack_cache_rt_make_sds_group(const A840PackLookupResult *lookup,
                                               uint32_t stage_index,
                                               uint32_t group_id,
                                               uint32_t enable_mask,
                                               A840SdsGroupDesc *out_sds)
{
    const A840PackStageGpuAddrs *ga;
    if (!lookup || !out_sds ||
        lookup->hit_tier == A840_PACK_HIT_NONE ||
        stage_index >= lookup->entry.stage_count || stage_index >= 2u ||
        group_id >= ADRENO840_MAX_SDS_SLOTS) {
        return A840_RT_INVALID_ARG;
    }
    ga = &lookup->stage_gpu[stage_index];
    if (ga->sds_gpu_va == 0u || ga->sds_dwords == 0u) {
        return A840_RT_STATE_ERROR;
    }
    memset(out_sds, 0, sizeof(*out_sds));
    out_sds->group_id = group_id;
    out_sds->enable_mask = enable_mask;
    out_sds->state_dwords = ga->sds_dwords;
    out_sds->state.bo_handle = ga->bo_handle;
    out_sds->state.gpu_va = ga->sds_gpu_va;
    return A840_RT_OK;
}

A840PackCacheStats a840_pack_cache_rt_stats(const A840PackCacheRt *cache)
{
    A840PackCacheStats empty;
    if (!cache) {
        memset(&empty, 0, sizeof(empty));
        return empty;
    }
    return cache->stats;
}

void a840_pack_cache_rt_reset_stats(A840PackCacheRt *cache)
{
    if (cache) {
        memset(&cache->stats, 0, sizeof(cache->stats));
    }
}
