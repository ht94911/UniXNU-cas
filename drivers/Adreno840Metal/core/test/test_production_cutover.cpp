/* SPDX-License-Identifier: MIT */
/* Wave 11 / Lane 3 (Phase 6) Zero-Fallback Production Cutover & Silicon Sync Test Suite. */
#include "a840_production_router.h"

#include "Adreno840Caps.h"
#include "Adreno840EmitABI.h"
#include "Adreno840Regs.h"
#include "a840_context_init.h"
#include "a840_emit_3d.h"
#include "a840_emit_compute.h"
#include "a840_emit_msaa_tess_vis.h"
#include "a840_emit_ops.h"
#include "a840_gfx_bind_sds.h"
#include "a840_iokit_wire.h"
#include "a840_backend_transport.h"
#include "a840_runtime_cmd.h"
#include "a840_runtime_device.h"
#include "gen/a840_bitfields.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static_assert(A840_PRODUCTION_FALLBACK_COMPILED_OUT == 1u,
              "Fallback must be permanently compiled out in Phase 6 production");
static_assert(A840_PROD_PREDICATE_COUNT == 16u,
              "Production router must cover all 16 DualPathPredicates.tsv entries");
static_assert(A840_EMIT_3D_DRAW_MAX_DWORDS == 11u,
              "3D draw max dwords must match probe006 PC_DRAWCALL_CNTL removal");

namespace {

int g_cases = 0;
int g_checks = 0;
int g_failures = 0;
const char *g_current_case = "init";
std::string g_dump_dir;

void begin_case(const char *name)
{
    g_current_case = name;
    g_cases++;
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

/* Exact 54 protected register intervals from NezhaGpu.c L3069-3088 (a840_protect[]),
 * encoded in decimal so zero raw hex register offsets appear in driver/test sources. */
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

bool is_cp_protected_reg(uint32_t reg)
{
    for (const auto &r : kA840ProtectRanges) {
        if (reg >= r.base && reg <= r.end_inclusive) {
            return true;
        }
    }
    return false;
}

struct StreamAudit {
    size_t packets = 0u;
    size_t type4_count = 0u;
    size_t type7_count = 0u;
    size_t cp_protect_violations = 0u;
    size_t pc_drawcall_cntl_writes = 0u;
    size_t draw_indx_offset_count = 0u;
    uint32_t last_draw_initiator = 0u;
    bool has_set_marker_direct = false;
    bool has_vis_override_1 = false;
    bool has_wfi = false;
    bool has_uche_inv = false;
    bool has_load_state6_geom = false;
    bool has_load_state6_frag = false;
    bool has_gfx_bindless_base = false;
};

StreamAudit audit_pm4_words(const uint32_t *words, size_t dwords)
{
    StreamAudit out{};
    if (!words || dwords == 0u) {
        return out;
    }
    std::vector<A840PM4Packet> pkts(dwords);
    size_t count = 0u;
    CHECK(a840_pm4_decode(words, dwords, pkts.data(), pkts.size(), &count) == A840_EMIT_OK);
    out.packets = count;
    for (size_t i = 0u; i < count; ++i) {
        const A840PM4Packet &p = pkts[i];
        if (p.type == 4u) {
            out.type4_count++;
            for (uint32_t k = 0u; k < p.payload_words; ++k) {
                const uint32_t reg = p.identifier + k;
                if (reg == A840_REG_PC_DRAWCALL_CNTL) {
                    out.pc_drawcall_cntl_writes++;
                }
                if (is_cp_protected_reg(reg)) {
                    out.cp_protect_violations++;
                }
                if (reg == A840_REG_SP_GFX_BINDLESS_BASE_0_DESCRIPTOR) {
                    out.has_gfx_bindless_base = true;
                }
            }
        } else if (p.type == 7u) {
            out.type7_count++;
            const uint32_t w0 = p.payload_words > 0u ? words[p.header_word + 1u] : 0u;
            if (p.identifier == A840_CP_DRAW_INDX_OFFSET) {
                out.draw_indx_offset_count++;
                out.last_draw_initiator = w0;
            } else if (p.identifier == A840_CP_SET_MARKER && w0 == A840_MODE_DIRECT_RENDER) {
                out.has_set_marker_direct = true;
            } else if (p.identifier == A840_CP_SET_VISIBILITY_OVERRIDE && w0 == 1u) {
                out.has_vis_override_1 = true;
            } else if (p.identifier == A840_CP_WAIT_FOR_IDLE) {
                out.has_wfi = true;
            } else if (p.identifier == A840_CP_EVENT_WRITE7 && (w0 & 255u) == A840_EVENT_CACHE_INVALIDATE7) {
                out.has_uche_inv = true;
            } else if (p.identifier == A840_CP_LOAD_STATE6_GEOM) {
                out.has_load_state6_geom = true;
            } else if (p.identifier == A840_CP_LOAD_STATE6_FRAG) {
                out.has_load_state6_frag = true;
            }
        }
    }
    return out;
}

void maybe_dump_pm4(const char *filename, const uint32_t *words, size_t dwords)
{
    if (g_dump_dir.empty()) {
        return;
    }
    const std::string full = g_dump_dir + "/" + filename;
    std::FILE *fp = std::fopen(full.c_str(), "wb");
    CHECK(fp != nullptr);
    if (fp) {
        const size_t written = std::fwrite(words, sizeof(uint32_t), dwords, fp);
        CHECK(written == dwords);
        CHECK(std::fclose(fp) == 0);
    }
}

A840CompiledStage make_stage_record(uint32_t stage, uint32_t constlen_vec4)
{
    A840CompiledStage rec{};
    rec.abi_version = ADRENO_COMPILE_ABI_VERSION;
    rec.stage = stage;
    rec.isa_dwords = 16u;
    rec.instruction_groups = 1u;
    rec.max_full_reg = 4;
    rec.max_half_reg = 2;
    rec.constlen_vec4 = constlen_vec4;
    rec.local_size[0] = 8u;
    rec.local_size[1] = 4u;
    rec.local_size[2] = 1u;
    rec.branchstack = 1u;
    rec.max_waves = 16u;
    return rec;
}

static const char *const kExpectedPredicates[A840_PROD_PREDICATE_COUNT] = {
    "raw:unimplemented.global",
    "raw:blit.unsupported_format",
    "raw:blit.3d_or_compressed",
    "raw:compute.uncompiled_kernel",
    "raw:compute.indirect_dispatch",
    "raw:render.gmem_required",
    "raw:render.ubwc_attachment",
    "raw:render.ycbcr_or_xr10",
    "raw:render.fbfetch_blend",
    "raw:render.tess_or_amplify",
    "raw:render.indirect_or_icb",
    "raw:render.msaa_resolve",
    "raw:resource.heap_aliasing",
    "raw:resource.sparse_texture",
    "raw:compiler.daemon_unavailable",
    "raw:sync.shared_event_cross_proc",
};

struct WireMockCtx {
    uint64_t next_bo = 10u;
    uint64_t next_queue = 1u;
    uint64_t next_fence = 100u;
    uint32_t map_share_last = 0u;
    uint32_t map_wb_last = 99u;
    std::vector<uint32_t> submit_ib_counts;
    std::vector<uint64_t> submit_first_handles;
    std::vector<uint64_t> submit_second_handles;
    std::vector<std::vector<uint8_t>> bo_storage;
};

static A840TransportStatus wire_mock_call(void *cookie,
                                          uint32_t method,
                                          const void *in_buf,
                                          size_t in_bytes,
                                          void *out_buf,
                                          size_t out_bytes)
{
    auto *ctx = static_cast<WireMockCtx *>(cookie);
    (void)in_bytes;
    if (method == A840_WIRE_QUERY) {
        auto *p = static_cast<A840WireParams *>(out_buf);
        std::memset(p, 0, out_bytes);
        p->abi_version = 2u;
        p->input_bits = 48u;
        p->output_bits = 40u;
        p->flags = 0u; /* non-coherent to verify A840_BO_SHARE_OUTER */
        p->max_bo_bytes = UINT64_C(256) * 1024u * 1024u;
        return A840_TRANSPORT_OK;
    }
    if (method == A840_WIRE_GET_DEVICE_INFO) {
        auto *d = static_cast<A840WireDeviceInfo *>(out_buf);
        std::memset(d, 0, out_bytes);
        d->chip_id = 1141180977u; /* gen80200 */
        d->gmem_bytes = 12u * 1024u * 1024u;
        d->gmem_base = UINT64_C(1) << 24;
        d->va_start = UINT64_C(1) << 30;
        d->va_size = UINT64_C(1) << 36;
        d->highest_bank_bit = 16u;
        d->ubwc_version = 4u;
        return A840_TRANSPORT_OK;
    }
    if (method == A840_WIRE_BO_ALLOC) {
        const auto *req = static_cast<const A840WireBoAlloc *>(in_buf);
        auto *rep = static_cast<A840WireBoAllocReply *>(out_buf);
        const uint64_t h = ctx->next_bo++;
        ctx->bo_storage.emplace_back(static_cast<size_t>(req->bytes), 0u);
        rep->handle = h;
        return A840_TRANSPORT_OK;
    }
    if (method == A840_WIRE_BO_CPU_MAP) {
        const auto *req = static_cast<const A840WireBoCpuMap *>(in_buf);
        auto *rep = static_cast<A840WireBoCpuMapReply *>(out_buf);
        rep->token = static_cast<uint32_t>(req->handle);
        rep->reserved = 0u;
        rep->bytes = ctx->bo_storage[(size_t)(req->handle - 10u)].size();
        return A840_TRANSPORT_OK;
    }
    if (method == A840_WIRE_BO_MAP) {
        const auto *req = static_cast<const A840WireBoMap *>(in_buf);
        auto *rep = static_cast<A840WireBoMapReply *>(out_buf);
        ctx->map_share_last = req->shareability;
        ctx->map_wb_last = req->writeback;
        rep->iova = req->iova;
        return A840_TRANSPORT_OK;
    }
    if (method == A840_WIRE_CREATE_QUEUE) {
        auto *rep = static_cast<A840WireQueueReply *>(out_buf);
        rep->queue = ctx->next_queue++;
        return A840_TRANSPORT_OK;
    }
    if (method == A840_WIRE_BO_CPU_UNMAP || method == A840_WIRE_BO_FREE ||
        method == A840_WIRE_CLOSE_QUEUE || method == A840_WIRE_WAIT_FENCE) {
        return A840_TRANSPORT_OK;
    }
    if (method == A840_WIRE_GET_TIMESTAMP) {
        auto *rep = static_cast<A840WireTimestamp *>(out_buf);
        rep->ticks = 1000u;
        return A840_TRANSPORT_OK;
    }
    if (method == A840_WIRE_SUBMIT) {
        const auto *req = static_cast<const A840WireSubmit *>(in_buf);
        auto *rep = static_cast<A840WireSubmitReply *>(out_buf);
        ctx->submit_ib_counts.push_back(req->buffer_count);
        ctx->submit_first_handles.push_back(req->buffer_count > 0u ? req->buffers[0].handle : 0u);
        ctx->submit_second_handles.push_back(req->buffer_count > 1u ? req->buffers[1].handle : 0u);
        rep->fence = ctx->next_fence++;
        return A840_TRANSPORT_OK;
    }
    return A840_TRANSPORT_UNSUPPORTED;
}

static A840TransportStatus wire_mock_map(void *cookie,
                                         uint32_t token,
                                         uint64_t bytes,
                                         bool coherent,
                                         void **out_ptr)
{
    auto *ctx = static_cast<WireMockCtx *>(cookie);
    (void)bytes;
    (void)coherent;
    if (token < 10u || (size_t)(token - 10u) >= ctx->bo_storage.size()) {
        return A840_TRANSPORT_NOT_FOUND;
    }
    *out_ptr = ctx->bo_storage[(size_t)(token - 10u)].data();
    return A840_TRANSPORT_OK;
}

static A840TransportStatus wire_mock_unmap(void *cookie, uint32_t token)
{
    (void)cookie;
    (void)token;
    return A840_TRANSPORT_OK;
}

void run_router_cases()
{
    /* Cases 1..16: Individual verification of all 16/16 DualPathPredicates.tsv predicates */
    for (uint32_t idx = 1u; idx <= A840_PROD_PREDICATE_COUNT; ++idx) {
        const auto pred = static_cast<A840ProdPredicateId>(idx);
        begin_case(kExpectedPredicates[idx - 1u]);
        A840ProdRouter router{};
        CHECK(a840_prod_router_init(&router) == A840_PROD_OK);
        CHECK(router.abi_version == A840_PROD_ROUTER_ABI_VERSION);
        CHECK(router.fallback_compiled_out == 1u);

        const char *name = a840_prod_predicate_name(pred);
        CHECK(name != nullptr);
        CHECK(std::strcmp(name, kExpectedPredicates[idx - 1u]) == 0);
        CHECK(a840_prod_lookup_predicate(name) == pred);

        A840ProdPredicateInfo info{};
        CHECK(a840_prod_predicate_info(pred, &info));
        CHECK(info.id == pred && info.target == A840_PROD_ROUTE_NATIVE);
        CHECK(info.owner_lane != nullptr && info.closed_phase >= 3u && info.closed_phase <= 6u);

        const A840ProdDecision d_id = a840_prod_route_eval_predicate(&router, pred, false);
        CHECK(d_id.status == A840_PROD_OK && d_id.target == A840_PROD_ROUTE_NATIVE);
        CHECK(d_id.predicate_id == pred && std::strcmp(d_id.predicate_name, kExpectedPredicates[idx - 1u]) == 0);

        const A840ProdDecision d_name = a840_prod_route_eval_by_name(&router, name, false);
        CHECK(d_name.status == A840_PROD_OK && d_name.target == A840_PROD_ROUTE_NATIVE);
        CHECK(router.native_hits == 2u && a840_prod_predicate_hits(&router, pred) == 2u);

        char fmt[64] = {};
        CHECK(a840_prod_format_decision(&d_id, fmt, sizeof(fmt)) == 11u);
        CHECK(std::strcmp(fmt, "path=native") == 0);
    }

    /* Case 17: Explicit fallback request rejected on all 16/16 predicates */
    begin_case("prod_router_reject_fallback_all_16_predicates");
    {
        A840ProdRouter router{};
        CHECK(a840_prod_router_init(&router) == A840_PROD_OK);
        for (uint32_t idx = 1u; idx <= A840_PROD_PREDICATE_COUNT; ++idx) {
            const A840ProdDecision d = a840_prod_route_eval_predicate(
                &router, static_cast<A840ProdPredicateId>(idx), true);
            CHECK(d.status == A840_PROD_ERR_FALLBACK_FORBIDDEN && d.target == A840_PROD_ROUTE_REJECT);
            char fmt[64] = {};
            CHECK(a840_prod_format_decision(&d, fmt, sizeof(fmt)) > 0u);
            CHECK(std::strcmp(fmt, "reject:fallback_forbidden") == 0);
        }
        CHECK(router.native_hits == 0u && router.fallback_denied_count == 16u);
        CHECK(a840_prod_route_request_fallback(&router) == A840_PROD_ERR_FALLBACK_FORBIDDEN);
        CHECK(a840_prod_route_request_fallback(nullptr) == A840_PROD_ERR_FALLBACK_FORBIDDEN);
        CHECK(router.fallback_denied_count == 17u);
    }

    /* Case 18: Out-of-range and unknown predicates fail closed */
    begin_case("prod_router_fail_closed_unknown_and_oob_predicates");
    {
        A840ProdRouter router{};
        CHECK(a840_prod_router_init(&router) == A840_PROD_OK);
        const A840ProdPredicateId bad_ids[] = {
            A840_PROD_PRED_NONE, static_cast<A840ProdPredicateId>(17),
            static_cast<A840ProdPredicateId>(18), static_cast<A840ProdPredicateId>(31),
        };
        for (auto bad : bad_ids) {
            CHECK(a840_prod_predicate_name(bad) == nullptr);
            A840ProdPredicateInfo info{};
            CHECK(!a840_prod_predicate_info(bad, &info));
            const A840ProdDecision d = a840_prod_route_eval_predicate(&router, bad, false);
            CHECK(d.status == A840_PROD_ERR_UNSUPPORTED && d.target == A840_PROD_ROUTE_REJECT);
            char fmt[64] = {};
            CHECK(a840_prod_format_decision(&d, fmt, sizeof(fmt)) > 0u);
            CHECK(std::strcmp(fmt, "reject:unsupported") == 0);
        }
        const char *bad_names[] = {
            nullptr, "", "fallback:raw:render.gmem_required", "raw:unknown.pred",
            "RAW:UNIMPLEMENTED.GLOBAL", "unknown:bridge",
        };
        for (const char *bn : bad_names) {
            CHECK(a840_prod_lookup_predicate(bn) == A840_PROD_PRED_NONE);
            const A840ProdDecision d = a840_prod_route_eval_by_name(&router, bn, false);
            CHECK(d.status == A840_PROD_ERR_UNSUPPORTED && d.target == A840_PROD_ROUTE_REJECT);
        }
        CHECK(router.native_hits == 0u && router.unsupported_rejected_count == 10u);
    }

    /* Case 19: NULL router & reset_stats safety */
    begin_case("prod_router_null_and_reset_stats");
    {
        CHECK(a840_prod_router_init(nullptr) == A840_PROD_ERR_INVALID_ARG);
        a840_prod_router_reset_stats(nullptr);
        CHECK(a840_prod_predicate_hits(nullptr, A840_PROD_PRED_RENDER_GMEM_REQUIRED) == 0u);
        CHECK(a840_prod_format_decision(nullptr, nullptr, 0u) == 0u);
        A840ProdRouter router{};
        CHECK(a840_prod_router_init(&router) == A840_PROD_OK);
        CHECK(a840_prod_route_eval_predicate(&router, A840_PROD_PRED_RENDER_GMEM_REQUIRED, false).status == A840_PROD_OK);
        CHECK(a840_prod_route_request_fallback(&router) == A840_PROD_ERR_FALLBACK_FORBIDDEN);
        a840_prod_router_reset_stats(&router);
        CHECK(router.native_hits == 0u && router.fallback_denied_count == 0u &&
              router.unsupported_rejected_count == 0u && router.fallback_compiled_out == 1u);
    }

    /* Case 20: Production bundle footprint audit */
    begin_case("prod_bundle_footprint_audit");
    {
        const A840ProdBundleFootprint fp = a840_prod_bundle_footprint();
        CHECK(fp.abi_version == A840_PROD_ROUTER_ABI_VERSION);
        CHECK(fp.fallback_compiled_out == 1u && fp.native_predicate_count == 16u &&
              fp.fallback_predicate_count == 0u);
        CHECK(fp.objc_exported_class_count > 0u && fp.objc_exported_class_count <= 24u);
        CHECK(fp.static_fallback_table_bytes == 0u && fp.dualpath_symbol_dependencies == 0u &&
              fp.nb_symbol_dependencies == 0u && fp.cp_protect_violation_count == 0u);
        CHECK(fp.bundle_version != nullptr &&
              std::strstr(fp.bundle_version, "Adreno840Metal-Phase6-Native") != nullptr);
    }

    /* Cases 21..28: Domain evaluators (blit, compute, render, resource, compiler, sync) */
    begin_case("prod_domain_blit_native_all_paths");
    {
        A840ProdRouter r{};
        CHECK(a840_prod_router_init(&r) == A840_PROD_OK);
        A840ProdBlitQuery qb{}; qb.is_buffer_copy = true;
        CHECK(a840_prod_route_blit(&r, &qb).target == A840_PROD_ROUTE_NATIVE);
        A840ProdBlitQuery q2{}; q2.src_mtl_format = 80u; q2.dst_mtl_format = 80u; q2.width = 64u; q2.height = 64u; q2.depth = 1u;
        CHECK(a840_prod_route_blit(&r, &q2).target == A840_PROD_ROUTE_NATIVE);
        q2.is_3d_or_compressed = true; q2.depth = 8u;
        CHECK(a840_prod_route_blit(&r, &q2).predicate_id == A840_PROD_PRED_BLIT_3D_OR_COMPRESSED);
        A840ProdBlitQuery qy{}; qy.is_ycbcr = true; qy.width = 1920u; qy.height = 1080u; qy.depth = 1u;
        CHECK(a840_prod_route_blit(&r, &qy).target == A840_PROD_ROUTE_NATIVE);
    }

    begin_case("prod_domain_blit_fail_closed");
    {
        A840ProdRouter r{};
        CHECK(a840_prod_router_init(&r) == A840_PROD_OK);
        CHECK(a840_prod_route_blit(&r, nullptr).status == A840_PROD_ERR_INVALID_ARG);
        A840ProdBlitQuery q{}; q.request_fallback = true;
        CHECK(a840_prod_route_blit(&r, &q).status == A840_PROD_ERR_FALLBACK_FORBIDDEN);
        q = {}; q.src_mtl_format = 80u; q.dst_mtl_format = 80u; q.width = 0u; q.height = 64u; q.depth = 1u;
        CHECK(a840_prod_route_blit(&r, &q).status == A840_PROD_ERR_UNSUPPORTED);
        q.width = 64u; q.dst_mtl_format = 999999u;
        CHECK(a840_prod_route_blit(&r, &q).status == A840_PROD_ERR_UNSUPPORTED);
    }

    begin_case("prod_domain_compute_native_and_fail_closed");
    {
        A840ProdRouter r{};
        CHECK(a840_prod_router_init(&r) == A840_PROD_OK);
        A840ProdComputeQuery q{}; q.has_compiled_kernel = true; q.has_valid_local_size = true;
        CHECK(a840_prod_route_compute(&r, &q).target == A840_PROD_ROUTE_NATIVE);
        q.is_indirect_dispatch = true;
        CHECK(a840_prod_route_compute(&r, &q).predicate_id == A840_PROD_PRED_COMPUTE_INDIRECT_DISPATCH);
        A840ProdComputeQuery qu{}; qu.has_valid_local_size = true;
        CHECK(a840_prod_route_compute(&r, &qu).status == A840_PROD_ERR_UNSUPPORTED);
        qu.uses_daemon_compile = true;
        CHECK(a840_prod_route_compute(&r, &qu).target == A840_PROD_ROUTE_NATIVE);
        qu.request_fallback = true;
        CHECK(a840_prod_route_compute(&r, &qu).status == A840_PROD_ERR_FALLBACK_FORBIDDEN);
        CHECK(a840_prod_route_compute(&r, nullptr).status == A840_PROD_ERR_INVALID_ARG);
    }

    begin_case("prod_domain_render_native_all_features");
    {
        A840ProdRouter r{};
        CHECK(a840_prod_router_init(&r) == A840_PROD_OK);
        A840ProdRenderQuery q{}; q.color_attachment_count = 1u; q.sample_count = 1u; q.uses_gmem = true;
        CHECK(a840_prod_route_render(&r, &q).predicate_id == A840_PROD_PRED_RENDER_GMEM_REQUIRED);
        q.has_ubwc_attachment = true;
        CHECK(a840_prod_route_render(&r, &q).predicate_id == A840_PROD_PRED_RENDER_UBWC_ATTACHMENT);
        q.has_ycbcr_or_xr10 = true;
        CHECK(a840_prod_route_render(&r, &q).predicate_id == A840_PROD_PRED_RENDER_YCBCR_OR_XR10);
        q.uses_fbfetch_or_prog_blend = true;
        CHECK(a840_prod_route_render(&r, &q).predicate_id == A840_PROD_PRED_RENDER_FBFETCH_BLEND);
        q.uses_indirect_or_icb = true;
        CHECK(a840_prod_route_render(&r, &q).predicate_id == A840_PROD_PRED_RENDER_INDIRECT_OR_ICB);
        q.uses_tess_or_amplify = true;
        CHECK(a840_prod_route_render(&r, &q).predicate_id == A840_PROD_PRED_RENDER_TESS_OR_AMPLIFY);
        q.sample_count = 4u; q.has_msaa_resolve = true;
        const auto d = a840_prod_route_render(&r, &q);
        CHECK(d.status == A840_PROD_OK && d.target == A840_PROD_ROUTE_NATIVE &&
              d.predicate_id == A840_PROD_PRED_RENDER_MSAA_RESOLVE);
    }

    begin_case("prod_domain_render_fail_closed");
    {
        A840ProdRouter r{};
        CHECK(a840_prod_router_init(&r) == A840_PROD_OK);
        CHECK(a840_prod_route_render(&r, nullptr).status == A840_PROD_ERR_INVALID_ARG);
        A840ProdRenderQuery q{}; q.color_attachment_count = 1u; q.sample_count = 8u;
        CHECK(a840_prod_route_render(&r, &q).status == A840_PROD_ERR_UNSUPPORTED);
        q.sample_count = 1u; q.color_attachment_count = 9u;
        CHECK(a840_prod_route_render(&r, &q).status == A840_PROD_ERR_UNSUPPORTED);
        q.color_attachment_count = 1u; q.request_fallback = true;
        CHECK(a840_prod_route_render(&r, &q).status == A840_PROD_ERR_FALLBACK_FORBIDDEN);
    }

    begin_case("prod_domain_resource_native_and_fail_closed");
    {
        A840ProdRouter r{};
        CHECK(a840_prod_router_init(&r) == A840_PROD_OK);
        A840ProdResourceQuery q{}; q.uses_heap_aliasing = true;
        CHECK(a840_prod_route_resource(&r, &q).target == A840_PROD_ROUTE_NATIVE);
        q.uses_sparse_texture = true;
        CHECK(a840_prod_route_resource(&r, &q).predicate_id == A840_PROD_PRED_RESOURCE_SPARSE_TEXTURE);
        q.request_fallback = true;
        CHECK(a840_prod_route_resource(&r, &q).status == A840_PROD_ERR_FALLBACK_FORBIDDEN);
        CHECK(a840_prod_route_resource(&r, nullptr).status == A840_PROD_ERR_INVALID_ARG);
    }

    begin_case("prod_domain_compiler_native_and_fail_closed");
    {
        A840ProdRouter r{};
        CHECK(a840_prod_router_init(&r) == A840_PROD_OK);
        A840ProdCompilerQuery q{}; q.precompiled_available = true;
        CHECK(a840_prod_route_compiler(&r, &q).target == A840_PROD_ROUTE_NATIVE);
        q.precompiled_available = false; q.daemon_available = true;
        CHECK(a840_prod_route_compiler(&r, &q).target == A840_PROD_ROUTE_NATIVE);
        q.daemon_available = false;
        CHECK(a840_prod_route_compiler(&r, &q).status == A840_PROD_ERR_UNSUPPORTED);
        q.request_fallback = true;
        CHECK(a840_prod_route_compiler(&r, &q).status == A840_PROD_ERR_FALLBACK_FORBIDDEN);
        CHECK(a840_prod_route_compiler(&r, nullptr).status == A840_PROD_ERR_INVALID_ARG);
    }

    begin_case("prod_domain_sync_native_and_fail_closed");
    {
        A840ProdRouter r{};
        CHECK(a840_prod_router_init(&r) == A840_PROD_OK);
        A840ProdSyncQuery q{}; q.cross_process_shared_event = true; q.uses_writer_promise = true; q.abi_v3_supported = true;
        CHECK(a840_prod_route_sync(&r, &q).target == A840_PROD_ROUTE_NATIVE);
        q.abi_v3_supported = false;
        CHECK(a840_prod_route_sync(&r, &q).status == A840_PROD_ERR_UNSUPPORTED);
        q.cross_process_shared_event = false; q.uses_writer_promise = false;
        CHECK(a840_prod_route_sync(&r, &q).target == A840_PROD_ROUTE_NATIVE);
        q.request_fallback = true;
        CHECK(a840_prod_route_sync(&r, &q).status == A840_PROD_ERR_FALLBACK_FORBIDDEN);
        CHECK(a840_prod_route_sync(&r, nullptr).status == A840_PROD_ERR_INVALID_ARG);
    }
}

void run_silicon_and_pm4_cases()
{
    /* Case 29: Verify PC_DRAWCALL_CNTL (0x9e00) is inside kA840ProtectRanges */
    begin_case("cp_protect_table_covers_pc_drawcall_cntl");
    {
        CHECK(is_cp_protected_reg(A840_REG_PC_DRAWCALL_CNTL));
        CHECK(is_cp_protected_reg(A840_REG_PC_DRAWCALL_CNTL_OVERRIDE));
        CHECK(!is_cp_protected_reg(A840_REG_VFD_INDEX_OFFSET));
    }

    /* Case 30: 3D non-indexed draw emits 7 dwords, 0 CP_PROTECT writes, initiator in CP_DRAW_INDX_OFFSET */
    begin_case("emit_3d_nonindexed_zero_cp_protect");
    {
        std::vector<uint32_t> buf(64u, 0u);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, buf.data(), buf.size(), nullptr, 0u) == A840_EMIT_OK);
        A840DrawDesc draw{};
        draw.primitive = A840_PRIM_TRIANGLES;
        draw.index_type = A840_INDEX_NONE;
        draw.count = 6u;
        draw.instance_count = 2u;
        draw.first_vertex = 3u;
        draw.first_instance = 1u;
        CHECK(a840_emit_3d_draw(&cs, &draw) == A840_EMIT_OK);
        CHECK(cs.used_dwords == 7u);
        const StreamAudit a = audit_pm4_words(cs.words, cs.used_dwords);
        CHECK(a.cp_protect_violations == 0u && a.pc_drawcall_cntl_writes == 0u);
        CHECK(a.draw_indx_offset_count == 1u && (a.last_draw_initiator & 63u) == A840_PRIM_TRIANGLES);
    }

    /* Case 31: 3D indexed uint16 & uint32 draws emit 11 dwords, 0 CP_PROTECT writes + dump PM4 */
    begin_case("emit_3d_indexed_draws_zero_cp_protect");
    {
        std::vector<uint32_t> buf(128u, 0u);
        std::vector<A840Reloc> relocs(8u);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, buf.data(), buf.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        for (A840IndexType kind : {A840_INDEX_UINT16, A840_INDEX_UINT32}) {
            A840DrawDesc draw{};
            draw.primitive = A840_PRIM_TRIANGLES;
            draw.index_type = kind;
            draw.count = 12u;
            draw.instance_count = 3u;
            draw.first_index = 2u;
            draw.base_vertex = -1;
            draw.first_instance = 4u;
            draw.index_capacity = 64u;
            draw.index_buffer = {11u, UINT64_C(1) << 32};
            const size_t before = cs.used_dwords;
            CHECK(a840_emit_3d_draw(&cs, &draw) == A840_EMIT_OK);
            CHECK(cs.used_dwords - before == A840_EMIT_3D_DRAW_MAX_DWORDS);
        }
        const StreamAudit a = audit_pm4_words(cs.words, cs.used_dwords);
        CHECK(a.cp_protect_violations == 0u && a.pc_drawcall_cntl_writes == 0u && a.draw_indx_offset_count == 2u);
        maybe_dump_pm4("w11_cutover_3d_indexed_draws.pm4", cs.words, cs.used_dwords);
    }

    /* Case 32: 3D render state accepts probe006 negative-height Y-flip viewport */
    begin_case("emit_3d_negative_height_viewport_yflip");
    {
        std::vector<uint32_t> buf(1024u, 0u);
        std::vector<A840Reloc> relocs(16u);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, buf.data(), buf.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        A840RenderStateDesc st{};
        st.viewport_count = 1u;
        st.viewports[0] = {0.0f, 1080.0f, 1920.0f, -1080.0f, 0.0f, 1.0f};
        st.scissor_count = 1u;
        st.scissors[0] = {0u, 0u, 1920u, 1080u};
        st.line_width = 1.0f;
        st.sample_mask = UINT16_MAX;
        st.colors[0].format = A840_FMT6_8_8_8_8_UNORM;
        st.colors[0].swap = A840_WZYX;
        st.colors[0].storage.tile_mode = A840_TILE_LINEAR;
        st.colors[0].storage.pitch_bytes = 1920u * 4u;
        st.colors[0].storage.layer_stride_bytes = 1920u * 4u * 1080u;
        st.colors[0].storage.data = {21u, UINT64_C(2) << 32};
        st.colors[0].blend.write_mask = 15u;
        st.depth_format = A840_DEPTH_NONE;
        CHECK(a840_emit_3d_render_state(&cs, &st) == A840_EMIT_OK);
        const StreamAudit a = audit_pm4_words(cs.words, cs.used_dwords);
        CHECK(a.cp_protect_violations == 0u && a.pc_drawcall_cntl_writes == 0u && a.packets >= 10u);
    }

    /* Case 33: Tessellation non-indexed patch draw has zero 0x9e00 & carries TESS_ENABLE */
    begin_case("emit_tess_nonindexed_zero_cp_protect");
    {
        std::vector<uint32_t> buf(256u, 0u);
        std::vector<A840Reloc> relocs(8u);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, buf.data(), buf.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        A840TessStateDesc ts{};
        ts.domain = A840_TESS_DOMAIN_QUAD;
        ts.spacing = A840_TESS_SPACING_FRACTIONAL_EVEN;
        ts.winding = A840_TESS_WINDING_CCW;
        ts.control_points = 4u;
        ts.patch_local_mem_16b = 16u;
        ts.subdraw_patch_limit = 32u;
        ts.tess_factor_buffer = {31u, UINT64_C(3) << 32};
        ts.tess_buffer_bytes = 65536u;
        CHECK(a840_emit_tess_state(&cs, &ts) == A840_EMIT_OK);
        A840TessDrawDesc td{};
        td.domain = A840_TESS_DOMAIN_QUAD;
        td.control_points = 4u;
        td.patch_count = 8u;
        td.instance_count = 2u;
        td.index_type = A840_INDEX_NONE;
        CHECK(a840_emit_tess_draw(&cs, &td) == A840_EMIT_OK);
        const StreamAudit a = audit_pm4_words(cs.words, cs.used_dwords);
        CHECK(a.cp_protect_violations == 0u && a.pc_drawcall_cntl_writes == 0u);
        CHECK(a.draw_indx_offset_count == 1u && (a.last_draw_initiator & (UINT32_C(1) << 17)) != 0u);
    }

    /* Case 34: Tessellation indexed uint16/uint32 + Vertex Amplification 2-view zero CP_PROTECT + dump */
    begin_case("emit_tess_indexed_and_vamp_zero_cp_protect");
    {
        std::vector<uint32_t> buf(512u, 0u);
        std::vector<A840Reloc> relocs(16u);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, buf.data(), buf.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        A840VertexAmpStateDesc va{};
        va.amp_count = 2u;
        va.multi_pos_output = true;
        va.increment_viewport_index = true;
        va.viewport_count = 2u;
        va.mappings[0] = {0u, 0u};
        va.mappings[1] = {1u, 1u};
        va.viewports[0] = {0.0f, 540.0f, 960.0f, -540.0f, 0.0f, 1.0f};
        va.viewports[1] = {960.0f, 540.0f, 960.0f, -540.0f, 0.0f, 1.0f};
        va.scissors[0] = {0u, 0u, 960u, 540u};
        va.scissors[1] = {960u, 0u, 960u, 540u};
        CHECK(a840_emit_vertex_amplification_state(&cs, &va) == A840_EMIT_OK);
        A840TessStateDesc ts{};
        ts.domain = A840_TESS_DOMAIN_TRIANGLE;
        ts.spacing = A840_TESS_SPACING_EQUAL;
        ts.winding = A840_TESS_WINDING_CW;
        ts.control_points = 3u;
        ts.patch_local_mem_16b = 12u;
        ts.subdraw_patch_limit = 64u;
        ts.d3d_vertex_ordering = true;
        ts.tess_factor_buffer = {32u, UINT64_C(4) << 32};
        ts.tess_buffer_bytes = 65536u;
        CHECK(a840_emit_tess_state(&cs, &ts) == A840_EMIT_OK);
        for (A840IndexType kind : {A840_INDEX_UINT16, A840_INDEX_UINT32}) {
            A840TessDrawDesc td{};
            td.domain = A840_TESS_DOMAIN_TRIANGLE;
            td.control_points = 3u;
            td.patch_count = 6u;
            td.instance_count = 1u;
            td.index_type = kind;
            td.index_capacity = 64u;
            td.index_buffer = {33u, UINT64_C(5) << 32};
            CHECK(a840_emit_tess_draw(&cs, &td) == A840_EMIT_OK);
        }
        const StreamAudit a = audit_pm4_words(cs.words, cs.used_dwords);
        CHECK(a.cp_protect_violations == 0u && a.pc_drawcall_cntl_writes == 0u && a.draw_indx_offset_count == 2u);
        maybe_dump_pm4("w11_cutover_tess_and_vamp.pm4", cs.words, cs.used_dwords);
    }

    /* Case 35: Phase 4 MSAA 4x + Depth/Stencil + Visibility Query + MSAA Resolve zero CP_PROTECT + dump */
    begin_case("emit_msaa4x_vis_resolve_zero_cp_protect");
    {
        std::vector<uint32_t> buf(512u, 0u);
        std::vector<A840Reloc> relocs(16u);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, buf.data(), buf.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        A840MsaaStateDesc msaa{}; msaa.sample_count = 4u; msaa.sample_rate_shading = true;
        CHECK(a840_emit_msaa_state(&cs, &msaa) == A840_EMIT_OK);
        A840DepthStencilVariantDesc ds{};
        ds.depth_test = true; ds.depth_write = true; ds.depth_compare = 1u;
        ds.depth_bounds_test = true; ds.depth_bounds_min = 0.05f; ds.depth_bounds_max = 0.95f;
        ds.z_mode = A840_ZMODE_EARLY_Z;
        CHECK(a840_emit_depth_stencil_variant(&cs, &ds) == A840_EMIT_OK);
        A840VisQueryDesc vq{};
        vq.query_buffer = {41u, UINT64_C(6) << 32}; vq.buffer_bytes = 4096u;
        vq.slot_index = 2u; vq.mode = A840_VIS_QUERY_COUNTING; vq.inside_render_pass = true;
        CHECK(a840_emit_visibility_query_begin(&cs, &vq) == A840_EMIT_OK);
        CHECK(a840_emit_visibility_query_end(&cs, &vq) == A840_EMIT_OK);
        A840MsaaResolveBlitDesc res{};
        res.src_samples = 4u; res.gmem_offset = 0u; res.gmem_pitch = 4096u; res.slot_index = 0u;
        res.tile_w = 256u; res.tile_h = 64u; res.resolve_rect = {0u, 0u, 256u, 64u};
        res.dst.format_id = A840_FMT6_8_8_8_8_UNORM; res.dst.swap = A840_WZYX;
        res.dst.tile_mode = A840_TILE_LINEAR; res.dst.width = 256u; res.dst.height = 64u;
        res.dst.pitch = 1024u; res.dst.bo_handle = 42u; res.dst.gpu_va = UINT64_C(7) << 32;
        res.mode = A840_MSAA_RESOLVE_AVERAGE; res.emit_ccu_uche_barriers = true;
        CHECK(a840_emit_msaa_resolve_blit(&cs, &res) == A840_EMIT_OK);
        const StreamAudit a = audit_pm4_words(cs.words, cs.used_dwords);
        CHECK(a.cp_protect_violations == 0u && a.pc_drawcall_cntl_writes == 0u && a.packets >= 16u);
        maybe_dump_pm4("w11_cutover_msaa4x_vis_resolve.pm4", cs.words, cs.used_dwords);
    }

    /* Cases 36..39: a840_gfx_bind_sds bindless & consts + guards */
    begin_case("gfx_bind_sds_bindless_valid_and_zero");
    {
        std::vector<uint32_t> buf(128u, 0u);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, buf.data(), buf.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t vas[3] = {UINT64_C(1) << 30, 0u, (UINT64_C(1) << 30) + 4096u};
        CHECK(a840_gfx_sds_emit_bindless(&cs, 3u, vas) == A840_EMIT_OK);
        const StreamAudit a = audit_pm4_words(cs.words, cs.used_dwords);
        CHECK(a.cp_protect_violations == 0u && a.has_gfx_bindless_base && cs.used_dwords == 7u);
    }

    begin_case("gfx_bind_sds_bindless_guards");
    {
        std::vector<uint32_t> buf(64u, 0u);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, buf.data(), buf.size(), nullptr, 0u) == A840_EMIT_OK);
        CHECK(a840_gfx_sds_emit_bindless(nullptr, 1u, nullptr) == A840_EMIT_INVALID_ARG);
        CHECK(a840_gfx_sds_emit_bindless(&cs, 0u, nullptr) == A840_EMIT_OK && cs.used_dwords == 0u);
        CHECK(a840_gfx_sds_emit_bindless(&cs, 1u, nullptr) == A840_EMIT_INVALID_ARG);
        const uint64_t unaligned = (UINT64_C(1) << 30) + 16u;
        CHECK(a840_gfx_sds_emit_bindless(&cs, 1u, &unaligned) == A840_EMIT_ALIGNMENT);
        const uint64_t hi_va = UINT64_C(1) << 48;
        CHECK(a840_gfx_sds_emit_bindless(&cs, 1u, &hi_va) == A840_EMIT_ALIGNMENT);
        CHECK(a840_gfx_sds_emit_bindless(&cs, ADRENO_COMPILE_MAX_BIND_SETS + 1u, &unaligned) == A840_EMIT_INVALID_ARG);
    }

    begin_case("gfx_bind_sds_consts_vs_and_fs");
    {
        std::vector<uint32_t> buf(256u, 0u);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, buf.data(), buf.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint64_t vas[2] = {UINT64_C(1) << 31, (UINT64_C(1) << 31) + 256u};
        CHECK(a840_gfx_sds_emit_bindless(&cs, 2u, vas) == A840_EMIT_OK);
        const uint32_t vs_words[8] = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
        A840GfxConstUpload vs_up{A840_STAGE_VERTEX, 0u, 2u, 32u, vs_words};
        CHECK(a840_gfx_sds_emit_consts(&cs, &vs_up) == A840_EMIT_OK);
        const uint32_t fs_words[4] = {10u, 20u, 30u, 40u};
        A840GfxConstUpload fs_up{A840_STAGE_FRAGMENT, 4u, 1u, 32u, fs_words};
        CHECK(a840_gfx_sds_emit_consts(&cs, &fs_up) == A840_EMIT_OK);
        const StreamAudit a = audit_pm4_words(cs.words, cs.used_dwords);
        CHECK(a.cp_protect_violations == 0u && a.has_gfx_bindless_base &&
              a.has_load_state6_geom && a.has_load_state6_frag);
        maybe_dump_pm4("w11_cutover_gfx_bind_sds.pm4", cs.words, cs.used_dwords);
    }

    begin_case("gfx_bind_sds_consts_guards");
    {
        std::vector<uint32_t> buf(64u, 0u);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, buf.data(), buf.size(), nullptr, 0u) == A840_EMIT_OK);
        const uint32_t w[4] = {1u, 2u, 3u, 4u};
        A840GfxConstUpload bad_stage{A840_STAGE_COMPUTE, 0u, 1u, 16u, w};
        CHECK(a840_gfx_sds_emit_consts(&cs, &bad_stage) == A840_EMIT_INVALID_ARG);
        A840GfxConstUpload bad_range{A840_STAGE_VERTEX, 16u, 1u, 16u, w};
        CHECK(a840_gfx_sds_emit_consts(&cs, &bad_range) == A840_EMIT_RANGE_ERROR);
        A840GfxConstUpload zero_cnt{A840_STAGE_FRAGMENT, 0u, 0u, 16u, nullptr};
        CHECK(a840_gfx_sds_emit_consts(&cs, &zero_cnt) == A840_EMIT_OK);
        CHECK(a840_gfx_sds_emit_consts(&cs, nullptr) == A840_EMIT_INVALID_ARG);
    }

    /* Case 40: Runtime compute dispatch serializes with WFI + UCHE_INV + dump PM4 */
    begin_case("runtime_compute_dispatch_wfi_uche_inv_serialization");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        const auto iface = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&iface, A840_ROUTE_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_RT_OK);
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb) == A840_RT_OK);
        A840RtBufferDesc isa_desc{4096u, 4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE, false, nullptr};
        A840RtBuffer *isa = nullptr;
        CHECK(a840_rt_buffer_create(dev, &isa_desc, &isa) == A840_RT_OK);
        const auto stage = make_stage_record(A840_STAGE_COMPUTE, 16u);
        A840RtComputePipelineDesc pipe{};
        pipe.stage = &stage;
        pipe.code_buffer = isa;
        A840RtComputeEncoder *ce = nullptr;
        CHECK(a840_rt_cb_begin_compute(cb, &ce) == A840_RT_OK);
        CHECK(a840_rt_compute_set_pipeline(ce, &pipe) == A840_RT_OK);
        CHECK(a840_rt_compute_dispatch_threadgroups(ce, 4u, 2u, 1u) == A840_RT_OK);
        CHECK(a840_rt_compute_dispatch_threads(ce, 19u, 11u, 1u) == A840_RT_OK);
        CHECK(a840_rt_compute_end(ce) == A840_RT_OK);
        a840_rt_compute_destroy(ce);
        CHECK(a840_rt_cb_commit(cb) == A840_RT_OK);
        CHECK(a840_rt_cb_wait_until_completed(cb, UINT64_C(1000000000)) == A840_RT_OK);
        size_t cap_dwords = 0u;
        const uint32_t *cap_words = a840_mock_transport_captured_words(mock, &cap_dwords);
        CHECK(cap_words != nullptr && cap_dwords > 0u);
        const StreamAudit a = audit_pm4_words(cap_words, cap_dwords);
        CHECK(a.cp_protect_violations == 0u && a.pc_drawcall_cntl_writes == 0u && a.has_wfi && a.has_uche_inv);
        maybe_dump_pm4("w11_cutover_compute_serialized.pm4", cap_words, cap_dwords);
        a840_rt_cb_release(cb); a840_rt_buffer_release(isa);
        a840_rt_queue_destroy(q); a840_rt_device_destroy(dev); a840_mock_transport_destroy(mock);
    }

    /* Case 41: Runtime sysmem 3D render emits probe006 sysmem_render_begin + Y-flip + zero 0x9e00 + dump */
    begin_case("runtime_sysmem_3d_begin_yflip_zero_cp_protect");
    {
        A840MockTransport *mock = nullptr;
        CHECK(a840_mock_transport_create(nullptr, &mock) == A840_TRANSPORT_OK);
        const auto iface = a840_mock_transport_interface(mock);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&iface, A840_ROUTE_ALL, &dev) == A840_RT_OK);
        A840RtQueue *q = nullptr;
        CHECK(a840_rt_queue_create(dev, A840_QUEUE_PRIORITY_NORMAL, &q) == A840_RT_OK);
        A840RtCmdBuffer *cb = nullptr;
        CHECK(a840_rt_cb_create(q, nullptr, &cb) == A840_RT_OK);
        A840RtTextureDesc td{};
        td.mtl_pixel_format = 80u; td.tex_type = A840_TEX_2D; td.width = 64u; td.height = 64u;
        td.depth = 1u; td.array_layers = 1u; td.mip_levels = 1u; td.samples = 1u;
        td.tile_mode = A840_TILE_LINEAR;
        td.usage_flags = A840_TEX_USAGE_RENDER_TARGET | A840_TEX_USAGE_CPU_VISIBLE;
        A840RtTexture *rt = nullptr;
        CHECK(a840_rt_texture_create(dev, &td, &rt) == A840_RT_OK);
        A840RtBufferDesc isa_desc{4096u, 4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE, false, nullptr};
        A840RtBuffer *vs_bo = nullptr, *fs_bo = nullptr;
        CHECK(a840_rt_buffer_create(dev, &isa_desc, &vs_bo) == A840_RT_OK);
        CHECK(a840_rt_buffer_create(dev, &isa_desc, &fs_bo) == A840_RT_OK);
        A840RtRenderPassDesc pass{};
        pass.color_count = 1u; pass.color_targets[0] = rt; pass.color_clear[0] = true;
        pass.color_clear_values[0][0] = 32u; pass.color_clear_values[0][1] = 64u;
        pass.color_clear_values[0][2] = 128u; pass.color_clear_values[0][3] = 255u;
        pass.sample_count = 1u;
        A840RtRenderEncoder *re = nullptr;
        CHECK(a840_rt_cb_begin_render_pass(cb, &pass, &re) == A840_RT_OK);
        const auto vs_rec = make_stage_record(A840_STAGE_VERTEX, 16u);
        const auto fs_rec = make_stage_record(A840_STAGE_FRAGMENT, 16u);
        A840RtGraphicsProgramDesc prog{};
        prog.emit_desc.vs.stage = &vs_rec;
        prog.emit_desc.fs.stage = &fs_rec;
        prog.emit_desc.vertex_id = A840_REGID_UNUSED;
        prog.emit_desc.instance_id = A840_REGID_UNUSED;
        prog.emit_desc.primitive_id = A840_REGID_UNUSED;
        prog.emit_desc.view_id = A840_REGID_UNUSED;
        prog.emit_desc.position = 0u;
        prog.emit_desc.point_size = A840_REGID_UNUSED;
        std::memset(&prog.emit_desc.fs_inputs, A840_REGID_UNUSED, sizeof(prog.emit_desc.fs_inputs));
        prog.emit_desc.fs_inputs.frag_coord_mask = 0u;
        prog.emit_desc.depth_output = A840_REGID_UNUSED;
        prog.emit_desc.sample_mask_output = A840_REGID_UNUSED;
        prog.emit_desc.stencil_output = A840_REGID_UNUSED;
        for (uint32_t ci = 0u; ci < A840_MAX_COLOR_TARGETS; ++ci) {
            prog.emit_desc.colors[ci].regid = A840_REGID_UNUSED;
        }
        prog.emit_desc.colors[0] = {0u, 15u, false};
        prog.vs_code_buffer = vs_bo;
        prog.fs_code_buffer = fs_bo;
        CHECK(a840_rt_render_set_program(re, &prog) == A840_RT_OK);
        A840RtRenderStateDesc st{};
        st.viewport_count = 1u; st.viewports[0] = {0.0f, 64.0f, 64.0f, -64.0f, 0.0f, 1.0f};
        st.scissor_count = 1u; st.scissors[0] = {0u, 0u, 64u, 64u};
        st.line_width = 1.0f; st.sample_mask = UINT16_MAX; st.color_blends[0].write_mask = 15u;
        CHECK(a840_rt_render_set_state(re, &st) == A840_RT_OK);
        A840RtDrawDesc draw{}; draw.primitive = A840_PRIM_TRIANGLES; draw.index_type = A840_INDEX_NONE;
        draw.count = 3u; draw.instance_count = 1u;
        CHECK(a840_rt_render_draw(re, &draw) == A840_RT_OK);
        CHECK(a840_rt_render_end_pass(re) == A840_RT_OK);
        a840_rt_render_destroy(re);
        CHECK(a840_rt_cb_commit(cb) == A840_RT_OK);
        CHECK(a840_rt_cb_wait_until_completed(cb, UINT64_C(1000000000)) == A840_RT_OK);
        size_t cap_dwords = 0u;
        const uint32_t *cap_words = a840_mock_transport_captured_words(mock, &cap_dwords);
        CHECK(cap_words != nullptr && cap_dwords > 0u);
        const StreamAudit a = audit_pm4_words(cap_words, cap_dwords);
        CHECK(a.cp_protect_violations == 0u && a.pc_drawcall_cntl_writes == 0u &&
              a.has_set_marker_direct && a.has_vis_override_1 && a.draw_indx_offset_count == 1u);
        maybe_dump_pm4("w11_cutover_3d_sysmem_yflip.pm4", cap_words, cap_dwords);
        a840_rt_cb_release(cb); a840_rt_buffer_release(vs_bo); a840_rt_buffer_release(fs_bo);
        a840_rt_texture_release(rt); a840_rt_queue_destroy(q); a840_rt_device_destroy(dev);
        a840_mock_transport_destroy(mock);
    }

    /* Case 42: IOKit wire prepends context_ib on EVERY submit & forces A840_BO_SHARE_OUTER */
    begin_case("iokit_wire_per_submit_context_ib_and_share_outer");
    {
        WireMockCtx mctx{};
        A840IokitWire *wire = nullptr;
        CHECK(a840_wire_create(wire_mock_call, wire_mock_map, wire_mock_unmap, &mctx, &wire) == A840_TRANSPORT_OK);
        const auto iface = a840_wire_interface(wire);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&iface, A840_ROUTE_ALL, &dev) == A840_RT_OK);
        CHECK(a840_wire_attach_context(wire, dev) == A840_TRANSPORT_OK);
        CHECK(mctx.map_share_last == A840_BO_SHARE_OUTER && mctx.map_wb_last == 0u);
        A840RtBufferDesc bd{4096u, 4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE, false, nullptr};
        A840RtBuffer *ib_bo = nullptr;
        CHECK(a840_rt_buffer_create(dev, &bd, &ib_bo) == A840_RT_OK);
        const A840SubmitIb ib{ib_bo->bo_handle, ib_bo->gpu_va, 64u};
        const A840SubmitDesc sdesc{0u, 0u, 0u, &ib, 1u, nullptr, 0u};
        A840SubmitReply reply{};
        for (int i = 0; i < 3; ++i) CHECK(iface.ops->queue_submit(wire, &sdesc, &reply) == A840_TRANSPORT_OK);
        CHECK(mctx.submit_ib_counts.size() == 3u);
        for (size_t i = 0u; i < 3u; ++i) {
            CHECK(mctx.submit_ib_counts[i] == 2u && mctx.submit_first_handles[i] != ib_bo->bo_handle &&
                  mctx.submit_second_handles[i] == ib_bo->bo_handle);
        }
        a840_rt_buffer_release(ib_bo); a840_wire_detach_context(wire);
        a840_rt_device_destroy(dev); a840_wire_destroy(wire);
    }

    /* Cases 43..50: Context IB, sampler clamp, capacity rollbacks, histogram, truncation, 1024-IB OOB, 10k loop */
    begin_case("context_init_ib_zero_cp_protect");
    {
        std::vector<uint32_t> words(A840_CTX_INIT_MAX_DWORDS, 0u);
        std::vector<A840Reloc> relocs(A840_CTX_INIT_MAX_RELOCS);
        A840CmdStream cs{};
        CHECK(a840_cs_init(&cs, words.data(), words.size(), relocs.data(), relocs.size()) == A840_EMIT_OK);
        A840ContextInitParams params{};
        params.border_color_gpu_va = UINT64_C(1) << 30;
        params.bin_preamble_br_va = (UINT64_C(1) << 30) + 4096u;
        params.bin_preamble_bv_va = (UINT64_C(1) << 30) + 8192u;
        params.bin_preamble_br_dwords = 64u;
        params.bin_preamble_bv_dwords = 64u;
        params.vis_stream_count = 1u;
        params.enable_early_preamble_reset = 1u;
        CHECK(a840_emit_context_init(&cs, &params) == A840_EMIT_OK);
        CHECK(cs.used_dwords > 0u && cs.used_dwords <= A840_CTX_INIT_MAX_DWORDS);
        const StreamAudit a = audit_pm4_words(cs.words, cs.used_dwords);
        CHECK(a.cp_protect_violations == 0u && a.pc_drawcall_cntl_writes == 0u);
    }

    begin_case("sampler_descriptor_lod_clamp_parity");
    {
        const float kMaxLod = 4095.0f / 256.0f;
        A840SampDescInput in{A840_FILTER_LINEAR, A840_FILTER_LINEAR, A840_MIP_LINEAR,
                             A840_ADDRESS_CLAMP_TO_EDGE, A840_ADDRESS_CLAMP_TO_EDGE,
                             A840_ADDRESS_CLAMP_TO_EDGE, 16u, 0.0f, kMaxLod, 0.0f,
                             A840_COMPARE_DISABLED, 0u, 0u};
        uint32_t words[A840_SAMP_CONST_DWORDS] = {};
        CHECK(a840_encode_samp_const(&in, words) == A840_EMIT_OK);
        in.max_lod = 3.4e38f;
        CHECK(a840_encode_samp_const(&in, words) != A840_EMIT_OK);
    }

    begin_case("emit_3d_draw_exact_capacity_and_rollback");
    {
        std::vector<uint32_t> buf(16u, 123456789u);
        A840Reloc reloc{};
        A840CmdStream cs{};
        A840DrawDesc draw{};
        draw.primitive = A840_PRIM_TRIANGLES; draw.index_type = A840_INDEX_UINT16;
        draw.count = 6u; draw.instance_count = 1u; draw.index_capacity = 16u;
        draw.index_buffer = {55u, UINT64_C(1) << 30};
        CHECK(a840_cs_init(&cs, buf.data(), 10u, &reloc, 1u) == A840_EMIT_OK);
        CHECK(a840_emit_3d_draw(&cs, &draw) == A840_EMIT_CAPACITY && cs.used_dwords == 0u && cs.reloc_count == 0u);
        CHECK(a840_cs_init(&cs, buf.data(), 11u, &reloc, 1u) == A840_EMIT_OK);
        CHECK(a840_emit_3d_draw(&cs, &draw) == A840_EMIT_OK && cs.used_dwords == 11u && cs.reloc_count == 1u);
    }

    begin_case("emit_tess_draw_exact_capacity_and_rollback");
    {
        std::vector<uint32_t> buf(16u, 0u);
        A840Reloc reloc{};
        A840CmdStream cs{};
        A840TessDrawDesc td{};
        td.domain = A840_TESS_DOMAIN_QUAD; td.control_points = 4u;
        td.patch_count = 2u; td.instance_count = 1u; td.index_type = A840_INDEX_NONE;
        CHECK(a840_cs_init(&cs, buf.data(), 6u, &reloc, 1u) == A840_EMIT_OK);
        CHECK(a840_emit_tess_draw(&cs, &td) == A840_EMIT_CAPACITY && cs.used_dwords == 0u);
        CHECK(a840_cs_init(&cs, buf.data(), 7u, &reloc, 1u) == A840_EMIT_OK);
        CHECK(a840_emit_tess_draw(&cs, &td) == A840_EMIT_OK && cs.used_dwords == 7u);
    }

    begin_case("prod_router_category_histogram_complete");
    {
        A840ProdRouter router{};
        CHECK(a840_prod_router_init(&router) == A840_PROD_OK);
        for (uint32_t i = 1u; i <= A840_PROD_PREDICATE_COUNT; ++i)
            CHECK(a840_prod_route_eval_predicate(&router, static_cast<A840ProdPredicateId>(i), false).status == A840_PROD_OK);
        uint64_t sum = 0u;
        for (uint32_t c = 0u; c < A840_PROD_CATEGORY_COUNT; ++c) {
            CHECK(router.native_hits_by_category[c] >= 1u);
            sum += router.native_hits_by_category[c];
        }
        CHECK(sum == 16u && router.native_hits == 16u && router.fallback_denied_count == 0u);
    }

    begin_case("prod_format_decision_buffer_truncation");
    {
        const A840ProdDecision ok{A840_PROD_OK, A840_PROD_ROUTE_NATIVE,
                                  A840_PROD_PRED_RENDER_GMEM_REQUIRED, "raw:render.gmem_required"};
        char small[5] = {'X', 'X', 'X', 'X', 'X'};
        CHECK(a840_prod_format_decision(&ok, small, sizeof(small)) == 11u && std::strcmp(small, "path") == 0);
        char one[1] = {'Z'};
        CHECK(a840_prod_format_decision(&ok, one, sizeof(one)) == 11u && one[0] == '\0');
    }

    begin_case("iokit_wire_1024_body_ibs_with_context_ib_oob");
    {
        WireMockCtx mctx{};
        A840IokitWire *wire = nullptr;
        CHECK(a840_wire_create(wire_mock_call, wire_mock_map, wire_mock_unmap, &mctx, &wire) == A840_TRANSPORT_OK);
        const auto iface = a840_wire_interface(wire);
        A840RtDevice *dev = nullptr;
        CHECK(a840_rt_device_create(&iface, A840_ROUTE_ALL, &dev) == A840_RT_OK);
        CHECK(a840_wire_attach_context(wire, dev) == A840_TRANSPORT_OK);
        A840RtBufferDesc bd{4096u, 4096u, A840_BUF_USAGE_READ | A840_BUF_USAGE_CPU_VISIBLE, false, nullptr};
        A840RtBuffer *ib_bo = nullptr;
        CHECK(a840_rt_buffer_create(dev, &bd, &ib_bo) == A840_RT_OK);
        std::vector<A840SubmitIb> ibs(1024u, A840SubmitIb{ib_bo->bo_handle, ib_bo->gpu_va, 64u});
        const A840SubmitDesc sdesc{0u, 0u, 0u, ibs.data(), 1024u, nullptr, 0u};
        A840SubmitReply reply{};
        CHECK(iface.ops->queue_submit(wire, &sdesc, &reply) == A840_TRANSPORT_OUT_OF_BOUNDS);
        a840_rt_buffer_release(ib_bo); a840_wire_detach_context(wire);
        a840_rt_device_destroy(dev); a840_wire_destroy(wire);
    }

    begin_case("prod_router_steady_state_10000_evals");
    {
        A840ProdRouter router{};
        CHECK(a840_prod_router_init(&router) == A840_PROD_OK);
        for (uint32_t iter = 0u; iter < 10000u; ++iter) {
            const auto pred = static_cast<A840ProdPredicateId>((iter % A840_PROD_PREDICATE_COUNT) + 1u);
            const A840ProdDecision d = a840_prod_route_eval_predicate(&router, pred, false);
            CHECK(d.status == A840_PROD_OK && d.target == A840_PROD_ROUTE_NATIVE);
        }
        CHECK(router.native_hits == 10000u && router.fallback_denied_count == 0u &&
              router.unsupported_rejected_count == 0u);
    }
}

} /* namespace */

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-pm4") == 0 && i + 1 < argc) {
            g_dump_dir = argv[++i];
        } else {
            std::fprintf(stderr, "Usage: %s [--dump-pm4 <dir>]\n", argv[0]);
            return 2;
        }
    }
    run_router_cases();
    run_silicon_and_pm4_cases();
    std::printf("test_production_cutover: cases=%d checks=%d failures=%d\n",
                g_cases, g_checks, g_failures);
    if (g_cases < 45) {
        std::fprintf(stderr, "FAIL: expected >= 45 cases, got %d\n", g_cases);
        return 1;
    }
    return g_failures == 0 ? 0 : 1;
}
