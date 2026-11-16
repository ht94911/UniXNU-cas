/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * nezha (SM8850, IPA v5.5) version of QcomBaseband ipa/coachz_ipa_modem_proto.h (dd0199c, BSD-2-Clause, HoshinoYumeka),
 * derived by tools/baseband27/derive-ipa55.py.
 *
 * Portable decisions of the cellular data path, as mainline Linux runs it: ipa_modem.c (rmnet_ipa0: start/stop
 * states, the device's parameters, the TX path of ipa_start_xmit/ipa_endpoint_skb_tx) and the rmnet QMAP layer above it
 * (drivers/net/ethernet/qualcomm/rmnet: rmnet_map_egress_handler, rmnet_map_add_map_header, rmnet_map_validate_packet_len,
 * rmnet_map_deaggregate, __rmnet_map_ingress_handler, rmnet_vnd headroom and MTU). Linux 7.4-rc1 (6628e7d4c6b2); the code
 * is our own.
 *
 * Data format: the IPA endpoints fix it. The first phase runs AP_MODEM_TX and AP_MODEM_RX without checksum offload, so a
 * frame is the 4-byte QMAP header and the IP packet in both directions (rmnet with only INGRESS_DEAGGREGATION) and the
 * network stack computes and verifies checksums. WDA must negotiate the same format with the modem. The v5 inline checksum
 * header (8 bytes, both directions) is a later step.
 */
#ifndef NB_IPA55_MODEM_PROTO_H
#define NB_IPA55_MODEM_PROTO_H

#include <stdint.h>
#include "nb_ipa55_rx_proto.h"

/* ipa_modem.c */
#define CIM_STOPPED  0u
#define CIM_STARTING 1u
#define CIM_RUNNING  2u
#define CIM_STOPPING 3u
#define CIM_IPA_MTU 1500u                 /* rmnet_ipa0: max_mtu = mtu = IPA_MTU, raw IP, no link header */

/* if_link.h RMNET_FLAGS_* */
#define CIM_INGRESS_DEAGGREGATION 0x01u
#define CIM_INGRESS_MAP_COMMANDS  0x02u
#define CIM_INGRESS_MAP_CKSUMV4   0x04u
#define CIM_EGRESS_MAP_CKSUMV4    0x08u
#define CIM_DATA_FORMAT CIM_INGRESS_DEAGGREGATION   /* no checksum offload in the first phase */
#define CIM_MAX_LOGICAL_EP 255u           /* RMNET_MAX_LOGICAL_EP: mux ids 0..254 */
#define CIM_MAP_HEADER 4u
#define CIM_UL_CSUM_HEADER 4u
#define CIM_DL_CSUM_TRAILER 8u
#define CIM_MAP_PAD_LEN_MASK 0x3fu
#define CIM_MAP_CMD_FLAG 0x80u

/* rmnet_vnd_headroom and the MTU rmnet gives its data device (real device MTU minus headroom) */
static inline uint32_t cim_headroom(uint32_t data_format)
{
    return CIM_MAP_HEADER + (data_format & CIM_EGRESS_MAP_CKSUMV4 ? CIM_UL_CSUM_HEADER : 0u);
}
static inline uint32_t cim_data_mtu(uint32_t data_format) { return CIM_IPA_MTU - cim_headroom(data_format); }

/* The modem endpoints must match the format: QMAP on both, checksum offload as the format says, no status on the RX side */
static inline int cim_format_matches_endpoints(uint32_t data_format)
{
    const struct cipa_endpoint *tx = &cipa_endpoints[CIPA_EP_AP_MODEM_TX], *rx = &cipa_endpoints[CIPA_EP_AP_MODEM_RX];
    return tx->qmap && rx->qmap && !rx->status_enable && rx->aggregation &&
           !!tx->checksum == !!(data_format & CIM_EGRESS_MAP_CKSUMV4) && !!rx->checksum == !!(data_format & CIM_INGRESS_MAP_CKSUMV4) &&
           (data_format & CIM_INGRESS_DEAGGREGATION);
}

/* ------------------------------------------------------------------ egress: rmnet_map_egress_handler without TX aggregation */
/* Writes [QMAP header][UL checksum header if v4][IP packet] to out; returns the frame length or 0 if it does not fit.
   rmnet_map_add_map_header with RMNET_MAP_NO_PAD_BYTES: flags 0, mux_id, pkt_len = bytes after the QMAP header minus the
   checksum header (the IP packet), big endian. */
static inline uint32_t cim_egress(uint8_t *out, uint32_t room, const uint8_t *ip, uint32_t ip_len, uint32_t mux_id, uint32_t data_format)
{
    const uint32_t extra = data_format & CIM_EGRESS_MAP_CKSUMV4 ? CIM_UL_CSUM_HEADER : 0u;
    const uint32_t total = CIM_MAP_HEADER + extra + ip_len;
    if (!ip_len || ip_len > 0xffffu || mux_id >= CIM_MAX_LOGICAL_EP || total > room) return 0;
    out[0] = 0;
    out[1] = (uint8_t)mux_id;
    out[2] = (uint8_t)(ip_len >> 8);
    out[3] = (uint8_t)ip_len;
    for (uint32_t i = 0; i < extra; i++) out[CIM_MAP_HEADER + i] = 0;   /* UL checksum header: software checksum */
    for (uint32_t i = 0; i < ip_len; i++) out[CIM_MAP_HEADER + extra + i] = ip[i];
    return total;
}
/* ipa_start_xmit drops empty frames and, with QMAP, anything that is not ETH_P_MAP; the frame must fit one TX buffer */
static inline int cim_tx_acceptable(uint32_t frame_len, uint32_t buffer) { return frame_len && frame_len <= buffer; }

/* ------------------------------------------------------------------ ingress: rmnet_map_ingress_handler with deaggregation */
struct cim_packet {
    uint32_t consumed;   /* rmnet_map_validate_packet_len: 0 ends the buffer */
    int command;         /* MAP command: dropped unless INGRESS_MAP_COMMANDS */
    uint32_t mux_id, ip_offset, ip_len, ip_version;
};
static inline struct cim_packet cim_ingress_next(const uint8_t *p, uint32_t resid, uint32_t data_format)
{
    struct cim_packet r = { 0, 0, 0, 0, 0, 0 };
    if (resid < CIM_MAP_HEADER) return r;
    const uint32_t pkt_len = (uint32_t)p[2] << 8 | p[3];
    if (!pkt_len) return r;
    uint32_t len = pkt_len + CIM_MAP_HEADER;
    if (data_format & CIM_INGRESS_MAP_CKSUMV4) len += CIM_DL_CSUM_TRAILER;
    if (resid < len) return r;
    r.consumed = len;
    r.command = !!(p[0] & CIM_MAP_CMD_FLAG);
    r.mux_id = p[1];
    const uint32_t pad = p[0] & CIM_MAP_PAD_LEN_MASK;
    r.ip_offset = CIM_MAP_HEADER;
    r.ip_len = pkt_len > pad ? pkt_len - pad : 0u;
    r.ip_version = r.ip_len ? p[CIM_MAP_HEADER] >> 4 : 0u;   /* rmnet_set_skb_proto: 4 or 6, else ETH_P_MAP */
    return r;
}
/* __rmnet_map_ingress_handler drops MAP commands when INGRESS_MAP_COMMANDS is off (it is, in CIM_DATA_FORMAT; with it
   rmnet_map_command would answer flow control), mux ids out of range and mux ids with no device */
static inline int cim_ingress_deliverable(const struct cim_packet *p, uint32_t data_format, const uint8_t *mux_present)
{
    if (p->command) return 0;
    (void)data_format;
    return p->mux_id < CIM_MAX_LOGICAL_EP && mux_present[p->mux_id] && p->ip_len;
}

/* ------------------------------------------------------------------ TX TREs (gsi_trans_skb_add after linearizing) */
/* One TRE per frame from a DMA bounce slot: a data transfer, last, IEOT and BEI (TX channel) */
static inline void cim_tx_tre(uint8_t tre[16], uint64_t addr, uint32_t len) { cic_tre(tre, addr, len, CIC_CMD_NONE, 1, 1); }

#endif /* NB_IPA55_MODEM_PROTO_H */
