/* SPDX-License-Identifier: GPL-2.0-only */
#include "PeachAppleWire.h"
#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include <string.h>
#endif
static void erase(void *p, size_t n)
{ volatile uint8_t *v = p; while (n--) *v++ = 0; }
int peach_apple_decode_join(const void *data, size_t bytes, PeachAppleDecodedJoin *out)
{
    union { PeachAppleJoinLegacy old; PeachAppleJoin27 current; } wire;
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!data || (bytes != sizeof(wire.old) && bytes != sizeof(wire.current))) return -1;
    memset(&wire, 0, sizeof(wire));
    memcpy(&wire, data, bytes);
    unsigned modern = bytes == sizeof(wire.current);
    unsigned lower = modern ? wire.current.auth_lower : wire.old.auth_lower;
    unsigned upper = modern ? wire.current.auth_upper : wire.old.auth_upper;
    const PeachAppleNetwork *n = modern ? &wire.current.network : &wire.old.network;
    int valid = wire.old.version == 1 && lower == 1 &&
        n->ssid_bytes && n->ssid_bytes <= 32 && !(n->bssid[0] & 1);
    unsigned secured = upper != 0;
    if (secured) {
        valid &= (upper == 0x10 || (!modern && upper == 8)) && n->key_version == 1 &&
            ((n->key_cipher == 6 && n->key_bytes == 32) ||
             (n->key_cipher == 10 && n->key_bytes >= 8 && n->key_bytes <= 63));
        if (valid && n->key_cipher == 10)
            for (unsigned i = 0; i < n->key_bytes; i++)
                if (n->key[i] < 32 || n->key[i] > 126) valid = 0;
    } else valid &= n->key_bytes == 0;
    if (valid) { out->network = *n; out->secured = secured; out->modern = modern; }
    erase(&wire, sizeof(wire));
    return valid ? 0 : -1;
}
void peach_apple_assoc_event(PeachAppleAssocEvent27 *event, unsigned result, const uint8_t *bssid)
{
    memset(event, 0, sizeof(*event));
    event->result = result; event->has_result = 1;
    if (bssid) memcpy(event->attempts[0], bssid, 6);
}
