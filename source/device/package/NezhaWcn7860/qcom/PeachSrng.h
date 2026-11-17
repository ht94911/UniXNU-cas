/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PEACH_SRNG_H
#define PEACH_SRNG_H
#include "PeachHtt.h"

enum { PeachSrngReoRx, PeachSrngReoException, PeachSrngTclTx,
       PeachSrngWbmCompletion, PeachSrngRxBuffer, PeachSrngRxException,
       PeachSrngIdleLink,
       PeachSrngWbmRelease,
       PeachSrngTxCompletion,
       PeachSrngKinds, PeachSrngEmpty = -21 };
typedef struct {
    void *context;
    int (*read32)(void *, uint32_t, uint32_t *);
    int (*write32)(void *, uint32_t, uint32_t);
    int (*health)(void *);
    void (*barrier)(void *);
} PeachSrngOps;
typedef struct {
    PeachSrngOps ops;
    PeachMhiDma memory, descriptors, head, tail;
    unsigned kind, entries, dwords, source, lmac, ring_id;
    unsigned prepared, started, wp, rp, loop;
    uint32_t base, doorbell, last_register, last_value;
    int error;
} PeachSrng;

/* Coherent DMA lives until endpoint DMA is genuinely stopped or reboot.
 * One worker owns all rings; copying a descriptor does not release its
 * referenced buffers. Source TP advances mean descriptor consumption only. */
size_t peach_srng_bytes(unsigned kind, unsigned entries);
int peach_srng_prepare(PeachSrng *, unsigned kind, unsigned entries,
                       PeachMhiDma, const PeachSrngOps *);
int peach_srng_start_host(PeachSrng *);
/* Seed once. WBM subsequently owns this ring, including both pointers.
 * The zeroed link pool and all ring DMA must remain retained. */
int peach_srng_seed_idle(PeachSrng *, PeachMhiDma links, unsigned count);
/* RX buffer uses id 0/type SW_TO_HW, or vendor HOST1 id 5/type SW_TO_SW.
 * Caller selects the advertised radio mapping; no pdev is inferred here. */
int peach_srng_start_lmac(PeachSrng *, PeachHtt *, unsigned pdev, unsigned id, unsigned type);
int peach_srng_push(PeachSrng *, const void *, size_t);
int peach_srng_pop(PeachSrng *, void *, size_t);
#endif
