/* SPDX-License-Identifier: MIT */
#ifndef NEZHA_CAMERA_MEMORY_H
#define NEZHA_CAMERA_MEMORY_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* XNU12377 development kernels accept command22 for the calling process,
 * as dyld does for a root image. The increase is finite and preserves the
 * existing active/inactive fatal attributes. Camera103 repeatedly recorded
 * 50MiB inactive kills at 648–723MiB of real footprint. */
enum { NezhaCameraMemoryIncreaseCommand = 22 };
#define NEZHA_CAMERA_MEMORY_INCREASE_BYTES (UINT32_C(718) * UINT32_C(1048576))

typedef int (*NezhaCameraMemoryControl)(uint32_t, int32_t, uint32_t, void *, size_t);

static inline int nezha_camera_memory_increase(NezhaCameraMemoryControl control,
    const char *process, int32_t self)
{
    static const char expected[] = "cameracaptured";
    if (!control || !process || self <= 0) return -1;
    for (size_t i = 0; i < sizeof(expected); ++i) {
        if (process[i] != expected[i]) return -1;
    }
    return control(NezhaCameraMemoryIncreaseCommand, self,
        NEZHA_CAMERA_MEMORY_INCREASE_BYTES, NULL, 0);
}
#endif
