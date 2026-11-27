/* SPDX-License-Identifier: MIT */
#ifndef A840_CONTEXT_INIT_H
#define A840_CONTEXT_INIT_H

#include <stddef.h>
#include <stdint.h>
#include "Adreno840EmitABI.h"

/* Exact maxima for this emitter's uncoalesced Type4 packet representation.
 * All 49 static entries (two VA64 entries), RP/cache setup and optional
 * early reset/pre/postamble entries are included. No heap or external CS
 * symbol is required; packet encoding is crossed against a840_cs.c in tests.
 */
#define A840_CTX_INIT_MAX_DWORDS 173u
#define A840_BIN_PREAMBLE_MAX_DWORDS 124u
#define A840_CTX_INIT_MAX_RELOCS 6u
#define A840_BIN_PREAMBLE_MAX_RELOCS 2u
#define A840_CONTEXT_INIT_TABLE_ROWS 69u
#define A840_CONTEXT_BORDER_ALIGNMENT 128u
#define A840_CONTEXT_AMBLE_MAX_DWORDS ((UINT32_C(1) << 20) - 1u)
#define A840_CONTEXT_VIS_STREAM_MAX ((UINT32_C(1) << 16) - 1u)

enum {
    A840_CTX_STATIC = 0,
    A840_CTX_RP = 1,
    A840_CTX_UMD_BLOCKED = 2
};
enum {
    A840_CTX_NORMAL = 0,
    A840_CTX_NEAREST = 1,
    A840_CTX_BORDER_VA = 2,
    A840_CTX_EARLY_RESET = 3,
    A840_CTX_CACHE_BR = 4,
    A840_CTX_CACHE_BOTH = 5,
    A840_CTX_BIN_BR = 6,
    A840_CTX_BLOCKED = 7
};

/* vgt_event_type, pinned adreno_pm4.xml; these are event IDs, not opcodes. */
enum {
    A840_CTX_EVENT_CCU_INVALIDATE_COLOR = 25,
    A840_CTX_EVENT_CCU_INVALIDATE_DEPTH = 24,
    A840_CTX_EVENT_LRZ_INVALIDATE = 40,
    A840_CTX_EVENT_CACHE_INVALIDATE = 51
};

typedef struct A840ContextInitParams {
    uint64_t border_color_gpu_va;
    uint64_t border_color_bo; /* 0 is allowed for synthetic host/trusted VA tests. */
    uint64_t bin_preamble_br_va;
    uint64_t bin_preamble_bv_va;
    uint64_t preempt_preamble_va;
    uint64_t preempt_postamble_va;
    uint32_t bin_preamble_br_dwords;
    uint32_t bin_preamble_bv_dwords;
    uint32_t preempt_preamble_dwords;
    uint32_t preempt_postamble_dwords;
    uint32_t vis_stream_count; /* CP_BV_BR_COUNT_OPS BR_OFFSET: 1..65535 */
    uint32_t use_tex_coord_nearest_even; /* 0/1 */
    uint32_t enable_early_preamble_reset; /* 0/1; set 1 for normal A840 use */
} A840ContextInitParams;

/* Derived immutable data from caps/context_init_a840.tsv. Runtime VA64 and
 * source-dependent mode values are explicitly represented, never treated
 * as literal placeholder writes. Source order/repeated writes are retained.
 */
typedef struct A840ContextInitReg {
    uint32_t reg;
    uint32_t value;
    uint32_t bin_value;
    uint32_t scope;
    uint32_t role;
    uint32_t value_dwords;
    const char *reg_macro;
    const char *source_ref;
    const char *note;
} A840ContextInitReg;

#ifdef __cplusplus
extern "C" {
#endif

const A840ContextInitReg *a840_context_init_table(size_t *count);
const char *a840_context_init_tsv_sha256(void);
int a840_reg_is_umd_blocked_on_a8xx(uint32_t reg_offset);

/* Full cmd-buffer prefix, not KMD init or a submission. Bin BR/BV VAs must
 * be nonzero/word-aligned; optional pre/postambles must be either (0,0) or
 * a valid 48-bit word-aligned range. Border base is 128-byte aligned.
 * No write to CS words/relocs/metadata occurs on any rejected call.
 */
A840EmitStatus a840_emit_context_init(A840CmdStream *cs,
    const A840ContextInitParams *params);

/* Runs in the BR/BV thread assigned by CP_SET_AMBLE, as in Mesa. It does
 * not change CP thread selection. BV skips BR-only CCU/tile-begin writes.
 * Bin generation only needs border/rounding/reset fields (its own VA does
 * not exist yet); other prefix-only params are not consumed here.
 */
A840EmitStatus a840_emit_bin_preamble(A840CmdStream *cs,
    const A840ContextInitParams *params, int is_bv_thread);

#ifdef __cplusplus
}
#endif
#endif /* A840_CONTEXT_INIT_H */
