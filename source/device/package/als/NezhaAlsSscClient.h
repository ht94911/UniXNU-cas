// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef NEZHA_ALS_SSC_CLIENT_H
#define NEZHA_ALS_SSC_CLIENT_H
#include "NezhaAlsBackend.h"
#include "Tcs3760Sns.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NezhaAlsLookup = 1, NezhaAlsEnable, NezhaAlsRunning,
       NezhaAlsDisable, NezhaAlsDisabled, NezhaAlsFailed, NezhaAlsConfigure };
enum { NezhaAlsTxNone, NezhaAlsTxQueued, NezhaAlsTxAwaiting };
enum { NezhaAlsClientOkay = 0, NezhaAlsClientAgain = 1,
       NezhaAlsClientArgument = -1, NezhaAlsClientTransport = -2,
       NezhaAlsClientTimeout = -3, NezhaAlsClientWire = -4,
       NezhaAlsClientRemote = -5, NezhaAlsClientIdentity = -6,
       NezhaAlsClientFrontend = -7 };

// The sole established SSC/QRTR owner supplies its actual announced service400
// endpoint and an unadvertised local client port. No guessed node or instance.
struct nezha_als_endpoint { uint32_t node, port, instance; };
struct nezha_als_ssc_ops {
    void *context;
    // Runs on the sole owner worker, never reenters this client. 0 means queued;
    // Again means definitely NOT submitted, so the exact packet may be retained.
    // A negative/unknown result is terminal and is never retried.
    int (*send)(void *, const struct nezha_als_endpoint *, uint32_t local_port,
                const uint8_t *qmi, size_t bytes);
    // These callbacks marshal into the separate ALS front end. No sensor data
    // is generated here, and no HID-workloop caller mutates this client directly.
    // 0 means the prepared front end accepted the operation; rejection stays
    // visible and stops this client. Submission is separate from event dispatch.
    int (*ready)(void *, uint64_t epoch, bool ready);
    int (*sample)(void *, const struct nezha_als_sample *);
};

struct nezha_als_ssc_client {
    struct nezha_als_ssc_ops ops;
    struct nezha_als_endpoint endpoint;
    uint32_t local_port, interval_us, phase, tx_state;
    uint16_t transaction;
    uint64_t epoch, deadline_us, last_ssc_ticks, last_receipt_ticks;
    uint64_t last_delivery_us, sequence;
    Tcs3760SnsMatch match;
    struct nezha_als_sample latest;
    uint8_t tx[TCS3760_SNS_REQUEST_CAPACITY];
    size_t tx_bytes;
    uint32_t reports, responses, stale, invalid_samples, delivered;
    bool sensor_enabled, pending, ready;
    bool continuous;
    bool stream_active, stream_target, switch_lookup_pending;
    uint32_t configured_interval_us;
    int error;
};

// Host and kernel use the same integer-only binary32 conversion. Finite
// nonnegative calibrated lux within the selected range is rounded to nearest
// integer lux (ties upward); negative zero represents zero. No kernel FP.
bool nezha_als_integer_lux(uint32_t bits, uint32_t *lux);
// All operations are serialized on the existing transport owner's worker.
// Before a newer owner epoch, the owner must call frontend_begin(new_epoch);
// ready(false) alone does not change the front end's epoch. ready/sample are
// marshalled in that order. The HID request_interval callback only queues an
// owner command and maps client errors to IOReturn; it must not await an ACK
// while holding the HID gate or mutate this worker-owned state directly.
// now_us is monotonic owner time; receipt_ticks is current Darwin absolute time.
// Raw SSC ticks are used only for ordering, never assumed to share that epoch.
int nezha_als_ssc_begin(struct nezha_als_ssc_client *, const struct nezha_als_ssc_ops *,
                       struct nezha_als_endpoint, uint32_t local_port,
                       uint64_t epoch, uint32_t interval_us, uint64_t now_us);
int nezha_als_ssc_poll(struct nezha_als_ssc_client *, uint64_t now_us);
int nezha_als_ssc_interval(struct nezha_als_ssc_client *, uint32_t interval_us);
int nezha_als_ssc_sampling(struct nezha_als_ssc_client *, uint32_t interval_us, bool continuous);
int nezha_als_ssc_receive(struct nezha_als_ssc_client *, uint32_t node, uint32_t port,
                         const uint8_t *qmi, size_t bytes,
                         uint64_t now_us, uint64_t receipt_ticks);
void nezha_als_ssc_lost(struct nezha_als_ssc_client *, int error);

#ifdef __cplusplus
}
#endif
#endif
