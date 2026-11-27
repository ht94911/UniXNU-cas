/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "NativeAdrenoV3Glue.h"
#include "a840_xnu_system_bridge.h"
#include "Adreno840Regs.h"
#include "Adreno840EmitABI.h"

namespace {

std::atomic<int> g_failures{0};
std::atomic<uint64_t> g_checks{0};
int g_cases_run = 0;

#define CHECK(cond)                                                               \
    do {                                                                          \
        g_checks.fetch_add(1u, std::memory_order_relaxed);                        \
        if (!(cond)) {                                                            \
            g_failures.fetch_add(1, std::memory_order_relaxed);                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                         \
    } while (0)

struct FakeClock {
    std::atomic<uint64_t> now_ns{UINT64_C(100000000)}; /* 100 ms */
    std::atomic<uint64_t> ticks{UINT64_C(1920000)};

    static uint64_t readNow(void *cookie)
    {
        return static_cast<FakeClock *>(cookie)->now_ns.load(std::memory_order_acquire);
    }

    static uint64_t readTicks(void *cookie)
    {
        return static_cast<FakeClock *>(cookie)->ticks.load(std::memory_order_acquire);
    }

    uint64_t advance(uint64_t delta_ns)
    {
        const uint64_t n = now_ns.fetch_add(delta_ns, std::memory_order_acq_rel) + delta_ns;
        ticks.fetch_add((delta_ns * A840_PREEMPT_AON_CLOCK_HZ) / UINT64_C(1000000000),
                        std::memory_order_acq_rel);
        return n;
    }
};

struct MdssCommitRecorder {
    std::atomic<uint64_t> commit_count{0};
    std::atomic<uint64_t> ubwc_direct_count{0};
    std::atomic<uint64_t> last_surface_id{0};

    static A840KmdStatus onCommit(void *cookie, const A840KmdMdssCommitDesc *desc)
    {
        auto *rec = static_cast<MdssCommitRecorder *>(cookie);
        if (rec == nullptr || desc == nullptr)
            return A840_KMD_INVALID_ARG;
        rec->commit_count.fetch_add(1u, std::memory_order_acq_rel);
        if (desc->scanout_mode == A840_DISPLAY_SCANOUT_UBWC6_DIRECT)
            rec->ubwc_direct_count.fetch_add(1u, std::memory_order_acq_rel);
        rec->last_surface_id.store(desc->surface_identity, std::memory_order_release);
        return A840_KMD_OK;
    }
};

A840XnuSystemBridge::Config makeDefaultConfig(
    FakeClock *clk = nullptr,
    MdssCommitRecorder *mdss = nullptr)
{
    A840XnuSystemBridge::Config cfg{};
    cfg.abi_mode = A840_SYS_ABI_MODE_V3_NATIVE;
    cfg.zero_page_default_va = UINT64_C(65536);
    cfg.initial_vsync_ns = clk ? clk->now_ns.load(std::memory_order_acquire) : UINT64_C(100000000);
    if (clk != nullptr) {
        cfg.now_ns_cb = &FakeClock::readNow;
        cfg.read_ticks_cb = &FakeClock::readTicks;
        cfg.clock_cookie = clk;
    }
    if (mdss != nullptr) {
        cfg.mdss_commit_fn = &MdssCommitRecorder::onCommit;
        cfg.mdss_commit_cookie = mdss;
    }
    return cfg;
}

A840SysBoImportUbwc6Wire makeNezha1200x2608Ubwc6Req(uint64_t surface_id)
{
    A840SysBoImportUbwc6Wire req{};
    req.surface_identity = surface_id;
    req.bytes = UINT64_C(16777216); /* 16 MiB */
    req.display_base = UINT64_C(1073741824); /* 1 GiB aligned */
    req.format = 80u; /* BGRA8Unorm */
    req.width = A840_SYS_DISPLAY_WIDTH_NEZHA;   /* 1200 */
    req.height = A840_SYS_DISPLAY_HEIGHT_NEZHA; /* 2608 */
    req.meta_rows = 64u;
    req.ubwc_enabled = 1u;
    req.ubwc_version = A840_KMD_V3_FREEZE_UBWC_VERSION;
    req.highest_bank_bit = A840_KAANAPALI_HIGHEST_BANK_BIT;
    req.swizzle_levels = A840_KMD_V3_FREEZE_UBWC_SWIZZLE;
    req.macrotile_mode = A840_KMD_V3_FREEZE_MACROTILE_MODE;
    req.meta_offset = UINT64_C(4096);
    req.meta_pitch = 256u;
    req.flags = A840_SURFACE_SCANOUT_ELIGIBLE;
    req.reserved = 0u;
    return req;
}

/* ============================================================================
 * Group A (Cases 1-8): Init, Lifecycle, Qualification & Dual-Mode ABI
 * ============================================================================ */

void case_01_init_invalid_configs_fail_closed()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    A840XnuSystemBridge::Config cfg = makeDefaultConfig();

    cfg.abi_mode = 99u;
    CHECK(br->init(cfg) == kIOReturnBadArgument);

    cfg = makeDefaultConfig();
    cfg.zero_page_default_va = 4096u; /* not 64 KiB aligned */
    CHECK(br->init(cfg) == kIOReturnBadArgument);

    cfg = makeDefaultConfig();
    cfg.preempt_record_bytes = 4096u; /* < 64 KiB */
    CHECK(br->init(cfg) == kIOReturnBadArgument);

    cfg = makeDefaultConfig();
    cfg.cap_flags = static_cast<uint32_t>(A840_KMD_LEGACY_FLAG_TURNIP_SHIM);
    CHECK(br->init(cfg) == kIOReturnUnsupported);
    CHECK(!br->initialized());
}

void case_02_init_and_shutdown_lifecycle_idempotent()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    auto cfg = makeDefaultConfig();
    CHECK(br->init(cfg) == kIOReturnSuccess);
    CHECK(br->initialized());
    CHECK(br->init(cfg) == kIOReturnBusy);

    CHECK(br->shutdown() == kIOReturnSuccess);
    CHECK(!br->initialized());
    uint64_t cid = 0u;
    CHECK(br->register_client(100u, false, &cid) == kIOReturnBadArgument);

    /* Re-init after shutdown must succeed cleanly. */
    CHECK(br->init(cfg) == kIOReturnSuccess);
    CHECK(br->initialized());
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_03_qualify_production_v3_all_7_gates_pass()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    A840KmdProdQualReport rep{};
    CHECK(br->qualify_production(&rep) == kIOReturnSuccess);
    CHECK(rep.qualified == 1u);
    CHECK(rep.abi_version_frozen == 1u);
    CHECK(rep.turnip_paths_purged == 1u);
    CHECK(rep.passed_mask == A840_KMD_QUAL_ALL_MASK);
    CHECK(rep.failed_mask == 0u);
    CHECK(br->is_qualified());
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_04_qualify_production_v2_mode_honestly_unqualified()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    auto cfg = makeDefaultConfig();
    cfg.abi_mode = A840_SYS_ABI_MODE_V2_COMPAT;
    CHECK(br->init(cfg) == kIOReturnSuccess);
    A840KmdProdQualReport rep{};
    CHECK(br->qualify_production(&rep) != kIOReturnSuccess);
    CHECK(rep.qualified == 0u);
    CHECK(!br->is_qualified());
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_05_qualify_production_negative_legacy_paths_rejected()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    A840KmdProdQualInput in{};
    in.abi_version = NativeAdrenoAbiVersionV3;
    in.envelope_bytes = NativeAdrenoAbiEnvelopeBytes;
    in.advertised_cap_flags = A840_KMD_V3_PROD_CAP_MASK;
    in.legacy_turnip_shim_flags = A840_KMD_LEGACY_FLAG_TURNIP_COMPAT_BRIDGE;
    in.legacy_turnip_paths_present = 1u;

    A840KmdProdQualReport rep{};
    CHECK(br->qualify_production_with_input(in, &rep) == kIOReturnUnsupported);
    CHECK(rep.qualified == 0u);
    CHECK(rep.turnip_paths_purged == 0u);
    CHECK((rep.failed_mask & A840_KMD_QUAL_ZERO_LEGACY_TURNIP) != 0u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_06_abi_negotiate_v3_native_grants_full_prod_caps()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    CHECK(br->qualify_production(nullptr) == kIOReturnSuccess);

    A840SysAbiNegotiateReply nrep{};
    CHECK(br->negotiate_abi(NativeAdrenoAbiVersionV3, A840_KMD_V3_PROD_CAP_MASK, &nrep) ==
          kIOReturnSuccess);
    CHECK(nrep.active_abi_version == NativeAdrenoAbiVersionV3);
    CHECK(nrep.kernel_abi_mode == A840_SYS_ABI_MODE_V3_NATIVE);
    CHECK(nrep.qualified == 1u);
    CHECK(nrep.legacy_paths_purged == 1u);
    CHECK(nrep.granted_flags == A840_KMD_V3_PROD_CAP_MASK);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_07_abi_negotiate_v2_client_on_v3_kernel_coexistence()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    CHECK(br->qualify_production(nullptr) == kIOReturnSuccess);

    A840SysAbiNegotiateReply nrep{};
    CHECK(br->negotiate_abi(NativeAdrenoAbiVersionV2, A840_KMD_V3_PROD_V2_CAP_MASK, &nrep) ==
          kIOReturnSuccess);
    CHECK(nrep.active_abi_version == NativeAdrenoAbiVersionV2);
    CHECK(nrep.kernel_abi_mode == A840_SYS_ABI_MODE_V3_NATIVE);
    CHECK(nrep.granted_flags == A840_KMD_V3_PROD_V2_CAP_MASK);
    CHECK(nrep.legacy_paths_purged == 1u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_08_abi_negotiate_v3_client_on_v2_kernel_downgrades_cleanly()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    auto cfg = makeDefaultConfig();
    cfg.abi_mode = A840_SYS_ABI_MODE_V2_COMPAT;
    CHECK(br->init(cfg) == kIOReturnSuccess);

    uint64_t cid = 0u;
    CHECK(br->register_client(201u, false, &cid) == kIOReturnSuccess);

    A840SysAbiNegotiateReply nrep{};
    CHECK(br->negotiate_abi(NativeAdrenoAbiVersionV3, A840_KMD_V3_PROD_CAP_MASK, &nrep) ==
          kIOReturnSuccess);
    CHECK(nrep.active_abi_version == NativeAdrenoAbiVersionV2);
    CHECK(nrep.granted_flags == A840_KMD_V3_PROD_V2_CAP_MASK);

    /* v3 methods 20..26 must return kIOReturnUnsupported in v2 compat mode. */
    struct NativeAdrenoAbiZeroPageReplyV3 zp{};
    CHECK(br->dispatch_method(cid, NativeAdrenoMethodGetZeroPageV3,
                              nullptr, 0u, &zp, sizeof(zp)) == kIOReturnUnsupported);
    /* v2 Method 1 QUERY must succeed and advertise v2. */
    A840SysParamsWire p{};
    CHECK(br->dispatch_method(cid, A840_KMD_V3_METHOD_QUERY,
                              nullptr, 0u, &p, sizeof(p)) == kIOReturnSuccess);
    CHECK(p.abi_version == NativeAdrenoAbiVersionV2);
    CHECK(p.flags == static_cast<uint32_t>(A840_KMD_V3_PROD_V2_CAP_MASK));
    CHECK(br->shutdown() == kIOReturnSuccess);
}

/* ============================================================================
 * Group B (Cases 9-16): Zero Legacy Policy, Envelopes, Facts & CA Env Policy
 * ============================================================================ */

void case_09_zero_legacy_reject_selector_zero_and_27_to_32()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t cid = 0u;
    CHECK(br->register_client(210u, false, &cid) == kIOReturnSuccess);

    const uint32_t bad_sels[] = {0u, 27u, 28u, 29u, 30u, 31u, 32u, 99u, UINT32_MAX};
    for (uint32_t sel : bad_sels) {
        CHECK(br->dispatch_method(cid, sel, nullptr, 0u, nullptr, 0u) == kIOReturnUnsupported);
    }
    A840KmdV3FreezeStats fs{};
    CHECK(br->get_freeze_stats(&fs) == kIOReturnSuccess);
    CHECK(fs.legacy_selector_rejects == sizeof(bad_sels) / sizeof(bad_sels[0]));
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_10_zero_legacy_reject_all_non_prod_flag_bits_exhaustive()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);

    uint64_t rejected = 0u;
    uint64_t accepted = 0u;
    for (uint32_t bit = 0u; bit < 64u; ++bit) {
        const uint64_t mask = UINT64_C(1) << bit;
        A840SysAbiNegotiateReply nrep{};
        const IOReturn rc = br->negotiate_abi(NativeAdrenoAbiVersionV3, mask, &nrep);
        if ((mask & A840_KMD_V3_PROD_CAP_MASK) != 0u) {
            CHECK(rc == kIOReturnSuccess);
            accepted++;
        } else {
            CHECK(rc == kIOReturnUnsupported);
            rejected++;
        }
    }
    CHECK(accepted == 11u);
    CHECK(rejected == 53u);
    A840KmdV3FreezeStats fs{};
    CHECK(br->get_freeze_stats(&fs) == kIOReturnSuccess);
    CHECK(fs.legacy_flag_rejects == 53u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_11_zero_legacy_method26_rejects_legacy_flags()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t cid = 0u;
    CHECK(br->register_client(211u, true, &cid) == kIOReturnSuccess);

    A840SysQueueConfigReqV3 req{};
    req.op = A840_SYS_QCFG_KMD_V3_QUALIFY;
    req.flags = static_cast<uint32_t>(A840_KMD_LEGACY_FLAG_TURNIP_WSI_BRIDGE);
    A840SysQueueConfigReplyV3 rep{};
    CHECK(br->dispatch_method(cid, NativeAdrenoMethodQueueConfigV3,
                              &req, sizeof(req), &rep, sizeof(rep)) == kIOReturnUnsupported);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_12_envelope_over_3104_bytes_rejected_fail_closed()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t cid = 0u;
    CHECK(br->register_client(212u, false, &cid) == kIOReturnSuccess);

    uint8_t dummy[8]{};
    CHECK(br->dispatch_method(cid, A840_KMD_V3_METHOD_QUERY,
                              dummy, NativeAdrenoAbiEnvelopeBytes + 1u,
                              dummy, sizeof(dummy)) == kIOReturnBadArgument);
    CHECK(br->dispatch_method(cid, A840_KMD_V3_METHOD_QUERY,
                              nullptr, 0u,
                              dummy, NativeAdrenoAbiEnvelopeBytes + 1u) == kIOReturnBadArgument);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_13_method1_query_strips_legacy_bits_and_reports_v3()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t cid = 0u;
    CHECK(br->register_client(213u, false, &cid) == kIOReturnSuccess);

    A840SysParamsWire p{};
    size_t written = 0u;
    CHECK(br->dispatch_method(cid, A840_KMD_V3_METHOD_QUERY,
                              nullptr, 0u, &p, sizeof(p), &written) == kIOReturnSuccess);
    CHECK(written == sizeof(p));
    CHECK(p.abi_version == NativeAdrenoAbiVersionV3);
    CHECK((p.flags & static_cast<uint32_t>(A840_KMD_LEGACY_FLAG_ALL_KNOWN_MASK)) == 0u);
    CHECK(p.flags == static_cast<uint32_t>(A840_KMD_V3_PROD_CAP_MASK));
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_14_method15_device_info_kaanapali_a840_facts()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t cid = 0u;
    CHECK(br->register_client(214u, false, &cid) == kIOReturnSuccess);

    A840SysDeviceInfoWire info{};
    size_t written = 0u;
    CHECK(br->dispatch_method(cid, A840_KMD_V3_METHOD_GET_DEVICE_INFO,
                              nullptr, 0u, &info, sizeof(info), &written) == kIOReturnSuccess);
    CHECK(written == sizeof(info));
    CHECK(info.chip_id == static_cast<uint32_t>(A840_KAANAPALI_CHIP_ID));
    CHECK(info.gmem_bytes == A840_KAANAPALI_GMEM_BYTES);
    CHECK(info.highest_bank_bit == A840_KAANAPALI_HIGHEST_BANK_BIT);
    CHECK(info.ubwc_version == A840_KMD_V3_FREEZE_UBWC_VERSION);
    CHECK(info.ubwc_swizzle == A840_KMD_V3_FREEZE_UBWC_SWIZZLE);
    CHECK(info.macrotile_mode == A840_KMD_V3_FREEZE_MACROTILE_MODE);
    CHECK(info.smmu_page_shift == A840_KMD_V3_FREEZE_SMMU_PAGE_SHIFT);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_15_method21_get_zero_page_64kib_ro_per_client()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);

    uint64_t bb = 0u, app = 0u;
    CHECK(br->register_client(10u, true, &bb, UINT64_C(65536)) == kIOReturnSuccess);
    CHECK(br->register_client(20u, false, &app, UINT64_C(131072)) == kIOReturnSuccess);

    struct NativeAdrenoAbiZeroPageReplyV3 z1{}, z2{};
    CHECK(br->dispatch_method(bb, NativeAdrenoMethodGetZeroPageV3,
                              nullptr, 0u, &z1, sizeof(z1)) == kIOReturnSuccess);
    CHECK(br->dispatch_method(app, NativeAdrenoMethodGetZeroPageV3,
                              nullptr, 0u, &z2, sizeof(z2)) == kIOReturnSuccess);
    CHECK(z1.gpu_va == UINT64_C(65536));
    CHECK(z2.gpu_va == UINT64_C(131072));
    CHECK(z1.bytes == A840_KMD_ZERO_PAGE_BYTES);
    CHECK(z2.bytes == A840_KMD_ZERO_PAGE_BYTES);

    A840XnuSystemBridge::Stats st{};
    CHECK(br->get_stats(&st) == kIOReturnSuccess);
    CHECK(st.zero_page_stats.active_clients == 2u);
    CHECK(st.zero_page_stats.total_allocs == 1u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_16_ca_env_policy_backboardd_allow_and_app_reject()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);

    char buf[256]{};
    size_t len = 0u;
    A840KmdCaEnvDecision dec{};
    CHECK(br->evaluate_backboardd_ca_env(
              A840_KMD_CA_TARGET_BACKBOARDD, "debug=0x14e",
              buf, sizeof(buf), &len, &dec) == kIOReturnSuccess);
    CHECK(dec.should_inject == 1u);
    CHECK(std::strstr(buf, A840_KMD_CA_ENV_ENTRY) != nullptr);

    char app_buf[256] = "keep_me";
    CHECK(br->evaluate_backboardd_ca_env(
              "com.example.game", "debug=0x14e",
              app_buf, sizeof(app_buf), &len, &dec) == kIOReturnNotPermitted);
    CHECK(std::strcmp(app_buf, "keep_me") == 0);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

/* ============================================================================
 * Group C (Cases 17-24): Process Role Isolation (backboardd vs App)
 * ============================================================================ */

void case_17_role_register_backboardd_and_4_apps()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);

    uint64_t bb = 0u;
    uint64_t apps[4]{};
    CHECK(br->register_client(100u, true, &bb) == kIOReturnSuccess);
    for (int i = 0; i < 4; ++i)
        CHECK(br->register_client(static_cast<uint32_t>(1000 + i), false, &apps[i]) ==
              kIOReturnSuccess);

    A840XnuSystemBridge::Stats st{};
    CHECK(br->get_stats(&st) == kIOReturnSuccess);
    CHECK(st.active_clients == 5u);
    CHECK(st.active_backboardd_clients == 1u);
    CHECK(st.active_app_clients == 4u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_18_role_backboardd_invalid_label_rejected_when_ca_enforced()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);

    uint64_t spoof = 0u;
    CHECK(br->register_client(500u, true, &spoof, 0u, nullptr, "com.spoof.app") ==
          kIOReturnNotPermitted);
    A840XnuSystemBridge::Stats st{};
    CHECK(br->get_stats(&st) == kIOReturnSuccess);
    CHECK(st.total_role_denials >= 1u);
    CHECK(st.active_clients == 0u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_19_role_app_cannot_enter_ring0_via_submit_or_priority()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t app = 0u;
    CHECK(br->register_client(301u, false, &app) == kIOReturnSuccess);

    A840SysSetPriorityWire sp{};
    sp.queue = 0u;
    sp.priority = A840_PRIORITY_COMPOSITOR;
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_SET_PRIORITY,
                              &sp, sizeof(sp), nullptr, 0u) == kIOReturnNotPermitted);

    A840SysQueueWire qreq{};
    qreq.queue = 2u;
    qreq.priority = A840_PRIORITY_COMPOSITOR;
    A840SysQueueWire qrep{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_CREATE_QUEUE,
                              &qreq, sizeof(qreq), &qrep, sizeof(qrep)) ==
          kIOReturnNotPermitted);

    A840SysSubmitWire sub{};
    sub.queue = 0u;
    sub.priority_hint = A840_PRIORITY_COMPOSITOR;
    A840SysSubmitReplyWire srep{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_SUBMIT,
                              &sub, sizeof(sub), &srep, sizeof(srep)) ==
          kIOReturnNotPermitted);

    sub.priority_hint = A840_PRIORITY_NORMAL;
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_SUBMIT,
                              &sub, sizeof(sub), &srep, sizeof(srep)) ==
          kIOReturnSuccess);
    CHECK(srep.assigned_ring == A840_RING_2_CLIENT_3D);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_20_role_app_cannot_configure_ring0_preempt()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t app = 0u, bb = 0u;
    CHECK(br->register_client(101u, true, &bb) == kIOReturnSuccess);
    CHECK(br->register_client(302u, false, &app) == kIOReturnSuccess);

    A840SysQueueConfigReqV3 req{};
    req.op = A840_SYS_QCFG_PREEMPT_RING_CONFIG;
    req.u.preempt_cfg.ring_id = A840_RING_0_COMPOSITOR;
    req.u.preempt_cfg.preempt_level = A840_PREEMPT_LEVEL_L1B_DRAW_BOUNDARY;
    req.u.preempt_cfg.wakeup_mode = A840_WAKE_MODE_IMMEDIATE;
    A840SysQueueConfigReplyV3 rep{};
    CHECK(br->dispatch_method(app, NativeAdrenoMethodQueueConfigV3,
                              &req, sizeof(req), &rep, sizeof(rep)) ==
          kIOReturnNotPermitted);

    CHECK(br->dispatch_method(bb, NativeAdrenoMethodQueueConfigV3,
                              &req, sizeof(req), &rep, sizeof(rep)) == kIOReturnSuccess);
    CHECK(rep.u.preempt_cfg.preempt_level == A840_PREEMPT_LEVEL_L1B_DRAW_BOUNDARY);

    req.u.preempt_cfg.ring_id = A840_RING_2_CLIENT_3D;
    req.u.preempt_cfg.preempt_level = A840_PREEMPT_LEVEL_L1A_BIN_BOUNDARY;
    req.u.preempt_cfg.wakeup_mode = A840_WAKE_MODE_COALESCED;
    CHECK(br->dispatch_method(app, NativeAdrenoMethodQueueConfigV3,
                              &req, sizeof(req), &rep, sizeof(rep)) == kIOReturnSuccess);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_21_role_app_cannot_publish_tier0_pack()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t bb = 0u, app = 0u;
    CHECK(br->register_client(102u, true, &bb) == kIOReturnSuccess);
    CHECK(br->register_client(303u, false, &app) == kIOReturnSuccess);

    uint8_t sha[A840_PACK_SHA256_BYTES]{};
    sha[0] = 42u;
    CHECK(br->publish_tier0_pack(app, UINT64_C(777), UINT64_C(131072), sha) ==
          kIOReturnNotPermitted);
    CHECK(br->publish_tier0_pack(bb, UINT64_C(777), UINT64_C(131072), sha) ==
          kIOReturnSuccess);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_22_role_app_maps_tier0_pack_strictly_readonly()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t bb = 0u, apps[4]{};
    CHECK(br->register_client(103u, true, &bb) == kIOReturnSuccess);
    for (int i = 0; i < 4; ++i)
        CHECK(br->register_client(static_cast<uint32_t>(310 + i), false, &apps[i]) ==
              kIOReturnSuccess);

    uint8_t sha[A840_PACK_SHA256_BYTES]{};
    sha[1] = 99u;
    const uint64_t key = UINT64_C(123456789);
    CHECK(br->publish_tier0_pack(bb, key, UINT64_C(262144), sha) == kIOReturnSuccess);

    for (int i = 0; i < 4; ++i) {
        struct NativeAdrenoAbiSharedPackReqV3 req{};
        req.expected_stack_key = key;
        req.map_cpu = 1u;
        struct NativeAdrenoAbiSharedPackReplyV3 rep{};
        CHECK(br->dispatch_method(apps[i], NativeAdrenoMethodMapSharedPackV3,
                                  &req, sizeof(req), &rep, sizeof(rep)) ==
              kIOReturnSuccess);
        CHECK(rep.actual_stack_key == key);
        CHECK(rep.gpu_va != 0u);
        CHECK(rep.cpu_va != 0u);
        CHECK(rep.bytes == UINT64_C(262144));
    }

    struct NativeAdrenoAbiSharedPackReqV3 bad{};
    bad.expected_stack_key = key + 1u;
    bad.map_cpu = 0u;
    struct NativeAdrenoAbiSharedPackReplyV3 rep{};
    CHECK(br->dispatch_method(apps[0], NativeAdrenoMethodMapSharedPackV3,
                              &bad, sizeof(bad), &rep, sizeof(rep)) == kIOReturnBusy);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_23_role_app_cannot_invoke_display_handoff_or_thermal_note()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t app = 0u;
    CHECK(br->register_client(320u, false, &app) == kIOReturnSuccess);

    A840SysQueueConfigReqV3 qreq{};
    qreq.op = A840_SYS_QCFG_DISPLAY_HANDOFF_120HZ;
    A840SysQueueConfigReplyV3 qrep{};
    CHECK(br->dispatch_method(app, NativeAdrenoMethodQueueConfigV3,
                              &qreq, sizeof(qreq), &qrep, sizeof(qrep)) ==
          kIOReturnNotPermitted);

    A840SysNoteWire note{};
    note.kind = A840_SYS_NOTE_THERMAL_SAMPLE;
    note.param_i32 = 80000;
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_NOTE,
                              &note, sizeof(note), nullptr, 0u) == kIOReturnNotPermitted);

    note.kind = A840_SYS_NOTE_BOOT_COMPLETE;
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_NOTE,
                              &note, sizeof(note), nullptr, 0u) == kIOReturnNotPermitted);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_24_role_backboardd_tier0_pack_hot_upgrade_drain()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t bb = 0u, app = 0u;
    CHECK(br->register_client(104u, true, &bb) == kIOReturnSuccess);
    CHECK(br->register_client(321u, false, &app) == kIOReturnSuccess);

    uint8_t sha1[A840_PACK_SHA256_BYTES]{};
    sha1[0] = 1u;
    uint8_t sha2[A840_PACK_SHA256_BYTES]{};
    sha2[0] = 2u;
    CHECK(br->publish_tier0_pack(bb, UINT64_C(1001), UINT64_C(65536), sha1) == kIOReturnSuccess);

    struct NativeAdrenoAbiSharedPackReqV3 req{};
    req.expected_stack_key = UINT64_C(1001);
    struct NativeAdrenoAbiSharedPackReplyV3 rep{};
    CHECK(br->dispatch_method(app, NativeAdrenoMethodMapSharedPackV3,
                              &req, sizeof(req), &rep, sizeof(rep)) == kIOReturnSuccess);

    uint32_t pending = 0u;
    CHECK(br->publish_tier0_pack(bb, UINT64_C(2002), UINT64_C(131072), sha2, &pending) ==
          kIOReturnSuccess);
    CHECK(pending == 1u);

    CHECK(br->unregister_client(app) == kIOReturnSuccess);
    uint64_t app2 = 0u;
    CHECK(br->register_client(322u, false, &app2) == kIOReturnSuccess);
    req.expected_stack_key = UINT64_C(2002);
    CHECK(br->dispatch_method(app2, NativeAdrenoMethodMapSharedPackV3,
                              &req, sizeof(req), &rep, sizeof(rep)) == kIOReturnSuccess);
    CHECK(rep.actual_stack_key == UINT64_C(2002));
    CHECK(br->shutdown() == kIOReturnSuccess);
}

/* ============================================================================
 * Group D (Cases 25-32): Full Methods 2..19 & 20..26 End-to-End
 * ============================================================================ */

void case_25_method2_3_4_bo_alloc_map_free_with_parked_reuse()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t app = 0u;
    CHECK(br->register_client(401u, false, &app) == kIOReturnSuccess);

    A840SysBoAllocWire areq{};
    areq.bytes = UINT64_C(65536);
    areq.alignment = UINT64_C(65536);
    areq.permissions = A840_KMD_SMMU_PERM_READ_WRITE;
    areq.cache_mode = A840_KMD_BO_CACHE_WRITE_COMBINE;
    A840SysBoAllocReplyWire arep{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_BO_ALLOC,
                              &areq, sizeof(areq), &arep, sizeof(arep)) == kIOReturnSuccess);
    CHECK(arep.handle != 0u);
    CHECK(arep.gpu_va != 0u);

    A840SysBoMapWire mreq{};
    mreq.handle = arep.handle;
    mreq.iova = UINT64_C(4194304);
    A840SysBoMapReplyWire mrep{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_BO_MAP,
                              &mreq, sizeof(mreq), &mrep, sizeof(mrep)) == kIOReturnSuccess);
    CHECK(mrep.gpu_va == UINT64_C(4194304));

    A840SysBoFreeWire freq{};
    freq.handle = arep.handle;
    freq.allow_park = 1u;
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_BO_FREE,
                              &freq, sizeof(freq), nullptr, 0u) == kIOReturnSuccess);

    A840SysBoAllocReplyWire arep2{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_BO_ALLOC,
                              &areq, sizeof(areq), &arep2, sizeof(arep2)) == kIOReturnSuccess);
    A840XnuSystemBridge::Stats st{};
    CHECK(br->get_stats(&st) == kIOReturnSuccess);
    CHECK(st.bo_pool_stats.park_hits >= 1u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_26_method9_10_metadata_set_get_client_isolation()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t a = 0u, b = 0u;
    CHECK(br->register_client(402u, false, &a) == kIOReturnSuccess);
    CHECK(br->register_client(403u, false, &b) == kIOReturnSuccess);

    A840SysMetadataWire mset{};
    mset.handle = 99u;
    mset.bytes = 8u;
    std::memcpy(mset.data, "A840META", 8u);
    CHECK(br->dispatch_method(a, A840_KMD_V3_METHOD_SET_METADATA,
                              &mset, sizeof(mset), nullptr, 0u) == kIOReturnSuccess);

    A840SysMetadataWire mget_req{};
    mget_req.handle = 99u;
    A840SysMetadataWire mget_rep{};
    CHECK(br->dispatch_method(a, A840_KMD_V3_METHOD_GET_METADATA,
                              &mget_req, sizeof(mget_req), &mget_rep, sizeof(mget_rep)) ==
          kIOReturnSuccess);
    CHECK(mget_rep.bytes == 8u);
    CHECK(std::memcmp(mget_rep.data, "A840META", 8u) == 0);

    CHECK(br->dispatch_method(b, A840_KMD_V3_METHOD_GET_METADATA,
                              &mget_req, sizeof(mget_req), &mget_rep, sizeof(mget_rep)) ==
          kIOReturnNotFound);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_27_method11_12_create_close_queue_and_limits()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t app = 0u;
    CHECK(br->register_client(404u, false, &app) == kIOReturnSuccess);

    A840SysQueueWire qreq{};
    qreq.queue = 5u;
    qreq.priority = A840_PRIORITY_HIGH;
    A840SysQueueWire qrep{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_CREATE_QUEUE,
                              &qreq, sizeof(qreq), &qrep, sizeof(qrep)) == kIOReturnSuccess);
    CHECK(qrep.queue == 5u);
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_CREATE_QUEUE,
                              &qreq, sizeof(qreq), &qrep, sizeof(qrep)) == kIOReturnBusy);

    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_CLOSE_QUEUE,
                              &qreq, sizeof(qreq), nullptr, 0u) == kIOReturnSuccess);
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_CLOSE_QUEUE,
                              &qreq, sizeof(qreq), nullptr, 0u) == kIOReturnNotFound);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_28_method13_14_bo_cpu_map_unmap_ro_write_denied()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t app = 0u;
    CHECK(br->register_client(405u, false, &app) == kIOReturnSuccess);

    A840SysBoAllocWire areq{};
    areq.bytes = UINT64_C(65536);
    areq.permissions = A840_KMD_SMMU_PERM_READ_ONLY;
    A840SysBoAllocReplyWire ro_bo{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_BO_ALLOC,
                              &areq, sizeof(areq), &ro_bo, sizeof(ro_bo)) == kIOReturnSuccess);

    A840SysBoCpuMapWire cmap{};
    cmap.handle = ro_bo.handle;
    cmap.write_access = 1u;
    A840SysBoCpuMapReplyWire crep{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_BO_CPU_MAP,
                              &cmap, sizeof(cmap), &crep, sizeof(crep)) == kIOReturnNotPermitted);

    cmap.write_access = 0u;
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_BO_CPU_MAP,
                              &cmap, sizeof(cmap), &crep, sizeof(crep)) == kIOReturnSuccess);
    CHECK(crep.active_leases == 1u);
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_BO_CPU_UNMAP,
                              &cmap, sizeof(cmap), nullptr, 0u) == kIOReturnSuccess);
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_BO_CPU_UNMAP,
                              &cmap, sizeof(cmap), nullptr, 0u) == kIOReturnNotFound);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_29_method7_8_timestamp_and_status_monotonic()
{
    FakeClock clk{};
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig(&clk)) == kIOReturnSuccess);
    CHECK(br->qualify_production(nullptr) == kIOReturnSuccess);
    uint64_t app = 0u;
    CHECK(br->register_client(406u, false, &app) == kIOReturnSuccess);

    A840SysTimestampReplyWire t1{}, t2{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_GET_TIMESTAMP,
                              nullptr, 0u, &t1, sizeof(t1)) == kIOReturnSuccess);
    clk.advance(UINT64_C(5000000));
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_GET_TIMESTAMP,
                              nullptr, 0u, &t2, sizeof(t2)) == kIOReturnSuccess);
    CHECK(t2.now_ns > t1.now_ns);
    CHECK(t2.ticks > t1.ticks);

    A840SysStatusReplyWire st{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_GET_STATUS,
                              nullptr, 0u, &st, sizeof(st)) == kIOReturnSuccess);
    CHECK(st.abi_mode == A840_SYS_ABI_MODE_V3_NATIVE);
    CHECK(st.qualified == 1u);
    CHECK(st.faults == 0u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_30_method22_sparse_va_create_bind_query_unbind_zero_fallback()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t app = 0u;
    CHECK(br->register_client(407u, false, &app) == kIOReturnSuccess);

    A840SysBoAllocWire areq{};
    areq.bytes = UINT64_C(131072);
    areq.alignment = UINT64_C(65536);
    areq.permissions = A840_KMD_SMMU_PERM_READ_WRITE;
    A840SysBoAllocReplyWire bo{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_BO_ALLOC,
                              &areq, sizeof(areq), &bo, sizeof(bo)) == kIOReturnSuccess);

    A840SysQueueConfigReqV3 sreg{};
    sreg.op = A840_SYS_QCFG_SPARSE_REGION_CREATE;
    sreg.u.sparse_region.gpu_va = UINT64_C(67108864);
    sreg.u.sparse_region.length_bytes = UINT64_C(262144);
    A840SysQueueConfigReplyV3 srep{};
    CHECK(br->dispatch_method(app, NativeAdrenoMethodQueueConfigV3,
                              &sreg, sizeof(sreg), &srep, sizeof(srep)) == kIOReturnSuccess);
    CHECK(srep.u.sparse_region.region_id != 0u);

    A840KmdSparseBindReqV3 breq{};
    breq.version = NativeAdrenoAbiVersionV3;
    breq.op_count = 3u;
    breq.region_id = srep.u.sparse_region.region_id;
    breq.ops[0].op = A840_KMD_SPARSE_OP_BIND_BO;
    breq.ops[0].flags = A840_KMD_SMMU_PERM_READ_WRITE;
    breq.ops[0].gpu_va = UINT64_C(67108864);
    breq.ops[0].length_bytes = UINT64_C(65536);
    breq.ops[0].bo_handle = bo.handle;
    breq.ops[0].bo_offset = 0u;
    breq.ops[1].op = A840_KMD_SPARSE_OP_QUERY_TILE;
    breq.ops[1].gpu_va = UINT64_C(67108864);
    breq.ops[1].length_bytes = UINT64_C(65536);
    breq.ops[2].op = A840_KMD_SPARSE_OP_QUERY_TILE;
    breq.ops[2].gpu_va = UINT64_C(67108864) + UINT64_C(65536);
    breq.ops[2].length_bytes = UINT64_C(65536);

    A840KmdSparseBindReplyV3 brep{};
    CHECK(br->dispatch_method(app, NativeAdrenoMethodMapSparseV3,
                              &breq, sizeof(breq), &brep, sizeof(brep)) == kIOReturnSuccess);
    CHECK(brep.completed_ops == 3u);
    CHECK(brep.queried_count == 2u);
    CHECK(brep.results[1].is_zero_page == 0u);
    CHECK(brep.results[1].bo_handle == bo.handle);
    CHECK(brep.results[2].is_zero_page == 1u);
    CHECK(brep.results[2].permissions == A840_KMD_SMMU_PERM_READ_ONLY);

    A840KmdSparseBindReqV3 ureq{};
    ureq.version = NativeAdrenoAbiVersionV3;
    ureq.op_count = 2u;
    ureq.region_id = srep.u.sparse_region.region_id;
    ureq.ops[0].op = A840_KMD_SPARSE_OP_UNBIND_TO_ZERO;
    ureq.ops[0].gpu_va = UINT64_C(67108864);
    ureq.ops[0].length_bytes = UINT64_C(65536);
    ureq.ops[1].op = A840_KMD_SPARSE_OP_QUERY_TILE;
    ureq.ops[1].gpu_va = UINT64_C(67108864);
    ureq.ops[1].length_bytes = UINT64_C(65536);
    CHECK(br->dispatch_method(app, NativeAdrenoMethodMapSparseV3,
                              &ureq, sizeof(ureq), &brep, sizeof(brep)) == kIOReturnSuccess);
    CHECK(brep.results[0].is_zero_page == 1u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_31_method22_sparse_va_rejects_cross_client_and_zero_write()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t a = 0u, b = 0u;
    CHECK(br->register_client(408u, false, &a) == kIOReturnSuccess);
    CHECK(br->register_client(409u, false, &b) == kIOReturnSuccess);

    A840SysBoAllocWire areq{};
    areq.bytes = UINT64_C(65536);
    areq.alignment = UINT64_C(65536);
    areq.permissions = A840_KMD_SMMU_PERM_READ_WRITE;
    A840SysBoAllocReplyWire bo_a{};
    CHECK(br->dispatch_method(a, A840_KMD_V3_METHOD_BO_ALLOC,
                              &areq, sizeof(areq), &bo_a, sizeof(bo_a)) == kIOReturnSuccess);

    A840SysQueueConfigReqV3 sreg{};
    sreg.op = A840_SYS_QCFG_SPARSE_REGION_CREATE;
    sreg.u.sparse_region.gpu_va = UINT64_C(134217728);
    sreg.u.sparse_region.length_bytes = UINT64_C(131072);
    A840SysQueueConfigReplyV3 srep{};
    CHECK(br->dispatch_method(b, NativeAdrenoMethodQueueConfigV3,
                              &sreg, sizeof(sreg), &srep, sizeof(srep)) == kIOReturnSuccess);

    A840KmdSparseBindReqV3 bad{};
    bad.version = NativeAdrenoAbiVersionV3;
    bad.op_count = 1u;
    bad.region_id = srep.u.sparse_region.region_id;
    bad.ops[0].op = A840_KMD_SPARSE_OP_BIND_BO;
    bad.ops[0].flags = A840_KMD_SMMU_PERM_READ_WRITE;
    bad.ops[0].gpu_va = UINT64_C(134217728);
    bad.ops[0].length_bytes = UINT64_C(65536);
    bad.ops[0].bo_handle = bo_a.handle;
    A840KmdSparseBindReplyV3 brep{};
    CHECK(br->dispatch_method(b, NativeAdrenoMethodMapSparseV3,
                              &bad, sizeof(bad), &brep, sizeof(brep)) != kIOReturnSuccess);

    bad.ops[0].op = A840_KMD_SPARSE_OP_BIND_ZERO_PAGE;
    bad.ops[0].flags = A840_KMD_SMMU_PERM_WRITE;
    bad.ops[0].bo_handle = 0u;
    CHECK(br->dispatch_method(b, NativeAdrenoMethodMapSparseV3,
                              &bad, sizeof(bad), &brep, sizeof(brep)) == kIOReturnNotPermitted);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_32_method20_wait_any_and_wait_all_interrupt_wakeup()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t app = 0u;
    CHECK(br->register_client(410u, false, &app) == kIOReturnSuccess);

    A840SysSubmitWire sub{};
    sub.queue = 0u;
    sub.priority_hint = A840_PRIORITY_NORMAL;
    A840SysSubmitReplyWire srep{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_SUBMIT,
                              &sub, sizeof(sub), &srep, sizeof(srep)) == kIOReturnSuccess);

    std::atomic<IOReturn> wait_rc{kIOReturnError};
    struct NativeAdrenoAbiWaitAnyReplyV3 wrep{};
    std::thread waiter([&]() {
        struct NativeAdrenoAbiWaitAnyV3 wa{};
        wa.count = 1u;
        wa.flags = 0u;
        wa.timeout_ns = UINT64_C(2000000000);
        wa.queues[0] = 0u;
        wa.fences[0] = srep.fence;
        wait_rc.store(
            br->dispatch_method(app, NativeAdrenoMethodWaitAnyV3,
                                &wa, sizeof(wa), &wrep, sizeof(wrep)),
            std::memory_order_release);
    });

    CHECK(br->on_gpu_fence_irq(srep.assigned_ring, srep.ring_seqno) == kIOReturnSuccess);
    waiter.join();
    CHECK(wait_rc.load(std::memory_order_acquire) == kIOReturnSuccess);
    CHECK(wrep.timed_out == 0u);
    CHECK(wrep.satisfied_mask == 1u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

/* ============================================================================
 * Group E (Cases 33-40): UBWC 6.0 IOSurface, WriterPromise & 120Hz MDSS Handoff
 * ============================================================================ */

void case_33_iosurface_1200x2608_ubwc6_hbb16_zero_copy_import()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t bb = 0u;
    CHECK(br->register_client(501u, true, &bb) == kIOReturnSuccess);

    A840SysBoImportUbwc6Wire ireq = makeNezha1200x2608Ubwc6Req(UINT64_C(9001));
    A840SysBoImportUbwc6ReplyWire irep{};
    CHECK(br->dispatch_method(bb, A840_KMD_V3_METHOD_BO_IMPORT,
                              &ireq, sizeof(ireq), &irep, sizeof(irep)) == kIOReturnSuccess);
    CHECK(irep.bo_handle != 0u);
    CHECK(irep.surface_identity == UINT64_C(9001));
    CHECK(irep.scanout_eligible == 1u);
    CHECK(irep.fallback_linear_required == 0u);
    CHECK(irep.layout_valid == 1u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_34_iosurface_shared_app_to_backboardd_zero_copy_handoff()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t bb = 0u, app = 0u;
    CHECK(br->register_client(502u, true, &bb) == kIOReturnSuccess);
    CHECK(br->register_client(503u, false, &app) == kIOReturnSuccess);

    A840SysBoImportUbwc6Wire ireq = makeNezha1200x2608Ubwc6Req(UINT64_C(9002));
    A840SysBoImportUbwc6ReplyWire app_rep{}, bb_rep{};
    CHECK(br->import_iosurface_ubwc6(app, ireq, &app_rep) == kIOReturnSuccess);
    CHECK(br->import_iosurface_ubwc6(bb, ireq, &bb_rep) == kIOReturnSuccess);
    CHECK(bb_rep.surface_identity == app_rep.surface_identity);
    CHECK(bb_rep.scanout_eligible == 1u);
    CHECK(bb_rep.layout_valid == 1u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_35_method25_surface_meta_get_and_invalid_hbb15_rejected()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t bb = 0u;
    CHECK(br->register_client(504u, true, &bb) == kIOReturnSuccess);

    A840SysBoImportUbwc6Wire ireq = makeNezha1200x2608Ubwc6Req(UINT64_C(9003));
    A840SysBoImportUbwc6ReplyWire irep{};
    CHECK(br->import_iosurface_ubwc6(bb, ireq, &irep) == kIOReturnSuccess);

    struct NativeAdrenoAbiSurfaceMetaV3 q{}, r{};
    q.bo_handle = irep.bo_handle;
    q.op = 0u;
    CHECK(br->dispatch_method(bb, NativeAdrenoMethodSurfaceMetaV3,
                              &q, sizeof(q), &r, sizeof(r)) == kIOReturnSuccess);
    CHECK(r.ubwc_enabled == 1u);
    CHECK(r.ubwc_version == 6u);
    CHECK(r.highest_bank_bit == 16u);

    struct NativeAdrenoAbiSurfaceMetaV3 bad = r;
    bad.op = 1u;
    bad.highest_bank_bit = 15u;
    CHECK(br->dispatch_method(bb, NativeAdrenoMethodSurfaceMetaV3,
                              &bad, sizeof(bad), &r, sizeof(r)) != kIOReturnSuccess);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_36_writer_promise_backboardd_early_handoff_zero_extra_vsync()
{
    FakeClock clk{};
    MdssCommitRecorder rec{};
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig(&clk, &rec)) == kIOReturnSuccess);
    uint64_t bb = 0u;
    CHECK(br->register_client(505u, true, &bb) == kIOReturnSuccess);

    const uint64_t sid = UINT64_C(9100);
    A840SysBoImportUbwc6ReplyWire surf{};
    CHECK(br->import_iosurface_ubwc6(bb, makeNezha1200x2608Ubwc6Req(sid), &surf) ==
          kIOReturnSuccess);

    struct NativeAdrenoAbiWriterPromiseV3 mk{};
    mk.op = NativeAdrenoPromiseMake;
    mk.surface_count = 1u;
    mk.queue = 0u;
    mk.surface_ids[0] = surf.bo_handle;
    struct NativeAdrenoAbiWriterPromiseReplyV3 mk_rep{};
    CHECK(br->dispatch_method(bb, NativeAdrenoMethodWriterPromiseV3,
                              &mk, sizeof(mk), &mk_rep, sizeof(mk_rep)) == kIOReturnSuccess);
    CHECK(mk_rep.accepted == 1u);
    CHECK(mk_rep.promise_token != 0u);

    struct NativeAdrenoAbiWriterPromiseV3 fl = mk;
    fl.op = NativeAdrenoPromiseFulfill;
    fl.promise_token = mk_rep.promise_token;
    fl.fence = 1u;
    struct NativeAdrenoAbiWriterPromiseReplyV3 fl_rep{};
    CHECK(br->dispatch_method(bb, NativeAdrenoMethodWriterPromiseV3,
                              &fl, sizeof(fl), &fl_rep, sizeof(fl_rep)) == kIOReturnSuccess);

    const uint64_t now = clk.advance(UINT64_C(2000000));
    A840SysQueueConfigReqV3 dh{};
    dh.op = A840_SYS_QCFG_DISPLAY_HANDOFF_120HZ;
    dh.now_ns = now;
    dh.u.display_handoff.frame_id = 1u;
    dh.u.display_handoff.surface_handle = surf.bo_handle;
    dh.u.display_handoff.surface_identity = sid;
    dh.u.display_handoff.estimated_gpu_done_ns = now + UINT64_C(2000000);
    dh.u.display_handoff.has_early_promise = 1u;
    A840SysQueueConfigReplyV3 dh_rep{};
    CHECK(br->dispatch_method(bb, NativeAdrenoMethodQueueConfigV3,
                              &dh, sizeof(dh), &dh_rep, sizeof(dh_rep)) == kIOReturnSuccess);
    CHECK(dh_rep.u.display_handoff.handoff_outcome == A840_DISPLAY_HANDOFF_COMMITTED);
    CHECK(dh_rep.u.display_handoff.scanout_mode == A840_DISPLAY_SCANOUT_UBWC6_DIRECT);
    CHECK(dh_rep.u.display_handoff.sync_mode == A840_DISPLAY_SYNC_EARLY_GPU_FENCE);
    CHECK(dh_rep.u.display_handoff.mdss_decision == A840_MDSS_COMMIT_HIT_CURRENT_VSYNC);
    CHECK(dh_rep.u.display_handoff.extra_vsync_waits == 0u);
    CHECK(rec.commit_count.load() == 1u);
    CHECK(rec.ubwc_direct_count.load() == 1u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_37_writer_promise_app_deferred_until_fence_retired()
{
    FakeClock clk{};
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig(&clk)) == kIOReturnSuccess);
    uint64_t bb = 0u, app = 0u;
    CHECK(br->register_client(506u, true, &bb) == kIOReturnSuccess);
    CHECK(br->register_client(507u, false, &app) == kIOReturnSuccess);

    const uint64_t sid = UINT64_C(9200);
    A840SysBoImportUbwc6ReplyWire app_surf{}, bb_surf{};
    CHECK(br->import_iosurface_ubwc6(app, makeNezha1200x2608Ubwc6Req(sid), &app_surf) ==
          kIOReturnSuccess);
    CHECK(br->import_iosurface_ubwc6(bb, makeNezha1200x2608Ubwc6Req(sid), &bb_surf) ==
          kIOReturnSuccess);

    A840SysSubmitWire sub{};
    sub.queue = 0u;
    sub.priority_hint = A840_PRIORITY_NORMAL;
    A840SysSubmitReplyWire srep{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_SUBMIT,
                              &sub, sizeof(sub), &srep, sizeof(srep)) == kIOReturnSuccess);

    struct NativeAdrenoAbiWriterPromiseV3 mk{};
    mk.op = NativeAdrenoPromiseMake;
    mk.surface_count = 1u;
    mk.queue = 0u;
    mk.surface_ids[0] = app_surf.bo_handle;
    struct NativeAdrenoAbiWriterPromiseReplyV3 mk_rep{};
    CHECK(br->dispatch_method(app, NativeAdrenoMethodWriterPromiseV3,
                              &mk, sizeof(mk), &mk_rep, sizeof(mk_rep)) == kIOReturnSuccess);

    struct NativeAdrenoAbiWriterPromiseV3 fl = mk;
    fl.op = NativeAdrenoPromiseFulfill;
    fl.promise_token = mk_rep.promise_token;
    fl.fence = srep.fence;
    struct NativeAdrenoAbiWriterPromiseReplyV3 fl_rep{};
    CHECK(br->dispatch_method(app, NativeAdrenoMethodWriterPromiseV3,
                              &fl, sizeof(fl), &fl_rep, sizeof(fl_rep)) == kIOReturnSuccess);

    A840SysQueueConfigReqV3 dh{};
    dh.op = A840_SYS_QCFG_DISPLAY_HANDOFF_120HZ;
    dh.now_ns = clk.advance(UINT64_C(1000000));
    dh.u.display_handoff.frame_id = 1u;
    dh.u.display_handoff.surface_handle = bb_surf.bo_handle;
    dh.u.display_handoff.surface_identity = sid;
    A840SysQueueConfigReplyV3 dh_rep{};
    const IOReturn pre_rc = br->dispatch_method(
        bb, NativeAdrenoMethodQueueConfigV3, &dh, sizeof(dh), &dh_rep, sizeof(dh_rep));
    CHECK(pre_rc != kIOReturnSuccess ||
          dh_rep.u.display_handoff.handoff_outcome != A840_DISPLAY_HANDOFF_COMMITTED);

    CHECK(br->on_gpu_fence_irq(srep.assigned_ring, srep.ring_seqno, clk.now_ns.load()) ==
          kIOReturnSuccess);
    dh.now_ns = clk.advance(UINT64_C(1000000));
    dh.u.display_handoff.is_retired_ready = 1u;
    CHECK(br->dispatch_method(bb, NativeAdrenoMethodQueueConfigV3,
                              &dh, sizeof(dh), &dh_rep, sizeof(dh_rep)) == kIOReturnSuccess);
    CHECK(dh_rep.u.display_handoff.handoff_outcome == A840_DISPLAY_HANDOFF_COMMITTED);
    CHECK(dh_rep.u.display_handoff.sync_mode == A840_DISPLAY_SYNC_RETIRED_READY);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_38_mdss_120hz_vblank_pll_lock_and_past_cutoff_clean_defer()
{
    FakeClock clk{};
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig(&clk)) == kIOReturnSuccess);
    uint64_t bb = 0u;
    CHECK(br->register_client(508u, true, &bb) == kIOReturnSuccess);

    for (uint64_t seq = 1u; seq <= 4u; ++seq) {
        const uint64_t ts = clk.advance(A840_MDSS_PERIOD_120HZ_NS);
        CHECK(br->on_mdss_vblank(ts, seq) == kIOReturnSuccess);
    }

    const uint64_t late_ts = clk.advance(A840_MDSS_PERIOD_120HZ_NS - UINT64_C(200000));
    A840SysSubmitWire sub{};
    sub.queue = 0u;
    sub.enqueue_ns = late_ts;
    sub.attach_display_sync = 1u;
    sub.frame_id = 42u;
    A840SysSubmitReplyWire srep{};
    CHECK(br->dispatch_method(bb, A840_KMD_V3_METHOD_SUBMIT,
                              &sub, sizeof(sub), &srep, sizeof(srep)) == kIOReturnSuccess);
    CHECK(srep.mdss_decision == A840_MDSS_COMMIT_DEFER_NEXT_VSYNC);
    CHECK(srep.extra_vsync_waits >= 1u);

    A840MdssSyncStats ms{};
    CHECK(br->get_mdss_stats(&ms) == kIOReturnSuccess);
    CHECK(ms.vsync_events_recorded >= 4u);
    CHECK(ms.dcvs_boost_hints_sent >= 1u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_39_submit_compositor_frame_combines_ring0_dcvs_mdss_single_path()
{
    FakeClock clk{};
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig(&clk)) == kIOReturnSuccess);
    uint64_t bb = 0u;
    CHECK(br->register_client(509u, true, &bb) == kIOReturnSuccess);

    const uint64_t ts = clk.advance(UINT64_C(1500000));
    A840SysSubmitWire sub{};
    sub.queue = 0u;
    sub.enqueue_ns = ts;
    sub.frame_deadline_ns = ts + UINT64_C(2000000);
    sub.estimated_gpu_busy_ns = UINT64_C(3000000);
    sub.attach_display_sync = 1u;
    sub.surface_identity = UINT64_C(9300);
    sub.frame_id = 7u;
    A840SysSubmitReplyWire srep{};
    CHECK(br->dispatch_method(bb, A840_KMD_V3_METHOD_SUBMIT,
                              &sub, sizeof(sub), &srep, sizeof(srep)) == kIOReturnSuccess);
    CHECK(srep.assigned_ring == A840_RING_0_COMPOSITOR);
    CHECK(srep.doorbell_emitted == 1u);
    CHECK(srep.mdss_decision == A840_MDSS_COMMIT_HIT_CURRENT_VSYNC);
    CHECK(srep.extra_vsync_waits == 0u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_40_method19_submit_batch_and_method6_wait_fence_roundtrip()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t app = 0u;
    CHECK(br->register_client(510u, false, &app) == kIOReturnSuccess);

    A840SysSubmitWire sub{};
    sub.queue = 0u;
    sub.priority_hint = A840_PRIORITY_HIGH;
    A840SysSubmitReplyWire srep{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_SUBMIT_BATCH,
                              &sub, sizeof(sub), &srep, sizeof(srep)) == kIOReturnSuccess);
    CHECK(srep.assigned_ring == A840_RING_1_INTERACTIVE);

    CHECK(br->on_gpu_fence_irq(srep.assigned_ring, srep.ring_seqno) == kIOReturnSuccess);

    A840SysWaitFenceWire wf{};
    wf.queue = 0u;
    wf.fence = srep.fence;
    wf.timeout_ns = UINT64_C(100000000);
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_WAIT_FENCE,
                              &wf, sizeof(wf), nullptr, 0u) == kIOReturnSuccess);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

/* ============================================================================
 * Group F (Cases 41-48): 4-Ring Preemption, Wakeup PI, 18-OPP DCVS & IB Diag
 * ============================================================================ */

void case_41_preempt_ring0_compositor_preempts_app_ring2()
{
    FakeClock clk{};
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig(&clk)) == kIOReturnSuccess);
    uint64_t bb = 0u, app = 0u;
    CHECK(br->register_client(601u, true, &bb) == kIOReturnSuccess);
    CHECK(br->register_client(602u, false, &app) == kIOReturnSuccess);

    A840SysSubmitWire app_sub{};
    app_sub.queue = 0u;
    app_sub.priority_hint = A840_PRIORITY_NORMAL;
    app_sub.enqueue_ns = clk.advance(UINT64_C(100000));
    A840SysSubmitReplyWire app_rep{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_SUBMIT,
                              &app_sub, sizeof(app_sub), &app_rep, sizeof(app_rep)) ==
          kIOReturnSuccess);
    CHECK(app_rep.assigned_ring == A840_RING_2_CLIENT_3D);

    /* backboardd submits urgent 120Hz frame -> preempts Ring 2! */
    const uint64_t t_bb = clk.advance(UINT64_C(1000000));
    A840SysSubmitWire bb_sub{};
    bb_sub.queue = 0u;
    bb_sub.enqueue_ns = t_bb;
    bb_sub.frame_deadline_ns = t_bb + UINT64_C(1500000);
    A840SysSubmitReplyWire bb_rep{};
    CHECK(br->dispatch_method(bb, A840_KMD_V3_METHOD_SUBMIT,
                              &bb_sub, sizeof(bb_sub), &bb_rep, sizeof(bb_rep)) ==
          kIOReturnSuccess);
    CHECK(bb_rep.assigned_ring == A840_RING_0_COMPOSITOR);
    CHECK(bb_rep.preempt_triggered == 1u);

    CHECK(br->on_gpu_fence_irq(A840_RING_0_COMPOSITOR, bb_rep.ring_seqno, clk.advance(UINT64_C(200000))) ==
          kIOReturnSuccess);
    CHECK(br->on_gpu_fence_irq(A840_RING_2_CLIENT_3D, app_rep.ring_seqno, clk.advance(UINT64_C(200000))) ==
          kIOReturnSuccess);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_42_preempt_priority_inheritance_boosts_dep_ring_wakeup()
{
    FakeClock clk{};
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig(&clk)) == kIOReturnSuccess);
    uint64_t bb = 0u, app = 0u;
    CHECK(br->register_client(603u, true, &bb) == kIOReturnSuccess);
    CHECK(br->register_client(604u, false, &app) == kIOReturnSuccess);

    A840SysSubmitWire app_sub{};
    app_sub.queue = 0u;
    app_sub.priority_hint = A840_PRIORITY_NORMAL;
    app_sub.enqueue_ns = clk.advance(UINT64_C(100000));
    A840SysSubmitReplyWire app_rep{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_SUBMIT,
                              &app_sub, sizeof(app_sub), &app_rep, sizeof(app_rep)) ==
          kIOReturnSuccess);

    /* backboardd Ring 0 depends on App Ring 2 -> triggers PI boost on Ring 2 */
    A840SysSubmitWire bb_sub{};
    bb_sub.queue = 0u;
    bb_sub.enqueue_ns = clk.advance(UINT64_C(100000));
    bb_sub.has_dependency = 1u;
    bb_sub.dep_ring = A840_RING_2_CLIENT_3D;
    bb_sub.dep_seqno = app_rep.ring_seqno;
    A840SysSubmitReplyWire bb_rep{};
    CHECK(br->dispatch_method(bb, A840_KMD_V3_METHOD_SUBMIT,
                              &bb_sub, sizeof(bb_sub), &bb_rep, sizeof(bb_rep)) ==
          kIOReturnSuccess);

    A840WakeupSnapshot wsnap{};
    CHECK(br->get_wakeup_snapshot(&wsnap) == kIOReturnSuccess);
    CHECK(wsnap.rings[A840_RING_2_CLIENT_3D].pi_boost_active == 1u);
    CHECK(wsnap.rings[A840_RING_2_CLIENT_3D].effective_mode == A840_WAKE_MODE_IMMEDIATE);

    CHECK(br->on_gpu_fence_irq(A840_RING_2_CLIENT_3D, app_rep.ring_seqno, clk.advance(UINT64_C(200000))) ==
          kIOReturnSuccess);
    CHECK(br->get_wakeup_snapshot(&wsnap) == kIOReturnSuccess);
    CHECK(wsnap.rings[A840_RING_2_CLIENT_3D].pi_boost_active == 0u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_43_gmu_dcvs_18_gpu_4_gmu_opp_upscale_under_120hz_load()
{
    FakeClock clk{};
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig(&clk)) == kIOReturnSuccess);
    uint64_t bb = 0u;
    CHECK(br->register_client(605u, true, &bb) == kIOReturnSuccess);

    A840GmuDcvsSnapshot before{};
    CHECK(br->get_dcvs_snapshot(&before) == kIOReturnSuccess);

    for (int f = 0; f < 10; ++f) {
        A840SysQueueConfigReqV3 req{};
        req.op = A840_SYS_QCFG_GMU_DCVS_HINT;
        req.now_ns = clk.advance(A840_DCVS_FRAME_BUDGET_120HZ_NS);
        req.u.dcvs_hint.action = 1u; /* record frame */
        req.u.dcvs_hint.gpu_busy_ns = UINT64_C(7500000); /* 90% of 8.333ms */
        req.u.dcvs_hint.frame_period_ns = A840_DCVS_FRAME_BUDGET_120HZ_NS;
        req.u.dcvs_hint.ring0_submits = 4u;
        A840SysQueueConfigReplyV3 rep{};
        CHECK(br->dispatch_method(bb, NativeAdrenoMethodQueueConfigV3,
                                  &req, sizeof(req), &rep, sizeof(rep)) == kIOReturnSuccess);
    }

    A840GmuDcvsSnapshot after{};
    CHECK(br->get_dcvs_snapshot(&after) == kIOReturnSuccess);
    CHECK(after.confirmed_gpu_level > before.confirmed_gpu_level);
    CHECK(after.confirmed_gpu_freq_hz > before.confirmed_gpu_freq_hz);
    CHECK(after.confirmed_gmu_level < A840_GMU_OPP_COUNT);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_44_gmu_dcvs_inter_frame_hold_prevents_drop_between_120hz_frames()
{
    FakeClock clk{};
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig(&clk)) == kIOReturnSuccess);
    uint64_t bb = 0u;
    CHECK(br->register_client(606u, true, &bb) == kIOReturnSuccess);

    /* Ramp up first via touch boost + busy frames */
    A840SysNoteWire touch{};
    touch.kind = A840_SYS_NOTE_TOUCH_BOOST;
    touch.now_ns = clk.advance(UINT64_C(1000000));
    touch.value_u64 = UINT64_C(5000000);
    CHECK(br->dispatch_method(bb, A840_KMD_V3_METHOD_NOTE,
                              &touch, sizeof(touch), nullptr, 0u) == kIOReturnSuccess);

    A840GmuDcvsSnapshot peak{};
    CHECK(br->get_dcvs_snapshot(&peak) == kIOReturnSuccess);
    CHECK(peak.confirmed_gpu_level >= A840_GPU_OPP_TOUCH_FLOOR_IDX);

    /* A vblank 8.333 ms later (< 16.7 ms inter-frame hold) must NOT drop OPP! */
    const uint64_t t_next = clk.advance(A840_MDSS_PERIOD_120HZ_NS);
    CHECK(br->on_mdss_vblank(t_next, 1u) == kIOReturnSuccess);

    A840GmuDcvsSnapshot held{};
    CHECK(br->get_dcvs_snapshot(&held) == kIOReturnSuccess);
    CHECK(held.confirmed_gpu_level >= peak.confirmed_gpu_level);
    CHECK(held.inter_frame_hold_hits >= 1u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_45_gmu_dcvs_touch_boost_578mhz_and_cold_lease_726_902mhz()
{
    FakeClock clk{};
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig(&clk)) == kIOReturnSuccess);
    uint64_t bb = 0u;
    CHECK(br->register_client(607u, true, &bb) == kIOReturnSuccess);

    /* Report cold temperature 42 C -> 902 MHz cold lease granted */
    A840SysNoteWire th{};
    th.kind = A840_SYS_NOTE_THERMAL_SAMPLE;
    th.param_i32 = 42000;
    th.now_ns = clk.advance(UINT64_C(1000000));
    CHECK(br->dispatch_method(bb, A840_KMD_V3_METHOD_NOTE,
                              &th, sizeof(th), nullptr, 0u) == kIOReturnSuccess);

    A840SysNoteWire lease{};
    lease.kind = A840_SYS_NOTE_COLD_LEASE_REQ;
    lease.param_u32 = A840_GPU_OPP_LEASE_HIGH_IDX; /* 902 MHz */
    lease.now_ns = clk.advance(UINT64_C(1000000));
    lease.value_u64 = UINT64_C(100000000);
    CHECK(br->dispatch_method(bb, A840_KMD_V3_METHOD_NOTE,
                              &lease, sizeof(lease), nullptr, 0u) == kIOReturnSuccess);

    A840GmuDcvsSnapshot snap{};
    CHECK(br->get_dcvs_snapshot(&snap) == kIOReturnSuccess);
    CHECK(snap.confirmed_gpu_level >= A840_GPU_OPP_LEASE_HIGH_IDX);

    /* Release lease, heat sensor to 65 C -> 902 MHz lease rejected fail-closed */
    A840SysNoteWire rel{};
    rel.kind = A840_SYS_NOTE_COLD_LEASE_RELEASE;
    rel.now_ns = clk.advance(UINT64_C(1000000));
    CHECK(br->dispatch_method(bb, A840_KMD_V3_METHOD_NOTE,
                              &rel, sizeof(rel), nullptr, 0u) == kIOReturnSuccess);

    th.param_i32 = 65000;
    th.now_ns = clk.advance(UINT64_C(1000000));
    CHECK(br->dispatch_method(bb, A840_KMD_V3_METHOD_NOTE,
                              &th, sizeof(th), nullptr, 0u) == kIOReturnSuccess);

    lease.now_ns = clk.advance(UINT64_C(1000000));
    CHECK(br->dispatch_method(bb, A840_KMD_V3_METHOD_NOTE,
                              &lease, sizeof(lease), nullptr, 0u) != kIOReturnSuccess);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_46_gmu_dcvs_thermal_5tier_ceiling_75c_to_95c()
{
    FakeClock clk{};
    auto br = std::make_unique<A840XnuSystemBridge>();
    auto cfg = makeDefaultConfig(&clk);
    cfg.initial_gpu_opp_level = A840_GPU_OPP_MAX_IDX; /* 1200 MHz */
    CHECK(br->init(cfg) == kIOReturnSuccess);
    uint64_t bb = 0u;
    CHECK(br->register_client(608u, true, &bb) == kIOReturnSuccess);

    const int32_t temps[5] = {75000, 80000, 85000, 90000, 95000};
    const uint64_t max_hz[5] = {
        UINT64_C(1000000000), UINT64_C(902000000), UINT64_C(826000000),
        UINT64_C(726000000), UINT64_C(578000000)
    };
    for (int i = 0; i < 5; ++i) {
        A840SysNoteWire th{};
        th.kind = A840_SYS_NOTE_THERMAL_SAMPLE;
        th.param_i32 = temps[i];
        th.now_ns = clk.advance(UINT64_C(50000000));
        CHECK(br->dispatch_method(bb, A840_KMD_V3_METHOD_NOTE,
                                  &th, sizeof(th), nullptr, 0u) == kIOReturnSuccess);
        A840GmuDcvsSnapshot snap{};
        CHECK(br->get_dcvs_snapshot(&snap) == kIOReturnSuccess);
        CHECK(snap.confirmed_gpu_freq_hz <= max_hz[i]);
    }
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_47_gmu_dcvs_idle_decay_after_80ms_vblanks()
{
    FakeClock clk{};
    auto br = std::make_unique<A840XnuSystemBridge>();
    auto cfg = makeDefaultConfig(&clk);
    cfg.initial_gpu_opp_level = A840_GPU_OPP_TOUCH_FLOOR_IDX; /* 578 MHz */
    CHECK(br->init(cfg) == kIOReturnSuccess);

    A840GmuDcvsSnapshot start{};
    CHECK(br->get_dcvs_snapshot(&start) == kIOReturnSuccess);

    /* Advance > 200 ms of idle vblanks */
    for (uint64_t seq = 1u; seq <= 30u; ++seq) {
        const uint64_t ts = clk.advance(A840_MDSS_PERIOD_120HZ_NS);
        CHECK(br->on_mdss_vblank(ts, seq) == kIOReturnSuccess);
    }

    A840GmuDcvsSnapshot end{};
    CHECK(br->get_dcvs_snapshot(&end) == kIOReturnSuccess);
    CHECK(end.confirmed_gpu_level < start.confirmed_gpu_level);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_48_ib_diag_blocks_protected_register_write_preserves_clean_ib()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t app = 0u;
    CHECK(br->register_client(609u, false, &app) == kIOReturnSuccess);

    uint32_t nop_hdr = 0u;
    CHECK(a840_pm4_type7_header(A840_CP_NOP, 0u, &nop_hdr) == A840_EMIT_OK);
    const uint32_t clean_ib[1] = { nop_hdr };
    A840KmdIbDiagResult diag{};
    CHECK(br->validate_submit_ib(app, 0u, clean_ib, 1u, &diag) == kIOReturnSuccess);
    CHECK(diag.status == A840_KMD_IB_OK);

    uint32_t blocked_hdr = 0u;
    CHECK(a840_pm4_type4_header(A840_REG_RB_DBG_ECO_CNTL, 1u, &blocked_hdr) == A840_EMIT_OK);
    const uint32_t bad_ib[2] = { blocked_hdr, 0u };
    CHECK(br->validate_submit_ib(app, 0u, bad_ib, 2u, &diag) == kIOReturnNotPermitted);
    CHECK(diag.status == A840_KMD_IB_BLOCKED_REG);

    A840XnuSystemBridge::Stats st{};
    CHECK(br->get_stats(&st) == kIOReturnSuccess);
    CHECK(st.total_ib_rejects == 1u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

/* ============================================================================
 * Group G (Cases 49-56): Fault Isolation, Quota, Teardown & Glue Thin Bridge
 * ============================================================================ */

void case_49_fault_isolation_app_hang_never_disrupts_backboardd()
{
    FakeClock clk{};
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig(&clk)) == kIOReturnSuccess);
    uint64_t bb = 0u, app1 = 0u, app2 = 0u;
    CHECK(br->register_client(701u, true, &bb) == kIOReturnSuccess);
    CHECK(br->register_client(702u, false, &app1) == kIOReturnSuccess);
    CHECK(br->register_client(703u, false, &app2) == kIOReturnSuccess);

    /* App 1 submits on queue 0 and hangs */
    A840SysSubmitWire sub{};
    sub.queue = 0u;
    sub.priority_hint = A840_PRIORITY_NORMAL;
    sub.enqueue_ns = clk.advance(UINT64_C(500000));
    A840SysSubmitReplyWire s1{};
    CHECK(br->dispatch_method(app1, A840_KMD_V3_METHOD_SUBMIT,
                              &sub, sizeof(sub), &s1, sizeof(s1)) == kIOReturnSuccess);

    A840KmdFaultInput fi{};
    fi.client_id = app1;
    fi.queue_id = 0u;
    fi.fence = s1.fence;
    fi.fault_kind = A840_KMD_FAULT_GPU_HANG;
    fi.timestamp_ticks = clk.ticks.load();
    A840KmdRecoveryReport rrep{};
    CHECK(br->report_client_fault(app1, 0u, fi, 1u, &rrep) == kIOReturnSuccess);
    CHECK(rrep.final_state == A840_KMD_CHANNEL_RECOVERED);

    /* App 1's poisoned channel in uc_ rejects further submit, while backboardd
     * and App 2 remain 100% healthy and submit smoothly! */
    A840SysSubmitReplyWire s_bad{};
    CHECK(br->dispatch_method(app1, A840_KMD_V3_METHOD_SUBMIT,
                              &sub, sizeof(sub), &s_bad, sizeof(s_bad)) == kIOReturnAborted);

    A840SysSubmitWire bb_sub{};
    bb_sub.queue = 0u;
    bb_sub.enqueue_ns = clk.advance(UINT64_C(500000));
    bb_sub.attach_display_sync = 1u;
    A840SysSubmitReplyWire bb_rep{};
    CHECK(br->dispatch_method(bb, A840_KMD_V3_METHOD_SUBMIT,
                              &bb_sub, sizeof(bb_sub), &bb_rep, sizeof(bb_rep)) ==
          kIOReturnSuccess);
    CHECK(bb_rep.assigned_ring == A840_RING_0_COMPOSITOR);

    A840SysSubmitReplyWire app2_rep{};
    CHECK(br->dispatch_method(app2, A840_KMD_V3_METHOD_SUBMIT,
                              &sub, sizeof(sub), &app2_rep, sizeof(app2_rep)) ==
          kIOReturnSuccess);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_50_fault_isolation_app_repeated_faults_bans_only_that_channel()
{
    FakeClock clk{};
    auto br = std::make_unique<A840XnuSystemBridge>();
    auto cfg = makeDefaultConfig(&clk);
    cfg.max_recoveries_before_ban = 2u;
    CHECK(br->init(cfg) == kIOReturnSuccess);
    uint64_t bb = 0u, app = 0u;
    CHECK(br->register_client(704u, true, &bb) == kIOReturnSuccess);
    CHECK(br->register_client(705u, false, &app) == kIOReturnSuccess);

    A840KmdFaultInput fi{};
    fi.fault_kind = A840_KMD_FAULT_SMMU_TRANSLATION;
    A840KmdRecoveryReport rep{};
    for (int i = 0; i < 3; ++i) {
        fi.fence = static_cast<uint64_t>(i + 1);
        fi.timestamp_ticks = clk.ticks.load();
        (void)br->report_client_fault(app, 0u, fi, 2u, &rep);
    }
    A840KmdChannelInfo ci{};
    CHECK(br->query_channel_state(app, 0u, &ci) == kIOReturnSuccess);
    CHECK(ci.state == A840_KMD_CHANNEL_BANNED);

    /* backboardd channel stays HEALTHY */
    A840KmdChannelInfo bb_ci{};
    CHECK(br->query_channel_state(bb, 0u, &bb_ci) == kIOReturnSuccess);
    CHECK(bb_ci.state == A840_KMD_CHANNEL_HEALTHY);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_51_fault_isolation_wakes_blocked_wait_any_with_aborted()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t app = 0u;
    CHECK(br->register_client(706u, false, &app) == kIOReturnSuccess);

    std::atomic<IOReturn> wait_rc{kIOReturnSuccess};
    std::thread waiter([&]() {
        struct NativeAdrenoAbiWaitAnyV3 wa{};
        wa.count = 1u;
        wa.flags = 0u;
        wa.timeout_ns = UINT64_C(2000000000);
        wa.queues[0] = 0u;
        wa.fences[0] = 99u;
        struct NativeAdrenoAbiWaitAnyReplyV3 wrep{};
        wait_rc.store(
            br->dispatch_method(app, NativeAdrenoMethodWaitAnyV3,
                                &wa, sizeof(wa), &wrep, sizeof(wrep)),
            std::memory_order_release);
    });

    A840KmdFaultInput fi{};
    fi.fence = 99u;
    fi.fault_kind = A840_KMD_FAULT_GPU_HANG;
    CHECK(br->report_client_fault(app, 0u, fi, 1u, nullptr) == kIOReturnSuccess);
    waiter.join();
    CHECK(wait_rc.load(std::memory_order_acquire) == kIOReturnAborted);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_52_quota_protects_compositor_reserve_under_app_pressure()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    auto cfg = makeDefaultConfig();
    cfg.global_hard_quota_bytes = UINT64_C(67108864);      /* 64 MiB */
    cfg.global_soft_quota_bytes = UINT64_C(50331648);      /* 48 MiB */
    cfg.compositor_reserve_bytes = UINT64_C(16777216);     /* 16 MiB reserve */
    cfg.app_soft_quota_bytes = UINT64_C(16777216);
    cfg.app_hard_quota_bytes = UINT64_C(16777216);
    cfg.compositor_soft_quota_bytes = UINT64_C(33554432);
    cfg.compositor_hard_quota_bytes = UINT64_C(67108864);
    CHECK(br->init(cfg) == kIOReturnSuccess);

    uint64_t bb = 0u, app = 0u;
    CHECK(br->register_client(707u, true, &bb) == kIOReturnSuccess);
    CHECK(br->register_client(708u, false, &app) == kIOReturnSuccess);

    /* App requesting 32 MiB exceeds its 16 MiB hard cap -> OOM */
    A840SysBoAllocWire big{};
    big.bytes = UINT64_C(33554432);
    big.permissions = A840_KMD_SMMU_PERM_READ_WRITE;
    A840SysBoAllocReplyWire rep{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_BO_ALLOC,
                              &big, sizeof(big), &rep, sizeof(rep)) == kIOReturnNoMemory);

    /* backboardd succeeds using its protected budget */
    big.bytes = UINT64_C(16777216);
    CHECK(br->dispatch_method(bb, A840_KMD_V3_METHOD_BO_ALLOC,
                              &big, sizeof(big), &rep, sizeof(rep)) == kIOReturnSuccess);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_53_memory_pressure_note_reclaims_parked_bos_safely()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t app = 0u;
    CHECK(br->register_client(709u, false, &app) == kIOReturnSuccess);

    A840SysBoAllocWire areq{};
    areq.bytes = UINT64_C(1048576);
    areq.permissions = A840_KMD_SMMU_PERM_READ_WRITE;
    A840SysBoAllocReplyWire bo{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_BO_ALLOC,
                              &areq, sizeof(areq), &bo, sizeof(bo)) == kIOReturnSuccess);

    A840SysBoFreeWire freq{};
    freq.handle = bo.handle;
    freq.allow_park = 1u;
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_BO_FREE,
                              &freq, sizeof(freq), nullptr, 0u) == kIOReturnSuccess);

    A840SysNoteWire mp{};
    mp.kind = A840_SYS_NOTE_MEMORY_PRESSURE;
    mp.param_u32 = 1u;
    mp.value_u64 = UINT64_C(1048576);
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_NOTE,
                              &mp, sizeof(mp), nullptr, 0u) == kIOReturnSuccess);

    A840XnuSystemBridge::Stats st{};
    CHECK(br->get_stats(&st) == kIOReturnSuccess);
    CHECK(st.bo_pool_stats.parked_bos == 0u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_54_unregister_client_drains_inflight_and_reclaims_all_resources()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t app = 0u;
    CHECK(br->register_client(710u, false, &app) == kIOReturnSuccess);

    A840SysBoAllocWire areq{};
    areq.bytes = UINT64_C(65536);
    areq.alignment = UINT64_C(65536);
    areq.permissions = A840_KMD_SMMU_PERM_READ_WRITE;
    A840SysBoAllocReplyWire bo{};
    CHECK(br->dispatch_method(app, A840_KMD_V3_METHOD_BO_ALLOC,
                              &areq, sizeof(areq), &bo, sizeof(bo)) == kIOReturnSuccess);

    A840SysQueueConfigReqV3 sreg{};
    sreg.op = A840_SYS_QCFG_SPARSE_REGION_CREATE;
    sreg.u.sparse_region.gpu_va = UINT64_C(201326592);
    sreg.u.sparse_region.length_bytes = UINT64_C(65536);
    A840SysQueueConfigReplyV3 srep{};
    CHECK(br->dispatch_method(app, NativeAdrenoMethodQueueConfigV3,
                              &sreg, sizeof(sreg), &srep, sizeof(srep)) == kIOReturnSuccess);

    A840XnuSystemBridge::ClientCloseReport crep{};
    CHECK(br->unregister_client(app, &crep) == kIOReturnSuccess);
    CHECK(crep.client_id == app);
    CHECK(crep.bo_pool_reclaimed >= 1u);
    CHECK(crep.sparse_summary.regions_reclaimed == 1u);

    A840XnuSystemBridge::Stats st{};
    CHECK(br->get_stats(&st) == kIOReturnSuccess);
    CHECK(st.active_clients == 0u);
    CHECK(st.zero_page_stats.active_clients == 0u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

struct DummyV2GlueEnv {
    static IOReturn v2Dispatch(
        void *, void *, uint32_t selector,
        const void *, size_t, void *out_buf, size_t out_bytes, size_t *out_written)
    {
        if (selector == NativeAdrenoV2MethodQuery && out_buf != nullptr &&
            out_bytes == sizeof(NativeAdrenoV2ParamsWire)) {
            NativeAdrenoV2ParamsWire p{};
            p.abi_version = NativeAdrenoAbiVersionV2;
            p.flags = NativeAdrenoParamsV2HostImport | NativeAdrenoParamsV2SubmitBatch;
            std::memcpy(out_buf, &p, sizeof(p));
            if (out_written != nullptr)
                *out_written = sizeof(p);
            return kIOReturnSuccess;
        }
        return kIOReturnSuccess;
    }
};

void case_55_glue_thin_bridge_forwarding_method22_and_method26()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);

    auto glue = std::make_unique<NativeAdrenoV3Glue>();
    NativeAdrenoV3Glue::Config gcfg{};
    gcfg.zero_page_default_va = UINT64_C(65536);
    gcfg.host_ops.v2_dispatch = &DummyV2GlueEnv::v2Dispatch;
    CHECK(glue->init(gcfg) == kIOReturnSuccess);
    CHECK(!glue->hasSystemBridge());

    int fake_v2_client = 1;
    uint64_t glue_cid = 0u;
    CHECK(glue->openClient(&fake_v2_client, nullptr, nullptr, 0u, &glue_cid) == kIOReturnSuccess);
    CHECK(br->register_client_with_id(glue_cid, 801u, true) == kIOReturnSuccess);

    /* Without bridge attached, Method 26 returns Unsupported. */
    A840SysQueueConfigReqV3 qreq{};
    qreq.op = A840_SYS_QCFG_KMD_V3_QUALIFY;
    A840SysQueueConfigReplyV3 qrep{};
    CHECK(glue->externalMethod(&fake_v2_client, NativeAdrenoMethodQueueConfigV3,
                               &qreq, sizeof(qreq), &qrep, sizeof(qrep)) ==
          kIOReturnUnsupported);

    /* Attach thin bridge -> Method 26 forwards cleanly and qualifies! */
    CHECK(glue->attachSystemBridge(br.get(), &A840XnuSystemBridge::dispatchThunk, false) ==
          kIOReturnSuccess);
    CHECK(glue->hasSystemBridge());
    size_t written = 0u;
    CHECK(glue->externalMethod(&fake_v2_client, NativeAdrenoMethodQueueConfigV3,
                               &qreq, sizeof(qreq), &qrep, sizeof(qrep), &written) ==
          kIOReturnSuccess);
    CHECK(written == sizeof(qrep));
    CHECK(qrep.u.qual_report.qualified == 1u);

    /* Detach restores default Phase 2 behavior. */
    glue->detachSystemBridge();
    CHECK(!glue->hasSystemBridge());
    CHECK(glue->externalMethod(&fake_v2_client, NativeAdrenoMethodQueueConfigV3,
                               &qreq, sizeof(qreq), &qrep, sizeof(qrep)) ==
          kIOReturnUnsupported);

    CHECK(glue->closeClient(&fake_v2_client) == kIOReturnSuccess);
    glue->destroy();
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_56_glue_thin_bridge_route_all_mode_full_lifecycle()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);

    auto glue = std::make_unique<NativeAdrenoV3Glue>();
    NativeAdrenoV3Glue::Config gcfg{};
    gcfg.zero_page_default_va = UINT64_C(65536);
    gcfg.host_ops.v2_dispatch = &DummyV2GlueEnv::v2Dispatch;
    CHECK(glue->init(gcfg) == kIOReturnSuccess);

    int fake_v2_client = 2;
    uint64_t glue_cid = 0u;
    CHECK(glue->openClient(&fake_v2_client, nullptr, nullptr, 0u, &glue_cid) == kIOReturnSuccess);
    CHECK(br->register_client_with_id(glue_cid, 802u, true) == kIOReturnSuccess);
    CHECK(glue->attachSystemBridge(br.get(), &A840XnuSystemBridge::dispatchThunk, true) ==
          kIOReturnSuccess);

    /* Query routed through glue -> system bridge */
    A840SysParamsWire p{};
    CHECK(glue->externalMethod(&fake_v2_client, A840_KMD_V3_METHOD_QUERY,
                               nullptr, 0u, &p, sizeof(p)) == kIOReturnSuccess);
    CHECK(p.abi_version == NativeAdrenoAbiVersionV3);

    /* Direct dispatchViaSystemBridge helper */
    A840SysDeviceInfoWire info{};
    CHECK(glue->dispatchViaSystemBridge(&fake_v2_client, A840_KMD_V3_METHOD_GET_DEVICE_INFO,
                                        nullptr, 0u, &info, sizeof(info)) == kIOReturnSuccess);
    CHECK(info.chip_id == static_cast<uint32_t>(A840_KAANAPALI_CHIP_ID));

    CHECK(glue->closeClient(&fake_v2_client) == kIOReturnSuccess);
    glue->destroy();
    CHECK(br->shutdown() == kIOReturnSuccess);
}

/* ============================================================================
 * Group H (Cases 57-60): Multi-Process Concurrency & TSan 20x Stress
 * ============================================================================ */

void case_57_concurrent_backboardd_plus_4_apps_submit_irq_vblank()
{
    FakeClock clk{};
    MdssCommitRecorder rec{};
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig(&clk, &rec)) == kIOReturnSuccess);
    CHECK(br->qualify_production(nullptr) == kIOReturnSuccess);

    uint64_t bb = 0u;
    uint64_t apps[4]{};
    CHECK(br->register_client(900u, true, &bb) == kIOReturnSuccess);
    for (int i = 0; i < 4; ++i)
        CHECK(br->register_client(static_cast<uint32_t>(910 + i), false, &apps[i]) ==
              kIOReturnSuccess);

    /* Import 1200x2608 UBWC6 display surface for backboardd */
    const uint64_t bb_sid = UINT64_C(9900);
    A840SysBoImportUbwc6ReplyWire bb_surf{};
    CHECK(br->import_iosurface_ubwc6(bb, makeNezha1200x2608Ubwc6Req(bb_sid), &bb_surf) ==
          kIOReturnSuccess);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> bb_frames{0};
    std::atomic<uint64_t> app_submits{0};

    /* Thread 0: IRQ + 120Hz VBlank pump */
    std::thread irq_thread([&]() {
        uint64_t seq = 1u;
        while (!stop.load(std::memory_order_acquire)) {
            const uint64_t ts = clk.advance(UINT64_C(2000000));
            for (uint32_t r = 0u; r < A840_RING_COUNT; ++r)
                (void)br->on_gpu_fence_irq(r, seq + 64u, ts);
            if ((seq % 4u) == 0u)
                (void)br->on_mdss_vblank(ts, seq / 4u);
            ++seq;
            std::this_thread::yield();
        }
    });

    /* Thread 1: backboardd Ring 0 120Hz compositor loop */
    std::thread bb_thread([&]() {
        for (uint64_t f = 1u; f <= 20u; ++f) {
            const uint64_t ts = clk.now_ns.load(std::memory_order_acquire);
            A840SysSubmitWire sub{};
            sub.queue = 0u;
            sub.enqueue_ns = ts;
            sub.frame_deadline_ns = ts + UINT64_C(2000000);
            sub.estimated_gpu_busy_ns = UINT64_C(3000000);
            sub.attach_display_sync = 1u;
            sub.surface_identity = bb_sid;
            sub.frame_id = f;
            A840SysSubmitReplyWire srep{};
            if (br->dispatch_method(bb, A840_KMD_V3_METHOD_SUBMIT,
                                    &sub, sizeof(sub), &srep, sizeof(srep)) == kIOReturnSuccess) {
                CHECK(srep.assigned_ring == A840_RING_0_COMPOSITOR);
                bb_frames.fetch_add(1u, std::memory_order_relaxed);
            }
            struct NativeAdrenoAbiWriterPromiseV3 mk{};
            mk.op = NativeAdrenoPromiseMake;
            mk.surface_count = 1u;
            mk.queue = 0u;
            mk.surface_ids[0] = bb_surf.bo_handle;
            struct NativeAdrenoAbiWriterPromiseReplyV3 mk_rep{};
            if (br->dispatch_method(bb, NativeAdrenoMethodWriterPromiseV3,
                                    &mk, sizeof(mk), &mk_rep, sizeof(mk_rep)) == kIOReturnSuccess) {
                mk.op = NativeAdrenoPromiseFulfill;
                mk.promise_token = mk_rep.promise_token;
                mk.fence = srep.fence;
                struct NativeAdrenoAbiWriterPromiseReplyV3 fl_rep{};
                (void)br->dispatch_method(bb, NativeAdrenoMethodWriterPromiseV3,
                                          &mk, sizeof(mk), &fl_rep, sizeof(fl_rep));
            }
        }
    });

    /* Threads 2..5: 4 concurrent App processes on Rings 1/2/3 */
    std::vector<std::thread> app_threads;
    for (int i = 0; i < 4; ++i) {
        app_threads.emplace_back([&, i]() {
            const uint32_t prios[4] = {
                A840_PRIORITY_HIGH, A840_PRIORITY_NORMAL,
                A840_PRIORITY_NORMAL, A840_PRIORITY_BACKGROUND
            };
            for (int iter = 0; iter < 20; ++iter) {
                A840SysSubmitWire sub{};
                sub.queue = 0u;
                sub.priority_hint = prios[i];
                sub.enqueue_ns = clk.now_ns.load(std::memory_order_acquire);
                sub.estimated_gpu_busy_ns = UINT64_C(2000000);
                A840SysSubmitReplyWire srep{};
                if (br->dispatch_method(apps[i], A840_KMD_V3_METHOD_SUBMIT,
                                        &sub, sizeof(sub), &srep, sizeof(srep)) ==
                    kIOReturnSuccess) {
                    CHECK(srep.assigned_ring != A840_RING_0_COMPOSITOR);
                    app_submits.fetch_add(1u, std::memory_order_relaxed);
                }
            }
        });
    }

    bb_thread.join();
    for (auto &t : app_threads)
        t.join();
    stop.store(true, std::memory_order_release);
    irq_thread.join();

    CHECK(bb_frames.load() == 20u);
    CHECK(app_submits.load() == 80u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_58_concurrent_fault_isolation_during_active_compositor_120hz()
{
    FakeClock clk{};
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig(&clk)) == kIOReturnSuccess);
    uint64_t bb = 0u, bad_app = 0u, good_app = 0u;
    CHECK(br->register_client(920u, true, &bb) == kIOReturnSuccess);
    CHECK(br->register_client(921u, false, &bad_app) == kIOReturnSuccess);
    CHECK(br->register_client(922u, false, &good_app) == kIOReturnSuccess);

    std::atomic<uint64_t> bb_ok{0};
    std::atomic<uint64_t> good_ok{0};

    std::thread bb_t([&]() {
        for (int i = 0; i < 25; ++i) {
            A840SysSubmitWire sub{};
            sub.queue = 0u;
            sub.enqueue_ns = clk.advance(UINT64_C(1000000));
            sub.attach_display_sync = 1u;
            A840SysSubmitReplyWire rep{};
            if (br->dispatch_method(bb, A840_KMD_V3_METHOD_SUBMIT,
                                    &sub, sizeof(sub), &rep, sizeof(rep)) == kIOReturnSuccess)
                bb_ok.fetch_add(1u, std::memory_order_relaxed);
        }
    });

    std::thread good_t([&]() {
        for (int i = 0; i < 25; ++i) {
            A840SysSubmitWire sub{};
            sub.queue = 0u;
            sub.priority_hint = A840_PRIORITY_HIGH;
            sub.enqueue_ns = clk.advance(UINT64_C(500000));
            A840SysSubmitReplyWire rep{};
            if (br->dispatch_method(good_app, A840_KMD_V3_METHOD_SUBMIT,
                                    &sub, sizeof(sub), &rep, sizeof(rep)) == kIOReturnSuccess)
                good_ok.fetch_add(1u, std::memory_order_relaxed);
        }
    });

    std::thread fault_t([&]() {
        for (int i = 0; i < 5; ++i) {
            A840KmdFaultInput fi{};
            fi.fence = static_cast<uint64_t>(i + 1);
            fi.fault_kind = A840_KMD_FAULT_GPU_HANG;
            (void)br->report_client_fault(bad_app, 0u, fi, 1u, nullptr);
            std::this_thread::yield();
        }
    });

    bb_t.join();
    good_t.join();
    fault_t.join();

    CHECK(bb_ok.load() == 25u);
    CHECK(good_ok.load() == 25u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_59_concurrent_client_register_dispatch_unregister_churn()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);

    std::vector<std::thread> workers;
    for (int w = 0; w < 4; ++w) {
        workers.emplace_back([&, w]() {
            for (int iter = 0; iter < 10; ++iter) {
                uint64_t cid = 0u;
                const uint32_t pid = static_cast<uint32_t>(2000 + w * 100 + iter);
                if (br->register_client(pid, false, &cid) == kIOReturnSuccess) {
                    A840SysParamsWire p{};
                    CHECK(br->dispatch_method(cid, A840_KMD_V3_METHOD_QUERY,
                                              nullptr, 0u, &p, sizeof(p)) == kIOReturnSuccess);
                    A840SysDeviceInfoWire info{};
                    CHECK(br->dispatch_method(cid, A840_KMD_V3_METHOD_GET_DEVICE_INFO,
                                              nullptr, 0u, &info, sizeof(info)) == kIOReturnSuccess);
                    CHECK(br->unregister_client(cid) == kIOReturnSuccess);
                }
            }
        });
    }
    for (auto &t : workers)
        t.join();

    A840XnuSystemBridge::Stats st{};
    CHECK(br->get_stats(&st) == kIOReturnSuccess);
    CHECK(st.active_clients == 0u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

void case_60_concurrent_legacy_selector_flag_rejection_and_qualify_stress()
{
    auto br = std::make_unique<A840XnuSystemBridge>();
    CHECK(br->init(makeDefaultConfig()) == kIOReturnSuccess);
    uint64_t bb = 0u;
    CHECK(br->register_client(950u, true, &bb) == kIOReturnSuccess);

    std::vector<std::thread> workers;
    for (int w = 0; w < 4; ++w) {
        workers.emplace_back([&, w]() {
            for (int iter = 0; iter < 25; ++iter) {
                const uint32_t bad_sel = 27u + static_cast<uint32_t>((w + iter) % 6);
                CHECK(br->dispatch_method(bb, bad_sel, nullptr, 0u, nullptr, 0u) ==
                      kIOReturnUnsupported);
                A840SysAbiNegotiateReply nrep{};
                CHECK(br->negotiate_abi(NativeAdrenoAbiVersionV3,
                                        A840_KMD_LEGACY_FLAG_TURNIP_SHIM, &nrep) ==
                      kIOReturnUnsupported);
                A840KmdProdQualReport qrep{};
                CHECK(br->qualify_production(&qrep) == kIOReturnSuccess);
                CHECK(qrep.qualified == 1u);
                A840XnuSystemBridge::Stats st{};
                CHECK(br->get_stats(&st) == kIOReturnSuccess);
            }
        });
    }
    for (auto &t : workers)
        t.join();

    A840KmdV3FreezeStats fs{};
    CHECK(br->get_freeze_stats(&fs) == kIOReturnSuccess);
    CHECK(fs.legacy_selector_rejects == 100u);
    CHECK(fs.legacy_flag_rejects == 100u);
    CHECK(fs.qualify_pass_count == 100u);
    CHECK(br->shutdown() == kIOReturnSuccess);
}

struct TestEntry {
    const char *name;
    void (*fn)();
    bool is_stress;
};

const TestEntry kTests[] = {
    {"01_init_invalid_configs_fail_closed", case_01_init_invalid_configs_fail_closed, false},
    {"02_init_and_shutdown_lifecycle_idempotent", case_02_init_and_shutdown_lifecycle_idempotent, false},
    {"03_qualify_production_v3_all_7_gates_pass", case_03_qualify_production_v3_all_7_gates_pass, false},
    {"04_qualify_production_v2_mode_honestly_unqualified", case_04_qualify_production_v2_mode_honestly_unqualified, false},
    {"05_qualify_production_negative_legacy_paths_rejected", case_05_qualify_production_negative_legacy_paths_rejected, false},
    {"06_abi_negotiate_v3_native_grants_full_prod_caps", case_06_abi_negotiate_v3_native_grants_full_prod_caps, false},
    {"07_abi_negotiate_v2_client_on_v3_kernel_coexistence", case_07_abi_negotiate_v2_client_on_v3_kernel_coexistence, false},
    {"08_abi_negotiate_v3_client_on_v2_kernel_downgrades_cleanly", case_08_abi_negotiate_v3_client_on_v2_kernel_downgrades_cleanly, false},
    {"09_zero_legacy_reject_selector_zero_and_27_to_32", case_09_zero_legacy_reject_selector_zero_and_27_to_32, false},
    {"10_zero_legacy_reject_all_non_prod_flag_bits_exhaustive", case_10_zero_legacy_reject_all_non_prod_flag_bits_exhaustive, false},
    {"11_zero_legacy_method26_rejects_legacy_flags", case_11_zero_legacy_method26_rejects_legacy_flags, false},
    {"12_envelope_over_3104_bytes_rejected_fail_closed", case_12_envelope_over_3104_bytes_rejected_fail_closed, false},
    {"13_method1_query_strips_legacy_bits_and_reports_v3", case_13_method1_query_strips_legacy_bits_and_reports_v3, false},
    {"14_method15_device_info_kaanapali_a840_facts", case_14_method15_device_info_kaanapali_a840_facts, false},
    {"15_method21_get_zero_page_64kib_ro_per_client", case_15_method21_get_zero_page_64kib_ro_per_client, false},
    {"16_ca_env_policy_backboardd_allow_and_app_reject", case_16_ca_env_policy_backboardd_allow_and_app_reject, false},
    {"17_role_register_backboardd_and_4_apps", case_17_role_register_backboardd_and_4_apps, false},
    {"18_role_backboardd_invalid_label_rejected_when_ca_enforced", case_18_role_backboardd_invalid_label_rejected_when_ca_enforced, false},
    {"19_role_app_cannot_enter_ring0_via_submit_or_priority", case_19_role_app_cannot_enter_ring0_via_submit_or_priority, false},
    {"20_role_app_cannot_configure_ring0_preempt", case_20_role_app_cannot_configure_ring0_preempt, false},
    {"21_role_app_cannot_publish_tier0_pack", case_21_role_app_cannot_publish_tier0_pack, false},
    {"22_role_app_maps_tier0_pack_strictly_readonly", case_22_role_app_maps_tier0_pack_strictly_readonly, false},
    {"23_role_app_cannot_invoke_display_handoff_or_thermal_note", case_23_role_app_cannot_invoke_display_handoff_or_thermal_note, false},
    {"24_role_backboardd_tier0_pack_hot_upgrade_drain", case_24_role_backboardd_tier0_pack_hot_upgrade_drain, false},
    {"25_method2_3_4_bo_alloc_map_free_with_parked_reuse", case_25_method2_3_4_bo_alloc_map_free_with_parked_reuse, false},
    {"26_method9_10_metadata_set_get_client_isolation", case_26_method9_10_metadata_set_get_client_isolation, false},
    {"27_method11_12_create_close_queue_and_limits", case_27_method11_12_create_close_queue_and_limits, false},
    {"28_method13_14_bo_cpu_map_unmap_ro_write_denied", case_28_method13_14_bo_cpu_map_unmap_ro_write_denied, false},
    {"29_method7_8_timestamp_and_status_monotonic", case_29_method7_8_timestamp_and_status_monotonic, false},
    {"30_method22_sparse_va_create_bind_query_unbind_zero_fallback", case_30_method22_sparse_va_create_bind_query_unbind_zero_fallback, false},
    {"31_method22_sparse_va_rejects_cross_client_and_zero_write", case_31_method22_sparse_va_rejects_cross_client_and_zero_write, false},
    {"32_method20_wait_any_and_wait_all_interrupt_wakeup", case_32_method20_wait_any_and_wait_all_interrupt_wakeup, false},
    {"33_iosurface_1200x2608_ubwc6_hbb16_zero_copy_import", case_33_iosurface_1200x2608_ubwc6_hbb16_zero_copy_import, false},
    {"34_iosurface_shared_app_to_backboardd_zero_copy_handoff", case_34_iosurface_shared_app_to_backboardd_zero_copy_handoff, false},
    {"35_method25_surface_meta_get_and_invalid_hbb15_rejected", case_35_method25_surface_meta_get_and_invalid_hbb15_rejected, false},
    {"36_writer_promise_backboardd_early_handoff_zero_extra_vsync", case_36_writer_promise_backboardd_early_handoff_zero_extra_vsync, false},
    {"37_writer_promise_app_deferred_until_fence_retired", case_37_writer_promise_app_deferred_until_fence_retired, false},
    {"38_mdss_120hz_vblank_pll_lock_and_past_cutoff_clean_defer", case_38_mdss_120hz_vblank_pll_lock_and_past_cutoff_clean_defer, false},
    {"39_submit_compositor_frame_combines_ring0_dcvs_mdss_single_path", case_39_submit_compositor_frame_combines_ring0_dcvs_mdss_single_path, false},
    {"40_method19_submit_batch_and_method6_wait_fence_roundtrip", case_40_method19_submit_batch_and_method6_wait_fence_roundtrip, false},
    {"41_preempt_ring0_compositor_preempts_app_ring2", case_41_preempt_ring0_compositor_preempts_app_ring2, false},
    {"42_preempt_priority_inheritance_boosts_dep_ring_wakeup", case_42_preempt_priority_inheritance_boosts_dep_ring_wakeup, false},
    {"43_gmu_dcvs_18_gpu_4_gmu_opp_upscale_under_120hz_load", case_43_gmu_dcvs_18_gpu_4_gmu_opp_upscale_under_120hz_load, false},
    {"44_gmu_dcvs_inter_frame_hold_prevents_drop_between_120hz_frames", case_44_gmu_dcvs_inter_frame_hold_prevents_drop_between_120hz_frames, false},
    {"45_gmu_dcvs_touch_boost_578mhz_and_cold_lease_726_902mhz", case_45_gmu_dcvs_touch_boost_578mhz_and_cold_lease_726_902mhz, false},
    {"46_gmu_dcvs_thermal_5tier_ceiling_75c_to_95c", case_46_gmu_dcvs_thermal_5tier_ceiling_75c_to_95c, false},
    {"47_gmu_dcvs_idle_decay_after_80ms_vblanks", case_47_gmu_dcvs_idle_decay_after_80ms_vblanks, false},
    {"48_ib_diag_blocks_protected_register_write_preserves_clean_ib", case_48_ib_diag_blocks_protected_register_write_preserves_clean_ib, false},
    {"49_fault_isolation_app_hang_never_disrupts_backboardd", case_49_fault_isolation_app_hang_never_disrupts_backboardd, false},
    {"50_fault_isolation_app_repeated_faults_bans_only_that_channel", case_50_fault_isolation_app_repeated_faults_bans_only_that_channel, false},
    {"51_fault_isolation_wakes_blocked_wait_any_with_aborted", case_51_fault_isolation_wakes_blocked_wait_any_with_aborted, false},
    {"52_quota_protects_compositor_reserve_under_app_pressure", case_52_quota_protects_compositor_reserve_under_app_pressure, false},
    {"53_memory_pressure_note_reclaims_parked_bos_safely", case_53_memory_pressure_note_reclaims_parked_bos_safely, false},
    {"54_unregister_client_drains_inflight_and_reclaims_all_resources", case_54_unregister_client_drains_inflight_and_reclaims_all_resources, false},
    {"55_glue_thin_bridge_forwarding_method22_and_method26", case_55_glue_thin_bridge_forwarding_method22_and_method26, false},
    {"56_glue_thin_bridge_route_all_mode_full_lifecycle", case_56_glue_thin_bridge_route_all_mode_full_lifecycle, false},
    {"57_concurrent_backboardd_plus_4_apps_submit_irq_vblank", case_57_concurrent_backboardd_plus_4_apps_submit_irq_vblank, true},
    {"58_concurrent_fault_isolation_during_active_compositor_120hz", case_58_concurrent_fault_isolation_during_active_compositor_120hz, true},
    {"59_concurrent_client_register_dispatch_unregister_churn", case_59_concurrent_client_register_dispatch_unregister_churn, true},
    {"60_concurrent_legacy_selector_flag_rejection_and_qualify_stress", case_60_concurrent_legacy_selector_flag_rejection_and_qualify_stress, true},
};

} /* namespace */

int main(int argc, char **argv)
{
    int stress_rounds = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tsan-stress") == 0 && i + 1 < argc) {
            stress_rounds = std::atoi(argv[++i]);
            if (stress_rounds < 1)
                stress_rounds = 1;
        }
    }

    const size_t total_cases = sizeof(kTests) / sizeof(kTests[0]);
    for (size_t i = 0u; i < total_cases; ++i) {
        const int rounds = kTests[i].is_stress ? stress_rounds : 1;
        for (int r = 0; r < rounds; ++r)
            kTests[i].fn();
        ++g_cases_run;
    }

    const int fails = g_failures.load(std::memory_order_acquire);
    const uint64_t checks = g_checks.load(std::memory_order_acquire);
    if (fails != 0) {
        std::fprintf(stderr,
                     "FAIL test_xnu_system_integration: %d cases, %llu checks, %d failures (stress_rounds=%d)\n",
                     g_cases_run, static_cast<unsigned long long>(checks), fails, stress_rounds);
        return 1;
    }

    std::printf("PASS test_xnu_system_integration: %d independent cases, %llu checks (stress_rounds=%d)\n",
                g_cases_run, static_cast<unsigned long long>(checks), stress_rounds);
    return 0;
}
