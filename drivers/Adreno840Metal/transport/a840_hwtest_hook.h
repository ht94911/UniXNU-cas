/* SPDX-License-Identifier: MIT */
#ifndef A840_HWTEST_HOOK_H
#define A840_HWTEST_HOOK_H

#include "../core/a840_dualpath_router.h"

enum { A840_HWTEST_RECENT_COMMANDS = 256 };

#ifdef __cplusplus
extern "C" {
#endif

/* Call only after a backend accepted the CB. NONE describes the raw path. */
bool a840_hwtest_record_command_buffer(void *cb, A840FallbackPredicate predicate);
void a840_hwtest_forget_command_buffer(void *cb);

/* Exact signature used by NezhaGpuTest. The returned thread-local string
 * lasts until the next query on this thread. Unknown/evicted CBs return NULL. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("default")))
#endif
const char *a840_hwtest_path_for_command_buffer(void *cb);

#ifdef __cplusplus
}
#endif

#endif /* A840_HWTEST_HOOK_H */
