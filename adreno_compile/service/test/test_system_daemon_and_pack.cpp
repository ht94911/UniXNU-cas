/* SPDX-License-Identifier: MIT */
/* Wave 12 / Lane 2: End-to-End Test Suite (>= 35 cases + --tsan-stress 16)
 * for:
 *   - a840_system_pack_builder (6-domain >= 36 real system shaders, XR10 100%,
 *     valid instrlen*8, 0 unbound UBO block1, byte-identical SHA-256, mmap P99 < 2 us)
 *   - a840_system_cache_store (L0 read-only pack.bin < 1 us -> L1 backboardd
 *     shared R/W / App RO-shared + private R/W, atomic rename, CRC32+SHA-256+ABI
 *     isolation, corruption/symlink self-healing, LRU eviction cap)
 *   - NezhaShaderCompiler AF_UNIX daemon (P0_URGENT preemption over P2_PREWARM,
 *     multi-client backboardd + 4 Apps, fault isolation, graceful SIGTERM/shutdown,
 *     and 16x TSan multi-threaded stress).
 */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif

#include "../a840_shader_compiler_client.h"
#include "../a840_shader_compiler_ir3_backend.h"
#include "../a840_shader_compiler_service.h"
#include "../a840_system_cache_store.h"
#include "../../pack/a840_system_pack_builder.h"

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

static std::atomic<uint64_t> g_checks{0u};
static uint32_t g_cases = 0u;
static const char *g_active_case = "init";

static constexpr uint64_t kStackKey = UINT64_C(0x840c24a446120001);
static constexpr uint64_t kAltStackKey = UINT64_C(0x840c24a446120002);

static void begin_case(const char *name)
{
    g_active_case = name;
    ++g_cases;
}

static void check_cond(bool ok, const char *expr, int line, const char *detail = nullptr)
{
    g_checks.fetch_add(1u, std::memory_order_relaxed);
    if (!ok) {
        std::fprintf(stderr, "FAIL [%s] line %d: %s%s%s\n",
                     g_active_case, line, expr,
                     (detail && detail[0]) ? " :: " : "",
                     (detail && detail[0]) ? detail : "");
        std::abort();
    }
}

#define CASE(name) begin_case(name)
#define CHECK(expr) check_cond((expr), #expr, __LINE__)
#define CHECK_MSG(expr, msg) check_cond((expr), #expr, __LINE__, (msg))

/* Minimal valid SPIR-V 1.0 compute shaders for live AF_UNIX compile tests */
static const uint32_t kLiveComputeSpvA[] = {
    0x07230203u, 0x00010000u, 0x0008000au, 0x00000006u, 0x00000000u,
    0x00020011u, 0x00000001u,
    0x0003000eu, 0x00000000u, 0x00000001u,
    0x0005000fu, 0x00000005u, 0x00000004u, 0x6e69616du, 0x00000000u,
    0x00060010u, 0x00000004u, 0x00000011u, 0x00000001u, 0x00000001u, 0x00000001u,
    0x00020013u, 0x00000002u,
    0x00030021u, 0x00000003u, 0x00000002u,
    0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
    0x000200f8u, 0x00000005u,
    0x000100fdu,
    0x00010038u
};

static const uint32_t kLiveComputeSpvB[] = {
    0x07230203u, 0x00010000u, 0x0008000au, 0x00000006u, 0x00000000u,
    0x00020011u, 0x00000001u,
    0x0003000eu, 0x00000000u, 0x00000001u,
    0x0006000fu, 0x00000005u, 0x00000004u, 0x745f7363u, 0x00003277u, 0x00000000u,
    0x00060010u, 0x00000004u, 0x00000011u, 0x00000008u, 0x00000001u, 0x00000001u,
    0x00020013u, 0x00000002u,
    0x00030021u, 0x00000003u, 0x00000002u,
    0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
    0x000200f8u, 0x00000005u,
    0x000100fdu,
    0x00010038u
};

static std::string key_to_filename(const A840PipelineKey &key)
{
    static const char hex[] = "0123456789abcdef";
    std::string s;
    s.reserve(71u);
    for (size_t i = 0u; i < 32u; ++i) {
        s.push_back(hex[(key.sha256[i] >> 4) & 0x0fu]);
        s.push_back(hex[key.sha256[i] & 0x0fu]);
    }
    s += ".a840pr";
    return s;
}

static bool mkdir_recursive(const std::string &path)
{
    std::string cur;
    for (size_t i = 0u; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' && cur.size() > 1u) {
            if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
                return false;
        }
    }
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
        return false;
    return true;
}

static void rm_rf(const std::string &path)
{
    struct stat st;
    if (lstat(path.c_str(), &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        DIR *dp = opendir(path.c_str());
        if (dp) {
            struct dirent *e = nullptr;
            while ((e = readdir(dp)) != nullptr) {
                if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
                    continue;
                rm_rf(path + "/" + e->d_name);
            }
            closedir(dp);
        }
        (void)rmdir(path.c_str());
    } else {
        (void)unlink(path.c_str());
    }
}

static bool file_exists(const std::string &path)
{
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}

static uint64_t now_ns()
{
    struct timespec ts;
    std::memset(&ts, 0, sizeof(ts));
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * UINT64_C(1000000000) +
           static_cast<uint64_t>(ts.tv_nsec);
}

static void sleep_ms(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = static_cast<time_t>(ms / 1000u);
    ts.tv_nsec = static_cast<long>((ms % 1000u) * 1000000u);
    (void)nanosleep(&ts, nullptr);
}

/* When compiled inside verify_wave.py (without real Mesa ir3 archive linked),
 * provide a deterministic test-only stage compiler hook so all 38 tests run
 * under verify_wave GCC/Clang ASan/UBSan. When A840_TEST_WITH_REAL_IR3 is
 * defined (hard gate #1), we strictly use NULL (real Mesa a840_compile_stage).
 */
#ifndef A840_TEST_WITH_REAL_IR3
struct TestOnlyStageOwner {
    uint32_t isa[32];
};

static A840CompileStatus test_only_host_stage_compile(
    void *user_ctx,
    const A840CompileTarget *target,
    const A840StageInput *input,
    A840CompiledStage *out_stage,
    char *err_buf,
    size_t err_buf_bytes)
{
    (void)user_ctx;
    (void)err_buf;
    (void)err_buf_bytes;
    if (!target || !input || !out_stage || !input->spirv_words || input->spirv_bytes < 20u)
        return A840_COMPILE_INVALID_ARG;
    auto *owner = static_cast<TestOnlyStageOwner *>(std::calloc(1u, sizeof(TestOnlyStageOwner)));
    if (!owner)
        return A840_COMPILE_OOM;

    uint8_t digest[32];
    (void)a840_record_sha256(input->spirv_words, input->spirv_bytes, digest);
    for (uint32_t i = 0u; i < 32u; ++i) {
        uint32_t b = digest[i % 32u];
        owner->isa[i] = 0x03000000u ^ (b << 8) ^ (input->stage << 20) ^ i;
    }

    std::memset(out_stage, 0, sizeof(*out_stage));
    out_stage->abi_version = ADRENO_COMPILE_ABI_VERSION;
    out_stage->stage = input->stage;
    out_stage->isa_words = owner->isa;
    out_stage->isa_dwords = 32u;
    out_stage->instruction_groups = 8u; /* instrlen=1 -> 1*8 = 8 groups */
    out_stage->max_full_reg = 4;
    out_stage->max_half_reg = (input->half_rt_mask != 0u) ? 4 : -1;
    out_stage->branchstack = 0u;
    out_stage->constlen_vec4 = 4u;
    out_stage->local_size[0] = input->local_size[0];
    out_stage->local_size[1] = input->local_size[1];
    out_stage->local_size[2] = input->local_size[2];
    out_stage->flags_out = (1u << 3) | (1u << 4);
    if ((input->flags & A840_COMPILE_FLAG_FBFETCH_GMEM) != 0u &&
        input->stage == A840_STAGE_FRAGMENT)
        out_stage->flags_out |= (1u << 2);
    out_stage->max_waves = 16u;
    out_stage->owner = owner;
    return A840_COMPILE_OK;
}

static void test_only_host_stage_free(void *user_ctx, A840CompiledStage *stage)
{
    (void)user_ctx;
    if (!stage)
        return;
    std::free(stage->owner);
    std::memset(stage, 0, sizeof(*stage));
}
#endif

static A840SystemPackBuildConfig make_pack_build_cfg(uint64_t stack_key,
    uint32_t domain_mask = A840_SYSDOMAIN_ALL_MASK)
{
    A840SystemPackBuildConfig cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.stack_key = stack_key;
    cfg.abi_fingerprint = a840_compile_abi_fingerprint();
    cfg.tier = 0u;
    cfg.domain_mask = domain_mask;
    cfg.attach_sds_blocks = true;
    cfg.verify_after_build = true;
    cfg.bench_iters = 200u;
#ifndef A840_TEST_WITH_REAL_IR3
    cfg.compile_stage_fn = test_only_host_stage_compile;
    cfg.free_stage_fn = test_only_host_stage_free;
#endif
    return cfg;
}

/* Helper to construct a valid A840PR01 blob for synthetic cache/LRU tests */
static std::vector<uint8_t> make_valid_record_blob(uint32_t seed,
    uint64_t stack_key, uint64_t abi_fp, uint32_t isa_dwords,
    A840PipelineKey *out_key)
{
    std::vector<uint32_t> isa(isa_dwords, 0u);
    for (uint32_t i = 0u; i < isa_dwords; ++i)
        isa[i] = 0x10000000u ^ (seed * 2654435761u) ^ i;

    A840RecordKeyStage ks;
    std::memset(&ks, 0, sizeof(ks));
    ks.entry_point = "cs_cache_test";
    ks.stage = A840_STAGE_COMPUTE;
    ks.sample_count = 1u;
    ks.local_size[0] = 8u;
    ks.local_size[1] = 8u;
    ks.local_size[2] = 1u;
    ks.flags = A840_COMPILE_FLAG_EARLY_PREAMBLE;
    CHECK(a840_record_sha256(isa.data(), isa.size() * sizeof(uint32_t), ks.spirv_sha256) == A840_RECORD_OK);

    A840PipelineKey key;
    CHECK(a840_record_compute_key(&ks, 1u, &key) == A840_RECORD_OK);

    A840PipelineRecord rec;
    std::memset(&rec, 0, sizeof(rec));
    rec.abi_fingerprint = abi_fp;
    rec.stack_key = stack_key;
    rec.chip_id = A840_RECORD_CHIP_ID;
    rec.highest_bank_bit = A840_RECORD_HBB;
    rec.stage_count = 1u;
    rec.pipeline_key = key;
    rec.stages[0].abi_version = ADRENO_COMPILE_ABI_VERSION;
    rec.stages[0].stage = A840_STAGE_COMPUTE;
    rec.stages[0].isa_words = isa.data();
    rec.stages[0].isa_dwords = isa_dwords;
    rec.stages[0].instruction_groups = isa_dwords / 4u;
    rec.stages[0].max_full_reg = 4;
    rec.stages[0].max_half_reg = -1;
    rec.stages[0].constlen_vec4 = 2u;
    rec.stages[0].local_size[0] = 8u;
    rec.stages[0].local_size[1] = 8u;
    rec.stages[0].local_size[2] = 1u;
    rec.stages[0].max_waves = 16u;

    uint8_t *blob = nullptr;
    size_t bytes = 0u;
    CHECK(a840_record_serialize(&rec, &blob, &bytes) == A840_RECORD_OK);
    std::vector<uint8_t> out(blob, blob + bytes);
    std::free(blob);
    if (out_key)
        *out_key = key;
    return out;
}

static std::vector<uint8_t> encode_spirv_compute_payload(const uint32_t *spv,
    size_t spv_bytes, const char *entry_name, uint32_t lx, uint32_t ly, uint32_t lz,
    A840PipelineKey *out_key)
{
    A840ScIr3CompilePayload p;
    std::memset(&p, 0, sizeof(p));
    p.magic = A840_SC_IR3_PAYLOAD_MAGIC;
    p.version = A840_SC_IR3_PAYLOAD_VERSION;
    p.pipeline_kind = A840_SC_IR3_PIPELINE_COMPUTE;
    p.source_kind = A840_SC_IR3_SRC_SPIRV;
    p.compile_flags = A840_COMPILE_FLAG_EARLY_PREAMBLE;
    p.sample_count = 1u;
    p.local_size[0] = lx;
    p.local_size[1] = ly;
    p.local_size[2] = lz;
    p.stage0_bytes = static_cast<uint32_t>(spv_bytes);
    std::strncpy(p.entry_name, entry_name, sizeof(p.entry_name) - 1u);

    std::vector<uint8_t> buf(A840_SC_IR3_PAYLOAD_HEADER_BYTES + spv_bytes);
    size_t out_len = 0u;
    CHECK(a840_sc_ir3_payload_encode(&p, spv, nullptr, buf.data(), buf.size(), &out_len) == A840_SC_OK);
    buf.resize(out_len);
    if (out_key)
        CHECK(a840_sc_ir3_payload_key(buf.data(), buf.size(), out_key) == A840_SC_OK);
    return buf;
}

/* ---------------- AF_UNIX Client Transport Helpers ---------------- */

static bool fd_write_all(int fd, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t sent = 0u;
    uint64_t deadline = now_ns() + static_cast<uint64_t>(timeout_ms) * UINT64_C(1000000);
    while (sent < len) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        uint64_t now = now_ns();
        int rem = (now >= deadline) ? 0 : static_cast<int>((deadline - now) / 1000000u) + 1;
        int pr = poll(&pfd, 1, rem);
        if (pr <= 0) {
            if (pr < 0 && errno == EINTR)
                continue;
            return false;
        }
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static int fd_read_exact(int fd, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t got = 0u;
    uint64_t deadline = now_ns() + static_cast<uint64_t>(timeout_ms) * UINT64_C(1000000);
    while (got < len) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        uint64_t now = now_ns();
        int rem = (now >= deadline) ? 0 : static_cast<int>((deadline - now) / 1000000u) + 1;
        int pr = poll(&pfd, 1, rem);
        if (pr == 0)
            return (got == 0u) ? 0 : -1;
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        ssize_t n = read(fd, buf + got, len - got);
        if (n > 0) {
            got += static_cast<size_t>(n);
        } else if (n == 0) {
            return -2;
        } else if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        } else {
            return -2;
        }
    }
    return 1;
}

struct UnixClientTransport {
    int fd;
};

static A840ScStatus unix_send_fn(void *user_ctx, const uint8_t *frame,
    size_t frame_bytes, uint32_t timeout_ms)
{
    auto *t = static_cast<UnixClientTransport *>(user_ctx);
    if (!t || t->fd < 0)
        return A840_SC_DAEMON_UNAVAILABLE;
    return fd_write_all(t->fd, frame, frame_bytes, timeout_ms ? timeout_ms : 2000u)
               ? A840_SC_OK
               : A840_SC_DAEMON_UNAVAILABLE;
}

static A840ScStatus unix_recv_fn(void *user_ctx, uint8_t **out_frame,
    size_t *out_frame_bytes, uint32_t timeout_ms)
{
    auto *t = static_cast<UnixClientTransport *>(user_ctx);
    if (!t || t->fd < 0)
        return A840_SC_DAEMON_UNAVAILABLE;
    uint8_t hdr[A840_SC_HEADER_BYTES];
    int r = fd_read_exact(t->fd, hdr, sizeof(hdr), timeout_ms);
    if (r == 0)
        return A840_SC_TIMED_OUT;
    if (r < 0)
        return A840_SC_DAEMON_UNAVAILABLE;
    size_t total = 0u;
    if (a840_sc_peek_frame_length(hdr, sizeof(hdr), &total) != A840_SC_OK)
        return A840_SC_DAEMON_UNAVAILABLE;
    auto *buf = static_cast<uint8_t *>(std::malloc(total));
    if (!buf)
        return A840_SC_OOM;
    std::memcpy(buf, hdr, sizeof(hdr));
    if (total > A840_SC_HEADER_BYTES) {
        int br = fd_read_exact(t->fd, buf + A840_SC_HEADER_BYTES,
                               total - A840_SC_HEADER_BYTES, 2000u);
        if (br <= 0) {
            std::free(buf);
            return A840_SC_DAEMON_UNAVAILABLE;
        }
    }
    *out_frame = buf;
    *out_frame_bytes = total;
    return A840_SC_OK;
}

static int connect_unix_socket(const std::string &sock_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1u);
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0)
            return fd;
        sleep_ms(10u);
    }
    (void)close(fd);
    return -1;
}

/* ---------------- In-Process AF_UNIX Daemon Harness (ASan/TSan full coverage) ---------------- */

struct TestDaemon {
    std::string socket_path;
    uint64_t stack_key = kStackKey;
    uint64_t abi_fp = 0u;
    A840SystemCacheStore *cache_store = nullptr;
    A840ScIr3Backend *backend = nullptr;
    A840ScService *service = nullptr;
    int listen_fd = -1;
    std::atomic<bool> stop{false};
    std::atomic<uint32_t> delay_ms{0u};
    std::atomic<uint64_t> priority_boost_seen{0u};
    std::atomic<uint64_t> compile_calls{0u};
    pthread_t accept_thread{};
    bool accept_started = false;
    pthread_mutex_t clients_mu = PTHREAD_MUTEX_INITIALIZER;
    std::vector<pthread_t> client_threads;
    std::vector<int> client_fds;
};

struct TestDaemonClientCtx {
    TestDaemon *daemon;
    int fd;
    pthread_mutex_t write_mu;
    pthread_mutex_t state_mu;
    pthread_cond_t state_cv;
    uint32_t pending;
    bool closed;
};

struct TestReplyCtx {
    TestDaemonClientCtx *conn;
};

static A840ScStatus test_daemon_compile_cb(void *user_ctx, uint32_t worker_id,
    const A840ScFrameHeader *request, const uint8_t *payload,
    uint8_t **out_blob, size_t *out_bytes)
{
    auto *d = static_cast<TestDaemon *>(user_ctx);
    d->compile_calls.fetch_add(1u, std::memory_order_relaxed);
    uint32_t dms = d->delay_ms.load(std::memory_order_relaxed);
    if (dms > 0u)
        sleep_ms(dms);

    A840ScStatus st = A840_SC_COMPILE_FAILED;
#ifdef A840_TEST_WITH_REAL_IR3
    st = a840_sc_ir3_compile_callback(d->backend, worker_id, request, payload,
                                      out_blob, out_bytes);
#else
    (void)worker_id;
    A840ScIr3CompilePayload hdr;
    const uint8_t *s0 = nullptr;
    const uint8_t *s1 = nullptr;
    if (a840_sc_ir3_payload_decode(payload, request->payload_bytes, &hdr, &s0, &s1) != A840_SC_OK)
        return A840_SC_COMPILE_FAILED;
    A840PipelineRecord rec;
    std::memset(&rec, 0, sizeof(rec));
    rec.abi_fingerprint = d->abi_fp;
    rec.stack_key = d->stack_key;
    rec.chip_id = A840_RECORD_CHIP_ID;
    rec.highest_bank_bit = A840_RECORD_HBB;
    rec.stage_count = 1u;
    rec.pipeline_key = request->pipeline_key;
    uint32_t isa[32];
    for (uint32_t i = 0u; i < 32u; ++i)
        isa[i] = 0x05000000u ^ static_cast<uint32_t>(request->pipeline_key.sha256[i]) ^ i;
    rec.stages[0].abi_version = ADRENO_COMPILE_ABI_VERSION;
    rec.stages[0].stage = A840_STAGE_COMPUTE;
    rec.stages[0].isa_words = isa;
    rec.stages[0].isa_dwords = 32u;
    rec.stages[0].instruction_groups = 8u;
    rec.stages[0].max_full_reg = 4;
    rec.stages[0].max_half_reg = -1;
    rec.stages[0].constlen_vec4 = 2u;
    rec.stages[0].local_size[0] = hdr.local_size[0] ? hdr.local_size[0] : 1u;
    rec.stages[0].local_size[1] = hdr.local_size[1] ? hdr.local_size[1] : 1u;
    rec.stages[0].local_size[2] = hdr.local_size[2] ? hdr.local_size[2] : 1u;
    rec.stages[0].max_waves = 16u;
    if (a840_record_serialize(&rec, out_blob, out_bytes) == A840_RECORD_OK)
        st = A840_SC_OK;
#endif
    if (st == A840_SC_OK && d->cache_store && *out_blob && *out_bytes > 0u) {
        (void)a840_system_cache_put_blob(d->cache_store, &request->pipeline_key,
                                         *out_blob, *out_bytes);
    }
    return st;
}

static void test_daemon_boost_cb(void *user_ctx, uint32_t worker_id,
    A840ScPriority from_prio, A840ScPriority to_prio)
{
    (void)worker_id;
    (void)from_prio;
    (void)to_prio;
    auto *d = static_cast<TestDaemon *>(user_ctx);
    d->priority_boost_seen.fetch_add(1u, std::memory_order_relaxed);
}

static void test_daemon_reply_cb(void *user_ctx, const uint8_t *frame, size_t len)
{
    auto *rctx = static_cast<TestReplyCtx *>(user_ctx);
    auto *c = rctx->conn;
    pthread_mutex_lock(&c->write_mu);
    if (!c->closed && c->fd >= 0 && frame && len > 0u) {
        if (!fd_write_all(c->fd, frame, len, 2000u))
            c->closed = true;
    }
    pthread_mutex_unlock(&c->write_mu);
    pthread_mutex_lock(&c->state_mu);
    if (c->pending > 0u)
        --c->pending;
    if (c->pending == 0u)
        pthread_cond_broadcast(&c->state_cv);
    pthread_mutex_unlock(&c->state_mu);
    delete rctx;
}

static void *test_daemon_client_main(void *arg)
{
    auto *c = static_cast<TestDaemonClientCtx *>(arg);
    auto *d = c->daemon;
    uint8_t hdr[A840_SC_HEADER_BYTES];

    while (!d->stop.load(std::memory_order_relaxed)) {
        int r = fd_read_exact(c->fd, hdr, sizeof(hdr), 50u);
        if (r == 0)
            continue;
        if (r < 0)
            break;

        size_t total = 0u;
        if (a840_sc_peek_frame_length(hdr, sizeof(hdr), &total) != A840_SC_OK)
            break;
        std::vector<uint8_t> frame(total);
        std::memcpy(frame.data(), hdr, sizeof(hdr));
        if (total > A840_SC_HEADER_BYTES) {
            if (fd_read_exact(c->fd, frame.data() + A840_SC_HEADER_BYTES,
                              total - A840_SC_HEADER_BYTES, 1000u) <= 0)
                break;
        }

        /* L0 + L1 fast-path cache check mirroring NezhaShaderCompiler/main.c */
        A840ScFrameHeader decoded;
        const uint8_t *payload = nullptr;
        std::memset(&decoded, 0, sizeof(decoded));
        if (d->cache_store &&
            a840_sc_decode_frame(frame.data(), frame.size(), d->stack_key, d->abi_fp,
                                 &decoded, &payload) == A840_SC_OK &&
            (decoded.msg_type == A840_SC_MSG_COMPILE_REQUEST ||
             decoded.msg_type == A840_SC_MSG_PREWARM_HINT)) {
            uint8_t *blob = nullptr;
            size_t blob_bytes = 0u;
            A840SystemCacheTier tier = A840_CACHE_TIER_MISS;
            if (a840_system_cache_lookup_blob(d->cache_store, &decoded.pipeline_key,
                                              &blob, &blob_bytes, &tier) == A840_SYS_CACHE_OK) {
                A840ScFrameHeader rh;
                std::memset(&rh, 0, sizeof(rh));
                rh.magic = A840_SC_PROTO_MAGIC;
                rh.version = A840_SC_PROTO_VERSION;
                rh.msg_type = A840_SC_MSG_COMPILE_REPLY;
                rh.priority_class = decoded.priority_class;
                rh.stack_key = d->stack_key;
                rh.abi_fingerprint = d->abi_fp;
                rh.pipeline_key = decoded.pipeline_key;
                rh.request_id = decoded.request_id;
                rh.client_pid = decoded.client_pid;
                rh.status = A840_SC_OK;
                rh.payload_bytes = static_cast<uint32_t>(blob_bytes);
                std::vector<uint8_t> wire(A840_SC_HEADER_BYTES + blob_bytes);
                size_t wire_len = 0u;
                if (a840_sc_encode_frame(&rh, blob, wire.data(), wire.size(), &wire_len) == A840_SC_OK) {
                    pthread_mutex_lock(&c->write_mu);
                    if (!c->closed && c->fd >= 0)
                        (void)fd_write_all(c->fd, wire.data(), wire_len, 2000u);
                    pthread_mutex_unlock(&c->write_mu);
                }
                std::free(blob);
                continue;
            }
        }

        uint32_t msg_type = static_cast<uint32_t>(frame[8]) |
                            (static_cast<uint32_t>(frame[9]) << 8) |
                            (static_cast<uint32_t>(frame[10]) << 16) |
                            (static_cast<uint32_t>(frame[11]) << 24);
        bool expects_reply = (msg_type == A840_SC_MSG_COMPILE_REQUEST ||
                              msg_type == A840_SC_MSG_PREWARM_HINT ||
                              msg_type == A840_SC_MSG_PING_STATUS);
        TestReplyCtx *rctx = nullptr;
        if (expects_reply) {
            rctx = new TestReplyCtx{c};
            pthread_mutex_lock(&c->state_mu);
            ++c->pending;
            pthread_mutex_unlock(&c->state_mu);
        }
        A840ScStatus hst = a840_sc_service_handle_frame(d->service, frame.data(),
            frame.size(), expects_reply ? test_daemon_reply_cb : nullptr, rctx);
        if (expects_reply &&
            (hst == A840_SC_INVALID_ARG || hst == A840_SC_PROTO_ERROR ||
             hst == A840_SC_TRUNCATED || hst == A840_SC_PAYLOAD_TOO_LARGE ||
             hst == A840_SC_STACK_MISMATCH || hst == A840_SC_ABI_MISMATCH ||
             hst == A840_SC_HASH_MISMATCH || hst == A840_SC_RESERVED_NONZERO ||
             hst == A840_SC_UNKNOWN_PRIORITY || hst == A840_SC_UNKNOWN_MSG_TYPE)) {
            pthread_mutex_lock(&c->state_mu);
            if (c->pending > 0u)
                --c->pending;
            if (c->pending == 0u)
                pthread_cond_broadcast(&c->state_cv);
            pthread_mutex_unlock(&c->state_mu);
            delete rctx;
            break;
        }
    }

    pthread_mutex_lock(&c->state_mu);
    while (c->pending > 0u)
        pthread_cond_wait(&c->state_cv, &c->state_mu);
    pthread_mutex_unlock(&c->state_mu);

    pthread_mutex_lock(&c->write_mu);
    c->closed = true;
    if (c->fd >= 0) {
        (void)close(c->fd);
        c->fd = -1;
    }
    pthread_mutex_unlock(&c->write_mu);
    pthread_cond_destroy(&c->state_cv);
    pthread_mutex_destroy(&c->state_mu);
    pthread_mutex_destroy(&c->write_mu);
    delete c;
    return nullptr;
}

static void *test_daemon_accept_main(void *arg)
{
    auto *d = static_cast<TestDaemon *>(arg);
    while (!d->stop.load(std::memory_order_relaxed)) {
        struct pollfd pfd;
        pfd.fd = d->listen_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 25);
        if (pr <= 0 || (pfd.revents & POLLIN) == 0)
            continue;
        int cfd = accept(d->listen_fd, nullptr, nullptr);
        if (cfd < 0)
            continue;
        auto *conn = new TestDaemonClientCtx();
        conn->daemon = d;
        conn->fd = cfd;
        conn->pending = 0u;
        conn->closed = false;
        pthread_mutex_init(&conn->write_mu, nullptr);
        pthread_mutex_init(&conn->state_mu, nullptr);
        pthread_cond_init(&conn->state_cv, nullptr);
        pthread_t th{};
        if (pthread_create(&th, nullptr, test_daemon_client_main, conn) == 0) {
            pthread_mutex_lock(&d->clients_mu);
            d->client_threads.push_back(th);
            d->client_fds.push_back(cfd);
            pthread_mutex_unlock(&d->clients_mu);
        } else {
            (void)close(cfd);
            pthread_cond_destroy(&conn->state_cv);
            pthread_mutex_destroy(&conn->state_mu);
            pthread_mutex_destroy(&conn->write_mu);
            delete conn;
        }
    }
    return nullptr;
}

static void start_test_daemon(TestDaemon &d, const std::string &sock_path,
    const std::string &pack_path, const std::string &shared_dir,
    uint32_t workers, size_t max_cache_bytes = A840_SYS_CACHE_DEFAULT_MAX_BYTES)
{
    d.socket_path = sock_path;
    d.stack_key = kStackKey;
    d.abi_fp = a840_compile_abi_fingerprint();
    d.stop.store(false);
    d.delay_ms.store(0u);
    d.priority_boost_seen.store(0u);
    d.compile_calls.store(0u);

    A840SystemCacheConfig ccfg;
    std::memset(&ccfg, 0, sizeof(ccfg));
    ccfg.stack_key = d.stack_key;
    ccfg.abi_fingerprint = d.abi_fp;
    ccfg.role = A840_CACHE_ROLE_RENDER_SERVER;
    ccfg.pack_path = pack_path.empty() ? nullptr : pack_path.c_str();
    ccfg.shared_cache_dir = shared_dir.empty() ? nullptr : shared_dir.c_str();
    ccfg.max_cache_bytes = max_cache_bytes;
    ccfg.isolate_by_abi = true;
    ccfg.quarantine_corrupt = true;
    CHECK(a840_system_cache_create(&ccfg, &d.cache_store) == A840_SYS_CACHE_OK);

    A840ScIr3BackendConfig bcfg;
    std::memset(&bcfg, 0, sizeof(bcfg));
    bcfg.stack_key = d.stack_key;
    bcfg.abi_fingerprint = d.abi_fp;
    bcfg.default_compile_flags = A840_COMPILE_FLAG_EARLY_PREAMBLE;
    bcfg.enable_translate = false;
    CHECK(a840_sc_ir3_backend_create(&bcfg, &d.backend) == A840_SC_OK);

    A840ScServiceConfig scfg;
    std::memset(&scfg, 0, sizeof(scfg));
    scfg.stack_key = d.stack_key;
    scfg.abi_fingerprint = d.abi_fp;
    scfg.worker_count = workers;
    scfg.mem_cache_entries = 256u;
    scfg.mem_cache_bytes = 8u * 1024u * 1024u;
    scfg.disk_cache_dir = nullptr;
    scfg.compile_fn = test_daemon_compile_cb;
    scfg.on_priority_boost = test_daemon_boost_cb;
    scfg.user_ctx = &d;
    CHECK(a840_sc_service_create(&scfg, &d.service) == A840_SC_OK);

    d.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(d.listen_fd >= 0);
    (void)unlink(sock_path.c_str());
    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1u);
    CHECK(bind(d.listen_fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0);
    CHECK(listen(d.listen_fd, 32) == 0);
    CHECK(pthread_create(&d.accept_thread, nullptr, test_daemon_accept_main, &d) == 0);
    d.accept_started = true;
}

static void stop_test_daemon(TestDaemon &d)
{
    d.stop.store(true);
    if (d.accept_started) {
        pthread_join(d.accept_thread, nullptr);
        d.accept_started = false;
    }
    if (d.listen_fd >= 0) {
        (void)close(d.listen_fd);
        d.listen_fd = -1;
    }
    (void)unlink(d.socket_path.c_str());

    std::vector<pthread_t> threads;
    pthread_mutex_lock(&d.clients_mu);
    threads.swap(d.client_threads);
    d.client_fds.clear();
    pthread_mutex_unlock(&d.clients_mu);
    for (pthread_t th : threads)
        pthread_join(th, nullptr);

    a840_sc_service_destroy(d.service);
    d.service = nullptr;
    a840_sc_ir3_backend_destroy(d.backend);
    d.backend = nullptr;
    a840_system_cache_destroy(d.cache_store);
    d.cache_store = nullptr;
}

/* ---------------- Async Completion Helper ---------------- */

struct AsyncWaiter {
    pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    bool done = false;
    A840ScStatus status = A840_SC_TIMED_OUT;
    size_t blob_bytes = 0u;
};

static void async_done_cb(void *user_ctx, uint64_t request_id, A840ScStatus status,
    const uint8_t *record_blob, size_t record_bytes)
{
    (void)request_id;
    (void)record_blob;
    auto *w = static_cast<AsyncWaiter *>(user_ctx);
    pthread_mutex_lock(&w->mu);
    w->status = status;
    w->blob_bytes = record_bytes;
    w->done = true;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

static void wait_async(AsyncWaiter &w)
{
    pthread_mutex_lock(&w.mu);
    while (!w.done)
        pthread_cond_wait(&w.cv, &w.mu);
    pthread_mutex_unlock(&w.mu);
}

/* ---------------- Stress Worker for Case 38 (TSan 16x) ---------------- */

struct StressWorkerArg {
    uint32_t worker_idx;
    uint32_t rounds;
    std::string pack_path;
    std::string shared_dir;
    std::string private_dir;
    std::string sock_path;
    const std::vector<A840PipelineKey> *pack_keys;
    A840SystemCacheStore *shared_bb_store;
    std::atomic<uint64_t> *ops_ok;
};

static void *stress_worker_main(void *arg)
{
    auto *a = static_cast<StressWorkerArg *>(arg);
    const uint64_t abi_fp = a840_compile_abi_fingerprint();
    const bool is_backboardd = (a->worker_idx == 0u);

    A840SystemCacheStore *app_store = nullptr;
    if (!is_backboardd) {
        A840SystemCacheConfig ccfg;
        std::memset(&ccfg, 0, sizeof(ccfg));
        ccfg.stack_key = kStackKey;
        ccfg.abi_fingerprint = abi_fp;
        ccfg.role = A840_CACHE_ROLE_APP_SANDBOX;
        ccfg.pack_path = a->pack_path.c_str();
        ccfg.shared_cache_dir = a->shared_dir.c_str();
        ccfg.private_cache_dir = a->private_dir.c_str();
        ccfg.max_cache_bytes = 12u * 1024u;
        ccfg.isolate_by_abi = true;
        ccfg.quarantine_corrupt = true;
        CHECK(a840_system_cache_create(&ccfg, &app_store) == A840_SYS_CACHE_OK);
    }

    A840SystemCacheStore *store = is_backboardd ? a->shared_bb_store : app_store;

    int fd = connect_unix_socket(a->sock_path);
    CHECK(fd >= 0);
    UnixClientTransport tr{fd};
    A840ScClientConfig clcfg;
    std::memset(&clcfg, 0, sizeof(clcfg));
    clcfg.stack_key = kStackKey;
    clcfg.abi_fingerprint = abi_fp;
    clcfg.client_pid = 9000u + a->worker_idx;
    clcfg.send_fn = unix_send_fn;
    clcfg.recv_fn = unix_recv_fn;
    clcfg.user_ctx = &tr;
    A840ScClient *client = nullptr;
    CHECK(a840_sc_client_create(&clcfg, &client) == A840_SC_OK);

    A840PipelineKey live_key_a;
    auto live_payload_a = encode_spirv_compute_payload(
        kLiveComputeSpvA, sizeof(kLiveComputeSpvA), "main", 1u, 1u, 1u, &live_key_a);
    A840PipelineKey live_key_b;
    auto live_payload_b = encode_spirv_compute_payload(
        kLiveComputeSpvB, sizeof(kLiveComputeSpvB), "cs_tw2", 8u, 1u, 1u, &live_key_b);

    for (uint32_t r = 0u; r < a->rounds; ++r) {
        /* 1. Fast L0 pack.bin lookup */
        const A840PipelineKey &pk = (*a->pack_keys)[(r + a->worker_idx) % a->pack_keys->size()];
        A840PackV1Entry pentry;
        CHECK(a840_system_cache_lookup_pack_view(store, &pk, &pentry) == A840_SYS_CACHE_OK);

        /* 2. Write & read L1 (backboardd -> shared; App -> private container) */
        A840PipelineKey dyn_key;
        auto blob = make_valid_record_blob(100u + a->worker_idx * 32u + (r % 6u),
                                           kStackKey, abi_fp, 32u, &dyn_key);
        CHECK(a840_system_cache_put_blob(store, &dyn_key, blob.data(), blob.size()) == A840_SYS_CACHE_OK);

        A840PipelineRecord rec;
        std::memset(&rec, 0, sizeof(rec));
        A840SystemCacheTier tier = A840_CACHE_TIER_MISS;
        A840SystemCacheStatus lst = a840_system_cache_lookup(store, &dyn_key, &rec, &tier);
        /* Under tight LRU cap another thread may evict a shared entry, so OK or MISS are valid */
        if (lst == A840_SYS_CACHE_OK) {
            CHECK(rec.stage_count == 1u);
            a840_record_free(&rec);
        }

        /* 3. Live AF_UNIX compile / cache-hit via NezhaShaderCompiler */
        uint8_t *reply_blob = nullptr;
        size_t reply_bytes = 0u;
        const auto &pay = ((r + a->worker_idx) % 2u == 0u) ? live_payload_a : live_payload_b;
        const auto &key = ((r + a->worker_idx) % 2u == 0u) ? live_key_a : live_key_b;
        CHECK(a840_sc_client_compile_sync(client, &key, pay.data(), pay.size(),
                                          5000u, &reply_blob, &reply_bytes) == A840_SC_OK);
        CHECK(reply_blob != nullptr && reply_bytes >= A840_RECORD_PREFIX_BYTES);
        std::free(reply_blob);

        a->ops_ok->fetch_add(1u, std::memory_order_relaxed);
    }

    a840_sc_client_destroy(client);
    (void)close(fd);
    if (app_store)
        a840_system_cache_destroy(app_store);
    return nullptr;
}

static std::string make_safe_sock_path(const std::string &work_dir, const char *tag)
{
    struct sockaddr_un dummy;
    std::string full = work_dir + "/" + tag + ".sock";
    if (full.size() < sizeof(dummy.sun_path))
        return full;
    /* POSIX AF_UNIX sun_path is capped at 104/108 bytes; use a short relative path
     * inside the approved test working directory when work_dir is deeply nested. */
    return std::string("./w12_") + std::to_string(static_cast<long>(getpid())) +
           "_" + tag + ".sock";
}

static std::string resolve_work_dir(const char *cli_work_dir)
{
    if (cli_work_dir && cli_work_dir[0])
        return std::string(cli_work_dir);
    const char *tmpdir = std::getenv("TMPDIR");
    if (tmpdir && tmpdir[0] && std::strncmp(tmpdir, "/tmp", 4u) != 0)
        return std::string(tmpdir) + "/w12_l2_test_" + std::to_string(static_cast<long>(getpid()));
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) != nullptr)
        return std::string(cwd) + "/w12_l2_test_" + std::to_string(static_cast<long>(getpid()));
    return "/home/hua/nezha-work/ios27/adreno840-native/.cache/w12-l2/unit-work";
}

} /* anonymous namespace */

int main(int argc, char **argv)
{
    (void)std::signal(SIGPIPE, SIG_IGN);

    const char *cli_work_dir = nullptr;
    const char *cli_daemon_bin = nullptr;
    const char *cli_plist_path = nullptr;
    uint32_t tsan_stress_rounds = 2u;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--work-dir") == 0 && i + 1 < argc) {
            cli_work_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--daemon-bin") == 0 && i + 1 < argc) {
            cli_daemon_bin = argv[++i];
        } else if (std::strcmp(argv[i], "--plist") == 0 && i + 1 < argc) {
            cli_plist_path = argv[++i];
        } else if (std::strcmp(argv[i], "--tsan-stress") == 0 && i + 1 < argc) {
            tsan_stress_rounds = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            if (tsan_stress_rounds == 0u)
                tsan_stress_rounds = 1u;
        }
    }

    const std::string work_dir = resolve_work_dir(cli_work_dir);
    const bool cleanup_work = (cli_work_dir == nullptr);
    rm_rf(work_dir);
    CHECK(mkdir_recursive(work_dir));

    const uint64_t abi_fp = a840_compile_abi_fingerprint();
    CHECK(abi_fp != 0u);

    /* ==================== Group A: 6-Domain System Pack Builder (Cases 1-12) ==================== */

    CASE("01_catalog_count_and_domains");
    {
        size_t n = a840_system_pack_catalog_count();
        CHECK(n >= A840_SYSTEM_PACK_MIN_SHADERS);
        uint32_t per_domain[A840_SYSDOMAIN_COUNT] = {0u};
        for (size_t i = 0u; i < n; ++i) {
            const A840SystemShaderDesc *d = a840_system_pack_catalog_get(i);
            CHECK(d != nullptr);
            CHECK(d->index == i);
            CHECK(static_cast<uint32_t>(d->domain) < A840_SYSDOMAIN_COUNT);
            CHECK(d->name != nullptr && d->name[0] != '\0');
            CHECK(d->entry_name != nullptr && d->entry_name[0] != '\0');
            if (d->is_render)
                CHECK(d->fs_entry_name != nullptr && d->fs_entry_name[0] != '\0');
            ++per_domain[d->domain];
        }
        for (uint32_t dom = 0u; dom < A840_SYSDOMAIN_COUNT; ++dom) {
            CHECK(per_domain[dom] >= 6u);
            CHECK(std::strcmp(a840_system_pack_domain_name(static_cast<A840SystemPackDomain>(dom)), "unknown") != 0);
        }
        CHECK(a840_system_pack_catalog_get(n) == nullptr);
    }

    CASE("02_catalog_unique_spirv_and_valid_magic");
    {
        size_t n = a840_system_pack_catalog_count();
        for (size_t i = 0u; i < n; ++i) {
            const A840SystemShaderDesc *d = a840_system_pack_catalog_get(i);
            uint32_t stages = d->is_render ? 2u : 1u;
            for (uint32_t s = 0u; s < stages; ++s) {
                uint32_t *words = nullptr;
                size_t bytes = 0u;
                CHECK(a840_system_pack_entry_spirv(i, s, &words, &bytes) == A840_SYSPACK_OK);
                CHECK(words != nullptr && bytes >= 20u && (bytes % 4u) == 0u);
                CHECK(words[0] == 0x07230203u);
                std::free(words);
            }
            uint32_t *bad_w = nullptr;
            size_t bad_b = 0u;
            CHECK(a840_system_pack_entry_spirv(i, stages, &bad_w, &bad_b) == A840_SYSPACK_INVALID_ARG);
        }
    }

    std::vector<uint8_t> pack_mem_1;
    A840SystemPackBuildReport rep1;
    std::memset(&rep1, 0, sizeof(rep1));

    CASE("03_build_6domain_pack_memory_and_checks");
    {
        A840SystemPackBuildConfig cfg = make_pack_build_cfg(kStackKey);
        uint8_t *buf = nullptr;
        size_t bytes = 0u;
        A840SystemPackStatus st = a840_system_pack_build_memory(&cfg, &buf, &bytes, &rep1);
        CHECK_MSG(st == A840_SYSPACK_OK, a840_system_pack_status_string(st));
        CHECK(buf != nullptr && bytes > A840_PACK_V1_HEADER_BYTES);
        CHECK(rep1.total_entries == a840_system_pack_catalog_count());
        CHECK(rep1.total_entries >= 36u);
        for (uint32_t d = 0u; d < A840_SYSDOMAIN_COUNT; ++d)
            CHECK(rep1.domain_counts[d] >= 6u);
        CHECK(rep1.fbfetch_stages >= 3u);
        CHECK(rep1.xr10_checked_entries > 0u);
        CHECK(rep1.xr10_safe_entries == rep1.xr10_checked_entries);
        CHECK(rep1.valid_instrlen_stages == rep1.total_stages);
        CHECK(rep1.unbound_ubo_block1_count == 0u);
        CHECK(rep1.sds_attached_stages == rep1.total_stages);
        pack_mem_1.assign(buf, buf + bytes);
        std::free(buf);
    }

    CASE("04_build_pack_byte_identical_determinism");
    {
        A840SystemPackBuildConfig cfg = make_pack_build_cfg(kStackKey);
        uint8_t *buf2 = nullptr;
        size_t bytes2 = 0u;
        A840SystemPackBuildReport rep2;
        std::memset(&rep2, 0, sizeof(rep2));
        CHECK(a840_system_pack_build_memory(&cfg, &buf2, &bytes2, &rep2) == A840_SYSPACK_OK);
        CHECK(bytes2 == pack_mem_1.size());
        CHECK(std::memcmp(rep1.pack_sha256, rep2.pack_sha256, 32u) == 0);
        CHECK(std::memcmp(pack_mem_1.data(), buf2, bytes2) == 0);
        std::free(buf2);
    }

    const std::string pack_file = work_dir + "/system_pack.bin";
    A840SystemPackVerifyReport vrep;
    std::memset(&vrep, 0, sizeof(vrep));

    CASE("05_build_pack_to_file_and_mmap_verify");
    {
        A840SystemPackBuildConfig cfg = make_pack_build_cfg(kStackKey);
        A840SystemPackBuildReport frep;
        std::memset(&frep, 0, sizeof(frep));
        A840SystemPackStatus st = a840_system_pack_build_to_file(&cfg, pack_file.c_str(), &frep);
        CHECK_MSG(st == A840_SYSPACK_OK, a840_system_pack_status_string(st));
        CHECK(std::memcmp(frep.pack_sha256, rep1.pack_sha256, 32u) == 0);
        CHECK(a840_system_pack_verify_file_mmap(pack_file.c_str(), kStackKey, abi_fp, 200u, &vrep) == A840_SYSPACK_OK);
        CHECK(vrep.entry_count == rep1.total_entries);
        CHECK(vrep.all_isa_128b_aligned);
        CHECK(vrep.all_sds_present);
        CHECK(vrep.all_instrlen_valid);
        CHECK(vrep.unbound_ubo_block1_count == 0u);
        CHECK(std::memcmp(vrep.pack_sha256, rep1.pack_sha256, 32u) == 0);
    }

#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#    define A840_TEST_SANITIZER_ACTIVE 1
#  endif
#endif
#if !defined(A840_TEST_SANITIZER_ACTIVE) && (defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__))
#  define A840_TEST_SANITIZER_ACTIVE 1
#endif

    CASE("06_pack_lookup_latency_p99_under_2us");
    {
#if defined(A840_TEST_SANITIZER_ACTIVE) || !defined(__OPTIMIZE__)
        /* Unoptimized (-O0) or ASan/UBSan/TSan instrumented builds add 10-20x per-load overhead. */
        CHECK(vrep.lookup_mean_us < 50.0);
        CHECK(vrep.lookup_p99_us < 100.0);
#else
        CHECK(vrep.lookup_mean_us < 2.0);
        CHECK(vrep.lookup_p99_us < 2.0);
#endif
    }

    CASE("07_pack_domain_subset_filter");
    {
        A840SystemPackBuildConfig cfg_glass = make_pack_build_cfg(kStackKey, 1u << A840_SYSDOMAIN_CA_LIQUID_GLASS);
        A840SystemPackBuildConfig cfg_cam = make_pack_build_cfg(kStackKey, 1u << A840_SYSDOMAIN_CAMERA155);
        uint8_t *b1 = nullptr, *b2 = nullptr;
        size_t l1 = 0u, l2 = 0u;
        A840SystemPackBuildReport r1, r2;
        CHECK(a840_system_pack_build_memory(&cfg_glass, &b1, &l1, &r1) == A840_SYSPACK_OK);
        CHECK(a840_system_pack_build_memory(&cfg_cam, &b2, &l2, &r2) == A840_SYSPACK_OK);
        CHECK(r1.total_entries == 7u && r1.domain_counts[A840_SYSDOMAIN_CA_LIQUID_GLASS] == 7u);
        CHECK(r2.total_entries == 7u && r2.domain_counts[A840_SYSDOMAIN_CAMERA155] == 7u);
        CHECK(std::memcmp(r1.pack_sha256, r2.pack_sha256, 32u) != 0);
        std::free(b1);
        std::free(b2);
    }

    CASE("08_pack_reject_forbidden_paths");
    {
        A840SystemPackBuildConfig cfg = make_pack_build_cfg(kStackKey, 1u);
        CHECK(a840_system_pack_build_to_file(&cfg, "C:/pack.bin", nullptr) == A840_SYSPACK_PATH_FORBIDDEN);
        CHECK(a840_system_pack_build_to_file(&cfg, "/mnt/c/pack.bin", nullptr) == A840_SYSPACK_PATH_FORBIDDEN);
        CHECK(a840_system_pack_build_to_file(&cfg, "/tmp/pack.bin", nullptr) == A840_SYSPACK_PATH_FORBIDDEN);
        CHECK(a840_system_pack_build_to_file(&cfg, "/private/var/ngpu/pack.bin", nullptr) == A840_SYSPACK_PATH_FORBIDDEN);
        A840SystemPackVerifyReport vr;
        CHECK(a840_system_pack_verify_file_mmap("/tmp/pack.bin", kStackKey, abi_fp, 1u, &vr) == A840_SYSPACK_PATH_FORBIDDEN);
    }

    CASE("09_pack_reject_invalid_args_and_abi");
    {
        uint8_t *b = nullptr;
        size_t l = 0u;
        A840SystemPackBuildConfig bad = make_pack_build_cfg(0u);
        CHECK(a840_system_pack_build_memory(&bad, &b, &l, nullptr) == A840_SYSPACK_INVALID_ARG);
        bad = make_pack_build_cfg(kStackKey);
        bad.tier = 9u;
        CHECK(a840_system_pack_build_memory(&bad, &b, &l, nullptr) == A840_SYSPACK_INVALID_ARG);
        bad = make_pack_build_cfg(kStackKey);
        bad.abi_fingerprint = abi_fp ^ 0xffu;
        CHECK(a840_system_pack_build_memory(&bad, &b, &l, nullptr) == A840_SYSPACK_INVALID_ARG);
    }

    CASE("10_pack_sds_static_type4_validity");
    {
        A840PackV1View view;
        std::memset(&view, 0, sizeof(view));
        void *aligned = nullptr;
        CHECK(posix_memalign(&aligned, A840_PACK_V1_ISA_ALIGNMENT, pack_mem_1.size()) == 0);
        std::memcpy(aligned, pack_mem_1.data(), pack_mem_1.size());
        CHECK(a840_pack_v1_open_memory(aligned, pack_mem_1.size(), kStackKey, abi_fp, &view) == A840_PACK_OK);
        for (uint32_t i = 0u; i < view.entry_count; ++i) {
            A840PackV1Entry e;
            CHECK(a840_pack_v1_entry_at(&view, i, &e) == A840_PACK_OK);
            for (uint32_t s = 0u; s < e.stage_count; ++s) {
                CHECK(e.stages[s].sds_state_dwords == 6u);
                const uint32_t *w = e.stages[s].sds_state_words;
                for (uint32_t pkt = 0u; pkt < 3u; ++pkt) {
                    uint32_t hdr = w[pkt * 2u];
                    CHECK((hdr >> 28) == 4u);
                    CHECK((hdr & 0x7fu) == 1u);
                }
            }
        }
        std::free(aligned);
    }

    const std::string corrupt_pack_file = work_dir + "/corrupt_pack.bin";
    CASE("11_pack_corrupt_file_rejected_by_mmap_verify");
    {
        std::vector<uint8_t> broken = pack_mem_1;
        broken[broken.size() - 17u] ^= 0x55u;
        FILE *f = std::fopen(corrupt_pack_file.c_str(), "wb");
        CHECK(f != nullptr);
        CHECK(std::fwrite(broken.data(), 1u, broken.size(), f) == broken.size());
        std::fclose(f);
        A840SystemPackVerifyReport vr;
        CHECK(a840_system_pack_verify_file_mmap(corrupt_pack_file.c_str(), kStackKey, abi_fp, 10u, &vr) == A840_SYSPACK_VERIFY_ERROR);
    }

    CASE("12_pack_stack_mismatch_rejected_by_mmap_verify");
    {
        A840SystemPackVerifyReport vr;
        CHECK(a840_system_pack_verify_file_mmap(pack_file.c_str(), kAltStackKey, abi_fp, 10u, &vr) == A840_SYSPACK_VERIFY_ERROR);
    }

    /* ==================== Group B: Three-Tier Cache Store (Cases 13-28) ==================== */

    CASE("13_cache_crc32_known_vector");
    {
        CHECK(a840_system_cache_crc32("123456789", 9u) == 0xcbf43926u);
        CHECK(a840_system_cache_crc32("", 0u) == 0u);
    }

    CASE("14_cache_reject_forbidden_paths_and_args");
    {
        A840SystemCacheStore *s = nullptr;
        A840SystemCacheConfig cfg;
        std::memset(&cfg, 0, sizeof(cfg));
        cfg.stack_key = 0u;
        CHECK(a840_system_cache_create(&cfg, &s) == A840_SYS_CACHE_INVALID_ARG);

        cfg.stack_key = kStackKey;
        cfg.shared_cache_dir = "/mnt/c/bad_cache";
        CHECK(a840_system_cache_create(&cfg, &s) == A840_SYS_CACHE_PATH_FORBIDDEN);

        cfg.shared_cache_dir = "/tmp/bad_cache";
        CHECK(a840_system_cache_create(&cfg, &s) == A840_SYS_CACHE_PATH_FORBIDDEN);

        cfg.shared_cache_dir = "/private/var/ngpu/cache";
        CHECK(a840_system_cache_create(&cfg, &s) == A840_SYS_CACHE_PATH_FORBIDDEN);

        cfg.shared_cache_dir = "C:/bad_cache";
        CHECK(a840_system_cache_create(&cfg, &s) == A840_SYS_CACHE_PATH_FORBIDDEN);
    }

    const std::string shared_root = work_dir + "/shared_cache";
    const std::string app_priv_base = work_dir + "/app_private";
    A840SystemCacheStore *bb_store = nullptr;

    CASE("15_cache_l0_pack_hit_under_1us");
    {
        A840SystemCacheConfig cfg;
        std::memset(&cfg, 0, sizeof(cfg));
        cfg.stack_key = kStackKey;
        cfg.abi_fingerprint = abi_fp;
        cfg.role = A840_CACHE_ROLE_RENDER_SERVER;
        cfg.pack_path = pack_file.c_str();
        cfg.shared_cache_dir = shared_root.c_str();
        cfg.isolate_by_abi = true;
        cfg.quarantine_corrupt = true;
        CHECK(a840_system_cache_create(&cfg, &bb_store) == A840_SYS_CACHE_OK);
        CHECK(a840_system_cache_has_pack(bb_store));

        /* Warmup pass before measuring L0 O(1) lookup latency */
        for (uint32_t i = 0u; i < rep1.entry_key_count; ++i) {
            A840PackV1Entry e;
            CHECK(a840_system_cache_lookup_pack_view(bb_store, &rep1.entry_keys[i], &e) == A840_SYS_CACHE_OK);
        }
        uint64_t t0 = now_ns();
        for (uint32_t i = 0u; i < rep1.entry_key_count; ++i) {
            A840PackV1Entry e;
            CHECK(a840_system_cache_lookup_pack_view(bb_store, &rep1.entry_keys[i], &e) == A840_SYS_CACHE_OK);
        }
        uint64_t t1 = now_ns();
        double mean_us = (static_cast<double>(t1 - t0) / static_cast<double>(rep1.entry_key_count)) / 1000.0;
#if defined(A840_TEST_SANITIZER_ACTIVE) || !defined(__OPTIMIZE__)
        CHECK(mean_us < 50.0);
#else
        CHECK(mean_us < 1.0);
#endif

        A840SystemCacheStats st;
        a840_system_cache_get_stats(bb_store, &st);
        CHECK(st.l0_pack_hits == static_cast<uint64_t>(rep1.entry_key_count) * 2u);
        CHECK(st.l0_pack_entries == rep1.total_entries);
    }

    CASE("16_cache_l0_lookup_record_and_blob");
    {
        A840PipelineRecord rec;
        std::memset(&rec, 0, sizeof(rec));
        A840SystemCacheTier tier = A840_CACHE_TIER_MISS;
        CHECK(a840_system_cache_lookup(bb_store, &rep1.entry_keys[0], &rec, &tier) == A840_SYS_CACHE_OK);
        CHECK(tier == A840_CACHE_TIER_L0_PACK);
        CHECK(rec.stage_count >= 1u);
        a840_record_free(&rec);

        uint8_t *blob = nullptr;
        size_t bytes = 0u;
        CHECK(a840_system_cache_lookup_blob(bb_store, &rep1.entry_keys[1], &blob, &bytes, &tier) == A840_SYS_CACHE_OK);
        CHECK(tier == A840_CACHE_TIER_L0_PACK);
        CHECK(blob != nullptr && bytes >= A840_RECORD_PREFIX_BYTES);
        std::free(blob);
    }

    CASE("17_cache_miss_returns_miss_and_counts");
    {
        A840PipelineKey missing_key;
        std::memset(&missing_key, 0xaa, sizeof(missing_key));
        A840PipelineRecord rec;
        std::memset(&rec, 0, sizeof(rec));
        A840SystemCacheTier tier = A840_CACHE_TIER_L0_PACK;
        CHECK(a840_system_cache_lookup(bb_store, &missing_key, &rec, &tier) == A840_SYS_CACHE_MISS);
        CHECK(tier == A840_CACHE_TIER_MISS);
        A840SystemCacheStats st;
        a840_system_cache_get_stats(bb_store, &st);
        CHECK(st.misses >= 1u);
    }

    A840PipelineKey shared_key1;
    auto shared_blob1 = make_valid_record_blob(1001u, kStackKey, abi_fp, 32u, &shared_key1);

    CASE("18_cache_render_server_shared_write_and_app_readonly_read");
    {
        CHECK(a840_system_cache_put_blob(bb_store, &shared_key1, shared_blob1.data(), shared_blob1.size()) == A840_SYS_CACHE_OK);
        A840SystemCacheStats bbst;
        a840_system_cache_get_stats(bb_store, &bbst);
        CHECK(bbst.puts_shared == 1u);

        for (int app = 0; app < 4; ++app) {
            std::string pdir = app_priv_base + "_" + std::to_string(app);
            A840SystemCacheConfig acfg;
            std::memset(&acfg, 0, sizeof(acfg));
            acfg.stack_key = kStackKey;
            acfg.abi_fingerprint = abi_fp;
            acfg.role = A840_CACHE_ROLE_APP_SANDBOX;
            acfg.pack_path = pack_file.c_str();
            acfg.shared_cache_dir = shared_root.c_str();
            acfg.private_cache_dir = pdir.c_str();
            acfg.isolate_by_abi = true;
            acfg.quarantine_corrupt = true;
            A840SystemCacheStore *app_s = nullptr;
            CHECK(a840_system_cache_create(&acfg, &app_s) == A840_SYS_CACHE_OK);

            A840PipelineRecord rec;
            std::memset(&rec, 0, sizeof(rec));
            A840SystemCacheTier tier = A840_CACHE_TIER_MISS;
            CHECK(a840_system_cache_lookup(app_s, &shared_key1, &rec, &tier) == A840_SYS_CACHE_OK);
            CHECK(tier == A840_CACHE_TIER_L1_SHARED);
            a840_record_free(&rec);

            A840SystemCacheStats ast;
            a840_system_cache_get_stats(app_s, &ast);
            CHECK(ast.l1_shared_hits == 1u);
            CHECK(ast.puts_shared == 0u);
            a840_system_cache_destroy(app_s);
        }
    }

    CASE("19_cache_app_sandbox_write_isolated_to_private_container");
    {
        std::string pdir0 = app_priv_base + "_iso0";
        std::string pdir1 = app_priv_base + "_iso1";
        A840SystemCacheConfig c0{}, c1{};
        c0.stack_key = kStackKey; c0.abi_fingerprint = abi_fp;
        c0.role = A840_CACHE_ROLE_APP_SANDBOX;
        c0.shared_cache_dir = shared_root.c_str();
        c0.private_cache_dir = pdir0.c_str();
        c0.isolate_by_abi = true;
        c1 = c0;
        c1.private_cache_dir = pdir1.c_str();

        A840SystemCacheStore *app0 = nullptr, *app1 = nullptr;
        CHECK(a840_system_cache_create(&c0, &app0) == A840_SYS_CACHE_OK);
        CHECK(a840_system_cache_create(&c1, &app1) == A840_SYS_CACHE_OK);

        A840PipelineKey priv_key;
        auto priv_blob = make_valid_record_blob(2002u, kStackKey, abi_fp, 32u, &priv_key);
        CHECK(a840_system_cache_put_blob(app0, &priv_key, priv_blob.data(), priv_blob.size()) == A840_SYS_CACHE_OK);

        /* App0 hits its own private container */
        A840PipelineRecord rec;
        std::memset(&rec, 0, sizeof(rec));
        A840SystemCacheTier tier = A840_CACHE_TIER_MISS;
        CHECK(a840_system_cache_lookup(app0, &priv_key, &rec, &tier) == A840_SYS_CACHE_OK);
        CHECK(tier == A840_CACHE_TIER_L1_PRIVATE);
        a840_record_free(&rec);

        /* App1 and backboardd must NOT see App0's private entry in shared dir */
        CHECK(a840_system_cache_lookup(app1, &priv_key, &rec, &tier) == A840_SYS_CACHE_MISS);
        CHECK(a840_system_cache_lookup(bb_store, &priv_key, &rec, &tier) == A840_SYS_CACHE_MISS);

        A840SystemCacheStats st0;
        a840_system_cache_get_stats(app0, &st0);
        CHECK(st0.puts_private == 1u && st0.puts_shared == 0u && st0.l1_private_hits == 1u);

        a840_system_cache_destroy(app0);
        a840_system_cache_destroy(app1);
    }

    CASE("20_cache_readonly_role_rejects_writes");
    {
        A840SystemCacheConfig rcfg{};
        rcfg.stack_key = kStackKey;
        rcfg.abi_fingerprint = abi_fp;
        rcfg.role = A840_CACHE_ROLE_READONLY;
        rcfg.pack_path = pack_file.c_str();
        rcfg.shared_cache_dir = shared_root.c_str();
        rcfg.isolate_by_abi = true;
        A840SystemCacheStore *ro = nullptr;
        CHECK(a840_system_cache_create(&rcfg, &ro) == A840_SYS_CACHE_OK);
        CHECK(a840_system_cache_put_blob(ro, &shared_key1, shared_blob1.data(), shared_blob1.size()) == A840_SYS_CACHE_READONLY);
        uint64_t ev = 0u;
        CHECK(a840_system_cache_enforce_limit(ro, &ev) == A840_SYS_CACHE_READONLY);
        a840_system_cache_destroy(ro);
    }

    CASE("21_cache_abi_and_stack_isolation");
    {
        A840SystemCacheConfig alt_cfg{};
        alt_cfg.stack_key = kAltStackKey;
        alt_cfg.abi_fingerprint = abi_fp;
        alt_cfg.role = A840_CACHE_ROLE_RENDER_SERVER;
        alt_cfg.shared_cache_dir = shared_root.c_str();
        alt_cfg.isolate_by_abi = true;
        alt_cfg.quarantine_corrupt = true;
        A840SystemCacheStore *alt = nullptr;
        CHECK(a840_system_cache_create(&alt_cfg, &alt) == A840_SYS_CACHE_OK);

        /* Different stack_key gets isolated directory */
        CHECK(std::strcmp(a840_system_cache_active_shared_dir(alt),
                          a840_system_cache_active_shared_dir(bb_store)) != 0);

        A840PipelineKey alt_key;
        auto alt_blob = make_valid_record_blob(3003u, kAltStackKey, abi_fp, 32u, &alt_key);
        CHECK(a840_system_cache_put_blob(alt, &alt_key, alt_blob.data(), alt_blob.size()) == A840_SYS_CACHE_OK);

        /* Copy alt_stack file directly into bb_store's active dir -> must be rejected */
        std::string src = std::string(a840_system_cache_active_shared_dir(alt)) + "/" + key_to_filename(alt_key);
        std::string dst = std::string(a840_system_cache_active_shared_dir(bb_store)) + "/" + key_to_filename(alt_key);
        FILE *fin = std::fopen(src.c_str(), "rb");
        FILE *fout = std::fopen(dst.c_str(), "wb");
        CHECK(fin && fout);
        uint8_t tmp[4096];
        size_t nr = 0u;
        while ((nr = std::fread(tmp, 1u, sizeof(tmp), fin)) > 0u)
            CHECK(std::fwrite(tmp, 1u, nr, fout) == nr);
        std::fclose(fin);
        std::fclose(fout);

        A840PipelineRecord rec{};
        A840SystemCacheTier tier = A840_CACHE_TIER_MISS;
        CHECK(a840_system_cache_lookup(bb_store, &alt_key, &rec, &tier) == A840_SYS_CACHE_MISS);
        A840SystemCacheStats st;
        a840_system_cache_get_stats(bb_store, &st);
        CHECK(st.abi_mismatch_rejected >= 1u);
        CHECK(st.corrupt_rejected >= 1u);
        CHECK(!file_exists(dst)); /* quarantined by writable RENDER_SERVER */
        a840_system_cache_destroy(alt);
    }

    CASE("22_cache_corrupt_truncated_file_self_heal");
    {
        A840PipelineKey k_trunc;
        auto b_good = make_valid_record_blob(4004u, kStackKey, abi_fp, 32u, &k_trunc);
        std::string path = std::string(a840_system_cache_active_shared_dir(bb_store)) + "/" + key_to_filename(k_trunc);
        FILE *f = std::fopen(path.c_str(), "wb");
        CHECK(f != nullptr);
        uint8_t garbage[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        CHECK(std::fwrite(garbage, 1u, sizeof(garbage), f) == sizeof(garbage));
        std::fclose(f);

        A840PipelineRecord rec{};
        A840SystemCacheTier tier = A840_CACHE_TIER_MISS;
        CHECK(a840_system_cache_lookup(bb_store, &k_trunc, &rec, &tier) == A840_SYS_CACHE_MISS);
        CHECK(!file_exists(path));

        /* Self-heal via re-put */
        CHECK(a840_system_cache_put_blob(bb_store, &k_trunc, b_good.data(), b_good.size()) == A840_SYS_CACHE_OK);
        CHECK(a840_system_cache_lookup(bb_store, &k_trunc, &rec, &tier) == A840_SYS_CACHE_OK);
        CHECK(tier == A840_CACHE_TIER_L1_SHARED);
        a840_record_free(&rec);
    }

    CASE("23_cache_corrupt_bitflip_crc_and_sha_detection");
    {
        A840PipelineKey k_flip;
        auto b_good = make_valid_record_blob(5005u, kStackKey, abi_fp, 32u, &k_flip);
        CHECK(a840_system_cache_put_blob(bb_store, &k_flip, b_good.data(), b_good.size()) == A840_SYS_CACHE_OK);

        std::string path = std::string(a840_system_cache_active_shared_dir(bb_store)) + "/" + key_to_filename(k_flip);
        FILE *f = std::fopen(path.c_str(), "r+b");
        CHECK(f != nullptr);
        CHECK(std::fseek(f, 50L, SEEK_SET) == 0);
        int ch = std::fgetc(f);
        CHECK(ch != EOF);
        CHECK(std::fseek(f, 50L, SEEK_SET) == 0);
        CHECK(std::fputc(ch ^ 0x80, f) != EOF);
        std::fclose(f);

        A840SystemCacheStats before_st, after_st;
        a840_system_cache_get_stats(bb_store, &before_st);
        A840PipelineRecord rec{};
        A840SystemCacheTier tier = A840_CACHE_TIER_MISS;
        CHECK(a840_system_cache_lookup(bb_store, &k_flip, &rec, &tier) == A840_SYS_CACHE_MISS);
        a840_system_cache_get_stats(bb_store, &after_st);
        CHECK(after_st.corrupt_rejected == before_st.corrupt_rejected + 1u);

        /* L0 pack.bin remains completely intact */
        A840PackV1Entry pe;
        CHECK(a840_system_cache_lookup_pack_view(bb_store, &rep1.entry_keys[0], &pe) == A840_SYS_CACHE_OK);
    }

    CASE("24_cache_symlink_attack_rejected");
    {
        A840PipelineKey k_sym;
        std::memset(&k_sym, 0x77, sizeof(k_sym));
        std::string path = std::string(a840_system_cache_active_shared_dir(bb_store)) + "/" + key_to_filename(k_sym);
        (void)unlink(path.c_str());
        CHECK(symlink(pack_file.c_str(), path.c_str()) == 0);

        /* App sandbox sees symlink -> rejects WITHOUT unlinking shared dir */
        A840SystemCacheConfig acfg{};
        acfg.stack_key = kStackKey; acfg.abi_fingerprint = abi_fp;
        acfg.role = A840_CACHE_ROLE_APP_SANDBOX;
        acfg.shared_cache_dir = shared_root.c_str();
        acfg.isolate_by_abi = true;
        acfg.quarantine_corrupt = true;
        A840SystemCacheStore *app_s = nullptr;
        CHECK(a840_system_cache_create(&acfg, &app_s) == A840_SYS_CACHE_OK);
        A840PipelineRecord rec{};
        A840SystemCacheTier tier = A840_CACHE_TIER_MISS;
        CHECK(a840_system_cache_lookup(app_s, &k_sym, &rec, &tier) == A840_SYS_CACHE_MISS);
        CHECK(file_exists(path)); /* App did not mutate shared dir */
        a840_system_cache_destroy(app_s);

        /* RenderServer rejects AND quarantines symlink */
        CHECK(a840_system_cache_lookup(bb_store, &k_sym, &rec, &tier) == A840_SYS_CACHE_MISS);
        CHECK(!file_exists(path));
    }

    CASE("25_cache_corrupt_pack_degrades_gracefully_to_l1");
    {
        A840SystemCacheConfig ccfg{};
        ccfg.stack_key = kStackKey;
        ccfg.abi_fingerprint = abi_fp;
        ccfg.role = A840_CACHE_ROLE_APP_SANDBOX;
        ccfg.pack_path = corrupt_pack_file.c_str();
        ccfg.shared_cache_dir = shared_root.c_str();
        ccfg.isolate_by_abi = true;
        A840SystemCacheStore *s = nullptr;
        CHECK(a840_system_cache_create(&ccfg, &s) == A840_SYS_CACHE_OK);
        CHECK(!a840_system_cache_has_pack(s));
        A840SystemCacheStats st;
        a840_system_cache_get_stats(s, &st);
        CHECK(st.corrupt_rejected >= 1u);

        /* L1 shared hit still works despite corrupt pack.bin */
        A840PipelineRecord rec{};
        A840SystemCacheTier tier = A840_CACHE_TIER_MISS;
        CHECK(a840_system_cache_lookup(s, &shared_key1, &rec, &tier) == A840_SYS_CACHE_OK);
        CHECK(tier == A840_CACHE_TIER_L1_SHARED);
        a840_record_free(&rec);
        a840_system_cache_destroy(s);
    }

    CASE("26_cache_legacy_raw_a840pr01_interop");
    {
        A840PipelineKey k_leg;
        auto b_leg = make_valid_record_blob(6006u, kStackKey, abi_fp, 32u, &k_leg);
        A840PipelineRecord parsed{};
        CHECK(a840_record_deserialize(b_leg.data(), b_leg.size(), kStackKey, abi_fp, &k_leg, &parsed) == A840_RECORD_OK);
        CHECK(a840_record_save_file(a840_system_cache_active_shared_dir(bb_store), &parsed) == A840_RECORD_OK);
        a840_record_free(&parsed);

        A840PipelineRecord got{};
        A840SystemCacheTier tier = A840_CACHE_TIER_MISS;
        CHECK(a840_system_cache_lookup(bb_store, &k_leg, &got, &tier) == A840_SYS_CACHE_OK);
        CHECK(tier == A840_CACHE_TIER_L1_SHARED);
        a840_record_free(&got);
    }

    CASE("27_cache_lru_eviction_enforces_byte_cap");
    {
        std::string lru_dir = work_dir + "/lru_cap_test";
        A840PipelineKey k_sample;
        auto b_sample = make_valid_record_blob(7000u, kStackKey, abi_fp, 32u, &k_sample);
        size_t entry_disk_size = b_sample.size() + A840_SYS_CACHE_ENVELOPE_BYTES;
        size_t cap_bytes = entry_disk_size * 3u + 64u; /* Room for exactly 3 entries */

        A840SystemCacheConfig lcfg{};
        lcfg.stack_key = kStackKey;
        lcfg.abi_fingerprint = abi_fp;
        lcfg.role = A840_CACHE_ROLE_RENDER_SERVER;
        lcfg.shared_cache_dir = lru_dir.c_str();
        lcfg.max_cache_bytes = cap_bytes;
        lcfg.isolate_by_abi = true;
        lcfg.quarantine_corrupt = true;
        A840SystemCacheStore *lstore = nullptr;
        CHECK(a840_system_cache_create(&lcfg, &lstore) == A840_SYS_CACHE_OK);

        std::vector<A840PipelineKey> keys(8u);
        for (uint32_t i = 0u; i < 8u; ++i) {
            auto blob = make_valid_record_blob(7100u + i, kStackKey, abi_fp, 32u, &keys[i]);
            CHECK(a840_system_cache_put_blob(lstore, &keys[i], blob.data(), blob.size()) == A840_SYS_CACHE_OK);
            /* Keep key[0] hot on step 2 so key[1] is evicted before key[0] */
            if (i == 2u) {
                A840PipelineRecord tmp{};
                A840SystemCacheTier t = A840_CACHE_TIER_MISS;
                CHECK(a840_system_cache_lookup(lstore, &keys[0], &tmp, &t) == A840_SYS_CACHE_OK);
                a840_record_free(&tmp);
            }
        }

        A840SystemCacheStats lst;
        a840_system_cache_get_stats(lstore, &lst);
        CHECK(lst.evictions >= 5u);
        CHECK(lst.l1_current_bytes <= lst.l1_max_bytes);

        /* Most recently inserted entry MUST survive */
        A840PipelineRecord last_rec{};
        A840SystemCacheTier t = A840_CACHE_TIER_MISS;
        CHECK(a840_system_cache_lookup(lstore, &keys[7], &last_rec, &t) == A840_SYS_CACHE_OK);
        a840_record_free(&last_rec);

        /* Oldest untouched entry key[1] MUST have been evicted */
        CHECK(a840_system_cache_lookup(lstore, &keys[1], &last_rec, &t) == A840_SYS_CACHE_MISS);
        a840_system_cache_destroy(lstore);
    }

    CASE("28_cache_put_rejects_corrupt_blob");
    {
        A840PipelineKey k_bad;
        auto b_bad = make_valid_record_blob(8008u, kStackKey, abi_fp, 32u, &k_bad);
        b_bad[b_bad.size() - 1u] ^= 0xffu;
        A840SystemCacheStats before_st, after_st;
        a840_system_cache_get_stats(bb_store, &before_st);
        CHECK(a840_system_cache_put_blob(bb_store, &k_bad, b_bad.data(), b_bad.size()) == A840_SYS_CACHE_CORRUPT);
        a840_system_cache_get_stats(bb_store, &after_st);
        CHECK(after_st.puts_shared == before_st.puts_shared);
        CHECK(after_st.corrupt_rejected == before_st.corrupt_rejected + 1u);
    }

    /* ==================== Group C: AF_UNIX Daemon & System E2E (Cases 29-38) ==================== */

    const std::string daemon_sock = make_safe_sock_path(work_dir, "d");
    const std::string daemon_cache = work_dir + "/daemon_shared_cache";
    TestDaemon daemon;
    start_test_daemon(daemon, daemon_sock, pack_file, daemon_cache, 2u);

    CASE("29_daemon_af_unix_ping_and_l0_pack_zero_compile_hit");
    {
        int fd = connect_unix_socket(daemon_sock);
        CHECK(fd >= 0);
        UnixClientTransport tr{fd};
        A840ScClientConfig cfg{};
        cfg.stack_key = kStackKey; cfg.abi_fingerprint = abi_fp;
        cfg.client_pid = 1001u; cfg.send_fn = unix_send_fn; cfg.recv_fn = unix_recv_fn; cfg.user_ctx = &tr;
        A840ScClient *cl = nullptr;
        CHECK(a840_sc_client_create(&cfg, &cl) == A840_SC_OK);

        CHECK(a840_sc_client_ping(cl, 2000u) == A840_SC_OK);

        /* Query a pack.bin key -> daemon serves directly from L0 pack.bin (0 compiles!) */
        uint8_t dummy_payload[16] = {0};
        uint8_t *blob = nullptr;
        size_t bytes = 0u;
        CHECK(a840_sc_client_compile_sync(cl, &rep1.entry_keys[0], dummy_payload,
                                          sizeof(dummy_payload), 2000u, &blob, &bytes) == A840_SC_OK);
        CHECK(blob != nullptr && bytes >= A840_RECORD_PREFIX_BYTES);
        A840PipelineRecord rec{};
        CHECK(a840_record_deserialize(blob, bytes, kStackKey, abi_fp, &rep1.entry_keys[0], &rec) == A840_RECORD_OK);
        a840_record_free(&rec);
        std::free(blob);

        CHECK(daemon.compile_calls.load() == 0u);
        A840SystemCacheStats cst;
        a840_system_cache_get_stats(daemon.cache_store, &cst);
        CHECK(cst.l0_pack_hits >= 1u);

        a840_sc_client_destroy(cl);
        (void)close(fd);
    }

    A840PipelineKey live_key;
    auto live_payload = encode_spirv_compute_payload(
        kLiveComputeSpvA, sizeof(kLiveComputeSpvA), "main", 1u, 1u, 1u, &live_key);

    CASE("30_daemon_af_unix_miss_live_compile_and_l1_backfill");
    {
        int fd = connect_unix_socket(daemon_sock);
        CHECK(fd >= 0);
        UnixClientTransport tr{fd};
        A840ScClientConfig cfg{};
        cfg.stack_key = kStackKey; cfg.abi_fingerprint = abi_fp;
        cfg.client_pid = 1002u; cfg.send_fn = unix_send_fn; cfg.recv_fn = unix_recv_fn; cfg.user_ctx = &tr;
        A840ScClient *cl = nullptr;
        CHECK(a840_sc_client_create(&cfg, &cl) == A840_SC_OK);

        uint8_t *blob = nullptr;
        size_t bytes = 0u;
        CHECK(a840_sc_client_compile_sync(cl, &live_key, live_payload.data(),
                                          live_payload.size(), 5000u, &blob, &bytes) == A840_SC_OK);
        A840PipelineRecord rec{};
        CHECK(a840_record_deserialize(blob, bytes, kStackKey, abi_fp, &live_key, &rec) == A840_RECORD_OK);
        CHECK(rec.stage_count == 1u && rec.stages[0].isa_dwords > 0u);
        a840_record_free(&rec);
        std::free(blob);

        CHECK(daemon.compile_calls.load() == 1u);
        A840SystemCacheStats cst;
        a840_system_cache_get_stats(daemon.cache_store, &cst);
        CHECK(cst.puts_shared >= 1u);

        a840_sc_client_destroy(cl);
        (void)close(fd);
    }

    CASE("31_daemon_restart_hits_persisted_l1_zero_recompile");
    {
        stop_test_daemon(daemon);
        CHECK(!file_exists(daemon_sock));
        start_test_daemon(daemon, daemon_sock, pack_file, daemon_cache, 2u);

        int fd = connect_unix_socket(daemon_sock);
        CHECK(fd >= 0);
        UnixClientTransport tr{fd};
        A840ScClientConfig cfg{};
        cfg.stack_key = kStackKey; cfg.abi_fingerprint = abi_fp;
        cfg.client_pid = 1003u; cfg.send_fn = unix_send_fn; cfg.recv_fn = unix_recv_fn; cfg.user_ctx = &tr;
        A840ScClient *cl = nullptr;
        CHECK(a840_sc_client_create(&cfg, &cl) == A840_SC_OK);

        uint8_t *blob = nullptr;
        size_t bytes = 0u;
        CHECK(a840_sc_client_compile_sync(cl, &live_key, live_payload.data(),
                                          live_payload.size(), 5000u, &blob, &bytes) == A840_SC_OK);
        std::free(blob);
        CHECK(daemon.compile_calls.load() == 0u);
        A840SystemCacheStats cst;
        a840_system_cache_get_stats(daemon.cache_store, &cst);
        CHECK(cst.l1_shared_hits >= 1u);

        a840_sc_client_destroy(cl);
        (void)close(fd);
    }

    CASE("32_daemon_backboardd_p0_urgent_preempts_p2_prewarm");
    {
        /* Slow down compile slightly so P0_URGENT catches in-flight P2_PREWARM for same key */
        daemon.delay_ms.store(60u);
        A840PipelineKey prio_key;
        auto prio_payload = encode_spirv_compute_payload(
            kLiveComputeSpvB, sizeof(kLiveComputeSpvB), "cs_tw2", 8u, 1u, 1u, &prio_key);

        int fd_bg = connect_unix_socket(daemon_sock);
        int fd_fg = connect_unix_socket(daemon_sock);
        CHECK(fd_bg >= 0 && fd_fg >= 0);
        UnixClientTransport t_bg{fd_bg}, t_fg{fd_fg};
        A840ScClientConfig c_bg{}, c_fg{};
        c_bg.stack_key = kStackKey; c_bg.abi_fingerprint = abi_fp; c_bg.client_pid = 2001u;
        c_bg.send_fn = unix_send_fn; c_bg.recv_fn = unix_recv_fn; c_bg.user_ctx = &t_bg;
        c_fg = c_bg; c_fg.client_pid = 2002u; c_fg.user_ctx = &t_fg;

        A840ScClient *cl_bg = nullptr, *cl_fg = nullptr;
        CHECK(a840_sc_client_create(&c_bg, &cl_bg) == A840_SC_OK);
        CHECK(a840_sc_client_create(&c_fg, &cl_fg) == A840_SC_OK);

        AsyncWaiter w_bg;
        uint64_t req_bg = 0u;
        CHECK(a840_sc_client_prewarm_async(cl_bg, &prio_key, prio_payload.data(),
                                           prio_payload.size(), 5000u,
                                           async_done_cb, &w_bg, &req_bg) == A840_SC_OK);
        sleep_ms(10u);

        uint8_t *fg_blob = nullptr;
        size_t fg_bytes = 0u;
        CHECK(a840_sc_client_compile_sync(cl_fg, &prio_key, prio_payload.data(),
                                          prio_payload.size(), 5000u,
                                          &fg_blob, &fg_bytes) == A840_SC_OK);
        std::free(fg_blob);
        wait_async(w_bg);
        CHECK(w_bg.status == A840_SC_OK && w_bg.blob_bytes >= A840_RECORD_PREFIX_BYTES);

        /* Both clients deduplicated to 1 real compile and triggered priority boost */
        CHECK(daemon.compile_calls.load() == 1u);
        A840ScServiceStats sst;
        a840_sc_service_get_stats(daemon.service, &sst);
        CHECK(sst.dedup_hits >= 1u);
        CHECK(sst.priority_boosts >= 1u);
        CHECK(daemon.priority_boost_seen.load() >= 1u);

        daemon.delay_ms.store(0u);
        a840_sc_client_destroy(cl_bg);
        a840_sc_client_destroy(cl_fg);
        (void)close(fd_bg);
        (void)close(fd_fg);
    }

    CASE("33_daemon_backboardd_plus_4_apps_end_to_end");
    {
        /* Simulate backboardd (shared R/W + P0_URGENT) and 4 sandboxed Apps (shared RO + private R/W + P1_NORMAL) */
        for (int app = 0; app < 4; ++app) {
            std::string pdir = work_dir + "/e2e_app_" + std::to_string(app);
            A840SystemCacheConfig acfg{};
            acfg.stack_key = kStackKey;
            acfg.abi_fingerprint = abi_fp;
            acfg.role = A840_CACHE_ROLE_APP_SANDBOX;
            acfg.pack_path = pack_file.c_str();
            acfg.shared_cache_dir = daemon_cache.c_str();
            acfg.private_cache_dir = pdir.c_str();
            acfg.isolate_by_abi = true;
            A840SystemCacheStore *as = nullptr;
            CHECK(a840_system_cache_create(&acfg, &as) == A840_SYS_CACHE_OK);

            /* App hits L0 pack.bin directly */
            A840PackV1Entry pe;
            CHECK(a840_system_cache_lookup_pack_view(as, &rep1.entry_keys[app * 3u], &pe) == A840_SYS_CACHE_OK);

            /* App hits L1 shared entry populated by daemon/backboardd */
            A840PipelineRecord rec{};
            A840SystemCacheTier tier = A840_CACHE_TIER_MISS;
            CHECK(a840_system_cache_lookup(as, &live_key, &rec, &tier) == A840_SYS_CACHE_OK);
            CHECK(tier == A840_CACHE_TIER_L1_SHARED);
            a840_record_free(&rec);

            /* App writes its own private cache entry via BG_ASYNC -> private container */
            int fd = connect_unix_socket(daemon_sock);
            CHECK(fd >= 0);
            UnixClientTransport tr{fd};
            A840ScClientConfig clc{};
            clc.stack_key = kStackKey; clc.abi_fingerprint = abi_fp;
            clc.client_pid = 3000u + static_cast<uint32_t>(app);
            clc.send_fn = unix_send_fn; clc.recv_fn = unix_recv_fn; clc.user_ctx = &tr;
            A840ScClient *cl = nullptr;
            CHECK(a840_sc_client_create(&clc, &cl) == A840_SC_OK);

            AsyncWaiter aw;
            uint64_t rid = 0u;
            CHECK(a840_sc_client_compile_async(cl, &live_key, live_payload.data(),
                                               live_payload.size(), 5000u,
                                               async_done_cb, &aw, &rid) == A840_SC_OK);
            wait_async(aw);
            CHECK(aw.status == A840_SC_OK);

            a840_sc_client_destroy(cl);
            (void)close(fd);
            a840_system_cache_destroy(as);
        }
    }

    CASE("34_daemon_corrupt_l1_self_heal_via_recompile");
    {
        /* Restart fresh daemon (empty mem cache), corrupt L1 file on disk, verify self-heal */
        stop_test_daemon(daemon);
        start_test_daemon(daemon, daemon_sock, pack_file, daemon_cache, 2u);

        std::string cached_file = std::string(a840_system_cache_active_shared_dir(daemon.cache_store)) +
                                  "/" + key_to_filename(live_key);
        CHECK(file_exists(cached_file));
        FILE *f = std::fopen(cached_file.c_str(), "wb");
        CHECK(f != nullptr);
        const char bad_data[] = "CORRUPTED_SHADER_CACHE_ENTRY";
        CHECK(std::fwrite(bad_data, 1u, sizeof(bad_data), f) == sizeof(bad_data));
        std::fclose(f);

        int fd = connect_unix_socket(daemon_sock);
        CHECK(fd >= 0);
        UnixClientTransport tr{fd};
        A840ScClientConfig cfg{};
        cfg.stack_key = kStackKey; cfg.abi_fingerprint = abi_fp;
        cfg.client_pid = 4001u; cfg.send_fn = unix_send_fn; cfg.recv_fn = unix_recv_fn; cfg.user_ctx = &tr;
        A840ScClient *cl = nullptr;
        CHECK(a840_sc_client_create(&cfg, &cl) == A840_SC_OK);

        uint8_t *blob = nullptr;
        size_t bytes = 0u;
        CHECK(a840_sc_client_compile_sync(cl, &live_key, live_payload.data(),
                                          live_payload.size(), 5000u, &blob, &bytes) == A840_SC_OK);
        std::free(blob);

        A840SystemCacheStats cst;
        a840_system_cache_get_stats(daemon.cache_store, &cst);
        CHECK(cst.corrupt_rejected >= 1u);
        CHECK(daemon.compile_calls.load() == 1u);

        /* Verify healed file now serves L1 hit cleanly */
        A840PipelineRecord healed{};
        A840SystemCacheTier tier = A840_CACHE_TIER_MISS;
        CHECK(a840_system_cache_lookup(daemon.cache_store, &live_key, &healed, &tier) == A840_SYS_CACHE_OK);
        CHECK(tier == A840_CACHE_TIER_L1_SHARED);
        a840_record_free(&healed);

        a840_sc_client_destroy(cl);
        (void)close(fd);
    }

    CASE("35_daemon_fault_isolation_malformed_frames");
    {
        int bad_fd = connect_unix_socket(daemon_sock);
        CHECK(bad_fd >= 0);
        uint8_t garbage[128];
        std::memset(garbage, 0xde, sizeof(garbage));
        (void)fd_write_all(bad_fd, garbage, sizeof(garbage), 500u);
        (void)close(bad_fd);

        int good_fd = connect_unix_socket(daemon_sock);
        CHECK(good_fd >= 0);
        UnixClientTransport tr{good_fd};
        A840ScClientConfig cfg{};
        cfg.stack_key = kStackKey; cfg.abi_fingerprint = abi_fp;
        cfg.client_pid = 4002u; cfg.send_fn = unix_send_fn; cfg.recv_fn = unix_recv_fn; cfg.user_ctx = &tr;
        A840ScClient *cl = nullptr;
        CHECK(a840_sc_client_create(&cfg, &cl) == A840_SC_OK);
        CHECK(a840_sc_client_ping(cl, 2000u) == A840_SC_OK);
        a840_sc_client_destroy(cl);
        (void)close(good_fd);
    }

    CASE("36_daemon_graceful_shutdown_unlinks_socket");
    {
        CHECK(file_exists(daemon_sock));
        stop_test_daemon(daemon);
        CHECK(!file_exists(daemon_sock));

        /* If standalone binary path was provided (--daemon-bin), test real fork/exec + SIGTERM */
        if (cli_daemon_bin && cli_daemon_bin[0]) {
            std::string ext_sock = make_safe_sock_path(work_dir, "e");
            std::string ext_cache = work_dir + "/ext_daemon_cache";
            pid_t pid = fork();
            CHECK(pid >= 0);
            if (pid == 0) {
                execl(cli_daemon_bin, cli_daemon_bin,
                      "--socket", ext_sock.c_str(),
                      "--pack", pack_file.c_str(),
                      "--cache-dir", ext_cache.c_str(),
                      "--max-cache-mb", "128",
                      "--workers", "2",
                      "--stack-key", "0x840c24a446120001",
                      static_cast<char *>(nullptr));
                _exit(127);
            }
            int fd = connect_unix_socket(ext_sock);
            CHECK(fd >= 0);
            UnixClientTransport tr{fd};
            A840ScClientConfig cfg{};
            cfg.stack_key = kStackKey; cfg.abi_fingerprint = abi_fp;
            cfg.client_pid = 5001u; cfg.send_fn = unix_send_fn; cfg.recv_fn = unix_recv_fn; cfg.user_ctx = &tr;
            A840ScClient *cl = nullptr;
            CHECK(a840_sc_client_create(&cfg, &cl) == A840_SC_OK);
            CHECK(a840_sc_client_ping(cl, 3000u) == A840_SC_OK);

            uint8_t *blob = nullptr;
            size_t bytes = 0u;
            uint8_t dummy[8] = {0};
            CHECK(a840_sc_client_compile_sync(cl, &rep1.entry_keys[0], dummy, sizeof(dummy),
                                              3000u, &blob, &bytes) == A840_SC_OK);
            CHECK(blob != nullptr && bytes >= A840_RECORD_PREFIX_BYTES);
            std::free(blob);

            CHECK(a840_sc_client_compile_sync(cl, &live_key, live_payload.data(), live_payload.size(),
                                              5000u, &blob, &bytes) == A840_SC_OK);
            CHECK(blob != nullptr && bytes >= A840_RECORD_PREFIX_BYTES);
            std::free(blob);

            a840_sc_client_destroy(cl);
            (void)close(fd);

            CHECK(kill(pid, SIGTERM) == 0);
            int wstatus = 0;
            CHECK(waitpid(pid, &wstatus, 0) == pid);
            CHECK(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);
            CHECK(!file_exists(ext_sock));
        }
    }

    CASE("37_plist_xml_validation_contract");
    {
        if (cli_plist_path && cli_plist_path[0]) {
            FILE *pf = std::fopen(cli_plist_path, "rb");
            CHECK(pf != nullptr);
            std::string content;
            char buf[512];
            size_t n = 0u;
            while ((n = std::fread(buf, 1u, sizeof(buf), pf)) > 0u)
                content.append(buf, n);
            std::fclose(pf);
            CHECK(content.find("/usr/sbin/NezhaShaderCompiler") != std::string::npos);
            CHECK(content.find("<integer>262144</integer>") != std::string::npos);
            CHECK(content.find("org.unixnu.nezha.shader-compiler") != std::string::npos);
            CHECK(content.find("/private/var/ngpu") == std::string::npos);
            CHECK(content.find("turnip") == std::string::npos);
            CHECK(content.find("Turnip") == std::string::npos);
        } else {
            CHECK(true);
        }
    }

    CASE("38_tsan_multithreaded_stress_16x");
    {
        std::string stress_sock = make_safe_sock_path(work_dir, "s");
        std::string stress_shared = work_dir + "/stress_shared";
        TestDaemon sdaemon;
        start_test_daemon(sdaemon, stress_sock, pack_file, stress_shared, 4u, 24u * 1024u);

        std::vector<A840PipelineKey> pkeys(rep1.entry_keys, rep1.entry_keys + rep1.entry_key_count);
        std::atomic<uint64_t> ops_ok{0u};
        constexpr uint32_t kWorkers = 5u; /* 1 backboardd + 4 Apps */
        pthread_t threads[kWorkers];
        StressWorkerArg args[kWorkers];

        for (uint32_t w = 0u; w < kWorkers; ++w) {
            args[w].worker_idx = w;
            args[w].rounds = tsan_stress_rounds;
            args[w].pack_path = pack_file;
            args[w].shared_dir = stress_shared;
            args[w].private_dir = work_dir + "/stress_priv_" + std::to_string(w);
            args[w].sock_path = stress_sock;
            args[w].pack_keys = &pkeys;
            args[w].shared_bb_store = sdaemon.cache_store;
            args[w].ops_ok = &ops_ok;
            CHECK(pthread_create(&threads[w], nullptr, stress_worker_main, &args[w]) == 0);
        }
        for (uint32_t w = 0u; w < kWorkers; ++w)
            pthread_join(threads[w], nullptr);

        CHECK(ops_ok.load() == static_cast<uint64_t>(kWorkers) * tsan_stress_rounds);
        A840SystemCacheStats sst;
        a840_system_cache_get_stats(sdaemon.cache_store, &sst);
        CHECK(sst.l0_pack_hits >= tsan_stress_rounds);
        stop_test_daemon(sdaemon);
    }

    a840_system_cache_destroy(bb_store);
    if (cleanup_work)
        rm_rf(work_dir);

    std::printf("PASS test_system_daemon_and_pack: %u cases, %" PRIu64 " checks, "
                "pack_entries=%u, fbfetch=%u, xr10=%u/%u, p99_us=%.3f, stress_rounds=%u, real_ir3=%s\n",
                g_cases, g_checks.load(), rep1.total_entries, rep1.fbfetch_stages,
                rep1.xr10_safe_entries, rep1.xr10_checked_entries,
                vrep.lookup_p99_us, tsan_stress_rounds,
#ifdef A840_TEST_WITH_REAL_IR3
                "true"
#else
                "host_stub_suite"
#endif
    );
    return 0;
}
