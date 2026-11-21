/* SPDX-License-Identifier: BSD-2-Clause */
/* One LOC client belongs to the existing baseband worker. PDS clients retain
 * their QMUX identities; commands are serialized and real replies complete
 * them. No private firmware, NV writes, modem reset or new worker lives here.
 */
#ifndef NB_GNSS_H
#define NB_GNSS_H
#include "NbGnssProtocol.h"
#include "../bridge/coachz_bb_qmux.h"

#define NB_GNSS_REQUESTS 32u
#define NB_GNSS_PACKET_BYTES 160u
#define NB_GNSS_WAIT_MS 30000u
#define NB_GNSS_ERROR_BUSY 23u
#define NB_GNSS_ERROR_TIMEOUT 16u

enum NbGnssPhase { NbGnssAbsent, NbGnssQuery, NbGnssInform, NbGnssRegister, NbGnssReady, NbGnssFailed,
                   NbGnssMeasurementConfig };
enum NbGnssAction { NbGnssIdle, NbGnssStop, NbGnssMode, NbGnssStart, NbGnssReference };
enum NbGnssChange {
    NbGnssChangeState = 1, NbGnssChangeAuto = 2, NbGnssChangeForce = 4,
    NbGnssChangeParsed = 8, NbGnssChangeNmea = 16,
    NbGnssChangeTracking = 32, NbGnssChangeReset = 64, NbGnssChangeReference = 128
};
typedef struct {
    uint8_t used, channel, cid, enabled, automatic, forced_off, parsed_report, nmea_report;
    uint8_t timeout_seconds, mode, ready_notified, failed;
    uint32_t generation, interval_seconds, accuracy_metres;
    uint64_t last_utc_ms;
} NbGnssClient;
typedef struct {
    uint32_t client, generation;
    uint16_t transaction, message;
    uint8_t change;
    NbGnssClient after;
    uint8_t reference_length, reference[128];
} NbGnssRequest;
typedef struct {
    uint32_t generation, node, port, local_port, length;
    uint16_t transaction, message;
    uint8_t data[NB_GNSS_PACKET_BYTES];
} NbGnssPacket;
typedef struct {
    uint32_t interval_seconds;
    uint8_t timeout_seconds;
} NbGnssCriteria;
typedef int (*NbGnssEmit)(void *, uint32_t channel, uint32_t cid, const uint8_t *, uint32_t);
typedef void (*NbGnssFixReceived)(void *, const NbGnssFix *);
typedef struct {
    void *context;
    NbGnssEmit emit;
    NbGnssFixReceived fix_received;
    uint32_t node, port, local_port, generation, revision, last_error;
    enum NbGnssPhase phase;
    enum NbGnssAction action;
    uint8_t running, session, next_session, inhibited, active, cancelled, fault_cleanup;
    uint8_t waiting, sending, ack_seen, indication_seen;
    uint16_t next_transaction, transaction, command, failed_command;
    uint64_t sent_ms, last_fix_utc_ms, anchor_utc_ms, anchor_received_ms;
    NbGnssCriteria applied, desired;
    NbGnssClient clients[CBQ_MAX_CLIENTS];
    NbGnssRequest requests[NB_GNSS_REQUESTS], current;
    uint32_t head, count;
    NbGnssFix fix;
    NbGnssFix incoming;
    NbGnssSky sky;
    NbGnssMeasurement measurement;
    uint64_t measurement_received_ms, measurement_reports, measurement_published, measurement_rejected;
    uint32_t measurement_config_error;
    uint8_t measurement_seen, measurement_configured;
    uint8_t measurement_packet[NB_GNSS_MEASUREMENT_PACKET_BYTES];
    /* Diagnostic identity and values stay local, through the existing USB
     * provider. Raw LOC data is never replaced with generated positions. */
    char me_version[128], host_version[128], full_version[256];
    uint64_t positions, valid_fixes, published, rejected, stale, sky_reports;
    uint64_t commands, errors, output_drops, starts, stops;
    uint64_t reference_commands, reference_completed, reference_errors;
    uint32_t reference_status;
} NbGnss;

#ifdef __cplusplus
extern "C" {
#endif
void nb_gnss_init(NbGnss *, uint32_t local_port, void *, NbGnssEmit, NbGnssFixReceived);
void nb_gnss_endpoint(NbGnss *, uint32_t version, uint32_t node, uint32_t port);
void nb_gnss_lost(NbGnss *);
void nb_gnss_attach(NbGnss *, uint32_t client, uint32_t channel, uint32_t cid);
void nb_gnss_retire(NbGnss *, uint32_t client);
void nb_gnss_request(NbGnss *, uint32_t client, const uint8_t *, uint32_t);
int nb_gnss_next(NbGnss *, uint64_t now_ms, NbGnssPacket *);
void nb_gnss_sent(NbGnss *, const NbGnssPacket *, int result, uint64_t now_ms);
void nb_gnss_receive(NbGnss *, uint32_t node, uint32_t port, const uint8_t *, uint32_t, uint64_t now_ms);
void nb_gnss_poll(NbGnss *, uint64_t now_ms);
#ifdef __cplusplus
}
#endif
#endif
