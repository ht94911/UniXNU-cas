// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef NEZHA_SENSORS_H
#define NEZHA_SENSORS_H
#include "QcomSscClient.h"
#include "QcomSscMotion.h"
#include "QcomQrtr.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NEZHA_SENSOR_COMMANDS 32u
#define NEZHA_SENSOR_EVENTS 128u
#define NEZHA_SENSOR_FUSION_KIND 7u
struct nezha_sensor_owner_ops {
    void *context;
    void (*lock)(void *);
    void (*unlock)(void *);
    void (*wake)(void *); // Deferred wake of the existing owner thread only.
    bool (*online)(void *);
    uint64_t (*now_us)(void *);
    uint64_t (*counter_ticks)(void *);
    uint64_t (*host_ticks)(void *);
    int (*tx_room)(void *,uint32_t bytes); // QsscAgain is definitely not submitted.
    uint32_t counter_hz,host_hz;
};
struct nezha_sensor_status {
    uint64_t epoch;
    uint32_t reports,responses,delivered,invalid,stale,overflows;
    uint32_t phase[QSSC_SENSOR_COUNT];
    uint64_t samples[QSSC_OUTPUT_COUNT];
    uint32_t queued_commands;
    bool prepared,selected,stopped;
    int error;
};
struct nezha_sensor_command { uint32_t operation,kind,consumer;QsscDemand demand;uint64_t serial; };
struct nezha_sensor_adapter {
    struct nezha_sensor_owner_ops ops;
    QcomQrtr *router;
    QsscClient client;
    QsscMotion motion;
    QsscClock clock;
    struct nezha_sensor_command commands[NEZHA_SENSOR_COMMANDS];
    uint32_t head,count,local_port;
    uint64_t epoch;
    bool accepting,prepared,selected,stopped,paused;
    int error,consumer_fault;
};
// Sources only: selected test baseline will connect these to its single fresh
// ADSP owner after sensor PD/files/registry are up. No PAS/GLINK/IRQ here.
int nezha_sensors_prepare(struct nezha_sensor_adapter *,void *provider,QcomQrtr *,
                          const struct nezha_sensor_owner_ops *,uint64_t epoch);
int nezha_sensors_rebind(struct nezha_sensor_adapter *,QcomQrtr *fresh_router,uint64_t epoch);
int nezha_sensors_poll(struct nezha_sensor_adapter *);
void nezha_sensors_lost(struct nezha_sensor_adapter *,int error);
int nezha_sensors_stop(struct nezha_sensor_adapter *);
int nezha_sensors_sleep(struct nezha_sensor_adapter *,bool sleeping);
int32_t nezha_sensors_request(void *,uint32_t kind,uint32_t consumer,QsscDemand);
int32_t nezha_sensors_request_motion(void *,uint32_t consumer,QsscDemand);
void nezha_sensors_consumer_fault(void *,int error);
int32_t nezha_sensors_request_flush(void *,uint32_t kind,uint64_t serial);
void nezha_sensors_status(struct nezha_sensor_adapter *,struct nezha_sensor_status *);

struct nezha_sensor_hid_ops {
    void *context;
    int32_t (*request)(void *,uint32_t kind,uint32_t consumer,QsscDemand);
    int32_t (*flush)(void *,uint32_t kind,uint64_t serial);
    int32_t (*motion)(void *,uint32_t consumer,QsscDemand);
    void (*fault)(void *,int error);
};
bool nezha_sensor_hid_prepare(void *provider,const struct nezha_sensor_hid_ops *,uint64_t epoch);
int nezha_sensor_hid_sample(const QsscSample *);
int nezha_sensor_hid_motion(const QsscMotionFrame *);
int nezha_sensor_hid_state(uint32_t kind,uint64_t epoch,bool ready,int error);
int nezha_sensor_hid_flushed(uint32_t kind,uint64_t epoch,uint64_t serial);
void nezha_sensor_hid_stop(void);
bool nezha_sensor_hid_activate(void);
bool nezha_sensor_hid_rebind(uint64_t epoch);
#ifdef __cplusplus
}
#endif
#endif
