/* SPDX-License-Identifier: BSD-2-Clause */
/* AppleIPAppender add/remove/getFlow: original24A446 dispatch0/1/2 and
 * class0..9 flow table. Record caller-selected values; do not dial WDS,
 * choose a mux or publish fabricated addresses. */
#ifndef NB_PDP_FLOW_H
#define NB_PDP_FLOW_H
#include <stdint.h>
#include "coachz_bb_control.h"
#define NB_PDP_FLOW_CLASSES 10u
struct nb_pdp_flow { uint32_t value[NB_PDP_FLOW_CLASSES]; };
static inline int nb_pdp_flow_call(struct nb_pdp_flow *s, uint32_t selector,
                                  const uint64_t *in, uint32_t nin, const void *structure, uint64_t bytes,
                                  uint64_t *out, uint32_t *nout)
{
    if (!s || !nout) return CBC_BAD_ARGUMENT;
    const uint32_t capacity = *nout;
    uint32_t cls, value = 0;
    if (selector == 0) {
        if (nin || capacity || !structure || bytes != 8) return CBC_BAD_ARGUMENT;
        // Structure input is two little-endian u32s, not two u64 scalars.
        const uint8_t *v = (const uint8_t *)structure;
        cls = (uint32_t)v[0] | (uint32_t)v[1] << 8 | (uint32_t)v[2] << 16 | (uint32_t)v[3] << 24;
        value = (uint32_t)v[4] | (uint32_t)v[5] << 8 | (uint32_t)v[6] << 16 | (uint32_t)v[7] << 24;
    } else if (selector == 1 || selector == 2) {
        if (nin != 1 || !in || structure || bytes || (selector == 1 ? capacity != 0 : capacity != 1 || !out))
            return CBC_BAD_ARGUMENT;
        cls = (uint32_t)in[0]; // the original wrapper reads the low u32
    } else return CBC_UNSUPPORTED;
    if (cls >= NB_PDP_FLOW_CLASSES) return CBC_BAD_ARGUMENT;
    if (selector == 2) { out[0] = s->value[cls]; *nout = 1; }
    else { s->value[cls] = value; *nout = 0; }
    return CBC_OK;
}
#endif
