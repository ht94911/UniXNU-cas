/* SPDX-License-Identifier: GPL-2.0-only */
/* 24A446 wire contracts from UniXNU-sheng 460179565a018c741b93e0b1efa07c55d4de497a
 * package/WifiNet.cpp. These are user-memory messages, never C++ kernel objects.
 * Keep the bounded decoder shared by the real ioctl path and host tests. */
#ifndef PEACH_APPLE_WIRE_H
#define PEACH_APPLE_WIRE_H
#include <stddef.h>
#include <stdint.h>

enum { PeachAppleJoinLegacyBytes = 592, PeachAppleJoin27Bytes = 656 };
typedef struct {
    uint32_t ssid_bytes;
    uint8_t ssid[32], bssid[6], unused[2];
    uint32_t key_version, key_bytes, key_cipher;
    uint16_t key_flags, key_index;
    uint8_t key[64];
} PeachAppleNetwork;
typedef struct {
    uint32_t version;
    uint16_t mode, auth_lower, auth_upper, unused;
    PeachAppleNetwork network;
    uint8_t rest[592 - 0x88];
} PeachAppleJoinLegacy;
typedef struct {
    uint32_t version;
    uint16_t mode, unused;
    uint32_t auth_lower, auth_upper;
    PeachAppleNetwork network;
    uint8_t rest[656 - 0x8c];
} PeachAppleJoin27;
typedef struct {
    uint8_t attempts[10][52];
    uint32_t reserved[2], reason, result, has_result;
} PeachAppleAssocEvent27;
typedef struct {
    PeachAppleNetwork network;
    unsigned secured, modern;
} PeachAppleDecodedJoin;

_Static_assert(sizeof(PeachAppleNetwork) == 0x7c &&
    offsetof(PeachAppleNetwork, key) == 0x3c, "Apple80211 network prefix");
_Static_assert(sizeof(PeachAppleJoinLegacy) == 592 &&
    offsetof(PeachAppleJoinLegacy, network) == 0x0c, "legacy association");
_Static_assert(sizeof(PeachAppleJoin27) == 656 &&
    offsetof(PeachAppleJoin27, network) == 0x10, "24A446 association");
_Static_assert(sizeof(PeachAppleAssocEvent27) == 540 &&
    offsetof(PeachAppleAssocEvent27, result) == 0x214 &&
    offsetof(PeachAppleAssocEvent27, has_result) == 0x218, "24A446 result event");

/* 0 only for a supported, well-formed open/WPA2 request. Failure clears out.
 * Caller supplies kernel-owned bytes copied in using the exact observed length. */
int peach_apple_decode_join(const void *, size_t, PeachAppleDecodedJoin *);
void peach_apple_assoc_event(PeachAppleAssocEvent27 *, unsigned, const uint8_t *);
#endif
