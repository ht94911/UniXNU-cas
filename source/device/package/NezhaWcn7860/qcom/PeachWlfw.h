/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PEACH_WLFW_H
#define PEACH_WLFW_H
#include "PeachMhiHost.h"

enum { PeachWlfwProtocol = -10, PeachWlfwRejected = -11,
       PeachWlfwStorage = -12, PeachWlfwUnsupported = -13 };
#define PEACH_WLFW_SEGMENTS 52u

typedef struct {
    void *context;
    /* Return real, zeroed coherent DMA in this endpoint's domain. All storage
     * is retained, even if a later allocation/transaction fails. */
    int (*allocate)(void *, size_t, PeachMhiDma *);
    int (*file_read)(void *, const char *, uint64_t, void *, size_t, uint64_t *);
    void (*log)(void *, const char *, uint32_t, uint32_t);
    uint64_t iova_base, iova_bytes;
} PeachWlfwOps;

typedef struct {
    uint32_t size, type;
    PeachMhiDma dma;
} PeachWlfwMemory;

/* One worker, one outstanding QMI transaction. Receive callbacks never wait,
 * allocate, read UFS, or recursively poll MHI. Control replies have a bounded
 * queue; overflow is an error, never a silent lost flow-control message. */
typedef struct {
    PeachMhiHost *host;
    PeachWlfwOps ops;
    uint32_t peer, server_node, server_port, local_node, local_port;
    uint16_t transaction, awaiting, message, result, remote_error;
    unsigned hello, hello_sent, server, flow_pending, response_ready, response_bytes;
    unsigned control_rp, control_wp, control_count;
    unsigned memory_requested, memory_responded, memory_count, memory_ready, firmware_ready;
    unsigned phase, calibration_active, calibration_seen, calibration_done;
    PeachMhiDma calibration; /* Retained CAL region across the one firmware restart. */
    uint32_t chip_id, board_id, soc_id;
    int error;
    unsigned char controls[64][52];
    unsigned char packet[PEACH_MHI_IPCR_MTU];
    unsigned char response[2304];
    PeachWlfwMemory memory[PEACH_WLFW_SEGMENTS];
} PeachWlfw;

/* Install before IPCR is opened so an early HELLO cannot be lost. */
int peach_wlfw_init(PeachWlfw *, PeachMhiHost *, const PeachWlfwOps *);
int peach_wlfw_poll(PeachWlfw *);
/* Real service discovery, host memory, BDF/regdb and PHY/AUX download. A zero
 * result means FW_INIT_DONE was received, not that a Wi-Fi NIC exists. CE,
 * HTC/WMI/HTT and network integration belong to the subsequent data path. */
int peach_wlfw_bootstrap(PeachWlfw *);
/* Before WLAN_CFG: wait for real CAL_DONE, then OFF. The caller resets the
 * endpoint and boots mission firmware with this same retained CAL region. */
int peach_wlfw_calibrate(PeachWlfw *);
/* Call with CE DMA prepared, before enabling the host-owned CE rings. Does
 * not advertise MSI, shadow-register acceleration, MLO or a working NIC. */
int peach_wlfw_wlan_enable(PeachWlfw *, const unsigned char mac[6]);
/* Optional one-word diagnostic through WLFW ATHDIAG_READ (0x30). The
 * firmware may reject it; no direct host access to protected SRAM. */
int peach_wlfw_read_word(PeachWlfw *, uint32_t offset, unsigned memory_type, uint32_t *value);
#endif
