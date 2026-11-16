/* SPDX-License-Identifier: BSD-2-Clause */
/* The actual015 two-subscription bootstrap reached64 clients and rejected
 * QMIClientPool/NAS/DMS. Exercise allocation, both routes, revocation and
 * release above that boundary with synthetic endpoints and QMI messages. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "coachz_bb_qmux.h"

int main(void)
{
    struct cbq_bridge b;
    cbq_init(&b);
    assert(cbq_server_new(&b, 3, 1, 0, 0x123));
    assert(cbq_server_new(&b, 2, 1, 0, 0x124));
    unsigned ports[96];
    for (unsigned i = 0; i < 96; ++i) {
        unsigned service = i & 1 ? 2 : 3, channel = i & 1;
        int slot = cbq_client_alloc(&b, channel, service);
        assert(slot >= 0);
        ports[i] = CBQ_PORT_BASE + (unsigned)slot;
        const struct cbq_client *c = &b.client[slot];
        uint8_t request[13] = {1}, out[32];
        cbq_wr16(request + 1, 12);
        request[4] = (uint8_t)service; request[5] = c->cid;
        request[6] = CBQ_QMI_REQUEST;
        cbq_wr16(request + 7, 0x1234); cbq_wr16(request + 9, 0x24);
        struct cbq_result r = cbq_from_host(&b, channel, request, sizeof request, out, sizeof out);
        assert(r.kind == CBQ_TO_MODEM && r.local_port == ports[i] && r.port == 0x123u + (service == 2));
        uint8_t response[7] = {CBQ_QMI_RESPONSE};
        cbq_wr16(response + 1, 0x1234); cbq_wr16(response + 3, 0x24);
        r = cbq_from_modem(&b, ports[i], 0, r.port, response, sizeof response, out, sizeof out);
        assert(r.kind == CBQ_TO_HOST && r.channel == channel && r.cid == c->cid);
        assert(!memcmp(out + CBQ_HDR, response, sizeof response));
    }
    assert(ports[95] >= CBQ_PORT_BASE + 64u);
    /* Full capacity still fails honestly; every existing client is retained. */
    for (unsigned i = 96; i < CBQ_MAX_CLIENTS; ++i)
        assert(cbq_client_alloc(&b, 0, 3) >= 0);
    assert(cbq_client_alloc(&b, 0, 3) < 0);
    cbq_server_del(&b, 3, 0, 0x123);
    uint8_t out[32];
    unsigned revoked = 0, released = 0;
    while (cbq_next_revoke(&b, out, sizeof out).kind == CBQ_TO_HOST) ++revoked;
    while (cbq_next_release(&b)) ++released;
    assert(revoked == CBQ_MAX_CLIENTS - 48u && released == revoked);
    assert(cbq_server_new(&b, 3, 1, 0, 0x125));
    assert(cbq_client_alloc(&b, 0, 3) >= 0);
    puts("PASS QMUX capacity:96 native clients, routes above64, bounded exhaustion and endpoint release");
}
