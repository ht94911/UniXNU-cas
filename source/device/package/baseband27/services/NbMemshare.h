/* SPDX-License-Identifier: BSD-2-Clause */
/* Memory share (QMI service 52, version 1, instance 1 as announced): the modem asks the application processor for
 * physically contiguous memory for some of its clients and gives it back later.
 *
 * Message ids and TLVs: downstream drivers/soc/qcom/memshare heap_mem_ext_v01 (read for the wire format only; this is a
 * fresh implementation): 0x22 ALLOC_GENERIC {0x01 u32 bytes, 0x02 u32 client, 0x03 u32 processor, 0x04 u32 sequence,
 * 0x10 u8 contiguous, 0x11 u32 alignment} -> {0x02 result, 0x10 u32 sequence, 0x11 u8 count + {u64 address, u32 bytes}};
 * 0x23 FREE_GENERIC {0x01 u8 count + entries, 0x10 u32 client, 0x11 u32 processor} -> {0x02 result};
 * 0x24 QUERY_SIZE {0x01 u32 client, 0x10 u32 processor} -> {0x02 result, 0x10 u32 size}.
 * Behaviour as msm_memshare.c: a client is matched by id for the modem processor (0); the size granted is the configured
 * size (a request asks for at most that much; dynamic clients get their maximum); a second request of an allotted
 * client returns the same block; a failed allocation answers result 1 / error 2 (no memory); the block is assigned to the
 * modem's VM (HLOS + modem for "shared" clients) before the answer goes out; free assigns it back to HLOS and releases it
 * except for boot-time clients, which keep their block.
 *
 * nezha clients, from the phone's device tree (2026-11-07): gps id 0 size 0 boot-time, diag id 1 5 MiB on request,
 * ftm id 2 size 0, qmc_dma id 5 16 MiB shared, qmc_cma id 6 4 MiB shared, ims_1 id 7 1 MiB shared, ims_2 id 8 1 MiB.
 *
 * The board supplies the memory (a fixed reservation made at boot, never Android memory) and the hypervisor assignment
 * through the shared NezhaScm owner. An assignment that is busy leaves the request pending; it is answered on poll. */
#ifndef NB_MEMSHARE_H
#define NB_MEMSHARE_H
#include <stdint.h>
#include "../transport/NbQrtr.h"
#ifdef __cplusplus
extern "C" {
#endif

#define NB_MEMSHARE_SERVICE 52u
#define NB_MEMSHARE_INSTANCE 1u
#define NB_MEMSHARE_CLIENTS 8u
enum { NB_MEMSHARE_ALLOC = 0x22, NB_MEMSHARE_FREE = 0x23, NB_MEMSHARE_QUERY = 0x24 };
enum { NB_MEMSHARE_SHARED = 1u, NB_MEMSHARE_BOOT_TIME = 2u, NB_MEMSHARE_DYNAMIC = 4u };

typedef struct { uint32_t id, bytes, flags; const char *name; } NbMemshareClient;
extern const NbMemshareClient nb_nezha_memshare_clients[];
extern const unsigned nb_nezha_memshare_client_count;

typedef struct {
    void *context;
    /* A physically contiguous, zeroed block of bytes for client index; 0 and the address, or an error. */
    int (*alloc)(void *context, uint32_t index, uint32_t bytes, uint64_t *physical);
    void (*release)(void *context, uint32_t index, uint64_t physical, uint32_t bytes);
    /* to_modem 1: HLOS -> modem (shared: HLOS + modem); 0: back to HLOS. 0 done, NB_PENDING (1) busy, < 0 failed. */
    int (*assign)(void *context, uint64_t physical, uint32_t bytes, int to_modem, int shared);
} NbMemshareOps;

typedef struct {
    NbQrtr *qrtr;
    uint32_t port;
    NbMemshareOps ops;
    const NbMemshareClient *client;
    uint32_t count;
    struct {
        uint64_t physical;
        uint32_t bytes, sequence;
        uint8_t allotted, assigned;
        /* A request waiting for its assignment: who asked and the transaction. */
        uint8_t pending;
        uint32_t node, from;
        uint16_t transaction;
    } block[NB_MEMSHARE_CLIENTS];
    uint64_t allocs, frees, queries, failures, unknown;
} NbMemshare;

int nb_memshare_init(NbMemshare *m, NbQrtr *q, uint32_t port, const NbMemshareClient *clients, uint32_t count,
                     const NbMemshareOps *ops);
/* Answers requests whose assignment was busy. */
void nb_memshare_poll(NbMemshare *m);
/* The modem stopped: assignments go back to HLOS (pending ones through poll), blocks stay with their clients. */
void nb_memshare_modem_gone(NbMemshare *m);

#ifdef __cplusplus
}
#endif
#endif
