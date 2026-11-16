/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Shared result codes and address ranges of the nezha baseband board layer. Same meaning as the audio27 NA_* codes so the
 * shared owners (SMEM lock, IPCC dispatcher, NezhaScm, RSC/QMP lease) report one vocabulary across subsystems. */
#ifndef NB_COMMON_H
#define NB_COMMON_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    NB_OK = 0,
    NB_PENDING = 1,        /* not done yet: call again on the next event or timer tick */
    NB_ARGUMENT = -1,
    NB_IO = -2,
    NB_WIRE = -3,          /* the other side or an input file holds something malformed */
    NB_TIMEOUT = -4,
    NB_OWNERSHIP = -5,     /* a resource is not provably ours (live remote, overlap, lease missing) */
    NB_REMOTE = -6,        /* the modem reported a fatal error or its watchdog fired */
    NB_MISSING = -7,
    NB_BUSY = -8,          /* a shared owner is busy: same operation again later, nothing changed */
};

typedef struct { uint64_t base, bytes; } NbRange;

static inline int nb_range_valid(NbRange r) { return r.bytes && r.base + r.bytes > r.base; }
static inline int nb_ranges_overlap(NbRange a, NbRange b)
{
    return nb_range_valid(a) && nb_range_valid(b) && a.base < b.base + b.bytes && b.base < a.base + a.bytes;
}
static inline int nb_range_inside(NbRange inner, NbRange outer)
{
    return nb_range_valid(inner) && nb_range_valid(outer) && inner.base >= outer.base &&
           inner.base + inner.bytes <= outer.base + outer.bytes;
}

#ifdef __cplusplus
}
#endif
#endif
