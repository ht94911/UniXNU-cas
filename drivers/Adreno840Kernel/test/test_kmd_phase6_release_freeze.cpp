/* SPDX-License-Identifier: MIT */
/* Phase 6 / Wave 11 / Lane 1: NativeAdrenoABI v3 Official Release Freeze,
 * Zero Legacy Turnip Policy, mac-ci-phase2 Backport Regression, and Full
 * KMD v3 Subsystem Integration Test Suite (60 cases + TSan 20x stress).
 */
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Adreno840EmitABI.h"
#include "Adreno840Regs.h"
#include "NativeAdrenoABI_v3.h"
#include "a840_abi_v3_host.h"
#include "chip_facts_a840.h"

#include "a840_kmd_bo_pool.h"
#include "a840_kmd_ca_env_policy.h"
#include "a840_kmd_display_handoff.h"
#include "a840_kmd_fault_capture.h"
#include "a840_kmd_gmu_dcvs.h"
#include "a840_kmd_ib_diag.h"
#include "a840_kmd_mdss_sync.h"
#include "a840_kmd_preempt_ring.h"
#include "a840_kmd_quota_recovery.h"
#include "a840_kmd_sparse_va.h"
#include "a840_kmd_ubwc_surface.h"
#include "a840_kmd_userclient_v3.h"
#include "a840_kmd_v3_freeze.h"
#include "a840_kmd_wait_any.h"
#include "a840_kmd_wakeup_latency.h"
#include "a840_kmd_writer_promise.h"
#include "a840_kmd_zero_page.h"
#include "xnu/NativeAdrenoV3Glue.h"

namespace {

std::atomic<uint64_t> g_checks{0};
std::atomic<uint64_t> g_failures{0};
uint32_t g_cases = 0;
uint32_t g_stress_rounds = 1;
const char *g_current_case = "init";

#define CHECK(expr)                                                           \
    do {                                                                      \
        g_checks.fetch_add(1u, std::memory_order_relaxed);                    \
        if (!(expr)) {                                                        \
            g_failures.fetch_add(1u, std::memory_order_relaxed);              \
            std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n",                     \
                         g_current_case, __FILE__, __LINE__, #expr);          \
        }                                                                     \
    } while (0)

void begin_case(const char *name)
{
    g_current_case = name;
    g_cases++;
}

uint32_t odd_parity_bit(uint32_t val)
{
    uint32_t x = val;
    x ^= x >> 16u;
    x ^= x >> 8u;
    x ^= x >> 4u;
    x ^= x >> 2u;
    x ^= x >> 1u;
    return (x & 1u) ^ 1u;
}

uint32_t make_type4_hdr(uint32_t base_reg, uint32_t count)
{
    const uint32_t p_reg = odd_parity_bit(base_reg & 0x3ffffu);
    const uint32_t p_cnt = odd_parity_bit(count & 0x7fu);
    return (UINT32_C(4) << 28u) |
           (p_reg << 27u) |
           ((base_reg & 0x3ffffu) << 8u) |
           (p_cnt << 7u) |
           (count & 0x7fu);
}

uint32_t make_type7_hdr(uint32_t opcode, uint32_t count)
{
    const uint32_t p_op = odd_parity_bit(opcode & 0x7fu);
    const uint32_t p_cnt = odd_parity_bit(count & 0x7fffu);
    return (UINT32_C(7) << 28u) |
           (p_op << 23u) |
           ((opcode & 0x7fu) << 16u) |
           (p_cnt << 15u) |
           (count & 0x7fffu);
}

A840KmdProdQualInput make_valid_prod_input()
{
    A840KmdProdQualInput in{};
    const A840ChipFacts *facts = a840_chip_facts_kaanapali_default();
    size_t gpu_cnt = 0u;
    size_t gmu_cnt = 0u;
    const A840GpuOppEntry *gpu_tbl = a840_gmu_dcvs_get_opp_table(&gpu_cnt);
    const A840GmuOppEntry *gmu_tbl = a840_gmu_dcvs_get_gmu_table(&gmu_cnt);

    in.abi_version = NativeAdrenoAbiVersionV3;
    in.envelope_bytes = NativeAdrenoAbiEnvelopeBytes;
    in.advertised_cap_flags = A840_KMD_V3_PROD_CAP_MASK;

    in.chip_id = facts->chip_id;
    in.gmem_bytes = facts->gmem_bytes;
    in.highest_bank_bit = facts->highest_bank_bit;
    in.ubwc_version = facts->ubwc_version;
    in.smmu_page_shift = facts->smmu_page_shift;

    in.zero_page_ready = 1u;
    in.zero_page_readonly = 1u;
    in.zero_page_gpu_va = UINT64_C(0x10000000);
    in.zero_page_phys_addr = UINT64_C(0x80000000);
    in.zero_page_bytes = A840_KMD_ZERO_PAGE_BYTES;

    in.ca_target_label = A840_KMD_CA_TARGET_BACKBOARDD;
    in.ca_env_entry = A840_KMD_CA_ENV_ENTRY;
    in.ca_native_metal_enabled = 1u;
    in.ca_pack_verified = 1u;
    in.ca_policy_enforced = 1u;

    in.preempt_ring_count = A840_RING_COUNT;
    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        in.preempt_records[r].record_va = UINT64_C(0x20000000) +
            static_cast<uint64_t>(r) * UINT64_C(0x40000);
        in.preempt_records[r].record_bytes = UINT64_C(0x20000);
    }

    in.gpu_opp_table = gpu_tbl;
    in.gpu_opp_count = gpu_cnt;
    in.gmu_opp_table = gmu_tbl;
    in.gmu_opp_count = gmu_cnt;
    in.thermal_ladder_active = 1u;
    in.thermal_tier_count = A840_KMD_V3_FREEZE_THERMAL_TIERS;
    in.thermal_min_throttle_mc = A840_DCVS_THERMAL_TIER0_MC;
    in.thermal_cap_75c_mhz = A840_KMD_V3_FREEZE_THERMAL_CAP_75C_MHZ;
    in.thermal_cap_95c_mhz = A840_KMD_V3_FREEZE_THERMAL_CAP_95C_MHZ;
    return in;
}

struct MockSmmuZeroPage {
    std::mutex mu;
    std::vector<uint8_t> backing;
    uint64_t phys_base = UINT64_C(0x90000000);
    uint32_t allocs = 0u, frees = 0u, maps = 0u, unmaps = 0u;

    static A840KmdStatus alloc_cb(void *cookie, uint64_t bytes,
                                  uint64_t *out_phys, void **out_cpu,
                                  void **out_handle)
    {
        auto *self = static_cast<MockSmmuZeroPage *>(cookie);
        std::lock_guard<std::mutex> lk(self->mu);
        self->backing.assign(static_cast<size_t>(bytes), 0xAAu);
        self->allocs++;
        *out_phys = self->phys_base;
        *out_cpu = self->backing.data();
        *out_handle = self->backing.data();
        return A840_KMD_OK;
    }

    static void free_cb(void *cookie, uint64_t, void *, uint64_t)
    {
        auto *self = static_cast<MockSmmuZeroPage *>(cookie);
        std::lock_guard<std::mutex> lk(self->mu);
        self->frees++;
        self->backing.clear();
    }

    static A840KmdStatus map_cb(void *cookie, uint64_t, void *, uint64_t,
                                uint64_t, uint64_t, uint32_t perms)
    {
        auto *self = static_cast<MockSmmuZeroPage *>(cookie);
        if (perms != A840_KMD_SMMU_PERM_READ_ONLY)
            return A840_KMD_PERMISSION_DENIED;
        std::lock_guard<std::mutex> lk(self->mu);
        self->maps++;
        return A840_KMD_OK;
    }

    static A840KmdStatus unmap_cb(void *cookie, uint64_t, void *, uint64_t, uint64_t)
    {
        auto *self = static_cast<MockSmmuZeroPage *>(cookie);
        std::lock_guard<std::mutex> lk(self->mu);
        self->unmaps++;
        return A840_KMD_OK;
    }

    A840KmdSmmuOps ops()
    {
        A840KmdSmmuOps o{};
        o.cookie = this;
        o.alloc_backing_page = &MockSmmuZeroPage::alloc_cb;
        o.free_backing_page = &MockSmmuZeroPage::free_cb;
        o.map_client_ro = &MockSmmuZeroPage::map_cb;
        o.unmap_client = &MockSmmuZeroPage::unmap_cb;
        return o;
    }
};

struct GlueMockClient {
    uint64_t id = 0u;
    uint32_t inject_legacy_v2_flags = 0u;
    uint64_t bo_bytes = UINT64_C(262144);
    std::vector<uint32_t> ib_words;
    std::atomic<uint64_t> fence_seq{0u};
};

struct GlueMockEnv {
    std::mutex mu;
    uint64_t faults_logged = 0u;
    std::string last_fault_line;
    uint64_t ib_rejects = 0u;

    static IOReturn v2_dispatch(void *, void *client_ptr, uint32_t selector,
                                const void *, size_t, void *out_buf,
                                size_t out_bytes, size_t *out_written)
    {
        auto *c = static_cast<GlueMockClient *>(client_ptr);
        if (selector == NativeAdrenoV2MethodQuery) {
            if (out_buf == nullptr || out_bytes != sizeof(NativeAdrenoV2ParamsWire))
                return kIOReturnBadArgument;
            NativeAdrenoV2ParamsWire p{};
            p.abi_version = NativeAdrenoAbiVersionV2;
            p.input_bits = 48u;
            p.output_bits = 36u;
            p.flags = static_cast<uint32_t>(NativeAdrenoParamsV2HostImport |
                                            NativeAdrenoParamsV2SubmitBatch) |
                      c->inject_legacy_v2_flags;
            p.max_bo_bytes = UINT64_C(1) << 30u;
            std::memcpy(out_buf, &p, sizeof(p));
            if (out_written != nullptr)
                *out_written = sizeof(p);
            return kIOReturnSuccess;
        }
        if (selector == NativeAdrenoV2MethodSubmit) {
            if (out_buf == nullptr || out_bytes != sizeof(NativeAdrenoV2SubmitReplyWire))
                return kIOReturnBadArgument;
            NativeAdrenoV2SubmitReplyWire rep{};
            rep.fence = c->fence_seq.fetch_add(1u, std::memory_order_relaxed) + 1u;
            std::memcpy(out_buf, &rep, sizeof(rep));
            if (out_written != nullptr)
                *out_written = sizeof(rep);
            return kIOReturnSuccess;
        }
        if (selector == NativeAdrenoV2MethodSubmitBatch) {
            if (out_buf == nullptr || out_bytes != sizeof(NativeAdrenoV2SubmitBatchReplyWire))
                return kIOReturnBadArgument;
            NativeAdrenoV2SubmitBatchReplyWire rep{};
            rep.batch = 1u;
            rep.fence = c->fence_seq.fetch_add(1u, std::memory_order_relaxed) + 1u;
            std::memcpy(out_buf, &rep, sizeof(rep));
            if (out_written != nullptr)
                *out_written = sizeof(rep);
            return kIOReturnSuccess;
        }
        if (out_written != nullptr)
            *out_written = 0u;
        return kIOReturnSuccess;
    }

    static A840KmdStatus bo_size(void *, void *client_ptr, uint64_t bo_handle,
                                 uint64_t *out_bytes)
    {
        auto *c = static_cast<GlueMockClient *>(client_ptr);
        if (c == nullptr || bo_handle == 0u || out_bytes == nullptr)
            return A840_KMD_INVALID_ARG;
        *out_bytes = c->bo_bytes;
        return A840_KMD_OK;
    }

    static IOReturn resolve_ib(void *, void *client_ptr, void *, uint64_t,
                               uint64_t, uint64_t, const uint32_t **out_words,
                               size_t *out_dwords)
    {
        auto *c = static_cast<GlueMockClient *>(client_ptr);
        if (c == nullptr || c->ib_words.empty())
            return kIOReturnBadArgument;
        *out_words = c->ib_words.data();
        *out_dwords = c->ib_words.size();
        return kIOReturnSuccess;
    }

    static void fault_cb(void *cookie, const char *line, size_t len,
                         const A840KmdFaultRecord *)
    {
        auto *env = static_cast<GlueMockEnv *>(cookie);
        std::lock_guard<std::mutex> lk(env->mu);
        env->faults_logged++;
        env->last_fault_line.assign(line, len);
    }

    static void ib_reject_cb(void *cookie, uint64_t, uint64_t,
                             const A840KmdIbDiagResult *, const char *)
    {
        auto *env = static_cast<GlueMockEnv *>(cookie);
        std::lock_guard<std::mutex> lk(env->mu);
        env->ib_rejects++;
    }

    NativeAdrenoV3Glue::Config make_config(bool audit_ib = true)
    {
        NativeAdrenoV3Glue::Config cfg{};
        cfg.cap_flags = 0u;
        cfg.zero_page_default_va = UINT64_C(0x10000000);
        cfg.audit_ib_on_submit = audit_ib;
        cfg.fault_log_cb = &GlueMockEnv::fault_cb;
        cfg.fault_log_cookie = this;
        cfg.ib_reject_cb = &GlueMockEnv::ib_reject_cb;
        cfg.ib_reject_cookie = this;
        cfg.host_ops.cookie = this;
        cfg.host_ops.v2_dispatch = &GlueMockEnv::v2_dispatch;
        cfg.host_ops.lookup_bo_size = &GlueMockEnv::bo_size;
        cfg.host_ops.resolve_ib_words = &GlueMockEnv::resolve_ib;
        return cfg;
    }
};

/* Group 1: Compile-Time & Runtime ABI v3 Official Freeze (Cases 1..10) */

void case01_abi_freeze_info_constants()
{
    begin_case("case01_abi_freeze_info_constants");
    A840KmdV3AbiFreezeInfo info{};
    a840_kmd_v3_get_abi_freeze_info(&info);
    CHECK(info.abi_version == 3u);
    CHECK(info.envelope_bytes == 3104u);
    CHECK(info.page_bytes == 4096u);
    CHECK(info.zero_page_bytes == 65536u);
    CHECK(info.prod_selector_min == 1u);
    CHECK(info.prod_selector_max == 26u);
    CHECK(info.gpu_opp_count == 18u);
    CHECK(info.gmu_opp_count == 4u);
    CHECK(info.ring_count == 4u);
    CHECK(info.ubwc_version == 6u);
    CHECK(info.highest_bank_bit == 16u);
    CHECK(info.gmem_bytes == 18u * 1024u * 1024u);
    CHECK(info.chip_id == A840_KAANAPALI_CHIP_ID);
    CHECK(info.prod_cap_mask == A840_KMD_V3_PROD_CAP_MASK);
    CHECK((info.required_prod_caps & ~info.prod_cap_mask) == 0u);
}

void case02_abi_struct_sizes_and_offsets()
{
    begin_case("case02_abi_struct_sizes_and_offsets");
    CHECK(sizeof(struct NativeAdrenoAbiWaitAnyV3) == 528u);
    CHECK(offsetof(struct NativeAdrenoAbiWaitAnyV3, queues) == 16u);
    CHECK(offsetof(struct NativeAdrenoAbiWaitAnyV3, fences) == 272u);
    CHECK(sizeof(struct NativeAdrenoAbiWaitAnyReplyV3) == 16u);
    CHECK(sizeof(struct NativeAdrenoAbiZeroPageReplyV3) == 16u);
    CHECK(sizeof(struct NativeAdrenoAbiSharedPackReqV3) == 16u);
    CHECK(sizeof(struct NativeAdrenoAbiSharedPackReplyV3) == 32u);
    CHECK(sizeof(struct NativeAdrenoAbiWriterPromiseV3) == 160u);
    CHECK(offsetof(struct NativeAdrenoAbiWriterPromiseV3, surface_ids) == 32u);
    CHECK(sizeof(struct NativeAdrenoAbiWriterPromiseReplyV3) == 16u);
    CHECK(sizeof(struct NativeAdrenoAbiSurfaceMetaV3) == 48u);
    CHECK(offsetof(struct NativeAdrenoAbiSurfaceMetaV3, meta_offset) == 32u);
    CHECK(sizeof(A840KmdSparseBindReqV3) == 1568u);
    CHECK(sizeof(A840KmdSparseBindReplyV3) == 1568u);
    CHECK(sizeof(A840ChipFacts) == 168u);
}

void case03_method_numbers_10_to_26_locked()
{
    begin_case("case03_method_numbers_10_to_26_locked");
    CHECK(A840_KMD_V3_METHOD_GET_METADATA == 10u);
    CHECK(A840_KMD_V3_METHOD_CREATE_QUEUE == 11u);
    CHECK(A840_KMD_V3_METHOD_CLOSE_QUEUE == 12u);
    CHECK(A840_KMD_V3_METHOD_BO_CPU_MAP == 13u);
    CHECK(A840_KMD_V3_METHOD_BO_CPU_UNMAP == 14u);
    CHECK(A840_KMD_V3_METHOD_GET_DEVICE_INFO == 15u);
    CHECK(A840_KMD_V3_METHOD_BO_IMPORT == 16u);
    CHECK(A840_KMD_V3_METHOD_SET_PRIORITY == 17u);
    CHECK(A840_KMD_V3_METHOD_NOTE == 18u);
    CHECK(A840_KMD_V3_METHOD_SUBMIT_BATCH == 19u);
    CHECK(NativeAdrenoMethodWaitAnyV3 == 20u);
    CHECK(NativeAdrenoMethodGetZeroPageV3 == 21u);
    CHECK(NativeAdrenoMethodMapSparseV3 == 22u);
    CHECK(NativeAdrenoMethodMapSharedPackV3 == 23u);
    CHECK(NativeAdrenoMethodWriterPromiseV3 == 24u);
    CHECK(NativeAdrenoMethodSurfaceMetaV3 == 25u);
    CHECK(NativeAdrenoMethodQueueConfigV3 == 26u);
}

void case04_capability_flags_disjoint()
{
    begin_case("case04_capability_flags_disjoint");
    CHECK((A840_KMD_V3_PROD_V2_CAP_MASK & A840_KMD_V3_PROD_V3_CAP_MASK) == 0u);
    CHECK((A840_KMD_V3_PROD_CAP_MASK & A840_KMD_LEGACY_FLAG_ALL_KNOWN_MASK) == 0u);
    CHECK((A840_KMD_V3_REQUIRED_PROD_CAPS & A840_KMD_V3_PROD_CAP_MASK) == A840_KMD_V3_REQUIRED_PROD_CAPS);
}

void case05_abi_freeze_info_null_safe()
{
    begin_case("case05_abi_freeze_info_null_safe");
    a840_kmd_v3_get_abi_freeze_info(nullptr);
    CHECK(true);
}

void case06_chip_facts_kaanapali_default_matches()
{
    begin_case("case06_chip_facts_kaanapali_default_matches");
    const A840ChipFacts *f = a840_chip_facts_kaanapali_default();
    char err[128] = {};
    CHECK(f != nullptr);
    CHECK(a840_chip_facts_validate(f, err, sizeof(err)) == 0);
    CHECK(f->chip_id == A840_KAANAPALI_CHIP_ID);
    CHECK(f->gmem_bytes == A840_KAANAPALI_GMEM_BYTES);
    CHECK(f->highest_bank_bit == A840_KAANAPALI_HIGHEST_BANK_BIT);
    CHECK(f->ubwc_version == 6u);
    CHECK(f->smmu_page_shift == 12u);
}

void case07_chip_facts_rejects_mesa_wildcard()
{
    begin_case("case07_chip_facts_rejects_mesa_wildcard");
    A840ChipFacts copy = *a840_chip_facts_kaanapali_default();
    CHECK(a840_chip_facts_apply_runtime(&copy, A840_MESA_CATALOG_CHIP_ID,
                                        A840_KAANAPALI_GMEM_BYTES, 16u) != 0);
    CHECK(copy.chip_id == A840_KAANAPALI_CHIP_ID);
}

void case08_selector_whitelist_accepts_1_to_26()
{
    begin_case("case08_selector_whitelist_accepts_1_to_26");
    for (uint32_t s = 1u; s <= 26u; ++s) {
        CHECK(a840_kmd_v3_selector_is_production(s) == 1);
    }
}

void case09_selector_whitelist_rejects_zero_and_legacy()
{
    begin_case("case09_selector_whitelist_rejects_zero_and_legacy");
    const uint32_t bad[] = {
        A840_KMD_LEGACY_SELECTOR_INVALID,
        A840_KMD_LEGACY_SELECTOR_TURNIP_DRM_SUBMIT,
        A840_KMD_LEGACY_SELECTOR_TURNIP_SYNCOBJ,
        A840_KMD_LEGACY_SELECTOR_TURNIP_TIMELINE,
        A840_KMD_LEGACY_SELECTOR_TURNIP_SHADOW_COPY,
        A840_KMD_LEGACY_SELECTOR_TURNIP_BRIDGE_CALL,
        A840_KMD_LEGACY_SELECTOR_TURNIP_WSI_SYNC,
        64u, 255u, 4096u, UINT32_MAX
    };
    for (uint32_t s : bad) {
        CHECK(a840_kmd_v3_selector_is_production(s) == 0);
    }
}

void case10_validate_production_selector_counter()
{
    begin_case("case10_validate_production_selector_counter");
    a840_kmd_v3_reset_legacy_rejected_count();
    CHECK(a840_kmd_v3_legacy_rejected_count() == 0u);
    for (uint32_t s = 1u; s <= 26u; ++s) {
        CHECK(a840_kmd_v3_validate_production_selector(s) == A840_KMD_OK);
    }
    CHECK(a840_kmd_v3_legacy_rejected_count() == 0u);
    CHECK(a840_kmd_v3_validate_production_selector(0u) == A840_KMD_UNSUPPORTED);
    CHECK(a840_kmd_v3_validate_production_selector(A840_KMD_LEGACY_SELECTOR_TURNIP_DRM_SUBMIT) == A840_KMD_UNSUPPORTED);
    CHECK(a840_kmd_v3_validate_production_selector(A840_KMD_LEGACY_SELECTOR_TURNIP_WSI_SYNC) == A840_KMD_UNSUPPORTED);
    CHECK(a840_kmd_v3_legacy_rejected_count() == 3u);
}

/* Group 2: Zero Legacy Turnip Policy Fail-Closed Gates (Cases 11..18) */

void case11_flags_accepts_production_caps_only()
{
    begin_case("case11_flags_accepts_production_caps_only");
    a840_kmd_v3_reset_legacy_rejected_count();
    CHECK(a840_kmd_v3_reject_legacy_turnip_flags(0u) == A840_KMD_OK);
    CHECK(a840_kmd_v3_reject_legacy_turnip_flags(A840_KMD_V3_PROD_V2_CAP_MASK) == A840_KMD_OK);
    CHECK(a840_kmd_v3_reject_legacy_turnip_flags(A840_KMD_V3_PROD_V3_CAP_MASK) == A840_KMD_OK);
    CHECK(a840_kmd_v3_reject_legacy_turnip_flags(A840_KMD_V3_PROD_CAP_MASK) == A840_KMD_OK);
    CHECK(a840_kmd_v3_legacy_rejected_count() == 0u);
}

void case12_flags_rejects_all_explicit_legacy_turnip_bits()
{
    begin_case("case12_flags_rejects_all_explicit_legacy_turnip_bits");
    a840_kmd_v3_reset_legacy_rejected_count();
    const uint64_t legacy[] = {
        A840_KMD_LEGACY_FLAG_TURNIP_SHIM,
        A840_KMD_LEGACY_FLAG_TURNIP_SHADOW_BLIT,
        A840_KMD_LEGACY_FLAG_TURNIP_WSI_BRIDGE,
        A840_KMD_LEGACY_FLAG_TURNIP_FALLBACK_COMP,
        A840_KMD_LEGACY_FLAG_TURNIP_SYNCOBJ_EMU,
        A840_KMD_LEGACY_FLAG_TURNIP_COMPAT_BRIDGE
    };
    uint64_t expected = 0u;
    for (uint64_t f : legacy) {
        CHECK(a840_kmd_v3_reject_legacy_turnip_flags(f) == A840_KMD_UNSUPPORTED);
        expected++;
        CHECK(a840_kmd_v3_legacy_rejected_count() == expected);
    }
}

void case13_flags_exhaustive_64bit_fail_closed_sweep()
{
    begin_case("case13_flags_exhaustive_64bit_fail_closed_sweep");
    a840_kmd_v3_reset_legacy_rejected_count();
    uint64_t rejected = 0u;
    for (unsigned bit = 0u; bit < 64u; ++bit) {
        const uint64_t mask = UINT64_C(1) << bit;
        const bool is_prod = (mask & A840_KMD_V3_PROD_CAP_MASK) != 0u;
        const A840KmdStatus st = a840_kmd_v3_reject_legacy_turnip_flags(mask);
        if (is_prod) {
            CHECK(st == A840_KMD_OK);
        } else {
            CHECK(st == A840_KMD_UNSUPPORTED);
            rejected++;
        }
    }
    CHECK(rejected == 53u);
    CHECK(a840_kmd_v3_legacy_rejected_count() == 53u);
}

void case14_flags_mixed_prod_and_legacy_rejected()
{
    begin_case("case14_flags_mixed_prod_and_legacy_rejected");
    a840_kmd_v3_reset_legacy_rejected_count();
    const uint64_t mixed = A840_KMD_V3_PROD_CAP_MASK | A840_KMD_LEGACY_FLAG_TURNIP_SHIM;
    CHECK(a840_kmd_v3_reject_legacy_turnip_flags(mixed) == A840_KMD_UNSUPPORTED);
    CHECK(a840_kmd_v3_legacy_rejected_count() == 1u);
}

void case15_policy_instance_lifecycle_and_custom_sync()
{
    begin_case("case15_policy_instance_lifecycle_and_custom_sync");
    struct CustomLock {
        std::mutex mu;
        uint64_t locks = 0u, unlocks = 0u;
        static void lock_cb(void *c) { auto *l = static_cast<CustomLock *>(c); l->mu.lock(); l->locks++; }
        static void unlock_cb(void *c) { auto *l = static_cast<CustomLock *>(c); l->unlocks++; l->mu.unlock(); }
    } cl;

    A840KmdV3FreezePolicy policy{};
    A840KmdV3FreezeStats stats{};
    CHECK(a840_kmd_v3_freeze_policy_init(nullptr, nullptr) == A840_KMD_INVALID_ARG);
    A840KmdFreezeSyncOps bad_ops{&cl, &CustomLock::lock_cb, nullptr};
    CHECK(a840_kmd_v3_freeze_policy_init(&policy, &bad_ops) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_v3_freeze_policy_get_stats(&policy, &stats) == A840_KMD_INVALID_ARG);

    A840KmdFreezeSyncOps good_ops{&cl, &CustomLock::lock_cb, &CustomLock::unlock_cb};
    CHECK(a840_kmd_v3_freeze_policy_init(&policy, &good_ops) == A840_KMD_OK);
    CHECK(a840_kmd_v3_freeze_policy_check_selector(&policy, 20u) == A840_KMD_OK);
    CHECK(a840_kmd_v3_freeze_policy_get_stats(&policy, &stats) == A840_KMD_OK);
    CHECK(stats.selector_checks_passed == 1u);
    CHECK(cl.locks >= 2u && cl.locks == cl.unlocks);
    a840_kmd_v3_freeze_policy_destroy(&policy);
    CHECK(a840_kmd_v3_freeze_policy_get_stats(&policy, &stats) == A840_KMD_INVALID_ARG);
}

void case16_policy_instance_counters_accurate()
{
    begin_case("case16_policy_instance_counters_accurate");
    A840KmdV3FreezePolicy policy{};
    CHECK(a840_kmd_v3_freeze_policy_init(&policy, nullptr) == A840_KMD_OK);
    CHECK(a840_kmd_v3_freeze_policy_check_selector(&policy, 1u) == A840_KMD_OK);
    CHECK(a840_kmd_v3_freeze_policy_check_selector(&policy, 24u) == A840_KMD_OK);
    CHECK(a840_kmd_v3_freeze_policy_check_selector(&policy, 27u) == A840_KMD_UNSUPPORTED);
    CHECK(a840_kmd_v3_freeze_policy_check_selector(&policy, 0u) == A840_KMD_UNSUPPORTED);
    CHECK(a840_kmd_v3_freeze_policy_check_flags(&policy, A840_KMD_V3_PROD_CAP_MASK) == A840_KMD_OK);
    CHECK(a840_kmd_v3_freeze_policy_check_flags(&policy, A840_KMD_LEGACY_FLAG_TURNIP_WSI_BRIDGE) == A840_KMD_UNSUPPORTED);

    A840KmdV3FreezeStats stats{};
    CHECK(a840_kmd_v3_freeze_policy_get_stats(&policy, &stats) == A840_KMD_OK);
    CHECK(stats.selector_checks_passed == 2u && stats.legacy_selector_rejects == 2u);
    CHECK(stats.flag_checks_passed == 1u && stats.legacy_flag_rejects == 1u);
    CHECK(stats.legacy_rejected_count == 3u);
    a840_kmd_v3_freeze_policy_destroy(&policy);
}

void case17_glue_augment_query_strips_legacy_turnip_bits()
{
    begin_case("case17_glue_augment_query_strips_legacy_turnip_bits");
    GlueMockEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);

    GlueMockClient c{};
    c.id = 1u;
    c.inject_legacy_v2_flags = static_cast<uint32_t>(
        A840_KMD_LEGACY_FLAG_TURNIP_SHIM |
        A840_KMD_LEGACY_FLAG_TURNIP_SHADOW_BLIT |
        A840_KMD_LEGACY_FLAG_TURNIP_COMPAT_BRIDGE);
    CHECK(glue.openClient(&c, nullptr, nullptr) == kIOReturnSuccess);

    NativeAdrenoV2ParamsWire params{};
    size_t written = 0u;
    CHECK(glue.externalMethod(&c, NativeAdrenoV2MethodQuery, nullptr, 0u,
                              &params, sizeof(params), &written) == kIOReturnSuccess);
    CHECK(written == sizeof(params) && params.abi_version == NativeAdrenoAbiVersionV3);
    CHECK((params.flags & static_cast<uint32_t>(A840_KMD_LEGACY_FLAG_TURNIP_SHIM)) == 0u);
    CHECK((params.flags & static_cast<uint32_t>(A840_KMD_LEGACY_FLAG_TURNIP_SHADOW_BLIT)) == 0u);
    CHECK((params.flags & static_cast<uint32_t>(A840_KMD_LEGACY_FLAG_TURNIP_COMPAT_BRIDGE)) == 0u);
    CHECK(a840_kmd_v3_reject_legacy_turnip_flags(params.flags) == A840_KMD_OK);
    CHECK((params.flags & NativeAdrenoParamsV2HostImport) != 0u);
    CHECK((params.flags & NativeAdrenoParamsV3TimelineWaitAny) != 0u);
}

void case18_glue_rejects_all_legacy_turnip_selectors()
{
    begin_case("case18_glue_rejects_all_legacy_turnip_selectors");
    GlueMockEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);
    GlueMockClient c{};
    CHECK(glue.openClient(&c, nullptr, nullptr) == kIOReturnSuccess);

    const uint32_t legacy_sels[] = {
        A840_KMD_LEGACY_SELECTOR_INVALID,
        A840_KMD_LEGACY_SELECTOR_TURNIP_DRM_SUBMIT,
        A840_KMD_LEGACY_SELECTOR_TURNIP_SYNCOBJ,
        A840_KMD_LEGACY_SELECTOR_TURNIP_TIMELINE,
        A840_KMD_LEGACY_SELECTOR_TURNIP_SHADOW_COPY,
        A840_KMD_LEGACY_SELECTOR_TURNIP_BRIDGE_CALL,
        A840_KMD_LEGACY_SELECTOR_TURNIP_WSI_SYNC,
        UINT32_MAX
    };
    uint8_t buf[16] = {};
    for (uint32_t sel : legacy_sels) {
        CHECK(glue.externalMethod(&c, sel, buf, sizeof(buf), buf, sizeof(buf)) == kIOReturnUnsupported);
    }
}

/* Group 3: Unified Production Qualification Positive & Negative (Cases 19..37) */

void case19_qualify_production_all_pass()
{
    begin_case("case19_qualify_production_all_pass");
    a840_kmd_v3_reset_legacy_rejected_count();
    A840KmdProdQualInput in = make_valid_prod_input();
    const uint32_t audit_sels[] = {1u, 5u, 10u, 15u, 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u};
    in.selector_audit_list = audit_sels;
    in.selector_audit_count = sizeof(audit_sels) / sizeof(audit_sels[0]);

    A840KmdProdQualReport rep{};
    CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_OK);
    CHECK(rep.qualified == 1u && rep.passed_mask == A840_KMD_QUAL_ALL_MASK && rep.failed_mask == 0u);
    CHECK(rep.abi_version_frozen == 1u && rep.turnip_paths_purged == 1u && rep.legacy_rejected_count == 0u);
}

void case20_qualify_null_and_reserved_fail_closed()
{
    begin_case("case20_qualify_null_and_reserved_fail_closed");
    A840KmdProdQualInput in = make_valid_prod_input();
    A840KmdProdQualReport rep{};
    CHECK(a840_kmd_v3_qualify_production(nullptr, &rep) == A840_KMD_INVALID_ARG);
    CHECK(rep.qualified == 0u && rep.failed_mask == A840_KMD_QUAL_ALL_MASK);
    CHECK(a840_kmd_v3_qualify_production(&in, nullptr) == A840_KMD_INVALID_ARG);
    in.reserved = 1u;
    CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_INVALID_ARG);
    in = make_valid_prod_input();
    in.reserved_ca = 1u;
    CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_INVALID_ARG);
}

void case21_qualify_rejects_wrong_abi_version_and_envelope()
{
    begin_case("case21_qualify_rejects_wrong_abi_version_and_envelope");
    A840KmdProdQualInput in = make_valid_prod_input();
    A840KmdProdQualReport rep{};
    in.abi_version = NativeAdrenoAbiVersionV2;
    CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT);
    CHECK(rep.qualified == 0u && rep.abi_version_frozen == 0u && rep.failed_mask == A840_KMD_QUAL_ABI_V3_FROZEN);
    in = make_valid_prod_input();
    in.envelope_bytes = 4096u;
    CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT);
    CHECK(rep.failed_mask == A840_KMD_QUAL_ABI_V3_FROZEN);
}

void case22_qualify_rejects_missing_required_v3_cap()
{
    begin_case("case22_qualify_rejects_missing_required_v3_cap");
    A840KmdProdQualInput in = make_valid_prod_input();
    A840KmdProdQualReport rep{};
    in.advertised_cap_flags &= ~static_cast<uint64_t>(NativeAdrenoParamsV3PreemptRings);
    CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT);
    CHECK(rep.failed_mask == A840_KMD_QUAL_ABI_V3_FROZEN);
    in = make_valid_prod_input();
    in.advertised_cap_flags &= ~static_cast<uint64_t>(NativeAdrenoParamsV3SparseVmBind);
    CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT);
    CHECK(rep.failed_mask == A840_KMD_QUAL_ABI_V3_FROZEN);
}

void case23_qualify_rejects_each_chip_identity_mismatch()
{
    begin_case("case23_qualify_rejects_each_chip_identity_mismatch");
    A840KmdProdQualReport rep{};
    { A840KmdProdQualInput in = make_valid_prod_input(); in.chip_id = A840_MESA_CATALOG_CHIP_ID;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_CHIP_IDENTITY); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.gmem_bytes = 16u * 1024u * 1024u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_CHIP_IDENTITY); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.highest_bank_bit = A840_MESA_HIGHEST_BANK_BIT;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_CHIP_IDENTITY); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.ubwc_version = 5u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_CHIP_IDENTITY); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.smmu_page_shift = 14u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_CHIP_IDENTITY); }
}

void case24_qualify_rejects_unready_or_writable_zero_page()
{
    begin_case("case24_qualify_rejects_unready_or_writable_zero_page");
    A840KmdProdQualReport rep{};
    A840KmdProdQualInput in = make_valid_prod_input();
    in.zero_page_ready = 0u;
    CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_ZERO_PAGE_READY);
    in = make_valid_prod_input();
    in.zero_page_readonly = 0u;
    CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_ZERO_PAGE_READY);
}

void case25_qualify_rejects_bad_zero_page_geometry()
{
    begin_case("case25_qualify_rejects_bad_zero_page_geometry");
    A840KmdProdQualReport rep{};
    { A840KmdProdQualInput in = make_valid_prod_input(); in.zero_page_gpu_va += 4096u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_ZERO_PAGE_READY); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.zero_page_bytes = 4096u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_ZERO_PAGE_READY); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.zero_page_gpu_va = A840_KMD_GPU_VA_LIMIT;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_ZERO_PAGE_READY); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.zero_page_phys_addr = 0u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_ZERO_PAGE_READY); }
}

void case26_qualify_rejects_backboardd_ca_policy_gaps()
{
    begin_case("case26_qualify_rejects_backboardd_ca_policy_gaps");
    A840KmdProdQualReport rep{};
    { A840KmdProdQualInput in = make_valid_prod_input(); in.ca_target_label = "com.apple.SpringBoard";
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_BACKBOARDD_CA_POLICY); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.ca_env_entry = "CA_DISABLE_GENERIC_SHADERS=0";
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_BACKBOARDD_CA_POLICY); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.ca_native_metal_enabled = 0u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_BACKBOARDD_CA_POLICY); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.ca_pack_verified = 0u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_BACKBOARDD_CA_POLICY); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.ca_policy_enforced = 0u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_BACKBOARDD_CA_POLICY); }
}

void case27_qualify_rejects_bad_preempt_ring_count_or_alignment()
{
    begin_case("case27_qualify_rejects_bad_preempt_ring_count_or_alignment");
    A840KmdProdQualReport rep{};
    { A840KmdProdQualInput in = make_valid_prod_input(); in.preempt_ring_count = 3u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_PREEMPT_4RINGS); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.preempt_records[2].record_va += 512u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_PREEMPT_4RINGS); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.preempt_records[1].record_bytes = 32768u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_PREEMPT_4RINGS); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.preempt_records[3].record_va = UINT64_C(1) << 48u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_PREEMPT_4RINGS); }
}

void case28_qualify_rejects_overlapping_preempt_records_and_allows_adjacent()
{
    begin_case("case28_qualify_rejects_overlapping_preempt_records_and_allows_adjacent");
    A840KmdProdQualReport rep{};
    {
        A840KmdProdQualInput in = make_valid_prod_input();
        for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
            in.preempt_records[r].record_va = UINT64_C(0x30000000) + static_cast<uint64_t>(r) * UINT64_C(65536);
            in.preempt_records[r].record_bytes = UINT64_C(65536);
        }
        CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_OK);
    }
    {
        A840KmdProdQualInput in = make_valid_prod_input();
        in.preempt_records[2].record_va =
            in.preempt_records[1].record_va + in.preempt_records[1].record_bytes - 4096u;
        CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_PREEMPT_4RINGS);
    }
}

void case29_qualify_rejects_corrupted_gpu_opp_table()
{
    begin_case("case29_qualify_rejects_corrupted_gpu_opp_table");
    A840KmdProdQualReport rep{};
    size_t cnt = 0u;
    const A840GpuOppEntry *orig = a840_gmu_dcvs_get_opp_table(&cnt);
    std::vector<A840GpuOppEntry> bad(orig, orig + cnt);

    bad[10].freq_mhz = bad[9].freq_mhz;
    bad[10].freq_hz = static_cast<uint64_t>(bad[10].freq_mhz) * UINT64_C(1000000);
    { A840KmdProdQualInput in = make_valid_prod_input(); in.gpu_opp_table = bad.data();
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_OPP_TABLES_AND_THERMAL); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.gpu_opp_count = 17u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_OPP_TABLES_AND_THERMAL); }
    bad.assign(orig, orig + cnt);
    bad[17].freq_mhz = 1150u;
    bad[17].freq_hz = UINT64_C(1150000000);
    { A840KmdProdQualInput in = make_valid_prod_input(); in.gpu_opp_table = bad.data();
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_OPP_TABLES_AND_THERMAL); }
}

void case30_qualify_rejects_corrupted_gmu_opp_table()
{
    begin_case("case30_qualify_rejects_corrupted_gmu_opp_table");
    A840KmdProdQualReport rep{};
    size_t cnt = 0u;
    const A840GmuOppEntry *orig = a840_gmu_dcvs_get_gmu_table(&cnt);
    std::vector<A840GmuOppEntry> bad(orig, orig + cnt);
    bad[2].hfi_voltage_level = bad[1].hfi_voltage_level - 1u;
    { A840KmdProdQualInput in = make_valid_prod_input(); in.gmu_opp_table = bad.data();
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_OPP_TABLES_AND_THERMAL); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.gmu_opp_count = 3u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_OPP_TABLES_AND_THERMAL); }
}

void case31_qualify_rejects_inactive_or_wrong_thermal_ladder()
{
    begin_case("case31_qualify_rejects_inactive_or_wrong_thermal_ladder");
    A840KmdProdQualReport rep{};
    { A840KmdProdQualInput in = make_valid_prod_input(); in.thermal_ladder_active = 0u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_OPP_TABLES_AND_THERMAL); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.thermal_min_throttle_mc = 80000;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_OPP_TABLES_AND_THERMAL); }
    { A840KmdProdQualInput in = make_valid_prod_input(); in.thermal_cap_75c_mhz = 1200u;
      CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT && rep.failed_mask == A840_KMD_QUAL_OPP_TABLES_AND_THERMAL); }
}

void case32_qualify_rejects_legacy_turnip_shim_flags()
{
    begin_case("case32_qualify_rejects_legacy_turnip_shim_flags");
    a840_kmd_v3_reset_legacy_rejected_count();
    A840KmdProdQualInput in = make_valid_prod_input();
    in.legacy_turnip_shim_flags = A840_KMD_LEGACY_FLAG_TURNIP_SHIM;
    A840KmdProdQualReport rep{};
    CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_UNSUPPORTED);
    CHECK(rep.qualified == 0u && rep.turnip_paths_purged == 0u);
    CHECK(rep.failed_mask == A840_KMD_QUAL_ZERO_LEGACY_TURNIP && rep.legacy_rejected_count == 1u);
}

void case33_qualify_rejects_legacy_turnip_paths_present()
{
    begin_case("case33_qualify_rejects_legacy_turnip_paths_present");
    a840_kmd_v3_reset_legacy_rejected_count();
    A840KmdProdQualInput in = make_valid_prod_input();
    in.legacy_turnip_paths_present = 1u;
    A840KmdProdQualReport rep{};
    CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_UNSUPPORTED);
    CHECK(rep.turnip_paths_purged == 0u && rep.failed_mask == A840_KMD_QUAL_ZERO_LEGACY_TURNIP && rep.legacy_rejected_count == 1u);
}

void case34_qualify_rejects_legacy_bit_in_advertised_caps()
{
    begin_case("case34_qualify_rejects_legacy_bit_in_advertised_caps");
    a840_kmd_v3_reset_legacy_rejected_count();
    A840KmdProdQualInput in = make_valid_prod_input();
    in.advertised_cap_flags |= A840_KMD_LEGACY_FLAG_TURNIP_COMPAT_BRIDGE;
    A840KmdProdQualReport rep{};
    CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_UNSUPPORTED);
    CHECK(rep.abi_version_frozen == 0u && rep.turnip_paths_purged == 0u);
    CHECK(rep.failed_mask == (A840_KMD_QUAL_ABI_V3_FROZEN | A840_KMD_QUAL_ZERO_LEGACY_TURNIP));
    CHECK(rep.legacy_rejected_count == 1u);
}

void case35_qualify_rejects_legacy_selector_in_audit_list()
{
    begin_case("case35_qualify_rejects_legacy_selector_in_audit_list");
    a840_kmd_v3_reset_legacy_rejected_count();
    const uint32_t bad_list[] = {1u, 20u, 21u, A840_KMD_LEGACY_SELECTOR_TURNIP_SYNCOBJ};
    A840KmdProdQualInput in = make_valid_prod_input();
    in.selector_audit_list = bad_list;
    in.selector_audit_count = 4u;
    A840KmdProdQualReport rep{};
    CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_UNSUPPORTED);
    CHECK(rep.failed_mask == A840_KMD_QUAL_ZERO_LEGACY_TURNIP && rep.legacy_rejected_count == 1u);
    in.selector_audit_list = nullptr;
    in.selector_audit_count = 1u;
    CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_UNSUPPORTED && rep.legacy_rejected_count == 2u);
}

void case36_qualify_multi_domain_orthogonal_bitmasks()
{
    begin_case("case36_qualify_multi_domain_orthogonal_bitmasks");
    A840KmdProdQualInput in = make_valid_prod_input();
    in.zero_page_ready = 0u;
    in.ca_policy_enforced = 0u;
    in.thermal_ladder_active = 0u;
    A840KmdProdQualReport rep{};
    CHECK(a840_kmd_v3_qualify_production(&in, &rep) == A840_KMD_STATE_CONFLICT);
    const uint32_t expected_fail = A840_KMD_QUAL_ZERO_PAGE_READY |
                                   A840_KMD_QUAL_BACKBOARDD_CA_POLICY |
                                   A840_KMD_QUAL_OPP_TABLES_AND_THERMAL;
    CHECK(rep.failed_mask == expected_fail);
    CHECK((rep.passed_mask & rep.failed_mask) == 0u);
    CHECK((rep.passed_mask | rep.failed_mask) == A840_KMD_QUAL_ALL_MASK);
    CHECK(rep.abi_version_frozen == 1u && rep.turnip_paths_purged == 1u);
}

void case37_policy_qualify_tracks_pass_and_fail_counts()
{
    begin_case("case37_policy_qualify_tracks_pass_and_fail_counts");
    A840KmdV3FreezePolicy policy{};
    CHECK(a840_kmd_v3_freeze_policy_init(&policy, nullptr) == A840_KMD_OK);
    A840KmdProdQualInput good = make_valid_prod_input();
    A840KmdProdQualInput bad = make_valid_prod_input();
    bad.legacy_turnip_shim_flags = A840_KMD_LEGACY_FLAG_TURNIP_FALLBACK_COMP;
    A840KmdProdQualReport rep{};
    CHECK(a840_kmd_v3_freeze_policy_qualify(&policy, &good, &rep) == A840_KMD_OK);
    CHECK(a840_kmd_v3_freeze_policy_qualify(&policy, &bad, &rep) == A840_KMD_UNSUPPORTED);
    A840KmdV3FreezeStats stats{};
    CHECK(a840_kmd_v3_freeze_policy_get_stats(&policy, &stats) == A840_KMD_OK);
    CHECK(stats.qualify_pass_count == 1u && stats.qualify_fail_count == 1u);
    CHECK(stats.legacy_rejected_count == 1u && stats.last_failed_mask == A840_KMD_QUAL_ZERO_LEGACY_TURNIP);
    a840_kmd_v3_freeze_policy_destroy(&policy);
}

/* Group 4: Backported mac-ci-phase2 KMD Regression Verification (Cases 38..44) */

void case38_backport_ib_diag_hex_formatting()
{
    begin_case("case38_backport_ib_diag_hex_formatting");
    A840KmdFaultRecord rec{};
    rec.sequence = 7u;
    rec.fault_kind = A840_KMD_FAULT_SMMU_TRANSLATION;
    rec.client_id = 42u;
    rec.queue_id = 3u;
    rec.fence = 99u;
    rec.ib1_pc = UINT64_C(0x0123456789abcdef);
    rec.ib2_pc = UINT64_C(0xfedcba9876543210);
    rec.rbbm_status = 0xa5b6c7d8u;
    rec.cp_status = 0x10293847u;
    rec.smmu_fault_va = UINT64_C(0x0000cafe0000beef);
    rec.smmu_fsr = 0x0000000fu;
    rec.timestamp_ticks = UINT64_C(123456789);

    char line[A840_KMD_FAULT_SNAPSHOT_LINE_MAX] = {};
    const size_t n = a840_kmd_format_fault_snapshot(&rec, line, sizeof(line));
    CHECK(n > 0u && n < sizeof(line) && line[n] == '\0');
    CHECK(std::strstr(line, "ib1_pc=0x0123456789abcdef") != nullptr);
    CHECK(std::strstr(line, "ib2_pc=0xfedcba9876543210") != nullptr);
    CHECK(std::strstr(line, "rbbm=0xa5b6c7d8") != nullptr);
    CHECK(std::strstr(line, "smmu_va=0x0000cafe0000beef") != nullptr);
}

void case39_backport_ib_diag_valid_and_blocked_reg()
{
    begin_case("case39_backport_ib_diag_valid_and_blocked_reg");
    const uint32_t valid_ib[] = {
        make_type7_hdr(A840_CP_NOP, 1u), 0u,
        make_type4_hdr(A840_REG_GRAS_CL_CNTL, 1u), 0u
    };
    A840KmdIbDiagResult res{};
    CHECK(a840_kmd_validate_ib_words(valid_ib, 4u, &res) == A840_KMD_IB_OK);
    CHECK(res.total_packets == 2u && res.type4_packets == 1u && res.type7_packets == 1u);

    const uint32_t blocked_ib[] = {
        make_type4_hdr(A840_REG_RB_GC_GMEM_PROTECT, 1u), 0u
    };
    CHECK(a840_kmd_validate_ib_words(blocked_ib, 2u, &res) == A840_KMD_IB_BLOCKED_REG);
    CHECK(res.offending_reg == A840_REG_RB_GC_GMEM_PROTECT && res.offending_dword_index == 1u);

    char diag_line[A840_KMD_IB_DIAG_LINE_MAX] = {};
    const size_t dn = a840_kmd_format_ib_diag(&res, diag_line, sizeof(diag_line));
    CHECK(dn > 0u && std::strstr(diag_line, "blocked_reg") != nullptr);
}

void case40_backport_wait_any_blocking_and_timeout()
{
    begin_case("case40_backport_wait_any_blocking_and_timeout");
    auto eng = std::make_unique<A840KmdWaitAnyEngine>();
    CHECK(a840_kmd_wait_any_init(eng.get(), nullptr) == A840_KMD_OK);

    NativeAdrenoAbiWaitAnyV3 req{};
    req.count = 1u;
    req.timeout_ns = UINT64_C(5000000);
    req.queues[0] = 10u;
    req.fences[0] = 5u;
    NativeAdrenoAbiWaitAnyReplyV3 rep{};
    CHECK(a840_kmd_wait_any_wait(eng.get(), &req, &rep) == A840_KMD_OK);
    CHECK(rep.timed_out == 1u && rep.satisfied_mask == 0u);

    req.timeout_ns = UINT64_C(2000000000);
    std::thread irq([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        (void)a840_kmd_wait_any_signal_fence(eng.get(), 10u, 5u, UINT64_C(777));
    });
    CHECK(a840_kmd_wait_any_wait(eng.get(), &req, &rep) == A840_KMD_OK);
    irq.join();
    CHECK(rep.timed_out == 0u && rep.satisfied_mask == 1u && rep.observed_ticks == 777u);
    a840_kmd_wait_any_destroy(eng.get());
}

void case41_backport_wait_any_poison_and_disconnect()
{
    begin_case("case41_backport_wait_any_poison_and_disconnect");
    auto eng = std::make_unique<A840KmdWaitAnyEngine>();
    CHECK(a840_kmd_wait_any_init(eng.get(), nullptr) == A840_KMD_OK);

    NativeAdrenoAbiWaitAnyV3 req{};
    req.count = 1u;
    req.timeout_ns = UINT64_C(2000000000);
    req.queues[0] = 20u;
    req.fences[0] = 10u;
    NativeAdrenoAbiWaitAnyReplyV3 rep{};

    std::thread poisoner([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        (void)a840_kmd_wait_any_poison_queue(eng.get(), 20u);
    });
    CHECK(a840_kmd_wait_any_wait(eng.get(), &req, &rep) == A840_KMD_POISONED);
    poisoner.join();

    req.queues[0] = 21u;
    std::thread disc([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        (void)a840_kmd_wait_any_disconnect_queue(eng.get(), 21u);
    });
    CHECK(a840_kmd_wait_any_wait(eng.get(), &req, &rep) == A840_KMD_DISCONNECTED);
    disc.join();
    a840_kmd_wait_any_destroy(eng.get());
}

void case42_backport_glue_zero_page_wait_any_lifecycle()
{
    begin_case("case42_backport_glue_zero_page_wait_any_lifecycle");
    GlueMockEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);

    GlueMockClient c{};
    CHECK(glue.openClient(&c, nullptr, nullptr) == kIOReturnSuccess);

    NativeAdrenoAbiZeroPageReplyV3 zp{};
    size_t w = 0u;
    CHECK(glue.externalMethod(&c, NativeAdrenoMethodGetZeroPageV3, nullptr, 0u,
                              &zp, sizeof(zp), &w) == kIOReturnSuccess);
    CHECK(w == sizeof(zp) && zp.gpu_va == UINT64_C(0x10000000) && zp.bytes == 65536u);

    NativeAdrenoAbiWaitAnyV3 wreq{};
    wreq.count = 1u;
    wreq.timeout_ns = UINT64_C(5000000);
    wreq.queues[0] = 0u;
    wreq.fences[0] = 3u;
    NativeAdrenoAbiWaitAnyReplyV3 wrep{};
    CHECK(glue.externalMethod(&c, NativeAdrenoMethodWaitAnyV3, &wreq, sizeof(wreq),
                              &wrep, sizeof(wrep)) == kIOReturnSuccess);
    CHECK(wrep.timed_out == 1u);

    wreq.timeout_ns = UINT64_C(2000000000);
    std::thread irq([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        (void)glue.signalFenceIrq(&c, 0u, 3u, UINT64_C(888));
    });
    CHECK(glue.externalMethod(&c, NativeAdrenoMethodWaitAnyV3, &wreq, sizeof(wreq),
                              &wrep, sizeof(wrep)) == kIOReturnSuccess);
    irq.join();
    CHECK(wrep.timed_out == 0u && wrep.satisfied_mask == 1u && wrep.observed_ticks == 888u);

    A840KmdClientCloseSummary summary{};
    CHECK(glue.closeClient(&c, &summary) == kIOReturnSuccess);
    CHECK(summary.zero_page_unbound == 1u);
}

void case43_backport_glue_submit_audit_and_channel_isolation()
{
    begin_case("case43_backport_glue_submit_audit_and_channel_isolation");
    GlueMockEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(true)) == kIOReturnSuccess);

    GlueMockClient c1{}, c2{};
    c1.ib_words = {make_type7_hdr(A840_CP_NOP, 1u), 0u};
    c2.ib_words = {make_type4_hdr(A840_REG_RB_GC_GMEM_PROTECT, 1u), 0u};
    CHECK(glue.openClient(&c1, nullptr, nullptr) == kIOReturnSuccess);
    CHECK(glue.openClient(&c2, nullptr, nullptr) == kIOReturnSuccess);

    NativeAdrenoV2SubmitWire sub{};
    sub.buffer_count = 1u;
    sub.queue = 0u;
    sub.buffers[0].handle = 1u;
    sub.buffers[0].iova = UINT64_C(0x10000);
    sub.buffers[0].bytes = 8u;
    NativeAdrenoV2SubmitReplyWire srep{};

    CHECK(glue.externalMethod(&c1, NativeAdrenoV2MethodSubmit, &sub, sizeof(sub),
                              &srep, sizeof(srep)) == kIOReturnSuccess);
    CHECK(glue.externalMethod(&c2, NativeAdrenoV2MethodSubmit, &sub, sizeof(sub),
                              &srep, sizeof(srep)) == kIOReturnNotPermitted);
    CHECK(env.ib_rejects == 1u);
    const auto stats = glue.getStats();
    CHECK(stats.ib_validated_ok == 1u && stats.ib_rejected_blocked_reg == 1u);
}

void case44_backport_glue_fault_snapshot_telemetry()
{
    begin_case("case44_backport_glue_fault_snapshot_telemetry");
    GlueMockEnv env;
    NativeAdrenoV3Glue glue;
    CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);
    GlueMockClient c{};
    CHECK(glue.openClient(&c, nullptr, nullptr) == kIOReturnSuccess);

    A840KmdFaultInput fin{};
    fin.fault_kind = A840_KMD_FAULT_GPU_HANG;
    fin.queue_id = 1u;
    fin.fence = 55u;
    fin.ib1_pc = UINT64_C(0x0000000100004000);
    fin.rbbm_status = 0xdeadbeefu;
    fin.timestamp_ticks = UINT64_C(4242);

    A840KmdFaultRecord rec{};
    char line[A840_KMD_FAULT_SNAPSHOT_LINE_MAX] = {};
    size_t len = 0u;
    CHECK(glue.reportHardwareFault(&c, fin, &rec, line, sizeof(line), &len) == kIOReturnSuccess);
    CHECK(len > 0u && std::strstr(line, "rbbm=0xdeadbeef") != nullptr);
    CHECK(env.faults_logged == 1u);
    CHECK(glue.checkSubmitChannel(&c, 1u) == kIOReturnAborted);
    CHECK(glue.checkSubmitChannel(&c, 0u) == kIOReturnSuccess);
}

/* Group 5: End-to-End Integration Across ALL KMD Subsystems (Cases 45..56) */

void case45_e2e_zero_page_and_sparse_va_integration()
{
    begin_case("case45_e2e_zero_page_and_sparse_va_integration");
    MockSmmuZeroPage smmu;
    A840KmdSmmuOps sops = smmu.ops();
    auto zp = std::make_unique<A840KmdZeroPageManager>();
    CHECK(a840_kmd_zero_page_init(zp.get(), &sops, UINT64_C(0x10000000)) == A840_KMD_OK);

    NativeAdrenoAbiZeroPageReplyV3 zrep{};
    CHECK(a840_kmd_zero_page_bind_client(zp.get(), 1u, nullptr, 0u,
                                         A840_KMD_SMMU_PERM_READ_ONLY, &zrep) == A840_KMD_OK);

    auto sparse = std::make_unique<A840KmdSparseManager>();
    A840KmdSparseConfig scfg{};
    scfg.zero_page_mgr = zp.get();
    CHECK(a840_kmd_sparse_init(sparse.get(), &scfg) == A840_KMD_OK);
    CHECK(a840_kmd_sparse_register_bo(sparse.get(), 1u, 100u, UINT64_C(0xa0000000),
                                      262144u, A840_KMD_SMMU_PERM_READ_WRITE) == A840_KMD_OK);
    uint64_t region_id = 0u;
    CHECK(a840_kmd_sparse_region_create(sparse.get(), 1u, nullptr,
                                        UINT64_C(0x40000000), 262144u, &region_id) == A840_KMD_OK);

    A840KmdSparseTileInfo tinfo{};
    CHECK(a840_kmd_sparse_query_tile(sparse.get(), 1u, UINT64_C(0x40000000), &tinfo) == A840_KMD_OK);
    CHECK(tinfo.is_zero_page == 1u && tinfo.permissions == A840_KMD_SMMU_PERM_READ_ONLY);

    A840KmdSparseBindReqV3 sreq{};
    A840KmdSparseBindReplyV3 srep{};
    sreq.version = NativeAdrenoAbiVersionV3;
    sreq.op_count = 1u;
    sreq.region_id = region_id;
    sreq.ops[0].op = A840_KMD_SPARSE_OP_BIND_BO;
    sreq.ops[0].flags = A840_KMD_SMMU_PERM_READ_WRITE;
    sreq.ops[0].gpu_va = UINT64_C(0x40000000);
    sreq.ops[0].length_bytes = 65536u;
    sreq.ops[0].bo_handle = 100u;
    CHECK(a840_kmd_sparse_dispatch_v3(sparse.get(), 1u, &sreq, sizeof(sreq),
                                      &srep, sizeof(srep)) == A840_KMD_OK);
    CHECK(srep.completed_ops == 1u && srep.tlb_flush_generation >= 1u);

    CHECK(a840_kmd_sparse_query_tile(sparse.get(), 1u, UINT64_C(0x40000000), &tinfo) == A840_KMD_OK);
    CHECK(tinfo.is_zero_page == 0u && tinfo.bo_handle == 100u);

    uint32_t unbound = 0u;
    uint64_t tlb_gen = 0u;
    CHECK(a840_kmd_sparse_unbind_bo(sparse.get(), 1u, 100u, 1u, &unbound, &tlb_gen) == A840_KMD_OK);
    CHECK(unbound == 1u && tlb_gen > srep.tlb_flush_generation);
    CHECK(a840_kmd_sparse_query_tile(sparse.get(), 1u, UINT64_C(0x40000000), &tinfo) == A840_KMD_OK);
    CHECK(tinfo.is_zero_page == 1u && tinfo.permissions == A840_KMD_SMMU_PERM_READ_ONLY);

    a840_kmd_sparse_destroy(sparse.get());
    a840_kmd_zero_page_destroy(zp.get());
}

void case46_e2e_ubwc6_writer_promise_display_handoff()
{
    begin_case("case46_e2e_ubwc6_writer_promise_display_handoff");
    struct SurfLock {
        std::mutex mu;
        static void lk(void *c) { static_cast<SurfLock *>(c)->mu.lock(); }
        static void ul(void *c) { static_cast<SurfLock *>(c)->mu.unlock(); }
    } sl;
    A840KmdSurfaceOps sops{&sl, &SurfLock::lk, &SurfLock::ul};
    auto smgr = std::make_unique<A840KmdSurfaceManager>();
    CHECK(a840_kmd_surface_init(smgr.get(), &sops) == A840_KMD_OK);

    const uint32_t fmt = 80u;
    A840KmdSurfaceLayout lay{};
    lay.bytes = UINT64_C(4) * 1024u * 1024u;
    lay.display_base = UINT64_C(0x50000000);
    lay.format = fmt;
    lay.width = 1200u;
    lay.height = 2600u;
    lay.meta_rows = 128u;
    CHECK(a840_kmd_surface_create(smgr.get(), 10u, 1u, 9001u, &lay) == A840_KMD_OK);

    A840KmdMdssCaps mcaps{};
    mcaps.flags = A840_MDSS_UBWC6;
    mcaps.max_width = 4096u;
    mcaps.max_height = 4096u;
    mcaps.formats = &fmt;
    mcaps.format_count = 1u;

    NativeAdrenoAbiSurfaceMetaV3 smeta{};
    smeta.bo_handle = 1u;
    smeta.op = 1u;
    smeta.ubwc_enabled = 1u;
    smeta.ubwc_version = 6u;
    smeta.highest_bank_bit = 16u;
    smeta.swizzle_levels = 6u;
    smeta.macrotile_mode = 1u;
    smeta.meta_offset = 4096u;
    smeta.meta_pitch = 256u;
    A840KmdSurfaceResult sres{};
    CHECK(a840_kmd_surface_meta(smgr.get(), 10u, &smeta, 1u, &mcaps, &sres) == A840_KMD_OK);
    CHECK(sres.scanout_eligible == 1u && sres.fallback_linear_required == 0u);

    auto preg = std::make_unique<A840KmdWriterPromiseRegistry>();
    A840KmdPromiseConfig pcfg{};
    CHECK(a840_kmd_writer_promise_init(preg.get(), &pcfg) == A840_KMD_OK);
    CHECK(a840_kmd_writer_promise_register_client(preg.get(), 10u, A840_CLIENT_ROLE_RENDER_SERVER) == A840_KMD_OK);
    CHECK(a840_kmd_writer_promise_bind_surface(preg.get(), 10u, 1u, 9001u) == A840_KMD_OK);

    A840KmdWriterPromiseCall call{};
    call.writer_client = 10u;
    call.flags = A840_PROMISE_FLAG_EARLY_HANDOFF;
    call.abi.op = NativeAdrenoPromiseMake;
    call.abi.surface_count = 1u;
    call.abi.queue = 0u;
    call.abi.surface_ids[0] = 1u;
    NativeAdrenoAbiWriterPromiseReplyV3 preply{};
    CHECK(a840_kmd_writer_promise_dispatch(preg.get(), &call, &preply, nullptr) == A840_KMD_OK);
    CHECK(preply.accepted == 1u && preply.promise_token != 0u);

    call.abi.op = NativeAdrenoPromiseFulfill;
    call.abi.promise_token = preply.promise_token;
    call.abi.fence = 42u;
    CHECK(a840_kmd_writer_promise_dispatch(preg.get(), &call, &preply, nullptr) == A840_KMD_OK);

    uint32_t commits = 0u;
    auto commit_fn = [](void *cookie, const A840KmdMdssCommitDesc *desc) -> A840KmdStatus {
        (*static_cast<uint32_t *>(cookie))++;
        return (desc->ubwc_enabled == 1u && desc->sync_mode == A840_DISPLAY_SYNC_EARLY_GPU_FENCE)
            ? A840_KMD_OK : A840_KMD_STATE_CONFLICT;
    };

    auto dh = std::make_unique<A840KmdDisplayHandoff>();
    A840KmdDisplayHandoffConfig dcfg{};
    dcfg.surface_mgr = smgr.get();
    dcfg.promise_reg = preg.get();
    dcfg.commit_fn = commit_fn;
    dcfg.commit_cookie = &commits;
    CHECK(a840_kmd_display_handoff_init(dh.get(), &dcfg) == A840_KMD_OK);

    A840KmdDisplayFrameRequest freq{};
    freq.consumer_client = 10u;
    freq.surface_handle = 1u;
    freq.surface_identity = 9001u;
    freq.mdss_caps = &mcaps;
    A840KmdDisplayHandoffResult dres{};
    CHECK(a840_kmd_display_handoff_submit_frame(dh.get(), &freq, &dres) == A840_KMD_OK);
    CHECK(dres.outcome == A840_DISPLAY_HANDOFF_COMMITTED);
    CHECK(dres.scanout_mode == A840_DISPLAY_SCANOUT_UBWC6_DIRECT);
    CHECK(dres.sync_mode == A840_DISPLAY_SYNC_EARLY_GPU_FENCE && commits == 1u);

    a840_kmd_display_handoff_destroy(dh.get());
    a840_kmd_writer_promise_destroy(preg.get());
}

void case47_e2e_ca_env_policy_feeds_production_qualification()
{
    begin_case("case47_e2e_ca_env_policy_feeds_production_qualification");
    A840KmdCaEnvPolicyInput ca_in{A840_KMD_CA_TARGET_BACKBOARDD, 1u, 1u, 0u};
    A840KmdCaEnvDecision dec{};
    CHECK(a840_kmd_ca_env_evaluate(&ca_in, &dec) == A840_KMD_OK);
    CHECK(dec.should_inject == 1u && std::strcmp(dec.env_entry, A840_KMD_CA_ENV_ENTRY) == 0);

    const char *existing[] = {"PATH=/usr/bin", "HOME=/var/root"};
    char env_block[256] = {};
    size_t written = 0u, entries = 0u;
    CHECK(a840_kmd_ca_env_build_env_block(&ca_in, existing, 2u, env_block,
                                          sizeof(env_block), &written, &entries, &dec) == A840_KMD_OK);
    CHECK(entries == 3u && written > 0u);

    ca_in.target_label = "com.apple.mobilesafari";
    CHECK(a840_kmd_ca_env_evaluate(&ca_in, &dec) != A840_KMD_OK);

    ca_in.target_label = A840_KMD_CA_TARGET_BACKBOARDD;
    CHECK(a840_kmd_ca_env_evaluate(&ca_in, &dec) == A840_KMD_OK);
    A840KmdProdQualInput qin = make_valid_prod_input();
    qin.ca_target_label = ca_in.target_label;
    qin.ca_env_entry = dec.env_entry;
    qin.ca_policy_enforced = dec.should_inject;
    A840KmdProdQualReport qrep{};
    CHECK(a840_kmd_v3_qualify_production(&qin, &qrep) == A840_KMD_OK && qrep.qualified == 1u);
}

void case48_e2e_bo_pool_and_quota_recovery_integration()
{
    begin_case("case48_e2e_bo_pool_and_quota_recovery_integration");
    auto pool = std::make_unique<A840KmdBoPool>();
    A840KmdBoPoolConfig pcfg{};
    pcfg.max_parked_bytes = UINT64_C(8) * 1024u * 1024u;
    CHECK(a840_kmd_bo_pool_init(pool.get(), &pcfg) == A840_KMD_OK);

    A840KmdBoAcquireReq areq{};
    areq.client_id = 1u;
    areq.requested_bytes = 65536u;
    areq.permissions = A840_KMD_SMMU_PERM_READ_WRITE;
    areq.cache_mode = A840_KMD_BO_CACHE_COHERENT;
    A840KmdBoDesc bo{};
    CHECK(a840_kmd_bo_pool_acquire(pool.get(), &areq, &bo) == A840_KMD_OK);
    CHECK(bo.bo_handle != 0u && bo.bucket_bytes == 65536u);
    CHECK(a840_kmd_bo_pool_release(pool.get(), 1u, bo.bo_handle, 1u) == A840_KMD_OK);

    A840KmdBoDesc bo2{};
    CHECK(a840_kmd_bo_pool_acquire(pool.get(), &areq, &bo2) == A840_KMD_OK);
    CHECK(bo2.from_parked_pool == 1u);

    auto qr = std::make_unique<A840KmdQuotaRecovery>();
    A840KmdQuotaRecoveryConfig qcfg{};
    qcfg.global_hard_limit_bytes = UINT64_C(64) * 1024u * 1024u;
    qcfg.compositor_reserve_bytes = UINT64_C(8) * 1024u * 1024u;
    qcfg.bo_pool = pool.get();
    CHECK(a840_kmd_qr_init(qr.get(), &qcfg) == A840_KMD_OK);

    A840KmdClientQuotaConfig cc{1u, A840_KMD_PRIO_COMPOSITOR, 0u,
                                UINT64_C(16) * 1024u * 1024u, UINT64_C(32) * 1024u * 1024u};
    CHECK(a840_kmd_qr_register_client(qr.get(), &cc) == A840_KMD_OK);

    A840KmdAllocResult ares{};
    CHECK(a840_kmd_qr_request_alloc(qr.get(), 1u, UINT64_C(1024) * 1024u, &ares) == A840_KMD_OK);
    CHECK(ares.admitted == 1u);
    CHECK(a840_kmd_qr_open_channel(qr.get(), 1u, 0u) == A840_KMD_OK);
    CHECK(a840_kmd_qr_submit_begin(qr.get(), 1u, 0u, 1u, nullptr, 0u) == A840_KMD_OK);
    uint32_t qr_done = 0u;
    CHECK(a840_kmd_qr_submit_complete(qr.get(), 1u, 0u, 1u, &qr_done) == A840_KMD_OK && qr_done == 1u);

    a840_kmd_qr_destroy(qr.get());
    a840_kmd_bo_pool_destroy(pool.get());
}

void case49_e2e_preempt_4ring_and_wakeup_latency_bridge()
{
    begin_case("case49_e2e_preempt_4ring_and_wakeup_latency_bridge");
    auto wake = std::make_unique<A840KmdWakeupEngine>();
    CHECK(a840_kmd_wakeup_init(wake.get(), nullptr) == A840_KMD_OK);

    A840PreemptHwOps hw_ops{};
    hw_ops.cookie = wake.get();
    hw_ops.on_pi_boost_change = &a840_kmd_wakeup_bridge_pi_boost;
    hw_ops.on_latency_sample = &a840_kmd_wakeup_bridge_latency;

    A840PreemptConfig pcfg{};
    pcfg.hw_ops = &hw_ops;
    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        pcfg.rings[r].preempt_record_va = UINT64_C(0x20000000) + static_cast<uint64_t>(r) * UINT64_C(0x40000);
        pcfg.rings[r].preempt_record_bytes = UINT64_C(0x20000);
        pcfg.rings[r].preempt_level = (r <= 1u) ? A840_PREEMPT_LEVEL_L1B_DRAW_BOUNDARY
                                                : A840_PREEMPT_LEVEL_L1A_BIN_BOUNDARY;
    }

    auto sched = std::make_unique<A840KmdPreemptScheduler>();
    CHECK(a840_kmd_preempt_init(sched.get(), &pcfg) == A840_KMD_OK);

    A840PreemptSubmitReq r3{};
    r3.ring_id = A840_RING_3_BG_COMPUTE;
    r3.enqueue_ns = UINT64_C(1000000);
    A840PreemptSubmitResult res3{};
    CHECK(a840_kmd_preempt_submit(sched.get(), &r3, &res3) == A840_KMD_OK && res3.doorbell_emitted == 1u);

    A840PreemptSubmitReq r0{};
    r0.ring_id = A840_RING_0_COMPOSITOR;
    r0.enqueue_ns = UINT64_C(2000000);
    r0.frame_deadline_ns = UINT64_C(4000000);
    A840PreemptSubmitResult res0{};
    CHECK(a840_kmd_preempt_submit(sched.get(), &r0, &res0) == A840_KMD_OK && res0.preempt_triggered == 1u);

    CHECK(a840_kmd_preempt_on_hw_in_progress(sched.get(), UINT64_C(2050000)) == A840_KMD_OK);
    CHECK(a840_kmd_preempt_on_switch_done(sched.get(), UINT64_C(2120000)) == A840_KMD_OK);

    uint32_t woken_cb = 0u;
    auto wake_cb = [](void *c, uint64_t, uint32_t ring, uint64_t, uint64_t, uint32_t, uint64_t) {
        if (ring == A840_RING_0_COMPOSITOR) (*static_cast<uint32_t *>(c))++;
    };
    A840WakeupRegisterResult wreg{};
    CHECK(a840_kmd_wakeup_register_waiter(wake.get(), A840_RING_0_COMPOSITOR, res0.seqno,
                                          UINT64_C(2130000), wake_cb, &woken_cb, &wreg) == A840_KMD_OK);
    uint32_t woken = 0u;
    CHECK(a840_kmd_wakeup_on_irq(wake.get(), A840_RING_0_COMPOSITOR, res0.seqno,
                                 UINT64_C(2200000), &woken) == A840_KMD_OK);
    CHECK(woken == 1u && woken_cb == 1u);

    A840LatencyStats lstats{};
    CHECK(a840_kmd_wakeup_get_latency_stats(wake.get(), A840_LATENCY_PREEMPT_TRIGGER_TO_ACTIVE, &lstats) == A840_KMD_OK);
    CHECK(lstats.count == 1u && lstats.max_ns == UINT64_C(120000));

    a840_kmd_preempt_destroy(sched.get());
    a840_kmd_wakeup_destroy(wake.get());
}

void case50_e2e_gmu_dcvs_and_mdss_120hz_closed_loop()
{
    begin_case("case50_e2e_gmu_dcvs_and_mdss_120hz_closed_loop");
    auto gov = std::make_unique<A840KmdGmuDcvs>();
    CHECK(a840_gmu_dcvs_init(gov.get(), nullptr) == A840_KMD_OK);

    auto mdss = std::make_unique<A840KmdMdssSync>();
    A840MdssSyncCallbacks cbs{};
    cbs.cookie = gov.get();
    cbs.on_boost_hint = &a840_gmu_dcvs_bridge_mdss_boost;
    A840MdssSyncConfig mcfg{};
    mcfg.initial_vsync_ns = UINT64_C(100000000);
    mcfg.has_initial_vsync = 1u;
    mcfg.callbacks = &cbs;
    CHECK(a840_mdss_sync_init(mdss.get(), &mcfg) == A840_KMD_OK);

    A840MdssCommitRequest creq{};
    creq.frame_id = 1u;
    creq.now_ns = UINT64_C(104000000);
    creq.has_early_writer_promise = 1u;
    creq.ring0_compositor = 1u;
    A840MdssCommitPlan plan{};
    CHECK(a840_mdss_sync_plan_commit(mdss.get(), &creq, &plan) == A840_KMD_OK);
    CHECK(plan.decision == A840_MDSS_COMMIT_HIT_CURRENT_VSYNC && plan.extra_vsync_waits == 0u);

    creq.frame_id = 2u;
    creq.now_ns = UINT64_C(100000000) + A840_MDSS_PERIOD_120HZ_NS + UINT64_C(6500000);
    CHECK(a840_mdss_sync_plan_commit(mdss.get(), &creq, &plan) == A840_KMD_OK);
    CHECK(plan.boost_hint_emitted == 1u);

    A840GmuDcvsSnapshot snap{};
    CHECK(a840_gmu_dcvs_get_snapshot(gov.get(), &snap) == A840_KMD_OK && snap.mdss_boost_hints_applied >= 1u);

    CHECK(a840_gmu_dcvs_update_thermal(gov.get(), 75000, UINT64_C(120000000)) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(gov.get(), &snap) == A840_KMD_OK && snap.thermal_cap_level == A840_GPU_OPP_THERMAL_1000_IDX);
    CHECK(a840_gmu_dcvs_update_thermal(gov.get(), 95000, UINT64_C(125000000)) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_get_snapshot(gov.get(), &snap) == A840_KMD_OK && snap.thermal_cap_level == A840_GPU_OPP_TOUCH_FLOOR_IDX);

    a840_mdss_sync_destroy(mdss.get());
    a840_gmu_dcvs_destroy(gov.get());
}

void case51_e2e_live_subsystems_pass_production_qualification()
{
    begin_case("case51_e2e_live_subsystems_pass_production_qualification");
    a840_kmd_v3_reset_legacy_rejected_count();

    MockSmmuZeroPage smmu;
    A840KmdSmmuOps sops = smmu.ops();
    auto zp = std::make_unique<A840KmdZeroPageManager>();
    CHECK(a840_kmd_zero_page_init(zp.get(), &sops, UINT64_C(0x10000000)) == A840_KMD_OK);
    NativeAdrenoAbiZeroPageReplyV3 zrep{};
    CHECK(a840_kmd_zero_page_bind_client(zp.get(), 1u, nullptr, 0u,
                                         A840_KMD_SMMU_PERM_READ_ONLY, &zrep) == A840_KMD_OK);
    A840KmdZeroPageStats zstats{};
    CHECK(a840_kmd_zero_page_get_stats(zp.get(), &zstats) == A840_KMD_OK);

    A840KmdCaEnvPolicyInput ca_in{A840_KMD_CA_TARGET_BACKBOARDD, 1u, 1u, 0u};
    A840KmdCaEnvDecision ca_dec{};
    CHECK(a840_kmd_ca_env_evaluate(&ca_in, &ca_dec) == A840_KMD_OK);

    A840PreemptConfig pcfg{};
    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        pcfg.rings[r].preempt_record_va = UINT64_C(0x50000000) + static_cast<uint64_t>(r) * UINT64_C(0x20000);
        pcfg.rings[r].preempt_record_bytes = UINT64_C(0x20000);
        pcfg.rings[r].preempt_level = A840_PREEMPT_LEVEL_L1A_BIN_BOUNDARY;
    }
    auto sched = std::make_unique<A840KmdPreemptScheduler>();
    CHECK(a840_kmd_preempt_init(sched.get(), &pcfg) == A840_KMD_OK);

    auto gov = std::make_unique<A840KmdGmuDcvs>();
    CHECK(a840_gmu_dcvs_init(gov.get(), nullptr) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_update_thermal(gov.get(), 75000, UINT64_C(1000000)) == A840_KMD_OK);
    A840GmuDcvsSnapshot gsnap75{};
    CHECK(a840_gmu_dcvs_get_snapshot(gov.get(), &gsnap75) == A840_KMD_OK);
    CHECK(a840_gmu_dcvs_update_thermal(gov.get(), 95000, UINT64_C(2000000)) == A840_KMD_OK);
    A840GmuDcvsSnapshot gsnap95{};
    CHECK(a840_gmu_dcvs_get_snapshot(gov.get(), &gsnap95) == A840_KMD_OK);

    size_t gpu_cnt = 0u, gmu_cnt = 0u;
    const A840GpuOppEntry *gpu_tbl = a840_gmu_dcvs_get_opp_table(&gpu_cnt);
    const A840GmuOppEntry *gmu_tbl = a840_gmu_dcvs_get_gmu_table(&gmu_cnt);
    const A840ChipFacts *facts = a840_chip_facts_kaanapali_default();

    A840KmdProdQualInput qin{};
    qin.abi_version = NativeAdrenoAbiVersionV3;
    qin.envelope_bytes = NativeAdrenoAbiEnvelopeBytes;
    qin.advertised_cap_flags = A840_KMD_V3_PROD_CAP_MASK;
    qin.chip_id = facts->chip_id;
    qin.gmem_bytes = facts->gmem_bytes;
    qin.highest_bank_bit = facts->highest_bank_bit;
    qin.ubwc_version = facts->ubwc_version;
    qin.smmu_page_shift = facts->smmu_page_shift;
    qin.zero_page_ready = zstats.backing_allocated;
    qin.zero_page_readonly = (zstats.rejected_write_maps == 0u) ? 1u : 0u;
    qin.zero_page_gpu_va = zrep.gpu_va;
    qin.zero_page_phys_addr = zstats.backing_phys_addr;
    qin.zero_page_bytes = zrep.bytes;
    qin.ca_target_label = ca_in.target_label;
    qin.ca_env_entry = ca_dec.env_entry;
    qin.ca_native_metal_enabled = ca_in.native_metal_enabled;
    qin.ca_pack_verified = ca_in.phase4_pack_verified;
    qin.ca_policy_enforced = ca_dec.should_inject;
    qin.preempt_ring_count = A840_RING_COUNT;
    for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
        qin.preempt_records[r].record_va = pcfg.rings[r].preempt_record_va;
        qin.preempt_records[r].record_bytes = pcfg.rings[r].preempt_record_bytes;
    }
    qin.gpu_opp_table = gpu_tbl;
    qin.gpu_opp_count = gpu_cnt;
    qin.gmu_opp_table = gmu_tbl;
    qin.gmu_opp_count = gmu_cnt;
    qin.thermal_ladder_active = (gsnap75.thermal_tier == 1u && gsnap95.thermal_tier == 5u) ? 1u : 0u;
    qin.thermal_tier_count = 5u;
    qin.thermal_min_throttle_mc = A840_DCVS_THERMAL_TIER0_MC;
    qin.thermal_cap_75c_mhz = gpu_tbl[gsnap75.thermal_cap_level].freq_mhz;
    qin.thermal_cap_95c_mhz = gpu_tbl[gsnap95.thermal_cap_level].freq_mhz;

    A840KmdProdQualReport qrep{};
    CHECK(a840_kmd_v3_qualify_production(&qin, &qrep) == A840_KMD_OK);
    CHECK(qrep.qualified == 1u && qrep.passed_mask == A840_KMD_QUAL_ALL_MASK);
    CHECK(qrep.abi_version_frozen == 1u && qrep.turnip_paths_purged == 1u);

    a840_gmu_dcvs_destroy(gov.get());
    a840_kmd_preempt_destroy(sched.get());
    a840_kmd_zero_page_destroy(zp.get());
}

void case52_e2e_fault_isolation_ring2_never_stalls_ring0()
{
    begin_case("case52_e2e_fault_isolation_ring2_never_stalls_ring0");
    auto fc = std::make_unique<A840KmdFaultCapture>();
    CHECK(a840_kmd_fault_init(fc.get()) == A840_KMD_OK);

    auto preg = std::make_unique<A840KmdWriterPromiseRegistry>();
    A840KmdPromiseConfig pcfg{};
    pcfg.fault_capture = fc.get();
    CHECK(a840_kmd_writer_promise_init(preg.get(), &pcfg) == A840_KMD_OK);
    CHECK(a840_kmd_writer_promise_register_client(preg.get(), 2u, A840_CLIENT_ROLE_APP) == A840_KMD_OK);
    CHECK(a840_kmd_writer_promise_bind_surface(preg.get(), 2u, 1u, 2001u) == A840_KMD_OK);

    A840KmdWriterPromiseCall call{};
    call.writer_client = 2u;
    call.abi.op = NativeAdrenoPromiseMake;
    call.abi.surface_count = 1u;
    call.abi.queue = 2u;
    call.abi.surface_ids[0] = 1u;
    NativeAdrenoAbiWriterPromiseReplyV3 rep{};
    CHECK(a840_kmd_writer_promise_dispatch(preg.get(), &call, &rep, nullptr) == A840_KMD_OK);

    A840KmdFaultInput fin{};
    fin.fault_kind = A840_KMD_FAULT_GPU_HANG;
    fin.client_id = 2u;
    fin.queue_id = 2u;
    fin.fence = 1u;
    A840KmdFaultSnapshot snap{};
    CHECK(a840_kmd_fault_record(fc.get(), &fin, &snap) == A840_KMD_OK);
    a840_kmd_writer_promise_on_fault_poison(preg.get(), 2u, 2u, &snap);
    CHECK(a840_kmd_writer_promise_pending_count(preg.get(), 2u) == 0u);

    auto wake = std::make_unique<A840KmdWakeupEngine>();
    CHECK(a840_kmd_wakeup_init(wake.get(), nullptr) == A840_KMD_OK);
    uint32_t fault_woken = 0u;
    auto cb = [](void *c, uint64_t, uint32_t, uint64_t, uint64_t, uint32_t reason, uint64_t) {
        if (reason == A840_WAKE_REASON_RING_FAULTED) (*static_cast<uint32_t *>(c))++;
    };
    A840WakeupRegisterResult wreg{};
    CHECK(a840_kmd_wakeup_register_waiter(wake.get(), A840_RING_2_CLIENT_3D, 99u,
                                          UINT64_C(1000), cb, &fault_woken, &wreg) == A840_KMD_OK);
    uint32_t cnt = 0u;
    CHECK(a840_kmd_wakeup_fault_ring(wake.get(), A840_RING_2_CLIENT_3D, UINT64_C(2000), &cnt) == A840_KMD_OK);
    CHECK(cnt == 1u && fault_woken == 1u);

    a840_kmd_wakeup_destroy(wake.get());
    a840_kmd_writer_promise_destroy(preg.get());
    a840_kmd_fault_destroy(fc.get());
}

void case53_e2e_zero_page_and_sparse_write_permission_rejected()
{
    begin_case("case53_e2e_zero_page_and_sparse_write_permission_rejected");
    MockSmmuZeroPage smmu;
    A840KmdSmmuOps sops = smmu.ops();
    auto zp = std::make_unique<A840KmdZeroPageManager>();
    CHECK(a840_kmd_zero_page_init(zp.get(), &sops, UINT64_C(0x10000000)) == A840_KMD_OK);

    NativeAdrenoAbiZeroPageReplyV3 zrep{};
    CHECK(a840_kmd_zero_page_bind_client(zp.get(), 1u, nullptr, 0u,
                                         A840_KMD_SMMU_PERM_READ_WRITE, &zrep) == A840_KMD_PERMISSION_DENIED);
    CHECK(a840_kmd_zero_page_bind_client(zp.get(), 1u, nullptr, 0u,
                                         A840_KMD_SMMU_PERM_READ_ONLY, &zrep) == A840_KMD_OK);

    auto sparse = std::make_unique<A840KmdSparseManager>();
    A840KmdSparseConfig scfg{};
    scfg.zero_page_mgr = zp.get();
    CHECK(a840_kmd_sparse_init(sparse.get(), &scfg) == A840_KMD_OK);
    uint64_t rid = 0u;
    CHECK(a840_kmd_sparse_region_create(sparse.get(), 1u, nullptr,
                                        UINT64_C(0x40000000), 65536u, &rid) == A840_KMD_OK);

    A840KmdSparseBindReqV3 sreq{};
    A840KmdSparseBindReplyV3 srep{};
    sreq.version = NativeAdrenoAbiVersionV3;
    sreq.op_count = 1u;
    sreq.region_id = rid;
    sreq.ops[0].op = A840_KMD_SPARSE_OP_BIND_ZERO_PAGE;
    sreq.ops[0].flags = A840_KMD_SMMU_PERM_READ_WRITE;
    sreq.ops[0].gpu_va = UINT64_C(0x40000000);
    sreq.ops[0].length_bytes = 65536u;
    CHECK(a840_kmd_sparse_dispatch_v3(sparse.get(), 1u, &sreq, sizeof(sreq),
                                      &srep, sizeof(srep)) == A840_KMD_PERMISSION_DENIED);

    a840_kmd_sparse_destroy(sparse.get());
    a840_kmd_zero_page_destroy(zp.get());
}

void case54_e2e_ubwc_fallback_linear_when_mdss_lacks_ubwc6()
{
    begin_case("case54_e2e_ubwc_fallback_linear_when_mdss_lacks_ubwc6");
    struct SurfLock {
        std::mutex mu;
        static void lk(void *c) { static_cast<SurfLock *>(c)->mu.lock(); }
        static void ul(void *c) { static_cast<SurfLock *>(c)->mu.unlock(); }
    } sl;
    A840KmdSurfaceOps sops{&sl, &SurfLock::lk, &SurfLock::ul};
    auto smgr = std::make_unique<A840KmdSurfaceManager>();
    CHECK(a840_kmd_surface_init(smgr.get(), &sops) == A840_KMD_OK);

    const uint32_t fmt = 80u;
    A840KmdSurfaceLayout lay{};
    lay.bytes = UINT64_C(4) * 1024u * 1024u;
    lay.display_base = UINT64_C(0x50000000);
    lay.format = fmt;
    lay.width = 1200u;
    lay.height = 2600u;
    lay.meta_rows = 128u;
    CHECK(a840_kmd_surface_create(smgr.get(), 1u, 1u, 1001u, &lay) == A840_KMD_OK);

    A840KmdMdssCaps no_ubwc_caps{};
    no_ubwc_caps.flags = 0u;
    no_ubwc_caps.max_width = 4096u;
    no_ubwc_caps.max_height = 4096u;
    no_ubwc_caps.formats = &fmt;
    no_ubwc_caps.format_count = 1u;

    NativeAdrenoAbiSurfaceMetaV3 smeta{};
    smeta.bo_handle = 1u;
    smeta.op = 1u;
    smeta.ubwc_enabled = 1u;
    smeta.ubwc_version = 6u;
    smeta.highest_bank_bit = 16u;
    smeta.swizzle_levels = 6u;
    smeta.macrotile_mode = 1u;
    smeta.meta_offset = 4096u;
    smeta.meta_pitch = 256u;
    A840KmdSurfaceResult sres{};
    CHECK(a840_kmd_surface_meta(smgr.get(), 1u, &smeta, 1u, &no_ubwc_caps, &sres) == A840_KMD_OK);
    CHECK(sres.fallback_linear_required == 1u && sres.scanout_eligible == 0u);
}

void case55_e2e_quota_compositor_reserve_under_pressure()
{
    begin_case("case55_e2e_quota_compositor_reserve_under_pressure");
    auto qr = std::make_unique<A840KmdQuotaRecovery>();
    A840KmdQuotaRecoveryConfig qcfg{};
    qcfg.global_hard_limit_bytes = UINT64_C(16) * 1024u * 1024u;
    qcfg.compositor_reserve_bytes = UINT64_C(4) * 1024u * 1024u;
    CHECK(a840_kmd_qr_init(qr.get(), &qcfg) == A840_KMD_OK);

    A840KmdClientQuotaConfig bg{1u, A840_KMD_PRIO_BACKGROUND, 0u,
                                UINT64_C(12) * 1024u * 1024u, UINT64_C(16) * 1024u * 1024u};
    A840KmdClientQuotaConfig comp{2u, A840_KMD_PRIO_COMPOSITOR, 0u,
                                  UINT64_C(8) * 1024u * 1024u, UINT64_C(16) * 1024u * 1024u};
    CHECK(a840_kmd_qr_register_client(qr.get(), &bg) == A840_KMD_OK);
    CHECK(a840_kmd_qr_register_client(qr.get(), &comp) == A840_KMD_OK);

    A840KmdAllocResult res{};
    CHECK(a840_kmd_qr_request_alloc(qr.get(), 1u, UINT64_C(12) * 1024u * 1024u, &res) == A840_KMD_OK);
    CHECK(a840_kmd_qr_request_alloc(qr.get(), 1u, UINT64_C(2) * 1024u * 1024u, &res) == A840_KMD_OOM);
    CHECK(a840_kmd_qr_request_alloc(qr.get(), 2u, UINT64_C(3) * 1024u * 1024u, &res) == A840_KMD_OK);
    CHECK(res.used_compositor_reserve == 1u);

    a840_kmd_qr_destroy(qr.get());
}

void case56_e2e_wait_any_preempt_mdss_120hz_budget()
{
    begin_case("case56_e2e_wait_any_preempt_mdss_120hz_budget");
    auto eng = std::make_unique<A840KmdWaitAnyEngine>();
    CHECK(a840_kmd_wait_any_init(eng.get(), nullptr) == A840_KMD_OK);

    auto mdss = std::make_unique<A840KmdMdssSync>();
    A840MdssSyncConfig mcfg{};
    mcfg.initial_vsync_ns = UINT64_C(500000000);
    mcfg.has_initial_vsync = 1u;
    CHECK(a840_mdss_sync_init(mdss.get(), &mcfg) == A840_KMD_OK);

    for (uint64_t f = 1u; f <= 12u; ++f) {
        const uint64_t vsync = UINT64_C(500000000) + f * A840_MDSS_PERIOD_120HZ_NS;
        CHECK(a840_mdss_sync_on_vsync(mdss.get(), vsync, f) == A840_KMD_OK);
        CHECK(a840_kmd_wait_any_signal_fence(eng.get(), 0u, f, f * 100u) == A840_KMD_OK);

        NativeAdrenoAbiWaitAnyV3 wreq{};
        wreq.count = 1u;
        wreq.queues[0] = 0u;
        wreq.fences[0] = f;
        NativeAdrenoAbiWaitAnyReplyV3 wrep{};
        CHECK(a840_kmd_wait_any_wait(eng.get(), &wreq, &wrep) == A840_KMD_OK);
        CHECK(wrep.satisfied_mask == 1u && wrep.timed_out == 0u);

        A840MdssCommitRequest creq{};
        creq.frame_id = f;
        creq.now_ns = vsync + UINT64_C(3000000);
        creq.is_retired_ready = 1u;
        creq.ring0_compositor = 1u;
        A840MdssCommitPlan plan{};
        CHECK(a840_mdss_sync_plan_commit(mdss.get(), &creq, &plan) == A840_KMD_OK);
        CHECK(plan.decision == A840_MDSS_COMMIT_HIT_CURRENT_VSYNC && plan.extra_vsync_waits == 0u);
    }

    A840MdssSyncStats mstats{};
    CHECK(a840_mdss_sync_get_stats(mdss.get(), &mstats) == A840_KMD_OK);
    CHECK(mstats.frames_on_time == 12u && mstats.total_extra_vsync_waits == 0u);

    a840_mdss_sync_destroy(mdss.get());
    a840_kmd_wait_any_destroy(eng.get());
}

/* Group 6: 8-Thread Concurrency & TSan 20x Stress Tests (Cases 57..60) */

void case57_tsan_8thread_freeze_policy_and_atomic_audit()
{
    begin_case("case57_tsan_8thread_freeze_policy_and_atomic_audit");
    for (uint32_t round = 0u; round < g_stress_rounds; ++round) {
        a840_kmd_v3_reset_legacy_rejected_count();
        A840KmdV3FreezePolicy policy{};
        CHECK(a840_kmd_v3_freeze_policy_init(&policy, nullptr) == A840_KMD_OK);

        constexpr uint32_t kThreads = 8u;
        constexpr uint32_t kIters = 50u;
        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (uint32_t t = 0u; t < kThreads; ++t) {
            workers.emplace_back([&, t]() {
                for (uint32_t i = 0u; i < kIters; ++i) {
                    const uint32_t prod_sel = 1u + ((t + i) % 26u);
                    const uint32_t leg_sel = A840_KMD_LEGACY_SELECTOR_TURNIP_DRM_SUBMIT + ((t + i) % 6u);
                    CHECK(a840_kmd_v3_freeze_policy_check_selector(&policy, prod_sel) == A840_KMD_OK);
                    CHECK(a840_kmd_v3_freeze_policy_check_selector(&policy, leg_sel) == A840_KMD_UNSUPPORTED);
                    CHECK(a840_kmd_v3_freeze_policy_check_flags(&policy, A840_KMD_V3_PROD_CAP_MASK) == A840_KMD_OK);
                    CHECK(a840_kmd_v3_freeze_policy_check_flags(&policy, A840_KMD_LEGACY_FLAG_TURNIP_SHIM) == A840_KMD_UNSUPPORTED);
                    A840KmdV3FreezeStats st{};
                    CHECK(a840_kmd_v3_freeze_policy_get_stats(&policy, &st) == A840_KMD_OK);
                }
            });
        }
        for (auto &w : workers) w.join();

        A840KmdV3FreezeStats final_st{};
        CHECK(a840_kmd_v3_freeze_policy_get_stats(&policy, &final_st) == A840_KMD_OK);
        const uint64_t expected = static_cast<uint64_t>(kThreads) * kIters;
        CHECK(final_st.selector_checks_passed == expected);
        CHECK(final_st.legacy_selector_rejects == expected);
        CHECK(final_st.flag_checks_passed == expected);
        CHECK(final_st.legacy_flag_rejects == expected);
        CHECK(final_st.legacy_rejected_count == expected * 2u);
        CHECK(a840_kmd_v3_legacy_rejected_count() == expected * 2u);
        a840_kmd_v3_freeze_policy_destroy(&policy);
    }
}

void case58_tsan_8thread_preempt_wakeup_dcvs_mdss_stress()
{
    begin_case("case58_tsan_8thread_preempt_wakeup_dcvs_mdss_stress");
    for (uint32_t round = 0u; round < g_stress_rounds; ++round) {
        auto wake = std::make_unique<A840KmdWakeupEngine>();
        auto sched = std::make_unique<A840KmdPreemptScheduler>();
        auto gov = std::make_unique<A840KmdGmuDcvs>();
        auto mdss = std::make_unique<A840KmdMdssSync>();

        CHECK(a840_kmd_wakeup_init(wake.get(), nullptr) == A840_KMD_OK);
        CHECK(a840_gmu_dcvs_init(gov.get(), nullptr) == A840_KMD_OK);

        A840PreemptHwOps hw_ops{};
        hw_ops.cookie = wake.get();
        hw_ops.on_pi_boost_change = &a840_kmd_wakeup_bridge_pi_boost;
        hw_ops.on_latency_sample = &a840_kmd_wakeup_bridge_latency;

        A840PreemptConfig pcfg{};
        pcfg.hw_ops = &hw_ops;
        for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
            pcfg.rings[r].preempt_record_va = UINT64_C(0x20000000) + static_cast<uint64_t>(r) * UINT64_C(0x40000);
            pcfg.rings[r].preempt_record_bytes = UINT64_C(0x20000);
            pcfg.rings[r].preempt_level = A840_PREEMPT_LEVEL_L1A_BIN_BOUNDARY;
        }
        CHECK(a840_kmd_preempt_init(sched.get(), &pcfg) == A840_KMD_OK);

        A840MdssSyncCallbacks cbs{};
        cbs.cookie = gov.get();
        cbs.on_boost_hint = &a840_gmu_dcvs_bridge_mdss_boost;
        A840MdssSyncConfig mcfg{};
        mcfg.initial_vsync_ns = UINT64_C(100000000);
        mcfg.has_initial_vsync = 1u;
        mcfg.callbacks = &cbs;
        CHECK(a840_mdss_sync_init(mdss.get(), &mcfg) == A840_KMD_OK);

        std::atomic<uint64_t> clk{UINT64_C(100000000)};
        constexpr uint32_t kIters = 30u;
        std::vector<std::thread> threads;

        for (uint32_t r = 0u; r < A840_RING_COUNT; ++r) {
            threads.emplace_back([&, r]() {
                for (uint32_t i = 0u; i < kIters; ++i) {
                    const uint64_t now = clk.fetch_add(UINT64_C(200000), std::memory_order_relaxed);
                    A840PreemptSubmitReq req{};
                    req.ring_id = r;
                    req.enqueue_ns = now;
                    A840PreemptSubmitResult res{};
                    const A840KmdStatus st = a840_kmd_preempt_submit(sched.get(), &req, &res);
                    CHECK(st == A840_KMD_OK || st == A840_KMD_CAPACITY);
                    if (st == A840_KMD_OK) {
                        (void)a840_kmd_preempt_on_ring_retire(sched.get(), r, res.seqno, now + 50000u);
                        uint32_t woken = 0u;
                        (void)a840_kmd_wakeup_on_irq(wake.get(), r, res.seqno, now + 60000u, &woken);
                    }
                }
            });
        }
        threads.emplace_back([&]() {
            for (uint32_t i = 0u; i < kIters; ++i) {
                const uint64_t now = clk.fetch_add(UINT64_C(100000), std::memory_order_relaxed);
                (void)a840_kmd_preempt_on_hw_in_progress(sched.get(), now);
                (void)a840_kmd_preempt_on_switch_done(sched.get(), now + 20000u);
                (void)a840_kmd_preempt_tick(sched.get(), now + 40000u);
            }
        });
        threads.emplace_back([&]() {
            for (uint32_t i = 0u; i < kIters; ++i) {
                const uint64_t now = UINT64_C(100000000) + static_cast<uint64_t>(i + 1u) * UINT64_C(8333333);
                A840DcvsFrameSample fs{};
                fs.now_ns = now;
                fs.gpu_busy_ns = UINT64_C(6500000);
                fs.ring0_submits = 2u;
                (void)a840_gmu_dcvs_record_frame(gov.get(), &fs);
                (void)a840_gmu_dcvs_update_thermal(gov.get(), (i % 2u == 0u) ? 45000 : 80000, now);
            }
        });
        threads.emplace_back([&]() {
            for (uint32_t i = 0u; i < kIters; ++i) {
                const uint64_t vs = UINT64_C(100000000) + static_cast<uint64_t>(i + 1u) * A840_MDSS_PERIOD_120HZ_NS;
                (void)a840_mdss_sync_on_vsync(mdss.get(), vs, i + 1u);
                A840MdssCommitRequest creq{};
                creq.frame_id = i + 1u;
                creq.now_ns = vs + UINT64_C(4000000);
                creq.ring0_compositor = 1u;
                A840MdssCommitPlan plan{};
                CHECK(a840_mdss_sync_plan_commit(mdss.get(), &creq, &plan) == A840_KMD_OK);
            }
        });
        threads.emplace_back([&]() {
            for (uint32_t i = 0u; i < kIters; ++i) {
                A840PreemptSnapshot ps{};
                A840WakeupSnapshot ws{};
                A840GmuDcvsSnapshot gs{};
                A840MdssSyncStats ms{};
                CHECK(a840_kmd_preempt_get_snapshot(sched.get(), &ps) == A840_KMD_OK);
                CHECK(a840_kmd_wakeup_get_snapshot(wake.get(), &ws) == A840_KMD_OK);
                CHECK(a840_gmu_dcvs_get_snapshot(gov.get(), &gs) == A840_KMD_OK);
                CHECK(a840_mdss_sync_get_stats(mdss.get(), &ms) == A840_KMD_OK);
            }
        });

        for (auto &t : threads) t.join();
        a840_mdss_sync_destroy(mdss.get());
        a840_gmu_dcvs_destroy(gov.get());
        a840_kmd_preempt_destroy(sched.get());
        a840_kmd_wakeup_destroy(wake.get());
    }
}

void case59_tsan_8thread_glue_dispatch_and_legacy_rejection()
{
    begin_case("case59_tsan_8thread_glue_dispatch_and_legacy_rejection");
    for (uint32_t round = 0u; round < g_stress_rounds; ++round) {
        GlueMockEnv env;
        NativeAdrenoV3Glue glue;
        CHECK(glue.init(env.make_config(false)) == kIOReturnSuccess);

        constexpr uint32_t kClients = 8u;
        GlueMockClient clients[kClients];
        for (uint32_t i = 0u; i < kClients; ++i) {
            clients[i].id = i + 1u;
            clients[i].inject_legacy_v2_flags = static_cast<uint32_t>(A840_KMD_LEGACY_FLAG_TURNIP_SHIM);
            CHECK(glue.openClient(&clients[i], nullptr, nullptr) == kIOReturnSuccess);
        }

        std::vector<std::thread> workers;
        for (uint32_t i = 0u; i < kClients; ++i) {
            workers.emplace_back([&, i]() {
                for (uint32_t iter = 0u; iter < 25u; ++iter) {
                    NativeAdrenoV2ParamsWire p{};
                    CHECK(glue.externalMethod(&clients[i], NativeAdrenoV2MethodQuery,
                                              nullptr, 0u, &p, sizeof(p)) == kIOReturnSuccess);
                    CHECK(p.abi_version == NativeAdrenoAbiVersionV3);
                    CHECK((p.flags & static_cast<uint32_t>(A840_KMD_LEGACY_FLAG_TURNIP_SHIM)) == 0u);

                    NativeAdrenoAbiZeroPageReplyV3 zp{};
                    CHECK(glue.externalMethod(&clients[i], NativeAdrenoMethodGetZeroPageV3,
                                              nullptr, 0u, &zp, sizeof(zp)) == kIOReturnSuccess);
                    CHECK(zp.bytes == 65536u);

                    uint8_t dummy[16] = {};
                    CHECK(glue.externalMethod(&clients[i], A840_KMD_LEGACY_SELECTOR_TURNIP_SYNCOBJ,
                                              dummy, sizeof(dummy), dummy, sizeof(dummy)) == kIOReturnUnsupported);
                }
            });
        }
        for (auto &w : workers) w.join();
        glue.destroy();
    }
}

void case60_tsan_8thread_concurrent_qualify_positive_and_negative()
{
    begin_case("case60_tsan_8thread_concurrent_qualify_positive_and_negative");
    for (uint32_t round = 0u; round < g_stress_rounds; ++round) {
        A840KmdV3FreezePolicy policy{};
        CHECK(a840_kmd_v3_freeze_policy_init(&policy, nullptr) == A840_KMD_OK);

        constexpr uint32_t kThreads = 8u;
        constexpr uint32_t kIters = 40u;
        std::vector<std::thread> workers;
        for (uint32_t t = 0u; t < kThreads; ++t) {
            workers.emplace_back([&, t]() {
                A840KmdProdQualInput in = make_valid_prod_input();
                const bool make_fail = (t % 2u == 1u);
                if (make_fail)
                    in.legacy_turnip_shim_flags = A840_KMD_LEGACY_FLAG_TURNIP_COMPAT_BRIDGE;

                for (uint32_t i = 0u; i < kIters; ++i) {
                    A840KmdProdQualReport rep{};
                    const A840KmdStatus st = a840_kmd_v3_freeze_policy_qualify(&policy, &in, &rep);
                    if (!make_fail) {
                        CHECK(st == A840_KMD_OK && rep.qualified == 1u &&
                              rep.passed_mask == A840_KMD_QUAL_ALL_MASK &&
                              rep.failed_mask == 0u);
                    } else {
                        CHECK(st == A840_KMD_UNSUPPORTED && rep.qualified == 0u &&
                              rep.turnip_paths_purged == 0u &&
                              rep.failed_mask == A840_KMD_QUAL_ZERO_LEGACY_TURNIP);
                    }
                }
            });
        }
        for (auto &w : workers) w.join();

        A840KmdV3FreezeStats st{};
        CHECK(a840_kmd_v3_freeze_policy_get_stats(&policy, &st) == A840_KMD_OK);
        CHECK(st.qualify_pass_count == 4u * kIters);
        CHECK(st.qualify_fail_count == 4u * kIters);
        a840_kmd_v3_freeze_policy_destroy(&policy);
    }
}

} /* namespace */

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tsan-stress") == 0 && i + 1 < argc) {
            const long v = std::strtol(argv[++i], nullptr, 10);
            if (v >= 1 && v <= 1000)
                g_stress_rounds = static_cast<uint32_t>(v);
        }
    }

    case01_abi_freeze_info_constants();
    case02_abi_struct_sizes_and_offsets();
    case03_method_numbers_10_to_26_locked();
    case04_capability_flags_disjoint();
    case05_abi_freeze_info_null_safe();
    case06_chip_facts_kaanapali_default_matches();
    case07_chip_facts_rejects_mesa_wildcard();
    case08_selector_whitelist_accepts_1_to_26();
    case09_selector_whitelist_rejects_zero_and_legacy();
    case10_validate_production_selector_counter();

    case11_flags_accepts_production_caps_only();
    case12_flags_rejects_all_explicit_legacy_turnip_bits();
    case13_flags_exhaustive_64bit_fail_closed_sweep();
    case14_flags_mixed_prod_and_legacy_rejected();
    case15_policy_instance_lifecycle_and_custom_sync();
    case16_policy_instance_counters_accurate();
    case17_glue_augment_query_strips_legacy_turnip_bits();
    case18_glue_rejects_all_legacy_turnip_selectors();

    case19_qualify_production_all_pass();
    case20_qualify_null_and_reserved_fail_closed();
    case21_qualify_rejects_wrong_abi_version_and_envelope();
    case22_qualify_rejects_missing_required_v3_cap();
    case23_qualify_rejects_each_chip_identity_mismatch();
    case24_qualify_rejects_unready_or_writable_zero_page();
    case25_qualify_rejects_bad_zero_page_geometry();
    case26_qualify_rejects_backboardd_ca_policy_gaps();
    case27_qualify_rejects_bad_preempt_ring_count_or_alignment();
    case28_qualify_rejects_overlapping_preempt_records_and_allows_adjacent();
    case29_qualify_rejects_corrupted_gpu_opp_table();
    case30_qualify_rejects_corrupted_gmu_opp_table();
    case31_qualify_rejects_inactive_or_wrong_thermal_ladder();
    case32_qualify_rejects_legacy_turnip_shim_flags();
    case33_qualify_rejects_legacy_turnip_paths_present();
    case34_qualify_rejects_legacy_bit_in_advertised_caps();
    case35_qualify_rejects_legacy_selector_in_audit_list();
    case36_qualify_multi_domain_orthogonal_bitmasks();
    case37_policy_qualify_tracks_pass_and_fail_counts();

    case38_backport_ib_diag_hex_formatting();
    case39_backport_ib_diag_valid_and_blocked_reg();
    case40_backport_wait_any_blocking_and_timeout();
    case41_backport_wait_any_poison_and_disconnect();
    case42_backport_glue_zero_page_wait_any_lifecycle();
    case43_backport_glue_submit_audit_and_channel_isolation();
    case44_backport_glue_fault_snapshot_telemetry();

    case45_e2e_zero_page_and_sparse_va_integration();
    case46_e2e_ubwc6_writer_promise_display_handoff();
    case47_e2e_ca_env_policy_feeds_production_qualification();
    case48_e2e_bo_pool_and_quota_recovery_integration();
    case49_e2e_preempt_4ring_and_wakeup_latency_bridge();
    case50_e2e_gmu_dcvs_and_mdss_120hz_closed_loop();
    case51_e2e_live_subsystems_pass_production_qualification();
    case52_e2e_fault_isolation_ring2_never_stalls_ring0();
    case53_e2e_zero_page_and_sparse_write_permission_rejected();
    case54_e2e_ubwc_fallback_linear_when_mdss_lacks_ubwc6();
    case55_e2e_quota_compositor_reserve_under_pressure();
    case56_e2e_wait_any_preempt_mdss_120hz_budget();

    case57_tsan_8thread_freeze_policy_and_atomic_audit();
    case58_tsan_8thread_preempt_wakeup_dcvs_mdss_stress();
    case59_tsan_8thread_glue_dispatch_and_legacy_rejection();
    case60_tsan_8thread_concurrent_qualify_positive_and_negative();

    const uint64_t total_checks = g_checks.load(std::memory_order_relaxed);
    const uint64_t total_failures = g_failures.load(std::memory_order_relaxed);

    if (total_failures != 0u) {
        std::fprintf(stderr,
                     "FAIL test_kmd_phase6_release_freeze: %u cases, %" PRIu64
                     " checks, %" PRIu64 " failures (stress_rounds=%u)\n",
                     g_cases, total_checks, total_failures, g_stress_rounds);
        return 1;
    }

    std::printf("PASS test_kmd_phase6_release_freeze: %u independent cases, %" PRIu64
                " checks (stress_rounds=%u)\n",
                g_cases, total_checks, g_stress_rounds);
    return 0;
}
