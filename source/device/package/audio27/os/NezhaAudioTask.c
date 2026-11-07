/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The selected XNU's real task layout stays in one native C translation unit.
 * No copied structure, private offset or extra C++ ABI assumption. */
#ifndef MACH_KERNEL_PRIVATE
#error Build this accessor with the selected XNU private task declarations.
#endif
#include <kern/task.h>
#include <kern/thread.h>
#include <stdint.h>
int nezha_audio_current_slide(uint32_t *slide);
int nezha_audio_current_slide(uint32_t *slide) {
    task_t task=current_task();
    if (!slide || !task || !task->shared_region) return 0;
    *slide=__atomic_load_n(&task->task_shared_region_slide,__ATOMIC_ACQUIRE);
    return *slide!=UINT32_MAX && !(*slide&4095);
}
