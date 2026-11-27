/* SPDX-License-Identifier: MIT */
#ifndef A840_HWTEST_HOOK_H
#define A840_HWTEST_HOOK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/a840_dualpath_router.h"

enum {
    A840_HWTEST_RECENT_COMMANDS = 256,
    A840_HWTEST_PHASE3_EVIDENCE_VERSION = 1u,
    A840_HWTEST_SCOPE_MAX_BYTES = 64u
};

#ifdef __cplusplus
extern "C" {
#endif

/* Phase 3 executed-feature evidence layout aligned with NezhaGpuTestPhase3Runner.mm. */
typedef struct A840Phase3Evidence {
    uint32_t bytes;
    uint32_t version;
    uint32_t gmem_passes;
    uint32_t sysmem_passes;
    uint32_t memoryless_depth_passes;
    uint32_t fbfetch_draws;
    uint32_t tile_width;
    uint32_t tile_height;
    uint32_t tiles_x;
    uint32_t tiles_y;
    uint32_t ubwc_version;
    uint32_t highest_bank_bit;
    uint32_t swizzle_levels;
    uint32_t macrotile_mode;
    uint64_t compressed_blocks;
    uint64_t escape_blocks;
    uint32_t mdss_direct_ubwc;
    uint32_t mdss_native_linear;
} A840Phase3Evidence;

typedef void *(*A840HwtestPhase3NewUbwcFn)(void *device,
                                           uint32_t width,
                                           uint32_t height,
                                           unsigned for_scanout);

typedef int (*A840HwtestPhase3ScanoutFn)(void *command,
                                         void *source,
                                         void *destination,
                                         unsigned mode);

/* Call only after a backend accepted the CB. NONE describes the raw path. */
bool a840_hwtest_record_command_buffer(void *cb, A840FallbackPredicate predicate);
void a840_hwtest_forget_command_buffer(void *cb);

/* Exact signature used by NezhaGpuTest. The returned thread-local string
 * lasts until the next query on this thread. Unknown/evicted CBs return NULL. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("default")))
#endif
const char *a840_hwtest_path_for_command_buffer(void *cb);

/* Phase 3 real-device harness adapter registration (ObjC runtime or host test). */
void a840_hwtest_phase3_register_adapters(A840HwtestPhase3NewUbwcFn new_ubwc_fn,
                                          A840HwtestPhase3ScanoutFn scanout_fn);

/* Record executed Phase 3 evidence and inspect active scope for a command buffer. */
bool a840_hwtest_phase3_record_evidence(void *command, const A840Phase3Evidence *evidence);
bool a840_hwtest_phase3_get_scope(void *command, char *out_buf, size_t out_cap);

/* Exact exported Phase 3 adapter signatures imported by NezhaGpuTestPhase3Runner.mm. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("default")))
#endif
int a840_hwtest_phase3_begin_scope(void *command, const char *request);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("default")))
#endif
int a840_hwtest_phase3_evidence_for_command_buffer(void *command, A840Phase3Evidence *out_evidence);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("default")))
#endif
void *a840_hwtest_phase3_new_ubwc_texture(void *device,
                                          uint32_t width,
                                          uint32_t height,
                                          unsigned for_scanout);

#if defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("default")))
#endif
int a840_hwtest_phase3_encode_scanout(void *command,
                                      void *source,
                                      void *destination,
                                      unsigned mode);

#ifdef __cplusplus
}
#endif

#endif /* A840_HWTEST_HOOK_H */
