/* SPDX-License-Identifier: MIT */
/* Real End-to-End Test for NezhaShaderCompiler + NativeMetalTranslate +
 * Mesa ir3 Backend (W08-L2).
 *
 * Pure C11. Exercises:
 *   1. Payload encode/decode boundary validation & deterministic pipeline keys
 *   2. Real SPIR-V compute shader -> ir3 -> validated A840PR01 (non-empty ISA)
 *      over POSIX socketpair + A840ScClient (sync & async)
 *   3. Real 24A446 metallibs (MattingV2.metallib compute +
 *      NTKKaleidoscopeShaders.original.metallib render VS+FS) translated via
 *      libnative_metal_translate.a + compiled via libadreno_compile_ir3.a
 *      through A840ScService + A840ScClient over AF_UNIX stream socket
 *   4. Disk cache persistence across full service restart (zero recompile)
 *   5. Priority inheritance: FG_SYNC hitting an in-flight/queued BG_ASYNC job
 *      atomically triggers priority boost and deduplicates to 1 real compile
 *   6. Fault isolation: corrupt/truncated/malformed payloads fail cleanly
 *      without disrupting concurrent healthy real IR3 compiles
 *   7. Multi-threaded TSan concurrency stress (supports --tsan-stress N,
 *      default 1; gate runs 20x under GCC TSan with setarch x86_64 -R)
 */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif

#include "a840_shader_compiler_client.h"
#include "a840_shader_compiler_ir3_backend.h"
#include "a840_shader_compiler_service.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static const char k_compute_metallib_path[] =
    "/home/hua/nezha-work/private/ios27/camera27-c0/registration090-research/MattingV2.metallib";
static const char k_render_metallib_path[] =
    "/home/hua/nezha-work/ios27/gpu27-astra/private/metal-abi/NTKKaleidoscopeShaders.original.metallib";

static const uint64_t k_stack_key = UINT64_C(0x8408a84024a44608);
static const uint32_t k_client_pid = UINT32_C(0x00008402);

static _Atomic uint64_t g_checks = 0u;
static unsigned g_cases = 0u;
static const char *g_current_case = "";

#define CHECK(expr)                                                            \
    do {                                                                       \
        atomic_fetch_add(&g_checks, 1u);                                       \
        if (!(expr)) {                                                         \
            fprintf(stderr, "FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__,       \
                    g_current_case, #expr);                                    \
            abort();                                                           \
        }                                                                      \
    } while (0)

/* Minimal valid SPIR-V 1.0 GLSL compute shader: entry "main", LocalSize(1,1,1) */
static const uint32_t k_min_compute_spv[] = {
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

/* Second distinct SPIR-V 1.0 GLSL compute shader: entry "cs_alt", LocalSize(4,1,1) */
static const uint32_t k_alt_compute_spv[] = {
    0x07230203u, 0x00010000u, 0x0008000au, 0x00000006u, 0x00000000u,
    0x00020011u, 0x00000001u,
    0x0003000eu, 0x00000000u, 0x00000001u,
    0x0006000fu, 0x00000005u, 0x00000004u, 0x615f7363u, 0x0000746cu, 0x00000000u,
    0x00060010u, 0x00000004u, 0x00000011u, 0x00000004u, 0x00000001u, 0x00000001u,
    0x00020013u, 0x00000002u,
    0x00030021u, 0x00000003u, 0x00000002u,
    0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
    0x000200f8u, 0x00000005u,
    0x000100fdu,
    0x00010038u
};

static uint8_t *read_file_all(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) {
        free(buf);
        return NULL;
    }
    *out_len = got;
    return buf;
}

static bool write_all_timed(int fd, const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t sent = 0u;
    while (sent < len) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, (int)timeout_ms);
        if (pr <= 0) {
            if (pr < 0 && errno == EINTR)
                continue;
            return false;
        }
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        return false;
    }
    return true;
}

static int read_exact_timed(int fd, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t got = 0u;
    while (got < len) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, (int)timeout_ms);
        if (pr == 0)
            return (got == 0u) ? -2 : -1; /* -2 = idle timeout */
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        ssize_t n = read(fd, buf + got, len - got);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n == 0)
            return 0;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
        return -1;
    }
    return 1;
}

/* ---------------- Socket Transport for A840ScClient ---------------- */

typedef struct FdClientTransport {
    int fd;
} FdClientTransport;

static A840ScStatus fd_client_send(void *user_ctx, const uint8_t *frame,
    size_t frame_bytes, uint32_t timeout_ms)
{
    FdClientTransport *tr = (FdClientTransport *)user_ctx;
    if (tr->fd < 0)
        return A840_SC_DAEMON_UNAVAILABLE;
    return write_all_timed(tr->fd, frame, frame_bytes, timeout_ms ? timeout_ms : 1000u)
               ? A840_SC_OK
               : A840_SC_DAEMON_UNAVAILABLE;
}

static A840ScStatus fd_client_recv(void *user_ctx, uint8_t **out_frame,
    size_t *out_frame_bytes, uint32_t timeout_ms)
{
    FdClientTransport *tr = (FdClientTransport *)user_ctx;
    if (tr->fd < 0)
        return A840_SC_DAEMON_UNAVAILABLE;

    uint8_t hdr[A840_SC_HEADER_BYTES];
    int r = read_exact_timed(tr->fd, hdr, sizeof(hdr), timeout_ms);
    if (r == -2)
        return A840_SC_TIMED_OUT;
    if (r <= 0)
        return A840_SC_DAEMON_UNAVAILABLE;

    size_t total_len = 0u;
    A840ScStatus pst = a840_sc_peek_frame_length(hdr, sizeof(hdr), &total_len);
    if (pst != A840_SC_OK || total_len < A840_SC_HEADER_BYTES)
        return A840_SC_PROTO_ERROR;

    uint8_t *buf = (uint8_t *)malloc(total_len);
    if (!buf)
        return A840_SC_OOM;
    memcpy(buf, hdr, sizeof(hdr));
    if (total_len > A840_SC_HEADER_BYTES) {
        int br = read_exact_timed(tr->fd, buf + A840_SC_HEADER_BYTES,
            total_len - A840_SC_HEADER_BYTES, 5000u);
        if (br <= 0) {
            free(buf);
            return A840_SC_DAEMON_UNAVAILABLE;
        }
    }
    *out_frame = buf;
    *out_frame_bytes = total_len;
    return A840_SC_OK;
}

/* ---------------- In-Process Server Pump over Socketpair / AF_UNIX ---------------- */

typedef struct SocketPump {
    int fd;
    A840ScService *svc;
    pthread_mutex_t write_mutex;
    pthread_mutex_t state_mutex;
    pthread_cond_t state_cond;
    uint32_t pending;
    _Atomic bool stop;
    pthread_t thread;
} SocketPump;

static void pump_reply_cb(void *user_ctx, const uint8_t *frame, size_t frame_bytes)
{
    SocketPump *p = (SocketPump *)user_ctx;
    pthread_mutex_lock(&p->write_mutex);
    if (p->fd >= 0 && frame && frame_bytes > 0u)
        (void)write_all_timed(p->fd, frame, frame_bytes, 5000u);
    pthread_mutex_unlock(&p->write_mutex);

    pthread_mutex_lock(&p->state_mutex);
    if (p->pending > 0u)
        --p->pending;
    if (p->pending == 0u)
        pthread_cond_broadcast(&p->state_cond);
    pthread_mutex_unlock(&p->state_mutex);
}

static void *pump_thread_main(void *arg)
{
    SocketPump *p = (SocketPump *)arg;
    uint8_t hdr[A840_SC_HEADER_BYTES];

    while (!atomic_load(&p->stop)) {
        int r = read_exact_timed(p->fd, hdr, sizeof(hdr), 20u);
        if (r == -2)
            continue;
        if (r <= 0)
            break;

        size_t total = 0u;
        if (a840_sc_peek_frame_length(hdr, sizeof(hdr), &total) != A840_SC_OK)
            break;
        uint8_t *frame = (uint8_t *)malloc(total);
        if (!frame)
            break;
        memcpy(frame, hdr, sizeof(hdr));
        if (total > A840_SC_HEADER_BYTES) {
            if (read_exact_timed(p->fd, frame + A840_SC_HEADER_BYTES,
                    total - A840_SC_HEADER_BYTES, 5000u) <= 0) {
                free(frame);
                break;
            }
        }
        uint32_t msg_type = (uint32_t)frame[8] | ((uint32_t)frame[9] << 8) |
                            ((uint32_t)frame[10] << 16) | ((uint32_t)frame[11] << 24);
        bool expects_reply = (msg_type != A840_SC_MSG_CANCEL_REQUEST);
        if (expects_reply) {
            pthread_mutex_lock(&p->state_mutex);
            ++p->pending;
            pthread_mutex_unlock(&p->state_mutex);
        }
        A840ScStatus hst = a840_sc_service_handle_frame(p->svc, frame, total,
            expects_reply ? pump_reply_cb : NULL, p);
        free(frame);
        if (expects_reply &&
            (hst == A840_SC_INVALID_ARG || hst == A840_SC_PROTO_ERROR ||
             hst == A840_SC_TRUNCATED || hst == A840_SC_PAYLOAD_TOO_LARGE ||
             hst == A840_SC_STACK_MISMATCH || hst == A840_SC_ABI_MISMATCH ||
             hst == A840_SC_HASH_MISMATCH || hst == A840_SC_RESERVED_NONZERO ||
             hst == A840_SC_UNKNOWN_PRIORITY || hst == A840_SC_UNKNOWN_MSG_TYPE)) {
            pthread_mutex_lock(&p->state_mutex);
            if (p->pending > 0u)
                --p->pending;
            if (p->pending == 0u)
                pthread_cond_broadcast(&p->state_cond);
            pthread_mutex_unlock(&p->state_mutex);
            break;
        }
    }

    pthread_mutex_lock(&p->state_mutex);
    while (p->pending > 0u)
        pthread_cond_wait(&p->state_cond, &p->state_mutex);
    pthread_mutex_unlock(&p->state_mutex);
    return NULL;
}

static void pump_start(SocketPump *p, int fd, A840ScService *svc)
{
    memset(p, 0, sizeof(*p));
    p->fd = fd;
    p->svc = svc;
    atomic_store(&p->stop, false);
    pthread_mutex_init(&p->write_mutex, NULL);
    pthread_mutex_init(&p->state_mutex, NULL);
    pthread_cond_init(&p->state_cond, NULL);
    CHECK(pthread_create(&p->thread, NULL, pump_thread_main, p) == 0);
}

static void pump_stop(SocketPump *p)
{
    atomic_store(&p->stop, true);
    if (p->fd >= 0)
        (void)shutdown(p->fd, SHUT_RDWR);
    pthread_join(p->thread, NULL);
    if (p->fd >= 0) {
        (void)close(p->fd);
        p->fd = -1;
    }
    pthread_cond_destroy(&p->state_cond);
    pthread_mutex_destroy(&p->state_mutex);
    pthread_mutex_destroy(&p->write_mutex);
}

/* ---------------- Payload Builders ---------------- */

static uint8_t *build_spirv_compute_payload(const uint32_t *words, size_t bytes,
    const char *entry, uint32_t lx, uint32_t ly, uint32_t lz,
    size_t *out_len, A840PipelineKey *out_key)
{
    A840ScIr3CompilePayload hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = A840_SC_IR3_PAYLOAD_MAGIC;
    hdr.version = A840_SC_IR3_PAYLOAD_VERSION;
    hdr.pipeline_kind = A840_SC_IR3_PIPELINE_COMPUTE;
    hdr.source_kind = A840_SC_IR3_SRC_SPIRV;
    hdr.compile_flags = A840_COMPILE_FLAG_EARLY_PREAMBLE;
    hdr.sample_count = 1u;
    hdr.local_size[0] = lx;
    hdr.local_size[1] = ly;
    hdr.local_size[2] = lz;
    hdr.stage0_bytes = (uint32_t)bytes;
    strncpy(hdr.entry_name, entry, sizeof(hdr.entry_name) - 1u);

    size_t cap = A840_SC_IR3_PAYLOAD_HEADER_BYTES + bytes;
    uint8_t *buf = (uint8_t *)malloc(cap);
    CHECK(buf != NULL);
    CHECK(a840_sc_ir3_payload_encode(&hdr, words, NULL, buf, cap, out_len) == A840_SC_OK);
    CHECK(a840_sc_ir3_payload_key(buf, *out_len, out_key) == A840_SC_OK);
    return buf;
}

static uint8_t *build_metallib_compute_payload(const uint8_t *lib_bytes, size_t lib_len,
    const char *entry, size_t *out_len, A840PipelineKey *out_key)
{
    A840ScIr3CompilePayload hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = A840_SC_IR3_PAYLOAD_MAGIC;
    hdr.version = A840_SC_IR3_PAYLOAD_VERSION;
    hdr.pipeline_kind = A840_SC_IR3_PIPELINE_COMPUTE;
    hdr.source_kind = A840_SC_IR3_SRC_METALLIB;
    hdr.compile_flags = A840_COMPILE_FLAG_EARLY_PREAMBLE;
    hdr.sample_count = 1u;
    hdr.local_size[0] = 8u;
    hdr.local_size[1] = 8u;
    hdr.local_size[2] = 1u;
    hdr.stage0_bytes = (uint32_t)lib_len;
    strncpy(hdr.entry_name, entry, sizeof(hdr.entry_name) - 1u);

    size_t cap = A840_SC_IR3_PAYLOAD_HEADER_BYTES + lib_len;
    uint8_t *buf = (uint8_t *)malloc(cap);
    CHECK(buf != NULL);
    CHECK(a840_sc_ir3_payload_encode(&hdr, lib_bytes, NULL, buf, cap, out_len) == A840_SC_OK);
    CHECK(a840_sc_ir3_payload_key(buf, *out_len, out_key) == A840_SC_OK);
    return buf;
}

static uint8_t *build_metallib_render_payload(const uint8_t *lib_bytes, size_t lib_len,
    const char *vs_entry, const char *fs_entry, uint32_t color0_fmt,
    size_t *out_len, A840PipelineKey *out_key)
{
    A840ScIr3CompilePayload hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = A840_SC_IR3_PAYLOAD_MAGIC;
    hdr.version = A840_SC_IR3_PAYLOAD_VERSION;
    hdr.pipeline_kind = A840_SC_IR3_PIPELINE_RENDER_VS_FS;
    hdr.source_kind = A840_SC_IR3_SRC_METALLIB;
    hdr.compile_flags = A840_COMPILE_FLAG_EARLY_PREAMBLE;
    hdr.color_rt_mask = 1u;
    hdr.sample_count = 1u;
    hdr.color_mtl_formats[0] = color0_fmt;
    hdr.stage0_bytes = (uint32_t)lib_len;
    strncpy(hdr.entry_name, vs_entry, sizeof(hdr.entry_name) - 1u);
    strncpy(hdr.fs_entry_name, fs_entry, sizeof(hdr.fs_entry_name) - 1u);

    size_t cap = A840_SC_IR3_PAYLOAD_HEADER_BYTES + lib_len;
    uint8_t *buf = (uint8_t *)malloc(cap);
    CHECK(buf != NULL);
    CHECK(a840_sc_ir3_payload_encode(&hdr, lib_bytes, NULL, buf, cap, out_len) == A840_SC_OK);
    CHECK(a840_sc_ir3_payload_key(buf, *out_len, out_key) == A840_SC_OK);
    return buf;
}

/* ---------------- Async Completion Waiter ---------------- */

typedef struct AsyncWaiter {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool done;
    uint64_t request_id;
    A840ScStatus status;
    uint8_t *blob;
    size_t bytes;
} AsyncWaiter;

static void async_waiter_init(AsyncWaiter *w)
{
    memset(w, 0, sizeof(*w));
    pthread_mutex_init(&w->mutex, NULL);
    pthread_cond_init(&w->cond, NULL);
}

static void async_waiter_destroy(AsyncWaiter *w)
{
    free(w->blob);
    pthread_cond_destroy(&w->cond);
    pthread_mutex_destroy(&w->mutex);
}

static void async_waiter_cb(void *user_ctx, uint64_t request_id,
    A840ScStatus status, const uint8_t *record_blob, size_t record_bytes)
{
    AsyncWaiter *w = (AsyncWaiter *)user_ctx;
    pthread_mutex_lock(&w->mutex);
    w->request_id = request_id;
    w->status = status;
    if (status == A840_SC_OK && record_blob && record_bytes > 0u) {
        w->blob = (uint8_t *)malloc(record_bytes);
        if (w->blob) {
            memcpy(w->blob, record_blob, record_bytes);
            w->bytes = record_bytes;
        }
    }
    w->done = true;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->mutex);
}

static void async_waiter_wait(AsyncWaiter *w)
{
    pthread_mutex_lock(&w->mutex);
    while (!w->done)
        pthread_cond_wait(&w->cond, &w->mutex);
    pthread_mutex_unlock(&w->mutex);
}

/* ---------------- Test Cases ---------------- */

static void test_payload_validation(void)
{
    g_current_case = "payload_validation";

    size_t len = 0u;
    A840PipelineKey key1, key2;
    uint8_t *p1 = build_spirv_compute_payload(k_min_compute_spv, sizeof(k_min_compute_spv),
        "main", 1u, 1u, 1u, &len, &key1);
    uint8_t *p2 = build_spirv_compute_payload(k_min_compute_spv, sizeof(k_min_compute_spv),
        "main", 1u, 1u, 1u, &len, &key2);
    CHECK(memcmp(key1.sha256, key2.sha256, 32u) == 0);

    A840ScIr3CompilePayload decoded;
    const uint8_t *s0 = NULL;
    const uint8_t *s1 = NULL;
    CHECK(a840_sc_ir3_payload_decode(p1, len, &decoded, &s0, &s1) == A840_SC_OK);
    CHECK(decoded.pipeline_kind == A840_SC_IR3_PIPELINE_COMPUTE);
    CHECK(decoded.source_kind == A840_SC_IR3_SRC_SPIRV);
    CHECK(strcmp(decoded.entry_name, "main") == 0);
    CHECK(s0 != NULL && s1 == NULL);

    /* Truncation rejected */
    CHECK(a840_sc_ir3_payload_decode(p1, len - 1u, &decoded, &s0, &s1) == A840_SC_TRUNCATED);
    CHECK(a840_sc_ir3_payload_decode(p1, 100u, &decoded, &s0, &s1) == A840_SC_TRUNCATED);

    /* Bad magic rejected */
    p1[0] ^= 0xffu;
    CHECK(a840_sc_ir3_payload_decode(p1, len, &decoded, &s0, &s1) == A840_SC_PROTO_ERROR);
    p1[0] ^= 0xffu;

    /* Reserved non-zero rejected */
    p1[104] = 1u;
    CHECK(a840_sc_ir3_payload_decode(p1, len, &decoded, &s0, &s1) == A840_SC_RESERVED_NONZERO);
    p1[104] = 0u;

    /* Invalid SPIR-V magic rejected */
    p1[A840_SC_IR3_PAYLOAD_HEADER_BYTES] ^= 0x11u;
    CHECK(a840_sc_ir3_payload_decode(p1, len, &decoded, &s0, &s1) == A840_SC_PROTO_ERROR);

    free(p1);
    free(p2);
    ++g_cases;
}

static void verify_record_blob(const uint8_t *blob, size_t bytes,
    const A840PipelineKey *key, uint32_t expected_stages)
{
    CHECK(blob != NULL && bytes > A840_RECORD_PREFIX_BYTES);
    A840PipelineRecord rec;
    memset(&rec, 0, sizeof(rec));
    A840RecordStatus rst = a840_record_deserialize(blob, bytes,
        k_stack_key, a840_compile_abi_fingerprint(), key, &rec);
    CHECK(rst == A840_RECORD_OK);
    CHECK(rec.stage_count == expected_stages);
    for (uint32_t i = 0; i < expected_stages; ++i) {
        CHECK(rec.stages[i].isa_dwords > 0u);
        CHECK(rec.stages[i].isa_words != NULL);
        CHECK(rec.stages[i].instruction_groups > 0u);
    }
    a840_record_free(&rec);
}

static void test_spirv_socketpair_sync_async_and_disk_restart(const char *cache_dir)
{
    g_current_case = "spirv_socketpair_sync_async_and_disk_restart";
    const uint64_t abi_fp = a840_compile_abi_fingerprint();

    size_t p1_len = 0u, p2_len = 0u;
    A840PipelineKey k1, k2;
    uint8_t *p1 = build_spirv_compute_payload(k_min_compute_spv, sizeof(k_min_compute_spv),
        "main", 1u, 1u, 1u, &p1_len, &k1);
    uint8_t *p2 = build_spirv_compute_payload(k_alt_compute_spv, sizeof(k_alt_compute_spv),
        "cs_alt", 4u, 1u, 1u, &p2_len, &k2);

    /* Phase 1: cold compile via socketpair (sync + async) and persist to disk_cache_dir */
    {
        A840ScIr3BackendConfig bcfg = {
            .stack_key = k_stack_key,
            .abi_fingerprint = abi_fp,
            .nmb_device_flags = 0u,
            .default_compile_flags = A840_COMPILE_FLAG_EARLY_PREAMBLE,
            .enable_translate = true
        };
        A840ScIr3Backend *be = NULL;
        CHECK(a840_sc_ir3_backend_create(&bcfg, &be) == A840_SC_OK);

        A840ScServiceConfig scfg = {
            .stack_key = k_stack_key,
            .abi_fingerprint = abi_fp,
            .worker_count = 4u,
            .mem_cache_entries = 64u,
            .mem_cache_bytes = 8u * 1024u * 1024u,
            .disk_cache_dir = cache_dir,
            .compile_fn = a840_sc_ir3_compile_callback,
            .on_priority_boost = NULL,
            .user_ctx = be
        };
        A840ScService *svc = NULL;
        CHECK(a840_sc_service_create(&scfg, &svc) == A840_SC_OK);

        int sv[2] = {-1, -1};
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        SocketPump pump;
        pump_start(&pump, sv[1], svc);

        FdClientTransport tr = { .fd = sv[0] };
        A840ScClientConfig ccfg = {
            .stack_key = k_stack_key,
            .abi_fingerprint = abi_fp,
            .client_pid = k_client_pid,
            .initial_backoff_ms = 20u,
            .max_backoff_ms = 200u,
            .send_fn = fd_client_send,
            .recv_fn = fd_client_recv,
            .user_ctx = &tr
        };
        A840ScClient *client = NULL;
        CHECK(a840_sc_client_create(&ccfg, &client) == A840_SC_OK);
        CHECK(a840_sc_client_ping(client, 3000u) == A840_SC_OK);

        /* Sync compile k1 */
        uint8_t *blob1 = NULL;
        size_t blob1_len = 0u;
        CHECK(a840_sc_client_compile_sync(client, &k1, p1, p1_len, 10000u,
            &blob1, &blob1_len) == A840_SC_OK);
        verify_record_blob(blob1, blob1_len, &k1, 1u);
        free(blob1);

        /* Async compile k2 */
        AsyncWaiter w2;
        async_waiter_init(&w2);
        uint64_t req2 = 0u;
        CHECK(a840_sc_client_compile_async(client, &k2, p2, p2_len, 10000u,
            async_waiter_cb, &w2, &req2) == A840_SC_OK);
        CHECK(req2 != 0u);
        async_waiter_wait(&w2);
        CHECK(w2.status == A840_SC_OK);
        verify_record_blob(w2.blob, w2.bytes, &k2, 1u);
        async_waiter_destroy(&w2);

        A840ScServiceStats st;
        a840_sc_service_get_stats(svc, &st);
        CHECK(st.compiled_ok == 2u);
        CHECK(st.compiled_fail == 0u);

        a840_sc_client_destroy(client);
        (void)close(sv[0]);
        pump_stop(&pump);
        a840_sc_service_destroy(svc);
        a840_sc_ir3_backend_destroy(be);
    }

    /* Phase 2: fresh service instance with same disk_cache_dir -> zero recompile! */
    {
        A840ScIr3BackendConfig bcfg = {
            .stack_key = k_stack_key,
            .abi_fingerprint = abi_fp,
            .nmb_device_flags = 0u,
            .default_compile_flags = A840_COMPILE_FLAG_EARLY_PREAMBLE,
            .enable_translate = true
        };
        A840ScIr3Backend *be = NULL;
        CHECK(a840_sc_ir3_backend_create(&bcfg, &be) == A840_SC_OK);

        A840ScServiceConfig scfg = {
            .stack_key = k_stack_key,
            .abi_fingerprint = abi_fp,
            .worker_count = 2u,
            .mem_cache_entries = 64u,
            .mem_cache_bytes = 8u * 1024u * 1024u,
            .disk_cache_dir = cache_dir,
            .compile_fn = a840_sc_ir3_compile_callback,
            .on_priority_boost = NULL,
            .user_ctx = be
        };
        A840ScService *svc = NULL;
        CHECK(a840_sc_service_create(&scfg, &svc) == A840_SC_OK);

        int sv[2] = {-1, -1};
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        SocketPump pump;
        pump_start(&pump, sv[1], svc);

        FdClientTransport tr = { .fd = sv[0] };
        A840ScClientConfig ccfg = {
            .stack_key = k_stack_key,
            .abi_fingerprint = abi_fp,
            .client_pid = k_client_pid,
            .initial_backoff_ms = 20u,
            .max_backoff_ms = 200u,
            .send_fn = fd_client_send,
            .recv_fn = fd_client_recv,
            .user_ctx = &tr
        };
        A840ScClient *client = NULL;
        CHECK(a840_sc_client_create(&ccfg, &client) == A840_SC_OK);

        uint8_t *b1 = NULL, *b2 = NULL;
        size_t l1 = 0u, l2 = 0u;
        CHECK(a840_sc_client_compile_sync(client, &k1, p1, p1_len, 5000u, &b1, &l1) == A840_SC_OK);
        CHECK(a840_sc_client_compile_sync(client, &k2, p2, p2_len, 5000u, &b2, &l2) == A840_SC_OK);
        verify_record_blob(b1, l1, &k1, 1u);
        verify_record_blob(b2, l2, &k2, 1u);
        free(b1);
        free(b2);

        A840ScServiceStats st;
        a840_sc_service_get_stats(svc, &st);
        CHECK(st.cache_disk_hits == 2u);
        CHECK(st.compiled_ok == 0u);

        A840ScIr3BackendStats bst;
        a840_sc_ir3_backend_get_stats(be, &bst);
        CHECK(bst.total_calls == 0u);

        a840_sc_client_destroy(client);
        (void)close(sv[0]);
        pump_stop(&pump);
        a840_sc_service_destroy(svc);
        a840_sc_ir3_backend_destroy(be);
    }

    free(p1);
    free(p2);
    ++g_cases;
}

static void test_real_24a446_metallibs_over_af_unix(const char *work_dir)
{
    g_current_case = "real_24a446_metallibs_over_af_unix";
    const uint64_t abi_fp = a840_compile_abi_fingerprint();

    size_t comp_lib_len = 0u, rend_lib_len = 0u;
    uint8_t *comp_lib = read_file_all(k_compute_metallib_path, &comp_lib_len);
    uint8_t *rend_lib = read_file_all(k_render_metallib_path, &rend_lib_len);
    CHECK(comp_lib != NULL && comp_lib_len > 0u);
    CHECK(rend_lib != NULL && rend_lib_len > 0u);

    size_t cp_len = 0u, rp_len = 0u;
    A840PipelineKey ck, rk;
    uint8_t *cp = build_metallib_compute_payload(comp_lib, comp_lib_len,
        "copyTexture", &cp_len, &ck);
    uint8_t *rp = build_metallib_render_payload(rend_lib, rend_lib_len,
        "kal_fullscreen_vert", "kal_rosette_frag", 70u, &rp_len, &rk);
    free(comp_lib);
    free(rend_lib);

    A840ScIr3BackendConfig bcfg = {
        .stack_key = k_stack_key,
        .abi_fingerprint = abi_fp,
        .nmb_device_flags = 0u,
        .default_compile_flags = A840_COMPILE_FLAG_EARLY_PREAMBLE,
        .enable_translate = true
    };
    A840ScIr3Backend *be = NULL;
    CHECK(a840_sc_ir3_backend_create(&bcfg, &be) == A840_SC_OK);

    A840ScServiceConfig scfg = {
        .stack_key = k_stack_key,
        .abi_fingerprint = abi_fp,
        .worker_count = 4u,
        .mem_cache_entries = 64u,
        .mem_cache_bytes = 16u * 1024u * 1024u,
        .disk_cache_dir = NULL,
        .compile_fn = a840_sc_ir3_compile_callback,
        .on_priority_boost = NULL,
        .user_ctx = be
    };
    A840ScService *svc = NULL;
    CHECK(a840_sc_service_create(&scfg, &svc) == A840_SC_OK);

    /* Real AF_UNIX listener + client connect */
    char sock_path[104];
    int sn = snprintf(sock_path, sizeof(sock_path), "%s/e2e_ir3.sock", work_dir);
    CHECK(sn > 0 && (size_t)sn < sizeof(sock_path));
    (void)unlink(sock_path);

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(lfd >= 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, sock_path, (size_t)sn + 1u);
    CHECK(bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    CHECK(listen(lfd, 4) == 0);

    int cfd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(cfd >= 0);
    CHECK(connect(cfd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    int sfd = accept(lfd, NULL, NULL);
    CHECK(sfd >= 0);

    SocketPump pump;
    pump_start(&pump, sfd, svc);

    FdClientTransport tr = { .fd = cfd };
    A840ScClientConfig ccfg = {
        .stack_key = k_stack_key,
        .abi_fingerprint = abi_fp,
        .client_pid = k_client_pid,
        .initial_backoff_ms = 20u,
        .max_backoff_ms = 200u,
        .send_fn = fd_client_send,
        .recv_fn = fd_client_recv,
        .user_ctx = &tr
    };
    A840ScClient *client = NULL;
    CHECK(a840_sc_client_create(&ccfg, &client) == A840_SC_OK);

    /* Real 24A446 Compute metallib -> SPIR-V -> A840 ISA -> A840PR01 */
    uint8_t *cblob = NULL;
    size_t cblob_len = 0u;
    CHECK(a840_sc_client_compile_sync(client, &ck, cp, cp_len, 15000u,
        &cblob, &cblob_len) == A840_SC_OK);
    verify_record_blob(cblob, cblob_len, &ck, 1u);

    /* Real 24A446 Render VS+FS metallib -> SPIR-V -> A840 ISA -> A840PR01 */
    uint8_t *rblob = NULL;
    size_t rblob_len = 0u;
    CHECK(a840_sc_client_compile_sync(client, &rk, rp, rp_len, 15000u,
        &rblob, &rblob_len) == A840_SC_OK);
    verify_record_blob(rblob, rblob_len, &rk, 2u);

    /* Second fetch hits memory LRU cache */
    uint8_t *cblob2 = NULL;
    size_t cblob2_len = 0u;
    CHECK(a840_sc_client_compile_sync(client, &ck, cp, cp_len, 5000u,
        &cblob2, &cblob2_len) == A840_SC_OK);
    CHECK(cblob2_len == cblob_len && memcmp(cblob2, cblob, cblob_len) == 0);

    A840ScServiceStats st;
    a840_sc_service_get_stats(svc, &st);
    CHECK(st.compiled_ok == 2u);
    CHECK(st.cache_mem_hits == 1u);

    A840ScIr3BackendStats bst;
    a840_sc_ir3_backend_get_stats(be, &bst);
    CHECK(bst.compute_compiled == 1u);
    CHECK(bst.render_compiled == 1u);

    free(cblob);
    free(cblob2);
    free(rblob);
    free(cp);
    free(rp);

    a840_sc_client_destroy(client);
    (void)close(cfd);
    pump_stop(&pump);
    (void)close(lfd);
    (void)unlink(sock_path);
    a840_sc_service_destroy(svc);
    a840_sc_ir3_backend_destroy(be);
    ++g_cases;
}

/* ---------------- Priority Inheritance & Fault Isolation ---------------- */

typedef struct GatedIr3Ctx {
    A840ScIr3Backend *real_be;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool in_compile;
    bool release;
    _Atomic uint32_t boost_count;
    _Atomic uint32_t real_compiles;
} GatedIr3Ctx;

static A840ScStatus gated_compile_fn(void *user_ctx, uint32_t worker_id,
    const A840ScFrameHeader *request, const uint8_t *payload,
    uint8_t **out_record_blob, size_t *out_record_bytes)
{
    GatedIr3Ctx *g = (GatedIr3Ctx *)user_ctx;
    pthread_mutex_lock(&g->mu);
    g->in_compile = true;
    pthread_cond_broadcast(&g->cv);
    while (!g->release)
        pthread_cond_wait(&g->cv, &g->mu);
    pthread_mutex_unlock(&g->mu);

    atomic_fetch_add(&g->real_compiles, 1u);
    return a840_sc_ir3_compile_callback(g->real_be, worker_id, request, payload,
        out_record_blob, out_record_bytes);
}

static void gated_boost_cb(void *user_ctx, uint32_t worker_id,
    A840ScPriority from_prio, A840ScPriority to_prio)
{
    (void)worker_id;
    GatedIr3Ctx *g = (GatedIr3Ctx *)user_ctx;
    if (from_prio == A840_SC_PRIO_BG_ASYNC && to_prio == A840_SC_PRIO_FG_SYNC) {
        atomic_fetch_add(&g->boost_count, 1u);
        pthread_mutex_lock(&g->mu);
        g->release = true;
        pthread_cond_broadcast(&g->cv);
        pthread_mutex_unlock(&g->mu);
    }
}

static void test_priority_boost_and_fault_isolation(void)
{
    g_current_case = "priority_boost_and_fault_isolation";
    const uint64_t abi_fp = a840_compile_abi_fingerprint();

    A840ScIr3BackendConfig bcfg = {
        .stack_key = k_stack_key,
        .abi_fingerprint = abi_fp,
        .nmb_device_flags = 0u,
        .default_compile_flags = A840_COMPILE_FLAG_EARLY_PREAMBLE,
        .enable_translate = true
    };
    A840ScIr3Backend *be = NULL;
    CHECK(a840_sc_ir3_backend_create(&bcfg, &be) == A840_SC_OK);

    GatedIr3Ctx gctx;
    memset(&gctx, 0, sizeof(gctx));
    gctx.real_be = be;
    pthread_mutex_init(&gctx.mu, NULL);
    pthread_cond_init(&gctx.cv, NULL);
    atomic_store(&gctx.boost_count, 0u);
    atomic_store(&gctx.real_compiles, 0u);

    A840ScServiceConfig scfg = {
        .stack_key = k_stack_key,
        .abi_fingerprint = abi_fp,
        .worker_count = 2u,
        .mem_cache_entries = 64u,
        .mem_cache_bytes = 8u * 1024u * 1024u,
        .disk_cache_dir = NULL,
        .compile_fn = gated_compile_fn,
        .on_priority_boost = gated_boost_cb,
        .user_ctx = &gctx
    };
    A840ScService *svc = NULL;
    CHECK(a840_sc_service_create(&scfg, &svc) == A840_SC_OK);

    int sv[2] = {-1, -1};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    SocketPump pump;
    pump_start(&pump, sv[1], svc);

    FdClientTransport tr = { .fd = sv[0] };
    A840ScClientConfig ccfg = {
        .stack_key = k_stack_key,
        .abi_fingerprint = abi_fp,
        .client_pid = k_client_pid,
        .initial_backoff_ms = 20u,
        .max_backoff_ms = 200u,
        .send_fn = fd_client_send,
        .recv_fn = fd_client_recv,
        .user_ctx = &tr
    };
    A840ScClient *client = NULL;
    CHECK(a840_sc_client_create(&ccfg, &client) == A840_SC_OK);

    size_t p_len = 0u;
    A840PipelineKey key;
    uint8_t *p = build_spirv_compute_payload(k_min_compute_spv, sizeof(k_min_compute_spv),
        "main", 1u, 1u, 1u, &p_len, &key);

    /* 1. Submit BG_ASYNC first */
    AsyncWaiter bg_waiter;
    async_waiter_init(&bg_waiter);
    uint64_t bg_req_id = 0u;
    CHECK(a840_sc_client_compile_async(client, &key, p, p_len, 10000u,
        async_waiter_cb, &bg_waiter, &bg_req_id) == A840_SC_OK);

    /* Wait until worker has actually picked up the BG_ASYNC compile */
    pthread_mutex_lock(&gctx.mu);
    while (!gctx.in_compile)
        pthread_cond_wait(&gctx.cv, &gctx.mu);
    pthread_mutex_unlock(&gctx.mu);

    /* 2. Submit FG_SYNC for the exact same key -> triggers atomic boost & dedup */
    uint8_t *fg_blob = NULL;
    size_t fg_len = 0u;
    CHECK(a840_sc_client_compile_sync(client, &key, p, p_len, 10000u,
        &fg_blob, &fg_len) == A840_SC_OK);
    async_waiter_wait(&bg_waiter);

    CHECK(bg_waiter.status == A840_SC_OK);
    CHECK(fg_len == bg_waiter.bytes && memcmp(fg_blob, bg_waiter.blob, fg_len) == 0);
    verify_record_blob(fg_blob, fg_len, &key, 1u);
    CHECK(atomic_load(&gctx.boost_count) == 1u);
    CHECK(atomic_load(&gctx.real_compiles) == 1u);

    A840ScServiceStats st;
    a840_sc_service_get_stats(svc, &st);
    CHECK(st.priority_boosts == 1u);
    CHECK(st.dedup_hits == 1u);
    CHECK(st.compiled_ok == 1u);

    /* 3. Fault isolation: send invalid payload (garbage bytes) -> COMPILE_FAILED */
    A840PipelineKey bad_key;
    memset(&bad_key, 0xee, sizeof(bad_key));
    const uint8_t garbage[64] = {0xde, 0xad, 0xbe, 0xef};
    uint8_t *bad_blob = NULL;
    size_t bad_len = 0u;
    A840ScStatus bad_st = a840_sc_client_compile_sync(client, &bad_key,
        garbage, sizeof(garbage), 5000u, &bad_blob, &bad_len);
    CHECK(bad_st == A840_SC_COMPILE_FAILED);
    CHECK(bad_blob == NULL && bad_len == 0u);

    /* 4. Healthy request after fault succeeds cleanly */
    size_t p2_len = 0u;
    A840PipelineKey k2;
    uint8_t *p2 = build_spirv_compute_payload(k_alt_compute_spv, sizeof(k_alt_compute_spv),
        "cs_alt", 4u, 1u, 1u, &p2_len, &k2);
    uint8_t *ok_blob = NULL;
    size_t ok_len = 0u;
    CHECK(a840_sc_client_compile_sync(client, &k2, p2, p2_len, 10000u,
        &ok_blob, &ok_len) == A840_SC_OK);
    verify_record_blob(ok_blob, ok_len, &k2, 1u);

    free(ok_blob);
    free(p2);
    free(fg_blob);
    free(p);
    async_waiter_destroy(&bg_waiter);

    a840_sc_client_destroy(client);
    (void)close(sv[0]);
    pump_stop(&pump);
    a840_sc_service_destroy(svc);
    pthread_cond_destroy(&gctx.cv);
    pthread_mutex_destroy(&gctx.mu);
    a840_sc_ir3_backend_destroy(be);
    ++g_cases;
}

/* ---------------- TSan Concurrent Stress ---------------- */

typedef struct StressWorkerArg {
    A840ScService *svc;
    const uint8_t *p_good1;
    size_t p_good1_len;
    A840PipelineKey k_good1;
    const uint8_t *p_good2;
    size_t p_good2_len;
    A840PipelineKey k_good2;
    uint32_t thread_idx;
    uint32_t rounds;
} StressWorkerArg;

typedef struct SyncSubmitWait {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool done;
    A840ScStatus st;
    size_t bytes;
} SyncSubmitWait;

static void sync_submit_cb(void *ctx, const A840ScFrameHeader *reply,
    const uint8_t *blob, size_t bytes)
{
    (void)blob;
    SyncSubmitWait *w = (SyncSubmitWait *)ctx;
    pthread_mutex_lock(&w->mu);
    w->st = (A840ScStatus)reply->status;
    w->bytes = bytes;
    w->done = true;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->mu);
}

static void *stress_caller_thread(void *arg)
{
    StressWorkerArg *a = (StressWorkerArg *)arg;
    const uint64_t abi_fp = a840_compile_abi_fingerprint();

    for (uint32_t r = 0; r < a->rounds; ++r) {
        uint32_t sel = (r + a->thread_idx) % 3u;
        A840ScFrameHeader req;
        memset(&req, 0, sizeof(req));
        req.magic = A840_SC_PROTO_MAGIC;
        req.version = A840_SC_PROTO_VERSION;
        req.msg_type = A840_SC_MSG_COMPILE_REQUEST;
        req.priority_class = (a->thread_idx % 2u == 0u) ? A840_SC_PRIO_FG_SYNC
                                                        : A840_SC_PRIO_BG_ASYNC;
        req.stack_key = k_stack_key;
        req.abi_fingerprint = abi_fp;
        req.request_id = ((uint64_t)(a->thread_idx + 1u) << 32) | (uint64_t)(r + 1u);
        req.client_pid = k_client_pid + a->thread_idx;

        const uint8_t *payload = NULL;
        uint8_t bad_buf[16] = {1, 2, 3, 4};
        if (sel == 0u) {
            req.pipeline_key = a->k_good1;
            req.payload_bytes = (uint32_t)a->p_good1_len;
            payload = a->p_good1;
        } else if (sel == 1u) {
            req.pipeline_key = a->k_good2;
            req.payload_bytes = (uint32_t)a->p_good2_len;
            payload = a->p_good2;
        } else {
            memset(req.pipeline_key.sha256, (int)(0xa0u + (r & 0x1fu)), 32u);
            req.payload_bytes = (uint32_t)sizeof(bad_buf);
            payload = bad_buf;
        }

        SyncSubmitWait w;
        memset(&w, 0, sizeof(w));
        pthread_mutex_init(&w.mu, NULL);
        pthread_cond_init(&w.cv, NULL);

        A840ScStatus sst = a840_sc_service_submit(a->svc, &req, payload,
            sync_submit_cb, &w);
        CHECK(sst == A840_SC_OK);

        pthread_mutex_lock(&w.mu);
        while (!w.done)
            pthread_cond_wait(&w.cv, &w.mu);
        pthread_mutex_unlock(&w.mu);

        if (sel < 2u) {
            CHECK(w.st == A840_SC_OK && w.bytes > A840_RECORD_PREFIX_BYTES);
        } else {
            CHECK(w.st == A840_SC_COMPILE_FAILED);
        }
        pthread_cond_destroy(&w.cv);
        pthread_mutex_destroy(&w.mu);
    }
    return NULL;
}

static void test_tsan_concurrency_stress(uint32_t stress_rounds)
{
    g_current_case = "tsan_concurrency_stress";
    const uint64_t abi_fp = a840_compile_abi_fingerprint();

    size_t p1_len = 0u, p2_len = 0u;
    A840PipelineKey k1, k2;
    uint8_t *p1 = build_spirv_compute_payload(k_min_compute_spv, sizeof(k_min_compute_spv),
        "main", 1u, 1u, 1u, &p1_len, &k1);
    uint8_t *p2 = build_spirv_compute_payload(k_alt_compute_spv, sizeof(k_alt_compute_spv),
        "cs_alt", 4u, 1u, 1u, &p2_len, &k2);

    for (uint32_t iter = 0; iter < stress_rounds; ++iter) {
        A840ScIr3BackendConfig bcfg = {
            .stack_key = k_stack_key,
            .abi_fingerprint = abi_fp,
            .nmb_device_flags = 0u,
            .default_compile_flags = A840_COMPILE_FLAG_EARLY_PREAMBLE,
            .enable_translate = true
        };
        A840ScIr3Backend *be = NULL;
        CHECK(a840_sc_ir3_backend_create(&bcfg, &be) == A840_SC_OK);

        A840ScServiceConfig scfg = {
            .stack_key = k_stack_key,
            .abi_fingerprint = abi_fp,
            .worker_count = 4u,
            .mem_cache_entries = 16u,
            .mem_cache_bytes = 4u * 1024u * 1024u,
            .disk_cache_dir = NULL,
            .compile_fn = a840_sc_ir3_compile_callback,
            .on_priority_boost = NULL,
            .user_ctx = be
        };
        A840ScService *svc = NULL;
        CHECK(a840_sc_service_create(&scfg, &svc) == A840_SC_OK);

        enum { NUM_CALLERS = 6 };
        pthread_t threads[NUM_CALLERS];
        StressWorkerArg args[NUM_CALLERS];
        for (uint32_t t = 0; t < NUM_CALLERS; ++t) {
            args[t].svc = svc;
            args[t].p_good1 = p1;
            args[t].p_good1_len = p1_len;
            args[t].k_good1 = k1;
            args[t].p_good2 = p2;
            args[t].p_good2_len = p2_len;
            args[t].k_good2 = k2;
            args[t].thread_idx = t;
            args[t].rounds = 6u;
            CHECK(pthread_create(&threads[t], NULL, stress_caller_thread, &args[t]) == 0);
        }
        for (uint32_t t = 0; t < NUM_CALLERS; ++t)
            pthread_join(threads[t], NULL);

        A840ScServiceStats st;
        a840_sc_service_get_stats(svc, &st);
        CHECK(st.compiled_ok == 2u);
        CHECK(st.compiled_fail > 0u);

        a840_sc_service_destroy(svc);
        a840_sc_ir3_backend_destroy(be);
    }

    free(p1);
    free(p2);
    ++g_cases;
}

int main(int argc, char **argv)
{
    uint32_t stress_rounds = 1u;
    const char *work_dir = NULL;
    bool skip_metallib = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--tsan-stress") == 0 && i + 1 < argc) {
            stress_rounds = (uint32_t)strtoul(argv[++i], NULL, 10);
            if (stress_rounds == 0u)
                stress_rounds = 1u;
        } else if (strcmp(argv[i], "--work-dir") == 0 && i + 1 < argc) {
            work_dir = argv[++i];
        } else if (strcmp(argv[i], "--skip-metallib") == 0) {
            skip_metallib = true;
        } else {
            fprintf(stderr, "usage: %s [--work-dir <dir>] [--tsan-stress <N>] [--skip-metallib]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    (void)signal(SIGPIPE, SIG_IGN);

    char default_dir[256];
    if (!work_dir) {
        const char *tmp = getenv("TMPDIR");
        if (!tmp || !tmp[0])
            tmp = "/tmp";
        snprintf(default_dir, sizeof(default_dir), "%s/a840_ir3_e2e_%lu",
                 tmp, (unsigned long)getpid());
        (void)mkdir(default_dir, 0700);
        work_dir = default_dir;
    }

    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/disk_cache_%lu",
             work_dir, (unsigned long)getpid());
    (void)mkdir(cache_dir, 0700);

    test_payload_validation();
    test_spirv_socketpair_sync_async_and_disk_restart(cache_dir);
    if (!skip_metallib)
        test_real_24a446_metallibs_over_af_unix(work_dir);
    test_priority_boost_and_fault_isolation();
    test_tsan_concurrency_stress(stress_rounds);

    printf("PASS test_compiler_service_ir3_e2e: %u cases, %" PRIu64 " checks (stress_rounds=%u)\n",
           g_cases, (uint64_t)atomic_load(&g_checks), stress_rounds);
    return EXIT_SUCCESS;
}
