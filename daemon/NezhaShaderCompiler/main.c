/* SPDX-License-Identifier: MIT */
/* Standalone NezhaShaderCompiler AF_UNIX Daemon (W08-L2).
 *
 * Pure C11, zero Vulkan headers/symbols, Darwin-compatible (POSIX AF_UNIX,
 * poll(2), pthread, sigaction). Accepts multiple concurrent clients over a
 * UNIX domain stream socket, reads framed '8SC1' requests, dispatches them
 * through a840_sc_service_handle_frame backed by NativeMetalTranslate + real
 * Mesa ir3 (a840_sc_ir3_compile_callback), and shuts down cleanly on
 * SIGTERM/SIGINT (unlinking the socket file with zero memory leaks).
 */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif

#include "a840_shader_compiler_ir3_backend.h"
#include "a840_shader_compiler_service.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

enum {
    DAEMON_DEFAULT_WORKERS = 2u,
    DAEMON_DEFAULT_CACHE_ENTRIES = 1024u,
    DAEMON_DEFAULT_CACHE_BYTES = 64u * 1024u * 1024u,
    DAEMON_MAX_CLIENTS = 64u,
    DAEMON_POLL_MS = 50,
    DAEMON_BACKLOG = 32
};

#define DAEMON_DEFAULT_STACK_KEY UINT64_C(0x8400a84024a44601)

static volatile sig_atomic_t g_stop_requested = 0;

static void handle_stop_signal(int sig)
{
    (void)sig;
    g_stop_requested = 1;
}

typedef struct DaemonOptions {
    const char *socket_path;
    const char *cache_dir;
    uint32_t workers;
    uint64_t stack_key;
    bool once_mode;
    bool selftest_mode;
} DaemonOptions;

typedef struct ClientConn {
    int fd;
    pthread_mutex_t write_mutex;
    pthread_mutex_t state_mutex;
    pthread_cond_t state_cond;
    uint32_t pending_replies;
    bool closed;
    A840ScService *service;
    pthread_t thread;
    bool active;
} ClientConn;

typedef struct ReplyCtx {
    ClientConn *conn;
    bool track_pending;
} ReplyCtx;

static bool write_all_fd(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0u;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, DAEMON_POLL_MS);
            if (pr < 0 && errno != EINTR)
                return false;
            if (g_stop_requested)
                return false;
            continue;
        }
        return false;
    }
    return true;
}

/* Read exactly len bytes; returns 1 on success, 0 on clean EOF at offset 0,
 * -1 on error, truncation, or stop request.
 */
static int read_exact_fd(int fd, uint8_t *buf, size_t len)
{
    size_t got = 0u;
    while (got < len) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, DAEMON_POLL_MS);
        if (pr < 0) {
            if (errno == EINTR) {
                if (g_stop_requested)
                    return -1;
                continue;
            }
            return -1;
        }
        if (pr == 0) {
            if (g_stop_requested)
                return -1;
            continue;
        }
        ssize_t n = read(fd, buf + got, len - got);
        if (n > 0) {
            got += (size_t)n;
            continue;
        }
        if (n == 0)
            return (got == 0u) ? 0 : -1;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
        return -1;
    }
    return 1;
}

static void client_reply_callback(void *user_ctx, const uint8_t *frame,
    size_t frame_bytes)
{
    ReplyCtx *rctx = (ReplyCtx *)user_ctx;
    ClientConn *conn = rctx->conn;

    pthread_mutex_lock(&conn->write_mutex);
    if (!conn->closed && conn->fd >= 0 && frame && frame_bytes > 0u) {
        if (!write_all_fd(conn->fd, frame, frame_bytes))
            conn->closed = true;
    }
    pthread_mutex_unlock(&conn->write_mutex);

    if (rctx->track_pending) {
        pthread_mutex_lock(&conn->state_mutex);
        if (conn->pending_replies > 0u)
            --conn->pending_replies;
        if (conn->pending_replies == 0u)
            pthread_cond_broadcast(&conn->state_cond);
        pthread_mutex_unlock(&conn->state_mutex);
    }
    free(rctx);
}

static void *client_thread_main(void *arg)
{
    ClientConn *conn = (ClientConn *)arg;
    uint8_t hdr_buf[A840_SC_HEADER_BYTES];

    for (;;) {
        int r = read_exact_fd(conn->fd, hdr_buf, sizeof(hdr_buf));
        if (r <= 0)
            break;

        size_t total_len = 0u;
        A840ScStatus pst = a840_sc_peek_frame_length(hdr_buf, sizeof(hdr_buf), &total_len);
        if (pst != A840_SC_OK || total_len < A840_SC_HEADER_BYTES)
            break;

        uint8_t *frame = (uint8_t *)malloc(total_len);
        if (!frame)
            break;
        memcpy(frame, hdr_buf, sizeof(hdr_buf));

        if (total_len > A840_SC_HEADER_BYTES) {
            int br = read_exact_fd(conn->fd, frame + A840_SC_HEADER_BYTES,
                total_len - A840_SC_HEADER_BYTES);
            if (br <= 0) {
                free(frame);
                break;
            }
        }

        /* Peek msg_type (little-endian uint32 at offset 8) to know whether a
         * reply callback will be invoked by a840_sc_service_handle_frame.
         */
        uint32_t msg_type = (uint32_t)frame[8] |
                            ((uint32_t)frame[9] << 8) |
                            ((uint32_t)frame[10] << 16) |
                            ((uint32_t)frame[11] << 24);
        bool expects_reply = (msg_type == A840_SC_MSG_COMPILE_REQUEST ||
                              msg_type == A840_SC_MSG_PREWARM_HINT ||
                              msg_type == A840_SC_MSG_PING_STATUS);

        ReplyCtx *rctx = NULL;
        if (expects_reply) {
            rctx = (ReplyCtx *)calloc(1, sizeof(*rctx));
            if (!rctx) {
                free(frame);
                break;
            }
            rctx->conn = conn;
            rctx->track_pending = true;
            pthread_mutex_lock(&conn->state_mutex);
            ++conn->pending_replies;
            pthread_mutex_unlock(&conn->state_mutex);
        }

        A840ScStatus hst = a840_sc_service_handle_frame(conn->service,
            frame, total_len,
            expects_reply ? client_reply_callback : NULL,
            rctx);
        free(frame);

        /* On decode failure before dispatch (e.g. bad hash/stack/abi),
         * a840_sc_service_handle_frame does NOT invoke reply_fn.
         */
        if (expects_reply &&
            (hst == A840_SC_INVALID_ARG || hst == A840_SC_PROTO_ERROR ||
             hst == A840_SC_TRUNCATED || hst == A840_SC_PAYLOAD_TOO_LARGE ||
             hst == A840_SC_STACK_MISMATCH || hst == A840_SC_ABI_MISMATCH ||
             hst == A840_SC_HASH_MISMATCH || hst == A840_SC_RESERVED_NONZERO ||
             hst == A840_SC_UNKNOWN_PRIORITY || hst == A840_SC_UNKNOWN_MSG_TYPE)) {
            pthread_mutex_lock(&conn->state_mutex);
            if (conn->pending_replies > 0u)
                --conn->pending_replies;
            if (conn->pending_replies == 0u)
                pthread_cond_broadcast(&conn->state_cond);
            pthread_mutex_unlock(&conn->state_mutex);
            free(rctx);
            break;
        }
    }

    /* Wait for any in-flight compile replies to finish before closing fd. */
    pthread_mutex_lock(&conn->state_mutex);
    while (conn->pending_replies > 0u)
        pthread_cond_wait(&conn->state_cond, &conn->state_mutex);
    pthread_mutex_unlock(&conn->state_mutex);

    pthread_mutex_lock(&conn->write_mutex);
    conn->closed = true;
    if (conn->fd >= 0) {
        (void)close(conn->fd);
        conn->fd = -1;
    }
    pthread_mutex_unlock(&conn->write_mutex);
    return NULL;
}

/* Minimal valid SPIR-V 1.0 GLSL compute shader: OpEntryPoint GLCompute %main "main"
 * with LocalSize (1, 1, 1) and OpReturn. Used by --selftest to verify the real
 * Mesa ir3 backend + A840ScService + A840PR01 roundtrip with zero external files.
 */
static const uint32_t k_selftest_compute_spv[] = {
    0x07230203u, 0x00010000u, 0x0008000au, 0x00000006u, 0x00000000u,
    /* OpCapability Shader */
    0x00020011u, 0x00000001u,
    /* OpMemoryModel Logical GLSL450 */
    0x0003000eu, 0x00000000u, 0x00000001u,
    /* OpEntryPoint GLCompute %4 "main" */
    0x0005000fu, 0x00000005u, 0x00000004u, 0x6e69616du, 0x00000000u,
    /* OpExecutionMode %4 LocalSize 1 1 1 */
    0x00060010u, 0x00000004u, 0x00000011u, 0x00000001u, 0x00000001u, 0x00000001u,
    /* %2 = OpTypeVoid */
    0x00020013u, 0x00000002u,
    /* %3 = OpTypeFunction %2 */
    0x00030021u, 0x00000003u, 0x00000002u,
    /* %4 = OpFunction %2 None %3 */
    0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
    /* %5 = OpLabel */
    0x000200f8u, 0x00000005u,
    /* OpReturn */
    0x000100fdu,
    /* OpFunctionEnd */
    0x00010038u
};

typedef struct SelftestResult {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool done;
    A840ScStatus status;
    uint8_t *blob;
    size_t bytes;
} SelftestResult;

static void selftest_completion(void *user_ctx, const A840ScFrameHeader *reply_hdr,
    const uint8_t *record_blob, size_t record_bytes)
{
    SelftestResult *res = (SelftestResult *)user_ctx;
    pthread_mutex_lock(&res->mutex);
    res->status = (A840ScStatus)reply_hdr->status;
    if (res->status == A840_SC_OK && record_blob && record_bytes > 0u) {
        res->blob = (uint8_t *)malloc(record_bytes);
        if (res->blob) {
            memcpy(res->blob, record_blob, record_bytes);
            res->bytes = record_bytes;
        } else {
            res->status = A840_SC_OOM;
        }
    }
    res->done = true;
    pthread_cond_signal(&res->cond);
    pthread_mutex_unlock(&res->mutex);
}

static int run_selftest(const DaemonOptions *opts)
{
    const uint64_t abi_fp = a840_compile_abi_fingerprint();
    A840ScIr3BackendConfig bcfg;
    memset(&bcfg, 0, sizeof(bcfg));
    bcfg.stack_key = opts->stack_key;
    bcfg.abi_fingerprint = abi_fp;
    bcfg.default_compile_flags = A840_COMPILE_FLAG_EARLY_PREAMBLE;
    bcfg.enable_translate = true;

    A840ScIr3Backend *backend = NULL;
    A840ScStatus st = a840_sc_ir3_backend_create(&bcfg, &backend);
    if (st != A840_SC_OK) {
        fprintf(stderr, "NezhaShaderCompiler selftest: backend_create failed: %s\n",
                a840_sc_status_string(st));
        return EXIT_FAILURE;
    }

    A840ScServiceConfig scfg;
    memset(&scfg, 0, sizeof(scfg));
    scfg.stack_key = opts->stack_key;
    scfg.abi_fingerprint = abi_fp;
    scfg.worker_count = opts->workers;
    scfg.mem_cache_entries = 64u;
    scfg.mem_cache_bytes = 4u * 1024u * 1024u;
    scfg.disk_cache_dir = opts->cache_dir;
    scfg.compile_fn = a840_sc_ir3_compile_callback;
    scfg.user_ctx = backend;

    A840ScService *svc = NULL;
    st = a840_sc_service_create(&scfg, &svc);
    if (st != A840_SC_OK) {
        a840_sc_ir3_backend_destroy(backend);
        fprintf(stderr, "NezhaShaderCompiler selftest: service_create failed: %s\n",
                a840_sc_status_string(st));
        return EXIT_FAILURE;
    }

    A840ScIr3CompilePayload p;
    memset(&p, 0, sizeof(p));
    p.magic = A840_SC_IR3_PAYLOAD_MAGIC;
    p.version = A840_SC_IR3_PAYLOAD_VERSION;
    p.pipeline_kind = A840_SC_IR3_PIPELINE_COMPUTE;
    p.source_kind = A840_SC_IR3_SRC_SPIRV;
    p.compile_flags = A840_COMPILE_FLAG_EARLY_PREAMBLE;
    p.sample_count = 1u;
    p.local_size[0] = 1u;
    p.local_size[1] = 1u;
    p.local_size[2] = 1u;
    p.stage0_bytes = (uint32_t)sizeof(k_selftest_compute_spv);
    memcpy(p.entry_name, "main", 5u);

    uint8_t encoded[A840_SC_IR3_PAYLOAD_HEADER_BYTES + sizeof(k_selftest_compute_spv)];
    size_t encoded_len = 0u;
    st = a840_sc_ir3_payload_encode(&p, k_selftest_compute_spv, NULL,
        encoded, sizeof(encoded), &encoded_len);
    if (st != A840_SC_OK) {
        a840_sc_service_destroy(svc);
        a840_sc_ir3_backend_destroy(backend);
        fprintf(stderr, "NezhaShaderCompiler selftest: payload_encode failed\n");
        return EXIT_FAILURE;
    }

    A840PipelineKey key;
    st = a840_sc_ir3_payload_key(encoded, encoded_len, &key);
    if (st != A840_SC_OK) {
        a840_sc_service_destroy(svc);
        a840_sc_ir3_backend_destroy(backend);
        return EXIT_FAILURE;
    }

    A840ScFrameHeader req;
    memset(&req, 0, sizeof(req));
    req.magic = A840_SC_PROTO_MAGIC;
    req.version = A840_SC_PROTO_VERSION;
    req.msg_type = A840_SC_MSG_COMPILE_REQUEST;
    req.priority_class = A840_SC_PRIO_FG_SYNC;
    req.stack_key = opts->stack_key;
    req.abi_fingerprint = abi_fp;
    req.pipeline_key = key;
    req.request_id = 1u;
    req.client_pid = (uint32_t)getpid();
    req.payload_bytes = (uint32_t)encoded_len;

    SelftestResult res;
    memset(&res, 0, sizeof(res));
    pthread_mutex_init(&res.mutex, NULL);
    pthread_cond_init(&res.cond, NULL);

    st = a840_sc_service_submit(svc, &req, encoded, selftest_completion, &res);
    if (st == A840_SC_OK) {
        pthread_mutex_lock(&res.mutex);
        while (!res.done)
            pthread_cond_wait(&res.cond, &res.mutex);
        pthread_mutex_unlock(&res.mutex);
    }

    int exit_code = EXIT_FAILURE;
    if (st == A840_SC_OK && res.status == A840_SC_OK && res.blob && res.bytes > 0u) {
        A840PipelineRecord rec;
        memset(&rec, 0, sizeof(rec));
        A840RecordStatus rst = a840_record_deserialize(res.blob, res.bytes,
            opts->stack_key, abi_fp, &key, &rec);
        if (rst == A840_RECORD_OK && rec.stage_count == 1u &&
            rec.stages[0].isa_dwords > 0u) {
            printf("PASS NezhaShaderCompiler --selftest: A840PR01=%zu bytes, isa_dwords=%u, workers=%u, abi=0x%016" PRIx64 "\n",
                   res.bytes, rec.stages[0].isa_dwords, opts->workers, abi_fp);
            exit_code = EXIT_SUCCESS;
        }
        a840_record_free(&rec);
    } else {
        fprintf(stderr, "NezhaShaderCompiler selftest failed: submit=%s reply=%s\n",
                a840_sc_status_string(st), a840_sc_status_string(res.status));
    }

    free(res.blob);
    pthread_cond_destroy(&res.cond);
    pthread_mutex_destroy(&res.mutex);
    a840_sc_service_destroy(svc);
    a840_sc_ir3_backend_destroy(backend);
    return exit_code;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    if (!s || !s[0])
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 0);
    if (errno != 0 || !end || *end != '\0' || v > UINT32_MAX)
        return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_u64(const char *s, uint64_t *out)
{
    if (!s || !s[0])
        return false;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno != 0 || !end || *end != '\0')
        return false;
    *out = (uint64_t)v;
    return true;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [--socket <unix_path>] [--cache-dir <dir>] [--workers <1..8>]\n"
        "          [--stack-key <u64>] [--once] [--selftest]\n",
        prog);
}

static int create_unix_listener(const char *socket_path)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    size_t plen = strlen(socket_path);
    if (plen == 0u || plen >= sizeof(addr.sun_path)) {
        fprintf(stderr, "NezhaShaderCompiler: socket path invalid or too long\n");
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("NezhaShaderCompiler: socket");
        return -1;
    }

    (void)unlink(socket_path);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, socket_path, plen + 1u);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("NezhaShaderCompiler: bind");
        (void)close(fd);
        return -1;
    }
    (void)chmod(socket_path, 0600);
    if (listen(fd, DAEMON_BACKLOG) != 0) {
        perror("NezhaShaderCompiler: listen");
        (void)close(fd);
        (void)unlink(socket_path);
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    DaemonOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.workers = DAEMON_DEFAULT_WORKERS;
    opts.stack_key = DAEMON_DEFAULT_STACK_KEY;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            opts.socket_path = argv[++i];
        } else if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) {
            opts.cache_dir = argv[++i];
        } else if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            if (!parse_u32(argv[++i], &opts.workers) ||
                opts.workers < A840_SC_MIN_WORKERS ||
                opts.workers > A840_SC_MAX_WORKERS) {
                fprintf(stderr, "NezhaShaderCompiler: --workers must be in 1..8\n");
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "--stack-key") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &opts.stack_key) || opts.stack_key == 0u) {
                fprintf(stderr, "NezhaShaderCompiler: --stack-key must be nonzero u64\n");
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "--once") == 0) {
            opts.once_mode = true;
        } else if (strcmp(argv[i], "--selftest") == 0) {
            opts.selftest_mode = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        } else {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stop_signal;
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)signal(SIGPIPE, SIG_IGN);

    if (opts.selftest_mode)
        return run_selftest(&opts);

    if (!opts.socket_path || !opts.socket_path[0]) {
        fprintf(stderr, "NezhaShaderCompiler: --socket <unix_path> or --selftest is required\n");
        return EXIT_FAILURE;
    }

    const uint64_t abi_fp = a840_compile_abi_fingerprint();
    A840ScIr3BackendConfig bcfg;
    memset(&bcfg, 0, sizeof(bcfg));
    bcfg.stack_key = opts.stack_key;
    bcfg.abi_fingerprint = abi_fp;
    bcfg.default_compile_flags = A840_COMPILE_FLAG_EARLY_PREAMBLE;
    bcfg.enable_translate = true;

    A840ScIr3Backend *backend = NULL;
    A840ScStatus st = a840_sc_ir3_backend_create(&bcfg, &backend);
    if (st != A840_SC_OK) {
        fprintf(stderr, "NezhaShaderCompiler: backend init failed: %s\n",
                a840_sc_status_string(st));
        return EXIT_FAILURE;
    }

    A840ScServiceConfig scfg;
    memset(&scfg, 0, sizeof(scfg));
    scfg.stack_key = opts.stack_key;
    scfg.abi_fingerprint = abi_fp;
    scfg.worker_count = opts.workers;
    scfg.mem_cache_entries = DAEMON_DEFAULT_CACHE_ENTRIES;
    scfg.mem_cache_bytes = DAEMON_DEFAULT_CACHE_BYTES;
    scfg.disk_cache_dir = opts.cache_dir;
    scfg.compile_fn = a840_sc_ir3_compile_callback;
    scfg.user_ctx = backend;

    A840ScService *svc = NULL;
    st = a840_sc_service_create(&scfg, &svc);
    if (st != A840_SC_OK) {
        fprintf(stderr, "NezhaShaderCompiler: service init failed: %s\n",
                a840_sc_status_string(st));
        a840_sc_ir3_backend_destroy(backend);
        return EXIT_FAILURE;
    }

    int listen_fd = create_unix_listener(opts.socket_path);
    if (listen_fd < 0) {
        a840_sc_service_destroy(svc);
        a840_sc_ir3_backend_destroy(backend);
        return EXIT_FAILURE;
    }

    ClientConn *clients = (ClientConn *)calloc(DAEMON_MAX_CLIENTS, sizeof(ClientConn));
    if (!clients) {
        (void)close(listen_fd);
        (void)unlink(opts.socket_path);
        a840_sc_service_destroy(svc);
        a840_sc_ir3_backend_destroy(backend);
        return EXIT_FAILURE;
    }
    for (size_t i = 0; i < DAEMON_MAX_CLIENTS; ++i)
        clients[i].fd = -1;

    size_t accepted_count = 0u;
    while (!g_stop_requested) {
        /* Reap finished client slots opportunistically. */
        for (size_t i = 0; i < DAEMON_MAX_CLIENTS; ++i) {
            if (!clients[i].active)
                continue;
            pthread_mutex_lock(&clients[i].write_mutex);
            bool done = clients[i].closed;
            pthread_mutex_unlock(&clients[i].write_mutex);
            if (done) {
                pthread_join(clients[i].thread, NULL);
                pthread_cond_destroy(&clients[i].state_cond);
                pthread_mutex_destroy(&clients[i].state_mutex);
                pthread_mutex_destroy(&clients[i].write_mutex);
                clients[i].active = false;
            }
        }

        if (opts.once_mode && accepted_count > 0u) {
            bool any_active = false;
            for (size_t i = 0; i < DAEMON_MAX_CLIENTS; ++i) {
                if (clients[i].active) {
                    any_active = true;
                    break;
                }
            }
            if (!any_active)
                break;
        }

        struct pollfd pfd;
        pfd.fd = listen_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, DAEMON_POLL_MS);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0 || (pfd.revents & POLLIN) == 0)
            continue;
        if (opts.once_mode && accepted_count > 0u)
            continue;

        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }

        size_t slot = DAEMON_MAX_CLIENTS;
        for (size_t i = 0; i < DAEMON_MAX_CLIENTS; ++i) {
            if (!clients[i].active) {
                slot = i;
                break;
            }
        }
        if (slot == DAEMON_MAX_CLIENTS) {
            (void)close(cfd);
            continue;
        }

        ClientConn *c = &clients[slot];
        memset(c, 0, sizeof(*c));
        c->fd = cfd;
        c->service = svc;
        pthread_mutex_init(&c->write_mutex, NULL);
        pthread_mutex_init(&c->state_mutex, NULL);
        pthread_cond_init(&c->state_cond, NULL);

        if (pthread_create(&c->thread, NULL, client_thread_main, c) != 0) {
            pthread_cond_destroy(&c->state_cond);
            pthread_mutex_destroy(&c->state_mutex);
            pthread_mutex_destroy(&c->write_mutex);
            (void)close(cfd);
            c->fd = -1;
            continue;
        }
        c->active = true;
        ++accepted_count;
    }

    (void)close(listen_fd);
    (void)unlink(opts.socket_path);

    /* Trigger client disconnect on shutdown and join all client threads. */
    g_stop_requested = 1;
    for (size_t i = 0; i < DAEMON_MAX_CLIENTS; ++i) {
        if (!clients[i].active)
            continue;
        pthread_mutex_lock(&clients[i].write_mutex);
        if (clients[i].fd >= 0)
            (void)shutdown(clients[i].fd, SHUT_RDWR);
        pthread_mutex_unlock(&clients[i].write_mutex);
        pthread_join(clients[i].thread, NULL);
        pthread_cond_destroy(&clients[i].state_cond);
        pthread_mutex_destroy(&clients[i].state_mutex);
        pthread_mutex_destroy(&clients[i].write_mutex);
        clients[i].active = false;
    }

    free(clients);
    a840_sc_service_destroy(svc);
    a840_sc_ir3_backend_destroy(backend);
    return EXIT_SUCCESS;
}
