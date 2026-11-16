/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Command database and ACTIVE TCS contracts reused from the qualified nezha
 * Synaptics producer and data-a-live pandora_adsp.c (d2c7089). No GPU reset. */
#ifndef SM8850_RPMH_H
#define SM8850_RPMH_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    void *context;
    int (*read)(void *, unsigned window, uint32_t offset, uint32_t *value);
    int (*write)(void *, unsigned window, uint32_t offset, uint32_t value);
    uint64_t (*now_us)(void *);
    void (*sleep_us)(void *, unsigned);
    uint32_t db_bytes, rsc_bytes;
    unsigned poisoned;
} Sm8850Rpmh;
enum { Sm8850RpmhRsc, Sm8850RpmhDatabase };
/* The caller serializes complete transactions, only after other boot-time
 * apps-RSC clients finish initialization. No interrupt or global reset writes. */
int Sm8850RpmhResource(Sm8850Rpmh *, const char *, uint32_t offset, uint32_t value);
int Sm8850RpmhArc(Sm8850Rpmh *, const char *, uint32_t virtual_level);
#endif
