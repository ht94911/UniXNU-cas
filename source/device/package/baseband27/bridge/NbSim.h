/* SPDX-License-Identifier: BSD-2-Clause */
/* Passive identity collection from CommCenter's UIM requests. The real card
 * status maps provisioning sessions to physical cards; no slot is switched
 * and no additional card request is sent. See SOURCE.md for wire sources. */
#ifndef NB_SIM_H
#define NB_SIM_H
#include "coachz_bb_data.h"

#define NB_SIM_READS 8u
#define NB_SIM_ACTIVATION_BYTES (CBD_ACTIVATION_TLVS + 4u + 2u * (3u + CBD_ICCID_BYTES + 3u + CBD_IMSI_BYTES))
struct nb_sim_card {
    uint8_t present, iccid_known, imsi_known;
    uint8_t iccid[CBD_ICCID_BYTES], imsi[CBD_IMSI_BYTES];
};
struct nb_sim_read {
    uint8_t used, session;
    uint16_t txn, file;
    uint32_t local_port, generation, node, port;
};
struct nb_sim {
    struct nb_sim_card card[2];
    struct nb_sim_read read[NB_SIM_READS];
    uint8_t mapping_known, owner_known, provision[4];
    uint32_t generation, next, node, port;
};

static inline void nb_sim_reset(struct nb_sim *s) { __builtin_memset(s, 0, sizeof *s); }

/* Primary/secondary GW and 1x provisioning indices identify a physical card
 * and application. Card/nonprovisioning/channel sessions explicitly name a
 * physical slot. Unsupported sessions do not acquire another slot's identity. */
static inline unsigned nb_sim_physical(const struct nb_sim *s, unsigned session)
{
    if (session < 4) return s->mapping_known ? s->provision[session] : 0;
    if (session <= 9) return 1u + (session & 1u);
    return 0;
}
static inline unsigned nb_sim_subscription(const struct nb_sim *s, unsigned slot)
{
    if (!s->mapping_known || slot > 1) return 0;
    unsigned at = slot * 2;
    return s->provision[at] ? s->provision[at] : s->provision[at + 1];
}

/* Parse legacy Card Status (0x10) or Extended Card Status (response0x13,
 * indication0x12). The latter has counted GW and 1x index arrays, followed
 * by a counted card array. Both use the same card/application wire records.
 * Sources: libqmi UIM Card Status, Trogdor cbl_card_bytes, and the phone's
 * libqmiservices UIM type35/type41 descriptors. Return1 for a valid status,
 * 0 for a different message, -1 for malformed input. */
static inline int nb_sim_status(const uint8_t *sdu, uint32_t n, uint8_t present[2], uint8_t provision[4])
{
    if (!cbd_sdu_ok(sdu, n) || 7u + cbd_rd16(sdu + 5) != n) return -1;
    unsigned msg = cbd_rd16(sdu + 3);
    if (!((sdu[0] == 2 && msg == 0x2f && !cbd_error(sdu, n)) || (sdu[0] == 4 && msg == 0x32))) return 0;
    uint32_t bytes = 0;
    const uint8_t *v = cbd_find(sdu, n, sdu[0] == 2 ? 0x13 : 0x12, &bytes);
    uint16_t indices[4] = {0xffff, 0xffff, 0xffff, 0xffff};
    uint32_t at = 0;
    if (v) {
        /* Extended arrays are ordered by subscription, separately for GW
         * and1x. Keep the first two subscriptions and validate every entry. */
        for (unsigned family = 0; family < 2; ++family) {
            if (at >= bytes) return -1;
            unsigned count = v[at++];
            if (count > 5 || bytes - at < 2u * count) return -1;
            for (unsigned i = 0; i < count && i < 2; ++i)
                indices[2u * i + family] = cbd_rd16(v + at + 2u * i);
            at += 2u * count;
        }
    } else {
        v = cbd_find(sdu, n, 0x10, &bytes);
        if (!v || bytes < 9) return -1;
        for (unsigned i = 0; i < 4; ++i) indices[i] = cbd_rd16(v + 2u * i);
        at = 8;
    }
    if (at >= bytes) return -1;
    unsigned count = v[at++];
    if (count > 5) return -1;
    uint8_t cards[5] = {0}, apps[5] = {0};
    for (unsigned i = 0; i < count; ++i) {
        if (bytes - at < 6) return -1;
        cards[i] = v[at] == 1; apps[i] = v[at + 5]; at += 6;
        for (unsigned j = 0; j < apps[i]; ++j) {
            if (bytes - at < 7) return -1;
            unsigned aid = v[at + 6]; at += 7;
            if (bytes - at < aid + 7u) return -1;
            at += aid + 7u;
        }
    }
    if (at != bytes) return -1;
    present[0] = cards[0]; present[1] = cards[1];
    for (unsigned i = 0; i < 4; ++i) {
        unsigned index = indices[i], card = index >> 8, app = index & 255;
        provision[i] = index != 0xffff && card < 2 && cards[card] && app < apps[card] ? (uint8_t)(card + 1) : 0;
    }
    return 1;
}

static inline int nb_sim_read_begin(struct nb_sim *s, uint32_t local_port, uint32_t node, uint32_t port,
                                   const uint8_t *sdu, uint32_t n)
{
    if (!cbd_sdu_ok(sdu, n) || 7u + cbd_rd16(sdu + 5) != n) return 0;
    unsigned file = cbd_uim_read_file(sdu, n);
    if (file != CBD_EF_ICCID && file != CBD_EF_IMSI) return 0;
    uint32_t bytes = 0;
    const uint8_t *session = cbd_find(sdu, n, 1, &bytes);
    if (!session || bytes < 2 || bytes != 2u + session[1] || session[0] > 9 ||
        (s->owner_known && (s->node != node || s->port != port))) return 0;
    struct nb_sim_read *r = &s->read[s->next++ % NB_SIM_READS];
    *r = (struct nb_sim_read){1, session[0], (uint16_t)cbd_rd16(sdu + 1), (uint16_t)file,
                             local_port, s->generation, node, port};
    return 1;
}

/* Consume only a frame already accepted by the endpoint-bound QMUX router.
 * Return 1 when activation identity changed, including a removed card.
 * Failures and unrelated reads leave the collected identities unchanged. */
static inline int nb_sim_message(struct nb_sim *s, uint32_t local_port, uint32_t node, uint32_t port,
                                const uint8_t *sdu, uint32_t n)
{
    uint8_t present[2], provision[4];
    int status = nb_sim_status(sdu, n, present, provision);
    if (status < 0) return 0;
    if (status) {
        if (s->owner_known && (s->node != node || s->port != port)) return 0;
        unsigned changed = !s->mapping_known;
        for (unsigned i = 0; i < 4; ++i) changed |= s->provision[i] != provision[i];
        for (unsigned i = 0; i < 2; ++i) changed |= s->card[i].present != present[i];
        s->owner_known = s->mapping_known = 1; s->node = node; s->port = port;
        if (changed) {
            s->generation++;
            __builtin_memset(s->read, 0, sizeof s->read);
            for (unsigned i = 0; i < 2; ++i) {
                if (!present[i]) __builtin_memset(&s->card[i], 0, sizeof s->card[i]);
                s->card[i].present = present[i];
            }
            __builtin_memcpy(s->provision, provision, 4);
        }
        return changed != 0;
    }
    if (!cbd_sdu_ok(sdu, n) || 7u + cbd_rd16(sdu + 5) != n ||
        sdu[0] != 2 || cbd_rd16(sdu + 3) != CBD_UIM_READ_TRANSPARENT) return 0;
    for (unsigned i = 0; i < NB_SIM_READS; ++i) {
        struct nb_sim_read *r = &s->read[i];
        if (!r->used || r->local_port != local_port || r->txn != cbd_rd16(sdu + 1) ||
            r->node != node || r->port != port) continue;
        r->used = 0;
        unsigned physical = nb_sim_physical(s, r->session);
        if (!s->owner_known || s->node != node || s->port != port || r->generation != s->generation ||
            !physical || !s->card[physical - 1].present) return 0;
        uint8_t ef[16], value[CBD_ICCID_BYTES];
        unsigned bytes = cbd_uim_read_content(sdu, n, ef, sizeof ef);
        unsigned iccid = r->file == CBD_EF_ICCID;
        if (iccid ? !cbd_iccid_from_ef(ef, bytes, value) : !cbd_imsi_from_ef(ef, bytes, value)) return 0;
        struct nb_sim_card *card = &s->card[physical - 1];
        uint8_t *stored = iccid ? card->iccid : card->imsi;
        uint8_t *known = iccid ? &card->iccid_known : &card->imsi_known;
        unsigned size = iccid ? CBD_ICCID_BYTES : CBD_IMSI_BYTES;
        if (*known && !__builtin_memcmp(stored, value, size)) return 0;
        // A replacement card cannot temporarily publish the previous card's
        // IMSI with its new ICCID while CommCenter's new IMSI read is pending.
        if (iccid && *known) {
            card->imsi_known = 0;
            __builtin_memset(card->imsi, 0, sizeof card->imsi);
            for (unsigned j = 0; j < NB_SIM_READS; ++j) {
                if (s->read[j].used && nb_sim_physical(s, s->read[j].session) == physical)
                    s->read[j].used = 0;
            }
        }
        __builtin_memcpy(stored, value, size); *known = 1;
        return 1;
    }
    return 0;
}

static inline int nb_sim_endpoint_lost(struct nb_sim *s, uint32_t node, uint32_t port)
{
    if (!s->owner_known || s->node != node || s->port != port) return 0;
    nb_sim_reset(s); return 1;
}

/* Retain the existing activation/no-SIM-lock contract. Populate slot1/slot2
 * ICCID/IMSI only from their mapped, present physical cards. The original
 * iOS27 parsers accept TLV42/43 and46/47, respectively. */
static inline uint32_t nb_sim_activation(const struct nb_sim *s, uint8_t *tlvs, uint32_t room)
{
    unsigned first = nb_sim_subscription(s, 0), second = nb_sim_subscription(s, 1);
    const struct nb_sim_card *a = first ? &s->card[first - 1] : 0, *b = second ? &s->card[second - 1] : 0;
    unsigned need = CBD_ACTIVATION_TLVS;
    if (a && a->iccid_known) need += 3 + CBD_ICCID_BYTES + (a->imsi_known ? 3 + CBD_IMSI_BYTES : 0);
    if (b && b->iccid_known) need += 4 + 3 + CBD_ICCID_BYTES + (b->imsi_known ? 3 + CBD_IMSI_BYTES : 0);
    if (room < need) return 0;
    uint32_t n = cbd_activation_state(tlvs, room, a && a->iccid_known ? a->iccid : 0,
                                     a && a->imsi_known ? a->imsi : 0);
    if (b && b->iccid_known) {
        const uint8_t provisioned = CBD_PROVISIONED;
        n += cbd_tlv(tlvs + n, room - n, 0x51, &provisioned, 1);
        n += cbd_tlv(tlvs + n, room - n, 0x46, b->iccid, CBD_ICCID_BYTES);
        if (b->imsi_known) n += cbd_tlv(tlvs + n, room - n, 0x47, b->imsi, CBD_IMSI_BYTES);
    }
    return n;
}
#endif
