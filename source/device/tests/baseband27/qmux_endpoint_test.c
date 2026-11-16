/* SPDX-License-Identifier: BSD-2-Clause */
/* Public QMI wire numbers from libqmi@3454c2c. Endpoints, clients and payloads
 * are synthetic. This only exercises the bridge; no phone or network action. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "coachz_bb_qmux.h"

static unsigned failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #c); failures++; \
} } while (0)

static uint32_t ctl(uint8_t *frame, uint8_t txn, uint16_t id,
                    const uint8_t *value, unsigned bytes)
{
    const uint32_t size = 12u + (value ? 3u + bytes : 0u);
    memset(frame, 0, size);
    frame[0] = 1;
    cbq_wr16(frame + 1, size - 1u);
    frame[7] = txn;
    cbq_wr16(frame + 8, id);
    cbq_wr16(frame + 10, size - 12u);
    if (value) {
        frame[12] = 1;
        cbq_wr16(frame + 13, bytes);
        memcpy(frame + 15, value, bytes);
    }
    return size;
}

static uint32_t message(uint8_t *sdu, uint8_t type, uint16_t txn,
                        uint16_t id, const uint8_t *tlv, unsigned bytes)
{
    sdu[0] = type;
    cbq_wr16(sdu + 1, txn);
    cbq_wr16(sdu + 3, id);
    cbq_wr16(sdu + 5, bytes);
    if (bytes) memcpy(sdu + 7, tlv, bytes);
    return 7u + bytes;
}

static uint32_t host(uint8_t *frame, uint8_t service, uint8_t cid,
                     const uint8_t *sdu, unsigned bytes)
{
    cbq_frame_header(frame, bytes, service, cid);
    frame[3] = 0;
    memcpy(frame + 6, sdu, bytes);
    return 6u + bytes;
}

static struct cbq_result allocate(struct cbq_bridge *b, uint8_t channel, uint8_t service)
{
    uint8_t f[16], out[24];
    unsigned n = ctl(f, 3, CBQ_CTL_GET_CLIENT_ID, &service, 1);
    struct cbq_result r = cbq_from_host(b, channel, f, n, out, sizeof out);
    CHECK(r.kind == CBQ_TO_HOST && r.len == 24 && !cbq_rd16(out + 15));
    CHECK(out[22] == service && out[23] == r.cid);
    return r;
}

/* Preserve the 16-bit transaction and every service payload byte. Standard
 * VOICE, WMS and IMSA messages take the same QMUX/QRTR path as NAS and WDS. */
static void roundtrip(void)
{
    static const uint8_t result[] = { 2, 4, 0, 0, 0, 0, 0, 0x10, 1, 0, 7 };
    static const uint8_t status[] = { 0x10, 1, 0, 0x5a };
    static const struct {
        uint8_t service;
        uint16_t request, indication;
    } cases[] = {
        { 9, 0x20, 0x2e },   /* VOICE DialCall / AllCallStatus */
        { 5, 0x20, 0x01 },   /* WMS RawSend / EventReport */
        { 33, 0x20, 0x23 },  /* IMSA GetRegistration / RegistrationChanged */
        { 18, 0x20, 0x21 },  /* IMSS: payload is deliberately opaque */
        { 1, 0x20, 0x22 },   /* WDS */
        { 3, 0x24, 0x24 },   /* NAS */
        { 11, 0x2f, 0x32 }   /* UIM */
    };
    struct cbq_bridge b;
    uint8_t sdu[64], frame[80], out[80];
    cbq_init(&b);
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        CHECK(cbq_server_new(&b, cases[i].service, 1, 0, 0x100 + i));
        for (uint8_t channel = 0; channel < 2; channel++) {
            struct cbq_result client = allocate(&b, channel, cases[i].service);
            unsigned n = message(sdu, 0, 0xf123, cases[i].request, status, sizeof status);
            unsigned len = host(frame, cases[i].service, client.cid, sdu, n);
            struct cbq_result r = cbq_from_host(&b, channel, frame, len, out, sizeof out);
            CHECK(r.kind == CBQ_TO_MODEM && r.node == 0 && r.port == 0x100 + i);
            CHECK(r.local_port == client.local_port && r.len == n);
            CHECK(!memcmp(frame + 6, sdu, n) && cbq_rd16(frame + 7) == 0xf123);
            for (unsigned type = 2; type <= 4; type += 2) {
                n = message(sdu, type, type == 2 ? 0xf123 : 0,
                            type == 2 ? cases[i].request : cases[i].indication,
                            type == 2 ? result : status, type == 2 ? sizeof result : sizeof status);
                r = cbq_from_modem(&b, client.local_port, 0, 0x100 + i, sdu, n, out, sizeof out);
                CHECK(r.kind == CBQ_TO_HOST && r.channel == channel);
                CHECK(r.service == cases[i].service && r.cid == client.cid && r.len == n + 6);
                CHECK(out[0] == 1 && out[3] == 0x80 && cbq_rd16(out + 1) == n + 5);
                CHECK(!memcmp(out + 6, sdu, n));
                CHECK(cbq_from_modem(&b, client.local_port, 0, 0x101 + i, sdu, n, out, sizeof out).kind == CBQ_DROP);
                CHECK(cbq_from_modem(&b, client.local_port, 5, 0x100 + i, sdu, n, out, sizeof out).kind == CBQ_DROP);
            }
        }
    }
}

static void endpoint_lifetime(void)
{
    struct cbq_bridge b;
    uint8_t sdu[16], f[32], out[32];
    cbq_init(&b);
    CHECK(cbq_server_new(&b, 9, 1 | 2u << 8, 0, 0x182));
    struct cbq_result first = allocate(&b, 0, 9);
    CHECK(cbq_server_new(&b, 9, 1, 0, 0x183));
    struct cbq_result second = allocate(&b, 1, 9);
    unsigned n = message(sdu, 2, 0x1234, 0x20, NULL, 0);
    CHECK(cbq_from_modem(&b, first.local_port, 0, 0x182, sdu, n, out, sizeof out).kind == CBQ_TO_HOST);
    CHECK(cbq_from_modem(&b, first.local_port, 0, 0x183, sdu, n, out, sizeof out).kind == CBQ_DROP);
    CHECK(cbq_from_modem(&b, second.local_port, 0, 0x183, sdu, n, out, sizeof out).kind == CBQ_TO_HOST);
    n = message(sdu, 0, 0x1234, 0x20, NULL, 0);
    unsigned len = host(f, 9, first.cid, sdu, n);
    struct cbq_result r = cbq_from_host(&b, 0, f, len, out, sizeof out);
    CHECK(r.kind == CBQ_TO_MODEM && r.port == 0x182);

    cbq_server_del(&b, 9, 0, 0x182);
    CHECK(cbq_client_find(&b, 0, 9, first.cid) < 0);
    CHECK(cbq_client_find(&b, 1, 9, second.cid) >= 0);
    CHECK(cbq_from_host(&b, 0, f, len, out, sizeof out).kind == CBQ_DROP);
    /* A short reply keeps the revoke pending; emit its real client identity. */
    CHECK(cbq_next_revoke(&b, out, 16).kind == CBQ_DROP);
    r = cbq_next_revoke(&b, out, sizeof out);
    const uint8_t expected[] = { 1, 16, 0, 0x80, 0, 0, 2, 0, 0x24, 0, 5, 0, 1, 2, 0, 9, 1 };
    CHECK(r.kind == CBQ_TO_HOST && r.channel == 0 && r.local_port == first.local_port);
    CHECK(r.len == sizeof expected && !memcmp(out, expected, sizeof expected));
    CHECK(cbq_next_revoke(&b, out, sizeof out).kind == CBQ_DROP);
    CHECK(cbq_next_release(&b) == first.local_port && !cbq_next_release(&b));
    struct cbq_result replacement = allocate(&b, 0, 9);
    CHECK(replacement.cid != first.cid);
    n = message(sdu, 2, 0x1234, 0x20, NULL, 0);
    CHECK(cbq_from_modem(&b, replacement.local_port, 0, 0x182, sdu, n, out, sizeof out).kind == CBQ_DROP);
    CHECK(cbq_from_modem(&b, replacement.local_port, 0, 0x183, sdu, n, out, sizeof out).kind == CBQ_TO_HOST);
    /* Internal clients also lose their endpoint, without a host indication. */
    int slot = cbq_client_alloc(&b, CBQ_INTERNAL, 9);
    CHECK(slot >= 0);
    cbq_server_del(&b, 9, 0, 0x183);
    CHECK(!cbq_client_server(&b, (unsigned)slot));
    unsigned revoked = 0;
    while (cbq_next_revoke(&b, out, sizeof out).kind == CBQ_TO_HOST) revoked++;
    CHECK(revoked == 2);
}

static void framing(void)
{
    struct cbq_bridge b;
    uint8_t sdu[16], f[32], out[32];
    cbq_init(&b);
    CHECK(cbq_server_new(&b, 5, 1, 0, 0x198));
    struct cbq_result c = allocate(&b, 0, 5);
    unsigned n = message(sdu, 2, 0x55aa, 0x20, NULL, 0);
    for (unsigned size = 0; size < 7; size++)
        CHECK(cbq_from_modem(&b, c.local_port, 0, 0x198, sdu, size, out, sizeof out).kind == CBQ_DROP);
    CHECK(cbq_from_modem(&b, c.local_port, 0, 0x198, sdu, n + 1, out, sizeof out).kind == CBQ_DROP);
    sdu[0] = 0;
    CHECK(cbq_from_modem(&b, c.local_port, 0, 0x198, sdu, n, out, sizeof out).kind == CBQ_DROP);
    sdu[0] = 0x06;
    CHECK(cbq_from_modem(&b, c.local_port, 0, 0x198, sdu, n, out, sizeof out).kind == CBQ_DROP);
    sdu[0] = 4;
    memset(out, 0xa5, sizeof out);
    CHECK(cbq_from_modem(&b, c.local_port, 0, 0x198, sdu, n, out, n + 5).kind == CBQ_DROP);
    for (unsigned i = 0; i < sizeof out; i++) CHECK(out[i] == 0xa5);
    sdu[0] = 0;
    unsigned len = host(f, 5, c.cid, sdu, n + 1);
    CHECK(cbq_from_host(&b, 0, f, len, out, sizeof out).kind == CBQ_DROP);
    len = ctl(f, 9, CBQ_CTL_GET_VERSION_INFO, NULL, 0);
    cbq_wr16(f + 1, len);
    f[len] = 0;
    CHECK(cbq_from_host(&b, 0, f, len + 1, out, sizeof out).kind == CBQ_DROP);
}

int main(void)
{
    roundtrip();
    endpoint_lifetime();
    framing();
    if (failures) return 1;
    printf("PASS QMUX endpoint routing: %u checks, VOICE/WMS/IMS two-channel bytes, revoke and framing\n", checks);
    return 0;
}
