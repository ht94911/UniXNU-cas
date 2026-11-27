/* SPDX-License-Identifier: MIT */
#include "a840_kmd_tier0_pack.h"
#include "a840_kmd_ubwc_surface.h"

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/if_alg.h>
#include <map>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
constexpr auto ok = A840_KMD_OK;
constexpr uint32_t ro = A840_KMD_SMMU_PERM_READ_ONLY;
constexpr uint64_t page = A840_ABI_V3_PAGE_BYTES;
[[noreturn]] void fail(const char *expr, int line)
{
    std::fprintf(stderr, "FAIL line %d: %s (errno %d)\n", line, expr, errno);
    std::abort();
}
#define CHECK(expr) do { if (!(expr)) fail(#expr, __LINE__); } while (false)
struct Case { const char *name; void (*run)(); };
std::vector<Case>& cases() { static std::vector<Case> all; return all; }
struct Register { Register(const char *n, void (*f)()) { cases().push_back({n, f}); } };
#define TEST(n) static void n(); static Register reg_##n(#n, n); static void n()

struct Surface {
    std::mutex mutex;
    A840KmdSurfaceManager m{};
    A840KmdSurfaceLayout layout{page * 32, page * 16, 80, 1200, 2608, 163};
    uint32_t formats[3]{80, 70, 115};
    A840KmdMdssCaps caps{A840_MDSS_UBWC6, 1200, 2608, 4096, 64, formats, 3};
    NativeAdrenoAbiSurfaceMetaV3 req{11, 1, 1, 6, 16, 6, 1, page, 64, 0};
    A840KmdSurfaceResult out{};
    Surface() {
        A840KmdSurfaceOps ops{&mutex,
            [](void *p){ static_cast<std::mutex*>(p)->lock(); },
            [](void *p){ static_cast<std::mutex*>(p)->unlock(); }};
        CHECK(a840_kmd_surface_init(&m, &ops) == ok);
    }
    void create() { CHECK(a840_kmd_surface_create(&m, 1, 11, 101, &layout) == ok); }
    A840KmdStatus call(uint32_t scan = 1, uint64_t client = 1) {
        return a840_kmd_surface_meta(&m, client, &req, scan, &caps, &out);
    }
    void fallback(bool registered = true) {
        CHECK(call() == ok && !out.scanout_eligible && !out.meta.flags && out.fallback_linear_required);
        CHECK(out.layout_valid == uint32_t(registered) && out.meta.bo_handle == req.bo_handle && out.meta.op == req.op);
    }
    void imported() { CHECK(a840_kmd_surface_import(&m, 2, 22, 101) == ok); }
};

TEST(panel_bgra_direct) {
    Surface s; s.create(); CHECK(s.call() == ok);
    CHECK(s.out.layout_valid && s.out.scanout_eligible && s.out.meta.flags == 1 && !s.out.fallback_linear_required);
}
TEST(rgba_direct) { Surface s; s.layout.format = 70; s.create(); CHECK(s.call() == ok && s.out.scanout_eligible); }
TEST(rgba16f_direct) { Surface s; s.layout.format = 115; s.create(); CHECK(s.call() == ok && s.out.scanout_eligible); }
TEST(hbb15_rejected) { Surface s; s.create(); s.req.highest_bank_bit = 15; CHECK(s.call() == A840_KMD_INVALID_ARG); }
TEST(wrong_version) { Surface s; s.create(); s.req.ubwc_version = 5; CHECK(s.call() == A840_KMD_INVALID_ARG); }
TEST(wrong_swizzle) { Surface s; s.create(); s.req.swizzle_levels = 3; CHECK(s.call() == A840_KMD_INVALID_ARG); }
TEST(wrong_macrotile) { Surface s; s.create(); s.req.macrotile_mode = 0; CHECK(s.call() == A840_KMD_INVALID_ARG); }
TEST(linear_also_locks_hbb) {
    Surface s; s.create(); s.req.ubwc_enabled = 0; s.req.highest_bank_bit = 15; CHECK(s.call() == A840_KMD_INVALID_ARG);
}
TEST(linear_is_already_linear) {
    Surface s; s.create(); s.req.ubwc_enabled = 0; s.req.meta_pitch = 0;
    CHECK(s.call() == ok && s.out.layout_valid && !s.out.scanout_eligible && !s.out.fallback_linear_required);
}
TEST(forged_scanout_flag) { Surface s; s.create(); s.req.flags = 1; s.caps.flags = 0; s.fallback(); }
TEST(unknown_input_flags) { Surface s; s.create(); s.req.flags = 2; CHECK(s.call() == A840_KMD_INVALID_ARG); }
TEST(no_display_caps) {
    Surface s; s.create(); CHECK(a840_kmd_surface_meta(&s.m, 1, &s.req, 1, nullptr, &s.out) == ok);
    CHECK(s.out.fallback_linear_required && !s.out.scanout_eligible);
}
TEST(display_format_not_listed) { Surface s; s.create(); s.caps.formats = s.formats + 1; s.caps.format_count = 2; s.fallback(); }
TEST(no_display_address) { Surface s; s.layout.display_base = 0; s.create(); s.fallback(); }
TEST(misaligned_display_base) { Surface s; ++s.layout.display_base; s.create(); s.fallback(); }
TEST(display_iova_wrap) { Surface s; s.layout.display_base = UINT64_MAX - page + 1; s.create(); s.fallback(); }
TEST(width_limit) { Surface s; ++s.layout.width; s.create(); s.fallback(); }
TEST(height_limit) { Surface s; ++s.layout.height; s.create(); s.fallback(); }
TEST(stricter_plane_alignment) { Surface s; s.caps.base_alignment = 8192; s.create(); s.fallback(); }
TEST(stricter_pitch_alignment) { Surface s; s.caps.pitch_alignment = 128; s.create(); s.fallback(); }
TEST(unaligned_pitch_requires_separate_linear_bo) { Surface s; s.create(); ++s.req.meta_pitch; s.fallback(false); }
TEST(unaligned_offset_preserves_old_layout) {
    Surface s; s.create(); CHECK(s.call() == ok); ++s.req.meta_offset; s.fallback(false);
    s.req.op = 0; CHECK(s.call() == ok && s.out.meta.meta_offset == page);
}
TEST(invalid_render_layout_alignment) { Surface s; s.create(); ++s.req.meta_pitch; CHECK(s.call(0) == A840_KMD_ALIGNMENT); }
TEST(zero_compressed_pitch) { Surface s; s.create(); s.req.meta_pitch = 0; CHECK(s.call() == A840_KMD_INVALID_ARG); }
TEST(metadata_tail_bounds) {
    Surface s; s.layout.bytes = page + 64 * s.layout.meta_rows - 1; s.create(); CHECK(s.call() == A840_KMD_RANGE);
}
TEST(metadata_exact_end) {
    Surface s; s.layout.bytes = page + 64 * s.layout.meta_rows; s.create(); CHECK(s.call() == ok);
}
TEST(offset_overflow) { Surface s; s.create(); s.req.meta_offset = UINT64_MAX; CHECK(s.call() == A840_KMD_RANGE); }
TEST(xr_rejects_compression) {
    for (uint32_t format : {552u,553u,554u,555u}) {
        Surface s; s.layout.format = format; s.create();
        CHECK(s.call() == A840_KMD_UNSUPPORTED && s.out.fallback_linear_required && !s.out.layout_valid && !s.out.meta.flags);
    }
}
TEST(unknown_pixel_format) { Surface s; s.layout.format = UINT32_MAX; s.create(); CHECK(s.call() == A840_KMD_UNSUPPORTED); }
TEST(render_only_never_certifies_scanout) {
    Surface s; s.create(); s.req.flags = 1; CHECK(s.call(0) == ok && !s.out.meta.flags && !s.out.fallback_linear_required);
}
TEST(import_get_local_handle) {
    Surface s; s.create(); CHECK(s.call() == ok); s.imported(); s.req = {}; s.req.bo_handle = 22;
    CHECK(s.call(1, 2) == ok && s.out.meta.bo_handle == 22 && s.out.meta.highest_bank_bit == 16);
}
TEST(import_get_rechecks_display) {
    Surface s; s.create(); CHECK(s.call() == ok); s.imported(); s.req.op = 0; s.req.bo_handle = 22; s.caps.flags = 0;
    CHECK(s.call(1, 2) == ok && s.out.fallback_linear_required && !s.out.meta.flags);
}
TEST(import_set_denied) { Surface s; s.create(); s.imported(); s.req.bo_handle = 22; CHECK(s.call(1, 2) == A840_KMD_PERMISSION_DENIED); }
TEST(guessing_owner_handle_fails) { Surface s; s.create(); CHECK(s.call() == ok); s.req.op = 0; CHECK(s.call(1, 2) == A840_KMD_NOT_FOUND); }
TEST(get_before_first_set) { Surface s; s.create(); s.req.op = 0; CHECK(s.call() == A840_KMD_NOT_FOUND); }
TEST(creator_close_keeps_import) {
    Surface s; s.create(); CHECK(s.call() == ok); s.imported(); a840_kmd_surface_close_client(&s.m, 1);
    s.req.op = 0; CHECK(s.call() == A840_KMD_NOT_FOUND); s.req.bo_handle = 22; CHECK(s.call(1, 2) == ok);
    a840_kmd_surface_release(&s.m, 2, 22); CHECK(a840_kmd_surface_import(&s.m, 3, 33, 101) == A840_KMD_NOT_FOUND);
}
TEST(destroy_invalidates_all_aliases) {
    Surface s; s.create(); s.imported(); a840_kmd_surface_destroy_bo(&s.m, 101); s.create();
    s.req.op = 0; CHECK(s.call() == A840_KMD_NOT_FOUND); s.req.bo_handle = 22; CHECK(s.call(1, 2) == A840_KMD_NOT_FOUND);
}
TEST(local_handle_collision_isolated) {
    Surface s; s.create(); CHECK(s.call() == ok);
    CHECK(a840_kmd_surface_create(&s.m, 2, 11, 102, &s.layout) == ok);
    s.req.op = 0; CHECK(s.call(1, 2) == A840_KMD_NOT_FOUND);
}
TEST(duplicate_owner_identity) { Surface s; s.create(); CHECK(a840_kmd_surface_create(&s.m, 2, 12, 101, &s.layout) == A840_KMD_STATE_CONFLICT); }
TEST(surface_capacity_reclaimed) {
    Surface s;
    for (uint64_t i = 1; i <= A840_ABI_V3_SURFACE_SLOTS; ++i)
        CHECK(a840_kmd_surface_create(&s.m, 1, i, i, &s.layout) == ok);
    CHECK(a840_kmd_surface_create(&s.m, 1, 999, 999, &s.layout) == A840_KMD_CAPACITY);
    a840_kmd_surface_close_client(&s.m, 1); s.create();
}
TEST(import_capacity_reclaimed) {
    Surface s; s.create();
    for (uint64_t i = 1; i < A840_SURFACE_BINDINGS; ++i)
        CHECK(a840_kmd_surface_import(&s.m, 2, i, 101) == ok);
    CHECK(a840_kmd_surface_import(&s.m, 3, 1, 101) == A840_KMD_CAPACITY);
    a840_kmd_surface_close_client(&s.m, 2); s.imported();
}
/* Linux loader/VM fixture: sealed immutable file, whole-file kernel SHA-256,
 * one resident view, distinct PROT_READ / MAP_SHARED client CPU views. GPU
 * addresses model a per-client SMMU. Check the mapping table before unwire. */
struct Source {
    A840KmdPackIdentity identity{};
    int fd, retains = 0;
    Source(uint64_t key, const char *prefix, size_t bytes = 3) {
        int writer = memfd_create("tier0-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        CHECK(writer >= 0 && ftruncate(writer, static_cast<off_t>(bytes)) == 0);
        size_t n = std::strlen(prefix);
        CHECK(n <= bytes && pwrite(writer, prefix, n, 0) == static_cast<ssize_t>(n));
        CHECK(fcntl(writer, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0);
        fd = open(("/proc/self/fd/" + std::to_string(writer)).c_str(), O_RDONLY | O_CLOEXEC);
        CHECK(fd >= 0 && close(writer) == 0);
        identity.actual_stack_key = key;
        identity.bytes = bytes;
        int alg = socket(AF_ALG, SOCK_SEQPACKET, 0);
        sockaddr_alg addr{};
        addr.salg_family = AF_ALG;
        std::memcpy(addr.salg_type, "hash", 5);
        std::memcpy(addr.salg_name, "sha256", 7);
        CHECK(alg >= 0 && bind(alg, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        int hash = accept(alg, nullptr, nullptr);
        CHECK(hash >= 0);
        char chunk[16384];
        for (size_t pos = 0; pos < bytes;) {
            ssize_t count = pread(fd, chunk, sizeof(chunk), static_cast<off_t>(pos));
            CHECK(count > 0 && send(hash, chunk, static_cast<size_t>(count),
                pos + static_cast<size_t>(count) < bytes ? MSG_MORE : 0) == count);
            pos += static_cast<size_t>(count);
        }
        CHECK(read(hash, identity.sha256, sizeof(identity.sha256)) == sizeof(identity.sha256));
        CHECK(close(hash) == 0 && close(alg) == 0);
    }
    Source(const Source&) = delete;
    ~Source() { CHECK(retains == 0 && close(fd) == 0); }
};

struct Host {
    enum Fault { none, acquire_error, wire_error, bind_error, revoke_error,
        bad_gpu, bad_cpu, unexpected_cpu, unstable_gpu };
    std::mutex mutex;
    Fault fault = none;
    unsigned wires = 0, unwires = 0, binds = 0, revokes = 0;
    struct Mapping { Source *source; void *cpu; uint32_t permissions; };
    std::map<uint64_t, Mapping> mappings;
    void *resident = nullptr;
    static Host& host(void *p) { return *static_cast<Host*>(p); }
    static A840KmdStatus acquire(void *p, void *src, A840KmdPackImage *out) {
        if (host(p).fault == acquire_error) return A840_KMD_IO_ERROR;
        auto &s = *static_cast<Source*>(src);
        ++s.retains;
        *out = {src, s.identity};
        return ok;
    }
    static void release(void *, void *src) {
        CHECK(--static_cast<Source*>(src)->retains >= 0);
    }
    static A840KmdStatus wire(void *p, const A840KmdPackImage *img, uint32_t perm, void **out) {
        Host &h = host(p);
        CHECK(perm == ro && !h.resident && h.wires == h.unwires);
        if (h.fault == wire_error) return A840_KMD_NO_MEMORY;
        auto *s = static_cast<Source*>(img->source);
        CHECK(s->retains && img->identity.bytes == s->identity.bytes);
        h.resident = mmap(nullptr, img->identity.bytes, PROT_READ, MAP_SHARED, s->fd, 0);
        CHECK(h.resident != MAP_FAILED && mlock(h.resident, img->identity.bytes) == 0);
        ++h.wires; *out = s;
        return ok;
    }
    static void unwire(void *p, void *backing) {
        Host &h = host(p);
        auto *s = static_cast<Source*>(backing);
        CHECK(s->retains && h.resident && h.mappings.empty());
        CHECK(munlock(h.resident, s->identity.bytes) == 0 && munmap(h.resident, s->identity.bytes) == 0);
        h.resident = nullptr; ++h.unwires;
    }
    static A840KmdStatus bind_views(void *p, void *backing, uint64_t id, uint32_t perm,
        uint32_t cpu, A840KmdPackViews *out) {
        Host &h = host(p);
        CHECK(perm == ro && h.resident);
        if (h.fault == bind_error) return A840_KMD_IO_ERROR;
        auto *s = static_cast<Source*>(backing);
        auto &view = h.mappings[id];
        CHECK(!view.source || view.source == s);
        view.source = s; view.permissions = perm;
        if (cpu && !view.cpu) {
            view.cpu = mmap(nullptr, s->identity.bytes, PROT_READ, MAP_SHARED, s->fd, 0);
            CHECK(view.cpu != MAP_FAILED);
        }
        ++h.binds;
        out->gpu_va = (id + 1) * A840_PACK_MAX_BYTES;
        out->cpu_va = reinterpret_cast<uintptr_t>(view.cpu);
        if (h.fault == bad_gpu) out->gpu_va = A840_KMD_GPU_VA_LIMIT;
        if (h.fault == bad_cpu) ++out->cpu_va;
        if (h.fault == unexpected_cpu) out->cpu_va = page;
        if (h.fault == unstable_gpu) out->gpu_va += page;
        return ok;
    }
    static A840KmdStatus revoke(void *p, void *backing, uint64_t id) {
        Host &h = host(p);
        auto it = h.mappings.find(id);
        CHECK(it != h.mappings.end() && it->second.source == backing);
        if (h.fault == revoke_error) return A840_KMD_IO_ERROR;
        if (it->second.cpu)
            CHECK(munmap(it->second.cpu, it->second.source->identity.bytes) == 0);
        h.mappings.erase(it); ++h.revokes;
        return ok;
    }
    A840KmdPackMapOps ops() {
        return {this, [](void *p){ host(p).mutex.lock(); }, [](void *p){ host(p).mutex.unlock(); },
            acquire, release, wire, unwire, bind_views, revoke};
    }
};

struct Pack {
    Source a{101, "abc"}, b{202, "xyz"};
    Host h;
    A840KmdPackManager m{};
    NativeAdrenoAbiSharedPackReplyV3 out{};
    Pack() { auto ops = h.ops(); CHECK(a840_kmd_pack_init(&m, &ops) == ok); }
    ~Pack() {
        CHECK(a840_kmd_pack_destroy(&m) == ok);
        CHECK(!h.resident && h.wires == h.unwires && h.mappings.empty());
    }
    void open(uint64_t id = 1) { CHECK(a840_kmd_pack_open_client(&m, id) == ok); }
    void publish(Source &s) { CHECK(a840_kmd_pack_publish(&m, &s) == ok); }
    void start() { publish(a); open(); }
    A840KmdStatus map(uint64_t id = 1, uint32_t cpu = 0, uint64_t key = 101, uint32_t perm = ro) {
        NativeAdrenoAbiSharedPackReqV3 req{key,cpu,0};
        return a840_kmd_pack_map_shared_pack(&m, id, &req, perm, &out);
    }
    A840KmdPackStats stats(uint64_t id = 1) {
        A840KmdPackStats s{}; CHECK(a840_kmd_pack_stats(&m, id, &s) == ok); return s;
    }
};

TEST(pack_sha_whole_file) {
    Pack p; p.publish(p.a);
    constexpr uint8_t abc[32] = {186,120,22,191,143,1,207,234,65,65,64,222,93,174,34,35,
        176,3,97,163,150,23,122,156,180,16,255,97,242,0,21,173};
    auto info = p.stats().active;
    CHECK(!std::memcmp(info.sha256, abc, sizeof(abc)) && info.bytes == 3 && info.actual_stack_key == 101);
}
TEST(empty_pack_rejected) { Source s(1, "", 0); Pack p; CHECK(a840_kmd_pack_publish(&p.m, &s) == A840_KMD_RANGE && !s.retains); }
TEST(pack_64mib_accepted) {
    Source s(1, "x", A840_PACK_MAX_BYTES); Pack p; p.publish(s); p.open();
    CHECK(p.map(1,1,1) == ok && p.out.bytes == A840_PACK_MAX_BYTES);
    auto bytes = reinterpret_cast<const unsigned char*>(p.out.cpu_va);
    CHECK(bytes[0] == 'x' && bytes[p.out.bytes - 1] == 0);
}
TEST(pack_limit_plus_one_rejected) {
    Source s(1,"x", A840_PACK_MAX_BYTES + 1); Pack p; p.start();
    CHECK(a840_kmd_pack_publish(&p.m, &s) == A840_KMD_RANGE && p.stats().active.actual_stack_key == 101);
}
TEST(zero_stack_key_rejected) { Source s(0,"abc"); Pack p; CHECK(a840_kmd_pack_publish(&p.m, &s) == A840_KMD_INVALID_ARG); }
TEST(no_published_pack) { Pack p; p.open(); CHECK(p.map() == A840_KMD_NOT_FOUND && !p.out.gpu_va); }
TEST(closed_client_cannot_map) { Pack p; p.start(); CHECK(a840_kmd_pack_close_client(&p.m, 1) == ok && p.map() == A840_KMD_DISCONNECTED); }
TEST(key_mismatch_only_key) {
    Pack p; p.start(); CHECK(p.map(1,1) == ok && p.out.cpu_va);
    CHECK(p.map(1,1,999) == A840_KMD_STACK_KEY_MISMATCH);
    CHECK(p.out.actual_stack_key == 101 && !p.out.bytes && !p.out.gpu_va && !p.out.cpu_va && p.stats().total_refs == 1);
}
TEST(mismatch_before_first_wire) { Pack p; p.start(); CHECK(p.map(1,0,0) == A840_KMD_STACK_KEY_MISMATCH && !p.h.wires); }
TEST(write_or_unknown_permissions_rejected) {
    Pack p; p.start();
    for (uint32_t perm : {0u, 2u, 3u, 5u, UINT32_MAX}) {
        CHECK(p.map(1,1,101,perm) == A840_KMD_PERMISSION_DENIED);
        CHECK(!p.out.gpu_va && !p.out.cpu_va && !p.h.binds && !p.h.wires);
    }
}
TEST(cpu_view_read_only_at_os) {
    Pack p; p.start(); CHECK(p.map(1,1) == ok);
    auto addr = reinterpret_cast<void*>(p.out.cpu_va);
    CHECK(!std::memcmp(addr, "abc", 3));
    errno = 0;
    CHECK(mprotect(addr, page, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
    CHECK(p.h.mappings.at(1).permissions == ro);
}
TEST(repeated_map_counts_without_rebinding) {
    Pack p; p.start(); CHECK(p.map() == ok); auto gpu = p.out.gpu_va;
    CHECK(p.map() == ok && p.out.gpu_va == gpu && !p.out.cpu_va && p.h.binds == 1);
    CHECK(p.stats().client_refs == 2 && p.stats().total_refs == 2 && p.stats().bound_clients == 1);
}
TEST(cpu_upgrade_keeps_gpu) {
    Pack p; p.start(); CHECK(p.map() == ok); auto gpu = p.out.gpu_va;
    CHECK(p.map(1,1) == ok && p.out.cpu_va && p.out.gpu_va == gpu);
    CHECK(p.map() == ok && !p.out.cpu_va && p.h.binds == 2 && p.h.wires == 1);
}
TEST(shared_wire_distinct_clients) {
    Pack p; p.start(); p.open(2); CHECK(p.map(1,1) == ok);
    auto a = p.out; CHECK(p.map(2,1) == ok);
    CHECK(p.out.cpu_va != a.cpu_va && p.stats().backing_wire_count == 1 && p.h.wires == 1);
    CHECK(p.h.mappings.at(1).source == p.h.mappings.at(2).source && p.stats().bound_clients == 2);
}
TEST(unmap_last_ref_only) {
    Pack p; p.start(); CHECK(p.map() == ok && p.map() == ok);
    CHECK(a840_kmd_pack_unmap(&p.m, 1) == ok && !p.h.revokes);
    CHECK(a840_kmd_pack_unmap(&p.m, 1) == ok && p.h.revokes == 1 && p.stats().backing_wire_count == 1);
    CHECK(a840_kmd_pack_unmap(&p.m, 1) == A840_KMD_NOT_FOUND);
}
TEST(close_drops_all_client_refs) {
    Pack p; p.start(); p.open(2); CHECK(p.map() == ok && p.map(1,1) == ok && p.map(2) == ok);
    CHECK(a840_kmd_pack_close_client(&p.m, 1) == ok && p.stats(2).total_refs == 1 && p.stats(2).client_refs == 1);
}
TEST(hot_swap_drains_old_clients) {
    Pack p; p.start(); p.open(2); CHECK(p.map(1,1) == ok); auto old = p.out.cpu_va;
    CHECK(p.map(2) == ok); p.publish(p.b);
    CHECK(p.stats().replacement_pending && p.stats().active.actual_stack_key == 101 && !p.h.unwires);
    CHECK(p.map(1) == A840_KMD_STATE_CONFLICT && !p.out.gpu_va);
    CHECK(a840_kmd_pack_close_client(&p.m, 2) == ok && !std::memcmp(reinterpret_cast<void*>(old), "abc", 3));
    CHECK(a840_kmd_pack_unmap(&p.m, 1) == ok && p.h.unwires == 1 && !p.a.retains);
    CHECK(p.map(1,1,202) == ok && !std::memcmp(reinterpret_cast<void*>(p.out.cpu_va), "xyz", 3));
}
TEST(same_key_new_digest_replaces) {
    Source revised(101,"abd"); Pack p; p.start(); CHECK(p.map() == ok); p.publish(revised);
    CHECK(p.stats().replacement_pending && a840_kmd_pack_unmap(&p.m, 1) == ok);
    CHECK(p.map(1,1) == ok && !std::memcmp(reinterpret_cast<void*>(p.out.cpu_va), "abd", 3));
}
TEST(publish_duplicate_is_idempotent) {
    Pack p; p.start(); CHECK(p.map() == ok); p.publish(p.a);
    CHECK(!p.stats().replacement_pending && p.a.retains == 1 && p.map() == ok && p.h.wires == 1);
}
TEST(pending_image_not_overwritten) {
    Pack p; p.start(); CHECK(p.map() == ok); p.publish(p.b); p.publish(p.b);
    CHECK(a840_kmd_pack_publish(&p.m, &p.a) == A840_KMD_STATE_CONFLICT);
    CHECK(p.a.retains == 1 && p.b.retains == 1);
}
TEST(wire_failure_retry) {
    Pack p; p.start(); p.h.fault = Host::wire_error; CHECK(p.map() == A840_KMD_NO_MEMORY && !p.out.gpu_va && !p.stats().total_refs);
    p.h.fault = Host::none; CHECK(p.map() == ok && p.h.wires == 1);
}
TEST(bind_failure_preserves_gpu_only_map) {
    Pack p; p.start(); CHECK(p.map() == ok); p.h.fault = Host::bind_error;
    CHECK(p.map(1,1) == A840_KMD_IO_ERROR && !p.out.gpu_va && p.stats().client_refs == 1);
    p.h.fault = Host::none; CHECK(p.map(1,1) == ok);
}
TEST(acquire_failure_preserves_active) {
    Pack p; p.start(); p.h.fault = Host::acquire_error;
    CHECK(a840_kmd_pack_publish(&p.m,&p.b) == A840_KMD_IO_ERROR && !p.b.retains && p.map() == ok);
}
TEST(failed_close_holds_old_wire) {
    Pack p; p.start(); CHECK(p.map(1,1) == ok); auto old = p.out.cpu_va; p.publish(p.b);
    p.h.fault = Host::revoke_error;
    CHECK(a840_kmd_pack_close_client(&p.m, 1) == A840_KMD_IO_ERROR && !p.h.unwires && p.stats().total_refs == 1);
    CHECK(p.map() == A840_KMD_DISCONNECTED && !std::memcmp(reinterpret_cast<void*>(old), "abc", 3));
    p.h.fault = Host::none; CHECK(a840_kmd_pack_close_client(&p.m, 1) == ok && p.stats().active.actual_stack_key == 202);
}
TEST(failed_destroy_can_retry) {
    Pack p; p.start(); CHECK(p.map() == ok); p.h.fault = Host::revoke_error;
    CHECK(a840_kmd_pack_destroy(&p.m) == A840_KMD_IO_ERROR && p.stats().backing_wire_count == 1 && p.a.retains);
    p.h.fault = Host::none; CHECK(a840_kmd_pack_destroy(&p.m) == ok && !p.a.retains);
}
TEST(malformed_success_revoked) {
    for (auto f : {Host::bad_gpu, Host::bad_cpu, Host::unexpected_cpu}) {
        Pack p; p.start(); p.h.fault = f;
        CHECK(p.map(1, f == Host::bad_cpu) == A840_KMD_IO_ERROR);
        CHECK(!p.out.gpu_va && !p.out.cpu_va && !p.stats().bound_clients && p.h.revokes == 1);
    }
}
TEST(unstable_gpu_upgrade_quarantined) {
    Pack p; p.start(); CHECK(p.map() == ok); p.h.fault = Host::unstable_gpu;
    CHECK(p.map(1,1) == A840_KMD_IO_ERROR && p.map() == A840_KMD_DISCONNECTED);
    CHECK(p.stats().client_refs == 1 && !p.h.revokes); p.publish(p.b);
    CHECK(p.stats().replacement_pending && !p.h.unwires);
    CHECK(a840_kmd_pack_unmap(&p.m,1) == ok && p.h.revokes == 1 && p.h.unwires == 1);
}
TEST(invalid_request_no_mapping) {
    Pack p; p.start(); CHECK(p.map(1,2) == A840_KMD_INVALID_ARG);
    NativeAdrenoAbiSharedPackReqV3 req{101,0,1};
    CHECK(a840_kmd_pack_map_shared_pack(&p.m,1,&req,ro,&p.out) == A840_KMD_INVALID_ARG && !p.h.wires);
}
TEST(pack_client_capacity_reclaimed) {
    Pack p;
    for (unsigned id = 1; id <= A840_PACK_MAX_CLIENTS; ++id) p.open(id);
    CHECK(a840_kmd_pack_open_client(&p.m,1) == A840_KMD_STATE_CONFLICT);
    CHECK(a840_kmd_pack_open_client(&p.m,999) == A840_KMD_CAPACITY);
    CHECK(a840_kmd_pack_close_client(&p.m,1) == ok); p.open(999);
}
struct Warm {
    unsigned units = 0;
    A840KmdStatus status = ok;
    static A840KmdStatus unit(void *p, uint32_t *done) {
        auto &job = *static_cast<Warm*>(p);
        *done = ++job.units == 2;
        return job.status;
    }
    A840KmdStatus step(Pack &p) { return a840_kmd_pack_warm_step(&p.m, unit, this); }
};
TEST(no_work_before_boot_complete) {
    Pack p; Warm w; CHECK(p.stats().warm == A840_WARM_IDLE && w.step(p) == A840_KMD_STATE_CONFLICT && !w.units);
}
TEST(boot_arms_then_warms_to_ready) {
    Pack p; Warm w; p.start(); a840_kmd_pack_boot_complete(&p.m);
    CHECK(p.stats().warm == A840_WARM_ARMED && w.step(p) == ok && p.stats().warm == A840_WARM_WARMING);
    CHECK(w.step(p) == ok && p.stats().warm == A840_WARM_READY);
    a840_kmd_pack_boot_complete(&p.m); CHECK(w.step(p) == A840_KMD_STATE_CONFLICT && w.units == 2);
}
TEST(pressure_before_boot_blocks_first_unit) {
    Pack p; Warm w; a840_kmd_pack_set_params(&p.m, NativeAdrenoParamsV2MemoryPressure);
    a840_kmd_pack_boot_complete(&p.m); CHECK(p.stats().warm == A840_WARM_PAUSED_MEMORY_PRESSURE);
    CHECK(w.step(p) == A840_KMD_STATE_CONFLICT && !w.units);
    a840_kmd_pack_set_params(&p.m, 0); CHECK(p.stats().warm == A840_WARM_ARMED && w.step(p) == ok);
}
TEST(pressure_pauses_and_resumes_progress) {
    Pack p; Warm w; a840_kmd_pack_boot_complete(&p.m); CHECK(w.step(p) == ok);
    a840_kmd_pack_set_params(&p.m, NativeAdrenoParamsV2MemoryPressure | NativeAdrenoParamsV3SharedTier0Pack);
    CHECK(p.stats().warm == A840_WARM_PAUSED_MEMORY_PRESSURE && w.step(p) == A840_KMD_STATE_CONFLICT && w.units == 1);
    a840_kmd_pack_set_params(&p.m, NativeAdrenoParamsV3SharedTier0Pack);
    CHECK(p.stats().warm == A840_WARM_WARMING && w.step(p) == ok && p.stats().warm == A840_WARM_READY);
}
TEST(warm_failure_can_retry) {
    Pack p; Warm w; a840_kmd_pack_boot_complete(&p.m); w.status = A840_KMD_IO_ERROR;
    CHECK(w.step(p) == A840_KMD_IO_ERROR && p.stats().warm == A840_WARM_ARMED);
    w.status = ok; CHECK(w.step(p) == ok);
}
TEST(warm_generation_change_under_pressure) {
    Pack p; Warm w; p.start(); a840_kmd_pack_boot_complete(&p.m);
    CHECK(w.step(p) == ok && w.step(p) == ok); a840_kmd_pack_set_params(&p.m, NativeAdrenoParamsV2MemoryPressure);
    CHECK(p.stats().warm == A840_WARM_READY); p.publish(p.b);
    CHECK(p.stats().warm == A840_WARM_PAUSED_MEMORY_PRESSURE && w.step(p) == A840_KMD_STATE_CONFLICT);
}

class Barrier {
    std::mutex mutex;
    std::condition_variable changed;
    const unsigned parties;
    unsigned remaining, generation = 0;
public:
    explicit Barrier(unsigned n) : parties(n), remaining(n) {}
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        const auto current = generation;
        if (--remaining == 0) {
            remaining = parties; ++generation; changed.notify_all();
        } else {
            changed.wait(lock, [&]{ return current != generation; });
        }
    }
};

/* Owner, importer and client-close paths deliberately race on the SAME BO.
 * Creating/releasing lifecycle references is a trusted adapter operation. */
void surface_churn(Surface &s, unsigned role)
{
    for (unsigned n = 0; n < 256; ++n) {
        auto req = s.req;
        A840KmdSurfaceResult result{};
        if (role == 0) {
            req.meta_pitch = n % 2 ? 64 : 128;
            auto rc = a840_kmd_surface_meta(&s.m, 1, &req, 1, &s.caps, &result);
            CHECK(rc == ok || rc == A840_KMD_NOT_FOUND);
        } else if (role == 1) {
            a840_kmd_surface_close_client(&s.m, 2);
            auto rc = a840_kmd_surface_import(&s.m, 2, 22, 101);
            CHECK(rc == ok || rc == A840_KMD_NOT_FOUND);
        } else if (role == 2) {
            a840_kmd_surface_destroy_bo(&s.m, 101);
            s.create();
        } else {
            req.op = 0; req.bo_handle = 22;
            auto rc = a840_kmd_surface_meta(&s.m, 2, &req, 1, &s.caps, &result);
            CHECK(rc == A840_KMD_NOT_FOUND || (rc == ok && result.meta.bo_handle == 22 &&
                result.layout_valid && result.scanout_eligible && result.meta.highest_bank_bit == 16 &&
                (result.meta.meta_pitch == 64 || result.meta.meta_pitch == 128)));
        }
    }
}

void stress_round()
{
    constexpr unsigned clients = 8, swaps = 12;
    Pack p;
    Surface s;
    s.create(); p.publish(p.a);
    Barrier phases(clients + 1);
    std::vector<std::thread> workers;
    for (unsigned id = 1; id <= clients; ++id) {
        workers.emplace_back([&, id] {
            p.open(id);
            for (unsigned n = 0; n < swaps; ++n) {
                uint64_t key = n % 2 ? 202 : 101;
                NativeAdrenoAbiSharedPackReqV3 req{key,1,0};
                NativeAdrenoAbiSharedPackReplyV3 out{};
                CHECK(a840_kmd_pack_map_shared_pack(&p.m,id,&req,ro,&out) == ok);
                auto ptr = reinterpret_cast<const char*>(out.cpu_va);
                CHECK(out.actual_stack_key == key && out.bytes == 3);
                phases.wait(); // Every client now holds a lease on the old mapping.
                req.map_cpu = 0;
                auto rc = a840_kmd_pack_map_shared_pack(&p.m,id,&req,ro,&out);
                CHECK(rc == ok || rc == A840_KMD_STATE_CONFLICT);
                if (rc == ok) CHECK(a840_kmd_pack_unmap(&p.m,id) == ok);
                phases.wait(); // New image has been accepted, source still pinned.
                CHECK(!std::memcmp(ptr, n % 2 ? "xyz" : "abc", 3));
                CHECK(a840_kmd_pack_map_shared_pack(&p.m,id,&req,ro,&out) == A840_KMD_STATE_CONFLICT);
                CHECK(!out.gpu_va && !out.cpu_va);
                if (id % 2) {
                    CHECK(a840_kmd_pack_close_client(&p.m,id) == ok);
                    p.open(id);
                } else {
                    CHECK(a840_kmd_pack_unmap(&p.m,id) == ok);
                }
                phases.wait();
            }
            CHECK(a840_kmd_pack_close_client(&p.m,id) == ok);
        });
    }
    for (unsigned role = 0; role < 5; ++role)
        workers.emplace_back([&, role]{ surface_churn(s, role); });
    for (unsigned n = 0; n < swaps; ++n) {
        phases.wait();
        auto before = p.stats();
        CHECK(before.backing_wire_count == 1 && before.bound_clients == clients && before.total_refs >= clients);
        p.publish(n % 2 ? p.a : p.b);
        CHECK(p.stats().replacement_pending);
        phases.wait();
        phases.wait();
    }
    for (auto &worker : workers) worker.join();
    auto after = p.stats();
    CHECK(!after.replacement_pending && !after.bound_clients && !after.total_refs && !after.backing_wire_count);
    CHECK(p.h.wires == swaps && p.h.unwires == swaps && p.h.binds == swaps * clients);
    CHECK(p.h.revokes == swaps * clients);
    a840_kmd_surface_destroy_bo(&s.m, 101);
    CHECK(a840_kmd_surface_import(&s.m, 2, 22, 101) == A840_KMD_NOT_FOUND);
}
} // namespace

int main(int argc, char **argv)
{
    long rounds = 1;
    if (argc != 1) {
        if (argc != 3 || std::strcmp(argv[1], "--tsan-stress") != 0) return 2;
        char *end = nullptr;
        rounds = std::strtol(argv[2], &end, 10);
        if (*end || rounds <= 0 || rounds > 10000) return 2;
    }
    for (const auto &test : cases()) test.run();
    for (long i = 0; i < rounds; ++i) stress_round();
    std::printf("PASS phase3_ubwc_pack: %zu independent cases, %ld stress rounds, "
        "8 clients and 12 hot replacements per round\n", cases().size(), rounds);
    return 0;
}
