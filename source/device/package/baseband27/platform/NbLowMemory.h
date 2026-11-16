/* SPDX-License-Identifier: BSD-2-Clause */
/* A loader-allocated reserved pool. XNU owns only high RAM; PAS4 metadata
 * and hyp assignment tables require real physical addresses below4GiB. */
#ifndef NB_LOW_MEMORY_H
#define NB_LOW_MEMORY_H
#include "NbCommon.h"
#define NB_LOW_MEMORY_BYTES (5u * 1024u * 1024u)
typedef struct { uint64_t physical; uint32_t bytes, used; } NbLowMemory;
static inline int nb_low_memory_init(NbLowMemory *pool, uint64_t physical, uint64_t bytes)
{
    if (!pool || pool->physical || (physical & 4095) || physical < 0x80000000ULL ||
        bytes != NB_LOW_MEMORY_BYTES || physical > 0x100000000ULL - bytes) return NB_ARGUMENT;
    *pool = (NbLowMemory){physical, (uint32_t)bytes, 0};
    return NB_OK;
}
static inline int nb_low_memory_take(NbLowMemory *pool, uint32_t bytes, uint32_t align, uint64_t *physical)
{
    if (!pool || !physical || !pool->physical || !bytes || (bytes & 4095) ||
        align < 4096 || (align & (align - 1))) return NB_ARGUMENT;
    uint64_t at = (pool->physical + pool->used + align - 1) & ~(uint64_t)(align - 1);
    if (at < pool->physical || at - pool->physical > pool->bytes ||
        bytes > pool->bytes - (at - pool->physical)) return NB_IO;
    *physical = at;
    pool->used = (uint32_t)(at - pool->physical + bytes);
    return NB_OK;
}
#endif
