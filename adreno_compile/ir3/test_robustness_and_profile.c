/* SPDX-License-Identifier: MIT */
/* Wave 9 / Lane 2: Real Mesa ir3 Robustness A/B + ISA Static Profiler E2E Test.
 *
 * Links against real libadreno_compile_ir3.a (pinned Mesa 85fda3b36405, zero
 * src/freedreno/vulkan) and libnative_metal_translate.a. Compiles >=12 real
 * SPIR-V shaders (covering real 24A446 metallibs + dynamic UBO/SSBO bounds
 * kernels + Apple XR10 wide-color / FBFetch fragment shaders) under both
 * ROBUST_ON and ROBUST_OFF, verifies zero regressions and XR10 safety
 * preservation, checks negative policy/stats error paths, and optionally
 * dumps structured A/B metrics JSON via --dump-json <path>.
 */
#define _POSIX_C_SOURCE 200809L

#include "AdrenoCompileABI.h"
#include "NativeMetalTranslate.h"
#include "a840_ir3_stats.h"
#include "a840_robustness_policy.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t g_checks = 0u;
static size_t g_cases = 0u;

static void
check_cond(bool ok, const char *what, const char *detail)
{
    g_checks++;
    if (!ok) {
        fprintf(stderr, "FAIL: %s%s%s\n", what,
                (detail && detail[0]) ? ": " : "",
                (detail && detail[0]) ? detail : "");
        exit(EXIT_FAILURE);
    }
}

/* --- Minimal deterministic SPIR-V 1.3 builder in pure C11 --- */
typedef struct SpvBuf {
    uint32_t *words;
    size_t len;
    size_t cap;
} SpvBuf;

static void
spv_init(SpvBuf *b, uint32_t bound)
{
    b->cap = 512u;
    b->words = (uint32_t *)malloc(b->cap * sizeof(uint32_t));
    check_cond(b->words != NULL, "spv_init malloc", NULL);
    b->len = 0u;
    b->words[b->len++] = 0x07230203u; /* Magic */
    b->words[b->len++] = 0x00010300u; /* Version 1.3 */
    b->words[b->len++] = 0u;          /* Generator */
    b->words[b->len++] = bound;       /* Bound */
    b->words[b->len++] = 0u;          /* Schema */
}

static void
spv_free(SpvBuf *b)
{
    free(b->words);
    b->words = NULL;
    b->len = 0u;
    b->cap = 0u;
}

static void
spv_push(SpvBuf *b, uint32_t w)
{
    if (b->len >= b->cap) {
        b->cap *= 2u;
        b->words = (uint32_t *)realloc(b->words, b->cap * sizeof(uint32_t));
        check_cond(b->words != NULL, "spv_push realloc", NULL);
    }
    b->words[b->len++] = w;
}

static void
spv_op(SpvBuf *b, uint16_t opcode, const uint32_t *ops, size_t nops)
{
    uint32_t wc = (uint32_t)(nops + 1u);
    spv_push(b, (wc << 16) | (uint32_t)opcode);
    for (size_t i = 0u; i < nops; ++i)
        spv_push(b, ops[i]);
}

static void
spv_entry(SpvBuf *b, uint32_t exec_model, uint32_t fn_id, const char *name,
          const uint32_t *ifaces, size_t nifaces)
{
    size_t slen = strlen(name) + 1u;
    size_t sw = (slen + 3u) / 4u;
    uint32_t buf[64];
    check_cond(2u + sw + nifaces <= 64u, "spv_entry buf overflow", name);
    buf[0] = exec_model;
    buf[1] = fn_id;
    memset(&buf[2], 0, sw * sizeof(uint32_t));
    for (size_t i = 0u; i < slen; ++i) {
        buf[2u + i / 4u] |= ((uint32_t)(unsigned char)name[i]) << ((i % 4u) * 8u);
    }
    for (size_t i = 0u; i < nifaces; ++i)
        buf[2u + sw + i] = ifaces[i];
    spv_op(b, 15u /* OpEntryPoint */, buf, 2u + sw + nifaces);
}

#define SPV_OP(b, op, ...)                                                   \
    do {                                                                     \
        const uint32_t _ops[] = { __VA_ARGS__ };                             \
        spv_op((b), (op), _ops, sizeof(_ops) / sizeof(_ops[0]));             \
    } while (0)

#define SPV_OP0(b, op) spv_op((b), (op), NULL, 0u)

/* SPIR-V opcodes & constants used across synthetic dynamic-index / XR10 shaders */
enum {
    OP_MEMORY_MODEL         = 14u,
    OP_EXECUTION_MODE       = 16u,
    OP_CAPABILITY           = 17u,
    OP_TYPE_VOID            = 19u,
    OP_TYPE_BOOL            = 20u,
    OP_TYPE_INT             = 21u,
    OP_TYPE_FLOAT           = 22u,
    OP_TYPE_VECTOR          = 23u,
    OP_TYPE_IMAGE           = 25u,
    OP_TYPE_RUNTIME_ARRAY   = 29u,
    OP_TYPE_STRUCT          = 30u,
    OP_TYPE_POINTER         = 32u,
    OP_TYPE_FUNCTION        = 33u,
    OP_CONSTANT             = 43u,
    OP_CONSTANT_COMPOSITE   = 44u,
    OP_FUNCTION             = 54u,
    OP_FUNCTION_END         = 56u,
    OP_VARIABLE             = 59u,
    OP_LOAD                 = 61u,
    OP_STORE                = 62u,
    OP_ACCESS_CHAIN         = 65u,
    OP_DECORATE             = 71u,
    OP_MEMBER_DECORATE      = 72u,
    OP_COMPOSITE_CONSTRUCT  = 80u,
    OP_COMPOSITE_EXTRACT    = 81u,
    OP_IMAGE_READ           = 98u,
    OP_IADD                 = 128u,
    OP_FADD                 = 129u,
    OP_ISUB                 = 130u,
    OP_FMUL                 = 133u,
    OP_SELECT               = 169u,
    OP_ULESS_THAN           = 176u,
    OP_FORD_LESS_THAN       = 184u,
    OP_FORD_GREATER_THAN    = 186u,
    OP_ARRAY_LENGTH         = 68u,
    OP_LABEL                = 248u,
    OP_RETURN               = 253u,

    CAP_SHADER              = 1u,
    CAP_INPUT_ATTACHMENT    = 40u,
    EXEC_VERTEX             = 0u,
    EXEC_FRAGMENT           = 4u,
    EXEC_GLCOMPUTE          = 5u,
    MODE_ORIGIN_UPPER_LEFT  = 7u,
    MODE_LOCAL_SIZE         = 17u,

    SC_UNIFORM_CONSTANT     = 0u,
    SC_INPUT                = 1u,
    SC_UNIFORM              = 2u,
    SC_OUTPUT               = 3u,
    SC_STORAGE_BUFFER       = 12u,

    DEC_BLOCK               = 2u,
    DEC_ARRAY_STRIDE        = 6u,
    DEC_BUILTIN             = 11u,
    DEC_FLAT                = 14u,
    DEC_LOCATION            = 30u,
    DEC_BINDING             = 33u,
    DEC_DESC_SET            = 34u,
    DEC_OFFSET              = 35u,
    DEC_INPUT_ATT_IDX       = 43u,
    BUILTIN_POSITION        = 0u,
    BUILTIN_GLOBAL_INVOC_ID = 28u,
    BUILTIN_VERTEX_INDEX    = 42u
};

static uint32_t
f32_bits(float v)
{
    uint32_t u = 0u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

/* Synthetic Compute SSBO kernel with explicit software bounds checks
 * (OpArrayLength + ULessThan + OpSelect) across `num_loads` dynamic elements.
 */
static SpvBuf
build_compute_ssbo_bounds_spv(uint32_t num_loads)
{
    SpvBuf b;
    spv_init(&b, 160u);

    SPV_OP(&b, OP_CAPABILITY, CAP_SHADER);
    SPV_OP(&b, OP_MEMORY_MODEL, 0u, 1u);
    const uint32_t ifaces[] = { 16u };
    spv_entry(&b, EXEC_GLCOMPUTE, 3u, "main", ifaces, 1u);
    SPV_OP(&b, OP_EXECUTION_MODE, 3u, MODE_LOCAL_SIZE, 64u, 1u, 1u);

    SPV_OP(&b, OP_DECORATE, 10u, DEC_ARRAY_STRIDE, 4u);
    SPV_OP(&b, OP_DECORATE, 11u, DEC_BLOCK);
    SPV_OP(&b, OP_MEMBER_DECORATE, 11u, 0u, DEC_OFFSET, 0u);
    SPV_OP(&b, OP_DECORATE, 13u, DEC_DESC_SET, 0u);
    SPV_OP(&b, OP_DECORATE, 13u, DEC_BINDING, 0u);
    SPV_OP(&b, OP_DECORATE, 14u, DEC_DESC_SET, 0u);
    SPV_OP(&b, OP_DECORATE, 14u, DEC_BINDING, 1u);
    SPV_OP(&b, OP_DECORATE, 16u, DEC_BUILTIN, BUILTIN_GLOBAL_INVOC_ID);

    SPV_OP(&b, OP_TYPE_VOID, 1u);
    SPV_OP(&b, OP_TYPE_FUNCTION, 2u, 1u);
    SPV_OP(&b, OP_TYPE_BOOL, 4u);
    SPV_OP(&b, OP_TYPE_INT, 5u, 32u, 0u);
    SPV_OP(&b, OP_TYPE_FLOAT, 6u, 32u);
    SPV_OP(&b, OP_TYPE_VECTOR, 7u, 5u, 3u);
    SPV_OP(&b, OP_TYPE_POINTER, 8u, SC_INPUT, 7u);
    SPV_OP(&b, OP_TYPE_POINTER, 9u, SC_STORAGE_BUFFER, 6u);
    SPV_OP(&b, OP_TYPE_RUNTIME_ARRAY, 10u, 6u);
    SPV_OP(&b, OP_TYPE_STRUCT, 11u, 10u);
    SPV_OP(&b, OP_TYPE_POINTER, 12u, SC_STORAGE_BUFFER, 11u);

    SPV_OP(&b, OP_VARIABLE, 12u, 13u, SC_STORAGE_BUFFER);
    SPV_OP(&b, OP_VARIABLE, 12u, 14u, SC_STORAGE_BUFFER);
    SPV_OP(&b, OP_VARIABLE, 8u, 16u, SC_INPUT);

    SPV_OP(&b, OP_CONSTANT, 5u, 20u, 0u);
    SPV_OP(&b, OP_CONSTANT, 5u, 21u, 1u);
    SPV_OP(&b, OP_CONSTANT, 5u, 22u, 2u);
    SPV_OP(&b, OP_CONSTANT, 5u, 23u, 3u);
    SPV_OP(&b, OP_CONSTANT, 6u, 25u, f32_bits(0.0f));

    SPV_OP(&b, OP_FUNCTION, 1u, 3u, 0u, 2u);
    SPV_OP(&b, OP_LABEL, 30u);

    SPV_OP(&b, OP_LOAD, 7u, 31u, 16u);
    SPV_OP(&b, OP_COMPOSITE_EXTRACT, 5u, 32u, 31u, 0u);
    SPV_OP(&b, OP_ARRAY_LENGTH, 5u, 33u, 13u, 0u);
    SPV_OP(&b, OP_ARRAY_LENGTH, 5u, 34u, 14u, 0u);

    uint32_t acc_id = 25u;
    uint32_t next_id = 40u;
    const uint32_t off_ids[4] = { 20u, 21u, 22u, 23u };

    for (uint32_t k = 0u; k < num_loads && k < 4u; ++k) {
        uint32_t idx_id = next_id++;
        uint32_t cmp_id = next_id++;
        uint32_t safe_idx = next_id++;
        uint32_t ptr_id = next_id++;
        uint32_t val_id = next_id++;
        uint32_t masked_id = next_id++;
        uint32_t new_acc = next_id++;

        SPV_OP(&b, OP_IADD, 5u, idx_id, 32u, off_ids[k]);
        SPV_OP(&b, OP_ULESS_THAN, 4u, cmp_id, idx_id, 33u);
        SPV_OP(&b, OP_SELECT, 5u, safe_idx, cmp_id, idx_id, 20u);
        SPV_OP(&b, OP_ACCESS_CHAIN, 9u, ptr_id, 13u, 20u, safe_idx);
        SPV_OP(&b, OP_LOAD, 6u, val_id, ptr_id);
        SPV_OP(&b, OP_SELECT, 6u, masked_id, cmp_id, val_id, 25u);
        SPV_OP(&b, OP_FADD, 6u, new_acc, acc_id, masked_id);
        acc_id = new_acc;
    }

    uint32_t out_cmp = next_id++;
    uint32_t out_safe = next_id++;
    uint32_t out_ptr = next_id++;
    SPV_OP(&b, OP_ULESS_THAN, 4u, out_cmp, 32u, 34u);
    SPV_OP(&b, OP_SELECT, 5u, out_safe, out_cmp, 32u, 20u);
    SPV_OP(&b, OP_ACCESS_CHAIN, 9u, out_ptr, 14u, 20u, out_safe);
    SPV_OP(&b, OP_STORE, out_ptr, acc_id);

    SPV_OP0(&b, OP_RETURN);
    SPV_OP0(&b, OP_FUNCTION_END);
    return b;
}

/* Synthetic Vertex shader with dynamic SSBO vertex-pulling + bounds check. */
static SpvBuf
build_vertex_ssbo_pull_spv(void)
{
    SpvBuf b;
    spv_init(&b, 120u);

    SPV_OP(&b, OP_CAPABILITY, CAP_SHADER);
    SPV_OP(&b, OP_MEMORY_MODEL, 0u, 1u);
    const uint32_t ifaces[] = { 15u, 17u };
    spv_entry(&b, EXEC_VERTEX, 3u, "main", ifaces, 2u);

    SPV_OP(&b, OP_DECORATE, 10u, DEC_ARRAY_STRIDE, 16u);
    SPV_OP(&b, OP_DECORATE, 11u, DEC_BLOCK);
    SPV_OP(&b, OP_MEMBER_DECORATE, 11u, 0u, DEC_OFFSET, 0u);
    SPV_OP(&b, OP_DECORATE, 13u, DEC_DESC_SET, 0u);
    SPV_OP(&b, OP_DECORATE, 13u, DEC_BINDING, 0u);
    SPV_OP(&b, OP_DECORATE, 15u, DEC_BUILTIN, BUILTIN_VERTEX_INDEX);
    SPV_OP(&b, OP_DECORATE, 17u, DEC_BUILTIN, BUILTIN_POSITION);

    SPV_OP(&b, OP_TYPE_VOID, 1u);
    SPV_OP(&b, OP_TYPE_FUNCTION, 2u, 1u);
    SPV_OP(&b, OP_TYPE_BOOL, 4u);
    SPV_OP(&b, OP_TYPE_INT, 5u, 32u, 0u);
    SPV_OP(&b, OP_TYPE_FLOAT, 6u, 32u);
    SPV_OP(&b, OP_TYPE_VECTOR, 7u, 6u, 4u);
    SPV_OP(&b, OP_TYPE_POINTER, 8u, SC_INPUT, 5u);
    SPV_OP(&b, OP_TYPE_POINTER, 9u, SC_STORAGE_BUFFER, 7u);
    SPV_OP(&b, OP_TYPE_RUNTIME_ARRAY, 10u, 7u);
    SPV_OP(&b, OP_TYPE_STRUCT, 11u, 10u);
    SPV_OP(&b, OP_TYPE_POINTER, 12u, SC_STORAGE_BUFFER, 11u);
    SPV_OP(&b, OP_TYPE_POINTER, 16u, SC_OUTPUT, 7u);

    SPV_OP(&b, OP_VARIABLE, 12u, 13u, SC_STORAGE_BUFFER);
    SPV_OP(&b, OP_VARIABLE, 8u, 15u, SC_INPUT);
    SPV_OP(&b, OP_VARIABLE, 16u, 17u, SC_OUTPUT);

    SPV_OP(&b, OP_CONSTANT, 5u, 20u, 0u);
    SPV_OP(&b, OP_CONSTANT, 6u, 21u, f32_bits(0.0f));
    SPV_OP(&b, OP_CONSTANT, 6u, 22u, f32_bits(1.0f));
    SPV_OP(&b, OP_CONSTANT_COMPOSITE, 7u, 23u, 21u, 21u, 21u, 22u);

    SPV_OP(&b, OP_FUNCTION, 1u, 3u, 0u, 2u);
    SPV_OP(&b, OP_LABEL, 30u);
    SPV_OP(&b, OP_LOAD, 5u, 31u, 15u);
    SPV_OP(&b, OP_ARRAY_LENGTH, 5u, 32u, 13u, 0u);
    SPV_OP(&b, OP_ULESS_THAN, 4u, 33u, 31u, 32u);
    SPV_OP(&b, OP_SELECT, 5u, 34u, 33u, 31u, 20u);
    SPV_OP(&b, OP_ACCESS_CHAIN, 9u, 35u, 13u, 20u, 34u);
    SPV_OP(&b, OP_LOAD, 7u, 36u, 35u);
    SPV_OP(&b, OP_SELECT, 7u, 37u, 33u, 36u, 23u);
    SPV_OP(&b, OP_STORE, 17u, 37u);
    SPV_OP0(&b, OP_RETURN);
    SPV_OP0(&b, OP_FUNCTION_END);
    return b;
}

/* Synthetic Fragment XR10 wide-color + FBFetch GMEM + SSBO LUT shader. */
static SpvBuf
build_fragment_xr10_fbfetch_spv(void)
{
    SpvBuf b;
    spv_init(&b, 140u);

    SPV_OP(&b, OP_CAPABILITY, CAP_SHADER);
    SPV_OP(&b, OP_CAPABILITY, CAP_INPUT_ATTACHMENT);
    SPV_OP(&b, OP_MEMORY_MODEL, 0u, 1u);
    const uint32_t ifaces[] = { 18u, 20u };
    spv_entry(&b, EXEC_FRAGMENT, 3u, "main", ifaces, 2u);
    SPV_OP(&b, OP_EXECUTION_MODE, 3u, MODE_ORIGIN_UPPER_LEFT);

    SPV_OP(&b, OP_DECORATE, 10u, DEC_ARRAY_STRIDE, 16u);
    SPV_OP(&b, OP_DECORATE, 11u, DEC_BLOCK);
    SPV_OP(&b, OP_MEMBER_DECORATE, 11u, 0u, DEC_OFFSET, 0u);
    SPV_OP(&b, OP_DECORATE, 13u, DEC_DESC_SET, 0u);
    SPV_OP(&b, OP_DECORATE, 13u, DEC_BINDING, 0u);

    SPV_OP(&b, OP_DECORATE, 16u, DEC_DESC_SET, 0u);
    SPV_OP(&b, OP_DECORATE, 16u, DEC_BINDING, 1u);
    SPV_OP(&b, OP_DECORATE, 16u, DEC_INPUT_ATT_IDX, 0u);

    SPV_OP(&b, OP_DECORATE, 18u, DEC_LOCATION, 0u);
    SPV_OP(&b, OP_DECORATE, 18u, DEC_FLAT);
    SPV_OP(&b, OP_DECORATE, 20u, DEC_LOCATION, 0u);

    SPV_OP(&b, OP_TYPE_VOID, 1u);
    SPV_OP(&b, OP_TYPE_FUNCTION, 2u, 1u);
    SPV_OP(&b, OP_TYPE_BOOL, 4u);
    SPV_OP(&b, OP_TYPE_INT, 5u, 32u, 0u);
    SPV_OP(&b, OP_TYPE_FLOAT, 6u, 32u);
    SPV_OP(&b, OP_TYPE_VECTOR, 7u, 6u, 4u);
    SPV_OP(&b, OP_TYPE_VECTOR, 8u, 5u, 2u);
    SPV_OP(&b, OP_TYPE_POINTER, 9u, SC_STORAGE_BUFFER, 7u);
    SPV_OP(&b, OP_TYPE_RUNTIME_ARRAY, 10u, 7u);
    SPV_OP(&b, OP_TYPE_STRUCT, 11u, 10u);
    SPV_OP(&b, OP_TYPE_POINTER, 12u, SC_STORAGE_BUFFER, 11u);
    SPV_OP(&b, OP_TYPE_IMAGE, 14u, 6u, 6u, 0u, 0u, 0u, 2u, 0u);
    SPV_OP(&b, OP_TYPE_POINTER, 15u, SC_UNIFORM_CONSTANT, 14u);
    SPV_OP(&b, OP_TYPE_POINTER, 17u, SC_INPUT, 5u);
    SPV_OP(&b, OP_TYPE_POINTER, 19u, SC_OUTPUT, 7u);
    SPV_OP(&b, OP_TYPE_VECTOR, 21u, 4u, 4u);

    SPV_OP(&b, OP_VARIABLE, 12u, 13u, SC_STORAGE_BUFFER);
    SPV_OP(&b, OP_VARIABLE, 15u, 16u, SC_UNIFORM_CONSTANT);
    SPV_OP(&b, OP_VARIABLE, 17u, 18u, SC_INPUT);
    SPV_OP(&b, OP_VARIABLE, 19u, 20u, SC_OUTPUT);

    SPV_OP(&b, OP_CONSTANT, 5u, 25u, 0u);
    SPV_OP(&b, OP_CONSTANT_COMPOSITE, 8u, 26u, 25u, 25u);
    SPV_OP(&b, OP_CONSTANT, 6u, 27u, f32_bits(A840_XR10_MIN_VALUE));
    SPV_OP(&b, OP_CONSTANT, 6u, 28u, f32_bits(A840_XR10_MAX_VALUE));
    SPV_OP(&b, OP_CONSTANT_COMPOSITE, 7u, 29u, 27u, 27u, 27u, 27u);
    SPV_OP(&b, OP_CONSTANT_COMPOSITE, 7u, 30u, 28u, 28u, 28u, 28u);
    SPV_OP(&b, OP_CONSTANT, 6u, 31u, f32_bits(0.0f));
    SPV_OP(&b, OP_CONSTANT_COMPOSITE, 7u, 32u, 31u, 31u, 31u, 31u);

    SPV_OP(&b, OP_FUNCTION, 1u, 3u, 0u, 2u);
    SPV_OP(&b, OP_LABEL, 40u);

    SPV_OP(&b, OP_LOAD, 5u, 41u, 18u);
    SPV_OP(&b, OP_ARRAY_LENGTH, 5u, 42u, 13u, 0u);
    SPV_OP(&b, OP_ULESS_THAN, 4u, 43u, 41u, 42u);
    SPV_OP(&b, OP_SELECT, 5u, 44u, 43u, 41u, 25u);
    SPV_OP(&b, OP_ACCESS_CHAIN, 9u, 45u, 13u, 25u, 44u);
    SPV_OP(&b, OP_LOAD, 7u, 46u, 45u);
    SPV_OP(&b, OP_SELECT, 7u, 47u, 43u, 46u, 32u);

    SPV_OP(&b, OP_LOAD, 14u, 48u, 16u);
    SPV_OP(&b, OP_IMAGE_READ, 7u, 49u, 48u, 26u);
    SPV_OP(&b, OP_FADD, 7u, 50u, 47u, 49u);

    /* Apple XR10 wide-color clamp [-0.752941, 1.25098] (preserved) */
    SPV_OP(&b, OP_FORD_LESS_THAN, 21u, 51u, 50u, 29u);
    SPV_OP(&b, OP_SELECT, 7u, 52u, 51u, 29u, 50u);
    SPV_OP(&b, OP_FORD_GREATER_THAN, 21u, 53u, 52u, 30u);
    SPV_OP(&b, OP_SELECT, 7u, 54u, 53u, 30u, 52u);

    SPV_OP(&b, OP_STORE, 20u, 54u);
    SPV_OP0(&b, OP_RETURN);
    SPV_OP0(&b, OP_FUNCTION_END);
    return b;
}

/* Synthetic Fragment shader with 4 scalar SSBO loads at dynamic offset
 * (exercises nir_opt_load_store_vectorize under ROBUST_ON vs ROBUST_OFF).
 */
static SpvBuf
build_fragment_ssbo_vec_spv(void)
{
    SpvBuf b;
    spv_init(&b, 140u);

    SPV_OP(&b, OP_CAPABILITY, CAP_SHADER);
    SPV_OP(&b, OP_MEMORY_MODEL, 0u, 1u);
    const uint32_t ifaces[] = { 16u, 18u };
    spv_entry(&b, EXEC_FRAGMENT, 3u, "main", ifaces, 2u);
    SPV_OP(&b, OP_EXECUTION_MODE, 3u, MODE_ORIGIN_UPPER_LEFT);

    SPV_OP(&b, OP_DECORATE, 10u, DEC_ARRAY_STRIDE, 4u);
    SPV_OP(&b, OP_DECORATE, 11u, DEC_BLOCK);
    SPV_OP(&b, OP_MEMBER_DECORATE, 11u, 0u, DEC_OFFSET, 0u);
    SPV_OP(&b, OP_DECORATE, 13u, DEC_DESC_SET, 0u);
    SPV_OP(&b, OP_DECORATE, 13u, DEC_BINDING, 0u);
    SPV_OP(&b, OP_DECORATE, 16u, DEC_LOCATION, 0u);
    SPV_OP(&b, OP_DECORATE, 16u, DEC_FLAT);
    SPV_OP(&b, OP_DECORATE, 18u, DEC_LOCATION, 0u);

    SPV_OP(&b, OP_TYPE_VOID, 1u);
    SPV_OP(&b, OP_TYPE_FUNCTION, 2u, 1u);
    SPV_OP(&b, OP_TYPE_INT, 5u, 32u, 0u);
    SPV_OP(&b, OP_TYPE_FLOAT, 6u, 32u);
    SPV_OP(&b, OP_TYPE_VECTOR, 7u, 6u, 4u);
    SPV_OP(&b, OP_TYPE_POINTER, 9u, SC_STORAGE_BUFFER, 6u);
    SPV_OP(&b, OP_TYPE_RUNTIME_ARRAY, 10u, 6u);
    SPV_OP(&b, OP_TYPE_STRUCT, 11u, 10u);
    SPV_OP(&b, OP_TYPE_POINTER, 12u, SC_STORAGE_BUFFER, 11u);
    SPV_OP(&b, OP_TYPE_POINTER, 15u, SC_INPUT, 5u);
    SPV_OP(&b, OP_TYPE_POINTER, 17u, SC_OUTPUT, 7u);

    SPV_OP(&b, OP_VARIABLE, 12u, 13u, SC_STORAGE_BUFFER);
    SPV_OP(&b, OP_VARIABLE, 15u, 16u, SC_INPUT);
    SPV_OP(&b, OP_VARIABLE, 17u, 18u, SC_OUTPUT);

    SPV_OP(&b, OP_CONSTANT, 5u, 20u, 0u);
    SPV_OP(&b, OP_CONSTANT, 5u, 21u, 1u);
    SPV_OP(&b, OP_CONSTANT, 5u, 22u, 2u);
    SPV_OP(&b, OP_CONSTANT, 5u, 23u, 3u);

    SPV_OP(&b, OP_FUNCTION, 1u, 3u, 0u, 2u);
    SPV_OP(&b, OP_LABEL, 30u);
    SPV_OP(&b, OP_LOAD, 5u, 31u, 16u);

    SPV_OP(&b, OP_ACCESS_CHAIN, 9u, 32u, 13u, 20u, 31u);
    SPV_OP(&b, OP_LOAD, 6u, 33u, 32u);

    SPV_OP(&b, OP_IADD, 5u, 34u, 31u, 21u);
    SPV_OP(&b, OP_ACCESS_CHAIN, 9u, 35u, 13u, 20u, 34u);
    SPV_OP(&b, OP_LOAD, 6u, 36u, 35u);

    SPV_OP(&b, OP_IADD, 5u, 37u, 31u, 22u);
    SPV_OP(&b, OP_ACCESS_CHAIN, 9u, 38u, 13u, 20u, 37u);
    SPV_OP(&b, OP_LOAD, 6u, 39u, 38u);

    SPV_OP(&b, OP_IADD, 5u, 40u, 31u, 23u);
    SPV_OP(&b, OP_ACCESS_CHAIN, 9u, 41u, 13u, 20u, 40u);
    SPV_OP(&b, OP_LOAD, 6u, 42u, 41u);

    SPV_OP(&b, OP_COMPOSITE_CONSTRUCT, 7u, 43u, 33u, 36u, 39u, 42u);
    SPV_OP(&b, OP_STORE, 18u, 43u);
    SPV_OP0(&b, OP_RETURN);
    SPV_OP0(&b, OP_FUNCTION_END);
    return b;
}

typedef struct AbRecord {
    char name[128];
    char category[32];
    char source[64];
    char stage[16];
    bool is_xr10;
    bool expects_reduction;
    A840Ir3ShaderStats on_stats;
    A840Ir3ShaderStats off_stats;
    A840Ir3StatsDiff diff;
} AbRecord;

static A840CompileTarget
kaanapali_target(void)
{
    A840CompileTarget t;
    memset(&t, 0, sizeof(t));
    t.abi_version = ADRENO_COMPILE_ABI_VERSION;
    t.chip_id = UINT64_C(0x44050a31);
    t.num_slices = 3u;
    t.num_ccu = 6u;
    t.highest_bank_bit = 16u;
    t.ubwc_swizzle = 6u;
    t.macrotile_mode = 1u;
    return t;
}

static uint8_t *
read_file_all(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    check_cond(f != NULL, "fopen metallib", path);
    check_cond(fseek(f, 0, SEEK_END) == 0, "fseek end", path);
    long sz = ftell(f);
    check_cond(sz > 0, "ftell metallib", path);
    check_cond(fseek(f, 0, SEEK_SET) == 0, "fseek set", path);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    check_cond(buf != NULL, "malloc metallib", path);
    size_t got = fread(buf, 1u, (size_t)sz, f);
    fclose(f);
    check_cond(got == (size_t)sz, "fread metallib", path);
    *out_len = got;
    return buf;
}

static const char *
stage_name(uint32_t stage)
{
    switch (stage) {
    case A840_STAGE_VERTEX:   return "vertex";
    case A840_STAGE_FRAGMENT: return "fragment";
    case A840_STAGE_COMPUTE:  return "compute";
    default:                  return "unknown";
    }
}

static void
run_ab_case(const char *name,
            const char *category,
            const char *source,
            uint32_t stage,
            const uint32_t *spirv_words,
            size_t spirv_bytes,
            const char *entry_point,
            uint32_t extra_flags,
            uint32_t color_rt_mask,
            uint32_t half_rt_mask,
            bool is_xr10,
            bool expects_reduction,
            AbRecord *out_rec)
{
    g_cases++;
    A840CompileTarget target = kaanapali_target();

    A840StageInput in_on;
    memset(&in_on, 0, sizeof(in_on));
    in_on.abi_version = ADRENO_COMPILE_ABI_VERSION;
    in_on.stage = stage;
    in_on.spirv_words = spirv_words;
    in_on.spirv_bytes = spirv_bytes;
    in_on.entry_point = entry_point;
    in_on.color_rt_mask = color_rt_mask;
    in_on.half_rt_mask = half_rt_mask;
    in_on.sample_count = 1u;
    if (stage == A840_STAGE_COMPUTE) {
        in_on.local_size[0] = 8u;
        in_on.local_size[1] = 8u;
        in_on.local_size[2] = 1u;
    }
    in_on.flags = extra_flags;

    A840StageInput in_off = in_on;
    in_off.flags = extra_flags | A840_COMPILE_FLAG_ROBUST_OFF;

    A840RobustnessDecision dec_on, dec_off;
    uint32_t pol_extra = is_xr10 ? A840_COMPILE_FLAG_XR10_CLAMP : 0u;
    bool has_fb = (extra_flags & A840_COMPILE_FLAG_FBFETCH_GMEM) != 0u;
    check_cond(a840_robustness_decide_for_stage(&in_on, in_on.flags | pol_extra, true, false, has_fb, &dec_on) == A840_COMPILE_OK,
               "policy decide ROBUST_ON", name);
    check_cond(a840_robustness_decide_for_stage(&in_off, in_off.flags | pol_extra, true, false, has_fb, &dec_off) == A840_COMPILE_OK,
               "policy decide ROBUST_OFF", name);

    check_cond(!dec_on.elide_buffer_bounds &&
               dec_on.nir_robust_modes == (A840_NIR_ROBUST_MODE_UBO | A840_NIR_ROBUST_MODE_SSBO),
               "ROBUST_ON keeps UBO|SSBO robust_modes", name);
    check_cond(dec_off.elide_buffer_bounds &&
               dec_off.requires_zero_page_guard &&
               dec_off.nir_robust_modes == A840_NIR_ROBUST_MODE_NONE,
               "ROBUST_OFF elides buffer bounds with zero-page guard", name);
    if (is_xr10) {
        check_cond(dec_on.preserve_xr_oob && dec_off.preserve_xr_oob,
                   "XR10 OOB semantics strictly preserved", name);
    }

    A840CompiledStage st_on, st_off;
    memset(&st_on, 0, sizeof(st_on));
    memset(&st_off, 0, sizeof(st_off));
    char err[512] = { 0 };

    A840CompileStatus r_on = a840_compile_stage(&target, &in_on, &st_on, err, sizeof(err));
    check_cond(r_on == A840_COMPILE_OK, name, err);
    A840CompileStatus r_off = a840_compile_stage(&target, &in_off, &st_off, err, sizeof(err));
    check_cond(r_off == A840_COMPILE_OK, name, err);

    A840Ir3ShaderStats s_on, s_off;
    A840Ir3StatsDiff diff;
    check_cond(a840_ir3_stats_analyze(&st_on, dec_on.policy_flags, &s_on) == A840_COMPILE_OK,
               "stats analyze ROBUST_ON", name);
    check_cond(a840_ir3_stats_analyze(&st_off, dec_off.policy_flags, &s_off) == A840_COMPILE_OK,
               "stats analyze ROBUST_OFF", name);
    check_cond(a840_ir3_stats_diff(&s_on, &s_off, &diff) == A840_COMPILE_OK,
               "stats diff", name);

    check_cond(s_on.total_instrs > 0u && s_off.total_instrs > 0u, "non-empty ISA", name);
    check_cond(s_on.est_waves_per_sp > 0u && s_off.est_waves_per_sp > 0u, "waves > 0", name);
    check_cond(!diff.instr_regressed, "no instruction count regression under ROBUST_OFF", name);
    check_cond(!diff.reg_regressed, "no register pressure regression under ROBUST_OFF", name);
    check_cond(!diff.wave_regressed, "no wave occupancy regression under ROBUST_OFF", name);
    check_cond(diff.xr_preserved, "XR10 safety preserved across A/B", name);

    if (expects_reduction) {
        check_cond(diff.saved_total_instrs > 0, "expected real ISA instruction reduction", name);
    }
    if (has_fb) {
        check_cond(s_on.uses_fbfetch && s_off.uses_fbfetch, "FBFetch flag preserved", name);
    }

    printf("  [AB] %-40s %-12s %-8s instrs: %4u -> %4u (%+4d, %5.1f%%) "
           "alu:%3u->%3u mem:%2u->%2u regF:%2u->%2u regH:%2u->%2u waves:%2u->%2u xr=%u\n",
           name, category, stage_name(stage),
           s_on.total_instrs, s_off.total_instrs, diff.delta_total_instrs, diff.reduction_pct,
           s_on.cat234_alu, s_off.cat234_alu, s_on.cat6_mem, s_off.cat6_mem,
           s_on.full_regs, s_off.full_regs, s_on.half_regs, s_off.half_regs,
           s_on.est_waves_per_sp, s_off.est_waves_per_sp,
           dec_off.preserve_xr_oob ? 1u : 0u);

    memset(out_rec, 0, sizeof(*out_rec));
    snprintf(out_rec->name, sizeof(out_rec->name), "%s", name);
    snprintf(out_rec->category, sizeof(out_rec->category), "%s", category);
    snprintf(out_rec->source, sizeof(out_rec->source), "%s", source);
    snprintf(out_rec->stage, sizeof(out_rec->stage), "%s", stage_name(stage));
    out_rec->is_xr10 = is_xr10;
    out_rec->expects_reduction = expects_reduction;
    out_rec->on_stats = s_on;
    out_rec->off_stats = s_off;
    out_rec->diff = diff;

    a840_compile_stage_free(&st_on);
    a840_compile_stage_free(&st_off);
}

static void
run_policy_and_stats_unit_tests(void)
{
    g_cases++;
    A840RobustnessInput req;
    A840RobustnessDecision dec;
    memset(&dec, 0xA5, sizeof(dec));
    A840RobustnessDecision sentinel = dec;

    check_cond(a840_robustness_decide(NULL, &dec) == A840_COMPILE_INVALID_ARG, "NULL req", NULL);
    check_cond(memcmp(&dec, &sentinel, sizeof(dec)) == 0, "NULL req preserves out", NULL);
    memset(&req, 0, sizeof(req));
    check_cond(a840_robustness_decide(&req, NULL) == A840_COMPILE_INVALID_ARG, "NULL out", NULL);
    req.stage = 99u;
    check_cond(a840_robustness_decide(&req, &dec) == A840_COMPILE_INVALID_ARG, "bad stage", NULL);
    check_cond(memcmp(&dec, &sentinel, sizeof(dec)) == 0, "bad stage preserves out", NULL);

    memset(&req, 0, sizeof(req));
    req.stage = A840_STAGE_COMPUTE;
    req.effective_flags = A840_COMPILE_FLAG_ROBUST_OFF;
    req.policy_hints = A840_ROBUST_HINT_ZERO_PAGE_READY;
    req.zero_page_bytes = 4096u;
    check_cond(a840_robustness_decide(&req, &dec) == A840_COMPILE_INVALID_ARG,
               "zero_page_bytes < 64 KiB rejected", NULL);

    memset(&req, 0, sizeof(req));
    req.stage = A840_STAGE_COMPUTE;
    req.effective_flags = A840_COMPILE_FLAG_ROBUST_OFF;
    req.policy_hints = 0u;
    req.zero_page_bytes = 0u;
    check_cond(a840_robustness_decide(&req, &dec) == A840_COMPILE_OK, "no zero page ok", NULL);
    check_cond(!dec.elide_buffer_bounds &&
               dec.nir_robust_modes == (A840_NIR_ROBUST_MODE_UBO | A840_NIR_ROBUST_MODE_SSBO) &&
               (dec.reason_mask & A840_ROBUST_REASON_REJECTED_NO_ZERO_PAGE) != 0u,
               "no zero page fails closed to ROBUST_ON", NULL);

    req.policy_hints = A840_ROBUST_HINT_ZERO_PAGE_READY | A840_ROBUST_HINT_UNTRUSTED_SHADER;
    req.zero_page_bytes = A840_ZERO_PAGE_GUARD_BYTES;
    check_cond(a840_robustness_decide(&req, &dec) == A840_COMPILE_OK, "untrusted ok", NULL);
    check_cond(!dec.elide_buffer_bounds &&
               (dec.reason_mask & A840_ROBUST_REASON_REJECTED_UNTRUSTED) != 0u,
               "untrusted fails closed", NULL);

    req.policy_hints = A840_ROBUST_HINT_ZERO_PAGE_READY | A840_ROBUST_HINT_FORCE_FULL_BOUNDS;
    check_cond(a840_robustness_decide(&req, &dec) == A840_COMPILE_OK, "force bounds ok", NULL);
    check_cond(!dec.elide_buffer_bounds &&
               (dec.reason_mask & A840_ROBUST_REASON_REJECTED_FORCE_BOUNDS) != 0u,
               "force bounds fails closed", NULL);

    memset(&req, 0, sizeof(req));
    req.stage = A840_STAGE_FRAGMENT;
    req.effective_flags = A840_COMPILE_FLAG_ROBUST_OFF | A840_COMPILE_FLAG_XR10_CLAMP;
    req.policy_hints = A840_ROBUST_HINT_ZERO_PAGE_READY;
    req.zero_page_bytes = A840_ZERO_PAGE_GUARD_BYTES;
    req.color_rt_mask = 1u;
    check_cond(a840_robustness_decide(&req, &dec) == A840_COMPILE_OK, "xr10 robust_off ok", NULL);
    check_cond(dec.elide_buffer_bounds && dec.preserve_xr_oob && dec.preserve_image_oob &&
               dec.requires_zero_page_guard &&
               (dec.reason_mask & A840_ROBUST_REASON_XR10_SEMANTICS_KEPT) != 0u,
               "xr10 preserves XR & image OOB while eliding buffer bounds", NULL);

    A840Ir3ShaderStats st;
    A840Ir3StatsDiff df;
    A840CompiledStage bad;
    memset(&bad, 0, sizeof(bad));
    check_cond(a840_ir3_stats_analyze(NULL, 0u, &st) == A840_COMPILE_INVALID_ARG, "stats NULL stage", NULL);
    check_cond(a840_ir3_stats_analyze(&bad, 0u, NULL) == A840_COMPILE_INVALID_ARG, "stats NULL out", NULL);
    check_cond(a840_ir3_stats_analyze(&bad, 0u, &st) == A840_COMPILE_INVALID_ARG, "stats empty ISA", NULL);
    uint32_t odd_words[3] = { 0u, 0u, 0u };
    bad.isa_words = odd_words;
    bad.isa_dwords = 3u;
    check_cond(a840_ir3_stats_analyze(&bad, 0u, &st) == A840_COMPILE_INVALID_ARG, "stats odd dwords", NULL);
    check_cond(a840_ir3_stats_diff(NULL, &st, &df) == A840_COMPILE_INVALID_ARG, "diff NULL before", NULL);
    check_cond(a840_ir3_stats_diff(&st, NULL, &df) == A840_COMPILE_INVALID_ARG, "diff NULL after", NULL);
    check_cond(a840_ir3_stats_diff(&st, &st, NULL) == A840_COMPILE_INVALID_ARG, "diff NULL out", NULL);

    printf("  [POLICY+STATS] Negative/boundary checks passed (%zu checks so far)\n", g_checks);
}

static void
write_json_stats(FILE *f, const A840Ir3ShaderStats *s)
{
    fprintf(f,
            "{\"isa_bytes\":%u,\"isa_dwords\":%u,\"total_instrs\":%u,\"active_instrs\":%u,"
            "\"cat0_ctrl\":%u,\"cat1_mov\":%u,\"cat234_alu\":%u,\"cat2_alu\":%u,"
            "\"cat3_alu\":%u,\"cat4_sfu\":%u,\"cat5_tex\":%u,\"cat6_mem\":%u,"
            "\"cat7_sync\":%u,\"nop_count\":%u,\"full_regs\":%u,\"half_regs\":%u,"
            "\"max_full_reg\":%d,\"max_half_reg\":%d,\"const_dwords\":%u,"
            "\"branchstack\":%u,\"reg_spills\":%u,\"reg_fills\":%u,"
            "\"compiler_max_waves\":%u,\"est_waves_per_sp\":%u,"
            "\"has_kill\":%s,\"uses_fbfetch\":%s,\"merged_regs\":%s,"
            "\"early_preamble\":%s,\"robust_policy_flags\":%u}",
            s->isa_bytes, s->isa_dwords, s->total_instrs, s->active_instrs,
            s->cat0_ctrl, s->cat1_mov, s->cat234_alu, s->cat2_alu,
            s->cat3_alu, s->cat4_sfu, s->cat5_tex, s->cat6_mem,
            s->cat7_sync, s->nop_count, s->full_regs, s->half_regs,
            s->max_full_reg, s->max_half_reg, s->const_dwords,
            s->branchstack, s->reg_spills, s->reg_fills,
            s->compiler_max_waves, s->est_waves_per_sp,
            s->has_kill ? "true" : "false",
            s->uses_fbfetch ? "true" : "false",
            s->merged_regs ? "true" : "false",
            s->early_preamble ? "true" : "false",
            s->robust_policy_flags);
}

static void
dump_ab_json(const char *path, const AbRecord *recs, size_t count)
{
    FILE *f = fopen(path, "wb");
    check_cond(f != NULL, "fopen dump-json", path);
    fprintf(f, "{\n");
    fprintf(f, "  \"schema_version\": 1,\n");
    fprintf(f, "  \"mesa_commit\": \"85fda3b36405bcaea8bffc64aa4f2651dc90770d\",\n");
    fprintf(f, "  \"target_chip_id\": \"0x44050a31\",\n");
    fprintf(f, "  \"zero_page_guard_bytes\": %u,\n", (unsigned)A840_ZERO_PAGE_GUARD_BYTES);
    fprintf(f, "  \"device_verified\": false,\n");
    fprintf(f, "  \"hardware_acceptance\": false,\n");
    fprintf(f, "  \"shader_count\": %zu,\n", count);
    fprintf(f, "  \"shaders\": [\n");
    for (size_t i = 0u; i < count; ++i) {
        const AbRecord *r = &recs[i];
        fprintf(f, "    {\n");
        fprintf(f, "      \"name\": \"%s\",\n", r->name);
        fprintf(f, "      \"category\": \"%s\",\n", r->category);
        fprintf(f, "      \"source\": \"%s\",\n", r->source);
        fprintf(f, "      \"stage\": \"%s\",\n", r->stage);
        fprintf(f, "      \"is_xr10\": %s,\n", r->is_xr10 ? "true" : "false");
        fprintf(f, "      \"expects_reduction\": %s,\n", r->expects_reduction ? "true" : "false");
        fprintf(f, "      \"robust_on\": ");
        write_json_stats(f, &r->on_stats);
        fprintf(f, ",\n      \"robust_off\": ");
        write_json_stats(f, &r->off_stats);
        fprintf(f, ",\n      \"diff\": {\"delta_total_instrs\":%d,\"saved_total_instrs\":%d,"
                   "\"reduction_pct\":%.4f,\"delta_active_instrs\":%d,"
                   "\"delta_cat0_ctrl\":%d,\"delta_cat1_mov\":%d,\"delta_cat234_alu\":%d,"
                   "\"delta_cat5_tex\":%d,\"delta_cat6_mem\":%d,\"delta_cat7_sync\":%d,"
                   "\"delta_nop_count\":%d,\"delta_full_regs\":%d,\"delta_half_regs\":%d,"
                   "\"delta_const_dwords\":%d,\"delta_est_waves_per_sp\":%d,"
                   "\"delta_compiler_max_waves\":%d,\"reg_regressed\":%s,"
                   "\"instr_regressed\":%s,\"wave_regressed\":%s,\"xr_preserved\":%s}\n",
                r->diff.delta_total_instrs, r->diff.saved_total_instrs,
                r->diff.reduction_pct, r->diff.delta_active_instrs,
                r->diff.delta_cat0_ctrl, r->diff.delta_cat1_mov, r->diff.delta_cat234_alu,
                r->diff.delta_cat5_tex, r->diff.delta_cat6_mem, r->diff.delta_cat7_sync,
                r->diff.delta_nop_count, r->diff.delta_full_regs, r->diff.delta_half_regs,
                r->diff.delta_const_dwords, r->diff.delta_est_waves_per_sp,
                r->diff.delta_compiler_max_waves,
                r->diff.reg_regressed ? "true" : "false",
                r->diff.instr_regressed ? "true" : "false",
                r->diff.wave_regressed ? "true" : "false",
                r->diff.xr_preserved ? "true" : "false");
        fprintf(f, "    }%s\n", (i + 1u < count) ? "," : "");
    }
    fprintf(f, "  ]\n");
    fprintf(f, "}\n");
    fclose(f);
}

int
main(int argc, char **argv)
{
    const char *dump_json_path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dump-json") == 0 && i + 1 < argc) {
            dump_json_path = argv[++i];
        } else {
            fprintf(stderr, "usage: %s [--dump-json <path>]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    printf("=== Wave 9 / Lane 2: Real Mesa ir3 Robustness A/B + ISA Profiler ===\n");
    run_policy_and_stats_unit_tests();

    AbRecord records[32];
    size_t nrec = 0u;

    /* Part A: Real 24A446 metallib shaders translated via libnative_metal_translate.a */
    NmbDevice *dev = nmb_device_create_a840(0u);
    check_cond(dev != NULL, "nmb_device_create_a840", nmb_last_error());

    static const char MATTING_LIB[] =
        "/home/hua/nezha-work/private/ios27/camera27-c0/registration090-research/MattingV2.metallib";
    static const char VT_LIB[] =
        "/home/hua/nezha-work/private/ios27/camera27-c0/registration093-research/VideoToolbox-default.metallib";
    static const char KAL_LIB[] =
        "/home/hua/nezha-work/ios27/gpu27-astra/private/metal-abi/NTKKaleidoscopeShaders.original.metallib";

    /* A.1: MattingV2.metallib (camera compute shaders) */
    {
        size_t len = 0u;
        uint8_t *bytes = read_file_all(MATTING_LIB, &len);
        NmbLibrary *lib = nmb_library_parse(dev, bytes, len);
        free(bytes);
        check_cond(lib != NULL, "parse MattingV2", nmb_last_error());

        const char *fns[] = {
            "copyTexture",
            "addTextures",
            "bilinearScale",
            "composeRGBAGuide",
            "excludeNonSkin"
        };
        for (size_t i = 0u; i < sizeof(fns) / sizeof(fns[0]); ++i) {
            NmbSpirvResult *spv = nmb_compile_compute_spirv(dev, lib, fns[i]);
            check_cond(spv != NULL, "translate MattingV2 fn", fns[i]);
            size_t spv_bytes = 0u;
            const uint32_t *words = nmb_spirv_words(spv, 0u, &spv_bytes);
            const char *ep = nmb_spirv_entry_point(spv, 0u);
            char label[128];
            snprintf(label, sizeof(label), "24A446_matting_%s", fns[i]);
            run_ab_case(label, "camera", "MattingV2.metallib", A840_STAGE_COMPUTE,
                        words, spv_bytes, ep, A840_COMPILE_FLAG_EARLY_PREAMBLE,
                        0u, 0u, false, false, &records[nrec++]);
            nmb_spirv_result_free(spv);
        }
        nmb_library_destroy(lib);
    }

    /* A.2: VideoToolbox-default.metallib (compositor compute + render shaders) */
    {
        size_t len = 0u;
        uint8_t *bytes = read_file_all(VT_LIB, &len);
        NmbLibrary *lib = nmb_library_parse(dev, bytes, len);
        free(bytes);
        check_cond(lib != NULL, "parse VideoToolbox-default", nmb_last_error());

        const char *cs_fns[] = {
            "VTMTSComputeComposite",
            "VTMTSComputeFunction"
        };
        for (size_t i = 0u; i < sizeof(cs_fns) / sizeof(cs_fns[0]); ++i) {
            NmbSpirvResult *spv = nmb_compile_compute_spirv(dev, lib, cs_fns[i]);
            check_cond(spv != NULL, "translate VideoToolbox cs", cs_fns[i]);
            size_t spv_bytes = 0u;
            const uint32_t *words = nmb_spirv_words(spv, 0u, &spv_bytes);
            const char *ep = nmb_spirv_entry_point(spv, 0u);
            char label[128];
            snprintf(label, sizeof(label), "24A446_vt_%s", cs_fns[i]);
            run_ab_case(label, "compositor", "VideoToolbox-default.metallib", A840_STAGE_COMPUTE,
                        words, spv_bytes, ep, A840_COMPILE_FLAG_EARLY_PREAMBLE,
                        0u, 0u, false, false, &records[nrec++]);
            nmb_spirv_result_free(spv);
        }

        NmbSpirvResult *rend = nmb_compile_render_spirv(
            dev, lib, "VTMTSVertexFunction", "VTMTSFragmentFunction", 70u);
        check_cond(rend != NULL, "translate VideoToolbox render", nmb_last_error());
        {
            size_t vs_b = 0u, fs_b = 0u;
            const uint32_t *vs_w = nmb_spirv_words(rend, 0u, &vs_b);
            const uint32_t *fs_w = nmb_spirv_words(rend, 1u, &fs_b);
            run_ab_case("24A446_vt_VTMTSVertexFunction", "compositor",
                        "VideoToolbox-default.metallib", A840_STAGE_VERTEX,
                        vs_w, vs_b, nmb_spirv_entry_point(rend, 0u),
                        A840_COMPILE_FLAG_EARLY_PREAMBLE, 0u, 0u, false, false, &records[nrec++]);
            run_ab_case("24A446_vt_VTMTSFragmentFunction", "compositor",
                        "VideoToolbox-default.metallib", A840_STAGE_FRAGMENT,
                        fs_w, fs_b, nmb_spirv_entry_point(rend, 1u),
                        A840_COMPILE_FLAG_EARLY_PREAMBLE, 1u, 0u, false, false, &records[nrec++]);
        }
        nmb_spirv_result_free(rend);
        nmb_library_destroy(lib);
    }

    /* A.3: NTKKaleidoscopeShaders.original.metallib (liquid-glass / render shaders) */
    {
        size_t len = 0u;
        uint8_t *bytes = read_file_all(KAL_LIB, &len);
        NmbLibrary *lib = nmb_library_parse(dev, bytes, len);
        free(bytes);
        check_cond(lib != NULL, "parse NTKKaleidoscopeShaders", nmb_last_error());

        struct { const char *vs; const char *fs; const char *fs_label; } pairs[] = {
            { "kal_fullscreen_vert", "kal_rosette_frag",  "24A446_kal_rosette_frag" },
            { "kal_fullscreen_vert", "kal_radial_frag",   "24A446_kal_radial_frag" },
            { "kal_circle_vert",     "kal_faceted_frag",  "24A446_kal_faceted_frag" },
        };
        for (size_t i = 0u; i < sizeof(pairs) / sizeof(pairs[0]); ++i) {
            NmbSpirvResult *spv = nmb_compile_render_spirv(
                dev, lib, pairs[i].vs, pairs[i].fs, 70u);
            check_cond(spv != NULL, "translate NTKKaleidoscope", pairs[i].fs);
            if (i == 0u) {
                size_t vs_b = 0u;
                const uint32_t *vs_w = nmb_spirv_words(spv, 0u, &vs_b);
                run_ab_case("24A446_kal_fullscreen_vert", "liquid-glass",
                            "NTKKaleidoscopeShaders.original.metallib", A840_STAGE_VERTEX,
                            vs_w, vs_b, nmb_spirv_entry_point(spv, 0u),
                            A840_COMPILE_FLAG_EARLY_PREAMBLE, 0u, 0u, false, false, &records[nrec++]);
            }
            size_t fs_b = 0u;
            const uint32_t *fs_w = nmb_spirv_words(spv, 1u, &fs_b);
            run_ab_case(pairs[i].fs_label, "liquid-glass",
                        "NTKKaleidoscopeShaders.original.metallib", A840_STAGE_FRAGMENT,
                        fs_w, fs_b, nmb_spirv_entry_point(spv, 1u),
                        A840_COMPILE_FLAG_EARLY_PREAMBLE, 1u, 0u, false, false, &records[nrec++]);
            nmb_spirv_result_free(spv);
        }
        nmb_library_destroy(lib);
    }

    nmb_device_destroy(dev);

    /* Part B: Dynamic UBO/SSBO bounds-checked shaders + XR10 wide-color FBFetch */
    {
        SpvBuf cs1 = build_compute_ssbo_bounds_spv(1u);
        run_ab_case("dyn_ssbo_bounds_1load_cs", "compute", "spirv_dyn_ssbo",
                    A840_STAGE_COMPUTE, cs1.words, cs1.len * 4u, "main",
                    A840_COMPILE_FLAG_EARLY_PREAMBLE, 0u, 0u, false, true, &records[nrec++]);
        spv_free(&cs1);

        SpvBuf cs4 = build_compute_ssbo_bounds_spv(4u);
        run_ab_case("dyn_ssbo_bounds_4load_cs", "compute", "spirv_dyn_ssbo",
                    A840_STAGE_COMPUTE, cs4.words, cs4.len * 4u, "main",
                    A840_COMPILE_FLAG_EARLY_PREAMBLE, 0u, 0u, false, true, &records[nrec++]);
        spv_free(&cs4);

        SpvBuf vs_pull = build_vertex_ssbo_pull_spv();
        run_ab_case("dyn_ssbo_vertex_pull_vs", "compositor", "spirv_dyn_ssbo",
                    A840_STAGE_VERTEX, vs_pull.words, vs_pull.len * 4u, "main",
                    A840_COMPILE_FLAG_EARLY_PREAMBLE, 0u, 0u, false, true, &records[nrec++]);
        spv_free(&vs_pull);

        SpvBuf fs_vec = build_fragment_ssbo_vec_spv();
        run_ab_case("dyn_ssbo_scalar_vec4_fs", "liquid-glass", "spirv_dyn_vec",
                    A840_STAGE_FRAGMENT, fs_vec.words, fs_vec.len * 4u, "main",
                    A840_COMPILE_FLAG_EARLY_PREAMBLE, 1u, 0u, false, false, &records[nrec++]);
        spv_free(&fs_vec);

        SpvBuf fs_xr10 = build_fragment_xr10_fbfetch_spv();
        run_ab_case("xr10_fbfetch_ssbo_lut_fs", "xr10", "spirv_xr10_fbfetch",
                    A840_STAGE_FRAGMENT, fs_xr10.words, fs_xr10.len * 4u, "main",
                    A840_COMPILE_FLAG_EARLY_PREAMBLE | A840_COMPILE_FLAG_FBFETCH_GMEM,
                    1u, 1u, true, true, &records[nrec++]);
        spv_free(&fs_xr10);
    }

    check_cond(nrec >= 12u, "minimum 12 real SPIR-V A/B shaders", NULL);

    if (dump_json_path != NULL) {
        dump_ab_json(dump_json_path, records, nrec);
        printf("  [JSON] Wrote %zu real A/B records -> %s\n", nrec, dump_json_path);
    }

    printf("PASS: test_robustness_and_profile (%zu shaders, %zu cases, %zu checks)\n",
           nrec, g_cases, g_checks);
    return EXIT_SUCCESS;
}
