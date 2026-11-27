/* SPDX-License-Identifier: MIT */
#include "a840_hwtest_hook.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

typedef struct Entry {
    void *cb;
    A840FallbackPredicate predicate;
} Entry;

typedef struct Phase3Entry {
    void *cb;
    bool has_scope;
    bool has_evidence;
    char scope[A840_HWTEST_SCOPE_MAX_BYTES];
    A840Phase3Evidence evidence;
} Phase3Entry;

static Entry entries[A840_HWTEST_RECENT_COMMANDS];
static size_t write_index;
static Phase3Entry phase3_entries[A840_HWTEST_RECENT_COMMANDS];
static size_t phase3_write_index;
static A840HwtestPhase3NewUbwcFn g_phase3_new_ubwc_fn;
static A840HwtestPhase3ScanoutFn g_phase3_scanout_fn;
static pthread_mutex_t entry_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local char path[128];

static Entry *find_entry(void *cb)
{
    for (size_t i = 0; i < A840_HWTEST_RECENT_COMMANDS; ++i)
        if (entries[i].cb == cb) return &entries[i];
    return NULL;
}

static Phase3Entry *find_phase3_entry(void *cb)
{
    for (size_t i = 0; i < A840_HWTEST_RECENT_COMMANDS; ++i)
        if (phase3_entries[i].cb == cb) return &phase3_entries[i];
    return NULL;
}

static Phase3Entry *alloc_phase3_entry(void *cb)
{
    Phase3Entry *existing = find_phase3_entry(cb);
    if (existing) return existing;
    Phase3Entry *slot = &phase3_entries[phase3_write_index];
    phase3_write_index = (phase3_write_index + 1u) % A840_HWTEST_RECENT_COMMANDS;
    memset(slot, 0, sizeof(*slot));
    slot->cb = cb;
    return slot;
}

static bool valid_phase3_scope(const char *request)
{
    if (!request || request[0] == '\0') return false;
    return strcmp(request, "force_sysmem") == 0 ||
           strcmp(request, "force_gmem_96x32") == 0 ||
           strcmp(request, "ubwc6_write") == 0 ||
           strcmp(request, "ubwc6_sample") == 0 ||
           strcmp(request, "mdss_direct") == 0 ||
           strcmp(request, "mdss_linear") == 0;
}

bool a840_hwtest_record_command_buffer(void *cb, A840FallbackPredicate predicate)
{
    if (!cb || (predicate != A840_PRED_NONE && !a840_router_predicate_name(predicate)))
        return false;
    (void)pthread_mutex_lock(&entry_lock);
    Entry *previous = find_entry(cb);
    if (previous) previous->cb = NULL;
    entries[write_index] = (Entry){cb, predicate};
    write_index = (write_index + 1) % A840_HWTEST_RECENT_COMMANDS;
    (void)pthread_mutex_unlock(&entry_lock);
    return true;
}

void a840_hwtest_forget_command_buffer(void *cb)
{
    if (!cb) return;
    (void)pthread_mutex_lock(&entry_lock);
    Entry *entry = find_entry(cb);
    if (entry) entry->cb = NULL;
    Phase3Entry *p3 = find_phase3_entry(cb);
    if (p3) memset(p3, 0, sizeof(*p3));
    (void)pthread_mutex_unlock(&entry_lock);
}

const char *a840_hwtest_path_for_command_buffer(void *cb)
{
    if (!cb) return NULL;
    (void)pthread_mutex_lock(&entry_lock);
    const Entry *entry = find_entry(cb);
    const bool found = entry != NULL;
    const A840FallbackPredicate predicate = found ? entry->predicate : A840_PRED_NONE;
    (void)pthread_mutex_unlock(&entry_lock);
    if (!found) return NULL;
    if (predicate == A840_PRED_NONE) return "raw";
    int len = snprintf(path, sizeof(path), "fallback:%s", a840_router_predicate_name(predicate));
    return len >= 0 && (size_t)len < sizeof(path) ? path : NULL;
}

void a840_hwtest_phase3_register_adapters(A840HwtestPhase3NewUbwcFn new_ubwc_fn,
                                          A840HwtestPhase3ScanoutFn scanout_fn)
{
    (void)pthread_mutex_lock(&entry_lock);
    g_phase3_new_ubwc_fn = new_ubwc_fn;
    g_phase3_scanout_fn = scanout_fn;
    (void)pthread_mutex_unlock(&entry_lock);
}

int a840_hwtest_phase3_begin_scope(void *command, const char *request)
{
    if (!command || !valid_phase3_scope(request)) return -1;
    (void)pthread_mutex_lock(&entry_lock);
    Phase3Entry *slot = alloc_phase3_entry(command);
    slot->has_scope = true;
    (void)snprintf(slot->scope, sizeof(slot->scope), "%s", request);
    (void)pthread_mutex_unlock(&entry_lock);
    return 0;
}

bool a840_hwtest_phase3_get_scope(void *command, char *out_buf, size_t out_cap)
{
    bool ok = false;
    if (!command || !out_buf || out_cap == 0u) return false;
    out_buf[0] = '\0';
    (void)pthread_mutex_lock(&entry_lock);
    const Phase3Entry *slot = find_phase3_entry(command);
    if (slot && slot->has_scope) {
        (void)snprintf(out_buf, out_cap, "%s", slot->scope);
        ok = true;
    }
    (void)pthread_mutex_unlock(&entry_lock);
    return ok;
}

bool a840_hwtest_phase3_record_evidence(void *command, const A840Phase3Evidence *evidence)
{
    if (!command || !evidence) return false;
    (void)pthread_mutex_lock(&entry_lock);
    Phase3Entry *slot = alloc_phase3_entry(command);
    slot->evidence = *evidence;
    slot->evidence.bytes = (uint32_t)sizeof(A840Phase3Evidence);
    slot->evidence.version = A840_HWTEST_PHASE3_EVIDENCE_VERSION;
    slot->has_evidence = true;
    (void)pthread_mutex_unlock(&entry_lock);
    return true;
}

int a840_hwtest_phase3_evidence_for_command_buffer(void *command, A840Phase3Evidence *out_evidence)
{
    if (!command || !out_evidence) return -1;
    if (out_evidence->bytes != 0u && out_evidence->bytes != (uint32_t)sizeof(A840Phase3Evidence)) {
        return -1;
    }
    if (out_evidence->version != 0u && out_evidence->version != A840_HWTEST_PHASE3_EVIDENCE_VERSION) {
        return -1;
    }
    (void)pthread_mutex_lock(&entry_lock);
    const Phase3Entry *slot = find_phase3_entry(command);
    if (!slot || !slot->has_evidence) {
        (void)pthread_mutex_unlock(&entry_lock);
        return -1;
    }
    *out_evidence = slot->evidence;
    (void)pthread_mutex_unlock(&entry_lock);
    return 0;
}

void *a840_hwtest_phase3_new_ubwc_texture(void *device,
                                          uint32_t width,
                                          uint32_t height,
                                          unsigned for_scanout)
{
    A840HwtestPhase3NewUbwcFn fn = NULL;
    if (!device || width == 0u || height == 0u) return NULL;
    (void)pthread_mutex_lock(&entry_lock);
    fn = g_phase3_new_ubwc_fn;
    (void)pthread_mutex_unlock(&entry_lock);
    if (!fn) return NULL;
    return fn(device, width, height, for_scanout);
}

int a840_hwtest_phase3_encode_scanout(void *command,
                                      void *source,
                                      void *destination,
                                      unsigned mode)
{
    A840HwtestPhase3ScanoutFn fn = NULL;
    if (!command || !source || !destination || mode > 1u) return -1;
    (void)pthread_mutex_lock(&entry_lock);
    fn = g_phase3_scanout_fn;
    (void)pthread_mutex_unlock(&entry_lock);
    if (!fn) return -1;
    return fn(command, source, destination, mode);
}
