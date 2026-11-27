/* SPDX-License-Identifier: MIT */
/* Wave 10 / Lane 2: Real Mesa ir3 Metal Hotspot Specialization A/B Test.
 *
 * Links against real libadreno_compile_ir3.a (pinned Mesa 85fda3b36405, zero
 * src/freedreno/vulkan) and libnative_metal_translate.a.
 *
 * Compiles 20 real SPIR-V shaders (13 real 24A446 iOS/iPadOS metallib shaders
 * + 7 targeted hotspot SPIR-V shaders covering Apple FP16 liquid-glass blur &
 * RTZ compositing, dense Push-Constant & UBO duplicate/offset folding,
 * high-register-pressure branching & 16-tap convolution wave-occupancy boost,
 * and Apple XR10 [-0.752941, 1.25098] + GMEM FBFetch protection) under both
 * OPT_OFF (W09 baseline) and OPT_ON (all 3 W10-L2 hotspot passes enabled).
 *
 * Strictly enforces:
 *   - 0 instruction / full-reg / effective-reg / spill / wave regressions across all 20 shaders
 *   - Real measurable ISA / cat1_mov / full_regs / wave gains on targeted hotspots
 *   - 100% Apple XR10 wide-color & GMEM FBFetch preservation
 *   - Push-constant layout & const_push_mask invariance
 *   - Optional --dump-json <path> export for hotspot_opt_quantify.py
 */
#define _POSIX_C_SOURCE 200809L

#include "AdrenoCompileABI.h"
#include "NativeMetalTranslate.h"
#include "a840_ir3_stats.h"
#include "a840_metal_opt_pass.h"
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
    b->cap = 1024u;
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

enum {
    OP_MEMORY_MODEL          = 14u,
    OP_EXECUTION_MODE        = 16u,
    OP_CAPABILITY            = 17u,
    OP_TYPE_VOID             = 19u,
    OP_TYPE_BOOL             = 20u,
    OP_TYPE_INT              = 21u,
    OP_TYPE_FLOAT            = 22u,
    OP_TYPE_VECTOR           = 23u,
    OP_TYPE_IMAGE            = 25u,
    OP_TYPE_ARRAY            = 28u,
    OP_TYPE_RUNTIME_ARRAY    = 29u,
    OP_TYPE_STRUCT           = 30u,
    OP_TYPE_POINTER          = 32u,
    OP_TYPE_FUNCTION         = 33u,
    OP_CONSTANT              = 43u,
    OP_CONSTANT_COMPOSITE    = 44u,
    OP_FUNCTION              = 54u,
    OP_FUNCTION_END          = 56u,
    OP_VARIABLE              = 59u,
    OP_LOAD                  = 61u,
    OP_STORE                 = 62u,
    OP_ACCESS_CHAIN          = 65u,
    OP_DECORATE              = 71u,
    OP_MEMBER_DECORATE       = 72u,
    OP_COMPOSITE_CONSTRUCT   = 80u,
    OP_COMPOSITE_EXTRACT     = 81u,
    OP_IMAGE_READ            = 98u,
    OP_FCONVERT              = 115u,
    OP_IADD                  = 128u,
    OP_FADD                  = 129u,
    OP_FSUB                  = 131u,
    OP_FMUL                  = 133u,
    OP_SELECT                = 169u,
    OP_FORD_LESS_THAN        = 184u,
    OP_FORD_GREATER_THAN     = 186u,
    OP_PHI                   = 245u,
    OP_SELECTION_MERGE       = 247u,
    OP_LABEL                 = 248u,
    OP_BRANCH                = 249u,
    OP_BRANCH_CONDITIONAL    = 250u,
    OP_RETURN                = 253u,

    CAP_SHADER               = 1u,
    CAP_FLOAT16              = 9u,
    CAP_INPUT_ATTACHMENT     = 40u,
    CAP_STORAGE_IO16         = 4436u,

    EXEC_FRAGMENT            = 4u,
    EXEC_GLCOMPUTE           = 5u,
    MODE_ORIGIN_UPPER_LEFT   = 7u,
    MODE_LOCAL_SIZE          = 17u,

    SC_UNIFORM_CONSTANT      = 0u,
    SC_INPUT                 = 1u,
    SC_UNIFORM               = 2u,
    SC_OUTPUT                = 3u,
    SC_PUSH_CONSTANT         = 9u,
    SC_STORAGE_BUFFER        = 12u,

    DEC_BLOCK                = 2u,
    DEC_ARRAY_STRIDE         = 6u,
    DEC_BUILTIN              = 11u,
    DEC_LOCATION             = 30u,
    DEC_BINDING              = 33u,
    DEC_DESC_SET             = 34u,
    DEC_OFFSET               = 35u,
    DEC_INPUT_ATT_IDX        = 43u,
    BUILTIN_GLOBAL_INVOC_ID  = 28u
};

static uint32_t
f32_bits(float v)
{
    uint32_t u = 0u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

/* =========================================================================
 * Targeted Hotspot SPIR-V Shaders (7 real SPIR-V shaders)
 * ========================================================================= */

/* 1. Apple Metal half4 liquid-glass 4-tap blur/tint shader lowered with
 *    f2f32 -> alu32 -> f2f16 round-trips. Pass 1 narrows the pure-half cone
 *    and elides redundant cov.f16f32 / cov.f32f16 conversions.
 */
static SpvBuf
build_fp16_glass_blur_4tap_spv(void)
{
    SpvBuf b;
    spv_init(&b, 140u);

    SPV_OP(&b, OP_CAPABILITY, CAP_SHADER);
    SPV_OP(&b, OP_CAPABILITY, CAP_FLOAT16);
    SPV_OP(&b, OP_CAPABILITY, CAP_STORAGE_IO16);
    SPV_OP(&b, OP_MEMORY_MODEL, 0u, 1u);
    const uint32_t ifaces[] = { 10u, 11u, 12u, 13u, 14u };
    spv_entry(&b, EXEC_FRAGMENT, 3u, "main", ifaces, 5u);
    SPV_OP(&b, OP_EXECUTION_MODE, 3u, MODE_ORIGIN_UPPER_LEFT);

    SPV_OP(&b, OP_DECORATE, 10u, DEC_LOCATION, 0u);
    SPV_OP(&b, OP_DECORATE, 11u, DEC_LOCATION, 1u);
    SPV_OP(&b, OP_DECORATE, 12u, DEC_LOCATION, 2u);
    SPV_OP(&b, OP_DECORATE, 13u, DEC_LOCATION, 3u);
    SPV_OP(&b, OP_DECORATE, 14u, DEC_LOCATION, 0u);

    SPV_OP(&b, OP_TYPE_VOID, 1u);
    SPV_OP(&b, OP_TYPE_FUNCTION, 2u, 1u);
    SPV_OP(&b, OP_TYPE_FLOAT, 4u, 16u);      /* half */
    SPV_OP(&b, OP_TYPE_VECTOR, 5u, 4u, 4u);  /* half4 */
    SPV_OP(&b, OP_TYPE_FLOAT, 6u, 32u);      /* float */
    SPV_OP(&b, OP_TYPE_VECTOR, 7u, 6u, 4u);  /* float4 */
    SPV_OP(&b, OP_TYPE_POINTER, 8u, SC_INPUT, 5u);
    SPV_OP(&b, OP_TYPE_POINTER, 9u, SC_OUTPUT, 5u);

    SPV_OP(&b, OP_VARIABLE, 8u, 10u, SC_INPUT);  /* tap0 half4 */
    SPV_OP(&b, OP_VARIABLE, 8u, 11u, SC_INPUT);  /* tap1 half4 */
    SPV_OP(&b, OP_VARIABLE, 8u, 12u, SC_INPUT);  /* tap2 half4 */
    SPV_OP(&b, OP_VARIABLE, 8u, 13u, SC_INPUT);  /* tap3 half4 */
    SPV_OP(&b, OP_VARIABLE, 9u, 14u, SC_OUTPUT); /* out half4 */

    SPV_OP(&b, OP_CONSTANT, 6u, 20u, f32_bits(0.25f));
    SPV_OP(&b, OP_CONSTANT_COMPOSITE, 7u, 21u, 20u, 20u, 20u, 20u);
    SPV_OP(&b, OP_CONSTANT, 6u, 22u, f32_bits(0.5f));
    SPV_OP(&b, OP_CONSTANT_COMPOSITE, 7u, 23u, 22u, 22u, 22u, 22u);
    SPV_OP(&b, OP_CONSTANT, 6u, 24u, f32_bits(0.125f));
    SPV_OP(&b, OP_CONSTANT_COMPOSITE, 7u, 25u, 24u, 24u, 24u, 24u);

    SPV_OP(&b, OP_FUNCTION, 1u, 3u, 0u, 2u);
    SPV_OP(&b, OP_LABEL, 30u);

    /* Load 4 half4 taps */
    SPV_OP(&b, OP_LOAD, 5u, 31u, 10u);
    SPV_OP(&b, OP_LOAD, 5u, 32u, 11u);
    SPV_OP(&b, OP_LOAD, 5u, 33u, 12u);
    SPV_OP(&b, OP_LOAD, 5u, 34u, 13u);

    /* Widen each tap to f32 (typical metal2vulkan lowering artifact) */
    SPV_OP(&b, OP_FCONVERT, 7u, 35u, 31u);
    SPV_OP(&b, OP_FCONVERT, 7u, 36u, 32u);
    SPV_OP(&b, OP_FCONVERT, 7u, 37u, 33u);
    SPV_OP(&b, OP_FCONVERT, 7u, 38u, 34u);

    /* Weighted blur + tint in f32 */
    SPV_OP(&b, OP_FMUL, 7u, 40u, 35u, 21u);
    SPV_OP(&b, OP_FMUL, 7u, 41u, 36u, 21u);
    SPV_OP(&b, OP_FADD, 7u, 42u, 40u, 41u);
    SPV_OP(&b, OP_FMUL, 7u, 43u, 37u, 23u);
    SPV_OP(&b, OP_FADD, 7u, 44u, 42u, 43u);
    SPV_OP(&b, OP_FMUL, 7u, 45u, 38u, 25u);
    SPV_OP(&b, OP_FSUB, 7u, 46u, 44u, 45u);

    /* Convert back to half4 + redundant round-trip f16 -> f32 -> f16 */
    SPV_OP(&b, OP_FCONVERT, 5u, 47u, 46u);
    SPV_OP(&b, OP_FCONVERT, 7u, 48u, 47u);
    SPV_OP(&b, OP_FCONVERT, 5u, 49u, 48u);

    SPV_OP(&b, OP_STORE, 14u, 49u);
    SPV_OP0(&b, OP_RETURN);
    SPV_OP0(&b, OP_FUNCTION_END);
    return b;
}

/* 2. Apple Metal half4 premultiplied alpha composite with RTZ rounding mode. */
static SpvBuf
build_fp16_rtz_blend_spv(void)
{
    SpvBuf b;
    spv_init(&b, 120u);

    SPV_OP(&b, OP_CAPABILITY, CAP_SHADER);
    SPV_OP(&b, OP_CAPABILITY, CAP_FLOAT16);
    SPV_OP(&b, OP_CAPABILITY, CAP_STORAGE_IO16);
    SPV_OP(&b, OP_MEMORY_MODEL, 0u, 1u);
    const uint32_t ifaces[] = { 10u, 11u, 12u, 13u };
    spv_entry(&b, EXEC_FRAGMENT, 3u, "main", ifaces, 4u);
    SPV_OP(&b, OP_EXECUTION_MODE, 3u, MODE_ORIGIN_UPPER_LEFT);

    SPV_OP(&b, OP_DECORATE, 10u, DEC_LOCATION, 0u);
    SPV_OP(&b, OP_DECORATE, 11u, DEC_LOCATION, 1u);
    SPV_OP(&b, OP_DECORATE, 12u, DEC_LOCATION, 2u);
    SPV_OP(&b, OP_DECORATE, 13u, DEC_LOCATION, 0u);

    SPV_OP(&b, OP_TYPE_VOID, 1u);
    SPV_OP(&b, OP_TYPE_FUNCTION, 2u, 1u);
    SPV_OP(&b, OP_TYPE_FLOAT, 4u, 16u);
    SPV_OP(&b, OP_TYPE_VECTOR, 5u, 4u, 4u);
    SPV_OP(&b, OP_TYPE_FLOAT, 6u, 32u);
    SPV_OP(&b, OP_TYPE_VECTOR, 7u, 6u, 4u);
    SPV_OP(&b, OP_TYPE_POINTER, 8u, SC_INPUT, 5u);
    SPV_OP(&b, OP_TYPE_POINTER, 9u, SC_OUTPUT, 5u);

    SPV_OP(&b, OP_VARIABLE, 8u, 10u, SC_INPUT);  /* src half4 */
    SPV_OP(&b, OP_VARIABLE, 8u, 11u, SC_INPUT);  /* dst half4 */
    SPV_OP(&b, OP_VARIABLE, 8u, 12u, SC_INPUT);  /* mask/alpha half4 */
    SPV_OP(&b, OP_VARIABLE, 9u, 13u, SC_OUTPUT); /* out half4 */

    SPV_OP(&b, OP_CONSTANT, 6u, 20u, f32_bits(1.0f));
    SPV_OP(&b, OP_CONSTANT_COMPOSITE, 7u, 21u, 20u, 20u, 20u, 20u);

    SPV_OP(&b, OP_FUNCTION, 1u, 3u, 0u, 2u);
    SPV_OP(&b, OP_LABEL, 30u);

    SPV_OP(&b, OP_LOAD, 5u, 31u, 10u);
    SPV_OP(&b, OP_LOAD, 5u, 32u, 11u);
    SPV_OP(&b, OP_LOAD, 5u, 33u, 12u);

    /* Widen to f32, blend src + dst * (1 - alpha), narrow back with round-trip */
    SPV_OP(&b, OP_FCONVERT, 7u, 34u, 31u);
    SPV_OP(&b, OP_FCONVERT, 7u, 35u, 32u);
    SPV_OP(&b, OP_FCONVERT, 7u, 36u, 33u);

    SPV_OP(&b, OP_FSUB, 7u, 37u, 21u, 36u);
    SPV_OP(&b, OP_FMUL, 7u, 38u, 35u, 37u);
    SPV_OP(&b, OP_FADD, 7u, 39u, 34u, 38u);

    SPV_OP(&b, OP_FCONVERT, 5u, 40u, 39u);
    SPV_OP(&b, OP_FCONVERT, 7u, 41u, 40u);
    SPV_OP(&b, OP_FCONVERT, 5u, 42u, 41u);

    SPV_OP(&b, OP_STORE, 13u, 42u);
    SPV_OP0(&b, OP_RETURN);
    SPV_OP0(&b, OP_FUNCTION_END);
    return b;
}

/* 3. Compositor fragment shader with dynamically indexed Push-Constant array
 *    reads (duplicate overlapping vec4 + scalar .x/.y/.z/.w reads on the same
 *    dynamic layer index + nested offset arithmetic) and const_push_mask
 *    compatibility.
 */
static SpvBuf
build_push_const_dup_fs_spv(void)
{
    SpvBuf b;
    spv_init(&b, 180u);

    SPV_OP(&b, OP_CAPABILITY, CAP_SHADER);
    SPV_OP(&b, OP_MEMORY_MODEL, 0u, 1u);
    const uint32_t ifaces[] = { 18u, 19u, 25u };
    spv_entry(&b, EXEC_FRAGMENT, 3u, "main", ifaces, 3u);
    SPV_OP(&b, OP_EXECUTION_MODE, 3u, MODE_ORIGIN_UPPER_LEFT);

    /* Push constant struct: { vec4 tint; vec4 layers[8]; } */
    SPV_OP(&b, OP_DECORATE, 9u, DEC_ARRAY_STRIDE, 16u);
    SPV_OP(&b, OP_DECORATE, 10u, DEC_BLOCK);
    SPV_OP(&b, OP_MEMBER_DECORATE, 10u, 0u, DEC_OFFSET, 0u);
    SPV_OP(&b, OP_MEMBER_DECORATE, 10u, 1u, DEC_OFFSET, 16u);

    SPV_OP(&b, OP_DECORATE, 18u, DEC_LOCATION, 0u); /* vec4 color */
    SPV_OP(&b, OP_DECORATE, 25u, DEC_LOCATION, 1u); /* uint layer_idx (flat) */
    SPV_OP(&b, OP_DECORATE, 25u, 14u /* Flat */);
    SPV_OP(&b, OP_DECORATE, 19u, DEC_LOCATION, 0u); /* vec4 out */

    SPV_OP(&b, OP_TYPE_VOID, 1u);
    SPV_OP(&b, OP_TYPE_FUNCTION, 2u, 1u);
    SPV_OP(&b, OP_TYPE_FLOAT, 4u, 32u);
    SPV_OP(&b, OP_TYPE_VECTOR, 5u, 4u, 4u);
    SPV_OP(&b, OP_TYPE_INT, 6u, 32u, 0u);
    SPV_OP(&b, OP_CONSTANT, 6u, 8u, 8u);
    SPV_OP(&b, OP_TYPE_ARRAY, 9u, 5u, 8u);

    SPV_OP(&b, OP_TYPE_STRUCT, 10u, 5u, 9u);
    SPV_OP(&b, OP_TYPE_POINTER, 11u, SC_PUSH_CONSTANT, 10u);
    SPV_OP(&b, OP_VARIABLE, 11u, 12u, SC_PUSH_CONSTANT);

    SPV_OP(&b, OP_TYPE_POINTER, 16u, SC_INPUT, 5u);
    SPV_OP(&b, OP_TYPE_POINTER, 17u, SC_OUTPUT, 5u);
    SPV_OP(&b, OP_TYPE_POINTER, 24u, SC_INPUT, 6u);
    SPV_OP(&b, OP_VARIABLE, 16u, 18u, SC_INPUT);
    SPV_OP(&b, OP_VARIABLE, 17u, 19u, SC_OUTPUT);
    SPV_OP(&b, OP_VARIABLE, 24u, 25u, SC_INPUT);

    SPV_OP(&b, OP_TYPE_POINTER, 20u, SC_PUSH_CONSTANT, 5u);
    SPV_OP(&b, OP_TYPE_POINTER, 21u, SC_PUSH_CONSTANT, 4u);

    SPV_OP(&b, OP_CONSTANT, 6u, 26u, 0u);
    SPV_OP(&b, OP_CONSTANT, 6u, 27u, 1u);
    SPV_OP(&b, OP_CONSTANT, 6u, 28u, 2u);
    SPV_OP(&b, OP_CONSTANT, 6u, 29u, 3u);

    SPV_OP(&b, OP_FUNCTION, 1u, 3u, 0u, 2u);
    SPV_OP(&b, OP_LABEL, 30u);

    SPV_OP(&b, OP_LOAD, 5u, 31u, 18u); /* color */
    SPV_OP(&b, OP_LOAD, 6u, 32u, 25u); /* layer_idx */

    /* Constant push vec4 at offset 0 (also tests const_push_mask folding!) */
    SPV_OP(&b, OP_ACCESS_CHAIN, 20u, 33u, 12u, 26u);
    SPV_OP(&b, OP_LOAD, 5u, 34u, 33u);

    /* Dynamic push-constant vec4 at layers[idx] */
    SPV_OP(&b, OP_ACCESS_CHAIN, 20u, 35u, 12u, 27u, 32u);
    SPV_OP(&b, OP_LOAD, 5u, 36u, 35u);

    /* Redundant scalar push-constant re-reads of layers[idx].x/.y/.z/.w */
    SPV_OP(&b, OP_ACCESS_CHAIN, 21u, 37u, 12u, 27u, 32u, 26u);
    SPV_OP(&b, OP_LOAD, 4u, 38u, 37u);
    SPV_OP(&b, OP_ACCESS_CHAIN, 21u, 39u, 12u, 27u, 32u, 27u);
    SPV_OP(&b, OP_LOAD, 4u, 40u, 39u);
    SPV_OP(&b, OP_ACCESS_CHAIN, 21u, 41u, 12u, 27u, 32u, 28u);
    SPV_OP(&b, OP_LOAD, 4u, 42u, 41u);
    SPV_OP(&b, OP_ACCESS_CHAIN, 21u, 43u, 12u, 27u, 32u, 29u);
    SPV_OP(&b, OP_LOAD, 4u, 44u, 43u);
    SPV_OP(&b, OP_COMPOSITE_CONSTRUCT, 5u, 45u, 38u, 40u, 42u, 44u);

    /* Second redundant dynamic vec4 read at layers[(idx + 1) + 0] + scalars */
    SPV_OP(&b, OP_IADD, 6u, 46u, 32u, 27u);
    SPV_OP(&b, OP_ACCESS_CHAIN, 20u, 47u, 12u, 27u, 46u);
    SPV_OP(&b, OP_LOAD, 5u, 48u, 47u);
    SPV_OP(&b, OP_ACCESS_CHAIN, 21u, 49u, 12u, 27u, 46u, 27u);
    SPV_OP(&b, OP_LOAD, 4u, 50u, 49u);
    SPV_OP(&b, OP_ACCESS_CHAIN, 21u, 51u, 12u, 27u, 46u, 28u);
    SPV_OP(&b, OP_LOAD, 4u, 52u, 51u);
    SPV_OP(&b, OP_COMPOSITE_CONSTRUCT, 5u, 53u, 50u, 52u, 50u, 52u);

    /* Combine */
    SPV_OP(&b, OP_FMUL, 5u, 60u, 31u, 34u);
    SPV_OP(&b, OP_FMUL, 5u, 61u, 36u, 45u);
    SPV_OP(&b, OP_FADD, 5u, 62u, 60u, 61u);
    SPV_OP(&b, OP_FMUL, 5u, 63u, 48u, 53u);
    SPV_OP(&b, OP_FADD, 5u, 64u, 62u, 63u);

    SPV_OP(&b, OP_STORE, 19u, 64u);
    SPV_OP0(&b, OP_RETURN);
    SPV_OP0(&b, OP_FUNCTION_END);
    return b;
}

/* 4. Compute shader with Metal Push-Constant multi-row parameter table + UBO
 *    lookup (dynamically indexed `rows[idx]`, `rows[idx+1]`, `rows[idx+2]`,
 *    `rows[idx+3]` + UBO table lookup -> SSBO store).
 *    Pass 2 + Pass 2B hoist constant byte offsets into load_const_ir3.base,
 *    eliminate redundant (x << 4) >> 2 shifts, and share a single a0.x register.
 */
static SpvBuf
build_ubo_dup_offset_cs_spv(void)
{
    SpvBuf b;
    spv_init(&b, 200u);

    SPV_OP(&b, OP_CAPABILITY, CAP_SHADER);
    SPV_OP(&b, OP_MEMORY_MODEL, 0u, 1u);
    const uint32_t ifaces[] = { 16u };
    spv_entry(&b, EXEC_GLCOMPUTE, 3u, "main", ifaces, 1u);
    SPV_OP(&b, OP_EXECUTION_MODE, 3u, MODE_LOCAL_SIZE, 64u, 1u, 1u);

    /* Push constant: { vec4 rows[12]; } */
    SPV_OP(&b, OP_DECORATE, 90u, DEC_ARRAY_STRIDE, 16u);
    SPV_OP(&b, OP_DECORATE, 91u, DEC_BLOCK);
    SPV_OP(&b, OP_MEMBER_DECORATE, 91u, 0u, DEC_OFFSET, 0u);

    /* UBO: vec4 table[512] at set 0 binding 0 */
    SPV_OP(&b, OP_DECORATE, 9u, DEC_ARRAY_STRIDE, 16u);
    SPV_OP(&b, OP_DECORATE, 10u, DEC_BLOCK);
    SPV_OP(&b, OP_MEMBER_DECORATE, 10u, 0u, DEC_OFFSET, 0u);
    SPV_OP(&b, OP_DECORATE, 12u, DEC_DESC_SET, 0u);
    SPV_OP(&b, OP_DECORATE, 12u, DEC_BINDING, 0u);

    /* SSBO output: vec4 out[] at set 0 binding 1 */
    SPV_OP(&b, OP_DECORATE, 13u, DEC_ARRAY_STRIDE, 16u);
    SPV_OP(&b, OP_DECORATE, 14u, DEC_BLOCK);
    SPV_OP(&b, OP_MEMBER_DECORATE, 14u, 0u, DEC_OFFSET, 0u);
    SPV_OP(&b, OP_DECORATE, 15u, DEC_DESC_SET, 0u);
    SPV_OP(&b, OP_DECORATE, 15u, DEC_BINDING, 1u);

    SPV_OP(&b, OP_DECORATE, 16u, DEC_BUILTIN, BUILTIN_GLOBAL_INVOC_ID);

    SPV_OP(&b, OP_TYPE_VOID, 1u);
    SPV_OP(&b, OP_TYPE_FUNCTION, 2u, 1u);
    SPV_OP(&b, OP_TYPE_FLOAT, 4u, 32u);
    SPV_OP(&b, OP_TYPE_VECTOR, 5u, 4u, 4u);
    SPV_OP(&b, OP_TYPE_INT, 6u, 32u, 0u);
    SPV_OP(&b, OP_TYPE_VECTOR, 7u, 6u, 3u);
    SPV_OP(&b, OP_CONSTANT, 6u, 8u, 512u);
    SPV_OP(&b, OP_TYPE_ARRAY, 9u, 5u, 8u);
    SPV_OP(&b, OP_TYPE_STRUCT, 10u, 9u);
    SPV_OP(&b, OP_TYPE_POINTER, 11u, SC_UNIFORM, 10u);
    SPV_OP(&b, OP_VARIABLE, 11u, 12u, SC_UNIFORM);

    SPV_OP(&b, OP_CONSTANT, 6u, 89u, 12u);
    SPV_OP(&b, OP_TYPE_ARRAY, 90u, 5u, 89u);
    SPV_OP(&b, OP_TYPE_STRUCT, 91u, 90u);
    SPV_OP(&b, OP_TYPE_POINTER, 92u, SC_PUSH_CONSTANT, 91u);
    SPV_OP(&b, OP_VARIABLE, 92u, 93u, SC_PUSH_CONSTANT);
    SPV_OP(&b, OP_TYPE_POINTER, 94u, SC_PUSH_CONSTANT, 5u);

    SPV_OP(&b, OP_TYPE_RUNTIME_ARRAY, 13u, 5u);
    SPV_OP(&b, OP_TYPE_STRUCT, 14u, 13u);
    SPV_OP(&b, OP_TYPE_POINTER, 17u, SC_STORAGE_BUFFER, 14u);
    SPV_OP(&b, OP_VARIABLE, 17u, 15u, SC_STORAGE_BUFFER);

    SPV_OP(&b, OP_TYPE_POINTER, 18u, SC_INPUT, 7u);
    SPV_OP(&b, OP_VARIABLE, 18u, 16u, SC_INPUT);

    SPV_OP(&b, OP_TYPE_POINTER, 19u, SC_UNIFORM, 5u);
    SPV_OP(&b, OP_TYPE_POINTER, 21u, SC_STORAGE_BUFFER, 5u);

    SPV_OP(&b, OP_CONSTANT, 6u, 22u, 0u);
    SPV_OP(&b, OP_CONSTANT, 6u, 23u, 1u);
    SPV_OP(&b, OP_CONSTANT, 6u, 24u, 2u);
    SPV_OP(&b, OP_CONSTANT, 6u, 25u, 3u);

    SPV_OP(&b, OP_FUNCTION, 1u, 3u, 0u, 2u);
    SPV_OP(&b, OP_LABEL, 30u);

    SPV_OP(&b, OP_LOAD, 7u, 31u, 16u);
    SPV_OP(&b, OP_COMPOSITE_EXTRACT, 6u, 32u, 31u, 0u); /* tid */

    /* Read 4 consecutive rows from push constant table at base=tid:
     * rows[tid+0], rows[tid+1], rows[tid+2], rows[tid+3]
     */
    SPV_OP(&b, OP_ACCESS_CHAIN, 94u, 35u, 93u, 22u, 32u);
    SPV_OP(&b, OP_LOAD, 5u, 36u, 35u);

    SPV_OP(&b, OP_IADD, 6u, 37u, 32u, 23u);
    SPV_OP(&b, OP_ACCESS_CHAIN, 94u, 38u, 93u, 22u, 37u);
    SPV_OP(&b, OP_LOAD, 5u, 39u, 38u);

    SPV_OP(&b, OP_IADD, 6u, 40u, 32u, 24u);
    SPV_OP(&b, OP_ACCESS_CHAIN, 94u, 41u, 93u, 22u, 40u);
    SPV_OP(&b, OP_LOAD, 5u, 42u, 41u);

    SPV_OP(&b, OP_IADD, 6u, 43u, 32u, 25u);
    SPV_OP(&b, OP_ACCESS_CHAIN, 94u, 44u, 93u, 22u, 43u);
    SPV_OP(&b, OP_LOAD, 5u, 45u, 44u);

    /* Combine with UBO entry table[tid] */
    SPV_OP(&b, OP_ACCESS_CHAIN, 19u, 46u, 12u, 22u, 32u);
    SPV_OP(&b, OP_LOAD, 5u, 47u, 46u);

    SPV_OP(&b, OP_FMUL, 5u, 50u, 47u, 36u);
    SPV_OP(&b, OP_FADD, 5u, 51u, 50u, 39u);
    SPV_OP(&b, OP_FMUL, 5u, 52u, 51u, 42u);
    SPV_OP(&b, OP_FADD, 5u, 53u, 52u, 45u);

    SPV_OP(&b, OP_ACCESS_CHAIN, 21u, 54u, 15u, 22u, 32u);
    SPV_OP(&b, OP_STORE, 54u, 53u);
    SPV_OP0(&b, OP_RETURN);
    SPV_OP0(&b, OP_FUNCTION_END);
    return b;
}

/* 5. High-register-pressure branching liquid-glass shader:
 *    Hoists 24 dynamic UBO vec4 loads (12 for branch A, 12 for branch B) into
 *    the entry block before a conditional branch. Under OPT_OFF, live ranges
 *    span the branch and bloat full_regs (dropping SP waves below 16).
 *    Under OPT_ON, Pass 3 sinks each branch's 12 loads into its own basic
 *    block, cutting peak register pressure and boosting A840 SP waves.
 */
static SpvBuf
build_reg_pressure_branch_fs_spv(void)
{
    SpvBuf b;
    spv_init(&b, 600u);

    SPV_OP(&b, OP_CAPABILITY, CAP_SHADER);
    SPV_OP(&b, OP_MEMORY_MODEL, 0u, 1u);
    const uint32_t ifaces[] = { 18u, 19u, 20u };
    spv_entry(&b, EXEC_FRAGMENT, 3u, "main", ifaces, 3u);
    SPV_OP(&b, OP_EXECUTION_MODE, 3u, MODE_ORIGIN_UPPER_LEFT);

    /* UBO: vec4 table[64] at set 0 binding 0 */
    SPV_OP(&b, OP_DECORATE, 9u, DEC_ARRAY_STRIDE, 16u);
    SPV_OP(&b, OP_DECORATE, 10u, DEC_BLOCK);
    SPV_OP(&b, OP_MEMBER_DECORATE, 10u, 0u, DEC_OFFSET, 0u);
    SPV_OP(&b, OP_DECORATE, 12u, DEC_DESC_SET, 0u);
    SPV_OP(&b, OP_DECORATE, 12u, DEC_BINDING, 0u);

    SPV_OP(&b, OP_DECORATE, 18u, DEC_LOCATION, 0u); /* vec4 uv/base */
    SPV_OP(&b, OP_DECORATE, 19u, DEC_LOCATION, 1u); /* uint base_idx (flat) */
    SPV_OP(&b, OP_DECORATE, 19u, 14u /* Flat */);
    SPV_OP(&b, OP_DECORATE, 20u, DEC_LOCATION, 0u); /* vec4 out */

    SPV_OP(&b, OP_TYPE_VOID, 1u);
    SPV_OP(&b, OP_TYPE_FUNCTION, 2u, 1u);
    SPV_OP(&b, OP_TYPE_FLOAT, 4u, 32u);
    SPV_OP(&b, OP_TYPE_VECTOR, 5u, 4u, 4u);
    SPV_OP(&b, OP_TYPE_INT, 6u, 32u, 0u);
    SPV_OP(&b, OP_TYPE_BOOL, 7u);
    SPV_OP(&b, OP_CONSTANT, 6u, 8u, 64u);
    SPV_OP(&b, OP_TYPE_ARRAY, 9u, 5u, 8u);
    SPV_OP(&b, OP_TYPE_STRUCT, 10u, 9u);
    SPV_OP(&b, OP_TYPE_POINTER, 11u, SC_UNIFORM, 10u);
    SPV_OP(&b, OP_VARIABLE, 11u, 12u, SC_UNIFORM);

    SPV_OP(&b, OP_TYPE_POINTER, 14u, SC_INPUT, 5u);
    SPV_OP(&b, OP_TYPE_POINTER, 15u, SC_INPUT, 6u);
    SPV_OP(&b, OP_TYPE_POINTER, 16u, SC_OUTPUT, 5u);
    SPV_OP(&b, OP_TYPE_POINTER, 17u, SC_UNIFORM, 5u);

    SPV_OP(&b, OP_VARIABLE, 14u, 18u, SC_INPUT);
    SPV_OP(&b, OP_VARIABLE, 15u, 19u, SC_INPUT);
    SPV_OP(&b, OP_VARIABLE, 16u, 20u, SC_OUTPUT);

    SPV_OP(&b, OP_CONSTANT, 6u, 21u, 0u);
    SPV_OP(&b, OP_CONSTANT, 4u, 22u, f32_bits(0.5f));

    /* Constants 1..24 for dynamic UBO indices (ids 30..53) */
    uint32_t c_ids[24];
    for (uint32_t i = 0u; i < 24u; ++i) {
        c_ids[i] = 30u + i;
        SPV_OP(&b, OP_CONSTANT, 6u, c_ids[i], i + 1u);
    }

    SPV_OP(&b, OP_FUNCTION, 1u, 3u, 0u, 2u);
    SPV_OP(&b, OP_LABEL, 60u);

    SPV_OP(&b, OP_LOAD, 5u, 61u, 18u); /* uv */
    SPV_OP(&b, OP_LOAD, 6u, 62u, 19u); /* base_idx */
    SPV_OP(&b, OP_COMPOSITE_EXTRACT, 4u, 63u, 61u, 0u);
    SPV_OP(&b, OP_FORD_GREATER_THAN, 7u, 64u, 63u, 22u);

    /* Hoist 24 independent dynamic UBO loads BEFORE the branch! */
    uint32_t next_id = 70u;
    uint32_t tap_val[24];
    for (uint32_t i = 0u; i < 24u; ++i) {
        uint32_t idx_id = next_id++;
        uint32_t ptr_id = next_id++;
        uint32_t val_id = next_id++;
        SPV_OP(&b, OP_IADD, 6u, idx_id, 62u, c_ids[i]);
        SPV_OP(&b, OP_ACCESS_CHAIN, 17u, ptr_id, 12u, 21u, idx_id);
        SPV_OP(&b, OP_LOAD, 5u, val_id, ptr_id);
        tap_val[i] = val_id;
    }

    uint32_t lbl_then = next_id++;
    uint32_t lbl_else = next_id++;
    uint32_t lbl_merge = next_id++;

    SPV_OP(&b, OP_SELECTION_MERGE, lbl_merge, 0u);
    SPV_OP(&b, OP_BRANCH_CONDITIONAL, 64u, lbl_then, lbl_else);

    /* Then branch: only consumes taps 0..11 */
    SPV_OP(&b, OP_LABEL, lbl_then);
    uint32_t acc_a = tap_val[0];
    for (uint32_t i = 1u; i < 12u; ++i) {
        uint32_t prod = next_id++;
        uint32_t sum = next_id++;
        SPV_OP(&b, OP_FMUL, 5u, prod, tap_val[i], 61u);
        SPV_OP(&b, OP_FADD, 5u, sum, acc_a, prod);
        acc_a = sum;
    }
    SPV_OP(&b, OP_BRANCH, lbl_merge);

    /* Else branch: only consumes taps 12..23 */
    SPV_OP(&b, OP_LABEL, lbl_else);
    uint32_t acc_b = tap_val[12];
    for (uint32_t i = 13u; i < 24u; ++i) {
        uint32_t prod = next_id++;
        uint32_t sum = next_id++;
        SPV_OP(&b, OP_FMUL, 5u, prod, tap_val[i], 61u);
        SPV_OP(&b, OP_FSUB, 5u, sum, acc_b, prod);
        acc_b = sum;
    }
    SPV_OP(&b, OP_BRANCH, lbl_merge);

    /* Merge */
    SPV_OP(&b, OP_LABEL, lbl_merge);
    uint32_t res_phi = next_id++;
    SPV_OP(&b, OP_PHI, 5u, res_phi, acc_a, lbl_then, acc_b, lbl_else);
    SPV_OP(&b, OP_STORE, 20u, res_phi);
    SPV_OP0(&b, OP_RETURN);
    SPV_OP0(&b, OP_FUNCTION_END);
    return b;
}

/* 6. High-register-pressure 16-tap unrolled convolution compute kernel:
 *    All 16 dynamic UBO loads are emitted up front before sequential weighted
 *    accumulation, stressing intra-block live-range sinking & anti-spill.
 */
static SpvBuf
build_reg_pressure_16tap_cs_spv(void)
{
    SpvBuf b;
    spv_init(&b, 600u);

    SPV_OP(&b, OP_CAPABILITY, CAP_SHADER);
    SPV_OP(&b, OP_MEMORY_MODEL, 0u, 1u);
    const uint32_t ifaces[] = { 16u };
    spv_entry(&b, EXEC_GLCOMPUTE, 3u, "main", ifaces, 1u);
    SPV_OP(&b, OP_EXECUTION_MODE, 3u, MODE_LOCAL_SIZE, 64u, 1u, 1u);

    SPV_OP(&b, OP_DECORATE, 9u, DEC_ARRAY_STRIDE, 16u);
    SPV_OP(&b, OP_DECORATE, 10u, DEC_BLOCK);
    SPV_OP(&b, OP_MEMBER_DECORATE, 10u, 0u, DEC_OFFSET, 0u);
    SPV_OP(&b, OP_DECORATE, 12u, DEC_DESC_SET, 0u);
    SPV_OP(&b, OP_DECORATE, 12u, DEC_BINDING, 0u);

    SPV_OP(&b, OP_DECORATE, 13u, DEC_ARRAY_STRIDE, 16u);
    SPV_OP(&b, OP_DECORATE, 14u, DEC_BLOCK);
    SPV_OP(&b, OP_MEMBER_DECORATE, 14u, 0u, DEC_OFFSET, 0u);
    SPV_OP(&b, OP_DECORATE, 15u, DEC_DESC_SET, 0u);
    SPV_OP(&b, OP_DECORATE, 15u, DEC_BINDING, 1u);

    SPV_OP(&b, OP_DECORATE, 16u, DEC_BUILTIN, BUILTIN_GLOBAL_INVOC_ID);

    SPV_OP(&b, OP_TYPE_VOID, 1u);
    SPV_OP(&b, OP_TYPE_FUNCTION, 2u, 1u);
    SPV_OP(&b, OP_TYPE_FLOAT, 4u, 32u);
    SPV_OP(&b, OP_TYPE_VECTOR, 5u, 4u, 4u);
    SPV_OP(&b, OP_TYPE_INT, 6u, 32u, 0u);
    SPV_OP(&b, OP_TYPE_VECTOR, 7u, 6u, 3u);
    SPV_OP(&b, OP_CONSTANT, 6u, 8u, 128u);
    SPV_OP(&b, OP_TYPE_ARRAY, 9u, 5u, 8u);
    SPV_OP(&b, OP_TYPE_STRUCT, 10u, 9u);
    SPV_OP(&b, OP_TYPE_POINTER, 11u, SC_UNIFORM, 10u);
    SPV_OP(&b, OP_VARIABLE, 11u, 12u, SC_UNIFORM);

    SPV_OP(&b, OP_TYPE_RUNTIME_ARRAY, 13u, 5u);
    SPV_OP(&b, OP_TYPE_STRUCT, 14u, 13u);
    SPV_OP(&b, OP_TYPE_POINTER, 17u, SC_STORAGE_BUFFER, 14u);
    SPV_OP(&b, OP_VARIABLE, 17u, 15u, SC_STORAGE_BUFFER);

    SPV_OP(&b, OP_TYPE_POINTER, 18u, SC_INPUT, 7u);
    SPV_OP(&b, OP_VARIABLE, 18u, 16u, SC_INPUT);

    SPV_OP(&b, OP_TYPE_POINTER, 19u, SC_UNIFORM, 5u);
    SPV_OP(&b, OP_TYPE_POINTER, 20u, SC_STORAGE_BUFFER, 5u);

    SPV_OP(&b, OP_CONSTANT, 6u, 21u, 0u);

    uint32_t next_id = 30u;
    uint32_t idx_consts[16];
    uint32_t w_consts[16];
    for (uint32_t i = 0u; i < 16u; ++i) {
        idx_consts[i] = next_id++;
        SPV_OP(&b, OP_CONSTANT, 6u, idx_consts[i], i);
        uint32_t ws = next_id++;
        w_consts[i] = next_id++;
        SPV_OP(&b, OP_CONSTANT, 4u, ws, f32_bits(0.0625f * (float)(i + 1u)));
        SPV_OP(&b, OP_CONSTANT_COMPOSITE, 5u, w_consts[i], ws, ws, ws, ws);
    }

    SPV_OP(&b, OP_FUNCTION, 1u, 3u, 0u, 2u);
    SPV_OP(&b, OP_LABEL, next_id++);

    uint32_t gid_v = next_id++;
    uint32_t tid = next_id++;
    SPV_OP(&b, OP_LOAD, 7u, gid_v, 16u);
    SPV_OP(&b, OP_COMPOSITE_EXTRACT, 6u, tid, gid_v, 0u);

    /* Phase 1: hoist all 16 UBO loads before any accumulation */
    uint32_t taps[16];
    for (uint32_t i = 0u; i < 16u; ++i) {
        uint32_t idx = next_id++;
        uint32_t ptr = next_id++;
        taps[i] = next_id++;
        SPV_OP(&b, OP_IADD, 6u, idx, tid, idx_consts[i]);
        SPV_OP(&b, OP_ACCESS_CHAIN, 19u, ptr, 12u, 21u, idx);
        SPV_OP(&b, OP_LOAD, 5u, taps[i], ptr);
    }

    /* Phase 2: linear weighted accumulation */
    uint32_t acc = next_id++;
    SPV_OP(&b, OP_FMUL, 5u, acc, taps[0], w_consts[0]);
    for (uint32_t i = 1u; i < 16u; ++i) {
        uint32_t term = next_id++;
        uint32_t sum = next_id++;
        SPV_OP(&b, OP_FMUL, 5u, term, taps[i], w_consts[i]);
        SPV_OP(&b, OP_FADD, 5u, sum, acc, term);
        acc = sum;
    }

    uint32_t out_ptr = next_id++;
    SPV_OP(&b, OP_ACCESS_CHAIN, 20u, out_ptr, 15u, 21u, tid);
    SPV_OP(&b, OP_STORE, out_ptr, acc);
    SPV_OP0(&b, OP_RETURN);
    SPV_OP0(&b, OP_FUNCTION_END);
    return b;
}

/* 7. Apple XR10 wide-color [-0.752941, 1.25098] + GMEM FBFetch protection test.
 *    Uses half4 input -> f32 -> XR10 clamp -> blend with FBFetch -> half4 out.
 *    Pass 1 MUST detect XR10 boundaries and refuse to narrow the XR10 FP32
 *    clamping math (fp16_alu_narrowed == 0, xr10_detected == true, xr10_safe == true).
 */
static SpvBuf
build_xr10_wide_color_guard_fs_spv(void)
{
    SpvBuf b;
    spv_init(&b, 140u);

    SPV_OP(&b, OP_CAPABILITY, CAP_SHADER);
    SPV_OP(&b, OP_CAPABILITY, CAP_FLOAT16);
    SPV_OP(&b, OP_CAPABILITY, CAP_INPUT_ATTACHMENT);
    SPV_OP(&b, OP_CAPABILITY, CAP_STORAGE_IO16);
    SPV_OP(&b, OP_MEMORY_MODEL, 0u, 1u);
    const uint32_t ifaces[] = { 10u, 11u };
    spv_entry(&b, EXEC_FRAGMENT, 3u, "main", ifaces, 2u);
    SPV_OP(&b, OP_EXECUTION_MODE, 3u, MODE_ORIGIN_UPPER_LEFT);

    SPV_OP(&b, OP_DECORATE, 10u, DEC_LOCATION, 0u);
    SPV_OP(&b, OP_DECORATE, 11u, DEC_LOCATION, 0u);
    SPV_OP(&b, OP_DECORATE, 14u, DEC_DESC_SET, 0u);
    SPV_OP(&b, OP_DECORATE, 14u, DEC_BINDING, 0u);
    SPV_OP(&b, OP_DECORATE, 14u, DEC_INPUT_ATT_IDX, 0u);

    SPV_OP(&b, OP_TYPE_VOID, 1u);
    SPV_OP(&b, OP_TYPE_FUNCTION, 2u, 1u);
    SPV_OP(&b, OP_TYPE_FLOAT, 4u, 16u);
    SPV_OP(&b, OP_TYPE_VECTOR, 5u, 4u, 4u);
    SPV_OP(&b, OP_TYPE_FLOAT, 6u, 32u);
    SPV_OP(&b, OP_TYPE_VECTOR, 7u, 6u, 4u);
    SPV_OP(&b, OP_TYPE_INT, 8u, 32u, 1u);
    SPV_OP(&b, OP_TYPE_VECTOR, 9u, 8u, 2u);
    SPV_OP(&b, OP_TYPE_BOOL, 15u);
    SPV_OP(&b, OP_TYPE_VECTOR, 16u, 15u, 4u);

    SPV_OP(&b, OP_TYPE_POINTER, 17u, SC_INPUT, 5u);
    SPV_OP(&b, OP_TYPE_POINTER, 18u, SC_OUTPUT, 5u);
    SPV_OP(&b, OP_VARIABLE, 17u, 10u, SC_INPUT);
    SPV_OP(&b, OP_VARIABLE, 18u, 11u, SC_OUTPUT);

    /* SubpassData image for GMEM FBFetch */
    SPV_OP(&b, OP_TYPE_IMAGE, 12u, 6u, 6u /* SubpassData */, 0u, 0u, 0u, 2u, 0u);
    SPV_OP(&b, OP_TYPE_POINTER, 13u, SC_UNIFORM_CONSTANT, 12u);
    SPV_OP(&b, OP_VARIABLE, 13u, 14u, SC_UNIFORM_CONSTANT);

    SPV_OP(&b, OP_CONSTANT, 8u, 20u, 0u);
    SPV_OP(&b, OP_CONSTANT_COMPOSITE, 9u, 21u, 20u, 20u);

    /* XR10 bounds: [-0.752941, 1.25098] */
    SPV_OP(&b, OP_CONSTANT, 6u, 22u, f32_bits(-0.752941f));
    SPV_OP(&b, OP_CONSTANT_COMPOSITE, 7u, 23u, 22u, 22u, 22u, 22u);
    SPV_OP(&b, OP_CONSTANT, 6u, 24u, f32_bits(1.25098f));
    SPV_OP(&b, OP_CONSTANT_COMPOSITE, 7u, 25u, 24u, 24u, 24u, 24u);
    SPV_OP(&b, OP_CONSTANT, 6u, 26u, f32_bits(0.5f));
    SPV_OP(&b, OP_CONSTANT_COMPOSITE, 7u, 27u, 26u, 26u, 26u, 26u);

    SPV_OP(&b, OP_FUNCTION, 1u, 3u, 0u, 2u);
    SPV_OP(&b, OP_LABEL, 30u);

    SPV_OP(&b, OP_LOAD, 5u, 31u, 10u);
    SPV_OP(&b, OP_FCONVERT, 7u, 32u, 31u);

    SPV_OP(&b, OP_LOAD, 12u, 33u, 14u);
    SPV_OP(&b, OP_IMAGE_READ, 7u, 34u, 33u, 21u);

    /* XR10 wide-color blend + clamp in FP32 */
    SPV_OP(&b, OP_FMUL, 7u, 35u, 32u, 27u);
    SPV_OP(&b, OP_FADD, 7u, 36u, 35u, 34u);
    SPV_OP(&b, OP_FORD_LESS_THAN, 16u, 37u, 36u, 23u);
    SPV_OP(&b, OP_SELECT, 7u, 38u, 37u, 23u, 36u);
    SPV_OP(&b, OP_FORD_GREATER_THAN, 16u, 39u, 38u, 25u);
    SPV_OP(&b, OP_SELECT, 7u, 40u, 39u, 25u, 38u);

    SPV_OP(&b, OP_FCONVERT, 5u, 41u, 40u);
    SPV_OP(&b, OP_STORE, 11u, 41u);
    SPV_OP0(&b, OP_RETURN);
    SPV_OP0(&b, OP_FUNCTION_END);
    return b;
}

/* =========================================================================
 * A/B Compilation & Quantification Harness
 * ========================================================================= */

static uint8_t *
read_file_all(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    check_cond(f != NULL, "fopen metallib", path);
    check_cond(fseek(f, 0, SEEK_END) == 0, "fseek end", path);
    long sz = ftell(f);
    check_cond(sz > 0, "ftell > 0", path);
    check_cond(fseek(f, 0, SEEK_SET) == 0, "fseek set", path);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    check_cond(buf != NULL, "malloc metallib", path);
    size_t got = fread(buf, 1u, (size_t)sz, f);
    fclose(f);
    check_cond(got == (size_t)sz, "fread metallib", path);
    *out_len = (size_t)sz;
    return buf;
}

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
    t.flags = 0u;
    return t;
}

static uint32_t
effective_reg_vec4(const A840Ir3ShaderStats *s)
{
    uint32_t half_as_full = (s->half_regs + 1u) / 2u;
    if (s->merged_regs)
        return s->full_regs > half_as_full ? s->full_regs : half_as_full;
    return s->full_regs + half_as_full;
}

typedef struct HotspotAbRecord {
    char name[128];
    char category[32];
    char opt_group[32];
    char source[64];
    char stage[16];
    bool is_xr10;
    bool expects_improvement;
    A840Ir3ShaderStats off_stats;
    A840Ir3ShaderStats on_stats;
    A840Ir3StatsDiff diff;
    A840MetalOptReport opt_report;
} HotspotAbRecord;

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
run_hotspot_ab_case(const char *name,
                    const char *category,
                    const char *opt_group,
                    const char *source,
                    uint32_t stage,
                    const uint32_t *spirv_words,
                    size_t spirv_bytes,
                    const char *entry_point,
                    uint32_t extra_flags,
                    uint32_t color_rt_mask,
                    uint32_t half_rt_mask,
                    bool is_xr10,
                    bool expects_improvement,
                    HotspotAbRecord *out_rec)
{
    g_cases++;
    A840CompileTarget target = kaanapali_target();

    A840StageInput input;
    memset(&input, 0, sizeof(input));
    input.abi_version = ADRENO_COMPILE_ABI_VERSION;
    input.stage = stage;
    input.spirv_words = spirv_words;
    input.spirv_bytes = spirv_bytes;
    input.entry_point = entry_point;
    input.color_rt_mask = color_rt_mask;
    input.half_rt_mask = half_rt_mask;
    input.sample_count = 1u;
    if (stage == A840_STAGE_COMPUTE) {
        input.local_size[0] = 8u;
        input.local_size[1] = 8u;
        input.local_size[2] = 1u;
    }
    /* Run both OPT_OFF and OPT_ON on top of the W09 ROBUST_OFF baseline. */
    input.flags = extra_flags | A840_COMPILE_FLAG_ROBUST_OFF;

    A840RobustnessDecision rdec;
    uint32_t pol_extra = is_xr10 ? A840_COMPILE_FLAG_XR10_CLAMP : 0u;
    bool has_fb = (extra_flags & A840_COMPILE_FLAG_FBFETCH_GMEM) != 0u;
    check_cond(a840_robustness_decide_for_stage(&input, input.flags | pol_extra,
                                                true, false, has_fb, &rdec) == A840_COMPILE_OK,
               "robustness decide", name);

    A840CompiledStage st_off, st_on;
    memset(&st_off, 0, sizeof(st_off));
    memset(&st_on, 0, sizeof(st_on));
    char err[512] = { 0 };

    /* 1. Compile with OPT_OFF (W09 baseline) */
    a840_metal_opt_set_thread_mask(A840_METAL_OPT_NONE);
    A840CompileStatus rc_off = a840_compile_stage(&target, &input, &st_off, err, sizeof(err));
    check_cond(rc_off == A840_COMPILE_OK, "compile OPT_OFF", err[0] ? err : name);

    /* 2. Compile with OPT_ON (W10-L2 all 3 hotspot passes) */
    a840_metal_opt_set_thread_mask(A840_METAL_OPT_ALL);
    A840CompileStatus rc_on = a840_compile_stage(&target, &input, &st_on, err, sizeof(err));
    check_cond(rc_on == A840_COMPILE_OK, "compile OPT_ON", err[0] ? err : name);

    A840MetalOptReport rep_on;
    memset(&rep_on, 0, sizeof(rep_on));
    check_cond(a840_metal_opt_get_last_report(&rep_on), "get_last_report OPT_ON", name);
    check_cond(rep_on.active_pass_mask == A840_METAL_OPT_ALL, "active_pass_mask == ALL", name);
    check_cond(rep_on.xr10_safe, "report xr10_safe", name);
    check_cond(rep_on.push_layout_preserved, "report push_layout_preserved", name);

    A840Ir3ShaderStats s_off, s_on;
    A840Ir3StatsDiff diff;
    check_cond(a840_ir3_stats_analyze(&st_off, rdec.policy_flags, &s_off) == A840_COMPILE_OK,
               "stats analyze OPT_OFF", name);
    check_cond(a840_ir3_stats_analyze(&st_on, rdec.policy_flags, &s_on) == A840_COMPILE_OK,
               "stats analyze OPT_ON", name);
    check_cond(a840_ir3_stats_diff(&s_off, &s_on, &diff) == A840_COMPILE_OK,
               "stats diff", name);

    uint32_t eff_off = effective_reg_vec4(&s_off);
    uint32_t eff_on = effective_reg_vec4(&s_on);

    printf("  [AB] %-32s (%-12s/%-12s): instrs %3u -> %3u (%+4d, %5.1f%%) "
           "cat1 %+3d alu %+3d mem %+3d regF %2u->%2u regH %2u->%2u spill %u->%u "
           "constDW %2u->%2u waves %2u->%2u [fp16:%u/%u push:%u/%u sink:%u live:%u->%u]\n",
           name, opt_group, category,
           s_off.total_instrs, s_on.total_instrs, diff.delta_total_instrs, diff.reduction_pct,
           diff.delta_cat1_mov, diff.delta_cat234_alu, diff.delta_cat6_mem,
           s_off.full_regs, s_on.full_regs, s_off.half_regs, s_on.half_regs,
           s_off.reg_spills, s_on.reg_spills,
           s_off.const_dwords, s_on.const_dwords,
           s_off.est_waves_per_sp, s_on.est_waves_per_sp,
           rep_on.fp16_roundtrip_elided, rep_on.fp16_alu_narrowed,
           rep_on.push_const_loads_merged, rep_on.push_const_offsets_folded,
           rep_on.sink_moves_applied,
           rep_on.peak_live_vec4_before, rep_on.peak_live_vec4_after);

    check_cond(s_off.total_instrs > 0u && s_on.total_instrs > 0u, "non-empty ISA", name);
    check_cond(s_off.est_waves_per_sp > 0u && s_on.est_waves_per_sp > 0u, "waves > 0", name);
    check_cond(!diff.instr_regressed, "zero instruction regression", name);
    check_cond(diff.delta_full_regs <= 0, "zero full-reg regression", name);
    check_cond(eff_on <= eff_off, "zero effective register-file regression", name);
    check_cond(s_on.reg_spills <= s_off.reg_spills, "zero spill regression", name);
    check_cond(!diff.wave_regressed, "zero wave occupancy regression", name);
    check_cond(diff.xr_preserved, "XR10 safety preserved", name);

    if (has_fb) {
        check_cond(s_off.uses_fbfetch && s_on.uses_fbfetch, "FBFetch flag preserved", name);
    }

    if (is_xr10) {
        check_cond(rep_on.xr10_detected, "XR10 wide-color boundary detected", name);
        check_cond(rep_on.fp16_alu_narrowed == 0u, "XR10 FP32 clamp NOT narrowed", name);
        check_cond(rep_on.fp16_xr10_guarded > 0u, "XR10 FP16 guard counter incremented", name);
    }

    bool improved = (diff.saved_total_instrs > 0) ||
                    (diff.delta_cat1_mov < 0) ||
                    (diff.delta_full_regs < 0) ||
                    (s_on.reg_spills < s_off.reg_spills) ||
                    (diff.delta_est_waves_per_sp > 0) ||
                    (diff.delta_const_dwords < 0);
    if (expects_improvement) {
        check_cond(improved, "expected measurable hotspot improvement", name);
    }

    if (out_rec != NULL) {
        memset(out_rec, 0, sizeof(*out_rec));
        snprintf(out_rec->name, sizeof(out_rec->name), "%s", name);
        snprintf(out_rec->category, sizeof(out_rec->category), "%s", category);
        snprintf(out_rec->opt_group, sizeof(out_rec->opt_group), "%s", opt_group);
        snprintf(out_rec->source, sizeof(out_rec->source), "%s", source);
        snprintf(out_rec->stage, sizeof(out_rec->stage), "%s", stage_name(stage));
        out_rec->is_xr10 = is_xr10;
        out_rec->expects_improvement = expects_improvement;
        out_rec->off_stats = s_off;
        out_rec->on_stats = s_on;
        out_rec->diff = diff;
        out_rec->opt_report = rep_on;
    }

    a840_compile_stage_free(&st_off);
    a840_compile_stage_free(&st_on);
    a840_metal_opt_reset_thread_mask();
}

static void
test_opt_api_and_push_const_invariance(const SpvBuf *push_spv)
{
    g_cases++;

    /* Thread-local mask API checks */
    a840_metal_opt_reset_thread_mask();
    check_cond(a840_metal_opt_get_thread_mask() == A840_METAL_OPT_ALL,
               "default mask == ALL", NULL);
    a840_metal_opt_set_thread_mask(A840_METAL_OPT_FP16_ROUND_FOLD | 0xffff0000u);
    check_cond(a840_metal_opt_get_thread_mask() == A840_METAL_OPT_FP16_ROUND_FOLD,
               "high bits masked cleanly", NULL);
    a840_metal_opt_reset_thread_mask();

    A840MetalOptConfig c0 = a840_metal_opt_default_config(0u);
    check_cond(c0.preserve_xr10 && c0.preserve_push_layout && !c0.respect_half_rtz,
               "default config without RTZ", NULL);
    A840MetalOptConfig c1 = a840_metal_opt_default_config(A840_COMPILE_FLAG_HALF_ROUND_RTZ);
    check_cond(c1.respect_half_rtz && c1.preserve_xr10, "default config with RTZ", NULL);
    check_cond(!a840_metal_opt_get_last_report(NULL), "get_last_report(NULL) false", NULL);

    /* Verify const_push_mask folding compatibility under OPT_ON */
    A840CompileTarget target = kaanapali_target();
    A840StageInput in;
    memset(&in, 0, sizeof(in));
    in.abi_version = ADRENO_COMPILE_ABI_VERSION;
    in.stage = A840_STAGE_FRAGMENT;
    in.spirv_words = push_spv->words;
    in.spirv_bytes = push_spv->len * 4u;
    in.entry_point = "main";
    in.color_rt_mask = 1u;
    in.sample_count = 1u;
    in.flags = A840_COMPILE_FLAG_ROBUST_OFF;
    in.const_push_mask = 0x0fu;
    in.const_push_dwords[0] = f32_bits(1.0f);
    in.const_push_dwords[1] = f32_bits(0.5f);
    in.const_push_dwords[2] = f32_bits(0.25f);
    in.const_push_dwords[3] = f32_bits(1.0f);

    A840CompiledStage st;
    memset(&st, 0, sizeof(st));
    char err[256] = { 0 };
    A840CompileStatus rc = a840_compile_stage(&target, &in, &st, err, sizeof(err));
    check_cond(rc == A840_COMPILE_OK, "const_push_mask compile under OPT_ON", err);
    check_cond(st.isa_dwords > 0u, "const_push_mask emitted valid ISA", NULL);
    a840_compile_stage_free(&st);
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
dump_ab_json(const char *path, const HotspotAbRecord *recs, size_t count)
{
    FILE *f = fopen(path, "wb");
    check_cond(f != NULL, "fopen dump-json", path);
    fprintf(f, "{\n");
    fprintf(f, "  \"schema_version\": 1,\n");
    fprintf(f, "  \"wave\": \"W10-L2\",\n");
    fprintf(f, "  \"mesa_commit\": \"85fda3b36405bcaea8bffc64aa4f2651dc90770d\",\n");
    fprintf(f, "  \"target_chip_id\": \"0x44050a31\",\n");
    fprintf(f, "  \"device_verified\": false,\n");
    fprintf(f, "  \"hardware_acceptance\": false,\n");
    fprintf(f, "  \"shader_count\": %zu,\n", count);
    fprintf(f, "  \"shaders\": [\n");
    for (size_t i = 0u; i < count; ++i) {
        const HotspotAbRecord *r = &recs[i];
        const A840MetalOptReport *o = &r->opt_report;
        fprintf(f, "    {\n");
        fprintf(f, "      \"name\": \"%s\",\n", r->name);
        fprintf(f, "      \"category\": \"%s\",\n", r->category);
        fprintf(f, "      \"opt_group\": \"%s\",\n", r->opt_group);
        fprintf(f, "      \"source\": \"%s\",\n", r->source);
        fprintf(f, "      \"stage\": \"%s\",\n", r->stage);
        fprintf(f, "      \"is_xr10\": %s,\n", r->is_xr10 ? "true" : "false");
        fprintf(f, "      \"expects_improvement\": %s,\n", r->expects_improvement ? "true" : "false");
        fprintf(f, "      \"opt_off\": ");
        write_json_stats(f, &r->off_stats);
        fprintf(f, ",\n      \"opt_on\": ");
        write_json_stats(f, &r->on_stats);
        fprintf(f, ",\n      \"opt_report\": {\"active_pass_mask\":%u,"
                   "\"fp16_roundtrip_elided\":%u,\"fp16_alu_narrowed\":%u,"
                   "\"fp16_rtz_aligned\":%u,\"fp16_xr10_guarded\":%u,"
                   "\"push_const_loads_merged\":%u,\"push_const_offsets_folded\":%u,"
                   "\"uniform_widens_elided\":%u,\"sink_moves_applied\":%u,"
                   "\"peak_live_vec4_before\":%u,\"peak_live_vec4_after\":%u,"
                   "\"pre_opt_alu_count\":%u,\"post_opt_alu_count\":%u,"
                   "\"xr10_detected\":%s,\"xr10_safe\":%s,\"push_layout_preserved\":%s}\n",
                o->active_pass_mask, o->fp16_roundtrip_elided, o->fp16_alu_narrowed,
                o->fp16_rtz_aligned, o->fp16_xr10_guarded, o->push_const_loads_merged,
                o->push_const_offsets_folded, o->uniform_widens_elided, o->sink_moves_applied,
                o->peak_live_vec4_before, o->peak_live_vec4_after,
                o->pre_opt_alu_count, o->post_opt_alu_count,
                o->xr10_detected ? "true" : "false",
                o->xr10_safe ? "true" : "false",
                o->push_layout_preserved ? "true" : "false");
        fprintf(f, "    }%s\n", (i + 1u < count) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    check_cond(fclose(f) == 0, "fclose dump-json", path);
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

    HotspotAbRecord records[32];
    size_t nrec = 0u;

    /* Part A: 13 real 24A446 iOS/iPadOS metallib shaders translated via
     * libnative_metal_translate.a -> SPIR-V -> real Mesa ir3 A840 ISA.
     */
    NmbDevice *dev = nmb_device_create_a840(0u);
    check_cond(dev != NULL, "nmb_device_create_a840", nmb_last_error());

    static const char MATTING_LIB[] =
        "/home/hua/nezha-work/private/ios27/camera27-c0/registration090-research/MattingV2.metallib";
    static const char VT_LIB[] =
        "/home/hua/nezha-work/private/ios27/camera27-c0/registration093-research/VideoToolbox-default.metallib";
    static const char KAL_LIB[] =
        "/home/hua/nezha-work/ios27/gpu27-astra/private/metal-abi/NTKKaleidoscopeShaders.original.metallib";

    /* A.1: MattingV2.metallib (5 camera compute shaders) */
    {
        size_t len = 0u;
        uint8_t *bytes = read_file_all(MATTING_LIB, &len);
        NmbLibrary *lib = nmb_library_parse(dev, bytes, len);
        free(bytes);
        check_cond(lib != NULL, "parse MattingV2", nmb_last_error());

        const char *fns[] = {
            "copyTexture", "addTextures", "bilinearScale",
            "composeRGBAGuide", "excludeNonSkin"
        };
        for (size_t i = 0u; i < sizeof(fns) / sizeof(fns[0]); ++i) {
            NmbSpirvResult *spv = nmb_compile_compute_spirv(dev, lib, fns[i]);
            check_cond(spv != NULL, "translate MattingV2", fns[i]);
            size_t spv_bytes = 0u;
            const uint32_t *words = nmb_spirv_words(spv, 0u, &spv_bytes);
            char label[128];
            snprintf(label, sizeof(label), "24A446_matting_%s", fns[i]);
            run_hotspot_ab_case(label, "camera", "metallib_24a446",
                                "MattingV2.metallib", A840_STAGE_COMPUTE,
                                words, spv_bytes, nmb_spirv_entry_point(spv, 0u),
                                A840_COMPILE_FLAG_EARLY_PREAMBLE, 0u, 0u,
                                false, false, &records[nrec++]);
            nmb_spirv_result_free(spv);
        }
        nmb_library_destroy(lib);
    }

    /* A.2: VideoToolbox-default.metallib (4 compositor compute+render shaders) */
    {
        size_t len = 0u;
        uint8_t *bytes = read_file_all(VT_LIB, &len);
        NmbLibrary *lib = nmb_library_parse(dev, bytes, len);
        free(bytes);
        check_cond(lib != NULL, "parse VideoToolbox-default", nmb_last_error());

        const char *cs_fns[] = { "VTMTSComputeComposite", "VTMTSComputeFunction" };
        for (size_t i = 0u; i < sizeof(cs_fns) / sizeof(cs_fns[0]); ++i) {
            NmbSpirvResult *spv = nmb_compile_compute_spirv(dev, lib, cs_fns[i]);
            check_cond(spv != NULL, "translate VideoToolbox cs", cs_fns[i]);
            size_t spv_bytes = 0u;
            const uint32_t *words = nmb_spirv_words(spv, 0u, &spv_bytes);
            char label[128];
            snprintf(label, sizeof(label), "24A446_vt_%s", cs_fns[i]);
            run_hotspot_ab_case(label, "compositor", "metallib_24a446",
                                "VideoToolbox-default.metallib", A840_STAGE_COMPUTE,
                                words, spv_bytes, nmb_spirv_entry_point(spv, 0u),
                                A840_COMPILE_FLAG_EARLY_PREAMBLE, 0u, 0u,
                                false, false, &records[nrec++]);
            nmb_spirv_result_free(spv);
        }

        NmbSpirvResult *rend = nmb_compile_render_spirv(
            dev, lib, "VTMTSVertexFunction", "VTMTSFragmentFunction", 70u);
        check_cond(rend != NULL, "translate VideoToolbox render", nmb_last_error());
        {
            size_t vs_b = 0u, fs_b = 0u;
            const uint32_t *vs_w = nmb_spirv_words(rend, 0u, &vs_b);
            const uint32_t *fs_w = nmb_spirv_words(rend, 1u, &fs_b);
            run_hotspot_ab_case("24A446_vt_VTMTSVertexFunction", "compositor", "metallib_24a446",
                                "VideoToolbox-default.metallib", A840_STAGE_VERTEX,
                                vs_w, vs_b, nmb_spirv_entry_point(rend, 0u),
                                A840_COMPILE_FLAG_EARLY_PREAMBLE, 0u, 0u,
                                false, false, &records[nrec++]);
            run_hotspot_ab_case("24A446_vt_VTMTSFragmentFunction", "compositor", "metallib_24a446",
                                "VideoToolbox-default.metallib", A840_STAGE_FRAGMENT,
                                fs_w, fs_b, nmb_spirv_entry_point(rend, 1u),
                                A840_COMPILE_FLAG_EARLY_PREAMBLE, 1u, 0u,
                                false, false, &records[nrec++]);
        }
        nmb_spirv_result_free(rend);
        nmb_library_destroy(lib);
    }

    /* A.3: NTKKaleidoscopeShaders.original.metallib (4 liquid-glass shaders) */
    {
        size_t len = 0u;
        uint8_t *bytes = read_file_all(KAL_LIB, &len);
        NmbLibrary *lib = nmb_library_parse(dev, bytes, len);
        free(bytes);
        check_cond(lib != NULL, "parse NTKKaleidoscopeShaders", nmb_last_error());

        struct { const char *vs; const char *fs; const char *fs_label; } pairs[] = {
            { "kal_fullscreen_vert", "kal_rosette_frag",  "24A446_kal_rosette_frag" },
            { "kal_fullscreen_vert", "kal_radial_frag",   "24A446_kal_radial_frag" },
            { "kal_circle_vert",     "kal_faceted_frag",  "24A446_kal_faceted_frag" }
        };
        for (size_t i = 0u; i < sizeof(pairs) / sizeof(pairs[0]); ++i) {
            NmbSpirvResult *spv = nmb_compile_render_spirv(
                dev, lib, pairs[i].vs, pairs[i].fs, 70u);
            check_cond(spv != NULL, "translate NTKKaleidoscope", pairs[i].fs);
            if (i == 0u) {
                size_t vs_b = 0u;
                const uint32_t *vs_w = nmb_spirv_words(spv, 0u, &vs_b);
                run_hotspot_ab_case("24A446_kal_fullscreen_vert", "liquid-glass", "metallib_24a446",
                                    "NTKKaleidoscopeShaders.original.metallib", A840_STAGE_VERTEX,
                                    vs_w, vs_b, nmb_spirv_entry_point(spv, 0u),
                                    A840_COMPILE_FLAG_EARLY_PREAMBLE, 0u, 0u,
                                    false, false, &records[nrec++]);
            }
            size_t fs_b = 0u;
            const uint32_t *fs_w = nmb_spirv_words(spv, 1u, &fs_b);
            run_hotspot_ab_case(pairs[i].fs_label, "liquid-glass", "metallib_24a446",
                                "NTKKaleidoscopeShaders.original.metallib", A840_STAGE_FRAGMENT,
                                fs_w, fs_b, nmb_spirv_entry_point(spv, 1u),
                                A840_COMPILE_FLAG_EARLY_PREAMBLE, 1u, 0u,
                                false, false, &records[nrec++]);
            nmb_spirv_result_free(spv);
        }
        nmb_library_destroy(lib);
    }

    nmb_device_destroy(dev);

    /* Part B: 7 targeted Metal hotspot specialization shaders */
    {
        SpvBuf s_fp16_blur = build_fp16_glass_blur_4tap_spv();
        run_hotspot_ab_case("hotspot_fp16_glass_blur_4tap_fs", "liquid-glass", "fp16",
                            "spirv_hotspot_fp16", A840_STAGE_FRAGMENT,
                            s_fp16_blur.words, s_fp16_blur.len * 4u, "main",
                            A840_COMPILE_FLAG_EARLY_PREAMBLE, 1u, 1u,
                            false, true, &records[nrec++]);
        spv_free(&s_fp16_blur);

        SpvBuf s_fp16_rtz = build_fp16_rtz_blend_spv();
        run_hotspot_ab_case("hotspot_fp16_rtz_blend_fs", "liquid-glass", "fp16",
                            "spirv_hotspot_fp16_rtz", A840_STAGE_FRAGMENT,
                            s_fp16_rtz.words, s_fp16_rtz.len * 4u, "main",
                            A840_COMPILE_FLAG_EARLY_PREAMBLE | A840_COMPILE_FLAG_HALF_ROUND_RTZ,
                            1u, 1u, false, true, &records[nrec++]);
        spv_free(&s_fp16_rtz);

        SpvBuf s_push = build_push_const_dup_fs_spv();
        run_hotspot_ab_case("hotspot_push_const_dup_fs", "compositor", "uniform_push",
                            "spirv_hotspot_push_dup", A840_STAGE_FRAGMENT,
                            s_push.words, s_push.len * 4u, "main",
                            A840_COMPILE_FLAG_EARLY_PREAMBLE, 1u, 0u,
                            false, true, &records[nrec++]);
        test_opt_api_and_push_const_invariance(&s_push);
        spv_free(&s_push);

        SpvBuf s_ubo = build_ubo_dup_offset_cs_spv();
        run_hotspot_ab_case("hotspot_ubo_dup_offset_cs", "compute", "uniform_push",
                            "spirv_hotspot_ubo_dup", A840_STAGE_COMPUTE,
                            s_ubo.words, s_ubo.len * 4u, "main",
                            A840_COMPILE_FLAG_EARLY_PREAMBLE, 0u, 0u,
                            false, true, &records[nrec++]);
        spv_free(&s_ubo);

        SpvBuf s_reg_br = build_reg_pressure_branch_fs_spv();
        run_hotspot_ab_case("hotspot_reg_pressure_branch_fs", "liquid-glass", "reg_pressure",
                            "spirv_hotspot_reg_branch", A840_STAGE_FRAGMENT,
                            s_reg_br.words, s_reg_br.len * 4u, "main",
                            A840_COMPILE_FLAG_EARLY_PREAMBLE, 1u, 0u,
                            false, true, &records[nrec++]);
        spv_free(&s_reg_br);

        SpvBuf s_reg_16 = build_reg_pressure_16tap_cs_spv();
        run_hotspot_ab_case("hotspot_reg_pressure_16tap_cs", "liquid-glass", "reg_pressure",
                            "spirv_hotspot_reg_16tap", A840_STAGE_COMPUTE,
                            s_reg_16.words, s_reg_16.len * 4u, "main",
                            A840_COMPILE_FLAG_EARLY_PREAMBLE, 0u, 0u,
                            false, false, &records[nrec++]);
        spv_free(&s_reg_16);

        SpvBuf s_xr10 = build_xr10_wide_color_guard_fs_spv();
        run_hotspot_ab_case("hotspot_xr10_wide_color_guard_fs", "xr10", "xr10_guard",
                            "spirv_hotspot_xr10", A840_STAGE_FRAGMENT,
                            s_xr10.words, s_xr10.len * 4u, "main",
                            A840_COMPILE_FLAG_EARLY_PREAMBLE | A840_COMPILE_FLAG_FBFETCH_GMEM,
                            1u, 1u, true, false, &records[nrec++]);
        spv_free(&s_xr10);
    }

    check_cond(nrec >= 16u, "minimum 16 real SPIR-V shaders evaluated", NULL);

    if (dump_json_path != NULL) {
        dump_ab_json(dump_json_path, records, nrec);
        printf("  [JSON] Wrote %zu real hotspot A/B records -> %s\n", nrec, dump_json_path);
    }

    printf("PASS: test_metal_hotspot_opt (%zu shaders, %zu cases, %zu checks)\n",
           nrec, g_cases, g_checks);
    return EXIT_SUCCESS;
}
