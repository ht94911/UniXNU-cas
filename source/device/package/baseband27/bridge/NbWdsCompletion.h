/* SPDX-License-Identifier: BSD-2-Clause */
/* Apple's Start reply handler waits for a PacketServiceStatus indication for
 * 3GPP failures. This modem reports an IPv6-only rejection only in the reply.
 * Project that actual failed IPv4 request to a disconnected completion; keep
 * the original reply, reasons and other family untouched. */
#ifndef NB_WDS_COMPLETION_H
#define NB_WDS_COMPLETION_H
#include "coachz_bb_pdp.h"

static inline uint32_t nb_wds_failure_completion(const struct cbp_client *client,
    const uint8_t *sdu, uint32_t size, uint8_t *out, uint32_t room)
{
    if (!client || !cbd_sdu_ok(sdu, size) || sdu[0] != 2u ||
        cbd_rd16(sdu + 3) != CBP_START || !client->start_pending ||
        cbd_rd16(sdu + 1) != client->start_txn || client->active ||
        !cbp_mux_ok(client->mux) || client->family != 4u ||
        client->bind_pending || client->family_pending) return 0;
    const uint8_t *result = 0, *reason = 0, *verbose = 0;
    for (uint32_t at = 7; at < size;) {
        if (size - at < 3u) return 0;
        const uint32_t length = cbd_rd16(sdu + at + 1);
        if (length > size - at - 3u) return 0;
        const uint8_t *value = sdu + at + 3;
        switch (sdu[at]) {
        case 2:
            if (result || length != 4u) return 0;
            result = value; break;
        case 0x10:
            if (reason || length != 2u) return 0;
            reason = value; break;
        case 0x11:
            if (verbose || length != 4u) return 0;
            verbose = value; break;
        default: break;
        }
        at += 3u + length;
    }
    /* QMI_ERR_CALL_FAILED; 3GPP cause51 is IPv6 only allowed. A failed
     * response never produces a connected state, handle, address or success. */
    if (!result || cbd_rd16(result) != 1u || cbd_rd16(result + 2) != 14u ||
        !reason || !verbose || cbd_rd16(verbose) != 6u ||
        cbd_rd16(verbose + 2) != 51u || room < 21u) return 0;
    const uint8_t disconnected[2] = {1, 0}, family = 4;
    uint32_t bytes = cbd_tlv(out, room, 1, disconnected, sizeof disconnected);
    bytes += cbd_tlv(out + bytes, room - bytes, 0x10, reason, 2);
    bytes += cbd_tlv(out + bytes, room - bytes, 0x11, verbose, 4);
    bytes += cbd_tlv(out + bytes, room - bytes, 0x12, &family, 1);
    return bytes;
}
#endif
