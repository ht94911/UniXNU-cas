// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Board-only front end for the actual non-wakeup TCS3760 ALS. The backend must
// supply calibrated integer lux, an increasing sequence and the actual Darwin
// absolute reception timestamp. SSC ticks stay in the backend; no clock-epoch
// equivalence is assumed. It owns SSC/SUID/transport and fractional-lux conversion.
// valid means a real decoded sample from the selected sensor, never a seed.
struct nezha_als_sample {
    uint64_t epoch;
    uint64_t sequence;
    uint64_t timestamp;
    uint32_t integer_lux;
    bool valid;
};

struct nezha_als_backend_ops {
    void *context;
    // ReportInterval is microseconds. Zero requests that sampling stop. Return
    // an IOReturn. Accepted requests update the driver's requested interval;
    // registry publication can subsequently fail with kIOReturnNoMemory.
    int32_t (*request_interval)(void *context, uint32_t interval_us);
    // The initial requested reporting interval must be nonzero, matching the
    // SSC client's initial subscription. Later ReportInterval0 pauses it.
    uint32_t initial_interval_us;
};

struct nezha_als_status {
    struct nezha_als_sample latest;
    uint64_t dispatched;
    uint32_t interval_us;
    bool backend_ready;
    bool published;
    bool stopped;
    bool sampling_paused;
};

// Read without entering the HID gate or state lock. The owner and transport
// points have one writer, the existing DSP worker. Call/return pairs are read
// from one atomic word; the other fields are independent progress observations.
// Counters are diagnostic uint32 values, not transport sequence identities.
struct nezha_als_run_status {
    uint32_t worker_loops, worker_point, transport_point;
    uint32_t gate_calls, gate_returns, gate_point;
    uint32_t delivery_point, deliveries;
    uint64_t delivery_sequence;
    uint32_t copy_calls, copy_returns;
    uint32_t interval_calls, interval_returns, interval_requested, interval_accepted;
    int32_t interval_result;
    uint64_t sample_sequence;
    uint32_t sample_lux, state_flags;
};
enum nezha_als_worker_point {
    NezhaAlsWorkerLoop = 1, NezhaAlsWorkerAdsp, NezhaAlsWorkerAdspReturned,
    NezhaAlsWorkerSensor, NezhaAlsWorkerSensorReturned, NezhaAlsWorkerCrash,
    NezhaAlsWorkerPublish, NezhaAlsWorkerSleep, NezhaAlsWorkerExited
};
enum nezha_als_transport_point {
    NezhaAlsTransportShared = 1, NezhaAlsTransportSharedReturned,
    NezhaAlsTransportMapper, NezhaAlsTransportMapperReturned,
    NezhaAlsTransportGlink, NezhaAlsTransportGlinkReturned,
    NezhaAlsTransportFrontend, NezhaAlsTransportFrontendReturned,
    NezhaAlsTransportDrain, NezhaAlsTransportDrainReturned
};

#ifdef __cplusplus
extern "C" {
#endif
// prepare/stop are serialized, non-interrupt owner operations. provider is the
// actual IOService associated with the board/backend. This is one boot-lifetime
// service. Preparation does not register it or publish an ALS capability.
bool nezha_als_frontend_prepare(void *provider, const struct nezha_als_backend_ops *ops,
                                uint64_t epoch);
bool nezha_als_frontend_begin(uint64_t epoch);
// This gates sample delivery/copyEvent. Interval-control availability belongs
// to the prepared backend callback, so ready(false) cannot prevent re-enable.
bool nezha_als_frontend_ready(uint64_t epoch, bool ready);
bool nezha_als_frontend_sample(const struct nezha_als_sample *sample);
// A bounded software operation on the same transport-owner worker, serialized
// with HID interval acceptance. The action takes/releases its short command
// lock inside this gate, then performs client decode/poll and nested ready/sample
// calls. It must not wait for an ACK, another worker/gate, or perform hardware,
// GLINK/FIFO/PAS operations. result is the actual client action result; false
// means the frontend rejected entry and must remain visible to its owner.
bool nezha_als_frontend_worker(int (*action)(void *), void *context, int *result);
bool nezha_als_frontend_status(struct nezha_als_status *status);
void nezha_als_frontend_stop(void);
void nezha_als_diagnostic_worker(uint32_t point);
void nezha_als_diagnostic_transport(uint32_t point);
void nezha_als_diagnostic_status(struct nezha_als_run_status *status);
#ifdef __cplusplus
}
#endif
