/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PEACH_MHI_FIRMWARE_H
#define PEACH_MHI_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>

/* Peach amss20.bin: 512 KiB TME/SBL followed by the WLAN ELF, not a single
 * WCN7850 image. These routines only prepare/download firmware. They do not
 * establish MHI channels, associate with an AP, or publish a network device. */
#define PEACH_MHI_SEGMENT_BYTES (512u * 1024u)
#define PEACH_MHI_MAX_SEGMENTS 32u

enum {
    PeachMhiOk = 0,
    PeachMhiInvalid = -1,
    PeachMhiIoError = -2,
    PeachMhiTimeout = -3,
    PeachMhiDeviceError = -4,
    PeachMhiWrongEnvironment = -5,
};

typedef int (*PeachFirmwareRead)(void *context, uint64_t offset, void *to, size_t size);
typedef struct {
    void *bytes;
    uint64_t iova;
    size_t capacity;
} PeachMhiDma;

typedef struct {
    void *context;
    /* Offsets are in the endpoint's BAR0. Accessors must contain bus faults.
     * write32 must order posted MMIO writes; sync publishes CPU DMA writes. */
    int (*read32)(void *, uint32_t offset, uint32_t *value);
    int (*write32)(void *, uint32_t offset, uint32_t value);
    void (*sync)(void *, const PeachMhiDma *, size_t size);
    uint64_t (*now_us)(void *);
    void (*sleep_us)(void *, unsigned);
    uint32_t bar_bytes;
    uint64_t dma_base, dma_bytes; /* isolated IOVA aperture, including firmware */
} PeachMhiIo;

typedef struct {
    PeachMhiDma sbl;
    PeachMhiDma amss[PEACH_MHI_MAX_SEGMENTS];
    PeachMhiDma vectors; /* little-endian {u64 IOVA, u64 length} pairs */
    unsigned count;
    uint32_t last_status;
    uint32_t bhi, bhie;
    unsigned phase; /* 0 unprepared, 1 staged, 2 SBL done, 3 AMSS done, 4 error/reset required */
} PeachMhiFirmware;

/* DMA buffers must already belong to the endpoint's isolated SMMU domain.
 * Keep ALL buffers mapped until reset/disconnection has stopped endpoint DMA,
 * including on an error/timeout. This layer never frees DMA memory. */
int peach_mhi_firmware_stage(PeachMhiFirmware *, PeachFirmwareRead, void *context,
                            uint64_t file_bytes);
/* Call with PCIe/power/DMA ready and EE=PBL. session is a fresh nonzero 30-bit
 * value supplied by the kernel RNG. These functions sleep and need a worker. */
int peach_mhi_download_sbl(PeachMhiFirmware *, const PeachMhiIo *, uint32_t session);
/* The host must first finish RESET->READY, program MHI contexts/event rings,
 * enter M0 and wait for EE=SBL. This call checks EE; it cannot replace that
 * host state machine. The caller must verify EE=AMSS after it succeeds. */
int peach_mhi_download_amss(PeachMhiFirmware *, const PeachMhiIo *, uint32_t session);
/* Prearm RDDM in SBL before AMSS, as mhi_prepare_for_power_up does. Both
 * storage and vectors remain mapped through containment, also on failure. */
int peach_mhi_prepare_rddm(PeachMhiFirmware *, const PeachMhiIo *, PeachMhiDma dump,
                           PeachMhiDma vectors, uint32_t session);
/* Receive an already-crashed target's dump. No forced crash or reset. */
int peach_mhi_read_rddm(PeachMhiFirmware *, const PeachMhiIo *, uint32_t session);
#endif
