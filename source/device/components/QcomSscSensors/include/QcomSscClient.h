// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef QCOM_SSC_CLIENT_H
#define QCOM_SSC_CLIENT_H
#include "QcomSscMath.h"
#ifdef __cplusplus
extern "C" {
#endif
#define QSSC_SENSOR_COUNT 7u
#define QSSC_OUTPUT_COUNT 4u
#define QSSC_CONSUMERS 8u
#define QSSC_CAL_HISTORY 16u
#define QSSC_PENDING_SAMPLES 64u
enum QsscKind { QsscAccel,QsscGyro,QsscGravity,QsscProximityKind,QsscAccelCal,QsscGyroCal,QsscGameRv };
enum QsscPhase { QsscCold,QsscLookup,QsscMissing,QsscIdle,QsscRunning,QsscFailed };
typedef struct {
    uint64_t epoch,timestamp,ssc_ticks,sequence,client_id;
    uint32_t kind,status,raw_adc;
    int32_t value[6]; // Signed16.16 SI: m/s2 or rad/s; near/far is separate.
    bool valid,near;
} QsscSample;
typedef struct { uint32_t interval_us,batch_us;bool active,wakeup; } QsscDemand;
typedef struct {
    void *context;
    // Sole transport worker. Again means definitely not submitted; other errors
    // are terminal. A submitted packet is never automatically sent again.
    int (*send)(void *,QsscEndpoint,uint32_t,const uint8_t *,size_t);
    int (*sample)(void *,const QsscSample *);
    int (*state)(void *,uint32_t kind,uint64_t epoch,bool ready,int error);
    int (*flushed)(void *,uint32_t kind,uint64_t epoch,uint64_t serial);
    int (*identity)(void *,uint32_t kind,const QsscAttributes *);
    // A board whose physical accel already applies SEE calibration may use
    // its sample when the completed accel_cal lookup reports no provider.
    bool optional_accel_cal;
} QsscClientOps;
typedef struct {
    uint64_t ticks;
    int64_t bias[3];
    uint32_t status;
} QsscBias;
typedef struct {
    const char *datatype;
    QsscUid uid;
    uint64_t sequence,last_ticks,last_host,flush_serial,flush_deadline;
    uint32_t phase,interval_us,batch_us,configured_interval,configured_batch;
    QsscDemand demands[QSSC_CONSUMERS];
    QsscAttributes attributes;
    bool wakeup,configured_wakeup,present,enabled,lookup_sent,attributes_seen,flush_pending,flush_sent,flush_ack,flush_complete,ready;
    int error;
} QsscStream;
typedef struct {
    uint32_t kind;
    uint64_t ticks;
    QsscVector vector;
    QsscProximity proximity;
} QsscPending;
typedef struct {
    QsscClientOps ops;
    QsscEndpoint endpoint;
    QsscClock clock;
    QsscStream stream[QSSC_SENSOR_COUNT];
    QsscUid proximity_choices[QSSC_UID_CHOICES];
    uint32_t proximity_choice_count,proximity_choice_index;
    QsscBias calibration[2][QSSC_CAL_HISTORY];
    uint32_t calibration_count[2];
    QsscPending pending[QSSC_PENDING_SAMPLES];
    uint32_t pending_head,pending_count;
    uint64_t epoch,client_id,deadline_us,last_now_us,last_receipt_counter;
    uint64_t retired_ticks[QSSC_SENSOR_COUNT];
    uint16_t transaction;
    uint32_t local_port,tx_kind,tx_id,tx_interval,tx_batch;
    uint64_t subscription_floor[QSSC_SENSOR_COUNT];
    uint8_t tx[QSSC_REQUEST_BYTES];
    size_t tx_bytes;
    bool client_id_valid,awaiting,queued,lookup_ack,lookup_event,paused,stopped,tx_wakeup,attribute_ack,attribute_event;
    uint32_t reports,responses,delivered,invalid,stale,overflows;
    uint32_t last_report_kind,last_event_id;
    int error,stop_error;
} QsscClient;
// Caller serializes every API on the existing owner worker. HID/UI producers
// enqueue demands in the board adapter; they never enter these APIs concurrently.
int qssc_client_begin(QsscClient *,const QsscClientOps *,QsscEndpoint,uint32_t local_port,
                      uint64_t epoch,const QsscClock *,uint64_t now_us);
// Only after the real transport owner established a fresh remote session.
// Restore actual consumer demands and rediscover SUIDs/attributes/calibration.
int qssc_client_rebind(QsscClient *,QsscEndpoint,uint32_t local_port,uint64_t epoch,
                       const QsscClock *,uint64_t now_us);
int qssc_client_demand(QsscClient *,uint32_t kind,uint32_t consumer,QsscDemand);
int qssc_client_flush(QsscClient *,uint32_t kind,uint64_t serial);
int qssc_client_poll(QsscClient *,uint64_t now_us);
int qssc_client_receive(QsscClient *,uint32_t node,uint32_t port,const uint8_t *,size_t,
                        uint64_t now_us,uint64_t receipt_counter);
int qssc_client_pause(QsscClient *,bool pause,const QsscClock *wake_clock);
void qssc_client_lost(QsscClient *,int error);
int qssc_client_stop(QsscClient *);
// A local decode/consumer error remains in error while stop retires known
// subscriptions using their original transaction and real disable ACKs.
// stop_error records a failed retirement; no submitted request is replayed.
bool qssc_client_drained(const QsscClient *);
#ifdef __cplusplus
}
#endif
#endif
