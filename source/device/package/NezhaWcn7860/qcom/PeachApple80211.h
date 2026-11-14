/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef PEACH_APPLE80211_H
#define PEACH_APPLE80211_H
#include <sys/types.h>
#include <stdint.h>
enum { PeachApplePowerChanged = 1, PeachAppleScanDone = 10, PeachAppleJoinDone = 9 };
int peach_apple80211_ioctl(u_long, void *);
void peach_apple80211_event(unsigned);
void peach_apple80211_link(unsigned status, int rssi); /* 0 manual down, 1 up, 2 lost */
void peach_apple80211_join_done(unsigned, unsigned, const unsigned char[6], uint64_t);
/* Bounded software observations, copied without inspecting caller buffers.
 * Query rows are GET1/GET9/GET103; tags describe the exact source snapshot
 * used for a successful reply, not a later hardware/interface observation. */
typedef struct {
    uint64_t sequence, ticks, ssid_tag, bssid_tag;
    int32_t type, pid, result;
    uint32_t bytes, state, ssid_bytes;
} PeachAppleQueryObservation;
typedef struct {
    uint64_t sequence, ticks, calls;
    int32_t type, pid;
    uint32_t bytes;
} PeachAppleUnsupportedObservation;
typedef struct {
    PeachAppleQueryObservation query[3];
    PeachAppleUnsupportedObservation unsupported[4];
    uint64_t query_sequence, set_sequence;
    uint32_t unsupported_evictions;
} PeachAppleObservation;
void peach_apple80211_observation(PeachAppleObservation *);
#endif
