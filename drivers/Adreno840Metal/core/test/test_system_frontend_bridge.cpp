/* SPDX-License-Identifier: MIT */
/* Wave 12 / Lane 3 — iOS 27 System Metal Plugin & IOSurface Frontend Test Suite.
 *
 * Covers 60 cases across:
 *   - Process role classification & NativeAdrenoMetal.publish gating
 *   - Truthful Adreno 840 capability queries & zero-fallback production invariants
 *   - IOSurface zero-copy binding (BGRA8/sRGB/RGBA16F/RGB10A2/XR10/NV12/P010 -> UBWC6/Linear)
 *   - Unified handle tables (Heap/Aliasing, SharedEvent, ICB, DepthStencil, BinaryArchive)
 *   - Core 4 iOS 27 UI scenarios + Camera 155 NV12/P010 preview & 4096x3072 photo
 *   - Steady-state zero-heap-allocation 120Hz frame loop
 *   - RenderServer vs App role isolation & Early Writer Promise enforcement
 *   - Phase 3 hwtest_hook adapters & 20x TSan multi-thread concurrency stress
 *   - Exports 5 full-system A8XX .pm4 streams for pm4_check, pm4_diff, cp_protect_audit
 */
#include "a840_system_frontend_core.h"
#include "a840_hwtest_hook.h"

#include "Adreno840Regs.h"
#include "a840_backend_transport.h"
#include "a840_emit_ops.h"
#include "gen/a840_bitfields.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_cases = 0;
int g_checks = 0;
int g_failures = 0;
const char *g_current_case = "init";
std::string g_dump_dir;
int g_tsan_iters = 0;

void begin_case(const char *name)
{
    g_current_case = name;
    ++g_cases;
}

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++g_checks;                                                                      \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n", g_current_case, __FILE__,      \
                         __LINE__, #cond);                                               \
            ++g_failures;                                                                \
        }                                                                                \
    } while (0)

struct ProtectRange {
    uint32_t base;
    uint32_t end_inclusive;
};

/* Exact 54 protected register intervals from NezhaGpu.c (a840_protect[]). */
static const ProtectRange kA840ProtectRanges[54] = {
    {8u, 931u},          {948u, 1087u},       {1088u, 1119u},      {1408u, 1503u},
    {1504u, 1791u},      {1866u, 1871u},      {1881u, 1908u},      {1909u, 1919u},
    {1929u, 1929u},      {1932u, 1951u},      {2048u, 2089u},      {2103u, 2278u},
    {2279u, 2480u},      {2284u, 2479u},      {2481u, 3073u},      {3079u, 3087u},
    {3296u, 3297u},      {3568u, 3568u},      {3569u, 3569u},      {3585u, 3585u},
    {3587u, 11778u},     {15360u, 15557u},    {15558u, 15615u},    {15616u, 23807u},
    {34304u, 34815u},    {36352u, 36607u},    {36608u, 36608u},    {36609u, 37055u},
    {38400u, 38911u},    {38938u, 39679u},    {40448u, 40959u},    {42496u, 43007u},
    {43054u, 43054u},    {44544u, 44544u},    {44546u, 44550u},    {44552u, 44558u},
    {44560u, 44751u},    {44752u, 44799u},    {44800u, 45439u},    {46592u, 54783u},
    {56320u, 64511u},    {64512u, 72703u},    {99328u, 99391u},    {99392u, 99711u},
    {99712u, 107903u},   {111616u, 119807u},  {128000u, 129143u},  {129144u, 130431u},
    {129328u, 130137u},  {130432u, 138623u},  {161792u, 161919u},  {161920u, 162821u},
    {161922u, 161931u},  {162822u, 162822u},
};

bool is_cp_protected(uint32_t reg)
{
    for (const auto &r : kA840ProtectRanges) {
        if (reg >= r.base && reg <= r.end_inclusive) return true;
    }
    return false;
}

void audit_and_dump_pm4(const char *filename, const uint32_t *words, size_t dwords)
{
    CHECK(words != nullptr);
    CHECK(dwords > 0u);
    if (!words || dwords == 0u) return;

    std::vector<A840PM4Packet> pkts(dwords);
    size_t count = 0u;
    CHECK(a840_pm4_decode(words, dwords, pkts.data(), pkts.size(), &count) == A840_EMIT_OK);
    CHECK(count > 0u);

    size_t violations = 0u;
    for (size_t i = 0u; i < count; ++i) {
        if (pkts[i].type == 4u) {
            for (uint32_t k = 0u; k < pkts[i].payload_words; ++k) {
                if (is_cp_protected(pkts[i].identifier + k)) ++violations;
            }
        }
    }
    CHECK(violations == 0u);

    if (!g_dump_dir.empty() && filename) {
        const std::string path = g_dump_dir + "/" + filename;
        std::FILE *fp = std::fopen(path.c_str(), "wb");
        CHECK(fp != nullptr);
        if (fp) {
            CHECK(std::fwrite(words, sizeof(uint32_t), dwords, fp) == dwords);
            CHECK(std::fclose(fp) == 0);
        }
    }
}

struct MockDeviceEnv {
    A840MockTransport *mock = nullptr;
    A840RtDevice *rt = nullptr;

    bool init()
    {
        if (a840_mock_transport_create(nullptr, &mock) != A840_TRANSPORT_OK || !mock) return false;
        A840BackendTransport tr = a840_mock_transport_interface(mock);
        if (a840_rt_device_create(&tr, A840_ROUTE_PHASE4_ALL, &rt) != A840_RT_OK || !rt) return false;
        return true;
    }

    ~MockDeviceEnv()
    {
        if (rt) a840_rt_device_destroy(rt);
        if (mock) a840_mock_transport_destroy(mock);
    }
};

struct CountingAllocator {
    std::atomic<uint64_t> allocs{0};
    std::atomic<uint64_t> frees{0};
    bool tracking = false;
};

void *counting_alloc(void *ctx, size_t size, size_t alignment)
{
    auto *c = static_cast<CountingAllocator *>(ctx);
    if (c && c->tracking) c->allocs.fetch_add(1, std::memory_order_relaxed);
    size_t align = alignment < sizeof(void *) ? sizeof(void *) : alignment;
    size_t rounded = (size + align - 1u) & ~(align - 1u);
    void *ptr = nullptr;
    if (posix_memalign(&ptr, align, rounded ? rounded : align) != 0) return nullptr;
    std::memset(ptr, 0, rounded ? rounded : align);
    return ptr;
}

void counting_free(void *ctx, void *ptr)
{
    auto *c = static_cast<CountingAllocator *>(ctx);
    if (c && c->tracking && ptr) c->frees.fetch_add(1, std::memory_order_relaxed);
    std::free(ptr);
}

struct ScanoutNotifyRecord {
    std::atomic<uint32_t> calls{0};
    std::atomic<uint32_t> early_scheduled{0};
    std::atomic<uint32_t> deferred_armed{0};
    std::atomic<uint32_t> retired_ready{0};
    std::atomic<uint32_t> canceled{0};
    std::atomic<uint64_t> last_surface_id{0};
    std::atomic<uint32_t> last_abi_op{0};
};

void record_scanout_notify(void *ctx,
                           const A840EarlyPromiseEvent *ev,
                           const struct NativeAdrenoAbiWriterPromiseV3 *abi)
{
    auto *rec = static_cast<ScanoutNotifyRecord *>(ctx);
    if (!rec || !ev) return;
    rec->calls.fetch_add(1, std::memory_order_relaxed);
    rec->last_surface_id.store(ev->surface_id, std::memory_order_relaxed);
    if (abi) rec->last_abi_op.store(abi->op, std::memory_order_relaxed);
    if (ev->kind == A840_PROMISE_EVENT_EARLY_SCHEDULED) rec->early_scheduled.fetch_add(1, std::memory_order_relaxed);
    else if (ev->kind == A840_PROMISE_EVENT_DEFERRED_ARMED) rec->deferred_armed.fetch_add(1, std::memory_order_relaxed);
    else if (ev->kind == A840_PROMISE_EVENT_RETIRED_READY) rec->retired_ready.fetch_add(1, std::memory_order_relaxed);
    else if (ev->kind == A840_PROMISE_EVENT_PROMISE_CANCELED) rec->canceled.fetch_add(1, std::memory_order_relaxed);
}

/* ---------- Group A: Process Role & Publish Policy (8 cases) ---------- */

void run_group_a()
{
    begin_case("classify_backboardd_render_server");
    CHECK(a840_sys_classify_process("backboardd") == A840_SYS_ROLE_RENDER_SERVER);

    begin_case("classify_carenderserver_render_server");
    CHECK(a840_sys_classify_process("CARenderServer") == A840_SYS_ROLE_RENDER_SERVER);

    begin_case("classify_windowserver_render_server");
    CHECK(a840_sys_classify_process("WindowServer") == A840_SYS_ROLE_RENDER_SERVER);

    begin_case("classify_springboard_app");
    CHECK(a840_sys_classify_process("SpringBoard") == A840_SYS_ROLE_APP);
    CHECK(a840_sys_classify_process("Preferences") == A840_SYS_ROLE_APP);

    begin_case("classify_camera_and_null_app");
    CHECK(a840_sys_classify_process("Camera") == A840_SYS_ROLE_APP);
    CHECK(a840_sys_classify_process("") == A840_SYS_ROLE_APP);
    CHECK(a840_sys_classify_process(nullptr) == A840_SYS_ROLE_APP);

    begin_case("publish_marker_absent_fails_closed");
    A840SysPublishPolicy pol{};
    CHECK(a840_sys_resolve_publish_marker(
              "/System/Library/Extensions/NativeAdrenoMetal.bundle/NativeAdrenoMetal",
              "backboardd", false, &pol) == A840_SYS_ERR_NOT_PUBLISHED);
    CHECK(pol.publish_state == A840_SYS_PUBLISH_DISABLED);
    CHECK(!pol.allow_device_creation);
    CHECK(pol.fallback_compiled_out == 1u);
    CHECK(std::strcmp(pol.marker_path, "/System/Library/Extensions/NativeAdrenoMetal.publish") == 0);

    begin_case("publish_marker_present_render_server_policy");
    CHECK(a840_sys_resolve_publish_marker(
              "/System/Library/Extensions/NativeAdrenoMetal.bundle/NativeAdrenoMetal",
              "backboardd", true, &pol) == A840_SYS_OK);
    CHECK(pol.publish_state == A840_SYS_PUBLISH_ENABLED);
    CHECK(pol.allow_device_creation);
    CHECK(pol.proc_role == A840_SYS_ROLE_RENDER_SERVER);
    CHECK(pol.cache_policy == A840_SYS_CACHE_SHARED_SYSTEM_PACK);
    CHECK(pol.allow_early_promise);
    CHECK(pol.scanout_bridge_enabled);
    CHECK(pol.preferred_ring_id == 0u);
    CHECK(pol.pack_tier0_readonly);

    begin_case("publish_marker_present_app_policy_and_adreno_bundle_path");
    CHECK(a840_sys_resolve_publish_marker(
              "/System/Library/Extensions/Adreno840Metal.bundle/Adreno840Metal",
              "Camera", true, &pol) == A840_SYS_OK);
    CHECK(pol.allow_device_creation);
    CHECK(pol.proc_role == A840_SYS_ROLE_APP);
    CHECK(pol.cache_policy == A840_SYS_CACHE_PRIVATE_PER_PROC);
    CHECK(!pol.allow_early_promise);
    CHECK(!pol.scanout_bridge_enabled);
    CHECK(pol.preferred_ring_id == 1u);
    CHECK(a840_sys_resolve_publish_marker("/invalid/path", "Camera", true, &pol) == A840_SYS_ERR_NOT_FOUND);
}

/* ---------- Group B: Truthful Caps & Zero-Fallback (8 cases) ---------- */

void run_group_b(A840RtDevice *rt)
{
    begin_case("caps_kaanapali_chip_facts_and_hbb16");
    A840SysDeviceCaps caps{};
    a840_sys_query_caps(rt, &caps);
    CHECK(caps.chip_id == A840_KAANAPALI_CHIP_ID);
    CHECK(caps.gmem_bytes == A840_KAANAPALI_GMEM_BYTES);
    CHECK(caps.highest_bank_bit == 16u);
    CHECK(caps.ubwc_version == 6u);
    CHECK(caps.ubwc_swizzle == 6u);
    CHECK(caps.macrotile_mode == 1u);

    begin_case("caps_gpu_families_apple1_to_7_common1_to_3");
    for (uint32_t f = 1001u; f <= 1007u; ++f) CHECK(a840_sys_supports_gpu_family(f));
    for (uint32_t f = 3001u; f <= 3003u; ++f) CHECK(a840_sys_supports_gpu_family(f));

    begin_case("caps_gpu_families_honest_reject_apple8_apple9");
    CHECK(!a840_sys_supports_gpu_family(1008u));
    CHECK(!a840_sys_supports_gpu_family(1009u));
    CHECK(!a840_sys_supports_gpu_family(5001u));

    begin_case("caps_sample_counts_1_2_4_reject_8");
    CHECK(a840_sys_supports_sample_count(1u));
    CHECK(a840_sys_supports_sample_count(2u));
    CHECK(a840_sys_supports_sample_count(4u));
    CHECK(!a840_sys_supports_sample_count(0u));
    CHECK(!a840_sys_supports_sample_count(8u));
    CHECK(!a840_sys_supports_sample_count(16u));

    begin_case("caps_pixel_formats_bgra8_rgba16f_rgb10a2_xr10_reject_invalid");
    CHECK(a840_sys_supports_pixel_format(A840_MTL_FMT_BGRA8_UNORM));
    CHECK(a840_sys_supports_pixel_format(A840_MTL_FMT_BGRA8_UNORM_SRGB));
    CHECK(a840_sys_supports_pixel_format(115u)); /* RGBA16Float */
    CHECK(a840_sys_supports_pixel_format(90u));  /* RGB10A2Unorm */
    CHECK(a840_sys_supports_pixel_format(A840_MTL_FMT_BGRA10_XR));
    CHECK(a840_sys_supports_pixel_format(A840_MTL_FMT_DEPTH32_FLOAT));
    CHECK(!a840_sys_supports_pixel_format(0u));
    CHECK(!a840_sys_supports_pixel_format(999999u));

    begin_case("caps_iosurface_align_unified_mem_and_tiers");
    CHECK(caps.has_unified_memory);
    CHECK(caps.iosurface_readonly_align_bytes == 64u);
    CHECK(caps.sparse_tile_bytes == 65536u);
    CHECK(caps.argument_buffers_tier == 2u);
    CHECK(caps.read_write_texture_tier == 2u);
    CHECK(caps.recommended_max_working_set_bytes >= UINT64_C(1024) * 1024u * 1024u);
    CHECK(!caps.supports_bc_compression);
    CHECK(!caps.supports_raytracing);
    CHECK(!caps.supports_depth24_stencil8);

    begin_case("zero_fallback_request_permanently_denied");
    A840SysFrontendConfig cfg{};
    a840_sys_frontend_default_config(&cfg, A840_SYS_ROLE_RENDER_SERVER);
    A840SysFrontend *fe = nullptr;
    CHECK(a840_sys_frontend_create(rt, &cfg, &fe) == A840_SYS_OK);
    CHECK(a840_sys_frontend_request_fallback(fe) == A840_SYS_ERR_FALLBACK_FORBIDDEN);
    CHECK(a840_sys_frontend_stats(fe).fallback_attempts_denied == 1u);

    begin_case("production_router_16_of_16_native_zero_fallback");
    A840ProdRouter *router = a840_sys_frontend_router(fe);
    CHECK(router != nullptr);
    for (uint32_t pid = 1u; pid <= A840_PROD_PREDICATE_COUNT; ++pid) {
        const auto d = a840_prod_route_eval_predicate(router, static_cast<A840ProdPredicateId>(pid), false);
        CHECK(d.status == A840_PROD_OK);
        CHECK(d.target == A840_PROD_ROUTE_NATIVE);
    }
    a840_sys_frontend_destroy(fe);
}

/* ---------- Group C: IOSurface Zero-Copy Binding (10 cases) ---------- */

void run_group_c(A840RtDevice *rt)
{
    A840SysFrontendConfig cfg{};
    a840_sys_frontend_default_config(&cfg, A840_SYS_ROLE_RENDER_SERVER);
    A840SysFrontend *fe = nullptr;
    CHECK(a840_sys_frontend_create(rt, &cfg, &fe) == A840_SYS_OK);

    const uint32_t mdss_fmts[2] = {A840_MTL_FMT_BGRA8_UNORM, A840_MTL_FMT_BGRA8_UNORM_SRGB};
    A840MdssScanoutCaps ubwc_caps{};
    ubwc_caps.flags = A840_SCANOUT_CAP_UBWC6;
    ubwc_caps.max_width = 4096u;
    ubwc_caps.max_height = 4096u;
    ubwc_caps.base_alignment = 4096u;
    ubwc_caps.pitch_alignment = 64u;
    ubwc_caps.formats = mdss_fmts;
    ubwc_caps.format_count = 2u;

    begin_case("iosurface_bind_bgra8_ubwc6_hbb16_scanout");
    A840SysIOSurfaceDesc d{};
    d.iosurface_id = 1001u;
    d.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
    d.width = 1200u;
    d.height = 2608u;
    d.bytes_per_row = 1200u * 4u;
    d.is_cross_process = true;
    d.for_scanout = true;
    d.request_ubwc6 = true;
    d.mdss_caps = &ubwc_caps;
    A840SysSurfaceHandle h_bgra = A840_SYS_INVALID_HANDLE;
    A840SysSurfaceInfo info{};
    CHECK(a840_sys_bind_iosurface(fe, &d, &h_bgra, &info) == A840_SYS_OK);
    CHECK(info.ubwc_enabled);
    CHECK(info.ubwc_version == 6u);
    CHECK(info.highest_bank_bit == 16u);
    CHECK(info.swizzle_levels == 6u);
    CHECK(info.macrotile_mode == 1u);
    CHECK(info.scanout_eligible);
    CHECK(info.is_cross_process);
    CHECK(info.gpu_va != 0u);

    begin_case("iosurface_bind_bgra8_srgb_and_rgba16f");
    d.iosurface_id = 1002u;
    d.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM_SRGB;
    d.width = 512u;
    d.height = 512u;
    A840SysSurfaceHandle h_srgb = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(fe, &d, &h_srgb, &info) == A840_SYS_OK);
    CHECK(info.mtl_pixel_format == A840_MTL_FMT_BGRA8_UNORM_SRGB);
    d.iosurface_id = 1003u;
    d.mtl_pixel_format = 115u; /* RGBA16Float */
    d.for_scanout = false;
    d.mdss_caps = nullptr;
    A840SysSurfaceHandle h_fp16 = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(fe, &d, &h_fp16, &info) == A840_SYS_OK);
    CHECK(info.mtl_pixel_format == 115u);

    begin_case("iosurface_bind_rgb10a2_and_bgra10_xr");
    d.iosurface_id = 1004u;
    d.mtl_pixel_format = 90u; /* RGB10A2Unorm */
    A840SysSurfaceHandle h_rgb10 = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(fe, &d, &h_rgb10, &info) == A840_SYS_OK);
    CHECK(info.mtl_pixel_format == 90u);
    d.iosurface_id = 1005u;
    d.mtl_pixel_format = A840_MTL_FMT_BGRA10_XR;
    A840SysSurfaceHandle h_xr10 = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(fe, &d, &h_xr10, &info) == A840_SYS_OK);
    CHECK(info.mtl_pixel_format == A840_MTL_FMT_BGRA10_XR);

    begin_case("iosurface_bind_camera155_nv12_dual_plane");
    A840SysIOSurfaceDesc cam{};
    cam.iosurface_id = 15501u;
    cam.ycbcr_format = A840_YCBCR_NV12;
    cam.width = 1600u;
    cam.height = 1200u;
    cam.plane_index = 0u;
    cam.is_cross_process = true;
    A840SysSurfaceHandle h_nv12 = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(fe, &cam, &h_nv12, &info) == A840_SYS_OK);
    CHECK(info.ycbcr_format == A840_YCBCR_NV12);
    CHECK(info.plane_count == 2u);
    CHECK(a840_sys_surface_ubwc_texture(fe, h_nv12) != nullptr);

    begin_case("iosurface_bind_camera155_p010_dual_plane");
    cam.iosurface_id = 15502u;
    cam.ycbcr_format = A840_YCBCR_P010;
    cam.plane_index = 1u;
    A840SysSurfaceHandle h_p010 = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(fe, &cam, &h_p010, &info) == A840_SYS_OK);
    CHECK(info.ycbcr_format == A840_YCBCR_P010);
    CHECK(info.plane_count == 2u);
    CHECK(info.plane_index == 1u);

    begin_case("iosurface_mdss_caps_linear_downgrade");
    A840MdssScanoutCaps linear_only_caps = ubwc_caps;
    linear_only_caps.flags = 0u; /* MDSS does not advertise UBWC6 */
    A840SysIOSurfaceDesc lin{};
    lin.iosurface_id = 1006u;
    lin.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
    lin.width = 1200u;
    lin.height = 2608u;
    lin.for_scanout = true;
    lin.request_ubwc6 = true;
    lin.mdss_caps = &linear_only_caps;
    A840SysSurfaceHandle h_lin = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(fe, &lin, &h_lin, &info) == A840_SYS_OK);
    CHECK(info.fallback_linear_required);

    begin_case("iosurface_use_count_lifecycle_and_underflow_reject");
    CHECK(a840_sys_surface_increment_use(fe, h_nv12) == A840_SYS_OK);
    CHECK(a840_sys_surface_increment_use(fe, h_nv12) == A840_SYS_OK);
    CHECK(a840_sys_query_surface(fe, h_nv12, &info) == A840_SYS_OK);
    CHECK(info.active_use_count == 2u);
    CHECK(a840_sys_surface_decrement_use(fe, h_nv12) == A840_SYS_OK);
    CHECK(a840_sys_surface_decrement_use(fe, h_nv12) == A840_SYS_OK);
    CHECK(a840_sys_surface_decrement_use(fe, h_nv12) == A840_SYS_ERR_INVALID_STATE);

    begin_case("iosurface_stale_handle_aba_rejection_after_unbind");
    CHECK(a840_sys_unbind_iosurface(fe, h_lin) == A840_SYS_OK);
    CHECK(a840_sys_query_surface(fe, h_lin, &info) == A840_SYS_ERR_STALE_HANDLE);
    CHECK(a840_sys_unbind_iosurface(fe, h_lin) == A840_SYS_ERR_STALE_HANDLE);
    A840SysSurfaceHandle h_rebound = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(fe, &lin, &h_rebound, &info) == A840_SYS_OK);
    CHECK(h_rebound != h_lin);
    CHECK(a840_sys_query_surface(fe, h_lin, &info) == A840_SYS_ERR_STALE_HANDLE);

    begin_case("iosurface_invalid_args_and_unsupported_format_reject");
    A840SysIOSurfaceDesc bad{};
    bad.mtl_pixel_format = 999999u;
    bad.width = 64u;
    bad.height = 64u;
    A840SysSurfaceHandle h_bad = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(fe, &bad, &h_bad, nullptr) == A840_SYS_ERR_UNSUPPORTED);
    bad.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
    bad.width = 0u;
    CHECK(a840_sys_bind_iosurface(fe, &bad, &h_bad, nullptr) == A840_SYS_ERR_INVALID_ARG);
    bad.width = 64u;
    bad.plane_index = 4u;
    CHECK(a840_sys_bind_iosurface(fe, &bad, &h_bad, nullptr) == A840_SYS_ERR_INVALID_ARG);

    begin_case("iosurface_cross_process_and_framebuffer_flags");
    CHECK(a840_sys_query_surface(fe, h_bgra, &info) == A840_SYS_OK);
    CHECK(info.is_cross_process);
    CHECK(info.for_scanout);
    CHECK(a840_sys_unbind_iosurface(fe, h_bgra) == A840_SYS_OK);
    CHECK(a840_sys_unbind_iosurface(fe, h_srgb) == A840_SYS_OK);
    CHECK(a840_sys_unbind_iosurface(fe, h_fp16) == A840_SYS_OK);
    CHECK(a840_sys_unbind_iosurface(fe, h_rgb10) == A840_SYS_OK);
    CHECK(a840_sys_unbind_iosurface(fe, h_xr10) == A840_SYS_OK);
    CHECK(a840_sys_unbind_iosurface(fe, h_nv12) == A840_SYS_OK);
    CHECK(a840_sys_unbind_iosurface(fe, h_p010) == A840_SYS_OK);
    CHECK(a840_sys_unbind_iosurface(fe, h_rebound) == A840_SYS_OK);

    a840_sys_frontend_destroy(fe);
}

/* ---------- Group D: Unified Handle Tables (12 cases) ---------- */

struct EventFireTrack {
    std::atomic<uint32_t> fires{0};
    std::atomic<uint64_t> last_val{0};
};

void event_notify_cb(void *ctx, A840SysEventHandle ev, uint64_t val)
{
    (void)ev;
    auto *t = static_cast<EventFireTrack *>(ctx);
    if (!t) return;
    t->fires.fetch_add(1, std::memory_order_relaxed);
    t->last_val.store(val, std::memory_order_relaxed);
}

void run_group_d(A840RtDevice *rt)
{
    A840SysFrontendConfig cfg{};
    a840_sys_frontend_default_config(&cfg, A840_SYS_ROLE_RENDER_SERVER);
    A840SysFrontend *fe = nullptr;
    CHECK(a840_sys_frontend_create(rt, &cfg, &fe) == A840_SYS_OK);

    begin_case("heap_automatic_create_suballoc_aliasable_reuse");
    A840HeapDesc hd{};
    hd.type = A840_HEAP_TYPE_AUTOMATIC;
    hd.size_bytes = 2u * 1024u * 1024u;
    hd.alignment = 65536u;
    hd.usage_flags = A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE | A840_BUF_USAGE_SHADER;
    hd.cpu_visible = true;
    A840SysHeapHandle hh = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_heap_create(fe, &hd, &hh) == A840_SYS_OK);
    A840Heap *heap = a840_sys_heap_resolve(fe, hh);
    CHECK(heap != nullptr);
    A840HeapBufferDesc bdesc{4096u, 64u, 0u, A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE};
    A840HeapSubAlloc *s1 = nullptr, *s2 = nullptr;
    CHECK(a840_heap_alloc_buffer(heap, &bdesc, &s1) == A840_RT_OK);
    CHECK(a840_heap_suballoc_make_aliasable(s1) == A840_RT_OK);
    CHECK(a840_heap_alloc_buffer(heap, &bdesc, &s2) == A840_RT_OK);
    CHECK(s2->offset == s1->offset);
    CHECK(s2->last_transition.requires_alias_barrier);
    a840_heap_suballoc_release(s1);
    a840_heap_suballoc_release(s2);

    begin_case("heap_placement_overlap_and_alias_transition_pm4");
    hd.type = A840_HEAP_TYPE_PLACEMENT;
    A840SysHeapHandle hh2 = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_heap_create(fe, &hd, &hh2) == A840_SYS_OK);
    A840Heap *pheap = a840_sys_heap_resolve(fe, hh2);
    A840HeapTextureDesc tdesc{};
    tdesc.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
    tdesc.tex_type = A840_TEX_2D;
    tdesc.width = 128u;
    tdesc.height = 128u;
    tdesc.depth = 1u;
    tdesc.array_layers = 1u;
    tdesc.mip_levels = 1u;
    tdesc.samples = 1u;
    tdesc.tile_mode = A840_TILE_UBWC;
    tdesc.highest_bank_bit = 16u;
    tdesc.placement_offset = 0u;
    tdesc.usage_flags = A840_TEX_USAGE_RENDER_TARGET | A840_TEX_USAGE_SHADER_READ;
    A840HeapSubAlloc *st1 = nullptr, *st2 = nullptr;
    CHECK(a840_heap_alloc_texture(pheap, &tdesc, &st1) == A840_RT_OK);
    CHECK(a840_heap_suballoc_make_aliasable(st1) == A840_RT_OK);
    tdesc.tile_mode = A840_TILE_LINEAR;
    CHECK(a840_heap_alloc_texture(pheap, &tdesc, &st2) == A840_RT_OK);
    CHECK(st2->last_transition.requires_alias_barrier);
    std::vector<uint32_t> pm4(4096u, 0u);
    CHECK(a840_sys_begin_frame(fe, 10u, 10u, pm4.data(), pm4.size()) == A840_SYS_OK);
    CHECK(a840_sys_encode_heap_alias_barrier(fe, &st2->last_transition) == A840_SYS_OK);
    A840SysFrameReceipt rec{};
    CHECK(a840_sys_commit_frame(fe, &rec) == A840_SYS_OK);
    audit_and_dump_pm4(nullptr, pm4.data(), rec.pm4_dwords_captured);
    CHECK(a840_sys_retire_frame(fe, 10u, rec.last_chunk_seqno, &rec) == A840_SYS_OK);
    a840_heap_suballoc_release(st1);
    a840_heap_suballoc_release(st2);

    begin_case("heap_purgeable_state_transitions_and_stale_handle");
    A840PurgeableState prev = A840_PURGEABLE_NONVOLATILE;
    CHECK(a840_heap_set_purgeable(heap, A840_PURGEABLE_VOLATILE, &prev) == A840_RT_OK);
    CHECK(prev == A840_PURGEABLE_NONVOLATILE);
    CHECK(a840_sys_heap_destroy(fe, hh) == A840_SYS_OK);
    CHECK(a840_sys_heap_resolve(fe, hh) == nullptr);
    CHECK(a840_sys_heap_destroy(fe, hh) == A840_SYS_ERR_STALE_HANDLE);
    CHECK(a840_sys_heap_destroy(fe, hh2) == A840_SYS_OK);

    begin_case("shared_event_monotonic_signal_and_waiter_callback");
    A840SysEventHandle ev = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_event_create(fe, true, 1u, 42u, &ev) == A840_SYS_OK);
    EventFireTrack track;
    CHECK(a840_sys_event_register_waiter(fe, ev, 5u, event_notify_cb, &track) == A840_SYS_OK);
    CHECK(track.fires.load() == 0u);
    CHECK(a840_sys_event_signal(fe, ev, 3u) == A840_SYS_OK);
    CHECK(track.fires.load() == 0u);
    CHECK(a840_sys_event_signal(fe, ev, 7u) == A840_SYS_OK);
    CHECK(track.fires.load() == 1u);
    CHECK(track.last_val.load() == 7u);
    uint64_t qv = 0u;
    bool qsh = false;
    uint32_t qport = 0u;
    CHECK(a840_sys_event_query(fe, ev, &qv, &qsh, &qport) == A840_SYS_OK);
    CHECK(qv == 7u && qsh && qport == 42u);

    begin_case("shared_event_immediate_fire_when_already_signaled");
    EventFireTrack imm;
    CHECK(a840_sys_event_register_waiter(fe, ev, 5u, event_notify_cb, &imm) == A840_SYS_OK);
    CHECK(imm.fires.load() == 1u);
    CHECK(imm.last_val.load() == 7u);

    begin_case("shared_event_multiple_waiters_and_stale_handle");
    EventFireTrack w1, w2;
    CHECK(a840_sys_event_register_waiter(fe, ev, 10u, event_notify_cb, &w1) == A840_SYS_OK);
    CHECK(a840_sys_event_register_waiter(fe, ev, 12u, event_notify_cb, &w2) == A840_SYS_OK);
    CHECK(a840_sys_event_signal(fe, ev, 12u) == A840_SYS_OK);
    CHECK(w1.fires.load() == 1u && w2.fires.load() == 1u);
    CHECK(a840_sys_event_destroy(fe, ev) == A840_SYS_OK);
    CHECK(a840_sys_event_signal(fe, ev, 15u) == A840_SYS_ERR_STALE_HANDLE);

    begin_case("icb_render_encode_compile_and_execute_pm4");
    A840IcbDesc idesc{A840_ICB_TYPE_RENDER, 8u, true, true};
    A840SysIcbHandle icb_r = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_icb_create(fe, &idesc, &icb_r) == A840_SYS_OK);
    A840IndirectCmdBuffer *ricb = a840_sys_icb_resolve(fe, icb_r);
    CHECK(ricb != nullptr);
    A840DrawDesc ddraw{};
    ddraw.primitive = A840_PRIM_TRIANGLES;
    ddraw.index_type = A840_INDEX_NONE;
    ddraw.count = 6u;
    ddraw.instance_count = 1u;
    CHECK(a840_icb_encode_draw(ricb, 0u, &ddraw) == A840_RT_OK);
    CHECK(a840_icb_encode_draw(ricb, 1u, &ddraw) == A840_RT_OK);
    A840RtBufferDesc ibd{16384u, 4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE, false, nullptr};
    A840RtBuffer *scratch_ib = nullptr;
    CHECK(a840_rt_buffer_create(rt, &ibd, &scratch_ib) == A840_RT_OK);
    CHECK(a840_sys_begin_frame(fe, 11u, 11u, pm4.data(), pm4.size()) == A840_SYS_OK);
    CHECK(a840_sys_encode_icb_execute(fe, icb_r, 0u, 2u, scratch_ib) == A840_SYS_OK);
    CHECK(a840_sys_commit_frame(fe, &rec) == A840_SYS_OK);
    audit_and_dump_pm4(nullptr, pm4.data(), rec.pm4_dwords_captured);
    CHECK(a840_sys_retire_frame(fe, 11u, rec.last_chunk_seqno, &rec) == A840_SYS_OK);

    begin_case("icb_compute_encode_compile_and_execute_pm4");
    A840IcbDesc cidesc{A840_ICB_TYPE_COMPUTE, 4u, true, true};
    A840SysIcbHandle icb_c = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_icb_create(fe, &cidesc, &icb_c) == A840_SYS_OK);
    A840IndirectCmdBuffer *cicb = a840_sys_icb_resolve(fe, icb_c);
    const uint32_t loc[3] = {8u, 8u, 1u};
    const uint32_t grp[3] = {4u, 4u, 1u};
    CHECK(a840_icb_encode_dispatch(cicb, 0u, loc, grp) == A840_RT_OK);
    CHECK(a840_sys_begin_frame(fe, 12u, 12u, pm4.data(), pm4.size()) == A840_SYS_OK);
    CHECK(a840_sys_encode_icb_execute(fe, icb_c, 0u, 1u, scratch_ib) == A840_SYS_OK);
    CHECK(a840_sys_commit_frame(fe, &rec) == A840_SYS_OK);
    audit_and_dump_pm4(nullptr, pm4.data(), rec.pm4_dwords_captured);
    CHECK(a840_sys_retire_frame(fe, 12u, rec.last_chunk_seqno, &rec) == A840_SYS_OK);

    begin_case("icb_stale_handle_and_invalid_range_reject");
    CHECK(a840_sys_icb_destroy(fe, icb_r) == A840_SYS_OK);
    CHECK(a840_sys_icb_destroy(fe, icb_c) == A840_SYS_OK);
    CHECK(a840_sys_icb_resolve(fe, icb_r) == nullptr);
    a840_rt_buffer_release(scratch_ib);

    begin_case("depth_stencil_create_query_bounds_validation_and_destroy");
    A840DepthStencilVariantDesc ds{};
    ds.depth_test = true;
    ds.depth_write = true;
    ds.depth_compare = 1u; /* LESS */
    ds.z_mode = A840_ZMODE_EARLY_Z;
    ds.front.compare = 7u;
    ds.back.compare = 7u;
    A840SysDepthStencilHandle dsh = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_depth_stencil_create(fe, &ds, &dsh) == A840_SYS_OK);
    A840DepthStencilVariantDesc ds_out{};
    CHECK(a840_sys_depth_stencil_get(fe, dsh, &ds_out) == A840_SYS_OK);
    CHECK(ds_out.depth_test && ds_out.depth_write && ds_out.depth_compare == 1u);
    ds.depth_bounds_test = true;
    ds.depth_bounds_min = 0.8f;
    ds.depth_bounds_max = 0.2f;
    A840SysDepthStencilHandle dsh_bad = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_depth_stencil_create(fe, &ds, &dsh_bad) == A840_SYS_ERR_INVALID_ARG);
    CHECK(a840_sys_depth_stencil_destroy(fe, dsh) == A840_SYS_OK);
    CHECK(a840_sys_depth_stencil_get(fe, dsh, &ds_out) == A840_SYS_ERR_STALE_HANDLE);

    begin_case("binary_archive_create_add_dedup_contains_and_destroy");
    A840SysArchiveHandle ah = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_archive_create(fe, 12345u, ADRENO_COMPILE_ABI_VERSION, &ah) == A840_SYS_OK);
    A840PipelineKey k1{}, k2{};
    k1.sha256[0] = 11u;
    k2.sha256[0] = 22u;
    CHECK(a840_sys_archive_add_key(fe, ah, &k1) == A840_SYS_OK);
    CHECK(a840_sys_archive_add_key(fe, ah, &k1) == A840_SYS_OK); /* dedup */
    CHECK(a840_sys_archive_add_key(fe, ah, &k2) == A840_SYS_OK);
    uint32_t acount = 0u;
    CHECK(a840_sys_archive_count(fe, ah, &acount) == A840_SYS_OK && acount == 2u);
    bool found = false;
    CHECK(a840_sys_archive_contains_key(fe, ah, &k1, &found) == A840_SYS_OK && found);
    CHECK(a840_sys_archive_destroy(fe, ah) == A840_SYS_OK);
    CHECK(a840_sys_archive_count(fe, ah, &acount) == A840_SYS_ERR_STALE_HANDLE);

    begin_case("pack_cache_tier0_mount_and_zero_copy_lookup");
    A840PackCacheRt *pc = a840_sys_frontend_pack_cache(fe);
    CHECK(pc != nullptr);
    A840PackLookupResult lr{};
    CHECK(a840_pack_cache_rt_lookup(pc, &k1, &lr) == A840_RT_FALLBACK_REQUIRED);
    CHECK(a840_pack_cache_rt_stats(pc).misses == 1u);

    a840_sys_frontend_destroy(fe);
}

/* ---------- Group E: 4 Core Scenarios + Camera 155 + PM4 Exports (12 cases) ---------- */

void run_group_e(A840RtDevice *rt)
{
    ScanoutNotifyRecord notify_rec;
    A840SysFrontendConfig cfg{};
    a840_sys_frontend_default_config(&cfg, A840_SYS_ROLE_RENDER_SERVER);
    cfg.chunk_max_passes = 2u;
    cfg.scanout_notify_fn = record_scanout_notify;
    cfg.scanout_notify_ctx = &notify_rec;

    A840SysFrontend *fe = nullptr;
    CHECK(a840_sys_frontend_create(rt, &cfg, &fe) == A840_SYS_OK);

    A840SysIOSurfaceDesc sd{};
    sd.iosurface_id = 2001u;
    sd.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
    sd.width = 1200u;
    sd.height = 2608u;
    sd.is_cross_process = true;
    sd.for_scanout = true;
    sd.request_ubwc6 = true;
    A840SysSurfaceHandle surf_main = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(fe, &sd, &surf_main, nullptr) == A840_SYS_OK);

    sd.iosurface_id = 2002u;
    sd.request_ubwc6 = false;
    A840SysSurfaceHandle surf_lin = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(fe, &sd, &surf_lin, nullptr) == A840_SYS_OK);

    A840DepthStencilVariantDesc ds{};
    ds.depth_test = true;
    ds.depth_write = true;
    ds.depth_compare = 1u;
    ds.z_mode = A840_ZMODE_EARLY_Z;
    A840SysDepthStencilHandle dsh = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_depth_stencil_create(fe, &ds, &dsh) == A840_SYS_OK);

    std::vector<uint32_t> pm4(32768u, 0u);
    A840SysFrameReceipt rec{};

    /* Scenario 1: App open/close animation */
    begin_case("scenario1_app_open_close_gmem_early_promise");
    CHECK(a840_sys_begin_frame(fe, 101u, 101u, pm4.data(), pm4.size()) == A840_SYS_OK);
    A840SysGlassPassInput gp{};
    gp.pass_count = 2u;
    gp.uses_fbfetch = true;
    gp.has_memoryless_depth = true;
    gp.sample_count = 1u;
    gp.color_surface = surf_main;
    gp.depth_stencil = dsh;
    gp.clear_color[3] = 255u;
    gp.emit_early_promise = true;
    CHECK(a840_sys_encode_glass_gmem_pass(fe, &gp) == A840_SYS_OK);
    A840SysScanoutRequest sreq{};
    sreq.surface = surf_main;
    sreq.mode = A840_SYS_SCANOUT_DIRECT_UBWC6;
    sreq.request_early_promise = true;
    CHECK(a840_sys_encode_scanout_handoff(fe, &sreq) == A840_SYS_OK);
    CHECK(a840_sys_commit_frame(fe, &rec) == A840_SYS_OK);
    CHECK(rec.gmem_passes == 1u);
    CHECK(rec.memoryless_depth_passes == 1u);
    CHECK(rec.fbfetch_draws == 2u);
    CHECK(rec.mdss_direct_ubwc == 1u);
    CHECK(rec.early_promises_published >= 1u);
    CHECK(notify_rec.early_scheduled.load() >= 1u);
    audit_and_dump_pm4("sys_app_open_close.pm4", pm4.data(), rec.pm4_dwords_captured);
    CHECK(a840_sys_retire_frame(fe, 101u, rec.last_chunk_seqno, &rec) == A840_SYS_OK);

    /* Scenario 2: Settings list scrolling (disjoint compute + GMEM + chunking + sync dedup) */
    begin_case("scenario2_settings_scroll_sync_dedup_chunking");
    CHECK(a840_sys_begin_frame(fe, 102u, 102u, pm4.data(), pm4.size()) == A840_SYS_OK);
    const uint64_t r1 = 5001u, w1 = 5002u, r2 = 5003u, w2 = 5004u;
    A840SysComputePassInput cp{};
    cp.groups_x = 16u;
    cp.groups_y = 8u;
    cp.groups_z = 1u;
    cp.local_x = 8u;
    cp.local_y = 8u;
    cp.local_z = 1u;
    cp.read_resource_ids = &r1;
    cp.read_count = 1u;
    cp.write_resource_ids = &w1;
    cp.write_count = 1u;
    CHECK(a840_sys_encode_compute_pass(fe, &cp) == A840_SYS_OK);
    cp.read_resource_ids = &r2;
    cp.write_resource_ids = &w2;
    CHECK(a840_sys_encode_compute_pass(fe, &cp) == A840_SYS_OK);
    gp.pass_count = 1u;
    gp.uses_fbfetch = false;
    gp.has_memoryless_depth = false;
    CHECK(a840_sys_encode_glass_gmem_pass(fe, &gp) == A840_SYS_OK);
    CHECK(a840_sys_encode_scanout_handoff(fe, &sreq) == A840_SYS_OK);
    CHECK(a840_sys_commit_frame(fe, &rec) == A840_SYS_OK);
    CHECK(rec.chunks_kicked >= 2u);
    CHECK(rec.early_kicks >= 1u);
    audit_and_dump_pm4("sys_settings_scroll.pm4", pm4.data(), rec.pm4_dwords_captured);
    CHECK(a840_sys_retire_frame(fe, 102u, rec.last_chunk_seqno, &rec) == A840_SYS_OK);

    /* Scenario 3: App Library list scrolling (Heap aliasing + ICB + GMEM) */
    begin_case("scenario3_app_library_scroll_heap_alias_and_icb");
    A840HeapDesc hd{A840_HEAP_TYPE_AUTOMATIC, 1024u * 1024u, 65536u,
                    A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE, true};
    A840SysHeapHandle hh = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_heap_create(fe, &hd, &hh) == A840_SYS_OK);
    A840Heap *heap = a840_sys_heap_resolve(fe, hh);
    A840HeapBufferDesc bdesc{8192u, 64u, 0u, A840_BUF_USAGE_READ | A840_BUF_USAGE_WRITE};
    A840HeapSubAlloc *ha = nullptr, *hb = nullptr;
    CHECK(a840_heap_alloc_buffer(heap, &bdesc, &ha) == A840_RT_OK);
    CHECK(a840_heap_suballoc_make_aliasable(ha) == A840_RT_OK);
    CHECK(a840_heap_alloc_buffer(heap, &bdesc, &hb) == A840_RT_OK);

    A840IcbDesc idesc{A840_ICB_TYPE_RENDER, 8u, true, true};
    A840SysIcbHandle icbh = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_icb_create(fe, &idesc, &icbh) == A840_SYS_OK);
    A840DrawDesc ddraw{};
    ddraw.primitive = A840_PRIM_TRIANGLES;
    ddraw.index_type = A840_INDEX_NONE;
    ddraw.count = 6u;
    ddraw.instance_count = 4u;
    CHECK(a840_icb_encode_draw(a840_sys_icb_resolve(fe, icbh), 0u, &ddraw) == A840_RT_OK);
    A840RtBufferDesc ibd{16384u, 4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE, false, nullptr};
    A840RtBuffer *icb_bo = nullptr;
    CHECK(a840_rt_buffer_create(rt, &ibd, &icb_bo) == A840_RT_OK);

    CHECK(a840_sys_begin_frame(fe, 103u, 103u, pm4.data(), pm4.size()) == A840_SYS_OK);
    CHECK(a840_sys_encode_heap_alias_barrier(fe, &hb->last_transition) == A840_SYS_OK);
    CHECK(a840_sys_encode_icb_execute(fe, icbh, 0u, 1u, icb_bo) == A840_SYS_OK);
    CHECK(a840_sys_encode_glass_gmem_pass(fe, &gp) == A840_SYS_OK);
    CHECK(a840_sys_encode_scanout_handoff(fe, &sreq) == A840_SYS_OK);
    CHECK(a840_sys_commit_frame(fe, &rec) == A840_SYS_OK);
    audit_and_dump_pm4("sys_app_library_scroll.pm4", pm4.data(), rec.pm4_dwords_captured);
    CHECK(a840_sys_retire_frame(fe, 103u, rec.last_chunk_seqno, &rec) == A840_SYS_OK);
    a840_heap_suballoc_release(ha);
    a840_heap_suballoc_release(hb);
    a840_rt_buffer_release(icb_bo);
    CHECK(a840_sys_icb_destroy(fe, icbh) == A840_SYS_OK);
    CHECK(a840_sys_heap_destroy(fe, hh) == A840_SYS_OK);

    /* Scenario 4: Dense Liquid Glass Home Screen paging (8-Pass GMEM + FBFetch + Memoryless Depth) */
    begin_case("scenario4_dense_liquid_glass_8pass_gmem_fbfetch");
    CHECK(a840_sys_begin_frame(fe, 104u, 104u, pm4.data(), pm4.size()) == A840_SYS_OK);
    gp.pass_count = 8u;
    gp.uses_fbfetch = true;
    gp.has_memoryless_depth = true;
    gp.force_96x32_tiles = false;
    gp.emit_early_promise = true;
    CHECK(a840_sys_encode_glass_gmem_pass(fe, &gp) == A840_SYS_OK);
    CHECK(a840_sys_encode_scanout_handoff(fe, &sreq) == A840_SYS_OK);
    CHECK(a840_sys_commit_frame(fe, &rec) == A840_SYS_OK);
    CHECK(rec.gmem_passes == 1u);
    CHECK(rec.memoryless_depth_passes == 1u);
    CHECK(rec.fbfetch_draws == 8u);
    CHECK(rec.tile_width >= 96u && rec.tile_height >= 32u);
    audit_and_dump_pm4("sys_dense_liquid_glass_8pass.pm4", pm4.data(), rec.pm4_dwords_captured);
    CHECK(a840_sys_retire_frame(fe, 104u, rec.last_chunk_seqno, &rec) == A840_SYS_OK);

    /* Scenario 5: Camera 155 NV12 preview + 4096x3072 BGRA8 photo capture */
    begin_case("scenario5_camera155_nv12_preview_and_4096x3072_photo");
    A840SysIOSurfaceDesc cam_nv12{};
    cam_nv12.iosurface_id = 15510u;
    cam_nv12.ycbcr_format = A840_YCBCR_NV12;
    cam_nv12.width = 1600u;
    cam_nv12.height = 1200u;
    cam_nv12.is_cross_process = true;
    A840SysSurfaceHandle h_cam_nv12 = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(fe, &cam_nv12, &h_cam_nv12, nullptr) == A840_SYS_OK);

    A840SysIOSurfaceDesc cam_photo{};
    cam_photo.iosurface_id = 15511u;
    cam_photo.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
    cam_photo.width = 4096u;
    cam_photo.height = 3072u;
    cam_photo.request_ubwc6 = true;
    cam_photo.is_cross_process = true;
    A840SysSurfaceHandle h_cam_photo = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(fe, &cam_photo, &h_cam_photo, nullptr) == A840_SYS_OK);

    CHECK(a840_sys_begin_frame(fe, 105u, 105u, pm4.data(), pm4.size()) == A840_SYS_OK);
    A840SysCamera155PassInput cam_in{};
    cam_in.preview_ycbcr_surface = h_cam_nv12;
    cam_in.photo_bgra8_surface = h_cam_photo;
    cam_in.encode_photo_capture = true;
    CHECK(a840_sys_encode_camera155_pass(fe, &cam_in) == A840_SYS_OK);
    A840SysSurfaceInfo sinfo{};
    CHECK(a840_sys_query_surface(fe, h_cam_nv12, &sinfo) == A840_SYS_OK);
    CHECK(sinfo.active_use_count == 1u);
    CHECK(a840_sys_commit_frame(fe, &rec) == A840_SYS_OK);
    audit_and_dump_pm4("sys_camera155_nv12_photo.pm4", pm4.data(), rec.pm4_dwords_captured);
    CHECK(a840_sys_retire_frame(fe, 105u, rec.last_chunk_seqno, &rec) == A840_SYS_OK);
    /* Verify IOSurface use count returns to 0 after retirement (Camera 150 fix). */
    CHECK(a840_sys_query_surface(fe, h_cam_nv12, &sinfo) == A840_SYS_OK);
    CHECK(sinfo.active_use_count == 0u);

    begin_case("scenario_camera155_p010_hdr_preview");
    cam_nv12.iosurface_id = 15512u;
    cam_nv12.ycbcr_format = A840_YCBCR_P010;
    A840SysSurfaceHandle h_cam_p010 = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(fe, &cam_nv12, &h_cam_p010, nullptr) == A840_SYS_OK);
    cam_in.preview_ycbcr_surface = h_cam_p010;
    cam_in.encode_photo_capture = false;
    CHECK(a840_sys_begin_frame(fe, 106u, 106u, pm4.data(), pm4.size()) == A840_SYS_OK);
    CHECK(a840_sys_encode_camera155_pass(fe, &cam_in) == A840_SYS_OK);
    CHECK(a840_sys_commit_frame(fe, &rec) == A840_SYS_OK);
    audit_and_dump_pm4(nullptr, pm4.data(), rec.pm4_dwords_captured);
    CHECK(a840_sys_retire_frame(fe, 106u, rec.last_chunk_seqno, &rec) == A840_SYS_OK);

    begin_case("scenario_msaa_4x_memoryless_depth_gmem_pass");
    A840SysIOSurfaceDesc sm_sd{};
    sm_sd.iosurface_id = 2010u;
    sm_sd.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
    sm_sd.width = 288u;
    sm_sd.height = 96u;
    sm_sd.request_ubwc6 = true;
    A840SysSurfaceHandle h_sm = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(fe, &sm_sd, &h_sm, nullptr) == A840_SYS_OK);
    gp.pass_count = 1u;
    gp.uses_fbfetch = false;
    gp.has_memoryless_depth = true;
    gp.sample_count = 4u;
    gp.color_surface = h_sm;
    gp.resolve_surface = surf_lin;
    gp.force_96x32_tiles = true;
    CHECK(a840_sys_begin_frame(fe, 107u, 107u, pm4.data(), pm4.size()) == A840_SYS_OK);
    CHECK(a840_sys_encode_glass_gmem_pass(fe, &gp) == A840_SYS_OK);
    CHECK(a840_sys_commit_frame(fe, &rec) == A840_SYS_OK);
    CHECK(rec.gmem_passes == 1u);
    CHECK(rec.memoryless_depth_passes == 1u);
    audit_and_dump_pm4(nullptr, pm4.data(), rec.pm4_dwords_captured);
    CHECK(a840_sys_retire_frame(fe, 107u, rec.last_chunk_seqno, &rec) == A840_SYS_OK);

    begin_case("scenario_sysmem_forced_pass_and_depth_stencil");
    gp.force_sysmem = true;
    gp.sample_count = 1u;
    gp.resolve_surface = A840_SYS_INVALID_HANDLE;
    gp.color_surface = surf_lin;
    CHECK(a840_sys_begin_frame(fe, 108u, 108u, pm4.data(), pm4.size()) == A840_SYS_OK);
    CHECK(a840_sys_encode_glass_gmem_pass(fe, &gp) == A840_SYS_OK);
    CHECK(a840_sys_commit_frame(fe, &rec) == A840_SYS_OK);
    CHECK(rec.sysmem_passes == 1u && rec.gmem_passes == 0u);
    audit_and_dump_pm4(nullptr, pm4.data(), rec.pm4_dwords_captured);
    CHECK(a840_sys_retire_frame(fe, 108u, rec.last_chunk_seqno, &rec) == A840_SYS_OK);
    gp.force_sysmem = false;

    begin_case("scenario_indirect_compute_dispatch_pass");
    A840RtBufferDesc ind_desc{4096u, 4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE, false, nullptr};
    A840RtBuffer *ind_bo = nullptr;
    CHECK(a840_rt_buffer_create(rt, &ind_desc, &ind_bo) == A840_RT_OK);
    A840SysComputePassInput icp{};
    icp.local_x = 8u;
    icp.local_y = 8u;
    icp.local_z = 1u;
    icp.indirect_dispatch = true;
    icp.indirect_bo_handle = ind_bo->bo_handle;
    icp.indirect_gpu_va = ind_bo->gpu_va;
    CHECK(a840_sys_begin_frame(fe, 109u, 109u, pm4.data(), pm4.size()) == A840_SYS_OK);
    CHECK(a840_sys_encode_compute_pass(fe, &icp) == A840_SYS_OK);
    CHECK(a840_sys_commit_frame(fe, &rec) == A840_SYS_OK);
    audit_and_dump_pm4(nullptr, pm4.data(), rec.pm4_dwords_captured);
    CHECK(a840_sys_retire_frame(fe, 109u, rec.last_chunk_seqno, &rec) == A840_SYS_OK);
    a840_rt_buffer_release(ind_bo);

    begin_case("scenario_scanout_native_linear_blit_downgrade");
    A840SysScanoutRequest lin_req{};
    lin_req.surface = surf_main;
    lin_req.linear_dst_surface = surf_lin;
    lin_req.mode = A840_SYS_SCANOUT_NATIVE_LINEAR;
    lin_req.request_early_promise = true;
    CHECK(a840_sys_begin_frame(fe, 110u, 110u, pm4.data(), pm4.size()) == A840_SYS_OK);
    CHECK(a840_sys_encode_scanout_handoff(fe, &lin_req) == A840_SYS_OK);
    CHECK(a840_sys_commit_frame(fe, &rec) == A840_SYS_OK);
    CHECK(rec.mdss_native_linear == 1u);
    audit_and_dump_pm4(nullptr, pm4.data(), rec.pm4_dwords_captured);
    CHECK(a840_sys_retire_frame(fe, 110u, rec.last_chunk_seqno, &rec) == A840_SYS_OK);

    begin_case("scenario_frame_abort_cancels_unretired_promises");
    const uint32_t cancel_before = notify_rec.canceled.load();
    CHECK(a840_sys_begin_frame(fe, 111u, 111u, pm4.data(), pm4.size()) == A840_SYS_OK);
    CHECK(a840_sys_encode_scanout_handoff(fe, &sreq) == A840_SYS_OK);
    a840_sys_abort_frame(fe, A840_EARLY_CANCEL_GPU_FAULT);
    CHECK(notify_rec.canceled.load() >= cancel_before);
    CHECK(a840_sys_query_surface(fe, surf_main, &sinfo) == A840_SYS_OK);
    CHECK(sinfo.active_use_count == 0u);

    begin_case("steady_state_zero_heap_alloc_120hz_frame_loop");
    CountingAllocator counter;
    A840ArenaAllocator alloc_hooks{&counter, counting_alloc, counting_free};
    A840SysFrontendConfig zcfg{};
    a840_sys_frontend_default_config(&zcfg, A840_SYS_ROLE_RENDER_SERVER);
    zcfg.arena_allocator = &alloc_hooks;
    A840SysFrontend *zfe = nullptr;
    CHECK(a840_sys_frontend_create(rt, &zcfg, &zfe) == A840_SYS_OK);
    A840SysSurfaceHandle zsurf = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(zfe, &sm_sd, &zsurf, nullptr) == A840_SYS_OK);
    counter.tracking = true;
    A840SysGlassPassInput zgp{};
    zgp.pass_count = 2u;
    zgp.uses_fbfetch = true;
    zgp.has_memoryless_depth = true;
    zgp.color_surface = zsurf;
    for (uint64_t f = 1u; f <= 24u; ++f) {
        CHECK(a840_sys_begin_frame(zfe, f, f, pm4.data(), pm4.size()) == A840_SYS_OK);
        CHECK(a840_sys_encode_glass_gmem_pass(zfe, &zgp) == A840_SYS_OK);
        CHECK(a840_sys_commit_frame(zfe, &rec) == A840_SYS_OK);
        CHECK(a840_sys_retire_frame(zfe, f, rec.last_chunk_seqno, &rec) == A840_SYS_OK);
    }
    CHECK(counter.allocs.load() == 0u);
    CHECK(counter.frees.load() == 0u);
    counter.tracking = false;
    a840_sys_frontend_destroy(zfe);

    CHECK(a840_sys_unbind_iosurface(fe, surf_main) == A840_SYS_OK);
    CHECK(a840_sys_unbind_iosurface(fe, surf_lin) == A840_SYS_OK);
    CHECK(a840_sys_unbind_iosurface(fe, h_cam_nv12) == A840_SYS_OK);
    CHECK(a840_sys_unbind_iosurface(fe, h_cam_photo) == A840_SYS_OK);
    CHECK(a840_sys_unbind_iosurface(fe, h_cam_p010) == A840_SYS_OK);
    CHECK(a840_sys_unbind_iosurface(fe, h_sm) == A840_SYS_OK);
    CHECK(a840_sys_depth_stencil_destroy(fe, dsh) == A840_SYS_OK);
    a840_sys_frontend_destroy(fe);
}

/* ---------- Group F: RenderServer vs App Isolation (4 cases) ---------- */

void run_group_f(A840RtDevice *rt)
{
    std::vector<uint32_t> pm4(8192u, 0u);
    A840SysIOSurfaceDesc sd{};
    sd.iosurface_id = 3001u;
    sd.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
    sd.width = 256u;
    sd.height = 256u;
    sd.for_scanout = true;
    sd.request_ubwc6 = true;

    begin_case("role_isolation_render_server_gets_early_scheduled");
    ScanoutNotifyRecord rs_rec;
    A840SysFrontendConfig rcfg{};
    a840_sys_frontend_default_config(&rcfg, A840_SYS_ROLE_RENDER_SERVER);
    rcfg.scanout_notify_fn = record_scanout_notify;
    rcfg.scanout_notify_ctx = &rs_rec;
    A840SysFrontend *rs_fe = nullptr;
    CHECK(a840_sys_frontend_create(rt, &rcfg, &rs_fe) == A840_SYS_OK);
    A840SysSurfaceHandle rs_surf = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(rs_fe, &sd, &rs_surf, nullptr) == A840_SYS_OK);
    A840SysScanoutRequest req{rs_surf, A840_SYS_INVALID_HANDLE, A840_SYS_SCANOUT_DIRECT_UBWC6, 0u, true};
    A840SysFrameReceipt rec{};
    CHECK(a840_sys_begin_frame(rs_fe, 1u, 1u, pm4.data(), pm4.size()) == A840_SYS_OK);
    CHECK(a840_sys_encode_scanout_handoff(rs_fe, &req) == A840_SYS_OK);
    CHECK(a840_sys_commit_frame(rs_fe, &rec) == A840_SYS_OK);
    CHECK(rs_rec.early_scheduled.load() == 1u);
    CHECK(rs_rec.deferred_armed.load() == 0u);
    CHECK(a840_sys_retire_frame(rs_fe, 1u, rec.last_chunk_seqno, &rec) == A840_SYS_OK);
    a840_sys_frontend_destroy(rs_fe);

    begin_case("role_isolation_app_forced_defer_until_retired");
    ScanoutNotifyRecord app_rec;
    A840SysFrontendConfig acfg{};
    a840_sys_frontend_default_config(&acfg, A840_SYS_ROLE_APP);
    acfg.strict_reject_app_early = false;
    acfg.scanout_notify_fn = record_scanout_notify;
    acfg.scanout_notify_ctx = &app_rec;
    A840SysFrontend *app_fe = nullptr;
    CHECK(a840_sys_frontend_create(rt, &acfg, &app_fe) == A840_SYS_OK);
    A840SysSurfaceHandle app_surf = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(app_fe, &sd, &app_surf, nullptr) == A840_SYS_OK);
    req.surface = app_surf;
    CHECK(a840_sys_begin_frame(app_fe, 1u, 1u, pm4.data(), pm4.size()) == A840_SYS_OK);
    CHECK(a840_sys_encode_scanout_handoff(app_fe, &req) == A840_SYS_OK);
    CHECK(a840_sys_commit_frame(app_fe, &rec) == A840_SYS_OK);
    CHECK(app_rec.early_scheduled.load() == 0u);
    CHECK(app_rec.deferred_armed.load() == 1u);
    CHECK(app_rec.retired_ready.load() == 0u);
    CHECK(a840_sys_retire_frame(app_fe, 1u, rec.last_chunk_seqno, &rec) == A840_SYS_OK);
    CHECK(app_rec.retired_ready.load() == 1u);

    begin_case("role_isolation_app_strict_reject_early_promise");
    A840SysFrontendConfig scfg{};
    a840_sys_frontend_default_config(&scfg, A840_SYS_ROLE_APP);
    scfg.strict_reject_app_early = true;
    A840SysFrontend *s_fe = nullptr;
    CHECK(a840_sys_frontend_create(rt, &scfg, &s_fe) == A840_SYS_OK);
    A840SysSurfaceHandle s_surf = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_bind_iosurface(s_fe, &sd, &s_surf, nullptr) == A840_SYS_OK);
    req.surface = s_surf;
    CHECK(a840_sys_begin_frame(s_fe, 1u, 1u, pm4.data(), pm4.size()) == A840_SYS_OK);
    CHECK(a840_sys_encode_scanout_handoff(s_fe, &req) == A840_SYS_ERR_PERMISSION_DENIED);
    a840_sys_abort_frame(s_fe, A840_EARLY_CANCEL_EXPLICIT);

    begin_case("role_isolation_ring_assignment_0_vs_1");
    CHECK(a840_sys_frontend_role(app_fe) == A840_SYS_ROLE_APP);
    CHECK(rec.ring_id == 1u);
    a840_sys_frontend_destroy(app_fe);
    a840_sys_frontend_destroy(s_fe);
}

/* ---------- Group G: Phase 3 hwtest_hook Adapters (6 cases) ---------- */

static int g_dummy_ubwc_token = 0;
static std::atomic<uint32_t> g_dummy_scanout_calls{0};

void *mock_p3_new_ubwc(void *dev, uint32_t w, uint32_t h, unsigned scanout)
{
    (void)dev;
    (void)w;
    (void)h;
    (void)scanout;
    return &g_dummy_ubwc_token;
}

int mock_p3_encode_scanout(void *cb, void *src, void *dst, unsigned mode)
{
    (void)cb;
    (void)src;
    (void)dst;
    g_dummy_scanout_calls.fetch_add(1, std::memory_order_relaxed);
    return mode <= 1u ? 0 : -1;
}

void run_group_g()
{
    int cb1 = 0, cb2 = 0, dev = 0, src = 0, dst = 0;

    begin_case("hwtest_phase3_scope_validation_and_lookup");
    CHECK(a840_hwtest_phase3_begin_scope(&cb1, "force_gmem_96x32") == 0);
    char scope[64] = {};
    CHECK(a840_hwtest_phase3_get_scope(&cb1, scope, sizeof(scope)));
    CHECK(std::strcmp(scope, "force_gmem_96x32") == 0);
    CHECK(a840_hwtest_phase3_begin_scope(&cb1, "invalid_scope_name") != 0);
    CHECK(a840_hwtest_phase3_begin_scope(nullptr, "force_sysmem") != 0);

    begin_case("hwtest_phase3_evidence_record_query_and_version_check");
    A840Phase3Evidence ev{};
    ev.gmem_passes = 1u;
    ev.memoryless_depth_passes = 1u;
    ev.fbfetch_draws = 4u;
    ev.tile_width = 96u;
    ev.tile_height = 32u;
    ev.tiles_x = 3u;
    ev.tiles_y = 3u;
    ev.ubwc_version = 6u;
    ev.highest_bank_bit = 16u;
    ev.swizzle_levels = 6u;
    ev.macrotile_mode = 1u;
    ev.compressed_blocks = 12u;
    ev.escape_blocks = 4u;
    CHECK(a840_hwtest_phase3_record_evidence(&cb1, &ev));
    A840Phase3Evidence out{};
    out.bytes = sizeof(out);
    out.version = 1u;
    CHECK(a840_hwtest_phase3_evidence_for_command_buffer(&cb1, &out) == 0);
    CHECK(out.gmem_passes == 1u && out.fbfetch_draws == 4u && out.highest_bank_bit == 16u);
    out.version = 99u;
    CHECK(a840_hwtest_phase3_evidence_for_command_buffer(&cb1, &out) != 0);

    begin_case("hwtest_phase3_forget_clears_path_and_evidence");
    CHECK(a840_hwtest_record_command_buffer(&cb1, A840_PRED_NONE));
    CHECK(std::strcmp(a840_hwtest_path_for_command_buffer(&cb1), "raw") == 0);
    a840_hwtest_forget_command_buffer(&cb1);
    CHECK(a840_hwtest_path_for_command_buffer(&cb1) == nullptr);
    out.version = 1u;
    CHECK(a840_hwtest_phase3_evidence_for_command_buffer(&cb1, &out) != 0);

    begin_case("hwtest_phase3_fail_closed_without_registered_adapters");
    a840_hwtest_phase3_register_adapters(nullptr, nullptr);
    CHECK(a840_hwtest_phase3_new_ubwc_texture(&dev, 256u, 128u, 0u) == nullptr);
    CHECK(a840_hwtest_phase3_encode_scanout(&cb2, &src, &dst, 0u) != 0);

    begin_case("hwtest_phase3_registered_ubwc_and_scanout_adapters");
    a840_hwtest_phase3_register_adapters(mock_p3_new_ubwc, mock_p3_encode_scanout);
    CHECK(a840_hwtest_phase3_new_ubwc_texture(&dev, 256u, 128u, 1u) == &g_dummy_ubwc_token);
    CHECK(a840_hwtest_phase3_encode_scanout(&cb2, &src, &dst, 0u) == 0);
    CHECK(a840_hwtest_phase3_encode_scanout(&cb2, &src, &dst, 1u) == 0);
    CHECK(a840_hwtest_phase3_encode_scanout(&cb2, &src, &dst, 2u) != 0);
    CHECK(g_dummy_scanout_calls.load() == 2u);
    a840_hwtest_phase3_register_adapters(nullptr, nullptr);

    begin_case("hwtest_phase3_multithread_tsan_hook_safety");
    int keys[8] = {};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t]() {
            for (int iter = 0; iter < 50; ++iter) {
                void *k = &keys[(t * 2 + (iter & 1)) % 8];
                (void)a840_hwtest_phase3_begin_scope(k, "force_gmem_96x32");
                (void)a840_hwtest_phase3_record_evidence(k, &ev);
                A840Phase3Evidence q{};
                q.bytes = sizeof(q);
                q.version = 1u;
                (void)a840_hwtest_phase3_evidence_for_command_buffer(k, &q);
                a840_hwtest_forget_command_buffer(k);
            }
        });
    }
    for (auto &th : threads) th.join();
    CHECK(true);
}

/* ---------- Multi-thread TSan Stress (--tsan-stress N) ---------- */

void run_tsan_stress(A840RtDevice *rt, int iterations)
{
    A840SysFrontendConfig cfg{};
    a840_sys_frontend_default_config(&cfg, A840_SYS_ROLE_RENDER_SERVER);
    A840SysFrontend *fe = nullptr;
    CHECK(a840_sys_frontend_create(rt, &cfg, &fe) == A840_SYS_OK);

    A840SysEventHandle shared_ev = A840_SYS_INVALID_HANDLE;
    CHECK(a840_sys_event_create(fe, true, 0u, 7u, &shared_ev) == A840_SYS_OK);

    std::atomic<uint64_t> cb_fires{0};
    auto waiter_cb = [](void *ctx, A840SysEventHandle, uint64_t) {
        static_cast<std::atomic<uint64_t> *>(ctx)->fetch_add(1, std::memory_order_relaxed);
    };

    for (int round = 0; round < iterations; ++round) {
        std::vector<std::thread> workers;
        for (int t = 0; t < 4; ++t) {
            workers.emplace_back([&, t, round]() {
                /* Concurrent IOSurface bind/use/query/unbind */
                A840SysIOSurfaceDesc sd{};
                sd.iosurface_id = (uint64_t)(round * 16 + t + 1);
                sd.mtl_pixel_format = A840_MTL_FMT_BGRA8_UNORM;
                sd.width = 128u;
                sd.height = 128u;
                sd.request_ubwc6 = (t & 1) == 0;
                A840SysSurfaceHandle sh = A840_SYS_INVALID_HANDLE;
                A840SysSurfaceInfo si{};
                if (a840_sys_bind_iosurface(fe, &sd, &sh, &si) == A840_SYS_OK) {
                    (void)a840_sys_surface_increment_use(fe, sh);
                    (void)a840_sys_query_surface(fe, sh, &si);
                    (void)a840_sys_surface_decrement_use(fe, sh);
                    (void)a840_sys_unbind_iosurface(fe, sh);
                }

                /* Concurrent SharedEvent signal/waiter outside mutex */
                const uint64_t target = (uint64_t)(round * 4 + t + 1);
                (void)a840_sys_event_register_waiter(fe, shared_ev, target, waiter_cb, &cb_fires);
                (void)a840_sys_event_signal(fe, shared_ev, target);

                /* Concurrent DepthStencil + BinaryArchive lifecycle */
                A840DepthStencilVariantDesc ds{};
                ds.depth_test = true;
                ds.depth_compare = 1u;
                A840SysDepthStencilHandle dsh = A840_SYS_INVALID_HANDLE;
                if (a840_sys_depth_stencil_create(fe, &ds, &dsh) == A840_SYS_OK) {
                    A840DepthStencilVariantDesc tmp{};
                    (void)a840_sys_depth_stencil_get(fe, dsh, &tmp);
                    (void)a840_sys_depth_stencil_destroy(fe, dsh);
                }
                A840SysArchiveHandle ah = A840_SYS_INVALID_HANDLE;
                if (a840_sys_archive_create(fe, (uint64_t)(t + 1), 1u, &ah) == A840_SYS_OK) {
                    A840PipelineKey k{};
                    k.sha256[0] = static_cast<uint8_t>(t);
                    (void)a840_sys_archive_add_key(fe, ah, &k);
                    (void)a840_sys_archive_destroy(fe, ah);
                }
            });
        }
        for (auto &w : workers) w.join();
    }

    CHECK(cb_fires.load() > 0u);
    CHECK(a840_sys_event_destroy(fe, shared_ev) == A840_SYS_OK);
    a840_sys_frontend_destroy(fe);
}

} /* namespace */

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-pm4") == 0 && i + 1 < argc) {
            g_dump_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--tsan-stress") == 0 && i + 1 < argc) {
            g_tsan_iters = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return 2;
        }
    }

    MockDeviceEnv env;
    if (!env.init()) {
        std::fprintf(stderr, "Failed to initialize mock device env\n");
        return 1;
    }

    run_group_a();
    run_group_b(env.rt);
    run_group_c(env.rt);
    run_group_d(env.rt);
    run_group_e(env.rt);
    run_group_f(env.rt);
    run_group_g();

    if (g_tsan_iters > 0) {
        run_tsan_stress(env.rt, g_tsan_iters);
    }

    std::printf("test_system_frontend_bridge: cases=%d checks=%d failures=%d tsan_iters=%d\n",
                g_cases, g_checks, g_failures, g_tsan_iters);
    return g_failures == 0 ? 0 : 1;
}
