/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PEACH_CE_H
#define PEACH_CE_H
#include "PeachMhiFirmware.h"

/* Peach v2 register/descriptor ABI, Kiwi CE assignment selected by the
 * vendor's TARGET_TYPE_PEACH path. CE9-11 belong to MHI and are never touched.
 * One worker owns all rings. Callbacks may copy/parse; they must not wait,
 * recursively poll, or keep the borrowed RX buffer after returning. */
#define PEACH_CE_DMA_BYTES 0x220000u
#define PEACH_CE_CHANNELS 5u
enum { PeachCeFull = -6, PeachCeProtocol = -20 };
typedef struct { uint32_t pipe, direction, entries, max_bytes, flags; } PeachCeTarget;
typedef struct { uint32_t service, direction, pipe; } PeachCeService;
extern const PeachCeTarget peach_ce_target[9];
extern const PeachCeService peach_ce_services[15];

typedef struct {
    void *context;
    /* Target register offsets, not BAR offsets. The platform supplies the
     * Peach PCI aperture, bus-fault containment and own-domain DMA checks. */
    int (*read32)(void *, uint32_t, uint32_t *);
    int (*write32)(void *, uint32_t, uint32_t);
    void (*barrier)(void *);
    void (*receive)(void *, unsigned pipe, const void *, size_t);
    void *receive_context;
} PeachCeOps;

typedef struct {
    PeachMhiDma descriptors, pointer;
    uint32_t base, doorbell;
    unsigned entries, dwords, wp, rp;
} PeachCeRing;
typedef struct {
    PeachCeRing ring, status;
    PeachMhiDma buffers;
    unsigned limit, receive;
} PeachCeChannel;
typedef struct {
    PeachCeOps ops;
    PeachMhiDma memory;
    PeachCeChannel channel[PEACH_CE_CHANNELS];
    unsigned prepared, started, polling, next_channel;
    uint32_t last_register, last_value;
    int error;
} PeachCe;

/* Prepare DMA first; WLFW WLAN_CFG/MISSION precede hardware ring setup, as
 * hif_pci_bus_configure does. DMA must be zeroed, coherent and retained. */
int peach_ce_prepare(PeachCe *, PeachMhiDma, const PeachCeOps *);
int peach_ce_start(PeachCe *);
int peach_ce_poll(PeachCe *, unsigned budget);
/* Copies into retained per-slot DMA; completion is reaped before reuse. */
int peach_ce_send(PeachCe *, unsigned pipe, unsigned metadata, const void *, size_t);
#endif
