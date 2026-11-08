/* SPDX-License-Identifier: APSL-2.0 */
/* Board integration derived from pandora_adsp.c, d2c7089b. */
#ifndef NEZHA_SOCCP_H
#define NEZHA_SOCCP_H
#include "QcomSmem.h"
#include "QcomSmp2p.h"
#include "QcomGlink.h"
#include "NezhaMca.h"

typedef struct {
    void *context;
    /* One shared software lock for ALL AP SMEM allocators, followed by TCSR
     * hardware lock3 (AP owner1). A separate per-driver lock is insufficient. */
    int (*smem_lock)(void *);
    void (*smem_unlock)(void *);
    void (*doorbell)(void *, uint32_t client, uint32_t signal);
    uint64_t (*now_ms)(void *);
    void (*link)(void *, int up, uint64_t epoch);
    void (*receive)(void *, uint64_t epoch, const uint8_t *, uint32_t);
} NpSoccpIo;
typedef struct {
    NpSoccpIo io;
    QcomSmem smem;
    QcomSmemPartition partition;
    QcomSmp2p smp2p;
    QcomGlink glink;
    QcomGlinkChannel *channel;
    uint8_t intents[8 * 1024];
    uint64_t epoch, open_deadline, retry_at, report_at;
    uint32_t slave, restart_flag, in_bytes, item_id, reports;
    int item_error;
    int out_entry;
    uint8_t attached, active, up, broken, saw_remote_down, restart_seen, retries;
} NpSoccp;
/* Only returned before any outbound allocation/write. The board may repeat
 * this read-only readiness probe with a finite deadline. */
#define NP_SOCCP_WAIT_REMOTE 1
/* Requires an EXCLUSIVE lease of AP<->SOCCP, including its shared PMIC channel.
 * Audio's AP<->ADSP is a separate edge. An existing UCSI/SOCCP owner must host
 * this client instead of constructing another NpSoccp. No remote reset/load. */
int np_soccp_attach(NpSoccp *, volatile uint8_t *smem, uint32_t smem_bytes, const NpSoccpIo *);
int np_soccp_poll(NpSoccp *);
int np_soccp_send(NpSoccp *, const uint8_t *, uint32_t);
void np_soccp_quiesce(NpSoccp *);
int np_soccp_resume(NpSoccp *);
#endif
