/* SPDX-License-Identifier: MIT */
#include "a840_hwtest_hook.h"

#include <pthread.h>
#include <stdio.h>

typedef struct Entry {
    void *cb;
    A840FallbackPredicate predicate;
} Entry;

static Entry entries[A840_HWTEST_RECENT_COMMANDS];
static size_t write_index;
static pthread_mutex_t entry_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local char path[128];

static Entry *find_entry(void *cb)
{
    for (size_t i = 0; i < A840_HWTEST_RECENT_COMMANDS; ++i)
        if (entries[i].cb == cb) return &entries[i];
    return NULL;
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
