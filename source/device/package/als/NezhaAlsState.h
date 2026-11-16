// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "NezhaAlsBackend.h"

#define NEZHA_ALS_MAX_INTEGER_LUX 66110u

// Caller serializes mutations. The class additionally locks this state for
// copyEvent/status readers. No transport, time source or generated samples.
struct nezha_als_state {
    struct nezha_als_sample latest;
    bool ready;
    bool pending;
};

static inline bool nezha_als_begin(struct nezha_als_state *s, uint64_t epoch) {
    if (!epoch || epoch <= s->latest.epoch) return false;
    const struct nezha_als_sample empty = {epoch, 0, 0, 0, false};
    s->latest = empty;
    s->ready = false;
    s->pending = false;
    return true;
}

static inline bool nezha_als_ready(struct nezha_als_state *s, uint64_t epoch, bool ready) {
    if (!epoch || epoch != s->latest.epoch) return false;
    s->ready = ready;
    if (!ready) { s->latest.valid = false; s->pending = false; }
    return true;
}

// 1: new valid sample, 0: stale/unready input, -1: current invalid sample.
static inline int nezha_als_submit(struct nezha_als_state *s,
                                    const struct nezha_als_sample *sample) {
    if (!sample || !s->ready || sample->epoch != s->latest.epoch ||
        !sample->sequence || sample->sequence <= s->latest.sequence ||
        !sample->timestamp || sample->timestamp < s->latest.timestamp) return 0;
    s->latest = *sample;
    if (!sample->valid || sample->integer_lux > NEZHA_ALS_MAX_INTEGER_LUX) {
        s->latest.valid = false;
        s->pending = false;
        return -1;
    }
    s->pending = true;
    return 1;
}
