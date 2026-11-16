/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PEACH_STATION_H
#define PEACH_STATION_H
#include "PeachWmi.h"
#include "PeachRx.h"
#include "PeachTx.h"

enum { PeachStationIdle, PeachStationPeer, PeachStationAuth, PeachStationAssoc,
       PeachStationAwaitKeys, PeachStationOpenAssociated, PeachStationFailed };
enum { PeachStationOpen, PeachStationWpa2Psk };
/* Local progress metadata only. These values never go over the air. */
enum { PeachStationStepNone, PeachStationStepSelect, PeachStationStepPeerCreate,
       PeachStationStepPeerMap, PeachStationStepVdevStart, PeachStationStepQueue,
       PeachStationStepAuthTx, PeachStationStepAuthResponse,
       PeachStationStepAssocTx, PeachStationStepAssocResponse,
       PeachStationStepWmm, PeachStationStepPeerAssoc, PeachStationStepVdevUp,
       PeachStationStepTxBind, PeachStationStepMonitor };
typedef struct {
    PeachWmi *wmi;
    PeachHtt *htt;
    PeachRx *rx;
    PeachTx *tx;
    unsigned phase, security, sequence, on_air_status, requested_caps;
    unsigned operation, failed_phase, failed_operation;
    unsigned auth_attempts, assoc_attempts, responses_seen, last_response_subtype;
    uint32_t peer_generation;
    unsigned char bssid[6], rates[12], rsn[22], frame[512];
    unsigned rate_count, rsn_bytes;
    PeachWmiMgmt received;
    PeachWmiAssociation association;
    int error;
} PeachStation;

int peach_station_init(PeachStation *, PeachWmi *, PeachHtt *, PeachRx *, PeachTx *);
/* Selection-time compatibility check; does not mutate station/hardware state. */
int peach_station_candidate(const PeachWmiBss *, unsigned security);
/* Explicit BSS selection by the future network control path. Open-system
 * 802.11 auth + real association response, legacy 20MHz and optional WMM.
 * WPA2 supports PSK/CCMP selection and stops in AwaitKeys; no ordinary data
 * authorization, key installation, NIC or address acquisition is implied.
 * Hidden SSIDs, required PMF, enterprise, WPA1 and WPA3 are unsupported. */
int peach_station_associate(PeachStation *, unsigned bss_index, unsigned security);
/* Single worker; called by protocol progress, never recursively polls HTC.
 * Unmap/deauth/regulatory changes revoke TX and propagate failure. */
int peach_station_poll(PeachStation *);
/* Recover an unsuccessful pre-key association. Refuses installed keys,
 * transport errors or ambiguous completions; never reuses old REO queues. */
int peach_station_recover_unkeyed(PeachStation *);
#endif
