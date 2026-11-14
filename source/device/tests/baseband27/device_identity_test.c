/* SPDX-License-Identifier: BSD-2-Clause */
/* Synthetic identities and QMI frames; no phone or RF operation. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../package/baseband27/bridge/NbDeviceIdentity.h"

static uint8_t wire[512], activation[64];
static uint32_t message(unsigned type, unsigned command, unsigned transaction,
                        const uint8_t *body, unsigned bytes)
{
    memset(wire, 0, sizeof wire);
    wire[0] = (uint8_t)type;
    cbd_wr16(wire + 1, transaction); cbd_wr16(wire + 3, command);
    cbd_wr16(wire + 5, bytes);
    if (bytes) memcpy(wire + 7, body, bytes);
    return 7 + bytes;
}
static void request(struct nb_device_identity *s, unsigned slot, unsigned cid,
                    unsigned command, unsigned transaction, unsigned subscription)
{
    uint8_t body[7] = {1, 4, 0};
    cbd_wr32(body + 3, subscription);
    unsigned n = message(0, command, transaction,
                         command == NB_DEVICE_BIND ? body : NULL,
                         command == NB_DEVICE_BIND ? sizeof body : 0);
    nb_device_request(s, slot, 0, cid, 0, 212, wire, n);
}
static uint32_t response(unsigned command, unsigned transaction, unsigned error,
                         const char *imei, unsigned bytes)
{
    uint8_t body[160] = {2, 4, 0};
    cbd_wr16(body + 3, error ? 1 : 0); cbd_wr16(body + 5, error);
    unsigned n = 7;
    if (imei) n += cbd_tlv(body + n, sizeof body - n, 0x11, (const uint8_t *)imei, bytes);
    return message(2, command, transaction, body, n);
}
static void bind(struct nb_device_identity *s, unsigned slot, unsigned cid,
                 unsigned subscription, unsigned transaction)
{
    request(s, slot, cid, NB_DEVICE_BIND, transaction, subscription);
    unsigned n = response(NB_DEVICE_BIND, transaction, 0, NULL, 0);
    assert(!nb_device_answer(s, slot, 0, cid, 0, 212, wire, n));
    assert(s->client[slot].bound && s->client[slot].subscription == subscription - 1);
}
/* Expected result of the actual native ActivationCommandDriver::parseImei. */
static void native_imei(const uint8_t in[8], char out[16])
{
    for (unsigned i = 0; i < 8; ++i) {
        out[2 * i] = (char)('0' + (in[i] >> 4));
        if (i < 7) out[2 * i + 1] = (char)('0' + (in[i + 1] & 15));
    }
    out[15] = 0;
}
int main(void)
{
    static const char first[] = "123456789012345";
    static const char second[] = "987654321098765";
    struct nb_device_identity s;
    nb_device_reset(&s);
    unsigned n;
    /* An unbound response must not take primary as an invented default. */
    request(&s, 0, 1, NB_DEVICE_GET_IDS, 1, 0);
    n = response(NB_DEVICE_GET_IDS, 1, 0, first, 15);
    assert(!nb_device_answer(&s, 0, 0, 1, 0, 212, wire, n) && !s.imei[0].known);
    /* Both subscriptions use the same QMUX channel, CID/transaction matters. */
    bind(&s, 0, 1, 1, 2); bind(&s, 1, 2, 2, 2);
    request(&s, 0, 1, NB_DEVICE_GET_IDS, 3, 0);
    request(&s, 1, 2, NB_DEVICE_GET_IDS, 3, 0);
    n = response(NB_DEVICE_GET_IDS, 3, 0, second, 15);
    assert(nb_device_answer(&s, 1, 0, 2, 0, 212, wire, n));
    n = response(NB_DEVICE_GET_IDS, 3, 0, first, 15);
    assert(nb_device_answer(&s, 0, 0, 1, 0, 212, wire, n));
    n = nb_device_activation(&s, activation, sizeof activation);
    assert(n == 22 && activation[0] == 0x12 && activation[11] == 0x55);
    assert(!nb_device_activation(&s, activation, 21));
    char rendered[16];
    native_imei(s.imei[0].value, rendered); assert(!strcmp(rendered, first));
    native_imei(s.imei[1].value, rendered); assert(!strcmp(rendered, second));
    request(&s, 0, 1, NB_DEVICE_GET_IDS, 4, 0);
    n = response(NB_DEVICE_GET_IDS, 4, 0, second, 15);
    assert(!nb_device_answer(&s, 0, 0, 1, 1, 212, wire, n));
    assert(!nb_device_answer(&s, 0, 0, 1, 0, 213, wire, n));
    assert(nb_device_answer(&s, 0, 0, 1, 0, 212, wire, n));
    /* Error, duplicate fields, short or non-decimal values cannot publish. */
    const char *invalid[] = {"000000000000000", "12345678901234", "1234567890123456",
                             "12345678901234x"};
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        request(&s, 0, 1, NB_DEVICE_GET_IDS, 10 + i, 0);
        n = response(NB_DEVICE_GET_IDS, 10 + i, 0, invalid[i], (unsigned)strlen(invalid[i]));
        assert(!nb_device_answer(&s, 0, 0, 1, 0, 212, wire, n));
    }
    request(&s, 0, 1, NB_DEVICE_GET_IDS, 20, 0);
    n = response(NB_DEVICE_GET_IDS, 20, 3, first, 15);
    assert(!nb_device_answer(&s, 0, 0, 1, 0, 212, wire, n));
    request(&s, 0, 1, NB_DEVICE_GET_IDS, 21, 0);
    n = response(NB_DEVICE_GET_IDS, 21, 0, first, 15);
    uint8_t frame[512]; memcpy(frame, wire, n);
    for (unsigned cut = 0; cut < n; ++cut)
        assert(!nb_device_answer(&s, 0, 0, 1, 0, 212, frame, cut));
    memcpy(wire + n, wire + 14, 18); n += 18; cbd_wr16(wire + 5, n - 7);
    assert(!nb_device_answer(&s, 0, 0, 1, 0, 212, wire, n));
    /* A released endpoint loses pending reads and cached identity. */
    assert(nb_device_endpoint_lost(&s, 0, 212));
    assert(!s.imei[0].known && !s.imei[1].known);
    n = response(NB_DEVICE_GET_IDS, 21, 0, first, 15);
    assert(!nb_device_answer(&s, 0, 0, 1, 0, 212, wire, n));
    bind(&s, 0, 3, 1, 30);
    request(&s, 0, 3, NB_DEVICE_GET_IDS, 31, 0);
    nb_device_retire(&s, 0);
    bind(&s, 0, 4, 1, 32);
    n = response(NB_DEVICE_GET_IDS, 31, 0, first, 15);
    assert(!nb_device_answer(&s, 0, 0, 4, 0, 212, wire, n));
    /* A query crossing binding is invalidated; a failed bind retains prior slot. */
    request(&s, 0, 4, NB_DEVICE_GET_IDS, 33, 0);
    request(&s, 0, 4, NB_DEVICE_BIND, 34, 2);
    n = response(NB_DEVICE_BIND, 34, 3, NULL, 0);
    assert(!nb_device_answer(&s, 0, 0, 4, 0, 212, wire, n));
    assert(s.client[0].subscription == 0);
    n = response(NB_DEVICE_GET_IDS, 33, 0, second, 15);
    assert(!nb_device_answer(&s, 0, 0, 4, 0, 212, wire, n));
    /* The stock tertiary binding must not leave a stale primary owner. */
    request(&s, 0, 4, NB_DEVICE_BIND, 35, 3);
    n = response(NB_DEVICE_BIND, 35, 0, NULL, 0);
    assert(!nb_device_answer(&s, 0, 0, 4, 0, 212, wire, n));
    assert(!s.client[0].bound);
    request(&s, 0, 4, NB_DEVICE_GET_IDS, 36, 0);
    n = response(NB_DEVICE_GET_IDS, 36, 0, first, 15);
    assert(!nb_device_answer(&s, 0, 0, 4, 0, 212, wire, n));
    puts("device identity: bound real response, dual subscription, native packed IMEI, errors/endpoints/lifetime pass");
    return 0;
}
