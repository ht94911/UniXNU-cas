/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

/* A timeout is also the normal answer to a pending fence/frame poll. Keep
 * every occurrence in an atomic counter, and print the first four and powers
 * of two. The independent observer reports changing totals every two seconds.
 * This only decides whether to repeat a log line; it cannot alter an RPC.
 */
struct NativeAdrenoPollLog {
    uint64_t counts[2]{};
    bool keep(unsigned kind) {
        if(kind>=2)return true;
        const uint64_t n=__atomic_add_fetch(&counts[kind],uint64_t(1),__ATOMIC_RELAXED);
        return n<=4 || (n && !(n&(n-1)));
    }
    uint64_t count(unsigned kind) const {
        return kind<2?__atomic_load_n(&counts[kind],__ATOMIC_RELAXED):0;
    }
};
