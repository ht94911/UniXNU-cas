/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../package/baseband27/platform/NbLowMemory.h"
int main(void)
{
    NbLowMemory pool = {0};
    uint64_t physical = 0;
    assert(nb_low_memory_init(&pool, 0x100000000ULL, NB_LOW_MEMORY_BYTES) == NB_ARGUMENT);
    assert(nb_low_memory_init(&pool, 0xffffff000ULL, NB_LOW_MEMORY_BYTES) == NB_ARGUMENT);
    assert(nb_low_memory_init(&pool, 0xb1000000, 4096) == NB_ARGUMENT);
    assert(nb_low_memory_init(&pool, 0xb1000001, NB_LOW_MEMORY_BYTES) == NB_ARGUMENT);
    assert(!nb_low_memory_init(&pool, 0xb1000000, NB_LOW_MEMORY_BYTES));
    assert(nb_low_memory_init(&pool, 0xb2000000, NB_LOW_MEMORY_BYTES) == NB_ARGUMENT);
    assert(!nb_low_memory_take(&pool, 4096, 4096, &physical) && physical == 0xb1000000);
    for (unsigned i = 0; i < 3; i++)
        assert(!nb_low_memory_take(&pool, 65536, 4096, &physical) && physical == 0xb1001000 + i * 65536);
    assert(!nb_low_memory_take(&pool, 4 * 1024 * 1024, 4096, &physical) && physical == 0xb1031000);
    uint32_t previous = pool.used;
    assert(nb_low_memory_take(&pool, 1024 * 1024, 4096, &physical) == NB_IO && pool.used == previous);
    assert(nb_low_memory_take(&pool, 1, 4096, &physical) == NB_ARGUMENT);
    assert(nb_low_memory_take(&pool, 4096, 8191, &physical) == NB_ARGUMENT);
    assert(!nb_low_memory_take(&pool, 4096, 65536, &physical) && !(physical & 65535));
    assert(pool.used <= NB_LOW_MEMORY_BYTES && physical + 4096 <= 0x100000000ULL);
    puts("PASS low memory: real 5MiB pool, parameter/three metadata/4MiB rmtfs, bounds and alignment");
    return 0;
}
