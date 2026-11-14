/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../package/baseband27/bridge/NbSim.h"
static uint8_t msg[512], wire[512];
static uint32_t message(unsigned type, unsigned id, unsigned txn, const uint8_t *tlvs, unsigned bytes)
{
    msg[0] = type; cbd_wr16(msg + 1, txn); cbd_wr16(msg + 3, id); cbd_wr16(msg + 5, bytes);
    memcpy(msg + 7, tlvs, bytes); return bytes + 7;
}
static uint32_t status(unsigned reversed, unsigned removed)
{
    uint8_t card[128] = {0}, tlvs[192];
    cbd_wr16(card, reversed ? 0x100 : 0); cbd_wr16(card + 2, 0xffff);
    cbd_wr16(card + 4, removed ? 0xffff : reversed ? 0 : 0x100); cbd_wr16(card + 6, 0xffff);
    card[8] = 2; unsigned at = 9;
    for (unsigned i = 0; i < 2; ++i) {
        card[at] = i == 1 && removed ? 0 : 1; card[at + 1] = 3; card[at + 5] = 1; at += 6;
        card[at] = 2; card[at + 1] = 7; card[at + 6] = 3; at += 7;
        card[at++] = 0xa0; card[at++] = 0; card[at++] = (uint8_t)(i + 1);
        card[at + 1] = 3; card[at + 4] = 3; at += 7;
    }
    unsigned n = cbd_tlv(tlvs, sizeof tlvs, 0x10, card, at);
    return message(4, 0x32, 0, tlvs, n);
}
/* Real UIM Extended Card Status wire: independently counted GW and1x
 * application-index lists, then cards. Payloads contain synthetic AIDs. */
static uint32_t extended_status(unsigned type, unsigned reversed, unsigned removed)
{
    status(reversed, removed);
    unsigned bytes;
    const uint8_t *legacy = cbd_find(msg, 7u + cbd_rd16(msg + 5), 0x10, &bytes);
    assert(legacy && bytes >= 9);
    uint8_t value[160], tlvs[192];
    value[0] = 2;   /* two GW subscriptions */
    memcpy(value + 1, legacy, 2);
    memcpy(value + 3, legacy + 4, 2);
    value[5] = 0;   /* no 1x provisioning sessions */
    memcpy(value + 6, legacy + 8, bytes - 8);
    unsigned n = 0;
    if (type == 2) {
        const uint8_t success[4] = {0};
        n = cbd_tlv(tlvs, sizeof tlvs, 2, success, sizeof success);
    }
    n += cbd_tlv(tlvs + n, sizeof tlvs - n, type == 2 ? 0x13 : 0x12,
                 value, bytes - 2);
    return message(type, type == 2 ? 0x2f : 0x32, type == 2 ? 11 : 0, tlvs, n);
}
static uint32_t request(unsigned session, unsigned file, unsigned txn)
{
    uint8_t tlvs[64], sess[2] = { (uint8_t)session, 0 }, f[5] = {0};
    cbd_wr16(f, file); f[2] = 2; f[3] = 0; f[4] = 0x3f;
    unsigned n = cbd_tlv(tlvs, sizeof tlvs, 1, sess, 2);
    n += cbd_tlv(tlvs + n, sizeof tlvs - n, 2, f, sizeof f);
    return message(0, 0x20, txn, tlvs, n);
}
static uint32_t response(unsigned txn, const uint8_t *ef, unsigned bytes, unsigned error)
{
    uint8_t tlvs[64], content[32];unsigned n = 7;
    tlvs[0] = 2; cbd_wr16(tlvs + 1, 4); cbd_wr16(tlvs + 3, error ? 1 : 0); cbd_wr16(tlvs + 5, error);
    cbd_wr16(content, bytes);memcpy(content + 2, ef, bytes);
    n += cbd_tlv(tlvs + n, sizeof tlvs - n, 0x11, content, bytes + 2);
    return message(2, 0x20, txn, tlvs, n);
}
static int observe(struct nb_sim *s, unsigned local, unsigned session, unsigned file, unsigned txn,
                   const uint8_t *ef, unsigned bytes)
{
    unsigned n = request(session, file, txn);
    assert(nb_sim_read_begin(s, local, 0, 212, msg, n));
    n = response(txn, ef, bytes, 0);
    return nb_sim_message(s, local, 0, 212, msg, n);
}
static const uint8_t iccid1[10] = {0x98,0x10,0x32,0x54,0x76,0x98,0x10,0x32,0x54,0x76};
static const uint8_t iccid2[10] = {0x98,0x21,0x43,0x65,0x87,0x09,0x21,0x43,0x65,0x87};
static const uint8_t imsi1[9] = {8,0x09,0x10,0x10,0x10,0x32,0x54,0x76,0x98};
static const uint8_t imsi2[9] = {8,0x09,0x20,0x20,0x20,0x43,0x65,0x87,0x09};
static void check_activation(struct nb_sim *s, unsigned second, unsigned reversed)
{
    uint8_t tlvs[NB_SIM_ACTIVATION_BYTES], want[10];unsigned bytes;
    unsigned n = nb_sim_activation(s, tlvs, sizeof tlvs);
    assert(n == CBD_ACTIVATION_SIM_TLVS + (second ? 28u : 0u));
    n = message(4, 0xb00c, 0, tlvs, n);
    const uint8_t *a = cbd_find(msg, n, 0x42, &bytes);
    assert(a && bytes == 10);
    assert(cbd_iccid_from_ef(reversed ? iccid2 : iccid1, 10, want) && !memcmp(a, want, 10));
    const uint8_t *b = cbd_find(msg, n, 0x46, &bytes);
    assert((b != NULL) == (second != 0));
    if (b) assert(bytes == 10 && cbd_iccid_from_ef(reversed ? iccid1 : iccid2, 10, want) && !memcmp(b, want, 10));
    assert(cbd_find(msg, n, 0x43, &bytes) && bytes == 8);
    assert((cbd_find(msg, n, 0x47, &bytes) != NULL) == (second != 0));
    assert((cbd_find(msg, n, 0x51, &bytes) != NULL) == (second != 0));
    assert(!nb_sim_activation(s, wire, n - 8));
}
static void extended_identity(void)
{
    struct nb_sim s;
    nb_sim_reset(&s);
    unsigned n = extended_status(2, 0, 0);
    assert(nb_sim_message(&s, 0x4100, 0, 212, msg, n));
    assert(s.mapping_known && nb_sim_physical(&s, 0) == 1 && nb_sim_physical(&s, 2) == 2);
    assert(!nb_sim_physical(&s, 1) && !nb_sim_physical(&s, 3));
    assert(observe(&s, 0x4100, 0, CBD_EF_ICCID, 12, iccid1, 10));
    assert(observe(&s, 0x4100, 2, CBD_EF_ICCID, 13, iccid2, 10));
    assert(observe(&s, 0x4100, 0, CBD_EF_IMSI, 14, imsi1, 9));
    assert(observe(&s, 0x4100, 2, CBD_EF_IMSI, 15, imsi2, 9));
    check_activation(&s, 1, 0);
    n = extended_status(4, 1, 0);
    assert(nb_sim_message(&s, 0x4100, 0, 212, msg, n));
    check_activation(&s, 1, 1);
    n = extended_status(4, 0, 1);
    assert(nb_sim_message(&s, 0x4100, 0, 212, msg, n));
    check_activation(&s, 0, 0);
    n = extended_status(4, 0, 1);
    memcpy(wire, msg, n);
    uint8_t present[2], provision[4];
    for (unsigned cut = 0; cut < n; ++cut)
        assert(nb_sim_status(wire, cut, present, provision) <= 0);
    unsigned bytes;
    const uint8_t *value = cbd_find(wire, n, 0x12, &bytes);
    assert(value && bytes > 6);
    wire[value - wire] = 6;   /* count exceeds the real UIM max5 */
    assert(nb_sim_status(wire, n, present, provision) == -1);
    n = extended_status(2, 0, 0);
    memcpy(wire, msg, n);
    value = cbd_find(wire, n, 0x13, &bytes);
    wire[value - wire + 6] = 5;   /* advertised cards exceed the supplied records */
    assert(nb_sim_status(wire, n, present, provision) == -1);
}
int main(void)
{
    extended_identity();
    struct nb_sim s;nb_sim_reset(&s);
    assert(!nb_sim_physical(&s, 0) && nb_sim_physical(&s, 6) == 1 && nb_sim_physical(&s, 7) == 2);
    unsigned n = request(0, CBD_EF_ICCID, 1);assert(nb_sim_read_begin(&s, 0x4100, 0, 212, msg, n));
    n = response(1, iccid1, 10, 0);assert(!nb_sim_message(&s, 0x4100, 0, 212, msg, n));
    n = status(0, 0);assert(nb_sim_message(&s, 0x4100, 0, 212, msg, n) && s.generation == 1);
    assert(nb_sim_physical(&s, 0) == 1 && nb_sim_physical(&s, 2) == 2 && !nb_sim_physical(&s, 1));
    assert(observe(&s, 0x4100, 6, CBD_EF_ICCID, 2, iccid1, 10));
    assert(observe(&s, 0x4101, 7, CBD_EF_ICCID, 2, iccid2, 10));
    assert(observe(&s, 0x4100, 0, CBD_EF_IMSI, 3, imsi1, 9));
    assert(observe(&s, 0x4101, 2, CBD_EF_IMSI, 3, imsi2, 9));
    assert(!observe(&s, 0x4100, 0, CBD_EF_IMSI, 4, imsi1, 9));
    check_activation(&s, 1, 0);
    /* Same transaction on different clients: neither CID nor arrival order
     * may assign card2 bytes to card1. Responses also stay on the same endpoint. */
    n = request(6, CBD_EF_ICCID, 5);assert(nb_sim_read_begin(&s, 0x4100, 0, 212, msg, n));
    n = response(5, iccid2, 10, 0);assert(!nb_sim_message(&s, 0x4100, 1, 212, msg, n));
    assert(!nb_sim_message(&s, 0x4100, 0, 213, msg, n));
    n = response(5, iccid1, 10, 1);assert(!nb_sim_message(&s, 0x4100, 0, 212, msg, n));
    check_activation(&s, 1, 0);
    /* Rebinding changes logical subscription ordering, retaining physical
     * identities while invalidating requests made against the prior map. */
    n = request(0, CBD_EF_ICCID, 6);assert(nb_sim_read_begin(&s, 0x4100, 0, 212, msg, n));
    n = status(1, 0);assert(nb_sim_message(&s, 0x4100, 0, 212, msg, n) && s.generation == 2);
    n = response(6, iccid2, 10, 0);assert(!nb_sim_message(&s, 0x4100, 0, 212, msg, n));
    check_activation(&s, 1, 1);
    n = status(0, 1);assert(nb_sim_message(&s, 0x4100, 0, 212, msg, n));
    assert(!s.card[1].iccid_known && !s.card[1].imsi_known);
    check_activation(&s, 0, 0);
    n = status(0, 1);memcpy(wire, msg, n);
    for (unsigned cut = 0; cut < n; ++cut) assert(!nb_sim_message(&s, 0x4100, 0, 212, wire, cut));
    wire[16] = 6;assert(!nb_sim_message(&s, 0x4100, 0, 212, wire, n)); /* too many physical cards */
    n = request(10, CBD_EF_ICCID, 7);assert(!nb_sim_read_begin(&s, 0x4100, 0, 212, msg, n));
    n = request(0, CBD_EF_IMSI, 8);assert(nb_sim_read_begin(&s, 0x4100, 0, 212, msg, n));
    assert(observe(&s, 0x4100, 6, CBD_EF_ICCID, 9, iccid2, 10));
    assert(s.card[0].iccid_known && !s.card[0].imsi_known);
    n = response(8, imsi1, 9, 0);assert(!nb_sim_message(&s, 0x4100, 0, 212, msg, n) && !s.card[0].imsi_known);
    uint8_t tlvs[NB_SIM_ACTIVATION_BYTES];n = nb_sim_activation(&s, tlvs, sizeof tlvs);
    assert(n == CBD_ACTIVATION_TLVS + 13);n = message(4, 0xb00c, 0, tlvs, n);
    unsigned bytes;assert(!cbd_find(msg, n, 0x43, &bytes));
    assert(!nb_sim_endpoint_lost(&s, 0, 213) && s.mapping_known);
    assert(nb_sim_endpoint_lost(&s, 0, 212) && !s.mapping_known && !s.card[0].iccid_known);
    puts("PASS dual SIM identity: real UIM sessions/status, two native activation records, swapped provisioning, removal, endpoint/txn bounds");
}
