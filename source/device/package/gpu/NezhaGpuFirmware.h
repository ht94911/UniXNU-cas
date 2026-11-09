/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define NEZHA_GPU_FW_CAPACITY (256u * 1024u)
#define NEZHA_GPU_FW_MANIFEST 512u
#define NEZHA_GPU_FW_HASHES 544u
struct NezhaGpuFirmwareView {
    const uint8_t *data[3];
    size_t bytes[3];
};
enum NezhaGpuFirmwareStatus {
    NezhaGpuFirmwareOK = 0, NezhaGpuFirmwareMissing, NezhaGpuFirmwareHeader,
    NezhaGpuFirmwareRange, NezhaGpuFirmwareDigest
};
#ifdef __cplusplus
extern "C" {
#endif
int nezha_gpu_firmware_validate(const uint8_t *, size_t, struct NezhaGpuFirmwareView *);
extern const uint8_t nezha_gpu_fwpack[];
extern const uint64_t nezha_gpu_fwpack_size;
#ifdef __cplusplus
}
#endif
