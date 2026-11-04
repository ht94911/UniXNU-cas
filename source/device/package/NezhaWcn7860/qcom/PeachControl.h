/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PEACH_CONTROL_H
#define PEACH_CONTROL_H
#include <stdint.h>
#define PEACH_CONTROL_NAME "org.unixnu.nezha.wifi"
#define PEACH_CONTROL_VERSION 1u
#define PEACH_CONTROL_BSS 32u
enum { PeachControlStatus = 1, PeachControlJoin, PeachControlScan, PeachControlLeave };
enum { PeachNetStarting, PeachNetReady, PeachNetScanning, PeachNetAssociating,
       PeachNetHandshake, PeachNetOnline, PeachNetFailed, PeachNetRecovering };
typedef struct {
    uint32_t frequency, ssid_bytes, privacy, reserved;
    int32_t rssi;
    unsigned char bssid[6], padding[2], ssid[32];
} PeachControlBss;
typedef struct {
    uint32_t version, state;
    int32_t error;
    uint32_t count;
    char interface_name[16];
    PeachControlBss bss[PEACH_CONTROL_BSS];
} PeachControlStatusData;
typedef struct {
    uint32_t version, security, ssid_bytes, bssid_valid;
    unsigned char bssid[6], reserved[2], ssid[32], pmk[32];
} PeachControlJoinData;
_Static_assert(sizeof(PeachControlBss) == 60 && sizeof(PeachControlJoinData) == 88,
               "Peach control socket ABI");
#endif
