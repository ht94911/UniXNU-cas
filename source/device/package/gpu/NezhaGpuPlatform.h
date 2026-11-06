/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* This board object retains all mappings until reboot. Its MMIO windows are
 * valid only after prepare succeeds. One caller, a normal XNU kernel thread. */
struct NezhaGpuPlatform {
    volatile uint8_t *gpu, *gmu, *smmu;
    size_t gpu_bytes, gmu_bytes, smmu_bytes;
    unsigned gpu_bank, gpu_asid, gpu_slot, gpu_sid;
    uint32_t chip_id;
};
int nezha_gpu_platform_prepare(struct NezhaGpuPlatform *);
int nezha_gpu_platform_prepare_context(void);
int nezha_gpu_platform_program_cp(uint64_t sqe_iova, uint64_t memptrs_iova);
int nezha_gpu_platform_start_sqe(void);
int nezha_gpu_platform_authenticate_zap(void);
int nezha_gpu_platform_render_state(void);
int nezha_gpu_platform_perf_start(void);
int nezha_gpu_platform_perf_copy(uint64_t out[16]);
int nezha_gpu_platform_perf_control(uint32_t mode,uint32_t milliseconds);
#ifdef __cplusplus
}
#endif
