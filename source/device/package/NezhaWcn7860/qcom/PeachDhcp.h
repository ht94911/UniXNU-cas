/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Adapted from cas@15e5361 package/wifi/CasWifiDhcp.h. Packet metadata only;
 * an observed offer/ACK is never represented as an installed system address. */
#ifndef PEACH_DHCP_H
#define PEACH_DHCP_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef struct {
    unsigned message, outbound;
    uint8_t xid[4], client[6], address[4], mask[4], router[4], dns[4];
} PeachDhcpObservation;
static inline unsigned peach_dhcp_be16(const uint8_t *p)
{ return (unsigned)p[0] * 256u + p[1]; }
static inline int peach_dhcp_inspect(const uint8_t *frame, size_t bytes, PeachDhcpObservation *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!frame || bytes < 34 || peach_dhcp_be16(frame + 12) != 0x0800) return 0;
    const uint8_t *ip = frame + 14;
    unsigned h = (ip[0] & 15) * 4u, total = peach_dhcp_be16(ip + 2);
    if ((ip[0] >> 4) != 4 || h < 20 || h > bytes - 14 || total < h + 8 ||
        total > bytes - 14 || ip[9] != 17 || (peach_dhcp_be16(ip + 6) & 0x3fff)) return 0;
    const uint8_t *udp = ip + h;
    unsigned source = peach_dhcp_be16(udp), dest = peach_dhcp_be16(udp + 2), size = peach_dhcp_be16(udp + 4);
    if (!((source == 67 && dest == 68) || (source == 68 && dest == 67)) ||
        size < 248 || size > total - h) return 0;
    const uint8_t *boot = udp + 8;
    unsigned n = size - 8;
    if (boot[0] != (source == 68 ? 1 : 2) || boot[1] != 1 || boot[2] != 6 ||
        memcmp(boot + 236, "\x63\x82\x53\x63", 4)) return 0;
    PeachDhcpObservation d = {0}; d.outbound = source == 68;
    unsigned ended = 0;
    for (unsigned at = 240; at < n;) {
        unsigned tag = boot[at++];
        if (tag == 255) { ended = 1; break; }
        if (!tag) continue;
        if (at == n) return 0;
        unsigned len = boot[at++];
        if (len > n - at) return 0;
        if (tag == 53) {
            if (len != 1 || d.message || boot[at] < 1 || boot[at] > 8) return 0;
            d.message = boot[at];
        }
        if (tag == 1) { if (len != 4) return 0; memcpy(d.mask, boot + at, 4); }
        if (tag == 3 || tag == 6) {
            if (!len || (len & 3)) return 0;
            memcpy(tag == 3 ? d.router : d.dns, boot + at, 4);
        }
        at += len;
    }
    if (!ended || !d.message) return 0;
    memcpy(d.xid, boot + 4, 4); memcpy(d.client, boot + 28, 6); memcpy(d.address, boot + 16, 4);
    *out = d; return 1;
}
#endif
