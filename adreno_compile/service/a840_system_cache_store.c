/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif

#include "a840_system_cache_store.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
    SC_ENV_OFF_MAGIC        = 0u,
    SC_ENV_OFF_VERSION      = 4u,
    SC_ENV_OFF_PAYLOAD_LEN  = 8u,
    SC_ENV_OFF_PAYLOAD_CRC  = 12u,
    SC_ENV_OFF_ABI_FP       = 16u,
    SC_ENV_OFF_STACK_KEY    = 24u,
    SC_ENV_OFF_HEADER_CRC   = 32u,
    SC_ENV_OFF_RESERVED     = 36u,
    SC_KEY_HEX_LEN          = 64u,
    SC_FILENAME_LEN         = 71u, /* 64 hex + ".a840pr" */
    SC_LRU_INITIAL_CAP      = 64u
};

typedef struct LruEntry {
    char filename[72];
    size_t file_bytes;
    uint64_t lru_seq;
} LruEntry;

typedef struct DiskScanItem {
    char filename[72];
    size_t file_bytes;
    uint64_t lru_seq;
    time_t mtime;
} DiskScanItem;

struct A840SystemCacheStore {
    uint64_t stack_key;
    uint64_t abi_fingerprint;
    A840SystemCacheRole role;
    size_t max_cache_bytes;
    bool isolate_by_abi;
    bool quarantine_corrupt;

    char *shared_dir;
    char *private_dir;

    pthread_rwlock_t pack_lock;
    void *pack_map;
    size_t pack_map_bytes;
    A840PackV1View pack_view;
    bool pack_mounted;

    pthread_mutex_t l1_mutex;
    LruEntry *lru_items;
    size_t lru_count;
    size_t lru_cap;
    uint64_t next_lru_seq;

    _Atomic uint64_t l0_pack_hits;
    _Atomic uint64_t l1_shared_hits;
    _Atomic uint64_t l1_private_hits;
    _Atomic uint64_t misses;
    _Atomic uint64_t corrupt_rejected;
    _Atomic uint64_t evictions;
    _Atomic uint64_t puts_shared;
    _Atomic uint64_t puts_private;
    _Atomic uint64_t abi_mismatch_rejected;
    _Atomic uint64_t l1_current_bytes;
};

static _Atomic uint64_t g_tmp_seq = 0u;

static void put_le32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xffu);
    dst[1] = (uint8_t)((v >> 8) & 0xffu);
    dst[2] = (uint8_t)((v >> 16) & 0xffu);
    dst[3] = (uint8_t)((v >> 24) & 0xffu);
}

static void put_le64(uint8_t *dst, uint64_t v)
{
    put_le32(dst, (uint32_t)(v & 0xffffffffu));
    put_le32(dst + 4, (uint32_t)(v >> 32));
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

static uint64_t get_le64(const uint8_t *src)
{
    return (uint64_t)get_le32(src) | ((uint64_t)get_le32(src + 4) << 32);
}

static int sync_fd(int fd)
{
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    if (fcntl(fd, F_FULLFSYNC) == 0)
        return 0;
#endif
    return fsync(fd);
}

static void set_cloexec(int fd)
{
    if (fd >= 0)
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static bool starts_with_ci(const char *s, const char *prefix)
{
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
            return false;
        ++s;
        ++prefix;
    }
    return true;
}

static bool path_is_forbidden(const char *path)
{
    if (!path || !path[0])
        return true;
    for (const char *p = path; *p; ++p) {
        if (*p == '\\')
            return true;
    }
    if (isalpha((unsigned char)path[0]) && path[1] == ':')
        return true;
    if (starts_with_ci(path, "/mnt/c") && (path[6] == '\0' || path[6] == '/'))
        return true;
    if (strcmp(path, "/tmp") == 0 || strncmp(path, "/tmp/", 5u) == 0)
        return true;
    if (starts_with_ci(path, "/private/var/ngpu") &&
        (path[17] == '\0' || path[17] == '/'))
        return true;
    return false;
}

static bool key_is_nonzero(const A840PipelineKey *key)
{
    if (!key)
        return false;
    uint8_t acc = 0u;
    for (size_t i = 0; i < sizeof(key->sha256); ++i)
        acc |= key->sha256[i];
    return acc != 0u;
}

static void format_key_filename(const A840PipelineKey *key, char out[72])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32u; ++i) {
        out[i * 2u] = hex[(key->sha256[i] >> 4) & 0x0fu];
        out[i * 2u + 1u] = hex[key->sha256[i] & 0x0fu];
    }
    memcpy(out + 64u, ".a840pr", 8u);
}

static bool is_valid_entry_filename(const char *name)
{
    if (!name || strlen(name) != SC_FILENAME_LEN)
        return false;
    for (size_t i = 0; i < SC_KEY_HEX_LEN; ++i) {
        char c = name[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok)
            return false;
    }
    return memcmp(name + SC_KEY_HEX_LEN, ".a840pr", 7u) == 0;
}

static bool mkdir_p(const char *path, mode_t mode)
{
    if (!path || !path[0] || strlen(path) >= PATH_MAX)
        return false;
    char buf[PATH_MAX];
    size_t len = strlen(path);
    memcpy(buf, path, len + 1u);

    for (size_t i = 1u; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, mode) != 0 && errno != EEXIST)
                return false;
            buf[i] = '/';
        }
    }
    if (mkdir(buf, mode) != 0 && errno != EEXIST)
        return false;
    struct stat st;
    if (lstat(buf, &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    return true;
}

static char *build_active_dir_path(const char *base_dir, bool isolate_by_abi,
    uint64_t abi_fp, uint64_t stack_key)
{
    if (!base_dir || !base_dir[0])
        return NULL;
    size_t blen = strlen(base_dir);
    while (blen > 1u && base_dir[blen - 1u] == '/')
        --blen;

    char suffix[64];
    size_t slen = 0u;
    if (isolate_by_abi) {
        int n = snprintf(suffix, sizeof(suffix), "/abi-%016" PRIx64 "-%016" PRIx64,
                         abi_fp, stack_key);
        if (n <= 0 || (size_t)n >= sizeof(suffix))
            return NULL;
        slen = (size_t)n;
    }
    if (blen + slen + 1u >= PATH_MAX)
        return NULL;
    char *out = (char *)malloc(blen + slen + 1u);
    if (!out)
        return NULL;
    memcpy(out, base_dir, blen);
    if (slen > 0u)
        memcpy(out + blen, suffix, slen);
    out[blen + slen] = '\0';
    return out;
}

static int open_dir_readonly(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    set_cloexec(fd);
    return fd;
}

static const char *writable_target_dir(const A840SystemCacheStore *store,
    bool *out_is_shared)
{
    if (out_is_shared)
        *out_is_shared = false;
    if (!store)
        return NULL;
    if (store->role == A840_CACHE_ROLE_RENDER_SERVER) {
        if (store->shared_dir) {
            if (out_is_shared)
                *out_is_shared = true;
            return store->shared_dir;
        }
        return store->private_dir;
    }
    if (store->role == A840_CACHE_ROLE_APP_SANDBOX)
        return store->private_dir;
    return NULL;
}

static bool dir_is_writable_by_role(const A840SystemCacheStore *store, bool is_shared)
{
    if (!store)
        return false;
    if (store->role == A840_CACHE_ROLE_READONLY)
        return false;
    if (is_shared)
        return store->role == A840_CACHE_ROLE_RENDER_SERVER;
    return store->role == A840_CACHE_ROLE_RENDER_SERVER ||
           store->role == A840_CACHE_ROLE_APP_SANDBOX;
}

/* ---------------- CRC32 ---------------- */

uint32_t a840_system_cache_crc32(const void *data, size_t bytes)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xffffffffu;
    if (!p && bytes > 0u)
        return 0u;
    for (size_t i = 0u; i < bytes; ++i) {
        crc ^= (uint32_t)p[i];
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return ~crc;
}

const char *a840_system_cache_status_string(A840SystemCacheStatus status)
{
    switch (status) {
    case A840_SYS_CACHE_OK:             return "OK";
    case A840_SYS_CACHE_MISS:           return "MISS";
    case A840_SYS_CACHE_INVALID_ARG:    return "INVALID_ARG";
    case A840_SYS_CACHE_ABI_MISMATCH:   return "ABI_MISMATCH";
    case A840_SYS_CACHE_CORRUPT:        return "CORRUPT";
    case A840_SYS_CACHE_READONLY:       return "READONLY";
    case A840_SYS_CACHE_IO_ERROR:       return "IO_ERROR";
    case A840_SYS_CACHE_OOM:            return "OOM";
    case A840_SYS_CACHE_PATH_FORBIDDEN: return "PATH_FORBIDDEN";
    default:                            return "UNKNOWN";
    }
}

/* ---------------- LRU Tracking Helpers (caller holds l1_mutex) ---------------- */

static LruEntry *lru_find_locked(A840SystemCacheStore *store, const char *filename)
{
    for (size_t i = 0u; i < store->lru_count; ++i) {
        if (strcmp(store->lru_items[i].filename, filename) == 0)
            return &store->lru_items[i];
    }
    return NULL;
}

static void lru_touch_locked(A840SystemCacheStore *store, const char *filename,
    size_t file_bytes)
{
    LruEntry *e = lru_find_locked(store, filename);
    uint64_t seq = ++store->next_lru_seq;
    if (e) {
        e->lru_seq = seq;
        if (file_bytes > 0u)
            e->file_bytes = file_bytes;
        return;
    }
    if (store->lru_count == store->lru_cap) {
        size_t new_cap = store->lru_cap ? store->lru_cap * 2u : SC_LRU_INITIAL_CAP;
        LruEntry *grown = (LruEntry *)realloc(store->lru_items, new_cap * sizeof(LruEntry));
        if (!grown)
            return;
        store->lru_items = grown;
        store->lru_cap = new_cap;
    }
    LruEntry *dst = &store->lru_items[store->lru_count++];
    memset(dst, 0, sizeof(*dst));
    size_t nlen = strlen(filename);
    if (nlen >= sizeof(dst->filename))
        nlen = sizeof(dst->filename) - 1u;
    memcpy(dst->filename, filename, nlen);
    dst->filename[nlen] = '\0';
    dst->file_bytes = file_bytes;
    dst->lru_seq = seq;
}

static void lru_remove_locked(A840SystemCacheStore *store, const char *filename)
{
    for (size_t i = 0u; i < store->lru_count; ++i) {
        if (strcmp(store->lru_items[i].filename, filename) == 0) {
            store->lru_items[i] = store->lru_items[store->lru_count - 1u];
            --store->lru_count;
            return;
        }
    }
}

static int compare_disk_scan_items(const void *a, const void *b)
{
    const DiskScanItem *x = (const DiskScanItem *)a;
    const DiskScanItem *y = (const DiskScanItem *)b;
    if (x->lru_seq != y->lru_seq)
        return (x->lru_seq < y->lru_seq) ? -1 : 1;
    if (x->mtime != y->mtime)
        return (x->mtime < y->mtime) ? -1 : 1;
    return strcmp(x->filename, y->filename);
}

static A840SystemCacheStatus enforce_limit_locked(A840SystemCacheStore *store,
    const char *dir_path, const char *protected_filename, uint64_t *out_evicted)
{
    if (out_evicted)
        *out_evicted = 0u;
    if (!dir_path) {
        atomic_store_explicit(&store->l1_current_bytes, 0u, memory_order_relaxed);
        return A840_SYS_CACHE_OK;
    }

    int dirfd = open_dir_readonly(dir_path);
    if (dirfd < 0)
        return (errno == ENOENT) ? A840_SYS_CACHE_OK : A840_SYS_CACHE_IO_ERROR;

    DIR *dp = fdopendir(dirfd);
    if (!dp) {
        (void)close(dirfd);
        return A840_SYS_CACHE_IO_ERROR;
    }

    DiskScanItem *items = NULL;
    size_t count = 0u;
    size_t cap = 0u;
    uint64_t total_bytes = 0u;

    struct dirent *ent = NULL;
    while ((ent = readdir(dp)) != NULL) {
        if (!is_valid_entry_filename(ent->d_name))
            continue;
        struct stat st;
        if (fstatat(dirfd, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        if (!S_ISREG(st.st_mode) || st.st_size < 0)
            continue;
        if (count == cap) {
            size_t new_cap = cap ? cap * 2u : 32u;
            DiskScanItem *grown = (DiskScanItem *)realloc(items, new_cap * sizeof(DiskScanItem));
            if (!grown) {
                free(items);
                (void)closedir(dp);
                return A840_SYS_CACHE_OOM;
            }
            items = grown;
            cap = new_cap;
        }
        DiskScanItem *it = &items[count++];
        memset(it, 0, sizeof(*it));
        memcpy(it->filename, ent->d_name, SC_FILENAME_LEN + 1u);
        it->file_bytes = (size_t)st.st_size;
        it->mtime = st.st_mtime;
        LruEntry *tracked = lru_find_locked(store, it->filename);
        it->lru_seq = tracked ? tracked->lru_seq : 0u;
        total_bytes += (uint64_t)it->file_bytes;
    }

    uint64_t evicted = 0u;
    if (total_bytes > (uint64_t)store->max_cache_bytes && count > 0u) {
        qsort(items, count, sizeof(DiskScanItem), compare_disk_scan_items);
        for (size_t i = 0u; i < count && total_bytes > (uint64_t)store->max_cache_bytes; ++i) {
            if (protected_filename && strcmp(items[i].filename, protected_filename) == 0)
                continue;
            if (unlinkat(dirfd, items[i].filename, 0) == 0 || errno == ENOENT) {
                if (total_bytes >= (uint64_t)items[i].file_bytes)
                    total_bytes -= (uint64_t)items[i].file_bytes;
                else
                    total_bytes = 0u;
                lru_remove_locked(store, items[i].filename);
                ++evicted;
            }
        }
    }

    if (evicted > 0u) {
        (void)sync_fd(dirfd);
        atomic_fetch_add_explicit(&store->evictions, evicted, memory_order_relaxed);
    }
    atomic_store_explicit(&store->l1_current_bytes, total_bytes, memory_order_relaxed);

    free(items);
    (void)closedir(dp);
    if (out_evicted)
        *out_evicted = evicted;
    return A840_SYS_CACHE_OK;
}

/* ---------------- L0 Pack Mount ---------------- */

static void unmap_pack_locked(A840SystemCacheStore *store)
{
    if (store->pack_map && store->pack_map_bytes > 0u)
        (void)munmap(store->pack_map, store->pack_map_bytes);
    store->pack_map = NULL;
    store->pack_map_bytes = 0u;
    memset(&store->pack_view, 0, sizeof(store->pack_view));
    store->pack_mounted = false;
}

A840SystemCacheStatus a840_system_cache_mount_pack(A840SystemCacheStore *store,
    const char *pack_path)
{
    if (!store || !pack_path || !pack_path[0])
        return A840_SYS_CACHE_INVALID_ARG;
    if (path_is_forbidden(pack_path))
        return A840_SYS_CACHE_PATH_FORBIDDEN;

    int fd = open(pack_path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return (errno == ENOENT) ? A840_SYS_CACHE_MISS : A840_SYS_CACHE_IO_ERROR;
    set_cloexec(fd);

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)close(fd);
        atomic_fetch_add_explicit(&store->corrupt_rejected, 1u, memory_order_relaxed);
        return A840_SYS_CACHE_CORRUPT;
    }
    if (st.st_size < (off_t)A840_PACK_V1_HEADER_BYTES) {
        (void)close(fd);
        atomic_fetch_add_explicit(&store->corrupt_rejected, 1u, memory_order_relaxed);
        return A840_SYS_CACHE_CORRUPT;
    }

    size_t map_bytes = (size_t)st.st_size;
    void *mapped = mmap(NULL, map_bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    (void)close(fd);
    if (mapped == MAP_FAILED)
        return A840_SYS_CACHE_IO_ERROR;

    A840PackV1View view;
    memset(&view, 0, sizeof(view));
    A840PackV1Status pst = a840_pack_v1_open_memory(mapped, map_bytes,
        store->stack_key, store->abi_fingerprint, &view);
    if (pst != A840_PACK_OK) {
        (void)munmap(mapped, map_bytes);
        atomic_fetch_add_explicit(&store->corrupt_rejected, 1u, memory_order_relaxed);
        if (pst == A840_PACK_ABI_MISMATCH || pst == A840_PACK_STACK_MISMATCH) {
            atomic_fetch_add_explicit(&store->abi_mismatch_rejected, 1u, memory_order_relaxed);
            return A840_SYS_CACHE_ABI_MISMATCH;
        }
        return A840_SYS_CACHE_CORRUPT;
    }

    pthread_rwlock_wrlock(&store->pack_lock);
    unmap_pack_locked(store);
    store->pack_map = mapped;
    store->pack_map_bytes = map_bytes;
    store->pack_view = view;
    store->pack_mounted = true;
    pthread_rwlock_unlock(&store->pack_lock);
    return A840_SYS_CACHE_OK;
}

/* ---------------- Lifecycle ---------------- */

A840SystemCacheStatus a840_system_cache_create(const A840SystemCacheConfig *cfg,
    A840SystemCacheStore **out_store)
{
    if (!cfg || !out_store || cfg->stack_key == 0u)
        return A840_SYS_CACHE_INVALID_ARG;
    if (cfg->role > A840_CACHE_ROLE_READONLY)
        return A840_SYS_CACHE_INVALID_ARG;

    const uint64_t expected_abi = a840_compile_abi_fingerprint();
    const uint64_t abi_fp = cfg->abi_fingerprint ? cfg->abi_fingerprint : expected_abi;
    if (abi_fp == 0u)
        return A840_SYS_CACHE_INVALID_ARG;

    if (cfg->pack_path && cfg->pack_path[0] && path_is_forbidden(cfg->pack_path))
        return A840_SYS_CACHE_PATH_FORBIDDEN;
    if (cfg->shared_cache_dir && cfg->shared_cache_dir[0] &&
        path_is_forbidden(cfg->shared_cache_dir))
        return A840_SYS_CACHE_PATH_FORBIDDEN;
    if (cfg->private_cache_dir && cfg->private_cache_dir[0] &&
        path_is_forbidden(cfg->private_cache_dir))
        return A840_SYS_CACHE_PATH_FORBIDDEN;

    A840SystemCacheStore *store = (A840SystemCacheStore *)calloc(1, sizeof(*store));
    if (!store)
        return A840_SYS_CACHE_OOM;

    store->stack_key = cfg->stack_key;
    store->abi_fingerprint = abi_fp;
    store->role = cfg->role;
    store->max_cache_bytes = cfg->max_cache_bytes ? cfg->max_cache_bytes
                                                  : (size_t)A840_SYS_CACHE_DEFAULT_MAX_BYTES;
    store->isolate_by_abi = cfg->isolate_by_abi;
    store->quarantine_corrupt = cfg->quarantine_corrupt;

    if (pthread_rwlock_init(&store->pack_lock, NULL) != 0) {
        free(store);
        return A840_SYS_CACHE_OOM;
    }
    if (pthread_mutex_init(&store->l1_mutex, NULL) != 0) {
        pthread_rwlock_destroy(&store->pack_lock);
        free(store);
        return A840_SYS_CACHE_OOM;
    }

    if (cfg->shared_cache_dir && cfg->shared_cache_dir[0]) {
        store->shared_dir = build_active_dir_path(cfg->shared_cache_dir,
            cfg->isolate_by_abi, abi_fp, cfg->stack_key);
        if (!store->shared_dir) {
            a840_system_cache_destroy(store);
            return A840_SYS_CACHE_OOM;
        }
        if (store->role == A840_CACHE_ROLE_RENDER_SERVER) {
            if (!mkdir_p(store->shared_dir, 0755)) {
                a840_system_cache_destroy(store);
                return A840_SYS_CACHE_IO_ERROR;
            }
        }
    }

    if (cfg->private_cache_dir && cfg->private_cache_dir[0]) {
        store->private_dir = build_active_dir_path(cfg->private_cache_dir,
            cfg->isolate_by_abi, abi_fp, cfg->stack_key);
        if (!store->private_dir) {
            a840_system_cache_destroy(store);
            return A840_SYS_CACHE_OOM;
        }
        if (store->role != A840_CACHE_ROLE_READONLY) {
            if (!mkdir_p(store->private_dir, 0700)) {
                a840_system_cache_destroy(store);
                return A840_SYS_CACHE_IO_ERROR;
            }
        }
    }

    if (cfg->pack_path && cfg->pack_path[0])
        (void)a840_system_cache_mount_pack(store, cfg->pack_path);

    bool is_shared = false;
    const char *wdir = writable_target_dir(store, &is_shared);
    if (wdir) {
        pthread_mutex_lock(&store->l1_mutex);
        (void)enforce_limit_locked(store, wdir, NULL, NULL);
        pthread_mutex_unlock(&store->l1_mutex);
    }

    *out_store = store;
    return A840_SYS_CACHE_OK;
}

void a840_system_cache_destroy(A840SystemCacheStore *store)
{
    if (!store)
        return;
    pthread_rwlock_wrlock(&store->pack_lock);
    unmap_pack_locked(store);
    pthread_rwlock_unlock(&store->pack_lock);
    pthread_rwlock_destroy(&store->pack_lock);
    pthread_mutex_destroy(&store->l1_mutex);
    free(store->lru_items);
    free(store->shared_dir);
    free(store->private_dir);
    free(store);
}

bool a840_system_cache_has_pack(const A840SystemCacheStore *store)
{
    if (!store)
        return false;
    A840SystemCacheStore *mut = (A840SystemCacheStore *)store;
    pthread_rwlock_rdlock(&mut->pack_lock);
    bool mounted = mut->pack_mounted;
    pthread_rwlock_unlock(&mut->pack_lock);
    return mounted;
}

const char *a840_system_cache_active_shared_dir(const A840SystemCacheStore *store)
{
    return store ? store->shared_dir : NULL;
}

const char *a840_system_cache_active_private_dir(const A840SystemCacheStore *store)
{
    return store ? store->private_dir : NULL;
}

/* ---------------- L0 Fast Zero-Copy Lookup ---------------- */

A840SystemCacheStatus a840_system_cache_lookup_pack_view(
    A840SystemCacheStore *store,
    const A840PipelineKey *key,
    A840PackV1Entry *out_entry)
{
    if (!store || !key_is_nonzero(key) || !out_entry)
        return A840_SYS_CACHE_INVALID_ARG;

    pthread_rwlock_rdlock(&store->pack_lock);
    if (!store->pack_mounted) {
        pthread_rwlock_unlock(&store->pack_lock);
        return A840_SYS_CACHE_MISS;
    }
    A840PackV1Status pst = a840_pack_v1_lookup(&store->pack_view, key, out_entry);
    pthread_rwlock_unlock(&store->pack_lock);

    if (pst == A840_PACK_OK) {
        atomic_fetch_add_explicit(&store->l0_pack_hits, 1u, memory_order_relaxed);
        return A840_SYS_CACHE_OK;
    }
    return A840_SYS_CACHE_MISS;
}

/* ---------------- L1 File Read / Validate ---------------- */

static A840SystemCacheStatus read_l1_dir_blob(
    A840SystemCacheStore *store,
    const char *dir_path,
    bool is_shared,
    const A840PipelineKey *key,
    uint8_t **out_blob,
    size_t *out_bytes,
    A840PipelineRecord *out_record)
{
    if (!dir_path)
        return A840_SYS_CACHE_MISS;

    int dirfd = open_dir_readonly(dir_path);
    if (dirfd < 0)
        return A840_SYS_CACHE_MISS;

    char filename[72];
    format_key_filename(key, filename);

    const bool can_quarantine = store->quarantine_corrupt &&
                                dir_is_writable_by_role(store, is_shared);

    int fd = openat(dirfd, filename, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        int err = errno;
        if (err == ELOOP) {
            atomic_fetch_add_explicit(&store->corrupt_rejected, 1u, memory_order_relaxed);
            if (can_quarantine)
                (void)unlinkat(dirfd, filename, 0);
            (void)close(dirfd);
            return A840_SYS_CACHE_CORRUPT;
        }
        (void)close(dirfd);
        return A840_SYS_CACHE_MISS;
    }
    set_cloexec(fd);

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < (off_t)A840_SYS_CACHE_ENVELOPE_BYTES ||
        st.st_size > (off_t)(A840_RECORD_MAX_BYTES + A840_SYS_CACHE_ENVELOPE_BYTES)) {
        (void)close(fd);
        atomic_fetch_add_explicit(&store->corrupt_rejected, 1u, memory_order_relaxed);
        if (can_quarantine)
            (void)unlinkat(dirfd, filename, 0);
        (void)close(dirfd);
        return A840_SYS_CACHE_CORRUPT;
    }

    size_t file_bytes = (size_t)st.st_size;
    uint8_t *raw = (uint8_t *)malloc(file_bytes);
    if (!raw) {
        (void)close(fd);
        (void)close(dirfd);
        return A840_SYS_CACHE_OOM;
    }

    size_t got = 0u;
    while (got < file_bytes) {
        ssize_t n = read(fd, raw + got, file_bytes - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            free(raw);
            (void)close(fd);
            atomic_fetch_add_explicit(&store->corrupt_rejected, 1u, memory_order_relaxed);
            if (can_quarantine)
                (void)unlinkat(dirfd, filename, 0);
            (void)close(dirfd);
            return A840_SYS_CACHE_CORRUPT;
        }
        got += (size_t)n;
    }
    (void)close(fd);

    const uint8_t *payload = NULL;
    size_t payload_bytes = 0u;
    bool abi_bad = false;
    bool corrupt = false;

    if (file_bytes >= A840_SYS_CACHE_ENVELOPE_BYTES &&
        get_le32(raw + SC_ENV_OFF_MAGIC) == A840_SYS_CACHE_ENVELOPE_MAGIC) {
        uint32_t ver = get_le32(raw + SC_ENV_OFF_VERSION);
        uint32_t plen = get_le32(raw + SC_ENV_OFF_PAYLOAD_LEN);
        uint32_t pcrc = get_le32(raw + SC_ENV_OFF_PAYLOAD_CRC);
        uint64_t abi = get_le64(raw + SC_ENV_OFF_ABI_FP);
        uint64_t skey = get_le64(raw + SC_ENV_OFF_STACK_KEY);
        uint32_t hcrc = get_le32(raw + SC_ENV_OFF_HEADER_CRC);
        uint32_t rsv = get_le32(raw + SC_ENV_OFF_RESERVED);

        uint32_t calc_hcrc = a840_system_cache_crc32(raw, SC_ENV_OFF_HEADER_CRC);
        if (ver != A840_SYS_CACHE_ENVELOPE_VERSION || rsv != 0u ||
            hcrc != calc_hcrc ||
            (size_t)plen != file_bytes - A840_SYS_CACHE_ENVELOPE_BYTES ||
            plen < A840_RECORD_PREFIX_BYTES) {
            corrupt = true;
        } else if (abi != store->abi_fingerprint || skey != store->stack_key) {
            abi_bad = true;
            corrupt = true;
        } else {
            payload = raw + A840_SYS_CACHE_ENVELOPE_BYTES;
            payload_bytes = (size_t)plen;
            if (a840_system_cache_crc32(payload, payload_bytes) != pcrc)
                corrupt = true;
        }
    } else if (file_bytes >= A840_RECORD_PREFIX_BYTES &&
               memcmp(raw, "A840PR01", 8u) == 0) {
        payload = raw;
        payload_bytes = file_bytes;
    } else {
        corrupt = true;
    }

    A840PipelineRecord parsed;
    memset(&parsed, 0, sizeof(parsed));
    if (!corrupt) {
        A840RecordStatus rst = a840_record_deserialize(payload, payload_bytes,
            store->stack_key, store->abi_fingerprint, key, &parsed);
        if (rst != A840_RECORD_OK) {
            corrupt = true;
            if (rst == A840_RECORD_ABI_MISMATCH || rst == A840_RECORD_STACK_MISMATCH)
                abi_bad = true;
        }
    }

    if (corrupt) {
        free(raw);
        atomic_fetch_add_explicit(&store->corrupt_rejected, 1u, memory_order_relaxed);
        if (abi_bad)
            atomic_fetch_add_explicit(&store->abi_mismatch_rejected, 1u, memory_order_relaxed);
        if (can_quarantine)
            (void)unlinkat(dirfd, filename, 0);
        (void)close(dirfd);
        return abi_bad ? A840_SYS_CACHE_ABI_MISMATCH : A840_SYS_CACHE_CORRUPT;
    }

    bool writable_is_shared = false;
    const char *wdir = writable_target_dir(store, &writable_is_shared);
    if (wdir && writable_is_shared == is_shared) {
        pthread_mutex_lock(&store->l1_mutex);
        lru_touch_locked(store, filename, file_bytes);
        pthread_mutex_unlock(&store->l1_mutex);
    }
    (void)close(dirfd);

    if (out_blob && out_bytes) {
        uint8_t *copy = (uint8_t *)malloc(payload_bytes);
        if (!copy) {
            a840_record_free(&parsed);
            free(raw);
            return A840_SYS_CACHE_OOM;
        }
        memcpy(copy, payload, payload_bytes);
        *out_blob = copy;
        *out_bytes = payload_bytes;
    }
    if (out_record)
        *out_record = parsed;
    else
        a840_record_free(&parsed);

    free(raw);
    return A840_SYS_CACHE_OK;
}

/* ---------------- Full Three-Tier Lookup ---------------- */

static A840SystemCacheStatus lookup_l0_internal(
    A840SystemCacheStore *store,
    const A840PipelineKey *key,
    uint8_t **out_blob,
    size_t *out_bytes,
    A840PipelineRecord *out_record)
{
    pthread_rwlock_rdlock(&store->pack_lock);
    if (!store->pack_mounted) {
        pthread_rwlock_unlock(&store->pack_lock);
        return A840_SYS_CACHE_MISS;
    }

    A840PackV1Entry entry;
    memset(&entry, 0, sizeof(entry));
    if (a840_pack_v1_lookup(&store->pack_view, key, &entry) != A840_PACK_OK) {
        pthread_rwlock_unlock(&store->pack_lock);
        return A840_SYS_CACHE_MISS;
    }

    A840PipelineRecord borrowed;
    memset(&borrowed, 0, sizeof(borrowed));
    if (a840_pack_v1_entry_record(&store->pack_view, &entry, &borrowed) != A840_PACK_OK) {
        pthread_rwlock_unlock(&store->pack_lock);
        return A840_SYS_CACHE_CORRUPT;
    }

    uint8_t *wire = NULL;
    size_t wire_bytes = 0u;
    A840RecordStatus sst = a840_record_serialize(&borrowed, &wire, &wire_bytes);
    pthread_rwlock_unlock(&store->pack_lock);
    if (sst != A840_RECORD_OK)
        return A840_SYS_CACHE_OOM;

    if (out_record) {
        A840RecordStatus dst = a840_record_deserialize(wire, wire_bytes,
            store->stack_key, store->abi_fingerprint, key, out_record);
        if (dst != A840_RECORD_OK) {
            free(wire);
            return A840_SYS_CACHE_CORRUPT;
        }
    }
    if (out_blob && out_bytes) {
        *out_blob = wire;
        *out_bytes = wire_bytes;
    } else {
        free(wire);
    }

    atomic_fetch_add_explicit(&store->l0_pack_hits, 1u, memory_order_relaxed);
    return A840_SYS_CACHE_OK;
}

A840SystemCacheStatus a840_system_cache_lookup(
    A840SystemCacheStore *store,
    const A840PipelineKey *key,
    A840PipelineRecord *out_record,
    A840SystemCacheTier *out_tier)
{
    if (!store || !key_is_nonzero(key) || !out_record)
        return A840_SYS_CACHE_INVALID_ARG;
    if (out_tier)
        *out_tier = A840_CACHE_TIER_MISS;

    A840SystemCacheStatus st = lookup_l0_internal(store, key, NULL, NULL, out_record);
    if (st == A840_SYS_CACHE_OK) {
        if (out_tier)
            *out_tier = A840_CACHE_TIER_L0_PACK;
        return A840_SYS_CACHE_OK;
    }

    if (store->shared_dir) {
        st = read_l1_dir_blob(store, store->shared_dir, true, key,
            NULL, NULL, out_record);
        if (st == A840_SYS_CACHE_OK) {
            atomic_fetch_add_explicit(&store->l1_shared_hits, 1u, memory_order_relaxed);
            if (out_tier)
                *out_tier = A840_CACHE_TIER_L1_SHARED;
            return A840_SYS_CACHE_OK;
        }
        if (st == A840_SYS_CACHE_OOM)
            return st;
    }

    if (store->private_dir) {
        st = read_l1_dir_blob(store, store->private_dir, false, key,
            NULL, NULL, out_record);
        if (st == A840_SYS_CACHE_OK) {
            atomic_fetch_add_explicit(&store->l1_private_hits, 1u, memory_order_relaxed);
            if (out_tier)
                *out_tier = A840_CACHE_TIER_L1_PRIVATE;
            return A840_SYS_CACHE_OK;
        }
        if (st == A840_SYS_CACHE_OOM)
            return st;
    }

    atomic_fetch_add_explicit(&store->misses, 1u, memory_order_relaxed);
    return A840_SYS_CACHE_MISS;
}

A840SystemCacheStatus a840_system_cache_lookup_blob(
    A840SystemCacheStore *store,
    const A840PipelineKey *key,
    uint8_t **out_blob,
    size_t *out_bytes,
    A840SystemCacheTier *out_tier)
{
    if (!store || !key_is_nonzero(key) || !out_blob || !out_bytes)
        return A840_SYS_CACHE_INVALID_ARG;
    if (out_tier)
        *out_tier = A840_CACHE_TIER_MISS;

    A840SystemCacheStatus st = lookup_l0_internal(store, key, out_blob, out_bytes, NULL);
    if (st == A840_SYS_CACHE_OK) {
        if (out_tier)
            *out_tier = A840_CACHE_TIER_L0_PACK;
        return A840_SYS_CACHE_OK;
    }

    if (store->shared_dir) {
        st = read_l1_dir_blob(store, store->shared_dir, true, key,
            out_blob, out_bytes, NULL);
        if (st == A840_SYS_CACHE_OK) {
            atomic_fetch_add_explicit(&store->l1_shared_hits, 1u, memory_order_relaxed);
            if (out_tier)
                *out_tier = A840_CACHE_TIER_L1_SHARED;
            return A840_SYS_CACHE_OK;
        }
        if (st == A840_SYS_CACHE_OOM)
            return st;
    }

    if (store->private_dir) {
        st = read_l1_dir_blob(store, store->private_dir, false, key,
            out_blob, out_bytes, NULL);
        if (st == A840_SYS_CACHE_OK) {
            atomic_fetch_add_explicit(&store->l1_private_hits, 1u, memory_order_relaxed);
            if (out_tier)
                *out_tier = A840_CACHE_TIER_L1_PRIVATE;
            return A840_SYS_CACHE_OK;
        }
        if (st == A840_SYS_CACHE_OOM)
            return st;
    }

    atomic_fetch_add_explicit(&store->misses, 1u, memory_order_relaxed);
    return A840_SYS_CACHE_MISS;
}

/* ---------------- Atomic L1 Write + LRU Eviction ---------------- */

A840SystemCacheStatus a840_system_cache_put_blob(
    A840SystemCacheStore *store,
    const A840PipelineKey *key,
    const uint8_t *blob,
    size_t bytes)
{
    if (!store || !key_is_nonzero(key) || !blob ||
        bytes < A840_RECORD_PREFIX_BYTES || bytes > A840_RECORD_MAX_BYTES)
        return A840_SYS_CACHE_INVALID_ARG;

    bool is_shared = false;
    const char *target_dir = writable_target_dir(store, &is_shared);
    if (!target_dir)
        return A840_SYS_CACHE_READONLY;

    A840PipelineRecord check_rec;
    memset(&check_rec, 0, sizeof(check_rec));
    A840RecordStatus rst = a840_record_deserialize(blob, bytes,
        store->stack_key, store->abi_fingerprint, key, &check_rec);
    a840_record_free(&check_rec);
    if (rst != A840_RECORD_OK) {
        atomic_fetch_add_explicit(&store->corrupt_rejected, 1u, memory_order_relaxed);
        if (rst == A840_RECORD_ABI_MISMATCH || rst == A840_RECORD_STACK_MISMATCH) {
            atomic_fetch_add_explicit(&store->abi_mismatch_rejected, 1u, memory_order_relaxed);
            return A840_SYS_CACHE_ABI_MISMATCH;
        }
        return A840_SYS_CACHE_CORRUPT;
    }

    const size_t total_bytes = A840_SYS_CACHE_ENVELOPE_BYTES + bytes;
    uint8_t *framed = (uint8_t *)malloc(total_bytes);
    if (!framed)
        return A840_SYS_CACHE_OOM;

    put_le32(framed + SC_ENV_OFF_MAGIC, A840_SYS_CACHE_ENVELOPE_MAGIC);
    put_le32(framed + SC_ENV_OFF_VERSION, A840_SYS_CACHE_ENVELOPE_VERSION);
    put_le32(framed + SC_ENV_OFF_PAYLOAD_LEN, (uint32_t)bytes);
    put_le32(framed + SC_ENV_OFF_PAYLOAD_CRC, a840_system_cache_crc32(blob, bytes));
    put_le64(framed + SC_ENV_OFF_ABI_FP, store->abi_fingerprint);
    put_le64(framed + SC_ENV_OFF_STACK_KEY, store->stack_key);
    put_le32(framed + SC_ENV_OFF_HEADER_CRC,
             a840_system_cache_crc32(framed, SC_ENV_OFF_HEADER_CRC));
    put_le32(framed + SC_ENV_OFF_RESERVED, 0u);
    memcpy(framed + A840_SYS_CACHE_ENVELOPE_BYTES, blob, bytes);

    int dirfd = open_dir_readonly(target_dir);
    if (dirfd < 0) {
        free(framed);
        return A840_SYS_CACHE_IO_ERROR;
    }

    char filename[72];
    format_key_filename(key, filename);

    char tmpname[128];
    int fd = -1;
    mode_t file_mode = is_shared ? (mode_t)0644 : (mode_t)0600;
    for (unsigned attempt = 0u; attempt < 64u; ++attempt) {
        uint64_t seq = atomic_fetch_add_explicit(&g_tmp_seq, 1u, memory_order_relaxed);
        int n = snprintf(tmpname, sizeof(tmpname), ".a840sc.%ld.%" PRIu64 ".tmp",
                         (long)getpid(), seq);
        if (n <= 0 || (size_t)n >= sizeof(tmpname)) {
            (void)close(dirfd);
            free(framed);
            return A840_SYS_CACHE_IO_ERROR;
        }
        fd = openat(dirfd, tmpname, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, file_mode);
        if (fd >= 0) {
            set_cloexec(fd);
            break;
        }
        if (errno != EEXIST) {
            (void)close(dirfd);
            free(framed);
            return A840_SYS_CACHE_IO_ERROR;
        }
    }
    if (fd < 0) {
        (void)close(dirfd);
        free(framed);
        return A840_SYS_CACHE_IO_ERROR;
    }

    A840SystemCacheStatus st = A840_SYS_CACHE_OK;
    size_t written = 0u;
    while (written < total_bytes) {
        ssize_t n = write(fd, framed + written, total_bytes - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            st = A840_SYS_CACHE_IO_ERROR;
            break;
        }
        written += (size_t)n;
    }
    free(framed);

    if (st == A840_SYS_CACHE_OK && sync_fd(fd) != 0)
        st = A840_SYS_CACHE_IO_ERROR;
    if (close(fd) != 0)
        st = A840_SYS_CACHE_IO_ERROR;

    if (st == A840_SYS_CACHE_OK && renameat(dirfd, tmpname, dirfd, filename) != 0)
        st = A840_SYS_CACHE_IO_ERROR;

    if (st != A840_SYS_CACHE_OK) {
        (void)unlinkat(dirfd, tmpname, 0);
        (void)close(dirfd);
        return st;
    }
    (void)sync_fd(dirfd);
    (void)close(dirfd);

    if (is_shared)
        atomic_fetch_add_explicit(&store->puts_shared, 1u, memory_order_relaxed);
    else
        atomic_fetch_add_explicit(&store->puts_private, 1u, memory_order_relaxed);

    pthread_mutex_lock(&store->l1_mutex);
    lru_touch_locked(store, filename, total_bytes);
    (void)enforce_limit_locked(store, target_dir, filename, NULL);
    pthread_mutex_unlock(&store->l1_mutex);

    return A840_SYS_CACHE_OK;
}

A840SystemCacheStatus a840_system_cache_put(
    A840SystemCacheStore *store,
    const A840PipelineRecord *rec)
{
    if (!store || !rec)
        return A840_SYS_CACHE_INVALID_ARG;
    uint8_t *blob = NULL;
    size_t bytes = 0u;
    A840RecordStatus rst = a840_record_serialize(rec, &blob, &bytes);
    if (rst != A840_RECORD_OK) {
        if (rst == A840_RECORD_OOM)
            return A840_SYS_CACHE_OOM;
        return A840_SYS_CACHE_INVALID_ARG;
    }
    A840SystemCacheStatus st = a840_system_cache_put_blob(store, &rec->pipeline_key,
        blob, bytes);
    free(blob);
    return st;
}

A840SystemCacheStatus a840_system_cache_enforce_limit(
    A840SystemCacheStore *store,
    uint64_t *out_evicted)
{
    if (!store)
        return A840_SYS_CACHE_INVALID_ARG;
    bool is_shared = false;
    const char *wdir = writable_target_dir(store, &is_shared);
    if (!wdir) {
        if (out_evicted)
            *out_evicted = 0u;
        return A840_SYS_CACHE_READONLY;
    }
    pthread_mutex_lock(&store->l1_mutex);
    A840SystemCacheStatus st = enforce_limit_locked(store, wdir, NULL, out_evicted);
    pthread_mutex_unlock(&store->l1_mutex);
    return st;
}

void a840_system_cache_get_stats(const A840SystemCacheStore *store,
    A840SystemCacheStats *out_stats)
{
    if (!out_stats)
        return;
    memset(out_stats, 0, sizeof(*out_stats));
    if (!store)
        return;

    A840SystemCacheStore *mut = (A840SystemCacheStore *)store;
    pthread_rwlock_rdlock(&mut->pack_lock);
    out_stats->l0_pack_entries = mut->pack_mounted ? (uint64_t)mut->pack_view.entry_count : 0u;
    pthread_rwlock_unlock(&mut->pack_lock);

    out_stats->l0_pack_hits = atomic_load_explicit(&store->l0_pack_hits, memory_order_relaxed);
    out_stats->l1_shared_hits = atomic_load_explicit(&store->l1_shared_hits, memory_order_relaxed);
    out_stats->l1_private_hits = atomic_load_explicit(&store->l1_private_hits, memory_order_relaxed);
    out_stats->misses = atomic_load_explicit(&store->misses, memory_order_relaxed);
    out_stats->corrupt_rejected = atomic_load_explicit(&store->corrupt_rejected, memory_order_relaxed);
    out_stats->evictions = atomic_load_explicit(&store->evictions, memory_order_relaxed);
    out_stats->puts_shared = atomic_load_explicit(&store->puts_shared, memory_order_relaxed);
    out_stats->puts_private = atomic_load_explicit(&store->puts_private, memory_order_relaxed);
    out_stats->abi_mismatch_rejected = atomic_load_explicit(&store->abi_mismatch_rejected, memory_order_relaxed);
    out_stats->l1_current_bytes = atomic_load_explicit(&store->l1_current_bytes, memory_order_relaxed);
    out_stats->l1_max_bytes = (uint64_t)store->max_cache_bytes;
}
