// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef NEZHA_ALS_QRTR_BACKEND_H
#define NEZHA_ALS_QRTR_BACKEND_H
#include "NezhaAlsSscClient.h"
#include "QcomQrtr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NEZHA_ALS_INTERVAL_SLOTS 8u

// Board adapter only. The established transport owner supplies its existing
// router, actual selected service endpoint, clocks and deferred worker wake.
// This adapter never initializes a router, channel, IRQ, DSP or resource lease.
struct nezha_als_qrtr_ops {
    void *context;
    // A separate short command-queue lock, never the APPS configuration lock.
    void (*lock)(void *);
    void (*unlock)(void *);
    // Deferred notification only: no synchronous worker execution or waiting.
    void (*wake)(void *);
    bool (*online)(void *);
    uint64_t (*now_us)(void *);
    uint64_t (*receipt_ticks)(void *);
    // On the sole worker, check room for one complete QRTR packet without
    // reserving/submitting it. 0=room; Again=definitely no submission; negative
    // or unknown result=terminal. No other thread may mutate the owner TX queue.
    int (*tx_room)(void *, uint32_t bytes);
    // Actual kernel IOReturn values supplied by the board's native caller.
    // Both must be nonzero; queue rejection must not claim interval acceptance.
    int32_t interval_not_ready, interval_no_space;
};

struct nezha_als_qrtr_backend {
    struct nezha_als_qrtr_ops ops;
    QcomQrtr *router;
    struct nezha_als_ssc_client *client;
    struct nezha_als_endpoint endpoint;
    uint64_t epoch;
    uint32_t local_port, initial_interval_us, worker_interval_us;
    // Only these queue fields and accepting are shared with the HID callback.
    uint32_t intervals[NEZHA_ALS_INTERVAL_SLOTS], head, count, accepted_interval_us;
    bool modes[NEZHA_ALS_INTERVAL_SLOTS], worker_continuous, accepted_continuous;
    bool accepting;
    // Remaining state belongs exclusively to the transport owner's worker.
    bool stopped;
    int error;
};

// Fresh zeroed boot-lifetime storage and an actual owner IOService. Register an
// unadvertised port in the same router; retain storage and callback context for
// that entire router lifetime, including failure/stop. initial interval >0.
int nezha_als_qrtr_prepare(struct nezha_als_qrtr_backend *, void *provider,
                          QcomQrtr *, uint32_t local_port,
                          const struct nezha_als_qrtr_ops *, uint64_t epoch,
                          uint32_t initial_interval_us);
// Called explicitly by the actual owner after establishing the selected edge
// and uniquely announced service400 endpoint. Never choose a first-match or a
// guessed instance. Recovery requires a genuinely new owned session, a larger
// epoch, and new zeroed client storage; a larger integer alone is not proof.
int nezha_als_qrtr_select(struct nezha_als_qrtr_backend *,
                         struct nezha_als_ssc_client *,
                         struct nezha_als_endpoint, uint64_t epoch);
// Sole owner worker only, after its GLINK/QRTR receive and before owner TX drain.
// Interval dequeue and bounded SSC poll share the frontend gate with HID
// interval acceptance; no hardware/FIFO/PAS work runs inside that gate.
int nezha_als_qrtr_poll(struct nezha_als_qrtr_backend *);
// HID callback: queue requested interval/mode, then wake after unlocking. Success
// means accepted into the queue, separately from eventual SSC/DSP ACK.
int32_t nezha_als_qrtr_request_sampling(void *, uint32_t interval_us, bool continuous);
void nezha_als_qrtr_lost(struct nezha_als_qrtr_backend *, int error);
// Explicit final boot-lifetime stop; does not reset/release the owner transport.
void nezha_als_qrtr_stop(struct nezha_als_qrtr_backend *);

#ifdef __cplusplus
}
#endif
#endif
