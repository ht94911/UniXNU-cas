/* SPDX-License-Identifier: MIT */
#include "a840_kmd_display_handoff.h"
#include "a840_kmd_fault_capture.h"
#include "a840_kmd_ubwc_surface.h"
#include "a840_kmd_wait_any.h"
#include "a840_kmd_writer_promise.h"
#include "chip_facts_a840.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr auto kOk = A840_KMD_OK;
constexpr uint64_t kPage = A840_ABI_V3_PAGE_BYTES;
constexpr uint64_t kMs = UINT64_C(1000000);

[[noreturn]] void fail(const char *expr, int line)
{
    std::fprintf(stderr, "FAIL line %d: %s\n", line, expr);
    std::abort();
}

#define CHECK(expr) do { if (!(expr)) fail(#expr, __LINE__); } while (false)

struct Case {
    const char *name;
    void (*run)();
};

std::vector<Case> &cases()
{
    static std::vector<Case> all;
    return all;
}

struct Register {
    Register(const char *name, void (*fn)()) { cases().push_back({name, fn}); }
};

#define TEST(name)                               \
    static void name();                          \
    static Register reg_##name(#name, name);     \
    static void name()

struct ObserverLog {
    std::mutex mu;
    std::vector<A840KmdPromiseEvent> events;
    bool try_reenter_query = false;
    A840KmdWriterPromiseRegistry *reg = nullptr;
    uint32_t reenter_ok = 0;

    static void callback(void *cookie, const A840KmdPromiseEvent *ev)
    {
        auto *self = static_cast<ObserverLog *>(cookie);
        if (self->try_reenter_query && self->reg != nullptr) {
            A840KmdPromiseStats st{};
            if (a840_kmd_writer_promise_get_stats(self->reg, &st) == kOk) {
                std::lock_guard<std::mutex> g(self->mu);
                self->reenter_ok++;
            }
        }
        std::lock_guard<std::mutex> g(self->mu);
        self->events.push_back(*ev);
    }
};

struct CommitLog {
    std::mutex mu;
    std::vector<A840KmdMdssCommitDesc> commits;
    A840KmdStatus next_status = kOk;
    A840KmdDisplayHandoff *dh = nullptr;
    bool verify_unlocked = false;

    static A840KmdStatus callback(void *cookie, const A840KmdMdssCommitDesc *desc)
    {
        auto *self = static_cast<CommitLog *>(cookie);
        if (self->verify_unlocked && self->dh != nullptr) {
            A840KmdDisplayHandoffStats st{};
            CHECK(a840_kmd_display_handoff_get_stats(self->dh, &st) == kOk);
        }
        std::lock_guard<std::mutex> g(self->mu);
        if (self->next_status != kOk)
            return self->next_status;
        self->commits.push_back(*desc);
        return kOk;
    }
};

struct Env {
    std::mutex surf_mu;
    A840KmdWaitAnyEngine wait_any{};
    A840KmdFaultCapture fault_capture{};
    A840KmdSurfaceManager surface_mgr{};
    A840KmdWriterPromiseRegistry promise_reg{};
    A840KmdDisplayHandoff handoff{};
    ObserverLog obs{};
    CommitLog commits{};

    uint32_t panel_formats[3]{80u, 70u, 115u};
    A840KmdMdssCaps caps{A840_MDSS_UBWC6, 1200u, 2608u, 4096u, 64u, panel_formats, 3u};

    static constexpr uint64_t kRenderServer = 100u;
    static constexpr uint64_t kDisplayClient = 900u;
    static constexpr uint64_t kAppA = 201u;
    static constexpr uint64_t kAppB = 202u;

    Env()
    {
        CHECK(a840_kmd_wait_any_init(&wait_any, nullptr) == kOk);
        CHECK(a840_kmd_fault_init(&fault_capture) == kOk);

        A840KmdSurfaceOps sops{
            &surf_mu,
            [](void *p) { static_cast<std::mutex *>(p)->lock(); },
            [](void *p) { static_cast<std::mutex *>(p)->unlock(); }
        };
        CHECK(a840_kmd_surface_init(&surface_mgr, &sops) == kOk);

        obs.reg = &promise_reg;
        A840KmdPromiseConfig pcfg{};
        pcfg.wait_any = &wait_any;
        pcfg.fault_capture = &fault_capture;
        pcfg.observer = &ObserverLog::callback;
        pcfg.observer_cookie = &obs;
        CHECK(a840_kmd_writer_promise_init(&promise_reg, &pcfg) == kOk);

        CHECK(a840_kmd_fault_set_poison_callback(
                  &fault_capture, a840_kmd_writer_promise_on_fault_poison, &promise_reg) == kOk);

        commits.dh = &handoff;
        A840KmdDisplayHandoffConfig dcfg{};
        dcfg.surface_mgr = &surface_mgr;
        dcfg.promise_reg = &promise_reg;
        dcfg.commit_fn = &CommitLog::callback;
        dcfg.commit_cookie = &commits;
        CHECK(a840_kmd_display_handoff_init(&handoff, &dcfg) == kOk);

        CHECK(a840_kmd_writer_promise_register_client(
                  &promise_reg, kRenderServer, A840_CLIENT_ROLE_RENDER_SERVER) == kOk);
        CHECK(a840_kmd_writer_promise_register_client(
                  &promise_reg, kAppA, A840_CLIENT_ROLE_APP) == kOk);
        CHECK(a840_kmd_writer_promise_register_client(
                  &promise_reg, kAppB, A840_CLIENT_ROLE_APP) == kOk);
    }

    ~Env()
    {
        a840_kmd_display_handoff_destroy(&handoff);
        a840_kmd_fault_destroy(&fault_capture);
        a840_kmd_writer_promise_destroy(&promise_reg);
        a840_kmd_wait_any_destroy(&wait_any);
    }

    void add_ubwc_surface(uint64_t owner, uint64_t handle, uint64_t identity,
                          uint64_t display_handle = 0, uint32_t fmt = 80u,
                          uint32_t w = 1200u, uint32_t h = 2608u,
                          uint64_t display_base = kPage * 16u)
    {
        A840KmdSurfaceLayout layout{kPage * 32u, display_base, fmt, w, h, 163u};
        CHECK(a840_kmd_surface_create(&surface_mgr, owner, handle, identity, &layout) == kOk);
        NativeAdrenoAbiSurfaceMetaV3 meta{handle, 1u, 1u, 6u, 16u, 6u, 1u, kPage, 64u, 0u};
        A840KmdSurfaceResult res{};
        CHECK(a840_kmd_surface_meta(&surface_mgr, owner, &meta, 0u, nullptr, &res) == kOk);
        if (display_handle != 0u)
            CHECK(a840_kmd_surface_import(&surface_mgr, kDisplayClient, display_handle, identity) == kOk);
        CHECK(a840_kmd_writer_promise_bind_surface(&promise_reg, owner, handle, identity) == kOk);
    }

    void add_linear_surface(uint64_t owner, uint64_t handle, uint64_t identity,
                            uint64_t display_handle = 0, uint32_t fmt = 80u,
                            uint32_t w = 1200u, uint32_t h = 2608u)
    {
        A840KmdSurfaceLayout layout{kPage * 32u, kPage * 32u, fmt, w, h, 0u};
        CHECK(a840_kmd_surface_create(&surface_mgr, owner, handle, identity, &layout) == kOk);
        NativeAdrenoAbiSurfaceMetaV3 meta{handle, 1u, 0u, 6u, 16u, 6u, 1u, 0u, 0u, 0u};
        A840KmdSurfaceResult res{};
        CHECK(a840_kmd_surface_meta(&surface_mgr, owner, &meta, 0u, nullptr, &res) == kOk);
        if (display_handle != 0u)
            CHECK(a840_kmd_surface_import(&surface_mgr, kDisplayClient, display_handle, identity) == kOk);
        CHECK(a840_kmd_writer_promise_bind_surface(&promise_reg, owner, handle, identity) == kOk);
    }

    A840KmdWriterPromiseCall make_call(uint64_t client, uint64_t queue, uint64_t handle,
                                       uint64_t gen = 0, uint32_t flags = A840_PROMISE_FLAG_NONE)
    {
        A840KmdWriterPromiseCall c{};
        c.writer_client = client;
        c.generation = gen;
        c.flags = flags;
        c.abi.op = NativeAdrenoPromiseMake;
        c.abi.surface_count = 1u;
        c.abi.queue = queue;
        c.abi.surface_ids[0] = handle;
        return c;
    }

    A840KmdWriterPromiseCall fulfill_call(uint64_t client, uint64_t queue, uint64_t handle,
                                          uint64_t token, uint64_t fence,
                                          uint32_t flags = A840_PROMISE_FLAG_NONE)
    {
        A840KmdWriterPromiseCall c{};
        c.writer_client = client;
        c.flags = flags;
        c.abi.op = NativeAdrenoPromiseFulfill;
        c.abi.surface_count = 1u;
        c.abi.promise_token = token;
        c.abi.queue = queue;
        c.abi.fence = fence;
        c.abi.surface_ids[0] = handle;
        return c;
    }

    A840KmdWriterPromiseCall cancel_call(uint64_t client, uint64_t queue, uint64_t handle,
                                         uint64_t token)
    {
        A840KmdWriterPromiseCall c{};
        c.writer_client = client;
        c.abi.op = NativeAdrenoPromiseCancel;
        c.abi.surface_count = 1u;
        c.abi.promise_token = token;
        c.abi.queue = queue;
        c.abi.surface_ids[0] = handle;
        return c;
    }
};

/* ============================================================================
 * Group 1: Lifecycle, Init, ABI Guards & Fail-Closed Validation (Cases 1..18)
 * ============================================================================ */

TEST(abi_struct_sizes_locked) {
    CHECK(sizeof(NativeAdrenoAbiWriterPromiseV3) == 160u);
    CHECK(sizeof(NativeAdrenoAbiWriterPromiseReplyV3) == 16u);
    CHECK(sizeof(NativeAdrenoAbiSurfaceMetaV3) == 48u);
}

TEST(uninitialized_registry_rejected) {
    A840KmdWriterPromiseRegistry reg{};
    A840KmdPromiseStats st{};
    CHECK(a840_kmd_writer_promise_get_stats(&reg, &st) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_writer_promise_register_client(&reg, 1u, A840_CLIENT_ROLE_APP) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_writer_promise_pending_count(&reg, 0u) == 0u);
    a840_kmd_writer_promise_destroy(&reg);
}

TEST(init_null_and_bad_sync_ops) {
    CHECK(a840_kmd_writer_promise_init(nullptr, nullptr) == A840_KMD_INVALID_ARG);
    A840KmdWriterPromiseRegistry reg{};
    A840KmdPromiseSyncOps bad_ops{};
    A840KmdPromiseConfig cfg{};
    cfg.sync_ops = &bad_ops;
    CHECK(a840_kmd_writer_promise_init(&reg, &cfg) == A840_KMD_INVALID_ARG);
}

TEST(display_handoff_init_guards) {
    Env e;
    A840KmdDisplayHandoff dh{};
    A840KmdDisplayHandoffConfig cfg{};
    CHECK(a840_kmd_display_handoff_init(nullptr, &cfg) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_display_handoff_init(&dh, nullptr) == A840_KMD_INVALID_ARG);
    cfg.surface_mgr = &e.surface_mgr;
    CHECK(a840_kmd_display_handoff_init(&dh, &cfg) == A840_KMD_INVALID_ARG);
    cfg.promise_reg = &e.promise_reg;
    A840KmdDisplaySyncOps bad_sync{};
    cfg.sync_ops = &bad_sync;
    CHECK(a840_kmd_display_handoff_init(&dh, &cfg) == A840_KMD_INVALID_ARG);
    A840KmdDisplayHandoffStats stats{};
    CHECK(a840_kmd_display_handoff_get_stats(&dh, &stats) == A840_KMD_STATE_CONFLICT);
}

TEST(register_client_validation_and_role_escalation_blocked) {
    Env e;
    CHECK(a840_kmd_writer_promise_register_client(&e.promise_reg, 0u, A840_CLIENT_ROLE_APP) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_writer_promise_register_client(&e.promise_reg, 500u, static_cast<A840ClientRole>(99)) == A840_KMD_INVALID_ARG);
    /* Same client & same role is idempotent. */
    CHECK(a840_kmd_writer_promise_register_client(&e.promise_reg, Env::kAppA, A840_CLIENT_ROLE_APP) == kOk);
    /* Role escalation APP -> RENDER_SERVER MUST fail with STATE_CONFLICT. */
    CHECK(a840_kmd_writer_promise_register_client(&e.promise_reg, Env::kAppA, A840_CLIENT_ROLE_RENDER_SERVER) == A840_KMD_STATE_CONFLICT);
}

TEST(bind_surface_validation_and_conflicts) {
    Env e;
    CHECK(a840_kmd_writer_promise_bind_surface(&e.promise_reg, 0u, 1u, 1001u) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_writer_promise_bind_surface(&e.promise_reg, Env::kAppA, 0u, 1001u) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_writer_promise_bind_surface(&e.promise_reg, Env::kAppA, 1u, 0u) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_writer_promise_bind_surface(&e.promise_reg, 9999u, 1u, 1001u) == A840_KMD_NOT_FOUND);
    CHECK(a840_kmd_writer_promise_bind_surface(&e.promise_reg, Env::kAppA, 10u, 1010u) == kOk);
    /* Rebinding same handle to same identity is idempotent. */
    CHECK(a840_kmd_writer_promise_bind_surface(&e.promise_reg, Env::kAppA, 10u, 1010u) == kOk);
    /* Rebinding same handle to different identity rejected. */
    CHECK(a840_kmd_writer_promise_bind_surface(&e.promise_reg, Env::kAppA, 10u, 1011u) == A840_KMD_STATE_CONFLICT);
    /* Cross-client claiming another client's identity rejected. */
    CHECK(a840_kmd_writer_promise_bind_surface(&e.promise_reg, Env::kAppB, 20u, 1010u) == A840_KMD_PERMISSION_DENIED);
}

TEST(dispatch_reserved_nonzero_fail_closed) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    auto call = e.make_call(Env::kRenderServer, 1u, 11u);
    call.reserved = 1u;
    NativeAdrenoAbiWriterPromiseReplyV3 reply{0xdeadbeefULL, 99u, 77u};
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_INVALID_ARG);
    CHECK(reply.promise_token == 0xdeadbeefULL && reply.accepted == 99u && reply.reserved == 77u);
    call.reserved = 0u;
    call.reserved2 = 2u;
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_writer_promise_pending_count(&e.promise_reg, 0u) == 0u);
}

TEST(dispatch_unknown_flags_and_op_rejected) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    auto call = e.make_call(Env::kRenderServer, 1u, 11u);
    NativeAdrenoAbiWriterPromiseReplyV3 reply{};
    call.flags = 1u << 7;
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_INVALID_ARG);
    call.flags = 0u;
    call.abi.op = 0u;
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_INVALID_ARG);
    call.abi.op = 4u;
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_INVALID_ARG);
    call.abi.op = NativeAdrenoPromiseMake;
    call.has_surface_identities = 2u;
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_INVALID_ARG);
}

TEST(dispatch_surface_count_and_tail_garbage_rejected) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    auto call = e.make_call(Env::kRenderServer, 1u, 11u);
    NativeAdrenoAbiWriterPromiseReplyV3 reply{};
    call.abi.surface_count = 0u;
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_INVALID_ARG);
    call.abi.surface_count = 17u;
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_INVALID_ARG);
    call.abi.surface_count = 1u;
    call.abi.surface_ids[0] = 0u;
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_INVALID_ARG);
    call.abi.surface_ids[0] = 11u;
    call.abi.surface_ids[5] = 999u; /* non-zero garbage in unused tail */
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_INVALID_ARG);
}

TEST(dispatch_duplicate_surface_handles_rejected) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    auto call = e.make_call(Env::kRenderServer, 1u, 11u);
    call.abi.surface_count = 2u;
    call.abi.surface_ids[1] = 11u;
    NativeAdrenoAbiWriterPromiseReplyV3 reply{};
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_STATE_CONFLICT);
}

TEST(dispatch_make_nonzero_token_or_fence_rejected) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    auto call = e.make_call(Env::kRenderServer, 1u, 11u);
    NativeAdrenoAbiWriterPromiseReplyV3 reply{};
    call.abi.promise_token = 1u;
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_INVALID_ARG);
    call.abi.promise_token = 0u;
    call.abi.fence = 5u;
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_INVALID_ARG);
}

TEST(dispatch_unknown_handle_rejected) {
    Env e;
    auto call = e.make_call(Env::kRenderServer, 1u, 777u);
    NativeAdrenoAbiWriterPromiseReplyV3 reply{};
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_NOT_FOUND);
}

TEST(cross_client_handle_forgery_rejected) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 33u, 3001u);
    /* AppB tries to promise AppA's surface handle 33. */
    auto call = e.make_call(Env::kAppB, 2u, 33u);
    NativeAdrenoAbiWriterPromiseReplyV3 reply{};
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_PERMISSION_DENIED);
    CHECK(a840_kmd_writer_promise_pending_count(&e.promise_reg, 0u) == 0u);
}

TEST(cross_client_explicit_identity_forgery_rejected) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 33u, 3001u);
    e.add_ubwc_surface(Env::kAppB, 44u, 4001u);
    /* AppB passes its own handle 44, but tries to forge AppA's surface_identity 3001. */
    auto call = e.make_call(Env::kAppB, 2u, 44u);
    call.has_surface_identities = 1u;
    call.surface_identities[0] = 3001u;
    NativeAdrenoAbiWriterPromiseReplyV3 reply{};
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_PERMISSION_DENIED);
    /* Matching explicit identity succeeds. */
    call.surface_identities[0] = 4001u;
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == kOk);
}

TEST(cross_client_fulfill_and_cancel_blocked) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 33u, 3001u);
    e.add_ubwc_surface(Env::kAppB, 44u, 4001u);
    auto mk = e.make_call(Env::kAppA, 2u, 33u);
    NativeAdrenoAbiWriterPromiseReplyV3 reply{};
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &reply, nullptr) == kOk);

    /* AppB tries to fulfill AppA's token using AppB's handle. */
    auto bad_f = e.fulfill_call(Env::kAppB, 2u, 44u, reply.promise_token, 10u);
    NativeAdrenoAbiWriterPromiseReplyV3 r2{};
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &bad_f, &r2, nullptr) == A840_KMD_PERMISSION_DENIED);

    /* AppB tries to cancel AppA's token. */
    auto bad_c = e.cancel_call(Env::kAppB, 2u, 44u, reply.promise_token);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &bad_c, &r2, nullptr) == A840_KMD_PERMISSION_DENIED);

    /* Original promise stays PENDING untouched. */
    A840KmdPromiseEvent ev{};
    CHECK(a840_kmd_writer_promise_query_token(&e.promise_reg, reply.promise_token, &ev) == kOk);
    CHECK(ev.state == A840_PROMISE_STATE_PENDING);
}

TEST(unregistered_client_rejected) {
    Env e;
    auto call = e.make_call(7777u, 1u, 11u);
    NativeAdrenoAbiWriterPromiseReplyV3 reply{};
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &reply, nullptr) == A840_KMD_NOT_FOUND);
}

TEST(external_resolver_callback_works_and_prevents_forgery) {
    struct ResolverCtx {
        uint64_t expected_client = 555u;
        uint64_t expected_handle = 88u;
        uint64_t identity = 8800u;
    } rctx;

    A840KmdWriterPromiseRegistry reg{};
    A840KmdPromiseConfig cfg{};
    cfg.surface_resolver = [](void *cookie, uint64_t client, uint64_t handle, uint64_t *out_id) -> A840KmdStatus {
        auto *c = static_cast<ResolverCtx *>(cookie);
        if (handle != c->expected_handle)
            return A840_KMD_NOT_FOUND;
        if (client != c->expected_client)
            return A840_KMD_PERMISSION_DENIED;
        *out_id = c->identity;
        return A840_KMD_OK;
    };
    cfg.surface_resolver_cookie = &rctx;
    CHECK(a840_kmd_writer_promise_init(&reg, &cfg) == kOk);
    CHECK(a840_kmd_writer_promise_register_client(&reg, 555u, A840_CLIENT_ROLE_RENDER_SERVER) == kOk);
    CHECK(a840_kmd_writer_promise_register_client(&reg, 556u, A840_CLIENT_ROLE_APP) == kOk);

    A840KmdWriterPromiseCall call{};
    call.writer_client = 556u;
    call.abi.op = NativeAdrenoPromiseMake;
    call.abi.surface_count = 1u;
    call.abi.queue = 1u;
    call.abi.surface_ids[0] = 88u;
    NativeAdrenoAbiWriterPromiseReplyV3 rep{};
    CHECK(a840_kmd_writer_promise_dispatch(&reg, &call, &rep, nullptr) == A840_KMD_PERMISSION_DENIED);

    call.writer_client = 555u;
    CHECK(a840_kmd_writer_promise_dispatch(&reg, &call, &rep, nullptr) == kOk);
    A840KmdSurfacePromiseSnapshot snap{};
    CHECK(a840_kmd_writer_promise_query_surface(&reg, 8800u, 0u, &snap) == kOk);
    CHECK(snap.valid && snap.promise_token == rep.promise_token);
    a840_kmd_writer_promise_destroy(&reg);
}

TEST(unbind_surface_blocks_future_promises) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 15u, 1500u);
    a840_kmd_writer_promise_unbind_surface(&e.promise_reg, Env::kAppA, 15u);
    auto call = e.make_call(Env::kAppA, 1u, 15u);
    NativeAdrenoAbiWriterPromiseReplyV3 rep{};
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &call, &rep, nullptr) == A840_KMD_NOT_FOUND);
}

/* ============================================================================
 * Group 2: Privilege Split — RenderServer Early Scheduled vs App Deferred (Cases 19..30)
 * ============================================================================ */

TEST(renderserver_early_scheduled_handoff_before_fence_retires) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    A840PromiseHandoffPolicy pol = A840_PROMISE_POLICY_NONE;

    auto mk = e.make_call(Env::kRenderServer, 10u, 11u, 0u, A840_PROMISE_FLAG_EARLY_HANDOFF);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, &pol) == kOk);
    CHECK(r1.accepted == 1u && r1.reserved == 0u && r1.promise_token == 1u);
    CHECK(pol == A840_PROMISE_POLICY_EARLY_SCHEDULED);

    auto ff = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 42u, A840_PROMISE_FLAG_EARLY_HANDOFF);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, &pol) == kOk);
    CHECK(pol == A840_PROMISE_POLICY_EARLY_SCHEDULED);

    /* Surface is IMMEDIATELY display_ready with SCHEDULED_READY before fence 42 retires. */
    A840KmdSurfacePromiseSnapshot snap{};
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 1001u, 0u, &snap) == kOk);
    CHECK(snap.display_ready == 1u);
    CHECK(snap.state == A840_PROMISE_STATE_SCHEDULED_READY);
    CHECK(snap.fence_retired == 0u && snap.fence == 42u && snap.queue == 10u);

    /* Display handoff commits immediately with EARLY_GPU_FENCE + UBWC6_DIRECT. */
    A840KmdDisplayFrameRequest dreq{};
    dreq.consumer_client = Env::kDisplayClient;
    dreq.surface_handle = 511u;
    dreq.surface_identity = 1001u;
    dreq.mdss_caps = &e.caps;
    A840KmdDisplayHandoffResult dres{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &dreq, &dres) == kOk);
    CHECK(dres.outcome == A840_DISPLAY_HANDOFF_COMMITTED);
    CHECK(dres.scanout_mode == A840_DISPLAY_SCANOUT_UBWC6_DIRECT);
    CHECK(dres.sync_mode == A840_DISPLAY_SYNC_EARLY_GPU_FENCE);
    CHECK(dres.commit.gpu_fence == 42u && dres.commit.gpu_queue == 10u);
    CHECK(e.commits.commits.size() == 1u);

    A840KmdDisplayHandoffStats ds{};
    CHECK(a840_kmd_display_handoff_get_stats(&e.handoff, &ds) == kOk);
    CHECK(ds.early_scheduled_handoffs == 1u && ds.ubwc_direct_handoffs == 1u);
    CHECK(ds.deferred_app_handoffs == 0u);
}

TEST(renderserver_scheduled_then_fence_retires_transitions_to_retired_ready) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 50u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    /* Fence 49 does not retire 50. */
    CHECK(a840_kmd_writer_promise_signal_fence(&e.promise_reg, Env::kRenderServer, 10u, 49u, 1000u) == 0u);
    A840KmdSurfacePromiseSnapshot snap{};
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 1001u, 0u, &snap) == kOk);
    CHECK(snap.state == A840_PROMISE_STATE_SCHEDULED_READY && snap.fence_retired == 0u);

    /* Fence 50 retires it. */
    CHECK(a840_kmd_writer_promise_signal_fence(&e.promise_reg, Env::kRenderServer, 10u, 50u, 2000u) == 1u);
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 1001u, 0u, &snap) == kOk);
    CHECK(snap.state == A840_PROMISE_STATE_RETIRED_READY && snap.fence_retired == 1u);
    CHECK(a840_kmd_writer_promise_pending_count(&e.promise_reg, Env::kRenderServer) == 0u);
}

TEST(app_early_handoff_forced_to_defer_until_retired) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 21u, 2001u, 621u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    A840PromiseHandoffPolicy pol = A840_PROMISE_POLICY_NONE;

    /* App passes EARLY_HANDOFF without STRICT_REJECT -> coerced to DEFER_UNTIL_RETIRED. */
    auto mk = e.make_call(Env::kAppA, 20u, 21u, 0u, A840_PROMISE_FLAG_EARLY_HANDOFF);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, &pol) == kOk);
    CHECK(pol == A840_PROMISE_POLICY_DEFER_UNTIL_RETIRED);

    auto ff = e.fulfill_call(Env::kAppA, 20u, 21u, r1.promise_token, 77u, A840_PROMISE_FLAG_EARLY_HANDOFF);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, &pol) == kOk);
    CHECK(pol == A840_PROMISE_POLICY_DEFER_UNTIL_RETIRED);

    /* Surface MUST NOT be display_ready before fence 77 retires! */
    A840KmdSurfacePromiseSnapshot snap{};
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 2001u, 0u, &snap) == kOk);
    CHECK(snap.display_ready == 0u);
    CHECK(snap.state == A840_PROMISE_STATE_DEFERRED_WAITING_FENCE);
    CHECK(snap.fence_retired == 0u);

    /* Non-blocking display submit returns TIMED_OUT (not committed!). */
    A840KmdDisplayFrameRequest dreq{};
    dreq.consumer_client = Env::kDisplayClient;
    dreq.surface_handle = 621u;
    dreq.surface_identity = 2001u;
    dreq.mdss_caps = &e.caps;
    A840KmdDisplayHandoffResult dres{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &dreq, &dres) == kOk);
    CHECK(dres.outcome == A840_DISPLAY_HANDOFF_TIMED_OUT && dres.timed_out == 1u);
    CHECK(e.commits.commits.empty());

    /* Now retire fence 77 -> becomes RETIRED_READY and display commits. */
    CHECK(a840_kmd_writer_promise_signal_fence(&e.promise_reg, Env::kAppA, 20u, 77u, 5555u) == 1u);
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &dreq, &dres) == kOk);
    CHECK(dres.outcome == A840_DISPLAY_HANDOFF_COMMITTED);
    CHECK(dres.sync_mode == A840_DISPLAY_SYNC_RETIRED_READY);
    CHECK(dres.promise_snapshot.fence_retired == 1u);

    A840KmdDisplayHandoffStats ds{};
    CHECK(a840_kmd_display_handoff_get_stats(&e.handoff, &ds) == kOk);
    CHECK(ds.deferred_app_handoffs == 1u && ds.fence_satisfied_handoffs == 1u);
    CHECK(ds.early_scheduled_handoffs == 0u);
}

TEST(app_strict_early_handoff_explicitly_denied) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 21u, 2001u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{};
    uint32_t strict_flags = A840_PROMISE_FLAG_EARLY_HANDOFF | A840_PROMISE_FLAG_STRICT_REJECT_APP_EARLY;

    auto mk = e.make_call(Env::kAppA, 20u, 21u, 0u, strict_flags);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == A840_KMD_PERMISSION_DENIED);
    CHECK(a840_kmd_writer_promise_pending_count(&e.promise_reg, Env::kAppA) == 0u);

    /* Make normally, then try strict early handoff on Fulfill -> denied without mutating slot! */
    auto mk2 = e.make_call(Env::kAppA, 20u, 21u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk2, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kAppA, 20u, 21u, r1.promise_token, 10u, strict_flags);
    NativeAdrenoAbiWriterPromiseReplyV3 r2{};
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == A840_KMD_PERMISSION_DENIED);

    A840KmdPromiseEvent ev{};
    CHECK(a840_kmd_writer_promise_query_token(&e.promise_reg, r1.promise_token, &ev) == kOk);
    CHECK(ev.state == A840_PROMISE_STATE_PENDING && ev.fence == 0u);
}

TEST(app_already_retired_fence_satisfies_immediately_on_fulfill) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 21u, 2001u, 621u);
    /* Fence 100 already completed before Fulfill(90) arrives. */
    (void)a840_kmd_writer_promise_signal_fence(&e.promise_reg, Env::kAppA, 20u, 100u, 9000u);

    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kAppA, 20u, 21u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kAppA, 20u, 21u, r1.promise_token, 90u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    A840KmdSurfacePromiseSnapshot snap{};
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 2001u, 0u, &snap) == kOk);
    CHECK(snap.state == A840_PROMISE_STATE_RETIRED_READY && snap.display_ready == 1u && snap.fence_retired == 1u);
}

TEST(wait_surface_blocks_app_until_fence_retires) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 21u, 2001u, 621u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kAppA, 20u, 21u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kAppA, 20u, 21u, r1.promise_token, 12u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    std::atomic<bool> signaled{false};
    std::thread irq([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        signaled.store(true, std::memory_order_release);
        CHECK(a840_kmd_writer_promise_signal_fence(&e.promise_reg, Env::kAppA, 20u, 12u, 12345u) == 1u);
    });

    A840KmdDisplayFrameRequest dreq{};
    dreq.consumer_client = Env::kDisplayClient;
    dreq.surface_handle = 621u;
    dreq.surface_identity = 2001u;
    dreq.min_generation = 1u;
    dreq.timeout_ns = 2000u * kMs;
    dreq.flags = A840_DISPLAY_REQ_WAIT_FOR_PROMISE;
    dreq.mdss_caps = &e.caps;

    A840KmdDisplayHandoffResult dres{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &dreq, &dres) == kOk);
    CHECK(signaled.load(std::memory_order_acquire));
    CHECK(dres.outcome == A840_DISPLAY_HANDOFF_COMMITTED);
    CHECK(dres.sync_mode == A840_DISPLAY_SYNC_RETIRED_READY);
    irq.join();
}

TEST(wait_surface_wakes_immediately_on_renderserver_fulfill) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);

    std::thread rs([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        NativeAdrenoAbiWriterPromiseReplyV3 r2{};
        auto ff = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 88u);
        CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);
    });

    A840KmdSurfacePromiseSnapshot snap{};
    uint32_t timed_out = 1u;
    CHECK(a840_kmd_writer_promise_wait_surface(
              &e.promise_reg, 1001u, 1u, 2000u * kMs, &snap, &timed_out) == kOk);
    CHECK(timed_out == 0u);
    CHECK(snap.state == A840_PROMISE_STATE_SCHEDULED_READY && snap.fence == 88u);
    rs.join();
}

TEST(wait_surface_timeout_returns_timed_out) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 21u, 2001u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kAppA, 20u, 21u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kAppA, 20u, 21u, r1.promise_token, 99u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    A840KmdSurfacePromiseSnapshot snap{};
    uint32_t timed_out = 0u;
    CHECK(a840_kmd_writer_promise_wait_surface(
              &e.promise_reg, 2001u, 1u, 5u * kMs, &snap, &timed_out) == kOk);
    CHECK(timed_out == 1u);
    CHECK(snap.state == A840_PROMISE_STATE_DEFERRED_WAITING_FENCE);
}

TEST(wait_surface_zero_timeout_poll_behavior) {
    Env e;
    A840KmdSurfacePromiseSnapshot snap{};
    uint32_t timed_out = 0u;
    CHECK(a840_kmd_writer_promise_wait_surface(&e.promise_reg, 9999u, 1u, 0u, &snap, &timed_out) == kOk);
    CHECK(timed_out == 1u && snap.valid == 0u);
}

TEST(renderserver_and_app_multi_surface_atomic_promise) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    e.add_ubwc_surface(Env::kRenderServer, 12u, 1002u);
    e.add_ubwc_surface(Env::kRenderServer, 13u, 1003u);

    A840KmdWriterPromiseCall mk{};
    mk.writer_client = Env::kRenderServer;
    mk.abi.op = NativeAdrenoPromiseMake;
    mk.abi.surface_count = 3u;
    mk.abi.queue = 10u;
    mk.abi.surface_ids[0] = 11u;
    mk.abi.surface_ids[1] = 12u;
    mk.abi.surface_ids[2] = 13u;
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);

    A840KmdWriterPromiseCall ff = mk;
    ff.abi.op = NativeAdrenoPromiseFulfill;
    ff.abi.promise_token = r1.promise_token;
    ff.abi.fence = 30u;
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    for (uint64_t sid : {1001ULL, 1002ULL, 1003ULL}) {
        A840KmdSurfacePromiseSnapshot snap{};
        CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, sid, 0u, &snap) == kOk);
        CHECK(snap.state == A840_PROMISE_STATE_SCHEDULED_READY && snap.fence == 30u);
    }
}

TEST(fulfill_mismatched_surface_or_queue_rejected_atomically) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    e.add_ubwc_surface(Env::kRenderServer, 12u, 1002u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);

    /* Wrong surface handle */
    auto ff_bad_surf = e.fulfill_call(Env::kRenderServer, 10u, 12u, r1.promise_token, 5u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff_bad_surf, &r2, nullptr) == A840_KMD_STATE_CONFLICT);

    /* Wrong queue */
    auto ff_bad_q = e.fulfill_call(Env::kRenderServer, 99u, 11u, r1.promise_token, 5u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff_bad_q, &r2, nullptr) == A840_KMD_STATE_CONFLICT);

    /* Zero fence */
    auto ff_zero_f = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 0u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff_zero_f, &r2, nullptr) == A840_KMD_INVALID_ARG);

    /* Valid fulfill still succeeds. */
    auto ff_ok = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 5u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff_ok, &r2, nullptr) == kOk);

    /* Double fulfill rejected. */
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff_ok, &r2, nullptr) == A840_KMD_STATE_CONFLICT);
}

TEST(observer_callback_runs_strictly_outside_mutex) {
    Env e;
    e.obs.try_reenter_query = true;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 10u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);
    CHECK(a840_kmd_writer_promise_signal_fence(&e.promise_reg, Env::kRenderServer, 10u, 10u, 1u) == 1u);
    /* Reentering get_stats inside observer would deadlock if lock were held. */
    CHECK(e.obs.reenter_ok >= 3u);
}

/* ============================================================================
 * Group 3: Monotonic Generation, Superseding & Frame Pipeline (Cases 31..40)
 * ============================================================================ */

TEST(generation_auto_monotonic_increment) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    for (uint64_t g = 1u; g <= 5u; ++g) {
        NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
        auto mk = e.make_call(Env::kRenderServer, 10u, 11u);
        CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
        auto ff = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, g * 10u);
        CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);
        A840KmdSurfacePromiseSnapshot snap{};
        CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 1001u, 0u, &snap) == kOk);
        CHECK(snap.generation == g);
    }
}

TEST(generation_explicit_strictly_increasing_enforced) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{};
    auto mk10 = e.make_call(Env::kRenderServer, 10u, 11u, 10u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk10, &r1, nullptr) == kOk);

    /* Same generation rejected without mutating existing head. */
    auto mk_dup = e.make_call(Env::kRenderServer, 10u, 11u, 10u);
    NativeAdrenoAbiWriterPromiseReplyV3 r_bad{};
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk_dup, &r_bad, nullptr) == A840_KMD_STATE_CONFLICT);

    /* Rollback generation rejected. */
    auto mk_back = e.make_call(Env::kRenderServer, 10u, 11u, 9u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk_back, &r_bad, nullptr) == A840_KMD_STATE_CONFLICT);

    /* Head still points to token from gen 10. */
    A840KmdSurfacePromiseSnapshot snap{};
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 1001u, 0u, &snap) == kOk);
    CHECK(snap.generation == 10u && snap.promise_token == r1.promise_token);

    /* Strictly greater generation 15 succeeds. */
    auto mk15 = e.make_call(Env::kRenderServer, 10u, 11u, 15u);
    NativeAdrenoAbiWriterPromiseReplyV3 r15{};
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk15, &r15, nullptr) == kOk);
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 1001u, 0u, &snap) == kOk);
    CHECK(snap.generation == 15u && snap.promise_token == r15.promise_token);
}

TEST(newer_frame_supersedes_older_inflight_frame_and_invalidates_old_token) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{}, r3{};

    auto mk1 = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk1, &r1, nullptr) == kOk);

    /* Second frame arrives on same surface before frame 1 fulfills. */
    auto mk2 = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk2, &r2, nullptr) == kOk);

    /* Old token is now CANCELED/SUPERSEDED and cannot be fulfilled. */
    A840KmdPromiseEvent ev1{};
    CHECK(a840_kmd_writer_promise_query_token(&e.promise_reg, r1.promise_token, &ev1) == kOk);
    CHECK(ev1.state == A840_PROMISE_STATE_CANCELED);
    CHECK(ev1.cancel_reason == A840_PROMISE_CANCEL_SUPERSEDED);

    auto ff_old = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 10u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff_old, &r3, nullptr) == A840_KMD_STATE_CONFLICT);

    /* New token fulfills cleanly and owns surface head. */
    auto ff_new = e.fulfill_call(Env::kRenderServer, 10u, 11u, r2.promise_token, 20u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff_new, &r3, nullptr) == kOk);

    A840KmdSurfacePromiseSnapshot snap{};
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 1001u, 0u, &snap) == kOk);
    CHECK(snap.generation == 2u && snap.promise_token == r2.promise_token && snap.fence == 20u);

    A840KmdPromiseStats ps{};
    CHECK(a840_kmd_writer_promise_get_stats(&e.promise_reg, &ps) == kOk);
    CHECK(ps.total_superseded == 1u);
}

TEST(superseding_scheduled_frame_does_not_leave_stale_fence) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r1f{}, r2{}, r2f{};

    auto mk1 = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk1, &r1, nullptr) == kOk);
    auto ff1 = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 10u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff1, &r1f, nullptr) == kOk);

    auto mk2 = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk2, &r2, nullptr) == kOk);
    /* Surface head is now PENDING gen=2 until gen=2 fulfills. */
    A840KmdSurfacePromiseSnapshot snap{};
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 1001u, 0u, &snap) == kOk);
    CHECK(snap.generation == 2u && snap.state == A840_PROMISE_STATE_PENDING && snap.fence == 0u);

    /* Retiring old fence 10 must NOT flip gen=2 to RETIRED_READY! */
    (void)a840_kmd_writer_promise_signal_fence(&e.promise_reg, Env::kRenderServer, 10u, 10u, 100u);
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 1001u, 0u, &snap) == kOk);
    CHECK(snap.generation == 2u && snap.state == A840_PROMISE_STATE_PENDING);

    auto ff2 = e.fulfill_call(Env::kRenderServer, 10u, 11u, r2.promise_token, 20u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff2, &r2f, nullptr) == kOk);
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 1001u, 0u, &snap) == kOk);
    CHECK(snap.generation == 2u && snap.state == A840_PROMISE_STATE_SCHEDULED_READY && snap.fence == 20u);
}

TEST(explicit_cancel_wakes_waiter_and_frees_pending) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);

    std::thread canceler([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        NativeAdrenoAbiWriterPromiseReplyV3 rc{};
        auto cc = e.cancel_call(Env::kRenderServer, 10u, 11u, r1.promise_token);
        CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &cc, &rc, nullptr) == kOk);
    });

    A840KmdSurfacePromiseSnapshot snap{};
    uint32_t timed_out = 1u;
    CHECK(a840_kmd_writer_promise_wait_surface(&e.promise_reg, 1001u, 1u, 2000u * kMs, &snap, &timed_out) == kOk);
    CHECK(timed_out == 0u);
    CHECK(snap.state == A840_PROMISE_STATE_CANCELED);
    CHECK(snap.cancel_reason == A840_PROMISE_CANCEL_EXPLICIT);
    CHECK(a840_kmd_writer_promise_pending_count(&e.promise_reg, Env::kRenderServer) == 0u);
    canceler.join();
}

TEST(cancel_with_nonzero_fence_or_already_retired_rejected) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{}, rc{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);

    auto bad_cc = e.cancel_call(Env::kRenderServer, 10u, 11u, r1.promise_token);
    bad_cc.abi.fence = 1u;
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &bad_cc, &rc, nullptr) == A840_KMD_INVALID_ARG);

    auto ff = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 5u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);
    CHECK(a840_kmd_writer_promise_signal_fence(&e.promise_reg, Env::kRenderServer, 10u, 5u, 10u) == 1u);

    auto late_cc = e.cancel_call(Env::kRenderServer, 10u, 11u, r1.promise_token);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &late_cc, &rc, nullptr) == A840_KMD_STATE_CONFLICT);
}

TEST(slot_recycling_allows_exceeding_slot_capacity_over_time) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    uint64_t prev_token = 0u;
    for (uint32_t i = 1u; i <= A840_KMD_PROMISE_MAX_SLOTS + 40u; ++i) {
        NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
        auto mk = e.make_call(Env::kRenderServer, 10u, 11u);
        CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
        CHECK(r1.promise_token > prev_token);
        prev_token = r1.promise_token;
        auto ff = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, i);
        CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);
        CHECK(a840_kmd_writer_promise_signal_fence(&e.promise_reg, Env::kRenderServer, 10u, i, i * 10u) == 1u);
    }
    A840KmdSurfacePromiseSnapshot snap{};
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 1001u, 0u, &snap) == kOk);
    CHECK(snap.generation == A840_KMD_PROMISE_MAX_SLOTS + 40u);
}

TEST(slot_exhaustion_when_all_pending_returns_capacity_fail_closed) {
    Env e;
    for (uint64_t i = 1u; i <= A840_KMD_PROMISE_MAX_SLOTS + 1u; ++i)
        CHECK(a840_kmd_writer_promise_bind_surface(&e.promise_reg, Env::kAppA, i, 5000u + i) == kOk);
    for (uint64_t i = 1u; i <= A840_KMD_PROMISE_MAX_SLOTS; ++i) {
        NativeAdrenoAbiWriterPromiseReplyV3 r{};
        auto mk = e.make_call(Env::kAppA, 1u, i);
        CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r, nullptr) == kOk);
    }
    NativeAdrenoAbiWriterPromiseReplyV3 r_over{0x1234ULL, 5u, 6u};
    auto mk_over = e.make_call(Env::kAppA, 1u, A840_KMD_PROMISE_MAX_SLOTS + 1u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk_over, &r_over, nullptr) == A840_KMD_CAPACITY);
    CHECK(r_over.promise_token == 0x1234ULL && r_over.accepted == 5u);
    CHECK(a840_kmd_writer_promise_pending_count(&e.promise_reg, Env::kAppA) == A840_KMD_PROMISE_MAX_SLOTS);
}

TEST(fulfill_with_mismatched_explicit_generation_rejected) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 11u, 7u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 11u);
    ff.generation = 8u;
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == A840_KMD_STATE_CONFLICT);
    ff.generation = 7u;
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);
}

TEST(query_surface_writer_filter) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);

    A840KmdSurfacePromiseSnapshot snap{};
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 1001u, Env::kAppA, &snap) == A840_KMD_NOT_FOUND);
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 1001u, Env::kRenderServer, &snap) == kOk);
    CHECK(snap.writer_client == Env::kRenderServer);
}

/* ============================================================================
 * Group 4: Fault / Channel Reset / Client Close Atomic Cancellation & WaitAny (Cases 41..50)
 * ============================================================================ */

TEST(gpu_hang_fault_record_atomically_fails_promises_and_poisons_wait_any) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 21u, 2001u, 621u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kAppA, 300u, 21u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kAppA, 300u, 21u, r1.promise_token, 15u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    /* Launch both a Display handoff waiter AND a WaitAny waiter on queue 300. */
    std::atomic<A840KmdStatus> disp_rc{kOk};
    A840KmdDisplayHandoffResult dres{};
    std::thread disp_t([&] {
        A840KmdDisplayFrameRequest dreq{};
        dreq.consumer_client = Env::kDisplayClient;
        dreq.surface_handle = 621u;
        dreq.surface_identity = 2001u;
        dreq.min_generation = 1u;
        dreq.timeout_ns = A840_KMD_WAIT_INFINITE;
        dreq.flags = A840_DISPLAY_REQ_WAIT_FOR_PROMISE;
        dreq.mdss_caps = &e.caps;
        disp_rc.store(a840_kmd_display_handoff_submit_frame(&e.handoff, &dreq, &dres));
    });

    std::atomic<A840KmdStatus> wa_rc{kOk};
    std::thread wa_t([&] {
        NativeAdrenoAbiWaitAnyV3 wreq{};
        wreq.count = 1u;
        wreq.timeout_ns = A840_KMD_WAIT_INFINITE;
        wreq.queues[0] = 300u;
        wreq.fences[0] = 15u;
        NativeAdrenoAbiWaitAnyReplyV3 wrep{};
        wa_rc.store(a840_kmd_wait_any_wait(&e.wait_any, &wreq, &wrep));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    /* Record GPU hang via FaultCapture -> triggers poison callback -> fails promises + poisons WaitAny. */
    A840KmdFaultInput fi{};
    fi.client_id = Env::kAppA;
    fi.queue_id = 300u;
    fi.fence = 15u;
    fi.fault_kind = A840_KMD_FAULT_GPU_HANG;
    A840KmdFaultSnapshot fsnap{};
    CHECK(a840_kmd_fault_record(&e.fault_capture, &fi, &fsnap) == kOk);

    disp_t.join();
    wa_t.join();

    CHECK(disp_rc.load() == A840_KMD_POISONED);
    CHECK(dres.outcome == A840_DISPLAY_HANDOFF_CANCELED_ON_FAULT);
    CHECK(dres.promise_state == A840_PROMISE_STATE_FAILED);
    CHECK(dres.cancel_reason == A840_PROMISE_CANCEL_GPU_FAULT);
    CHECK(wa_rc.load() == A840_KMD_POISONED);

    A840KmdDisplayHandoffStats ds{};
    CHECK(a840_kmd_display_handoff_get_stats(&e.handoff, &ds) == kOk);
    CHECK(ds.canceled_on_fault == 1u);
}

TEST(fault_isolation_only_affects_faulted_channel_not_healthy_channels) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 21u, 2001u, 621u);
    e.add_ubwc_surface(Env::kAppB, 22u, 2002u, 622u);
    NativeAdrenoAbiWriterPromiseReplyV3 ra1{}, ra2{}, rb1{}, rb2{};

    auto mka = e.make_call(Env::kAppA, 301u, 21u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mka, &ra1, nullptr) == kOk);
    auto ffa = e.fulfill_call(Env::kAppA, 301u, 21u, ra1.promise_token, 5u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ffa, &ra2, nullptr) == kOk);

    auto mkb = e.make_call(Env::kAppB, 302u, 22u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mkb, &rb1, nullptr) == kOk);
    auto ffb = e.fulfill_call(Env::kAppB, 302u, 22u, rb1.promise_token, 5u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ffb, &rb2, nullptr) == kOk);

    /* Fail ONLY AppA queue 301. */
    CHECK(a840_kmd_writer_promise_fail_channel(
              &e.promise_reg, Env::kAppA, 301u, A840_PROMISE_CANCEL_CHANNEL_RESET) == 1u);

    /* AppA future promise on poisoned queue 301 rejected with POISONED. */
    auto mka2 = e.make_call(Env::kAppA, 301u, 21u);
    NativeAdrenoAbiWriterPromiseReplyV3 rbad{};
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mka2, &rbad, nullptr) == A840_KMD_POISONED);

    /* AppB queue 302 retires cleanly and commits to MDSS. */
    CHECK(a840_kmd_writer_promise_signal_fence(&e.promise_reg, Env::kAppB, 302u, 5u, 777u) == 1u);
    A840KmdDisplayFrameRequest dreq{};
    dreq.consumer_client = Env::kDisplayClient;
    dreq.surface_handle = 622u;
    dreq.surface_identity = 2002u;
    dreq.mdss_caps = &e.caps;
    A840KmdDisplayHandoffResult dres{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &dreq, &dres) == kOk);
    CHECK(dres.outcome == A840_DISPLAY_HANDOFF_COMMITTED);
}

TEST(smmu_fault_cancels_renderserver_scheduled_promise_before_retirement) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kRenderServer, 400u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kRenderServer, 400u, 11u, r1.promise_token, 100u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    A840KmdFaultInput fi{};
    fi.client_id = Env::kRenderServer;
    fi.queue_id = 400u;
    fi.fence = 100u;
    fi.fault_kind = A840_KMD_FAULT_SMMU_TRANSLATION;
    fi.smmu_fault_va = 0x100000ULL;
    CHECK(a840_kmd_fault_record(&e.fault_capture, &fi, nullptr) == kOk);

    A840KmdSurfacePromiseSnapshot snap{};
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 1001u, 0u, &snap) == kOk);
    CHECK(snap.state == A840_PROMISE_STATE_FAILED && snap.display_ready == 0u);

    /* Late fence signal on a poisoned channel MUST be ignored. */
    CHECK(a840_kmd_writer_promise_signal_fence(&e.promise_reg, Env::kRenderServer, 400u, 100u, 999u) == 0u);
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 1001u, 0u, &snap) == kOk);
    CHECK(snap.state == A840_PROMISE_STATE_FAILED);
}

TEST(client_close_cancels_all_inflight_wakes_waiters_and_disconnects_wait_any) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 21u, 2001u, 621u);
    e.add_ubwc_surface(Env::kAppA, 22u, 2002u, 622u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{}, r3{};

    auto mk1 = e.make_call(Env::kAppA, 501u, 21u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk1, &r1, nullptr) == kOk);
    auto ff1 = e.fulfill_call(Env::kAppA, 501u, 21u, r1.promise_token, 20u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff1, &r2, nullptr) == kOk);
    auto mk2 = e.make_call(Env::kAppA, 502u, 22u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk2, &r3, nullptr) == kOk);

    std::atomic<A840KmdStatus> disp_rc{kOk};
    A840KmdDisplayHandoffResult dres{};
    std::thread disp_t([&] {
        A840KmdDisplayFrameRequest dreq{};
        dreq.consumer_client = Env::kDisplayClient;
        dreq.surface_handle = 621u;
        dreq.surface_identity = 2001u;
        dreq.min_generation = 1u;
        dreq.timeout_ns = A840_KMD_WAIT_INFINITE;
        dreq.flags = A840_DISPLAY_REQ_WAIT_FOR_PROMISE;
        dreq.mdss_caps = &e.caps;
        disp_rc.store(a840_kmd_display_handoff_submit_frame(&e.handoff, &dreq, &dres));
    });

    std::atomic<A840KmdStatus> wa_rc{kOk};
    std::thread wa_t([&] {
        NativeAdrenoAbiWaitAnyV3 wreq{};
        wreq.count = 1u;
        wreq.timeout_ns = A840_KMD_WAIT_INFINITE;
        wreq.queues[0] = 501u;
        wreq.fences[0] = 20u;
        NativeAdrenoAbiWaitAnyReplyV3 wrep{};
        wa_rc.store(a840_kmd_wait_any_wait(&e.wait_any, &wreq, &wrep));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    CHECK(a840_kmd_writer_promise_close_client(&e.promise_reg, Env::kAppA) == 2u);

    disp_t.join();
    wa_t.join();

    CHECK(disp_rc.load() == A840_KMD_DISCONNECTED);
    CHECK(dres.outcome == A840_DISPLAY_HANDOFF_CANCELED_ON_FAULT);
    CHECK(dres.cancel_reason == A840_PROMISE_CANCEL_CLIENT_CLOSE);
    CHECK(wa_rc.load() == A840_KMD_DISCONNECTED);
    CHECK(a840_kmd_writer_promise_pending_count(&e.promise_reg, Env::kAppA) == 0u);
}

TEST(client_close_does_not_cancel_already_retired_promises) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 21u, 2001u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kAppA, 600u, 21u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kAppA, 600u, 21u, r1.promise_token, 10u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);
    CHECK(a840_kmd_writer_promise_signal_fence(&e.promise_reg, Env::kAppA, 600u, 10u, 50u) == 1u);

    CHECK(a840_kmd_writer_promise_close_client(&e.promise_reg, Env::kAppA) == 0u);
    A840KmdSurfacePromiseSnapshot snap{};
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 2001u, 0u, &snap) == kOk);
    CHECK(snap.state == A840_PROMISE_STATE_RETIRED_READY);
}

TEST(fail_channel_wakes_multiple_blocked_waiters_zero_deadlock) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 21u, 2001u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kAppA, 700u, 21u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kAppA, 700u, 21u, r1.promise_token, 99u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    constexpr int kWaiters = 6;
    std::atomic<int> woke{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kWaiters; ++i) {
        threads.emplace_back([&] {
            A840KmdSurfacePromiseSnapshot snap{};
            uint32_t to = 1u;
            CHECK(a840_kmd_writer_promise_wait_surface(&e.promise_reg, 2001u, 1u, A840_KMD_WAIT_INFINITE, &snap, &to) == kOk);
            CHECK(to == 0u && snap.state == A840_PROMISE_STATE_FAILED);
            woke.fetch_add(1);
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    CHECK(a840_kmd_writer_promise_fail_channel(&e.promise_reg, Env::kAppA, 700u, A840_PROMISE_CANCEL_GPU_FAULT) == 1u);
    for (auto &t : threads)
        t.join();
    CHECK(woke.load() == kWaiters);
}

TEST(wait_any_advances_on_promise_signal_fence) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 21u, 2001u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kAppA, 800u, 21u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kAppA, 800u, 21u, r1.promise_token, 44u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    std::thread irq([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(a840_kmd_writer_promise_signal_fence(&e.promise_reg, Env::kAppA, 800u, 44u, 77777u) == 1u);
    });

    NativeAdrenoAbiWaitAnyV3 wreq{};
    wreq.count = 1u;
    wreq.timeout_ns = 2000u * kMs;
    wreq.queues[0] = 800u;
    wreq.fences[0] = 44u;
    NativeAdrenoAbiWaitAnyReplyV3 wrep{};
    CHECK(a840_kmd_wait_any_wait(&e.wait_any, &wreq, &wrep) == kOk);
    CHECK(wrep.satisfied_mask == 1u && wrep.timed_out == 0u && wrep.observed_ticks == 77777u);
    irq.join();
}

TEST(batch_observer_delivery_exceeds_stack_chunk_size_zero_heap) {
    Env e;
    /* Create 40 surfaces (> 32 batch chunk size) on one queue and fail them all at once. */
    for (uint64_t i = 1u; i <= 40u; ++i) {
        CHECK(a840_kmd_writer_promise_bind_surface(&e.promise_reg, Env::kAppA, i, 9000u + i) == kOk);
        NativeAdrenoAbiWriterPromiseReplyV3 r{};
        auto mk = e.make_call(Env::kAppA, 810u, i);
        CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r, nullptr) == kOk);
    }
    size_t before = e.obs.events.size();
    CHECK(a840_kmd_writer_promise_fail_channel(&e.promise_reg, Env::kAppA, 810u, A840_PROMISE_CANCEL_GPU_FAULT) == 40u);
    CHECK(e.obs.events.size() - before == 40u);
    for (size_t i = before; i < e.obs.events.size(); ++i)
        CHECK(e.obs.events[i].state == A840_PROMISE_STATE_FAILED);
}

TEST(fail_channel_default_reason_maps_to_gpu_fault) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 21u, 2001u);
    NativeAdrenoAbiWriterPromiseReplyV3 r{};
    auto mk = e.make_call(Env::kAppA, 820u, 21u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r, nullptr) == kOk);
    CHECK(a840_kmd_writer_promise_fail_channel(&e.promise_reg, Env::kAppA, 820u, A840_PROMISE_CANCEL_NONE) == 1u);
    A840KmdSurfacePromiseSnapshot snap{};
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 2001u, 0u, &snap) == kOk);
    CHECK(snap.cancel_reason == A840_PROMISE_CANCEL_GPU_FAULT);
}

TEST(client_reregister_after_close_works_cleanly) {
    Env e;
    e.add_ubwc_surface(Env::kAppA, 21u, 2001u);
    CHECK(a840_kmd_writer_promise_close_client(&e.promise_reg, Env::kAppA) == 0u);
    CHECK(a840_kmd_writer_promise_register_client(&e.promise_reg, Env::kAppA, A840_CLIENT_ROLE_APP) == kOk);
    CHECK(a840_kmd_writer_promise_bind_surface(&e.promise_reg, Env::kAppA, 21u, 2001u) == kOk);
    NativeAdrenoAbiWriterPromiseReplyV3 r{};
    auto mk = e.make_call(Env::kAppA, 830u, 21u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r, nullptr) == kOk);
}

/* ============================================================================
 * Group 5: MDSS Display Handoff & UBWC 6.0 vs Linear Negotiation (Cases 51..66)
 * ============================================================================ */

TEST(display_handoff_ubwc6_bgra8_1200x2608_direct_scanout) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u, 80u, 1200u, 2608u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 101u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    A840KmdDisplayFrameRequest req{};
    req.consumer_client = Env::kDisplayClient;
    req.surface_handle = 511u;
    req.surface_identity = 1001u;
    req.mdss_caps = &e.caps;
    A840KmdDisplayHandoffResult res{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == kOk);
    CHECK(res.outcome == A840_DISPLAY_HANDOFF_COMMITTED);
    CHECK(res.scanout_mode == A840_DISPLAY_SCANOUT_UBWC6_DIRECT);
    CHECK(res.commit.ubwc_enabled == 1u && res.commit.ubwc_version == 6u &&
          res.commit.highest_bank_bit == 16u && res.commit.swizzle_levels == 6u &&
          res.commit.macrotile_mode == 1u);
}

TEST(display_handoff_ubwc6_rgba16f_direct_scanout) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u, 115u, 1200u, 2608u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 102u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    A840KmdDisplayFrameRequest req{};
    req.consumer_client = Env::kDisplayClient;
    req.surface_handle = 511u;
    req.surface_identity = 1001u;
    req.mdss_caps = &e.caps;
    A840KmdDisplayHandoffResult res{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == kOk);
    CHECK(res.outcome == A840_DISPLAY_HANDOFF_COMMITTED);
    CHECK(res.scanout_mode == A840_DISPLAY_SCANOUT_UBWC6_DIRECT);
}

TEST(display_handoff_incompatible_format_rejects_direct_ubwc_without_linear_fallback) {
    Env e;
    /* Format 70 is UBWC-capable on GPU, but restrict MDSS caps to only format 80. */
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u, 70u, 1200u, 2608u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 103u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    uint32_t bgra_only[1]{80u};
    A840KmdMdssCaps limited_caps = e.caps;
    limited_caps.formats = bgra_only;
    limited_caps.format_count = 1u;

    A840KmdDisplayFrameRequest req{};
    req.consumer_client = Env::kDisplayClient;
    req.surface_handle = 511u;
    req.surface_identity = 1001u;
    req.mdss_caps = &limited_caps;
    A840KmdDisplayHandoffResult res{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == A840_KMD_UNSUPPORTED);
    CHECK(res.outcome == A840_DISPLAY_HANDOFF_NEED_LINEAR_RESOLVE);
    CHECK(res.fallback_linear_required == 1u);
    CHECK(e.commits.commits.empty());
}

TEST(display_handoff_linear_fallback_commits_separate_linear_resolve_surface) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u, 70u, 1200u, 2608u);
    e.add_linear_surface(Env::kRenderServer, 12u, 1002u, 512u, 70u, 1200u, 2608u);

    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 12u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kRenderServer, 10u, 12u, r1.promise_token, 104u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    A840KmdMdssCaps no_ubwc_caps = e.caps;
    no_ubwc_caps.flags = 0u; /* MDSS cannot scan out UBWC 6.0 */

    A840KmdDisplayFrameRequest req{};
    req.consumer_client = Env::kDisplayClient;
    req.surface_handle = 511u;
    req.surface_identity = 1001u;
    req.linear_resolve_handle = 512u;
    req.linear_resolve_identity = 1002u;
    req.flags = A840_DISPLAY_REQ_ALLOW_LINEAR_FALLBACK;
    req.mdss_caps = &no_ubwc_caps;

    A840KmdDisplayHandoffResult res{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == kOk);
    CHECK(res.outcome == A840_DISPLAY_HANDOFF_COMMITTED);
    CHECK(res.scanout_mode == A840_DISPLAY_SCANOUT_LINEAR_FALLBACK);
    CHECK(res.commit.bo_handle == 512u && res.commit.surface_identity == 1002u);
    CHECK(res.commit.ubwc_enabled == 0u);

    A840KmdDisplayHandoffStats ds{};
    CHECK(a840_kmd_display_handoff_get_stats(&e.handoff, &ds) == kOk);
    CHECK(ds.linear_fallback_handoffs == 1u && ds.early_scheduled_handoffs == 1u);
}

TEST(display_handoff_linear_fallback_rejects_same_handle_or_compressed_resolve) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u, 80u, 1200u, 2608u);
    e.add_ubwc_surface(Env::kRenderServer, 12u, 1002u, 512u, 80u, 1200u, 2608u);

    A840KmdMdssCaps no_ubwc_caps = e.caps;
    no_ubwc_caps.flags = 0u;

    /* Same handle as primary rejected. */
    A840KmdDisplayFrameRequest req{};
    req.consumer_client = Env::kDisplayClient;
    req.surface_handle = 511u;
    req.surface_identity = 1001u;
    req.linear_resolve_handle = 511u;
    req.flags = A840_DISPLAY_REQ_ALLOW_LINEAR_FALLBACK;
    req.mdss_caps = &no_ubwc_caps;
    A840KmdDisplayHandoffResult res{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == A840_KMD_INVALID_ARG);

    /* Compressed resolve BO rejected with STATE_CONFLICT. */
    req.linear_resolve_handle = 512u;
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == A840_KMD_STATE_CONFLICT);
    CHECK(res.outcome == A840_DISPLAY_HANDOFF_NEED_LINEAR_RESOLVE);
}

TEST(display_handoff_primary_linear_surface_direct_scanout) {
    Env e;
    e.add_linear_surface(Env::kRenderServer, 12u, 1002u, 512u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 12u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kRenderServer, 10u, 12u, r1.promise_token, 55u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    A840KmdDisplayFrameRequest req{};
    req.consumer_client = Env::kDisplayClient;
    req.surface_handle = 512u;
    req.surface_identity = 1002u;
    req.mdss_caps = &e.caps;
    A840KmdDisplayHandoffResult res{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == kOk);
    CHECK(res.outcome == A840_DISPLAY_HANDOFF_COMMITTED);
    CHECK(res.scanout_mode == A840_DISPLAY_SCANOUT_LINEAR_DIRECT);

    A840KmdDisplayHandoffStats ds{};
    CHECK(a840_kmd_display_handoff_get_stats(&e.handoff, &ds) == kOk);
    CHECK(ds.linear_direct_handoffs == 1u);
}

TEST(display_handoff_null_mdss_caps_requires_linear_resolve) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u);
    A840KmdDisplayFrameRequest req{};
    req.consumer_client = Env::kDisplayClient;
    req.surface_handle = 511u;
    req.surface_identity = 1001u;
    req.mdss_caps = nullptr;
    A840KmdDisplayHandoffResult res{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == A840_KMD_UNSUPPORTED);
    CHECK(res.outcome == A840_DISPLAY_HANDOFF_NEED_LINEAR_RESOLVE);
}

TEST(display_handoff_zero_display_iova_requires_linear_resolve) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u, 80u, 1200u, 2608u, 0u);
    A840KmdDisplayFrameRequest req{};
    req.consumer_client = Env::kDisplayClient;
    req.surface_handle = 511u;
    req.surface_identity = 1001u;
    req.mdss_caps = &e.caps;
    A840KmdDisplayHandoffResult res{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == A840_KMD_UNSUPPORTED);
    CHECK(res.outcome == A840_DISPLAY_HANDOFF_NEED_LINEAR_RESOLVE);
}

TEST(display_handoff_oversized_surface_exceeding_mdss_limits_requires_linear) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u, 80u, 2400u, 2608u);
    A840KmdDisplayFrameRequest req{};
    req.consumer_client = Env::kDisplayClient;
    req.surface_handle = 511u;
    req.surface_identity = 1001u;
    req.mdss_caps = &e.caps;
    A840KmdDisplayHandoffResult res{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == A840_KMD_UNSUPPORTED);
    CHECK(res.outcome == A840_DISPLAY_HANDOFF_NEED_LINEAR_RESOLVE);
}

TEST(display_handoff_stricter_mdss_alignment_requires_linear) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u, 80u, 1200u, 2608u);
    A840KmdMdssCaps strict_caps = e.caps;
    strict_caps.pitch_alignment = 128u; /* meta_pitch is 64 */
    A840KmdDisplayFrameRequest req{};
    req.consumer_client = Env::kDisplayClient;
    req.surface_handle = 511u;
    req.surface_identity = 1001u;
    req.mdss_caps = &strict_caps;
    A840KmdDisplayHandoffResult res{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == A840_KMD_UNSUPPORTED);
    CHECK(res.outcome == A840_DISPLAY_HANDOFF_NEED_LINEAR_RESOLVE);
}

TEST(display_handoff_request_reserved_and_flags_fail_closed) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u);
    A840KmdDisplayFrameRequest req{};
    req.consumer_client = Env::kDisplayClient;
    req.surface_handle = 511u;
    req.surface_identity = 1001u;
    req.reserved = 1u;
    req.mdss_caps = &e.caps;
    A840KmdDisplayHandoffResult res{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == A840_KMD_INVALID_ARG);
    req.reserved = 0u;
    req.flags = 1u << 10;
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == A840_KMD_INVALID_ARG);
    req.flags = 0u;
    req.consumer_client = 0u;
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == A840_KMD_INVALID_ARG);
}

TEST(display_handoff_unknown_surface_or_promise_rejected) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u);
    /* Surface exists in surface_mgr, but no WriterPromise has been made yet. */
    A840KmdDisplayFrameRequest req{};
    req.consumer_client = Env::kDisplayClient;
    req.surface_handle = 511u;
    req.surface_identity = 1001u;
    req.mdss_caps = &e.caps;
    A840KmdDisplayHandoffResult res{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == A840_KMD_NOT_FOUND);
    CHECK(res.outcome == A840_DISPLAY_HANDOFF_REJECTED);

    /* Unknown surface handle in surface_mgr */
    req.surface_handle = 9999u;
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == A840_KMD_NOT_FOUND);
}

TEST(display_handoff_commit_callback_runs_outside_lock_and_propagates_error) {
    Env e;
    e.commits.verify_unlocked = true;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 111u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    A840KmdDisplayFrameRequest req{};
    req.consumer_client = Env::kDisplayClient;
    req.surface_handle = 511u;
    req.surface_identity = 1001u;
    req.mdss_caps = &e.caps;
    A840KmdDisplayHandoffResult res{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == kOk);

    e.commits.next_status = A840_KMD_IO_ERROR;
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == A840_KMD_IO_ERROR);
    CHECK(res.outcome == A840_DISPLAY_HANDOFF_REJECTED);
}

TEST(display_handoff_min_generation_gate_blocks_stale_frame) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 10u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    /* Display asks for min_generation=2 while only gen=1 exists -> TIMED_OUT. */
    A840KmdDisplayFrameRequest req{};
    req.consumer_client = Env::kDisplayClient;
    req.surface_handle = 511u;
    req.surface_identity = 1001u;
    req.min_generation = 2u;
    req.mdss_caps = &e.caps;
    A840KmdDisplayHandoffResult res{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &req, &res) == kOk);
    CHECK(res.outcome == A840_DISPLAY_HANDOFF_TIMED_OUT && res.timed_out == 1u);
}

TEST(display_handoff_custom_sync_ops_backend) {
    struct CustomSync {
        std::mutex mu;
        std::condition_variable cv;
        uint64_t ticks = 0;
    } psync, dsync;

    A840KmdSurfaceManager sm{};
    A840KmdSurfaceOps sops{&dsync,
        [](void *p) { static_cast<CustomSync *>(p)->mu.lock(); },
        [](void *p) { static_cast<CustomSync *>(p)->mu.unlock(); }};
    CHECK(a840_kmd_surface_init(&sm, &sops) == kOk);

    A840KmdPromiseSyncOps pops{};
    pops.cookie = &psync;
    pops.lock = [](void *p) { static_cast<CustomSync *>(p)->mu.lock(); };
    pops.unlock = [](void *p) { static_cast<CustomSync *>(p)->mu.unlock(); };
    pops.wake_all = [](void *p) { static_cast<CustomSync *>(p)->cv.notify_all(); };
    pops.now_ns = [](void *p) -> uint64_t {
        auto *s = static_cast<CustomSync *>(p);
        s->ticks += 1000u;
        return s->ticks;
    };
    pops.wait = [](void *p, uint64_t, int) -> int {
        auto *s = static_cast<CustomSync *>(p);
        std::unique_lock<std::mutex> lk(s->mu, std::adopt_lock);
        auto rc = s->cv.wait_for(lk, std::chrono::milliseconds(5));
        lk.release();
        return (rc == std::cv_status::timeout) ? 1 : 0;
    };

    A840KmdWriterPromiseRegistry preg{};
    A840KmdPromiseConfig pcfg{};
    pcfg.sync_ops = &pops;
    CHECK(a840_kmd_writer_promise_init(&preg, &pcfg) == kOk);

    A840KmdDisplaySyncOps dops{&dsync,
        [](void *p) { static_cast<CustomSync *>(p)->mu.lock(); },
        [](void *p) { static_cast<CustomSync *>(p)->mu.unlock(); }};
    A840KmdDisplayHandoff dh{};
    A840KmdDisplayHandoffConfig dcfg{};
    dcfg.surface_mgr = &sm;
    dcfg.promise_reg = &preg;
    dcfg.sync_ops = &dops;
    CHECK(a840_kmd_display_handoff_init(&dh, &dcfg) == kOk);

    CHECK(a840_kmd_writer_promise_register_client(&preg, 1u, A840_CLIENT_ROLE_RENDER_SERVER) == kOk);
    CHECK(a840_kmd_writer_promise_bind_surface(&preg, 1u, 10u, 100u) == kOk);
    A840KmdSurfaceLayout layout{kPage * 32u, kPage * 16u, 80u, 1200u, 2608u, 163u};
    CHECK(a840_kmd_surface_create(&sm, 1u, 10u, 100u, &layout) == kOk);
    NativeAdrenoAbiSurfaceMetaV3 meta{10u, 1u, 1u, 6u, 16u, 6u, 1u, kPage, 64u, 0u};
    A840KmdSurfaceResult sres{};
    CHECK(a840_kmd_surface_meta(&sm, 1u, &meta, 0u, nullptr, &sres) == kOk);

    A840KmdWriterPromiseCall mk{};
    mk.writer_client = 1u;
    mk.abi.op = NativeAdrenoPromiseMake;
    mk.abi.surface_count = 1u;
    mk.abi.queue = 1u;
    mk.abi.surface_ids[0] = 10u;
    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    CHECK(a840_kmd_writer_promise_dispatch(&preg, &mk, &r1, nullptr) == kOk);
    auto ff = mk;
    ff.abi.op = NativeAdrenoPromiseFulfill;
    ff.abi.promise_token = r1.promise_token;
    ff.abi.fence = 5u;
    CHECK(a840_kmd_writer_promise_dispatch(&preg, &ff, &r2, nullptr) == kOk);

    uint32_t fmts[1]{80u};
    A840KmdMdssCaps caps{A840_MDSS_UBWC6, 1200u, 2608u, 4096u, 64u, fmts, 1u};
    A840KmdDisplayFrameRequest req{};
    req.consumer_client = 1u;
    req.surface_handle = 10u;
    req.surface_identity = 100u;
    req.mdss_caps = &caps;
    A840KmdDisplayHandoffResult dres{};
    CHECK(a840_kmd_display_handoff_submit_frame(&dh, &req, &dres) == kOk);
    CHECK(dres.outcome == A840_DISPLAY_HANDOFF_COMMITTED);

    a840_kmd_display_handoff_destroy(&dh);
    a840_kmd_writer_promise_destroy(&preg);
}

TEST(display_handoff_full_diagnostics_accounting) {
    Env e;
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u, 511u, 80u, 1200u, 2608u);
    e.add_ubwc_surface(Env::kAppA, 21u, 2001u, 621u, 80u, 1200u, 2608u);
    e.add_ubwc_surface(Env::kRenderServer, 13u, 1003u, 513u, 70u, 1200u, 2608u);
    e.add_linear_surface(Env::kRenderServer, 14u, 1004u, 514u, 70u, 1200u, 2608u);
    e.add_ubwc_surface(Env::kAppB, 22u, 2002u, 622u, 80u, 1200u, 2608u);

    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    /* 1. RenderServer early scheduled UBWC direct */
    auto mk1 = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk1, &r1, nullptr) == kOk);
    auto ff1 = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 10u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff1, &r2, nullptr) == kOk);
    A840KmdDisplayFrameRequest d1{Env::kDisplayClient, 511u, 1001u, 1u, 0u, 0u, 0u, 0u, 0u, &e.caps};
    A840KmdDisplayHandoffResult res{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &d1, &res) == kOk);

    /* 2. AppA deferred -> retired -> fence_satisfied + deferred_app */
    auto mk2 = e.make_call(Env::kAppA, 20u, 21u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk2, &r1, nullptr) == kOk);
    auto ff2 = e.fulfill_call(Env::kAppA, 20u, 21u, r1.promise_token, 20u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff2, &r2, nullptr) == kOk);
    CHECK(a840_kmd_writer_promise_signal_fence(&e.promise_reg, Env::kAppA, 20u, 20u, 100u) == 1u);
    A840KmdDisplayFrameRequest d2{Env::kDisplayClient, 621u, 2001u, 1u, 0u, 0u, 0u, 0u, 0u, &e.caps};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &d2, &res) == kOk);

    /* 3. Linear fallback handoff */
    auto mk3 = e.make_call(Env::kRenderServer, 10u, 14u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk3, &r1, nullptr) == kOk);
    auto ff3 = e.fulfill_call(Env::kRenderServer, 10u, 14u, r1.promise_token, 30u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff3, &r2, nullptr) == kOk);
    A840KmdMdssCaps no_ubwc = e.caps;
    no_ubwc.flags = 0u;
    A840KmdDisplayFrameRequest d3{Env::kDisplayClient, 513u, 1003u, 1u, 0u, 514u, 1004u, A840_DISPLAY_REQ_ALLOW_LINEAR_FALLBACK, 0u, &no_ubwc};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &d3, &res) == kOk);

    /* 4. Fault canceled */
    auto mk4 = e.make_call(Env::kAppB, 30u, 22u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk4, &r1, nullptr) == kOk);
    auto ff4 = e.fulfill_call(Env::kAppB, 30u, 22u, r1.promise_token, 40u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff4, &r2, nullptr) == kOk);
    CHECK(a840_kmd_writer_promise_fail_channel(&e.promise_reg, Env::kAppB, 30u, A840_PROMISE_CANCEL_GPU_FAULT) == 1u);
    A840KmdDisplayFrameRequest d4{Env::kDisplayClient, 622u, 2002u, 1u, 0u, 0u, 0u, 0u, 0u, &e.caps};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &d4, &res) == A840_KMD_POISONED);

    A840KmdDisplayHandoffStats ds{};
    CHECK(a840_kmd_display_handoff_get_stats(&e.handoff, &ds) == kOk);
    CHECK(ds.early_scheduled_handoffs == 2u);
    CHECK(ds.deferred_app_handoffs == 1u);
    CHECK(ds.fence_satisfied_handoffs == 1u);
    CHECK(ds.canceled_on_fault == 1u);
    CHECK(ds.linear_fallback_handoffs == 1u);
    CHECK(ds.ubwc_direct_handoffs == 2u);
}

/* ============================================================================
 * Group 6: Extra Edge Cases & Concurrency / TSan Stress (Cases 67..72)
 * ============================================================================ */

TEST(noncompressible_xr_format_rejected_for_ubwc_requires_linear) {
    Env e;
    /* BGR10_XR format (554) does not support UBWC on Adreno 840. */
    A840KmdSurfaceLayout layout{kPage * 32u, kPage * 16u, 554u, 1200u, 2608u, 163u};
    CHECK(a840_kmd_surface_create(&e.surface_mgr, Env::kRenderServer, 77u, 7700u, &layout) == kOk);
    NativeAdrenoAbiSurfaceMetaV3 meta{77u, 1u, 1u, 6u, 16u, 6u, 1u, kPage, 64u, 0u};
    A840KmdSurfaceResult sres{};
    CHECK(a840_kmd_surface_meta(&e.surface_mgr, Env::kRenderServer, &meta, 1u, &e.caps, &sres) == A840_KMD_UNSUPPORTED);
    CHECK(sres.fallback_linear_required == 1u && sres.scanout_eligible == 0u);

    /* Register as linear instead -> display_handoff commits LINEAR_DIRECT. */
    meta.ubwc_enabled = 0u;
    meta.meta_offset = 0u;
    meta.meta_pitch = 0u;
    CHECK(a840_kmd_surface_meta(&e.surface_mgr, Env::kRenderServer, &meta, 1u, &e.caps, &sres) == kOk);
    CHECK(a840_kmd_surface_import(&e.surface_mgr, Env::kDisplayClient, 777u, 7700u) == kOk);
    CHECK(a840_kmd_writer_promise_bind_surface(&e.promise_reg, Env::kRenderServer, 77u, 7700u) == kOk);

    NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 77u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    auto ff = e.fulfill_call(Env::kRenderServer, 10u, 77u, r1.promise_token, 1u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);

    A840KmdDisplayFrameRequest dreq{Env::kDisplayClient, 777u, 7700u, 1u, 0u, 0u, 0u, 0u, 0u, &e.caps};
    A840KmdDisplayHandoffResult dres{};
    CHECK(a840_kmd_display_handoff_submit_frame(&e.handoff, &dreq, &dres) == kOk);
    CHECK(dres.outcome == A840_DISPLAY_HANDOFF_COMMITTED && dres.scanout_mode == A840_DISPLAY_SCANOUT_LINEAR_DIRECT);
}

TEST(destroy_registry_wakes_blocked_waiters_with_disconnected) {
    A840KmdWriterPromiseRegistry reg{};
    CHECK(a840_kmd_writer_promise_init(&reg, nullptr) == kOk);
    CHECK(a840_kmd_writer_promise_register_client(&reg, 1u, A840_CLIENT_ROLE_APP) == kOk);
    CHECK(a840_kmd_writer_promise_bind_surface(&reg, 1u, 10u, 100u) == kOk);

    A840KmdWriterPromiseCall mk{};
    mk.writer_client = 1u;
    mk.abi.op = NativeAdrenoPromiseMake;
    mk.abi.surface_count = 1u;
    mk.abi.queue = 1u;
    mk.abi.surface_ids[0] = 10u;
    NativeAdrenoAbiWriterPromiseReplyV3 r1{};
    CHECK(a840_kmd_writer_promise_dispatch(&reg, &mk, &r1, nullptr) == kOk);

    std::atomic<A840KmdStatus> rc{kOk};
    A840KmdSurfacePromiseSnapshot snap{};
    std::thread waiter([&] {
        uint32_t to = 0u;
        rc.store(a840_kmd_writer_promise_wait_surface(&reg, 100u, 1u, A840_KMD_WAIT_INFINITE, &snap, &to));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    /* Cancel via close_client so thread exits before destroy destroys pthread_mutex. */
    CHECK(a840_kmd_writer_promise_close_client(&reg, 1u) == 1u);
    waiter.join();
    CHECK(rc.load() == kOk && snap.state == A840_PROMISE_STATE_CANCELED);
    a840_kmd_writer_promise_destroy(&reg);
}

TEST(query_token_and_surface_invalid_args) {
    Env e;
    A840KmdSurfacePromiseSnapshot snap{};
    A840KmdPromiseEvent ev{};
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 0u, 0u, &snap) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_writer_promise_query_surface(&e.promise_reg, 1u, 0u, nullptr) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_writer_promise_query_token(&e.promise_reg, 0u, &ev) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_writer_promise_query_token(&e.promise_reg, 99999u, &ev) == A840_KMD_NOT_FOUND);
    CHECK(a840_kmd_writer_promise_wait_surface(&e.promise_reg, 0u, 1u, 0u, &snap, nullptr) == A840_KMD_INVALID_ARG);
    CHECK(a840_kmd_writer_promise_wait_surface(&e.promise_reg, 1u, 1u, 0u, nullptr, nullptr) == A840_KMD_INVALID_ARG);
}

TEST(set_observer_can_clear_and_rebind) {
    Env e;
    CHECK(a840_kmd_writer_promise_set_observer(&e.promise_reg, nullptr, nullptr) == kOk);
    e.add_ubwc_surface(Env::kRenderServer, 11u, 1001u);
    NativeAdrenoAbiWriterPromiseReplyV3 r1{};
    auto mk = e.make_call(Env::kRenderServer, 10u, 11u);
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
    CHECK(e.obs.events.empty());
    CHECK(a840_kmd_writer_promise_set_observer(&e.promise_reg, &ObserverLog::callback, &e.obs) == kOk);
    auto ff = e.fulfill_call(Env::kRenderServer, 10u, 11u, r1.promise_token, 10u);
    NativeAdrenoAbiWriterPromiseReplyV3 r2{};
    CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);
    CHECK(e.obs.events.size() == 1u);
}

TEST(fault_callback_null_cookie_safe) {
    a840_kmd_writer_promise_on_fault_poison(nullptr, 1u, 2u, nullptr);
}

/* Stress routine shared between standalone test case and --tsan-stress N CLI mode:
 * Runs RenderServer + 4 Apps + Display Consumer + WaitAny Consumer + Fault Injector
 * simultaneously with zero data races and zero deadlocks.
 */
void run_concurrent_stress_round(uint32_t round_idx)
{
    Env e;
    constexpr uint64_t kRsClient = 100u;
    constexpr uint64_t kRsQueue = 1000u;
    constexpr uint64_t kRsSurfHandle = 11u;
    constexpr uint64_t kRsSurfId = 10001u;
    constexpr uint64_t kRsDispHandle = 5001u;

    e.add_ubwc_surface(kRsClient, kRsSurfHandle, kRsSurfId, kRsDispHandle, 80u, 1200u, 2608u);

    constexpr int kNumApps = 4;
    uint64_t app_clients[kNumApps]{301u, 302u, 303u, 304u};
    uint64_t app_queues[kNumApps]{2001u, 2002u, 2003u, 2004u};
    uint64_t app_handles[kNumApps]{31u, 32u, 33u, 34u};
    uint64_t app_ids[kNumApps]{20001u, 20002u, 20003u, 20004u};
    uint64_t app_disp[kNumApps]{6001u, 6002u, 6003u, 6004u};

    for (int a = 0; a < kNumApps; ++a) {
        CHECK(a840_kmd_writer_promise_register_client(&e.promise_reg, app_clients[a], A840_CLIENT_ROLE_APP) == kOk);
        e.add_ubwc_surface(app_clients[a], app_handles[a], app_ids[a], app_disp[a], 80u, 1200u, 2608u);
    }

    constexpr uint32_t kFrames = 12u;
    std::atomic<uint32_t> rs_done{0u};
    std::atomic<uint32_t> apps_done{0u};
    std::atomic<uint32_t> committed_frames{0u};
    std::atomic<uint32_t> faulted_frames{0u};

    /* 1. RenderServer producer thread (early scheduled handoff + delayed fence retirement). */
    std::thread rs_thread([&] {
        for (uint32_t f = 1u; f <= kFrames; ++f) {
            NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
            auto mk = e.make_call(kRsClient, kRsQueue, kRsSurfHandle, 0u, A840_PROMISE_FLAG_EARLY_HANDOFF);
            CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr) == kOk);
            auto ff = e.fulfill_call(kRsClient, kRsQueue, kRsSurfHandle, r1.promise_token, f, A840_PROMISE_FLAG_EARLY_HANDOFF);
            CHECK(a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr) == kOk);
            if ((f % 3u) == 0u)
                (void)a840_kmd_writer_promise_signal_fence(&e.promise_reg, kRsClient, kRsQueue, f, f * 100u);
        }
        (void)a840_kmd_writer_promise_signal_fence(&e.promise_reg, kRsClient, kRsQueue, kFrames, kFrames * 100u);
        rs_done.store(1u, std::memory_order_release);
    });

    /* 2. Four App producer threads:
     *    - App 0 & 1: normal deferred -> fence retirement
     *    - App 2: gets GPU Hang fault injected mid-stream
     *    - App 3: closes client mid-stream
     */
    std::vector<std::thread> app_threads;
    for (int a = 0; a < kNumApps; ++a) {
        app_threads.emplace_back([&, a] {
            for (uint32_t f = 1u; f <= kFrames; ++f) {
                NativeAdrenoAbiWriterPromiseReplyV3 r1{}, r2{};
                auto mk = e.make_call(app_clients[a], app_queues[a], app_handles[a], 0u, A840_PROMISE_FLAG_EARLY_HANDOFF);
                A840KmdStatus st = a840_kmd_writer_promise_dispatch(&e.promise_reg, &mk, &r1, nullptr);
                if (st == A840_KMD_POISONED || st == A840_KMD_NOT_FOUND || st == A840_KMD_PERMISSION_DENIED)
                    break;
                CHECK(st == kOk);

                auto ff = e.fulfill_call(app_clients[a], app_queues[a], app_handles[a], r1.promise_token, f);
                st = a840_kmd_writer_promise_dispatch(&e.promise_reg, &ff, &r2, nullptr);
                if (st == A840_KMD_POISONED || st == A840_KMD_NOT_FOUND || st == A840_KMD_STATE_CONFLICT)
                    break;
                CHECK(st == kOk);

                if (a < 2) {
                    (void)a840_kmd_writer_promise_signal_fence(
                        &e.promise_reg, app_clients[a], app_queues[a], f, (round_idx + 1u) * 1000u + f);
                } else if (a == 2 && f == 4u) {
                    A840KmdFaultInput fi{};
                    fi.client_id = app_clients[a];
                    fi.queue_id = app_queues[a];
                    fi.fence = f;
                    fi.fault_kind = A840_KMD_FAULT_GPU_HANG;
                    CHECK(a840_kmd_fault_record(&e.fault_capture, &fi, nullptr) == kOk);
                } else if (a == 3 && f == 4u) {
                    (void)a840_kmd_writer_promise_close_client(&e.promise_reg, app_clients[a]);
                } else {
                    (void)a840_kmd_writer_promise_signal_fence(
                        &e.promise_reg, app_clients[a], app_queues[a], f, (round_idx + 1u) * 2000u + f);
                }
            }
            apps_done.fetch_add(1u, std::memory_order_release);
        });
    }

    /* 3. Display consumer thread polling & waiting on RenderServer + all 4 Apps. */
    std::thread display_thread([&] {
        for (uint32_t iter = 0u; iter < kFrames * 2u; ++iter) {
            /* RenderServer surface */
            A840KmdDisplayFrameRequest rreq{};
            rreq.consumer_client = Env::kDisplayClient;
            rreq.surface_handle = kRsDispHandle;
            rreq.surface_identity = kRsSurfId;
            rreq.min_generation = 1u;
            rreq.timeout_ns = 10u * kMs;
            rreq.flags = A840_DISPLAY_REQ_WAIT_FOR_PROMISE;
            rreq.mdss_caps = &e.caps;
            A840KmdDisplayHandoffResult dres{};
            A840KmdStatus st = a840_kmd_display_handoff_submit_frame(&e.handoff, &rreq, &dres);
            if (st == kOk && dres.outcome == A840_DISPLAY_HANDOFF_COMMITTED)
                committed_frames.fetch_add(1u);

            for (int a = 0; a < kNumApps; ++a) {
                A840KmdDisplayFrameRequest areq{};
                areq.consumer_client = Env::kDisplayClient;
                areq.surface_handle = app_disp[a];
                areq.surface_identity = app_ids[a];
                areq.min_generation = 1u;
                areq.timeout_ns = 5u * kMs;
                areq.flags = A840_DISPLAY_REQ_WAIT_FOR_PROMISE;
                areq.mdss_caps = &e.caps;
                A840KmdDisplayHandoffResult ares{};
                A840KmdStatus ast = a840_kmd_display_handoff_submit_frame(&e.handoff, &areq, &ares);
                if (ast == kOk && ares.outcome == A840_DISPLAY_HANDOFF_COMMITTED) {
                    /* Defensive invariant: App handoff MUST NEVER be early GPU fence! */
                    CHECK(ares.sync_mode == A840_DISPLAY_SYNC_RETIRED_READY);
                    CHECK(ares.promise_snapshot.fence_retired == 1u);
                    committed_frames.fetch_add(1u);
                } else if (ast == A840_KMD_POISONED || ast == A840_KMD_DISCONNECTED) {
                    faulted_frames.fetch_add(1u);
                }
            }
        }
    });

    /* 4. WaitAny consumer thread observing healthy App0 + faulted App2 + closed App3. */
    std::thread wait_any_thread([&] {
        for (int a = 0; a < kNumApps; ++a) {
            NativeAdrenoAbiWaitAnyV3 wreq{};
            wreq.count = 1u;
            wreq.timeout_ns = 500u * kMs;
            wreq.queues[0] = app_queues[a];
            wreq.fences[0] = (a < 2) ? kFrames : 4u;
            NativeAdrenoAbiWaitAnyReplyV3 wrep{};
            A840KmdStatus wst = a840_kmd_wait_any_wait(&e.wait_any, &wreq, &wrep);
            if (a < 2) {
                CHECK(wst == kOk && wrep.satisfied_mask == 1u);
            } else if (a == 2) {
                CHECK(wst == A840_KMD_POISONED);
            } else {
                CHECK(wst == A840_KMD_DISCONNECTED || wst == kOk);
            }
        }
    });

    rs_thread.join();
    for (auto &t : app_threads)
        t.join();
    display_thread.join();
    wait_any_thread.join();

    CHECK(rs_done.load() == 1u);
    CHECK(apps_done.load() == kNumApps);
    CHECK(committed_frames.load() > 0u);
    CHECK(faulted_frames.load() > 0u);

    /* Faulted App2 and closed App3 must have 0 pending promises remaining. */
    CHECK(a840_kmd_writer_promise_pending_count(&e.promise_reg, app_clients[2]) == 0u);
    CHECK(a840_kmd_writer_promise_pending_count(&e.promise_reg, app_clients[3]) == 0u);
}

TEST(multithreaded_renderserver_4apps_display_waitany_fault_stress) {
    run_concurrent_stress_round(0u);
}

} // namespace

int main(int argc, char **argv)
{
    uint32_t tsan_rounds = 1u;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--tsan-stress") == 0 && i + 1 < argc) {
            tsan_rounds = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            if (tsan_rounds == 0u)
                tsan_rounds = 1u;
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    for (const auto &c : cases())
        c.run();

    for (uint32_t r = 1u; r < tsan_rounds; ++r)
        run_concurrent_stress_round(r);

    std::printf("PASS phase3_promise_display: %zu independent cases, %u stress rounds\n",
                cases().size(), tsan_rounds);
    return 0;
}
